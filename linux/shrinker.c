
#include <stdio.h>
#include <pthread.h>

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>
#include <linux/kthread.h>

#include "tools-util.h"

static LIST_HEAD(shrinker_list);
static DEFINE_MUTEX(shrinker_lock);

struct meminfo {
	u64		mem_total;
	u64		mem_available;
	u64     swap_total;
	u64     swap_available;
	u64 	usage;
};

struct task_struct *shrinker_thread_task;

void create_shrinker_thread(void);

int register_shrinker(struct shrinker *shrinker)
{
	if (!shrinker_thread_task)
		create_shrinker_thread();
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

static u64 parse_meminfo_line(const char *line)
{
	u64 v;

	if (sscanf(line, " %llu kB", &v) < 1)
		die("sscanf error");
	return v << 10;
}

static int read_meminfo(struct meminfo *info)
{
	size_t len, n = 0;
	char *line = NULL;
	const char *v;
	FILE *f;

	char buf[64];

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;

	while ((len = getline(&line, &n, f)) != -1) {
		if ((v = strcmp_prefix(line, "MemTotal:")))
			info->mem_total = parse_meminfo_line(v);

		if ((v = strcmp_prefix(line, "MemAvailable:")))
			info->mem_available = parse_meminfo_line(v);

		if ((v = strcmp_prefix(line, "SwapTotal:")))
			info->swap_total = parse_meminfo_line(v);

		if ((v = strcmp_prefix(line, "SwapFree:")))
			info->swap_available = parse_meminfo_line(v);
	}
	fclose(f);

	sprintf(buf, "/proc/%u/smaps_rollup", getpid());
	f = fopen(buf, "r");
	if (!f)
		return -1;

	/* This is really slow > 1s time */
	while ((len = getline(&line, &n, f)) != -1) {
		if ((v = strcmp_prefix(line, "Rss:")))
			info->usage = parse_meminfo_line(v);

		if ((v = strcmp_prefix(line, "Swap:")))
			info->usage += parse_meminfo_line(v);
	}

	fclose(f);
	free(line);

	return 0;
}

static void run_shrinkers(s64);
static int shrinker_thread(void *vargp) {
	struct meminfo info = { 0 };
    s64 want_shrink = 0;
	s64 want_shrink_mem;
	s64 want_shrink_swap;

	while (true) {
		read_meminfo(&info);

		/* We want to bail if we have less than 1GB of free memory. This should
		 * prevent system from freezing if OOM reaper doesn't kick in.
		 */
		if (info.mem_available + info.swap_available < 1 << 30)
			die("we're using too much memory");

		want_shrink_mem   = (info.mem_total >> 3) - (info.mem_total - info.usage);
		want_shrink_swap   = (info.swap_total >> 2) - (info.swap_available);

		want_shrink = max(want_shrink_mem, want_shrink_swap);

		if (want_shrink > 0) {
			run_shrinkers(want_shrink); }
		sleep(1);
	}
	return 0;
}


void create_shrinker_thread(void) {
	struct task_struct *ret;
	ret = kthread_create(&shrinker_thread, NULL, "bch-shrinker");
	if (IS_ERR(ret)) {
		printf("ret = %d", ret);
		die("shrinker thread failed to start");
	}
	get_task_struct(ret);
	shrinker_thread_task = ret;
	wake_up_process(ret);
}

void run_shrinkers_allocation_failed(gfp_t gfp_mask)
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

static void run_shrinkers(s64 want_shrink)
{
	struct shrinker *shrinker;
	s64 done_shrink = 0;
	s64 ret;

	/* Fast out if there are no shrinkers to run. */
	if (list_empty(&shrinker_list))
		return;

	/* Loop as much as possible unless we can't free anything.
	 * If we can't free anything, we can assume all remaining
	 * nodes are dirty, we bail to sleep for a bit so we don't
	 * just burn CPU, and let us die if we're running out of
	 * memory.
	 * */

	mutex_lock(&shrinker_lock);
	while (done_shrink < want_shrink) {
		list_for_each_entry(shrinker, &shrinker_list, list) {

			struct shrink_control sc = {
				.gfp_mask	= GFP_KERNEL,
				.nr_to_scan	= (want_shrink - done_shrink) >> PAGE_SHIFT
			};

			ret = shrinker->scan_objects(shrinker, &sc);
			done_shrink += ret << PAGE_SHIFT;

			if (!ret || (done_shrink > 1 << 16) || (want_shrink - done_shrink <= 0))
				goto out;
		}
	}
out:
	mutex_unlock(&shrinker_lock);
}
