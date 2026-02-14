# UCK — Unified Compute Kernel (SMP Hypervisor)

A Linux kernel module that turns multiple QEMU VMs into a single SMP-like
system with transparent shared memory, automatic fork distribution across
nodes, distributed command execution, and unified resource reporting.

When you run a program under `uck_smp`, its `fork()`/`clone()` calls are
intercepted at the kernel level and child processes are automatically placed
on remote nodes — giving you the effect of a single machine with the combined
CPUs and memory of your entire cluster.

## Features

### SMP Hypervisor (fork interception)
- **Transparent fork distribution**: Kretprobe hooks `kernel_clone()` to
  intercept `fork()`/`clone()` syscalls at the kernel level
- **Round-robin scheduling**: Child processes are distributed across all
  alive cluster nodes automatically
- **Zero code changes**: Existing programs work unmodified — just run them
  under `uck_smp`
- **Per-process opt-in**: Only processes registered via `UCK_IOC_ENABLE_SMP`
  (or launched with `uck_smp`) have their forks distributed
- **Workqueue-based migration**: Fork interception schedules async migration
  to avoid blocking in probe context

### Distributed Execution Engine
- **Remote command execution**: Submit commands via ioctl or CLI; they run
  on the least-loaded node
- **Auto-routing**: Dest node 0 = automatic selection based on load
- **Job tracking**: Full job lifecycle tracking with status, exit codes,
  node assignment, visible via `/proc/uck/jobs`
- **Parallel submission**: `uck_run -p N` runs N copies of a command across
  the cluster simultaneously

### Shared Memory (DSM)
- **Transparent page sharing**: Applications `mmap()` shared regions that
  are backed by a custom fault handler
- **Remote page fetch**: On page fault, the kernel module fetches the page
  from the owning node over TCP
- **RB-tree page cache**: Per-region page tracking with shared/modified/
  invalid states
- **Verified**: 64 MB shared memory test — all 16,384 pages transferred
  correctly between nodes

### Process Migration
- **Full checkpoint**: Captures registers (all x86_64 pt_regs), VMAs
  (via maple tree iterator), and file descriptors
- **Network transfer**: Serialized state + anonymous pages sent over TCP
- **Userspace restore**: `uck_restore` stub reconstructs the process using
  `mmap()`, `setcontext()`, and file descriptor reopening
- **Load balancer**: Automatic threshold-based migration when local load
  exceeds 1.5x cluster average

### Cluster Management
- **Heartbeat**: 2-second interval stats exchange with 6-second failure
  detection timeout
- **Unified reporting**: `/proc/uck/{nodes,cpuinfo,meminfo,status,jobs}`
  shows aggregate cluster resources
- **CLI tools**: `uckctl status`, `uckctl nodes`, `uckctl jobs`,
  `uckctl exec`, `uckctl migrate`

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                         User Space                           │
│                                                              │
│  uckd       uckctl      uck_run     uck_smp     uck_test     │
│  (daemon)   (CLI)       (parallel)  (SMP wrap)  (DSM test)   │
│    │          │            │           │                      │
│    └──────────┴────────────┴───────────┴── /dev/uck (ioctl)   │
└──────────────────────────────────────────────────────────────┘
        │                     │                  │
┌───────┴─────────────────────┴──────────────────┴─────────────┐
│               UCK Kernel Module (uck.ko)                      │
│                                                              │
│  ┌─────────────┐  ┌─────────────────┐  ┌──────────────────┐  │
│  │   Shared    │  │  SMP Hypervisor  │  │   Distributed    │  │
│  │   Memory    │  │  (kretprobe on   │  │   Execution      │  │
│  │   (DSM)     │  │   kernel_clone)  │  │   Engine         │  │
│  └─────────────┘  └─────────────────┘  └──────────────────┘  │
│  ┌─────────────┐  ┌─────────────────┐  ┌──────────────────┐  │
│  │  Process    │  │   Heartbeat /   │  │    Resource      │  │
│  │  Migration  │  │   Node Health   │  │    Reporting     │  │
│  └─────────────┘  └─────────────────┘  └──────────────────┘  │
│  ┌─────────────┐  ┌─────────────────┐  ┌──────────────────┐  │
│  │   TCP       │  │  Load Balancer  │  │  Job Tracking    │  │
│  │   Network   │  │  (auto-migrate) │  │  /proc/uck/*     │  │
│  └─────────────┘  └─────────────────┘  └──────────────────┘  │
└──────────────────────────────────────────────────────────────┘
                           │
                    TCP (port 9999)
                           │
┌──────────────────────────┴───────────────────────────────────┐
│                     Remote UCK Nodes                          │
│         (identical uck.ko + uckd on each VM)                  │
└──────────────────────────────────────────────────────────────┘
```

## Installation

### From APT Repository (Recommended)

```bash
# Add the GPG key
curl -fsSL https://amosdavis.github.io/uckSMP-hypervisor/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/ucksmp.gpg

# Add the repository
echo "deb [signed-by=/usr/share/keyrings/ucksmp.gpg] \
  https://amosdavis.github.io/uckSMP-hypervisor stable main" \
  | sudo tee /etc/apt/sources.list.d/ucksmp.list

# Install kernel module (DKMS) + userspace tools
sudo apt update
sudo apt install ucksmp-dkms ucksmp-tools
```

The DKMS package automatically builds `uck.ko` for your running kernel
and rebuilds it whenever you install a new kernel.

### From GitHub Releases

Download `.deb` packages from the
[Releases page](https://github.com/amosdavis/uckSMP-hypervisor/releases)
and install with `sudo dpkg -i ucksmp-dkms_*.deb ucksmp-tools_*.deb`.

## Quick Start

### Prerequisites

- Debian Bookworm or later (x86_64)
- Kernel headers installed (`apt install linux-headers-$(uname -r)`)
- QEMU for multi-VM clusters (`apt install qemu-system-x86`)

### Building from Source

```bash
# Build kernel module + userspace tools
make

# Or build only userspace tools
make tools

# Install into a target directory
make install DESTDIR=/mnt/node1
```

### Building in a VM (chroot method)

```bash
# Mount a VM image and build inside it (kernel headers required)
mount -o loop node1.img /mnt/node1
mount --bind /proc /mnt/node1/proc
mount --bind /sys /mnt/node1/sys
mount --bind /dev /mnt/node1/dev

# Copy sources into the image
cp *.c *.h Kbuild Makefile /mnt/node1/root/dsm/

# Build kernel module + userspace tools
chroot /mnt/node1 bash -c "cd /root/dsm && make"

# Install
chroot /mnt/node1 bash -c "cd /root/dsm && make install DESTDIR=/"

# Clean up
umount /mnt/node1/{dev,sys,proc}
umount /mnt/node1
```

### Configuring Nodes

Each node runs `uckd` at boot (typically via `/etc/rc.local`):

```bash
# Node 1 (10.4.4.100)
insmod /lib/modules/6.1.0-42-amd64/extra/uck.ko
uckd --node-id 1 --ip 10.4.4.100 --port 9999 \
     --remote 2:10.4.4.101:9999 \
     --create-region 1:67108864 --daemon &

# Node 2 (10.4.4.101)
insmod /lib/modules/6.1.0-42-amd64/extra/uck.ko
uckd --node-id 2 --ip 10.4.4.101 --port 9999 \
     --remote 1:10.4.4.100:9999 \
     --join-region 1:67108864:1 --daemon &
```

### Usage

```bash
# Check cluster status
uckctl status
# UCK Cluster Status
# ==================
# Active Nodes:    2
# Total CPUs:      4
# Total Memory:    1920 MB

# Run a program with SMP fork distribution
uck_smp make -j4                    # compile jobs distributed across cluster
uck_smp bash -c 'for i in 1 2 3 4; do ./worker & done; wait'

# Execute a command on the least-loaded node
uckctl exec "echo hello"
uckctl exec "stress --cpu 2 --timeout 30"

# Run N parallel copies across the cluster
uck_run -p 4 "dd if=/dev/zero of=/dev/null bs=1M count=1000"

# Check job status
uckctl jobs
cat /proc/uck/jobs

# View per-node details
cat /proc/uck/nodes

# View cluster resources
cat /proc/uck/cpuinfo
cat /proc/uck/meminfo

# Test shared memory
uck_test write 1    # on node 1 (writes 64 MB)
uck_test read 2     # on node 2 (reads via remote page faults)

# Manually migrate a process
uckctl migrate <pid> <dest_node>
```

## Source Files

### Kernel Module

| File | Lines | Description |
|------|-------|-------------|
| `uck.h` | 240 | Public API — ioctls, wire protocol, shared structs |
| `uck_internal.h` | 130 | Kernel-internal structs, function declarations |
| `uck_main.c` | 360 | Module core — char device, mmap fault handler, ioctl dispatch |
| `uck_hyper.c` | 300 | **SMP hypervisor** — kretprobe fork interception, process distribution |
| `uck_exec.c` | 280 | **Distributed execution** — remote command dispatch, job tracking |
| `uck_net.c` | 475 | TCP networking — listener, page transfer, message dispatch |
| `uck_proc.c` | 410 | Process checkpoint — register capture, VMA/FD serialization |
| `uck_migrate.c` | 265 | Migration receive — state deserialization, restore stub launch |
| `uck_load.c` | 190 | Load balancer — threshold-based automatic migration |
| `uck_heartbeat.c` | 160 | Node health — periodic stats exchange, failure detection |
| `uck_sysinfo.c` | 200 | Procfs — `/proc/uck/{nodes,cpuinfo,meminfo,status,jobs}` |
| `uck_region.c` | 60 | Shared memory region management |
| `uck_page.c` | 80 | Per-page RB-tree cache |

### Userspace Tools

| File | Description |
|------|-------------|
| `uckd.c` | Daemon — configures kernel module via ioctls at boot |
| `uckctl.c` | CLI — `status`, `nodes`, `jobs`, `exec`, `migrate` commands |
| `uck_smp.c` | SMP wrapper — registers process then exec's target program |
| `uck_run.c` | Parallel runner — `-p N` for N copies, job wait, job listing |
| `uck_test.c` | Shared memory test — write/read 64 MB across nodes |
| `uck_restore.c` | Process restore stub — reconstructs migrated process in userspace |

### Build System

| File | Description |
|------|-------------|
| `Kbuild` | Kernel build — 12 object files → `uck.ko` |
| `Makefile` | Module + tools build, install target |
| `build_uck.sh` | Automated build script for WSL2 environment |
| `fix_rclocal.sh` | Updates `/etc/rc.local` on VM images for auto-start |

## How It Works

### SMP Fork Distribution

1. A process is registered for SMP via `uck_smp` (calls `UCK_IOC_ENABLE_SMP`)
2. The kernel module's kretprobe fires on every `kernel_clone()` call
3. If the calling process is registered, the child PID is captured
4. First fork stays local (avoid startup latency); subsequent forks are
   distributed round-robin across alive nodes
5. Migration is scheduled on a workqueue (can't block in probe context)
6. The child process is checkpointed (registers + memory + FDs) and sent
   to the target node over TCP
7. The remote node's `uck_restore` stub reconstructs the process

### Distributed Execution

1. User submits a command via `UCK_IOC_REMOTE_EXEC` ioctl
2. Module selects least-loaded node (or specified node)
3. If remote: sends `UCK_MSG_EXEC_REQ` with command over TCP
4. Remote node runs `/bin/sh -c "<command>"` via `call_usermodehelper`
5. On completion, sends `UCK_MSG_EXEC_DONE` with exit code back
6. Job table updated; visible via `/proc/uck/jobs`

### Shared Memory (DSM)

1. User calls `mmap()` on `/dev/uck` with region ID as offset
2. Pages start unmapped — first access triggers a page fault
3. Fault handler checks local RB-tree cache
4. Cache miss → TCP request to the owning node → page data returned
5. Page is cached locally as `UCK_PAGE_SHARED`
6. Subsequent accesses served from local cache

## Wire Protocol

All nodes communicate via TCP on port 9999. Messages use a fixed header:

```c
struct uck_msg_hdr {
    uint32_t type;          // Message type (see enum uck_msg_type)
    uint32_t src_node;      // Sender's node ID
    uint64_t region_id;     // Shared memory region (for page ops)
    uint64_t page_offset;   // Page offset within region
    uint32_t payload_len;   // Bytes following this header
    uint32_t flags;         // Type-specific flags
};
```

Message types: `PAGE_REQ/RESP`, `INVALIDATE/ACK`, `HEARTBEAT`,
`MIGRATE_REQ/ACCEPT/REJECT`, `PROC_STATE`, `PROC_PAGES`,
`EXEC_REQ/STARTED/DONE`.

## ioctl Interface

| ioctl | Direction | Description |
|-------|-----------|-------------|
| `UCK_IOC_SET_NODE` | Write | Set local node identity, start listener |
| `UCK_IOC_ADD_NODE` | Write | Register a remote node |
| `UCK_IOC_CREATE_REGION` | Write | Create a shared memory region |
| `UCK_IOC_JOIN_REGION` | Write | Join an existing remote region |
| `UCK_IOC_GET_CLUSTER` | Read | Get aggregate cluster stats |
| `UCK_IOC_MIGRATE_PROC` | Write | Migrate a process to a node |
| `UCK_IOC_REMOTE_EXEC` | R/W | Submit command, returns job ID |
| `UCK_IOC_GET_JOBS` | R/W | Query job status |
| `UCK_IOC_ENABLE_SMP` | Write | Register process for fork distribution |

## Target Environment

- **Kernel**: Linux 6.1 LTS (Debian Bookworm, `6.1.0-42-amd64`)
- **Arch**: x86_64
- **VMs**: QEMU with `-smp 2 -m 1024` and socket-based networking
- **Networking**: Private network (10.4.4.0/24), nodes on .100 and .101

## Linux 6.1 API Notes

This module handles several kernel 6.1 API differences vs older/newer kernels:

- `class_create()` requires 2 args: `class_create(THIS_MODULE, name)`
- VMA iteration uses maple tree: `struct vma_iterator` + `for_each_vma()`
- `get_user_pages_remote()` takes 7 args (pass NULL for locked param)
- `sock_setsockopt()` with `KERNEL_SOCKPTR()` instead of `kernel_setsockopt()`
- `nr_running()` not exported to modules — approximated with `num_online_cpus()`
- `call_usermodehelper()` requires `<linux/umh.h>`
- `find_task_by_vpid()` not exported — use `find_vpid()` + `pid_task()`
- kretprobe target is `kernel_clone` (not `_do_fork`)

## Limitations

- **Single-writer DSM**: No concurrent writes to the same shared page
  from different nodes (single-owner model)
- **Process migration**: Single-threaded processes only; multi-threaded
  processes are rejected
- **File descriptors**: Reopened by path on destination — pipes, sockets,
  and anonymous FDs are skipped
- **TCP transport**: Page-at-a-time fetching (~0.1 MB/s read throughput);
  no RDMA or batch transfer
- **fork distribution**: Migrated child processes depend on the process
  restore pipeline, which has limitations for complex processes
- **No distributed locking**: Shared memory has no built-in synchronization
  primitives between nodes
- **Prototype quality**: Not production-hardened; intended for research and
  experimentation

## Future Work

- Batch page transfer (multiple pages per TCP round trip)
- RDMA transport option for low-latency page fetch
- Write-fault interception for proper DSM coherence protocol
- Multi-threaded process migration support
- Distributed futex / cross-node synchronization
- Dynamic node join/leave protocol
- cgroup-based resource accounting for distributed jobs

## License

GPL v2 (required for Linux kernel modules)
