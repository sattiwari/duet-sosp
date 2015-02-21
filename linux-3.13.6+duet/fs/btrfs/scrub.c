/*
 * Copyright (C) 2011, 2012 STRATO.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/blkdev.h>
#include <linux/ratelimit.h>
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "ordered-data.h"
#include "transaction.h"
#include "backref.h"
#include "extent_io.h"
#include "dev-replace.h"
#include "check-integrity.h"
#include "rcu-string.h"
#include "raid56.h"
#ifdef CONFIG_BTRFS_DUET_SCRUB
#include "mapping.h"
#include <linux/duet.h>
#include <linux/genhd.h>
#endif /* CONFIG_BTRFS_DUET_SCRUB */
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
#include <linux/timer.h>

#define BTRFS_SCRUB_WAIT_TO	1+5*HZ/1000 /* Wait timeout */
#define MAX_BIOS_PER_SCTX	1024
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
#ifdef CONFIG_BTRFS_FS_SCRUB_READA
#define BTRFS_SCRUB_MAX_READA	20 /* Max concurrent readahead sessions */
#endif /* CONFIG_BTRFS_FS_SCRUB_READA */

#ifdef CONFIG_BTRFS_DUET_SCRUB_DEBUG
#define scrub_dbg(...)	printk(__VA_ARGS__)
#else
#define scrub_dbg(...)
#endif

/*
 * This is only the first step towards a full-features scrub. It reads all
 * extent and super block and verifies the checksums. In case a bad checksum
 * is found or the extent cannot be read, good data will be written back if
 * any can be found.
 *
 * Future enhancements:
 *  - In case an unrepairable extent is encountered, track which files are
 *    affected and report them
 *  - track and record media errors, throw out bad devices
 *  - add a mode to also read unallocated space
 */

struct scrub_block;
struct scrub_ctx;

/*
 * the following three values only influence the performance.
 * The last one configures the number of parallel and outstanding I/O
 * operations. The first two values configure an upper limit for the number
 * of (dynamically allocated) pages that are added to a bio.
 */
#define SCRUB_PAGES_PER_RD_BIO	32	/* 128k per bio */
#define SCRUB_PAGES_PER_WR_BIO	32	/* 128k per bio */

/*
 * the following value times PAGE_SIZE needs to be large enough to match the
 * largest node/leaf/sector size that shall be supported.
 * Values larger than BTRFS_STRIPE_LEN are not supported.
 */
#define SCRUB_MAX_PAGES_PER_BLOCK	16	/* 64k per node/leaf/sector */

struct scrub_page {
	struct scrub_block	*sblock;
	struct page		*page;
	struct btrfs_device	*dev;
	u64			flags;  /* extent flags */
	u64			generation;
	u64			logical;
	u64			physical;
	u64			physical_for_dev_replace;
	atomic_t		ref_count;
	struct {
		unsigned int	mirror_num:8;
		unsigned int	have_csum:1;
		unsigned int	io_error:1;
	};
	u8			csum[BTRFS_CSUM_SIZE];
};

struct scrub_bio {
	int			index;
	struct scrub_ctx	*sctx;
	struct btrfs_device	*dev;
	struct bio		*bio;
	int			err;
	u64			logical;
	u64			physical;
#if SCRUB_PAGES_PER_WR_BIO >= SCRUB_PAGES_PER_RD_BIO
	struct scrub_page	*pagev[SCRUB_PAGES_PER_WR_BIO];
#else
	struct scrub_page	*pagev[SCRUB_PAGES_PER_RD_BIO];
#endif
	int			page_count;
	int			next_free;
	struct btrfs_work	work;
	struct btrfs_work	work_end;

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	/* Timer-related fields for delayed bio deallocation */
	unsigned long		t_start;	/* jiffies at bio issue time */
	unsigned long		t_wasted;	/* wasted: timer => wqueue */
	struct timer_list	timer;
	unsigned long		timer_start;	/* jiffies, timer issued */
	unsigned long		timer_expires;	/* jiffies, timer expiration */
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
};

struct scrub_block {
	struct scrub_page	*pagev[SCRUB_MAX_PAGES_PER_BLOCK];
	int			page_count;
	atomic_t		outstanding_pages;
	atomic_t		ref_count; /* free mem on transition to zero */
	struct scrub_ctx	*sctx;
	struct {
		unsigned int	header_error:1;
		unsigned int	checksum_error:1;
		unsigned int	no_io_error_seen:1;
		unsigned int	generation_error:1; /* also sets header_error */
	};
};

struct scrub_wr_ctx {
	struct scrub_bio *wr_curr_bio;
	struct btrfs_device *tgtdev;
	int pages_per_wr_bio; /* <= SCRUB_PAGES_PER_WR_BIO */
	atomic_t flush_all_writes;
	struct mutex wr_lock;
};

struct scrub_ctx {
	struct scrub_bio	**bios;
	u16			bios_per_sctx;
	u16			bios_alloc_size;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	struct mutex		bios_lock;
	atomic_t		bios_allocated;
	spinlock_t		curr_lock;

	/* Trickle rate vars */
	u8			bgflags;
	int			old_ioprio;
	u64			deadline;
	u64			used_bytes; 		/* bytes in extents */
	atomic_t		pending_removals;
	atomic64_t		delay;			/* delay b/w bios */
	struct timeval		t_start;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	struct btrfs_root	*dev_root;
	int			first_free;
	int			curr;
	atomic_t		bios_in_flight;
	atomic_t		workers_pending;
	spinlock_t		list_lock;
	wait_queue_head_t	list_wait;
	u16			csum_size;
	struct list_head	csum_list;
	atomic_t		cancel_req;
	int			readonly;
	int			pages_per_rd_bio;
	u32			sectorsize;
	u32			nodesize;
	u32			leafsize;

	int			is_dev_replace;
	struct scrub_wr_ctx	wr_ctx;

	/*
	 * statistics
	 */
	struct btrfs_scrub_progress stat;
	spinlock_t		stat_lock;

#ifdef CONFIG_BTRFS_DUET_SCRUB
	u8			taskid;
	struct block_device	*scrub_dev;
#endif /* CONFIG_BTRFS_DUET_SCRUB */
};

struct scrub_fixup_nodatasum {
	struct scrub_ctx	*sctx;
	struct btrfs_device	*dev;
	u64			logical;
	struct btrfs_root	*root;
	struct btrfs_work	work;
	int			mirror_num;
};

struct scrub_nocow_inode {
	u64			inum;
	u64			offset;
	u64			root;
	struct list_head	list;
};

struct scrub_copy_nocow_ctx {
	struct scrub_ctx	*sctx;
	u64			logical;
	u64			len;
	int			mirror_num;
	u64			physical_for_dev_replace;
	struct list_head	inodes;
	struct btrfs_work	work;
};

struct scrub_warning {
	struct btrfs_path	*path;
	u64			extent_item_size;
	char			*scratch_buf;
	char			*msg_buf;
	const char		*errstr;
	sector_t		sector;
	u64			logical;
	struct btrfs_device	*dev;
	int			msg_bufsize;
	int			scratch_bufsize;
};


static void scrub_pending_bio_inc(struct scrub_ctx *sctx);
static void scrub_pending_bio_dec(struct scrub_ctx *sctx);
static void scrub_pending_trans_workers_inc(struct scrub_ctx *sctx);
static void scrub_pending_trans_workers_dec(struct scrub_ctx *sctx);
static int scrub_handle_errored_block(struct scrub_block *sblock_to_check);
static int scrub_setup_recheck_block(struct scrub_ctx *sctx,
				     struct btrfs_fs_info *fs_info,
				     struct scrub_block *original_sblock,
				     u64 length, u64 logical,
				     struct scrub_block *sblocks_for_recheck);
static void scrub_recheck_block(struct btrfs_fs_info *fs_info,
				struct scrub_block *sblock, int is_metadata,
				int have_csum, u8 *csum, u64 generation,
				u16 csum_size);
static void scrub_recheck_block_checksum(struct btrfs_fs_info *fs_info,
					 struct scrub_block *sblock,
					 int is_metadata, int have_csum,
					 const u8 *csum, u64 generation,
					 u16 csum_size);
static int scrub_repair_block_from_good_copy(struct scrub_block *sblock_bad,
					     struct scrub_block *sblock_good,
					     int force_write);
static int scrub_repair_page_from_good_copy(struct scrub_block *sblock_bad,
					    struct scrub_block *sblock_good,
					    int page_num, int force_write);
static void scrub_write_block_to_dev_replace(struct scrub_block *sblock);
static int scrub_write_page_to_dev_replace(struct scrub_block *sblock,
					   int page_num);
static int scrub_checksum_data(struct scrub_block *sblock);
static int scrub_checksum_tree_block(struct scrub_block *sblock);
static int scrub_checksum_super(struct scrub_block *sblock);
static void scrub_block_get(struct scrub_block *sblock);
static void scrub_block_put(struct scrub_block *sblock);
static void scrub_page_get(struct scrub_page *spage);
static void scrub_page_put(struct scrub_page *spage);
static int scrub_add_page_to_rd_bio(struct scrub_ctx *sctx,
				    struct scrub_page *spage);
static int scrub_pages(struct scrub_ctx *sctx, u64 logical, u64 len,
		       u64 physical, struct btrfs_device *dev, u64 flags,
		       u64 gen, int mirror_num, u8 *csum, int force,
		       u64 physical_for_dev_replace);
static void scrub_bio_end_io(struct bio *bio, int err);
static void scrub_bio_end_io_worker(struct btrfs_work *work);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
static void scrub_bio_end_io_timer(unsigned long arg);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
static void scrub_bio_end_io_wrapup_worker(struct btrfs_work *work);
static void scrub_block_complete(struct scrub_block *sblock);
static void scrub_remap_extent(struct btrfs_fs_info *fs_info,
			       u64 extent_logical, u64 extent_len,
			       u64 *extent_physical,
			       struct btrfs_device **extent_dev,
			       int *extent_mirror_num);
static int scrub_setup_wr_ctx(struct scrub_ctx *sctx,
			      struct scrub_wr_ctx *wr_ctx,
			      struct btrfs_fs_info *fs_info,
			      struct btrfs_device *dev,
			      int is_dev_replace);
static void scrub_free_wr_ctx(struct scrub_wr_ctx *wr_ctx);
static int scrub_add_page_to_wr_bio(struct scrub_ctx *sctx,
				    struct scrub_page *spage);
static void scrub_wr_submit(struct scrub_ctx *sctx);
static void scrub_wr_bio_end_io(struct bio *bio, int err);
static void scrub_wr_bio_end_io_worker(struct btrfs_work *work);
static int write_page_nocow(struct scrub_ctx *sctx,
			    u64 physical_for_dev_replace, struct page *page);
static int copy_nocow_pages_for_inode(u64 inum, u64 offset, u64 root,
				      struct scrub_copy_nocow_ctx *ctx);
static int copy_nocow_pages(struct scrub_ctx *sctx, u64 logical, u64 len,
			    int mirror_num, u64 physical_for_dev_replace);
static void copy_nocow_pages_worker(struct btrfs_work *work);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
static void scrub_adjust_rate(struct scrub_ctx *sctx, u64 elapsed,
	u64 total_scrubbed, u16 *bios_per_sctx, long *delay);
static int scrub_realloc_bios_array(struct scrub_ctx *sctx, u16 new_size);
static int scrub_remove_bio(struct scrub_ctx *sctx, u16 idx);

static void scrub_pending_bio_alloc_inc(struct scrub_ctx *sctx)
{
	atomic_inc(&sctx->bios_allocated);
}

static void scrub_pending_bio_alloc_dec(struct scrub_ctx *sctx)
{
	atomic_dec(&sctx->bios_allocated);
	wake_up(&sctx->list_wait);
}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

static void scrub_pending_bio_inc(struct scrub_ctx *sctx)
{
	atomic_inc(&sctx->bios_in_flight);
}

static void scrub_pending_bio_dec(struct scrub_ctx *sctx)
{
	atomic_dec(&sctx->bios_in_flight);
	wake_up(&sctx->list_wait);
}

/*
 * used for workers that require transaction commits (i.e., for the
 * NOCOW case)
 */
static void scrub_pending_trans_workers_inc(struct scrub_ctx *sctx)
{
	struct btrfs_fs_info *fs_info = sctx->dev_root->fs_info;

	/*
	 * increment scrubs_running to prevent cancel requests from
	 * completing as long as a worker is running. we must also
	 * increment scrubs_paused to prevent deadlocking on pause
	 * requests used for transactions commits (as the worker uses a
	 * transaction context). it is safe to regard the worker
	 * as paused for all matters practical. effectively, we only
	 * avoid cancellation requests from completing.
	 */
	mutex_lock(&fs_info->scrub_lock);
	atomic_inc(&fs_info->scrubs_running);
	atomic_inc(&fs_info->scrubs_paused);
	mutex_unlock(&fs_info->scrub_lock);
	atomic_inc(&sctx->workers_pending);
}

/* used for workers that require transaction commits */
static void scrub_pending_trans_workers_dec(struct scrub_ctx *sctx)
{
	struct btrfs_fs_info *fs_info = sctx->dev_root->fs_info;

	/*
	 * see scrub_pending_trans_workers_inc() why we're pretending
	 * to be paused in the scrub counters
	 */
	mutex_lock(&fs_info->scrub_lock);
	atomic_dec(&fs_info->scrubs_running);
	atomic_dec(&fs_info->scrubs_paused);
	mutex_unlock(&fs_info->scrub_lock);
	atomic_dec(&sctx->workers_pending);
	wake_up(&fs_info->scrub_pause_wait);
	wake_up(&sctx->list_wait);
}

static void scrub_free_csums(struct scrub_ctx *sctx)
{
	while (!list_empty(&sctx->csum_list)) {
		struct btrfs_ordered_sum *sum;
		sum = list_first_entry(&sctx->csum_list,
				       struct btrfs_ordered_sum, list);
		list_del(&sum->list);
		kfree(sum);
	}
}

static noinline_for_stack void scrub_free_ctx(struct scrub_ctx *sctx)
{
	int i;

	if (!sctx)
		return;

	scrub_free_wr_ctx(&sctx->wr_ctx);

#ifdef CONFIG_BTRFS_DUET_SCRUB
	/* Deregister the task from the Duet framework */
	if (sctx->taskid && duet_deregister(sctx->taskid))
		printk(KERN_ERR "scrub: failed to deregister with duet\n");
#endif /* CONFIG_BTRFS_DUET_SCRUB */

	/* this can happen when scrub is cancelled */
	if (sctx->curr != -1) {
		struct scrub_bio *sbio = sctx->bios[sctx->curr];

		for (i = 0; i < sbio->page_count; i++) {
			WARN_ON(!sbio->pagev[i]->page);
			scrub_block_put(sbio->pagev[i]->sblock);
		}
		bio_put(sbio->bio);
	}

	/* Before freeing anything, print some stats */
	printk(KERN_INFO "btrfs scrub: Total scrubbed is %llu, of which %llu "
		"were in data extents, and %llu were in tree extents.\n",
		sctx->stat.data_bytes_scrubbed + sctx->stat.tree_bytes_scrubbed,
		sctx->stat.data_bytes_scrubbed, sctx->stat.tree_bytes_scrubbed);

	/* We waited for all pending requests before we got here,
	 * so no need to lock before destroying */
	for (i = 0; i < sctx->bios_per_sctx; ++i) {
		struct scrub_bio *sbio = sctx->bios[i];

		if (!sbio)
			break;
		kfree(sbio);
	}

	kfree(sctx->bios);
	scrub_free_csums(sctx);
	kfree(sctx);
}

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
/* Make sure bios_lock mutex is held before getting in here */
static void scrub_adjust_rate(struct scrub_ctx *sctx, u64 elapsed,
	u64 total_scrubbed, u16 *bios_per_sctx, long *delay)
{
	u64 rem_bytes = 64 * PAGE_SIZE * SCRUB_PAGES_PER_RD_BIO;
	u64 rem_time = 1;
	u64 bytes_per_sec;
	struct timeval cur;

	/* Get current time first */
	if (!elapsed) {
		do_gettimeofday(&cur);
		elapsed = cur.tv_sec - sctx->t_start.tv_sec;
	}

	if (sctx->used_bytes > total_scrubbed)
		rem_bytes = sctx->used_bytes - total_scrubbed;

	if (elapsed < sctx->deadline) {
		rem_time = sctx->deadline - elapsed;
	} else {
		*bios_per_sctx = 64;
		*delay = 0;
		return;
	}

	bytes_per_sec = rem_bytes / rem_time;
	if (rem_bytes % rem_time)
		bytes_per_sec++;

	/* Convert bytes per sec to bios_in_flight and delay */
	if (bytes_per_sec < (SCRUB_PAGES_PER_RD_BIO * PAGE_SIZE)) {
		*bios_per_sctx = 1;
		*delay = (SCRUB_PAGES_PER_RD_BIO * PAGE_SIZE) / bytes_per_sec;
	} else {
		*delay = 1;
		*bios_per_sctx = bytes_per_sec /
			(SCRUB_PAGES_PER_RD_BIO * PAGE_SIZE);
		if (bytes_per_sec % (SCRUB_PAGES_PER_RD_BIO * PAGE_SIZE))
			(*bios_per_sctx)++;
		if (*bios_per_sctx > MAX_BIOS_PER_SCTX)
			*bios_per_sctx = MAX_BIOS_PER_SCTX;
	}
}

/* Make sure mutex bios_lock is held *before* calling this function! */
static int scrub_realloc_bios_array(struct scrub_ctx *sctx, u16 new_size)
{
	int i;
	u16 old_size;
	struct scrub_bio **buf, **old_ptr;

	spin_lock(&sctx->list_lock);
	old_ptr = sctx->bios;
	old_size = sctx->bios_per_sctx;

	BUG_ON(old_size > new_size);

	/*
	 * We check the current allocation size for **bios, and avoid
	 * reallocating if there's unused memory due to removals.
	 */
	if (new_size > sctx->bios_alloc_size) {
		buf = kcalloc(new_size, sizeof(struct scrub_bio *), GFP_NOFS);
		if (!buf)
			goto nomem;

		memcpy(buf, sctx->bios, sctx->bios_per_sctx *
			sizeof(struct scrub_bio *));
		sctx->bios = buf;
	}

	sctx->bios_per_sctx = new_size;

	/* Now initialize the _new_ bios! */
	for (i = old_size; i < sctx->bios_per_sctx; ++i) {
		struct scrub_bio *sbio;

		sbio = kzalloc(sizeof(*sbio), GFP_NOFS);
		if (!sbio)
			goto nomembios;
		sctx->bios[i] = sbio;

		sbio->index = i;
		sbio->sctx = sctx;
		sbio->page_count = 0;
		sbio->work.func = scrub_bio_end_io_worker;
		sbio->work_end.func = scrub_bio_end_io_wrapup_worker;

		init_timer(&sbio->timer);
		sbio->t_wasted = 0;

		/*
		 * We are daisy-chaining all new bios, and adding them
		 * in the beginning of the free list
		 */
		if (i != sctx->bios_per_sctx - 1)
			sctx->bios[i]->next_free = i + 1;
		else
			sctx->bios[i]->next_free = sctx->first_free;
	}

	sctx->first_free = old_size;

	if (new_size > sctx->bios_alloc_size) {
		sctx->bios_alloc_size = new_size;
		spin_unlock(&sctx->list_lock);
		kfree(old_ptr);
	} else {
		spin_unlock(&sctx->list_lock);
	}

	return 0;

nomembios:
	for (i = old_size; i < sctx->bios_per_sctx; ++i)
		if (sctx->bios[i])
			kfree(sctx->bios[i]);
	kfree(sctx->bios);

nomem:
	sctx->bios = old_ptr;
	sctx->bios_per_sctx = old_size;
	spin_unlock(&sctx->list_lock);
	printk("btrfs scrub: Ran out of memory!\n");
	return -ENOMEM;
}

/* Make sure mutex bios_lock is held *before* calling this function! */
static int scrub_remove_bio(struct scrub_ctx *sctx, u16 idx)
{
	int cur;

	if (atomic_read(&sctx->pending_removals) == 0) {
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
		printk(KERN_DEBUG "btrfs scrub: pending_removals == 0");
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */

		/* This guy will not be getting removed, so add it to
		 * the list before returning */
		sctx->bios[idx]->t_wasted = jiffies - sctx->bios[idx]->t_wasted;
		spin_lock(&sctx->list_lock);
		sctx->bios[idx]->next_free = sctx->first_free;
		sctx->first_free = idx;
		spin_unlock(&sctx->list_lock);
		goto out;
	}

	if (idx >= sctx->bios_per_sctx) {
		printk(KERN_ERR "btrfs scrub: idx too large for "
			"scrub_remove_bio (%u)!\n", idx);
		goto out;
	}

	/* First, we need to free this bio. No ritual needed, it's not used */
	if (!sctx->bios[idx]) {
		printk(KERN_ERR "btrfs scrub: bios[idx] doesn't exist at "
			"scrub_remove_bio!\n");
		goto out;
	}
	kfree(sctx->bios[idx]);
	if (idx == sctx->bios_per_sctx-1) {
		sctx->bios_per_sctx--;
		atomic_dec(&sctx->pending_removals);
		goto out;
	}

	/*
	 * Now take the last bio, and place it where the removed bio was.
	 * Then, traverse the free bio list, and if the idx of the moved
	 * bio is found, update. Also update bios_per_sctx. The bio that
	 * we removed was not free, so we're good with that one.
	 */
	sctx->bios[idx] = sctx->bios[sctx->bios_per_sctx-1];
	sctx->bios[idx]->index = idx;
	spin_lock(&sctx->list_lock);
	if (sctx->first_free == sctx->bios_per_sctx - 1) {
		sctx->first_free = idx;
	} else {
		cur = sctx->first_free;
		while (cur != -1) {
			if (sctx->bios[cur]->next_free == sctx->bios_per_sctx-1) {
				sctx->bios[cur]->next_free = idx;
				break;
			}
			cur = sctx->bios[cur]->next_free;
		}
	}
	spin_unlock(&sctx->list_lock);

	spin_lock(&sctx->curr_lock);
	if (sctx->curr == sctx->bios_per_sctx - 1)
		sctx->curr = idx;
	spin_unlock(&sctx->curr_lock);

	sctx->bios_per_sctx--;
	atomic_dec(&sctx->pending_removals);
out:
	return 0;
}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

#ifdef CONFIG_BTRFS_DUET_SCRUB
/*
 * This is the core of the synergistic scrubber. We fetch page-related events,
 * and mark or unmark the corresponding LBN range(s), depending on whether the
 * event that occurred was an ADD or MOD of a page.
 *
 * Pages that are not found to have a logical/physical mapping yet, are enqueued
 * until we receive another event for them. Then we check them again.
 * TODO: Implement this functionality. It's not that important for scrubbing.
 *
 * We try to process up to 256 events at a time. However, we will stop if an
 * event requires us to fetch metadata from disk. If all operations took place
 * in memory, we return 0 if we run out of items, so that the scrubber knows it
 * can go ahead and queue bios. Otherwise we return 1, indicating that some IO
 * already occurred, or there's more than 256 events, so we need to give the
 * foreground workload a chance. That's ok, though, because we'll be processing
 * while the foreground workload is running anyway.
 */
static int process_duet_events(struct scrub_ctx *sctx)
{
	int ret = 256, mret = 0, stop = 0;
	u16 itret;
	u64 pstart, mapped_length;
	u64 len = PAGE_CACHE_SIZE;
	u64 dstart = sctx->scrub_dev->bd_part->start_sect << 9;
	struct duet_item itm;
	struct extent_map *em;
	struct inode *inode;
	struct btrfs_bio *bbio = NULL;
	struct btrfs_fs_info *fs_info = sctx->dev_root->fs_info;
	struct btrfs_device *pdev;

	while (ret) {
		if (duet_fetch(sctx->taskid, 1, &itm, &itret)) {
			printk(KERN_ERR "duet-scrub: duet_fetch failed\n");
			return 0;
		}

		/* If there were no events, return 0 */
		if (!itret)
			return 0;

		/* If this inode came from disk, remember to stop and give the
		 * other processes a chance */
		if (btrfs_iget_ino(fs_info, itm.ino, &inode, &stop))
			goto done;

		if (btrfs_get_logical(inode, itm.idx, &em, &stop))
			goto idone;

		/* Get the LBN range(s) corresponding to this item */
		bbio = NULL;
		mapped_length = PAGE_CACHE_SIZE;
		mret = btrfs_map_block(fs_info, READ, em->block_start,
			&mapped_length, &bbio, 0);
		if (mret || !bbio || mapped_length < len ||
			!bbio->stripes[0].dev->bdev) {
			scrub_dbg(KERN_INFO "duet-scrub: btrfs_map_block failed\n");
			kfree(bbio);
			goto idone;
		}

		pstart = bbio->stripes[0].physical +
			 ((itm.idx << PAGE_CACHE_SHIFT) - em->start);
		pdev = bbio->stripes[0].dev;
		kfree(bbio);

		scrub_dbg(KERN_INFO "duet-scrub: dev offt %llu\n", dstart);
		scrub_dbg(KERN_INFO "duet-scrub: phys offt %llu, len %llu\n",
			pstart, len);

		if (pdev->bdev->bd_contains != sctx->scrub_dev->bd_contains) {
			printk(KERN_INFO "duet-scrub: event refers to wrong "
				"device\n");
			goto idone;
		}

		switch (itm.state) {
		case DUET_PAGE_MODIFIED:
		case DUET_PAGE_ADDED_MODIFIED:
			scrub_dbg(KERN_INFO "duet-scrub: clearing [%llu, %llu] --"
				" dstart = %llu\n",
				dstart+pstart, dstart+pstart+len, dstart);
			if (duet_unmark(sctx->taskid, dstart+pstart, len) == -1)
				printk(KERN_ERR "duet-scrub: failed to unmark"
					" [%llu, %llu] range for task #%d\n",
					dstart + pstart, dstart + pstart +
					mapped_length, sctx->taskid);
			break;
		case DUET_PAGE_ADDED:
			scrub_dbg(KERN_INFO "duet-scrub: marking [%llu, %llu] --"
				" dstart = %llu\n",
				dstart+pstart, dstart+pstart+len, dstart);
			if (duet_mark(sctx->taskid, dstart+pstart, len) == -1)
				printk(KERN_ERR "duet-scrub: failed to mark"
					" [%llu, %llu] range for task #%d\n",
					dstart + pstart, dstart + pstart +
					mapped_length, sctx->taskid);
			break;
		}

idone:
		free_extent_map(em);
		iput(inode);
done:
		ret--;
		if (stop)
			break;
	}

	/*
	 * We need to let the foreground workload go if we've touched the disk.
	 * Otherwise, let the scrubber do one stripe.
	 * 1. If ret == 0 and stop == 0: there may be more, return 1
	 * 2. If ret > 0 and stop == 0: out of items, return 0
	 * 3. If ret > 0 and stop == 1: we hit the disk, return 1
	 */

	return (!ret || stop);
}
#endif /* CONFIG_BTRFS_DUET_SCRUB */

static noinline_for_stack
#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
struct scrub_ctx *scrub_setup_ctx(struct btrfs_device *dev, int is_dev_replace)
#else /* Adaptive scrubber code */
struct scrub_ctx *scrub_setup_ctx(struct btrfs_device *dev, u64 deadline,
	u8 bgflags, int is_dev_replace)
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
{
	struct scrub_ctx *sctx;
	struct btrfs_fs_info *fs_info = dev->dev_root->fs_info;
	int pages_per_rd_bio;
	int i, ret;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	long delay;
	struct timeval cur;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

	/*
	 * the setting of pages_per_rd_bio is correct for scrub but might
	 * be wrong for the dev_replace code where we might read from
	 * different devices in the initial huge bios. However, that
	 * code is able to correctly handle the case when adding a page
	 * to a bio fails.
	 */
	if (dev->bdev)
		pages_per_rd_bio = min_t(int, SCRUB_PAGES_PER_RD_BIO,
					 bio_get_nr_vecs(dev->bdev));
	else
		pages_per_rd_bio = SCRUB_PAGES_PER_RD_BIO;
	sctx = kzalloc(sizeof(*sctx), GFP_NOFS);
	if (!sctx)
		goto nomem;
	sctx->is_dev_replace = is_dev_replace;
	sctx->pages_per_rd_bio = pages_per_rd_bio;
	sctx->curr = -1;
	sctx->dev_root = dev->dev_root;
#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
	sctx->bios_per_sctx = 64;

	printk(KERN_INFO "btrfs scrubber: device size = %llu bytes; device "
		"used size = %llu bytes; bios per sctx = %u.\n",
		dev->disk_total_bytes, dev->bytes_used, sctx->bios_per_sctx);
#else /* Adaptive scrubber code */
	/*
	 * Find the rate at which the disk needs to be scrubbed, in order to
	 * meet our deadline (say, R MB/sec). We want >= 128kB per bio, so if
	 * R < 128kB, find the number of seconds by which we need to separate
	 * 128kB, and pick that as the delay. If deadline is zero, leave
	 * default values in place.
	 */
	sctx->bgflags = bgflags;
	sctx->old_ioprio = -1;
	sctx->deadline = deadline;
	sctx->used_bytes = 0;
	atomic_set(&sctx->pending_removals, 0);

	if (!sctx->deadline) {
		sctx->bios_per_sctx = 64;
		atomic64_set(&sctx->delay, 0);
	} else {
		/* Record time when scrub starts */
		do_gettimeofday(&sctx->t_start);
		if (sctx->bgflags & BTRFS_BGSC_ENUM) {
			/*
			 * Let's try and enumerate all extents on device to
			 * estimate bytes to scrubbed (~14sec/2GB of metadata)
			 */
			printk(KERN_INFO "btrfs scrub: begin devext enum\n");

			if (btrfs_calc_dev_extents_size(dev, 0,
			    dev->total_bytes, &sctx->used_bytes)) {
				printk(KERN_INFO "btrfs scrub: enum error\n");

				/* Assume all chunks are allocated */
				sctx->used_bytes = dev->bytes_used;
			}

			do_gettimeofday(&cur);
			sctx->deadline -= cur.tv_sec - sctx->t_start.tv_sec;
			sctx->t_start.tv_sec = cur.tv_sec;
			sctx->t_start.tv_usec = cur.tv_usec;
		} else {
			/* Assume we're going to scrub the entire device */
			sctx->used_bytes = dev->bytes_used;
		}
		printk(KERN_INFO "btrfs scrub: dev uses %llu bytes\n",
			sctx->used_bytes);

		scrub_adjust_rate(sctx, 0/* elapsed */, 0/* bytes scrubbed */,
			&sctx->bios_per_sctx, &delay);
		atomic64_set(&sctx->delay, delay);
	}

	printk(KERN_INFO "btrfs scrubber:\n\tdeadline = %llu sec\n"
		"\tdevice = %llu (%llu in chunks, %llu allocated to extents)\n"
		"\tbios per sctx = %u\n\tdelay = %ld (HZ=%u)\n",
		sctx->deadline, dev->disk_total_bytes, dev->bytes_used,
		sctx->used_bytes, sctx->bios_per_sctx,
		atomic64_read(&sctx->delay), HZ);
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */

	/* Now that we know how many bios we need, allocate them */
	sctx->bios = kcalloc(sctx->bios_per_sctx, sizeof(struct scrub_bio *),
		GFP_NOFS);
	sctx->bios_alloc_size = sctx->bios_per_sctx;

	/* Finally, create/initialize bios */
	for (i = 0; i < sctx->bios_per_sctx; ++i) {
		struct scrub_bio *sbio;

		sbio = kzalloc(sizeof(*sbio), GFP_NOFS);
		if (!sbio)
			goto nomem;
		sctx->bios[i] = sbio;

		sbio->index = i;
		sbio->sctx = sctx;
		sbio->page_count = 0;
		sbio->work.func = scrub_bio_end_io_worker;
		sbio->work_end.func = scrub_bio_end_io_wrapup_worker;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
		init_timer(&sbio->timer);
		sbio->t_wasted = 0;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
		if (i != sctx->bios_per_sctx - 1)
			sctx->bios[i]->next_free = i + 1;
		else
			sctx->bios[i]->next_free = -1;
	}
	sctx->first_free = 0;
	sctx->nodesize = dev->dev_root->nodesize;
	sctx->leafsize = dev->dev_root->leafsize;
	sctx->sectorsize = dev->dev_root->sectorsize;
	atomic_set(&sctx->bios_in_flight, 0);
	atomic_set(&sctx->workers_pending, 0);
	atomic_set(&sctx->cancel_req, 0);
	sctx->csum_size = btrfs_super_csum_size(fs_info->super_copy);
	INIT_LIST_HEAD(&sctx->csum_list);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	atomic_set(&sctx->bios_allocated, 0);

	mutex_init(&sctx->bios_lock);
	spin_lock_init(&sctx->curr_lock);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

	spin_lock_init(&sctx->list_lock);
	spin_lock_init(&sctx->stat_lock);
	init_waitqueue_head(&sctx->list_wait);

	ret = scrub_setup_wr_ctx(sctx, &sctx->wr_ctx, fs_info,
				 fs_info->dev_replace.tgtdev, is_dev_replace);
	if (ret) {
		scrub_free_ctx(sctx);
		return ERR_PTR(ret);
	}

#ifdef CONFIG_BTRFS_DUET_SCRUB
	sctx->scrub_dev = dev->bdev;

	/* Register the task with the Duet framework */
	if (duet_online() && duet_register(&sctx->taskid, "btrfs-scrub",
	    DUET_EVT_ADD|DUET_EVT_MOD, fs_info->sb->s_blocksize, fs_info->sb)) {
		printk(KERN_ERR "scrub: failed to register with duet\n");
		return ERR_PTR(-EFAULT);
	}
#endif /* CONFIG_BTRFS_DUET_SCRUB */
	return sctx;

nomem:
	scrub_free_ctx(sctx);
	return ERR_PTR(-ENOMEM);
}

static int scrub_print_warning_inode(u64 inum, u64 offset, u64 root,
				     void *warn_ctx)
{
	u64 isize;
	u32 nlink;
	int ret;
	int i;
	struct extent_buffer *eb;
	struct btrfs_inode_item *inode_item;
	struct scrub_warning *swarn = warn_ctx;
	struct btrfs_fs_info *fs_info = swarn->dev->dev_root->fs_info;
	struct inode_fs_paths *ipath = NULL;
	struct btrfs_root *local_root;
	struct btrfs_key root_key;

	root_key.objectid = root;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = (u64)-1;
	local_root = btrfs_read_fs_root_no_name(fs_info, &root_key);
	if (IS_ERR(local_root)) {
		ret = PTR_ERR(local_root);
		goto err;
	}

	ret = inode_item_info(inum, 0, local_root, swarn->path);
	if (ret) {
		btrfs_release_path(swarn->path);
		goto err;
	}

	eb = swarn->path->nodes[0];
	inode_item = btrfs_item_ptr(eb, swarn->path->slots[0],
					struct btrfs_inode_item);
	isize = btrfs_inode_size(eb, inode_item);
	nlink = btrfs_inode_nlink(eb, inode_item);
	btrfs_release_path(swarn->path);

	ipath = init_ipath(4096, local_root, swarn->path);
	if (IS_ERR(ipath)) {
		ret = PTR_ERR(ipath);
		ipath = NULL;
		goto err;
	}
	ret = paths_from_inode(inum, ipath);

	if (ret < 0)
		goto err;

	/*
	 * we deliberately ignore the bit ipath might have been too small to
	 * hold all of the paths here
	 */
	for (i = 0; i < ipath->fspath->elem_cnt; ++i)
		printk_in_rcu(KERN_WARNING "btrfs: %s at logical %llu on dev "
			"%s, sector %llu, root %llu, inode %llu, offset %llu, "
			"length %llu, links %u (path: %s)\n", swarn->errstr,
			swarn->logical, rcu_str_deref(swarn->dev->name),
			(unsigned long long)swarn->sector, root, inum, offset,
			min(isize - offset, (u64)PAGE_SIZE), nlink,
			(char *)(unsigned long)ipath->fspath->val[i]);

	free_ipath(ipath);
	return 0;

err:
	printk_in_rcu(KERN_WARNING "btrfs: %s at logical %llu on dev "
		"%s, sector %llu, root %llu, inode %llu, offset %llu: path "
		"resolving failed with ret=%d\n", swarn->errstr,
		swarn->logical, rcu_str_deref(swarn->dev->name),
		(unsigned long long)swarn->sector, root, inum, offset, ret);

	free_ipath(ipath);
	return 0;
}

static void scrub_print_warning(const char *errstr, struct scrub_block *sblock)
{
	struct btrfs_device *dev;
	struct btrfs_fs_info *fs_info;
	struct btrfs_path *path;
	struct btrfs_key found_key;
	struct extent_buffer *eb;
	struct btrfs_extent_item *ei;
	struct scrub_warning swarn;
	unsigned long ptr = 0;
	u64 extent_item_pos;
	u64 flags = 0;
	u64 ref_root;
	u32 item_size;
	u8 ref_level;
	const int bufsize = 4096;
	int ret;

	WARN_ON(sblock->page_count < 1);
	dev = sblock->pagev[0]->dev;
	fs_info = sblock->sctx->dev_root->fs_info;

	path = btrfs_alloc_path();

	swarn.scratch_buf = kmalloc(bufsize, GFP_NOFS);
	swarn.msg_buf = kmalloc(bufsize, GFP_NOFS);
	swarn.sector = (sblock->pagev[0]->physical) >> 9;
	swarn.logical = sblock->pagev[0]->logical;
	swarn.errstr = errstr;
	swarn.dev = NULL;
	swarn.msg_bufsize = bufsize;
	swarn.scratch_bufsize = bufsize;

	if (!path || !swarn.scratch_buf || !swarn.msg_buf)
		goto out;

	ret = extent_from_logical(fs_info, swarn.logical, path, &found_key,
				  &flags);
	if (ret < 0)
		goto out;

	extent_item_pos = swarn.logical - found_key.objectid;
	swarn.extent_item_size = found_key.offset;

	eb = path->nodes[0];
	ei = btrfs_item_ptr(eb, path->slots[0], struct btrfs_extent_item);
	item_size = btrfs_item_size_nr(eb, path->slots[0]);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		do {
			ret = tree_backref_for_extent(&ptr, eb, ei, item_size,
							&ref_root, &ref_level);
			printk_in_rcu(KERN_WARNING
				"btrfs: %s at logical %llu on dev %s, "
				"sector %llu: metadata %s (level %d) in tree "
				"%llu\n", errstr, swarn.logical,
				rcu_str_deref(dev->name),
				(unsigned long long)swarn.sector,
				ref_level ? "node" : "leaf",
				ret < 0 ? -1 : ref_level,
				ret < 0 ? -1 : ref_root);
		} while (ret != 1);
		btrfs_release_path(path);
	} else {
		btrfs_release_path(path);
		swarn.path = path;
		swarn.dev = dev;
		iterate_extent_inodes(fs_info, found_key.objectid,
					extent_item_pos, 1,
					scrub_print_warning_inode, &swarn);
	}

out:
	btrfs_free_path(path);
	kfree(swarn.scratch_buf);
	kfree(swarn.msg_buf);
}

static int scrub_fixup_readpage(u64 inum, u64 offset, u64 root, void *fixup_ctx)
{
	struct page *page = NULL;
	unsigned long index;
	struct scrub_fixup_nodatasum *fixup = fixup_ctx;
	int ret;
	int corrected = 0;
	struct btrfs_key key;
	struct inode *inode = NULL;
	struct btrfs_fs_info *fs_info;
	u64 end = offset + PAGE_SIZE - 1;
	struct btrfs_root *local_root;
	int srcu_index;

	key.objectid = root;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	fs_info = fixup->root->fs_info;
	srcu_index = srcu_read_lock(&fs_info->subvol_srcu);

	local_root = btrfs_read_fs_root_no_name(fs_info, &key);
	if (IS_ERR(local_root)) {
		srcu_read_unlock(&fs_info->subvol_srcu, srcu_index);
		return PTR_ERR(local_root);
	}

	key.type = BTRFS_INODE_ITEM_KEY;
	key.objectid = inum;
	key.offset = 0;
	inode = btrfs_iget(fs_info->sb, &key, local_root, NULL);
	srcu_read_unlock(&fs_info->subvol_srcu, srcu_index);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	index = offset >> PAGE_CACHE_SHIFT;

	page = find_or_create_page(inode->i_mapping, index, GFP_NOFS);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	if (PageUptodate(page)) {
		if (PageDirty(page)) {
			/*
			 * we need to write the data to the defect sector. the
			 * data that was in that sector is not in memory,
			 * because the page was modified. we must not write the
			 * modified page to that sector.
			 *
			 * TODO: what could be done here: wait for the delalloc
			 *       runner to write out that page (might involve
			 *       COW) and see whether the sector is still
			 *       referenced afterwards.
			 *
			 * For the meantime, we'll treat this error
			 * incorrectable, although there is a chance that a
			 * later scrub will find the bad sector again and that
			 * there's no dirty page in memory, then.
			 */
			ret = -EIO;
			goto out;
		}
		fs_info = BTRFS_I(inode)->root->fs_info;
		ret = repair_io_failure(fs_info, offset, PAGE_SIZE,
					fixup->logical, page,
					fixup->mirror_num);
		unlock_page(page);
		corrected = !ret;
	} else {
		/*
		 * we need to get good data first. the general readpage path
		 * will call repair_io_failure for us, we just have to make
		 * sure we read the bad mirror.
		 */
		ret = set_extent_bits(&BTRFS_I(inode)->io_tree, offset, end,
					EXTENT_DAMAGED, GFP_NOFS);
		if (ret) {
			/* set_extent_bits should give proper error */
			WARN_ON(ret > 0);
			if (ret > 0)
				ret = -EFAULT;
			goto out;
		}

		ret = extent_read_full_page(&BTRFS_I(inode)->io_tree, page,
						btrfs_get_extent,
						fixup->mirror_num);
		wait_on_page_locked(page);

		corrected = !test_range_bit(&BTRFS_I(inode)->io_tree, offset,
						end, EXTENT_DAMAGED, 0, NULL);
		if (!corrected)
			clear_extent_bits(&BTRFS_I(inode)->io_tree, offset, end,
						EXTENT_DAMAGED, GFP_NOFS);
	}

out:
	if (page)
		put_page(page);
	if (inode)
		iput(inode);

	if (ret < 0)
		return ret;

	if (ret == 0 && corrected) {
		/*
		 * we only need to call readpage for one of the inodes belonging
		 * to this extent. so make iterate_extent_inodes stop
		 */
		return 1;
	}

	return -EIO;
}

static void scrub_fixup_nodatasum(struct btrfs_work *work)
{
	int ret;
	struct scrub_fixup_nodatasum *fixup;
	struct scrub_ctx *sctx;
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_fs_info *fs_info;
	struct btrfs_path *path;
	int uncorrectable = 0;

	fixup = container_of(work, struct scrub_fixup_nodatasum, work);
	sctx = fixup->sctx;
	fs_info = fixup->root->fs_info;

	path = btrfs_alloc_path();
	if (!path) {
		spin_lock(&sctx->stat_lock);
		++sctx->stat.malloc_errors;
		spin_unlock(&sctx->stat_lock);
		uncorrectable = 1;
		goto out;
	}

	trans = btrfs_join_transaction(fixup->root);
	if (IS_ERR(trans)) {
		uncorrectable = 1;
		goto out;
	}

	/*
	 * the idea is to trigger a regular read through the standard path. we
	 * read a page from the (failed) logical address by specifying the
	 * corresponding copynum of the failed sector. thus, that readpage is
	 * expected to fail.
	 * that is the point where on-the-fly error correction will kick in
	 * (once it's finished) and rewrite the failed sector if a good copy
	 * can be found.
	 */
	ret = iterate_inodes_from_logical(fixup->logical, fixup->root->fs_info,
						path, scrub_fixup_readpage,
						fixup);
	if (ret < 0) {
		uncorrectable = 1;
		goto out;
	}
	WARN_ON(ret != 1);

	spin_lock(&sctx->stat_lock);
	++sctx->stat.corrected_errors;
	spin_unlock(&sctx->stat_lock);

out:
	if (trans && !IS_ERR(trans))
		btrfs_end_transaction(trans, fixup->root);
	if (uncorrectable) {
		spin_lock(&sctx->stat_lock);
		++sctx->stat.uncorrectable_errors;
		spin_unlock(&sctx->stat_lock);
		btrfs_dev_replace_stats_inc(
			&sctx->dev_root->fs_info->dev_replace.
			num_uncorrectable_read_errors);
		printk_ratelimited_in_rcu(KERN_ERR
			"btrfs: unable to fixup (nodatasum) error at logical %llu on dev %s\n",
			fixup->logical, rcu_str_deref(fixup->dev->name));
	}

	btrfs_free_path(path);
	kfree(fixup);

	scrub_pending_trans_workers_dec(sctx);
}

/*
 * scrub_handle_errored_block gets called when either verification of the
 * pages failed or the bio failed to read, e.g. with EIO. In the latter
 * case, this function handles all pages in the bio, even though only one
 * may be bad.
 * The goal of this function is to repair the errored block by using the
 * contents of one of the mirrors.
 */
static int scrub_handle_errored_block(struct scrub_block *sblock_to_check)
{
	struct scrub_ctx *sctx = sblock_to_check->sctx;
	struct btrfs_device *dev;
	struct btrfs_fs_info *fs_info;
	u64 length;
	u64 logical;
	u64 generation;
	unsigned int failed_mirror_index;
	unsigned int is_metadata;
	unsigned int have_csum;
	u8 *csum;
	struct scrub_block *sblocks_for_recheck; /* holds one for each mirror */
	struct scrub_block *sblock_bad;
	int ret;
	int mirror_index;
	int page_num;
	int success;
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	BUG_ON(sblock_to_check->page_count < 1);
	fs_info = sctx->dev_root->fs_info;
	if (sblock_to_check->pagev[0]->flags & BTRFS_EXTENT_FLAG_SUPER) {
		/*
		 * if we find an error in a super block, we just report it.
		 * They will get written with the next transaction commit
		 * anyway
		 */
		spin_lock(&sctx->stat_lock);
		++sctx->stat.super_errors;
		spin_unlock(&sctx->stat_lock);
		return 0;
	}
	length = sblock_to_check->page_count * PAGE_SIZE;
	logical = sblock_to_check->pagev[0]->logical;
	generation = sblock_to_check->pagev[0]->generation;
	BUG_ON(sblock_to_check->pagev[0]->mirror_num < 1);
	failed_mirror_index = sblock_to_check->pagev[0]->mirror_num - 1;
	is_metadata = !(sblock_to_check->pagev[0]->flags &
			BTRFS_EXTENT_FLAG_DATA);
	have_csum = sblock_to_check->pagev[0]->have_csum;
	csum = sblock_to_check->pagev[0]->csum;
	dev = sblock_to_check->pagev[0]->dev;

	if (sctx->is_dev_replace && !is_metadata && !have_csum) {
		sblocks_for_recheck = NULL;
		goto nodatasum_case;
	}

	/*
	 * read all mirrors one after the other. This includes to
	 * re-read the extent or metadata block that failed (that was
	 * the cause that this fixup code is called) another time,
	 * page by page this time in order to know which pages
	 * caused I/O errors and which ones are good (for all mirrors).
	 * It is the goal to handle the situation when more than one
	 * mirror contains I/O errors, but the errors do not
	 * overlap, i.e. the data can be repaired by selecting the
	 * pages from those mirrors without I/O error on the
	 * particular pages. One example (with blocks >= 2 * PAGE_SIZE)
	 * would be that mirror #1 has an I/O error on the first page,
	 * the second page is good, and mirror #2 has an I/O error on
	 * the second page, but the first page is good.
	 * Then the first page of the first mirror can be repaired by
	 * taking the first page of the second mirror, and the
	 * second page of the second mirror can be repaired by
	 * copying the contents of the 2nd page of the 1st mirror.
	 * One more note: if the pages of one mirror contain I/O
	 * errors, the checksum cannot be verified. In order to get
	 * the best data for repairing, the first attempt is to find
	 * a mirror without I/O errors and with a validated checksum.
	 * Only if this is not possible, the pages are picked from
	 * mirrors with I/O errors without considering the checksum.
	 * If the latter is the case, at the end, the checksum of the
	 * repaired area is verified in order to correctly maintain
	 * the statistics.
	 */

	sblocks_for_recheck = kzalloc(BTRFS_MAX_MIRRORS *
				     sizeof(*sblocks_for_recheck),
				     GFP_NOFS);
	if (!sblocks_for_recheck) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		sctx->stat.read_errors++;
		sctx->stat.uncorrectable_errors++;
		spin_unlock(&sctx->stat_lock);
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_READ_ERRS);
		goto out;
	}

	/* setup the context, map the logical blocks and alloc the pages */
	ret = scrub_setup_recheck_block(sctx, fs_info, sblock_to_check, length,
					logical, sblocks_for_recheck);
	if (ret) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.read_errors++;
		sctx->stat.uncorrectable_errors++;
		spin_unlock(&sctx->stat_lock);
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_READ_ERRS);
		goto out;
	}
	BUG_ON(failed_mirror_index >= BTRFS_MAX_MIRRORS);
	sblock_bad = sblocks_for_recheck + failed_mirror_index;

	/* build and submit the bios for the failed mirror, check checksums */
	scrub_recheck_block(fs_info, sblock_bad, is_metadata, have_csum,
			    csum, generation, sctx->csum_size);

	if (!sblock_bad->header_error && !sblock_bad->checksum_error &&
	    sblock_bad->no_io_error_seen) {
		/*
		 * the error disappeared after reading page by page, or
		 * the area was part of a huge bio and other parts of the
		 * bio caused I/O errors, or the block layer merged several
		 * read requests into one and the error is caused by a
		 * different bio (usually one of the two latter cases is
		 * the cause)
		 */
		spin_lock(&sctx->stat_lock);
		sctx->stat.unverified_errors++;
		spin_unlock(&sctx->stat_lock);

		if (sctx->is_dev_replace)
			scrub_write_block_to_dev_replace(sblock_bad);
		goto out;
	}

	if (!sblock_bad->no_io_error_seen) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.read_errors++;
		spin_unlock(&sctx->stat_lock);
		if (__ratelimit(&_rs))
			scrub_print_warning("i/o error", sblock_to_check);
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_READ_ERRS);
	} else if (sblock_bad->checksum_error) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.csum_errors++;
		spin_unlock(&sctx->stat_lock);
		if (__ratelimit(&_rs))
			scrub_print_warning("checksum error", sblock_to_check);
		btrfs_dev_stat_inc_and_print(dev,
					     BTRFS_DEV_STAT_CORRUPTION_ERRS);
	} else if (sblock_bad->header_error) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.verify_errors++;
		spin_unlock(&sctx->stat_lock);
		if (__ratelimit(&_rs))
			scrub_print_warning("checksum/header error",
					    sblock_to_check);
		if (sblock_bad->generation_error)
			btrfs_dev_stat_inc_and_print(dev,
				BTRFS_DEV_STAT_GENERATION_ERRS);
		else
			btrfs_dev_stat_inc_and_print(dev,
				BTRFS_DEV_STAT_CORRUPTION_ERRS);
	}

	if (sctx->readonly) {
		ASSERT(!sctx->is_dev_replace);
		goto out;
	}

	if (!is_metadata && !have_csum) {
		struct scrub_fixup_nodatasum *fixup_nodatasum;

nodatasum_case:
		WARN_ON(sctx->is_dev_replace);

		/*
		 * !is_metadata and !have_csum, this means that the data
		 * might not be COW'ed, that it might be modified
		 * concurrently. The general strategy to work on the
		 * commit root does not help in the case when COW is not
		 * used.
		 */
		fixup_nodatasum = kzalloc(sizeof(*fixup_nodatasum), GFP_NOFS);
		if (!fixup_nodatasum)
			goto did_not_correct_error;
		fixup_nodatasum->sctx = sctx;
		fixup_nodatasum->dev = dev;
		fixup_nodatasum->logical = logical;
		fixup_nodatasum->root = fs_info->extent_root;
		fixup_nodatasum->mirror_num = failed_mirror_index + 1;
		scrub_pending_trans_workers_inc(sctx);
		fixup_nodatasum->work.func = scrub_fixup_nodatasum;
		btrfs_queue_worker(&fs_info->scrub_workers,
				   &fixup_nodatasum->work);
		goto out;
	}

	/*
	 * now build and submit the bios for the other mirrors, check
	 * checksums.
	 * First try to pick the mirror which is completely without I/O
	 * errors and also does not have a checksum error.
	 * If one is found, and if a checksum is present, the full block
	 * that is known to contain an error is rewritten. Afterwards
	 * the block is known to be corrected.
	 * If a mirror is found which is completely correct, and no
	 * checksum is present, only those pages are rewritten that had
	 * an I/O error in the block to be repaired, since it cannot be
	 * determined, which copy of the other pages is better (and it
	 * could happen otherwise that a correct page would be
	 * overwritten by a bad one).
	 */
	for (mirror_index = 0;
	     mirror_index < BTRFS_MAX_MIRRORS &&
	     sblocks_for_recheck[mirror_index].page_count > 0;
	     mirror_index++) {
		struct scrub_block *sblock_other;

		if (mirror_index == failed_mirror_index)
			continue;
		sblock_other = sblocks_for_recheck + mirror_index;

		/* build and submit the bios, check checksums */
		scrub_recheck_block(fs_info, sblock_other, is_metadata,
				    have_csum, csum, generation,
				    sctx->csum_size);

		if (!sblock_other->header_error &&
		    !sblock_other->checksum_error &&
		    sblock_other->no_io_error_seen) {
			if (sctx->is_dev_replace) {
				scrub_write_block_to_dev_replace(sblock_other);
			} else {
				int force_write = is_metadata || have_csum;

				ret = scrub_repair_block_from_good_copy(
						sblock_bad, sblock_other,
						force_write);
			}
			if (0 == ret)
				goto corrected_error;
		}
	}

	/*
	 * for dev_replace, pick good pages and write to the target device.
	 */
	if (sctx->is_dev_replace) {
		success = 1;
		for (page_num = 0; page_num < sblock_bad->page_count;
		     page_num++) {
			int sub_success;

			sub_success = 0;
			for (mirror_index = 0;
			     mirror_index < BTRFS_MAX_MIRRORS &&
			     sblocks_for_recheck[mirror_index].page_count > 0;
			     mirror_index++) {
				struct scrub_block *sblock_other =
					sblocks_for_recheck + mirror_index;
				struct scrub_page *page_other =
					sblock_other->pagev[page_num];

				if (!page_other->io_error) {
					ret = scrub_write_page_to_dev_replace(
							sblock_other, page_num);
					if (ret == 0) {
						/* succeeded for this page */
						sub_success = 1;
						break;
					} else {
						btrfs_dev_replace_stats_inc(
							&sctx->dev_root->
							fs_info->dev_replace.
							num_write_errors);
					}
				}
			}

			if (!sub_success) {
				/*
				 * did not find a mirror to fetch the page
				 * from. scrub_write_page_to_dev_replace()
				 * handles this case (page->io_error), by
				 * filling the block with zeros before
				 * submitting the write request
				 */
				success = 0;
				ret = scrub_write_page_to_dev_replace(
						sblock_bad, page_num);
				if (ret)
					btrfs_dev_replace_stats_inc(
						&sctx->dev_root->fs_info->
						dev_replace.num_write_errors);
			}
		}

		goto out;
	}

	/*
	 * for regular scrub, repair those pages that are errored.
	 * In case of I/O errors in the area that is supposed to be
	 * repaired, continue by picking good copies of those pages.
	 * Select the good pages from mirrors to rewrite bad pages from
	 * the area to fix. Afterwards verify the checksum of the block
	 * that is supposed to be repaired. This verification step is
	 * only done for the purpose of statistic counting and for the
	 * final scrub report, whether errors remain.
	 * A perfect algorithm could make use of the checksum and try
	 * all possible combinations of pages from the different mirrors
	 * until the checksum verification succeeds. For example, when
	 * the 2nd page of mirror #1 faces I/O errors, and the 2nd page
	 * of mirror #2 is readable but the final checksum test fails,
	 * then the 2nd page of mirror #3 could be tried, whether now
	 * the final checksum succeedes. But this would be a rare
	 * exception and is therefore not implemented. At least it is
	 * avoided that the good copy is overwritten.
	 * A more useful improvement would be to pick the sectors
	 * without I/O error based on sector sizes (512 bytes on legacy
	 * disks) instead of on PAGE_SIZE. Then maybe 512 byte of one
	 * mirror could be repaired by taking 512 byte of a different
	 * mirror, even if other 512 byte sectors in the same PAGE_SIZE
	 * area are unreadable.
	 */

	/* can only fix I/O errors from here on */
	if (sblock_bad->no_io_error_seen)
		goto did_not_correct_error;

	success = 1;
	for (page_num = 0; page_num < sblock_bad->page_count; page_num++) {
		struct scrub_page *page_bad = sblock_bad->pagev[page_num];

		if (!page_bad->io_error)
			continue;

		for (mirror_index = 0;
		     mirror_index < BTRFS_MAX_MIRRORS &&
		     sblocks_for_recheck[mirror_index].page_count > 0;
		     mirror_index++) {
			struct scrub_block *sblock_other = sblocks_for_recheck +
							   mirror_index;
			struct scrub_page *page_other = sblock_other->pagev[
							page_num];

			if (!page_other->io_error) {
				ret = scrub_repair_page_from_good_copy(
					sblock_bad, sblock_other, page_num, 0);
				if (0 == ret) {
					page_bad->io_error = 0;
					break; /* succeeded for this page */
				}
			}
		}

		if (page_bad->io_error) {
			/* did not find a mirror to copy the page from */
			success = 0;
		}
	}

	if (success) {
		if (is_metadata || have_csum) {
			/*
			 * need to verify the checksum now that all
			 * sectors on disk are repaired (the write
			 * request for data to be repaired is on its way).
			 * Just be lazy and use scrub_recheck_block()
			 * which re-reads the data before the checksum
			 * is verified, but most likely the data comes out
			 * of the page cache.
			 */
			scrub_recheck_block(fs_info, sblock_bad,
					    is_metadata, have_csum, csum,
					    generation, sctx->csum_size);
			if (!sblock_bad->header_error &&
			    !sblock_bad->checksum_error &&
			    sblock_bad->no_io_error_seen)
				goto corrected_error;
			else
				goto did_not_correct_error;
		} else {
corrected_error:
			spin_lock(&sctx->stat_lock);
			sctx->stat.corrected_errors++;
			spin_unlock(&sctx->stat_lock);
			printk_ratelimited_in_rcu(KERN_ERR
				"btrfs: fixed up error at logical %llu on dev %s\n",
				logical, rcu_str_deref(dev->name));
		}
	} else {
did_not_correct_error:
		spin_lock(&sctx->stat_lock);
		sctx->stat.uncorrectable_errors++;
		spin_unlock(&sctx->stat_lock);
		printk_ratelimited_in_rcu(KERN_ERR
			"btrfs: unable to fixup (regular) error at logical %llu on dev %s\n",
			logical, rcu_str_deref(dev->name));
	}

out:
	if (sblocks_for_recheck) {
		for (mirror_index = 0; mirror_index < BTRFS_MAX_MIRRORS;
		     mirror_index++) {
			struct scrub_block *sblock = sblocks_for_recheck +
						     mirror_index;
			int page_index;

			for (page_index = 0; page_index < sblock->page_count;
			     page_index++) {
				sblock->pagev[page_index]->sblock = NULL;
				scrub_page_put(sblock->pagev[page_index]);
			}
		}
		kfree(sblocks_for_recheck);
	}

	return 0;
}

static int scrub_setup_recheck_block(struct scrub_ctx *sctx,
				     struct btrfs_fs_info *fs_info,
				     struct scrub_block *original_sblock,
				     u64 length, u64 logical,
				     struct scrub_block *sblocks_for_recheck)
{
	int page_index;
	int mirror_index;
	int ret;

	/*
	 * note: the two members ref_count and outstanding_pages
	 * are not used (and not set) in the blocks that are used for
	 * the recheck procedure
	 */

	page_index = 0;
	while (length > 0) {
		u64 sublen = min_t(u64, length, PAGE_SIZE);
		u64 mapped_length = sublen;
		struct btrfs_bio *bbio = NULL;

		/*
		 * with a length of PAGE_SIZE, each returned stripe
		 * represents one mirror
		 */
		ret = btrfs_map_block(fs_info, REQ_GET_READ_MIRRORS, logical,
				      &mapped_length, &bbio, 0);
		if (ret || !bbio || mapped_length < sublen) {
			kfree(bbio);
			return -EIO;
		}

		BUG_ON(page_index >= SCRUB_PAGES_PER_RD_BIO);
		for (mirror_index = 0; mirror_index < (int)bbio->num_stripes;
		     mirror_index++) {
			struct scrub_block *sblock;
			struct scrub_page *page;

			if (mirror_index >= BTRFS_MAX_MIRRORS)
				continue;

			sblock = sblocks_for_recheck + mirror_index;
			sblock->sctx = sctx;
			page = kzalloc(sizeof(*page), GFP_NOFS);
			if (!page) {
leave_nomem:
				spin_lock(&sctx->stat_lock);
				sctx->stat.malloc_errors++;
				spin_unlock(&sctx->stat_lock);
				kfree(bbio);
				return -ENOMEM;
			}
			scrub_page_get(page);
			sblock->pagev[page_index] = page;
			page->logical = logical;
			page->physical = bbio->stripes[mirror_index].physical;
			BUG_ON(page_index >= original_sblock->page_count);
			page->physical_for_dev_replace =
				original_sblock->pagev[page_index]->
				physical_for_dev_replace;
			/* for missing devices, dev->bdev is NULL */
			page->dev = bbio->stripes[mirror_index].dev;
			page->mirror_num = mirror_index + 1;
			sblock->page_count++;
			page->page = alloc_page(GFP_NOFS);
			if (!page->page)
				goto leave_nomem;
		}
		kfree(bbio);
		length -= sublen;
		logical += sublen;
		page_index++;
	}

	return 0;
}

/*
 * this function will check the on disk data for checksum errors, header
 * errors and read I/O errors. If any I/O errors happen, the exact pages
 * which are errored are marked as being bad. The goal is to enable scrub
 * to take those pages that are not errored from all the mirrors so that
 * the pages that are errored in the just handled mirror can be repaired.
 */
static void scrub_recheck_block(struct btrfs_fs_info *fs_info,
				struct scrub_block *sblock, int is_metadata,
				int have_csum, u8 *csum, u64 generation,
				u16 csum_size)
{
	int page_num;

	sblock->no_io_error_seen = 1;
	sblock->header_error = 0;
	sblock->checksum_error = 0;

	for (page_num = 0; page_num < sblock->page_count; page_num++) {
		struct bio *bio;
		struct scrub_page *page = sblock->pagev[page_num];

		if (page->dev->bdev == NULL) {
			page->io_error = 1;
			sblock->no_io_error_seen = 0;
			continue;
		}

		WARN_ON(!page->page);
		bio = btrfs_io_bio_alloc(GFP_NOFS, 1);
		if (!bio) {
			page->io_error = 1;
			sblock->no_io_error_seen = 0;
			continue;
		}
		bio->bi_bdev = page->dev->bdev;
		bio->bi_sector = page->physical >> 9;

		bio_add_page(bio, page->page, PAGE_SIZE, 0);
		if (btrfsic_submit_bio_wait(READ, bio))
			sblock->no_io_error_seen = 0;

		bio_put(bio);
	}

	if (sblock->no_io_error_seen)
		scrub_recheck_block_checksum(fs_info, sblock, is_metadata,
					     have_csum, csum, generation,
					     csum_size);

	return;
}

static void scrub_recheck_block_checksum(struct btrfs_fs_info *fs_info,
					 struct scrub_block *sblock,
					 int is_metadata, int have_csum,
					 const u8 *csum, u64 generation,
					 u16 csum_size)
{
	int page_num;
	u8 calculated_csum[BTRFS_CSUM_SIZE];
	u32 crc = ~(u32)0;
	void *mapped_buffer;

	WARN_ON(!sblock->pagev[0]->page);
	if (is_metadata) {
		struct btrfs_header *h;

		mapped_buffer = kmap_atomic(sblock->pagev[0]->page);
		h = (struct btrfs_header *)mapped_buffer;

		if (sblock->pagev[0]->logical != btrfs_stack_header_bytenr(h) ||
		    memcmp(h->fsid, fs_info->fsid, BTRFS_UUID_SIZE) ||
		    memcmp(h->chunk_tree_uuid, fs_info->chunk_tree_uuid,
			   BTRFS_UUID_SIZE)) {
			sblock->header_error = 1;
		} else if (generation != btrfs_stack_header_generation(h)) {
			sblock->header_error = 1;
			sblock->generation_error = 1;
		}
		csum = h->csum;
	} else {
		if (!have_csum)
			return;

		mapped_buffer = kmap_atomic(sblock->pagev[0]->page);
	}

	for (page_num = 0;;) {
		if (page_num == 0 && is_metadata)
			crc = btrfs_csum_data(
				((u8 *)mapped_buffer) + BTRFS_CSUM_SIZE,
				crc, PAGE_SIZE - BTRFS_CSUM_SIZE);
		else
			crc = btrfs_csum_data(mapped_buffer, crc, PAGE_SIZE);

		kunmap_atomic(mapped_buffer);
		page_num++;
		if (page_num >= sblock->page_count)
			break;
		WARN_ON(!sblock->pagev[page_num]->page);

		mapped_buffer = kmap_atomic(sblock->pagev[page_num]->page);
	}

	btrfs_csum_final(crc, calculated_csum);
	if (memcmp(calculated_csum, csum, csum_size))
		sblock->checksum_error = 1;
}

static int scrub_repair_block_from_good_copy(struct scrub_block *sblock_bad,
					     struct scrub_block *sblock_good,
					     int force_write)
{
	int page_num;
	int ret = 0;

	for (page_num = 0; page_num < sblock_bad->page_count; page_num++) {
		int ret_sub;

		ret_sub = scrub_repair_page_from_good_copy(sblock_bad,
							   sblock_good,
							   page_num,
							   force_write);
		if (ret_sub)
			ret = ret_sub;
	}

	return ret;
}

static int scrub_repair_page_from_good_copy(struct scrub_block *sblock_bad,
					    struct scrub_block *sblock_good,
					    int page_num, int force_write)
{
	struct scrub_page *page_bad = sblock_bad->pagev[page_num];
	struct scrub_page *page_good = sblock_good->pagev[page_num];

	BUG_ON(page_bad->page == NULL);
	BUG_ON(page_good->page == NULL);
	if (force_write || sblock_bad->header_error ||
	    sblock_bad->checksum_error || page_bad->io_error) {
		struct bio *bio;
		int ret;

		if (!page_bad->dev->bdev) {
			printk_ratelimited(KERN_WARNING
				"btrfs: scrub_repair_page_from_good_copy(bdev == NULL) is unexpected!\n");
			return -EIO;
		}

		bio = btrfs_io_bio_alloc(GFP_NOFS, 1);
		if (!bio)
			return -EIO;
		bio->bi_bdev = page_bad->dev->bdev;
		bio->bi_sector = page_bad->physical >> 9;

		ret = bio_add_page(bio, page_good->page, PAGE_SIZE, 0);
		if (PAGE_SIZE != ret) {
			bio_put(bio);
			return -EIO;
		}

		if (btrfsic_submit_bio_wait(WRITE, bio)) {
			btrfs_dev_stat_inc_and_print(page_bad->dev,
				BTRFS_DEV_STAT_WRITE_ERRS);
			btrfs_dev_replace_stats_inc(
				&sblock_bad->sctx->dev_root->fs_info->
				dev_replace.num_write_errors);
			bio_put(bio);
			return -EIO;
		}
		bio_put(bio);
	}

	return 0;
}

static void scrub_write_block_to_dev_replace(struct scrub_block *sblock)
{
	int page_num;

	for (page_num = 0; page_num < sblock->page_count; page_num++) {
		int ret;

		ret = scrub_write_page_to_dev_replace(sblock, page_num);
		if (ret)
			btrfs_dev_replace_stats_inc(
				&sblock->sctx->dev_root->fs_info->dev_replace.
				num_write_errors);
	}
}

static int scrub_write_page_to_dev_replace(struct scrub_block *sblock,
					   int page_num)
{
	struct scrub_page *spage = sblock->pagev[page_num];

	BUG_ON(spage->page == NULL);
	if (spage->io_error) {
		void *mapped_buffer = kmap_atomic(spage->page);

		memset(mapped_buffer, 0, PAGE_CACHE_SIZE);
		flush_dcache_page(spage->page);
		kunmap_atomic(mapped_buffer);
	}
	return scrub_add_page_to_wr_bio(sblock->sctx, spage);
}

static int scrub_add_page_to_wr_bio(struct scrub_ctx *sctx,
				    struct scrub_page *spage)
{
	struct scrub_wr_ctx *wr_ctx = &sctx->wr_ctx;
	struct scrub_bio *sbio;
	int ret;

	mutex_lock(&wr_ctx->wr_lock);
again:
	if (!wr_ctx->wr_curr_bio) {
		wr_ctx->wr_curr_bio = kzalloc(sizeof(*wr_ctx->wr_curr_bio),
					      GFP_NOFS);
		if (!wr_ctx->wr_curr_bio) {
			mutex_unlock(&wr_ctx->wr_lock);
			return -ENOMEM;
		}
		wr_ctx->wr_curr_bio->sctx = sctx;
		wr_ctx->wr_curr_bio->page_count = 0;
	}
	sbio = wr_ctx->wr_curr_bio;
	if (sbio->page_count == 0) {
		struct bio *bio;

		sbio->physical = spage->physical_for_dev_replace;
		sbio->logical = spage->logical;
		sbio->dev = wr_ctx->tgtdev;
		bio = sbio->bio;
		if (!bio) {
			bio = btrfs_io_bio_alloc(GFP_NOFS, wr_ctx->pages_per_wr_bio);
			if (!bio) {
				mutex_unlock(&wr_ctx->wr_lock);
				return -ENOMEM;
			}
			sbio->bio = bio;
		}

		bio->bi_private = sbio;
		bio->bi_end_io = scrub_wr_bio_end_io;
		bio->bi_bdev = sbio->dev->bdev;
		bio->bi_sector = sbio->physical >> 9;
		sbio->err = 0;
	} else if (sbio->physical + sbio->page_count * PAGE_SIZE !=
		   spage->physical_for_dev_replace ||
		   sbio->logical + sbio->page_count * PAGE_SIZE !=
		   spage->logical) {
		scrub_wr_submit(sctx);
		goto again;
	}

	ret = bio_add_page(sbio->bio, spage->page, PAGE_SIZE, 0);
	if (ret != PAGE_SIZE) {
		if (sbio->page_count < 1) {
			bio_put(sbio->bio);
			sbio->bio = NULL;
			mutex_unlock(&wr_ctx->wr_lock);
			return -EIO;
		}
		scrub_wr_submit(sctx);
		goto again;
	}

	sbio->pagev[sbio->page_count] = spage;
	scrub_page_get(spage);
	sbio->page_count++;
	if (sbio->page_count == wr_ctx->pages_per_wr_bio)
		scrub_wr_submit(sctx);
	mutex_unlock(&wr_ctx->wr_lock);

	return 0;
}

static void scrub_wr_submit(struct scrub_ctx *sctx)
{
	struct scrub_wr_ctx *wr_ctx = &sctx->wr_ctx;
	struct scrub_bio *sbio;

	if (!wr_ctx->wr_curr_bio)
		return;

	sbio = wr_ctx->wr_curr_bio;
	wr_ctx->wr_curr_bio = NULL;
	WARN_ON(!sbio->bio->bi_bdev);
	scrub_pending_bio_inc(sctx);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	scrub_pending_bio_alloc_inc(sctx);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	/* process all writes in a single worker thread. Then the block layer
	 * orders the requests before sending them to the driver which
	 * doubled the write performance on spinning disks when measured
	 * with Linux 3.5 */
	btrfsic_submit_bio(WRITE, sbio->bio);
}

static void scrub_wr_bio_end_io(struct bio *bio, int err)
{
	struct scrub_bio *sbio = bio->bi_private;
	struct btrfs_fs_info *fs_info = sbio->dev->dev_root->fs_info;

	sbio->err = err;
	sbio->bio = bio;

	sbio->work.func = scrub_wr_bio_end_io_worker;
	btrfs_queue_worker(&fs_info->scrub_wr_completion_workers, &sbio->work);
}

static void scrub_wr_bio_end_io_worker(struct btrfs_work *work)
{
	struct scrub_bio *sbio = container_of(work, struct scrub_bio, work);
	struct scrub_ctx *sctx = sbio->sctx;
	int i;

	WARN_ON(sbio->page_count > SCRUB_PAGES_PER_WR_BIO);
	if (sbio->err) {
		struct btrfs_dev_replace *dev_replace =
			&sbio->sctx->dev_root->fs_info->dev_replace;

		for (i = 0; i < sbio->page_count; i++) {
			struct scrub_page *spage = sbio->pagev[i];

			spage->io_error = 1;
			btrfs_dev_replace_stats_inc(&dev_replace->
						    num_write_errors);
		}
	}

	for (i = 0; i < sbio->page_count; i++)
		scrub_page_put(sbio->pagev[i]);

	bio_put(sbio->bio);
	kfree(sbio);
	scrub_pending_bio_dec(sctx);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	scrub_pending_bio_alloc_dec(sctx);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
}

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
#define MAX_ORDERED_SUM_BYTES(r) ((PAGE_SIZE - \
			sizeof(struct btrfs_ordered_sum)) / \
			sizeof(u32) * (r)->sectorsize)

static int scrub_lookup_block_csum(struct scrub_block *sblock, u8 *csum)
{
	struct btrfs_key key;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_root *root = sblock->sctx->dev_root->fs_info->csum_root;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);
	struct btrfs_csum_item *item;
	unsigned long offset;
	u64 start, end, csum_end;
	size_t size;
	int i, ret, newidx;
	u32 tmpcsum;

	/* Assuming the pages in the block are contiguous? */
	start = sblock->pagev[0]->logical;
	end = start + (sblock->page_count * PAGE_SIZE) - 1;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->skip_locking = 1;
	path->reada = 2;
	path->search_commit_root = 1;

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto fail;
	if (ret > 0 && path->slots[0] > 0) {
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY) {
			offset = (start - key.offset) >>
				root->fs_info->sb->s_blocksize_bits;
			if (offset * csum_size < btrfs_item_size_nr(leaf,
			    path->slots[0] - 1))
				path->slots[0]--;
		}
	}

	while (start <= end) {
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				goto fail;
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.offset > end)
			break;
		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size_nr(leaf, path->slots[0]);
		csum_end = key.offset + (size / csum_size) * root->sectorsize;
		if (csum_end <= start) {
			path->slots[0]++;
			continue;
		}

		csum_end = min(csum_end, end + 1);
		item = btrfs_item_ptr(path->nodes[0], path->slots[0],
			struct btrfs_csum_item);
		while (start < csum_end) {
			size = min_t(size_t, csum_end - start,
				MAX_ORDERED_SUM_BYTES(root));

			offset = (start - key.offset) >>
				root->fs_info->sb->s_blocksize_bits;
			offset *= csum_size;

			newidx = 0;
			while (size > 0) {
				read_extent_buffer(path->nodes[0], &tmpcsum,
					((unsigned long)item) + offset,
					csum_size);
				for (i = 0; i < csum_size; i++) {
					csum[newidx+i] = (u8) tmpcsum & 0xff;
					tmpcsum = tmpcsum >> 8;
				}
				newidx += csum_size;

				size -= root->sectorsize;
				start += root->sectorsize;
				offset += csum_size;
			}
		}
		path->slots[0]++;
	}
	ret = 0;
fail:
	btrfs_free_path(path);
	return ret;
}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

static int scrub_checksum(struct scrub_block *sblock)
{
	u64 flags;
	int ret;

	WARN_ON(sblock->page_count < 1);
	flags = sblock->pagev[0]->flags;
	ret = 0;
	if (flags & BTRFS_EXTENT_FLAG_DATA)
		ret = scrub_checksum_data(sblock);
	else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		ret = scrub_checksum_tree_block(sblock);
	else if (flags & BTRFS_EXTENT_FLAG_SUPER)
		(void)scrub_checksum_super(sblock);
	else
		WARN_ON(1);
	if (ret) {
		printk(KERN_DEBUG "scrub_checksum: calling scrub_handle_errored_block");
		scrub_handle_errored_block(sblock);
	}

	return ret;
}

static int scrub_checksum_data(struct scrub_block *sblock)
{
	struct scrub_ctx *sctx = sblock->sctx;
	u8 csum[BTRFS_CSUM_SIZE];
	u8 *on_disk_csum;
	struct page *page;
	void *buffer;
	u32 crc = ~(u32)0;
	int fail = 0;
	u64 len;
	int index;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	u8 new_csum[BTRFS_CSUM_SIZE];
	char buf[256];
	int i;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

	BUG_ON(sblock->page_count < 1);
	if (!sblock->pagev[0]->have_csum)
		return 0;

	on_disk_csum = sblock->pagev[0]->csum;
	page = sblock->pagev[0]->page;
	buffer = kmap_atomic(page);

	len = sctx->sectorsize;
	index = 0;
	for (;;) {
		u64 l = min_t(u64, len, PAGE_SIZE);

		crc = btrfs_csum_data(buffer, crc, l);
		kunmap_atomic(buffer);
		len -= l;
		if (len == 0)
			break;
		index++;
		BUG_ON(index >= sblock->page_count);
		BUG_ON(!sblock->pagev[index]->page);
		page = sblock->pagev[index]->page;
		buffer = kmap_atomic(page);
	}

	btrfs_csum_final(crc, csum);
	if (memcmp(csum, on_disk_csum, sctx->csum_size)) {
#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
		fail = 1;
#else /* Adaptive scrub code */
		/*
		 * Make sure the checksum we hold is not stale by re-reading
		 * it. We don't go through this process for metadata blocks,
		 * because "check-summing" for them means field sanity checking.
		 */
		sprintf(buf, "scrub_checksum_data: couldn't verify %02x",
			csum[0]);
		for (i = 1; i < sctx->csum_size; i++)
			sprintf(buf, "%s:%02x", buf, csum[i]);

		sprintf(buf, "%s, with ondisk checksum %02x", buf,
			on_disk_csum[0]);
		for (i = 1; i < sctx->csum_size; i++)
			sprintf(buf, "%s:%02x", buf, on_disk_csum[i]);

		scrub_lookup_block_csum(sblock, new_csum);

		sprintf(buf, "%s (re-read yielded %02x", buf, new_csum[0]);
		for (i = 1; i < sctx->csum_size; i++)
			sprintf(buf, "%s:%02x", buf, new_csum[i]);
		printk(KERN_DEBUG "%s).\n", buf);

		if (memcmp(csum, new_csum, sctx->csum_size)) {
			fail = 1;
		} else {
			spin_lock(&sctx->stat_lock);
			sctx->stat.sync_errors++;
			spin_unlock(&sctx->stat_lock);
		}
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
	}

	return fail;
}

static int scrub_checksum_tree_block(struct scrub_block *sblock)
{
	struct scrub_ctx *sctx = sblock->sctx;
	struct btrfs_header *h;
	struct btrfs_root *root = sctx->dev_root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u8 calculated_csum[BTRFS_CSUM_SIZE];
	u8 on_disk_csum[BTRFS_CSUM_SIZE];
	struct page *page;
	void *mapped_buffer;
	u64 mapped_size;
	void *p;
	u32 crc = ~(u32)0;
	int fail = 0;
	int crc_fail = 0;
	u64 len;
	int index;

	BUG_ON(sblock->page_count < 1);
	page = sblock->pagev[0]->page;
	mapped_buffer = kmap_atomic(page);
	h = (struct btrfs_header *)mapped_buffer;
	memcpy(on_disk_csum, h->csum, sctx->csum_size);

	/*
	 * we don't use the getter functions here, as we
	 * a) don't have an extent buffer and
	 * b) the page is already kmapped
	 */

	if (sblock->pagev[0]->logical != btrfs_stack_header_bytenr(h))
		++fail;

	if (sblock->pagev[0]->generation != btrfs_stack_header_generation(h))
		++fail;

	if (memcmp(h->fsid, fs_info->fsid, BTRFS_UUID_SIZE))
		++fail;

	if (memcmp(h->chunk_tree_uuid, fs_info->chunk_tree_uuid,
		   BTRFS_UUID_SIZE))
		++fail;

	WARN_ON(sctx->nodesize != sctx->leafsize);
	len = sctx->nodesize - BTRFS_CSUM_SIZE;
	mapped_size = PAGE_SIZE - BTRFS_CSUM_SIZE;
	p = ((u8 *)mapped_buffer) + BTRFS_CSUM_SIZE;
	index = 0;
	for (;;) {
		u64 l = min_t(u64, len, mapped_size);

		crc = btrfs_csum_data(p, crc, l);
		kunmap_atomic(mapped_buffer);
		len -= l;
		if (len == 0)
			break;
		index++;
		BUG_ON(index >= sblock->page_count);
		BUG_ON(!sblock->pagev[index]->page);
		page = sblock->pagev[index]->page;
		mapped_buffer = kmap_atomic(page);
		mapped_size = PAGE_SIZE;
		p = mapped_buffer;
	}

	btrfs_csum_final(crc, calculated_csum);
	if (memcmp(calculated_csum, on_disk_csum, sctx->csum_size))
		++crc_fail;

	return fail || crc_fail;
}

static int scrub_checksum_super(struct scrub_block *sblock)
{
	struct btrfs_super_block *s;
	struct scrub_ctx *sctx = sblock->sctx;
	struct btrfs_root *root = sctx->dev_root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u8 calculated_csum[BTRFS_CSUM_SIZE];
	u8 on_disk_csum[BTRFS_CSUM_SIZE];
	struct page *page;
	void *mapped_buffer;
	u64 mapped_size;
	void *p;
	u32 crc = ~(u32)0;
	int fail_gen = 0;
	int fail_cor = 0;
	u64 len;
	int index;

	BUG_ON(sblock->page_count < 1);
	page = sblock->pagev[0]->page;
	mapped_buffer = kmap_atomic(page);
	s = (struct btrfs_super_block *)mapped_buffer;
	memcpy(on_disk_csum, s->csum, sctx->csum_size);

	if (sblock->pagev[0]->logical != btrfs_super_bytenr(s))
		++fail_cor;

	if (sblock->pagev[0]->generation != btrfs_super_generation(s))
		++fail_gen;

	if (memcmp(s->fsid, fs_info->fsid, BTRFS_UUID_SIZE))
		++fail_cor;

	len = BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE;
	mapped_size = PAGE_SIZE - BTRFS_CSUM_SIZE;
	p = ((u8 *)mapped_buffer) + BTRFS_CSUM_SIZE;
	index = 0;
	for (;;) {
		u64 l = min_t(u64, len, mapped_size);

		crc = btrfs_csum_data(p, crc, l);
		kunmap_atomic(mapped_buffer);
		len -= l;
		if (len == 0)
			break;
		index++;
		BUG_ON(index >= sblock->page_count);
		BUG_ON(!sblock->pagev[index]->page);
		page = sblock->pagev[index]->page;
		mapped_buffer = kmap_atomic(page);
		mapped_size = PAGE_SIZE;
		p = mapped_buffer;
	}

	btrfs_csum_final(crc, calculated_csum);
	if (memcmp(calculated_csum, on_disk_csum, sctx->csum_size))
		++fail_cor;

	if (fail_cor + fail_gen) {
		/*
		 * if we find an error in a super block, we just report it.
		 * They will get written with the next transaction commit
		 * anyway
		 */
		spin_lock(&sctx->stat_lock);
		++sctx->stat.super_errors;
		spin_unlock(&sctx->stat_lock);
		if (fail_cor)
			btrfs_dev_stat_inc_and_print(sblock->pagev[0]->dev,
				BTRFS_DEV_STAT_CORRUPTION_ERRS);
		else
			btrfs_dev_stat_inc_and_print(sblock->pagev[0]->dev,
				BTRFS_DEV_STAT_GENERATION_ERRS);
	}

	return fail_cor + fail_gen;
}

static void scrub_block_get(struct scrub_block *sblock)
{
	atomic_inc(&sblock->ref_count);
}

static void scrub_block_put(struct scrub_block *sblock)
{
	if (atomic_dec_and_test(&sblock->ref_count)) {
		int i;

		for (i = 0; i < sblock->page_count; i++)
			scrub_page_put(sblock->pagev[i]);
		kfree(sblock);
	}
}

static void scrub_page_get(struct scrub_page *spage)
{
	atomic_inc(&spage->ref_count);
}

static void scrub_page_put(struct scrub_page *spage)
{
	if (atomic_dec_and_test(&spage->ref_count)) {
		if (spage->page)
			__free_page(spage->page);
		kfree(spage);
	}
}

static void scrub_submit(struct scrub_ctx *sctx)
{
	struct scrub_bio *sbio;

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	if (sctx->deadline) {
		/* keep remove_bio() out while grabbing curr */
		spin_lock(&sctx->curr_lock);
	}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	if (sctx->curr == -1) {
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
		if (sctx->deadline)
			spin_unlock(&sctx->curr_lock);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
		return;
	}

	sbio = sctx->bios[sctx->curr];
	sctx->curr = -1;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	if (sctx->deadline)
		spin_unlock(&sctx->curr_lock);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	scrub_pending_bio_inc(sctx);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	scrub_pending_bio_alloc_inc(sctx);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

	if (!sbio->bio->bi_bdev) {
		/*
		 * this case should not happen. If btrfs_map_block() is
		 * wrong, it could happen for dev-replace operations on
		 * missing devices when no mirrors are available, but in
		 * this case it should already fail the mount.
		 * This case is handled correctly (but _very_ slowly).
		 */
		printk_ratelimited(KERN_WARNING
			"btrfs: scrub_submit(bio bdev == NULL) is unexpected!\n");
		bio_endio(sbio->bio, -EIO);
	} else {
		btrfsic_submit_bio(READ, sbio->bio);
	}
}

static int scrub_add_page_to_rd_bio(struct scrub_ctx *sctx,
				    struct scrub_page *spage)
{
	struct scrub_block *sblock = spage->sblock;
	struct scrub_bio *sbio;
	int ret;

again:
	/*
	 * grab a fresh bio or wait for one to become available
	 */
	while (sctx->curr == -1) {
		spin_lock(&sctx->list_lock);
		sctx->curr = sctx->first_free;
		if (sctx->curr != -1) {
			sctx->first_free = sctx->bios[sctx->curr]->next_free;
			sctx->bios[sctx->curr]->next_free = -1;
			sctx->bios[sctx->curr]->page_count = 0;
			spin_unlock(&sctx->list_lock);
		} else {
			spin_unlock(&sctx->list_lock);
#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
			wait_event(sctx->list_wait, sctx->first_free != -1);
#else /* Adaptive scrub code */
			if (sctx->deadline) {
				if (atomic_read(&sctx->dev_root->fs_info->scrub_pause_req)) {
					mutex_lock(&sctx->bios_lock);
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
					printk(KERN_DEBUG "scrub_add_page_to_rd_bio: allocating"
						"another bio (%u).\n", sctx->bios_per_sctx + 1);
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
					scrub_realloc_bios_array(sctx, sctx->bios_per_sctx + 1);

					if (!sctx->deadline)
						atomic_inc(&sctx->pending_removals);
					mutex_unlock(&sctx->bios_lock);
				}

				wait_event_timeout(sctx->list_wait,
					sctx->first_free != -1, BTRFS_SCRUB_WAIT_TO);
			} else {
				wait_event(sctx->list_wait, sctx->first_free != -1);
			}
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
		}
	}
	sbio = sctx->bios[sctx->curr];
	if (sbio->page_count == 0) {
		struct bio *bio;

		sbio->physical = spage->physical;
		sbio->logical = spage->logical;
		sbio->dev = spage->dev;
		bio = sbio->bio;
		if (!bio) {
			bio = btrfs_io_bio_alloc(GFP_NOFS, sctx->pages_per_rd_bio);
			if (!bio)
				return -ENOMEM;
			sbio->bio = bio;
		}

		bio->bi_private = sbio;
		bio->bi_end_io = scrub_bio_end_io;
		bio->bi_bdev = sbio->dev->bdev;
		bio->bi_sector = sbio->physical >> 9;
		sbio->err = 0;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
		/* We only need to do this for a fresh bio */
		sbio->t_start = jiffies;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	} else if (sbio->physical + sbio->page_count * PAGE_SIZE !=
		   spage->physical ||
		   sbio->logical + sbio->page_count * PAGE_SIZE !=
		   spage->logical ||
		   sbio->dev != spage->dev) {
		scrub_submit(sctx);
		goto again;
	}

	sbio->pagev[sbio->page_count] = spage;
	ret = bio_add_page(sbio->bio, spage->page, PAGE_SIZE, 0);
	if (ret != PAGE_SIZE) {
		if (sbio->page_count < 1) {
			bio_put(sbio->bio);
			sbio->bio = NULL;
			return -EIO;
		}
		scrub_submit(sctx);
		goto again;
	}

	scrub_block_get(sblock); /* one for the page added to the bio */
	atomic_inc(&sblock->outstanding_pages);
	sbio->page_count++;
	if (sbio->page_count == sctx->pages_per_rd_bio)
		scrub_submit(sctx);

	return 0;
}

static int scrub_pages(struct scrub_ctx *sctx, u64 logical, u64 len,
		       u64 physical, struct btrfs_device *dev, u64 flags,
		       u64 gen, int mirror_num, u8 *csum, int force,
		       u64 physical_for_dev_replace)
{
	struct scrub_block *sblock;
	int index;

	sblock = kzalloc(sizeof(*sblock), GFP_NOFS);
	if (!sblock) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		spin_unlock(&sctx->stat_lock);
		return -ENOMEM;
	}

	/* one ref inside this function, plus one for each page added to
	 * a bio later on */
	atomic_set(&sblock->ref_count, 1);
	sblock->sctx = sctx;
	sblock->no_io_error_seen = 1;

	for (index = 0; len > 0; index++) {
		struct scrub_page *spage;
		u64 l = min_t(u64, len, PAGE_SIZE);

		spage = kzalloc(sizeof(*spage), GFP_NOFS);
		if (!spage) {
leave_nomem:
			spin_lock(&sctx->stat_lock);
			sctx->stat.malloc_errors++;
			spin_unlock(&sctx->stat_lock);
			scrub_block_put(sblock);
			return -ENOMEM;
		}
		BUG_ON(index >= SCRUB_MAX_PAGES_PER_BLOCK);
		scrub_page_get(spage);
		sblock->pagev[index] = spage;
		spage->sblock = sblock;
		spage->dev = dev;
		spage->flags = flags;
		spage->generation = gen;
		spage->logical = logical;
		spage->physical = physical;
		spage->physical_for_dev_replace = physical_for_dev_replace;
		spage->mirror_num = mirror_num;
		if (csum) {
			spage->have_csum = 1;
			memcpy(spage->csum, csum, sctx->csum_size);
		} else {
			spage->have_csum = 0;
		}
		sblock->page_count++;
		spage->page = alloc_page(GFP_NOFS);
		if (!spage->page)
			goto leave_nomem;
		len -= l;
		logical += l;
		physical += l;
		physical_for_dev_replace += l;
	}

	WARN_ON(sblock->page_count == 0);
	for (index = 0; index < sblock->page_count; index++) {
		struct scrub_page *spage = sblock->pagev[index];
		int ret;

		ret = scrub_add_page_to_rd_bio(sctx, spage);
		if (ret) {
			scrub_block_put(sblock);
			return ret;
		}
	}

	if (force)
		scrub_submit(sctx);

	/* last one frees, either here or in bio completion for last page */
	scrub_block_put(sblock);
	return 0;
}

static void scrub_bio_end_io(struct bio *bio, int err)
{
	struct scrub_bio *sbio = bio->bi_private;
	struct btrfs_fs_info *fs_info = sbio->dev->dev_root->fs_info;

	sbio->err = err;
	sbio->bio = bio;

#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
	printk(KERN_DEBUG "scrub_bio_end_io: queueing work (bio %d)\n",
		sbio->index);
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
	btrfs_queue_worker(&fs_info->scrub_workers, &sbio->work);
}

static void scrub_bio_end_io_worker(struct btrfs_work *work)
{
	struct scrub_bio *sbio = container_of(work, struct scrub_bio, work);
	struct scrub_ctx *sctx = sbio->sctx;
	int i;
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	unsigned long j, scaled_delay, diffjiff = 0;
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

	BUG_ON(sbio->page_count > SCRUB_PAGES_PER_RD_BIO);
	if (sbio->err) {
		for (i = 0; i < sbio->page_count; i++) {
			struct scrub_page *spage = sbio->pagev[i];

			spage->io_error = 1;
			spage->sblock->no_io_error_seen = 0;
		}
	}

	/* now complete the scrub_block items that have all pages completed */
	for (i = 0; i < sbio->page_count; i++) {
		struct scrub_page *spage = sbio->pagev[i];
		struct scrub_block *sblock = spage->sblock;

		if (atomic_dec_and_test(&sblock->outstanding_pages))
			scrub_block_complete(sblock);
		scrub_block_put(sblock);
	}

	bio_put(sbio->bio);
	sbio->bio = NULL;
	/* Normally, we'd add the bio in the pool now, but we'll have to
	 * hold on to it in case we need to throttle scrubber bandwidth */

	if (sctx->is_dev_replace &&
	    atomic_read(&sctx->wr_ctx.flush_all_writes)) {
		mutex_lock(&sctx->wr_ctx.wr_lock);
		scrub_wr_submit(sctx);
		mutex_unlock(&sctx->wr_ctx.wr_lock);
	}

#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
	scrub_bio_end_io_wrapup_worker(&sbio->work_end);
#else /* Adaptive scrubber code */
	if (sctx->deadline) {
		/* Calling scrub_pending_bio_dec(sctx) would also wake
		 * up people waiting on list_wait; no need to disturb them
		 * though. Call atomic_dec directly, instead. */
		atomic_dec(&sctx->bios_in_flight);

		if (atomic_read(&sctx->dev_root->fs_info->scrub_pause_req)) {
			/* We can't delay now. Keep going (finish stripe), and
			 * we'll adjust for slower scrubbing in the future. When
			 * the scrubber is unpaused, it'll check if the delays
			 * for all bios are over. If not, it will wait. */
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
			printk(KERN_DEBUG "scrub_bio_end_io_worker: no delay "
				"(bio %d, in-flight %d)\n", sbio->index,
				atomic_read(&sctx->bios_in_flight));
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
			scrub_bio_end_io_wrapup_worker(&sbio->work_end);
		} else {
			/* Introduce delay to control scrubbing rate. We hold
			 * the bio for sctx->delay seconds, and set a timer to
			 * release it afterwards. */
			j = jiffies;
			scaled_delay = ((HZ * atomic64_read(&sctx->delay) *
				sbio->page_count) / SCRUB_PAGES_PER_RD_BIO);

			if (scaled_delay > (j - sbio->t_start) + sbio->t_wasted)
				diffjiff = scaled_delay - ((j -
					sbio->t_start) + sbio->t_wasted);

			sbio->timer_start = j;
			sbio->timer_expires = diffjiff;
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
			printk(KERN_DEBUG "scrub_bio_end_io_worker: being "
				"delayed (exp %ld, bio %d, in-flight %d)\n",
				j+diffjiff, sbio->index,
				atomic_read(&sctx->bios_in_flight));
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
			if (diffjiff > 0) {
				sbio->timer.function = scrub_bio_end_io_timer;
				sbio->timer.data = (unsigned long)sbio;
				sbio->timer.expires = j + diffjiff;

				/* Add timer to list of active timers */
				add_timer(&sbio->timer);
			} else {
				sbio->t_wasted = jiffies;
				scrub_bio_end_io_wrapup_worker(&sbio->work_end);
			}
		}
	} else {
		scrub_bio_end_io_wrapup_worker(&sbio->work_end);
	}
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
}

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
static void scrub_bio_end_io_timer(unsigned long arg)
{
	struct scrub_bio *sbio = (struct scrub_bio *)arg;
	struct btrfs_fs_info *fs_info = sbio->dev->dev_root->fs_info;

#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
	printk(KERN_DEBUG "btrfs scrub: In [scrub_bio_end_io_timer] "
		"(jiffies=%ld, bio %d, in-flight %d)\n", jiffies,
		sbio->index, atomic_read(&sbio->sctx->bios_in_flight));
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
	sbio->t_wasted = jiffies;
	btrfs_queue_worker(&fs_info->scrub_workers, &sbio->work_end);
}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */

#ifdef CONFIG_BTRFS_FS_SCRUB_BOOST
/* ioprio of boosted scrubbing is set to best-effort, with priority 4 */
#define BTRFS_IOPRIO_BOOSTED (IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4))
#endif /* CONFIG_BTRFS_FS_SCRUB_BOOST */
static void scrub_bio_end_io_wrapup_worker(struct btrfs_work *work)
{
	struct scrub_bio *sbio = container_of(work, struct scrub_bio,
		work_end);
	struct scrub_ctx *sctx = sbio->sctx;
#ifdef CONFIG_BTRFS_FS_SCRUB_NONE

	spin_lock(&sctx->list_lock);
	sbio->next_free = sctx->first_free;
	sctx->first_free = sbio->index;
	spin_unlock(&sctx->list_lock);

	/* We still need to do this, because we linked to the bio from the free
	 * list, so interested threads should be informed. Also decrements bios
	 * in flight, so we can proceed with pause requests. */
	scrub_pending_bio_dec(sctx);
#else /* Adaptive scrubber code */
	u64 progress, goal, min_inc, elapsed;
	u16 bios_per_sctx;
	struct timeval cur;
	long delay;

	if (sctx->deadline) {
		do_gettimeofday(&cur);
		elapsed = cur.tv_sec - sctx->t_start.tv_sec;

		/* Goal, progress, and min_inc all calculated in bytes */
		goal = (elapsed * sctx->used_bytes) / sctx->deadline;
		progress = sctx->stat.data_bytes_scrubbed +
			sctx->stat.tree_bytes_scrubbed;
		min_inc = PAGE_SIZE * SCRUB_PAGES_PER_RD_BIO; /* 1bio = 128kb */

		/* Check if we fell behind, or if we're ahead by more than min_inc */
		if (elapsed > sctx->deadline || progress + min_inc < goal ||
		    goal + min_inc < progress) {
#ifdef CONFIG_BTRFS_FS_SCRUB_BOOST
			if (sctx->bgflags & BTRFS_BGSC_BOOST) {
				if (progress + 100 * min_inc < goal) {
					sctx->old_ioprio = IOPRIO_PRIO_VALUE(
						task_nice_ioclass(current),
						task_nice_ioprio(current));
					set_task_ioprio(current,
						BTRFS_IOPRIO_BOOSTED);
				} else if (progress > goal &&
					   sctx->old_ioprio != -1) {
					set_task_ioprio(current,
						sctx->old_ioprio);
					sctx->old_ioprio = -1;
				}
			}
#endif /* CONFIG_BTRFS_FS_SCRUB_BOOST */

			mutex_lock(&sctx->bios_lock);
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
			printk(KERN_INFO "btrfs scrub: About to adjust "
				"(goal %llu, progress %llu, min_inc %llu).\n",
				goal, progress, min_inc);
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
			scrub_adjust_rate(sctx, elapsed, progress,
				&bios_per_sctx, &delay);
			if (bios_per_sctx > sctx->bios_per_sctx) {
				/* Allocate new bios, update bios_per_sctx */
				printk(KERN_INFO "btrfs scrub: Need to allocate"
					" more bios -- %u to %u.\n",
					sctx->bios_per_sctx, bios_per_sctx);
				scrub_realloc_bios_array(sctx, bios_per_sctx);
			} else if (bios_per_sctx < sctx->bios_per_sctx) {
				/* Update pending_removals, also marking this
				 * bio to be freed */
				atomic_set(&sctx->pending_removals,
					sctx->bios_per_sctx - bios_per_sctx);
				printk(KERN_INFO "btrfs scrub: Need to free "
					"some bios -- %u to %u.\n",
					sctx->bios_per_sctx, bios_per_sctx);
			}
			mutex_unlock(&sctx->bios_lock);

			/*
			 * Delay decreased: speed things up by increasing bios
			 * Delay increased: slow down by deleting some bios, or
			 * just increase the delay
			 * We go ahead and update delay (if needed) right away
			 */
			if (atomic64_read(&sctx->delay) != delay) {
				printk(KERN_INFO "btrfs scrub: adjusting delay"
					" -- %ld to %ld.\n",
					atomic64_read(&sctx->delay), delay);
				atomic64_set(&sctx->delay, delay);
			}
		}

		if (!atomic_read(&sctx->pending_removals)) {
			/* account for lost time */
			sbio->t_wasted = jiffies - sbio->t_wasted;
			spin_lock(&sctx->list_lock);
			sbio->next_free = sctx->first_free;
			sctx->first_free = sbio->index;
			spin_unlock(&sctx->list_lock);
		} else {
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
			printk(KERN_DEBUG "scrub_bio_end_io_wrapup_worker: "
				"(removing bio %d -- pending removals: %d)\n",
				sbio->index, atomic_read(&sctx->pending_removals));
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
			mutex_lock(&sctx->bios_lock);
			scrub_remove_bio(sctx, sbio->index);
			mutex_unlock(&sctx->bios_lock);
		}
	} else {
		/* account for lost time */
		sbio->t_wasted = jiffies - sbio->t_wasted;
		spin_lock(&sctx->list_lock);
		sbio->next_free = sctx->first_free;
		sctx->first_free = sbio->index;
		spin_unlock(&sctx->list_lock);

		/* We don't need to wake anyone up yet, we'll do this outside
		 * this block when scrub_pending_bio_alloc_dec() is called */
		atomic_dec(&sctx->bios_in_flight);
	}

	/* Still need this, because we linked the bio from the free list */
	scrub_pending_bio_alloc_dec(sctx);
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
}

static void scrub_block_complete(struct scrub_block *sblock)
{
	if (!sblock->no_io_error_seen) {
		scrub_handle_errored_block(sblock);
	} else {
		/*
		 * if has checksum error, write via repair mechanism in
		 * dev replace case, otherwise write here in dev replace
		 * case.
		 */
		if (!scrub_checksum(sblock) && sblock->sctx->is_dev_replace)
			scrub_write_block_to_dev_replace(sblock);
	}
}

static int scrub_find_csum(struct scrub_ctx *sctx, u64 logical, u64 len,
			   u8 *csum)
{
	struct btrfs_ordered_sum *sum = NULL;
	unsigned long index;
	unsigned long num_sectors;

	while (!list_empty(&sctx->csum_list)) {
		sum = list_first_entry(&sctx->csum_list,
				       struct btrfs_ordered_sum, list);
		if (sum->bytenr > logical)
			return 0;
		if (sum->bytenr + sum->len > logical)
			break;

		++sctx->stat.csum_discards;
		list_del(&sum->list);
		kfree(sum);
		sum = NULL;
	}
	if (!sum)
		return 0;

	index = ((u32)(logical - sum->bytenr)) / sctx->sectorsize;
	num_sectors = sum->len / sctx->sectorsize;
	memcpy(csum, sum->sums + index, sctx->csum_size);
	if (index == num_sectors - 1) {
		list_del(&sum->list);
		kfree(sum);
	}
	return 1;
}

/* scrub extent tries to collect up to 64 kB for each bio */
/*
 * Starts by breaking the extent down to segments of size min(u64, extent_length,
 * block_size), and calls scrub_pages for them. We advance between segments by
 * incrementing logical and physical by the segment size, since within a segment,
 * everything is contiguous in both the logical and physical space.
 */
static int scrub_extent(struct scrub_ctx *sctx, u64 logical, u64 len,
			u64 physical, struct btrfs_device *dev, u64 flags,
			u64 gen, int mirror_num, u64 physical_for_dev_replace)
{
	int ret=0;
	u8 csum[BTRFS_CSUM_SIZE];
	u32 blocksize;
#ifdef CONFIG_BTRFS_DUET_SCRUB
	u64 dstart = sctx->scrub_dev->bd_part->start_sect << 9;
#endif /* CONFIG_BTRFS_DUET_SCRUB */

	if (flags & BTRFS_EXTENT_FLAG_DATA) {
		blocksize = sctx->sectorsize;
		spin_lock(&sctx->stat_lock);
		sctx->stat.data_extents_scrubbed++;
		sctx->stat.data_bytes_scrubbed += len;
		spin_unlock(&sctx->stat_lock);
	} else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		WARN_ON(sctx->nodesize != sctx->leafsize);
		blocksize = sctx->nodesize;
		spin_lock(&sctx->stat_lock);
		sctx->stat.tree_extents_scrubbed++;
		sctx->stat.tree_bytes_scrubbed += len;
		spin_unlock(&sctx->stat_lock);
	} else {
		blocksize = sctx->sectorsize;
		WARN_ON(1);
	}

	while (len) {
		u64 l = min_t(u64, len, blocksize);
		int have_csum = 0;

#ifdef CONFIG_BTRFS_DUET_SCRUB
		/* Check that: we're not replacing a device, and chk_done gives
		 * us the green light. Only then we *skip* this block. Note
		 * that chk_done does not verify we're on the right device;
		 * this should be de facto since we're calling it from here */
		scrub_dbg(KERN_INFO "duet-scrub: checking [%llu, %llu] --"
			" dstart = %llu\n",
			dstart+physical, dstart+physical+l, dstart);
		if (!sctx->is_dev_replace && (duet_check(sctx->taskid,
		    dstart + physical, l) == 1)) {
			scrub_dbg(KERN_INFO "duet-scrub: found!\n");
			goto behind_scrub_pages;
		} else if (!sctx->is_dev_replace) {
			/* We're actually getting verified */
			if (flags & BTRFS_EXTENT_FLAG_DATA) {
				spin_lock(&sctx->stat_lock);
				sctx->stat.data_bytes_verified += l;
				spin_unlock(&sctx->stat_lock);
			} else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
				spin_lock(&sctx->stat_lock);
				sctx->stat.tree_bytes_verified += l;
				spin_unlock(&sctx->stat_lock);
			}
		}
#endif /* CONFIG_BTRFS_DUET_SCRUB */

		if (flags & BTRFS_EXTENT_FLAG_DATA) {
			/* push csums to sbio */
			have_csum = scrub_find_csum(sctx, logical, l, csum);
			if (have_csum == 0)
				++sctx->stat.no_csum;
			if (sctx->is_dev_replace && !have_csum) {
				ret = copy_nocow_pages(sctx, logical, l,
						       mirror_num,
						      physical_for_dev_replace);
				goto behind_scrub_pages;
			}
		}
		ret = scrub_pages(sctx, logical, l, physical, dev, flags, gen,
				  mirror_num, have_csum ? csum : NULL, 0,
				  physical_for_dev_replace);
behind_scrub_pages:
		if (ret)
			return ret;
		len -= l;
		logical += l;
		physical += l;
		physical_for_dev_replace += l;
	}
	return 0;
}

/*
 * Starts with the first stripe of a chunk, and proceeds by finding all the
 * extents within that stripe (btrfs_search_slot) and scrubbing them through
 * scrub_extent. Extends that span more than the current stripe are split
 * between separate scrub_extent calls. In any other case, we manually
 * proceed to the next stripe by adding _increment_ bytes to the current
 * logical address (starts at base=chunk_offset). The value of _increment_
 * is calculated based on the stripe length and the type of RAID scheme used
 * (e.g. equal to stripe length for RAID-1).
 * For each extent, its physical mapping extents from [start, end], where
 * start=stripe_physical+(extent_objectid-stripe_logical) -- since objectid
 * corresponds to the logical offset of the extent, and end=start+extent_offset.
 */
static noinline_for_stack int scrub_stripe(struct scrub_ctx *sctx,
					   struct map_lookup *map,
					   struct btrfs_device *scrub_dev,
					   int num, u64 base, u64 length,
					   int is_dev_replace)
{
	struct btrfs_path *path;
	struct btrfs_fs_info *fs_info = sctx->dev_root->fs_info;
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_root *csum_root = fs_info->csum_root;
	struct btrfs_extent_item *extent;
	struct blk_plug plug;
	u64 flags;
	int ret;
	int slot;
	u64 nstripes;
	struct extent_buffer *l;
	struct btrfs_key key;
	u64 physical;
	u64 logical;
	u64 logic_end;
	u64 generation;
	int mirror_num;
	struct reada_control *reada1;
	struct reada_control *reada2;
	struct btrfs_key key_start;
	struct btrfs_key key_end;
	u64 increment = map->stripe_len;
	u64 offset;
	u64 extent_logical;
	u64 extent_physical;
	u64 extent_len;
	struct btrfs_device *extent_dev;
	int extent_mirror_num;
	int stop_loop = 0;
#ifdef CONFIG_BTRFS_FS_SCRUB_READA
	u64 de_physical;
	u64 lstart, lend, p_increment;
	struct reada_control **readarr = NULL;
	int i, reada_maxidx;
#endif /* CONFIG_BTRFS_FS_SCRUB_READA */
#ifdef CONFIG_BTRFS_DUET_SCRUB
	u64 tot_skipped = 0;
	u64 dstart = scrub_dev->bdev->bd_part->start_sect << 9;
#endif /* CONFIG_BTRFS_DUET_SCRUB */

	if (map->type & (BTRFS_BLOCK_GROUP_RAID5 |
			 BTRFS_BLOCK_GROUP_RAID6)) {
		if (num >= nr_data_stripes(map)) {
			return 0;
		}
	}

	nstripes = length;
	offset = 0;
	do_div(nstripes, map->stripe_len);
	if (map->type & BTRFS_BLOCK_GROUP_RAID0) {
		offset = map->stripe_len * num;
		increment = map->stripe_len * map->num_stripes;
		mirror_num = 1;
	} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
		int factor = map->num_stripes / map->sub_stripes;
		offset = map->stripe_len * (num / map->sub_stripes);
		increment = map->stripe_len * factor;
		mirror_num = num % map->sub_stripes + 1;
	} else if (map->type & BTRFS_BLOCK_GROUP_RAID1) {
		increment = map->stripe_len;
		mirror_num = num % map->num_stripes + 1;
	} else if (map->type & BTRFS_BLOCK_GROUP_DUP) {
		increment = map->stripe_len;
		mirror_num = num % map->num_stripes + 1;
	} else {
		increment = map->stripe_len;
		mirror_num = 1;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * work on commit root. The related disk blocks are static as
	 * long as COW is applied. This means, it is save to rewrite
	 * them to repair disk errors without any race conditions
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;

	/*
	 * trigger the readahead for extent tree csum tree and wait for
	 * completion. During readahead, the scrub is officially paused
	 * to not hold off transaction commits
	 */
	logical = base + offset;

	wait_event(sctx->list_wait,
		   atomic_read(&sctx->bios_in_flight) == 0);
	atomic_inc(&fs_info->scrubs_paused);
	wake_up(&fs_info->scrub_pause_wait);

#ifdef CONFIG_BTRFS_FS_SCRUB_READA
	if (!sctx->is_dev_replace || !duet_online()) {
vanilla_reada:
		printk(KERN_INFO "scrub: vanilla readahead started\n");
#endif /* CONFIG_BTRFS_FS_SCRUB_READA */
		/* FIXME it might be better to start readahead at commit root */
		key_start.objectid = logical;
		key_start.type = BTRFS_EXTENT_ITEM_KEY;
		key_start.offset = (u64)0;
		key_end.objectid = base + offset + nstripes * increment;
		key_end.type = BTRFS_METADATA_ITEM_KEY;
		key_end.offset = (u64)-1;
		reada1 = btrfs_reada_add(root, &key_start, &key_end);

		key_start.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
		key_start.type = BTRFS_EXTENT_CSUM_KEY;
		key_start.offset = logical;
		key_end.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
		key_end.type = BTRFS_EXTENT_CSUM_KEY;
		key_end.offset = base + offset + nstripes * increment;
		reada2 = btrfs_reada_add(csum_root, &key_start, &key_end);

		//printk(KERN_INFO "btrfs scrub: readahead started at %lu.\n", jiffies);
		if (!IS_ERR(reada1))
			btrfs_reada_wait(reada1);
		if (!IS_ERR(reada2))
			btrfs_reada_wait(reada2);
#ifdef CONFIG_BTRFS_FS_SCRUB_READA
		printk(KERN_INFO "scrub: vanilla readahead ended\n");
	} else if (!sctx->is_dev_replace) {
		printk(KERN_INFO "scrub: selective readahead started\n");
		/* Only dispatch readahead requests for those ranges that
		 * won't get filtered out */
		logical = base + offset;
		physical = map->stripes[num].physical;
		logic_end = logical + increment * nstripes;
		p_increment = map->stripe_len;
		de_physical = physical;

		/* Allocate readahead handle array */
		readarr = kmalloc_array(2*BTRFS_SCRUB_MAX_READA,
			sizeof(struct reada_control *), GFP_NOFS);
		if (!readarr) {
			printk(KERN_INFO "scrub: failed to create reada array,"
				" trying vanilla readahead");
			goto vanilla_reada;
		}
		reada_maxidx = 0;

		while (logical < logic_end) {
			ret = 0;
			while (logical < logic_end) {
				/* Check if this stripe should be skipped */
				if (!duet_check(sctx->taskid, dstart + physical,
				    p_increment)) {
					ret = 1;
					break;
				} else {
					logical += increment;
					physical += p_increment;
				}
			}

			if (!ret)
				break;
			lstart = logical;

			while (logical <= logic_end) {
				/* Check if this stripe should _not_ be skipped */
				if (!duet_check(sctx->taskid, dstart + physical,
				    p_increment)) {
					break;
				} else {
					logical += increment;
					physical += p_increment;
				}
			}
			lend = logical;

			/* Dispatch readahead and keep handles */
			/* FIXME it might be better to start readahead at commit root */
			key_start.objectid = lstart;
			key_start.type = BTRFS_EXTENT_ITEM_KEY;
			key_start.offset = (u64)0;
			key_end.objectid = lend;
			key_end.type = BTRFS_METADATA_ITEM_KEY;
			key_end.offset = (u64)-1;
			readarr[reada_maxidx] = btrfs_reada_add(root, &key_start, &key_end);
			reada_maxidx++;

			key_start.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
			key_start.type = BTRFS_EXTENT_CSUM_KEY;
			key_start.offset = lstart;
			key_end.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
			key_end.type = BTRFS_EXTENT_CSUM_KEY;
			key_end.offset = lend;
			readarr[reada_maxidx] = btrfs_reada_add(csum_root, &key_start, &key_end);
			reada_maxidx++;

			if (reada_maxidx == BTRFS_SCRUB_MAX_READA) {
				for (i=0; i<reada_maxidx; i++)
					if (!IS_ERR(readarr[i]))
						btrfs_reada_wait(readarr[i]);
				reada_maxidx = 0;
			}

			logical += increment;
			physical += p_increment;
		}

		for (i=0; i<reada_maxidx; i++)
			if (!IS_ERR(readarr[i]))
				btrfs_reada_wait(readarr[i]);
		printk(KERN_INFO "scrub: selective readahead ended\n");
	}
#endif /* CONFIG_BTRFS_FS_SCRUB_READA */

	mutex_lock(&fs_info->scrub_lock);
	while (atomic_read(&fs_info->scrub_pause_req)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
		   atomic_read(&fs_info->scrub_pause_req) == 0);
		mutex_lock(&fs_info->scrub_lock);
	}
	atomic_dec(&fs_info->scrubs_paused);
	mutex_unlock(&fs_info->scrub_lock);
	wake_up(&fs_info->scrub_pause_wait);

	/*
	 * collect all data csums for the stripe to avoid seeking during
	 * the scrub. This might currently (crc32) end up to be about 1MB
	 */
	blk_start_plug(&plug);

	/*
	 * now find all extents for each stripe and scrub them
	 */
	logical = base + offset;
	physical = map->stripes[num].physical;
	logic_end = logical + increment * nstripes;
	ret = 0;
	while (logical < logic_end) {
		/*
		 * canceled?
		 */
		if (atomic_read(&fs_info->scrub_cancel_req) ||
		    atomic_read(&sctx->cancel_req)) {
			ret = -ECANCELED;
			goto out;
		}
		/*
		 * check to see if we have to pause
		 */
		if (atomic_read(&fs_info->scrub_pause_req)) {
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
pause_properly:
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
			/* push queued extents */
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
			printk(KERN_DEBUG "btrfs scrub: Pause requested. "
				"Waiting for bios (%lu).\n", jiffies);
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
			atomic_set(&sctx->wr_ctx.flush_all_writes, 1);
			scrub_submit(sctx);
			mutex_lock(&sctx->wr_ctx.wr_lock);
			scrub_wr_submit(sctx);
			mutex_unlock(&sctx->wr_ctx.wr_lock);
			wait_event(sctx->list_wait,
				   atomic_read(&sctx->bios_in_flight) == 0);
			atomic_set(&sctx->wr_ctx.flush_all_writes, 0);
			atomic_inc(&fs_info->scrubs_paused);
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
			/* At this point, we're officially paused */
back_to_paused:
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
			wake_up(&fs_info->scrub_pause_wait);
			mutex_lock(&fs_info->scrub_lock);
			/* Time to wait for the transaction to be over */
			while (atomic_read(&fs_info->scrub_pause_req)) {
			    mutex_unlock(&fs_info->scrub_lock);
#ifdef CONFIG_BTRFS_DUET_SCRUB
			    /* If there's no more to process, wait */
			    if (!duet_online() || !sctx->taskid || !process_duet_events(sctx))
#endif
				wait_event(fs_info->scrub_pause_wait,
				   atomic_read(&fs_info->scrub_pause_req) == 0);
			    mutex_lock(&fs_info->scrub_lock);
			}
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
			if (sctx->deadline && sctx->first_free == -1) {
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
				printk(KERN_DEBUG "btrfs scrub: Waiting for "
					"free bios (%lu).\n", jiffies);
#endif
				mutex_unlock(&fs_info->scrub_lock);
				wait_event(sctx->list_wait,
					sctx->first_free != -1);
				mutex_lock(&fs_info->scrub_lock);
				if (atomic_read(&fs_info->scrub_pause_req))
					goto back_to_paused;
			}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
			/* We have woken up again */
			atomic_dec(&fs_info->scrubs_paused);
			mutex_unlock(&fs_info->scrub_lock);
			wake_up(&fs_info->scrub_pause_wait);
		}

#ifdef CONFIG_BTRFS_DUET_SCRUB
		/* If we hit the disk, we try to give the workload a chance. */
		if (duet_online() && sctx->taskid && process_duet_events(sctx))
			continue;
#endif /* CONFIG_BTRFS_DUET_SCRUB */

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
		btrfs_release_path(path);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
		key.objectid = logical;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = (u64)-1;

		ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		if (ret < 0)
			goto out;

		if (ret > 0) {
			ret = btrfs_previous_item(root, path, 0,
						  BTRFS_EXTENT_ITEM_KEY);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				/* there's no smaller item, so stick with the
				 * larger one */
				btrfs_release_path(path);
				ret = btrfs_search_slot(NULL, root, &key,
							path, 0, 0);
				if (ret < 0)
					goto out;
			}
		}

		stop_loop = 0;
		while (1) {
			u64 bytes;

			l = path->nodes[0];
			slot = path->slots[0];
			if (slot >= btrfs_header_nritems(l)) {
				ret = btrfs_next_leaf(root, path);
				if (ret == 0)
					continue;
				if (ret < 0)
					goto out;

				stop_loop = 1;
				break;
			}
			btrfs_item_key_to_cpu(l, &key, slot);

			if (key.type == BTRFS_METADATA_ITEM_KEY)
				bytes = root->leafsize;
			else
				bytes = key.offset;

			if (key.objectid + bytes <= logical)
				goto next;

			if (key.type != BTRFS_EXTENT_ITEM_KEY &&
			    key.type != BTRFS_METADATA_ITEM_KEY)
				goto next;

			if (key.objectid >= logical + map->stripe_len) {
				/* out of this device extent */
				if (key.objectid >= logic_end)
					stop_loop = 1;
				break;
			}

			extent = btrfs_item_ptr(l, slot,
						struct btrfs_extent_item);
			flags = btrfs_extent_flags(l, extent);
			generation = btrfs_extent_generation(l, extent);

			if (key.objectid < logical &&
			    (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
				printk(KERN_ERR
				       "btrfs scrub: tree block %llu spanning "
				       "stripes, ignored. logical=%llu\n",
				       key.objectid, logical);
				goto next;
			}

again:
#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
			if (atomic_read(&fs_info->scrub_pause_req)) {
				/* We were asked to pause in the middle of an
				 * extent. When we come back, we'll continue
				 * from the logical offset we left off. Then
				 * we'll look for an extent all over again. */
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
				printk(KERN_DEBUG "btrfs scrub: Interrupted "
					"while scrubbing extent. Backing up. "
					"(%lu).\n", jiffies);
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
				goto pause_properly;
			}
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
			extent_logical = key.objectid;
			extent_len = bytes;

			/*
			 * trim extent to this stripe
			 */
			if (extent_logical < logical) {
				extent_len -= logical - extent_logical;
				extent_logical = logical;
			}
			if (extent_logical + extent_len >
			    logical + map->stripe_len) {
				extent_len = logical + map->stripe_len -
					     extent_logical;
			}

			extent_physical = extent_logical - logical + physical;
			extent_dev = scrub_dev;
			extent_mirror_num = mirror_num;
			if (is_dev_replace)
				scrub_remap_extent(fs_info, extent_logical,
						   extent_len, &extent_physical,
						   &extent_dev,
						   &extent_mirror_num);

#ifdef CONFIG_BTRFS_DUET_SCRUB
			/* Check whether we can skip this extent portion to
			 * save time and IO needed to look into the checksum
			 * tree. Criteria: if the entire extent portion can be
			 * filtered out, skip. */
			scrub_dbg(KERN_INFO "duet-scrub: checking [%llu, %llu] --"
				" dstart = %llu\n",
				dstart +extent_physical,
				dstart +extent_physical + extent_len,
				dstart);
			if (!is_dev_replace && (duet_check(sctx->taskid,
			    dstart + extent_physical, extent_len) == 1)) {
				scrub_dbg(KERN_INFO "duet-scrub: found!\n");
				tot_skipped++;
				if (flags & BTRFS_EXTENT_FLAG_DATA) {
					spin_lock(&sctx->stat_lock);
					sctx->stat.data_bytes_scrubbed += extent_len;
					spin_unlock(&sctx->stat_lock);
				} else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
					spin_lock(&sctx->stat_lock);
					sctx->stat.tree_bytes_scrubbed += extent_len;
					spin_unlock(&sctx->stat_lock);
				}
				goto skip_extent;
			}
#endif /* CONFIG_BTRFS_DUET_SCRUB */

			ret = btrfs_lookup_csums_range(csum_root, logical,
						logical + map->stripe_len - 1,
						&sctx->csum_list, 1);
			if (ret)
				goto out;

			ret = scrub_extent(sctx, extent_logical, extent_len,
					   extent_physical, extent_dev, flags,
					   generation, extent_mirror_num,
					   extent_logical - logical + physical);
			if (ret)
				goto out;

			scrub_free_csums(sctx);
#ifdef CONFIG_BTRFS_DUET_SCRUB
skip_extent:
#endif /* CONFIG_BTRFS_DUET_SCRUB */
			if (extent_logical + extent_len <
			    key.objectid + bytes) {
				logical += increment;
				physical += map->stripe_len;

				if (logical < key.objectid + bytes) {
					cond_resched();
					goto again;
				}

				if (logical >= logic_end) {
					stop_loop = 1;
					break;
				}
			}
next:
			path->slots[0]++;
		}
		btrfs_release_path(path);
		logical += increment;
		physical += map->stripe_len;
		spin_lock(&sctx->stat_lock);
		if (stop_loop)
			sctx->stat.last_physical = map->stripes[num].physical +
						   length;
		else
			sctx->stat.last_physical = physical;
		spin_unlock(&sctx->stat_lock);
		if (stop_loop)
			break;
	}
out:
	/* push queued extents */
	scrub_submit(sctx);
	mutex_lock(&sctx->wr_ctx.wr_lock);
	scrub_wr_submit(sctx);
	mutex_unlock(&sctx->wr_ctx.wr_lock);

	blk_finish_plug(&plug);
	btrfs_free_path(path);
#ifdef CONFIG_BTRFS_DUET_SCRUB
	printk(KERN_DEBUG "btrfs scrub: Skipped a total of %llu extent chunks\n",
		tot_skipped);
#endif /* CONFIG_BTRFS_DUET_SCRUB */
	return ret < 0 ? ret : 0;
}

/*
 * Different devices are scrubbed in parallel, and each device is scrubbed
 * sequentially, chunk by chunk (technically that's device extents, the portion
 * of a chunk on a single device). Here we initiate scrubbing of the chunk.
 * Looks up the map of stripes for this extent, then passes it down to
 * scrub_stripe that takes care of initiating scrubbing at the chunk.
 */
static noinline_for_stack int scrub_chunk(struct scrub_ctx *sctx,
					  struct btrfs_device *scrub_dev,
					  u64 chunk_tree, u64 chunk_objectid,
					  u64 chunk_offset, u64 length,
					  u64 dev_offset, int is_dev_replace)
{
	struct btrfs_mapping_tree *map_tree =
		&sctx->dev_root->fs_info->mapping_tree;
	struct map_lookup *map;
	struct extent_map *em;
	int i;
	int ret = 0;

	read_lock(&map_tree->map_tree.lock);
	em = lookup_extent_mapping(&map_tree->map_tree, chunk_offset, 1);
	read_unlock(&map_tree->map_tree.lock);

	if (!em)
		return -EINVAL;

	map = (struct map_lookup *)em->bdev;
	if (em->start != chunk_offset)
		goto out;

	if (em->len < length)
		goto out;

	for (i = 0; i < map->num_stripes; ++i) {
		if (map->stripes[i].dev->bdev == scrub_dev->bdev &&
		    map->stripes[i].physical == dev_offset) {
			ret = scrub_stripe(sctx, map, scrub_dev, i,
					   chunk_offset, length,
					   is_dev_replace);
			if (ret)
				goto out;
		}
	}
out:
	free_extent_map(em);

	return ret;
}

static noinline_for_stack
int scrub_enumerate_chunks(struct scrub_ctx *sctx,
			   struct btrfs_device *scrub_dev, u64 start, u64 end,
			   int is_dev_replace)
{
	struct btrfs_dev_extent *dev_extent = NULL;
	struct btrfs_path *path;
	struct btrfs_root *root = sctx->dev_root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 length;
	u64 chunk_tree;
	u64 chunk_objectid;
	u64 chunk_offset;
	int ret;
	int slot;
	struct extent_buffer *l;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_block_group_cache *cache;
	struct btrfs_dev_replace *dev_replace = &fs_info->dev_replace;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = 2;
	path->search_commit_root = 1;
	path->skip_locking = 1;

	key.objectid = scrub_dev->devid;
	key.offset = 0ull;
	key.type = BTRFS_DEV_EXTENT_KEY;

	while (1) {
		ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		if (ret < 0)
			break;
		if (ret > 0) {
			if (path->slots[0] >=
			    btrfs_header_nritems(path->nodes[0])) {
				ret = btrfs_next_leaf(root, path);
				if (ret)
					break;
			}
		}

		l = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(l, &found_key, slot);

		if (found_key.objectid != scrub_dev->devid)
			break;

		if (btrfs_key_type(&found_key) != BTRFS_DEV_EXTENT_KEY)
			break;

		if (found_key.offset >= end)
			break;

		if (found_key.offset < key.offset)
			break;

		dev_extent = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		length = btrfs_dev_extent_length(l, dev_extent);

		if (found_key.offset + length <= start) {
			key.offset = found_key.offset + length;
			btrfs_release_path(path);
			continue;
		}

		chunk_tree = btrfs_dev_extent_chunk_tree(l, dev_extent);
		chunk_objectid = btrfs_dev_extent_chunk_objectid(l, dev_extent);
		chunk_offset = btrfs_dev_extent_chunk_offset(l, dev_extent);
		printk(KERN_INFO "btrfs scrub: Chunk <obj=%llu, off=%llu> to "
			"be scrubbed.\n", chunk_objectid, chunk_offset);

		/*
		 * get a reference on the corresponding block group to prevent
		 * the chunk from going away while we scrub it
		 */
		cache = btrfs_lookup_block_group(fs_info, chunk_offset);
		if (!cache) {
			ret = -ENOENT;
			break;
		}
		dev_replace->cursor_right = found_key.offset + length;
		dev_replace->cursor_left = found_key.offset;
		dev_replace->item_needs_writeback = 1;
		ret = scrub_chunk(sctx, scrub_dev, chunk_tree, chunk_objectid,
				  chunk_offset, length, found_key.offset,
				  is_dev_replace);

		/*
		 * flush, submit all pending read and write bios, afterwards
		 * wait for them.
		 * Note that in the dev replace case, a read request causes
		 * write requests that are submitted in the read completion
		 * worker. Therefore in the current situation, it is required
		 * that all write requests are flushed, so that all read and
		 * write requests are really completed when bios_in_flight
		 * changes to 0.
		 */
		atomic_set(&sctx->wr_ctx.flush_all_writes, 1);
		scrub_submit(sctx);
		mutex_lock(&sctx->wr_ctx.wr_lock);
		scrub_wr_submit(sctx);
		mutex_unlock(&sctx->wr_ctx.wr_lock);

		wait_event(sctx->list_wait,
			   atomic_read(&sctx->bios_in_flight) == 0);
		atomic_set(&sctx->wr_ctx.flush_all_writes, 0);
		atomic_inc(&fs_info->scrubs_paused);
		wake_up(&fs_info->scrub_pause_wait);
		wait_event(sctx->list_wait,
			   atomic_read(&sctx->workers_pending) == 0);

		mutex_lock(&fs_info->scrub_lock);
		while (atomic_read(&fs_info->scrub_pause_req)) {
			mutex_unlock(&fs_info->scrub_lock);
			wait_event(fs_info->scrub_pause_wait,
			   atomic_read(&fs_info->scrub_pause_req) == 0);
			mutex_lock(&fs_info->scrub_lock);
		}
		atomic_dec(&fs_info->scrubs_paused);
		mutex_unlock(&fs_info->scrub_lock);
		wake_up(&fs_info->scrub_pause_wait);

		btrfs_put_block_group(cache);
		if (ret)
			break;
		if (is_dev_replace &&
		    atomic64_read(&dev_replace->num_write_errors) > 0) {
			ret = -EIO;
			break;
		}
		if (sctx->stat.malloc_errors > 0) {
			ret = -ENOMEM;
			break;
		}

		dev_replace->cursor_left = dev_replace->cursor_right;
		dev_replace->item_needs_writeback = 1;

		key.offset = found_key.offset + length;
		btrfs_release_path(path);
		printk(KERN_INFO "btrfs scrub: Done scrubbing chunk [%llu] "
			"(%lu)\n", chunk_offset, jiffies);
	}

	btrfs_free_path(path);

	/*
	 * ret can still be 1 from search_slot or next_leaf,
	 * that's not an error
	 */
	return ret < 0 ? ret : 0;
}

static noinline_for_stack int scrub_supers(struct scrub_ctx *sctx,
					   struct btrfs_device *scrub_dev)
{
	int	i;
	u64	bytenr;
	u64	gen;
	int	ret;
	struct btrfs_root *root = sctx->dev_root;

	if (test_bit(BTRFS_FS_STATE_ERROR, &root->fs_info->fs_state))
		return -EIO;

	gen = root->fs_info->last_trans_committed;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + BTRFS_SUPER_INFO_SIZE > scrub_dev->total_bytes)
			break;

		ret = scrub_pages(sctx, bytenr, BTRFS_SUPER_INFO_SIZE, bytenr,
				  scrub_dev, BTRFS_EXTENT_FLAG_SUPER, gen, i,
				  NULL, 1, bytenr);
		if (ret)
			return ret;
	}
	wait_event(sctx->list_wait, atomic_read(&sctx->bios_in_flight) == 0);

	return 0;
}

/*
 * get a reference count on fs_info->scrub_workers. start worker if necessary
 */
static noinline_for_stack int scrub_workers_get(struct btrfs_fs_info *fs_info,
						int is_dev_replace)
{
	int ret = 0;

	if (fs_info->scrub_workers_refcnt == 0) {
		if (is_dev_replace)
			btrfs_init_workers(&fs_info->scrub_workers, "scrub", 1,
					&fs_info->generic_worker);
		else
			btrfs_init_workers(&fs_info->scrub_workers, "scrub",
					fs_info->thread_pool_size,
					&fs_info->generic_worker);
		fs_info->scrub_workers.idle_thresh = 4;
		ret = btrfs_start_workers(&fs_info->scrub_workers);
		if (ret)
			goto out;
		btrfs_init_workers(&fs_info->scrub_wr_completion_workers,
				   "scrubwrc",
				   fs_info->thread_pool_size,
				   &fs_info->generic_worker);
		fs_info->scrub_wr_completion_workers.idle_thresh = 2;
		ret = btrfs_start_workers(
				&fs_info->scrub_wr_completion_workers);
		if (ret)
			goto out;
		btrfs_init_workers(&fs_info->scrub_nocow_workers, "scrubnc", 1,
				   &fs_info->generic_worker);
		ret = btrfs_start_workers(&fs_info->scrub_nocow_workers);
		if (ret)
			goto out;
	}
	++fs_info->scrub_workers_refcnt;
out:
	return ret;
}

static noinline_for_stack void scrub_workers_put(struct btrfs_fs_info *fs_info)
{
	if (--fs_info->scrub_workers_refcnt == 0) {
		btrfs_stop_workers(&fs_info->scrub_workers);
		btrfs_stop_workers(&fs_info->scrub_wr_completion_workers);
		btrfs_stop_workers(&fs_info->scrub_nocow_workers);
	}
	WARN_ON(fs_info->scrub_workers_refcnt < 0);
}

#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
int btrfs_scrub_dev(struct btrfs_fs_info *fs_info, u64 devid, u64 start,
		    u64 end, struct btrfs_scrub_progress *progress,
		    int readonly, int is_dev_replace)
#else /* Adaptive scrubber code */
int btrfs_scrub_dev(struct btrfs_fs_info *fs_info, u64 devid, u64 start,
		    u64 end, struct btrfs_scrub_progress *progress,
		    int readonly, u64 deadline, u8 bgflags, int is_dev_replace)
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
{
	struct scrub_ctx *sctx;
	int ret;
	struct btrfs_device *dev;

	if (btrfs_fs_closing(fs_info))
		return -EINVAL;

	/*
	 * check some assumptions
	 */
	if (fs_info->chunk_root->nodesize != fs_info->chunk_root->leafsize) {
		printk(KERN_ERR
		       "btrfs_scrub: size assumption nodesize == leafsize (%d == %d) fails\n",
		       fs_info->chunk_root->nodesize,
		       fs_info->chunk_root->leafsize);
		return -EINVAL;
	}

	if (fs_info->chunk_root->nodesize > BTRFS_STRIPE_LEN) {
		/*
		 * in this case scrub is unable to calculate the checksum
		 * the way scrub is implemented. Do not handle this
		 * situation at all because it won't ever happen.
		 */
		printk(KERN_ERR
		       "btrfs_scrub: size assumption nodesize <= BTRFS_STRIPE_LEN (%d <= %d) fails\n",
		       fs_info->chunk_root->nodesize, BTRFS_STRIPE_LEN);
		return -EINVAL;
	}

	if (fs_info->chunk_root->sectorsize != PAGE_SIZE) {
		/* not supported for data w/o checksums */
		printk(KERN_ERR
		       "btrfs_scrub: size assumption sectorsize != PAGE_SIZE (%d != %lu) fails\n",
		       fs_info->chunk_root->sectorsize, PAGE_SIZE);
		return -EINVAL;
	}

	if (fs_info->chunk_root->nodesize >
	    PAGE_SIZE * SCRUB_MAX_PAGES_PER_BLOCK ||
	    fs_info->chunk_root->sectorsize >
	    PAGE_SIZE * SCRUB_MAX_PAGES_PER_BLOCK) {
		/*
		 * would exhaust the array bounds of pagev member in
		 * struct scrub_block
		 */
		pr_err("btrfs_scrub: size assumption nodesize and sectorsize <= SCRUB_MAX_PAGES_PER_BLOCK (%d <= %d && %d <= %d) fails\n",
		       fs_info->chunk_root->nodesize,
		       SCRUB_MAX_PAGES_PER_BLOCK,
		       fs_info->chunk_root->sectorsize,
		       SCRUB_MAX_PAGES_PER_BLOCK);
		return -EINVAL;
	}


	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	dev = btrfs_find_device(fs_info, devid, NULL, NULL);
	if (!dev || (dev->missing && !is_dev_replace)) {
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		return -ENODEV;
	}

	mutex_lock(&fs_info->scrub_lock);
	if (!dev->in_fs_metadata || dev->is_tgtdev_for_dev_replace) {
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		return -EIO;
	}

	btrfs_dev_replace_lock(&fs_info->dev_replace);
	if (dev->scrub_device ||
	    (!is_dev_replace &&
	     btrfs_dev_replace_is_ongoing(&fs_info->dev_replace))) {
		btrfs_dev_replace_unlock(&fs_info->dev_replace);
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		return -EINPROGRESS;
	}
	btrfs_dev_replace_unlock(&fs_info->dev_replace);

	ret = scrub_workers_get(fs_info, is_dev_replace);
	if (ret) {
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		return ret;
	}

#ifdef CONFIG_BTRFS_FS_SCRUB_NONE
	sctx = scrub_setup_ctx(dev, is_dev_replace);
#else /* Adaptive scrubber code */
	sctx = scrub_setup_ctx(dev, deadline, bgflags, is_dev_replace);
#endif /* CONFIG_BTRFS_FS_SCRUB_NONE */
	if (IS_ERR(sctx)) {
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		scrub_workers_put(fs_info);
		return PTR_ERR(sctx);
	}
	sctx->readonly = readonly;
	dev->scrub_device = sctx;

	atomic_inc(&fs_info->scrubs_running);
	mutex_unlock(&fs_info->scrub_lock);

	if (!is_dev_replace) {
		/*
		 * by holding device list mutex, we can
		 * kick off writing super in log tree sync.
		 */
		ret = scrub_supers(sctx, dev);
	}
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	if (!ret)
		ret = scrub_enumerate_chunks(sctx, dev, start, end,
					     is_dev_replace);
	//printk(KERN_INFO "btrfs scrub: Done enumerating chunks at %lu.\n",
	//	jiffies);

#ifdef CONFIG_BTRFS_FS_SCRUB_ADAPT
	wait_event(sctx->list_wait, atomic_read(&sctx->bios_allocated) == 0);
#endif /* CONFIG_BTRFS_FS_SCRUB_ADAPT */
	wait_event(sctx->list_wait, atomic_read(&sctx->bios_in_flight) == 0);
	atomic_dec(&fs_info->scrubs_running);
	wake_up(&fs_info->scrub_pause_wait);

	wait_event(sctx->list_wait, atomic_read(&sctx->workers_pending) == 0);

	if (progress)
		memcpy(progress, &sctx->stat, sizeof(*progress));

	mutex_lock(&fs_info->scrub_lock);
	dev->scrub_device = NULL;
	scrub_workers_put(fs_info);
	mutex_unlock(&fs_info->scrub_lock);

	scrub_free_ctx(sctx);

	return ret;
}

void btrfs_scrub_pause(struct btrfs_root *root)
{
	struct btrfs_fs_info *fs_info = root->fs_info;

	mutex_lock(&fs_info->scrub_lock);
	atomic_inc(&fs_info->scrub_pause_req);
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
	printk(KERN_DEBUG "bgtask scrubber: Pause requested (P=%d,R=%d).\n",
		atomic_read(&fs_info->scrubs_paused),
		atomic_read(&fs_info->scrubs_running));
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
	while (atomic_read(&fs_info->scrubs_paused) !=
	       atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   atomic_read(&fs_info->scrubs_paused) ==
			   atomic_read(&fs_info->scrubs_running));
		mutex_lock(&fs_info->scrub_lock);
	}
#ifdef CONFIG_BTRFS_FS_SCRUB_DEBUG
	printk(KERN_DEBUG "bgtask scrubber: Scrubber paused.\n");
#endif /* CONFIG_BTRFS_FS_SCRUB_DEBUG */
	mutex_unlock(&fs_info->scrub_lock);
}

void btrfs_scrub_continue(struct btrfs_root *root)
{
	struct btrfs_fs_info *fs_info = root->fs_info;

	atomic_dec(&fs_info->scrub_pause_req);
	wake_up(&fs_info->scrub_pause_wait);
}

int btrfs_scrub_cancel(struct btrfs_fs_info *fs_info)
{
	mutex_lock(&fs_info->scrub_lock);
	if (!atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		return -ENOTCONN;
	}

	atomic_inc(&fs_info->scrub_cancel_req);
	while (atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   atomic_read(&fs_info->scrubs_running) == 0);
		mutex_lock(&fs_info->scrub_lock);
	}
	atomic_dec(&fs_info->scrub_cancel_req);
	mutex_unlock(&fs_info->scrub_lock);

	return 0;
}

int btrfs_scrub_cancel_dev(struct btrfs_fs_info *fs_info,
			   struct btrfs_device *dev)
{
	struct scrub_ctx *sctx;

	mutex_lock(&fs_info->scrub_lock);
	sctx = dev->scrub_device;
	if (!sctx) {
		mutex_unlock(&fs_info->scrub_lock);
		return -ENOTCONN;
	}
	atomic_inc(&sctx->cancel_req);
	while (dev->scrub_device) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   dev->scrub_device == NULL);
		mutex_lock(&fs_info->scrub_lock);
	}
	mutex_unlock(&fs_info->scrub_lock);

	return 0;
}

int btrfs_scrub_progress(struct btrfs_root *root, u64 devid,
			 struct btrfs_scrub_progress *progress)
{
	struct btrfs_device *dev;
	struct scrub_ctx *sctx = NULL;

	mutex_lock(&root->fs_info->fs_devices->device_list_mutex);
	dev = btrfs_find_device(root->fs_info, devid, NULL, NULL);
	if (dev)
		sctx = dev->scrub_device;
	if (sctx)
		memcpy(progress, &sctx->stat, sizeof(*progress));
	mutex_unlock(&root->fs_info->fs_devices->device_list_mutex);

	return dev ? (sctx ? 0 : -ENOTCONN) : -ENODEV;
}

static void scrub_remap_extent(struct btrfs_fs_info *fs_info,
			       u64 extent_logical, u64 extent_len,
			       u64 *extent_physical,
			       struct btrfs_device **extent_dev,
			       int *extent_mirror_num)
{
	u64 mapped_length;
	struct btrfs_bio *bbio = NULL;
	int ret;

	mapped_length = extent_len;
	ret = btrfs_map_block(fs_info, READ, extent_logical,
			      &mapped_length, &bbio, 0);
	if (ret || !bbio || mapped_length < extent_len ||
	    !bbio->stripes[0].dev->bdev) {
		kfree(bbio);
		return;
	}

	*extent_physical = bbio->stripes[0].physical;
	*extent_mirror_num = bbio->mirror_num;
	*extent_dev = bbio->stripes[0].dev;
	kfree(bbio);
}

static int scrub_setup_wr_ctx(struct scrub_ctx *sctx,
			      struct scrub_wr_ctx *wr_ctx,
			      struct btrfs_fs_info *fs_info,
			      struct btrfs_device *dev,
			      int is_dev_replace)
{
	WARN_ON(wr_ctx->wr_curr_bio != NULL);

	mutex_init(&wr_ctx->wr_lock);
	wr_ctx->wr_curr_bio = NULL;
	if (!is_dev_replace)
		return 0;

	WARN_ON(!dev->bdev);
	wr_ctx->pages_per_wr_bio = min_t(int, SCRUB_PAGES_PER_WR_BIO,
					 bio_get_nr_vecs(dev->bdev));
	wr_ctx->tgtdev = dev;
	atomic_set(&wr_ctx->flush_all_writes, 0);
	return 0;
}

static void scrub_free_wr_ctx(struct scrub_wr_ctx *wr_ctx)
{
	mutex_lock(&wr_ctx->wr_lock);
	kfree(wr_ctx->wr_curr_bio);
	wr_ctx->wr_curr_bio = NULL;
	mutex_unlock(&wr_ctx->wr_lock);
}

static int copy_nocow_pages(struct scrub_ctx *sctx, u64 logical, u64 len,
			    int mirror_num, u64 physical_for_dev_replace)
{
	struct scrub_copy_nocow_ctx *nocow_ctx;
	struct btrfs_fs_info *fs_info = sctx->dev_root->fs_info;

	nocow_ctx = kzalloc(sizeof(*nocow_ctx), GFP_NOFS);
	if (!nocow_ctx) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		spin_unlock(&sctx->stat_lock);
		return -ENOMEM;
	}

	scrub_pending_trans_workers_inc(sctx);

	nocow_ctx->sctx = sctx;
	nocow_ctx->logical = logical;
	nocow_ctx->len = len;
	nocow_ctx->mirror_num = mirror_num;
	nocow_ctx->physical_for_dev_replace = physical_for_dev_replace;
	nocow_ctx->work.func = copy_nocow_pages_worker;
	INIT_LIST_HEAD(&nocow_ctx->inodes);
	btrfs_queue_worker(&fs_info->scrub_nocow_workers,
			   &nocow_ctx->work);

	return 0;
}

static int record_inode_for_nocow(u64 inum, u64 offset, u64 root, void *ctx)
{
	struct scrub_copy_nocow_ctx *nocow_ctx = ctx;
	struct scrub_nocow_inode *nocow_inode;

	nocow_inode = kzalloc(sizeof(*nocow_inode), GFP_NOFS);
	if (!nocow_inode)
		return -ENOMEM;
	nocow_inode->inum = inum;
	nocow_inode->offset = offset;
	nocow_inode->root = root;
	list_add_tail(&nocow_inode->list, &nocow_ctx->inodes);
	return 0;
}

#define COPY_COMPLETE 1

static void copy_nocow_pages_worker(struct btrfs_work *work)
{
	struct scrub_copy_nocow_ctx *nocow_ctx =
		container_of(work, struct scrub_copy_nocow_ctx, work);
	struct scrub_ctx *sctx = nocow_ctx->sctx;
	u64 logical = nocow_ctx->logical;
	u64 len = nocow_ctx->len;
	int mirror_num = nocow_ctx->mirror_num;
	u64 physical_for_dev_replace = nocow_ctx->physical_for_dev_replace;
	int ret;
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_fs_info *fs_info;
	struct btrfs_path *path;
	struct btrfs_root *root;
	int not_written = 0;

	fs_info = sctx->dev_root->fs_info;
	root = fs_info->extent_root;

	path = btrfs_alloc_path();
	if (!path) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		spin_unlock(&sctx->stat_lock);
		not_written = 1;
		goto out;
	}

	trans = btrfs_join_transaction(root);
	if (IS_ERR(trans)) {
		not_written = 1;
		goto out;
	}

	ret = iterate_inodes_from_logical(logical, fs_info, path,
					  record_inode_for_nocow, nocow_ctx);
	if (ret != 0 && ret != -ENOENT) {
		pr_warn("iterate_inodes_from_logical() failed: log %llu, phys %llu, len %llu, mir %u, ret %d\n",
			logical, physical_for_dev_replace, len, mirror_num,
			ret);
		not_written = 1;
		goto out;
	}

	btrfs_end_transaction(trans, root);
	trans = NULL;
	while (!list_empty(&nocow_ctx->inodes)) {
		struct scrub_nocow_inode *entry;
		entry = list_first_entry(&nocow_ctx->inodes,
					 struct scrub_nocow_inode,
					 list);
		list_del_init(&entry->list);
		ret = copy_nocow_pages_for_inode(entry->inum, entry->offset,
						 entry->root, nocow_ctx);
		kfree(entry);
		if (ret == COPY_COMPLETE) {
			ret = 0;
			break;
		} else if (ret) {
			break;
		}
	}
out:
	while (!list_empty(&nocow_ctx->inodes)) {
		struct scrub_nocow_inode *entry;
		entry = list_first_entry(&nocow_ctx->inodes,
					 struct scrub_nocow_inode,
					 list);
		list_del_init(&entry->list);
		kfree(entry);
	}
	if (trans && !IS_ERR(trans))
		btrfs_end_transaction(trans, root);
	if (not_written)
		btrfs_dev_replace_stats_inc(&fs_info->dev_replace.
					    num_uncorrectable_read_errors);

	btrfs_free_path(path);
	kfree(nocow_ctx);

	scrub_pending_trans_workers_dec(sctx);
}

static int copy_nocow_pages_for_inode(u64 inum, u64 offset, u64 root,
				      struct scrub_copy_nocow_ctx *nocow_ctx)
{
	struct btrfs_fs_info *fs_info = nocow_ctx->sctx->dev_root->fs_info;
	struct btrfs_key key;
	struct inode *inode;
	struct page *page;
	struct btrfs_root *local_root;
	struct btrfs_ordered_extent *ordered;
	struct extent_map *em;
	struct extent_state *cached_state = NULL;
	struct extent_io_tree *io_tree;
	u64 physical_for_dev_replace;
	u64 len = nocow_ctx->len;
	u64 lockstart = offset, lockend = offset + len - 1;
	unsigned long index;
	int srcu_index;
	int ret = 0;
	int err = 0;

	key.objectid = root;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	srcu_index = srcu_read_lock(&fs_info->subvol_srcu);

	local_root = btrfs_read_fs_root_no_name(fs_info, &key);
	if (IS_ERR(local_root)) {
		srcu_read_unlock(&fs_info->subvol_srcu, srcu_index);
		return PTR_ERR(local_root);
	}

	key.type = BTRFS_INODE_ITEM_KEY;
	key.objectid = inum;
	key.offset = 0;
	inode = btrfs_iget(fs_info->sb, &key, local_root, NULL);
	srcu_read_unlock(&fs_info->subvol_srcu, srcu_index);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	/* Avoid truncate/dio/punch hole.. */
	mutex_lock(&inode->i_mutex);
	inode_dio_wait(inode);

	physical_for_dev_replace = nocow_ctx->physical_for_dev_replace;
	io_tree = &BTRFS_I(inode)->io_tree;

	lock_extent_bits(io_tree, lockstart, lockend, 0, &cached_state);
	ordered = btrfs_lookup_ordered_range(inode, lockstart, len);
	if (ordered) {
		btrfs_put_ordered_extent(ordered);
		goto out_unlock;
	}

	em = btrfs_get_extent(inode, NULL, 0, lockstart, len, 0);
	if (IS_ERR(em)) {
		ret = PTR_ERR(em);
		goto out_unlock;
	}

	/*
	 * This extent does not actually cover the logical extent anymore,
	 * move on to the next inode.
	 */
	if (em->block_start > nocow_ctx->logical ||
	    em->block_start + em->block_len < nocow_ctx->logical + len) {
		free_extent_map(em);
		goto out_unlock;
	}
	free_extent_map(em);

	while (len >= PAGE_CACHE_SIZE) {
		index = offset >> PAGE_CACHE_SHIFT;
again:
		page = find_or_create_page(inode->i_mapping, index, GFP_NOFS);
		if (!page) {
			pr_err("find_or_create_page() failed\n");
			ret = -ENOMEM;
			goto out;
		}

		if (PageUptodate(page)) {
			if (PageDirty(page))
				goto next_page;
		} else {
			ClearPageError(page);
			err = extent_read_full_page_nolock(io_tree, page,
							   btrfs_get_extent,
							   nocow_ctx->mirror_num);
			if (err) {
				ret = err;
				goto next_page;
			}

			lock_page(page);
			/*
			 * If the page has been remove from the page cache,
			 * the data on it is meaningless, because it may be
			 * old one, the new data may be written into the new
			 * page in the page cache.
			 */
			if (page->mapping != inode->i_mapping) {
				unlock_page(page);
				page_cache_release(page);
				goto again;
			}
			if (!PageUptodate(page)) {
				ret = -EIO;
				goto next_page;
			}
		}
		err = write_page_nocow(nocow_ctx->sctx,
				       physical_for_dev_replace, page);
		if (err)
			ret = err;
next_page:
		unlock_page(page);
		page_cache_release(page);

		if (ret)
			break;

		offset += PAGE_CACHE_SIZE;
		physical_for_dev_replace += PAGE_CACHE_SIZE;
		len -= PAGE_CACHE_SIZE;
	}
	ret = COPY_COMPLETE;
out_unlock:
	unlock_extent_cached(io_tree, lockstart, lockend, &cached_state,
			     GFP_NOFS);
out:
	mutex_unlock(&inode->i_mutex);
	iput(inode);
	return ret;
}

static int write_page_nocow(struct scrub_ctx *sctx,
			    u64 physical_for_dev_replace, struct page *page)
{
	struct bio *bio;
	struct btrfs_device *dev;
	int ret;

	dev = sctx->wr_ctx.tgtdev;
	if (!dev)
		return -EIO;
	if (!dev->bdev) {
		printk_ratelimited(KERN_WARNING
			"btrfs: scrub write_page_nocow(bdev == NULL) is unexpected!\n");
		return -EIO;
	}
	bio = btrfs_io_bio_alloc(GFP_NOFS, 1);
	if (!bio) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		spin_unlock(&sctx->stat_lock);
		return -ENOMEM;
	}
	bio->bi_size = 0;
	bio->bi_sector = physical_for_dev_replace >> 9;
	bio->bi_bdev = dev->bdev;
	ret = bio_add_page(bio, page, PAGE_CACHE_SIZE, 0);
	if (ret != PAGE_CACHE_SIZE) {
leave_with_eio:
		bio_put(bio);
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_WRITE_ERRS);
		return -EIO;
	}

	if (btrfsic_submit_bio_wait(WRITE_SYNC, bio))
		goto leave_with_eio;

	bio_put(bio);
	return 0;
}
