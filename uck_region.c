/*
 * uck_region.c - Region management for UCK
 */

#include <linux/slab.h>
#include <linux/cred.h>
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

	/* Initialize ACL with caller's credentials */
	region->acl.region_id = info->region_id;
	region->acl.owner_uid = __kuid_val(current_uid());
	region->acl.owner_gid = __kgid_val(current_gid());
	region->acl.mode = 0600;  /* Owner read/write by default */

	uck_state.num_regions++;

	return 0;
}

int uck_join_region(struct uck_region_info *info)
{
	/* For joining, we create a local region descriptor that mirrors a
	 * remote region. Pages start as INVALID and are fetched on demand. */
	return uck_create_region(info);
}

/* Check if current process has permission to access a region */
bool uck_region_check_access(struct uck_region *region, bool write)
{
	kuid_t uid = current_uid();
	kgid_t gid = current_gid();
	u32 mode = region->acl.mode;

	/* Root always has access */
	if (__kuid_val(uid) == 0)
		return true;

	/* Owner check */
	if (__kuid_val(uid) == region->acl.owner_uid) {
		if (write)
			return (mode & 0200) != 0;
		return (mode & 0400) != 0;
	}

	/* Group check */
	if (__kgid_val(gid) == region->acl.owner_gid) {
		if (write)
			return (mode & 0020) != 0;
		return (mode & 0040) != 0;
	}

	/* Other */
	if (write)
		return (mode & 0002) != 0;
	return (mode & 0004) != 0;
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
