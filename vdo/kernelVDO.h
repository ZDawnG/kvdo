/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"
#include "types.h"
#include "vdo.h"

#include "kernel-types.h"
#include "workQueue.h"


/**
 * Enqueue a work item to be processed in the base code context.
 *
 * @param vdo        The vdo object in which to run the work item
 * @param item       The work item to be run
 * @param thread_id  The thread on which to run the work item
 **/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id);

/**
 * Set up and enqueue a vio's work item to be processed in the base code
 * context.
 *
 * @param vio             The vio with the work item to be run
 * @param work            The function pointer to execute
 * @param priority        The priority of the work
 **/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 enum vdo_work_item_priority priority);

#endif /* KERNEL_VDO_H */
