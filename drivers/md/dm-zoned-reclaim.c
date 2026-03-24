// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */

#include "dm-zoned.h"
#include "dm-zoned-reclaim.h"
//#include "dm-zoned-metadata.c"

#include <linux/module.h>

#define	DM_MSG_PREFIX		"zoned reclaim"

/*
struct dmz_reclaim {
	struct dmz_metadata     *metadata;

	struct delayed_work	work;
	struct workqueue_struct *wq;

	struct dm_kcopyd_client	*kc;
	struct dm_kcopyd_throttle kc_throttle;
	int			kc_err;

	int			dev_idx;

	unsigned long		flags;

	//Last target access time 
	unsigned long		atime;
};
*/

/*
 * Reclaim state flags.
 */
enum {
	DMZ_RECLAIM_KCOPY,
};

/*
 * Number of seconds of target BIO inactivity to consider the target idle.
 */
#define DMZ_IDLE_PERIOD			(10UL * HZ)

/*
 * Percentage of unmapped (free) random zones below which reclaim starts
 * even if the target is busy.
 */
#define DMZ_RECLAIM_LOW_UNMAP_ZONES	30

/*
 * Percentage of unmapped (free) random zones above which reclaim will
 * stop if the target is busy.
 */
#define DMZ_RECLAIM_HIGH_UNMAP_ZONES	50

/*
 * Align a sequential zone write pointer to chunk_block.
 */
static int dmz_reclaim_align_wp(struct dmz_reclaim *zrc, struct dm_zone *zone,
				sector_t block)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dmz_dev *dev = zone->dev;
	sector_t wp_block = zone->wp_block;
	unsigned int nr_blocks;
	int ret;

	if (wp_block == block)
		return 0;

	if (wp_block > block)
		return -EIO;

	/*
	 * Zeroout the space between the write
	 * pointer and the requested position.
	 */
	nr_blocks = block - wp_block;
	ret = blkdev_issue_zeroout(dev->bdev,
				   dmz_start_sect(zmd, zone) + dmz_blk2sect(wp_block),
				   dmz_blk2sect(nr_blocks), GFP_NOIO, 0);
	if (ret) {
		dmz_dev_err(dev,
			    "Align zone %u wp %llu to %llu (wp+%u) blocks failed %d",
			    zone->id, (unsigned long long)wp_block,
			    (unsigned long long)block, nr_blocks, ret);
		dmz_check_bdev(dev);
		return ret;
	}

	zone->wp_block = block;

	return 0;
}

/*
 * dm_kcopyd_copy end notification.
 */
static void dmz_reclaim_kcopy_end(int read_err, unsigned long write_err,
				  void *context)
{
	struct dmz_reclaim *zrc = context;

	if (read_err || write_err) {
		trace_printk("[DEBUGRECL] dmz_reclaim_kcopy_end read_err %d, write_err %u\n", read_err, write_err);
		zrc->kc_err = -EIO;
	}
	else {
		zrc->kc_err = 0;
	}

	clear_bit_unlock(DMZ_RECLAIM_KCOPY, &zrc->flags);
	smp_mb__after_atomic();
	wake_up_bit(&zrc->flags, DMZ_RECLAIM_KCOPY);
}

static void dmz_update_mapped_chunk(struct dm_zone *zone, unsigned int c_id) {
	struct dm_chunk *c;
	list_for_each_entry(c, &zone->chunks, link) {
		if (c->id == c_id) { 
			zone->nr_mapped_chunk -= c->rz_weight; 
			if (c->weight > DMZ_BLOCK_PER_ZONE / 2) {
		        c->rz_weight = 4;
		    }
		    else if (c->weight > DMZ_BLOCK_PER_ZONE / 4) {
		        c->rz_weight = 2;
		    }
			else {
				c->rz_weight = 1;
			}
			zone->nr_mapped_chunk += c->rz_weight; 
			break; 
		}
	}
}

/*
 * Copy valid blocks of src_zone into dst_zone.
 */
static int dmz_reclaim_copy(struct dmz_reclaim *zrc,
			    struct dm_zone *src_zone, struct dm_zone *dst_zone)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_io_region src, dst;
	sector_t block = 0, end_block;
	sector_t nr_blocks;
	sector_t src_zone_block;
	sector_t dst_zone_block;
	unsigned long flags = 0;
	int ret;
	int is_rnd = 0;

	if (dmz_is_seq(src_zone))
		end_block = src_zone->wp_block;
	else {
		end_block = dmz_zone_nr_blocks(zmd);
		is_rnd = 1;
	}
	src_zone_block = dmz_start_block(zmd, src_zone);
	dst_zone_block = dmz_start_block(zmd, dst_zone);

	if (dmz_is_seq(dst_zone))
		flags |= BIT(DM_KCOPYD_WRITE_SEQ);

	/*modi*/
	uint64_t recl_block_log = 0;
	while (block < end_block) {
		if (src_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}
		if (dst_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}

		if (dmz_reclaim_should_terminate(src_zone)) {
			return -EINTR;
		}

		/* Get a valid region from the source zone */
		ret = dmz_first_valid_block(zmd, src_zone, &block);
		if (ret <= 0) {
			return ret;
		}
		nr_blocks = ret;
		/*modi*/
		recl_block_log += nr_blocks;

		/*
		 * If we are writing in a sequential zone, we must make sure
		 * that writes are sequential. So Zeroout any eventual hole
		 * between writes.
		 */
		if (dmz_is_seq(dst_zone)) {
			ret = dmz_reclaim_align_wp(zrc, dst_zone, block);
			if (ret) {
				return ret;
			}
		}

		src.bdev = src_zone->dev->bdev;
		src.sector = dmz_blk2sect(src_zone_block + block);
		src.count = dmz_blk2sect(nr_blocks);

		dst.bdev = dst_zone->dev->bdev;
		dst.sector = dmz_blk2sect(dst_zone_block + block);
		dst.count = src.count;

		/* Copy the valid region */
		set_bit(DMZ_RECLAIM_KCOPY, &zrc->flags);
		dm_kcopyd_copy(zrc->kc, &src, 1, &dst, flags,
			       dmz_reclaim_kcopy_end, zrc);

		/* Wait for copy to complete */
		//trace_printk("[DEBUGRECL] dmz_reclaim_copy wait_on_bit_io start\n");
		wait_on_bit_io(&zrc->flags, DMZ_RECLAIM_KCOPY,
			       TASK_UNINTERRUPTIBLE);
		//trace_printk("[DEBUGRECL] dmz_reclaim_copy wait_on_bit_io end\n");
		if (zrc->kc_err) {
			return zrc->kc_err;
		}

		block += nr_blocks;
		if (dmz_is_seq(dst_zone))
			dst_zone->wp_block = block;
	}

	/*modi*/

	return 0;
}

static int dmz_reclaim_copy_rnd(struct dmz_reclaim *zrc,
			    struct dm_zone *src_zone, struct dm_zone *dst_zone, struct dm_chunk *c)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_io_region src, dst;
	sector_t block = 0, chunk_block = 0, end_block;
	sector_t nr_blocks;
	sector_t src_zone_block;
	sector_t dst_zone_block;
	unsigned long flags = 0;
	int ret, recl_count = 0;
	int is_rnd = 0;
	ktime_t start, end;
    s64 actual_time;
	
	if (c == NULL) {
		trace_printk("\t\t\t[DEBUGRECL] dmz_reclaim_copy_rnd c == NULL error\n"); 
	}

	if (dmz_is_seq(src_zone))
		end_block = src_zone->wp_block;
	else {
		end_block = dmz_zone_nr_blocks(zmd);
		is_rnd = 1;
	}
	src_zone_block = dmz_start_block(zmd, src_zone);
	dst_zone_block = dmz_start_block(zmd, dst_zone);

	if (dmz_is_seq(dst_zone))
		flags |= BIT(DM_KCOPYD_WRITE_SEQ);

	/*modi*/
	uint64_t recl_block_log = 0;
	while (chunk_block < end_block) {
		if (src_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}
		if (dst_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}

//		if (dmz_reclaim_should_terminate(src_zone) {
		
		if (dmz_chunk_reclaim_should_terminate(c)) {
			trace_printk("[WRITELEN] reclaim_copy_rnd fail\n");
			return -EINTR;
		}
		

		/* Get a valid region from the source zone */
//		ret = dmz_first_valid_block(zmd, src_zone, &block);
		/*modi*/
		sector_t cur_rz_offset;
		sector_t i = chunk_block;
		start = ktime_get();
		while (i < DMZ_BLOCK_PER_ZONE) {
			if (c->offsets[i] == -1) { i++; continue; }
			cur_rz_offset = c->offsets[i];
			chunk_block = i;
//			i++;
			break;
		}
		if (DMZ_BLOCK_PER_ZONE <= i) {
			if (is_rnd == 1) { trace_printk("[EVAL] rnd_reclaim %llu count %d\n", recl_block_log, recl_count); }
			else if (is_rnd == 0) { trace_printk("[EVAL] seq_reclaim %llu count %d\n", recl_block_log, recl_count); }
			else { trace_printk("[EVAL] else_reclaim %llu count %d\n", recl_block_log, recl_count); }
			return 0;
		}
		block = cur_rz_offset;
		ret = 1;
		if (chunk_block < DMZ_BLOCK_PER_ZONE - 1) {
			int j = chunk_block + 1;
			while (j < DMZ_BLOCK_PER_ZONE) {
				int next_rz_offset = c->offsets[j];
				if (cur_rz_offset + 1 != next_rz_offset) { break; }
				cur_rz_offset = next_rz_offset;
				ret++;
				j++;
			}
		}
		end = ktime_get();
		actual_time = ktime_to_ns(ktime_sub(end, start));
//		trace_printk("[EVAL] block %d time %lld\n", ret, (long long)actual_time);
		/*modi*/

		if (ret <= 0) {
			if (ret == 0) {
				if (is_rnd == 1) { trace_printk("[EVAL] rnd_reclaim %llu count %d\n", recl_block_log, recl_count); }
				else if (is_rnd == 0) { trace_printk("[EVAL] seq_reclaim %llu count %d\n", recl_block_log, recl_count); }
				else { trace_printk("[EVAL] else_reclaim %llu count %d\n", recl_block_log, recl_count); }
			}
			return ret;
		}
		nr_blocks = ret;
		/*modi*/
		recl_block_log += nr_blocks;
		trace_printk("[SEQTEST] chunk %u chunk_block %u nr_blocks %u\n", c->id, chunk_block, nr_blocks);

		/*
		 * If we are writing in a sequential zone, we must make sure
		 * that writes are sequential. So Zeroout any eventual hole
		 * between writes.
		 */
		if (dmz_is_seq(dst_zone)) {
			ret = dmz_reclaim_align_wp(zrc, dst_zone, chunk_block);
			
			if (ret) {
				return ret;
			}
		}

		src.bdev = src_zone->dev->bdev;
		src.sector = dmz_blk2sect(src_zone_block + block);
		src.count = dmz_blk2sect(nr_blocks);

		dst.bdev = dst_zone->dev->bdev;
		dst.sector = dmz_blk2sect(dst_zone_block + chunk_block);
		dst.count = src.count;

		/* Copy the valid region */
		set_bit(DMZ_RECLAIM_KCOPY, &zrc->flags);
		dm_kcopyd_copy(zrc->kc, &src, 1, &dst, flags,
			       dmz_reclaim_kcopy_end, zrc);
		recl_count++;

		/* Wait for copy to complete */
		wait_on_bit_io(&zrc->flags, DMZ_RECLAIM_KCOPY,
			       TASK_UNINTERRUPTIBLE);
		if (zrc->kc_err) {
			return zrc->kc_err;
		}

		chunk_block += nr_blocks;
		if (dmz_is_seq(dst_zone)) {
			dst_zone->wp_block = chunk_block;
		}	
	}

	/*modi*/
	if (is_rnd == 1) { trace_printk("[EVAL] rnd_reclaim %llu count %d\n", recl_block_log, recl_count); }
    else if (is_rnd == 0) { trace_printk("[EVAL] seq_reclaim %llu count %d\n", recl_block_log, recl_count); }
	else { trace_printk("[EVAL] else_reclaim %llu count %d\n", recl_block_log, recl_count); }

	return 0;
}

static int dmz_reclaim_copy_buf(struct dmz_reclaim *zrc,
			    struct dm_zone *src_zone, struct dm_zone *dst_zone)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_io_region src, dst;
	sector_t block = 0, chunk_block = 0, end_block;
	sector_t nr_blocks;
	sector_t src_zone_block;
	sector_t dst_zone_block;
	unsigned long flags = 0;
	int ret;
	int is_rnd = 0;

	if (dmz_is_seq(src_zone))
		end_block = src_zone->wp_block;
	else {
		end_block = dmz_zone_nr_blocks(zmd);
		is_rnd = 1;
	}
	src_zone_block = dmz_start_block(zmd, src_zone);
	dst_zone_block = dmz_start_block(zmd, dst_zone);

	if (dmz_is_seq(dst_zone))
		flags |= BIT(DM_KCOPYD_WRITE_SEQ);

	/*modi*/
	uint64_t recl_block_log = 0;
	while (chunk_block < end_block) {
		if (src_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}
		if (dst_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}

		if (dmz_reclaim_should_terminate(src_zone)) {
			return -EINTR;
		}

		/* Get a valid region from the source zone */
		/*modi*/
		int cur_rz_offset;
		if (ret <= 0) {
			if (ret == 0) {
				if (is_rnd == 1) { trace_printk("[EVAL] rnd_reclaim %llu\n", recl_block_log); }
	            else if (is_rnd == 0) { trace_printk("[EVAL] seq_reclaim %llu\n", recl_block_log); }
	            else { trace_printk("[EVAL] else_reclaim %llu\n", recl_block_log); }
			}
			return ret;
		}
		nr_blocks = ret;
		/*modi*/
		recl_block_log += nr_blocks;

		/*
		 * If we are writing in a sequential zone, we must make sure
		 * that writes are sequential. So Zeroout any eventual hole
		 * between writes.
		 */
		if (dmz_is_seq(dst_zone)) {
			ret = dmz_reclaim_align_wp(zrc, dst_zone, block);
			if (ret) {
				return ret;
			}
		}

		src.bdev = src_zone->dev->bdev;
		src.sector = dmz_blk2sect(src_zone_block + block);
		src.count = dmz_blk2sect(nr_blocks);

		dst.bdev = dst_zone->dev->bdev;
		dst.sector = dmz_blk2sect(dst_zone_block + block);
		dst.count = src.count;

		/* Copy the valid region */
		set_bit(DMZ_RECLAIM_KCOPY, &zrc->flags);
		dm_kcopyd_copy(zrc->kc, &src, 1, &dst, flags,
			       dmz_reclaim_kcopy_end, zrc);

		/* Wait for copy to complete */
		wait_on_bit_io(&zrc->flags, DMZ_RECLAIM_KCOPY,
			       TASK_UNINTERRUPTIBLE);
		if (zrc->kc_err) {
			return zrc->kc_err;
		}

		block += nr_blocks;
		if (dmz_is_seq(dst_zone))
			dst_zone->wp_block = block;
	}

	/*modi*/
	if (is_rnd == 1) { trace_printk("rnd_reclaim %llu\n", recl_block_log); }
    else if (is_rnd == 0) { trace_printk("seq_reclaim %llu\n", recl_block_log); }
	else { trace_printk("else_reclaim %llu\n", recl_block_log); }

	return 0;
}

static int dmz_reclaim_copy_seq(struct dmz_reclaim *zrc,
			    struct dm_zone *src_zone, struct dm_zone *seq_zone, struct dm_zone *dst_zone, struct dm_chunk *c)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_io_region src, seq, dst;
	struct dmz_mblk *seq_mblk;
	sector_t block = 0, chunk_block = 0, end_block;
	sector_t nr_blocks;
	sector_t src_zone_block;
	sector_t seq_zone_block;
	sector_t dst_zone_block;
	unsigned long flags = 0;
	int ret, rnd_recl_count = 0, seq_recl_count = 0;
	int is_rnd = 0;
	ktime_t start, end;
    s64 actual_time;

	if (c == NULL) {
		trace_printk("\t\t\t[DEBUGRECL] dmz_reclaim_copy_seq c == NULL error\n");
	}

	if (dmz_is_seq(src_zone))
		end_block = src_zone->wp_block;
	else {
		end_block = dmz_zone_nr_blocks(zmd);
		is_rnd = 1;
	}
	src_zone_block = dmz_start_block(zmd, src_zone);
	seq_zone_block = dmz_start_block(zmd, seq_zone);
	dst_zone_block = dmz_start_block(zmd, dst_zone);

	if (dmz_is_seq(dst_zone))
		flags |= BIT(DM_KCOPYD_WRITE_SEQ);

	/*modi*/
	uint64_t rnd_recl_block_log = 0;
	uint64_t seq_recl_block_log = 0;
	while (chunk_block < end_block) {
		if (src_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}
		if (dst_zone->dev->flags & DMZ_BDEV_DYING) {
			return -EIO;
		}

//		if (dmz_reclaim_should_terminate(src_zone)) {
		
		if (dmz_chunk_reclaim_should_terminate(c)) {
			trace_printk("[WRITELEN] reclaim_copy_seq fail\n");
			return -EINTR;
		}
		

		/* Get a valid region from the source zone */
//		ret = dmz_first_valid_block(zmd, src_zone, &block);
		/*modi*/
		sector_t cur_rz_offset;
		/*
		seq_mblk = dmz_get_bitmap(zmd, seq_zone, chunk_block);
		if (IS_ERR(seq_mblk))
			return PTR_ERR(seq_mblk);
		*/
		// seq zone bitmap & chunk offset comp -> find continuous data
		sector_t seq_cont_num = 0, seq_first_block = chunk_block, rnd_cont_num = 0, rnd_first_block = 0;
		start = ktime_get();
		seq_cont_num = dmz_first_valid_block(zmd, seq_zone, &seq_first_block);

		int i = chunk_block;
		while (i < DMZ_BLOCK_PER_ZONE) {
			if (c->offsets[i] != -1) { break; }
			i++;
		}
		cur_rz_offset = c->offsets[i];
		rnd_first_block = i;
		if (DMZ_BLOCK_PER_ZONE <= i && seq_cont_num <= 0) {
			trace_printk("[EVAL] rnd_reclaim %llu count %d\n", rnd_recl_block_log, rnd_recl_count);
			trace_printk("[EVAL] seq_reclaim %llu count %d\n", seq_recl_block_log, seq_recl_count);
			return 0;
		}
		if (rnd_first_block < seq_first_block) {
			block = cur_rz_offset;
			chunk_block = rnd_first_block;
			ret = 1;
			if (chunk_block != DMZ_BLOCK_PER_ZONE-1) {
				int j = chunk_block + 1;
				while (j < DMZ_BLOCK_PER_ZONE) {
					int next_rz_offset = c->offsets[j];
					if (cur_rz_offset + 1 != next_rz_offset) { j++; break; }
					cur_rz_offset = next_rz_offset;
					ret++;
					j++;
				}
			}
			is_rnd = 1;
		}
		else {
			block = seq_first_block;
			chunk_block = block;
			ret = seq_cont_num;
			is_rnd = 0;
		}
		end = ktime_get();
		actual_time = ktime_to_ns(ktime_sub(end, start));
//		trace_printk("[EVAL] block %d time %lld\n", ret, (long long)actual_time);

//		dmz_release_mblock(zmd, seq_mblk);
		/*modi*/

		if (ret <= 0) {
			if (ret == 0) {
				trace_printk("[EVAL] rnd_reclaim %llu count %d\n", rnd_recl_block_log, rnd_recl_count);
				trace_printk("[EVAL] seq_reclaim %llu count %d\n", seq_recl_block_log, seq_recl_count);
			}
			return ret;
		}
		nr_blocks = ret;
		trace_printk("[SEQTEST] chunk_block %u nr_blocks %u\n", chunk_block, nr_blocks);
		/*modi*/
		if (is_rnd) {
			rnd_recl_block_log += nr_blocks;
			rnd_recl_count++;
		}
		else {
			seq_recl_block_log += nr_blocks;
			seq_recl_count++;
		}

		/*
		 * If we are writing in a sequential zone, we must make sure
		 * that writes are sequential. So Zeroout any eventual hole
		 * between writes.
		 */
		if (dmz_is_seq(dst_zone)) {
			ret = dmz_reclaim_align_wp(zrc, dst_zone, chunk_block);
			if (ret) {
				return ret;
			}
		}

		if (rnd_first_block < seq_first_block) {
			src.bdev = src_zone->dev->bdev;
			src.sector = dmz_blk2sect(src_zone_block + block);
			src.count = dmz_blk2sect(nr_blocks);
		}
		else {
			seq.bdev = seq_zone->dev->bdev;
			seq.sector = dmz_blk2sect(seq_zone_block + block);
			seq.count = dmz_blk2sect(nr_blocks);
		}

		dst.bdev = dst_zone->dev->bdev;
		dst.sector = dmz_blk2sect(dst_zone_block + chunk_block);
		dst.count = dmz_blk2sect(nr_blocks);

		/* Copy the valid region */
		set_bit(DMZ_RECLAIM_KCOPY, &zrc->flags);
		if (rnd_first_block < seq_first_block) {
			dm_kcopyd_copy(zrc->kc, &src, 1, &dst, flags,
				       dmz_reclaim_kcopy_end, zrc);
		}
		else {
			dm_kcopyd_copy(zrc->kc, &seq, 1, &dst, flags,
						dmz_reclaim_kcopy_end, zrc);
		}

		/* Wait for copy to complete */
		wait_on_bit_io(&zrc->flags, DMZ_RECLAIM_KCOPY,
			       TASK_UNINTERRUPTIBLE);
		if (zrc->kc_err) {
			return zrc->kc_err;
		}

		chunk_block += nr_blocks;
		if (dmz_is_seq(dst_zone))
			dst_zone->wp_block = chunk_block;
	}

	/*modi*/
	trace_printk("[EVAL] rnd_reclaim %llu count %d\n", rnd_recl_block_log, rnd_recl_count);
	trace_printk("[EVAL] seq_reclaim %llu count %d\n", seq_recl_block_log, seq_recl_count);

	return 0;
}

/*
 * Move valid blocks of dzone buffer zone into dzone (after its write pointer)
 * and free the buffer zone.
 */
/*modi*/
/*
static int dmz_reclaim_buf(struct dmz_reclaim *zrc, struct dm_zone *dzone)
{
	struct dm_zone *bzone = dzone->bzone;
	sector_t chunk_block = dzone->wp_block;
	struct dmz_metadata *zmd = zrc->metadata;
	int ret;

	DMDEBUG("(%s/%u): Chunk %u, move buf zone %u (weight %u) to data zone %u (weight %u)",
		dmz_metadata_label(zmd), zrc->dev_idx,
		dzone->chunk, bzone->id, dmz_weight(bzone),
		dzone->id, dmz_weight(dzone));
*/
	/* Flush data zone into the buffer zone */
/*
	ret = dmz_reclaim_copy(zrc, bzone, dzone);
	if (ret < 0)
		return ret;

	dmz_lock_flush(zmd);
*/
	/* Validate copied blocks */
/*
	ret = dmz_merge_valid_blocks(zmd, bzone, dzone, chunk_block);
	if (ret == 0) {
	*/
		/* Free the buffer zone */
/*
		dmz_invalidate_blocks(zmd, bzone, 0, dmz_zone_nr_blocks(zmd));
		dmz_lock_map(zmd);
		dmz_unmap_zone(zmd, bzone);
		dmz_unlock_zone_reclaim(dzone);
		dmz_free_zone(zmd, bzone);
		dmz_unlock_map(zmd);
	}

	dmz_unlock_flush(zmd);

	return ret;
}
*/
/*modi*/

/*modi*/
static int dmz_reclaim_buf_data(struct dmz_reclaim *zrc, struct dm_zone *dzone, struct dm_chunk *c, int is_zone)
{	
	if (c == NULL) {
		trace_printk("\t\t[DEBUGRECL] dmz_reclaim_buf_data c == NULL error\n");
	}
	struct dm_zone *bzone = c->szone; // sequential zone
	sector_t chunk_block = dzone->wp_block;
	struct dmz_metadata *zmd = zrc->metadata;
	int ret;

	/*
	DMDEBUG("(%s/%u): Chunk %u, move buf zone %u (weight %u) to data zone %u (weight %u)",
		dmz_metadata_label(zmd), zrc->dev_idx,
		dzone->chunk, bzone->id, dmz_weight(bzone),
		dzone->id, dmz_weight(dzone));
		*/

	/* Flush data zone into the buffer zone */
	ret = dmz_reclaim_copy_rnd(zrc, dzone, bzone, c);
	if (ret < 0)
		return ret;

	dmz_lock_flush(zmd);

	/* Validate copied blocks */
	/*
	if (is_zone) {
		ret = dmz_merge_valid_blocks_for_zone_reclaim(zmd, dzone, c, bzone, chunk_block);
	}
	else {
	*/
		ret = dmz_merge_valid_blocks(zmd, dzone, c, bzone, chunk_block);
//	}
	if (ret == 0) {
			/*
		if (is_zone) {
			dmz_invalidate_blocks_modi(zmd, dzone, 0, dmz_zone_nr_blocks(zmd), c->id);
			dmz_lock_map(zmd);
			dzone->nr_mapped_chunk -= c->rz_weight;
			dmz_update_mapped_chunk(dzone, c->id);
			kfree(c);
			dmz_unlock_map(zmd);
		}
		else {
		*/
			/* Free the buffer zone */
			dmz_invalidate_blocks_modi(zmd, dzone, 0, dmz_zone_nr_blocks(zmd), c->id);
			dmz_lock_map(zmd);
			dmz_unmap_zone(zmd, dzone, c->id, bzone->id);
			//dmz_unlock_zone_reclaim(dzone);
			/* chunk list modi */
			if (is_zone) {
				dmz_unlock_chunk_for_zone_reclaim(c);
			}
			else {
				dmz_unlock_chunk_reclaim(c);
			}
			/* chunk list modi */
			/* rz chunk weight modi */
			dzone->nr_mapped_chunk -= c->rz_weight;
			/* rz chunk weight modi */
			if (dzone->nr_mapped_chunk < DMZ_CHUNK_PER_RZ) {
				dmz_free_zone(zmd, dzone);
			}
			list_del(&c->link);
			/* chunk list modi */
			list_del(&c->map_link);
			/* chunk list modi */
			kfree(c);
			dmz_unlock_map(zmd);
//		}
	}

	dmz_unlock_flush(zmd);


	return ret;
}
/*modi*/

/*
 * Merge valid blocks of dzone into its buffer zone and free dzone.
 */
static int dmz_reclaim_seq_data(struct dmz_reclaim *zrc, struct dm_zone *dzone, /*modi*/ struct dm_chunk *c, int is_zone)
{
	if (c == NULL) {
		trace_printk("[DEBUG] dmz_reclaim_seq_data c == NULL error\n");
	}
	unsigned int chunk = c->id;
	struct dm_zone *szone = NULL;
	struct dm_zone *bzone = c->szone; // sequential zone
	struct dmz_metadata *zmd = zrc->metadata;
	int ret = 0;
	/*modi*/
	int alloc_flags = DMZ_ALLOC_SEQ;

	/* Get a free random or sequential zone */
	dmz_lock_map(zmd);
again:
	szone = dmz_alloc_zone(zmd, zrc->dev_idx,
					alloc_flags | DMZ_ALLOC_RECLAIM);
	if (!szone && alloc_flags == DMZ_ALLOC_SEQ && dmz_nr_cache_zones(zmd)) {
		alloc_flags = DMZ_ALLOC_RND;
		goto again;
	}
	dmz_unlock_map(zmd);
	if (!szone)
		return -ENOSPC;
	/*modi*/

	DMDEBUG("(%s/%u): Chunk %u, move data zone %u (weight %u) to buf zone %u (weight %u)",
		dmz_metadata_label(zmd), zrc->dev_idx,
		chunk, dzone->id, dmz_weight(dzone),
		bzone->id, dmz_weight(bzone));

	/* Flush data zone into the buffer zone */
	ret = dmz_reclaim_copy_seq(zrc, dzone, bzone, szone, c);

	if (ret < 0)
		return ret;

	dmz_lock_flush(zmd);

	/* Validate copied blocks */
	/*
	if (is_zone) {
		ret = dmz_merge_valid_blocks_seq_for_zone_reclaim(zmd, dzone, bzone, c, szone, 0);
	}
	else {
	*/
		ret = dmz_merge_valid_blocks_seq(zmd, dzone, bzone, c, szone, 0); // dmz_merge_valid_blocks_seq
//	}
	if (ret == 0) {
			/*
		if (is_zone) {
			dmz_invalidate_blocks_modi(zmd, dzone, 0, dmz_zone_nr_blocks(zmd), c->id);
			dmz_invalidate_blocks(zmd, bzone, 0, dmz_zone_nr_blocks(zmd));
			dmz_lock_map(zmd);
			dzone->nr_mapped_chunk -= c->rz_weight;
			dmz_update_mapped_chunk(dzone, c->id);
			dmz_map_zone(zmd, szone, chunk, 0);
			kfree(c);
			dmz_unlock_map(zmd);
		}
		else {
		*/
			/*
			 * Free the data zone and remap the chunk to
			 * the buffer zone.
			 */
			dmz_invalidate_blocks_modi(zmd, dzone, 0, dmz_zone_nr_blocks(zmd), c->id);
			dmz_invalidate_blocks(zmd, bzone, 0, dmz_zone_nr_blocks(zmd));
			dmz_lock_map(zmd);
			dmz_unmap_zone(zmd, bzone, c->id, -1);
			dmz_unmap_zone(zmd, dzone, c->id, -1);
			//dmz_unlock_zone_reclaim(dzone);
			/* chunk list modi */
			if (is_zone) {
				dmz_unlock_chunk_for_zone_reclaim(c);
			}
			else {
				dmz_unlock_chunk_reclaim(c);
			}
			/* chunk list modi */
	//		dmz_free_zone(zmd, dzone);
			dmz_free_zone(zmd, bzone);
			/* rz chunk weight modi */
			dzone->nr_mapped_chunk -= c->rz_weight;
			/* rz chunk weight modi */
			if (dzone->nr_mapped_chunk < DMZ_CHUNK_PER_RZ) {
				dmz_free_zone(zmd, dzone);
			}
			list_del(&c->link);
			/* chunk list modi */
			list_del(&c->map_link);
			/* chunk list modi */
			kfree(c);
	//		dmz_free_zone(zmd, bzone);
	//		dmz_map_zone(zmd, bzone, chunk);
			/*modi*/
			dmz_map_seq_zone(zmd, szone, chunk);
			/*modi*/
			dmz_unlock_map(zmd);
//		}
	}

	dmz_unlock_flush(zmd);


	return ret;
}

/*
 * Move valid blocks of the random data zone dzone into a free sequential zone.
 * Once blocks are moved, remap the zone chunk to the sequential zone.
 */
static int dmz_reclaim_rnd_data(struct dmz_reclaim *zrc, struct dm_zone *dzone, /*modi*/ struct dm_chunk *c, int is_zone)
{
	if (c == NULL) {
		trace_printk("\t\t[DEBUGRECL] dmz_reclaim_rnd_data c == NULL error\n");
	}
	unsigned int chunk = c->id;
	struct dm_zone *szone = NULL;
	struct dmz_metadata *zmd = zrc->metadata;
	int ret;
	int alloc_flags = DMZ_ALLOC_SEQ;
	ktime_t start, end;
	s64 actual_time;

	/* Get a free random or sequential zone */
	dmz_lock_map(zmd);
again:
	szone = dmz_alloc_zone(zmd, zrc->dev_idx,
			       alloc_flags | DMZ_ALLOC_RECLAIM);
	if (!szone && alloc_flags == DMZ_ALLOC_SEQ && dmz_nr_cache_zones(zmd)) {
		alloc_flags = DMZ_ALLOC_RND;
		goto again;
	}
	dmz_unlock_map(zmd);
	if (!szone) {
		return -ENOSPC;
	}

	DMDEBUG("(%s/%u): Chunk %u, move %s zone %u (weight %u) to %s zone %u",
		dmz_metadata_label(zmd), zrc->dev_idx, chunk,
		dmz_is_cache(dzone) ? "cache" : "rnd",
		dzone->id, dmz_weight(dzone),
		dmz_is_rnd(szone) ? "rnd" : "seq", szone->id);

	/* Flush the random data zone into the sequential zone */
	trace_printk("[SEQTEST] dmz_reclaim_copy_rnd start szone %u chunk %u weight %u\n", szone->id, c->id, c->weight);
	start = ktime_get();
	ret = dmz_reclaim_copy_rnd(zrc, dzone, szone, c); // dmz_reclaim_copy_rnd
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
	trace_printk("[SEQTEST] dmz_reclaim_copy_rnd end szone %u chunk %u weight %u time %lld ret %d\n", 
					szone->id, c->id, c->weight, (long long)actual_time, ret);


	dmz_lock_flush(zmd);

	if (ret == 0) {
		/* Validate copied blocks */
			/*
		if (is_zone) {
			trace_printk("[TEST] dmz_copy_valid_blocks_for_zone_reclaim start\n");
			ret = dmz_copy_valid_blocks_for_zone_reclaim(zmd, dzone, szone, c);
			trace_printk("[TEST] dmz_copy_valid_blocks_for_zone_reclaim end\n");
		}
		else {
			trace_printk("[TEST] dmz_copy_valid_blocks start\n");
			*/
			
		trace_printk("[TEST] dmz_copy_valid_blocks start\n");
		ret = dmz_copy_valid_blocks(zmd, dzone, szone, c);
		trace_printk("[TEST] dmz_copy_valid_blocks end ret %d\n", ret);
			/*
			trace_printk("[TEST] dmz_copy_valid_blocks end\n");
		}
		*/
	}
	if (ret) {
		/* Free the sequential zone */
		dmz_lock_map(zmd);
		dmz_free_zone(zmd, szone);
		dmz_unlock_map(zmd);
	} else{
		/* Free the data zone and remap the chunk */
//		dmz_invalidate_blocks(zmd, dzone, 0, dmz_zone_nr_blocks(zmd));
		//dmz_invalidate_blocks_modi(zmd, dzone, 0, dmz_zone_nr_blocks(zmd), c->id);
			
		if (is_zone) {
			dmz_lock_map(zmd);
			dmz_unmap_zone(zmd, dzone, chunk, szone->id);
			dzone->nr_mapped_chunk -= c->rz_weight;
			trace_printk("[TEST] dmz_update_mapped_chunk start\n");
			dmz_update_mapped_chunk(dzone, c->id);
			trace_printk("[TEST] dmz_update_mapped_chunk end\n");
			trace_printk("[TEST] dmz_map_zone start\n");
			dmz_map_zone(zmd, szone, chunk, 0);
			trace_printk("[TEST] dmz_map_zone end\n");
			trace_printk("[TEST] dmz_unlock_chunk_for_zone_reclaim start\n");
			dmz_unlock_chunk_for_zone_reclaim(c);
			trace_printk("[TEST] dmz_unlock_chunk_for_zone_reclaim end\n");
//			kfree(c);
			trace_printk("[TEST] dmz_unlock_map start\n");
			dmz_unlock_map(zmd);
			trace_printk("[TEST] dmz_unlock_map end\n");
		}
		else {
		
			dmz_lock_map(zmd);
			trace_printk("[TEST] dmz_unmap_zone start\n");
			dmz_unmap_zone(zmd, dzone, chunk, szone->id);
			trace_printk("[TEST] dmz_unmap_zone end\n");
			//dmz_unlock_zone_reclaim(dzone);
			/* chunk list modi */
			trace_printk("[TEST] dmz_unlock_chunk_reclaim start\n");
			dmz_unlock_chunk_reclaim(c);
			trace_printk("[TEST] dmz_unlock_chunk_reclaim end\n");
			/* chunk list modi */
			/* rz chunk weight modi */
			trace_printk("[TEST] dzone nr_mapped_chunk change start\n");
			dzone->nr_mapped_chunk -= c->rz_weight;
			trace_printk("[TEST] dzone nr_mapped_chunk change end\n");
			/* rz chunk weight modi */
			if (dzone->nr_mapped_chunk < DMZ_CHUNK_PER_RZ) {
				trace_printk("[TEST] dmz_free_zone start\n");
				dmz_free_zone(zmd, dzone);
				trace_printk("[TEST] dmz_free_zone end\n");
			}
			trace_printk("[TEST] dmz_map_zone start\n");
			dmz_map_zone(zmd, szone, chunk, 0);
			trace_printk("[TEST] dmz_map_zone end\n");
			trace_printk("[TEST] list_del1 start\n");
			list_del(&c->link);
			trace_printk("[TEST] list_del1 end\n");
			/* chunk list modi */
			trace_printk("[TEST] list_del2 start\n");
			list_del(&c->map_link);
			trace_printk("[TEST] list_del2 end\n");
			/* chunk list modi */
			trace_printk("[TEST] kfree start\n");
			kfree(c);
			trace_printk("[TEST] kfree end\n");
			dmz_unlock_map(zmd);
		}
	}

	dmz_unlock_flush(zmd);

	return ret;
}

/*
 * Reclaim an empty zone.
 */
/*
static void dmz_reclaim_empty(struct dmz_reclaim *zrc, struct dm_zone *dzone)
{
	struct dmz_metadata *zmd = zrc->metadata;

	dmz_lock_flush(zmd);
	dmz_lock_map(zmd);
	dmz_unmap_zone(zmd, dzone);
	dmz_unlock_zone_reclaim(dzone);
	dmz_free_zone(zmd, dzone);
	dmz_unlock_map(zmd);
	dmz_unlock_flush(zmd);
}
*/
/*
 * Test if the target device is idle.
 */
static inline int dmz_target_idle(struct dmz_reclaim *zrc)
{
	return time_is_before_jiffies(zrc->atime + DMZ_IDLE_PERIOD);
}

/*
 * Find a candidate zone for reclaim and process it.
 */
static int dmz_do_reclaim_all_chunk(struct dmz_reclaim *zrc)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_zone *dzone;
	struct dm_zone *rzone;
	unsigned long start;
	int ret;

	/* Get a data zone */
	dzone = dmz_get_zone_for_reclaim(zmd, zrc->dev_idx,
					 dmz_target_idle(zrc));
	if (!dzone) { 
		trace_printk("[DEBUGRAC] dmz_get_zone_for_reclaim dzone == NULL error\n"); 
		return 0;
	}
	if (!dzone) {
		DMDEBUG("(%s/%u): No zone found to reclaim",
			dmz_metadata_label(zmd), zrc->dev_idx);
//		trace_printk("\t[DEBUG] dmz_do_reclaim !dzone\n");
		return -EBUSY;
	}
	rzone = dzone;

	start = jiffies;
	/*modi*/
	struct dm_chunk *max_w_c = NULL;
	list_for_each_entry(max_w_c, &dzone->chunks, link) {
		if (max_w_c == NULL) { trace_printk("\t[DEBUGRAC] max_w_c is NULL error\n"); return 0; }
		if (!max_w_c->szone) {
			ret = dmz_reclaim_rnd_data(zrc, dzone, max_w_c, 0);
			if (ret) { break; }
		}
		else {
			int min_offset = DMZ_BLOCK_PER_ZONE + 1;
			int i = 0;
			if (max_w_c == NULL) { trace_printk("\t[DEBUGRAC] max_w_c is NULL error2\n"); return 0; }
			while (i < DMZ_BLOCK_PER_ZONE) {
				if (max_w_c->offsets[i] == -1) { i++; continue; }
				min_offset = i;
				break;
			}
			if (max_w_c->szone->wp_block <= min_offset) {
				ret = dmz_reclaim_buf_data(zrc, dzone, max_w_c, 0); // buf -> default seq
				if (ret) { break; }
			}
			else {
				ret = dmz_reclaim_seq_data(zrc, dzone, max_w_c, 0); // buf & seq -> new seq
				if (ret) { break; }
			}
		}
	}
	/*modi*/
out:
	if (ret) {
		if (ret == -EINTR)
			DMDEBUG("(%s/%u): reclaim zone %u interrupted",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id);
		else
			DMDEBUG("(%s/%u): Failed to reclaim zone %u, err %d",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id, ret);
		dmz_unlock_zone_reclaim(dzone);
		return ret;
	}

	ret = dmz_flush_metadata(zrc->metadata);
	if (ret) {
		DMDEBUG("(%s/%u): Metadata flush for zone %u failed, err %d",
			dmz_metadata_label(zmd), zrc->dev_idx, rzone->id, ret);
		return ret;
	}

	DMDEBUG("(%s/%u): Reclaimed zone %u in %u ms",
		dmz_metadata_label(zmd), zrc->dev_idx,
		rzone->id, jiffies_to_msecs(jiffies - start));
	return 0;
}

static int dmz_do_reclaim(struct dmz_reclaim *zrc)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_zone *dzone;
	struct dm_zone *rzone;
	unsigned long start;
	int ret;

	/* Get a data zone */
	dzone = dmz_get_zone_for_reclaim(zmd, zrc->dev_idx,
					 dmz_target_idle(zrc));
	if (!dzone) { 
		return 0;
	}
	if (!dzone) {
		DMDEBUG("(%s/%u): No zone found to reclaim",
			dmz_metadata_label(zmd), zrc->dev_idx);
//		trace_printk("\t[DEBUG] dmz_do_reclaim !dzone\n");
		return -EBUSY;
	}
	rzone = dzone;

	start = jiffies;
	/*modi*/
	struct dm_chunk *c, *max_w_c = NULL;
	struct list_head *chunk_list = &(dzone->chunks);
	if (chunk_list == NULL) { trace_printk("[DEBUGRECL] chunk_list null error\n"); }
	list_for_each_entry(c, &(dzone->chunks), link) {
		if (!max_w_c || max_w_c->weight < c->weight) {
			max_w_c = c;
		}
	}
	if (max_w_c == NULL) { trace_printk("\t[DEBUGRECL] max_w_c is NULL error\n"); }
	if (!max_w_c->szone) {
		ret = dmz_reclaim_rnd_data(zrc, dzone, max_w_c, 0);
	}
	else {
		int min_offset = DMZ_BLOCK_PER_ZONE + 1;
		int i = 0;
		if (max_w_c == NULL) { trace_printk("\t[DEBUGRECL] max_w_c is NULL error2\n"); }
		while (i < DMZ_BLOCK_PER_ZONE) {
			if (max_w_c->offsets[i] == -1) { i++; continue; }
			min_offset = i;
			break;
		}
		if (max_w_c->szone->wp_block <= min_offset) {
			ret = dmz_reclaim_buf_data(zrc, dzone, max_w_c, 0); // buf -> default seq
		}
		else {
			ret = dmz_reclaim_seq_data(zrc, dzone, max_w_c, 0); // buf & seq -> new seq
		}
	}
	/*modi*/
out:
	if (ret) {
		if (ret == -EINTR)
			DMDEBUG("(%s/%u): reclaim zone %u interrupted",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id);
		else
			DMDEBUG("(%s/%u): Failed to reclaim zone %u, err %d",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id, ret);
		dmz_unlock_zone_reclaim(dzone);
		return ret;
	}

	ret = dmz_flush_metadata(zrc->metadata);
	if (ret) {
		DMDEBUG("(%s/%u): Metadata flush for zone %u failed, err %d",
			dmz_metadata_label(zmd), zrc->dev_idx, rzone->id, ret);
		return ret;
	}

	DMDEBUG("(%s/%u): Reclaimed zone %u in %u ms",
		dmz_metadata_label(zmd), zrc->dev_idx,
		rzone->id, jiffies_to_msecs(jiffies - start));
	return 0;
}

/* chunk list modi */
static int dmz_do_reclaim_chunk(struct dmz_reclaim *zrc)
{
	struct dmz_metadata *zmd = zrc->metadata;
	struct dm_zone *dzone;
	struct dm_zone *rzone;
	struct dm_chunk *chunk;
	unsigned long start;
	int ret;
	unsigned int chunk_id;

	/* Get a data zone */
//	dzone = dmz_get_zone_for_reclaim(zmd, zrc->dev_idx,
//					 dmz_target_idle(zrc));

	chunk = dmz_get_chunk_for_reclaim(zmd, zrc->dev_idx,
					dmz_target_idle(zrc));
	if (!chunk) {
		trace_printk("[WRITELEN] do_reclaim fail chunk == NULL\n");
		return -EBUSY;
	}
	dzone = chunk->rzone;
	if (!dzone) {
		DMDEBUG("(%s/%u): No zone found to reclaim",
			dmz_metadata_label(zmd), zrc->dev_idx);
//		trace_printk("\t[DEBUG] dmz_do_reclaim !dzone\n");
		return -EBUSY;
	}
	/* chunk list modi */
//	test_and_set_bit(DMZ_RECLAIM, &dzone->flags); // temp
	/* chunk list modi */
	rzone = dzone;

	start = jiffies;
	/*modi*/
	chunk_id = chunk->id;
	if (!chunk->szone) {
		ret = dmz_reclaim_rnd_data(zrc, dzone, chunk, 0);
	}
	else {
		int min_offset = DMZ_BLOCK_PER_ZONE + 1;
		int i = 0;
		while (i < DMZ_BLOCK_PER_ZONE) {
			if (chunk->offsets[i] == -1) { i++; continue; }
			min_offset = i;
			break;
		}
		if (chunk->szone->wp_block <= min_offset) {
			ret = dmz_reclaim_buf_data(zrc, dzone, chunk, 0); // buf -> default seq
		}
		else {
			ret = dmz_reclaim_seq_data(zrc, dzone, chunk, 0); // buf & seq -> new seq
		}
	}
	/*modi*/
out:
	if (ret) {
		if (ret == -EINTR)
			DMDEBUG("(%s/%u): reclaim zone %u interrupted",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id);
		else
			DMDEBUG("(%s/%u): Failed to reclaim zone %u, err %d",
				dmz_metadata_label(zmd), zrc->dev_idx,
				rzone->id, ret);
		//dmz_unlock_zone_reclaim(dzone);
		/* chunk list modi */
		dmz_unlock_chunk_reclaim(chunk);
		/* chunk list modi */
		return ret;
	}

	ret = dmz_flush_metadata(zrc->metadata);
	if (ret) {
		DMDEBUG("(%s/%u): Metadata flush for zone %u failed, err %d",
			dmz_metadata_label(zmd), zrc->dev_idx, rzone->id, ret);
		return ret;
	}

	DMDEBUG("(%s/%u): Reclaimed zone %u in %u ms",
		dmz_metadata_label(zmd), zrc->dev_idx,
		rzone->id, jiffies_to_msecs(jiffies - start));

	trace_printk("[RZW] reclaim chunk %u zone %u\n", chunk_id, dzone->id);
	return 0;
}
/* chunk list modi */

static unsigned int dmz_reclaim_percentage(struct dmz_reclaim *zrc)
{
	unsigned int nr_unmap_p;	
	nr_unmap_p = dmz_nr_unmap_chunk_cache_zones(zrc->metadata);

	return nr_unmap_p;
}

/*
 * Test if reclaim is necessary.
 */
static bool dmz_should_reclaim(struct dmz_reclaim *zrc, unsigned int p_unmap)
{
	unsigned int nr_reclaim;

	nr_reclaim = dmz_nr_rnd_zones(zrc->metadata, zrc->dev_idx);

	if (dmz_nr_cache_zones(zrc->metadata)) {
		/*
		 * The first device in a multi-device
		 * setup only contains cache zones, so
		 * never start reclaim there.
		 */
		if (zrc->dev_idx == 0) {
			return false;
		}
		nr_reclaim += dmz_nr_cache_zones(zrc->metadata);
	}

	/* Reclaim when idle */


	
	if (dmz_target_idle(zrc) && nr_reclaim) {
		return true;
	}
	
	

	/* If there are still plenty of cache zones, do not reclaim */
	
	if (p_unmap >= DMZ_RECLAIM_HIGH_UNMAP_ZONES) {
		return false;
	}
	

	/*
	 * If the percentage of unmapped cache zones is low,
	 * reclaim even if the target is busy.
	 */
	return p_unmap <= DMZ_RECLAIM_LOW_UNMAP_ZONES;
}

/* zone reclaim modi */
/*
bool dmz_get_high_weight_zone_for_reclaim(struct dmz_metadata *zmd, struct dm_zone *ret_zone, struct dm_chunk *ret_c) {
	struct dm_zone *zone;
	struct dm_chunk *c, *maxw_c = NULL;
	dmz_lock_map(zmd);
	if (zmd->nr_cache) {
		zone_list = &zmd->map_cache_list;
		if (idle && list_empty(zone_list))
			zone_list = &zmd->dev[idx].map_rnd_list;
	}
	else
		zone_list = &zmd->dev[idx].map_rnd_list;

	list_for_each_entry(zone, zone_list, link) {
		if (zone->weight * 100 / DMZ_BLOCK_PER_ZONE > DMZ_ZONE_RECLAIM_WEIGHT) {
			if (dmz_lock_zone_reclaim(zone)) {
				list_for_each_entry(c, &zone->chunks, link) {
					if (!maxw_c || maxw_c->weight < c->weight) {
						maxw_c = c;
					}
				}
				ret_c = maxw_c;
				ret_zone = zone;
				dmz_unlock_map(zmd);
				return true;
			}
		}
	}
	dmz_unlock_map(zmd);
	return false;
}
*/
static int dmz_do_zone_reclaim(struct dmz_reclaim *zrc, struct dm_zone *zone, struct dm_chunk *c) {
	struct dmz_metadata *zmd = zrc->metadata;
	unsigned long start;
	int ret;
	unsigned int chunk_id = c->id;

	start = jiffies;
	if (!c->szone)
		ret = dmz_reclaim_rnd_data(zrc, zone, c, 1);
	else {
		int min_offset = DMZ_BLOCK_PER_ZONE + 1;
		int i = 0;
		while (i < DMZ_BLOCK_PER_ZONE) {
			if (c->offsets[i] == -1) { i++; continue; }
			min_offset = i;
			break;
		}
		if (c->szone->wp_block <= min_offset)
			ret = dmz_reclaim_buf_data(zrc, zone, c, 1);
		else 
			ret = dmz_reclaim_seq_data(zrc, zone, c, 1);
	}

out:
	if (ret) {
		if (ret == -EINTR)
            DMDEBUG("(%s/%u): reclaim zone %u interrupted",
                dmz_metadata_label(zmd), zrc->dev_idx,
                zone->id);
        else
            DMDEBUG("(%s/%u): Failed to reclaim zone %u, err %d",
                dmz_metadata_label(zmd), zrc->dev_idx,
                zone->id, ret);
        //dmz_unlock_zone_reclaim(zone);
		/* chunk list modi */
		dmz_unlock_chunk_for_zone_reclaim(c);
		/* chunk list modi */
        return ret;
    }

    ret = dmz_flush_metadata(zrc->metadata);
    if (ret) {
        DMDEBUG("(%s/%u): Metadata flush for zone %u failed, err %d",
            dmz_metadata_label(zmd), zrc->dev_idx, zone->id, ret);
        return ret;
    }

    DMDEBUG("(%s/%u): Reclaimed zone %u in %u ms",
        dmz_metadata_label(zmd), zrc->dev_idx,
        zone->id, jiffies_to_msecs(jiffies - start));
	trace_printk("[RZW] zone_reclaim chunk %u zone %u\n", chunk_id, zone->id);
    return 0;
}
/* zone reclaim modi */

/*
 * Reclaim work function.
 */
static void dmz_reclaim_work(struct work_struct *work)
{
//	trace_printk("[DEBUG] dmz_reclaim_work start\n");
	struct dmz_reclaim *zrc = container_of(work, struct dmz_reclaim, work.work);
	/*modi*/
	atomic_inc(&zrc->active_reclaim);
	/*modi*/
	struct dmz_metadata *zmd = zrc->metadata;
	unsigned int p_unmap;
	int ret;
	/* time debug */
	ktime_t start, end;
	s64 actual_time;
	/* time debug */

	if (dmz_dev_is_dying(zmd)) {
		/* modi */
		atomic_dec(&zrc->active_reclaim);
		/* modi */
		return;
	}

	/* zone reclaim modi */

	trace_printk("[WRITELEN] dmz_reclaim_work start is_zone_reclaim %d\n", zrc->is_zone_reclaim);
	if (zrc->is_zone_reclaim) {
		trace_printk("[WRITELEN] zone_reclaim start\n");
		struct dm_zone *zone = zrc->recl_zone;
		struct dm_chunk *c = zrc->recl_chunk;
		/* time debug */
		start = ktime_get();
		/* time debug */
//		bool get_reclaim_zone = dmz_get_high_weight_zone_for_reclaim(zmd, zrc->dev_idx, 
//						dmz_target_idle(zrc), zone, &c);
		trace_printk("[TEST] dmz_get_high_weight_c start\n");
		/*
		bool get_recl_chunk = dmz_get_high_weight_c(zmd, zrc->dev_idx,
						dmz_target_idle(zrc), zone, &c);
		*/
		bool get_recl_chunk = 1;
		trace_printk("[TEST] dmz_get_high_weight_c %d end\n", get_recl_chunk);
//		if (get_reclaim_zone) {
//		trace_printk("[ZONERECL] zone %u get_recl_chunk %d\n", zone->id, get_recl_chunk);
		if (get_recl_chunk) {
			
			if (zone == NULL) { 
				trace_printk("[WRITELEN] zone_reclaim fail zone == NULL\n");
				atomic_dec(&zrc->active_reclaim);
				return; 
			}
			
//			list_for_each_entry(c, &zone->chunks, link) {
			if (c == NULL) { 
				trace_printk("[WRITELEN] zone_reclaim fail chunk == NULL\n");
				atomic_dec(&zrc->active_reclaim);
				return; 
			}
				//trace_printk("[DEBUGRZ] dmz_get_high_weight_zone_for_reclaim chunk %u zone %u\n", c->id, zone->id);
			ret = dmz_do_zone_reclaim(zrc, zone, c);
//				if (ret) { break; }
//			}
			if (ret && ret != -EINTR) {
				if (!dmz_check_dev(zmd)) {
					atomic_dec(&zrc->active_reclaim);
					return;
				}
			}
			dmz_schedule_reclaim(zrc);
			atomic_dec(&zrc->active_reclaim);
			/* time debug */
			end = ktime_get();
			actual_time = ktime_to_ns(ktime_sub(end, start));
			if (ret == 0) {
//				trace_printk("[TIME] reclaim success time %lld ns\n", (long long)actual_time);
			}
			else {
//				trace_printk("[TIME] reclaim fail %d time %lld ns\n", ret, (long long)actual_time);
			}
			/* time debug */
			return;	
		}
		else {
			trace_printk("[WRITELEN] dmz_get_high_weight_zone_for_reclaim == 0 zone_recl fail\n");
		}
	}

	/* zone reclaim modi */

	p_unmap = dmz_reclaim_percentage(zrc);

	if (!dmz_should_reclaim(zrc, p_unmap)) {
		/* zone reclaim modi */
		/*
		struct dm_zone *zone;
		struct dm_chunk *c;
		if (dmz_get_high_weight_zone_for_reclaim(zmd, zrc->dev_idx, dmz_target_idle(zrc), zone, c)) {
			ret = dmz_do_zone_reclaim(zrc, zone, c);
			if (ret && ret != -EINTR) {
				trace_printk("[DEBUGRECL] dmz_do_zone_reclaim error\n");
				if (!dmz_check_dev(zmd))
					return;
			}
			dmz_schedule_reclaim(zrc);
			return;
		}
		else {
			mod_delayed_work(zrc->wq, &zrc->work, DMZ_IDLE_PERIOD);
			return;
		}
		*/
		/* zone reclaim modi */

		zrc->is_zone_reclaim = 0;
		mod_delayed_work(zrc->wq, &zrc->work, DMZ_IDLE_PERIOD);
		/* modi */
		atomic_dec(&zrc->active_reclaim);
		/* modi */
		return;
	}
	

	/*
	 * We need to start reclaiming random zones: set up zone copy
	 * throttling to either go fast if we are very low on random zones
	 * and slower if there are still some free random zones to avoid
	 * as much as possible to negatively impact the user workload.
	 */

	/*
	if (p_unmap < DMZ_RECLAIM_LOW_UNMAP_ZONES / 2) {
        zrc->kc_throttle.throttle = 100;
    }
    else {
        zrc->kc_throttle.throttle = 0;
    }
	*/

	if (dmz_target_idle(zrc) || p_unmap < DMZ_RECLAIM_LOW_UNMAP_ZONES / 2) {
		/* Idle or very low percentage: go fast */
//		if (!zrc->kc_throttle) { trace_printk("[DEBUG] zrc->kc_throttle == NULL error\n"); }
		zrc->kc_throttle.throttle = 100;
	} else {
		/* Busy but we still have some random zone: throttle */
//		if (!zrc->kc_throttle) { trace_printk("[DEBUG] zrc->kc_throttle == NULL error\n"); }
		zrc->kc_throttle.throttle = min(75U, 100U - p_unmap / 2);
	}

	DMDEBUG("(%s/%u): Reclaim (%u): %s, %u%% free zones (%u/%u cache %u/%u random)",
		dmz_metadata_label(zmd), zrc->dev_idx,
		zrc->kc_throttle.throttle,
		(dmz_target_idle(zrc) ? "Idle" : "Busy"),
		p_unmap, dmz_nr_unmap_cache_zones(zmd),
		dmz_nr_cache_zones(zmd),
		dmz_nr_unmap_rnd_zones(zmd, zrc->dev_idx),
		dmz_nr_rnd_zones(zmd, zrc->dev_idx));

//	ret = dmz_do_reclaim(zrc);
//	ret = dmz_do_reclaim_all_chunk(zrc);
	start = ktime_get();
	ret = dmz_do_reclaim_chunk(zrc);
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
	if (ret == 0) {
		trace_printk("[TIME] reclaim success time %lld ns\n", (long long)actual_time);
	}
	else {
		trace_printk("[TIME] reclaim fail %d time %lld ns\n", ret, (long long)actual_time);
	}
	if (ret && ret != -EINTR) {
		if (!dmz_check_dev(zmd)) {
			/* modi */
			atomic_dec(&zrc->active_reclaim);
			/* modi */
			return;
		}
	}

	dmz_schedule_reclaim(zrc);
	/* modi */
	atomic_dec(&zrc->active_reclaim);
	/* modi */
}

/*
 * Initialize reclaim.
 */
int dmz_ctr_reclaim(struct dmz_metadata *zmd,
		    struct dmz_reclaim **reclaim, int idx)
{
	struct dmz_reclaim *zrc;
	int ret;

	zrc = kzalloc(sizeof(struct dmz_reclaim), GFP_KERNEL);
	if (!zrc)
		return -ENOMEM;

	zrc->metadata = zmd;
	zrc->atime = jiffies;
	zrc->dev_idx = idx;

	/* Reclaim kcopyd client */
	zrc->kc = dm_kcopyd_client_create(&zrc->kc_throttle);
	if (IS_ERR(zrc->kc)) {
		ret = PTR_ERR(zrc->kc);
		zrc->kc = NULL;
		goto err;
	}

	/* Reclaim work */
	INIT_DELAYED_WORK(&zrc->work, dmz_reclaim_work);
	zrc->wq = alloc_ordered_workqueue("dmz_rwq_%s_%d", WQ_MEM_RECLAIM,
					  dmz_metadata_label(zmd), idx);
	if (!zrc->wq) {
		ret = -ENOMEM;
		goto err;
	}

	/* modi*/
	atomic_set(&zrc->active_reclaim, 0);
	/* modi */
	*reclaim = zrc;
	queue_delayed_work(zrc->wq, &zrc->work, 0);

	return 0;
err:
	if (zrc->kc)
		dm_kcopyd_client_destroy(zrc->kc);
	kfree(zrc);

	return ret;
}

/*
 * Terminate reclaim.
 */
void dmz_dtr_reclaim(struct dmz_reclaim *zrc)
{
	cancel_delayed_work_sync(&zrc->work);
	destroy_workqueue(zrc->wq);
	dm_kcopyd_client_destroy(zrc->kc);
	kfree(zrc);
}

/*
 * Suspend reclaim.
 */
void dmz_suspend_reclaim(struct dmz_reclaim *zrc)
{
	cancel_delayed_work_sync(&zrc->work);
}

/*
 * Resume reclaim.
 */
void dmz_resume_reclaim(struct dmz_reclaim *zrc)
{
	queue_delayed_work(zrc->wq, &zrc->work, DMZ_IDLE_PERIOD);
}

/*
 * BIO accounting.
 */
void dmz_reclaim_bio_acc(struct dmz_reclaim *zrc)
{
	zrc->atime = jiffies;
}

/*
 * Start reclaim if necessary.
 */
void dmz_schedule_reclaim(struct dmz_reclaim *zrc)
{	
	unsigned int p_unmap = dmz_reclaim_percentage(zrc);

	if (dmz_should_reclaim(zrc, p_unmap)) {
		zrc->is_zone_reclaim = 0;
		mod_delayed_work(zrc->wq, &zrc->work, 0);
	}
}
