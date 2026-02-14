/*
 * uck_exec.c - Distributed execution engine for UCK
 *
 * Provides transparent CPU sharing across the cluster:
 *   - Submit a command, it runs on the least-loaded node
 *   - Job tracking with status, exit codes, and PID reporting
 *   - Remote nodes receive exec requests and run them locally
 *   - Results are sent back to the submitting node
 *
 * This gives the effect of a unified CPU pool: you submit work
 * and it runs on whichever node has spare capacity.
 */

#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/umh.h>

#include "uck_internal.h"

void uck_exec_init(void)
{
	mutex_init(&uck_state.jobs_lock);
	uck_state.next_job_id = 1;
	memset(uck_state.jobs, 0, sizeof(uck_state.jobs));
}

/* Find the least-loaded alive node (including local) */
u32 uck_pick_exec_node(void)
{
	struct uck_remote_node *best_remote = NULL;
	u32 best_load = UINT_MAX;
	u32 local_load;
	int i;

	uck_update_local_stats();
	local_load = uck_state.local_stats.load_avg;

	for (i = 0; i < uck_state.num_nodes; i++) {
		struct uck_remote_node *node = &uck_state.nodes[i];
		if (!node->alive)
			continue;
		if (node->stats.load_avg < best_load) {
			best_load = node->stats.load_avg;
			best_remote = node;
		}
	}

	/* If no remote node is less loaded, run locally */
	if (!best_remote || local_load <= best_load)
		return uck_state.local_node.node_id;

	return best_remote->info.node_id;
}

static int uck_alloc_job_slot(void)
{
	int i;
	for (i = 0; i < UCK_MAX_JOBS; i++) {
		if (uck_state.jobs[i].state == UCK_JOB_EMPTY ||
		    uck_state.jobs[i].state == UCK_JOB_DONE ||
		    uck_state.jobs[i].state == UCK_JOB_FAILED)
			return i;
	}
	return -1;
}

/*
 * Execute a command locally using call_usermodehelper.
 * Runs: /bin/sh -c "<command>"
 * This is a kthread that waits for completion and updates job state.
 */
struct uck_local_exec_ctx {
	u32 job_id;
	int slot;
	char command[512];
	u32 src_node;       /* Node that submitted the job */
};

static int uck_local_exec_thread(void *data)
{
	struct uck_local_exec_ctx *ctx = data;
	char *argv[] = { "/bin/sh", "-c", ctx->command, NULL };
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin",
			 "TERM=linux", NULL };
	int ret;

	pr_info("uck: exec job %u: running '%s'\n", ctx->job_id, ctx->command);

	mutex_lock(&uck_state.jobs_lock);
	uck_state.jobs[ctx->slot].state = UCK_JOB_RUNNING;
	mutex_unlock(&uck_state.jobs_lock);

	/* UMH_WAIT_PROC returns wait status: real exit = (ret >> 8) & 0xff */
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret >= 0)
		ret = (ret >> 8) & 0xff;

	pr_info("uck: exec job %u: finished with code %d\n",
		ctx->job_id, ret);

	mutex_lock(&uck_state.jobs_lock);
	uck_state.jobs[ctx->slot].exit_code = ret;
	uck_state.jobs[ctx->slot].state =
		(ret == 0) ? UCK_JOB_DONE : UCK_JOB_FAILED;
	mutex_unlock(&uck_state.jobs_lock);

	/* If this job was submitted from a remote node, send result back */
	if (ctx->src_node != uck_state.local_node.node_id) {
		struct uck_msg_hdr hdr;
		struct uck_exec_result result;

		memset(&hdr, 0, sizeof(hdr));
		hdr.type = UCK_MSG_EXEC_DONE;
		hdr.src_node = uck_state.local_node.node_id;
		hdr.payload_len = sizeof(result);

		memset(&result, 0, sizeof(result));
		result.job_id = ctx->job_id;
		result.exit_code = ret;

		uck_net_send_to_node(ctx->src_node, &hdr, sizeof(hdr));
		uck_net_send_to_node(ctx->src_node, &result, sizeof(result));
	}

	kfree(ctx);
	return 0;
}

static int uck_exec_local(int slot, u32 job_id, const char *command,
			   u32 src_node)
{
	struct uck_local_exec_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->job_id = job_id;
	ctx->slot = slot;
	ctx->src_node = src_node;
	strscpy(ctx->command, command, sizeof(ctx->command));

	kthread_run(uck_local_exec_thread, ctx, "uck_job_%u", job_id);
	return 0;
}

/*
 * Submit a remote execution request.
 * Called from ioctl handler.
 * If dest_node is 0, auto-selects the least loaded node.
 */
int uck_submit_remote_exec(struct uck_remote_exec_req *req)
{
	u32 dest_node;
	u32 job_id;
	int slot;

	mutex_lock(&uck_state.jobs_lock);
	slot = uck_alloc_job_slot();
	if (slot < 0) {
		mutex_unlock(&uck_state.jobs_lock);
		return -ENOMEM;
	}

	job_id = uck_state.next_job_id++;
	memset(&uck_state.jobs[slot], 0, sizeof(uck_state.jobs[slot]));
	uck_state.jobs[slot].job_id = job_id;
	uck_state.jobs[slot].state = UCK_JOB_PENDING;
	uck_state.jobs[slot].exit_code = -1;
	uck_state.jobs[slot].src_node = uck_state.local_node.node_id;
	strscpy(uck_state.jobs[slot].command, req->command,
		sizeof(uck_state.jobs[slot].command));
	mutex_unlock(&uck_state.jobs_lock);

	/* Pick destination */
	if (req->dest_node == 0)
		dest_node = uck_pick_exec_node();
	else
		dest_node = req->dest_node;

	mutex_lock(&uck_state.jobs_lock);
	uck_state.jobs[slot].node_id = dest_node;
	mutex_unlock(&uck_state.jobs_lock);

	pr_info("uck: submitting job %u '%s' to node %u\n",
		job_id, req->command, dest_node);

	/* Store the job_id back into dest_node field for userspace to read */
	req->dest_node = job_id;

	if (dest_node == uck_state.local_node.node_id) {
		/* Run locally */
		return uck_exec_local(slot, job_id, req->command,
				      uck_state.local_node.node_id);
	} else {
		/* Send to remote node */
		struct uck_msg_hdr hdr;
		struct uck_exec_wire wire;
		int ret;

		memset(&hdr, 0, sizeof(hdr));
		hdr.type = UCK_MSG_EXEC_REQ;
		hdr.src_node = uck_state.local_node.node_id;
		hdr.payload_len = sizeof(wire);

		memset(&wire, 0, sizeof(wire));
		wire.job_id = job_id;
		wire.src_node = uck_state.local_node.node_id;
		strscpy(wire.command, req->command, sizeof(wire.command));
		if (req->workdir[0])
			strscpy(wire.workdir, req->workdir,
				sizeof(wire.workdir));

		ret = uck_net_send_to_node(dest_node, &hdr, sizeof(hdr));
		if (ret < 0) {
			mutex_lock(&uck_state.jobs_lock);
			uck_state.jobs[slot].state = UCK_JOB_FAILED;
			uck_state.jobs[slot].exit_code = ret;
			mutex_unlock(&uck_state.jobs_lock);
			return ret;
		}
		ret = uck_net_send_to_node(dest_node, &wire, sizeof(wire));
		if (ret < 0) {
			mutex_lock(&uck_state.jobs_lock);
			uck_state.jobs[slot].state = UCK_JOB_FAILED;
			uck_state.jobs[slot].exit_code = ret;
			mutex_unlock(&uck_state.jobs_lock);
			return ret;
		}

		return 0;
	}
}

/* Query job status */
int uck_query_jobs(struct uck_job_query *query)
{
	int i, count = 0;

	mutex_lock(&uck_state.jobs_lock);
	if (query->job_id != 0) {
		/* Query specific job */
		for (i = 0; i < UCK_MAX_JOBS && count < 16; i++) {
			if (uck_state.jobs[i].job_id == query->job_id &&
			    uck_state.jobs[i].state != UCK_JOB_EMPTY) {
				query->jobs[count++] = uck_state.jobs[i];
				break;
			}
		}
	} else {
		/* List all active jobs */
		for (i = 0; i < UCK_MAX_JOBS && count < 16; i++) {
			if (uck_state.jobs[i].state != UCK_JOB_EMPTY)
				query->jobs[count++] = uck_state.jobs[i];
		}
	}
	query->count = count;
	mutex_unlock(&uck_state.jobs_lock);
	return 0;
}

/*
 * Handle incoming UCK_MSG_EXEC_REQ from a remote node.
 * We run the command locally and send the result back.
 */
void uck_handle_exec_req(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_exec_wire wire;
	int ret, slot;

	if (hdr->payload_len != sizeof(wire)) {
		pr_warn("uck: exec_req: bad payload len %u\n",
			hdr->payload_len);
		return;
	}

	ret = uck_sock_recv(client, &wire, sizeof(wire));
	if (ret < 0)
		return;

	pr_info("uck: received exec request job %u from node %u: '%s'\n",
		wire.job_id, wire.src_node, wire.command);

	/* Create a local job entry */
	mutex_lock(&uck_state.jobs_lock);
	slot = uck_alloc_job_slot();
	if (slot < 0) {
		mutex_unlock(&uck_state.jobs_lock);
		pr_err("uck: no free job slots for remote exec\n");
		return;
	}

	memset(&uck_state.jobs[slot], 0, sizeof(uck_state.jobs[slot]));
	uck_state.jobs[slot].job_id = wire.job_id;
	uck_state.jobs[slot].state = UCK_JOB_PENDING;
	uck_state.jobs[slot].exit_code = -1;
	uck_state.jobs[slot].node_id = uck_state.local_node.node_id;
	uck_state.jobs[slot].src_node = wire.src_node;
	strscpy(uck_state.jobs[slot].command, wire.command,
		sizeof(uck_state.jobs[slot].command));
	mutex_unlock(&uck_state.jobs_lock);

	/* Execute locally, result will be sent back by the exec thread */
	uck_exec_local(slot, wire.job_id, wire.command, wire.src_node);
}

/* Handle UCK_MSG_EXEC_STARTED from remote (ack that job started) */
void uck_handle_exec_started(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_exec_result result;
	int i, ret;

	if (hdr->payload_len != sizeof(result))
		return;

	ret = uck_sock_recv(client, &result, sizeof(result));
	if (ret < 0)
		return;

	mutex_lock(&uck_state.jobs_lock);
	for (i = 0; i < UCK_MAX_JOBS; i++) {
		if (uck_state.jobs[i].job_id == result.job_id) {
			uck_state.jobs[i].state = UCK_JOB_RUNNING;
			uck_state.jobs[i].pid = result.pid;
			break;
		}
	}
	mutex_unlock(&uck_state.jobs_lock);
}

/* Handle UCK_MSG_EXEC_DONE from remote (job completed) */
void uck_handle_exec_done(struct socket *client, struct uck_msg_hdr *hdr)
{
	struct uck_exec_result result;
	int i, ret;

	if (hdr->payload_len != sizeof(result))
		return;

	ret = uck_sock_recv(client, &result, sizeof(result));
	if (ret < 0)
		return;

	pr_info("uck: remote job %u completed with exit code %d\n",
		result.job_id, result.exit_code);

	mutex_lock(&uck_state.jobs_lock);
	for (i = 0; i < UCK_MAX_JOBS; i++) {
		if (uck_state.jobs[i].job_id == result.job_id) {
			uck_state.jobs[i].exit_code = result.exit_code;
			uck_state.jobs[i].state =
				(result.exit_code == 0) ? UCK_JOB_DONE
							: UCK_JOB_FAILED;
			break;
		}
	}
	mutex_unlock(&uck_state.jobs_lock);
}
