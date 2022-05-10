
#include <stdio.h>
#include <pthread.h>

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/shrinker.h>

#include "tools-util.h"

static LIST_HEAD(shrinker_list);
static DEFINE_MUTEX(shrinker_lock);

struct meminfo {
	u64		total;
	u64		available;
	u64 	usage;
};

struct meminfo g_meminfo = { 0 };

pthread_t shrinker_thread_id;
void create_shrinker_thread(void);

int register_shrinker(struct shrinker *shrinker)
{
	if (!shrinker_thread_id)
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

static int read_meminfo(bool full)
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
			g_meminfo.total = parse_meminfo_line(v);

		if ((v = strcmp_prefix(line, "MemAvailable:")))
			g_meminfo.available = parse_meminfo_line(v);
	}
	fclose(f);

	if (full) {
		sprintf(buf, "/proc/%u/smaps_rollup", getpid());
		f = fopen(buf, "r");
		if (!f)
			return -1;

		/* This is really slow > 1s time */
		while ((len = getline(&line, &n, f)) != -1) {
			if ((v = strcmp_prefix(line, "Rss:")))
				g_meminfo.usage = parse_meminfo_line(v);

			if ((v = strcmp_prefix(line, "Swap:")))
				g_meminfo.usage += parse_meminfo_line(v);
		}

		fclose(f);
	}
	free(line);

	return 0;
}

static void run_shrinkers(void);
void* shrinker_thread(void *vargp) {
	int i = 0;

	while (1) {
		i++;
		read_meminfo(!(i % 30));
		run_shrinkers();
		sleep(1);
	}

}


void create_shrinker_thread(void) {
	int ret;
	ret = pthread_create(&shrinker_thread_id, NULL, &shrinker_thread, NULL);
	if (ret) {
		printf("ret = %d", ret);
		die("shrinker thread failed to start");
	}
	pthread_setname_np(shrinker_thread_id, "shrinker");
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

static void run_shrinkers(void)
{
	struct shrinker *shrinker;
	s64 want_shrink;

	/* Fast out if there are no shrinkers to run. */
	if (list_empty(&shrinker_list))
		return;

	if (g_meminfo.total && g_meminfo.available) {
		want_shrink   = (g_meminfo.total >> 3) - (g_meminfo.total - g_meminfo.usage);

		if (want_shrink <= 0)
			return;
	} else {
		/* If we weren't able to read /proc/meminfo, we must be pretty
		 * low: */

		want_shrink = 8 << 20;
	}
	/* Free twice as much as we need so we don't reach the limit on
	 * next kmalloc
	*/
	want_shrink = want_shrink << 1;

	mutex_lock(&shrinker_lock);
	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask	= __GFP_ZERO,
			.nr_to_scan	= want_shrink >> PAGE_SHIFT
		};

		shrinker->scan_objects(shrinker, &sc);
	}
	mutex_unlock(&shrinker_lock);
}
