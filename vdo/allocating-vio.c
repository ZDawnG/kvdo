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

#include "allocating-vio.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "allocation-selector.h"
#include "block-allocator.h"
#include "data-vio.h"
#include "kernel-types.h"
#include "pbn-lock.h"
#include "slab-depot.h"
#include "types.h"
#include "vdo.h"
#include "vio-write.h"

/**
 * Make a single attempt to acquire a write lock on a newly-allocated PBN.
 *
 * @param allocating_vio  The allocating_vio that wants a write lock for its
 *                        newly allocated block
 *
 * @return VDO_SUCCESS or an error code
 **/
static int attempt_pbn_write_lock(struct allocating_vio *allocating_vio)
{
	struct pbn_lock *lock;
	int result;

	assert_vio_in_physical_zone(allocating_vio);

	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not acquire a lock while already referencing one");

	result = attempt_vdo_physical_zone_pbn_lock(allocating_vio->zone,
						    allocating_vio->allocation,
						    allocating_vio->write_lock_type,
						    &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock->holder_count > 0) {
		/* This block is already locked, which should be impossible. */
		return uds_log_error_strerror(VDO_LOCK_ERROR,
					      "Newly allocated block %llu was spuriously locked (holder_count=%u)",
					      (unsigned long long) allocating_vio->allocation,
					      lock->holder_count);
	}

	/* We've successfully acquired a new lock, so mark it as ours. */
	lock->holder_count += 1;
	allocating_vio->allocation_lock = lock;
	assign_vdo_pbn_lock_provisional_reference(lock);
	return VDO_SUCCESS;
}

/**
 * Finish the allocation process.
 *
 * @param allocating_vio  The allocating vio
 * @param result          The allocation result
 **/
static void finish_allocation(struct allocating_vio *allocating_vio,
			      int result)
{
	struct vdo_completion *completion =
		allocating_vio_as_completion(allocating_vio);

	if (result == VDO_NO_SPACE) {
		/*
		 * We will still try to deduplicate if we didn't get an
		 * allocation, so don't treat no space as an error.
		 */
		result = VDO_SUCCESS;
	}

	completion->callback = allocating_vio->allocation_callback;
	continue_vdo_completion(completion, result);
}

static void allocate_block_in_zone(struct vdo_completion *completion);

/**
 * Retry allocating a block now that we're done waiting for scrubbing.
 *
 * @param waiter   The allocating_vio that was waiting to allocate
 * @param context  The context (unused)
 **/
static void
retry_allocate_block_in_zone(struct waiter *waiter,
			     void *context __always_unused)
{
	struct allocating_vio *allocating_vio =
		waiter_as_allocating_vio(waiter);

	/* Now that some slab has scrubbed, start the allocation process anew. */
	allocating_vio->wait_for_clean_slab = false;
	allocating_vio->allocation_attempts = 0;
	allocate_block_in_zone(allocating_vio_as_completion(allocating_vio));
}

/**
 * Check whether an allocating_vio still has zones to attempt to allocate from.
 *
 * @param allocating_vio  The allocating_vio which needs an allocation
 *
 * @return true if there are still zones to try
 **/
static inline bool
has_zones_to_try(struct allocating_vio *allocating_vio)
{
	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);

	return (allocating_vio->allocation_attempts <
		vdo->thread_config->physical_zone_count);
}

/**
 * Check whether to move on to the next allocation zone now. If not, either
 * there are no more zones to try, we've enqueued to wait for scrubbing,
 * or there was an error.
 *
 * @param allocating_vio  The vio
 *
 * @return true if the we should try allocating in the next zone
 **/
static bool should_try_next_zone(struct allocating_vio *allocating_vio)
{
	struct block_allocator *allocator =
		get_vdo_physical_zone_block_allocator(allocating_vio->zone);
	struct waiter *waiter = allocating_vio_as_waiter(allocating_vio);
	int result;

	if (!allocating_vio->wait_for_clean_slab) {
		if (has_zones_to_try(allocating_vio)) {
			return true;
		}

		/*
		 * No zone has known free blocks, so check them all again after 
		 * waiting for scrubbing. 
		 */
		allocating_vio->wait_for_clean_slab = true;
		allocating_vio->allocation_attempts = 1;
	}

	waiter->callback = retry_allocate_block_in_zone;
	result = enqueue_for_clean_vdo_slab(allocator, waiter);
	if (result == VDO_SUCCESS) {
		return false;
	}

	if ((result != VDO_NO_SPACE) || !has_zones_to_try(allocating_vio)) {
		/*
		 * Either there was an error, or we've tried everything and 
		 * found nothing. 
		 */
		finish_allocation(allocating_vio, result);
		return false;
	}

	return true;
}

/**
 * Try the next zone since we didn't find a free block in the current one.
 *
 * @param allocating_vio  The allocating_vio
 **/
static void try_next_zone(struct allocating_vio *allocating_vio)
{
	zone_count_t zone_number;
	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);

	if (!should_try_next_zone(allocating_vio)) {
		return;
	}

	zone_number = get_vdo_physical_zone_number(allocating_vio->zone) + 1;
	if (zone_number == vdo->thread_config->physical_zone_count) {
		zone_number = 0;
	}

	allocating_vio->zone = vdo->physical_zones[zone_number];
	vio_launch_physical_zone_callback(allocating_vio,
					  allocate_block_in_zone);
}

/**
 * Attempt to allocate a block. This callback is registered in
 * vio_allocate_data_block() and from itself.
 *
 * @param completion  The allocating_vio needing an allocation
 **/
static void allocate_block_in_zone(struct vdo_completion *completion)
{
	int result;
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);
	struct block_allocator *allocator =
		get_vdo_physical_zone_block_allocator(allocating_vio->zone);

	assert_vio_in_physical_zone(allocating_vio);

	allocating_vio->allocation_attempts++;
	result = allocate_vdo_block(allocator, &allocating_vio->allocation);
	if (result == VDO_NO_SPACE) {
		try_next_zone(allocating_vio);
		return;
	}

	if (result == VDO_SUCCESS) {
		result = attempt_pbn_write_lock(allocating_vio);
	}

	finish_allocation(allocating_vio, result);
}

/**********************************************************************/
void vio_allocate_data_block(struct allocating_vio *allocating_vio,
			     struct allocation_selector *selector,
			     enum pbn_lock_type write_lock_type,
			     vdo_action *callback)
{
	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);

	allocating_vio->write_lock_type = write_lock_type;
	allocating_vio->allocation_callback = callback;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->allocation = VDO_ZERO_BLOCK;

	allocating_vio->zone =
		vdo->physical_zones[get_next_vdo_allocation_zone(selector)];

	vio_launch_physical_zone_callback(allocating_vio,
					  allocate_block_in_zone);
}

/**********************************************************************/
void vio_release_allocation_lock(struct allocating_vio *allocating_vio)
{
	physical_block_number_t locked_pbn;

	assert_vio_in_physical_zone(allocating_vio);
	locked_pbn = allocating_vio->allocation;
	if (vdo_pbn_lock_has_provisional_reference(allocating_vio->allocation_lock)) {
		allocating_vio->allocation = VDO_ZERO_BLOCK;
	}

	release_vdo_physical_zone_pbn_lock(allocating_vio->zone,
					   locked_pbn,
					   UDS_FORGET(allocating_vio->allocation_lock));
}

/**********************************************************************/
void vio_reset_allocation(struct allocating_vio *allocating_vio)
{
	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not reset allocation while holding a PBN lock");

	allocating_vio->zone = NULL;
	allocating_vio->allocation = VDO_ZERO_BLOCK;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->wait_for_clean_slab = false;
}

/**********************************************************************/
int create_compressed_write_vio(struct vdo *vdo,
				void *parent,
				char *data,
				struct allocating_vio **allocating_vio_ptr)
{
	struct bio *bio;
	struct allocating_vio *allocating_vio;
	struct vio *vio;

	/*
	 * Compressed write vios should use direct allocation and not use the 
	 * buffer pool, which is reserved for submissions from the linux block 
	 * layer.
	 */
	int result = UDS_ALLOCATE(1, struct allocating_vio, __func__,
				  &allocating_vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("compressed write vio allocation failure %d",
			      result);
		return result;
	}

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(allocating_vio);
		return result;
	}

	vio = allocating_vio_as_vio(allocating_vio);
	initialize_vio(vio,
		       bio,
		       VIO_TYPE_COMPRESSED_BLOCK,
		       VIO_PRIORITY_COMPRESSED_DATA,
		       parent,
		       vdo,
		       data);
	*allocating_vio_ptr = allocating_vio;
	return VDO_SUCCESS;
}
