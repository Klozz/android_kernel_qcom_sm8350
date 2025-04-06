/*
 * Adapted CASS Scheduler for Linux Kernel 5.4
 * Original author: Sultan Alsawaf <sultan@kerneltoast.com>
 * Ported by carlos 'klozz' jesus <carlosj@klozz.dev> for 5.4 compatibility
 */

#ifndef _SCHED_CASS_H
#define _SCHED_CASS_H

#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/sched/idle.h>
#include <linux/sched/deadline.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/rt.h>
#include <linux/sched/cpumask.h>
#include <linux/sched/stats.h>
#include <linux/sched/debug.h>
#include <linux/sched/cputime.h>
#include <linux/sched/loadavg.h>
#include <linux/cpuidle.h>
#include <linux/sched/nohz.h>
#include <linux/rcupdate.h>

/* Fake uclamp support for kernels that don't have it (e.g., 5.4) */
static inline unsigned int uclamp_eff_value(struct task_struct *p, unsigned int clamp_id)
{
	/* clamp_id == 0 → UCLAMP_MIN
	   clamp_id == 1 → UCLAMP_MAX */

	if (clamp_id == 0)
		return 0; // No min clamp

	if (clamp_id == 1)
		return SCHED_CAPACITY_SCALE; // 1024, no max cap

	return 0;
}

#endif /* _SCHED_CASS_H */