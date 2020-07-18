// SPDX-License-Identifier: ISC
/* Copyright (C) 2019 MediaTek Inc.
 *
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *         Roy Luo <royluo@google.com>
 *         Lorenzo Bianconi <lorenzo@kernel.org>
 *         Felix Fietkau <nbd@nbd.name>
 */

#include "mt7615.h"
#include "../dma.h"
#include "mac.h"

static int
mt7615_init_tx_queue(struct mt7615_dev *dev, struct mt76_sw_queue *q,
		      int idx, int n_desc)
{
	struct mt76_queue *hwq;
	int err;

	hwq = devm_kzalloc(dev->mt76.dev, sizeof(*hwq), GFP_KERNEL);
	if (!hwq)
		return -ENOMEM;

	err = mt76_queue_alloc(dev, hwq, idx, n_desc, 0, MT_TX_RING_BASE);
	if (err < 0)
		return err;

	INIT_LIST_HEAD(&q->swq);
	q->q = hwq;

	return 0;
}

static int
mt7622_init_tx_queues_multi(struct mt7615_dev *dev)
{
	static const u8 wmm_queue_map[] = {
		MT7622_TXQ_AC0,
		MT7622_TXQ_AC1,
		MT7622_TXQ_AC2,
		MT7622_TXQ_AC3,
	};
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(wmm_queue_map); i++) {
		ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[i],
					   wmm_queue_map[i],
					   MT7615_TX_RING_SIZE / 2);
		if (ret)
			return ret;
	}

	ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_PSD],
				   MT7622_TXQ_MGMT, MT7615_TX_MGMT_RING_SIZE);
	if (ret)
		return ret;

	ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_MCU],
				   MT7622_TXQ_MCU, MT7615_TX_MCU_RING_SIZE);
	return ret;
}

static int
mt7615_init_tx_queues(struct mt7615_dev *dev)
{
	struct mt76_sw_queue *q;
	int ret, i;

	ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_FWDL],
				   MT7615_TXQ_FWDL,
				   MT7615_TX_FWDL_RING_SIZE);
	if (ret)
		return ret;

	if (!is_mt7615(&dev->mt76))
		return mt7622_init_tx_queues_multi(dev);

	ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[0], 0,
				   MT7615_TX_RING_SIZE);
	if (ret)
		return ret;

	for (i = 1; i < MT_TXQ_MCU; i++) {
		q = &dev->mt76.q_tx[i];
		INIT_LIST_HEAD(&q->swq);
		q->q = dev->mt76.q_tx[0].q;
	}

	ret = mt7615_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_MCU],
				   MT7615_TXQ_MCU,
				   MT7615_TX_MCU_RING_SIZE);
	return 0;
}

void mt7615_queue_rx_skb(struct mt76_dev *mdev, enum mt76_rxq_id q,
			 struct sk_buff *skb)
{
	struct mt7615_dev *dev = container_of(mdev, struct mt7615_dev, mt76);
	__le32 *rxd = (__le32 *)skb->data;
	__le32 *end = (__le32 *)&skb->data[skb->len];
	enum rx_pkt_type type;
	u16 flag;

	type = FIELD_GET(MT_RXD0_PKT_TYPE, le32_to_cpu(rxd[0]));
	flag = FIELD_GET(MT_RXD0_PKT_FLAG, le32_to_cpu(rxd[0]));
	if (type == PKT_TYPE_RX_EVENT && flag == 0x1)
		type = PKT_TYPE_NORMAL_MCU;

	switch (type) {
	case PKT_TYPE_TXS:
		for (rxd++; rxd + 7 <= end; rxd += 7)
			mt7615_mac_add_txs(dev, rxd);
		dev_kfree_skb(skb);
		break;
	case PKT_TYPE_TXRX_NOTIFY:
		mt7615_mac_tx_free(dev, skb);
		break;
	case PKT_TYPE_RX_EVENT:
		mt7615_mcu_rx_event(dev, skb);
		break;
	case PKT_TYPE_NORMAL_MCU:
	case PKT_TYPE_NORMAL:
		if (!mt7615_mac_fill_rx(dev, skb)) {
			mt76_rx(&dev->mt76, q, skb);
			return;
		}
		/* fall through */
	default:
		dev_kfree_skb(skb);
		break;
	}
}

static void
mt7615_tx_cleanup(struct mt7615_dev *dev)
{
	int i;

	mt76_queue_tx_cleanup(dev, MT_TXQ_MCU, false);
	if (is_mt7615(&dev->mt76)) {
		mt76_queue_tx_cleanup(dev, MT_TXQ_BE, false);
	} else {
		for (i = 0; i < IEEE80211_NUM_ACS; i++)
			mt76_queue_tx_cleanup(dev, i, false);
	}
}

static int mt7615_poll_tx(struct napi_struct *napi, int budget)
{
	struct mt7615_dev *dev;

	dev = container_of(napi, struct mt7615_dev, mt76.tx_napi);

	mt7615_tx_cleanup(dev);

	if (napi_complete_done(napi, 0))
		mt7615_irq_enable(dev, MT_INT_TX_DONE_ALL);

	mt7615_tx_cleanup(dev);

	mt7615_mac_sta_poll(dev);

	tasklet_schedule(&dev->mt76.tx_tasklet);

	return 0;
}

static void mt7622_dma_sched_init(struct mt7615_dev *dev)
{
	u32 reg = mt7615_reg_map(dev, MT_DMASHDL_BASE);
	int i;

	mt76_rmw(dev, reg + MT_DMASHDL_PKT_MAX_SIZE,
		 MT_DMASHDL_PKT_MAX_SIZE_PLE | MT_DMASHDL_PKT_MAX_SIZE_PSE,
		 FIELD_PREP(MT_DMASHDL_PKT_MAX_SIZE_PLE, 1) |
		 FIELD_PREP(MT_DMASHDL_PKT_MAX_SIZE_PSE, 8));

	for (i = 0; i <= 5; i++)
		mt76_wr(dev, reg + MT_DMASHDL_GROUP_QUOTA(i),
			FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MIN, 0x10) |
			FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MAX, 0x800));

	mt76_wr(dev, reg + MT_DMASHDL_Q_MAP(0), 0x42104210);
	mt76_wr(dev, reg + MT_DMASHDL_Q_MAP(1), 0x42104210);
	mt76_wr(dev, reg + MT_DMASHDL_Q_MAP(2), 0x5);
	mt76_wr(dev, reg + MT_DMASHDL_Q_MAP(3), 0);

	mt76_wr(dev, reg + MT_DMASHDL_SCHED_SET0, 0x6012345f);
	mt76_wr(dev, reg + MT_DMASHDL_SCHED_SET1, 0xedcba987);
}

static void mt7663_dma_sched_init(struct mt7615_dev *dev)
{
	int i;

	mt76_rmw(dev, MT_DMA_SHDL(MT_DMASHDL_PKT_MAX_SIZE),
		 MT_DMASHDL_PKT_MAX_SIZE_PLE | MT_DMASHDL_PKT_MAX_SIZE_PSE,
		 FIELD_PREP(MT_DMASHDL_PKT_MAX_SIZE_PLE, 1) |
		 FIELD_PREP(MT_DMASHDL_PKT_MAX_SIZE_PSE, 8));

	/* enable refill control group 0, 1, 2, 4, 5 */
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_REFILL), 0xffc80000);
	/* enable group 0, 1, 2, 4, 5, 15 */
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_OPTIONAL), 0x70068037);

	/* each group min quota must larger then PLE_PKT_MAX_SIZE_NUM */
	for (i = 0; i < 5; i++)
		mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_GROUP_QUOTA(i)),
			FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MIN, 0x40) |
			FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MAX, 0x800));
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_GROUP_QUOTA(5)),
		FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MIN, 0x40) |
		FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MAX, 0x40));
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_GROUP_QUOTA(15)),
		FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MIN, 0x20) |
		FIELD_PREP(MT_DMASHDL_GROUP_QUOTA_MAX, 0x20));

	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_Q_MAP(0)), 0x42104210);
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_Q_MAP(1)), 0x42104210);
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_Q_MAP(2)), 0x00050005);
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_Q_MAP(3)), 0);
	/* ALTX0 and ALTX1 QID mapping to group 5 */
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_SCHED_SET0), 0x6012345f);
	mt76_wr(dev, MT_DMA_SHDL(MT_DMASHDL_SCHED_SET1), 0xedcba987);
}

int mt7615_dma_init(struct mt7615_dev *dev)
{
	int rx_ring_size = MT7615_RX_RING_SIZE;
	int ret;

	mt76_dma_attach(&dev->mt76);

	mt76_wr(dev, MT_WPDMA_GLO_CFG,
		MT_WPDMA_GLO_CFG_TX_WRITEBACK_DONE |
		MT_WPDMA_GLO_CFG_FIFO_LITTLE_ENDIAN |
		MT_WPDMA_GLO_CFG_OMIT_TX_INFO);

	mt76_rmw_field(dev, MT_WPDMA_GLO_CFG,
		       MT_WPDMA_GLO_CFG_TX_BT_SIZE_BIT0, 0x1);

	mt76_rmw_field(dev, MT_WPDMA_GLO_CFG,
		       MT_WPDMA_GLO_CFG_TX_BT_SIZE_BIT21, 0x1);

	mt76_rmw_field(dev, MT_WPDMA_GLO_CFG,
		       MT_WPDMA_GLO_CFG_DMA_BURST_SIZE, 0x3);

	mt76_rmw_field(dev, MT_WPDMA_GLO_CFG,
		       MT_WPDMA_GLO_CFG_MULTI_DMA_EN, 0x3);

	if (is_mt7615(&dev->mt76)) {
		mt76_set(dev, MT_WPDMA_GLO_CFG,
			 MT_WPDMA_GLO_CFG_FIRST_TOKEN_ONLY);

		mt76_wr(dev, MT_WPDMA_GLO_CFG1, 0x1);
		mt76_wr(dev, MT_WPDMA_TX_PRE_CFG, 0xf0000);
		mt76_wr(dev, MT_WPDMA_RX_PRE_CFG, 0xf7f0000);
		mt76_wr(dev, MT_WPDMA_ABT_CFG, 0x4000026);
		mt76_wr(dev, MT_WPDMA_ABT_CFG1, 0x18811881);
		mt76_set(dev, 0x7158, BIT(16));
		mt76_clear(dev, 0x7000, BIT(23));
	}

	mt76_wr(dev, MT_WPDMA_RST_IDX, ~0);

	ret = mt7615_init_tx_queues(dev);
	if (ret)
		return ret;

	/* init rx queues */
	ret = mt76_queue_alloc(dev, &dev->mt76.q_rx[MT_RXQ_MCU], 1,
			       MT7615_RX_MCU_RING_SIZE, MT_RX_BUF_SIZE,
			       MT_RX_RING_BASE);
	if (ret)
		return ret;

	if (!is_mt7615(&dev->mt76))
	    rx_ring_size /= 2;

	ret = mt76_queue_alloc(dev, &dev->mt76.q_rx[MT_RXQ_MAIN], 0,
			       rx_ring_size, MT_RX_BUF_SIZE, MT_RX_RING_BASE);
	if (ret)
		return ret;

	mt76_wr(dev, MT_DELAY_INT_CFG, 0);

	ret = mt76_init_queues(dev);
	if (ret < 0)
		return ret;

	netif_tx_napi_add(&dev->mt76.napi_dev, &dev->mt76.tx_napi,
			  mt7615_poll_tx, NAPI_POLL_WEIGHT);
	napi_enable(&dev->mt76.tx_napi);

	mt76_poll(dev, MT_WPDMA_GLO_CFG,
		  MT_WPDMA_GLO_CFG_TX_DMA_BUSY |
		  MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 1000);

	/* start dma engine */
	mt76_set(dev, MT_WPDMA_GLO_CFG,
		 MT_WPDMA_GLO_CFG_TX_DMA_EN |
		 MT_WPDMA_GLO_CFG_RX_DMA_EN);

	/* enable interrupts for TX/RX rings */
	mt7615_irq_enable(dev, MT_INT_RX_DONE_ALL | MT_INT_TX_DONE_ALL |
			       MT_INT_MCU_CMD);

	if (is_mt7622(&dev->mt76))
		mt7622_dma_sched_init(dev);

	if (is_mt7663(&dev->mt76))
		mt7663_dma_sched_init(dev);

	return 0;
}

void mt7615_dma_cleanup(struct mt7615_dev *dev)
{
	mt76_clear(dev, MT_WPDMA_GLO_CFG,
		   MT_WPDMA_GLO_CFG_TX_DMA_EN |
		   MT_WPDMA_GLO_CFG_RX_DMA_EN);
	mt76_set(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_SW_RESET);

	tasklet_kill(&dev->mt76.tx_tasklet);
	mt76_dma_cleanup(&dev->mt76);
}
