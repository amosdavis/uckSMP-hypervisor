/*
 * uck_internal.h - Internal kernel-side data structures and function prototypes
 */

#ifndef _UCK_INTERNAL_H_
#define _UCK_INTERNAL_H_

#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/net.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/proc_fs.h>

#include "uck.h"

/* Per-page tracking entry */
struct uck_page_entry {
	struct rb_node rb_node;
	pgoff_t index;                /* Page index within region */
	struct page *page;            /* Local page (or NULL) */
	enum uck_page_state state;
	u32 owner_node;               /* Which node owns this page */
};

/* A shared memory region */
struct uck_region {
	struct uck_region_info info;
	struct rb_root pages;         /* RB tree of uck_page_entry */
	struct mutex lock;
	bool active;
};

/* Remote node connection */
struct uck_remote_node {
	struct uck_node_info info;
	struct socket *sock;          /* Outbound socket (or NULL) */
	struct mutex sock_lock;       /* Protects sock for concurrent sends */
	bool connected;

	/* Stats from heartbeat */
	struct uck_node_stats stats;
	unsigned long last_heartbeat; /* jiffies of last received heartbeat */
	bool alive;
};

/* Global module state */
struct uck_state {
	struct mutex lock;
	bool initialized;

	struct uck_node_info local_node;

	struct uck_remote_node nodes[UCK_MAX_NODES];
	int num_nodes;

	struct uck_region regions[UCK_MAX_REGIONS];
	int num_regions;

	/* Network listener */
	struct socket *listen_sock;
	struct task_struct *listen_thread;
	bool net_running;

	/* Heartbeat */
	struct task_struct *heartbeat_thread;
	struct uck_node_stats local_stats;

	/* Procfs */
	struct proc_dir_entry *proc_dir;    /* /proc/uck */
	struct proc_dir_entry *proc_nodes;
	struct proc_dir_entry *proc_cpuinfo;
	struct proc_dir_entry *proc_meminfo;
	struct proc_dir_entry *proc_status;
	struct proc_dir_entry *proc_jobs;

	/* Load balancer */
	struct task_struct *loadbal_thread;
	bool loadbal_enabled;

	/* Remote execution job table */
	struct uck_job_info jobs[UCK_MAX_JOBS];
	struct mutex jobs_lock;
	u32 next_job_id;
};

extern struct uck_state uck_state;

/* uck_region.c */
struct uck_region *uck_find_region(u64 region_id);
int uck_create_region(struct uck_region_info *info);
int uck_join_region(struct uck_region_info *info);
void uck_cleanup_regions(void);

/* uck_page.c */
struct uck_page_entry *uck_page_lookup(struct uck_region *region, pgoff_t index);
struct uck_page_entry *uck_page_alloc_entry(struct uck_region *region, pgoff_t index);
void uck_page_free_all(struct uck_region *region);

/* uck_net.c */
int uck_net_start_listener(u16 port);
void uck_net_stop(void);
int uck_net_fetch_page(struct uck_region *region, pgoff_t page_index, void *dst);
int uck_add_remote_node(struct uck_node_info *info);
int uck_net_connect_node(struct uck_remote_node *node);
int uck_net_send_to_node(u32 node_id, void *buf, int len);
int uck_sock_send(struct socket *sock, void *buf, int len);
int uck_sock_recv(struct socket *sock, void *buf, int len);
int uck_net_send_msg(struct socket *sock, struct uck_msg_hdr *hdr,
		     void *payload, int payload_len);

/* uck_heartbeat.c */
int uck_heartbeat_init(void);
void uck_heartbeat_stop(void);
void uck_update_local_stats(void);
void uck_get_cluster_info(struct uck_cluster_info *info);
void uck_handle_heartbeat(struct socket *client, struct uck_msg_hdr *hdr);

/* uck_sysinfo.c */
int uck_sysinfo_init(void);
void uck_sysinfo_exit(void);

/* uck_proc.c + uck_migrate.c */
int uck_migrate_process(u32 pid, u32 dest_node);
void uck_handle_migrate_request(struct socket *client, struct uck_msg_hdr *hdr);
void uck_handle_proc_state(struct socket *client, struct uck_msg_hdr *hdr);

/* uck_load.c */
int uck_loadbal_init(void);
void uck_loadbal_stop(void);

/* uck_exec.c */
void uck_exec_init(void);
int uck_submit_remote_exec(struct uck_remote_exec_req *req);
int uck_query_jobs(struct uck_job_query *query);
void uck_handle_exec_req(struct socket *client, struct uck_msg_hdr *hdr);
void uck_handle_exec_started(struct socket *client, struct uck_msg_hdr *hdr);
void uck_handle_exec_done(struct socket *client, struct uck_msg_hdr *hdr);
u32 uck_pick_exec_node(void);

/* uck_hyper.c */
int uck_hyper_init(void);
void uck_hyper_stop(void);
int uck_hyper_register_pid(pid_t pid);
void uck_hyper_unregister_pid(pid_t pid);
void uck_hyper_schedule_migration(pid_t child_pid, u32 dest_node);

#endif /* _UCK_INTERNAL_H_ */
