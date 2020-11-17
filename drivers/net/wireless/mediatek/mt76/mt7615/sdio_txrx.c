// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 MediaTek Inc.
 *
 * Author: Felix Fietkau <nbd@nbd.name>
 *	   Lorenzo Bianconi <lorenzo@kernel.org>
 *	   Sean Wang <sean.wang@mediatek.com>
 */

#include <linux/kernel.h>
#include <linux/iopoll.h>
#include <linux/module.h>

#include <linux/mmc/host.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio_func.h>

#include "../trace.h"
#include "mt7615.h"
#include "sdio.h"
#include "mac.h"

static void mt7663s_refill_sched_quota(struct mt7615_dev *dev, u32 *data)
{
	struct mt76_sdio *sdio = &dev->mt76.sdio;

	mutex_lock(&sdio->sched.lock);
	sdio->sched.pse_data_quota += FIELD_GET(TXQ_CNT_L, data[0]) + /* BK */
				      FIELD_GET(TXQ_CNT_H, data[0]) + /* BE */
				      FIELD_GET(TXQ_CNT_L, data[1]) + /* VI */
				      FIELD_GET(TXQ_CNT_H, data[1]);  /* VO */
	sdio->sched.ple_data_quota += FIELD_GET(TXQ_CNT_H, data[2]) + /* BK */
				      FIELD_GET(TXQ_CNT_L, data[3]) + /* BE */
				      FIELD_GET(TXQ_CNT_H, data[3]) + /* VI */
				      FIELD_GET(TXQ_CNT_L, data[4]);  /* VO */
	sdio->sched.pse_mcu_quota += FIELD_GET(TXQ_CNT_L, data[2]);
	mutex_unlock(&sdio->sched.lock);
}

static struct sk_buff *mt7663s_build_rx_skb(void *data, int data_len,
					    int buf_len)
{
	int len = min_t(int, data_len, MT_SKB_HEAD_LEN);
	struct sk_buff *skb;

	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return NULL;

	skb_put_data(skb, data, len);
	if (data_len > len) {
		struct page *page;

		data += len;
		page = virt_to_head_page(data);
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags,
				page, data - page_address(page),
				data_len - len, buf_len);
		get_page(page);
	}

	return skb;
}

static int mt7663s_rx_run_queue(struct mt7615_dev *dev, enum mt76_rxq_id qid,
				struct mt76s_intr *intr)
{
	struct mt76_queue *q = &dev->mt76.q_rx[qid];
	struct mt76_sdio *sdio = &dev->mt76.sdio;
	int len = 0, err, i, order;
	struct page *page;
	u8 *buf;

	for (i = 0; i < intr->rx.num[qid]; i++)
		len += round_up(intr->rx.len[qid][i] + 4, 4);

	if (!len)
		return 0;

	if (len > sdio->func->cur_blksize)
		len = roundup(len, sdio->func->cur_blksize);

	order = get_order(len);
	page = __dev_alloc_pages(GFP_KERNEL, order);
	if (!page)
		return -ENOMEM;

	buf = page_address(page);

	err = sdio_readsb(sdio->func, buf, MCR_WRDR(qid), len);
	if (err < 0) {
		dev_err(dev->mt76.dev, "sdio read data failed:%d\n", err);
		__free_pages(page, order);
		return err;
	}

	for (i = 0; i < intr->rx.num[qid]; i++) {
		int index = (q->tail + i) % q->ndesc;
		struct mt76_queue_entry *e = &q->entry[index];

		len = intr->rx.len[qid][i];
		e->skb = mt7663s_build_rx_skb(buf, len, round_up(len + 4, 4));
		if (!e->skb)
			break;

		buf += round_up(len + 4, 4);
		if (q->queued + i + 1 == q->ndesc)
			break;
	}
	__free_pages(page, order);

	spin_lock_bh(&q->lock);
	q->tail = (q->tail + i) % q->ndesc;
	q->queued += i;
	spin_unlock_bh(&q->lock);

	return err;
}

static int mt7663s_tx_update_sched(struct mt7615_dev *dev,
				   struct mt76_queue_entry *e,
				   bool mcu)
{
	struct mt76_sdio *sdio = &dev->mt76.sdio;
	struct mt76_phy *mphy = &dev->mt76.phy;
	struct ieee80211_hdr *hdr;
	int size, ret = -EBUSY;

	size = DIV_ROUND_UP(e->buf_sz + sdio->sched.deficit, MT_PSE_PAGE_SZ);

	if (mcu) {
		if (!test_bit(MT76_STATE_MCU_RUNNING, &mphy->state))
			return 0;

		mutex_lock(&sdio->sched.lock);
		if (sdio->sched.pse_mcu_quota > size) {
			sdio->sched.pse_mcu_quota -= size;
			ret = 0;
		}
		mutex_unlock(&sdio->sched.lock);

		return ret;
	}

	hdr = (struct ieee80211_hdr *)(e->skb->data + MT_USB_TXD_SIZE);
	if (ieee80211_is_ctl(hdr->frame_control))
		return 0;

	mutex_lock(&sdio->sched.lock);
	if (sdio->sched.pse_data_quota > size &&
	    sdio->sched.ple_data_quota > 0) {
		sdio->sched.pse_data_quota -= size;
		sdio->sched.ple_data_quota--;
		ret = 0;
	}
	mutex_unlock(&sdio->sched.lock);

	return ret;
}

static int mt7663s_tx_run_queue(struct mt7615_dev *dev, struct mt76_queue *q)
{
	bool mcu = q == dev->mt76.q_tx[MT_TXQ_MCU].q;
	struct mt76_sdio *sdio = &dev->mt76.sdio;
	int nframes = 0;

	while (q->first != q->tail) {
		struct mt76_queue_entry *e = &q->entry[q->first];
		int err, len = e->skb->len;

		if (mt7663s_tx_update_sched(dev, e, mcu))
			break;

		if (len > sdio->func->cur_blksize)
			len = roundup(len, sdio->func->cur_blksize);

		/* TODO: skb_walk_frags and then write to SDIO port */
		err = sdio_writesb(sdio->func, MCR_WTDR1, e->skb->data, len);
		if (err) {
			dev_err(dev->mt76.dev, "sdio write failed: %d\n", err);
			return -EIO;
		}

		e->done = true;
		q->first = (q->first + 1) % q->ndesc;
		nframes++;
	}

	return nframes;
}

static int mt7663s_tx_run_queues(struct mt7615_dev *dev)
{
	int i, nframes = 0;

	for (i = 0; i < MT_TXQ_MCU_WA; i++) {
		int ret;

		ret = mt7663s_tx_run_queue(dev, dev->mt76.q_tx[i].q);
		if (ret < 0)
			return ret;

		nframes += ret;
	}

	return nframes;
}

int mt7663s_kthread_run(void *data)
{
	struct mt7615_dev *dev = data;
	struct mt76_phy *mphy = &dev->mt76.phy;

	while (!kthread_should_stop()) {
		int ret;

		cond_resched();

		sdio_claim_host(dev->mt76.sdio.func);
		ret = mt7663s_tx_run_queues(dev);
		sdio_release_host(dev->mt76.sdio.func);

		if (ret <= 0 || !test_bit(MT76_STATE_RUNNING, &mphy->state)) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		} else {
			wake_up_process(dev->mt76.sdio.kthread);
		}
	}

	return 0;
}

void mt7663s_sdio_irq(struct sdio_func *func)
{
	struct mt7615_dev *dev = sdio_get_drvdata(func);
	struct mt76_sdio *sdio = &dev->mt76.sdio;
	struct mt76s_intr intr;

	/* disable interrupt */
	sdio_writel(func, WHLPCR_INT_EN_CLR, MCR_WHLPCR, 0);

	do {
		sdio_readsb(func, &intr, MCR_WHISR, sizeof(struct mt76s_intr));
		trace_dev_irq(&dev->mt76, intr.isr, 0);

		if (!test_bit(MT76_STATE_INITIALIZED, &dev->mt76.phy.state))
			goto out;

		if (intr.isr & WHIER_RX0_DONE_INT_EN) {
			mt7663s_rx_run_queue(dev, 0, &intr);
			wake_up_process(sdio->kthread);
		}

		if (intr.isr & WHIER_RX1_DONE_INT_EN) {
			mt7663s_rx_run_queue(dev, 1, &intr);
			wake_up_process(sdio->kthread);
		}

		if (intr.isr & WHIER_TX_DONE_INT_EN) {
			mt7663s_refill_sched_quota(dev, intr.tx.wtqcr);
			mt7663s_tx_run_queues(dev);
			wake_up_process(sdio->kthread);
		}
	} while (intr.isr);
out:
	/* enable interrupt */
	sdio_writel(func, WHLPCR_INT_EN_SET, MCR_WHLPCR, 0);
}
