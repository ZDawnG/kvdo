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
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/vdoInit.c#5 $
 */

#include "vdoInit.h"

#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/list.h>

#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "instanceNumber.h"
#include "poolSysfs.h"
#include "types.h"
#include "vdoInternal.h"
#include "volumeGeometry.h"

/**********************************************************************/
const char *get_vdo_device_name(const struct dm_target *target)
{
	return dm_device_name(dm_table_get_md(target->table));
}

/**
 * Initialize the vdo and work queue sysfs directories.
 *
 * @param vdo     The vdo being initialized
 * @param target  The device-mapper target this vdo is
 * @param parent  The parent directory object
 * @param reason  A pointer to hold an error message on failure
 *
 * @return VDO_SUCCESS or an error code
 **/
static int initialize_vdo_kobjects(struct vdo *vdo,
				   struct dm_target *target,
				   struct kobject *parent,
				   char **reason)
{
	int result;
	kobject_init(&vdo->vdo_directory, &vdo_directory_type);
	result = kobject_add(&vdo->vdo_directory,
			     parent,
			     get_vdo_device_name(target));
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&vdo->vdo_directory);
		return result;
	}

	kobject_init(&vdo->work_queue_directory,
		     &work_queue_directory_type);
	result = kobject_add(&vdo->work_queue_directory,
			     &vdo->vdo_directory,
			     "work_queues");
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&vdo->work_queue_directory);
		kobject_put(&vdo->vdo_directory);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int initialize_vdo(struct vdo *vdo,
		   PhysicalLayer *layer,
		   struct device_config *config,
		   struct kobject *parent,
		   unsigned int instance,
		   char **reason)
{
	int result;

	vdo->layer = layer;
	vdo->device_config = config;
	vdo->starting_sector_offset = config->owning_target->begin;
	vdo->instance = instance;
	INIT_LIST_HEAD(&vdo->device_config_list);
	initialize_admin_completion(vdo, &vdo->admin_completion);

	result = read_geometry_block(get_vdo_backing_device(vdo),
				     &vdo->geometry);
	if (result != VDO_SUCCESS) {
		*reason = "Could not load geometry block";
		release_vdo_instance(instance);
		FREE(layer);
		return result;
	}

	/*
	 * After this point, calling kobject_put on vdo_directory will
	 * decrement its reference count, and when the count goes to 0 the
	 * struct kernel_layer (which contains this vdo) will be freed.
	 */
	result = initialize_vdo_kobjects(vdo,
					 config->owning_target,
					 parent,
					 reason);
	if (result != VDO_SUCCESS) {
		release_vdo_instance(instance);
		FREE(layer);
		return result;
	}

	return VDO_SUCCESS;
}
