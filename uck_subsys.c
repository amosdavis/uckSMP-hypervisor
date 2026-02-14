// SPDX-License-Identifier: GPL-2.0
/*
 * uck_subsys.c - Modular subsystem registration framework
 *
 * Provides a lightweight publish-subscribe mechanism for kernel subsystems.
 * Each subsystem registers callbacks for lifecycle events; the core
 * dispatches events in priority order.
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "uck_subsys.h"

static LIST_HEAD(uck_subsys_list);
static DEFINE_MUTEX(uck_subsys_lock);

int uck_subsys_register(struct uck_subsystem *subsys)
{
	struct uck_subsystem *pos;

	if (!subsys || !subsys->name)
		return -EINVAL;

	mutex_lock(&uck_subsys_lock);

	/* Insert in priority order (lower priority value = earlier) */
	list_for_each_entry(pos, &uck_subsys_list, list) {
		if (subsys->priority < pos->priority) {
			list_add_tail(&subsys->list, &pos->list);
			goto out;
		}
	}
	list_add_tail(&subsys->list, &uck_subsys_list);

out:
	pr_info("uck: subsystem '%s' registered (priority %d)\n",
		subsys->name, subsys->priority);
	mutex_unlock(&uck_subsys_lock);
	return 0;
}

void uck_subsys_unregister(struct uck_subsystem *subsys)
{
	if (!subsys)
		return;

	mutex_lock(&uck_subsys_lock);
	list_del(&subsys->list);
	pr_info("uck: subsystem '%s' unregistered\n", subsys->name);
	mutex_unlock(&uck_subsys_lock);
}

void uck_subsys_notify_all(enum uck_event event, void *data)
{
	struct uck_subsystem *subsys;

	/*
	 * No lock here — we tolerate slightly stale list to avoid
	 * holding a mutex in page-fault / interrupt context.
	 * Registration/unregistration is rare and protected by RCU
	 * in production; for now the list is effectively static
	 * after module init.
	 */
	list_for_each_entry(subsys, &uck_subsys_list, list) {
		if (subsys->notify)
			subsys->notify(event, data);
	}
}

void uck_subsys_init(void)
{
	pr_info("uck: subsystem framework initialized\n");
}

void uck_subsys_exit(void)
{
	struct uck_subsystem *subsys, *tmp;

	mutex_lock(&uck_subsys_lock);
	list_for_each_entry_safe(subsys, tmp, &uck_subsys_list, list) {
		if (subsys->exit)
			subsys->exit();
		list_del(&subsys->list);
		pr_info("uck: subsystem '%s' torn down\n", subsys->name);
	}
	mutex_unlock(&uck_subsys_lock);
}
