/*
 * uck_region.c - Region management for UCK
 */

#include <linux/slab.h>
#include "uck_internal.h"

struct uck_region *uck_find_region(u64 region_id)
{
	int i;
	for (i = 0; i < uck_state.num_regions; i++) {
		if (uck_state.regions[i].active &&
		    uck_state.regions[i].info.region_id == region_id)
			return &uck_state.regions[i];
	}
	return NULL;
}

int uck_create_region(struct uck_region_info *info)
{
	struct uck_region *region;

	if (uck_state.num_regions >= UCK_MAX_REGIONS)
		return -ENOMEM;

	if (uck_find_region(info->region_id))
		return -EEXIST;

	region = &uck_state.regions[uck_state.num_regions];
	region->info = *info;
	region->pages = RB_ROOT;
	mutex_init(&region->lock);
	region->active = true;
	uck_state.num_regions++;

	return 0;
}

int uck_join_region(struct uck_region_info *info)
{
	/* For joining, we create a local region descriptor that mirrors a
	 * remote region. Pages start as INVALID and are fetched on demand. */
	return uck_create_region(info);
}

void uck_cleanup_regions(void)
{
	int i;
	for (i = 0; i < uck_state.num_regions; i++) {
		if (uck_state.regions[i].active) {
			uck_page_free_all(&uck_state.regions[i]);
			uck_state.regions[i].active = false;
		}
	}
	uck_state.num_regions = 0;
}
