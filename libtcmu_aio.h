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

#ifndef __LIBTCMU_AIO_H
#define __LIBTCMU_AIO_H

struct tcmu_device;
struct tcmulib_cmd;
struct tcmu_call_stub;

int setup_io_work_queue(struct tcmu_device *);
void cleanup_io_work_queue(struct tcmu_device *, bool);

int setup_aio_tracking(struct tcmu_device *);
void cleanup_aio_tracking(struct tcmu_device *);

int async_call_command(struct tcmu_device *,
		       struct tcmulib_cmd *,
		       struct tcmu_call_stub *);

#endif /* __LIBTCMU_AIO_H */
