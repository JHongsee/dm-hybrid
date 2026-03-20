#ifndef _DM_ZONED_RECLAIM_H
#define _DM_ZONED_RECLAIM_H

struct dmz_reclaim {
    struct dmz_metadata     *metadata;

    struct delayed_work work;
    struct workqueue_struct *wq;

    struct dm_kcopyd_client *kc;
    struct dm_kcopyd_throttle kc_throttle;
    int         kc_err;

    int         dev_idx;

    unsigned long       flags;

    //Last target access time
    unsigned long       atime;

	/*modi*/
	atomic_t			active_reclaim;

	int					is_zone_reclaim;

	struct dm_zone		*recl_zone;
	struct dm_chunk		*recl_chunk;
};

#endif
