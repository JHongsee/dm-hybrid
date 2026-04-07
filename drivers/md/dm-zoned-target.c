// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */

#include "dm-zoned.h"
/* chunk list modi */
#include "dm-zoned-metadata.h"
/* chunk list modi */
#include "dm-zoned-reclaim.h"

#include <linux/module.h>

#define	DM_MSG_PREFIX		"zoned"

#define DMZ_MIN_BIOS		8192

/*
 * Zone BIO context.
 */
struct dmz_bioctx {
	struct dmz_dev		*dev;
	struct dm_zone		*zone;
	struct bio		*bio;
	refcount_t		ref;
	/* chunk list modi */
	struct dmz_metadata *zmd;
	unsigned int chunk_id;
	/* chunk list modi */
	/* chunk snapshot modi */
	/*
	unsigned int wp;
	unsigned int nr_blocks;
	*/
	/* chunk snapshot modi */
};

/*
 * Chunk work descriptor.
 */
struct dm_chunk_work {
	struct work_struct	work;
	refcount_t		refcount;
	struct dmz_target	*target;
	unsigned int		chunk;
	struct bio_list		bio_list;
};

/*
 * Target descriptor.
 */
struct dmz_target {
	struct dm_dev		**ddev;
	unsigned int		nr_ddevs;

	unsigned int		flags;

	/* Zoned block device information */
	struct dmz_dev		*dev;

	/* For metadata handling */
	struct dmz_metadata     *metadata;

	/* For chunk work */
	struct radix_tree_root	chunk_rxtree;
	struct workqueue_struct *chunk_wq;
	struct mutex		chunk_lock;

	/* For cloned BIOs to zones */
	struct bio_set		bio_set;

	/* For flush */
	spinlock_t		flush_lock;
	struct bio_list		flush_list;
	struct delayed_work	flush_work;
	struct workqueue_struct *flush_wq;
};

/*
 * Flush intervals (seconds).
 */
#define DMZ_FLUSH_PERIOD	(10 * HZ)

/*
 * Target BIO completion.
 */
static inline void dmz_bio_endio(struct bio *bio, blk_status_t status/*modi*/, int chunk /*modi*/, int parent)
{
	struct dmz_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct dmz_bioctx));
	/* chunk list modi */
	struct dmz_metadata *zmd;
	unsigned int chunk_id;
	//trace_printk("[BIOCHECK] dmz_bio_endio nr_blocks %u\n", nr_blocks);
//	unsigned int nr_blocks = bioctx->nr_blocks;
	if (chunk == -1) {
		zmd = bioctx->zmd;
		chunk_id = bioctx->chunk_id;
	}
	else if (chunk == -2) {
	}
	else {
		zmd = bioctx->zmd;
		chunk_id = chunk;
	}
	/* chunk list modi */

	if (status != BLK_STS_OK && bio->bi_status == BLK_STS_OK)
		bio->bi_status = status;
	if (bioctx->dev && bio->bi_status != BLK_STS_OK)
		bioctx->dev->flags |= DMZ_CHECK_BDEV;

	if (refcount_dec_and_test(&bioctx->ref)) {
		struct dm_zone *zone = bioctx->zone;

		if (zone) {
			if (bio->bi_status != BLK_STS_OK &&
			    bio_op(bio) == REQ_OP_WRITE &&
			    dmz_is_seq(zone))
				set_bit(DMZ_SEQ_WRITE_ERR, &zone->flags);

			dmz_deactivate_zone(zone);
			/* chunk list modi */
//			if (parent != 2) {
			    struct dm_chunk *c;
			    list_for_each_entry(c, &(zone->chunks), link) {
					trace_printk("[TEST] zone %u c->id %u chunk_id %u\n", zone->id, c->id, chunk_id);
			        if (c->id == chunk_id) { 
						/* chunk snapshot modi */
							/*
						if (bio_op(bio) == REQ_OP_WRITE && !dmz_is_seq(zone)) {
							trace_printk("[BIOCHECK] chunk_block %u nr_blocks %u wp %u\n"
											, dmz_chunk_block(zmd, dmz_bio_block(bio)), nr_blocks, bioctx->wp);
							trace_printk("[BIOCHECK] bitmap write chunk %u zone %u chunk_block %u nr_blocks %u\n",
											chunk_id, zone->id, dmz_chunk_block(zmd, dmz_bio_block(bio)), nr_blocks);
							spin_lock(&c->snap_lock);
							while (nr_blocks) {
								c->write_check[dmz_chunk_block(zmd, dmz_bio_block(bio)) + i] = c->id;
								nr_blocks--;
								i++;
							}
							*/
							/*
							if (c->weight > zmd->zone_nr_blocks / 2) {
					            zone->nr_mapped_chunk -= c->rz_weight;
					            c->rz_weight = 4;
					            zone->nr_mapped_chunk += c->rz_weight;
					        }
					        else if (c->weight > zmd->zone_nr_blocks / 4) {
					            zone->nr_mapped_chunk -= c->rz_weight;
					            c->rz_weight = 2;
					            zone->nr_mapped_chunk += c->rz_weight;
					        }
							*/
							/*
							spin_unlock(&c->snap_lock);
						}
						*/
						/* chunk snapshot modi */
						//trace_printk("[TEST] dmz_deactivate_chunk before c %u ref %d\n", c->id, atomic_read(&c->refcount));
						dmz_deactivate_chunk(c);
						//trace_printk("[TEST] dmz_deactivate_chunk after c %u ref %d\n", c->id, atomic_read(&c->refcount));
						break; 
					}
			    }
//			}
		    /* chunk list modi */
		}
		bio_endio(bio);
	}
}

/*
 * Completion callback for an internally cloned target BIO. This terminates the
 * target BIO when there are no more references to its context.
 */
static void dmz_clone_endio(struct bio *clone)
{
	struct dmz_bioctx *bioctx = clone->bi_private;
	struct dm_zone *zone = bioctx->zone;
	struct dmz_reclaim *zrc = bioctx->dev->reclaim;
	blk_status_t status = clone->bi_status;
	int ret;
	trace_printk("[TEST] dmz_clone_endio start zone %u chunk %u\n", zone->id, bioctx->chunk_id);

	bio_put(clone);
	//trace_printk("[BIOCHECK] dmz_clone_endio nr_blocks %u\n", bioctx->nr_blocks);
	dmz_bio_endio(bioctx->bio, status, -1, 1);
	/*modi*/

//	zone = bioctx->zone;
	trace_printk("[TEST] zone %u weight %u\n", zone->id, zone->weight);
	if (zone && !dmz_is_seq(zone) && zrc && /*!atomic_read(&zrc->active_reclaim) && */
					(zone->weight * 100 / DMZ_BLOCK_PER_ZONE > DMZ_ZONE_RECLAIM_WEIGHT)) {
//	if (zone->nr_mapped_chunk > DMZ_CHUNK_PER_RZ) {
		/*
		trace_printk("[WRITELEN] clone_endio zone %u weight %u\n", zone->id, zone->weight);
		zrc->is_zone_reclaim = 1;
		zrc->recl_zone = zone;
		mod_delayed_work(zrc->wq, &zrc->work, 0);
		*/
		/* chunk IO block modi */
		struct dm_chunk *c, *maxw_c = NULL;
		list_for_each_entry(c, &zone->chunks, link) {
//			trace_printk("[TEST] zone %u chunk %u c_weight %u\n", zone->id, c->id, c->weight);
			if (!maxw_c || maxw_c->weight < c->weight) {
				maxw_c = c;
			}
		}

		if (atomic_read(&maxw_c->zone_recl))
			return;

		ret = test_and_set_bit(DMZ_CHUNK_ZONE_RECLAIM, &maxw_c->flags);
		trace_printk("[TEST] zone %u chunk %u weight %u flag %u refcount %d DMZ_CHUNK_ZONE_RECLAIM %d ret %d\n", 
						zone->id, maxw_c->id, maxw_c->weight, maxw_c->flags, atomic_read(&maxw_c->refcount), DMZ_CHUNK_ZONE_RECLAIM, ret);
		if (atomic_read(&maxw_c->refcount) == 0) {
			atomic_inc(&maxw_c->zone_recl);
			zrc->is_zone_reclaim = 1;
			zrc->recl_zone = zone;
			zrc->recl_chunk = maxw_c;
			trace_printk("[TEST] recl wake\n");
			mod_delayed_work(zrc->wq, &zrc->work, 0);
		}
		/* chunk IO block modi */
	}
	trace_printk("[TEST] dmz_clone_endio zone %u end\n", zone->id);
	
	/*modi*/
}

/*
 * Issue a clone of a target BIO. The clone may only partially process the
 * original target BIO.
 */
static int dmz_submit_bio(struct dmz_target *dmz, struct dm_zone *zone,
			  struct bio *bio, sector_t chunk_block,
			  unsigned int nr_blocks, unsigned int chunk_id, unsigned int wp)
{
	struct dmz_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct dmz_bioctx));
	struct dmz_dev *dev = zone->dev;
	struct bio *clone;

	if (dev->flags & DMZ_BDEV_DYING)
		return -EIO;

	clone = bio_clone_fast(bio, GFP_NOIO, &dmz->bio_set);
	if (!clone)
		return -ENOMEM;

	bio_set_dev(clone, dev->bdev);
	bioctx->dev = dev;
	/* chunk list modi */
	bioctx->chunk_id = chunk_id;
	/* chunk list modi */
	/* chunk snapshot modi */
	/*
	bioctx->wp = wp;
	bioctx->nr_blocks = nr_blocks;
	trace_printk("[BIOCHECK] dmz_submit_bio nr_blocks %u\n", nr_blocks);
	*/
	/* chunk snapshot modi */
	clone->bi_iter.bi_sector =
		dmz_start_sect(dmz->metadata, zone) + dmz_blk2sect(chunk_block);
	clone->bi_iter.bi_size = dmz_blk2sect(nr_blocks) << SECTOR_SHIFT;
	clone->bi_end_io = dmz_clone_endio;
	clone->bi_private = bioctx;

	bio_advance(bio, clone->bi_iter.bi_size);

	refcount_inc(&bioctx->ref);
	submit_bio_noacct(clone);

	trace_printk("[TEST] submit bio zone %u chunk %u\n", zone->id, chunk_id);

	if (bio_op(bio) == REQ_OP_WRITE && dmz_is_seq(zone))
		zone->wp_block += nr_blocks;

	return 0;
}

/*
 * Zero out pages of discarded blocks accessed by a read BIO.
 */
static void dmz_handle_read_zero(struct dmz_target *dmz, struct bio *bio,
				 sector_t chunk_block, unsigned int nr_blocks)
{
	unsigned int size = nr_blocks << DMZ_BLOCK_SHIFT;

	/* Clear nr_blocks */
	swap(bio->bi_iter.bi_size, size);
	zero_fill_bio(bio);
	swap(bio->bi_iter.bi_size, size);

	bio_advance(bio, size);
}

/*
 * Process a read BIO.
 */
static int dmz_handle_read(struct dmz_target *dmz, struct dm_zone *zone,
			   struct bio *bio, unsigned int chunk_id)
{
	struct dmz_metadata *zmd = dmz->metadata;
	sector_t chunk_block = dmz_chunk_block(zmd, dmz_bio_block(bio));
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	sector_t end_block = chunk_block + nr_blocks;
	sector_t rp;
	struct dm_zone *rzone, *bzone;
	int ret;
	
	/* Read into unmapped chunks need only zeroing the BIO buffer */
	if (!zone) {
		//trace_printk("[BIOCHECK] dmz_handle_read !zone\n");
		zero_fill_bio(bio);
		return 0;
	}

//	trace_printk("[BIOCHECK] read chunk %u zone %u chunk_block %u nr_blocks %u\n",
//					chunk_id, zone->id, chunk_block, nr_blocks);
	DMDEBUG("(%s): READ chunk %llu -> %s zone %u, block %llu, %u blocks",
		dmz_metadata_label(zmd),
		(unsigned long long)dmz_bio_chunk(zmd, bio),
		(dmz_is_rnd(zone) ? "RND" :
		 (dmz_is_cache(zone) ? "CACHE" : "SEQ")),
		zone->id,
		(unsigned long long)chunk_block, nr_blocks);

	/* Check block validity to determine the read location */
	bzone = zone->bzone;
	/*modi*/
	while (chunk_block < end_block) {
		nr_blocks = 0;
		if (dmz_is_rnd(zone) || dmz_is_cache(zone)) {
			struct dm_chunk *c;
			list_for_each_entry(c, &zone->chunks, link) {
				if (c->id == chunk_id) { break; }
			}
			if (c->id != chunk_id) {
				//trace_printk("[BIOCHECK] dmz_handle_read c->id != chunk_id\n");
				return -10;
			}
			sector_t cur_rz_offset;
			sector_t i = chunk_block;
			while (i < DMZ_BLOCK_PER_ZONE) { // find first valid block
				if (c->offsets[i] == -1) { i++; continue; }
				cur_rz_offset = c->offsets[i];
				chunk_block = i;
				break;
			}
			//trace_printk("[BIOCHECK] dmz_handle_read cur_rz_offset %u i %u\n", cur_rz_offset, i);
			if (end_block <= i) {
			//	trace_printk("[BIOCHECK] dmz_handle_read end_block <= i\n");
				return 0;
			}
			rp = cur_rz_offset;
			ret = 1;
			if (chunk_block < end_block) {
				sector_t j = chunk_block + 1;
				while (j < end_block) {
					sector_t next_rz_offset = c->offsets[j];
					if (cur_rz_offset + 1 != next_rz_offset) { break; }
					cur_rz_offset = next_rz_offset;
					ret++;
					j++;
				}
			}
			nr_blocks = ret;
			rzone = zone;
		}
		if (dmz_is_seq(zone) && chunk_block < zone->wp_block) {
			ret = dmz_block_valid(zmd, zone, chunk_block);
			if (ret < 0) {
				//trace_printk("[BIOCHECK] dmz_handle_read ret < 0\n");
				return ret;
			}
			if (ret > 0) {
				/* Read data zone blocks */
				nr_blocks = ret;
				rzone = zone;
			}
		}
		if (!nr_blocks && bzone) {
			struct dm_chunk *c;
			list_for_each_entry(c, &bzone->chunks, link) {
				if (c->id == chunk_id) { break; }
			}
			if (c->id != chunk_id) {
				//trace_printk("[BIOCHECK] dmz_handle_read c->id != chunk_id2\n");
				return -10;
			}
			sector_t cur_rz_offset;
			sector_t i = chunk_block;
			while (i < DMZ_BLOCK_PER_ZONE) { // find first valid block
				if (c->offsets[i] == -1) { i++; continue; }
				cur_rz_offset = c->offsets[i];
				chunk_block = i;
				break;
			}
			if (end_block <= i) {
				//trace_printk("[BIOCHECK] dmz_handle_read end_block <= i\n");
				return 0;
			}
			rp = cur_rz_offset;
			ret = 1;
			if (chunk_block < end_block) {
				sector_t j = chunk_block + 1;
				while (j < end_block) {
					sector_t next_rz_offset = c->offsets[j];
					if (cur_rz_offset + 1 != next_rz_offset) { break; }
					cur_rz_offset = next_rz_offset;
					ret++;
					j++;
				}
			}
			nr_blocks = ret;
			rzone = bzone;
		}

		if (nr_blocks) {
			/* Valid blocks found: read them */
			nr_blocks = min_t(unsigned int, nr_blocks,
					  end_block - chunk_block);
			if (dmz_is_seq(rzone)) {
				//trace_printk("[BIOCHECK] dmz_handle_read dmz_is_seq\n");
				ret = dmz_submit_bio(dmz, rzone, bio,
						     chunk_block, nr_blocks, chunk_id, -1);
			}
			else {
				//trace_printk("[BIOCHECK] dmz_handle_read chunk %u chunk_block %u nr_blocks %u offset %u\n",
				//					chunk_id, chunk_block, nr_blocks, rp);
				ret = dmz_submit_bio(dmz, rzone, bio,
						     rp, nr_blocks, chunk_id, -1);
			}
			if (ret) {
				//trace_printk("[BIOCHECK] dmz_handle_read ret\n");
				return ret;
			}
			chunk_block += nr_blocks;
		} else {
			/* No valid block: zeroout the current BIO block */
			dmz_handle_read_zero(dmz, bio, chunk_block, 1);
			chunk_block++;
		}
	}
	/*modi*/

	return 0;
}

/*
void dmz_set_cache_wp(struct dmz_metadata *zmd, struct dm_zone *rzone) {
	int ret;
	ret = dmz_to_next_set_block(zmd, rzone, rzone->wp_block, zmd->zone_nr_blocks - rzone->wp_block, 0);
	if (ret >= 0) {
		rzone->wp_block += ret;
		return;
	}

	ret = dmz_to_next_set_block(zmd, rzone, 0, zmd->zone_nr_blocks, 0);
	rzone->wp_block = ret;
}
*/

/*
 * Write blocks directly in a data zone, at the write pointer.
 * If a buffer zone is assigned, invalidate the blocks written
 * in place.
 */
static int dmz_handle_direct_write(struct dmz_target *dmz,
				   struct dm_zone *zone, struct bio *bio,
				   sector_t chunk_block,
				   unsigned int nr_blocks,
				   /*modi*/ unsigned int chunk)
{
	struct dmz_metadata *zmd = dmz->metadata;
	struct dm_zone *bzone = zone->bzone;
	int ret, already_chunk = 0, c_off = -1, before_wp = 0;
	ktime_t start, end;
    s64 actual_time;

	if (dmz_is_readonly(zone))
		return -EROFS;

	/* Submit write */
	unsigned int sect_len_log = bio->bi_iter.bi_size;
	uint64_t c_idx_log = dmz_bio_chunk(zmd, bio);
	uint64_t sect_n_log = bio->bi_iter.bi_sector;
	int is_ext4 = bio_flagged(bio, BIO_EXT4_FILE);
	int is_clone = bio_flagged(bio, BIO_CLONED);

	start = ktime_get();
	if (!dmz_is_seq(zone)) {
		struct dm_chunk* c;
		list_for_each_entry(c, &(zone->chunks), link) {
			if (c->id == chunk) { break; }
		}
		if (c->id != chunk) {
			trace_printk("[DEBUG7] dmz_handle_direct_write zone %u c->id %u != chunk %u error\n", zone->id, c->id, chunk);
		}
		c_off = c->offsets[chunk_block];

		if (c_off != -1) {
			already_chunk = 1;
			trace_printk("[SEQTEST] overwrite chunk_id: %u zone_id: %u wp: %u start_block: %llu nr_blocks: %u\n",
						chunk, zone->id, c_off, chunk_block, nr_blocks);
			ret = dmz_submit_bio(dmz, zone, bio, c_off, nr_blocks, chunk, before_wp);
			before_wp = c_off;
			trace_printk("[CDF] chunk %u nr_blocks %u\n", chunk, nr_blocks);
			//trace_printk("[DEBUGBIT] dmz_submit_bio zone %u start %llu nr_blocks %u\n", zone->id, c_off, nr_blocks);
		}
		else {
			trace_printk("[CHUNKWEIGHT] isdata %d chunk %u nr_blocks %u\n", is_ext4, chunk, nr_blocks);
			
			spin_lock(&zone->wp_lock);
			dmz_set_cache_wp(zmd, zone, nr_blocks);
			before_wp = zone->wp_block;
			if (zone->wp_block + nr_blocks >= zmd->zone_nr_blocks - 1) {
				zone->wp_block = 0;
			}
			else {
				zone->wp_block += nr_blocks;
			}
			spin_unlock(&zone->wp_lock);
			
			trace_printk("[SEQTEST] direct_write chunk_id: %u zone_id: %u wp: %u start_block: %llu nr_blocks: %u\n",
						chunk, zone->id, before_wp, chunk_block, nr_blocks);
			ret = dmz_submit_bio(dmz, zone, bio, before_wp, nr_blocks, chunk, before_wp);
			trace_printk("[CDF] chunk %u nr_blocks %u\n", chunk, nr_blocks);
			//chunk_block = before_wp;
//			trace_printk("[DEBUGBIT] dmz_submit_bio zone %u start %llu nr_blocks %u\n", zone->id, before_wp, nr_blocks);
		}
//		trace_printk("\t\t[DEBUG] dmz_handle_direct_write only rnd zone\n");
//		trace_printk("\t\t[DEBUG] dmz_set_cache_wp start\n");
//		trace_printk("\t\t[DEBUG] dmz_set_cache_wp end\n");

//		trace_printk("\t\t[DEBUG] dmz_submit_bio start\n");
//		trace_printk("\t\t[DEBUG] dmz_submit_bio end\n");
	}
	else {
//		trace_printk("\t\t[DEBUG] dmz_handle_direct_write seq zone\n");
//		trace_printk("\t\t[DEBUG] dmz_submit_bio start\n");
		trace_printk("[SEQTEST] direct_write chunk_id: %u zone_id: %u wp: %u start_block: %llu nr_blocks: %u\n",
						chunk, zone->id, zone->wp_block, chunk_block, nr_blocks);
		ret = dmz_submit_bio(dmz, zone, bio, chunk_block, nr_blocks, chunk, -1);
//		trace_printk("\t\t[DEBUG] dmz_submit_bio end\n");
	}
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
//	trace_printk("[DIRWRITE] dmz_submit_bio time %lld ns\n", (long long) actual_time);
	if (ret) {
		return ret;
	}

	/*
	 * Validate the blocks in the data zone and invalidate
	 * in the buffer zone, if there is one.
	 */
//	trace_printk("\t\t[DEBUG] dmz_validate_blocks start\n");
	start = ktime_get();
//	if (!already_chunk) {
		trace_printk("[DEBUGINV] dmz_handle_direct_write dmz_validate_blocks start chunk_id: %u zone_id: %u\n", chunk, zone->id);
		ret = dmz_validate_blocks(zmd, zone, chunk_block, nr_blocks, chunk, before_wp);
		trace_printk("[DEBUGINV] dmz_handle_direct_write dmz_validate_blocks end chunk_id: %u zone_id: %u\n", chunk, zone->id);
//		trace_printk("\t\t[DEBUG] dmz_validate_blocks end\n");
		if (ret == 0 && bzone) {
//			trace_printk("\t\t[DEBUG] dmz_invalidate_blocks start\n");
			trace_printk("[DEBUGINV] dmz_handle_direct_write dmz_invalidate_blocks start zone_id: %u\n", bzone->id);
		/*	
			struct dm_chunk *c;
			list_for_each_entry(c, &bzone->chunks, link) {
				if (c->id == chunk) { break; }
			}
			if (c->id == chunk) {
				chunk_block = c->offsets[chunk_block & DMZ_BLOCK_MASK_BITS];
			}
			else {
				trace_printk("[DEBUGINV] dmz_handle_bio c->id != chunk error5\n");
			}
		*/	
			ret = dmz_invalidate_blocks_modi(zmd, bzone, chunk_block, nr_blocks, chunk, 32768);
			trace_printk("[DEBUGINV] dmz_handle_direct_write dmz_invalidate_blocks end\n");
	//		trace_printk("\t\t[DEBUG] dmz_invalidate_blocks end\n");
		}
//	}
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
//	trace_printk("[DIRWRITE] update_bitmap time %lld ns\n", (long long) actual_time);
/*	
	if (is_ext4) {
        trace_printk("[EVAL] ext4directwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else if (is_jrnl) {
        trace_printk("[EVAL] jbddirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else if (is_clone) {
        trace_printk("[EVAL] clonedirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else {
        trace_printk("[EVAL] nodirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
*/	

	return ret;
}

/*
 * Write blocks in the buffer zone of @zone.
 * If no buffer zone is assigned yet, get one.
 * Called with @zone write locked.
 */
static int dmz_handle_buffered_write(struct dmz_target *dmz,
				     struct dm_zone *zone, struct bio *bio,
				     sector_t chunk_block,
				     unsigned int nr_blocks,
			/*modi*/ unsigned int chunk_id)
{
	struct dmz_metadata *zmd = dmz->metadata;
	struct dm_zone *bzone;
	int ret, already_chunk = 0, c_off;
	unsigned int before_wp;
	ktime_t start, end;
    s64 actual_time;

	/* Get the buffer zone. One will be allocated if needed */
	start = ktime_get();
	trace_printk("[TEST] dmz_get_chunk_buffer start\n");
	bzone = dmz_get_chunk_buffer(zmd, zone, chunk_id, nr_blocks);
	trace_printk("[TEST] dmz_get_chunk_buffer end\n");
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
//	trace_printk("[BUFWRITE] dmz_get_chunk_buffer time %lld ns\n", (long long)actual_time);

	
	if (IS_ERR(bzone)) {
//		trace_printk("\t\t[DEBUG] dmz_handle_buffered_write IS_ERR(bzone)\n");
		return PTR_ERR(bzone);
	}

	if (dmz_is_readonly(bzone)) {
//		trace_printk("\t\t[DEBUG] dmz_handle_buffered_write dmz_is_readonly(bzone)\n");
		return -EROFS;
	}

	/* Submit write */
	unsigned int sect_len_log = bio->bi_iter.bi_size;
	uint64_t c_idx_log = dmz_bio_chunk(zmd, bio);
	uint64_t sect_n_log = bio->bi_iter.bi_sector;
	int is_ext4 = bio_flagged(bio, BIO_EXT4_FILE);
	int is_clone = bio_flagged(bio, BIO_CLONED);

	start = ktime_get();
	struct dm_chunk* c;
    list_for_each_entry(c, &(bzone->chunks), link) {
        if (c->id == chunk_id) { break; }
    }
    if (c->id != chunk_id) {
        trace_printk("[DEBUG7] dmz_handle_buffered_write zone %u c->id %u != chunk %u error\n", bzone->id, c->id, chunk_id);
    }
    c_off = c->offsets[chunk_block];
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
//	trace_printk("[BUFWRITE] find_chunk time %lld ns\n", (long long)actual_time);

	start = ktime_get();
	if (c_off != -1) {
    	already_chunk = 1;
        trace_printk("[SEQTEST] buf_overwrite chunk_id: %u zone_id: %u wp: %u start_block: %llu nr_blocks: %u\n",
                    chunk_id, bzone->id, c_off, chunk_block, nr_blocks);
        ret = dmz_submit_bio(dmz, bzone, bio, c_off, nr_blocks, chunk_id, before_wp);
		before_wp = c_off;
		trace_printk("[CDF] chunk %u nr_blocks %u\n", chunk_id, nr_blocks);
    }
    else {
		trace_printk("[CHUNKWEIGHT] isdata %d chunk %u nr_blocks %u\n", is_ext4, chunk_id, nr_blocks);

		spin_lock(&bzone->wp_lock);	
        dmz_set_cache_wp(zmd, bzone, nr_blocks);
		before_wp = bzone->wp_block;
		if (bzone->wp_block + nr_blocks >= zmd->zone_nr_blocks - 1) {
			bzone->wp_block = 0;
		}
		else {
			bzone->wp_block += nr_blocks;
		}
		spin_unlock(&bzone->wp_lock);
        trace_printk("[SEQTEST] buffered_write chunk_id: %u zone_id: %u wp: %u start_block: %llu nr_blocks: %u\n",
                    chunk_id, bzone->id, before_wp, chunk_block, nr_blocks);
        ret = dmz_submit_bio(dmz, bzone, bio, before_wp, nr_blocks, chunk_id, before_wp);
		trace_printk("[CDF] chunk %u nr_blocks %u\n", chunk_id, nr_blocks);
    }
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));

	if (ret) {
		return ret;
	}

	/*
	 * Validate the blocks in the buffer zone
	 * and invalidate in the data zone.
	 */
	start = ktime_get();
//	if (!already_chunk) {
		trace_printk("[DEBUGINV] dmz_handle_buffered_write dmz_validate_blocks start chunk_id: %u zone_id: %u\n", chunk_id, bzone->id);
		ret = dmz_validate_blocks(zmd, bzone, chunk_block, nr_blocks, chunk_id, before_wp);
		trace_printk("[DEBUGINV] dmz_handle_buffered_write dmz_validate_blocks end\n");
		if (ret == 0 && chunk_block < zone->wp_block) {
			trace_printk("[DEBUGINV] dmz_handle_buffered_write dmz_invalidate_blocks start zone_id: %u\n", zone->id);
			ret = dmz_invalidate_blocks(zmd, zone, chunk_block, nr_blocks);
			trace_printk("[DEBUGINV] dmz_handle_buffered_write dmz_invalidate_blocks end\n");
		}
//	}
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
//	trace_printk("[BUFWRITE] update_bitmap time %lld ns\n", (long long)actual_time);
/*	
	if (is_ext4) {
        trace_printk("[EVAL] ext4directwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else if (is_jrnl) {
        trace_printk("[EVAL] jbddirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else if (is_clone) {
        trace_printk("[EVAL] clonedirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
    else {
        trace_printk("[EVAL] nodirectwrite | Zone_ID: %u | Zone_Type: SEQ | Chunk_Number: %llu | Sector_Number: %llu | Sector_Len: %u\n",
                    zone->id, c_idx_log, sect_n_log, sect_len_log);
    }
*/	

	return ret;
}

/*
 * Process a write BIO.
 */
static int dmz_handle_write(struct dmz_target *dmz, struct dm_zone *zone,
			    struct bio *bio, /*modi*/ unsigned int chunk)
{
	struct dmz_metadata *zmd = dmz->metadata;
	sector_t chunk_block = dmz_chunk_block(zmd, dmz_bio_block(bio));
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	ktime_t start, end;
    s64 actual_time;

	if (!zone) {
		return -ENOSPC;
	}

	DMDEBUG("(%s): WRITE chunk %llu -> %s zone %u, block %llu, %u blocks",
		dmz_metadata_label(zmd),
		(unsigned long long)dmz_bio_chunk(zmd, bio),
		(dmz_is_rnd(zone) ? "RND" :
		 (dmz_is_cache(zone) ? "CACHE" : "SEQ")),
		zone->id,
		(unsigned long long)chunk_block, nr_blocks);

	if (dmz_is_rnd(zone) || dmz_is_cache(zone) ||
	    chunk_block == zone->wp_block) {
		/*
		 * zone is a random zone or it is a sequential zone
		 * and the BIO is aligned to the zone write pointer:
		 * direct write the zone.
		 */
		start = ktime_get();
		int ret = dmz_handle_direct_write(dmz, zone, bio, chunk_block, nr_blocks, chunk);
		end = ktime_get();
		actual_time = ktime_to_ns(ktime_sub(end, start));
//		trace_printk("[WRITETIME] direct write %lld ns \n", actual_time);
		return ret;
		/*
		return dmz_handle_direct_write(dmz, zone, bio,
					       chunk_block, nr_blocks, chunk);
	   */
	}

	/*
	 * This is an unaligned write in a sequential zone:
	 * use buffered write.
	 */
	start = ktime_get();
	int ret = dmz_handle_buffered_write(dmz, zone, bio, chunk_block, nr_blocks, chunk);
	end = ktime_get();
	actual_time = ktime_to_ns(ktime_sub(end, start));
	//trace_printk("[WRITETIME] buffered write %lld ns \n", actual_time);
	return ret;
//	return dmz_handle_buffered_write(dmz, zone, bio, chunk_block, nr_blocks, chunk);
}

/*
 * Process a discard BIO.
 */
static int dmz_handle_discard(struct dmz_target *dmz, struct dm_zone *zone,
			      struct bio *bio)
{
	struct dmz_metadata *zmd = dmz->metadata;
	sector_t block = dmz_bio_block(bio);
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	sector_t chunk_block = dmz_chunk_block(zmd, block);
	int ret = 0;

	/* For unmapped chunks, there is nothing to do */
	if (!zone)
		return 0;

	if (dmz_is_readonly(zone))
		return -EROFS;

	DMDEBUG("(%s): DISCARD chunk %llu -> zone %u, block %llu, %u blocks",
		dmz_metadata_label(dmz->metadata),
		(unsigned long long)dmz_bio_chunk(zmd, bio),
		zone->id,
		(unsigned long long)chunk_block, nr_blocks);

	/*
	 * Invalidate blocks in the data zone and its
	 * buffer zone if one is mapped.
	 */
	if (dmz_is_rnd(zone) || dmz_is_cache(zone) ||
	    chunk_block < zone->wp_block) {
		trace_printk("[DEBUGINV] dmz_handle_discard dmz_invalidate_blocks1 start zone_id: %u\n", zone->id);
		ret = dmz_invalidate_blocks(zmd, zone, chunk_block, nr_blocks);
		trace_printk("[DEBUGINV] dmz_handle_discard dmz_invalidate_blocks1 end\n");
	}
	if (ret == 0 && zone->bzone) {
		trace_printk("[DEBUGINV] dmz_handle_discard dmz_invalidate_blocks2 start zone_id: %u\n", zone->bzone->id);
		ret = dmz_invalidate_blocks(zmd, zone->bzone,
					    chunk_block, nr_blocks);
		trace_printk("[DEBUGINV] dmz_handle_discard dmz_invalidate_blocks2 end\n");
	}
	return ret;
}

/*
 * Process a BIO.
 */
static void dmz_handle_bio(struct dmz_target *dmz, struct dm_chunk_work *cw,
			   struct bio *bio)
{
	struct dmz_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct dmz_bioctx));
	struct dmz_metadata *zmd = dmz->metadata;
	struct dm_zone *zone;
	unsigned int chunk_id = dmz_bio_chunk(zmd, bio);
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	int ret;
	ktime_t start, end;
    s64 actual_time;


	/* chunk list modi */
	bioctx->zmd = zmd;
	bioctx->chunk_id = chunk_id;
	/* chunk list modi */

	dmz_lock_metadata(zmd);

	/*
	 * Get the data zone mapping the chunk. There may be no
	 * mapping for read and discard. If a mapping is obtained,
	 + the zone returned will be set to active state.
	 */
	zone = dmz_get_chunk_mapping(zmd, chunk_id,
				     bio_op(bio), nr_blocks);
	if (IS_ERR(zone)) {
		trace_printk("[BIOCHECK] IS_ERR(zone)\n");
		ret = PTR_ERR(zone);
		goto out;
	}

	/* Process the BIO */
	if (zone) {
		dmz_activate_zone(zone);
		/* chunk list modi */
		/*
	    struct dm_chunk *c;
	    list_for_each_entry(c, &zmd->mapped_chunk_list, map_link) {
	        if (c->id == chunk_id) { 
				dmz_activate_chunk(c);
				trace_printk("[DEBUGCREF] dmz_activate_chunk2 c %u refcount %u\n", c->id, atomic_read(&c->refcount));
				break; 
			}
	    }
		*/
	    /* chunk list modi */
		bioctx->zone = zone;
		dmz_reclaim_bio_acc(zone->dev->reclaim);
	}

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		start = ktime_get();
		ret = dmz_handle_read(dmz, zone, bio, chunk_id);
		end = ktime_get();
		actual_time = ktime_to_ns(ktime_sub(end, start));
		//trace_printk("[READTIME] read time %lld ns\n", (long long)actual_time);
		break;
	case REQ_OP_WRITE:
		start = ktime_get();
		//trace_printk("[BIOCHECK] dmz_handle_write chunk %u zone %u chunk_block %u nr_blocks %u\n", 
		//				chunk_id, zone->id, dmz_chunk_block(zmd, dmz_bio_block(bio)), nr_blocks);
		ret = dmz_handle_write(dmz, zone, bio, chunk_id);
		end = ktime_get();
		actual_time = ktime_to_ns(ktime_sub(end, start));
		//trace_printk("[WRITETIME] write time %lld ns\n", (long long)actual_time);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		ret = dmz_handle_discard(dmz, zone, bio);
		break;
	default:
		DMERR("(%s): Unsupported BIO operation 0x%x",
		      dmz_metadata_label(dmz->metadata), bio_op(bio));
		ret = -EIO;
	}

	/*
	 * Release the chunk mapping. This will check that the mapping
	 * is still valid, that is, that the zone used still has valid blocks.
	 */
	if (zone) {
		dmz_put_chunk_mapping(zmd, zone, chunk_id);
	}
out:
	/*
	if (!IS_ERR(zone)) {
		trace_printk("[TEST] dmz_handle_bio dmz_bio_endio zone %u chunk %u\n", zone->id, chunk_id);
	}
	*/
	dmz_bio_endio(bio, errno_to_blk_status(ret), chunk_id, 0);

	dmz_unlock_metadata(zmd);
}

/*
 * Increment a chunk reference counter.
 */
static inline void dmz_get_chunk_work(struct dm_chunk_work *cw)
{
	refcount_inc(&cw->refcount);
}

/*
 * Decrement a chunk work reference count and
 * free it if it becomes 0.
 */
static void dmz_put_chunk_work(struct dm_chunk_work *cw)
{
	if (refcount_dec_and_test(&cw->refcount)) {
		WARN_ON(!bio_list_empty(&cw->bio_list));
		radix_tree_delete(&cw->target->chunk_rxtree, cw->chunk);
		kfree(cw);
	}
}

/*
 * Chunk BIO work function.
 */
static void dmz_chunk_work(struct work_struct *work)
{
	struct dm_chunk_work *cw = container_of(work, struct dm_chunk_work, work);
	struct dmz_target *dmz = cw->target;
	struct bio *bio;

	mutex_lock(&dmz->chunk_lock);

	/* Process the chunk BIOs */
	while ((bio = bio_list_pop(&cw->bio_list))) {
		mutex_unlock(&dmz->chunk_lock);
		dmz_handle_bio(dmz, cw, bio);
		mutex_lock(&dmz->chunk_lock);
		dmz_put_chunk_work(cw);
	}

	/* Queueing the work incremented the work refcount */
	dmz_put_chunk_work(cw);

	mutex_unlock(&dmz->chunk_lock);
}

/*
 * Flush work.
 */
static void dmz_flush_work(struct work_struct *work)
{
	struct dmz_target *dmz = container_of(work, struct dmz_target, flush_work.work);
	struct bio *bio;
	int ret;

	/* Flush dirty metadata blocks */
	ret = dmz_flush_metadata(dmz->metadata);
	if (ret)
		DMDEBUG("(%s): Metadata flush failed, rc=%d",
			dmz_metadata_label(dmz->metadata), ret);

	/* Process queued flush requests */
	while (1) {
		spin_lock(&dmz->flush_lock);
		bio = bio_list_pop(&dmz->flush_list);
		spin_unlock(&dmz->flush_lock);

		if (!bio)
			break;
		//trace_printk("[BIOCHECK] dmz_flush_work nr_blocks %u \n", dmz_bio_blocks(bio));
		dmz_bio_endio(bio, errno_to_blk_status(ret), -2, 2);
	}

	queue_delayed_work(dmz->flush_wq, &dmz->flush_work, DMZ_FLUSH_PERIOD);
}

/*
 * Get a chunk work and start it to process a new BIO.
 * If the BIO chunk has no work yet, create one.
 */
static int dmz_queue_chunk_work(struct dmz_target *dmz, struct bio *bio)
{
	unsigned int chunk = dmz_bio_chunk(dmz->metadata, bio);
	struct dm_chunk_work *cw;
	int ret = 0;

	mutex_lock(&dmz->chunk_lock);

	/* Get the BIO chunk work. If one is not active yet, create one */
	cw = radix_tree_lookup(&dmz->chunk_rxtree, chunk);
	if (cw) {
		dmz_get_chunk_work(cw);
	} else {
		/* Create a new chunk work */
		cw = kmalloc(sizeof(struct dm_chunk_work), GFP_NOIO);
		if (unlikely(!cw)) {
			ret = -ENOMEM;
			goto out;
		}

		INIT_WORK(&cw->work, dmz_chunk_work);
		refcount_set(&cw->refcount, 1);
		cw->target = dmz;
		cw->chunk = chunk;
		bio_list_init(&cw->bio_list);

		ret = radix_tree_insert(&dmz->chunk_rxtree, chunk, cw);
		if (unlikely(ret)) {
			kfree(cw);
			goto out;
		}
	}

	bio_list_add(&cw->bio_list, bio);

	if (queue_work(dmz->chunk_wq, &cw->work))
		dmz_get_chunk_work(cw);
out:
	mutex_unlock(&dmz->chunk_lock);
	return ret;
}

/*
 * Check if the backing device is being removed. If it's on the way out,
 * start failing I/O. Reclaim and metadata components also call this
 * function to cleanly abort operation in the event of such failure.
 */
bool dmz_bdev_is_dying(struct dmz_dev *dmz_dev)
{
	if (dmz_dev->flags & DMZ_BDEV_DYING)
		return true;

	if (dmz_dev->flags & DMZ_CHECK_BDEV)
		return !dmz_check_bdev(dmz_dev);

	if (blk_queue_dying(bdev_get_queue(dmz_dev->bdev))) {
		dmz_dev_warn(dmz_dev, "Backing device queue dying");
		dmz_dev->flags |= DMZ_BDEV_DYING;
	}

	return dmz_dev->flags & DMZ_BDEV_DYING;
}

/*
 * Check the backing device availability. This detects such events as
 * backing device going offline due to errors, media removals, etc.
 * This check is less efficient than dmz_bdev_is_dying() and should
 * only be performed as a part of error handling.
 */
bool dmz_check_bdev(struct dmz_dev *dmz_dev)
{
	struct gendisk *disk;

	dmz_dev->flags &= ~DMZ_CHECK_BDEV;

	if (dmz_bdev_is_dying(dmz_dev))
		return false;

	disk = dmz_dev->bdev->bd_disk;
	if (disk->fops->check_events &&
	    disk->fops->check_events(disk, 0) & DISK_EVENT_MEDIA_CHANGE) {
		dmz_dev_warn(dmz_dev, "Backing device offline");
		dmz_dev->flags |= DMZ_BDEV_DYING;
	}

	return !(dmz_dev->flags & DMZ_BDEV_DYING);
}

/*
 * Process a new BIO.
 */
static int dmz_map(struct dm_target *ti, struct bio *bio)
{
	struct dmz_target *dmz = ti->private;
	struct dmz_metadata *zmd = dmz->metadata;
	struct dmz_bioctx *bioctx = dm_per_bio_data(bio, sizeof(struct dmz_bioctx));
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int nr_sectors = bio_sectors(bio);
	sector_t chunk_sector;
	int ret;

	if (dmz_dev_is_dying(zmd))
		return DM_MAPIO_KILL;

	DMDEBUG("(%s): BIO op %d sector %llu + %u => chunk %llu, block %llu, %u blocks",
		dmz_metadata_label(zmd),
		bio_op(bio), (unsigned long long)sector, nr_sectors,
		(unsigned long long)dmz_bio_chunk(zmd, bio),
		(unsigned long long)dmz_chunk_block(zmd, dmz_bio_block(bio)),
		(unsigned int)dmz_bio_blocks(bio));

	if (!nr_sectors && bio_op(bio) != REQ_OP_WRITE)
		return DM_MAPIO_REMAPPED;

	/* The BIO should be block aligned */
	if ((nr_sectors & DMZ_BLOCK_SECTORS_MASK) || (sector & DMZ_BLOCK_SECTORS_MASK))
		return DM_MAPIO_KILL;

	/* Initialize the BIO context */
	bioctx->dev = NULL;
	bioctx->zone = NULL;
	bioctx->bio = bio;
	refcount_set(&bioctx->ref, 1);

	/* Set the BIO pending in the flush list */
	if (!nr_sectors && bio_op(bio) == REQ_OP_WRITE) {
		spin_lock(&dmz->flush_lock);
		bio_list_add(&dmz->flush_list, bio);
		spin_unlock(&dmz->flush_lock);
		mod_delayed_work(dmz->flush_wq, &dmz->flush_work, 0);
		return DM_MAPIO_SUBMITTED;
	}

	/* Split zone BIOs to fit entirely into a zone */
	chunk_sector = sector & (dmz_zone_nr_sectors(zmd) - 1);
	if (chunk_sector + nr_sectors > dmz_zone_nr_sectors(zmd))
		dm_accept_partial_bio(bio, dmz_zone_nr_sectors(zmd) - chunk_sector);

	/* Now ready to handle this BIO */
	ret = dmz_queue_chunk_work(dmz, bio);
	if (ret) {
		DMDEBUG("(%s): BIO op %d, can't process chunk %llu, err %i",
			dmz_metadata_label(zmd),
			bio_op(bio), (u64)dmz_bio_chunk(zmd, bio),
			ret);
		return DM_MAPIO_REQUEUE;
	}

	return DM_MAPIO_SUBMITTED;
}

/*
 * Get zoned device information.
 */
static int dmz_get_zoned_device(struct dm_target *ti, char *path,
				int idx, int nr_devs)
{
	struct dmz_target *dmz = ti->private;
	struct dm_dev *ddev;
	struct dmz_dev *dev;
	int ret;
	struct block_device *bdev;

	/* Get the target device */
	ret = dm_get_device(ti, path, dm_table_get_mode(ti->table), &ddev);
	if (ret) {
		ti->error = "Get target device failed";
		return ret;
	}

	bdev = ddev->bdev;
	if (bdev_zoned_model(bdev) == BLK_ZONED_NONE) {
		if (nr_devs == 1) {
			ti->error = "Invalid regular device";
			goto err;
		}
		if (idx != 0) {
			ti->error = "First device must be a regular device";
			goto err;
		}
		if (dmz->ddev[0]) {
			ti->error = "Too many regular devices";
			goto err;
		}
		dev = &dmz->dev[idx];
		dev->flags = DMZ_BDEV_REGULAR;
	} else {
		if (dmz->ddev[idx]) {
			ti->error = "Too many zoned devices";
			goto err;
		}
		if (nr_devs > 1 && idx == 0) {
			ti->error = "First device must be a regular device";
			goto err;
		}
		dev = &dmz->dev[idx];
	}
	dev->bdev = bdev;
	dev->dev_idx = idx;
	(void)bdevname(dev->bdev, dev->name);

	dev->capacity = i_size_read(bdev->bd_inode) >> SECTOR_SHIFT;
	if (ti->begin) {
		ti->error = "Partial mapping is not supported";
		goto err;
	}

	dmz->ddev[idx] = ddev;

	return 0;
err:
	dm_put_device(ti, ddev);
	return -EINVAL;
}

/*
 * Cleanup zoned device information.
 */
static void dmz_put_zoned_devices(struct dm_target *ti)
{
	struct dmz_target *dmz = ti->private;
	int i;

	for (i = 0; i < dmz->nr_ddevs; i++)
		if (dmz->ddev[i])
			dm_put_device(ti, dmz->ddev[i]);

	kfree(dmz->ddev);
}

static int dmz_fixup_devices(struct dm_target *ti)
{
	struct dmz_target *dmz = ti->private;
	struct dmz_dev *reg_dev, *zoned_dev;
	struct request_queue *q;
	sector_t zone_nr_sectors = 0;
	int i;

	/*
	 * When we have more than on devices, the first one must be a
	 * regular block device and the others zoned block devices.
	 */
	if (dmz->nr_ddevs > 1) {
		reg_dev = &dmz->dev[0];
		if (!(reg_dev->flags & DMZ_BDEV_REGULAR)) {
			ti->error = "Primary disk is not a regular device";
			return -EINVAL;
		}
		for (i = 1; i < dmz->nr_ddevs; i++) {
			zoned_dev = &dmz->dev[i];
			if (zoned_dev->flags & DMZ_BDEV_REGULAR) {
				ti->error = "Secondary disk is not a zoned device";
				return -EINVAL;
			}
			q = bdev_get_queue(zoned_dev->bdev);
			if (zone_nr_sectors &&
			    zone_nr_sectors != blk_queue_zone_sectors(q)) {
				ti->error = "Zone nr sectors mismatch";
				return -EINVAL;
			}
			zone_nr_sectors = blk_queue_zone_sectors(q);
			zoned_dev->zone_nr_sectors = zone_nr_sectors;
			zoned_dev->nr_zones =
				blkdev_nr_zones(zoned_dev->bdev->bd_disk);
		}
	} else {
		reg_dev = NULL;
		zoned_dev = &dmz->dev[0];
		if (zoned_dev->flags & DMZ_BDEV_REGULAR) {
			ti->error = "Disk is not a zoned device";
			return -EINVAL;
		}
		q = bdev_get_queue(zoned_dev->bdev);
		zoned_dev->zone_nr_sectors = blk_queue_zone_sectors(q);
		zoned_dev->nr_zones = blkdev_nr_zones(zoned_dev->bdev->bd_disk);
	}

	if (reg_dev) {
		sector_t zone_offset;

		reg_dev->zone_nr_sectors = zone_nr_sectors;
		reg_dev->nr_zones =
			DIV_ROUND_UP_SECTOR_T(reg_dev->capacity,
					      reg_dev->zone_nr_sectors);
		reg_dev->zone_offset = 0;
		zone_offset = reg_dev->nr_zones;
		for (i = 1; i < dmz->nr_ddevs; i++) {
			dmz->dev[i].zone_offset = zone_offset;
			zone_offset += dmz->dev[i].nr_zones;
		}
	}
	return 0;
}

/*
 * Setup target.
 */
static int dmz_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dmz_target *dmz;
	int ret, i;

	/* Check arguments */
	if (argc < 1) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	/* Allocate and initialize the target descriptor */
	dmz = kzalloc(sizeof(struct dmz_target), GFP_KERNEL);
	if (!dmz) {
		ti->error = "Unable to allocate the zoned target descriptor";
		return -ENOMEM;
	}
	dmz->dev = kcalloc(argc, sizeof(struct dmz_dev), GFP_KERNEL);
	if (!dmz->dev) {
		ti->error = "Unable to allocate the zoned device descriptors";
		kfree(dmz);
		return -ENOMEM;
	}
	dmz->ddev = kcalloc(argc, sizeof(struct dm_dev *), GFP_KERNEL);
	if (!dmz->ddev) {
		ti->error = "Unable to allocate the dm device descriptors";
		ret = -ENOMEM;
		goto err;
	}
	dmz->nr_ddevs = argc;

	ti->private = dmz;

	/* Get the target zoned block device */
	for (i = 0; i < argc; i++) {
		ret = dmz_get_zoned_device(ti, argv[i], i, argc);
		if (ret)
			goto err_dev;
	}
	ret = dmz_fixup_devices(ti);
	if (ret)
		goto err_dev;

	/* Initialize metadata */
	ret = dmz_ctr_metadata(dmz->dev, argc, &dmz->metadata,
			       dm_table_device_name(ti->table));
	if (ret) {
		ti->error = "Metadata initialization failed";
		goto err_dev;
	}

	/* Set target (no write same support) */
	ti->max_io_len = dmz_zone_nr_sectors(dmz->metadata);
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_write_zeroes_bios = 1;
	ti->per_io_data_size = sizeof(struct dmz_bioctx);
	ti->flush_supported = true;
	ti->discards_supported = true;

	/* The exposed capacity is the number of chunks that can be mapped */
	ti->len = (sector_t)dmz_nr_chunks(dmz->metadata) <<
		dmz_zone_nr_sectors_shift(dmz->metadata);

	/* Zone BIO */
	ret = bioset_init(&dmz->bio_set, DMZ_MIN_BIOS, 0, 0);
	if (ret) {
		ti->error = "Create BIO set failed";
		goto err_meta;
	}

	/* Chunk BIO work */
	mutex_init(&dmz->chunk_lock);
	INIT_RADIX_TREE(&dmz->chunk_rxtree, GFP_NOIO);
	dmz->chunk_wq = alloc_workqueue("dmz_cwq_%s",
					WQ_MEM_RECLAIM | WQ_UNBOUND, 0,
					dmz_metadata_label(dmz->metadata));
	if (!dmz->chunk_wq) {
		ti->error = "Create chunk workqueue failed";
		ret = -ENOMEM;
		goto err_bio;
	}

	/* Flush work */
	spin_lock_init(&dmz->flush_lock);
	bio_list_init(&dmz->flush_list);
	INIT_DELAYED_WORK(&dmz->flush_work, dmz_flush_work);
	dmz->flush_wq = alloc_ordered_workqueue("dmz_fwq_%s", WQ_MEM_RECLAIM,
						dmz_metadata_label(dmz->metadata));
	if (!dmz->flush_wq) {
		ti->error = "Create flush workqueue failed";
		ret = -ENOMEM;
		goto err_cwq;
	}
	mod_delayed_work(dmz->flush_wq, &dmz->flush_work, DMZ_FLUSH_PERIOD);

	/* Initialize reclaim */
	for (i = 0; i < dmz->nr_ddevs; i++) {
		ret = dmz_ctr_reclaim(dmz->metadata, &dmz->dev[i].reclaim, i);
		if (ret) {
			ti->error = "Zone reclaim initialization failed";
			goto err_fwq;
		}
	}

	DMINFO("(%s): Target device: %llu 512-byte logical sectors (%llu blocks)",
	       dmz_metadata_label(dmz->metadata),
	       (unsigned long long)ti->len,
	       (unsigned long long)dmz_sect2blk(ti->len));

	return 0;
err_fwq:
	destroy_workqueue(dmz->flush_wq);
err_cwq:
	destroy_workqueue(dmz->chunk_wq);
err_bio:
	mutex_destroy(&dmz->chunk_lock);
	bioset_exit(&dmz->bio_set);
err_meta:
	dmz_dtr_metadata(dmz->metadata);
err_dev:
	dmz_put_zoned_devices(ti);
err:
	kfree(dmz->dev);
	kfree(dmz);

	return ret;
}

/*
 * Cleanup target.
 */
static void dmz_dtr(struct dm_target *ti)
{
	struct dmz_target *dmz = ti->private;
	int i;

	flush_workqueue(dmz->chunk_wq);
	destroy_workqueue(dmz->chunk_wq);

	for (i = 0; i < dmz->nr_ddevs; i++)
		dmz_dtr_reclaim(dmz->dev[i].reclaim);

	cancel_delayed_work_sync(&dmz->flush_work);
	destroy_workqueue(dmz->flush_wq);

	(void) dmz_flush_metadata(dmz->metadata);

	dmz_dtr_metadata(dmz->metadata);

	bioset_exit(&dmz->bio_set);

	dmz_put_zoned_devices(ti);

	mutex_destroy(&dmz->chunk_lock);

	kfree(dmz->dev);
	kfree(dmz);
}

/*
 * Setup target request queue limits.
 */
static void dmz_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dmz_target *dmz = ti->private;
	unsigned int chunk_sectors = dmz_zone_nr_sectors(dmz->metadata);

	limits->logical_block_size = DMZ_BLOCK_SIZE;
	limits->physical_block_size = DMZ_BLOCK_SIZE;

	blk_limits_io_min(limits, DMZ_BLOCK_SIZE);
	blk_limits_io_opt(limits, DMZ_BLOCK_SIZE);

	limits->discard_alignment = DMZ_BLOCK_SIZE;
	limits->discard_granularity = DMZ_BLOCK_SIZE;
	limits->max_discard_sectors = chunk_sectors;
	limits->max_hw_discard_sectors = chunk_sectors;
	limits->max_write_zeroes_sectors = chunk_sectors;

	/* FS hint to try to align to the device zone size */
	limits->chunk_sectors = chunk_sectors;
	limits->max_sectors = chunk_sectors;

	/* We are exposing a drive-managed zoned block device */
	limits->zoned = BLK_ZONED_NONE;
}

/*
 * Pass on ioctl to the backend device.
 */
static int dmz_prepare_ioctl(struct dm_target *ti, struct block_device **bdev)
{
	struct dmz_target *dmz = ti->private;
	struct dmz_dev *dev = &dmz->dev[0];

	if (!dmz_check_bdev(dev))
		return -EIO;

	*bdev = dev->bdev;

	return 0;
}

/*
 * Stop works on suspend.
 */
static void dmz_suspend(struct dm_target *ti)
{
	struct dmz_target *dmz = ti->private;
	int i;

	flush_workqueue(dmz->chunk_wq);
	for (i = 0; i < dmz->nr_ddevs; i++)
		dmz_suspend_reclaim(dmz->dev[i].reclaim);
	cancel_delayed_work_sync(&dmz->flush_work);
}

/*
 * Restart works on resume or if suspend failed.
 */
static void dmz_resume(struct dm_target *ti)
{
	struct dmz_target *dmz = ti->private;
	int i;

	queue_delayed_work(dmz->flush_wq, &dmz->flush_work, DMZ_FLUSH_PERIOD);
	for (i = 0; i < dmz->nr_ddevs; i++)
		dmz_resume_reclaim(dmz->dev[i].reclaim);
}

static int dmz_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn, void *data)
{
	struct dmz_target *dmz = ti->private;
	unsigned int zone_nr_sectors = dmz_zone_nr_sectors(dmz->metadata);
	sector_t capacity;
	int i, r;

	for (i = 0; i < dmz->nr_ddevs; i++) {
		capacity = dmz->dev[i].capacity & ~(zone_nr_sectors - 1);
		r = fn(ti, dmz->ddev[i], 0, capacity, data);
		if (r)
			break;
	}
	return r;
}

static void dmz_status(struct dm_target *ti, status_type_t type,
		       unsigned int status_flags, char *result,
		       unsigned int maxlen)
{
	struct dmz_target *dmz = ti->private;
	ssize_t sz = 0;
	char buf[BDEVNAME_SIZE];
	struct dmz_dev *dev;
	int i;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u zones %u/%u cache",
		       dmz_nr_zones(dmz->metadata),
		       dmz_nr_unmap_cache_zones(dmz->metadata),
		       dmz_nr_cache_zones(dmz->metadata));
		for (i = 0; i < dmz->nr_ddevs; i++) {
			/*
			 * For a multi-device setup the first device
			 * contains only cache zones.
			 */
			if ((i == 0) &&
			    (dmz_nr_cache_zones(dmz->metadata) > 0))
				continue;
			DMEMIT(" %u/%u random %u/%u sequential",
			       dmz_nr_unmap_rnd_zones(dmz->metadata, i),
			       dmz_nr_rnd_zones(dmz->metadata, i),
			       dmz_nr_unmap_seq_zones(dmz->metadata, i),
			       dmz_nr_seq_zones(dmz->metadata, i));
		}
		break;
	case STATUSTYPE_TABLE:
		dev = &dmz->dev[0];
		format_dev_t(buf, dev->bdev->bd_dev);
		DMEMIT("%s", buf);
		for (i = 1; i < dmz->nr_ddevs; i++) {
			dev = &dmz->dev[i];
			format_dev_t(buf, dev->bdev->bd_dev);
			DMEMIT(" %s", buf);
		}
		break;
	case STATUSTYPE_IMA:
		*result = '\0';
		break;
	}
	return;
}

static int dmz_message(struct dm_target *ti, unsigned int argc, char **argv,
		       char *result, unsigned int maxlen)
{
	struct dmz_target *dmz = ti->private;
	int r = -EINVAL;

	if (!strcasecmp(argv[0], "reclaim")) {
		int i;

		for (i = 0; i < dmz->nr_ddevs; i++)
			dmz_schedule_reclaim(dmz->dev[i].reclaim);
		r = 0;
	} else
		DMERR("unrecognized message %s", argv[0]);
	return r;
}

static struct target_type dmz_type = {
	.name		 = "zoned",
	.version	 = {2, 0, 0},
	.features	 = DM_TARGET_SINGLETON | DM_TARGET_MIXED_ZONED_MODEL,
	.module		 = THIS_MODULE,
	.ctr		 = dmz_ctr,
	.dtr		 = dmz_dtr,
	.map		 = dmz_map,
	.io_hints	 = dmz_io_hints,
	.prepare_ioctl	 = dmz_prepare_ioctl,
	.postsuspend	 = dmz_suspend,
	.resume		 = dmz_resume,
	.iterate_devices = dmz_iterate_devices,
	.status		 = dmz_status,
	.message	 = dmz_message,
};

static int __init dmz_init(void)
{
	return dm_register_target(&dmz_type);
}

static void __exit dmz_exit(void)
{
	dm_unregister_target(&dmz_type);
}

module_init(dmz_init);
module_exit(dmz_exit);

MODULE_DESCRIPTION(DM_NAME " target for zoned block devices");
MODULE_AUTHOR("Damien Le Moal <damien.lemoal@wdc.com>");
MODULE_LICENSE("GPL");
