#include <linux/version.h>
#include <linux/swap.h>
#include <linux/sched.h>
#include <linux/kthread.h> // If needed for other parts of your code
#include <trace/hooks/binder.h>
#include <uapi/linux/android/binder.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/prio.h>
#include <linux/namei.h>
#include <../../android/binder_internal.h>
#include <../../../kernel/sched/sched.h>
#include <linux/string.h>
#include <linux/kobject.h>  // For sysfs access
#include <linux/fs.h>      // For file operations
#include <linux/slab.h>    // For kmalloc/kfree
#include <linux/kallsyms.h> // For property_get
#include <linux/module.h> // For MODULE_NAME

#ifdef CONFIG_BINDER_PRIO_DEBUG
#include <linux/module.h>

static uint __read_mostly debug = 0;
module_param(debug, uint, 0644);
#endif

static bool __read_mostly is_miui_rom = false;
static const char *miui_framework = "/system/framework/MiuiBooster.jar";

#define KERNEL_VERSION_5_4_0 KERNEL_VERSION(5, 4, 0)
#define KERNEL_VERSION_5_10_0 KERNEL_VERSION(5, 10, 0)

static const char *task_name[] = {
	"droid.launcher3",  // com.android.launcher3
	"ndroid.systemui",  // com.android.systemui
	// "surfaceflinger",
	"cameraserver",
};

static const char *task_name_miui[] = {
	"com.miui.home",
	".globallauncher",  // com.mi.android.globallauncher
	"rsonalassistant",  // com.miui.personalassistant
};

static int to_userspace_prio(int policy, int kernel_priority) {
	if (fair_policy(policy))
		return PRIO_TO_NICE(kernel_priority);
	else
		return MAX_USER_RT_PRIO - 1 - kernel_priority;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_4_0 && LINUX_VERSION_CODE < KERNEL_VERSION_5_10_0
static void set_binder_task_priority(struct task_struct *task, int policy, int priority) {
	struct sched_param param;
	param.sched_priority = to_userspace_prio(policy, priority);

	int ret = sched_setscheduler_nocheck(task, policy | SCHED_RESET_ON_FORK, &param);
	if (ret < 0) {
		printk(KERN_ERR "binder_prio: Failed to set priority for task %s (%d): %d\n", task->comm, task->pid, ret);
	}
}
#endif

static bool set_binder_rt_task(struct binder_transaction *t) {
	int i;

	if (t && t->from && t->from->task && t->to_proc && t->to_proc->tsk && (!(t->flags & TF_ONE_WAY)) &&
	    rt_policy(t->from->task->policy)) {
		#define from_task_comm    t->from->task->comm
		#define from_task_gl_comm t->from->task->group_leader->comm

		if (is_miui_rom) {
			if (!strncmp(from_task_gl_comm, "com.miui.home", strlen("com.miui.home")) &&
			    !strncmp(from_task_comm, "RenderThread", strlen("RenderThread")) &&
			    !strncmp(t->to_proc->tsk->comm, "surfaceflinger", strlen("surfaceflinger")))
				goto yes_and_exit;
			if (!strncmp(from_task_gl_comm, "surfaceflinger", strlen("surfaceflinger")) &&
			    !strncmp(from_task_comm, "passBlur", strlen("passBlur")))
				goto yes_and_exit;
		}
		if (!strncmp(from_task_gl_comm, "cameraserver", strlen("cameraserver")) &&
		    !strncmp(from_task_comm, "C3Dev-", strlen("C3Dev-")) &&
		    strstr(from_task_comm, "-ReqQ"))
			goto yes_and_exit;
		/*
		 * `wmshell.main` and `wmshell.splashscreen` threads are defined in
		 * `com.android.wm.shell.dagger.WMShellConcurrencyModule` in the Android source code.
		 */
		if (!strncmp(from_task_comm, "wmshell.main", strlen("wmshell.main")) ||
		    !strncmp(from_task_comm, "ll.splashscreen", strlen("ll.splashscreen")))
			goto yes_and_exit;
		if (t->from->task->pid == t->from->task->tgid) {
			for (i = 0; i < ARRAY_SIZE(task_name); i++)
				if (strncmp(from_task_comm, task_name[i], strlen(task_name[i])) == 0)
					goto yes_and_exit;
			if (is_miui_rom) {
				for (i = 0; i < ARRAY_SIZE(task_name_miui); i++)
					if (strncmp(from_task_comm, task_name_miui[i], strlen(task_name_miui[i])) == 0)
						goto yes_and_exit;
			}
		}

		return false;

yes_and_exit:
#ifdef CONFIG_BINDER_PRIO_DEBUG
		if (debug)
			pr_info("binder_prio: %s: tid: %d, from_task: %s, from_task_gl: %s; to_task: %s\n",
				__func__, t->from->task->pid,
				t->from->task->comm, t->from->task->group_leader->comm, t->to_proc->tsk->comm);
#endif
		return true;

		#undef from_task_comm
		#undef from_task_gl_comm
	}
	return false;
}

static void extend_surfacefinger_binder_set_priority_handler(void *data, struct binder_transaction *t, struct task_struct *task) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_10_0
	struct sched_param params;
#endif
	struct binder_priority desired;
	unsigned int policy;
	struct binder_node *target_node = t->buffer->target_node;

	desired.prio = target_node->min_priority;
	desired.sched_policy = target_node->sched_policy;
	policy = desired.sched_policy;

	if (set_binder_rt_task(t)) {
		desired.sched_policy = SCHED_FIFO;
		desired.prio = 98;
		policy = desired.sched_policy;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_4_0 && LINUX_VERSION_CODE < KERNEL_VERSION_5_10_0
    if (rt_policy(policy) && task->policy != policy) {
        set_binder_task_priority(task, policy, desired.prio);
    }
#else
	if (rt_policy(policy) && task->policy != policy) {
		params.sched_priority = to_userspace_prio(policy, desired.prio);
		sched_setscheduler_nocheck(task, policy | SCHED_RESET_ON_FORK, &params);
	}
#endif
}

// Placeholder functions for 5.4 (doing the best we can without tracepoints)
#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_4_0 && LINUX_VERSION_CODE < KERNEL_VERSION_5_10_0
static void extend_surfacefinger_binder_trans_handler_54(struct binder_proc *target_proc,
    struct binder_proc *proc, struct binder_thread *thread, struct binder_transaction_data *tr) {

    struct sched_param params; // Declare params here
    int ret; // Declare ret here

    if (target_proc && target_proc->tsk && strncmp(target_proc->tsk->comm, "surfaceflinger",
        strlen("surfaceflinger")) == 0) {
        // Try to set surfaceflinger priority directly (coarse-grained approach)

        params.sched_priority = 98; // Example priority

        ret = sched_setscheduler_nocheck(target_proc->tsk, SCHED_FIFO | SCHED_RESET_ON_FORK, &params);
        if(ret){
            printk(KERN_ERR "binder_prio: Failed to set surfaceflinger priority %d\n", ret); // Include MODULE_NAME here too
        }
    }
}

 /*this code should be removed but we need to test? */
static void extend_skip_binder_thread_priority_from_rt_to_normal_handler_54(struct task_struct *task, bool *skip) {
    if (task && rt_policy(task->policy)) {
        // Since we can't intercept the priority boost, we'll try to prevent it
        // by keeping the task's priority high.  This is a very indirect approach.
        *skip = true; // Try to prevent any further priority changes

    }
}
#endif //End of version check

static void extend_surfacefinger_binder_trans_handler(void *data, struct binder_proc *target_proc,
    struct binder_proc *proc,struct binder_thread *thread, struct binder_transaction_data *tr) {
	if (target_proc && target_proc->tsk && strncmp(target_proc->tsk->comm, "surfaceflinger",
		strlen("surfaceflinger")) == 0) {
		if (thread && proc && tr && thread->transaction_stack
			&& (!(thread->transaction_stack->flags & TF_ONE_WAY))) {
			target_proc->default_priority.sched_policy = SCHED_FIFO;
			target_proc->default_priority.prio = 98;
		}
	}
}

static void extend_skip_binder_thread_priority_from_rt_to_normal_handler(void *data, struct task_struct *task, bool *skip) {
	if (task && rt_policy(task->policy)) {
		*skip = true;
	}
}

int __init binder_prio_init(void)
{
	struct path path;

	pr_info("binder_prio: module init!");

	if (kern_path(miui_framework, LOOKUP_FOLLOW, &path) == 0) {
		pr_info("binder_prio: Miui/HyperOS rom detected!\n");
		is_miui_rom = true;
	} else {
		pr_info("binder_prio: AOSP rom detected!\n");
		is_miui_rom = false;
	}
	path_put(&path);

#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_10_0
    register_trace_android_vh_binder_set_priority(extend_surfacefinger_binder_set_priority_handler, NULL);
    register_trace_android_vh_binder_trans(extend_surfacefinger_binder_trans_handler, NULL);
    register_trace_android_vh_binder_priority_skip(extend_skip_binder_thread_priority_from_rt_to_normal_handler, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION_5_4_0 && LINUX_VERSION_CODE < KERNEL_VERSION_5_10_0
    printk(KERN_INFO "binder_prio: Running on kernel 5.4.x - Using task priority manipulation -  Limited functionality.\n");
    // On 5.4, we use the _54 functions:
#endif
    return 0;
}

void __exit binder_prio_exit(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION_5_10_0
    unregister_trace_android_vh_binder_set_priority(extend_surfacefinger_binder_set_priority_handler, NULL);
    unregister_trace_android_vh_binder_trans(extend_surfacefinger_binder_trans_handler, NULL);
    unregister_trace_android_vh_binder_priority_skip(extend_skip_binder_thread_priority_from_rt_to_normal_handler, NULL);
#endif
    pr_info("binder_prio: module exit!");
}

module_init(binder_prio_init);
module_exit(binder_prio_exit);
MODULE_LICENSE("GPL");
