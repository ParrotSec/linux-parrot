// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2021-2022 Intel Corporation. All rights reserved.
//
// Authors: Cezary Rojewski <cezary.rojewski@intel.com>
//          Amadeusz Slawinski <amadeuszx.slawinski@linux.intel.com>
//

#include <linux/slab.h>
#include <sound/hdaudio_ext.h>
#include "avs.h"
#include "messages.h"
#include "registers.h"

#define AVS_IPC_TIMEOUT_MS	300

static void avs_dsp_receive_rx(struct avs_dev *adev, u64 header)
{
	struct avs_ipc *ipc = adev->ipc;
	union avs_reply_msg msg = AVS_MSG(header);

	ipc->rx.header = header;
	/* Abort copying payload if request processing was unsuccessful. */
	if (!msg.status) {
		/* update size in case of LARGE_CONFIG_GET */
		if (msg.msg_target == AVS_MOD_MSG &&
		    msg.global_msg_type == AVS_MOD_LARGE_CONFIG_GET)
			ipc->rx.size = msg.ext.large_config.data_off_size;

		memcpy_fromio(ipc->rx.data, avs_uplink_addr(adev), ipc->rx.size);
	}
}

static void avs_dsp_process_notification(struct avs_dev *adev, u64 header)
{
	struct avs_notify_mod_data mod_data;
	union avs_notify_msg msg = AVS_MSG(header);
	size_t data_size = 0;
	void *data = NULL;

	/* Ignore spurious notifications until handshake is established. */
	if (!adev->ipc->ready && msg.notify_msg_type != AVS_NOTIFY_FW_READY) {
		dev_dbg(adev->dev, "FW not ready, skip notification: 0x%08x\n", msg.primary);
		return;
	}

	/* Calculate notification payload size. */
	switch (msg.notify_msg_type) {
	case AVS_NOTIFY_FW_READY:
		break;

	case AVS_NOTIFY_PHRASE_DETECTED:
		data_size = sizeof(struct avs_notify_voice_data);
		break;

	case AVS_NOTIFY_RESOURCE_EVENT:
		data_size = sizeof(struct avs_notify_res_data);
		break;

	case AVS_NOTIFY_MODULE_EVENT:
		/* To know the total payload size, header needs to be read first. */
		memcpy_fromio(&mod_data, avs_uplink_addr(adev), sizeof(mod_data));
		data_size = sizeof(mod_data) + mod_data.data_size;
		break;

	default:
		dev_info(adev->dev, "unknown notification: 0x%08x\n", msg.primary);
		break;
	}

	if (data_size) {
		data = kmalloc(data_size, GFP_KERNEL);
		if (!data)
			return;

		memcpy_fromio(data, avs_uplink_addr(adev), data_size);
	}

	/* Perform notification-specific operations. */
	switch (msg.notify_msg_type) {
	case AVS_NOTIFY_FW_READY:
		dev_dbg(adev->dev, "FW READY 0x%08x\n", msg.primary);
		adev->ipc->ready = true;
		complete(&adev->fw_ready);
		break;

	default:
		break;
	}

	kfree(data);
}

void avs_dsp_process_response(struct avs_dev *adev, u64 header)
{
	struct avs_ipc *ipc = adev->ipc;

	/*
	 * Response may either be solicited - a reply for a request that has
	 * been sent beforehand - or unsolicited (notification).
	 */
	if (avs_msg_is_reply(header)) {
		/* Response processing is invoked from IRQ thread. */
		spin_lock_irq(&ipc->rx_lock);
		avs_dsp_receive_rx(adev, header);
		ipc->rx_completed = true;
		spin_unlock_irq(&ipc->rx_lock);
	} else {
		avs_dsp_process_notification(adev, header);
	}

	complete(&ipc->busy_completion);
}

irqreturn_t avs_dsp_irq_handler(int irq, void *dev_id)
{
	struct avs_dev *adev = dev_id;
	struct avs_ipc *ipc = adev->ipc;
	u32 adspis, hipc_rsp, hipc_ack;
	irqreturn_t ret = IRQ_NONE;

	adspis = snd_hdac_adsp_readl(adev, AVS_ADSP_REG_ADSPIS);
	if (adspis == UINT_MAX || !(adspis & AVS_ADSP_ADSPIS_IPC))
		return ret;

	hipc_ack = snd_hdac_adsp_readl(adev, SKL_ADSP_REG_HIPCIE);
	hipc_rsp = snd_hdac_adsp_readl(adev, SKL_ADSP_REG_HIPCT);

	/* DSP acked host's request */
	if (hipc_ack & SKL_ADSP_HIPCIE_DONE) {
		/*
		 * As an extra precaution, mask done interrupt. Code executed
		 * due to complete() found below does not assume any masking.
		 */
		snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCCTL,
				      AVS_ADSP_HIPCCTL_DONE, 0);

		complete(&ipc->done_completion);

		/* tell DSP it has our attention */
		snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCIE,
				      SKL_ADSP_HIPCIE_DONE,
				      SKL_ADSP_HIPCIE_DONE);
		/* unmask done interrupt */
		snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCCTL,
				      AVS_ADSP_HIPCCTL_DONE,
				      AVS_ADSP_HIPCCTL_DONE);
		ret = IRQ_HANDLED;
	}

	/* DSP sent new response to process */
	if (hipc_rsp & SKL_ADSP_HIPCT_BUSY) {
		/* mask busy interrupt */
		snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCCTL,
				      AVS_ADSP_HIPCCTL_BUSY, 0);

		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

irqreturn_t avs_dsp_irq_thread(int irq, void *dev_id)
{
	struct avs_dev *adev = dev_id;
	union avs_reply_msg msg;
	u32 hipct, hipcte;

	hipct = snd_hdac_adsp_readl(adev, SKL_ADSP_REG_HIPCT);
	hipcte = snd_hdac_adsp_readl(adev, SKL_ADSP_REG_HIPCTE);

	/* ensure DSP sent new response to process */
	if (!(hipct & SKL_ADSP_HIPCT_BUSY))
		return IRQ_NONE;

	msg.primary = hipct;
	msg.ext.val = hipcte;
	avs_dsp_process_response(adev, msg.val);

	/* tell DSP we accepted its message */
	snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCT,
			      SKL_ADSP_HIPCT_BUSY, SKL_ADSP_HIPCT_BUSY);
	/* unmask busy interrupt */
	snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCCTL,
			      AVS_ADSP_HIPCCTL_BUSY, AVS_ADSP_HIPCCTL_BUSY);

	return IRQ_HANDLED;
}

static bool avs_ipc_is_busy(struct avs_ipc *ipc)
{
	struct avs_dev *adev = to_avs_dev(ipc->dev);
	u32 hipc_rsp;

	hipc_rsp = snd_hdac_adsp_readl(adev, SKL_ADSP_REG_HIPCT);
	return hipc_rsp & SKL_ADSP_HIPCT_BUSY;
}

static int avs_ipc_wait_busy_completion(struct avs_ipc *ipc, int timeout)
{
	u32 repeats_left = 128; /* to avoid infinite looping */
	int ret;

again:
	ret = wait_for_completion_timeout(&ipc->busy_completion, msecs_to_jiffies(timeout));

	/* DSP could be unresponsive at this point. */
	if (!ipc->ready)
		return -EPERM;

	if (!ret) {
		if (!avs_ipc_is_busy(ipc))
			return -ETIMEDOUT;
		/*
		 * Firmware did its job, either notification or reply
		 * has been received - now wait until it's processed.
		 */
		wait_for_completion_killable(&ipc->busy_completion);
	}

	/* Ongoing notification's bottom-half may cause early wakeup */
	spin_lock(&ipc->rx_lock);
	if (!ipc->rx_completed) {
		if (repeats_left) {
			/* Reply delayed due to notification. */
			repeats_left--;
			reinit_completion(&ipc->busy_completion);
			spin_unlock(&ipc->rx_lock);
			goto again;
		}

		spin_unlock(&ipc->rx_lock);
		return -ETIMEDOUT;
	}

	spin_unlock(&ipc->rx_lock);
	return 0;
}

static void avs_ipc_msg_init(struct avs_ipc *ipc, struct avs_ipc_msg *reply)
{
	lockdep_assert_held(&ipc->rx_lock);

	ipc->rx.header = 0;
	ipc->rx.size = reply ? reply->size : 0;
	ipc->rx_completed = false;

	reinit_completion(&ipc->done_completion);
	reinit_completion(&ipc->busy_completion);
}

static void avs_dsp_send_tx(struct avs_dev *adev, struct avs_ipc_msg *tx)
{
	tx->header |= SKL_ADSP_HIPCI_BUSY;

	if (tx->size)
		memcpy_toio(avs_downlink_addr(adev), tx->data, tx->size);
	snd_hdac_adsp_writel(adev, SKL_ADSP_REG_HIPCIE, tx->header >> 32);
	snd_hdac_adsp_writel(adev, SKL_ADSP_REG_HIPCI, tx->header & UINT_MAX);
}

static int avs_dsp_do_send_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
			       struct avs_ipc_msg *reply, int timeout)
{
	struct avs_ipc *ipc = adev->ipc;
	int ret;

	if (!ipc->ready)
		return -EPERM;

	mutex_lock(&ipc->msg_mutex);

	spin_lock(&ipc->rx_lock);
	avs_ipc_msg_init(ipc, reply);
	avs_dsp_send_tx(adev, request);
	spin_unlock(&ipc->rx_lock);

	ret = avs_ipc_wait_busy_completion(ipc, timeout);
	if (ret) {
		if (ret == -ETIMEDOUT) {
			dev_crit(adev->dev, "communication severed: %d, rebooting dsp..\n", ret);

			avs_ipc_block(ipc);
		}
		goto exit;
	}

	ret = ipc->rx.rsp.status;
	if (reply) {
		reply->header = ipc->rx.header;
		if (reply->data && ipc->rx.size)
			memcpy(reply->data, ipc->rx.data, reply->size);
	}

exit:
	mutex_unlock(&ipc->msg_mutex);
	return ret;
}

int avs_dsp_send_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request,
			     struct avs_ipc_msg *reply, int timeout)
{
	return avs_dsp_do_send_msg(adev, request, reply, timeout);
}

int avs_dsp_send_msg(struct avs_dev *adev, struct avs_ipc_msg *request,
		     struct avs_ipc_msg *reply)
{
	return avs_dsp_send_msg_timeout(adev, request, reply, adev->ipc->default_timeout_ms);
}

static int avs_dsp_do_send_rom_msg(struct avs_dev *adev, struct avs_ipc_msg *request, int timeout)
{
	struct avs_ipc *ipc = adev->ipc;
	int ret;

	mutex_lock(&ipc->msg_mutex);

	spin_lock(&ipc->rx_lock);
	avs_ipc_msg_init(ipc, NULL);
	avs_dsp_send_tx(adev, request);
	spin_unlock(&ipc->rx_lock);

	/* ROM messages must be sent before main core is unstalled */
	ret = avs_dsp_op(adev, stall, AVS_MAIN_CORE_MASK, false);
	if (!ret) {
		ret = wait_for_completion_timeout(&ipc->done_completion, msecs_to_jiffies(timeout));
		ret = ret ? 0 : -ETIMEDOUT;
	}

	mutex_unlock(&ipc->msg_mutex);

	return ret;
}

int avs_dsp_send_rom_msg_timeout(struct avs_dev *adev, struct avs_ipc_msg *request, int timeout)
{
	return avs_dsp_do_send_rom_msg(adev, request, timeout);
}

int avs_dsp_send_rom_msg(struct avs_dev *adev, struct avs_ipc_msg *request)
{
	return avs_dsp_send_rom_msg_timeout(adev, request, adev->ipc->default_timeout_ms);
}

void avs_dsp_interrupt_control(struct avs_dev *adev, bool enable)
{
	u32 value, mask;

	/*
	 * No particular bit setting order. All of these are required
	 * to have a functional SW <-> FW communication.
	 */
	value = enable ? AVS_ADSP_ADSPIC_IPC : 0;
	snd_hdac_adsp_updatel(adev, AVS_ADSP_REG_ADSPIC, AVS_ADSP_ADSPIC_IPC, value);

	mask = AVS_ADSP_HIPCCTL_DONE | AVS_ADSP_HIPCCTL_BUSY;
	value = enable ? mask : 0;
	snd_hdac_adsp_updatel(adev, SKL_ADSP_REG_HIPCCTL, mask, value);
}

int avs_ipc_init(struct avs_ipc *ipc, struct device *dev)
{
	ipc->rx.data = devm_kzalloc(dev, AVS_MAILBOX_SIZE, GFP_KERNEL);
	if (!ipc->rx.data)
		return -ENOMEM;

	ipc->dev = dev;
	ipc->ready = false;
	ipc->default_timeout_ms = AVS_IPC_TIMEOUT_MS;
	init_completion(&ipc->done_completion);
	init_completion(&ipc->busy_completion);
	spin_lock_init(&ipc->rx_lock);
	mutex_init(&ipc->msg_mutex);

	return 0;
}

void avs_ipc_block(struct avs_ipc *ipc)
{
	ipc->ready = false;
}
