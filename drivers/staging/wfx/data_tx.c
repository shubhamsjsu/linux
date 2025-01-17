// SPDX-License-Identifier: GPL-2.0-only
/*
 * Datapath implementation.
 *
 * Copyright (c) 2017-2019, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 */
#include <net/mac80211.h>

#include "data_tx.h"
#include "wfx.h"
#include "bh.h"
#include "sta.h"
#include "queue.h"
#include "debug.h"
#include "traces.h"
#include "hif_tx_mib.h"

#define WFX_INVALID_RATE_ID (0xFF)
#define WFX_LINK_ID_NO_ASSOC   15
#define WFX_LINK_ID_GC_TIMEOUT ((unsigned long)(10 * HZ))

static int wfx_get_hw_rate(struct wfx_dev *wdev,
			   const struct ieee80211_tx_rate *rate)
{
	if (rate->idx < 0)
		return -1;
	if (rate->flags & IEEE80211_TX_RC_MCS) {
		if (rate->idx > 7) {
			WARN(1, "wrong rate->idx value: %d", rate->idx);
			return -1;
		}
		return rate->idx + 14;
	}
	// WFx only support 2GHz, else band information should be retrieved
	// from ieee80211_tx_info
	return wdev->hw->wiphy->bands[NL80211_BAND_2GHZ]->bitrates[rate->idx].hw_value;
}

/* TX policy cache implementation */

static void wfx_tx_policy_build(struct wfx_vif *wvif, struct tx_policy *policy,
				struct ieee80211_tx_rate *rates)
{
	int i;
	size_t count;
	struct wfx_dev *wdev = wvif->wdev;

	WARN(rates[0].idx < 0, "invalid rate policy");
	memset(policy, 0, sizeof(*policy));
	for (i = 1; i < IEEE80211_TX_MAX_RATES; i++)
		if (rates[i].idx < 0)
			break;
	count = i;

	/* HACK!!! Device has problems (at least) switching from
	 * 54Mbps CTS to 1Mbps. This switch takes enormous amount
	 * of time (100-200 ms), leading to valuable throughput drop.
	 * As a workaround, additional g-rates are injected to the
	 * policy.
	 */
	if (count == 2 && !(rates[0].flags & IEEE80211_TX_RC_MCS) &&
	    rates[0].idx > 4 && rates[0].count > 2 &&
	    rates[1].idx < 2) {
		int mid_rate = (rates[0].idx + 4) >> 1;

		/* Decrease number of retries for the initial rate */
		rates[0].count -= 2;

		if (mid_rate != 4) {
			/* Keep fallback rate at 1Mbps. */
			rates[3] = rates[1];

			/* Inject 1 transmission on lowest g-rate */
			rates[2].idx = 4;
			rates[2].count = 1;
			rates[2].flags = rates[1].flags;

			/* Inject 1 transmission on mid-rate */
			rates[1].idx = mid_rate;
			rates[1].count = 1;

			/* Fallback to 1 Mbps is a really bad thing,
			 * so let's try to increase probability of
			 * successful transmission on the lowest g rate
			 * even more
			 */
			if (rates[0].count >= 3) {
				--rates[0].count;
				++rates[2].count;
			}

			/* Adjust amount of rates defined */
			count += 2;
		} else {
			/* Keep fallback rate at 1Mbps. */
			rates[2] = rates[1];

			/* Inject 2 transmissions on lowest g-rate */
			rates[1].idx = 4;
			rates[1].count = 2;

			/* Adjust amount of rates defined */
			count += 1;
		}
	}

	for (i = 0; i < IEEE80211_TX_MAX_RATES; ++i) {
		int rateid;
		u8 count;

		if (rates[i].idx < 0)
			break;
		WARN_ON(rates[i].count > 15);
		rateid = wfx_get_hw_rate(wdev, &rates[i]);
		// Pack two values in each byte of policy->rates
		count = rates[i].count;
		if (rateid % 2)
			count <<= 4;
		policy->rates[rateid / 2] |= count;
	}
}

static bool tx_policy_is_equal(const struct tx_policy *a,
			       const struct tx_policy *b)
{
	return !memcmp(a->rates, b->rates, sizeof(a->rates));
}

static int wfx_tx_policy_find(struct tx_policy_cache *cache,
			      struct tx_policy *wanted)
{
	struct tx_policy *it;

	list_for_each_entry(it, &cache->used, link)
		if (tx_policy_is_equal(wanted, it))
			return it - cache->cache;
	list_for_each_entry(it, &cache->free, link)
		if (tx_policy_is_equal(wanted, it))
			return it - cache->cache;
	return -1;
}

static void wfx_tx_policy_use(struct tx_policy_cache *cache,
			      struct tx_policy *entry)
{
	++entry->usage_count;
	list_move(&entry->link, &cache->used);
}

static int wfx_tx_policy_release(struct tx_policy_cache *cache,
				 struct tx_policy *entry)
{
	int ret = --entry->usage_count;

	if (!ret)
		list_move(&entry->link, &cache->free);
	return ret;
}

static int wfx_tx_policy_get(struct wfx_vif *wvif,
			     struct ieee80211_tx_rate *rates,
			     bool *renew)
{
	int idx;
	struct tx_policy_cache *cache = &wvif->tx_policy_cache;
	struct tx_policy wanted;

	wfx_tx_policy_build(wvif, &wanted, rates);

	spin_lock_bh(&cache->lock);
	if (WARN_ON(list_empty(&cache->free))) {
		spin_unlock_bh(&cache->lock);
		return WFX_INVALID_RATE_ID;
	}
	idx = wfx_tx_policy_find(cache, &wanted);
	if (idx >= 0) {
		*renew = false;
	} else {
		struct tx_policy *entry;
		*renew = true;
		/* If policy is not found create a new one
		 * using the oldest entry in "free" list
		 */
		entry = list_entry(cache->free.prev, struct tx_policy, link);
		memcpy(entry->rates, wanted.rates, sizeof(entry->rates));
		entry->uploaded = 0;
		entry->usage_count = 0;
		idx = entry - cache->cache;
	}
	wfx_tx_policy_use(cache, &cache->cache[idx]);
	if (list_empty(&cache->free)) {
		/* Lock TX queues. */
		wfx_tx_queues_lock(wvif->wdev);
	}
	spin_unlock_bh(&cache->lock);
	return idx;
}

static void wfx_tx_policy_put(struct wfx_vif *wvif, int idx)
{
	int usage, locked;
	struct tx_policy_cache *cache = &wvif->tx_policy_cache;

	spin_lock_bh(&cache->lock);
	locked = list_empty(&cache->free);
	usage = wfx_tx_policy_release(cache, &cache->cache[idx]);
	if (locked && !usage) {
		/* Unlock TX queues. */
		wfx_tx_queues_unlock(wvif->wdev);
	}
	spin_unlock_bh(&cache->lock);
}

static int wfx_tx_policy_upload(struct wfx_vif *wvif)
{
	int i;
	struct tx_policy_cache *cache = &wvif->tx_policy_cache;
	struct hif_mib_set_tx_rate_retry_policy *arg =
		kzalloc(struct_size(arg,
				    tx_rate_retry_policy,
				    HIF_MIB_NUM_TX_RATE_RETRY_POLICIES),
			GFP_KERNEL);
	struct hif_mib_tx_rate_retry_policy *dst;

	spin_lock_bh(&cache->lock);
	/* Upload only modified entries. */
	for (i = 0; i < HIF_MIB_NUM_TX_RATE_RETRY_POLICIES; ++i) {
		struct tx_policy *src = &cache->cache[i];

		if (!src->uploaded && memzcmp(src->rates, sizeof(src->rates))) {
			dst = arg->tx_rate_retry_policy +
				arg->num_tx_rate_policies;

			dst->policy_index = i;
			dst->short_retry_count = 255;
			dst->long_retry_count = 255;
			dst->first_rate_sel = 1;
			dst->terminate = 1;
			dst->count_init = 1;
			memcpy(&dst->rates, src->rates, sizeof(src->rates));
			src->uploaded = 1;
			arg->num_tx_rate_policies++;
		}
	}
	spin_unlock_bh(&cache->lock);
	hif_set_tx_rate_retry_policy(wvif, arg);
	kfree(arg);
	return 0;
}

static void wfx_tx_policy_upload_work(struct work_struct *work)
{
	struct wfx_vif *wvif =
		container_of(work, struct wfx_vif, tx_policy_upload_work);

	wfx_tx_policy_upload(wvif);

	wfx_tx_unlock(wvif->wdev);
	wfx_tx_queues_unlock(wvif->wdev);
}

void wfx_tx_policy_init(struct wfx_vif *wvif)
{
	struct tx_policy_cache *cache = &wvif->tx_policy_cache;
	int i;

	memset(cache, 0, sizeof(*cache));

	spin_lock_init(&cache->lock);
	INIT_LIST_HEAD(&cache->used);
	INIT_LIST_HEAD(&cache->free);
	INIT_WORK(&wvif->tx_policy_upload_work, wfx_tx_policy_upload_work);

	for (i = 0; i < HIF_MIB_NUM_TX_RATE_RETRY_POLICIES; ++i)
		list_add(&cache->cache[i].link, &cache->free);
}

/* Link ID related functions */

static int wfx_alloc_link_id(struct wfx_vif *wvif, const u8 *mac)
{
	int i, ret = 0;
	unsigned long max_inactivity = 0;
	unsigned long now = jiffies;

	spin_lock_bh(&wvif->ps_state_lock);
	for (i = 0; i < WFX_MAX_STA_IN_AP_MODE; ++i) {
		if (!wvif->link_id_db[i].status) {
			ret = i + 1;
			break;
		} else if (wvif->link_id_db[i].status != WFX_LINK_HARD &&
			   !wvif->wdev->tx_queue_stats.link_map_cache[i + 1]) {
			unsigned long inactivity =
				now - wvif->link_id_db[i].timestamp;

			if (inactivity < max_inactivity)
				continue;
			max_inactivity = inactivity;
			ret = i + 1;
		}
	}

	if (ret) {
		struct wfx_link_entry *entry = &wvif->link_id_db[ret - 1];

		entry->status = WFX_LINK_RESERVE;
		ether_addr_copy(entry->mac, mac);
		memset(&entry->buffered, 0, WFX_MAX_TID);
		skb_queue_head_init(&entry->rx_queue);
		wfx_tx_lock(wvif->wdev);

		if (!schedule_work(&wvif->link_id_work))
			wfx_tx_unlock(wvif->wdev);
	} else {
		dev_info(wvif->wdev->dev, "no more link-id available\n");
	}
	spin_unlock_bh(&wvif->ps_state_lock);
	return ret;
}

int wfx_find_link_id(struct wfx_vif *wvif, const u8 *mac)
{
	int i, ret = 0;

	spin_lock_bh(&wvif->ps_state_lock);
	for (i = 0; i < WFX_MAX_STA_IN_AP_MODE; ++i) {
		if (ether_addr_equal(mac, wvif->link_id_db[i].mac) &&
		    wvif->link_id_db[i].status) {
			wvif->link_id_db[i].timestamp = jiffies;
			ret = i + 1;
			break;
		}
	}
	spin_unlock_bh(&wvif->ps_state_lock);
	return ret;
}

static int wfx_map_link(struct wfx_vif *wvif,
			struct wfx_link_entry *link_entry, int sta_id)
{
	int ret;

	ret = hif_map_link(wvif, link_entry->mac, 0, sta_id);

	if (ret == 0)
		/* Save the MAC address currently associated with the peer
		 * for future unmap request
		 */
		ether_addr_copy(link_entry->old_mac, link_entry->mac);

	return ret;
}

int wfx_unmap_link(struct wfx_vif *wvif, int sta_id)
{
	u8 *mac_addr = NULL;

	if (sta_id)
		mac_addr = wvif->link_id_db[sta_id - 1].old_mac;

	return hif_map_link(wvif, mac_addr, 1, sta_id);
}

void wfx_link_id_gc_work(struct work_struct *work)
{
	struct wfx_vif *wvif =
		container_of(work, struct wfx_vif, link_id_gc_work.work);
	unsigned long now = jiffies;
	unsigned long next_gc = -1;
	long ttl;
	u32 mask;
	int i;

	if (wvif->state != WFX_STATE_AP)
		return;

	wfx_tx_lock_flush(wvif->wdev);
	spin_lock_bh(&wvif->ps_state_lock);
	for (i = 0; i < WFX_MAX_STA_IN_AP_MODE; ++i) {
		bool need_reset = false;

		mask = BIT(i + 1);
		if (wvif->link_id_db[i].status == WFX_LINK_RESERVE ||
		    (wvif->link_id_db[i].status == WFX_LINK_HARD &&
		     !(wvif->link_id_map & mask))) {
			if (wvif->link_id_map & mask) {
				wvif->sta_asleep_mask &= ~mask;
				wvif->pspoll_mask &= ~mask;
				need_reset = true;
			}
			wvif->link_id_map |= mask;
			if (wvif->link_id_db[i].status != WFX_LINK_HARD)
				wvif->link_id_db[i].status = WFX_LINK_SOFT;

			spin_unlock_bh(&wvif->ps_state_lock);
			if (need_reset)
				wfx_unmap_link(wvif, i + 1);
			wfx_map_link(wvif, &wvif->link_id_db[i], i + 1);
			next_gc = min(next_gc, WFX_LINK_ID_GC_TIMEOUT);
			spin_lock_bh(&wvif->ps_state_lock);
		} else if (wvif->link_id_db[i].status == WFX_LINK_SOFT) {
			ttl = wvif->link_id_db[i].timestamp - now +
					WFX_LINK_ID_GC_TIMEOUT;
			if (ttl <= 0) {
				need_reset = true;
				wvif->link_id_db[i].status = WFX_LINK_OFF;
				wvif->link_id_map &= ~mask;
				wvif->sta_asleep_mask &= ~mask;
				wvif->pspoll_mask &= ~mask;
				spin_unlock_bh(&wvif->ps_state_lock);
				wfx_unmap_link(wvif, i + 1);
				spin_lock_bh(&wvif->ps_state_lock);
			} else {
				next_gc = min_t(unsigned long, next_gc, ttl);
			}
		}
		if (need_reset)
			skb_queue_purge(&wvif->link_id_db[i].rx_queue);
	}
	spin_unlock_bh(&wvif->ps_state_lock);
	if (next_gc != -1)
		schedule_delayed_work(&wvif->link_id_gc_work, next_gc);
	wfx_tx_unlock(wvif->wdev);
}

void wfx_link_id_work(struct work_struct *work)
{
	struct wfx_vif *wvif =
		container_of(work, struct wfx_vif, link_id_work);

	wfx_tx_flush(wvif->wdev);
	wfx_link_id_gc_work(&wvif->link_id_gc_work.work);
	wfx_tx_unlock(wvif->wdev);
}

/* Tx implementation */

static bool ieee80211_is_action_back(struct ieee80211_hdr *hdr)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)hdr;

	if (!ieee80211_is_action(mgmt->frame_control))
		return false;
	if (mgmt->u.action.category != WLAN_CATEGORY_BACK)
		return false;
	return true;
}

static void wfx_tx_manage_pm(struct wfx_vif *wvif, struct ieee80211_hdr *hdr,
			     struct wfx_tx_priv *tx_priv,
			     struct ieee80211_sta *sta)
{
	u32 mask = ~BIT(tx_priv->raw_link_id);

	spin_lock_bh(&wvif->ps_state_lock);
	if (ieee80211_is_auth(hdr->frame_control)) {
		wvif->sta_asleep_mask &= mask;
		wvif->pspoll_mask &= mask;
	}

	if (tx_priv->link_id == WFX_LINK_ID_AFTER_DTIM &&
	    !wvif->mcast_buffered) {
		wvif->mcast_buffered = true;
		if (wvif->sta_asleep_mask)
			schedule_work(&wvif->mcast_start_work);
	}

	if (tx_priv->raw_link_id) {
		wvif->link_id_db[tx_priv->raw_link_id - 1].timestamp = jiffies;
		if (tx_priv->tid < WFX_MAX_TID)
			wvif->link_id_db[tx_priv->raw_link_id - 1].buffered[tx_priv->tid]++;
	}
	spin_unlock_bh(&wvif->ps_state_lock);

	if (sta)
		ieee80211_sta_set_buffered(sta, tx_priv->tid, true);
}

static u8 wfx_tx_get_raw_link_id(struct wfx_vif *wvif,
				      struct ieee80211_sta *sta,
				      struct ieee80211_hdr *hdr)
{
	struct wfx_sta_priv *sta_priv =
		sta ? (struct wfx_sta_priv *) &sta->drv_priv : NULL;
	const u8 *da = ieee80211_get_DA(hdr);
	int ret;

	if (sta_priv && sta_priv->link_id)
		return sta_priv->link_id;
	if (wvif->vif->type != NL80211_IFTYPE_AP)
		return 0;
	if (is_multicast_ether_addr(da))
		return 0;
	ret = wfx_find_link_id(wvif, da);
	if (!ret)
		ret = wfx_alloc_link_id(wvif, da);
	if (!ret) {
		dev_err(wvif->wdev->dev, "no more link-id available\n");
		return WFX_LINK_ID_NO_ASSOC;
	}
	return ret;
}

static void wfx_tx_fixup_rates(struct ieee80211_tx_rate *rates)
{
	int i;
	bool finished;

	// Firmware is not able to mix rates with differents flags
	for (i = 0; i < IEEE80211_TX_MAX_RATES; i++) {
		if (rates[0].flags & IEEE80211_TX_RC_SHORT_GI)
			rates[i].flags |= IEEE80211_TX_RC_SHORT_GI;
		if (!(rates[0].flags & IEEE80211_TX_RC_SHORT_GI))
			rates[i].flags &= ~IEEE80211_TX_RC_SHORT_GI;
		if (!(rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS))
			rates[i].flags &= ~IEEE80211_TX_RC_USE_RTS_CTS;
	}

	// Sort rates and remove duplicates
	do {
		finished = true;
		for (i = 0; i < IEEE80211_TX_MAX_RATES - 1; i++) {
			if (rates[i + 1].idx == rates[i].idx &&
			    rates[i].idx != -1) {
				rates[i].count =
					max_t(int, rates[i].count,
					      rates[i + 1].count);
				rates[i + 1].idx = -1;
				rates[i + 1].count = 0;

				finished = false;
			}
			if (rates[i + 1].idx > rates[i].idx) {
				swap(rates[i + 1], rates[i]);
				finished = false;
			}
		}
	} while (!finished);
	// All retries use long GI
	for (i = 1; i < IEEE80211_TX_MAX_RATES; i++)
		rates[i].flags &= ~IEEE80211_TX_RC_SHORT_GI;
}

static u8 wfx_tx_get_rate_id(struct wfx_vif *wvif,
				  struct ieee80211_tx_info *tx_info)
{
	bool tx_policy_renew = false;
	u8 rate_id;

	rate_id = wfx_tx_policy_get(wvif,
				    tx_info->driver_rates, &tx_policy_renew);
	WARN(rate_id == WFX_INVALID_RATE_ID, "unable to get a valid Tx policy");

	if (tx_policy_renew) {
		/* FIXME: It's not so optimal to stop TX queues every now and
		 * then.  Better to reimplement task scheduling with a counter.
		 */
		wfx_tx_lock(wvif->wdev);
		wfx_tx_queues_lock(wvif->wdev);
		if (!schedule_work(&wvif->tx_policy_upload_work)) {
			wfx_tx_queues_unlock(wvif->wdev);
			wfx_tx_unlock(wvif->wdev);
		}
	}
	return rate_id;
}

static struct hif_ht_tx_parameters wfx_tx_get_tx_parms(struct wfx_dev *wdev, struct ieee80211_tx_info *tx_info)
{
	struct ieee80211_tx_rate *rate = &tx_info->driver_rates[0];
	struct hif_ht_tx_parameters ret = { };

	if (!(rate->flags & IEEE80211_TX_RC_MCS))
		ret.frame_format = HIF_FRAME_FORMAT_NON_HT;
	else if (!(rate->flags & IEEE80211_TX_RC_GREEN_FIELD))
		ret.frame_format = HIF_FRAME_FORMAT_MIXED_FORMAT_HT;
	else
		ret.frame_format = HIF_FRAME_FORMAT_GF_HT_11N;
	if (rate->flags & IEEE80211_TX_RC_SHORT_GI)
		ret.short_gi = 1;
	if (tx_info->flags & IEEE80211_TX_CTL_STBC)
		ret.stbc = 0; // FIXME: Not yet supported by firmware?
	return ret;
}

static u8 wfx_tx_get_tid(struct ieee80211_hdr *hdr)
{
	// FIXME: ieee80211_get_tid(hdr) should be sufficient for all cases.
	if (!ieee80211_is_data(hdr->frame_control))
		return WFX_MAX_TID;
	if (ieee80211_is_data_qos(hdr->frame_control))
		return ieee80211_get_tid(hdr);
	else
		return 0;
}

static int wfx_tx_get_icv_len(struct ieee80211_key_conf *hw_key)
{
	int mic_space;

	if (!hw_key)
		return 0;
	mic_space = (hw_key->cipher == WLAN_CIPHER_SUITE_TKIP) ? 8 : 0;
	return hw_key->icv_len + mic_space;
}

static int wfx_tx_inner(struct wfx_vif *wvif, struct ieee80211_sta *sta,
			struct sk_buff *skb)
{
	struct hif_msg *hif_msg;
	struct hif_req_tx *req;
	struct wfx_tx_priv *tx_priv;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_key_conf *hw_key = tx_info->control.hw_key;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int queue_id = tx_info->hw_queue;
	size_t offset = (size_t) skb->data & 3;
	int wmsg_len = sizeof(struct hif_msg) +
			sizeof(struct hif_req_tx) + offset;

	WARN(queue_id >= IEEE80211_NUM_ACS, "unsupported queue_id");
	wfx_tx_fixup_rates(tx_info->driver_rates);

	// From now tx_info->control is unusable
	memset(tx_info->rate_driver_data, 0, sizeof(struct wfx_tx_priv));
	// Fill tx_priv
	tx_priv = (struct wfx_tx_priv *)tx_info->rate_driver_data;
	tx_priv->tid = wfx_tx_get_tid(hdr);
	tx_priv->raw_link_id = wfx_tx_get_raw_link_id(wvif, sta, hdr);
	tx_priv->link_id = tx_priv->raw_link_id;
	if (ieee80211_has_protected(hdr->frame_control))
		tx_priv->hw_key = hw_key;
	if (tx_info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM)
		tx_priv->link_id = WFX_LINK_ID_AFTER_DTIM;
	if (sta && (sta->uapsd_queues & BIT(queue_id)))
		tx_priv->link_id = WFX_LINK_ID_UAPSD;

	// Fill hif_msg
	WARN(skb_headroom(skb) < wmsg_len, "not enough space in skb");
	WARN(offset & 1, "attempt to transmit an unaligned frame");
	skb_put(skb, wfx_tx_get_icv_len(tx_priv->hw_key));
	skb_push(skb, wmsg_len);
	memset(skb->data, 0, wmsg_len);
	hif_msg = (struct hif_msg *)skb->data;
	hif_msg->len = cpu_to_le16(skb->len);
	hif_msg->id = HIF_REQ_ID_TX;
	hif_msg->interface = wvif->id;
	if (skb->len > wvif->wdev->hw_caps.size_inp_ch_buf) {
		dev_warn(wvif->wdev->dev, "requested frame size (%d) is larger than maximum supported (%d)\n",
			 skb->len, wvif->wdev->hw_caps.size_inp_ch_buf);
		skb_pull(skb, wmsg_len);
		return -EIO;
	}

	// Fill tx request
	req = (struct hif_req_tx *)hif_msg->body;
	req->packet_id = queue_id << 16 |
			 IEEE80211_SEQ_TO_SN(le16_to_cpu(hdr->seq_ctrl));
	req->data_flags.fc_offset = offset;
	req->queue_id.peer_sta_id = tx_priv->raw_link_id;
	// Queue index are inverted between firmware and Linux
	req->queue_id.queue_id = 3 - queue_id;
	req->ht_tx_parameters = wfx_tx_get_tx_parms(wvif->wdev, tx_info);
	req->tx_flags.retry_policy_index = wfx_tx_get_rate_id(wvif, tx_info);

	// Auxiliary operations
	wfx_tx_manage_pm(wvif, hdr, tx_priv, sta);
	wfx_tx_queue_put(wvif->wdev, &wvif->wdev->tx_queue[queue_id], skb);
	wfx_bh_request_tx(wvif->wdev);
	return 0;
}

void wfx_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb)
{
	struct wfx_dev *wdev = hw->priv;
	struct wfx_vif *wvif;
	struct ieee80211_sta *sta = control ? control->sta : NULL;
	struct ieee80211_tx_info *tx_info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	size_t driver_data_room = sizeof_field(struct ieee80211_tx_info,
					       rate_driver_data);

	compiletime_assert(sizeof(struct wfx_tx_priv) <= driver_data_room,
			   "struct tx_priv is too large");
	WARN(skb->next || skb->prev, "skb is already member of a list");
	// control.vif can be NULL for injected frames
	if (tx_info->control.vif)
		wvif = (struct wfx_vif *)tx_info->control.vif->drv_priv;
	else
		wvif = wvif_iterate(wdev, NULL);
	if (WARN_ON(!wvif))
		goto drop;
	// FIXME: why?
	if (ieee80211_is_action_back(hdr)) {
		dev_info(wdev->dev, "drop BA action\n");
		goto drop;
	}
	if (wfx_tx_inner(wvif, sta, skb))
		goto drop;

	return;

drop:
	ieee80211_tx_status_irqsafe(wdev->hw, skb);
}

void wfx_tx_confirm_cb(struct wfx_vif *wvif, struct hif_cnf_tx *arg)
{
	int i;
	int tx_count;
	struct sk_buff *skb;
	struct ieee80211_tx_rate *rate;
	struct ieee80211_tx_info *tx_info;
	const struct wfx_tx_priv *tx_priv;

	skb = wfx_pending_get(wvif->wdev, arg->packet_id);
	if (!skb) {
		dev_warn(wvif->wdev->dev,
			 "received unknown packet_id (%#.8x) from chip\n",
			 arg->packet_id);
		return;
	}
	tx_info = IEEE80211_SKB_CB(skb);
	tx_priv = wfx_skb_tx_priv(skb);
	_trace_tx_stats(arg, skb,
			wfx_pending_get_pkt_us_delay(wvif->wdev, skb));

	// You can touch to tx_priv, but don't touch to tx_info->status.
	tx_count = arg->ack_failures;
	if (!arg->status || arg->ack_failures)
		tx_count += 1; // Also report success
	for (i = 0; i < IEEE80211_TX_MAX_RATES; i++) {
		rate = &tx_info->status.rates[i];
		if (rate->idx < 0)
			break;
		if (tx_count < rate->count && arg->status && arg->ack_failures)
			dev_dbg(wvif->wdev->dev, "all retries were not consumed: %d != %d\n",
				rate->count, tx_count);
		if (tx_count <= rate->count && tx_count &&
		    arg->txed_rate != wfx_get_hw_rate(wvif->wdev, rate))
			dev_dbg(wvif->wdev->dev,
				"inconsistent tx_info rates: %d != %d\n",
				arg->txed_rate,
				wfx_get_hw_rate(wvif->wdev, rate));
		if (tx_count > rate->count) {
			tx_count -= rate->count;
		} else if (!tx_count) {
			rate->count = 0;
			rate->idx = -1;
		} else {
			rate->count = tx_count;
			tx_count = 0;
		}
	}
	if (tx_count)
		dev_dbg(wvif->wdev->dev,
			"%d more retries than expected\n", tx_count);
	skb_trim(skb, skb->len - wfx_tx_get_icv_len(tx_priv->hw_key));

	// From now, you can touch to tx_info->status, but do not touch to
	// tx_priv anymore
	// FIXME: use ieee80211_tx_info_clear_status()
	memset(tx_info->rate_driver_data, 0, sizeof(tx_info->rate_driver_data));
	memset(tx_info->pad, 0, sizeof(tx_info->pad));

	if (!arg->status) {
		if (wvif->bss_loss_state &&
		    arg->packet_id == wvif->bss_loss_confirm_id)
			wfx_cqm_bssloss_sm(wvif, 0, 1, 0);
		tx_info->status.tx_time =
		arg->media_delay - arg->tx_queue_delay;
		if (tx_info->flags & IEEE80211_TX_CTL_NO_ACK)
			tx_info->flags |= IEEE80211_TX_STAT_NOACK_TRANSMITTED;
		else
			tx_info->flags |= IEEE80211_TX_STAT_ACK;
	} else if (arg->status == HIF_REQUEUE) {
		/* "REQUEUE" means "implicit suspend" */
		struct hif_ind_suspend_resume_tx suspend = {
			.suspend_resume_flags.resume = 0,
			.suspend_resume_flags.bc_mc_only = 1,
		};

		WARN(!arg->tx_result_flags.requeue, "incoherent status and result_flags");
		wfx_suspend_resume(wvif, &suspend);
		tx_info->flags |= IEEE80211_TX_STAT_TX_FILTERED;
	} else {
		if (wvif->bss_loss_state &&
		    arg->packet_id == wvif->bss_loss_confirm_id)
			wfx_cqm_bssloss_sm(wvif, 0, 0, 1);
	}
	wfx_pending_remove(wvif->wdev, skb);
}

static void wfx_notify_buffered_tx(struct wfx_vif *wvif, struct sk_buff *skb,
				   struct hif_req_tx *req)
{
	struct ieee80211_sta *sta;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int tid = wfx_tx_get_tid(hdr);
	int raw_link_id = req->queue_id.peer_sta_id;
	u8 *buffered;

	if (raw_link_id && tid < WFX_MAX_TID) {
		buffered = wvif->link_id_db[raw_link_id - 1].buffered;

		spin_lock_bh(&wvif->ps_state_lock);
		WARN(!buffered[tid], "inconsistent notification");
		buffered[tid]--;
		spin_unlock_bh(&wvif->ps_state_lock);

		if (!buffered[tid]) {
			rcu_read_lock();
			sta = ieee80211_find_sta(wvif->vif, hdr->addr1);
			if (sta)
				ieee80211_sta_set_buffered(sta, tid, false);
			rcu_read_unlock();
		}
	}
}

void wfx_skb_dtor(struct wfx_dev *wdev, struct sk_buff *skb)
{
	struct hif_msg *hif = (struct hif_msg *)skb->data;
	struct hif_req_tx *req = (struct hif_req_tx *)hif->body;
	struct wfx_vif *wvif = wdev_to_wvif(wdev, hif->interface);
	unsigned int offset = sizeof(struct hif_req_tx) +
				sizeof(struct hif_msg) +
				req->data_flags.fc_offset;

	WARN_ON(!wvif);
	skb_pull(skb, offset);
	wfx_notify_buffered_tx(wvif, skb, req);
	wfx_tx_policy_put(wvif, req->tx_flags.retry_policy_index);
	ieee80211_tx_status_irqsafe(wdev->hw, skb);
}
