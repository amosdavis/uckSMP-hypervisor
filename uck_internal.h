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
#include <linux/version.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/ratelimit.h>

#include "uck.h"

/* vm_flags_set() added in kernel 6.3 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define uck_vm_flags_set(vma, flags) vm_flags_set(vma, flags)
#else
#define uck_vm_flags_set(vma, flags) ((vma)->vm_flags |= (flags))
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define uck_class_create(name) class_create(name)
#else
#define uck_class_create(name) class_create(THIS_MODULE, name)
#endif

/* get_user_pages_remote(): vmas parameter removed in kernel 6.5 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define uck_get_user_pages_remote(mm, addr, nr, flags, pages) \
	get_user_pages_remote(mm, addr, nr, flags, pages, NULL)
#else
#define uck_get_user_pages_remote(mm, addr, nr, flags, pages) \
	get_user_pages_remote(mm, addr, nr, flags, pages, NULL, NULL)
#endif

/*
 * Lock ordering (always acquire in this order to prevent deadlocks):
 *   1. uck_state.lock          (global state)
 *   2. region->lock            (per-region)
 *   3. uck_state.jobs_lock     (job table)
 *   4. uck_state.nodes[i].sock_lock (per-node socket)
 *   5. page tree spinlock      (per-page, if converted)
 *
 * Never hold a socket lock while acquiring uck_state.lock.
 * Never hold a region lock while acquiring uck_state.lock.
 * Network I/O must not occur while holding spinlocks.
 */

/* Per-page tracking entry */
struct uck_page_entry {
	struct rb_node rb_node;
	pgoff_t index;                /* Page index within region */
	struct page *page;            /* Local page (or NULL) */
	atomic_t state;              /* enum uck_page_state, atomic for cmpxchg */
	u32 owner_node;               /* Which node owns this page */
	bool write_mapped;            /* True if mapped writable (for coherence) */
	wait_queue_head_t waitq;     /* waiters for IN_TRANSIT completion */
	struct kref refcount;        /* reference counting for UAF prevention */
};

/* A shared memory region */
struct uck_region {
	struct uck_region_info info;
	struct rb_root pages;         /* RB tree of uck_page_entry */
	struct mutex lock;
	bool active;
	struct uck_region_acl acl;   /* access control */
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
	u32 missed_heartbeats;       /* consecutive missed heartbeats */
	u32 cpu_features;            /* CPU feature flags from remote */
	bool suspect;                /* suspect state before dead */

	/* cgroup stats from this node */
	struct uck_cgroup_stats cgroup_stats;
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

	/* cgroup accounting */
	struct uck_cgroup_stats local_cgroup_stats;
	struct proc_dir_entry *proc_cgroup;
	struct proc_dir_entry *proc_futex;

	/* Quorum / epoch */
	u32 cluster_epoch;           /* current cluster generation */
	bool degraded;               /* true if in minority partition */
	int quorum_threshold;        /* minimum nodes for majority */

	/* Resource limits */
	u32 max_tasks_per_node;
	u64 max_pages_total;
	u64 current_pages_total;

	/* Audit */
	struct proc_dir_entry *proc_audit;

	/* Rate limiting */
	struct ratelimit_state ioctl_ratelimit;
	struct ratelimit_state net_msg_ratelimit;

	/* Additional procfs entries */
	struct proc_dir_entry *proc_latency;
	struct proc_dir_entry *proc_pages;
	struct proc_dir_entry *proc_migrations;
	struct proc_dir_entry *proc_errors;
	struct proc_dir_entry *proc_config;

	/* CPU feature flags for local node (migration compat check) */
	u64 local_cpu_features;

	/* Error counters */
	atomic_long_t err_page_fetch;
	atomic_long_t err_migration;
	atomic_long_t err_migrate;
	atomic_long_t err_auth;
	atomic_long_t err_ratelimit;
	atomic_long_t err_futex;
	atomic_long_t err_page_fault;

	/* Latency histogram buckets (16us, 32us, ..., 2048us) */
	atomic_long_t latency_hist[8];

	/* Page state counters */
	atomic_long_t total_pages;
	atomic_long_t pages_shared;
	atomic_long_t pages_exclusive;
	atomic_long_t pages_transit;

	/* Resource limits */
	u64 max_pages;

	/* Quorum active flag */
	bool quorum_active;

	/* Graceful shutdown */
	bool shutting_down;
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
int uck_net_fetch_pages_batch(struct uck_region *region,
			      pgoff_t *indices, int nr_pages);
int uck_add_remote_node(struct uck_node_info *info);
int uck_net_connect_node(struct uck_remote_node *node);
int uck_net_send_to_node(u32 node_id, void *buf, int len);
int uck_net_send_msg_to_node(u32 node_id, struct uck_msg_hdr *hdr,
			     void *payload, int payload_len);
int uck_sock_send(struct socket *sock, void *buf, int len);
int uck_sock_recv(struct socket *sock, void *buf, int len);
int uck_net_send_msg(struct socket *sock, struct uck_msg_hdr *hdr,
		     void *payload, int payload_len);

/* uck_batch.c */
void uck_handle_batch_page_req(struct socket *client,
			       struct uck_msg_hdr *hdr);
int uck_build_prefetch_list(struct uck_region *region, pgoff_t fault_index,
			    pgoff_t *out_indices, int max_pages);

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

/* uck_futex.c */
int uck_futex_init(void);
void uck_futex_exit(void);
int uck_futex_op(struct uck_futex_req *req);
void uck_handle_futex_wait(struct socket *client, struct uck_msg_hdr *hdr);
void uck_handle_futex_wake(struct socket *client, struct uck_msg_hdr *hdr);

/* uck_cgroup.c */
int uck_cgroup_init(void);
void uck_cgroup_exit(void);
int uck_cgroup_update_local(void);
void uck_handle_cgroup_stats(struct socket *client, struct uck_msg_hdr *hdr);

/* uck_rdma.c */
int uck_rdma_init(void);
void uck_rdma_exit(void);
int uck_rdma_fetch_page(struct uck_region *region, pgoff_t page_index, void *dst);
bool uck_rdma_available(void);

/* Dynamic join/leave */
void uck_handle_node_announce(struct socket *client, struct uck_msg_hdr *hdr);
void uck_handle_node_leave(struct socket *client, struct uck_msg_hdr *hdr);
int uck_node_leave_graceful(void);

/* uck_audit.c */
int uck_audit_init(void);
void uck_audit_exit(void);
void uck_audit_log(const char *event, const char *fmt, ...);

/* uck_ratelimit.c */
int uck_ratelimit_init(void);
void uck_ratelimit_exit(void);
bool uck_ratelimit_check_net(u32 node_id);
bool uck_ratelimit_check_ioctl(void);

/* uck_quorum.c */
int uck_quorum_init(void);
void uck_quorum_exit(void);
void uck_quorum_check(void);
bool uck_quorum_is_degraded(void);

/* uck_memctl.c */
int uck_memctl_init(void);
void uck_memctl_exit(void);
bool uck_memctl_can_alloc(unsigned int nr_pages);
void uck_memctl_account_alloc(unsigned int nr_pages);
void uck_memctl_account_free(unsigned int nr_pages);

/* Page state helpers */
static inline enum uck_page_state uck_page_get_state(struct uck_page_entry *e)
{
	return (enum uck_page_state)atomic_read(&e->state);
}

static inline bool uck_page_try_set_state(struct uck_page_entry *e,
					   enum uck_page_state old,
					   enum uck_page_state new)
{
	return atomic_cmpxchg(&e->state, (int)old, (int)new) == (int)old;
}

#endif /* _UCK_INTERNAL_H_ */
