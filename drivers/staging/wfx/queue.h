/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * O(1) TX queue with built-in allocator.
 *
 * Copyright (c) 2017-2018, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#ifndef WFX_QUEUE_H
#define WFX_QUEUE_H

#include <linux/skbuff.h>

#include "hif_api_cmd.h"

#define WFX_MAX_STA_IN_AP_MODE    14
#define WFX_LINK_ID_AFTER_DTIM    (WFX_MAX_STA_IN_AP_MODE + 1)
#define WFX_LINK_ID_UAPSD         (WFX_MAX_STA_IN_AP_MODE + 2)
#define WFX_LINK_ID_MAX           (WFX_MAX_STA_IN_AP_MODE + 3)

struct wfx_dev;
struct wfx_vif;

struct wfx_queue {
	struct sk_buff_head	queue;
	int			tx_locked_cnt;
	int			link_map_cache[WFX_LINK_ID_MAX];
	u8			queue_id;
};

struct wfx_queue_stats {
	int			link_map_cache[WFX_LINK_ID_MAX];
	struct sk_buff_head	pending;
	wait_queue_head_t	wait_link_id_empty;
};

void wfx_tx_lock(struct wfx_dev *wdev);
void wfx_tx_unlock(struct wfx_dev *wdev);
void wfx_tx_flush(struct wfx_dev *wdev);
void wfx_tx_lock_flush(struct wfx_dev *wdev);

void wfx_tx_queues_init(struct wfx_dev *wdev);
void wfx_tx_queues_deinit(struct wfx_dev *wdev);
void wfx_tx_queues_lock(struct wfx_dev *wdev);
void wfx_tx_queues_unlock(struct wfx_dev *wdev);
void wfx_tx_queues_clear(struct wfx_dev *wdev);
bool wfx_tx_queues_is_empty(struct wfx_dev *wdev);
void wfx_tx_queues_wait_empty_vif(struct wfx_vif *wvif);
struct hif_msg *wfx_tx_queues_get(struct wfx_dev *wdev);

void wfx_tx_queue_put(struct wfx_dev *wdev, struct wfx_queue *queue,
		      struct sk_buff *skb);
size_t wfx_tx_queue_get_num_queued(struct wfx_queue *queue, u32 link_id_map);

struct sk_buff *wfx_pending_get(struct wfx_dev *wdev, u32 packet_id);
int wfx_pending_remove(struct wfx_dev *wdev, struct sk_buff *skb);
int wfx_pending_requeue(struct wfx_dev *wdev, struct sk_buff *skb);
unsigned int wfx_pending_get_pkt_us_delay(struct wfx_dev *wdev,
					  struct sk_buff *skb);
void wfx_pending_dump_old_frames(struct wfx_dev *wdev, unsigned int limit_ms);

#endif /* WFX_QUEUE_H */
