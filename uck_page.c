/*
 * uck_page.c - Per-page tracking using an RB tree
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include "uck_internal.h"

struct uck_page_entry *uck_page_lookup(struct uck_region *region, pgoff_t index)
{
	struct rb_node *n = region->pages.rb_node;

	while (n) {
		struct uck_page_entry *entry =
			container_of(n, struct uck_page_entry, rb_node);
		if (index < entry->index)
			n = n->rb_left;
		else if (index > entry->index)
			n = n->rb_right;
		else
			return entry;
	}
	return NULL;
}

static void uck_page_insert(struct uck_region *region,
			     struct uck_page_entry *entry)
{
	struct rb_node **p = &region->pages.rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct uck_page_entry *cur =
			container_of(*p, struct uck_page_entry, rb_node);
		parent = *p;
		if (entry->index < cur->index)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&entry->rb_node, parent, p);
	rb_insert_color(&entry->rb_node, &region->pages);
}

struct uck_page_entry *uck_page_alloc_entry(struct uck_region *region,
					     pgoff_t index)
{
	struct uck_page_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return NULL;

	entry->index = index;
	atomic_set(&entry->state, UCK_PAGE_INVALID);
	init_waitqueue_head(&entry->waitq);
	kref_init(&entry->refcount);
	entry->page = NULL;
	entry->owner_node = region->info.owner_node;
	entry->write_mapped = false;

	uck_page_insert(region, entry);
	return entry;
}

static void uck_page_entry_release(struct kref *kref)
{
	struct uck_page_entry *entry =
		container_of(kref, struct uck_page_entry, refcount);
	if (entry->page) {
		void *kaddr = page_address(entry->page);
		if (kaddr)
			memzero_explicit(kaddr, PAGE_SIZE);
		else {
			kaddr = kmap(entry->page);
			memzero_explicit(kaddr, PAGE_SIZE);
			kunmap(entry->page);
		}
		__free_page(entry->page);
		uck_memctl_account_free(1);
	}
	kfree(entry);
}

void uck_page_free_all(struct uck_region *region)
{
	struct rb_node *n;
	struct uck_page_entry *entry;

	mutex_lock(&region->lock);
	while ((n = rb_first(&region->pages))) {
		entry = container_of(n, struct uck_page_entry, rb_node);
		rb_erase(n, &region->pages);
		if (entry->page) {
			void *kaddr = page_address(entry->page);
			if (kaddr)
				memzero_explicit(kaddr, PAGE_SIZE);
			else {
				kaddr = kmap(entry->page);
				memzero_explicit(kaddr, PAGE_SIZE);
				kunmap(entry->page);
			}
			__free_page(entry->page);
			uck_memctl_account_free(1);
		}
		kfree(entry);
	}
	mutex_unlock(&region->lock);
}
