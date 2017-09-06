/*
 * Copyright 2016, China Mobile, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>

#include <scsi/scsi.h>

#include "tcmu-runner.h"
#include "tcmur_cmd_handler.h"
#include "libtcmu.h"
#include "tcmur_device.h"

#include <rbd/librbd.h>
#include <rados/librados.h>

/*
 * rbd_lock_acquire exclusive lock support was added in librbd 0.1.11
 */
#if LIBRBD_VERSION_CODE >= LIBRBD_VERSION(0, 1, 11)
#define RBD_LOCK_ACQUIRE_SUPPORT
#endif

/* rbd_aio_discard added in 0.1.2 */
#if LIBRBD_VERSION_CODE >= LIBRBD_VERSION(0, 1, 2)
#define RBD_DISCARD_SUPPORT
#endif

/*
 * rbd_aio_writesame support was added in librbd 1.12.0
 */
#if LIBRBD_VERSION_CODE >= LIBRBD_VERSION(1, 12, 0) || LIBRBD_SUPPORTS_WRITESAME
#define RBD_WRITE_SAME_SUPPORT
#endif

struct tcmu_rbd_state {
	rados_t cluster;
	rados_ioctx_t io_ctx;
	rbd_image_t image;

	char *image_name;
	char *pool_name;
	char *osd_op_timeout;
};

struct rbd_aio_cb {
	struct tcmu_device *dev;
	struct tcmulib_cmd *tcmulib_cmd;

	int64_t length;
	char *bounce_buffer;
};

#ifdef LIBRADOS_SUPPORTS_SERVICES

#ifdef RBD_LOCK_ACQUIRE_SUPPORT
static void tcmu_rbd_service_status_update(struct tcmu_device *dev,
					   bool has_lock)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	char *status_buf = NULL;
	int ret;

	ret = asprintf(&status_buf, "%s%c%s%c", "lock_owner", '\0',
		       has_lock ? "true" : "false", '\0');
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not allocate status buf. Service will not be updated.\n");
		return;
	}

	ret = rados_service_update_status(state->cluster, status_buf);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not update service status. (Err %d)\n",
			     ret);
	}

	free(status_buf);
}
#endif /* RBD_LOCK_ACQUIRE_SUPPORT */

static int tcmu_rbd_service_register(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct utsname u;
	char *daemon_buf = NULL;
	char *metadata_buf = NULL;
	int ret;

	ret = uname(&u);
	if (ret < 0) {
		ret = -errno;
		tcmu_dev_err(dev, "Could not query uname. (Err %d)\n", ret);
		return ret;
	}

	ret = asprintf(&daemon_buf, "%s:%s/%s",
		       u.nodename, state->pool_name, state->image_name);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not allocate daemon buf.\n");
		return -ENOMEM;
	}

	ret = asprintf(&metadata_buf, "pool_name%c%s%cimage_name%c%s%c",
		       '\0', state->pool_name, '\0',
		       '\0', state->image_name, '\0');
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not allocate metadata buf.\n");
		ret = ENOMEM;
		goto free_daemon_buf;
	}

	ret = rados_service_register(state->cluster, "tcmu-runner",
				     daemon_buf, metadata_buf);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not register service to cluster. (Err %d)\n",
			     ret);
	}

	free(metadata_buf);
free_daemon_buf:
	free(daemon_buf);
	return ret;
}

#else /* LIBRADOS_SUPPORTS_SERVICES */

static int tcmu_rbd_service_register(struct tcmu_device *dev)
{
	/* Ignorable. Just log in dbg mode just in case. */
	tcmu_dev_dbg(dev, "Ceph service registration not supported.\n");
	return 0;
}

#ifdef RBD_LOCK_ACQUIRE_SUPPORT
static void tcmu_rbd_service_status_update(struct tcmu_device *dev,
					   bool has_lock)
{
}
#endif /* RBD_LOCK_ACQUIRE_SUPPORT */

#endif /* LIBRADOS_SUPPORTS_SERVICES */

static void tcmu_rbd_image_close(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);

	rbd_close(state->image);
	rados_ioctx_destroy(state->io_ctx);
	rados_shutdown(state->cluster);

	state->cluster = NULL;
	state->io_ctx = NULL;
	state->image = NULL;
}

static int timer_check_and_set_def(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	char buf[128];
	int grace, interval, ret, len;
	float timeout;

	ret = rados_conf_get(state->cluster, "osd_heartbeat_grace",
			     buf, 128);
	if (ret) {
		tcmu_dev_err(dev, "Failed to get cluster's default osd_heartbeat_grace\n");
		return ret;
	}
	grace = atoi(buf);

	ret = rados_conf_get(state->cluster, "osd_heartbeat_interval",
			     buf, 128);
	if (ret) {
		tcmu_dev_err(dev, "Failed to get cluster's default osd_heartbeat_interval\n");
		return ret;
	}
	interval = atoi(buf);

	ret = rados_conf_get(state->cluster, "rados_osd_op_timeout",
			     buf, 128);
	if (ret) {
		tcmu_dev_err(dev, "Failed to get cluster's default rados_osd_op_timeout\n");
		return ret;
	}
	timeout = atof(buf);

	tcmu_dev_dbg(dev, "The cluster's default osd op timeout(%f), osd heartbeat grace(%d) interval(%d)\n",
		     timeout, grace, interval);

	/* Frist: Try to use new osd op timeout value */
	if (state->osd_op_timeout && atof(state->osd_op_timeout) > grace + interval)
		goto set;

	/* Second: Try to use the default osd op timeout value as read from the cluster */
	if (timeout > grace + interval) {
		tcmu_dev_dbg(dev, "The osd op timeout will remain the default value: %f\n", timeout);
		return 0;
	}

	tcmu_dev_warn(dev, "osd op timeout (%s) must be larger than osd heartbeat grace (%d) + interval (%d)!\n",
		      state->osd_op_timeout, grace, interval);

	/*
	 * At last: Set the default rados_osd_op_timeout to grace + interval + 5
	 * to make sure rados_osd_op_timeout > grace + interval.
	 */
	len = sprintf(buf, "%d", grace + interval + 5);
	buf[len] = '\0';

	if (state->osd_op_timeout)
		free(state->osd_op_timeout);

	state->osd_op_timeout = strdup(buf);
	if (!state->osd_op_timeout) {
		tcmu_dev_err(dev, "Failed to alloc memory for ->osd_op_timeout\n");
		return -ENOMEM;
	}

	tcmu_dev_warn(dev, "Will set the osd op timeout to %s instead!\n",
		      state->osd_op_timeout);

set:
	return rados_conf_set(state->cluster, "rados_osd_op_timeout",
			      state->osd_op_timeout);
}

static int tcmu_rbd_image_open(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret;

	ret = rados_create(&state->cluster, NULL);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not create cluster. (Err %d)\n", ret);
		return ret;
	}

	/* Fow now, we will only read /etc/ceph/ceph.conf */
	rados_conf_read_file(state->cluster, NULL);
	rados_conf_set(state->cluster, "rbd_cache", "false");

	ret = timer_check_and_set_def(dev);
	if (ret)
		tcmu_dev_warn(dev,
			      "Could not set rados osd op timeout to %s (Err %d. Failover may be delayed.)\n",
			      state->osd_op_timeout, ret);

	ret = rados_connect(state->cluster);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not connect to cluster. (Err %d)\n",
			     ret);
		goto set_cluster_null;
	}

	ret = tcmu_rbd_service_register(dev);
	if (ret < 0)
		goto rados_shutdown;

	ret = rados_ioctx_create(state->cluster, state->pool_name,
				 &state->io_ctx);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not create ioctx for pool %s. (Err %d)\n",
			     state->pool_name, ret);
		goto rados_shutdown;
	}

	ret = rbd_open(state->io_ctx, state->image_name, &state->image, NULL);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not open image %s. (Err %d)\n",
			     state->image_name, ret);
		goto rados_destroy;
	}
	return 0;

rados_destroy:
	rados_ioctx_destroy(state->io_ctx);
	state->io_ctx = NULL;
rados_shutdown:
	rados_shutdown(state->cluster);
set_cluster_null:
	state->cluster = NULL;
	return ret;
}

#ifdef RBD_LOCK_ACQUIRE_SUPPORT

/*
 * Returns:
 * 0 = client is not owner.
 * 1 = client is owner.
 * -ESHUTDOWN/-EBLACKLISTED(-108) = client is blacklisted.
 * -ETIMEDOUT = rados osd op timeout has expired.
 * -EIO = misc error.
 */
static int tcmu_rbd_has_lock(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret, is_owner;

	ret = rbd_is_exclusive_lock_owner(state->image, &is_owner);
	if (ret == -ESHUTDOWN || ret == -ETIMEDOUT) {
		return ret;
	} else if (ret < 0) {
		/* let initiator figure things out */
		tcmu_dev_err(dev, "Could not check lock ownership. (Err %d).\n", ret);
		return -EIO;
	} else if (is_owner) {
		tcmu_dev_dbg(dev, "Is owner\n");
		return 1;
	}
	tcmu_dev_dbg(dev, "Not owner\n");

	return 0;
}

/**
 * tcmu_rbd_lock_break - break rbd exclusive lock if needed
 * @dev: device to break the lock for.
 * @orig_owner: if non null, only break the lock if get owners matches
 *
 * If orig_owner is null and tcmu_rbd_lock_break fails to break the lock
 * for a retryable error (-EAGAIN) the owner of the lock will be returned.
 * The caller must free the string returned.
 *
 * Returns:
 * 0 = lock has been broken.
 * -EAGAIN = retryable error
 * -ETIMEDOUT = could not complete operation in rados osd op timeout seconds.
 * -EIO = hard failure.
 */
static int tcmu_rbd_lock_break(struct tcmu_device *dev, char **orig_owner)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	rbd_lock_mode_t lock_mode;
	char *owners[1];
	size_t num_owners = 1;
	int ret;

	ret = rbd_lock_get_owners(state->image, &lock_mode, owners,
				  &num_owners);
	if (ret == -ENOENT || (!ret && !num_owners))
		return 0;

	if (ret < 0) {
		tcmu_dev_err(dev, "Could not get lock owners %d\n", ret);
		if (ret == -ETIMEDOUT)
			return ret;
		else
			return -EAGAIN;
	}

	if (lock_mode != RBD_LOCK_MODE_EXCLUSIVE) {
		tcmu_dev_err(dev, "Invalid lock type (%d) found\n", lock_mode);
		ret = -EIO;
		goto free_owners;
	}

	if (*orig_owner && strcmp(*orig_owner, owners[0])) {
		/* someone took the lock while we were retrying */
		ret = -EIO;
		goto free_owners;
	}

	tcmu_dev_dbg(dev, "Attempting to break lock from %s.\n", owners[0]);

	ret = rbd_lock_break(state->image, lock_mode, owners[0]);
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not break lock from %s. (Err %d)\n",
			     owners[0], ret);
		if (ret == -ETIMEDOUT)
			return ret;

		ret = -EAGAIN;
		if (!*orig_owner) {
			*orig_owner = strdup(owners[0]);
			if (!*orig_owner)
				ret = -EIO;
		}
	}

free_owners:
	rbd_lock_get_owners_cleanup(owners, num_owners);
	return ret;
}

static int tcmu_rbd_lock(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	int ret = 0, attempts = 0;
	char *orig_owner = NULL;

	/*
	 * TODO: Add retry/timeout settings to handle windows/ESX.
	 * Or, set to transitioning and grab the lock in the background.
	 */
	while (attempts++ < 5) {
		ret = tcmu_rbd_has_lock(dev);
		if (ret == 1) {
			ret = 0;
			break;
		} else if (ret == -ETIMEDOUT ||  ret == -ESHUTDOWN) {
			break;
		} else if (ret < 0) {
			sleep(1);
			continue;
		}

		ret = tcmu_rbd_lock_break(dev, &orig_owner);
		if (ret == -EIO || ret == -ETIMEDOUT) {
			break;
		} else if (ret == -EAGAIN) {
			sleep(1);
			continue;
		}

		ret = rbd_lock_acquire(state->image, RBD_LOCK_MODE_EXCLUSIVE);
		if (!ret) {
			tcmu_dev_warn(dev, "Acquired exclusive lock.\n");
			break;
		} else if (ret == -ETIMEDOUT) {
			break;
		}

		tcmu_dev_err(dev, "Unknown error %d while trying to acquire lock.\n",
			     ret);
	}

	if (orig_owner)
		free(orig_owner);

	if (ret == -ETIMEDOUT || ret == -ESHUTDOWN)
		ret = TCMUR_LOCK_NOTCONN;
	else if (ret)
		ret = TCMUR_LOCK_FAILED;
	else
		ret = TCMUR_LOCK_SUCCESS;

	tcmu_rbd_service_status_update(dev, ret == TCMUR_LOCK_SUCCESS ?
				       true : false);
	return ret;
}

static void tcmu_rbd_check_excl_lock_enabled(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	uint64_t features = 0;
	int ret;

	ret = rbd_get_features(state->image, &features);
	if (ret) {
		tcmu_dev_warn(dev, "Could not get rbd features. HA may not be supported. Err %d.\n", ret);
		return;
	}

	if (!(features & RBD_FEATURE_EXCLUSIVE_LOCK)) {
		tcmu_dev_warn(dev, "exclusive-lock not enabled for image. HA not supported.\n");
	}
}

#else /* RBD_LOCK_ACQUIRE_SUPPORT */

static void tcmu_rbd_check_excl_lock_enabled(struct tcmu_device *dev)
{
	tcmu_dev_warn(dev, "HA not supported.\n");
}

#endif /* RBD_LOCK_ACQUIRE_SUPPORT */

static void tcmu_rbd_state_free(struct tcmu_rbd_state *state)
{
	if (state->osd_op_timeout)
		free(state->osd_op_timeout);
	if (state->image_name)
		free(state->image_name);
	if (state->pool_name)
		free(state->pool_name);
	free(state);
}

static int tcmu_rbd_open(struct tcmu_device *dev)
{
	rbd_image_info_t image_info;
	char *pool, *name, *next_opt;
	char *config, *dev_cfg_dup;
	struct tcmu_rbd_state *state;
	uint64_t rbd_size;
	int ret;

	state = calloc(1, sizeof(*state));
	if (!state)
		return -ENOMEM;
	tcmu_set_dev_private(dev, state);

	dev_cfg_dup = strdup(tcmu_get_dev_cfgstring(dev));
	config = dev_cfg_dup;
	if (!config) {
		ret = -ENOMEM;
		goto free_state;
	}

	tcmu_dev_dbg(dev, "tcmu_rbd_open config %s\n", config);
	config = strchr(config, '/');
	if (!config) {
		tcmu_dev_err(dev, "no configuration found in cfgstring\n");
		ret = -EINVAL;
		goto free_config;
	}
	config += 1; /* get past '/' */

	pool = strtok(config, "/");
	if (!pool) {
		tcmu_dev_err(dev, "Could not get pool name\n");
		ret = -EINVAL;
		goto free_config;
	}
	state->pool_name = strdup(pool);
	if (!state->pool_name) {
		ret = -ENOMEM;
		tcmu_dev_err(dev, "Could not copy pool name\n");
		goto free_config;
	}

	name = strtok(NULL, ",");
	if (!name) {
		tcmu_dev_err(dev, "Could not get image name\n");
		ret = -EINVAL;
		goto free_config;
	}

	state->image_name = strdup(name);
	if (!state->image_name) {
		ret = -ENOMEM;
		tcmu_dev_err(dev, "Could not copy image name\n");
		goto free_config;
	}

	/* The next options are optional */
	next_opt = strtok(NULL, ",");
	if (next_opt) {
		if (!strncmp(next_opt, "osd_op_timeout=", 15)) {
			state->osd_op_timeout = strdup(next_opt + 15);
			if (!state->osd_op_timeout ||
			    !strlen(state->osd_op_timeout)) {
				ret = -ENOMEM;
				tcmu_dev_err(dev, "Could not copy osd op timeout.\n");
				goto free_config;
			}
		}
	}

	ret = tcmu_rbd_image_open(dev);
	if (ret < 0) {
		goto free_config;
	}

	tcmu_rbd_check_excl_lock_enabled(dev);

	ret = rbd_get_size(state->image, &rbd_size);
	if (ret < 0) {
		tcmu_dev_err(dev, "error getting rbd_size %s\n", name);
		goto stop_image;
	}

	if (rbd_size !=
	    tcmu_get_dev_num_lbas(dev) * tcmu_get_dev_block_size(dev)) {
		tcmu_dev_err(dev, "device size and backing size disagree: device (num LBAs %lld, block size %ld) backing %lld\n",
			     tcmu_get_dev_num_lbas(dev),
			     tcmu_get_dev_block_size(dev), rbd_size);
		ret = -EIO;
		goto stop_image;
	}

	ret = rbd_stat(state->image, &image_info, sizeof(image_info));
	if (ret < 0) {
		tcmu_dev_err(dev, "Could not stat image.\n");
		goto stop_image;
	}
	tcmu_set_dev_max_xfer_len(dev, image_info.obj_size /
				  tcmu_get_dev_block_size(dev));

	tcmu_set_dev_write_cache_enabled(dev, 0);

	tcmu_dev_dbg(dev, "config %s, size %lld\n", tcmu_get_dev_cfgstring(dev),
		     rbd_size);
	free(dev_cfg_dup);
	return 0;

stop_image:
	tcmu_rbd_image_close(dev);
free_config:
	free(dev_cfg_dup);
free_state:
	tcmu_rbd_state_free(state);
	return ret;
}

static void tcmu_rbd_close(struct tcmu_device *dev)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);

	tcmu_rbd_image_close(dev);
	tcmu_rbd_state_free(state);
}

static int tcmu_rbd_handle_blacklisted_cmd(struct tcmu_device *dev,
					   struct tcmulib_cmd *cmd)
{
       tcmu_notify_lock_lost(dev);
	/*
	 * This will happen during failback normally, because
	 * running IO is failed due to librbd's immediate blacklisting
	 * during lock acquisition on a higher priority path.
	 */
	return tcmu_set_sense_data(cmd->sense_buf, NOT_READY,
				   ASC_STATE_TRANSITION, NULL);
}

/*
 * TODO: Check timers.
 * The rados osd op timeout must be longer than the timeouts to detect
 * unreachable OSDs (osd heartbeat grace + osd heartbeat interval) or
 * we will end up failing the transport connection when we just needed
 * to try a different OSD.
 */
static int tcmu_rbd_handle_timedout_cmd(struct tcmu_device *dev,
					struct tcmulib_cmd *cmd)
{
	tcmu_dev_err(dev, "Timing out cmd.\n");
	tcmu_notify_conn_lost(dev);

	/*
	 * TODO: For AA, we will want to kill the ceph tcp connections
	 * with LINGER on and set to 0, so there are no TCP retries,
	 * and we need something on the OSD side to drop requests
	 * that end up reaching it after the initiator's failover/recovery
	 * timeout. For implicit and explicit FO, we will just disable
	 * the iscsi port, and let the initiator switch paths which will
	 * result in us getting blacklisted, so fail with a retryable
	 * error.
	 */
	return SAM_STAT_BUSY;
}

/*
 * NOTE: RBD async APIs almost always return 0 (success), except
 * when allocation (via new) fails - which is not caught. So,
 * the only errno we've to bother about as of now are memory
 * allocation errors.
 */

static void rbd_finish_aio_read(rbd_completion_t completion,
				struct rbd_aio_cb *aio_cb)
{
	struct tcmu_device *dev = aio_cb->dev;
	struct tcmulib_cmd *tcmulib_cmd = aio_cb->tcmulib_cmd;
	struct iovec *iovec = tcmulib_cmd->iovec;
	size_t iov_cnt = tcmulib_cmd->iov_cnt;
	int64_t ret;
	int tcmu_r;

	ret = rbd_aio_get_return_value(completion);
	rbd_aio_release(completion);

	if (ret == -ETIMEDOUT) {
		tcmu_r = tcmu_rbd_handle_timedout_cmd(dev, tcmulib_cmd);
	} else if (ret == -ESHUTDOWN) {
		tcmu_r = tcmu_rbd_handle_blacklisted_cmd(dev, tcmulib_cmd);
	} else if (ret < 0) {
		tcmu_dev_err(dev, "Got fatal read error %d.\n", ret);
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     MEDIUM_ERROR, ASC_READ_ERROR, NULL);
	} else {
		tcmu_r = SAM_STAT_GOOD;
		tcmu_memcpy_into_iovec(iovec, iov_cnt,
				       aio_cb->bounce_buffer, aio_cb->length);
	}

	tcmulib_cmd->done(dev, tcmulib_cmd, tcmu_r);

	free(aio_cb->bounce_buffer);
	free(aio_cb);
}

static int tcmu_rbd_read(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			     struct iovec *iov, size_t iov_cnt, size_t length,
			     off_t offset)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->length = length;
	aio_cb->tcmulib_cmd = cmd;

	aio_cb->bounce_buffer = malloc(length);
	if (!aio_cb->bounce_buffer) {
		tcmu_dev_err(dev, "Could not allocate bounce buffer.\n");
		goto out_free_aio_cb;
	}

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_read, &completion);
	if (ret < 0) {
		goto out_free_bounce_buffer;
	}

	ret = rbd_aio_read(state->image, offset, length, aio_cb->bounce_buffer,
			   completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_bounce_buffer:
	free(aio_cb->bounce_buffer);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

static void rbd_finish_aio_generic(rbd_completion_t completion,
				   struct rbd_aio_cb *aio_cb)
{
	struct tcmu_device *dev = aio_cb->dev;
	struct tcmulib_cmd *tcmulib_cmd = aio_cb->tcmulib_cmd;
	int64_t ret;
	int tcmu_r;

	ret = rbd_aio_get_return_value(completion);
	rbd_aio_release(completion);

	if (ret == -ETIMEDOUT) {
		tcmu_r = tcmu_rbd_handle_timedout_cmd(dev, tcmulib_cmd);
	} else if (ret == -ESHUTDOWN) {
		tcmu_r = tcmu_rbd_handle_blacklisted_cmd(dev, tcmulib_cmd);
	} else if (ret < 0) {
		tcmu_dev_err(dev, "Got fatal write error %d.\n", ret);
		tcmu_r = tcmu_set_sense_data(tcmulib_cmd->sense_buf,
					     MEDIUM_ERROR, ASC_WRITE_ERROR,
					     NULL);
	} else {
		tcmu_r = SAM_STAT_GOOD;
	}

	tcmulib_cmd->done(dev, tcmulib_cmd, tcmu_r);

	if (aio_cb->bounce_buffer) {
		free(aio_cb->bounce_buffer);
	}
	free(aio_cb);
}

static int tcmu_rbd_write(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			  struct iovec *iov, size_t iov_cnt, size_t length,
			  off_t offset)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->length = length;
	aio_cb->tcmulib_cmd = cmd;

	aio_cb->bounce_buffer = malloc(length);
	if (!aio_cb->bounce_buffer) {
		tcmu_dev_err(dev, "Failed to allocate bounce buffer.\n");
		goto out_free_aio_cb;
	}

	tcmu_memcpy_from_iovec(aio_cb->bounce_buffer, length, iov, iov_cnt);

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		goto out_free_bounce_buffer;
	}

	ret = rbd_aio_write(state->image, offset,
			    length, aio_cb->bounce_buffer, completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_bounce_buffer:
	free(aio_cb->bounce_buffer);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

#ifdef RBD_DISCARD_SUPPORT
static int tcmu_rbd_unmap(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			  uint64_t off, uint64_t len)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->tcmulib_cmd = cmd;
	aio_cb->bounce_buffer = NULL;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0)
		goto out_free_aio_cb;

	ret = rbd_aio_discard(state->image, off, len, completion);
	if (ret < 0)
		goto out_remove_tracked_aio;

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}
#endif /* RBD_DISCARD_SUPPORT */

#ifdef LIBRBD_SUPPORTS_AIO_FLUSH

static int tcmu_rbd_flush(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret = -ENOMEM;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->tcmulib_cmd = cmd;
	aio_cb->bounce_buffer = NULL;

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0) {
		goto out_free_aio_cb;
	}

	ret = rbd_aio_flush(state->image, completion);
	if (ret < 0) {
		goto out_remove_tracked_aio;
	}

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}

#endif

#ifdef RBD_WRITE_SAME_SUPPORT
static int tcmu_rbd_aio_writesame(struct tcmu_device *dev,
				  struct tcmulib_cmd *cmd,
				  uint64_t off, uint64_t len,
				  struct iovec *iov, size_t iov_cnt)
{
	struct tcmu_rbd_state *state = tcmu_get_dev_private(dev);
	struct rbd_aio_cb *aio_cb;
	rbd_completion_t completion;
	ssize_t ret;

	aio_cb = calloc(1, sizeof(*aio_cb));
	if (!aio_cb) {
		tcmu_dev_err(dev, "Could not allocate aio_cb.\n");
		goto out;
	}

	aio_cb->dev = dev;
	aio_cb->tcmulib_cmd = cmd;
	aio_cb->length = tcmu_iovec_length(iov, iov_cnt);

	aio_cb->bounce_buffer = malloc(aio_cb->length);
	if (!aio_cb->bounce_buffer) {
		tcmu_dev_err(dev, "Failed to allocate bounce buffer.\n");
		goto out_free_aio_cb;
	}

	tcmu_memcpy_from_iovec(aio_cb->bounce_buffer, aio_cb->length, iov, iov_cnt);

	ret = rbd_aio_create_completion
		(aio_cb, (rbd_callback_t) rbd_finish_aio_generic, &completion);
	if (ret < 0)
		goto out_free_bounce_buffer;

	tcmu_dev_dbg(dev, "Start write same off:%llu, len:%llu\n", off, len);

	ret = rbd_aio_writesame(state->image, off, len, aio_cb->bounce_buffer,
				aio_cb->length, completion, 0);
	if (ret < 0)
		goto out_remove_tracked_aio;

	return 0;

out_remove_tracked_aio:
	rbd_aio_release(completion);
out_free_bounce_buffer:
	free(aio_cb->bounce_buffer);
out_free_aio_cb:
	free(aio_cb);
out:
	return SAM_STAT_TASK_SET_FULL;
}
#endif /* RBD_WRITE_SAME_SUPPORT */

/*
 * Return scsi status or TCMU_NOT_HANDLED
 */
static int tcmu_rbd_handle_cmd(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	int ret;

	switch(cdb[0]) {
#ifdef RBD_WRITE_SAME_SUPPORT
	case WRITE_SAME:
	case WRITE_SAME_16:
		ret = tcmur_handle_writesame(dev, cmd, tcmu_rbd_aio_writesame);
		break;
#endif
	default:
		ret = TCMU_NOT_HANDLED;
	}

	return ret;
}

/*
 * For backstore creation
 *
 * Specify poolname/devicename, e.g,
 *
 * $ targetcli /backstores/user:rbd create test 2G rbd/test/osd_op_timeout=30
 *
 * poolname must be the name of an existing rados pool.
 *
 * devicename is the name of the rbd image.
 */
static const char tcmu_rbd_cfg_desc[] =
	"RBD config string is of the form:\n"
	"poolname/devicename/optional osd_op_timeout=N secs\n"
	"where:\n"
	"poolname:	Existing RADOS pool\n"
	"devicename:	Name of the RBD image\n";

struct tcmur_handler tcmu_rbd_handler = {
	.name	       = "Ceph RBD handler",
	.subtype       = "rbd",
	.cfg_desc      = tcmu_rbd_cfg_desc,
	.open	       = tcmu_rbd_open,
	.close	       = tcmu_rbd_close,
	.read	       = tcmu_rbd_read,
	.write	       = tcmu_rbd_write,
#ifdef LIBRBD_SUPPORTS_AIO_FLUSH
	.flush	       = tcmu_rbd_flush,
#endif
#ifdef RBD_DISCARD_SUPPORT
	.unmap         = tcmu_rbd_unmap,
#endif
	.handle_cmd    = tcmu_rbd_handle_cmd,
#ifdef RBD_LOCK_ACQUIRE_SUPPORT
	.lock          = tcmu_rbd_lock,
#endif
};

int handler_init(void)
{
	return tcmur_register_handler(&tcmu_rbd_handler);
}
