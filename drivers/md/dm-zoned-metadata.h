#ifndef _DM_ZONED_METADATA_H
#define _DM_ZONED_METADATA_H

#include "dm-zoned.h"
#include <linux/module.h>
#include <linux/crc32.h>
#include <linux/sched/mm.h>

struct dmz_super {
    /* Magic number */
    __le32      magic;          /*   4 */

    /* Metadata version number */
    __le32      version;        /*   8 */

    /* Generation number */
    __le64      gen;            /*  16 */

    /* This block number */
    __le64      sb_block;       /*  24 */

    /* The number of metadata blocks, including this super block */
    __le32      nr_meta_blocks;     /*  28 */

    /* The number of sequential zones reserved for reclaim */
    __le32      nr_reserved_seq;    /*  32 */

    /* The number of entries in the mapping table */
    __le32      nr_chunks;      /*  36 */

    /* The number of blocks used for the chunk mapping table */
    __le32      nr_map_blocks;      /*  40 */

    /* The number of blocks used for the block bitmaps */
    __le32      nr_bitmap_blocks;   /*  44 */

    /* Checksum */
    __le32      crc;            /*  48 */

    /* DM-Zoned label */
    u8      dmz_label[32];      /*  80 */

    /* DM-Zoned UUID */
    u8      dmz_uuid[16];       /*  96 */

    /* Device UUID */
    u8      dev_uuid[16];       /* 112 */

    /* Padding to full 512B sector */
  
	u8      reserved[400];      /* 512 */
};

struct dmz_mblock {
    struct rb_node      node;
    struct list_head    link;
    sector_t        no;
    unsigned int        ref;
    unsigned long       state;
    struct page     *page;
    void            *data;
};

struct dmz_sb {
    sector_t        block;
    struct dmz_dev      *dev;
    struct dmz_mblock   *mblk;
    struct dmz_super    *sb;
    struct dm_zone      *zone;
};

struct dmz_metadata {
    struct dmz_dev      *dev;
    unsigned int        nr_devs;

    char            devname[BDEVNAME_SIZE];
    char            label[BDEVNAME_SIZE];
    uuid_t          uuid;

    sector_t        zone_bitmap_size;
    unsigned int        zone_nr_bitmap_blocks;
    unsigned int        zone_bits_per_mblk;

    sector_t        zone_nr_blocks;
    sector_t        zone_nr_blocks_shift;

    sector_t        zone_nr_sectors;
    sector_t        zone_nr_sectors_shift;

    unsigned int        nr_bitmap_blocks;
    unsigned int        nr_map_blocks;

    unsigned int        nr_zones;
    unsigned int        nr_useable_zones;
    unsigned int        nr_meta_blocks;
    unsigned int        nr_meta_zones;
    unsigned int        nr_data_zones;
    unsigned int        nr_cache_zones;
    unsigned int        nr_rnd_zones;
    unsigned int        nr_reserved_seq;
    unsigned int        nr_chunks;

    // Zone information array
    struct xarray       zones;

    struct dmz_sb       sb[2];
    unsigned int        mblk_primary;
    unsigned int        sb_version;
    u64         sb_gen;
    unsigned int        min_nr_mblks;
    unsigned int        max_nr_mblks;
    atomic_t        nr_mblks;
    struct rw_semaphore mblk_sem;
    struct mutex        mblk_flush_lock;
    spinlock_t      mblk_lock;
    struct rb_root      mblk_rbtree;
    struct list_head    mblk_lru_list;
    struct list_head    mblk_dirty_list;
    struct shrinker     mblk_shrinker;

    // Zone allocation management
    struct mutex        map_lock;
    struct dmz_mblock   **map_mblk;

    unsigned int        nr_cache;
    atomic_t        unmap_nr_cache;
    struct list_head    unmap_cache_list;
    struct list_head    map_cache_list;
    // chunk list modi
    struct list_head    mapped_chunk_list;
    // chunk list modi

    atomic_t        nr_reserved_seq_zones;
    struct list_head    reserved_seq_zones_list;

    wait_queue_head_t   free_wq;
};

#endif
