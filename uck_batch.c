/*
 * uck_batch.c - Batch page transfer (multiple pages per TCP round trip)
 *
 * Instead of one page per request/response, we send up to
 * UCK_BATCH_MAX_PAGES in a single TCP exchange. On fault,
 * we prefetch UCK_PREFETCH_WINDOW adjacent pages as well.
 */

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>

#include "uck_internal.h"

/*
 * Fetch multiple pages from a remote node in one round trip.
 * Allocates pages, fetches data, inserts into region cache.
 * Returns number of pages successfully fetched, or negative on error.
 */
int uck_net_fetch_pages_batch(struct uck_region *region,
			      pgoff_t *indices, int nr_pages)
{
	struct uck_remote_node *owner = NULL;
	struct uck_msg_hdr hdr;
	struct uck_batch_page_req req;
	struct uck_batch_page_resp resp;
	int i, ret, fetched = 0;

	if (nr_pages <= 0 || nr_pages > UCK_BATCH_MAX_PAGES)
		return -EINVAL;

	/* Find the owning node */
	for (i = 0; i < uck_state.num_nodes; i++) {
		if (uck_state.nodes[i].info.node_id ==
		    region->info.owner_node) {
			owner = &uck_state.nodes[i];
			break;
		}
	}

	if (!owner) {
		if (region->info.owner_node ==
		    uck_state.local_node.node_id)
			return -ENOENT;
		return -ENOENT;
	}

	if (!owner->connected) {
		ret = uck_net_connect_node(owner);
		if (ret < 0)
			return ret;
	}

	/* Build batch request */
	memset(&req, 0, sizeof(req));
	req.region_id = region->info.region_id;
	req.nr_pages = nr_pages;
	for (i = 0; i < nr_pages; i++)
		req.offsets[i] = (u64)indices[i] << PAGE_SHIFT;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = UCK_MSG_BATCH_PAGE_REQ;
	hdr.src_node = uck_state.local_node.node_id;
	hdr.region_id = region->info.region_id;
	hdr.payload_len = sizeof(req);

	mutex_lock(&owner->sock_lock);

	ret = uck_net_send_msg(owner->sock, &hdr, &req, sizeof(req));
	if (ret < 0)
		goto out_unlock;

	/* Receive batch response header */
	ret = uck_sock_recv(owner->sock, &resp, sizeof(resp));
	if (ret < 0)
		goto out_unlock;

	if (resp.flags != 0 || resp.nr_pages == 0) {
		ret = -EIO;
		goto out_unlock;
	}

	/* Receive page data and insert into cache */
	for (i = 0; i < (int)resp.nr_pages; i++) {
		struct page *page;
		struct uck_page_entry *entry;
		void *kaddr;
		pgoff_t idx = indices[i];

		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page)
			break;

		kaddr = kmap(page);
		ret = uck_sock_recv(owner->sock, kaddr, PAGE_SIZE);
		kunmap(page);

		if (ret < 0) {
			__free_page(page);
			break;
		}

		SetPageUptodate(page);

		mutex_lock(&region->lock);
		entry = uck_page_lookup(region, idx);
		if (!entry)
			entry = uck_page_alloc_entry(region, idx);
		if (entry) {
			if (entry->page)
				__free_page(entry->page);
			entry->page = page;
			entry->state = UCK_PAGE_SHARED;
			entry->write_mapped = false;
			get_page(page);
		}
		mutex_unlock(&region->lock);

		if (!entry)
			__free_page(page);
		else
			fetched++;
	}

	mutex_unlock(&owner->sock_lock);
	return fetched;

out_unlock:
	mutex_unlock(&owner->sock_lock);
	return ret;
}

/*
 * Handle incoming UCK_MSG_BATCH_PAGE_REQ on the server side.
 * Reads the batch request, sends back all requested pages.
 */
void uck_handle_batch_page_req(struct socket *client,
			       struct uck_msg_hdr *hdr)
{
	struct uck_batch_page_req req;
	struct uck_batch_page_resp resp;
	struct uck_region *region;
	int i, ret;
	char *zero_page;

	if (hdr->payload_len != sizeof(req))
		return;

	ret = uck_sock_recv(client, &req, sizeof(req));
	if (ret < 0)
		return;

	if (req.nr_pages == 0 || req.nr_pages > UCK_BATCH_MAX_PAGES)
		return;

	zero_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!zero_page)
		return;

	mutex_lock(&uck_state.lock);
	region = uck_find_region(req.region_id);
	mutex_unlock(&uck_state.lock);

	memset(&resp, 0, sizeof(resp));
	resp.region_id = req.region_id;

	if (!region) {
		resp.flags = 1;
		resp.nr_pages = 0;
		uck_sock_send(client, &resp, sizeof(resp));
		kfree(zero_page);
		return;
	}

	resp.nr_pages = req.nr_pages;
	resp.flags = 0;
	uck_sock_send(client, &resp, sizeof(resp));

	/* Send each page */
	for (i = 0; i < (int)req.nr_pages; i++) {
		struct uck_page_entry *entry;
		pgoff_t idx = req.offsets[i] >> PAGE_SHIFT;

		mutex_lock(&region->lock);
		entry = uck_page_lookup(region, idx);
		mutex_unlock(&region->lock);

		if (entry && entry->page) {
			void *kaddr = kmap(entry->page);
			uck_sock_send(client, kaddr, PAGE_SIZE);
			kunmap(entry->page);
		} else {
			uck_sock_send(client, zero_page, PAGE_SIZE);
		}
	}

	kfree(zero_page);
}

/*
 * Build a prefetch list: given a faulting page index, add adjacent
 * pages that are not yet in the cache.
 */
int uck_build_prefetch_list(struct uck_region *region, pgoff_t fault_index,
			    pgoff_t *out_indices, int max_pages)
{
	pgoff_t max_index;
	pgoff_t start;
	int count = 0;
	pgoff_t idx;
	int nr_prefetch;

	max_index = region->info.size >> PAGE_SHIFT;

	/* Always include the faulting page first */
	out_indices[count++] = fault_index;

	/* Adaptive prefetch: track access pattern */
	{
		static unsigned long last_page_index;
		static int sequential_count;
		static int prefetch_window = UCK_PREFETCH_WINDOW;

		if (fault_index == last_page_index + 1) {
			/* Sequential access detected */
			sequential_count++;
			if (sequential_count > 4 && prefetch_window < 32)
				prefetch_window = min(prefetch_window * 2, 32);
		} else if (fault_index != last_page_index) {
			/* Random access detected */
			sequential_count = 0;
			prefetch_window = max(prefetch_window / 2, 1);
		}
		last_page_index = fault_index;

		/* Use adaptive window instead of fixed */
		nr_prefetch = prefetch_window;
	}

	/* Add adjacent pages using adaptive window */
	start = (fault_index > (pgoff_t)nr_prefetch) ?
		fault_index - nr_prefetch : 0;

	for (idx = start; idx < max_index && count < max_pages; idx++) {
		struct uck_page_entry *entry;

		if (idx == fault_index)
			continue;

		entry = uck_page_lookup(region, idx);
		if (entry && entry->state != UCK_PAGE_INVALID)
			continue;

		out_indices[count++] = idx;
	}

	return count;
}
