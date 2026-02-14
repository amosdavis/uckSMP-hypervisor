/*
 * uck_memctl.c - Memory overcommit protection for UCK
 *
 * Tracks total page allocations across all regions and enforces
 * configurable limits to prevent OOM conditions.
 */

#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "uck_internal.h"

/* Default: 80% of system memory */
static unsigned long uck_max_pages_pct = 80;
module_param(uck_max_pages_pct, ulong, 0644);
MODULE_PARM_DESC(uck_max_pages_pct,
		 "Max percentage of system memory for UCK pages (default 80)");

static unsigned long uck_warn_pct_70 = 0;
static unsigned long uck_warn_pct_80 = 0;
static unsigned long uck_warn_pct_90 = 0;

bool uck_memctl_can_alloc(unsigned int nr_pages)
{
	u64 current_pages, max_pages;

	if (uck_state.max_pages_total == 0) {
		/* Calculate based on system memory */
		struct sysinfo si;
		si_meminfo(&si);
		uck_state.max_pages_total =
			si.totalram * uck_max_pages_pct / 100;
	}

	max_pages = uck_state.max_pages_total;
	current_pages = uck_state.current_pages_total;

	if (current_pages + nr_pages > max_pages) {
		atomic_long_inc(&uck_state.err_page_fetch);
		uck_audit_log("memctl",
			      "page allocation denied: %llu + %u > %llu limit",
			      current_pages, nr_pages, max_pages);
		return false;
	}

	/* Watermark alerts */
	if (!uck_warn_pct_90 && current_pages > max_pages * 90 / 100) {
		uck_warn_pct_90 = 1;
		pr_warn("uck_memctl: 90%% memory watermark reached "
			"(%llu/%llu pages)\n", current_pages, max_pages);
		uck_audit_log("memctl", "90%% watermark: %llu/%llu pages",
			      current_pages, max_pages);
	} else if (!uck_warn_pct_80 && current_pages > max_pages * 80 / 100) {
		uck_warn_pct_80 = 1;
		pr_warn("uck_memctl: 80%% memory watermark reached\n");
	} else if (!uck_warn_pct_70 && current_pages > max_pages * 70 / 100) {
		uck_warn_pct_70 = 1;
		pr_info("uck_memctl: 70%% memory watermark reached\n");
	}

	return true;
}

void uck_memctl_account_alloc(unsigned int nr_pages)
{
	uck_state.current_pages_total += nr_pages;
}

void uck_memctl_account_free(unsigned int nr_pages)
{
	if (uck_state.current_pages_total >= nr_pages)
		uck_state.current_pages_total -= nr_pages;
	else
		uck_state.current_pages_total = 0;

	/* Reset watermarks if we go below thresholds */
	if (uck_state.max_pages_total > 0) {
		u64 max = uck_state.max_pages_total;
		u64 cur = uck_state.current_pages_total;
		if (cur < max * 70 / 100)
			uck_warn_pct_70 = uck_warn_pct_80 = uck_warn_pct_90 = 0;
		else if (cur < max * 80 / 100)
			uck_warn_pct_80 = uck_warn_pct_90 = 0;
		else if (cur < max * 90 / 100)
			uck_warn_pct_90 = 0;
	}
}

int uck_memctl_init(void)
{
	uck_state.current_pages_total = 0;
	uck_state.max_pages_total = 0;  /* calculated on first alloc */
	pr_info("uck_memctl: memory overcommit protection initialized "
		"(limit=%lu%%)\n", uck_max_pages_pct);
	return 0;
}

void uck_memctl_exit(void)
{
	pr_info("uck_memctl: stopped (peak pages: %llu)\n",
		uck_state.current_pages_total);
}
