// SPDX-License-Identifier: GPL-2.0
/*
 * c2c_kfix: patches an L4T 5.10 kernel bug at runtime. We hit it when swapping
 * over the network (c2c-rmem), so this gets loaded on the master before any
 * remote memory is attached.
 *
 * What goes wrong: kthreads find their struct kthread via
 * task->set_child_tid (to_kthread()). kthreadd (PID 2) never goes through
 * kthread() so it doesn't have one, the pointer is NULL. If kthreadd ends up
 * in direct reclaim while forking a kthread it can swap a page out to nbd:
 * tcp_sendmsg -> sk_stream_wait_memory -> wait_woken ->
 * kthread_should_stop() -> NULL->flags and the box panics. Got this on the
 * Xavier:
 *     pc : kthread_should_stop+0x28/0x50
 *     sk_stream_wait_memory / tcp_sendmsg / nbd / swap_writepage / kthreadd
 * Mainline fixed it in 5.17 by giving every kthread (kthreadd too) a struct
 * kthread. I do the same thing here on the running kernel: kthreadd gets a
 * zeroed struct kthread, flags = 0, so should_stop/should_park just say no.
 *
 * Why this is safe:
 *  - only touches PID 2 if it's "kthreadd", has PF_KTHREAD and
 *    set_child_tid is still NULL
 *  - set_child_tid is only read for user tasks on their first schedule
 *    (schedule_tail). kthreadd is way past that, and kids don't inherit it
 *    (copy_process clears it unless CLONE_CHILD_SETTID)
 *  - nobody ever stops/parks kthreadd so the completions are never used
 *  - unload clears the pointer but leaks the memory on purpose, someone
 *    could still be reading it
 */
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

/* struct kthread lives in kernel/kthread.c so we can't see it. On 5.10 it's
 * under 160 bytes, just allocate way more and zero it */
#define KTHREAD_STRUCT_SPACE 512

static struct task_struct *kt_task;
static void *fake_kthread;

static int __init c2c_kfix_init(void)
{
	struct pid *pid = find_get_pid(2);
	struct task_struct *t = pid ? get_pid_task(pid, PIDTYPE_PID) : NULL;

	put_pid(pid);
	if (!t)
		return -ESRCH;
	if (strcmp(t->comm, "kthreadd") || !(t->flags & PF_KTHREAD)) {
		pr_err("c2c_kfix: PID 2 is '%s', not kthreadd; not touching it\n", t->comm);
		put_task_struct(t);
		return -ENODEV;
	}
	if (t->set_child_tid) {
		pr_info("c2c_kfix: kthreadd already has a struct kthread (kernel is fixed); nothing to do\n");
		put_task_struct(t);
		return 0;	/* stay loaded anyway, c2c-rmem checks for it */
	}
	fake_kthread = kzalloc(KTHREAD_STRUCT_SPACE, GFP_KERNEL);
	if (!fake_kthread) {
		put_task_struct(t);
		return -ENOMEM;
	}
	WRITE_ONCE(t->set_child_tid, (int __user *)fake_kthread);
	kt_task = t;	/* keep the reference */

	/* quick self-test: kthread_func() uses to_kthread(). Without the fix
	 * that's a NULL deref for kthreadd, now it should just return NULL */
	if (kthread_func(t) != NULL) {
		pr_err("c2c_kfix: self-test failed, reverting\n");
		WRITE_ONCE(t->set_child_tid, NULL);
		kt_task = NULL;
		put_task_struct(t);
		return -EIO;	/* leak fake_kthread, on purpose */
	}
	pr_info("c2c_kfix: kthreadd now has a struct kthread (set_child_tid @ +%#zx); "
		"swap-over-network panic in kthread_should_stop() fixed\n",
		offsetof(struct task_struct, set_child_tid));
	return 0;
}

static void __exit c2c_kfix_exit(void)
{
	if (kt_task) {
		WRITE_ONCE(kt_task->set_child_tid, NULL);
		put_task_struct(kt_task);
		pr_info("c2c_kfix: unloaded (kthreadd fix removed)\n");
	}
	/* don't free fake_kthread, a reader might still have it */
}

module_init(c2c_kfix_init);
module_exit(c2c_kfix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("C2C: give kthreadd a struct kthread (fixes kthread_should_stop NULL deref under swap-over-network)");
