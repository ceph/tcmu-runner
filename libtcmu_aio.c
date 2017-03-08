/*
 * Copyright 2017, Red Hat, Inc.
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
#include <errno.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>

#include "libtcmu.h"
#include "libtcmu_priv.h"
#include "libtcmu_aio.h"
#include "tcmu-runner.h"

void tcmulib_track_aio_request_start(struct tcmu_device *dev)
{
	struct tcmu_track_aio *aio_track = &dev->track_queue;

	pthread_cleanup_push(_cleanup_spin_lock, (void *)&aio_track->track_lock);
	pthread_spin_lock(&aio_track->track_lock);

	++aio_track->tracked_aio_ops;

	pthread_spin_unlock(&aio_track->track_lock);
	pthread_cleanup_pop(0);
}

void tcmulib_track_aio_request_finish(struct tcmu_device *dev, int *is_idle)
{
	struct tcmu_track_aio *aio_track = &dev->track_queue;

	pthread_cleanup_push(_cleanup_spin_lock, (void *)&aio_track->track_lock);
	pthread_spin_lock(&aio_track->track_lock);

	assert(aio_track->tracked_aio_ops > 0);

	--aio_track->tracked_aio_ops;
	if (is_idle) {
		*is_idle = (aio_track->tracked_aio_ops == 0) ? 1 : 0;
	}

	pthread_spin_unlock(&aio_track->track_lock);
	pthread_cleanup_pop(0);
}

static void tcmulib_callout_finished(struct tcmu_device *dev,
				     struct tcmulib_cmd *cmd, int ret)
{
	cmd->callout_cbk(dev, cmd, ret);
}

static int call_stub_exec_async(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd,
				struct tcmu_call_stub *stub)
{
	int rc;
	switch(stub->sop) {
	case TCMU_STORE_OP_READ:
	case TCMU_STORE_OP_WRITE:
		rc = (int) stub->u.rw.exec(dev, stub->u.rw.iov,
					   stub->u.rw.iov_cnt, stub->u.rw.off);
		break;
	case TCMU_STORE_OP_FLUSH:
		rc = stub->u.flush.exec(dev);
		break;
	case TCMU_STORE_OP_HANDLE_CMD:
		rc = stub->u.handle_cmd.exec(dev, cmd);
		break;
	default:
		tcmu_err("unhandled store operation\n");
		assert(0 == "unhandled store operation");
	}

	return rc;
}

static void call_stub_exec_sync(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd,
				struct tcmu_call_stub *stub)
{
	ssize_t ret;
	ssize_t requested;
	int err = SAM_STAT_GOOD;
	uint8_t *sense_buf = cmd->sense_buf;

	switch(stub->sop) {
	case TCMU_STORE_OP_READ:
	case TCMU_STORE_OP_WRITE:
		requested = tcmu_iovec_length(stub->u.rw.iov, stub->u.rw.iov_cnt);
		ret  = stub->u.rw.exec(dev, stub->u.rw.iov,
				       stub->u.rw.iov_cnt, stub->u.rw.off);
		if (ret != requested)
			err = errno_to_sam_status(-EIO, sense_buf);
		break;
	case TCMU_STORE_OP_FLUSH:
		ret = (ssize_t) stub->u.flush.exec(dev);
		if (ret < 0)
			err = errno_to_sam_status(-EIO, sense_buf);
		break;
	case TCMU_STORE_OP_HANDLE_CMD:
		ret = (ssize_t) stub->u.handle_cmd.exec(dev, cmd);
		if (ret < 0)
			err = errno_to_sam_status(-EIO, sense_buf);
		break;
	default:
		tcmu_err("unhandled store operation\n");
		assert(0 == "unhandled store operation");
	}

	tcmulib_callout_finished(dev, cmd, err);
}

static void _cleanup_io_work(void *arg)
{
	free(arg);
}

static void *io_work_queue(void *arg)
{
	struct tcmu_device *dev = arg;
	struct tcmu_io_queue *io_wq = &dev->work_queue;

	while (1) {
		struct tcmu_io_entry *io_entry;
		struct tcmulib_cmd *cmd;

		pthread_cleanup_push(_cleanup_mutex_lock, &io_wq->io_lock);
		pthread_mutex_lock(&io_wq->io_lock);

		while (list_empty(&io_wq->io_queue)) {
			pthread_cond_wait(&io_wq->io_cond,
					  &io_wq->io_lock);
		}

		io_entry = list_first_entry(&io_wq->io_queue,
					    struct tcmu_io_entry, entry);
		list_del(&io_entry->entry);

		pthread_mutex_unlock(&io_wq->io_lock);
		pthread_cleanup_pop(0);

		/* kick start I/O request */
		cmd = io_entry->cmd;
		pthread_cleanup_push(_cleanup_io_work, io_entry);

		call_stub_exec_sync(io_entry->dev, cmd, &io_entry->stub);

		pthread_cleanup_pop(1); /* cleanup io_entry */
	}

	return NULL;
}

static int aio_schedule(struct tcmu_device *dev,
			struct tcmulib_cmd *cmd,
			struct tcmu_call_stub *stub)
{
	struct tcmu_io_entry *io_entry;
	uint8_t *sense = cmd->sense_buf;
	struct tcmu_io_queue *io_wq = &dev->work_queue;

	io_entry = malloc(sizeof(*io_entry));
	if (!io_entry) {
		return errno_to_sam_status(-ENOMEM, sense);
	}

	io_entry->dev = dev;
	io_entry->cmd = cmd;
	memcpy(&io_entry->stub, stub, sizeof(*stub));
	list_node_init(&io_entry->entry);

	/* cleanup push/pop not _really_ required here atm */
	pthread_cleanup_push(_cleanup_mutex_lock, &io_wq->io_lock);
	pthread_mutex_lock(&io_wq->io_lock);

	list_add_tail(&io_wq->io_queue, &io_entry->entry);
	pthread_cond_signal(&io_wq->io_cond); // TODO: conditional

	pthread_mutex_unlock(&io_wq->io_lock);
	pthread_cleanup_pop(0);

	return TCMU_ASYNC_HANDLED;
}

/* execute a given call stub asynchronously */
int async_call_command(struct tcmu_device *dev,
		       struct tcmulib_cmd *cmd,
		       struct tcmu_call_stub *stub)
{
	int ret;
	struct tcmulib_handler *handler = tcmu_get_dev_handler(dev);
	struct tcmur_handler *r_handler = handler->hm_private;

	cmd->callout_cbk = stub->callout_cbk;

	if (r_handler->aio_supported) {
		ret = call_stub_exec_async(dev, cmd, stub);
	} else {
		ret = aio_schedule(dev, cmd, stub);
	}

	return ret;
}

int setup_aio_tracking(struct tcmu_device *dev)
{
	int ret;
	struct tcmu_track_aio *aio_track = &dev->track_queue;

	aio_track->tracked_aio_ops = 0;
	ret = pthread_spin_init(&aio_track->track_lock, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

void cleanup_aio_tracking(struct tcmu_device *dev)
{
	int ret;
	struct tcmu_track_aio *aio_track = &dev->track_queue;

	assert(aio_track->tracked_aio_ops == 0);

	ret = pthread_spin_destroy(&aio_track->track_lock);
	if (ret < 0) {
		tcmu_err("failed to destroy track lock\n");
	}
}

int setup_io_work_queue(struct tcmu_device *dev)
{
	int ret;
	struct tcmu_io_queue *io_wq = &dev->work_queue;

	list_head_init(&io_wq->io_queue);

	ret = pthread_mutex_init(&io_wq->io_lock, NULL);
	if (ret < 0) {
		goto out;
	}
	ret = pthread_cond_init(&io_wq->io_cond, NULL);
	if (ret < 0) {
		goto cleanup_lock;
	}

	// TODO: >1 worker threads (per device via config)
	ret = pthread_create(&io_wq->io_wq_thread, NULL, io_work_queue, dev);
	if (ret < 0) {
		goto cleanup_cond;
	}

	return 0;

cleanup_cond:
	pthread_cond_destroy(&io_wq->io_cond);
cleanup_lock:
	pthread_mutex_destroy(&io_wq->io_lock);
out:
	return ret;
}

void cleanup_io_work_queue(struct tcmu_device *dev, bool cancel)
{
	int ret;
	struct tcmu_io_queue *io_wq = &dev->work_queue;

	if (cancel) {
		cancel_thread(io_wq->io_wq_thread);
	}

	/*
	 * Note that there's no need to drain ->io_queue at this point
	 * as it _should_ be empty (target layer would call this path
	 * when no commands are running - thanks Mike).
	 *
	 * Out of tree handlers which do not use the aio code are not
	 * supported in this path.
	 */

	ret = pthread_mutex_destroy(&io_wq->io_lock);
	if (ret != 0) {
		tcmu_err("failed to destroy io workqueue lock\n");
	}

	ret = pthread_cond_destroy(&io_wq->io_cond);
	if (ret != 0) {
		tcmu_err("failed to destroy io workqueue cond\n");
	}
}
