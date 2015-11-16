/*
 * Copyright (C) 2014-2015 George Amvrosiadis.  All rights reserved.
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

#include "common.h"

#define BMAP_READ	0x01	/* Read bmaps (overrides other flags) */
#define BMAP_CHECK	0x02	/* Check given bmap value expression */
				/* Sets bmaps to match expression if not set */

/* Bmap expressions can be formed using the following flags: */
#define BMAP_DONE_SET	0x04	/* Set done bmap values */
#define BMAP_DONE_RST	0x08	/* Reset done bmap values */
#define BMAP_RELV_SET	0x10	/* Set relevant bmap values */
#define BMAP_RELV_RST	0x20	/* Reset relevant bmap values */

/*
 * The following two functions are wrappers for the basic bitmap functions.
 * The wrappers translate an arbitrary range of numbers to the range and
 * granularity represented in the bitmap.
 * A bitmap is characterized by a starting offset (bstart), and a granularity
 * per bit (bgran).
 */

/* Sets (or clears) bits in [start, start+len) */
static int duet_bmap_set(unsigned long *bmap, __u64 bstart, __u32 bgran,
	__u64 start, __u32 len, __u8 do_set)
{
	__u64 bofft = start - bstart;
	__u32 blen = len;

	if (bofft + blen >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Convert range to bitmap granularity */
	do_div(bofft, bgran);
	if (do_div(blen, bgran))
		blen++;

	if (do_set)
		bitmap_set(bmap, (unsigned int)bofft, (int)blen);
	else
		bitmap_clear(bmap, (unsigned int)bofft, (int)blen);

	return 0;
}

/* Returns value of bit at idx */
static int duet_bmap_read(unsigned long *bmap, __u64 bstart, __u32 bgran,
	__u64 idx)
{
	__u64 bofft64 = idx - bstart;
	unsigned long *p, mask;
	unsigned int bofft;

	if (bofft64 + 1 >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Convert offset to bitmap granularity */
	do_div(bofft64, bgran);
	bofft = (unsigned int)bofft64;

	/* Check the bits */
	p = bmap + BIT_WORD(bofft);
	mask = BITMAP_FIRST_WORD_MASK(bofft);

	if (((*p) & mask) == mask)
		return 1;

	return 0;
}

/* Checks whether *all* bits in [start, start+len) are set (or cleared) */
static int duet_bmap_chk(unsigned long *bmap, __u64 bstart, __u32 bgran,
	__u64 start, __u32 len, __u8 do_set)
{
	__u64 bofft64 = start - bstart;
	__u32 blen32 = len;
	int bits_to_chk, blen;
	unsigned long *p;
	unsigned long mask_to_chk;
	unsigned int size;
	unsigned int bofft;

	if (bofft64 + blen32 >= (bstart + (DUET_BITS_PER_NODE * bgran)))
		return -1;

	/* Convert range to bitmap granularity */
	do_div(bofft64, bgran);
	if (do_div(blen32, bgran))
		blen32++;

	/* Now it is safe to cast these variables */
	bofft = (unsigned int)bofft64;
	blen = (int)blen32;

	/* Check the bits */
	p = bmap + BIT_WORD(bofft);
	size = bofft + blen;
	bits_to_chk = BITS_PER_LONG - (bofft % BITS_PER_LONG);
	mask_to_chk = BITMAP_FIRST_WORD_MASK(bofft);

	while (blen - bits_to_chk >= 0) {
		if (do_set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!do_set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;

		blen -= bits_to_chk;
		bits_to_chk = BITS_PER_LONG;
		mask_to_chk = ~0UL;
		p++;
	}

	if (blen) {
		mask_to_chk &= BITMAP_LAST_WORD_MASK(size);
		if (do_set && !(((*p) & mask_to_chk) == mask_to_chk))
			return 0;
		else if (!do_set && !((~(*p) & mask_to_chk) == mask_to_chk))
			return 0;
	}

	return 1;
}

/* Initializes a bitmap tree node */
static struct bmap_rbnode *bnode_init(struct duet_bittree *bt, __u64 idx)
{
	struct bmap_rbnode *bnode = NULL;

#ifdef CONFIG_DUET_STATS
	if (bt) {
		(bt->statcur)++;
		if (bt->statcur > bt->statmax) {
			bt->statmax = bt->statcur;
			printk(KERN_INFO "duet: %llu nodes (%llu bytes) in BitTree.\n",
				bt->statmax, bt->statmax * DUET_BITS_PER_NODE / 8);
		}
	}
#endif /* CONFIG_DUET_STATS */
	
	bnode = kmalloc(sizeof(*bnode), GFP_NOWAIT);
	if (!bnode)
		return NULL;

	bnode->done = kzalloc(sizeof(unsigned long) *
			BITS_TO_LONGS(DUET_BITS_PER_NODE), GFP_NOWAIT);
	if (!bnode->done) {
		kfree(bnode);
		return NULL;
	}

	/* Allocate relevant bitmap, if needed */
	if (bt->is_file) {
		bnode->relv = kzalloc(sizeof(unsigned long) *
			BITS_TO_LONGS(DUET_BITS_PER_NODE), GFP_NOWAIT);
		if (!bnode->relv) {
			kfree(bnode->done);
			kfree(bnode);
			return NULL;
		}
	}

	RB_CLEAR_NODE(&bnode->node);
	bnode->idx = idx;
	return bnode;
}

static void bnode_dispose(struct bmap_rbnode *bnode, struct rb_node *rbnode,
	struct duet_bittree *bt)
{
#ifdef CONFIG_DUET_STATS
	if (bt)
		(bt->statcur)--;
#endif /* CONFIG_DUET_STATS */
	rb_erase(rbnode, &bt->root);
	if (bt->is_file)
		kfree(bnode->relv);
	kfree(bnode->done);
	kfree(bnode);
}

/*
 * Traverses bitmap nodes to read/set/unset/check bits on one or both bitmaps.
 * May insert/remove bitmap nodes as needed.
 *
 * If DUET_BMAP_READ is set:
 * - the bitmap values for idx are read for one or both bitmaps
 * - return value -1 denotes an error
 * Otherwise, if DUET_BMAP_CHECK flag is set:
 * - return value 1 means the range does follow the expression of given flags
 * - return value 0 means the range doesn't follow the expression of given flags
 * - return value -1 denotes an error
 * Otherwise, if neither flag is set:
 * - return value 0 means the range was updated according to given flags
 * - return value -1 denotes an error
 */
static int __update_tree(struct duet_bittree *bt, __u64 idx, __u32 len,
	__u8 flags)
{
	int found, ret, res;
	__u64 node_offt, div_rem;
	__u32 node_len;
	struct rb_node **link, *parent;
	struct bmap_rbnode *bnode = NULL;
	unsigned long iflags;

	local_irq_save(iflags);
	spin_lock(&bt->lock);
	duet_dbg(KERN_INFO "duet: %s idx %llu, len %u %s%s%s%s\n", idx, len,
		(flags & BMAP_READ) ? "reading" :
			((flags & BMAP_CHECK) ? "checking" : "marking"),
		(flags & BMAP_DONE_SET) ? "[set done] " : "",
		(flags & BMAP_DONE_RST) ? "[rst done] " : "",
		(flags & BMAP_RELV_SET) ? "[set relv] " : "",
		(flags & BMAP_RELV_RST) ? "[rst relv] " : "");

	div64_u64_rem(idx, bt->range * DUET_BITS_PER_NODE, &div_rem);
	node_offt = idx - div_rem;

	while (len) {
		/* Look up BitTree node */
		found = 0;
		link = &(bt->root).rb_node;
		parent = NULL;

		while (*link) {
			parent = *link;
			bnode = rb_entry(parent, struct bmap_rbnode, node);

			if (bnode->idx > node_offt) {
				link = &(*link)->rb_left;
			} else if (bnode->idx < node_offt) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}

		duet_dbg(KERN_DEBUG "duet: node starting at %llu %sfound\n",
			node_offt, found ? "" : "not ");

		/* If we're just reading bitmap values, return them now */
		if (flags & BMAP_READ) {
			ret = 0;

			if (!found)
				goto done;

			if (bt->is_file) {
				res = duet_bmap_read(bnode->relv, bnode->idx,
					bt->range, idx);
				if (res == -1) {
					ret = -1;
					goto done;
				}

				ret |= res << 1;
			}

			res = duet_bmap_read(bnode->done, bnode->idx, bt->range,
						idx);
			if (res == -1) {
				ret = -1;
				goto done;
			}

			ret |= res;
			goto done;
		}

		/*
		 * Take appropriate action based on whether we found the node
		 * and whether we plan to update (SET/RST), or only CHECK it.
		 *
		 *   NULL  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    SET  |     Set Bits     |  Init new node  |
		 *         |------------------+-----------------|
		 *    RST  | Clear (dispose?) |     Nothing     |
		 *  -------+------------------------------------+
		 *
		 *  CHECK  |       Found            !Found      |
		 *  -------+------------------------------------+
		 *    SET  |    Check Bits    |  Return false   |
		 *         |------------------+-----------------|
		 *    RST  |    Check Bits    |    Continue     |
		 *  -------+------------------------------------+
		 */

		/* Trim len to this node */
		node_len = min(idx + len, node_offt + (bt->range *
						DUET_BITS_PER_NODE)) - idx;

		/* First handle setting (or checking set) bits */
		if (flags & (BMAP_DONE_SET | BMAP_RELV_SET)) {
			if (!found && !(flags & BMAP_CHECK)) {
				/* Insert the new node */
				bnode = bnode_init(bt, node_offt);
				if (!bnode) {
					ret = -1;
					goto done;
				}

				rb_link_node(&bnode->node, parent, link);
				rb_insert_color(&bnode->node, &bt->root);

			} else if (!found && (flags & BMAP_CHECK)) {
				/* Looking for set bits, node didn't exist */
				ret = 0;
				goto done;
			}

			/* Set the bits */
			if (!(flags & BMAP_CHECK)) {
				if (bt->is_file && (flags & BMAP_RELV_SET) &&
				    duet_bmap_set(bnode->relv, bnode->idx,
						bt->range, idx, node_len, 1)) {
					/* Something went wrong */
					ret = -1;
					goto done;
				}

				if ((flags & BMAP_DONE_SET) &&
				    duet_bmap_set(bnode->done, bnode->idx,
					    bt->range, idx, node_len, 1)) {
					/* Something went wrong */
					ret = -1;
					goto done;
				}

			/* Check the bits */
			} else {
				if (bt->is_file && (flags & BMAP_RELV_SET)) {
					ret = duet_bmap_chk(bnode->relv,
						bnode->idx, bt->range, idx,
						node_len, 1);
					/* Check if we failed, or found a bit
					 * set/unset when it shouldn't be */
					if (ret != 1)
						goto done;
				}

				ret = duet_bmap_chk(bnode->done, bnode->idx,
					bt->range, idx, node_len, 1);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

		}

		/* Now handle unsetting any bits */
		if (found && (flags & (BMAP_DONE_RST | BMAP_RELV_RST))) {
			/* Clear the bits */
			if (!(flags & BMAP_CHECK)) {
				if (bt->is_file && (flags & BMAP_RELV_RST) &&
				    duet_bmap_set(bnode->relv, bnode->idx,
					    bt->range, idx, node_len, 0)) {
					/* Something went wrong */
					ret = -1;
					goto done;
				}

				if ((flags & BMAP_DONE_RST) &&
				    duet_bmap_set(bnode->done, bnode->idx,
					    bt->range, idx, node_len, 0)) {
					/* Something went wrong */
					ret = -1;
					goto done;
				}

			/* Check the bits */
			} else {
				if (bt->is_file && (flags & BMAP_RELV_RST)) {
					ret = duet_bmap_chk(bnode->relv,
						bnode->idx, bt->range, idx,
						node_len, 0);
					if (ret != 1)
						goto done;
				}

				ret = duet_bmap_chk(bnode->done, bnode->idx,
					bt->range, idx, node_len, 0);
				/* Check if we failed, or found a bit set/unset
				 * when it shouldn't be */
				if (ret != 1)
					goto done;
			}

			/* Dispose of the node if empty */
			if (!(flags & BMAP_CHECK) &&
			    bitmap_empty(bnode->done, DUET_BITS_PER_NODE) &&
			    (!bt->is_file ||
			     bitmap_empty(bnode->relv, DUET_BITS_PER_NODE)))
				bnode_dispose(bnode, parent, bt);
		}

		len -= node_len;
		idx += node_len;
		node_offt = idx;
	}

	/* If we managed to get here, then everything worked as planned.
	 * Return 0 for success in the case that CHECK is not set, or 1 for
	 * success when CHECK is set. */
	if (!(flags & BMAP_CHECK))
		ret = 0;
	else
		ret = 1;

done:
	if (ret == -1)
		printk(KERN_ERR "duet: blocks were not %s\n",
			(flags & BMAP_READ) ? "read" :
			((flags & BMAP_CHECK) ? "checked" : "modified"));
	spin_unlock(&bt->lock);
	local_irq_restore(iflags);
	return ret;
}

/*
 * For block tasks, proceed with done bitmap.
 * For file tasks, check if we have seen this inode before. If not, check if it
 * is relevant. Then, check whether it's done.
 */
static int do_bittree_check(struct duet_bittree *bt, __u64 idx, __u32 len,
	struct duet_task *task, struct inode *inode)
{
	int ret, flags, relv;

	if (bt->is_file) { /* File task */
		if (len != 1) {
			printk(KERN_ERR "duet: can't check more than one inode at a time\n");
			return -1;
		}

		flags = __update_tree(bt, idx, len, BMAP_READ);

		if (!flags) {
			/* We have not seen this inode before */
			if (inode) {
				relv = do_find_path(task, inode, 0, NULL);
			} else if (task) {
				relv = duet_find_path(task, idx, 0, NULL);
			} else {
				printk(KERN_ERR "duet: check failed -- no task/inode given\n");
				return -1;
			}

			if (!relv) { /* Mark as relevant */
				ret = __update_tree(bt, idx, len, BMAP_RELV_SET);
				if (ret != -1)
					ret = 0;
			} else if (relv == 1) { /* Mark as irrelevant */
				ret = __update_tree(bt, idx, len, BMAP_DONE_SET);
				if (ret != -1)
					ret = 1;
			} else {
				printk(KERN_ERR "duet: couldn't determine inode relevance\n");
				return -1;
			}
		} else {
			/* We know this inode, return done bit */
			ret = flags & 0x1;
		}
	} else { /* Block task */
		ret = __update_tree(bt, idx, len, BMAP_DONE_SET | BMAP_CHECK);
	}

	return ret;
}

/* Use this check function if you have a pointer to the inode you refer to */
int bittree_check_inode(struct duet_bittree *bt, struct duet_task *task,
	struct inode *inode)
{
	return do_bittree_check(bt, inode->i_ino, 1, task, inode);
}

int bittree_check(struct duet_bittree *bt, __u64 idx, __u32 len,
	struct duet_task *task)
{
	return do_bittree_check(bt, idx, len, task, NULL);
}

inline int bittree_set_done(struct duet_bittree *bt, __u64 idx, __u32 len)
{
	return __update_tree(bt, idx, len, BMAP_DONE_SET);
}

inline int bittree_unset_done(struct duet_bittree *bt, __u64 idx, __u32 len)
{
	return __update_tree(bt, idx, len, BMAP_DONE_RST);
}

inline int bittree_clear_bits(struct duet_bittree *bt, __u64 idx, __u32 len)
{
	return __update_tree(bt, idx, len, BMAP_DONE_RST | BMAP_RELV_RST);
}

/*inline int bittree_set_relevance(struct duet_bittree *bt, __u64 idx, __u32 len,
	int is_relevant)
{
	__u8 flags;

	if (is_relevant)
		flags = BMAP_RELV_SET | BMAP_DONE_RST;
	else
		flags = BMAP_RELV_RST | BMAP_DONE_SET;

	return __update_tree(bt, idx, len, flags);
}*/

int bittree_print(struct duet_task *task)
{
	struct bmap_rbnode *bnode = NULL;
	struct rb_node *node;
	unsigned long flags;

	spin_lock(&task->bittree.lock);
	printk(KERN_INFO "duet: Printing global hash table\n");
	node = rb_first(&task->bittree.root);
	while (node) {
		bnode = rb_entry(node, struct bmap_rbnode, node);

		/* Print node information */
		printk(KERN_INFO "duet: Node key = %llu\n", bnode->idx);
		printk(KERN_INFO "duet:   Done bits set: %d out of %d\n",
			bitmap_weight(bnode->done, DUET_BITS_PER_NODE),
			DUET_BITS_PER_NODE);
		printk(KERN_INFO "duet:   Relv bits set: %d out of %d\n",
			bitmap_weight(bnode->relv, DUET_BITS_PER_NODE),
			DUET_BITS_PER_NODE);

		node = rb_next(node);
	}
	spin_unlock(&task->bittree.lock);

	local_irq_save(flags);
	spin_lock(&task->bbmap_lock);
	printk(KERN_INFO "duet: Task #%d bitmap has %d out of %lu bits set\n",
		task->id, bitmap_weight(task->bucket_bmap,
		duet_env.itm_hash_size), duet_env.itm_hash_size);
	spin_unlock(&task->bbmap_lock);
	local_irq_restore(flags);

	return 0;
}

void bittree_init(struct duet_bittree *bittree, __u32 range, __u8 is_file)
{
	bittree->range = range;
	bittree->is_file = is_file;
	spin_lock_init(&bittree->lock);
	bittree->root = RB_ROOT;
#ifdef CONFIG_DUET_STATS
	bittree->statcur = bittree->statmax = 0;
#endif /* CONFIG_DUET_STATS */
}

void bittree_destroy(struct duet_bittree *bittree)
{
	struct rb_node *rbnode;
	struct bmap_rbnode *bnode;

	while (!RB_EMPTY_ROOT(&bittree->root)) {
		rbnode = rb_first(&bittree->root);
		bnode = rb_entry(rbnode, struct bmap_rbnode, node);
		bnode_dispose(bnode, rbnode, bittree);
	}
}
