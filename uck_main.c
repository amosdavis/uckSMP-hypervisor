/*
 * uck_main.c - Unified Compute Kernel module
 *
 * Core module: char device registration, ioctl handling, mmap with
 * custom fault handler for transparent remote page fetching.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>

#include "uck.h"
#include "uck_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("uck");
MODULE_DESCRIPTION("UCK: Unified Compute Kernel");
MODULE_VERSION("0.2");

static dev_t uck_devno;
static struct cdev uck_cdev;
static struct class *uck_class;
static struct device *uck_device;

/* Global UCK state */
struct uck_state uck_state;

/* ---- Page fault handler ---- */

static void uck_invalidate_remote_copies(struct uck_region *region,
					  pgoff_t page_index);

static vm_fault_t uck_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct uck_region *region = vma->vm_private_data;
	unsigned long offset;
	pgoff_t page_index;
	struct uck_page_entry *entry;
	struct page *page;
	void *kaddr;
	int ret;
	bool is_write;

	if (!region)
		return VM_FAULT_SIGBUS;

	offset = vmf->address - vma->vm_start;
	page_index = offset >> PAGE_SHIFT;
	is_write = (vmf->flags & FAULT_FLAG_WRITE) != 0;

	if (offset >= region->info.size) {
		pr_warn("uck: fault offset %lu >= region size %llu\n",
			offset, region->info.size);
		return VM_FAULT_SIGBUS;
	}

	pr_info_ratelimited("uck: fault region %llu page %lu %s\n",
			    region->info.region_id, page_index,
			    is_write ? "WRITE" : "READ");

	mutex_lock(&region->lock);

	/* Look up page in our local cache */
	entry = uck_page_lookup(region, page_index);

	if (entry && uck_page_get_state(entry) != UCK_PAGE_INVALID) {
		if (is_write && uck_page_get_state(entry) == UCK_PAGE_SHARED) {
			/*
			 * Write fault on a SHARED page: transition to
			 * EXCLUSIVE state. Must invalidate copies on
			 * other nodes before allowing the write.
			 */
			uck_invalidate_remote_copies(region, page_index);
			atomic_set(&entry->state, UCK_PAGE_EXCLUSIVE);
			entry->write_mapped = true;

			page = entry->page;
			get_page(page);
			mutex_unlock(&region->lock);
			vmf->page = page;
			return 0;
		}

		/* Read fault or already exclusive — serve from cache */
		page = entry->page;
		get_page(page);
		mutex_unlock(&region->lock);
		vmf->page = page;
		return 0;
	}

	/* Page not present locally - need to fetch from owner */
	if (!entry) {
		entry = uck_page_alloc_entry(region, page_index);
		if (!entry) {
			mutex_unlock(&region->lock);
			return VM_FAULT_OOM;
		}
	}

	/* Check if another CPU is already fetching this page */
	if (uck_page_get_state(entry) == UCK_PAGE_IN_TRANSIT) {
		mutex_unlock(&region->lock);
		/* Wait for the in-flight fetch to complete */
		wait_event(entry->waitq,
			   uck_page_get_state(entry) != UCK_PAGE_IN_TRANSIT);
		mutex_lock(&region->lock);
		if (entry->page && uck_page_get_state(entry) != UCK_PAGE_INVALID) {
			page = entry->page;
			get_page(page);
			mutex_unlock(&region->lock);
			vmf->page = page;
			return 0;
		}
		mutex_unlock(&region->lock);
		return VM_FAULT_SIGBUS;
	}

	/* Mark page as in-transit before releasing lock */
	uck_page_try_set_state(entry, UCK_PAGE_INVALID, UCK_PAGE_IN_TRANSIT);

	/* Check memory overcommit limits */
	if (!uck_memctl_can_alloc(1)) {
		uck_page_try_set_state(entry, UCK_PAGE_IN_TRANSIT,
				       UCK_PAGE_INVALID);
		wake_up_all(&entry->waitq);
		mutex_unlock(&region->lock);
		return VM_FAULT_OOM;
	}

	/* Allocate a local page */
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		uck_page_try_set_state(entry, UCK_PAGE_IN_TRANSIT,
				       UCK_PAGE_INVALID);
		wake_up_all(&entry->waitq);
		mutex_unlock(&region->lock);
		return VM_FAULT_OOM;
	}

	mutex_unlock(&region->lock);

	/* Try batch prefetch first, fall back to single page */
	if (uck_rdma_available()) {
		kaddr = kmap(page);
		ret = uck_rdma_fetch_page(region, page_index, kaddr);
		kunmap(page);
	} else {
		pgoff_t prefetch[UCK_BATCH_MAX_PAGES];
		int nr_prefetch;

		mutex_lock(&region->lock);
		nr_prefetch = uck_build_prefetch_list(region, page_index,
						      prefetch,
						      UCK_PREFETCH_WINDOW);
		mutex_unlock(&region->lock);

		if (nr_prefetch > 1) {
			ret = uck_net_fetch_pages_batch(region, prefetch,
							nr_prefetch);
			/* Check if faulting page was fetched */
			if (ret > 0) {
				mutex_lock(&region->lock);
				entry = uck_page_lookup(region, page_index);
				if (entry && entry->page) {
					__free_page(page);
					page = entry->page;
					get_page(page);
					if (is_write) {
						uck_invalidate_remote_copies(
							region, page_index);
						atomic_set(&entry->state,
							UCK_PAGE_EXCLUSIVE);
						entry->write_mapped = true;
					}
					mutex_unlock(&region->lock);
					vmf->page = page;
					return 0;
				}
				mutex_unlock(&region->lock);
			}
		}

		/* Fallback: single page fetch */
		kaddr = kmap(page);
		ret = uck_net_fetch_page(region, page_index, kaddr);
		kunmap(page);
	}

	if (ret < 0) {
		/* Network fetch failed - if we're the owner, page is just zero */
		if (region->info.owner_node == uck_state.local_node.node_id) {
			ret = 0;
		} else {
			pr_err("uck: fetch page %lu from owner failed: %d\n",
			       page_index, ret);
			uck_page_try_set_state(entry, UCK_PAGE_IN_TRANSIT,
					       UCK_PAGE_INVALID);
			wake_up_all(&entry->waitq);
			__free_page(page);
			return VM_FAULT_SIGBUS;
		}
	}

	SetPageUptodate(page);

	mutex_lock(&region->lock);
	entry->page = page;
	if (is_write) {
		atomic_set(&entry->state, UCK_PAGE_EXCLUSIVE);
		entry->write_mapped = true;
	} else {
		atomic_set(&entry->state, UCK_PAGE_SHARED);
		entry->write_mapped = false;
	}
	get_page(page);
	uck_memctl_account_alloc(1);
	wake_up_all(&entry->waitq);
	mutex_unlock(&region->lock);

	vmf->page = page;
	return 0;
}

/*
 * Invalidate remote copies of a page before allowing a local write.
 * Sends UCK_MSG_INVALIDATE to all nodes that might have a copy.
 */
static void uck_invalidate_remote_copies(struct uck_region *region,
					  pgoff_t page_index)
{
	struct uck_msg_hdr inv;
	int i;

	memset(&inv, 0, sizeof(inv));
	inv.type = UCK_MSG_INVALIDATE;
	inv.src_node = uck_state.local_node.node_id;
	inv.region_id = region->info.region_id;
	inv.page_offset = (u64)page_index << PAGE_SHIFT;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		uck_net_send_to_node(node->info.node_id, &inv, sizeof(inv));
	}
}

static vm_fault_t uck_page_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct uck_region *region = vma->vm_private_data;
	unsigned long offset;
	pgoff_t page_index;
	struct uck_page_entry *entry;

	if (!region)
		return VM_FAULT_SIGBUS;

	offset = vmf->address - vma->vm_start;
	page_index = offset >> PAGE_SHIFT;

	pr_info_ratelimited("uck: page_mkwrite region %llu page %lu\n",
			    region->info.region_id, page_index);

	mutex_lock(&region->lock);
	entry = uck_page_lookup(region, page_index);
	if (entry && uck_page_get_state(entry) == UCK_PAGE_SHARED) {
		uck_invalidate_remote_copies(region, page_index);
		atomic_set(&entry->state, UCK_PAGE_EXCLUSIVE);
		entry->write_mapped = true;
	}
	mutex_unlock(&region->lock);

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct uck_vm_ops = {
	.fault = uck_fault,
	.page_mkwrite = uck_page_mkwrite,
};

/* ---- File operations ---- */

static int uck_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int uck_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int uck_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct uck_region *region;
	u64 region_id;
	unsigned long size;

	/*
	 * vm_pgoff encodes the region_id.
	 * The user does: mmap(NULL, size, ..., fd, region_id * PAGE_SIZE)
	 */
	region_id = vma->vm_pgoff;

	mutex_lock(&uck_state.lock);
	region = uck_find_region(region_id);
	mutex_unlock(&uck_state.lock);

	if (!region) {
		pr_err("uck: mmap: region %llu not found\n", region_id);
		return -EINVAL;
	}

	size = vma->vm_end - vma->vm_start;
	if (size > region->info.size) {
		pr_err("uck: mmap: requested %lu but region is %llu\n",
		       size, region->info.size);
		return -EINVAL;
	}

	/* Don't pre-populate - let faults handle it */
	uck_vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY | VM_IO);
	vma->vm_ops = &uck_vm_ops;
	vma->vm_private_data = region;

	pr_info("uck: mmap region %llu, size %lu\n", region_id, size);
	return 0;
}

static long uck_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	if (!uck_ratelimit_check_ioctl()) {
		pr_warn_ratelimited("uck: ioctl rate limit exceeded\n");
		return -EBUSY;
	}

	switch (cmd) {
	case UCK_IOC_SET_NODE: {
		struct uck_node_info info;
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		mutex_lock(&uck_state.lock);
		uck_state.local_node = info;
		uck_state.initialized = true;
		mutex_unlock(&uck_state.lock);
		pr_info("uck: local node set to id=%u ip=0x%08x port=%u\n",
			info.node_id, info.ip_addr, info.port);
		/* Start the network listener */
		ret = uck_net_start_listener(info.port);
		if (ret == 0) {
			/* Start heartbeat and load balancer after listener */
			uck_heartbeat_init();
			uck_loadbal_init();
		}
		break;
	}

	case UCK_IOC_ADD_NODE: {
		struct uck_node_info info;
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		mutex_lock(&uck_state.lock);
		ret = uck_add_remote_node(&info);
		mutex_unlock(&uck_state.lock);
		if (ret == 0)
			pr_info("uck: added remote node id=%u ip=0x%08x\n",
				info.node_id, info.ip_addr);
		break;
	}

	case UCK_IOC_CREATE_REGION: {
		struct uck_region_info rinfo;
		if (copy_from_user(&rinfo, (void __user *)arg, sizeof(rinfo)))
			return -EFAULT;
		if (rinfo.size == 0 || (rinfo.size & ~PAGE_MASK))
			return -EINVAL;
		mutex_lock(&uck_state.lock);
		ret = uck_create_region(&rinfo);
		mutex_unlock(&uck_state.lock);
		if (ret == 0)
			pr_info("uck: created region %llu, size %llu, owner %u\n",
				rinfo.region_id, rinfo.size, rinfo.owner_node);
		break;
	}

	case UCK_IOC_JOIN_REGION: {
		struct uck_region_info rinfo;
		if (copy_from_user(&rinfo, (void __user *)arg, sizeof(rinfo)))
			return -EFAULT;
		mutex_lock(&uck_state.lock);
		ret = uck_join_region(&rinfo);
		mutex_unlock(&uck_state.lock);
		if (ret == 0)
			pr_info("uck: joined region %llu\n", rinfo.region_id);
		break;
	}

	case UCK_IOC_MIGRATE_PROC: {
		struct uck_migrate_req mreq;
		if (copy_from_user(&mreq, (void __user *)arg, sizeof(mreq)))
			return -EFAULT;
		ret = uck_migrate_process(mreq.pid, mreq.dest_node);
		break;
	}

	case UCK_IOC_GET_CLUSTER: {
		struct uck_cluster_info cinfo;
		uck_get_cluster_info(&cinfo);
		if (copy_to_user((void __user *)arg, &cinfo, sizeof(cinfo)))
			return -EFAULT;
		break;
	}

	case UCK_IOC_REMOTE_EXEC: {
		struct uck_remote_exec_req ereq;
		if (copy_from_user(&ereq, (void __user *)arg, sizeof(ereq)))
			return -EFAULT;
		ereq.command[sizeof(ereq.command) - 1] = '\0';
		ereq.workdir[sizeof(ereq.workdir) - 1] = '\0';
		ret = uck_submit_remote_exec(&ereq);
		if (ret == 0) {
			/* Copy back with job_id in dest_node field */
			if (copy_to_user((void __user *)arg, &ereq,
					 sizeof(ereq)))
				return -EFAULT;
		}
		break;
	}

	case UCK_IOC_GET_JOBS: {
		struct uck_job_query *jquery;
		jquery = kzalloc(sizeof(*jquery), GFP_KERNEL);
		if (!jquery)
			return -ENOMEM;
		if (copy_from_user(jquery, (void __user *)arg,
				   sizeof(*jquery))) {
			kfree(jquery);
			return -EFAULT;
		}
		uck_query_jobs(jquery);
		if (copy_to_user((void __user *)arg, jquery,
				 sizeof(*jquery))) {
			kfree(jquery);
			return -EFAULT;
		}
		kfree(jquery);
		break;
	}

	case UCK_IOC_ENABLE_SMP: {
		struct uck_smp_req sreq;
		if (copy_from_user(&sreq, (void __user *)arg, sizeof(sreq)))
			return -EFAULT;
		if (sreq.pid == 0)
			sreq.pid = current->pid;
		ret = uck_hyper_register_pid(sreq.pid);
		break;
	}

	case UCK_IOC_FUTEX: {
		struct uck_futex_req freq;
		if (copy_from_user(&freq, (void __user *)arg, sizeof(freq)))
			return -EFAULT;
		ret = uck_futex_op(&freq);
		break;
	}

	case UCK_IOC_NODE_LEAVE:
		ret = uck_node_leave_graceful();
		break;

	case UCK_IOC_GET_CGROUP: {
		struct uck_cgroup_stats cstats;
		uck_cgroup_update_local();
		cstats = uck_state.local_cgroup_stats;
		if (copy_to_user((void __user *)arg, &cstats, sizeof(cstats)))
			return -EFAULT;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations uck_fops = {
	.owner          = THIS_MODULE,
	.open           = uck_open,
	.release        = uck_release,
	.mmap           = uck_mmap,
	.unlocked_ioctl = uck_ioctl,
};

/* ---- Module init/exit ---- */

static int __init uck_init(void)
{
	int ret;

	pr_info("uck: Unified Compute Kernel module loading\n");

	memset(&uck_state, 0, sizeof(uck_state));
	mutex_init(&uck_state.lock);

	/* Allocate char device number */
	ret = alloc_chrdev_region(&uck_devno, 0, 1, UCK_DEVICE_NAME);
	if (ret < 0) {
		pr_err("uck: failed to alloc chrdev region\n");
		return ret;
	}

	/* Init and add cdev */
	cdev_init(&uck_cdev, &uck_fops);
	uck_cdev.owner = THIS_MODULE;
	ret = cdev_add(&uck_cdev, uck_devno, 1);
	if (ret < 0) {
		pr_err("uck: failed to add cdev\n");
		goto err_cdev;
	}

	/* Create device class and device node */
	uck_class = uck_class_create(UCK_DEVICE_NAME);
	if (IS_ERR(uck_class)) {
		ret = PTR_ERR(uck_class);
		pr_err("uck: failed to create class\n");
		goto err_class;
	}

	uck_device = device_create(uck_class, NULL, uck_devno, NULL,
				   UCK_DEVICE_NAME);
	if (IS_ERR(uck_device)) {
		ret = PTR_ERR(uck_device);
		pr_err("uck: failed to create device\n");
		goto err_device;
	}

	pr_info("uck: module loaded, /dev/%s created (major %d)\n",
		UCK_DEVICE_NAME, MAJOR(uck_devno));

	/* Create /proc/uck */
	uck_sysinfo_init();

	/* Initialize exec subsystem */
	uck_exec_init();

	/* Initialize hypervisor layer */
	ret = uck_hyper_init();
	if (ret < 0)
		pr_warn("uck: hypervisor init failed: %d (non-fatal)\n", ret);

	/* Initialize distributed futex */
	uck_futex_init();

	/* Initialize cgroup accounting */
	uck_cgroup_init();

	/* Initialize RDMA transport (falls back to TCP if unavailable) */
	uck_rdma_init();

	/* Security hardening subsystems */
	uck_audit_init();
	uck_ratelimit_init();
	uck_quorum_init();
	uck_memctl_init();

	return 0;

err_device:
	class_destroy(uck_class);
err_class:
	cdev_del(&uck_cdev);
err_cdev:
	unregister_chrdev_region(uck_devno, 1);
	return ret;
}

static void __exit uck_exit(void)
{
	uck_memctl_exit();
	uck_quorum_exit();
	uck_ratelimit_exit();
	uck_audit_exit();
	uck_rdma_exit();
	uck_cgroup_exit();
	uck_futex_exit();
	uck_hyper_stop();
	uck_loadbal_stop();
	uck_heartbeat_stop();
	uck_net_stop();
	uck_cleanup_regions();
	uck_sysinfo_exit();

	device_destroy(uck_class, uck_devno);
	class_destroy(uck_class);
	cdev_del(&uck_cdev);
	unregister_chrdev_region(uck_devno, 1);

	pr_info("uck: module unloaded\n");
}

module_init(uck_init);
module_exit(uck_exit);
