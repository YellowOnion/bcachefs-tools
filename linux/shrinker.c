
#include <stdio.h>
#include <unistd.h>

#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>

#include "tools-util.h"

static LIST_HEAD(shrinker_list);
static DEFINE_MUTEX(shrinker_lock);

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	return calloc(sizeof(struct shrinker), 1);
}

int shrinker_register(struct shrinker *shrinker)
{
	mutex_lock(&shrinker_lock);
	list_add_tail(&shrinker->list, &shrinker_list);
	mutex_unlock(&shrinker_lock);
	return 0;
}

void unregister_shrinker(struct shrinker *shrinker)
{
	mutex_lock(&shrinker_lock);
	list_del(&shrinker->list);
	mutex_unlock(&shrinker_lock);
}

struct meminfo {
	u64		total;
	u64		available;
};

void si_meminfo(struct sysinfo *val)
{
	long page_size = sysconf(_SC_PAGESIZE);
	memset(val, 0, sizeof(*val));
	val->mem_unit = 1;

	val->totalram = sysconf(_SC_PHYS_PAGES) * page_size;
	val->freeram  = sysconf(_SC_AVPHYS_PAGES) * page_size;
}

static void run_shrinkers_allocation_failed(gfp_t gfp_mask)
{
	struct shrinker *shrinker;

	mutex_lock(&shrinker_lock);
	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = { .gfp_mask	= gfp_mask, };

		unsigned long have = shrinker->count_objects(shrinker, &sc);

		sc.nr_to_scan = have / 8;

		shrinker->scan_objects(shrinker, &sc);
	}
	mutex_unlock(&shrinker_lock);
}

void run_shrinkers(gfp_t gfp_mask, bool allocation_failed)
{
	struct shrinker *shrinker;
	struct sysinfo info;
	struct mallinfo2 malloc_info = mallinfo2();
	s64 want_shrink;

	if (!(gfp_mask & GFP_KERNEL))
		return;

	/* Fast out if there are no shrinkers to run. */
	if (list_empty(&shrinker_list))
		return;

	if (allocation_failed) {
		run_shrinkers_allocation_failed(gfp_mask);
		return;
	}

	si_meminfo(&info);

	if (info.totalram && info.totalram >> 4 < info.freeram) {
		/* freeram goes up when system swaps, use malloced data instead */
		want_shrink = -malloc_info.arena + (info.totalram / 10 * 8);

		if (want_shrink <= 0)
			return;
	} else {
		/* We want to play nice with other apps keep 6% avaliable, free 3% */
		want_shrink = (info.totalram >> 5);
	}

	mutex_lock(&shrinker_lock);
	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask	= gfp_mask,
			.nr_to_scan	= want_shrink >> PAGE_SHIFT
		};

		shrinker->scan_objects(shrinker, &sc);
	}
	mutex_unlock(&shrinker_lock);
}

static int shrinker_thread(void *arg)
{
	while (!kthread_should_stop()) {
		struct timespec to;
		int v;

		clock_gettime(CLOCK_MONOTONIC, &to);
		to.tv_sec += 1;
		__set_current_state(TASK_INTERRUPTIBLE);
		errno = 0;
		while ((v = READ_ONCE(current->state)) != TASK_RUNNING &&
		       errno != ETIMEDOUT)
			futex(&current->state, FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG,
			      v, &to, NULL, (uint32_t)~0);
		if (kthread_should_stop())
			break;
		if (v != TASK_RUNNING)
			__set_current_state(TASK_RUNNING);
		run_shrinkers(GFP_KERNEL, false);
	}

	return 0;
}

struct task_struct *shrinker_task;

__attribute__((constructor(103)))
static void shrinker_thread_init(void)
{
	shrinker_task = kthread_run(shrinker_thread, NULL, "shrinkers");
	BUG_ON(IS_ERR(shrinker_task));
}

__attribute__((destructor(103)))
static void shrinker_thread_exit(void)
{
	int ret = kthread_stop(shrinker_task);
	BUG_ON(ret);

	shrinker_task = NULL;
}
