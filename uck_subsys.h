/* SPDX-License-Identifier: GPL-2.0 */
/*
 * uck_subsys.h - Modular subsystem registration interface
 *
 * Each subsystem (heartbeat, load balancer, exec, migration, futex)
 * registers itself with the core via this interface.  The core dispatches
 * lifecycle events to all registered subsystems.
 */

#ifndef _UCK_SUBSYS_H
#define _UCK_SUBSYS_H

#include <linux/list.h>

/* Events dispatched to subsystems */
enum uck_event {
	UCK_EVENT_NODE_UP,
	UCK_EVENT_NODE_DOWN,
	UCK_EVENT_NODE_SUSPECT,
	UCK_EVENT_PAGE_FAULT,
	UCK_EVENT_REGION_CREATE,
	UCK_EVENT_REGION_DESTROY,
	UCK_EVENT_QUORUM_GAINED,
	UCK_EVENT_QUORUM_LOST,
	UCK_EVENT_SHUTDOWN,
};

struct uck_subsystem {
	const char *name;
	struct list_head list;

	/* Lifecycle */
	int  (*init)(void);
	void (*exit)(void);

	/* Event notification */
	void (*notify)(enum uck_event event, void *data);

	/* Priority: lower = called first (0=highest) */
	int priority;
};

/* Registration API */
int  uck_subsys_register(struct uck_subsystem *subsys);
void uck_subsys_unregister(struct uck_subsystem *subsys);

/* Dispatch an event to all registered subsystems */
void uck_subsys_notify_all(enum uck_event event, void *data);

/* Initialize/teardown the subsystem framework */
void uck_subsys_init(void);
void uck_subsys_exit(void);

#endif /* _UCK_SUBSYS_H */
