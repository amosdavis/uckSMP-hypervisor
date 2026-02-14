/*
 * uck.h - Unified Compute Kernel header
 *
 * Provides:
 *   - Shared memory across networked nodes
 *   - Process migration between nodes
 *   - Unified resource reporting (aggregated CPU/memory)
 *   - Load-balanced process placement
 *
 * Targeting Linux 6.1 LTS (Debian Bookworm)
 */

#ifndef _UCK_H_
#define _UCK_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define UCK_DEVICE_NAME  "uck"
#define UCK_MAX_NODES    16
#define UCK_MAX_REGIONS  64
#define UCK_PORT_DEFAULT 9999
#define UCK_HEARTBEAT_INTERVAL_MS 2000
#define UCK_HEARTBEAT_TIMEOUT_MS  6000

/* Batch page transfer limits */
#define UCK_BATCH_MAX_PAGES  32   /* Max pages per batch request */
#define UCK_PREFETCH_WINDOW   8   /* Adjacent pages to prefetch on fault */

#define UCK_PROTOCOL_VERSION  2   /* Wire protocol version */

/* Page states in the coherence protocol */
enum uck_page_state {
	UCK_PAGE_INVALID = 0,
	UCK_PAGE_SHARED,
	UCK_PAGE_MODIFIED,
	UCK_PAGE_EXCLUSIVE,       /* Writable, sole copy */
	UCK_PAGE_IN_TRANSIT,      /* Page being transferred between nodes */
};

/* Node identity */
struct uck_node_info {
	__u32 node_id;
	__u32 ip_addr;          /* Network byte order */
	__u16 port;
	__u16 flags;
};

/* Per-node runtime stats (exchanged via heartbeat) */
struct uck_node_stats {
	__u32 node_id;
	__u32 nr_cpus;          /* Number of online CPUs */
	__u64 total_mem;        /* Total RAM in bytes */
	__u64 free_mem;         /* Free RAM in bytes */
	__u32 load_avg;         /* Load average * 100 (fixed point) */
	__u32 nr_running;       /* Number of runnable tasks */
	__u64 timestamp;        /* Sender's jiffies */
	__u8  hmac[32];         /* HMAC-SHA256 of heartbeat payload */
};

/* Cluster-wide aggregated info (returned to userspace) */
struct uck_cluster_info {
	__u32 num_nodes;
	__u32 total_cpus;
	__u64 total_mem;
	__u64 total_free_mem;
	__u32 total_running;
};

/* Process migration request (userspace → kernel) */
struct uck_migrate_req {
	__u32 pid;              /* PID to migrate */
	__u32 dest_node;        /* Destination node ID */
};

/* Shared memory region descriptor */
struct uck_region_info {
	__u64 region_id;
	__u64 size;             /* Must be page-aligned */
	__u32 owner_node;       /* Node that initially owns all pages */
};

/* Per-region access control */
struct uck_region_acl {
	__u64 region_id;
	__u32 owner_uid;
	__u32 owner_gid;
	__u32 mode;           /* Unix-style: rwxrwxrwx */
};

/* Remote execution request (userspace → kernel) */
struct uck_remote_exec_req {
	__u32 dest_node;        /* 0 = auto-select least loaded */
	__u32 flags;            /* UCK_EXEC_* flags */
	char  command[512];     /* Command line to execute */
	char  workdir[256];     /* Working directory (empty = /) */
};

/* Remote execution flags */
#define UCK_EXEC_ASYNC    0x01   /* Don't wait for completion */
#define UCK_EXEC_ANYWHERE 0x02   /* Run on any node including local */

/* Job status tracking */
#define UCK_MAX_JOBS      256

enum uck_job_state {
	UCK_JOB_EMPTY = 0,
	UCK_JOB_PENDING,
	UCK_JOB_RUNNING,
	UCK_JOB_DONE,
	UCK_JOB_FAILED,
};

struct uck_job_info {
	__u32 job_id;
	__u32 node_id;          /* Node where job is running */
	__u32 pid;              /* PID on remote node (0 if not yet started) */
	__u32 state;            /* enum uck_job_state */
	__s32 exit_code;        /* Exit code (-1 if not done) */
	__u32 src_node;         /* Node that submitted the job */
	char  command[512];
};

/* Job query request */
struct uck_job_query {
	__u32 job_id;           /* Input: job to query (0 = list all) */
	__u32 count;            /* Output: number of jobs returned */
	struct uck_job_info jobs[16]; /* Output: job info array */
};

/* Network message types */
enum uck_msg_type {
	/* Memory */
	UCK_MSG_PAGE_REQ = 1,
	UCK_MSG_PAGE_RESP,
	UCK_MSG_INVALIDATE,
	UCK_MSG_INVALIDATE_ACK,
	UCK_MSG_PAGE_WRITE_REQ,
	UCK_MSG_PAGE_WRITE_RESP,
	UCK_MSG_NODE_JOIN,
	UCK_MSG_NODE_JOIN_ACK,

	/* Batch page transfer */
	UCK_MSG_BATCH_PAGE_REQ = 10,
	UCK_MSG_BATCH_PAGE_RESP,

	/* Heartbeat */
	UCK_MSG_HEARTBEAT = 20,

	/* Process migration */
	UCK_MSG_MIGRATE_REQ = 30,
	UCK_MSG_MIGRATE_ACCEPT,
	UCK_MSG_MIGRATE_REJECT,
	UCK_MSG_PROC_STATE,     /* Serialized process state follows */
	UCK_MSG_PROC_PAGES,     /* Memory pages for migrated process */
	UCK_MSG_MIGRATE_DONE,
	UCK_MSG_MIGRATE_FAIL,

	/* Remote execution */
	UCK_MSG_EXEC_REQ = 40,  /* Execute command on remote node */
	UCK_MSG_EXEC_STARTED,   /* Remote confirms execution started */
	UCK_MSG_EXEC_DONE,      /* Remote reports completion + exit code */

	/* Node join/leave */
	UCK_MSG_NODE_ANNOUNCE = 50,
	UCK_MSG_NODE_ANNOUNCE_ACK,
	UCK_MSG_NODE_LEAVE,
	UCK_MSG_NODE_LEAVE_ACK,

	/* Distributed futex */
	UCK_MSG_FUTEX_WAIT = 60,
	UCK_MSG_FUTEX_WAKE,
	UCK_MSG_FUTEX_WAKE_RESP,

	/* cgroup accounting */
	UCK_MSG_CGROUP_STATS = 70,

	/* Security */
	UCK_MSG_FUTEX_WAKE_NACK = 80,
	UCK_MSG_MIGRATE_ABORT,

	/* Quorum/fencing */
	UCK_MSG_EPOCH_UPDATE = 90,
};

/* Remote exec wire payload (sent after msg_hdr with type UCK_MSG_EXEC_REQ) */
struct uck_exec_wire {
	__u32 job_id;
	__u32 src_node;
	__u32 uid;
	__u32 gid;
	char  command[512];
	char  workdir[256];
};

/* Remote exec result (sent after msg_hdr with type UCK_MSG_EXEC_DONE) */
struct uck_exec_result {
	__u32 job_id;
	__s32 exit_code;
	__u32 pid;
	__u32 _pad;
};

/* Batch page request: request multiple pages at once */
struct uck_batch_page_req {
	__u64 region_id;
	__u32 nr_pages;
	__u32 _pad;
	__u64 offsets[UCK_BATCH_MAX_PAGES]; /* Page-aligned offsets */
};

/* Batch page response header: followed by nr_pages * PAGE_SIZE data */
struct uck_batch_page_resp {
	__u64 region_id;
	__u32 nr_pages;
	__u32 flags;             /* 0 = success */
};

/* Node announce payload (for dynamic join) */
struct uck_node_announce {
	struct uck_node_info info;
	struct uck_node_stats stats;
	__u32 epoch;            /* Cluster epoch of announcing node */
	__u32 _ann_pad;
};

/* Futex operation request (cross-node) */
struct uck_futex_req {
	__u64 region_id;
	__u64 offset;            /* Byte offset within region */
	__u32 op;                /* UCK_FUTEX_WAIT or UCK_FUTEX_WAKE */
	__u32 val;               /* Expected value (WAIT) or nr to wake (WAKE) */
};

#define UCK_FUTEX_WAIT  0
#define UCK_FUTEX_WAKE  1

/* cgroup stats per node (exchanged via UCK_MSG_CGROUP_STATS) */
struct uck_cgroup_stats {
	__u32 node_id;
	__u32 nr_tasks;          /* Tasks in UCK cgroup on this node */
	__u64 cpu_usage_ns;      /* Total CPU time in nanoseconds */
	__u64 mem_usage;         /* Memory usage in bytes */
	__u64 mem_limit;         /* Memory limit in bytes (0 = unlimited) */
};

/* Wire protocol header */
struct uck_msg_hdr {
	__u32 type;             /* enum uck_msg_type */
	__u32 src_node;
	__u64 region_id;
	__u64 page_offset;      /* Offset within region (page-aligned) */
	__u32 payload_len;      /* Length of data following header */
	__u32 flags;
	__u32 epoch;            /* Cluster epoch for quorum/fencing */
};

#define UCK_MSG_HDR_SIZE sizeof(struct uck_msg_hdr)

/*
 * Serialized process state header — sent with UCK_MSG_PROC_STATE.
 * Followed by payload_len bytes of VMA descriptors, then page data.
 */
struct uck_proc_state_hdr {
	__u32 src_node;
	__u32 pid;
	__u64 nr_vmas;
	__u64 nr_pages;

	/* Saved registers (x86_64 pt_regs subset) */
	__u64 ip;               /* RIP */
	__u64 sp;               /* RSP */
	__u64 flags;            /* RFLAGS */
	__u64 ax, bx, cx, dx;
	__u64 si, di, bp;
	__u64 r8, r9, r10, r11, r12, r13, r14, r15;
	__u64 cs, ss;

	/* File descriptors */
	__u32 nr_fds;
	__u32 _pad;
	__u32 crc32;            /* CRC-32 of serialized state */
};

/* VMA descriptor in serialized state */
struct uck_vma_desc {
	__u64 start;
	__u64 end;
	__u64 flags;            /* VM_READ|VM_WRITE|VM_EXEC etc. */
	__u64 pgoff;
	__u32 is_anon;          /* 1 = anonymous, 0 = file-backed */
	__u32 _pad;
	char  path[256];        /* File path for file-backed, empty for anon */
};

/* FD descriptor in serialized state */
struct uck_fd_desc {
	__u32 fd_num;
	__u32 flags;            /* O_RDONLY, etc. */
	__u64 pos;              /* File position */
	char  path[256];
};

/* SMP mode request */
struct uck_smp_req {
	__u32 pid;              /* PID to enable SMP for (0 = calling process) */
	__u32 flags;
};

/* ioctl commands */
#define UCK_IOC_MAGIC 'U'

#define UCK_IOC_SET_NODE       _IOW(UCK_IOC_MAGIC, 1, struct uck_node_info)
#define UCK_IOC_ADD_NODE       _IOW(UCK_IOC_MAGIC, 2, struct uck_node_info)
#define UCK_IOC_CREATE_REGION  _IOW(UCK_IOC_MAGIC, 3, struct uck_region_info)
#define UCK_IOC_JOIN_REGION    _IOW(UCK_IOC_MAGIC, 4, struct uck_region_info)
#define UCK_IOC_GET_STATUS     _IOR(UCK_IOC_MAGIC, 5, struct uck_node_info)
#define UCK_IOC_MIGRATE_PROC   _IOW(UCK_IOC_MAGIC, 6, struct uck_migrate_req)
#define UCK_IOC_GET_CLUSTER    _IOR(UCK_IOC_MAGIC, 7, struct uck_cluster_info)
#define UCK_IOC_REMOTE_EXEC    _IOWR(UCK_IOC_MAGIC, 8, struct uck_remote_exec_req)
#define UCK_IOC_GET_JOBS       _IOWR(UCK_IOC_MAGIC, 9, struct uck_job_query)
#define UCK_IOC_ENABLE_SMP     _IOW(UCK_IOC_MAGIC, 10, struct uck_smp_req)
#define UCK_IOC_FUTEX          _IOWR(UCK_IOC_MAGIC, 11, struct uck_futex_req)
#define UCK_IOC_NODE_LEAVE     _IO(UCK_IOC_MAGIC, 12)
#define UCK_IOC_GET_CGROUP     _IOR(UCK_IOC_MAGIC, 13, struct uck_cgroup_stats)
#define UCK_IOC_SET_REGION_ACL _IOW(UCK_IOC_MAGIC, 14, struct uck_region_acl)
#define UCK_IOC_FORCE_NODE_STATE _IOW(UCK_IOC_MAGIC, 15, struct uck_node_info)

#endif /* _UCK_H_ */
