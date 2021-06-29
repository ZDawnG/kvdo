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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/sysfs.c#26 $
 */

#include "sysfs.h"

#include <linux/module.h>

#include "dedupeIndex.h"
#include "dmvdo.h"
#include "logger.h"
#include "vdoInit.h"

static char *status_strings[] = {
	"UNINITIALIZED",
	"READY",
	"SHUTTING DOWN",
};

/**********************************************************************/
static int vdo_status_show(char *buf,
			   const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", status_strings[vdo_module_status]);
}

/**********************************************************************/
static int vdo_log_level_show(char *buf,
			      const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", uds_log_priority_to_string(get_uds_log_level()));
}

/**********************************************************************/
static int vdo_log_level_store(const char *buf,
			       const struct kernel_param *kp)
{
	static char internal_buf[11];

	int n = strlen(buf);
	if (n > 10) {
		return -EINVAL;
	}

	memset(internal_buf, '\000', sizeof(internal_buf));
	memcpy(internal_buf, buf, n);
	if (internal_buf[n - 1] == '\n') {
		internal_buf[n - 1] = '\000';
	}
	set_uds_log_level(uds_log_string_to_priority(internal_buf));
	return 0;
}


/**********************************************************************/
static int vdo_dedupe_timeout_interval_store(const char *buf,
					     const struct kernel_param *kp)
{
	int result = param_set_uint(buf, kp);
	if (result != 0) {
		return result;
	}
	set_vdo_dedupe_index_timeout_interval(*(uint *)kp->arg);
	return 0;
}

/**********************************************************************/
static int vdo_min_dedupe_timer_interval_store(const char *buf,
					       const struct kernel_param *kp)
{
	int result = param_set_uint(buf, kp);
	if (result != 0) {
		return result;
	}
	set_vdo_dedupe_index_min_timer_interval(*(uint *)kp->arg);
	return 0;
}

static const struct kernel_param_ops status_ops = {
	.get = vdo_status_show,
};

static const struct kernel_param_ops log_level_ops = {
	.set = vdo_log_level_store,
	.get = vdo_log_level_show,
};


static const struct kernel_param_ops dedupe_timeout_ops = {
	.set = vdo_dedupe_timeout_interval_store,
	.get = param_get_uint,
};

static const struct kernel_param_ops dedupe_timer_ops = {
	.set = vdo_min_dedupe_timer_interval_store,
	.get = param_get_uint,
};

module_param_cb(status, &status_ops, NULL, 0444);

module_param_cb(log_level, &log_level_ops, NULL, 0644);


module_param_cb(deduplication_timeout_interval, &dedupe_timeout_ops,
		&vdo_dedupe_index_timeout_interval, 0644);

module_param_cb(min_deduplication_timer_interval, &dedupe_timer_ops,
		&vdo_dedupe_index_min_timer_interval, 0644);
