// SPDX-License-Identifier: GPL-2.0-only
/*
 * BSS client mode implementation
 * Copyright 2003-2008, Jouni Malinen <j@w1.fi>
 * Copyright 2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 * Copyright 2013-2014  Intel Mobile Communications GmbH
 * Copyright (C) 2015 - 2017 Intel Deutschland GmbH
 * Copyright (C) 2018 - 2025 Intel Corporation
 */

#include <linux/delay.h>
#include <linux/fips.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/moduleparam.h>
#include <linux/rtnetlink.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <net/mac80211.h>
#include <linux/unaligned.h>

#include "ieee80211_i.h"
#include "driver-ops.h"
#include "rate.h"
#include "led.h"
#include "fils_aead.h"

#include <kunit/static_stub.h>

#define IEEE80211_AUTH_TIMEOUT		(HZ / 5)
#define IEEE80211_AUTH_TIMEOUT_LONG	(HZ / 2)
#define IEEE80211_AUTH_TIMEOUT_SHORT	(HZ / 10)
#define IEEE80211_AUTH_TIMEOUT_SAE	(HZ * 2)
#define IEEE80211_AUTH_MAX_TRIES	3
#define IEEE80211_AUTH_WAIT_ASSOC	(HZ * 5)
#define IEEE80211_AUTH_WAIT_SAE_RETRY	(HZ * 2)
#define IEEE80211_ASSOC_TIMEOUT		(HZ / 5)
#define IEEE80211_ASSOC_TIMEOUT_LONG	(HZ / 2)
#define IEEE80211_ASSOC_TIMEOUT_SHORT	(HZ / 10)
#define IEEE80211_ASSOC_MAX_TRIES	3

#define IEEE80211_ADV_TTLM_SAFETY_BUFFER_MS msecs_to_jiffies(100)
#define IEEE80211_ADV_TTLM_ST_UNDERFLOW 0xff00

#define IEEE80211_NEG_TTLM_REQ_TIMEOUT (HZ / 5)

static int max_nullfunc_tries = 2;
module_param(max_nullfunc_tries, int, 0644);
MODULE_PARM_DESC(max_nullfunc_tries,
		 "Maximum nullfunc tx tries before disconnecting (reason 4).");

static int max_probe_tries = 5;
module_param(max_probe_tries, int, 0644);
MODULE_PARM_DESC(max_probe_tries,
		 "Maximum probe tries before disconnecting (reason 4).");

/*
 * Beacon loss timeout is calculated as N frames times the
 * advertised beacon interval.  This may need to be somewhat
 * higher than what hardware might detect to account for
 * delays in the host processing frames. But since we also
 * probe on beacon miss before declaring the connection lost
 * default to what we want.
 */
static int beacon_loss_count = 7;
module_param(beacon_loss_count, int, 0644);
MODULE_PARM_DESC(beacon_loss_count,
		 "Number of beacon intervals before we decide beacon was lost.");

/*
 * Time the connection can be idle before we probe
 * it to see if we can still talk to the AP.
 */
#define IEEE80211_CONNECTION_IDLE_TIME	(30 * HZ)
/*
 * Time we wait for a probe response after sending
 * a probe request because of beacon loss or for
 * checking the connection still works.
 */
static int probe_wait_ms = 500;
module_param(probe_wait_ms, int, 0644);
MODULE_PARM_DESC(probe_wait_ms,
		 "Maximum time(ms) to wait for probe response"
		 " before disconnecting (reason 4).");

/*
 * How many Beacon frames need to have been used in average signal strength
 * before starting to indicate signal change events.
 */
#define IEEE80211_SIGNAL_AVE_MIN_COUNT	4

/*
 * We can have multiple work items (and connection probing)
 * scheduling this timer, but we need to take care to only
 * reschedule it when it should fire _earlier_ than it was
 * asked for before, or if it's not pending right now. This
 * function ensures that. Note that it then is required to
 * run this function for all timeouts after the first one
 * has happened -- the work that runs from this timer will
 * do that.
 */
static void run_again(struct ieee80211_sub_if_data *sdata,
		      unsigned long timeout)
{
	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!timer_pending(&sdata->u.mgd.timer) ||
	    time_before(timeout, sdata->u.mgd.timer.expires))
		mod_timer(&sdata->u.mgd.timer, timeout);
}

void ieee80211_sta_reset_beacon_monitor(struct ieee80211_sub_if_data *sdata)
{
	if (sdata->vif.driver_flags & IEEE80211_VIF_BEACON_FILTER)
		return;

	if (ieee80211_hw_check(&sdata->local->hw, CONNECTION_MONITOR))
		return;

	mod_timer(&sdata->u.mgd.bcn_mon_timer,
		  round_jiffies_up(jiffies + sdata->u.mgd.beacon_timeout));
}

void ieee80211_sta_reset_conn_monitor(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	if (unlikely(!ifmgd->associated))
		return;

	if (ifmgd->probe_send_count)
		ifmgd->probe_send_count = 0;

	if (ieee80211_hw_check(&sdata->local->hw, CONNECTION_MONITOR))
		return;

	mod_timer(&ifmgd->conn_mon_timer,
		  round_jiffies_up(jiffies + IEEE80211_CONNECTION_IDLE_TIME));
}

static int ecw2cw(int ecw)
{
	return (1 << ecw) - 1;
}

static enum ieee80211_conn_mode
ieee80211_determine_ap_chan(struct ieee80211_sub_if_data *sdata,
			    struct ieee80211_channel *channel,
			    u32 vht_cap_info,
			    const struct ieee802_11_elems *elems,
			    bool ignore_ht_channel_mismatch,
			    const struct ieee80211_conn_settings *conn,
			    struct cfg80211_chan_def *chandef)
{
	const struct ieee80211_ht_operation *ht_oper = elems->ht_operation;
	const struct ieee80211_vht_operation *vht_oper = elems->vht_operation;
	const struct ieee80211_he_operation *he_oper = elems->he_operation;
	const struct ieee80211_eht_operation *eht_oper = elems->eht_operation;
	struct ieee80211_supported_band *sband =
		sdata->local->hw.wiphy->bands[channel->band];
	struct cfg80211_chan_def vht_chandef;
	bool no_vht = false;
	u32 ht_cfreq;

	if (ieee80211_hw_check(&sdata->local->hw, STRICT))
		ignore_ht_channel_mismatch = false;

	*chandef = (struct cfg80211_chan_def) {
		.chan = channel,
		.width = NL80211_CHAN_WIDTH_20_NOHT,
		.center_freq1 = channel->center_freq,
		.freq1_offset = channel->freq_offset,
	};

	/* get special S1G case out of the way */
	if (sband->band == NL80211_BAND_S1GHZ) {
		if (!ieee80211_chandef_s1g_oper(elems->s1g_oper, chandef)) {
			sdata_info(sdata,
				   "Missing S1G Operation Element? Trying operating == primary\n");
			chandef->width = ieee80211_s1g_channel_width(channel);
		}

		return IEEE80211_CONN_MODE_S1G;
	}

	/* get special 6 GHz case out of the way */
	if (sband->band == NL80211_BAND_6GHZ) {
		enum ieee80211_conn_mode mode = IEEE80211_CONN_MODE_EHT;

		/* this is an error */
		if (conn->mode < IEEE80211_CONN_MODE_HE)
			return IEEE80211_CONN_MODE_LEGACY;

		if (!elems->he_6ghz_capa || !elems->he_cap) {
			sdata_info(sdata,
				   "HE 6 GHz AP is missing HE/HE 6 GHz band capability\n");
			return IEEE80211_CONN_MODE_LEGACY;
		}

		if (!eht_oper || !elems->eht_cap) {
			eht_oper = NULL;
			mode = IEEE80211_CONN_MODE_HE;
		}

		if (!ieee80211_chandef_he_6ghz_oper(sdata->local, he_oper,
						    eht_oper, chandef)) {
			sdata_info(sdata, "bad HE/EHT 6 GHz operation\n");
			return IEEE80211_CONN_MODE_LEGACY;
		}

		return mode;
	}

	/* now we have the progression HT, VHT, ... */
	if (conn->mode < IEEE80211_CONN_MODE_HT)
		return IEEE80211_CONN_MODE_LEGACY;

	if (!ht_oper || !elems->ht_cap_elem)
		return IEEE80211_CONN_MODE_LEGACY;

	chandef->width = NL80211_CHAN_WIDTH_20;

	ht_cfreq = ieee80211_channel_to_frequency(ht_oper->primary_chan,
						  channel->band);
	/* check that channel matches the right operating channel */
	if (!ignore_ht_channel_mismatch && channel->center_freq != ht_cfreq) {
		/*
		 * It's possible that some APs are confused here;
		 * Netgear WNDR3700 sometimes reports 4 higher than
		 * the actual channel in association responses, but
		 * since we look at probe response/beacon data here
		 * it should be OK.
		 */
		sdata_info(sdata,
			   "Wrong control channel: center-freq: %d ht-cfreq: %d ht->primary_chan: %d band: %d - Disabling HT\n",
			   channel->center_freq, ht_cfreq,
			   ht_oper->primary_chan, channel->band);
		return IEEE80211_CONN_MODE_LEGACY;
	}

	ieee80211_chandef_ht_oper(ht_oper, chandef);

	if (conn->mode < IEEE80211_CONN_MODE_VHT)
		return IEEE80211_CONN_MODE_HT;

	vht_chandef = *chandef;

	/*
	 * having he_cap/he_oper parsed out implies we're at
	 * least operating as HE STA
	 */
	if (elems->he_cap && he_oper &&
	    he_oper->he_oper_params & cpu_to_le32(IEEE80211_HE_OPERATION_VHT_OPER_INFO)) {
		struct ieee80211_vht_operation he_oper_vht_cap;

		/*
		 * Set only first 3 bytes (other 2 aren't used in
		 * ieee80211_chandef_vht_oper() anyway)
		 */
		memcpy(&he_oper_vht_cap, he_oper->optional, 3);
		he_oper_vht_cap.basic_mcs_set = cpu_to_le16(0);

		if (!ieee80211_chandef_vht_oper(&sdata->local->hw, vht_cap_info,
						&he_oper_vht_cap, ht_oper,
						&vht_chandef)) {
			sdata_info(sdata,
				   "HE AP VHT information is invalid, disabling HE\n");
			/* this will cause us to re-parse as VHT STA */
			return IEEE80211_CONN_MODE_VHT;
		}
	} else if (!vht_oper || !elems->vht_cap_elem) {
		if (sband->band == NL80211_BAND_5GHZ) {
			sdata_info(sdata,
				   "VHT information is missing, disabling VHT\n");
			return IEEE80211_CONN_MODE_HT;
		}
		no_vht = true;
	} else if (sband->band == NL80211_BAND_2GHZ) {
		no_vht = true;
	} else if (!ieee80211_chandef_vht_oper(&sdata->local->hw,
					       vht_cap_info,
					       vht_oper, ht_oper,
					       &vht_chandef)) {
		sdata_info(sdata,
			   "AP VHT information is invalid, disabling VHT\n");
		return IEEE80211_CONN_MODE_HT;
	}

	if (!cfg80211_chandef_compatible(chandef, &vht_chandef)) {
		sdata_info(sdata,
			   "AP VHT information doesn't match HT, disabling VHT\n");
		return IEEE80211_CONN_MODE_HT;
	}

	*chandef = vht_chandef;

	/* stick to current max mode if we or the AP don't have HE */
	if (conn->mode < IEEE80211_CONN_MODE_HE ||
	    !elems->he_operation || !elems->he_cap) {
		if (no_vht)
			return IEEE80211_CONN_MODE_HT;
		return IEEE80211_CONN_MODE_VHT;
	}

	/* stick to HE if we or the AP don't have EHT */
	if (conn->mode < IEEE80211_CONN_MODE_EHT ||
	    !eht_oper || !elems->eht_cap)
		return IEEE80211_CONN_MODE_HE;

	/*
	 * handle the case that the EHT operation indicates that it holds EHT
	 * operation information (in case that the channel width differs from
	 * the channel width reported in HT/VHT/HE).
	 */
	if (eht_oper->params & IEEE80211_EHT_OPER_INFO_PRESENT) {
		struct cfg80211_chan_def eht_chandef = *chandef;

		ieee80211_chandef_eht_oper((const void *)eht_oper->optional,
					   &eht_chandef);

		eht_chandef.punctured =
			ieee80211_eht_oper_dis_subchan_bitmap(eht_oper);

		if (!cfg80211_chandef_valid(&eht_chandef)) {
			sdata_info(sdata,
				   "AP EHT information is invalid, disabling EHT\n");
			return IEEE80211_CONN_MODE_HE;
		}

		if (!cfg80211_chandef_compatible(chandef, &eht_chandef)) {
			sdata_info(sdata,
				   "AP EHT information doesn't match HT/VHT/HE, disabling EHT\n");
			return IEEE80211_CONN_MODE_HE;
		}

		*chandef = eht_chandef;
	}

	return IEEE80211_CONN_MODE_EHT;
}

static bool
ieee80211_verify_sta_ht_mcs_support(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_supported_band *sband,
				    const struct ieee80211_ht_operation *ht_op)
{
	struct ieee80211_sta_ht_cap sta_ht_cap;
	int i;

	if (sband->band == NL80211_BAND_6GHZ)
		return true;

	if (!ht_op)
		return false;

	memcpy(&sta_ht_cap, &sband->ht_cap, sizeof(sta_ht_cap));
	ieee80211_apply_htcap_overrides(sdata, &sta_ht_cap);

	/*
	 * P802.11REVme/D7.0 - 6.5.4.2.4
	 * ...
	 * If the MLME of an HT STA receives an MLME-JOIN.request primitive
	 * with the SelectedBSS parameter containing a Basic HT-MCS Set field
	 * in the HT Operation parameter that contains any unsupported MCSs,
	 * the MLME response in the resulting MLME-JOIN.confirm primitive shall
	 * contain a ResultCode parameter that is not set to the value SUCCESS.
	 * ...
	 */

	/* Simply check that all basic rates are in the STA RX mask */
	for (i = 0; i < IEEE80211_HT_MCS_MASK_LEN; i++) {
		if ((ht_op->basic_set[i] & sta_ht_cap.mcs.rx_mask[i]) !=
		    ht_op->basic_set[i])
			return false;
	}

	return true;
}

static bool
ieee80211_verify_sta_vht_mcs_support(struct ieee80211_sub_if_data *sdata,
				     int link_id,
				     struct ieee80211_supported_band *sband,
				     const struct ieee80211_vht_operation *vht_op)
{
	struct ieee80211_sta_vht_cap sta_vht_cap;
	u16 ap_min_req_set, sta_rx_mcs_map, sta_tx_mcs_map;
	int nss;

	if (sband->band != NL80211_BAND_5GHZ)
		return true;

	if (!vht_op)
		return false;

	memcpy(&sta_vht_cap, &sband->vht_cap, sizeof(sta_vht_cap));
	ieee80211_apply_vhtcap_overrides(sdata, &sta_vht_cap);

	ap_min_req_set = le16_to_cpu(vht_op->basic_mcs_set);
	sta_rx_mcs_map = le16_to_cpu(sta_vht_cap.vht_mcs.rx_mcs_map);
	sta_tx_mcs_map = le16_to_cpu(sta_vht_cap.vht_mcs.tx_mcs_map);

	/*
	 * Many APs are incorrectly advertising an all-zero value here,
	 * which really means MCS 0-7 are required for 1-8 streams, but
	 * they don't really mean it that way.
	 * Some other APs are incorrectly advertising 3 spatial streams
	 * with MCS 0-7 are required, but don't really mean it that way
	 * and we'll connect only with HT, rather than even HE.
	 * As a result, unfortunately the VHT basic MCS/NSS set cannot
	 * be used at all, so check it only in strict mode.
	 */
	if (!ieee80211_hw_check(&sdata->local->hw, STRICT))
		return true;

	/*
	 * P802.11REVme/D7.0 - 6.5.4.2.4
	 * ...
	 * If the MLME of a VHT STA receives an MLME-JOIN.request primitive
	 * with a SelectedBSS parameter containing a Basic VHT-MCS And NSS Set
	 * field in the VHT Operation parameter that contains any unsupported
	 * <VHT-MCS, NSS> tuple, the MLME response in the resulting
	 * MLME-JOIN.confirm primitive shall contain a ResultCode parameter
	 * that is not set to the value SUCCESS.
	 * ...
	 */
	for (nss = 8; nss > 0; nss--) {
		u8 ap_op_val = (ap_min_req_set >> (2 * (nss - 1))) & 3;
		u8 sta_rx_val;
		u8 sta_tx_val;

		if (ap_op_val == IEEE80211_HE_MCS_NOT_SUPPORTED)
			continue;

		sta_rx_val = (sta_rx_mcs_map >> (2 * (nss - 1))) & 3;
		sta_tx_val = (sta_tx_mcs_map >> (2 * (nss - 1))) & 3;

		if (sta_rx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    sta_tx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    sta_rx_val < ap_op_val || sta_tx_val < ap_op_val) {
			link_id_info(sdata, link_id,
				     "Missing mandatory rates for %d Nss, rx %d, tx %d oper %d, disable VHT\n",
				     nss, sta_rx_val, sta_tx_val, ap_op_val);
			return false;
		}
	}

	return true;
}

static bool
ieee80211_verify_peer_he_mcs_support(struct ieee80211_sub_if_data *sdata,
				     int link_id,
				     const struct ieee80211_he_cap_elem *he_cap,
				     const struct ieee80211_he_operation *he_op)
{
	struct ieee80211_he_mcs_nss_supp *he_mcs_nss_supp;
	u16 mcs_80_map_tx, mcs_80_map_rx;
	u16 ap_min_req_set;
	int nss;

	if (!he_cap)
		return false;

	/* mcs_nss is right after he_cap info */
	he_mcs_nss_supp = (void *)(he_cap + 1);

	mcs_80_map_tx = le16_to_cpu(he_mcs_nss_supp->tx_mcs_80);
	mcs_80_map_rx = le16_to_cpu(he_mcs_nss_supp->rx_mcs_80);

	/* P802.11-REVme/D0.3
	 * 27.1.1 Introduction to the HE PHY
	 * ...
	 * An HE STA shall support the following features:
	 * ...
	 * Single spatial stream HE-MCSs 0 to 7 (transmit and receive) in all
	 * supported channel widths for HE SU PPDUs
	 */
	if ((mcs_80_map_tx & 0x3) == IEEE80211_HE_MCS_NOT_SUPPORTED ||
	    (mcs_80_map_rx & 0x3) == IEEE80211_HE_MCS_NOT_SUPPORTED) {
		link_id_info(sdata, link_id,
			     "Missing mandatory rates for 1 Nss, rx 0x%x, tx 0x%x, disable HE\n",
			     mcs_80_map_tx, mcs_80_map_rx);
		return false;
	}

	if (!he_op)
		return true;

	ap_min_req_set = le16_to_cpu(he_op->he_mcs_nss_set);

	/*
	 * Apparently iPhone 13 (at least iOS version 15.3.1) sets this to all
	 * zeroes, which is nonsense, and completely inconsistent with itself
	 * (it doesn't have 8 streams). Accept the settings in this case anyway.
	 */
	if (!ieee80211_hw_check(&sdata->local->hw, STRICT) && !ap_min_req_set)
		return true;

	/* make sure the AP is consistent with itself
	 *
	 * P802.11-REVme/D0.3
	 * 26.17.1 Basic HE BSS operation
	 *
	 * A STA that is operating in an HE BSS shall be able to receive and
	 * transmit at each of the <HE-MCS, NSS> tuple values indicated by the
	 * Basic HE-MCS And NSS Set field of the HE Operation parameter of the
	 * MLME-START.request primitive and shall be able to receive at each of
	 * the <HE-MCS, NSS> tuple values indicated by the Supported HE-MCS and
	 * NSS Set field in the HE Capabilities parameter of the MLMESTART.request
	 * primitive
	 */
	for (nss = 8; nss > 0; nss--) {
		u8 ap_op_val = (ap_min_req_set >> (2 * (nss - 1))) & 3;
		u8 ap_rx_val;
		u8 ap_tx_val;

		if (ap_op_val == IEEE80211_HE_MCS_NOT_SUPPORTED)
			continue;

		ap_rx_val = (mcs_80_map_rx >> (2 * (nss - 1))) & 3;
		ap_tx_val = (mcs_80_map_tx >> (2 * (nss - 1))) & 3;

		if (ap_rx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    ap_tx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
		    ap_rx_val < ap_op_val || ap_tx_val < ap_op_val) {
			link_id_info(sdata, link_id,
				     "Invalid rates for %d Nss, rx %d, tx %d oper %d, disable HE\n",
				     nss, ap_rx_val, ap_tx_val, ap_op_val);
			return false;
		}
	}

	return true;
}

static bool
ieee80211_verify_sta_he_mcs_support(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_supported_band *sband,
				    const struct ieee80211_he_operation *he_op)
{
	const struct ieee80211_sta_he_cap *sta_he_cap =
		ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);
	u16 ap_min_req_set;
	int i;

	if (!sta_he_cap || !he_op)
		return false;

	ap_min_req_set = le16_to_cpu(he_op->he_mcs_nss_set);

	/*
	 * Apparently iPhone 13 (at least iOS version 15.3.1) sets this to all
	 * zeroes, which is nonsense, and completely inconsistent with itself
	 * (it doesn't have 8 streams). Accept the settings in this case anyway.
	 */
	if (!ieee80211_hw_check(&sdata->local->hw, STRICT) && !ap_min_req_set)
		return true;

	/* Need to go over for 80MHz, 160MHz and for 80+80 */
	for (i = 0; i < 3; i++) {
		const struct ieee80211_he_mcs_nss_supp *sta_mcs_nss_supp =
			&sta_he_cap->he_mcs_nss_supp;
		u16 sta_mcs_map_rx =
			le16_to_cpu(((__le16 *)sta_mcs_nss_supp)[2 * i]);
		u16 sta_mcs_map_tx =
			le16_to_cpu(((__le16 *)sta_mcs_nss_supp)[2 * i + 1]);
		u8 nss;
		bool verified = true;

		/*
		 * For each band there is a maximum of 8 spatial streams
		 * possible. Each of the sta_mcs_map_* is a 16-bit struct built
		 * of 2 bits per NSS (1-8), with the values defined in enum
		 * ieee80211_he_mcs_support. Need to make sure STA TX and RX
		 * capabilities aren't less than the AP's minimum requirements
		 * for this HE BSS per SS.
		 * It is enough to find one such band that meets the reqs.
		 */
		for (nss = 8; nss > 0; nss--) {
			u8 sta_rx_val = (sta_mcs_map_rx >> (2 * (nss - 1))) & 3;
			u8 sta_tx_val = (sta_mcs_map_tx >> (2 * (nss - 1))) & 3;
			u8 ap_val = (ap_min_req_set >> (2 * (nss - 1))) & 3;

			if (ap_val == IEEE80211_HE_MCS_NOT_SUPPORTED)
				continue;

			/*
			 * Make sure the HE AP doesn't require MCSs that aren't
			 * supported by the client as required by spec
			 *
			 * P802.11-REVme/D0.3
			 * 26.17.1 Basic HE BSS operation
			 *
			 * An HE STA shall not attempt to join * (MLME-JOIN.request primitive)
			 * a BSS, unless it supports (i.e., is able to both transmit and
			 * receive using) all of the <HE-MCS, NSS> tuples in the basic
			 * HE-MCS and NSS set.
			 */
			if (sta_rx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
			    sta_tx_val == IEEE80211_HE_MCS_NOT_SUPPORTED ||
			    (ap_val > sta_rx_val) || (ap_val > sta_tx_val)) {
				verified = false;
				break;
			}
		}

		if (verified)
			return true;
	}

	/* If here, STA doesn't meet AP's HE min requirements */
	return false;
}

static u8
ieee80211_get_eht_cap_mcs_nss(const struct ieee80211_sta_he_cap *sta_he_cap,
			      const struct ieee80211_sta_eht_cap *sta_eht_cap,
			      unsigned int idx, int bw)
{
	u8 he_phy_cap0 = sta_he_cap->he_cap_elem.phy_cap_info[0];
	u8 eht_phy_cap0 = sta_eht_cap->eht_cap_elem.phy_cap_info[0];

	/* handle us being a 20 MHz-only EHT STA - with four values
	 * for MCS 0-7, 8-9, 10-11, 12-13.
	 */
	if (!(he_phy_cap0 & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK_ALL))
		return sta_eht_cap->eht_mcs_nss_supp.only_20mhz.rx_tx_max_nss[idx];

	/* the others have MCS 0-9 together, rather than separately from 0-7 */
	if (idx > 0)
		idx--;

	switch (bw) {
	case 0:
		return sta_eht_cap->eht_mcs_nss_supp.bw._80.rx_tx_max_nss[idx];
	case 1:
		if (!(he_phy_cap0 &
		      (IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G |
		       IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G)))
			return 0xff; /* pass check */
		return sta_eht_cap->eht_mcs_nss_supp.bw._160.rx_tx_max_nss[idx];
	case 2:
		if (!(eht_phy_cap0 & IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ))
			return 0xff; /* pass check */
		return sta_eht_cap->eht_mcs_nss_supp.bw._320.rx_tx_max_nss[idx];
	}

	WARN_ON(1);
	return 0;
}

static bool
ieee80211_verify_sta_eht_mcs_support(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_supported_band *sband,
				     const struct ieee80211_eht_operation *eht_op)
{
	const struct ieee80211_sta_he_cap *sta_he_cap =
		ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);
	const struct ieee80211_sta_eht_cap *sta_eht_cap =
		ieee80211_get_eht_iftype_cap_vif(sband, &sdata->vif);
	const struct ieee80211_eht_mcs_nss_supp_20mhz_only *req;
	unsigned int i;

	if (!sta_he_cap || !sta_eht_cap || !eht_op)
		return false;

	req = &eht_op->basic_mcs_nss;

	for (i = 0; i < ARRAY_SIZE(req->rx_tx_max_nss); i++) {
		u8 req_rx_nss, req_tx_nss;
		unsigned int bw;

		req_rx_nss = u8_get_bits(req->rx_tx_max_nss[i],
					 IEEE80211_EHT_MCS_NSS_RX);
		req_tx_nss = u8_get_bits(req->rx_tx_max_nss[i],
					 IEEE80211_EHT_MCS_NSS_TX);

		for (bw = 0; bw < 3; bw++) {
			u8 have, have_rx_nss, have_tx_nss;

			have = ieee80211_get_eht_cap_mcs_nss(sta_he_cap,
							     sta_eht_cap,
							     i, bw);
			have_rx_nss = u8_get_bits(have,
						  IEEE80211_EHT_MCS_NSS_RX);
			have_tx_nss = u8_get_bits(have,
						  IEEE80211_EHT_MCS_NSS_TX);

			if (req_rx_nss > have_rx_nss ||
			    req_tx_nss > have_tx_nss)
				return false;
		}
	}

	return true;
}

static void ieee80211_get_rates(struct ieee80211_supported_band *sband,
				const u8 *supp_rates,
				unsigned int supp_rates_len,
				const u8 *ext_supp_rates,
				unsigned int ext_supp_rates_len,
				u32 *rates, u32 *basic_rates,
				unsigned long *unknown_rates_selectors,
				bool *have_higher_than_11mbit,
				int *min_rate, int *min_rate_index)
{
	int i, j;

	for (i = 0; i < supp_rates_len + ext_supp_rates_len; i++) {
		u8 supp_rate = i < supp_rates_len ?
				supp_rates[i] :
				ext_supp_rates[i - supp_rates_len];
		int rate = supp_rate & 0x7f;
		bool is_basic = !!(supp_rate & 0x80);

		if ((rate * 5) > 110 && have_higher_than_11mbit)
			*have_higher_than_11mbit = true;

		/*
		 * Skip membership selectors since they're not rates.
		 *
		 * Note: Even though the membership selector and the basic
		 *	 rate flag share the same bit, they are not exactly
		 *	 the same.
		 */
		if (is_basic && rate >= BSS_MEMBERSHIP_SELECTOR_MIN) {
			if (unknown_rates_selectors)
				set_bit(rate, unknown_rates_selectors);
			continue;
		}

		for (j = 0; j < sband->n_bitrates; j++) {
			struct ieee80211_rate *br;
			int brate;

			br = &sband->bitrates[j];

			brate = DIV_ROUND_UP(br->bitrate, 5);
			if (brate == rate) {
				if (rates)
					*rates |= BIT(j);
				if (is_basic && basic_rates)
					*basic_rates |= BIT(j);
				if (min_rate && (rate * 5) < *min_rate) {
					*min_rate = rate * 5;
					if (min_rate_index)
						*min_rate_index = j;
				}
				break;
			}
		}

		/* Handle an unknown entry as if it is an unknown selector */
		if (is_basic && unknown_rates_selectors && j == sband->n_bitrates)
			set_bit(rate, unknown_rates_selectors);
	}
}

static bool ieee80211_chandef_usable(struct ieee80211_sub_if_data *sdata,
				     const struct cfg80211_chan_def *chandef,
				     u32 prohibited_flags)
{
	if (!cfg80211_chandef_usable(sdata->local->hw.wiphy,
				     chandef, prohibited_flags))
		return false;

	if (chandef->punctured &&
	    ieee80211_hw_check(&sdata->local->hw, DISALLOW_PUNCTURING))
		return false;

	if (chandef->punctured && chandef->chan->band == NL80211_BAND_5GHZ &&
	    ieee80211_hw_check(&sdata->local->hw, DISALLOW_PUNCTURING_5GHZ))
		return false;

	return true;
}

static int ieee80211_chandef_num_subchans(const struct cfg80211_chan_def *c)
{
	if (c->width == NL80211_CHAN_WIDTH_80P80)
		return 4 + 4;

	return cfg80211_chandef_get_width(c) / 20;
}

static int ieee80211_chandef_num_widths(const struct cfg80211_chan_def *c)
{
	switch (c->width) {
	case NL80211_CHAN_WIDTH_20:
	case NL80211_CHAN_WIDTH_20_NOHT:
		return 1;
	case NL80211_CHAN_WIDTH_40:
		return 2;
	case NL80211_CHAN_WIDTH_80P80:
	case NL80211_CHAN_WIDTH_80:
		return 3;
	case NL80211_CHAN_WIDTH_160:
		return 4;
	case NL80211_CHAN_WIDTH_320:
		return 5;
	default:
		WARN_ON(1);
		return 0;
	}
}

VISIBLE_IF_MAC80211_KUNIT int
ieee80211_calc_chandef_subchan_offset(const struct cfg80211_chan_def *ap,
				      u8 n_partial_subchans)
{
	int n = ieee80211_chandef_num_subchans(ap);
	struct cfg80211_chan_def tmp = *ap;
	int offset = 0;

	/*
	 * Given a chandef (in this context, it's the AP's) and a number
	 * of subchannels that we want to look at ('n_partial_subchans'),
	 * calculate the offset in number of subchannels between the full
	 * and the subset with the desired width.
	 */

	/* same number of subchannels means no offset, obviously */
	if (n == n_partial_subchans)
		return 0;

	/* don't WARN - misconfigured APs could cause this if their N > width */
	if (n < n_partial_subchans)
		return 0;

	while (ieee80211_chandef_num_subchans(&tmp) > n_partial_subchans) {
		u32 prev = tmp.center_freq1;

		ieee80211_chandef_downgrade(&tmp, NULL);

		/*
		 * if center_freq moved up, half the original channels
		 * are gone now but were below, so increase offset
		 */
		if (prev < tmp.center_freq1)
			offset += ieee80211_chandef_num_subchans(&tmp);
	}

	/*
	 * 80+80 with secondary 80 below primary - four subchannels for it
	 * (we cannot downgrade *to* 80+80, so no need to consider 'tmp')
	 */
	if (ap->width == NL80211_CHAN_WIDTH_80P80 &&
	    ap->center_freq2 < ap->center_freq1)
		offset += 4;

	return offset;
}
EXPORT_SYMBOL_IF_MAC80211_KUNIT(ieee80211_calc_chandef_subchan_offset);

VISIBLE_IF_MAC80211_KUNIT void
ieee80211_rearrange_tpe_psd(struct ieee80211_parsed_tpe_psd *psd,
			    const struct cfg80211_chan_def *ap,
			    const struct cfg80211_chan_def *used)
{
	u8 needed = ieee80211_chandef_num_subchans(used);
	u8 have = ieee80211_chandef_num_subchans(ap);
	u8 tmp[IEEE80211_TPE_PSD_ENTRIES_320MHZ];
	u8 offset;

	if (!psd->valid)
		return;

	/* if N is zero, all defaults were used, no point in rearranging */
	if (!psd->n)
		goto out;

	BUILD_BUG_ON(sizeof(tmp) != sizeof(psd->power));

	/*
	 * This assumes that 'N' is consistent with the HE channel, as
	 * it should be (otherwise the AP is broken).
	 *
	 * In psd->power we have values in the order 0..N, 0..K, where
	 * N+K should cover the entire channel per 'ap', but even if it
	 * doesn't then we've pre-filled 'unlimited' as defaults.
	 *
	 * But this is all the wrong order, we want to have them in the
	 * order of the 'used' channel.
	 *
	 * So for example, we could have a 320 MHz EHT AP, which has the
	 * HE channel as 80 MHz (e.g. due to puncturing, which doesn't
	 * seem to be considered for the TPE), as follows:
	 *
	 * EHT  320:   |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
	 * HE    80:                           |  |  |  |  |
	 * used 160:                           |  |  |  |  |  |  |  |  |
	 *
	 * N entries:                          |--|--|--|--|
	 * K entries:  |--|--|--|--|--|--|--|--|           |--|--|--|--|
	 * power idx:   4  5  6  7  8  9  10 11 0  1  2  3  12 13 14 15
	 * full chan:   0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
	 * used chan:                           0  1  2  3  4  5  6  7
	 *
	 * The idx in the power array ('power idx') is like this since it
	 * comes directly from the element's N and K entries in their
	 * element order, and those are this way for HE compatibility.
	 *
	 * Rearrange them as desired here, first by putting them into the
	 * 'full chan' order, and then selecting the necessary subset for
	 * the 'used chan'.
	 */

	/* first reorder according to AP channel */
	offset = ieee80211_calc_chandef_subchan_offset(ap, psd->n);
	for (int i = 0; i < have; i++) {
		if (i < offset)
			tmp[i] = psd->power[i + psd->n];
		else if (i < offset + psd->n)
			tmp[i] = psd->power[i - offset];
		else
			tmp[i] = psd->power[i];
	}

	/*
	 * and then select the subset for the used channel
	 * (set everything to defaults first in case a driver is confused)
	 */
	memset(psd->power, IEEE80211_TPE_PSD_NO_LIMIT, sizeof(psd->power));
	offset = ieee80211_calc_chandef_subchan_offset(ap, needed);
	for (int i = 0; i < needed; i++)
		psd->power[i] = tmp[offset + i];

out:
	/* limit, but don't lie if there are defaults in the data */
	if (needed < psd->count)
		psd->count = needed;
}
EXPORT_SYMBOL_IF_MAC80211_KUNIT(ieee80211_rearrange_tpe_psd);

static void ieee80211_rearrange_tpe(struct ieee80211_parsed_tpe *tpe,
				    const struct cfg80211_chan_def *ap,
				    const struct cfg80211_chan_def *used)
{
	/* ignore this completely for narrow/invalid channels */
	if (!ieee80211_chandef_num_subchans(ap) ||
	    !ieee80211_chandef_num_subchans(used)) {
		ieee80211_clear_tpe(tpe);
		return;
	}

	for (int i = 0; i < 2; i++) {
		int needed_pwr_count;

		ieee80211_rearrange_tpe_psd(&tpe->psd_local[i], ap, used);
		ieee80211_rearrange_tpe_psd(&tpe->psd_reg_client[i], ap, used);

		/* limit this to the widths we actually need */
		needed_pwr_count = ieee80211_chandef_num_widths(used);
		if (needed_pwr_count < tpe->max_local[i].count)
			tpe->max_local[i].count = needed_pwr_count;
		if (needed_pwr_count < tpe->max_reg_client[i].count)
			tpe->max_reg_client[i].count = needed_pwr_count;
	}
}

/*
 * The AP part of the channel request is used to distinguish settings
 * to the device used for wider bandwidth OFDMA. This is used in the
 * channel context code to assign two channel contexts even if they're
 * both for the same channel, if the AP bandwidths are incompatible.
 * If not EHT (or driver override) then ap.chan == NULL indicates that
 * there's no wider BW OFDMA used.
 */
static void ieee80211_set_chanreq_ap(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_chan_req *chanreq,
				     struct ieee80211_conn_settings *conn,
				     struct cfg80211_chan_def *ap_chandef)
{
	chanreq->ap.chan = NULL;

	if (conn->mode < IEEE80211_CONN_MODE_EHT)
		return;
	if (sdata->vif.driver_flags & IEEE80211_VIF_IGNORE_OFDMA_WIDER_BW)
		return;

	chanreq->ap = *ap_chandef;
}

VISIBLE_IF_MAC80211_KUNIT struct ieee802_11_elems *
ieee80211_determine_chan_mode(struct ieee80211_sub_if_data *sdata,
			      struct ieee80211_conn_settings *conn,
			      struct cfg80211_bss *cbss, int link_id,
			      struct ieee80211_chan_req *chanreq,
			      struct cfg80211_chan_def *ap_chandef,
			      unsigned long *userspace_selectors)
{
	const struct cfg80211_bss_ies *ies = rcu_dereference(cbss->ies);
	struct ieee80211_bss *bss = (void *)cbss->priv;
	struct ieee80211_channel *channel = cbss->channel;
	struct ieee80211_elems_parse_params parse_params = {
		.link_id = -1,
		.from_ap = true,
		.start = ies->data,
		.len = ies->len,
	};
	struct ieee802_11_elems *elems;
	struct ieee80211_supported_band *sband;
	enum ieee80211_conn_mode ap_mode;
	unsigned long unknown_rates_selectors[BITS_TO_LONGS(128)] = {};
	unsigned long sta_selectors[BITS_TO_LONGS(128)] = {};
	int ret;

again:
	parse_params.mode = conn->mode;
	elems = ieee802_11_parse_elems_full(&parse_params);
	if (!elems)
		return ERR_PTR(-ENOMEM);

	ap_mode = ieee80211_determine_ap_chan(sdata, channel, bss->vht_cap_info,
					      elems, false, conn, ap_chandef);

	/* this should be impossible since parsing depends on our mode */
	if (WARN_ON(ap_mode > conn->mode)) {
		ret = -EINVAL;
		goto free;
	}

	if (conn->mode != ap_mode) {
		conn->mode = ap_mode;
		kfree(elems);
		goto again;
	}

	mlme_link_id_dbg(sdata, link_id, "determined AP %pM to be %s\n",
			 cbss->bssid, ieee80211_conn_mode_str(ap_mode));

	sband = sdata->local->hw.wiphy->bands[channel->band];

	ieee80211_get_rates(sband, elems->supp_rates, elems->supp_rates_len,
			    elems->ext_supp_rates, elems->ext_supp_rates_len,
			    NULL, NULL, unknown_rates_selectors, NULL, NULL,
			    NULL);

	switch (channel->band) {
	case NL80211_BAND_S1GHZ:
		if (WARN_ON(ap_mode != IEEE80211_CONN_MODE_S1G)) {
			ret = -EINVAL;
			goto free;
		}
		return elems;
	case NL80211_BAND_6GHZ:
		if (ap_mode < IEEE80211_CONN_MODE_HE) {
			link_id_info(sdata, link_id,
				     "Rejecting non-HE 6/7 GHz connection");
			ret = -EINVAL;
			goto free;
		}
		break;
	default:
		if (WARN_ON(ap_mode == IEEE80211_CONN_MODE_S1G)) {
			ret = -EINVAL;
			goto free;
		}
	}

	switch (ap_mode) {
	case IEEE80211_CONN_MODE_S1G:
		WARN_ON(1);
		ret = -EINVAL;
		goto free;
	case IEEE80211_CONN_MODE_LEGACY:
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;
		break;
	case IEEE80211_CONN_MODE_HT:
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_40);
		break;
	case IEEE80211_CONN_MODE_VHT:
	case IEEE80211_CONN_MODE_HE:
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_160);
		break;
	case IEEE80211_CONN_MODE_EHT:
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_320);
		break;
	}

	chanreq->oper = *ap_chandef;

	bitmap_copy(sta_selectors, userspace_selectors, 128);
	if (conn->mode >= IEEE80211_CONN_MODE_HT)
		set_bit(BSS_MEMBERSHIP_SELECTOR_HT_PHY, sta_selectors);
	if (conn->mode >= IEEE80211_CONN_MODE_VHT)
		set_bit(BSS_MEMBERSHIP_SELECTOR_VHT_PHY, sta_selectors);
	if (conn->mode >= IEEE80211_CONN_MODE_HE)
		set_bit(BSS_MEMBERSHIP_SELECTOR_HE_PHY, sta_selectors);
	if (conn->mode >= IEEE80211_CONN_MODE_EHT)
		set_bit(BSS_MEMBERSHIP_SELECTOR_EHT_PHY, sta_selectors);

	/*
	 * We do not support EPD or GLK so never add them.
	 * SAE_H2E is handled through userspace_selectors.
	 */

	/* Check if we support all required features */
	if (!bitmap_subset(unknown_rates_selectors, sta_selectors, 128)) {
		link_id_info(sdata, link_id,
			     "required basic rate or BSS membership selectors not supported or disabled, rejecting connection\n");
		ret = -EINVAL;
		goto free;
	}

	ieee80211_set_chanreq_ap(sdata, chanreq, conn, ap_chandef);

	while (!ieee80211_chandef_usable(sdata, &chanreq->oper,
					 IEEE80211_CHAN_DISABLED)) {
		if (WARN_ON(chanreq->oper.width == NL80211_CHAN_WIDTH_20_NOHT)) {
			ret = -EINVAL;
			goto free;
		}

		ieee80211_chanreq_downgrade(chanreq, conn);
	}

	if (conn->mode >= IEEE80211_CONN_MODE_HE &&
	    !cfg80211_chandef_usable(sdata->wdev.wiphy, &chanreq->oper,
				     IEEE80211_CHAN_NO_HE)) {
		conn->mode = IEEE80211_CONN_MODE_VHT;
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_160);
	}

	if (conn->mode >= IEEE80211_CONN_MODE_EHT &&
	    !cfg80211_chandef_usable(sdata->wdev.wiphy, &chanreq->oper,
				     IEEE80211_CHAN_NO_EHT)) {
		conn->mode = IEEE80211_CONN_MODE_HE;
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_160);
	}

	if (chanreq->oper.width != ap_chandef->width || ap_mode != conn->mode)
		link_id_info(sdata, link_id,
			     "regulatory prevented using AP config, downgraded\n");

	if (conn->mode >= IEEE80211_CONN_MODE_HT &&
	    !ieee80211_verify_sta_ht_mcs_support(sdata, sband,
						 elems->ht_operation)) {
		conn->mode = IEEE80211_CONN_MODE_LEGACY;
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;
		link_id_info(sdata, link_id,
			     "required MCSes not supported, disabling HT\n");
	}

	if (conn->mode >= IEEE80211_CONN_MODE_VHT &&
	    !ieee80211_verify_sta_vht_mcs_support(sdata, link_id, sband,
						  elems->vht_operation)) {
		conn->mode = IEEE80211_CONN_MODE_HT;
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_40);
		link_id_info(sdata, link_id,
			     "required MCSes not supported, disabling VHT\n");
	}

	if (conn->mode >= IEEE80211_CONN_MODE_HE &&
	    (!ieee80211_verify_peer_he_mcs_support(sdata, link_id,
						   (void *)elems->he_cap,
						   elems->he_operation) ||
	     !ieee80211_verify_sta_he_mcs_support(sdata, sband,
						  elems->he_operation))) {
		conn->mode = IEEE80211_CONN_MODE_VHT;
		link_id_info(sdata, link_id,
			     "required MCSes not supported, disabling HE\n");
	}

	if (conn->mode >= IEEE80211_CONN_MODE_EHT &&
	    !ieee80211_verify_sta_eht_mcs_support(sdata, sband,
						  elems->eht_operation)) {
		conn->mode = IEEE80211_CONN_MODE_HE;
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_160);
		link_id_info(sdata, link_id,
			     "required MCSes not supported, disabling EHT\n");
	}

	/* the mode can only decrease, so this must terminate */
	if (ap_mode != conn->mode) {
		kfree(elems);
		goto again;
	}

	mlme_link_id_dbg(sdata, link_id,
			 "connecting with %s mode, max bandwidth %d MHz\n",
			 ieee80211_conn_mode_str(conn->mode),
			 20 * (1 << conn->bw_limit));

	if (WARN_ON_ONCE(!cfg80211_chandef_valid(&chanreq->oper))) {
		ret = -EINVAL;
		goto free;
	}

	return elems;
free:
	kfree(elems);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_IF_MAC80211_KUNIT(ieee80211_determine_chan_mode);

static int ieee80211_config_bw(struct ieee80211_link_data *link,
			       struct ieee802_11_elems *elems,
			       bool update, u64 *changed,
			       const char *frame)
{
	struct ieee80211_channel *channel = link->conf->chanreq.oper.chan;
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_chan_req chanreq = {};
	struct cfg80211_chan_def ap_chandef;
	enum ieee80211_conn_mode ap_mode;
	u32 vht_cap_info = 0;
	u16 ht_opmode;
	int ret;

	/* don't track any bandwidth changes in legacy/S1G modes */
	if (link->u.mgd.conn.mode == IEEE80211_CONN_MODE_LEGACY ||
	    link->u.mgd.conn.mode == IEEE80211_CONN_MODE_S1G)
		return 0;

	if (elems->vht_cap_elem)
		vht_cap_info = le32_to_cpu(elems->vht_cap_elem->vht_cap_info);

	ap_mode = ieee80211_determine_ap_chan(sdata, channel, vht_cap_info,
					      elems, true, &link->u.mgd.conn,
					      &ap_chandef);

	if (ap_mode != link->u.mgd.conn.mode) {
		link_info(link,
			  "AP %pM appears to change mode (expected %s, found %s) in %s, disconnect\n",
			  link->u.mgd.bssid,
			  ieee80211_conn_mode_str(link->u.mgd.conn.mode),
			  ieee80211_conn_mode_str(ap_mode), frame);
		return -EINVAL;
	}

	chanreq.oper = ap_chandef;
	ieee80211_set_chanreq_ap(sdata, &chanreq, &link->u.mgd.conn,
				 &ap_chandef);

	/*
	 * if HT operation mode changed store the new one -
	 * this may be applicable even if channel is identical
	 */
	if (elems->ht_operation) {
		ht_opmode = le16_to_cpu(elems->ht_operation->operation_mode);
		if (link->conf->ht_operation_mode != ht_opmode) {
			*changed |= BSS_CHANGED_HT;
			link->conf->ht_operation_mode = ht_opmode;
		}
	}

	/*
	 * Downgrade the new channel if we associated with restricted
	 * bandwidth capabilities. For example, if we associated as a
	 * 20 MHz STA to a 40 MHz AP (due to regulatory, capabilities
	 * or config reasons) then switching to a 40 MHz channel now
	 * won't do us any good -- we couldn't use it with the AP.
	 */
	while (link->u.mgd.conn.bw_limit <
			ieee80211_min_bw_limit_from_chandef(&chanreq.oper))
		ieee80211_chandef_downgrade(&chanreq.oper, NULL);

	if (ap_chandef.chan->band == NL80211_BAND_6GHZ &&
	    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HE) {
		ieee80211_rearrange_tpe(&elems->tpe, &ap_chandef,
					&chanreq.oper);
		if (memcmp(&link->conf->tpe, &elems->tpe, sizeof(elems->tpe))) {
			link->conf->tpe = elems->tpe;
			*changed |= BSS_CHANGED_TPE;
		}
	}

	if (ieee80211_chanreq_identical(&chanreq, &link->conf->chanreq))
		return 0;

	link_info(link,
		  "AP %pM changed bandwidth in %s, new used config is %d.%03d MHz, width %d (%d.%03d/%d MHz)\n",
		  link->u.mgd.bssid, frame, chanreq.oper.chan->center_freq,
		  chanreq.oper.chan->freq_offset, chanreq.oper.width,
		  chanreq.oper.center_freq1, chanreq.oper.freq1_offset,
		  chanreq.oper.center_freq2);

	if (!cfg80211_chandef_valid(&chanreq.oper)) {
		sdata_info(sdata,
			   "AP %pM changed caps/bw in %s in a way we can't support - disconnect\n",
			   link->u.mgd.bssid, frame);
		return -EINVAL;
	}

	if (!update) {
		link->conf->chanreq = chanreq;
		return 0;
	}

	/*
	 * We're tracking the current AP here, so don't do any further checks
	 * here. This keeps us from playing ping-pong with regulatory, without
	 * it the following can happen (for example):
	 *  - connect to an AP with 80 MHz, world regdom allows 80 MHz
	 *  - AP advertises regdom US
	 *  - CRDA loads regdom US with 80 MHz prohibited (old database)
	 *  - we detect an unsupported channel and disconnect
	 *  - disconnect causes CRDA to reload world regdomain and the game
	 *    starts anew.
	 * (see https://bugzilla.kernel.org/show_bug.cgi?id=70881)
	 *
	 * It seems possible that there are still scenarios with CSA or real
	 * bandwidth changes where a this could happen, but those cases are
	 * less common and wouldn't completely prevent using the AP.
	 */

	ret = ieee80211_link_change_chanreq(link, &chanreq, changed);
	if (ret) {
		sdata_info(sdata,
			   "AP %pM changed bandwidth in %s to incompatible one - disconnect\n",
			   link->u.mgd.bssid, frame);
		return ret;
	}

	cfg80211_schedule_channels_check(&sdata->wdev);
	return 0;
}

/* frame sending functions */

static void ieee80211_add_ht_ie(struct ieee80211_sub_if_data *sdata,
				struct sk_buff *skb, u8 ap_ht_param,
				struct ieee80211_supported_band *sband,
				struct ieee80211_channel *channel,
				enum ieee80211_smps_mode smps,
				const struct ieee80211_conn_settings *conn)
{
	u8 *pos;
	u32 flags = channel->flags;
	u16 cap;
	struct ieee80211_sta_ht_cap ht_cap;

	BUILD_BUG_ON(sizeof(ht_cap) != sizeof(sband->ht_cap));

	memcpy(&ht_cap, &sband->ht_cap, sizeof(ht_cap));
	ieee80211_apply_htcap_overrides(sdata, &ht_cap);

	/* determine capability flags */
	cap = ht_cap.cap;

	switch (ap_ht_param & IEEE80211_HT_PARAM_CHA_SEC_OFFSET) {
	case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
		if (flags & IEEE80211_CHAN_NO_HT40PLUS) {
			cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
			cap &= ~IEEE80211_HT_CAP_SGI_40;
		}
		break;
	case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
		if (flags & IEEE80211_CHAN_NO_HT40MINUS) {
			cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
			cap &= ~IEEE80211_HT_CAP_SGI_40;
		}
		break;
	}

	/*
	 * If 40 MHz was disabled associate as though we weren't
	 * capable of 40 MHz -- some broken APs will never fall
	 * back to trying to transmit in 20 MHz.
	 */
	if (conn->bw_limit <= IEEE80211_CONN_BW_LIMIT_20) {
		cap &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
		cap &= ~IEEE80211_HT_CAP_SGI_40;
	}

	/* set SM PS mode properly */
	cap &= ~IEEE80211_HT_CAP_SM_PS;
	switch (smps) {
	case IEEE80211_SMPS_AUTOMATIC:
	case IEEE80211_SMPS_NUM_MODES:
		WARN_ON(1);
		fallthrough;
	case IEEE80211_SMPS_OFF:
		cap |= WLAN_HT_CAP_SM_PS_DISABLED <<
			IEEE80211_HT_CAP_SM_PS_SHIFT;
		break;
	case IEEE80211_SMPS_STATIC:
		cap |= WLAN_HT_CAP_SM_PS_STATIC <<
			IEEE80211_HT_CAP_SM_PS_SHIFT;
		break;
	case IEEE80211_SMPS_DYNAMIC:
		cap |= WLAN_HT_CAP_SM_PS_DYNAMIC <<
			IEEE80211_HT_CAP_SM_PS_SHIFT;
		break;
	}

	/* reserve and fill IE */
	pos = skb_put(skb, sizeof(struct ieee80211_ht_cap) + 2);
	ieee80211_ie_build_ht_cap(pos, &ht_cap, cap);
}

/* This function determines vht capability flags for the association
 * and builds the IE.
 * Note - the function returns true to own the MU-MIMO capability
 */
static bool ieee80211_add_vht_ie(struct ieee80211_sub_if_data *sdata,
				 struct sk_buff *skb,
				 struct ieee80211_supported_band *sband,
				 struct ieee80211_vht_cap *ap_vht_cap,
				 const struct ieee80211_conn_settings *conn)
{
	struct ieee80211_local *local = sdata->local;
	u8 *pos;
	u32 cap;
	struct ieee80211_sta_vht_cap vht_cap;
	u32 mask, ap_bf_sts, our_bf_sts;
	bool mu_mimo_owner = false;

	BUILD_BUG_ON(sizeof(vht_cap) != sizeof(sband->vht_cap));

	memcpy(&vht_cap, &sband->vht_cap, sizeof(vht_cap));
	ieee80211_apply_vhtcap_overrides(sdata, &vht_cap);

	/* determine capability flags */
	cap = vht_cap.cap;

	if (conn->bw_limit <= IEEE80211_CONN_BW_LIMIT_80) {
		cap &= ~IEEE80211_VHT_CAP_SHORT_GI_160;
		cap &= ~IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK;
	}

	/*
	 * Some APs apparently get confused if our capabilities are better
	 * than theirs, so restrict what we advertise in the assoc request.
	 */
	if (!ieee80211_hw_check(&local->hw, STRICT)) {
		if (!(ap_vht_cap->vht_cap_info &
				cpu_to_le32(IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE)))
			cap &= ~(IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
				 IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE);
		else if (!(ap_vht_cap->vht_cap_info &
				cpu_to_le32(IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE)))
			cap &= ~IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE;
	}

	/*
	 * If some other vif is using the MU-MIMO capability we cannot associate
	 * using MU-MIMO - this will lead to contradictions in the group-id
	 * mechanism.
	 * Ownership is defined since association request, in order to avoid
	 * simultaneous associations with MU-MIMO.
	 */
	if (cap & IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE) {
		bool disable_mu_mimo = false;
		struct ieee80211_sub_if_data *other;

		list_for_each_entry(other, &local->interfaces, list) {
			if (other->vif.bss_conf.mu_mimo_owner) {
				disable_mu_mimo = true;
				break;
			}
		}
		if (disable_mu_mimo)
			cap &= ~IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE;
		else
			mu_mimo_owner = true;
	}

	mask = IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK;

	ap_bf_sts = le32_to_cpu(ap_vht_cap->vht_cap_info) & mask;
	our_bf_sts = cap & mask;

	if (ap_bf_sts < our_bf_sts) {
		cap &= ~mask;
		cap |= ap_bf_sts;
	}

	/* reserve and fill IE */
	pos = skb_put(skb, sizeof(struct ieee80211_vht_cap) + 2);
	ieee80211_ie_build_vht_cap(pos, &vht_cap, cap);

	return mu_mimo_owner;
}

static void ieee80211_assoc_add_rates(struct ieee80211_local *local,
				      struct sk_buff *skb,
				      enum nl80211_chan_width width,
				      struct ieee80211_supported_band *sband,
				      struct ieee80211_mgd_assoc_data *assoc_data)
{
	u32 rates;

	if (assoc_data->supp_rates_len &&
	    !ieee80211_hw_check(&local->hw, STRICT)) {
		/*
		 * Get all rates supported by the device and the AP as
		 * some APs don't like getting a superset of their rates
		 * in the association request (e.g. D-Link DAP 1353 in
		 * b-only mode)...
		 */
		ieee80211_parse_bitrates(width, sband,
					 assoc_data->supp_rates,
					 assoc_data->supp_rates_len,
					 &rates);
	} else {
		/*
		 * In case AP not provide any supported rates information
		 * before association, we send information element(s) with
		 * all rates that we support.
		 */
		rates = ~0;
	}

	ieee80211_put_srates_elem(skb, sband, 0, ~rates,
				  WLAN_EID_SUPP_RATES);
	ieee80211_put_srates_elem(skb, sband, 0, ~rates,
				  WLAN_EID_EXT_SUPP_RATES);
}

static size_t ieee80211_add_before_ht_elems(struct sk_buff *skb,
					    const u8 *elems,
					    size_t elems_len,
					    size_t offset)
{
	size_t noffset;

	static const u8 before_ht[] = {
		WLAN_EID_SSID,
		WLAN_EID_SUPP_RATES,
		WLAN_EID_EXT_SUPP_RATES,
		WLAN_EID_PWR_CAPABILITY,
		WLAN_EID_SUPPORTED_CHANNELS,
		WLAN_EID_RSN,
		WLAN_EID_QOS_CAPA,
		WLAN_EID_RRM_ENABLED_CAPABILITIES,
		WLAN_EID_MOBILITY_DOMAIN,
		WLAN_EID_FAST_BSS_TRANSITION,	/* reassoc only */
		WLAN_EID_RIC_DATA,		/* reassoc only */
		WLAN_EID_SUPPORTED_REGULATORY_CLASSES,
	};
	static const u8 after_ric[] = {
		WLAN_EID_SUPPORTED_REGULATORY_CLASSES,
		WLAN_EID_HT_CAPABILITY,
		WLAN_EID_BSS_COEX_2040,
		/* luckily this is almost always there */
		WLAN_EID_EXT_CAPABILITY,
		WLAN_EID_QOS_TRAFFIC_CAPA,
		WLAN_EID_TIM_BCAST_REQ,
		WLAN_EID_INTERWORKING,
		/* 60 GHz (Multi-band, DMG, MMS) can't happen */
		WLAN_EID_VHT_CAPABILITY,
		WLAN_EID_OPMODE_NOTIF,
	};

	if (!elems_len)
		return offset;

	noffset = ieee80211_ie_split_ric(elems, elems_len,
					 before_ht,
					 ARRAY_SIZE(before_ht),
					 after_ric,
					 ARRAY_SIZE(after_ric),
					 offset);
	skb_put_data(skb, elems + offset, noffset - offset);

	return noffset;
}

static size_t ieee80211_add_before_vht_elems(struct sk_buff *skb,
					     const u8 *elems,
					     size_t elems_len,
					     size_t offset)
{
	static const u8 before_vht[] = {
		/*
		 * no need to list the ones split off before HT
		 * or generated here
		 */
		WLAN_EID_BSS_COEX_2040,
		WLAN_EID_EXT_CAPABILITY,
		WLAN_EID_QOS_TRAFFIC_CAPA,
		WLAN_EID_TIM_BCAST_REQ,
		WLAN_EID_INTERWORKING,
		/* 60 GHz (Multi-band, DMG, MMS) can't happen */
	};
	size_t noffset;

	if (!elems_len)
		return offset;

	/* RIC already taken care of in ieee80211_add_before_ht_elems() */
	noffset = ieee80211_ie_split(elems, elems_len,
				     before_vht, ARRAY_SIZE(before_vht),
				     offset);
	skb_put_data(skb, elems + offset, noffset - offset);

	return noffset;
}

static size_t ieee80211_add_before_he_elems(struct sk_buff *skb,
					    const u8 *elems,
					    size_t elems_len,
					    size_t offset)
{
	static const u8 before_he[] = {
		/*
		 * no need to list the ones split off before VHT
		 * or generated here
		 */
		WLAN_EID_OPMODE_NOTIF,
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FUTURE_CHAN_GUIDANCE,
		/* 11ai elements */
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FILS_SESSION,
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FILS_PUBLIC_KEY,
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FILS_KEY_CONFIRM,
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FILS_HLP_CONTAINER,
		WLAN_EID_EXTENSION, WLAN_EID_EXT_FILS_IP_ADDR_ASSIGN,
		/* TODO: add 11ah/11aj/11ak elements */
	};
	size_t noffset;

	if (!elems_len)
		return offset;

	/* RIC already taken care of in ieee80211_add_before_ht_elems() */
	noffset = ieee80211_ie_split(elems, elems_len,
				     before_he, ARRAY_SIZE(before_he),
				     offset);
	skb_put_data(skb, elems + offset, noffset - offset);

	return noffset;
}

#define PRESENT_ELEMS_MAX	8
#define PRESENT_ELEM_EXT_OFFS	0x100

static void
ieee80211_assoc_add_ml_elem(struct ieee80211_sub_if_data *sdata,
			    struct sk_buff *skb, u16 capab,
			    const struct element *ext_capa,
			    const u16 *present_elems,
			    struct ieee80211_mgd_assoc_data *assoc_data);

static size_t
ieee80211_add_link_elems(struct ieee80211_sub_if_data *sdata,
			 struct sk_buff *skb, u16 *capab,
			 const struct element *ext_capa,
			 const u8 *extra_elems,
			 size_t extra_elems_len,
			 unsigned int link_id,
			 struct ieee80211_link_data *link,
			 u16 *present_elems,
			 struct ieee80211_mgd_assoc_data *assoc_data)
{
	enum nl80211_iftype iftype = ieee80211_vif_type_p2p(&sdata->vif);
	struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;
	struct ieee80211_channel *chan = cbss->channel;
	const struct ieee80211_sband_iftype_data *iftd;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_supported_band *sband;
	enum nl80211_chan_width width = NL80211_CHAN_WIDTH_20;
	struct ieee80211_chanctx_conf *chanctx_conf;
	enum ieee80211_smps_mode smps_mode;
	u16 orig_capab = *capab;
	size_t offset = 0;
	int present_elems_len = 0;
	u8 *pos;
	int i;

#define ADD_PRESENT_ELEM(id) do {					\
	/* need a last for termination - we use 0 == SSID */		\
	if (!WARN_ON(present_elems_len >= PRESENT_ELEMS_MAX - 1))	\
		present_elems[present_elems_len++] = (id);		\
} while (0)
#define ADD_PRESENT_EXT_ELEM(id) ADD_PRESENT_ELEM(PRESENT_ELEM_EXT_OFFS | (id))

	if (link)
		smps_mode = link->smps_mode;
	else if (sdata->u.mgd.powersave)
		smps_mode = IEEE80211_SMPS_DYNAMIC;
	else
		smps_mode = IEEE80211_SMPS_OFF;

	if (link) {
		/*
		 * 5/10 MHz scenarios are only viable without MLO, in which
		 * case this pointer should be used ... All of this is a bit
		 * unclear though, not sure this even works at all.
		 */
		rcu_read_lock();
		chanctx_conf = rcu_dereference(link->conf->chanctx_conf);
		if (chanctx_conf)
			width = chanctx_conf->def.width;
		rcu_read_unlock();
	}

	sband = local->hw.wiphy->bands[chan->band];
	iftd = ieee80211_get_sband_iftype_data(sband, iftype);

	if (sband->band == NL80211_BAND_2GHZ) {
		*capab |= WLAN_CAPABILITY_SHORT_SLOT_TIME;
		*capab |= WLAN_CAPABILITY_SHORT_PREAMBLE;
	}

	if ((cbss->capability & WLAN_CAPABILITY_SPECTRUM_MGMT) &&
	    ieee80211_hw_check(&local->hw, SPECTRUM_MGMT))
		*capab |= WLAN_CAPABILITY_SPECTRUM_MGMT;

	if (sband->band != NL80211_BAND_S1GHZ)
		ieee80211_assoc_add_rates(local, skb, width, sband, assoc_data);

	if (*capab & WLAN_CAPABILITY_SPECTRUM_MGMT ||
	    *capab & WLAN_CAPABILITY_RADIO_MEASURE) {
		struct cfg80211_chan_def chandef = {
			.width = width,
			.chan = chan,
		};

		pos = skb_put(skb, 4);
		*pos++ = WLAN_EID_PWR_CAPABILITY;
		*pos++ = 2;
		*pos++ = 0; /* min tx power */
		 /* max tx power */
		*pos++ = ieee80211_chandef_max_power(&chandef);
		ADD_PRESENT_ELEM(WLAN_EID_PWR_CAPABILITY);
	}

	/*
	 * Per spec, we shouldn't include the list of channels if we advertise
	 * support for extended channel switching, but we've always done that;
	 * (for now?) apply this restriction only on the (new) 6 GHz band.
	 */
	if (*capab & WLAN_CAPABILITY_SPECTRUM_MGMT &&
	    (sband->band != NL80211_BAND_6GHZ ||
	     !ext_capa || ext_capa->datalen < 1 ||
	     !(ext_capa->data[0] & WLAN_EXT_CAPA1_EXT_CHANNEL_SWITCHING))) {
		/* TODO: get this in reg domain format */
		pos = skb_put(skb, 2 * sband->n_channels + 2);
		*pos++ = WLAN_EID_SUPPORTED_CHANNELS;
		*pos++ = 2 * sband->n_channels;
		for (i = 0; i < sband->n_channels; i++) {
			int cf = sband->channels[i].center_freq;

			*pos++ = ieee80211_frequency_to_channel(cf);
			*pos++ = 1; /* one channel in the subband*/
		}
		ADD_PRESENT_ELEM(WLAN_EID_SUPPORTED_CHANNELS);
	}

	/* if present, add any custom IEs that go before HT */
	offset = ieee80211_add_before_ht_elems(skb, extra_elems,
					       extra_elems_len,
					       offset);

	if (sband->band != NL80211_BAND_6GHZ &&
	    assoc_data->link[link_id].conn.mode >= IEEE80211_CONN_MODE_HT) {
		ieee80211_add_ht_ie(sdata, skb,
				    assoc_data->link[link_id].ap_ht_param,
				    sband, chan, smps_mode,
				    &assoc_data->link[link_id].conn);
		ADD_PRESENT_ELEM(WLAN_EID_HT_CAPABILITY);
	}

	/* if present, add any custom IEs that go before VHT */
	offset = ieee80211_add_before_vht_elems(skb, extra_elems,
						extra_elems_len,
						offset);

	if (sband->band != NL80211_BAND_6GHZ &&
	    assoc_data->link[link_id].conn.mode >= IEEE80211_CONN_MODE_VHT &&
	    sband->vht_cap.vht_supported) {
		bool mu_mimo_owner =
			ieee80211_add_vht_ie(sdata, skb, sband,
					     &assoc_data->link[link_id].ap_vht_cap,
					     &assoc_data->link[link_id].conn);

		if (link)
			link->conf->mu_mimo_owner = mu_mimo_owner;
		ADD_PRESENT_ELEM(WLAN_EID_VHT_CAPABILITY);
	}

	/* if present, add any custom IEs that go before HE */
	offset = ieee80211_add_before_he_elems(skb, extra_elems,
					       extra_elems_len,
					       offset);

	if (assoc_data->link[link_id].conn.mode >= IEEE80211_CONN_MODE_HE) {
		ieee80211_put_he_cap(skb, sdata, sband,
				     &assoc_data->link[link_id].conn);
		ADD_PRESENT_EXT_ELEM(WLAN_EID_EXT_HE_CAPABILITY);
		ieee80211_put_he_6ghz_cap(skb, sdata, smps_mode);
	}

	/*
	 * careful - need to know about all the present elems before
	 * calling ieee80211_assoc_add_ml_elem(), so add this one if
	 * we're going to put it after the ML element
	 */
	if (assoc_data->link[link_id].conn.mode >= IEEE80211_CONN_MODE_EHT)
		ADD_PRESENT_EXT_ELEM(WLAN_EID_EXT_EHT_CAPABILITY);

	if (link_id == assoc_data->assoc_link_id)
		ieee80211_assoc_add_ml_elem(sdata, skb, orig_capab, ext_capa,
					    present_elems, assoc_data);

	/* crash if somebody gets it wrong */
	present_elems = NULL;

	if (assoc_data->link[link_id].conn.mode >= IEEE80211_CONN_MODE_EHT)
		ieee80211_put_eht_cap(skb, sdata, sband,
				      &assoc_data->link[link_id].conn);

	if (sband->band == NL80211_BAND_S1GHZ) {
		ieee80211_add_aid_request_ie(sdata, skb);
		ieee80211_add_s1g_capab_ie(sdata, &sband->s1g_cap, skb);
	}

	if (iftd && iftd->vendor_elems.data && iftd->vendor_elems.len)
		skb_put_data(skb, iftd->vendor_elems.data, iftd->vendor_elems.len);

	return offset;
}

static void ieee80211_add_non_inheritance_elem(struct sk_buff *skb,
					       const u16 *outer,
					       const u16 *inner)
{
	unsigned int skb_len = skb->len;
	bool at_extension = false;
	bool added = false;
	int i, j;
	u8 *len, *list_len = NULL;

	skb_put_u8(skb, WLAN_EID_EXTENSION);
	len = skb_put(skb, 1);
	skb_put_u8(skb, WLAN_EID_EXT_NON_INHERITANCE);

	for (i = 0; i < PRESENT_ELEMS_MAX && outer[i]; i++) {
		u16 elem = outer[i];
		bool have_inner = false;

		/* should at least be sorted in the sense of normal -> ext */
		WARN_ON(at_extension && elem < PRESENT_ELEM_EXT_OFFS);

		/* switch to extension list */
		if (!at_extension && elem >= PRESENT_ELEM_EXT_OFFS) {
			at_extension = true;
			if (!list_len)
				skb_put_u8(skb, 0);
			list_len = NULL;
		}

		for (j = 0; j < PRESENT_ELEMS_MAX && inner[j]; j++) {
			if (elem == inner[j]) {
				have_inner = true;
				break;
			}
		}

		if (have_inner)
			continue;

		if (!list_len) {
			list_len = skb_put(skb, 1);
			*list_len = 0;
		}
		*list_len += 1;
		skb_put_u8(skb, (u8)elem);
		added = true;
	}

	/* if we added a list but no extension list, make a zero-len one */
	if (added && (!at_extension || !list_len))
		skb_put_u8(skb, 0);

	/* if nothing added remove extension element completely */
	if (!added)
		skb_trim(skb, skb_len);
	else
		*len = skb->len - skb_len - 2;
}

static void
ieee80211_assoc_add_ml_elem(struct ieee80211_sub_if_data *sdata,
			    struct sk_buff *skb, u16 capab,
			    const struct element *ext_capa,
			    const u16 *outer_present_elems,
			    struct ieee80211_mgd_assoc_data *assoc_data)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_multi_link_elem *ml_elem;
	struct ieee80211_mle_basic_common_info *common;
	const struct wiphy_iftype_ext_capab *ift_ext_capa;
	__le16 eml_capa = 0, mld_capa_ops = 0;
	unsigned int link_id;
	u8 *ml_elem_len;
	void *capab_pos;

	if (!ieee80211_vif_is_mld(&sdata->vif))
		return;

	ift_ext_capa = cfg80211_get_iftype_ext_capa(local->hw.wiphy,
						    ieee80211_vif_type_p2p(&sdata->vif));
	if (ift_ext_capa) {
		eml_capa = cpu_to_le16(ift_ext_capa->eml_capabilities);
		mld_capa_ops = cpu_to_le16(ift_ext_capa->mld_capa_and_ops);
	}

	skb_put_u8(skb, WLAN_EID_EXTENSION);
	ml_elem_len = skb_put(skb, 1);
	skb_put_u8(skb, WLAN_EID_EXT_EHT_MULTI_LINK);
	ml_elem = skb_put(skb, sizeof(*ml_elem));
	ml_elem->control =
		cpu_to_le16(IEEE80211_ML_CONTROL_TYPE_BASIC |
			    IEEE80211_MLC_BASIC_PRES_MLD_CAPA_OP);
	common = skb_put(skb, sizeof(*common));
	common->len = sizeof(*common) +
		      2;  /* MLD capa/ops */
	memcpy(common->mld_mac_addr, sdata->vif.addr, ETH_ALEN);

	/* add EML_CAPA only if needed, see Draft P802.11be_D2.1, 35.3.17 */
	if (eml_capa &
	    cpu_to_le16((IEEE80211_EML_CAP_EMLSR_SUPP |
			 IEEE80211_EML_CAP_EMLMR_SUPPORT))) {
		common->len += 2; /* EML capabilities */
		ml_elem->control |=
			cpu_to_le16(IEEE80211_MLC_BASIC_PRES_EML_CAPA);
		skb_put_data(skb, &eml_capa, sizeof(eml_capa));
	}
	skb_put_data(skb, &mld_capa_ops, sizeof(mld_capa_ops));

	/* Many APs have broken parsing of the extended MLD capa/ops field,
	 * dropping (re-)association request frames or replying with association
	 * response with a failure status if it's present. Without a clear
	 * indication as to whether the AP supports parsing this field or not do
	 * not include it in the common information unless strict mode is set.
	 */
	if (ieee80211_hw_check(&local->hw, STRICT) &&
	    assoc_data->ext_mld_capa_ops) {
		ml_elem->control |=
			cpu_to_le16(IEEE80211_MLC_BASIC_PRES_EXT_MLD_CAPA_OP);
		common->len += 2;
		skb_put_data(skb, &assoc_data->ext_mld_capa_ops,
			     sizeof(assoc_data->ext_mld_capa_ops));
	}

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		u16 link_present_elems[PRESENT_ELEMS_MAX] = {};
		const u8 *extra_elems;
		size_t extra_elems_len;
		size_t extra_used;
		u8 *subelem_len = NULL;
		__le16 ctrl;

		if (!assoc_data->link[link_id].bss ||
		    link_id == assoc_data->assoc_link_id)
			continue;

		extra_elems = assoc_data->link[link_id].elems;
		extra_elems_len = assoc_data->link[link_id].elems_len;

		skb_put_u8(skb, IEEE80211_MLE_SUBELEM_PER_STA_PROFILE);
		subelem_len = skb_put(skb, 1);

		ctrl = cpu_to_le16(link_id |
				   IEEE80211_MLE_STA_CONTROL_COMPLETE_PROFILE |
				   IEEE80211_MLE_STA_CONTROL_STA_MAC_ADDR_PRESENT);
		skb_put_data(skb, &ctrl, sizeof(ctrl));
		skb_put_u8(skb, 1 + ETH_ALEN); /* STA Info Length */
		skb_put_data(skb, assoc_data->link[link_id].addr,
			     ETH_ALEN);
		/*
		 * Now add the contents of the (re)association request,
		 * but the "listen interval" and "current AP address"
		 * (if applicable) are skipped. So we only have
		 * the capability field (remember the position and fill
		 * later), followed by the elements added below by
		 * calling ieee80211_add_link_elems().
		 */
		capab_pos = skb_put(skb, 2);

		extra_used = ieee80211_add_link_elems(sdata, skb, &capab,
						      ext_capa,
						      extra_elems,
						      extra_elems_len,
						      link_id, NULL,
						      link_present_elems,
						      assoc_data);
		if (extra_elems)
			skb_put_data(skb, extra_elems + extra_used,
				     extra_elems_len - extra_used);

		put_unaligned_le16(capab, capab_pos);

		ieee80211_add_non_inheritance_elem(skb, outer_present_elems,
						   link_present_elems);

		ieee80211_fragment_element(skb, subelem_len,
					   IEEE80211_MLE_SUBELEM_FRAGMENT);
	}

	ieee80211_fragment_element(skb, ml_elem_len, WLAN_EID_FRAGMENT);
}

static int
ieee80211_link_common_elems_size(struct ieee80211_sub_if_data *sdata,
				 enum nl80211_iftype iftype,
				 struct cfg80211_bss *cbss,
				 size_t elems_len)
{
	struct ieee80211_local *local = sdata->local;
	const struct ieee80211_sband_iftype_data *iftd;
	struct ieee80211_supported_band *sband;
	size_t size = 0;

	if (!cbss)
		return size;

	sband = local->hw.wiphy->bands[cbss->channel->band];

	/* add STA profile elements length */
	size += elems_len;

	/* and supported rates length */
	size += 4 + sband->n_bitrates;

	/* supported channels */
	size += 2 + 2 * sband->n_channels;

	iftd = ieee80211_get_sband_iftype_data(sband, iftype);
	if (iftd)
		size += iftd->vendor_elems.len;

	/* power capability */
	size += 4;

	/* HT, VHT, HE, EHT */
	size += 2 + sizeof(struct ieee80211_ht_cap);
	size += 2 + sizeof(struct ieee80211_vht_cap);
	size += 2 + 1 + sizeof(struct ieee80211_he_cap_elem) +
		sizeof(struct ieee80211_he_mcs_nss_supp) +
		IEEE80211_HE_PPE_THRES_MAX_LEN;

	if (sband->band == NL80211_BAND_6GHZ)
		size += 2 + 1 + sizeof(struct ieee80211_he_6ghz_capa);

	size += 2 + 1 + sizeof(struct ieee80211_eht_cap_elem) +
		sizeof(struct ieee80211_eht_mcs_nss_supp) +
		IEEE80211_EHT_PPE_THRES_MAX_LEN;

	return size;
}

static int ieee80211_send_assoc(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_assoc_data *assoc_data = ifmgd->assoc_data;
	struct ieee80211_link_data *link;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u8 *pos, qos_info, *ie_start;
	size_t offset, noffset;
	u16 capab = 0, link_capab;
	__le16 listen_int;
	struct element *ext_capa = NULL;
	enum nl80211_iftype iftype = ieee80211_vif_type_p2p(&sdata->vif);
	struct ieee80211_prep_tx_info info = {};
	unsigned int link_id, n_links = 0;
	u16 present_elems[PRESENT_ELEMS_MAX] = {};
	void *capab_pos;
	size_t size;
	int ret;

	/* we know it's writable, cast away the const */
	if (assoc_data->ie_len)
		ext_capa = (void *)cfg80211_find_elem(WLAN_EID_EXT_CAPABILITY,
						      assoc_data->ie,
						      assoc_data->ie_len);

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	size = local->hw.extra_tx_headroom +
	       sizeof(*mgmt) + /* bit too much but doesn't matter */
	       2 + assoc_data->ssid_len + /* SSID */
	       assoc_data->ie_len + /* extra IEs */
	       (assoc_data->fils_kek_len ? 16 /* AES-SIV */ : 0) +
	       9; /* WMM */

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;
		size_t elems_len = assoc_data->link[link_id].elems_len;

		if (!cbss)
			continue;

		n_links++;

		size += ieee80211_link_common_elems_size(sdata, iftype, cbss,
							 elems_len);

		/* non-inheritance element */
		size += 2 + 2 + PRESENT_ELEMS_MAX;

		/* should be the same across all BSSes */
		if (cbss->capability & WLAN_CAPABILITY_PRIVACY)
			capab |= WLAN_CAPABILITY_PRIVACY;
	}

	if (ieee80211_vif_is_mld(&sdata->vif)) {
		/* consider the multi-link element with STA profile */
		size += sizeof(struct ieee80211_multi_link_elem);
		/* max common info field in basic multi-link element */
		size += sizeof(struct ieee80211_mle_basic_common_info) +
			2 + /* capa & op */
			2 + /* ext capa & op */
			2; /* EML capa */

		/*
		 * The capability elements were already considered above;
		 * note this over-estimates a bit because there's no
		 * STA profile for the assoc link.
		 */
		size += (n_links - 1) *
			(1 + 1 + /* subelement ID/length */
			 2 + /* STA control */
			 1 + ETH_ALEN + 2 /* STA Info field */);
	}

	link = sdata_dereference(sdata->link[assoc_data->assoc_link_id], sdata);
	if (WARN_ON(!link))
		return -EINVAL;

	if (WARN_ON(!assoc_data->link[assoc_data->assoc_link_id].bss))
		return -EINVAL;

	skb = alloc_skb(size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	skb_reserve(skb, local->hw.extra_tx_headroom);

	if (ifmgd->flags & IEEE80211_STA_ENABLE_RRM)
		capab |= WLAN_CAPABILITY_RADIO_MEASURE;

	/* Set MBSSID support for HE AP if needed */
	if (ieee80211_hw_check(&local->hw, SUPPORTS_ONLY_HE_MULTI_BSSID) &&
	    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HE &&
	    ext_capa && ext_capa->datalen >= 3)
		ext_capa->data[2] |= WLAN_EXT_CAPA3_MULTI_BSSID_SUPPORT;

	mgmt = skb_put_zero(skb, 24);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	listen_int = cpu_to_le16(assoc_data->s1g ?
			ieee80211_encode_usf(local->hw.conf.listen_interval) :
			local->hw.conf.listen_interval);
	if (!is_zero_ether_addr(assoc_data->prev_ap_addr)) {
		skb_put(skb, 10);
		mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_REASSOC_REQ);
		capab_pos = &mgmt->u.reassoc_req.capab_info;
		mgmt->u.reassoc_req.listen_interval = listen_int;
		memcpy(mgmt->u.reassoc_req.current_ap,
		       assoc_data->prev_ap_addr, ETH_ALEN);
		info.subtype = IEEE80211_STYPE_REASSOC_REQ;
	} else {
		skb_put(skb, 4);
		mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_ASSOC_REQ);
		capab_pos = &mgmt->u.assoc_req.capab_info;
		mgmt->u.assoc_req.listen_interval = listen_int;
		info.subtype = IEEE80211_STYPE_ASSOC_REQ;
	}

	/* SSID */
	pos = skb_put(skb, 2 + assoc_data->ssid_len);
	ie_start = pos;
	*pos++ = WLAN_EID_SSID;
	*pos++ = assoc_data->ssid_len;
	memcpy(pos, assoc_data->ssid, assoc_data->ssid_len);

	/*
	 * This bit is technically reserved, so it shouldn't matter for either
	 * the AP or us, but it also means we shouldn't set it. However, we've
	 * always set it in the past, and apparently some EHT APs check that
	 * we don't set it. To avoid interoperability issues with old APs that
	 * for some reason check it and want it to be set, set the bit for all
	 * pre-EHT connections as we used to do.
	 */
	if (link->u.mgd.conn.mode < IEEE80211_CONN_MODE_EHT &&
	    !ieee80211_hw_check(&local->hw, STRICT))
		capab |= WLAN_CAPABILITY_ESS;

	/* add the elements for the assoc (main) link */
	link_capab = capab;
	offset = ieee80211_add_link_elems(sdata, skb, &link_capab,
					  ext_capa,
					  assoc_data->ie,
					  assoc_data->ie_len,
					  assoc_data->assoc_link_id, link,
					  present_elems, assoc_data);
	put_unaligned_le16(link_capab, capab_pos);

	/* if present, add any custom non-vendor IEs */
	if (assoc_data->ie_len) {
		noffset = ieee80211_ie_split_vendor(assoc_data->ie,
						    assoc_data->ie_len,
						    offset);
		skb_put_data(skb, assoc_data->ie + offset, noffset - offset);
		offset = noffset;
	}

	if (assoc_data->wmm) {
		if (assoc_data->uapsd) {
			qos_info = ifmgd->uapsd_queues;
			qos_info |= (ifmgd->uapsd_max_sp_len <<
				     IEEE80211_WMM_IE_STA_QOSINFO_SP_SHIFT);
		} else {
			qos_info = 0;
		}

		pos = ieee80211_add_wmm_info_ie(skb_put(skb, 9), qos_info);
	}

	/* add any remaining custom (i.e. vendor specific here) IEs */
	if (assoc_data->ie_len) {
		noffset = assoc_data->ie_len;
		skb_put_data(skb, assoc_data->ie + offset, noffset - offset);
	}

	if (assoc_data->fils_kek_len) {
		ret = fils_encrypt_assoc_req(skb, assoc_data);
		if (ret < 0) {
			dev_kfree_skb(skb);
			return ret;
		}
	}

	pos = skb_tail_pointer(skb);
	kfree(ifmgd->assoc_req_ies);
	ifmgd->assoc_req_ies = kmemdup(ie_start, pos - ie_start, GFP_ATOMIC);
	if (!ifmgd->assoc_req_ies) {
		dev_kfree_skb(skb);
		return -ENOMEM;
	}

	ifmgd->assoc_req_ies_len = pos - ie_start;

	info.link_id = assoc_data->assoc_link_id;
	drv_mgd_prepare_tx(local, sdata, &info);

	IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_INTFL_DONT_ENCRYPT;
	if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
		IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS |
						IEEE80211_TX_INTFL_MLME_CONN_TX;
	ieee80211_tx_skb(sdata, skb);

	return 0;
}

void ieee80211_send_pspoll(struct ieee80211_local *local,
			   struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_pspoll *pspoll;
	struct sk_buff *skb;

	skb = ieee80211_pspoll_get(&local->hw, &sdata->vif);
	if (!skb)
		return;

	pspoll = (struct ieee80211_pspoll *) skb->data;
	pspoll->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);

	IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_INTFL_DONT_ENCRYPT;
	ieee80211_tx_skb(sdata, skb);
}

void ieee80211_send_nullfunc(struct ieee80211_local *local,
			     struct ieee80211_sub_if_data *sdata,
			     bool powersave)
{
	struct sk_buff *skb;
	struct ieee80211_hdr_3addr *nullfunc;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	skb = ieee80211_nullfunc_get(&local->hw, &sdata->vif, -1,
				     !ieee80211_hw_check(&local->hw,
							 DOESNT_SUPPORT_QOS_NDP));
	if (!skb)
		return;

	nullfunc = (struct ieee80211_hdr_3addr *) skb->data;
	if (powersave)
		nullfunc->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);

	IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_INTFL_DONT_ENCRYPT |
					IEEE80211_TX_INTFL_OFFCHAN_TX_OK;

	if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
		IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;

	if (ifmgd->flags & IEEE80211_STA_CONNECTION_POLL)
		IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_USE_MINRATE;

	ieee80211_tx_skb(sdata, skb);
}

void ieee80211_send_4addr_nullfunc(struct ieee80211_local *local,
				   struct ieee80211_sub_if_data *sdata)
{
	struct sk_buff *skb;
	struct ieee80211_hdr *nullfunc;
	__le16 fc;

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_STATION))
		return;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + 30);
	if (!skb)
		return;

	skb_reserve(skb, local->hw.extra_tx_headroom);

	nullfunc = skb_put_zero(skb, 30);
	fc = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_NULLFUNC |
			 IEEE80211_FCTL_FROMDS | IEEE80211_FCTL_TODS);
	nullfunc->frame_control = fc;
	memcpy(nullfunc->addr1, sdata->deflink.u.mgd.bssid, ETH_ALEN);
	memcpy(nullfunc->addr2, sdata->vif.addr, ETH_ALEN);
	memcpy(nullfunc->addr3, sdata->deflink.u.mgd.bssid, ETH_ALEN);
	memcpy(nullfunc->addr4, sdata->vif.addr, ETH_ALEN);

	IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_INTFL_DONT_ENCRYPT;
	IEEE80211_SKB_CB(skb)->flags |= IEEE80211_TX_CTL_USE_MINRATE;
	ieee80211_tx_skb(sdata, skb);
}

/* spectrum management related things */
static void ieee80211_csa_switch_work(struct wiphy *wiphy,
				      struct wiphy_work *work)
{
	struct ieee80211_link_data *link =
		container_of(work, struct ieee80211_link_data,
			     u.mgd.csa.switch_work.work);
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	int ret;

	if (!ieee80211_sdata_running(sdata))
		return;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!ifmgd->associated)
		return;

	if (!link->conf->csa_active)
		return;

	/*
	 * If the link isn't active (now), we cannot wait for beacons, won't
	 * have a reserved chanctx, etc. Just switch over the chandef and
	 * update cfg80211 directly.
	 */
	if (!ieee80211_vif_link_active(&sdata->vif, link->link_id)) {
		link->conf->chanreq = link->csa.chanreq;
		cfg80211_ch_switch_notify(sdata->dev, &link->csa.chanreq.oper,
					  link->link_id);
		return;
	}

	/*
	 * using reservation isn't immediate as it may be deferred until later
	 * with multi-vif. once reservation is complete it will re-schedule the
	 * work with no reserved_chanctx so verify chandef to check if it
	 * completed successfully
	 */

	if (link->reserved_chanctx) {
		/*
		 * with multi-vif csa driver may call ieee80211_csa_finish()
		 * many times while waiting for other interfaces to use their
		 * reservations
		 */
		if (link->reserved_ready)
			return;

		ret = ieee80211_link_use_reserved_context(link);
		if (ret) {
			link_info(link,
				  "failed to use reserved channel context, disconnecting (err=%d)\n",
				  ret);
			wiphy_work_queue(sdata->local->hw.wiphy,
					 &ifmgd->csa_connection_drop_work);
		}
		return;
	}

	if (!ieee80211_chanreq_identical(&link->conf->chanreq,
					 &link->csa.chanreq)) {
		link_info(link,
			  "failed to finalize channel switch, disconnecting\n");
		wiphy_work_queue(sdata->local->hw.wiphy,
				 &ifmgd->csa_connection_drop_work);
		return;
	}

	link->u.mgd.csa.waiting_bcn = true;

	/* apply new TPE restrictions immediately on the new channel */
	if (link->u.mgd.csa.ap_chandef.chan->band == NL80211_BAND_6GHZ &&
	    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HE) {
		ieee80211_rearrange_tpe(&link->u.mgd.csa.tpe,
					&link->u.mgd.csa.ap_chandef,
					&link->conf->chanreq.oper);
		if (memcmp(&link->conf->tpe, &link->u.mgd.csa.tpe,
			   sizeof(link->u.mgd.csa.tpe))) {
			link->conf->tpe = link->u.mgd.csa.tpe;
			ieee80211_link_info_change_notify(sdata, link,
							  BSS_CHANGED_TPE);
		}
	}

	ieee80211_sta_reset_beacon_monitor(sdata);
	ieee80211_sta_reset_conn_monitor(sdata);
}

static void ieee80211_chswitch_post_beacon(struct ieee80211_link_data *link)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	int ret;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	WARN_ON(!link->conf->csa_active);

	ieee80211_vif_unblock_queues_csa(sdata);

	link->conf->csa_active = false;
	link->u.mgd.csa.blocked_tx = false;
	link->u.mgd.csa.waiting_bcn = false;

	ret = drv_post_channel_switch(link);
	if (ret) {
		link_info(link,
			  "driver post channel switch failed, disconnecting\n");
		wiphy_work_queue(sdata->local->hw.wiphy,
				 &ifmgd->csa_connection_drop_work);
		return;
	}

	cfg80211_ch_switch_notify(sdata->dev, &link->conf->chanreq.oper,
				  link->link_id);
}

void ieee80211_chswitch_done(struct ieee80211_vif *vif, bool success,
			     unsigned int link_id)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	trace_api_chswitch_done(sdata, success, link_id);

	rcu_read_lock();

	if (!success) {
		sdata_info(sdata,
			   "driver channel switch failed (link %d), disconnecting\n",
			   link_id);
		wiphy_work_queue(sdata->local->hw.wiphy,
				 &sdata->u.mgd.csa_connection_drop_work);
	} else {
		struct ieee80211_link_data *link =
			rcu_dereference(sdata->link[link_id]);

		if (WARN_ON(!link)) {
			rcu_read_unlock();
			return;
		}

		wiphy_delayed_work_queue(sdata->local->hw.wiphy,
					 &link->u.mgd.csa.switch_work, 0);
	}

	rcu_read_unlock();
}
EXPORT_SYMBOL(ieee80211_chswitch_done);

static void
ieee80211_sta_abort_chanswitch(struct ieee80211_link_data *link)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!local->ops->abort_channel_switch)
		return;

	ieee80211_link_unreserve_chanctx(link);

	ieee80211_vif_unblock_queues_csa(sdata);

	link->conf->csa_active = false;
	link->u.mgd.csa.blocked_tx = false;

	drv_abort_channel_switch(link);
}

struct sta_csa_rnr_iter_data {
	struct ieee80211_link_data *link;
	struct ieee80211_channel *chan;
	u8 mld_id;
};

static enum cfg80211_rnr_iter_ret
ieee80211_sta_csa_rnr_iter(void *_data, u8 type,
			   const struct ieee80211_neighbor_ap_info *info,
			   const u8 *tbtt_info, u8 tbtt_info_len)
{
	struct sta_csa_rnr_iter_data *data = _data;
	struct ieee80211_link_data *link = data->link;
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	const struct ieee80211_tbtt_info_ge_11 *ti;
	enum nl80211_band band;
	unsigned int center_freq;
	int link_id;

	if (type != IEEE80211_TBTT_INFO_TYPE_TBTT)
		return RNR_ITER_CONTINUE;

	if (tbtt_info_len < sizeof(*ti))
		return RNR_ITER_CONTINUE;

	ti = (const void *)tbtt_info;

	if (ti->mld_params.mld_id != data->mld_id)
		return RNR_ITER_CONTINUE;

	link_id = le16_get_bits(ti->mld_params.params,
				IEEE80211_RNR_MLD_PARAMS_LINK_ID);
	if (link_id != data->link->link_id)
		return RNR_ITER_CONTINUE;

	/* we found the entry for our link! */

	/* this AP is confused, it had this right before ... just disconnect */
	if (!ieee80211_operating_class_to_band(info->op_class, &band)) {
		link_info(link,
			  "AP now has invalid operating class in RNR, disconnect\n");
		wiphy_work_queue(sdata->local->hw.wiphy,
				 &ifmgd->csa_connection_drop_work);
		return RNR_ITER_BREAK;
	}

	center_freq = ieee80211_channel_to_frequency(info->channel, band);
	data->chan = ieee80211_get_channel(sdata->local->hw.wiphy, center_freq);

	return RNR_ITER_BREAK;
}

static void
ieee80211_sta_other_link_csa_disappeared(struct ieee80211_link_data *link,
					 struct ieee802_11_elems *elems)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct sta_csa_rnr_iter_data data = {
		.link = link,
	};

	/*
	 * If we get here, we see a beacon from another link without
	 * CSA still being reported for it, so now we have to check
	 * if the CSA was aborted or completed. This may not even be
	 * perfectly possible if the CSA was only done for changing
	 * the puncturing, but in that case if the link in inactive
	 * we don't really care, and if it's an active link (or when
	 * it's activated later) we'll get a beacon and adjust.
	 */

	if (WARN_ON(!elems->ml_basic))
		return;

	data.mld_id = ieee80211_mle_get_mld_id((const void *)elems->ml_basic);

	/*
	 * So in order to do this, iterate the RNR element(s) and see
	 * what channel is reported now.
	 */
	cfg80211_iter_rnr(elems->ie_start, elems->total_len,
			  ieee80211_sta_csa_rnr_iter, &data);

	if (!data.chan) {
		link_info(link,
			  "couldn't find (valid) channel in RNR for CSA, disconnect\n");
		wiphy_work_queue(sdata->local->hw.wiphy,
				 &ifmgd->csa_connection_drop_work);
		return;
	}

	/*
	 * If it doesn't match the CSA, then assume it aborted. This
	 * may erroneously detect that it was _not_ aborted when it
	 * was in fact aborted, but only changed the bandwidth or the
	 * puncturing configuration, but we don't have enough data to
	 * detect that.
	 */
	if (data.chan != link->csa.chanreq.oper.chan)
		ieee80211_sta_abort_chanswitch(link);
}

enum ieee80211_csa_source {
	IEEE80211_CSA_SOURCE_BEACON,
	IEEE80211_CSA_SOURCE_OTHER_LINK,
	IEEE80211_CSA_SOURCE_PROT_ACTION,
	IEEE80211_CSA_SOURCE_UNPROT_ACTION,
};

static void
ieee80211_sta_process_chanswitch(struct ieee80211_link_data *link,
				 u64 timestamp, u32 device_timestamp,
				 struct ieee802_11_elems *full_elems,
				 struct ieee802_11_elems *csa_elems,
				 enum ieee80211_csa_source source)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_chanctx *chanctx = NULL;
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_csa_ie csa_ie = {};
	struct ieee80211_channel_switch ch_switch = {
		.link_id = link->link_id,
		.timestamp = timestamp,
		.device_timestamp = device_timestamp,
	};
	unsigned long now;
	int res;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (csa_elems) {
		struct cfg80211_bss *cbss = link->conf->bss;
		enum nl80211_band current_band;
		struct ieee80211_bss *bss;

		if (WARN_ON(!cbss))
			return;

		current_band = cbss->channel->band;
		bss = (void *)cbss->priv;

		res = ieee80211_parse_ch_switch_ie(sdata, csa_elems,
						   current_band,
						   bss->vht_cap_info,
						   &link->u.mgd.conn,
						   link->u.mgd.bssid,
						   source == IEEE80211_CSA_SOURCE_UNPROT_ACTION,
						   &csa_ie);
		if (res == 0) {
			ch_switch.block_tx = csa_ie.mode;
			ch_switch.chandef = csa_ie.chanreq.oper;
			ch_switch.count = csa_ie.count;
			ch_switch.delay = csa_ie.max_switch_time;
		}

		link->u.mgd.csa.tpe = csa_elems->csa_tpe;
	} else {
		/*
		 * If there was no per-STA profile for this link, we
		 * get called with csa_elems == NULL. This of course means
		 * there are no CSA elements, so set res=1 indicating
		 * no more CSA.
		 */
		res = 1;
	}

	if (res < 0) {
		/* ignore this case, not a protected frame */
		if (source == IEEE80211_CSA_SOURCE_UNPROT_ACTION)
			return;
		goto drop_connection;
	}

	if (link->conf->csa_active) {
		switch (source) {
		case IEEE80211_CSA_SOURCE_PROT_ACTION:
		case IEEE80211_CSA_SOURCE_UNPROT_ACTION:
			/* already processing - disregard action frames */
			return;
		case IEEE80211_CSA_SOURCE_BEACON:
			if (link->u.mgd.csa.waiting_bcn) {
				ieee80211_chswitch_post_beacon(link);
				/*
				 * If the CSA is still present after the switch
				 * we need to consider it as a new CSA (possibly
				 * to self). This happens by not returning here
				 * so we'll get to the check below.
				 */
			} else if (res) {
				ieee80211_sta_abort_chanswitch(link);
				return;
			} else {
				drv_channel_switch_rx_beacon(sdata, &ch_switch);
				return;
			}
			break;
		case IEEE80211_CSA_SOURCE_OTHER_LINK:
			/* active link: we want to see the beacon to continue */
			if (ieee80211_vif_link_active(&sdata->vif,
						      link->link_id))
				return;

			/* switch work ran, so just complete the process */
			if (link->u.mgd.csa.waiting_bcn) {
				ieee80211_chswitch_post_beacon(link);
				/*
				 * If the CSA is still present after the switch
				 * we need to consider it as a new CSA (possibly
				 * to self). This happens by not returning here
				 * so we'll get to the check below.
				 */
				break;
			}

			/* link still has CSA but we already know, do nothing */
			if (!res)
				return;

			/* check in the RNR if the CSA aborted */
			ieee80211_sta_other_link_csa_disappeared(link,
								 full_elems);
			return;
		}
	}

	/* no active CSA nor a new one */
	if (res) {
		/*
		 * However, we may have stopped queues when receiving a public
		 * action frame that couldn't be protected, if it had the quiet
		 * bit set. This is a trade-off, we want to be quiet as soon as
		 * possible, but also don't trust the public action frame much,
		 * as it can't be protected.
		 */
		if (unlikely(link->u.mgd.csa.blocked_tx)) {
			link->u.mgd.csa.blocked_tx = false;
			ieee80211_vif_unblock_queues_csa(sdata);
		}
		return;
	}

	/*
	 * We don't really trust public action frames, but block queues (go to
	 * quiet mode) for them anyway, we should get a beacon soon to either
	 * know what the CSA really is, or figure out the public action frame
	 * was actually an attack.
	 */
	if (source == IEEE80211_CSA_SOURCE_UNPROT_ACTION) {
		if (csa_ie.mode) {
			link->u.mgd.csa.blocked_tx = true;
			ieee80211_vif_block_queues_csa(sdata);
		}
		return;
	}

	if (link->conf->chanreq.oper.chan->band !=
	    csa_ie.chanreq.oper.chan->band) {
		link_info(link,
			  "AP %pM switches to different band (%d MHz, width:%d, CF1/2: %d/%d MHz), disconnecting\n",
			  link->u.mgd.bssid,
			  csa_ie.chanreq.oper.chan->center_freq,
			  csa_ie.chanreq.oper.width,
			  csa_ie.chanreq.oper.center_freq1,
			  csa_ie.chanreq.oper.center_freq2);
		goto drop_connection;
	}

	if (!cfg80211_chandef_usable(local->hw.wiphy, &csa_ie.chanreq.oper,
				     IEEE80211_CHAN_DISABLED)) {
		link_info(link,
			  "AP %pM switches to unsupported channel (%d.%03d MHz, width:%d, CF1/2: %d.%03d/%d MHz), disconnecting\n",
			  link->u.mgd.bssid,
			  csa_ie.chanreq.oper.chan->center_freq,
			  csa_ie.chanreq.oper.chan->freq_offset,
			  csa_ie.chanreq.oper.width,
			  csa_ie.chanreq.oper.center_freq1,
			  csa_ie.chanreq.oper.freq1_offset,
			  csa_ie.chanreq.oper.center_freq2);
		goto drop_connection;
	}

	if (cfg80211_chandef_identical(&csa_ie.chanreq.oper,
				       &link->conf->chanreq.oper) &&
	    (!csa_ie.mode || source != IEEE80211_CSA_SOURCE_BEACON)) {
		if (link->u.mgd.csa.ignored_same_chan)
			return;
		link_info(link,
			  "AP %pM tries to chanswitch to same channel, ignore\n",
			  link->u.mgd.bssid);
		link->u.mgd.csa.ignored_same_chan = true;
		return;
	}

	/*
	 * Drop all TDLS peers on the affected link - either we disconnect or
	 * move to a different channel from this point on. There's no telling
	 * what our peer will do.
	 * The TDLS WIDER_BW scenario is also problematic, as peers might now
	 * have an incompatible wider chandef.
	 */
	ieee80211_teardown_tdls_peers(link);

	conf = rcu_dereference_protected(link->conf->chanctx_conf,
					 lockdep_is_held(&local->hw.wiphy->mtx));
	if (ieee80211_vif_link_active(&sdata->vif, link->link_id) && !conf) {
		link_info(link,
			  "no channel context assigned to vif?, disconnecting\n");
		goto drop_connection;
	}

	if (conf)
		chanctx = container_of(conf, struct ieee80211_chanctx, conf);

	if (!ieee80211_hw_check(&local->hw, CHANCTX_STA_CSA)) {
		link_info(link,
			  "driver doesn't support chan-switch with channel contexts\n");
		goto drop_connection;
	}

	if (drv_pre_channel_switch(sdata, &ch_switch)) {
		link_info(link,
			  "preparing for channel switch failed, disconnecting\n");
		goto drop_connection;
	}

	link->u.mgd.csa.ap_chandef = csa_ie.chanreq.ap;

	link->csa.chanreq.oper = csa_ie.chanreq.oper;
	ieee80211_set_chanreq_ap(sdata, &link->csa.chanreq, &link->u.mgd.conn,
				 &csa_ie.chanreq.ap);

	if (chanctx) {
		res = ieee80211_link_reserve_chanctx(link, &link->csa.chanreq,
						     chanctx->mode, false);
		if (res) {
			link_info(link,
				  "failed to reserve channel context for channel switch, disconnecting (err=%d)\n",
				  res);
			goto drop_connection;
		}
	}

	link->conf->csa_active = true;
	link->u.mgd.csa.ignored_same_chan = false;
	link->u.mgd.beacon_crc_valid = false;
	link->u.mgd.csa.blocked_tx = csa_ie.mode;

	if (csa_ie.mode)
		ieee80211_vif_block_queues_csa(sdata);

	cfg80211_ch_switch_started_notify(sdata->dev, &csa_ie.chanreq.oper,
					  link->link_id, csa_ie.count,
					  csa_ie.mode);

	/* we may have to handle timeout for deactivated link in software */
	now = jiffies;
	link->u.mgd.csa.time = now +
			       TU_TO_JIFFIES((max_t(int, csa_ie.count, 1) - 1) *
					     link->conf->beacon_int);

	if (ieee80211_vif_link_active(&sdata->vif, link->link_id) &&
	    local->ops->channel_switch) {
		/*
		 * Use driver's channel switch callback, the driver will
		 * later call ieee80211_chswitch_done(). It may deactivate
		 * the link as well, we handle that elsewhere and queue
		 * the csa.switch_work for the calculated time then.
		 */
		drv_channel_switch(local, sdata, &ch_switch);
		return;
	}

	/* channel switch handled in software */
	wiphy_delayed_work_queue(local->hw.wiphy,
				 &link->u.mgd.csa.switch_work,
				 link->u.mgd.csa.time - now);
	return;
 drop_connection:
	/*
	 * This is just so that the disconnect flow will know that
	 * we were trying to switch channel and failed. In case the
	 * mode is 1 (we are not allowed to Tx), we will know not to
	 * send a deauthentication frame. Those two fields will be
	 * reset when the disconnection worker runs.
	 */
	link->conf->csa_active = true;
	link->u.mgd.csa.blocked_tx = csa_ie.mode;

	wiphy_work_queue(sdata->local->hw.wiphy,
			 &ifmgd->csa_connection_drop_work);
}

struct sta_bss_param_ch_cnt_data {
	struct ieee80211_sub_if_data *sdata;
	u8 reporting_link_id;
	u8 mld_id;
};

static enum cfg80211_rnr_iter_ret
ieee80211_sta_bss_param_ch_cnt_iter(void *_data, u8 type,
				    const struct ieee80211_neighbor_ap_info *info,
				    const u8 *tbtt_info, u8 tbtt_info_len)
{
	struct sta_bss_param_ch_cnt_data *data = _data;
	struct ieee80211_sub_if_data *sdata = data->sdata;
	const struct ieee80211_tbtt_info_ge_11 *ti;
	u8 bss_param_ch_cnt;
	int link_id;

	if (type != IEEE80211_TBTT_INFO_TYPE_TBTT)
		return RNR_ITER_CONTINUE;

	if (tbtt_info_len < sizeof(*ti))
		return RNR_ITER_CONTINUE;

	ti = (const void *)tbtt_info;

	if (ti->mld_params.mld_id != data->mld_id)
		return RNR_ITER_CONTINUE;

	link_id = le16_get_bits(ti->mld_params.params,
				IEEE80211_RNR_MLD_PARAMS_LINK_ID);
	bss_param_ch_cnt =
		le16_get_bits(ti->mld_params.params,
			      IEEE80211_RNR_MLD_PARAMS_BSS_CHANGE_COUNT);

	if (bss_param_ch_cnt != 255 &&
	    link_id < ARRAY_SIZE(sdata->link)) {
		struct ieee80211_link_data *link =
			sdata_dereference(sdata->link[link_id], sdata);

		if (link && link->conf->bss_param_ch_cnt != bss_param_ch_cnt) {
			link->conf->bss_param_ch_cnt = bss_param_ch_cnt;
			link->conf->bss_param_ch_cnt_link_id =
				data->reporting_link_id;
		}
	}

	return RNR_ITER_CONTINUE;
}

static void
ieee80211_mgd_update_bss_param_ch_cnt(struct ieee80211_sub_if_data *sdata,
				      struct ieee80211_bss_conf *bss_conf,
				      struct ieee802_11_elems *elems)
{
	struct sta_bss_param_ch_cnt_data data = {
		.reporting_link_id = bss_conf->link_id,
		.sdata = sdata,
	};
	int bss_param_ch_cnt;

	if (!elems->ml_basic)
		return;

	data.mld_id = ieee80211_mle_get_mld_id((const void *)elems->ml_basic);

	cfg80211_iter_rnr(elems->ie_start, elems->total_len,
			  ieee80211_sta_bss_param_ch_cnt_iter, &data);

	bss_param_ch_cnt =
		ieee80211_mle_get_bss_param_ch_cnt((const void *)elems->ml_basic);

	/*
	 * Update bss_param_ch_cnt_link_id even if bss_param_ch_cnt
	 * didn't change to indicate that we got a beacon on our own
	 * link.
	 */
	if (bss_param_ch_cnt >= 0 && bss_param_ch_cnt != 255) {
		bss_conf->bss_param_ch_cnt = bss_param_ch_cnt;
		bss_conf->bss_param_ch_cnt_link_id =
			bss_conf->link_id;
	}
}

static bool
ieee80211_find_80211h_pwr_constr(struct ieee80211_channel *channel,
				 const u8 *country_ie, u8 country_ie_len,
				 const u8 *pwr_constr_elem,
				 int *chan_pwr, int *pwr_reduction)
{
	struct ieee80211_country_ie_triplet *triplet;
	int chan = ieee80211_frequency_to_channel(channel->center_freq);
	int i, chan_increment;
	bool have_chan_pwr = false;

	/* Invalid IE */
	if (country_ie_len % 2 || country_ie_len < IEEE80211_COUNTRY_IE_MIN_LEN)
		return false;

	triplet = (void *)(country_ie + 3);
	country_ie_len -= 3;

	switch (channel->band) {
	default:
		WARN_ON_ONCE(1);
		fallthrough;
	case NL80211_BAND_2GHZ:
	case NL80211_BAND_60GHZ:
	case NL80211_BAND_LC:
		chan_increment = 1;
		break;
	case NL80211_BAND_5GHZ:
		chan_increment = 4;
		break;
	case NL80211_BAND_6GHZ:
		/*
		 * In the 6 GHz band, the "maximum transmit power level"
		 * field in the triplets is reserved, and thus will be
		 * zero and we shouldn't use it to control TX power.
		 * The actual TX power will be given in the transmit
		 * power envelope element instead.
		 */
		return false;
	}

	/* find channel */
	while (country_ie_len >= 3) {
		u8 first_channel = triplet->chans.first_channel;

		if (first_channel >= IEEE80211_COUNTRY_EXTENSION_ID)
			goto next;

		for (i = 0; i < triplet->chans.num_channels; i++) {
			if (first_channel + i * chan_increment == chan) {
				have_chan_pwr = true;
				*chan_pwr = triplet->chans.max_power;
				break;
			}
		}
		if (have_chan_pwr)
			break;

 next:
		triplet++;
		country_ie_len -= 3;
	}

	if (have_chan_pwr && pwr_constr_elem)
		*pwr_reduction = *pwr_constr_elem;
	else
		*pwr_reduction = 0;

	return have_chan_pwr;
}

static void ieee80211_find_cisco_dtpc(struct ieee80211_channel *channel,
				      const u8 *cisco_dtpc_ie,
				      int *pwr_level)
{
	/* From practical testing, the first data byte of the DTPC element
	 * seems to contain the requested dBm level, and the CLI on Cisco
	 * APs clearly state the range is -127 to 127 dBm, which indicates
	 * a signed byte, although it seemingly never actually goes negative.
	 * The other byte seems to always be zero.
	 */
	*pwr_level = (__s8)cisco_dtpc_ie[4];
}

static u64 ieee80211_handle_pwr_constr(struct ieee80211_link_data *link,
				       struct ieee80211_channel *channel,
				       struct ieee80211_mgmt *mgmt,
				       const u8 *country_ie, u8 country_ie_len,
				       const u8 *pwr_constr_ie,
				       const u8 *cisco_dtpc_ie)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	bool has_80211h_pwr = false, has_cisco_pwr = false;
	int chan_pwr = 0, pwr_reduction_80211h = 0;
	int pwr_level_cisco, pwr_level_80211h;
	int new_ap_level;
	__le16 capab = mgmt->u.probe_resp.capab_info;

	if (ieee80211_is_s1g_beacon(mgmt->frame_control))
		return 0;	/* TODO */

	if (country_ie &&
	    (capab & cpu_to_le16(WLAN_CAPABILITY_SPECTRUM_MGMT) ||
	     capab & cpu_to_le16(WLAN_CAPABILITY_RADIO_MEASURE))) {
		has_80211h_pwr = ieee80211_find_80211h_pwr_constr(
			channel, country_ie, country_ie_len,
			pwr_constr_ie, &chan_pwr, &pwr_reduction_80211h);
		pwr_level_80211h =
			max_t(int, 0, chan_pwr - pwr_reduction_80211h);
	}

	if (cisco_dtpc_ie) {
		ieee80211_find_cisco_dtpc(
			channel, cisco_dtpc_ie, &pwr_level_cisco);
		has_cisco_pwr = true;
	}

	if (!has_80211h_pwr && !has_cisco_pwr)
		return 0;

	/* If we have both 802.11h and Cisco DTPC, apply both limits
	 * by picking the smallest of the two power levels advertised.
	 */
	if (has_80211h_pwr &&
	    (!has_cisco_pwr || pwr_level_80211h <= pwr_level_cisco)) {
		new_ap_level = pwr_level_80211h;

		if (link->ap_power_level == new_ap_level)
			return 0;

		sdata_dbg(sdata,
			  "Limiting TX power to %d (%d - %d) dBm as advertised by %pM\n",
			  pwr_level_80211h, chan_pwr, pwr_reduction_80211h,
			  link->u.mgd.bssid);
	} else {  /* has_cisco_pwr is always true here. */
		new_ap_level = pwr_level_cisco;

		if (link->ap_power_level == new_ap_level)
			return 0;

		sdata_dbg(sdata,
			  "Limiting TX power to %d dBm as advertised by %pM\n",
			  pwr_level_cisco, link->u.mgd.bssid);
	}

	link->ap_power_level = new_ap_level;
	if (__ieee80211_recalc_txpower(link))
		return BSS_CHANGED_TXPOWER;
	return 0;
}

/* powersave */
static void ieee80211_enable_ps(struct ieee80211_local *local,
				struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_conf *conf = &local->hw.conf;

	/*
	 * If we are scanning right now then the parameters will
	 * take effect when scan finishes.
	 */
	if (local->scanning)
		return;

	if (conf->dynamic_ps_timeout > 0 &&
	    !ieee80211_hw_check(&local->hw, SUPPORTS_DYNAMIC_PS)) {
		mod_timer(&local->dynamic_ps_timer, jiffies +
			  msecs_to_jiffies(conf->dynamic_ps_timeout));
	} else {
		if (ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK))
			ieee80211_send_nullfunc(local, sdata, true);

		if (ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK) &&
		    ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
			return;

		conf->flags |= IEEE80211_CONF_PS;
		ieee80211_hw_config(local, -1, IEEE80211_CONF_CHANGE_PS);
	}
}

static void ieee80211_change_ps(struct ieee80211_local *local)
{
	struct ieee80211_conf *conf = &local->hw.conf;

	if (local->ps_sdata) {
		ieee80211_enable_ps(local, local->ps_sdata);
	} else if (conf->flags & IEEE80211_CONF_PS) {
		conf->flags &= ~IEEE80211_CONF_PS;
		ieee80211_hw_config(local, -1, IEEE80211_CONF_CHANGE_PS);
		timer_delete_sync(&local->dynamic_ps_timer);
		wiphy_work_cancel(local->hw.wiphy,
				  &local->dynamic_ps_enable_work);
	}
}

static bool ieee80211_powersave_allowed(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *mgd = &sdata->u.mgd;
	struct sta_info *sta = NULL;
	bool authorized = false;

	if (!mgd->powersave)
		return false;

	if (mgd->broken_ap)
		return false;

	if (!mgd->associated)
		return false;

	if (mgd->flags & IEEE80211_STA_CONNECTION_POLL)
		return false;

	if (!(local->hw.wiphy->flags & WIPHY_FLAG_SUPPORTS_MLO) &&
	    !sdata->deflink.u.mgd.have_beacon)
		return false;

	rcu_read_lock();
	sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	if (sta)
		authorized = test_sta_flag(sta, WLAN_STA_AUTHORIZED);
	rcu_read_unlock();

	return authorized;
}

/* need to hold RTNL or interface lock */
void ieee80211_recalc_ps(struct ieee80211_local *local)
{
	struct ieee80211_sub_if_data *sdata, *found = NULL;
	int count = 0;
	int timeout;

	if (!ieee80211_hw_check(&local->hw, SUPPORTS_PS) ||
	    ieee80211_hw_check(&local->hw, SUPPORTS_DYNAMIC_PS)) {
		local->ps_sdata = NULL;
		return;
	}

	list_for_each_entry(sdata, &local->interfaces, list) {
		if (!ieee80211_sdata_running(sdata))
			continue;
		if (sdata->vif.type == NL80211_IFTYPE_AP) {
			/* If an AP vif is found, then disable PS
			 * by setting the count to zero thereby setting
			 * ps_sdata to NULL.
			 */
			count = 0;
			break;
		}
		if (sdata->vif.type != NL80211_IFTYPE_STATION)
			continue;
		found = sdata;
		count++;
	}

	if (count == 1 && ieee80211_powersave_allowed(found)) {
		u8 dtimper = found->deflink.u.mgd.dtim_period;

		timeout = local->dynamic_ps_forced_timeout;
		if (timeout < 0)
			timeout = 100;
		local->hw.conf.dynamic_ps_timeout = timeout;

		/* If the TIM IE is invalid, pretend the value is 1 */
		if (!dtimper)
			dtimper = 1;

		local->hw.conf.ps_dtim_period = dtimper;
		local->ps_sdata = found;
	} else {
		local->ps_sdata = NULL;
	}

	ieee80211_change_ps(local);
}

void ieee80211_recalc_ps_vif(struct ieee80211_sub_if_data *sdata)
{
	bool ps_allowed = ieee80211_powersave_allowed(sdata);

	if (sdata->vif.cfg.ps != ps_allowed) {
		sdata->vif.cfg.ps = ps_allowed;
		ieee80211_vif_cfg_change_notify(sdata, BSS_CHANGED_PS);
	}
}

void ieee80211_dynamic_ps_disable_work(struct wiphy *wiphy,
				       struct wiphy_work *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local,
			     dynamic_ps_disable_work);

	if (local->hw.conf.flags & IEEE80211_CONF_PS) {
		local->hw.conf.flags &= ~IEEE80211_CONF_PS;
		ieee80211_hw_config(local, -1, IEEE80211_CONF_CHANGE_PS);
	}

	ieee80211_wake_queues_by_reason(&local->hw,
					IEEE80211_MAX_QUEUE_MAP,
					IEEE80211_QUEUE_STOP_REASON_PS,
					false);
}

void ieee80211_dynamic_ps_enable_work(struct wiphy *wiphy,
				      struct wiphy_work *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local,
			     dynamic_ps_enable_work);
	struct ieee80211_sub_if_data *sdata = local->ps_sdata;
	struct ieee80211_if_managed *ifmgd;
	unsigned long flags;
	int q;

	/* can only happen when PS was just disabled anyway */
	if (!sdata)
		return;

	ifmgd = &sdata->u.mgd;

	if (local->hw.conf.flags & IEEE80211_CONF_PS)
		return;

	if (local->hw.conf.dynamic_ps_timeout > 0) {
		/* don't enter PS if TX frames are pending */
		if (drv_tx_frames_pending(local)) {
			mod_timer(&local->dynamic_ps_timer, jiffies +
				  msecs_to_jiffies(
				  local->hw.conf.dynamic_ps_timeout));
			return;
		}

		/*
		 * transmission can be stopped by others which leads to
		 * dynamic_ps_timer expiry. Postpone the ps timer if it
		 * is not the actual idle state.
		 */
		spin_lock_irqsave(&local->queue_stop_reason_lock, flags);
		for (q = 0; q < local->hw.queues; q++) {
			if (local->queue_stop_reasons[q]) {
				spin_unlock_irqrestore(&local->queue_stop_reason_lock,
						       flags);
				mod_timer(&local->dynamic_ps_timer, jiffies +
					  msecs_to_jiffies(
					  local->hw.conf.dynamic_ps_timeout));
				return;
			}
		}
		spin_unlock_irqrestore(&local->queue_stop_reason_lock, flags);
	}

	if (ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK) &&
	    !(ifmgd->flags & IEEE80211_STA_NULLFUNC_ACKED)) {
		if (drv_tx_frames_pending(local)) {
			mod_timer(&local->dynamic_ps_timer, jiffies +
				  msecs_to_jiffies(
				  local->hw.conf.dynamic_ps_timeout));
		} else {
			ieee80211_send_nullfunc(local, sdata, true);
			/* Flush to get the tx status of nullfunc frame */
			ieee80211_flush_queues(local, sdata, false);
		}
	}

	if (!(ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS) &&
	      ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK)) ||
	    (ifmgd->flags & IEEE80211_STA_NULLFUNC_ACKED)) {
		ifmgd->flags &= ~IEEE80211_STA_NULLFUNC_ACKED;
		local->hw.conf.flags |= IEEE80211_CONF_PS;
		ieee80211_hw_config(local, -1, IEEE80211_CONF_CHANGE_PS);
	}
}

void ieee80211_dynamic_ps_timer(struct timer_list *t)
{
	struct ieee80211_local *local = timer_container_of(local, t,
							   dynamic_ps_timer);

	wiphy_work_queue(local->hw.wiphy, &local->dynamic_ps_enable_work);
}

void ieee80211_dfs_cac_timer_work(struct wiphy *wiphy, struct wiphy_work *work)
{
	struct ieee80211_link_data *link =
		container_of(work, struct ieee80211_link_data,
			     dfs_cac_timer_work.work);
	struct cfg80211_chan_def chandef = link->conf->chanreq.oper;
	struct ieee80211_sub_if_data *sdata = link->sdata;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (sdata->wdev.links[link->link_id].cac_started) {
		ieee80211_link_release_channel(link);
		cfg80211_cac_event(sdata->dev, &chandef,
				   NL80211_RADAR_CAC_FINISHED,
				   GFP_KERNEL, link->link_id);
	}
}

static bool
__ieee80211_sta_handle_tspec_ac_params(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	bool ret = false;
	int ac;

	if (local->hw.queues < IEEE80211_NUM_ACS)
		return false;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_sta_tx_tspec *tx_tspec = &ifmgd->tx_tspec[ac];
		int non_acm_ac;
		unsigned long now = jiffies;

		if (tx_tspec->action == TX_TSPEC_ACTION_NONE &&
		    tx_tspec->admitted_time &&
		    time_after(now, tx_tspec->time_slice_start + HZ)) {
			tx_tspec->consumed_tx_time = 0;
			tx_tspec->time_slice_start = now;

			if (tx_tspec->downgraded)
				tx_tspec->action =
					TX_TSPEC_ACTION_STOP_DOWNGRADE;
		}

		switch (tx_tspec->action) {
		case TX_TSPEC_ACTION_STOP_DOWNGRADE:
			/* take the original parameters */
			if (drv_conf_tx(local, &sdata->deflink, ac,
					&sdata->deflink.tx_conf[ac]))
				link_err(&sdata->deflink,
					 "failed to set TX queue parameters for queue %d\n",
					 ac);
			tx_tspec->action = TX_TSPEC_ACTION_NONE;
			tx_tspec->downgraded = false;
			ret = true;
			break;
		case TX_TSPEC_ACTION_DOWNGRADE:
			if (time_after(now, tx_tspec->time_slice_start + HZ)) {
				tx_tspec->action = TX_TSPEC_ACTION_NONE;
				ret = true;
				break;
			}
			/* downgrade next lower non-ACM AC */
			for (non_acm_ac = ac + 1;
			     non_acm_ac < IEEE80211_NUM_ACS;
			     non_acm_ac++)
				if (!(sdata->wmm_acm & BIT(7 - 2 * non_acm_ac)))
					break;
			/* Usually the loop will result in using BK even if it
			 * requires admission control, but such a configuration
			 * makes no sense and we have to transmit somehow - the
			 * AC selection does the same thing.
			 * If we started out trying to downgrade from BK, then
			 * the extra condition here might be needed.
			 */
			if (non_acm_ac >= IEEE80211_NUM_ACS)
				non_acm_ac = IEEE80211_AC_BK;
			if (drv_conf_tx(local, &sdata->deflink, ac,
					&sdata->deflink.tx_conf[non_acm_ac]))
				link_err(&sdata->deflink,
					 "failed to set TX queue parameters for queue %d\n",
					 ac);
			tx_tspec->action = TX_TSPEC_ACTION_NONE;
			ret = true;
			wiphy_delayed_work_queue(local->hw.wiphy,
						 &ifmgd->tx_tspec_wk,
						 tx_tspec->time_slice_start +
						 HZ - now + 1);
			break;
		case TX_TSPEC_ACTION_NONE:
			/* nothing now */
			break;
		}
	}

	return ret;
}

void ieee80211_sta_handle_tspec_ac_params(struct ieee80211_sub_if_data *sdata)
{
	if (__ieee80211_sta_handle_tspec_ac_params(sdata))
		ieee80211_link_info_change_notify(sdata, &sdata->deflink,
						  BSS_CHANGED_QOS);
}

static void ieee80211_sta_handle_tspec_ac_params_wk(struct wiphy *wiphy,
						    struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata;

	sdata = container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.tx_tspec_wk.work);
	ieee80211_sta_handle_tspec_ac_params(sdata);
}

void ieee80211_mgd_set_link_qos_params(struct ieee80211_link_data *link)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_tx_queue_params *params = link->tx_conf;
	u8 ac;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		mlme_dbg(sdata,
			 "WMM AC=%d acm=%d aifs=%d cWmin=%d cWmax=%d txop=%d uapsd=%d, downgraded=%d\n",
			 ac, params[ac].acm,
			 params[ac].aifs, params[ac].cw_min, params[ac].cw_max,
			 params[ac].txop, params[ac].uapsd,
			 ifmgd->tx_tspec[ac].downgraded);
		if (!ifmgd->tx_tspec[ac].downgraded &&
		    drv_conf_tx(local, link, ac, &params[ac]))
			link_err(link,
				 "failed to set TX queue parameters for AC %d\n",
				 ac);
	}
}

/* MLME */
static bool
_ieee80211_sta_wmm_params(struct ieee80211_local *local,
			  struct ieee80211_link_data *link,
			  const u8 *wmm_param, size_t wmm_param_len,
			  const struct ieee80211_mu_edca_param_set *mu_edca)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_tx_queue_params params[IEEE80211_NUM_ACS];
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	size_t left;
	int count, mu_edca_count, ac;
	const u8 *pos;
	u8 uapsd_queues = 0;

	if (!local->ops->conf_tx)
		return false;

	if (local->hw.queues < IEEE80211_NUM_ACS)
		return false;

	if (!wmm_param)
		return false;

	if (wmm_param_len < 8 || wmm_param[5] /* version */ != 1)
		return false;

	if (ifmgd->flags & IEEE80211_STA_UAPSD_ENABLED)
		uapsd_queues = ifmgd->uapsd_queues;

	count = wmm_param[6] & 0x0f;
	/* -1 is the initial value of ifmgd->mu_edca_last_param_set.
	 * if mu_edca was preset before and now it disappeared tell
	 * the driver about it.
	 */
	mu_edca_count = mu_edca ? mu_edca->mu_qos_info & 0x0f : -1;
	if (count == link->u.mgd.wmm_last_param_set &&
	    mu_edca_count == link->u.mgd.mu_edca_last_param_set)
		return false;
	link->u.mgd.wmm_last_param_set = count;
	link->u.mgd.mu_edca_last_param_set = mu_edca_count;

	pos = wmm_param + 8;
	left = wmm_param_len - 8;

	memset(&params, 0, sizeof(params));

	sdata->wmm_acm = 0;
	for (; left >= 4; left -= 4, pos += 4) {
		int aci = (pos[0] >> 5) & 0x03;
		int acm = (pos[0] >> 4) & 0x01;
		bool uapsd = false;

		switch (aci) {
		case 1: /* AC_BK */
			ac = IEEE80211_AC_BK;
			if (acm)
				sdata->wmm_acm |= BIT(1) | BIT(2); /* BK/- */
			if (uapsd_queues & IEEE80211_WMM_IE_STA_QOSINFO_AC_BK)
				uapsd = true;
			params[ac].mu_edca = !!mu_edca;
			if (mu_edca)
				params[ac].mu_edca_param_rec = mu_edca->ac_bk;
			break;
		case 2: /* AC_VI */
			ac = IEEE80211_AC_VI;
			if (acm)
				sdata->wmm_acm |= BIT(4) | BIT(5); /* CL/VI */
			if (uapsd_queues & IEEE80211_WMM_IE_STA_QOSINFO_AC_VI)
				uapsd = true;
			params[ac].mu_edca = !!mu_edca;
			if (mu_edca)
				params[ac].mu_edca_param_rec = mu_edca->ac_vi;
			break;
		case 3: /* AC_VO */
			ac = IEEE80211_AC_VO;
			if (acm)
				sdata->wmm_acm |= BIT(6) | BIT(7); /* VO/NC */
			if (uapsd_queues & IEEE80211_WMM_IE_STA_QOSINFO_AC_VO)
				uapsd = true;
			params[ac].mu_edca = !!mu_edca;
			if (mu_edca)
				params[ac].mu_edca_param_rec = mu_edca->ac_vo;
			break;
		case 0: /* AC_BE */
		default:
			ac = IEEE80211_AC_BE;
			if (acm)
				sdata->wmm_acm |= BIT(0) | BIT(3); /* BE/EE */
			if (uapsd_queues & IEEE80211_WMM_IE_STA_QOSINFO_AC_BE)
				uapsd = true;
			params[ac].mu_edca = !!mu_edca;
			if (mu_edca)
				params[ac].mu_edca_param_rec = mu_edca->ac_be;
			break;
		}

		params[ac].aifs = pos[0] & 0x0f;

		if (params[ac].aifs < 2) {
			link_info(link,
				  "AP has invalid WMM params (AIFSN=%d for ACI %d), will use 2\n",
				  params[ac].aifs, aci);
			params[ac].aifs = 2;
		}
		params[ac].cw_max = ecw2cw((pos[1] & 0xf0) >> 4);
		params[ac].cw_min = ecw2cw(pos[1] & 0x0f);
		params[ac].txop = get_unaligned_le16(pos + 2);
		params[ac].acm = acm;
		params[ac].uapsd = uapsd;

		if (params[ac].cw_min == 0 ||
		    params[ac].cw_min > params[ac].cw_max) {
			link_info(link,
				  "AP has invalid WMM params (CWmin/max=%d/%d for ACI %d), using defaults\n",
				  params[ac].cw_min, params[ac].cw_max, aci);
			return false;
		}
		ieee80211_regulatory_limit_wmm_params(sdata, &params[ac], ac);
	}

	/* WMM specification requires all 4 ACIs. */
	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		if (params[ac].cw_min == 0) {
			link_info(link,
				  "AP has invalid WMM params (missing AC %d), using defaults\n",
				  ac);
			return false;
		}
	}

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
		link->tx_conf[ac] = params[ac];

	return true;
}

static bool
ieee80211_sta_wmm_params(struct ieee80211_local *local,
			 struct ieee80211_link_data *link,
			 const u8 *wmm_param, size_t wmm_param_len,
			 const struct ieee80211_mu_edca_param_set *mu_edca)
{
	if (!_ieee80211_sta_wmm_params(local, link, wmm_param, wmm_param_len,
				       mu_edca))
		return false;

	ieee80211_mgd_set_link_qos_params(link);

	/* enable WMM or activate new settings */
	link->conf->qos = true;
	return true;
}

static void __ieee80211_stop_poll(struct ieee80211_sub_if_data *sdata)
{
	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	sdata->u.mgd.flags &= ~IEEE80211_STA_CONNECTION_POLL;
	ieee80211_run_deferred_scan(sdata->local);
}

static void ieee80211_stop_poll(struct ieee80211_sub_if_data *sdata)
{
	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	__ieee80211_stop_poll(sdata);
}

static u64 ieee80211_handle_bss_capability(struct ieee80211_link_data *link,
					   u16 capab, bool erp_valid, u8 erp)
{
	struct ieee80211_bss_conf *bss_conf = link->conf;
	struct ieee80211_supported_band *sband;
	u64 changed = 0;
	bool use_protection;
	bool use_short_preamble;
	bool use_short_slot;

	sband = ieee80211_get_link_sband(link);
	if (!sband)
		return changed;

	if (erp_valid) {
		use_protection = (erp & WLAN_ERP_USE_PROTECTION) != 0;
		use_short_preamble = (erp & WLAN_ERP_BARKER_PREAMBLE) == 0;
	} else {
		use_protection = false;
		use_short_preamble = !!(capab & WLAN_CAPABILITY_SHORT_PREAMBLE);
	}

	use_short_slot = !!(capab & WLAN_CAPABILITY_SHORT_SLOT_TIME);
	if (sband->band == NL80211_BAND_5GHZ ||
	    sband->band == NL80211_BAND_6GHZ)
		use_short_slot = true;

	if (use_protection != bss_conf->use_cts_prot) {
		bss_conf->use_cts_prot = use_protection;
		changed |= BSS_CHANGED_ERP_CTS_PROT;
	}

	if (use_short_preamble != bss_conf->use_short_preamble) {
		bss_conf->use_short_preamble = use_short_preamble;
		changed |= BSS_CHANGED_ERP_PREAMBLE;
	}

	if (use_short_slot != bss_conf->use_short_slot) {
		bss_conf->use_short_slot = use_short_slot;
		changed |= BSS_CHANGED_ERP_SLOT;
	}

	return changed;
}

static u64 ieee80211_link_set_associated(struct ieee80211_link_data *link,
					 struct cfg80211_bss *cbss)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_bss_conf *bss_conf = link->conf;
	struct ieee80211_bss *bss = (void *)cbss->priv;
	u64 changed = BSS_CHANGED_QOS;

	/* not really used in MLO */
	sdata->u.mgd.beacon_timeout =
		usecs_to_jiffies(ieee80211_tu_to_usec(beacon_loss_count *
						      bss_conf->beacon_int));

	changed |= ieee80211_handle_bss_capability(link,
						   bss_conf->assoc_capability,
						   bss->has_erp_value,
						   bss->erp_value);

	ieee80211_check_rate_mask(link);

	link->conf->bss = cbss;
	memcpy(link->u.mgd.bssid, cbss->bssid, ETH_ALEN);

	if (sdata->vif.p2p ||
	    sdata->vif.driver_flags & IEEE80211_VIF_GET_NOA_UPDATE) {
		const struct cfg80211_bss_ies *ies;

		rcu_read_lock();
		ies = rcu_dereference(cbss->ies);
		if (ies) {
			int ret;

			ret = cfg80211_get_p2p_attr(
					ies->data, ies->len,
					IEEE80211_P2P_ATTR_ABSENCE_NOTICE,
					(u8 *) &bss_conf->p2p_noa_attr,
					sizeof(bss_conf->p2p_noa_attr));
			if (ret >= 2) {
				link->u.mgd.p2p_noa_index =
					bss_conf->p2p_noa_attr.index;
				changed |= BSS_CHANGED_P2P_PS;
			}
		}
		rcu_read_unlock();
	}

	if (link->u.mgd.have_beacon) {
		bss_conf->beacon_rate = bss->beacon_rate;
		changed |= BSS_CHANGED_BEACON_INFO;
	} else {
		bss_conf->beacon_rate = NULL;
	}

	/* Tell the driver to monitor connection quality (if supported) */
	if (sdata->vif.driver_flags & IEEE80211_VIF_SUPPORTS_CQM_RSSI &&
	    bss_conf->cqm_rssi_thold)
		changed |= BSS_CHANGED_CQM;

	return changed;
}

static void ieee80211_set_associated(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgd_assoc_data *assoc_data,
				     u64 changed[IEEE80211_MLD_MAX_NUM_LINKS])
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_vif_cfg *vif_cfg = &sdata->vif.cfg;
	u64 vif_changed = BSS_CHANGED_ASSOC;
	unsigned int link_id;

	lockdep_assert_wiphy(local->hw.wiphy);

	sdata->u.mgd.associated = true;

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;
		struct ieee80211_link_data *link;

		if (!cbss ||
		    assoc_data->link[link_id].status != WLAN_STATUS_SUCCESS)
			continue;

		if (ieee80211_vif_is_mld(&sdata->vif) &&
		    !(ieee80211_vif_usable_links(&sdata->vif) & BIT(link_id)))
			continue;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (WARN_ON(!link))
			return;

		changed[link_id] |= ieee80211_link_set_associated(link, cbss);
	}

	/* just to be sure */
	ieee80211_stop_poll(sdata);

	ieee80211_led_assoc(local, 1);

	vif_cfg->assoc = 1;

	/* Enable ARP filtering */
	if (vif_cfg->arp_addr_cnt)
		vif_changed |= BSS_CHANGED_ARP_FILTER;

	if (ieee80211_vif_is_mld(&sdata->vif)) {
		for (link_id = 0;
		     link_id < IEEE80211_MLD_MAX_NUM_LINKS;
		     link_id++) {
			struct ieee80211_link_data *link;
			struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;

			if (!cbss ||
			    !(BIT(link_id) &
			      ieee80211_vif_usable_links(&sdata->vif)) ||
			    assoc_data->link[link_id].status != WLAN_STATUS_SUCCESS)
				continue;

			link = sdata_dereference(sdata->link[link_id], sdata);
			if (WARN_ON(!link))
				return;

			ieee80211_link_info_change_notify(sdata, link,
							  changed[link_id]);

			ieee80211_recalc_smps(sdata, link);
		}

		ieee80211_vif_cfg_change_notify(sdata, vif_changed);
	} else {
		ieee80211_bss_info_change_notify(sdata,
						 vif_changed | changed[0]);
	}

	ieee80211_recalc_ps(local);

	/* leave this here to not change ordering in non-MLO cases */
	if (!ieee80211_vif_is_mld(&sdata->vif))
		ieee80211_recalc_smps(sdata, &sdata->deflink);
	ieee80211_recalc_ps_vif(sdata);

	netif_carrier_on(sdata->dev);
}

static void ieee80211_ml_reconf_reset(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_mgd_assoc_data *add_links_data =
		sdata->u.mgd.reconf.add_links_data;

	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    !(sdata->u.mgd.reconf.added_links |
	      sdata->u.mgd.reconf.removed_links))
		return;

	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &sdata->u.mgd.reconf.wk);
	sdata->u.mgd.reconf.added_links = 0;
	sdata->u.mgd.reconf.removed_links = 0;
	sdata->u.mgd.reconf.dialog_token = 0;

	if (add_links_data) {
		struct cfg80211_mlo_reconf_done_data done_data = {};
		u8 link_id;

		for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS;
		     link_id++)
			done_data.links[link_id].bss =
				add_links_data->link[link_id].bss;

		cfg80211_mlo_reconf_add_done(sdata->dev, &done_data);

		kfree(sdata->u.mgd.reconf.add_links_data);
		sdata->u.mgd.reconf.add_links_data = NULL;
	}
}

static void ieee80211_set_disassoc(struct ieee80211_sub_if_data *sdata,
				   u16 stype, u16 reason, bool tx,
				   u8 *frame_buf)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_local *local = sdata->local;
	struct sta_info *ap_sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	unsigned int link_id;
	u64 changed = 0;
	struct ieee80211_prep_tx_info info = {
		.subtype = stype,
		.was_assoc = true,
		.link_id = ffs(sdata->vif.active_links) - 1,
	};

	lockdep_assert_wiphy(local->hw.wiphy);

	if (WARN_ON(!ap_sta))
		return;

	if (WARN_ON_ONCE(tx && !frame_buf))
		return;

	if (WARN_ON(!ifmgd->associated))
		return;

	ieee80211_stop_poll(sdata);

	ifmgd->associated = false;

	if (tx) {
		bool tx_link_found = false;

		for (link_id = 0;
		     link_id < ARRAY_SIZE(sdata->link);
		     link_id++) {
			struct ieee80211_link_data *link;

			if (!ieee80211_vif_link_active(&sdata->vif, link_id))
				continue;

			link = sdata_dereference(sdata->link[link_id], sdata);
			if (WARN_ON_ONCE(!link))
				continue;

			if (link->u.mgd.csa.blocked_tx)
				continue;

			tx_link_found = true;
			break;
		}

		tx = tx_link_found;
	}

	/* other links will be destroyed */
	sdata->deflink.conf->bss = NULL;
	sdata->deflink.conf->epcs_support = false;
	sdata->deflink.smps_mode = IEEE80211_SMPS_OFF;

	netif_carrier_off(sdata->dev);

	/*
	 * if we want to get out of ps before disassoc (why?) we have
	 * to do it before sending disassoc, as otherwise the null-packet
	 * won't be valid.
	 */
	if (local->hw.conf.flags & IEEE80211_CONF_PS) {
		local->hw.conf.flags &= ~IEEE80211_CONF_PS;
		ieee80211_hw_config(local, -1, IEEE80211_CONF_CHANGE_PS);
	}
	local->ps_sdata = NULL;

	/* disable per-vif ps */
	ieee80211_recalc_ps_vif(sdata);

	/* make sure ongoing transmission finishes */
	synchronize_net();

	/*
	 * drop any frame before deauth/disassoc, this can be data or
	 * management frame. Since we are disconnecting, we should not
	 * insist sending these frames which can take time and delay
	 * the disconnection and possible the roaming.
	 */
	ieee80211_flush_queues(local, sdata, true);

	if (tx) {
		drv_mgd_prepare_tx(sdata->local, sdata, &info);

		ieee80211_send_deauth_disassoc(sdata, sdata->vif.cfg.ap_addr,
					       sdata->vif.cfg.ap_addr, stype,
					       reason, true, frame_buf);

		/* flush out frame - make sure the deauth was actually sent */
		ieee80211_flush_queues(local, sdata, false);

		drv_mgd_complete_tx(sdata->local, sdata, &info);
	} else if (frame_buf) {
		ieee80211_send_deauth_disassoc(sdata, sdata->vif.cfg.ap_addr,
					       sdata->vif.cfg.ap_addr, stype,
					       reason, false, frame_buf);
	}

	/* clear AP addr only after building the needed mgmt frames */
	eth_zero_addr(sdata->deflink.u.mgd.bssid);
	eth_zero_addr(sdata->vif.cfg.ap_addr);

	sdata->vif.cfg.ssid_len = 0;

	/* Remove TDLS peers */
	__sta_info_flush(sdata, false, -1, ap_sta);

	if (sdata->vif.driver_flags & IEEE80211_VIF_REMOVE_AP_AFTER_DISASSOC) {
		/* Only move the AP state */
		sta_info_move_state(ap_sta, IEEE80211_STA_NONE);
	} else {
		/* Remove AP peer */
		sta_info_flush(sdata, -1);
	}

	/* finally reset all BSS / config parameters */
	if (!ieee80211_vif_is_mld(&sdata->vif))
		changed |= ieee80211_reset_erp_info(sdata);

	ieee80211_led_assoc(local, 0);
	changed |= BSS_CHANGED_ASSOC;
	sdata->vif.cfg.assoc = false;

	sdata->deflink.u.mgd.p2p_noa_index = -1;
	memset(&sdata->vif.bss_conf.p2p_noa_attr, 0,
	       sizeof(sdata->vif.bss_conf.p2p_noa_attr));

	/* on the next assoc, re-program HT/VHT parameters */
	memset(&ifmgd->ht_capa, 0, sizeof(ifmgd->ht_capa));
	memset(&ifmgd->ht_capa_mask, 0, sizeof(ifmgd->ht_capa_mask));
	memset(&ifmgd->vht_capa, 0, sizeof(ifmgd->vht_capa));
	memset(&ifmgd->vht_capa_mask, 0, sizeof(ifmgd->vht_capa_mask));

	/*
	 * reset MU-MIMO ownership and group data in default link,
	 * if used, other links are destroyed
	 */
	memset(sdata->vif.bss_conf.mu_group.membership, 0,
	       sizeof(sdata->vif.bss_conf.mu_group.membership));
	memset(sdata->vif.bss_conf.mu_group.position, 0,
	       sizeof(sdata->vif.bss_conf.mu_group.position));
	if (!ieee80211_vif_is_mld(&sdata->vif))
		changed |= BSS_CHANGED_MU_GROUPS;
	sdata->vif.bss_conf.mu_mimo_owner = false;

	sdata->deflink.ap_power_level = IEEE80211_UNSET_POWER_LEVEL;

	timer_delete_sync(&local->dynamic_ps_timer);
	wiphy_work_cancel(local->hw.wiphy, &local->dynamic_ps_enable_work);

	/* Disable ARP filtering */
	if (sdata->vif.cfg.arp_addr_cnt)
		changed |= BSS_CHANGED_ARP_FILTER;

	sdata->vif.bss_conf.qos = false;
	if (!ieee80211_vif_is_mld(&sdata->vif)) {
		changed |= BSS_CHANGED_QOS;
		/* The BSSID (not really interesting) and HT changed */
		changed |= BSS_CHANGED_BSSID | BSS_CHANGED_HT;
		ieee80211_bss_info_change_notify(sdata, changed);
	} else {
		ieee80211_vif_cfg_change_notify(sdata, changed);
	}

	if (sdata->vif.driver_flags & IEEE80211_VIF_REMOVE_AP_AFTER_DISASSOC) {
		/*
		 * After notifying the driver about the disassoc,
		 * remove the ap sta.
		 */
		sta_info_flush(sdata, -1);
	}

	/* disassociated - set to defaults now */
	ieee80211_set_wmm_default(&sdata->deflink, false, false);

	timer_delete_sync(&sdata->u.mgd.conn_mon_timer);
	timer_delete_sync(&sdata->u.mgd.bcn_mon_timer);
	timer_delete_sync(&sdata->u.mgd.timer);

	sdata->vif.bss_conf.dtim_period = 0;
	sdata->vif.bss_conf.beacon_rate = NULL;

	sdata->deflink.u.mgd.have_beacon = false;
	sdata->deflink.u.mgd.tracking_signal_avg = false;
	sdata->deflink.u.mgd.disable_wmm_tracking = false;

	ifmgd->flags = 0;

	for (link_id = 0; link_id < ARRAY_SIZE(sdata->link); link_id++) {
		struct ieee80211_link_data *link;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link)
			continue;
		ieee80211_link_release_channel(link);
	}

	sdata->vif.bss_conf.csa_active = false;
	sdata->deflink.u.mgd.csa.blocked_tx = false;
	sdata->deflink.u.mgd.csa.waiting_bcn = false;
	sdata->deflink.u.mgd.csa.ignored_same_chan = false;
	ieee80211_vif_unblock_queues_csa(sdata);

	/* existing TX TSPEC sessions no longer exist */
	memset(ifmgd->tx_tspec, 0, sizeof(ifmgd->tx_tspec));
	wiphy_delayed_work_cancel(local->hw.wiphy, &ifmgd->tx_tspec_wk);

	sdata->vif.bss_conf.power_type = IEEE80211_REG_UNSET_AP;
	sdata->vif.bss_conf.pwr_reduction = 0;
	ieee80211_clear_tpe(&sdata->vif.bss_conf.tpe);

	sdata->vif.cfg.eml_cap = 0;
	sdata->vif.cfg.eml_med_sync_delay = 0;
	sdata->vif.cfg.mld_capa_op = 0;

	memset(&sdata->u.mgd.ttlm_info, 0,
	       sizeof(sdata->u.mgd.ttlm_info));
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy, &ifmgd->ttlm_work);

	memset(&sdata->vif.neg_ttlm, 0, sizeof(sdata->vif.neg_ttlm));
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &ifmgd->neg_ttlm_timeout_work);

	sdata->u.mgd.removed_links = 0;
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &sdata->u.mgd.ml_reconf_work);

	wiphy_work_cancel(sdata->local->hw.wiphy,
			  &ifmgd->teardown_ttlm_work);

	/* if disconnection happens in the middle of the ML reconfiguration
	 * flow, cfg80211 must called to release the BSS references obtained
	 * when the flow started.
	 */
	ieee80211_ml_reconf_reset(sdata);

	ieee80211_vif_set_links(sdata, 0, 0);

	ifmgd->mcast_seq_last = IEEE80211_SN_MODULO;

	ifmgd->epcs.enabled = false;
	ifmgd->epcs.dialog_token = 0;

	memset(ifmgd->userspace_selectors, 0,
	       sizeof(ifmgd->userspace_selectors));
}

static void ieee80211_reset_ap_probe(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_local *local = sdata->local;

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!(ifmgd->flags & IEEE80211_STA_CONNECTION_POLL))
		return;

	__ieee80211_stop_poll(sdata);

	ieee80211_recalc_ps(local);

	if (ieee80211_hw_check(&sdata->local->hw, CONNECTION_MONITOR))
		return;

	/*
	 * We've received a probe response, but are not sure whether
	 * we have or will be receiving any beacons or data, so let's
	 * schedule the timers again, just in case.
	 */
	ieee80211_sta_reset_beacon_monitor(sdata);

	mod_timer(&ifmgd->conn_mon_timer,
		  round_jiffies_up(jiffies +
				   IEEE80211_CONNECTION_IDLE_TIME));
}

static void ieee80211_sta_tx_wmm_ac_notify(struct ieee80211_sub_if_data *sdata,
					   struct ieee80211_hdr *hdr,
					   u16 tx_time)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u16 tid;
	int ac;
	struct ieee80211_sta_tx_tspec *tx_tspec;
	unsigned long now = jiffies;

	if (!ieee80211_is_data_qos(hdr->frame_control))
		return;

	tid = ieee80211_get_tid(hdr);
	ac = ieee80211_ac_from_tid(tid);
	tx_tspec = &ifmgd->tx_tspec[ac];

	if (likely(!tx_tspec->admitted_time))
		return;

	if (time_after(now, tx_tspec->time_slice_start + HZ)) {
		tx_tspec->consumed_tx_time = 0;
		tx_tspec->time_slice_start = now;

		if (tx_tspec->downgraded) {
			tx_tspec->action = TX_TSPEC_ACTION_STOP_DOWNGRADE;
			wiphy_delayed_work_queue(sdata->local->hw.wiphy,
						 &ifmgd->tx_tspec_wk, 0);
		}
	}

	if (tx_tspec->downgraded)
		return;

	tx_tspec->consumed_tx_time += tx_time;

	if (tx_tspec->consumed_tx_time >= tx_tspec->admitted_time) {
		tx_tspec->downgraded = true;
		tx_tspec->action = TX_TSPEC_ACTION_DOWNGRADE;
		wiphy_delayed_work_queue(sdata->local->hw.wiphy,
					 &ifmgd->tx_tspec_wk, 0);
	}
}

void ieee80211_sta_tx_notify(struct ieee80211_sub_if_data *sdata,
			     struct ieee80211_hdr *hdr, bool ack, u16 tx_time)
{
	ieee80211_sta_tx_wmm_ac_notify(sdata, hdr, tx_time);

	if (!ieee80211_is_any_nullfunc(hdr->frame_control) ||
	    !sdata->u.mgd.probe_send_count)
		return;

	if (ack)
		sdata->u.mgd.probe_send_count = 0;
	else
		sdata->u.mgd.nullfunc_failed = true;
	wiphy_work_queue(sdata->local->hw.wiphy, &sdata->work);
}

static void ieee80211_mlme_send_probe_req(struct ieee80211_sub_if_data *sdata,
					  const u8 *src, const u8 *dst,
					  const u8 *ssid, size_t ssid_len,
					  struct ieee80211_channel *channel)
{
	struct sk_buff *skb;

	skb = ieee80211_build_probe_req(sdata, src, dst, (u32)-1, channel,
					ssid, ssid_len, NULL, 0,
					IEEE80211_PROBE_FLAG_DIRECTED);
	if (skb)
		ieee80211_tx_skb(sdata, skb);
}

static void ieee80211_mgd_probe_ap_send(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u8 *dst = sdata->vif.cfg.ap_addr;
	u8 unicast_limit = max(1, max_probe_tries - 3);
	struct sta_info *sta;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (WARN_ON(ieee80211_vif_is_mld(&sdata->vif)))
		return;

	/*
	 * Try sending broadcast probe requests for the last three
	 * probe requests after the first ones failed since some
	 * buggy APs only support broadcast probe requests.
	 */
	if (ifmgd->probe_send_count >= unicast_limit)
		dst = NULL;

	/*
	 * When the hardware reports an accurate Tx ACK status, it's
	 * better to send a nullfunc frame instead of a probe request,
	 * as it will kick us off the AP quickly if we aren't associated
	 * anymore. The timeout will be reset if the frame is ACKed by
	 * the AP.
	 */
	ifmgd->probe_send_count++;

	if (dst) {
		sta = sta_info_get(sdata, dst);
		if (!WARN_ON(!sta))
			ieee80211_check_fast_rx(sta);
	}

	if (ieee80211_hw_check(&sdata->local->hw, REPORTS_TX_ACK_STATUS)) {
		ifmgd->nullfunc_failed = false;
		ieee80211_send_nullfunc(sdata->local, sdata, false);
	} else {
		ieee80211_mlme_send_probe_req(sdata, sdata->vif.addr, dst,
					      sdata->vif.cfg.ssid,
					      sdata->vif.cfg.ssid_len,
					      sdata->deflink.conf->bss->channel);
	}

	ifmgd->probe_timeout = jiffies + msecs_to_jiffies(probe_wait_ms);
	run_again(sdata, ifmgd->probe_timeout);
}

static void ieee80211_mgd_probe_ap(struct ieee80211_sub_if_data *sdata,
				   bool beacon)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	bool already = false;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (WARN_ON_ONCE(ieee80211_vif_is_mld(&sdata->vif)))
		return;

	if (!ieee80211_sdata_running(sdata))
		return;

	if (!ifmgd->associated)
		return;

	if (sdata->local->tmp_channel || sdata->local->scanning)
		return;

	if (sdata->local->suspending) {
		/* reschedule after resume */
		ieee80211_reset_ap_probe(sdata);
		return;
	}

	if (beacon) {
		mlme_dbg_ratelimited(sdata,
				     "detected beacon loss from AP (missed %d beacons) - probing\n",
				     beacon_loss_count);

		ieee80211_cqm_beacon_loss_notify(&sdata->vif, GFP_KERNEL);
	}

	/*
	 * The driver/our work has already reported this event or the
	 * connection monitoring has kicked in and we have already sent
	 * a probe request. Or maybe the AP died and the driver keeps
	 * reporting until we disassociate...
	 *
	 * In either case we have to ignore the current call to this
	 * function (except for setting the correct probe reason bit)
	 * because otherwise we would reset the timer every time and
	 * never check whether we received a probe response!
	 */
	if (ifmgd->flags & IEEE80211_STA_CONNECTION_POLL)
		already = true;

	ifmgd->flags |= IEEE80211_STA_CONNECTION_POLL;

	if (already)
		return;

	ieee80211_recalc_ps(sdata->local);

	ifmgd->probe_send_count = 0;
	ieee80211_mgd_probe_ap_send(sdata);
}

struct sk_buff *ieee80211_ap_probereq_get(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct cfg80211_bss *cbss;
	struct sk_buff *skb;
	const struct element *ssid;
	int ssid_len;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_STATION ||
		    ieee80211_vif_is_mld(&sdata->vif)))
		return NULL;

	if (ifmgd->associated)
		cbss = sdata->deflink.conf->bss;
	else if (ifmgd->auth_data)
		cbss = ifmgd->auth_data->bss;
	else if (ifmgd->assoc_data && ifmgd->assoc_data->link[0].bss)
		cbss = ifmgd->assoc_data->link[0].bss;
	else
		return NULL;

	rcu_read_lock();
	ssid = ieee80211_bss_get_elem(cbss, WLAN_EID_SSID);
	if (WARN_ONCE(!ssid || ssid->datalen > IEEE80211_MAX_SSID_LEN,
		      "invalid SSID element (len=%d)",
		      ssid ? ssid->datalen : -1))
		ssid_len = 0;
	else
		ssid_len = ssid->datalen;

	skb = ieee80211_build_probe_req(sdata, sdata->vif.addr, cbss->bssid,
					(u32) -1, cbss->channel,
					ssid->data, ssid_len,
					NULL, 0, IEEE80211_PROBE_FLAG_DIRECTED);
	rcu_read_unlock();

	return skb;
}
EXPORT_SYMBOL(ieee80211_ap_probereq_get);

static void ieee80211_report_disconnect(struct ieee80211_sub_if_data *sdata,
					const u8 *buf, size_t len, bool tx,
					u16 reason, bool reconnect)
{
	struct ieee80211_event event = {
		.type = MLME_EVENT,
		.u.mlme.data = tx ? DEAUTH_TX_EVENT : DEAUTH_RX_EVENT,
		.u.mlme.reason = reason,
	};

	if (tx)
		cfg80211_tx_mlme_mgmt(sdata->dev, buf, len, reconnect);
	else
		cfg80211_rx_mlme_mgmt(sdata->dev, buf, len);

	drv_event_callback(sdata->local, sdata, &event);
}

static void __ieee80211_disconnect(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

	lockdep_assert_wiphy(local->hw.wiphy);

	if (!ifmgd->associated)
		return;

	if (!ifmgd->driver_disconnect) {
		unsigned int link_id;

		/*
		 * AP is probably out of range (or not reachable for another
		 * reason) so remove the bss structs for that AP. In the case
		 * of multi-link, it's not clear that all of them really are
		 * out of range, but if they weren't the driver likely would
		 * have switched to just have a single link active?
		 */
		for (link_id = 0;
		     link_id < ARRAY_SIZE(sdata->link);
		     link_id++) {
			struct ieee80211_link_data *link;

			link = sdata_dereference(sdata->link[link_id], sdata);
			if (!link || !link->conf->bss)
				continue;
			cfg80211_unlink_bss(local->hw.wiphy, link->conf->bss);
			link->conf->bss = NULL;
		}
	}

	ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH,
			       ifmgd->driver_disconnect ?
					WLAN_REASON_DEAUTH_LEAVING :
					WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY,
			       true, frame_buf);
	/* the other links will be destroyed */
	sdata->vif.bss_conf.csa_active = false;
	sdata->deflink.u.mgd.csa.waiting_bcn = false;
	sdata->deflink.u.mgd.csa.blocked_tx = false;
	ieee80211_vif_unblock_queues_csa(sdata);

	ieee80211_report_disconnect(sdata, frame_buf, sizeof(frame_buf), true,
				    WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY,
				    ifmgd->reconnect);
	ifmgd->reconnect = false;
}

static void ieee80211_beacon_connection_loss_work(struct wiphy *wiphy,
						  struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.beacon_connection_loss_work);
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	if (ifmgd->connection_loss) {
		sdata_info(sdata, "Connection to AP %pM lost\n",
			   sdata->vif.cfg.ap_addr);
		__ieee80211_disconnect(sdata);
		ifmgd->connection_loss = false;
	} else if (ifmgd->driver_disconnect) {
		sdata_info(sdata,
			   "Driver requested disconnection from AP %pM\n",
			   sdata->vif.cfg.ap_addr);
		__ieee80211_disconnect(sdata);
		ifmgd->driver_disconnect = false;
	} else {
		if (ifmgd->associated)
			sdata->deflink.u.mgd.beacon_loss_count++;
		ieee80211_mgd_probe_ap(sdata, true);
	}
}

static void ieee80211_csa_connection_drop_work(struct wiphy *wiphy,
					       struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.csa_connection_drop_work);

	__ieee80211_disconnect(sdata);
}

void ieee80211_beacon_loss(struct ieee80211_vif *vif)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);
	struct ieee80211_hw *hw = &sdata->local->hw;

	trace_api_beacon_loss(sdata);

	sdata->u.mgd.connection_loss = false;
	wiphy_work_queue(hw->wiphy, &sdata->u.mgd.beacon_connection_loss_work);
}
EXPORT_SYMBOL(ieee80211_beacon_loss);

void ieee80211_connection_loss(struct ieee80211_vif *vif)
{
	struct ieee80211_sub_if_data *sdata;
	struct ieee80211_hw *hw;

	KUNIT_STATIC_STUB_REDIRECT(ieee80211_connection_loss, vif);

	sdata = vif_to_sdata(vif);
	hw = &sdata->local->hw;

	trace_api_connection_loss(sdata);

	sdata->u.mgd.connection_loss = true;
	wiphy_work_queue(hw->wiphy, &sdata->u.mgd.beacon_connection_loss_work);
}
EXPORT_SYMBOL(ieee80211_connection_loss);

void ieee80211_disconnect(struct ieee80211_vif *vif, bool reconnect)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);
	struct ieee80211_hw *hw = &sdata->local->hw;

	trace_api_disconnect(sdata, reconnect);

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_STATION))
		return;

	sdata->u.mgd.driver_disconnect = true;
	sdata->u.mgd.reconnect = reconnect;
	wiphy_work_queue(hw->wiphy, &sdata->u.mgd.beacon_connection_loss_work);
}
EXPORT_SYMBOL(ieee80211_disconnect);

static void ieee80211_destroy_auth_data(struct ieee80211_sub_if_data *sdata,
					bool assoc)
{
	struct ieee80211_mgd_auth_data *auth_data = sdata->u.mgd.auth_data;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	sdata->u.mgd.auth_data = NULL;

	if (!assoc) {
		/*
		 * we are not authenticated yet, the only timer that could be
		 * running is the timeout for the authentication response which
		 * which is not relevant anymore.
		 */
		timer_delete_sync(&sdata->u.mgd.timer);
		sta_info_destroy_addr(sdata, auth_data->ap_addr);

		/* other links are destroyed */
		eth_zero_addr(sdata->deflink.u.mgd.bssid);
		ieee80211_link_info_change_notify(sdata, &sdata->deflink,
						  BSS_CHANGED_BSSID);
		sdata->u.mgd.flags = 0;

		ieee80211_link_release_channel(&sdata->deflink);
		ieee80211_vif_set_links(sdata, 0, 0);
	}

	cfg80211_put_bss(sdata->local->hw.wiphy, auth_data->bss);
	kfree(auth_data);
}

enum assoc_status {
	ASSOC_SUCCESS,
	ASSOC_REJECTED,
	ASSOC_TIMEOUT,
	ASSOC_ABANDON,
};

static void ieee80211_destroy_assoc_data(struct ieee80211_sub_if_data *sdata,
					 enum assoc_status status)
{
	struct ieee80211_mgd_assoc_data *assoc_data = sdata->u.mgd.assoc_data;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	sdata->u.mgd.assoc_data = NULL;

	if (status != ASSOC_SUCCESS) {
		/*
		 * we are not associated yet, the only timer that could be
		 * running is the timeout for the association response which
		 * which is not relevant anymore.
		 */
		timer_delete_sync(&sdata->u.mgd.timer);
		sta_info_destroy_addr(sdata, assoc_data->ap_addr);

		eth_zero_addr(sdata->deflink.u.mgd.bssid);
		ieee80211_link_info_change_notify(sdata, &sdata->deflink,
						  BSS_CHANGED_BSSID);
		sdata->u.mgd.flags = 0;
		sdata->vif.bss_conf.mu_mimo_owner = false;

		if (status != ASSOC_REJECTED) {
			struct cfg80211_assoc_failure data = {
				.timeout = status == ASSOC_TIMEOUT,
			};
			int i;

			BUILD_BUG_ON(ARRAY_SIZE(data.bss) !=
				     ARRAY_SIZE(assoc_data->link));

			for (i = 0; i < ARRAY_SIZE(data.bss); i++)
				data.bss[i] = assoc_data->link[i].bss;

			if (ieee80211_vif_is_mld(&sdata->vif))
				data.ap_mld_addr = assoc_data->ap_addr;

			cfg80211_assoc_failure(sdata->dev, &data);
		}

		ieee80211_link_release_channel(&sdata->deflink);
		ieee80211_vif_set_links(sdata, 0, 0);
	}

	kfree(assoc_data);
}

static void ieee80211_auth_challenge(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgd_auth_data *auth_data = sdata->u.mgd.auth_data;
	const struct element *challenge;
	u8 *pos;
	u32 tx_flags = 0;
	struct ieee80211_prep_tx_info info = {
		.subtype = IEEE80211_STYPE_AUTH,
		.link_id = auth_data->link_id,
	};

	pos = mgmt->u.auth.variable;
	challenge = cfg80211_find_elem(WLAN_EID_CHALLENGE, pos,
				       len - (pos - (u8 *)mgmt));
	if (!challenge)
		return;
	auth_data->expected_transaction = 4;
	drv_mgd_prepare_tx(sdata->local, sdata, &info);
	if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
		tx_flags = IEEE80211_TX_CTL_REQ_TX_STATUS |
			   IEEE80211_TX_INTFL_MLME_CONN_TX;
	ieee80211_send_auth(sdata, 3, auth_data->algorithm, 0,
			    (void *)challenge,
			    challenge->datalen + sizeof(*challenge),
			    auth_data->ap_addr, auth_data->ap_addr,
			    auth_data->key, auth_data->key_len,
			    auth_data->key_idx, tx_flags);
}

static bool ieee80211_mark_sta_auth(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	const u8 *ap_addr = ifmgd->auth_data->ap_addr;
	struct sta_info *sta;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	sdata_info(sdata, "authenticated\n");
	ifmgd->auth_data->done = true;
	ifmgd->auth_data->timeout = jiffies + IEEE80211_AUTH_WAIT_ASSOC;
	ifmgd->auth_data->timeout_started = true;
	run_again(sdata, ifmgd->auth_data->timeout);

	/* move station state to auth */
	sta = sta_info_get(sdata, ap_addr);
	if (!sta) {
		WARN_ONCE(1, "%s: STA %pM not found", sdata->name, ap_addr);
		return false;
	}
	if (sta_info_move_state(sta, IEEE80211_STA_AUTH)) {
		sdata_info(sdata, "failed moving %pM to auth\n", ap_addr);
		return false;
	}

	return true;
}

static void ieee80211_rx_mgmt_auth(struct ieee80211_sub_if_data *sdata,
				   struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u16 auth_alg, auth_transaction, status_code;
	struct ieee80211_event event = {
		.type = MLME_EVENT,
		.u.mlme.data = AUTH_EVENT,
	};
	struct ieee80211_prep_tx_info info = {
		.subtype = IEEE80211_STYPE_AUTH,
	};

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (len < 24 + 6)
		return;

	if (!ifmgd->auth_data || ifmgd->auth_data->done)
		return;

	if (!ether_addr_equal(ifmgd->auth_data->ap_addr, mgmt->bssid))
		return;

	auth_alg = le16_to_cpu(mgmt->u.auth.auth_alg);
	auth_transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);
	status_code = le16_to_cpu(mgmt->u.auth.status_code);

	info.link_id = ifmgd->auth_data->link_id;

	if (auth_alg != ifmgd->auth_data->algorithm ||
	    (auth_alg != WLAN_AUTH_SAE &&
	     auth_transaction != ifmgd->auth_data->expected_transaction) ||
	    (auth_alg == WLAN_AUTH_SAE &&
	     (auth_transaction < ifmgd->auth_data->expected_transaction ||
	      auth_transaction > 2))) {
		sdata_info(sdata, "%pM unexpected authentication state: alg %d (expected %d) transact %d (expected %d)\n",
			   mgmt->sa, auth_alg, ifmgd->auth_data->algorithm,
			   auth_transaction,
			   ifmgd->auth_data->expected_transaction);
		goto notify_driver;
	}

	if (status_code != WLAN_STATUS_SUCCESS) {
		cfg80211_rx_mlme_mgmt(sdata->dev, (u8 *)mgmt, len);

		if (auth_alg == WLAN_AUTH_SAE &&
		    (status_code == WLAN_STATUS_ANTI_CLOG_REQUIRED ||
		     (auth_transaction == 1 &&
		      (status_code == WLAN_STATUS_SAE_HASH_TO_ELEMENT ||
		       status_code == WLAN_STATUS_SAE_PK)))) {
			/* waiting for userspace now */
			ifmgd->auth_data->waiting = true;
			ifmgd->auth_data->timeout =
				jiffies + IEEE80211_AUTH_WAIT_SAE_RETRY;
			ifmgd->auth_data->timeout_started = true;
			run_again(sdata, ifmgd->auth_data->timeout);
			goto notify_driver;
		}

		sdata_info(sdata, "%pM denied authentication (status %d)\n",
			   mgmt->sa, status_code);
		ieee80211_destroy_auth_data(sdata, false);
		event.u.mlme.status = MLME_DENIED;
		event.u.mlme.reason = status_code;
		drv_event_callback(sdata->local, sdata, &event);
		goto notify_driver;
	}

	switch (ifmgd->auth_data->algorithm) {
	case WLAN_AUTH_OPEN:
	case WLAN_AUTH_LEAP:
	case WLAN_AUTH_FT:
	case WLAN_AUTH_SAE:
	case WLAN_AUTH_FILS_SK:
	case WLAN_AUTH_FILS_SK_PFS:
	case WLAN_AUTH_FILS_PK:
		break;
	case WLAN_AUTH_SHARED_KEY:
		if (ifmgd->auth_data->expected_transaction != 4) {
			ieee80211_auth_challenge(sdata, mgmt, len);
			/* need another frame */
			return;
		}
		break;
	default:
		WARN_ONCE(1, "invalid auth alg %d",
			  ifmgd->auth_data->algorithm);
		goto notify_driver;
	}

	event.u.mlme.status = MLME_SUCCESS;
	info.success = 1;
	drv_event_callback(sdata->local, sdata, &event);
	if (ifmgd->auth_data->algorithm != WLAN_AUTH_SAE ||
	    (auth_transaction == 2 &&
	     ifmgd->auth_data->expected_transaction == 2)) {
		if (!ieee80211_mark_sta_auth(sdata))
			return; /* ignore frame -- wait for timeout */
	} else if (ifmgd->auth_data->algorithm == WLAN_AUTH_SAE &&
		   auth_transaction == 2) {
		sdata_info(sdata, "SAE peer confirmed\n");
		ifmgd->auth_data->peer_confirmed = true;
	}

	cfg80211_rx_mlme_mgmt(sdata->dev, (u8 *)mgmt, len);
notify_driver:
	drv_mgd_complete_tx(sdata->local, sdata, &info);
}

#define case_WLAN(type) \
	case WLAN_REASON_##type: return #type

const char *ieee80211_get_reason_code_string(u16 reason_code)
{
	switch (reason_code) {
	case_WLAN(UNSPECIFIED);
	case_WLAN(PREV_AUTH_NOT_VALID);
	case_WLAN(DEAUTH_LEAVING);
	case_WLAN(DISASSOC_DUE_TO_INACTIVITY);
	case_WLAN(DISASSOC_AP_BUSY);
	case_WLAN(CLASS2_FRAME_FROM_NONAUTH_STA);
	case_WLAN(CLASS3_FRAME_FROM_NONASSOC_STA);
	case_WLAN(DISASSOC_STA_HAS_LEFT);
	case_WLAN(STA_REQ_ASSOC_WITHOUT_AUTH);
	case_WLAN(DISASSOC_BAD_POWER);
	case_WLAN(DISASSOC_BAD_SUPP_CHAN);
	case_WLAN(INVALID_IE);
	case_WLAN(MIC_FAILURE);
	case_WLAN(4WAY_HANDSHAKE_TIMEOUT);
	case_WLAN(GROUP_KEY_HANDSHAKE_TIMEOUT);
	case_WLAN(IE_DIFFERENT);
	case_WLAN(INVALID_GROUP_CIPHER);
	case_WLAN(INVALID_PAIRWISE_CIPHER);
	case_WLAN(INVALID_AKMP);
	case_WLAN(UNSUPP_RSN_VERSION);
	case_WLAN(INVALID_RSN_IE_CAP);
	case_WLAN(IEEE8021X_FAILED);
	case_WLAN(CIPHER_SUITE_REJECTED);
	case_WLAN(DISASSOC_UNSPECIFIED_QOS);
	case_WLAN(DISASSOC_QAP_NO_BANDWIDTH);
	case_WLAN(DISASSOC_LOW_ACK);
	case_WLAN(DISASSOC_QAP_EXCEED_TXOP);
	case_WLAN(QSTA_LEAVE_QBSS);
	case_WLAN(QSTA_NOT_USE);
	case_WLAN(QSTA_REQUIRE_SETUP);
	case_WLAN(QSTA_TIMEOUT);
	case_WLAN(QSTA_CIPHER_NOT_SUPP);
	case_WLAN(MESH_PEER_CANCELED);
	case_WLAN(MESH_MAX_PEERS);
	case_WLAN(MESH_CONFIG);
	case_WLAN(MESH_CLOSE);
	case_WLAN(MESH_MAX_RETRIES);
	case_WLAN(MESH_CONFIRM_TIMEOUT);
	case_WLAN(MESH_INVALID_GTK);
	case_WLAN(MESH_INCONSISTENT_PARAM);
	case_WLAN(MESH_INVALID_SECURITY);
	case_WLAN(MESH_PATH_ERROR);
	case_WLAN(MESH_PATH_NOFORWARD);
	case_WLAN(MESH_PATH_DEST_UNREACHABLE);
	case_WLAN(MAC_EXISTS_IN_MBSS);
	case_WLAN(MESH_CHAN_REGULATORY);
	case_WLAN(MESH_CHAN);
	default: return "<unknown>";
	}
}

static void ieee80211_rx_mgmt_deauth(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u16 reason_code = le16_to_cpu(mgmt->u.deauth.reason_code);

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (len < 24 + 2)
		return;

	if (!ether_addr_equal(mgmt->bssid, mgmt->sa)) {
		ieee80211_tdls_handle_disconnect(sdata, mgmt->sa, reason_code);
		return;
	}

	if (ifmgd->associated &&
	    ether_addr_equal(mgmt->bssid, sdata->vif.cfg.ap_addr)) {
		sdata_info(sdata, "deauthenticated from %pM (Reason: %u=%s)\n",
			   sdata->vif.cfg.ap_addr, reason_code,
			   ieee80211_get_reason_code_string(reason_code));

		ieee80211_set_disassoc(sdata, 0, 0, false, NULL);

		ieee80211_report_disconnect(sdata, (u8 *)mgmt, len, false,
					    reason_code, false);
		return;
	}

	if (ifmgd->assoc_data &&
	    ether_addr_equal(mgmt->bssid, ifmgd->assoc_data->ap_addr)) {
		sdata_info(sdata,
			   "deauthenticated from %pM while associating (Reason: %u=%s)\n",
			   ifmgd->assoc_data->ap_addr, reason_code,
			   ieee80211_get_reason_code_string(reason_code));

		ieee80211_destroy_assoc_data(sdata, ASSOC_ABANDON);

		cfg80211_rx_mlme_mgmt(sdata->dev, (u8 *)mgmt, len);
		return;
	}
}


static void ieee80211_rx_mgmt_disassoc(struct ieee80211_sub_if_data *sdata,
				       struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u16 reason_code;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (len < 24 + 2)
		return;

	if (!ifmgd->associated ||
	    !ether_addr_equal(mgmt->bssid, sdata->vif.cfg.ap_addr))
		return;

	reason_code = le16_to_cpu(mgmt->u.disassoc.reason_code);

	if (!ether_addr_equal(mgmt->bssid, mgmt->sa)) {
		ieee80211_tdls_handle_disconnect(sdata, mgmt->sa, reason_code);
		return;
	}

	sdata_info(sdata, "disassociated from %pM (Reason: %u=%s)\n",
		   sdata->vif.cfg.ap_addr, reason_code,
		   ieee80211_get_reason_code_string(reason_code));

	ieee80211_set_disassoc(sdata, 0, 0, false, NULL);

	ieee80211_report_disconnect(sdata, (u8 *)mgmt, len, false, reason_code,
				    false);
}

static bool ieee80211_twt_req_supported(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_supported_band *sband,
					const struct link_sta_info *link_sta,
					const struct ieee802_11_elems *elems)
{
	const struct ieee80211_sta_he_cap *own_he_cap =
		ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);

	if (elems->ext_capab_len < 10)
		return false;

	if (!(elems->ext_capab[9] & WLAN_EXT_CAPA10_TWT_RESPONDER_SUPPORT))
		return false;

	return link_sta->pub->he_cap.he_cap_elem.mac_cap_info[0] &
		IEEE80211_HE_MAC_CAP0_TWT_RES &&
		own_he_cap &&
		(own_he_cap->he_cap_elem.mac_cap_info[0] &
			IEEE80211_HE_MAC_CAP0_TWT_REQ);
}

static u64 ieee80211_recalc_twt_req(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_supported_band *sband,
				    struct ieee80211_link_data *link,
				    struct link_sta_info *link_sta,
				    struct ieee802_11_elems *elems)
{
	bool twt = ieee80211_twt_req_supported(sdata, sband, link_sta, elems);

	if (link->conf->twt_requester != twt) {
		link->conf->twt_requester = twt;
		return BSS_CHANGED_TWT;
	}
	return 0;
}

static bool ieee80211_twt_bcast_support(struct ieee80211_sub_if_data *sdata,
					struct ieee80211_bss_conf *bss_conf,
					struct ieee80211_supported_band *sband,
					struct link_sta_info *link_sta)
{
	const struct ieee80211_sta_he_cap *own_he_cap =
		ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);

	return bss_conf->he_support &&
		(link_sta->pub->he_cap.he_cap_elem.mac_cap_info[2] &
			IEEE80211_HE_MAC_CAP2_BCAST_TWT) &&
		own_he_cap &&
		(own_he_cap->he_cap_elem.mac_cap_info[2] &
			IEEE80211_HE_MAC_CAP2_BCAST_TWT);
}

static void ieee80211_epcs_changed(struct ieee80211_sub_if_data *sdata,
				   bool enabled)
{
	/* in any case this is called, dialog token should be reset */
	sdata->u.mgd.epcs.dialog_token = 0;

	if (sdata->u.mgd.epcs.enabled == enabled)
		return;

	sdata->u.mgd.epcs.enabled = enabled;
	cfg80211_epcs_changed(sdata->dev, enabled);
}

static void ieee80211_epcs_teardown(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	u8 link_id;

	if (!sdata->u.mgd.epcs.enabled)
		return;

	lockdep_assert_wiphy(local->hw.wiphy);

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct ieee802_11_elems *elems;
		struct ieee80211_link_data *link;
		const struct cfg80211_bss_ies *ies;
		bool ret;

		rcu_read_lock();

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link || !link->conf || !link->conf->bss) {
			rcu_read_unlock();
			continue;
		}

		if (link->u.mgd.disable_wmm_tracking) {
			rcu_read_unlock();
			ieee80211_set_wmm_default(link, false, false);
			continue;
		}

		ies = rcu_dereference(link->conf->bss->beacon_ies);
		if (!ies) {
			rcu_read_unlock();
			ieee80211_set_wmm_default(link, false, false);
			continue;
		}

		elems = ieee802_11_parse_elems(ies->data, ies->len, false,
					       NULL);
		if (!elems) {
			rcu_read_unlock();
			ieee80211_set_wmm_default(link, false, false);
			continue;
		}

		ret = _ieee80211_sta_wmm_params(local, link,
						elems->wmm_param,
						elems->wmm_param_len,
						elems->mu_edca_param_set);

		kfree(elems);
		rcu_read_unlock();

		if (!ret) {
			ieee80211_set_wmm_default(link, false, false);
			continue;
		}

		ieee80211_mgd_set_link_qos_params(link);
		ieee80211_link_info_change_notify(sdata, link, BSS_CHANGED_QOS);
	}
}

static bool ieee80211_assoc_config_link(struct ieee80211_link_data *link,
					struct link_sta_info *link_sta,
					struct cfg80211_bss *cbss,
					struct ieee80211_mgmt *mgmt,
					const u8 *elem_start,
					unsigned int elem_len,
					u64 *changed)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_mgd_assoc_data *assoc_data =
		sdata->u.mgd.assoc_data ?: sdata->u.mgd.reconf.add_links_data;
	struct ieee80211_bss_conf *bss_conf = link->conf;
	struct ieee80211_local *local = sdata->local;
	unsigned int link_id = link->link_id;
	struct ieee80211_elems_parse_params parse_params = {
		.mode = link->u.mgd.conn.mode,
		.start = elem_start,
		.len = elem_len,
		.link_id = link_id == assoc_data->assoc_link_id ? -1 : link_id,
		.from_ap = true,
	};
	bool is_5ghz = cbss->channel->band == NL80211_BAND_5GHZ;
	bool is_6ghz = cbss->channel->band == NL80211_BAND_6GHZ;
	bool is_s1g = cbss->channel->band == NL80211_BAND_S1GHZ;
	const struct cfg80211_bss_ies *bss_ies = NULL;
	struct ieee80211_supported_band *sband;
	struct ieee802_11_elems *elems;
	const __le16 prof_bss_param_ch_present =
		cpu_to_le16(IEEE80211_MLE_STA_CONTROL_BSS_PARAM_CHANGE_CNT_PRESENT);
	u16 capab_info;
	bool ret;

	elems = ieee802_11_parse_elems_full(&parse_params);
	if (!elems)
		return false;

	if (link_id == assoc_data->assoc_link_id) {
		capab_info = le16_to_cpu(mgmt->u.assoc_resp.capab_info);

		/*
		 * we should not get to this flow unless the association was
		 * successful, so set the status directly to success
		 */
		assoc_data->link[link_id].status = WLAN_STATUS_SUCCESS;
		if (elems->ml_basic) {
			int bss_param_ch_cnt =
				ieee80211_mle_get_bss_param_ch_cnt((const void *)elems->ml_basic);

			if (bss_param_ch_cnt < 0) {
				ret = false;
				goto out;
			}
			bss_conf->bss_param_ch_cnt = bss_param_ch_cnt;
			bss_conf->bss_param_ch_cnt_link_id = link_id;
		}
	} else if (elems->parse_error & IEEE80211_PARSE_ERR_DUP_NEST_ML_BASIC ||
		   !elems->prof ||
		   !(elems->prof->control & prof_bss_param_ch_present)) {
		ret = false;
		goto out;
	} else {
		const u8 *ptr = elems->prof->variable +
				elems->prof->sta_info_len - 1;
		int bss_param_ch_cnt;

		/*
		 * During parsing, we validated that these fields exist,
		 * otherwise elems->prof would have been set to NULL.
		 */
		capab_info = get_unaligned_le16(ptr);
		assoc_data->link[link_id].status = get_unaligned_le16(ptr + 2);
		bss_param_ch_cnt =
			ieee80211_mle_basic_sta_prof_bss_param_ch_cnt(elems->prof);
		bss_conf->bss_param_ch_cnt = bss_param_ch_cnt;
		bss_conf->bss_param_ch_cnt_link_id = link_id;

		if (assoc_data->link[link_id].status != WLAN_STATUS_SUCCESS) {
			link_info(link, "association response status code=%u\n",
				  assoc_data->link[link_id].status);
			ret = true;
			goto out;
		}
	}

	if (!is_s1g && !elems->supp_rates) {
		sdata_info(sdata, "no SuppRates element in AssocResp\n");
		ret = false;
		goto out;
	}

	link->u.mgd.tdls_chan_switch_prohibited =
		elems->ext_capab && elems->ext_capab_len >= 5 &&
		(elems->ext_capab[4] & WLAN_EXT_CAPA5_TDLS_CH_SW_PROHIBITED);

	/*
	 * Some APs are erroneously not including some information in their
	 * (re)association response frames. Try to recover by using the data
	 * from the beacon or probe response. This seems to afflict mobile
	 * 2G/3G/4G wifi routers, reported models include the "Onda PN51T",
	 * "Vodafone PocketWiFi 2", "ZTE MF60" and a similar T-Mobile device.
	 */
	if (!ieee80211_hw_check(&local->hw, STRICT) && !is_6ghz &&
	    ((assoc_data->wmm && !elems->wmm_param) ||
	     (link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT &&
	      (!elems->ht_cap_elem || !elems->ht_operation)) ||
	     (is_5ghz && link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_VHT &&
	      (!elems->vht_cap_elem || !elems->vht_operation)))) {
		const struct cfg80211_bss_ies *ies;
		struct ieee802_11_elems *bss_elems;

		rcu_read_lock();
		ies = rcu_dereference(cbss->ies);
		if (ies)
			bss_ies = kmemdup(ies, sizeof(*ies) + ies->len,
					  GFP_ATOMIC);
		rcu_read_unlock();
		if (!bss_ies) {
			ret = false;
			goto out;
		}

		parse_params.start = bss_ies->data;
		parse_params.len = bss_ies->len;
		parse_params.bss = cbss;
		parse_params.link_id = -1;
		bss_elems = ieee802_11_parse_elems_full(&parse_params);
		if (!bss_elems) {
			ret = false;
			goto out;
		}

		if (assoc_data->wmm &&
		    !elems->wmm_param && bss_elems->wmm_param) {
			elems->wmm_param = bss_elems->wmm_param;
			sdata_info(sdata,
				   "AP bug: WMM param missing from AssocResp\n");
		}

		/*
		 * Also check if we requested HT/VHT, otherwise the AP doesn't
		 * have to include the IEs in the (re)association response.
		 */
		if (!elems->ht_cap_elem && bss_elems->ht_cap_elem &&
		    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT) {
			elems->ht_cap_elem = bss_elems->ht_cap_elem;
			sdata_info(sdata,
				   "AP bug: HT capability missing from AssocResp\n");
		}
		if (!elems->ht_operation && bss_elems->ht_operation &&
		    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT) {
			elems->ht_operation = bss_elems->ht_operation;
			sdata_info(sdata,
				   "AP bug: HT operation missing from AssocResp\n");
		}

		if (is_5ghz) {
			if (!elems->vht_cap_elem && bss_elems->vht_cap_elem &&
			    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_VHT) {
				elems->vht_cap_elem = bss_elems->vht_cap_elem;
				sdata_info(sdata,
					   "AP bug: VHT capa missing from AssocResp\n");
			}

			if (!elems->vht_operation && bss_elems->vht_operation &&
			    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_VHT) {
				elems->vht_operation = bss_elems->vht_operation;
				sdata_info(sdata,
					   "AP bug: VHT operation missing from AssocResp\n");
			}
		}
		kfree(bss_elems);
	}

	/*
	 * We previously checked these in the beacon/probe response, so
	 * they should be present here. This is just a safety net.
	 * Note that the ieee80211_config_bw() below would also check
	 * for this (and more), but this has better error reporting.
	 */
	if (!is_6ghz && link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT &&
	    (!elems->wmm_param || !elems->ht_cap_elem || !elems->ht_operation)) {
		sdata_info(sdata,
			   "HT AP is missing WMM params or HT capability/operation\n");
		ret = false;
		goto out;
	}

	if (is_5ghz && link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_VHT &&
	    (!elems->vht_cap_elem || !elems->vht_operation)) {
		sdata_info(sdata,
			   "VHT AP is missing VHT capability/operation\n");
		ret = false;
		goto out;
	}

	/* check/update if AP changed anything in assoc response vs. scan */
	if (ieee80211_config_bw(link, elems,
				link_id == assoc_data->assoc_link_id,
				changed, "assoc response")) {
		ret = false;
		goto out;
	}

	if (WARN_ON(!link->conf->chanreq.oper.chan)) {
		ret = false;
		goto out;
	}
	sband = local->hw.wiphy->bands[link->conf->chanreq.oper.chan->band];

	/* Set up internal HT/VHT capabilities */
	if (elems->ht_cap_elem && link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HT)
		ieee80211_ht_cap_ie_to_sta_ht_cap(sdata, sband,
						  elems->ht_cap_elem,
						  link_sta);

	if (elems->vht_cap_elem &&
	    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_VHT) {
		const struct ieee80211_vht_cap *bss_vht_cap = NULL;
		const struct cfg80211_bss_ies *ies;

		/*
		 * Cisco AP module 9115 with FW 17.3 has a bug and sends a
		 * too large maximum MPDU length in the association response
		 * (indicating 12k) that it cannot actually process ...
		 * Work around that.
		 */
		rcu_read_lock();
		ies = rcu_dereference(cbss->ies);
		if (ies) {
			const struct element *elem;

			elem = cfg80211_find_elem(WLAN_EID_VHT_CAPABILITY,
						  ies->data, ies->len);
			if (elem && elem->datalen >= sizeof(*bss_vht_cap))
				bss_vht_cap = (const void *)elem->data;
		}

		if (ieee80211_hw_check(&local->hw, STRICT) &&
		    (!bss_vht_cap || memcmp(bss_vht_cap, elems->vht_cap_elem,
					    sizeof(*bss_vht_cap)))) {
			rcu_read_unlock();
			ret = false;
			link_info(link, "VHT capabilities mismatch\n");
			goto out;
		}

		ieee80211_vht_cap_ie_to_sta_vht_cap(sdata, sband,
						    elems->vht_cap_elem,
						    bss_vht_cap, link_sta);
		rcu_read_unlock();
	}

	if (elems->he_operation &&
	    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_HE &&
	    elems->he_cap) {
		ieee80211_he_cap_ie_to_sta_he_cap(sdata, sband,
						  elems->he_cap,
						  elems->he_cap_len,
						  elems->he_6ghz_capa,
						  link_sta);

		bss_conf->he_support = link_sta->pub->he_cap.has_he;
		if (elems->rsnx && elems->rsnx_len &&
		    (elems->rsnx[0] & WLAN_RSNX_CAPA_PROTECTED_TWT) &&
		    wiphy_ext_feature_isset(local->hw.wiphy,
					    NL80211_EXT_FEATURE_PROTECTED_TWT))
			bss_conf->twt_protected = true;
		else
			bss_conf->twt_protected = false;

		*changed |= ieee80211_recalc_twt_req(sdata, sband, link,
						     link_sta, elems);

		if (elems->eht_operation && elems->eht_cap &&
		    link->u.mgd.conn.mode >= IEEE80211_CONN_MODE_EHT) {
			ieee80211_eht_cap_ie_to_sta_eht_cap(sdata, sband,
							    elems->he_cap,
							    elems->he_cap_len,
							    elems->eht_cap,
							    elems->eht_cap_len,
							    link_sta);

			bss_conf->eht_support = link_sta->pub->eht_cap.has_eht;
			bss_conf->epcs_support = bss_conf->eht_support &&
				!!(elems->eht_cap->fixed.mac_cap_info[0] &
				   IEEE80211_EHT_MAC_CAP0_EPCS_PRIO_ACCESS);

			/* EPCS might be already enabled but a new added link
			 * does not support EPCS. This should not really happen
			 * in practice.
			 */
			if (sdata->u.mgd.epcs.enabled &&
			    !bss_conf->epcs_support)
				ieee80211_epcs_teardown(sdata);
		} else {
			bss_conf->eht_support = false;
			bss_conf->epcs_support = false;
		}
	} else {
		bss_conf->he_support = false;
		bss_conf->twt_requester = false;
		bss_conf->twt_protected = false;
		bss_conf->eht_support = false;
		bss_conf->epcs_support = false;
	}

	if (elems->s1g_oper &&
	    link->u.mgd.conn.mode == IEEE80211_CONN_MODE_S1G &&
	    elems->s1g_capab)
		ieee80211_s1g_cap_to_sta_s1g_cap(sdata, elems->s1g_capab,
						 link_sta);

	bss_conf->twt_broadcast =
		ieee80211_twt_bcast_support(sdata, bss_conf, sband, link_sta);

	if (bss_conf->he_support) {
		bss_conf->he_bss_color.color =
			le32_get_bits(elems->he_operation->he_oper_params,
				      IEEE80211_HE_OPERATION_BSS_COLOR_MASK);
		bss_conf->he_bss_color.partial =
			le32_get_bits(elems->he_operation->he_oper_params,
				      IEEE80211_HE_OPERATION_PARTIAL_BSS_COLOR);
		bss_conf->he_bss_color.enabled =
			!le32_get_bits(elems->he_operation->he_oper_params,
				       IEEE80211_HE_OPERATION_BSS_COLOR_DISABLED);

		if (bss_conf->he_bss_color.enabled)
			*changed |= BSS_CHANGED_HE_BSS_COLOR;

		bss_conf->htc_trig_based_pkt_ext =
			le32_get_bits(elems->he_operation->he_oper_params,
				      IEEE80211_HE_OPERATION_DFLT_PE_DURATION_MASK);
		bss_conf->frame_time_rts_th =
			le32_get_bits(elems->he_operation->he_oper_params,
				      IEEE80211_HE_OPERATION_RTS_THRESHOLD_MASK);

		bss_conf->uora_exists = !!elems->uora_element;
		if (elems->uora_element)
			bss_conf->uora_ocw_range = elems->uora_element[0];

		ieee80211_he_op_ie_to_bss_conf(&sdata->vif, elems->he_operation);
		ieee80211_he_spr_ie_to_bss_conf(&sdata->vif, elems->he_spr);
		/* TODO: OPEN: what happens if BSS color disable is set? */
	}

	if (cbss->transmitted_bss) {
		bss_conf->nontransmitted = true;
		ether_addr_copy(bss_conf->transmitter_bssid,
				cbss->transmitted_bss->bssid);
		bss_conf->bssid_indicator = cbss->max_bssid_indicator;
		bss_conf->bssid_index = cbss->bssid_index;
	}

	/*
	 * Some APs, e.g. Netgear WNDR3700, report invalid HT operation data
	 * in their association response, so ignore that data for our own
	 * configuration. If it changed since the last beacon, we'll get the
	 * next beacon and update then.
	 */

	/*
	 * If an operating mode notification IE is present, override the
	 * NSS calculation (that would be done in rate_control_rate_init())
	 * and use the # of streams from that element.
	 */
	if (elems->opmode_notif &&
	    !(*elems->opmode_notif & IEEE80211_OPMODE_NOTIF_RX_NSS_TYPE_BF)) {
		u8 nss;

		nss = *elems->opmode_notif & IEEE80211_OPMODE_NOTIF_RX_NSS_MASK;
		nss >>= IEEE80211_OPMODE_NOTIF_RX_NSS_SHIFT;
		nss += 1;
		link_sta->pub->rx_nss = nss;
	}

	/*
	 * Always handle WMM once after association regardless
	 * of the first value the AP uses. Setting -1 here has
	 * that effect because the AP values is an unsigned
	 * 4-bit value.
	 */
	link->u.mgd.wmm_last_param_set = -1;
	link->u.mgd.mu_edca_last_param_set = -1;

	if (link->u.mgd.disable_wmm_tracking) {
		ieee80211_set_wmm_default(link, false, false);
	} else if (!ieee80211_sta_wmm_params(local, link, elems->wmm_param,
					     elems->wmm_param_len,
					     elems->mu_edca_param_set)) {
		/* still enable QoS since we might have HT/VHT */
		ieee80211_set_wmm_default(link, false, true);
		/* disable WMM tracking in this case to disable
		 * tracking WMM parameter changes in the beacon if
		 * the parameters weren't actually valid. Doing so
		 * avoids changing parameters very strangely when
		 * the AP is going back and forth between valid and
		 * invalid parameters.
		 */
		link->u.mgd.disable_wmm_tracking = true;
	}

	if (elems->max_idle_period_ie) {
		bss_conf->max_idle_period =
			le16_to_cpu(elems->max_idle_period_ie->max_idle_period);
		bss_conf->protected_keep_alive =
			!!(elems->max_idle_period_ie->idle_options &
			   WLAN_IDLE_OPTIONS_PROTECTED_KEEP_ALIVE);
		*changed |= BSS_CHANGED_KEEP_ALIVE;
	} else {
		bss_conf->max_idle_period = 0;
		bss_conf->protected_keep_alive = false;
	}

	/* set assoc capability (AID was already set earlier),
	 * ieee80211_set_associated() will tell the driver */
	bss_conf->assoc_capability = capab_info;

	ret = true;
out:
	kfree(elems);
	kfree(bss_ies);
	return ret;
}

static int ieee80211_mgd_setup_link_sta(struct ieee80211_link_data *link,
					struct sta_info *sta,
					struct link_sta_info *link_sta,
					struct cfg80211_bss *cbss)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss = (void *)cbss->priv;
	u32 rates = 0, basic_rates = 0;
	bool have_higher_than_11mbit = false;
	int min_rate = INT_MAX, min_rate_index = -1;
	struct ieee80211_supported_band *sband;

	memcpy(link_sta->addr, cbss->bssid, ETH_ALEN);
	memcpy(link_sta->pub->addr, cbss->bssid, ETH_ALEN);

	/* TODO: S1G Basic Rate Set is expressed elsewhere */
	if (cbss->channel->band == NL80211_BAND_S1GHZ) {
		ieee80211_s1g_sta_rate_init(sta);
		return 0;
	}

	sband = local->hw.wiphy->bands[cbss->channel->band];

	ieee80211_get_rates(sband, bss->supp_rates, bss->supp_rates_len,
			    NULL, 0,
			    &rates, &basic_rates, NULL,
			    &have_higher_than_11mbit,
			    &min_rate, &min_rate_index);

	/*
	 * This used to be a workaround for basic rates missing
	 * in the association response frame. Now that we no
	 * longer use the basic rates from there, it probably
	 * doesn't happen any more, but keep the workaround so
	 * in case some *other* APs are buggy in different ways
	 * we can connect -- with a warning.
	 * Allow this workaround only in case the AP provided at least
	 * one rate.
	 */
	if (min_rate_index < 0) {
		link_info(link, "No legacy rates in association response\n");
		return -EINVAL;
	} else if (!basic_rates) {
		link_info(link, "No basic rates, using min rate instead\n");
		basic_rates = BIT(min_rate_index);
	}

	if (rates)
		link_sta->pub->supp_rates[cbss->channel->band] = rates;
	else
		link_info(link, "No rates found, keeping mandatory only\n");

	link->conf->basic_rates = basic_rates;

	/* cf. IEEE 802.11 9.2.12 */
	link->operating_11g_mode = sband->band == NL80211_BAND_2GHZ &&
				   have_higher_than_11mbit;

	return 0;
}

static u8 ieee80211_max_rx_chains(struct ieee80211_link_data *link,
				  struct cfg80211_bss *cbss)
{
	struct ieee80211_he_mcs_nss_supp *he_mcs_nss_supp;
	const struct element *ht_cap_elem, *vht_cap_elem;
	const struct cfg80211_bss_ies *ies;
	const struct ieee80211_ht_cap *ht_cap;
	const struct ieee80211_vht_cap *vht_cap;
	const struct ieee80211_he_cap_elem *he_cap;
	const struct element *he_cap_elem;
	u16 mcs_80_map, mcs_160_map;
	int i, mcs_nss_size;
	bool support_160;
	u8 chains = 1;

	if (link->u.mgd.conn.mode < IEEE80211_CONN_MODE_HT)
		return chains;

	ht_cap_elem = ieee80211_bss_get_elem(cbss, WLAN_EID_HT_CAPABILITY);
	if (ht_cap_elem && ht_cap_elem->datalen >= sizeof(*ht_cap)) {
		ht_cap = (void *)ht_cap_elem->data;
		chains = ieee80211_mcs_to_chains(&ht_cap->mcs);
		/*
		 * TODO: use "Tx Maximum Number Spatial Streams Supported" and
		 *	 "Tx Unequal Modulation Supported" fields.
		 */
	}

	if (link->u.mgd.conn.mode < IEEE80211_CONN_MODE_VHT)
		return chains;

	vht_cap_elem = ieee80211_bss_get_elem(cbss, WLAN_EID_VHT_CAPABILITY);
	if (vht_cap_elem && vht_cap_elem->datalen >= sizeof(*vht_cap)) {
		u8 nss;
		u16 tx_mcs_map;

		vht_cap = (void *)vht_cap_elem->data;
		tx_mcs_map = le16_to_cpu(vht_cap->supp_mcs.tx_mcs_map);
		for (nss = 8; nss > 0; nss--) {
			if (((tx_mcs_map >> (2 * (nss - 1))) & 3) !=
					IEEE80211_VHT_MCS_NOT_SUPPORTED)
				break;
		}
		/* TODO: use "Tx Highest Supported Long GI Data Rate" field? */
		chains = max(chains, nss);
	}

	if (link->u.mgd.conn.mode < IEEE80211_CONN_MODE_HE)
		return chains;

	ies = rcu_dereference(cbss->ies);
	he_cap_elem = cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY,
					     ies->data, ies->len);

	if (!he_cap_elem || he_cap_elem->datalen < sizeof(*he_cap))
		return chains;

	/* skip one byte ext_tag_id */
	he_cap = (void *)(he_cap_elem->data + 1);
	mcs_nss_size = ieee80211_he_mcs_nss_size(he_cap);

	/* invalid HE IE */
	if (he_cap_elem->datalen < 1 + mcs_nss_size + sizeof(*he_cap))
		return chains;

	/* mcs_nss is right after he_cap info */
	he_mcs_nss_supp = (void *)(he_cap + 1);

	mcs_80_map = le16_to_cpu(he_mcs_nss_supp->tx_mcs_80);

	for (i = 7; i >= 0; i--) {
		u8 mcs_80 = mcs_80_map >> (2 * i) & 3;

		if (mcs_80 != IEEE80211_VHT_MCS_NOT_SUPPORTED) {
			chains = max_t(u8, chains, i + 1);
			break;
		}
	}

	support_160 = he_cap->phy_cap_info[0] &
		      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;

	if (!support_160)
		return chains;

	mcs_160_map = le16_to_cpu(he_mcs_nss_supp->tx_mcs_160);
	for (i = 7; i >= 0; i--) {
		u8 mcs_160 = mcs_160_map >> (2 * i) & 3;

		if (mcs_160 != IEEE80211_VHT_MCS_NOT_SUPPORTED) {
			chains = max_t(u8, chains, i + 1);
			break;
		}
	}

	return chains;
}

static void
ieee80211_determine_our_sta_mode(struct ieee80211_sub_if_data *sdata,
				 struct ieee80211_supported_band *sband,
				 struct cfg80211_assoc_request *req,
				 bool wmm_used, int link_id,
				 struct ieee80211_conn_settings *conn)
{
	struct ieee80211_sta_ht_cap sta_ht_cap = sband->ht_cap;
	bool is_5ghz = sband->band == NL80211_BAND_5GHZ;
	bool is_6ghz = sband->band == NL80211_BAND_6GHZ;
	const struct ieee80211_sta_he_cap *he_cap;
	const struct ieee80211_sta_eht_cap *eht_cap;
	struct ieee80211_sta_vht_cap vht_cap;

	if (sband->band == NL80211_BAND_S1GHZ) {
		conn->mode = IEEE80211_CONN_MODE_S1G;
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;
		mlme_dbg(sdata, "operating as S1G STA\n");
		return;
	}

	conn->mode = IEEE80211_CONN_MODE_LEGACY;
	conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;

	ieee80211_apply_htcap_overrides(sdata, &sta_ht_cap);

	if (req && req->flags & ASSOC_REQ_DISABLE_HT) {
		mlme_link_id_dbg(sdata, link_id,
				 "HT disabled by flag, limiting to legacy\n");
		goto out;
	}

	if (!wmm_used) {
		mlme_link_id_dbg(sdata, link_id,
				 "WMM/QoS not supported, limiting to legacy\n");
		goto out;
	}

	if (req) {
		unsigned int i;

		for (i = 0; i < req->crypto.n_ciphers_pairwise; i++) {
			if (req->crypto.ciphers_pairwise[i] == WLAN_CIPHER_SUITE_WEP40 ||
			    req->crypto.ciphers_pairwise[i] == WLAN_CIPHER_SUITE_TKIP ||
			    req->crypto.ciphers_pairwise[i] == WLAN_CIPHER_SUITE_WEP104) {
				netdev_info(sdata->dev,
					    "WEP/TKIP use, limiting to legacy\n");
				goto out;
			}
		}
	}

	if (!sta_ht_cap.ht_supported && !is_6ghz) {
		mlme_link_id_dbg(sdata, link_id,
				 "HT not supported (and not on 6 GHz), limiting to legacy\n");
		goto out;
	}

	/* HT is fine */
	conn->mode = IEEE80211_CONN_MODE_HT;
	conn->bw_limit = sta_ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40 ?
		IEEE80211_CONN_BW_LIMIT_40 :
		IEEE80211_CONN_BW_LIMIT_20;

	memcpy(&vht_cap, &sband->vht_cap, sizeof(vht_cap));
	ieee80211_apply_vhtcap_overrides(sdata, &vht_cap);

	if (req && req->flags & ASSOC_REQ_DISABLE_VHT) {
		mlme_link_id_dbg(sdata, link_id,
				 "VHT disabled by flag, limiting to HT\n");
		goto out;
	}

	if (vht_cap.vht_supported && is_5ghz) {
		bool have_80mhz = false;
		unsigned int i;

		if (conn->bw_limit == IEEE80211_CONN_BW_LIMIT_20) {
			mlme_link_id_dbg(sdata, link_id,
					 "no 40 MHz support on 5 GHz, limiting to HT\n");
			goto out;
		}

		/* Allow VHT if at least one channel on the sband supports 80 MHz */
		for (i = 0; i < sband->n_channels; i++) {
			if (sband->channels[i].flags & (IEEE80211_CHAN_DISABLED |
							IEEE80211_CHAN_NO_80MHZ))
				continue;

			have_80mhz = true;
			break;
		}

		if (!have_80mhz) {
			mlme_link_id_dbg(sdata, link_id,
					 "no 80 MHz channel support on 5 GHz, limiting to HT\n");
			goto out;
		}
	} else if (is_5ghz) { /* !vht_supported but on 5 GHz */
		mlme_link_id_dbg(sdata, link_id,
				 "no VHT support on 5 GHz, limiting to HT\n");
		goto out;
	}

	/* VHT - if we have - is fine, including 80 MHz, check 160 below again */
	if (sband->band != NL80211_BAND_2GHZ) {
		conn->mode = IEEE80211_CONN_MODE_VHT;
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_160;
	}

	if (is_5ghz &&
	    !(vht_cap.cap & (IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ |
			     IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160_80PLUS80MHZ))) {
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_80;
		mlme_link_id_dbg(sdata, link_id,
				 "no VHT 160 MHz capability on 5 GHz, limiting to 80 MHz");
	}

	if (req && req->flags & ASSOC_REQ_DISABLE_HE) {
		mlme_link_id_dbg(sdata, link_id,
				 "HE disabled by flag, limiting to HT/VHT\n");
		goto out;
	}

	he_cap = ieee80211_get_he_iftype_cap_vif(sband, &sdata->vif);
	if (!he_cap) {
		WARN_ON(is_6ghz);
		mlme_link_id_dbg(sdata, link_id,
				 "no HE support, limiting to HT/VHT\n");
		goto out;
	}

	/* so we have HE */
	conn->mode = IEEE80211_CONN_MODE_HE;

	/* check bandwidth */
	switch (sband->band) {
	default:
	case NL80211_BAND_2GHZ:
		if (he_cap->he_cap_elem.phy_cap_info[0] &
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G)
			break;
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;
		mlme_link_id_dbg(sdata, link_id,
				 "no 40 MHz HE cap in 2.4 GHz, limiting to 20 MHz\n");
		break;
	case NL80211_BAND_5GHZ:
		if (!(he_cap->he_cap_elem.phy_cap_info[0] &
		      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G)) {
			conn->bw_limit = IEEE80211_CONN_BW_LIMIT_20;
			mlme_link_id_dbg(sdata, link_id,
					 "no 40/80 MHz HE cap in 5 GHz, limiting to 20 MHz\n");
			break;
		}
		if (!(he_cap->he_cap_elem.phy_cap_info[0] &
		      IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G)) {
			conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
					       conn->bw_limit,
					       IEEE80211_CONN_BW_LIMIT_80);
			mlme_link_id_dbg(sdata, link_id,
					 "no 160 MHz HE cap in 5 GHz, limiting to 80 MHz\n");
		}
		break;
	case NL80211_BAND_6GHZ:
		if (he_cap->he_cap_elem.phy_cap_info[0] &
		    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G)
			break;
		conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
				       conn->bw_limit,
				       IEEE80211_CONN_BW_LIMIT_80);
		mlme_link_id_dbg(sdata, link_id,
				 "no 160 MHz HE cap in 6 GHz, limiting to 80 MHz\n");
		break;
	}

	if (req && req->flags & ASSOC_REQ_DISABLE_EHT) {
		mlme_link_id_dbg(sdata, link_id,
				 "EHT disabled by flag, limiting to HE\n");
		goto out;
	}

	eht_cap = ieee80211_get_eht_iftype_cap_vif(sband, &sdata->vif);
	if (!eht_cap) {
		mlme_link_id_dbg(sdata, link_id,
				 "no EHT support, limiting to HE\n");
		goto out;
	}

	/* we have EHT */

	conn->mode = IEEE80211_CONN_MODE_EHT;

	/* check bandwidth */
	if (is_6ghz &&
	    eht_cap->eht_cap_elem.phy_cap_info[0] & IEEE80211_EHT_PHY_CAP0_320MHZ_IN_6GHZ)
		conn->bw_limit = IEEE80211_CONN_BW_LIMIT_320;
	else if (is_6ghz)
		mlme_link_id_dbg(sdata, link_id,
				 "no EHT 320 MHz cap in 6 GHz, limiting to 160 MHz\n");

out:
	mlme_link_id_dbg(sdata, link_id,
			 "determined local STA to be %s, BW limited to %d MHz\n",
			 ieee80211_conn_mode_str(conn->mode),
			 20 * (1 << conn->bw_limit));
}

static void
ieee80211_determine_our_sta_mode_auth(struct ieee80211_sub_if_data *sdata,
				      struct ieee80211_supported_band *sband,
				      struct cfg80211_auth_request *req,
				      bool wmm_used,
				      struct ieee80211_conn_settings *conn)
{
	ieee80211_determine_our_sta_mode(sdata, sband, NULL, wmm_used,
					 req->link_id > 0 ? req->link_id : 0,
					 conn);
}

static void
ieee80211_determine_our_sta_mode_assoc(struct ieee80211_sub_if_data *sdata,
				       struct ieee80211_supported_band *sband,
				       struct cfg80211_assoc_request *req,
				       bool wmm_used, int link_id,
				       struct ieee80211_conn_settings *conn)
{
	struct ieee80211_conn_settings tmp;

	WARN_ON(!req);

	ieee80211_determine_our_sta_mode(sdata, sband, req, wmm_used, link_id,
					 &tmp);

	conn->mode = min_t(enum ieee80211_conn_mode,
			   conn->mode, tmp.mode);
	conn->bw_limit = min_t(enum ieee80211_conn_bw_limit,
			       conn->bw_limit, tmp.bw_limit);
}

static enum ieee80211_ap_reg_power
ieee80211_ap_power_type(u8 control)
{
	switch (u8_get_bits(control, IEEE80211_HE_6GHZ_OPER_CTRL_REG_INFO)) {
	case IEEE80211_6GHZ_CTRL_REG_LPI_AP:
	case IEEE80211_6GHZ_CTRL_REG_INDOOR_LPI_AP:
		return IEEE80211_REG_LPI_AP;
	case IEEE80211_6GHZ_CTRL_REG_SP_AP:
	case IEEE80211_6GHZ_CTRL_REG_INDOOR_SP_AP:
		return IEEE80211_REG_SP_AP;
	case IEEE80211_6GHZ_CTRL_REG_VLP_AP:
		return IEEE80211_REG_VLP_AP;
	default:
		return IEEE80211_REG_UNSET_AP;
	}
}

static int ieee80211_prep_channel(struct ieee80211_sub_if_data *sdata,
				  struct ieee80211_link_data *link,
				  int link_id,
				  struct cfg80211_bss *cbss, bool mlo,
				  struct ieee80211_conn_settings *conn,
				  unsigned long *userspace_selectors)
{
	struct ieee80211_local *local = sdata->local;
	bool is_6ghz = cbss->channel->band == NL80211_BAND_6GHZ;
	struct ieee80211_chan_req chanreq = {};
	struct cfg80211_chan_def ap_chandef;
	struct ieee802_11_elems *elems;
	int ret;

	lockdep_assert_wiphy(local->hw.wiphy);

	rcu_read_lock();
	elems = ieee80211_determine_chan_mode(sdata, conn, cbss, link_id,
					      &chanreq, &ap_chandef,
					      userspace_selectors);

	if (IS_ERR(elems)) {
		rcu_read_unlock();
		return PTR_ERR(elems);
	}

	if (mlo && !elems->ml_basic) {
		sdata_info(sdata, "Rejecting MLO as it is not supported by AP\n");
		rcu_read_unlock();
		kfree(elems);
		return -EINVAL;
	}

	if (link && is_6ghz && conn->mode >= IEEE80211_CONN_MODE_HE) {
		const struct ieee80211_he_6ghz_oper *he_6ghz_oper;

		if (elems->pwr_constr_elem)
			link->conf->pwr_reduction = *elems->pwr_constr_elem;

		he_6ghz_oper = ieee80211_he_6ghz_oper(elems->he_operation);
		if (he_6ghz_oper)
			link->conf->power_type =
				ieee80211_ap_power_type(he_6ghz_oper->control);
		else
			link_info(link,
				  "HE 6 GHz operation missing (on %d MHz), expect issues\n",
				  cbss->channel->center_freq);

		link->conf->tpe = elems->tpe;
		ieee80211_rearrange_tpe(&link->conf->tpe, &ap_chandef,
					&chanreq.oper);
	}
	rcu_read_unlock();
	/* the element data was RCU protected so no longer valid anyway */
	kfree(elems);
	elems = NULL;

	if (!link)
		return 0;

	rcu_read_lock();
	link->needed_rx_chains = min(ieee80211_max_rx_chains(link, cbss),
				     local->rx_chains);
	rcu_read_unlock();

	/*
	 * If this fails (possibly due to channel context sharing
	 * on incompatible channels, e.g. 80+80 and 160 sharing the
	 * same control channel) try to use a smaller bandwidth.
	 */
	ret = ieee80211_link_use_channel(link, &chanreq,
					 IEEE80211_CHANCTX_SHARED);

	/* don't downgrade for 5 and 10 MHz channels, though. */
	if (chanreq.oper.width == NL80211_CHAN_WIDTH_5 ||
	    chanreq.oper.width == NL80211_CHAN_WIDTH_10)
		return ret;

	while (ret && chanreq.oper.width != NL80211_CHAN_WIDTH_20_NOHT) {
		ieee80211_chanreq_downgrade(&chanreq, conn);

		ret = ieee80211_link_use_channel(link, &chanreq,
						 IEEE80211_CHANCTX_SHARED);
	}

	return ret;
}

static bool ieee80211_get_dtim(const struct cfg80211_bss_ies *ies,
			       u8 *dtim_count, u8 *dtim_period)
{
	const u8 *tim_ie = cfg80211_find_ie(WLAN_EID_TIM, ies->data, ies->len);
	const u8 *idx_ie = cfg80211_find_ie(WLAN_EID_MULTI_BSSID_IDX, ies->data,
					 ies->len);
	const struct ieee80211_tim_ie *tim = NULL;
	const struct ieee80211_bssid_index *idx;
	bool valid = tim_ie && tim_ie[1] >= 2;

	if (valid)
		tim = (void *)(tim_ie + 2);

	if (dtim_count)
		*dtim_count = valid ? tim->dtim_count : 0;

	if (dtim_period)
		*dtim_period = valid ? tim->dtim_period : 0;

	/* Check if value is overridden by non-transmitted profile */
	if (!idx_ie || idx_ie[1] < 3)
		return valid;

	idx = (void *)(idx_ie + 2);

	if (dtim_count)
		*dtim_count = idx->dtim_count;

	if (dtim_period)
		*dtim_period = idx->dtim_period;

	return true;
}

static bool ieee80211_assoc_success(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_mgmt *mgmt,
				    struct ieee802_11_elems *elems,
				    const u8 *elem_start, unsigned int elem_len)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_assoc_data *assoc_data = ifmgd->assoc_data;
	struct ieee80211_local *local = sdata->local;
	unsigned int link_id;
	struct sta_info *sta;
	u64 changed[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	u16 valid_links = 0, dormant_links = 0;
	int err;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);
	/*
	 * station info was already allocated and inserted before
	 * the association and should be available to us
	 */
	sta = sta_info_get(sdata, assoc_data->ap_addr);
	if (WARN_ON(!sta))
		goto out_err;

	sta->sta.spp_amsdu = assoc_data->spp_amsdu;

	if (ieee80211_vif_is_mld(&sdata->vif)) {
		for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
			if (!assoc_data->link[link_id].bss)
				continue;

			valid_links |= BIT(link_id);
			if (assoc_data->link[link_id].disabled)
				dormant_links |= BIT(link_id);

			if (link_id != assoc_data->assoc_link_id) {
				err = ieee80211_sta_allocate_link(sta, link_id);
				if (err)
					goto out_err;
			}
		}

		ieee80211_vif_set_links(sdata, valid_links, dormant_links);
	}

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;
		struct ieee80211_link_data *link;
		struct link_sta_info *link_sta;

		if (!cbss)
			continue;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (WARN_ON(!link))
			goto out_err;

		if (ieee80211_vif_is_mld(&sdata->vif))
			link_info(link,
				  "local address %pM, AP link address %pM%s\n",
				  link->conf->addr,
				  assoc_data->link[link_id].bss->bssid,
				  link_id == assoc_data->assoc_link_id ?
					" (assoc)" : "");

		link_sta = rcu_dereference_protected(sta->link[link_id],
						     lockdep_is_held(&local->hw.wiphy->mtx));
		if (WARN_ON(!link_sta))
			goto out_err;

		if (!link->u.mgd.have_beacon) {
			const struct cfg80211_bss_ies *ies;

			rcu_read_lock();
			ies = rcu_dereference(cbss->beacon_ies);
			if (ies)
				link->u.mgd.have_beacon = true;
			else
				ies = rcu_dereference(cbss->ies);
			ieee80211_get_dtim(ies,
					   &link->conf->sync_dtim_count,
					   &link->u.mgd.dtim_period);
			link->conf->beacon_int = cbss->beacon_interval;
			rcu_read_unlock();
		}

		link->conf->dtim_period = link->u.mgd.dtim_period ?: 1;

		if (link_id != assoc_data->assoc_link_id) {
			link->u.mgd.conn = assoc_data->link[link_id].conn;

			err = ieee80211_prep_channel(sdata, link, link_id, cbss,
						     true, &link->u.mgd.conn,
						     sdata->u.mgd.userspace_selectors);
			if (err) {
				link_info(link, "prep_channel failed\n");
				goto out_err;
			}
		}

		err = ieee80211_mgd_setup_link_sta(link, sta, link_sta,
						   assoc_data->link[link_id].bss);
		if (err)
			goto out_err;

		if (!ieee80211_assoc_config_link(link, link_sta,
						 assoc_data->link[link_id].bss,
						 mgmt, elem_start, elem_len,
						 &changed[link_id]))
			goto out_err;

		if (assoc_data->link[link_id].status != WLAN_STATUS_SUCCESS) {
			valid_links &= ~BIT(link_id);
			ieee80211_sta_remove_link(sta, link_id);
			continue;
		}

		if (link_id != assoc_data->assoc_link_id) {
			err = ieee80211_sta_activate_link(sta, link_id);
			if (err)
				goto out_err;
		}
	}

	/* links might have changed due to rejected ones, set them again */
	ieee80211_vif_set_links(sdata, valid_links, dormant_links);

	rate_control_rate_init_all_links(sta);

	if (ifmgd->flags & IEEE80211_STA_MFP_ENABLED) {
		set_sta_flag(sta, WLAN_STA_MFP);
		sta->sta.mfp = true;
	} else {
		sta->sta.mfp = false;
	}

	ieee80211_sta_set_max_amsdu_subframes(sta, elems->ext_capab,
					      elems->ext_capab_len);

	sta->sta.wme = (elems->wmm_param || elems->s1g_capab) &&
		       local->hw.queues >= IEEE80211_NUM_ACS;

	err = sta_info_move_state(sta, IEEE80211_STA_ASSOC);
	if (!err && !(ifmgd->flags & IEEE80211_STA_CONTROL_PORT))
		err = sta_info_move_state(sta, IEEE80211_STA_AUTHORIZED);
	if (err) {
		sdata_info(sdata,
			   "failed to move station %pM to desired state\n",
			   sta->sta.addr);
		WARN_ON(__sta_info_destroy(sta));
		goto out_err;
	}

	if (sdata->wdev.use_4addr)
		drv_sta_set_4addr(local, sdata, &sta->sta, true);

	ieee80211_set_associated(sdata, assoc_data, changed);

	/*
	 * If we're using 4-addr mode, let the AP know that we're
	 * doing so, so that it can create the STA VLAN on its side
	 */
	if (ifmgd->use_4addr)
		ieee80211_send_4addr_nullfunc(local, sdata);

	/*
	 * Start timer to probe the connection to the AP now.
	 * Also start the timer that will detect beacon loss.
	 */
	ieee80211_sta_reset_beacon_monitor(sdata);
	ieee80211_sta_reset_conn_monitor(sdata);

	return true;
out_err:
	eth_zero_addr(sdata->vif.cfg.ap_addr);
	return false;
}

static void ieee80211_rx_mgmt_assoc_resp(struct ieee80211_sub_if_data *sdata,
					 struct ieee80211_mgmt *mgmt,
					 size_t len)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_assoc_data *assoc_data = ifmgd->assoc_data;
	u16 capab_info, status_code, aid;
	struct ieee80211_elems_parse_params parse_params = {
		.bss = NULL,
		.link_id = -1,
		.from_ap = true,
	};
	struct ieee802_11_elems *elems;
	int ac;
	const u8 *elem_start;
	unsigned int elem_len;
	bool reassoc;
	struct ieee80211_event event = {
		.type = MLME_EVENT,
		.u.mlme.data = ASSOC_EVENT,
	};
	struct ieee80211_prep_tx_info info = {};
	struct cfg80211_rx_assoc_resp_data resp = {
		.uapsd_queues = -1,
	};
	u8 ap_mld_addr[ETH_ALEN] __aligned(2);
	unsigned int link_id;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!assoc_data)
		return;

	info.link_id = assoc_data->assoc_link_id;

	parse_params.mode =
		assoc_data->link[assoc_data->assoc_link_id].conn.mode;

	if (!ether_addr_equal(assoc_data->ap_addr, mgmt->bssid) ||
	    !ether_addr_equal(assoc_data->ap_addr, mgmt->sa))
		return;

	/*
	 * AssocResp and ReassocResp have identical structure, so process both
	 * of them in this function.
	 */

	if (len < 24 + 6)
		return;

	reassoc = ieee80211_is_reassoc_resp(mgmt->frame_control);
	capab_info = le16_to_cpu(mgmt->u.assoc_resp.capab_info);
	status_code = le16_to_cpu(mgmt->u.assoc_resp.status_code);
	if (assoc_data->s1g)
		elem_start = mgmt->u.s1g_assoc_resp.variable;
	else
		elem_start = mgmt->u.assoc_resp.variable;

	/*
	 * Note: this may not be perfect, AP might misbehave - if
	 * anyone needs to rely on perfect complete notification
	 * with the exact right subtype, then we need to track what
	 * we actually transmitted.
	 */
	info.subtype = reassoc ? IEEE80211_STYPE_REASSOC_REQ :
				 IEEE80211_STYPE_ASSOC_REQ;

	if (assoc_data->fils_kek_len &&
	    fils_decrypt_assoc_resp(sdata, (u8 *)mgmt, &len, assoc_data) < 0)
		return;

	elem_len = len - (elem_start - (u8 *)mgmt);
	parse_params.start = elem_start;
	parse_params.len = elem_len;
	elems = ieee802_11_parse_elems_full(&parse_params);
	if (!elems)
		goto notify_driver;

	if (elems->aid_resp)
		aid = le16_to_cpu(elems->aid_resp->aid);
	else if (assoc_data->s1g)
		aid = 0; /* TODO */
	else
		aid = le16_to_cpu(mgmt->u.assoc_resp.aid);

	/*
	 * The 5 MSB of the AID field are reserved
	 * (802.11-2016 9.4.1.8 AID field)
	 */
	aid &= 0x7ff;

	sdata_info(sdata,
		   "RX %sssocResp from %pM (capab=0x%x status=%d aid=%d)\n",
		   reassoc ? "Rea" : "A", assoc_data->ap_addr,
		   capab_info, status_code, (u16)(aid & ~(BIT(15) | BIT(14))));

	ifmgd->broken_ap = false;

	if (status_code == WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY &&
	    elems->timeout_int &&
	    elems->timeout_int->type == WLAN_TIMEOUT_ASSOC_COMEBACK) {
		u32 tu, ms;

		cfg80211_assoc_comeback(sdata->dev, assoc_data->ap_addr,
					le32_to_cpu(elems->timeout_int->value));

		tu = le32_to_cpu(elems->timeout_int->value);
		ms = tu * 1024 / 1000;
		sdata_info(sdata,
			   "%pM rejected association temporarily; comeback duration %u TU (%u ms)\n",
			   assoc_data->ap_addr, tu, ms);
		assoc_data->timeout = jiffies + msecs_to_jiffies(ms);
		assoc_data->timeout_started = true;
		assoc_data->comeback = true;
		if (ms > IEEE80211_ASSOC_TIMEOUT)
			run_again(sdata, assoc_data->timeout);
		goto notify_driver;
	}

	if (status_code != WLAN_STATUS_SUCCESS) {
		sdata_info(sdata, "%pM denied association (code=%d)\n",
			   assoc_data->ap_addr, status_code);
		event.u.mlme.status = MLME_DENIED;
		event.u.mlme.reason = status_code;
		drv_event_callback(sdata->local, sdata, &event);
	} else {
		if (aid == 0 || aid > IEEE80211_MAX_AID) {
			sdata_info(sdata,
				   "invalid AID value %d (out of range), turn off PS\n",
				   aid);
			aid = 0;
			ifmgd->broken_ap = true;
		}

		if (ieee80211_vif_is_mld(&sdata->vif)) {
			struct ieee80211_mle_basic_common_info *common;

			if (!elems->ml_basic) {
				sdata_info(sdata,
					   "MLO association with %pM but no (basic) multi-link element in response!\n",
					   assoc_data->ap_addr);
				goto abandon_assoc;
			}

			common = (void *)elems->ml_basic->variable;

			if (memcmp(assoc_data->ap_addr,
				   common->mld_mac_addr, ETH_ALEN)) {
				sdata_info(sdata,
					   "AP MLD MAC address mismatch: got %pM expected %pM\n",
					   common->mld_mac_addr,
					   assoc_data->ap_addr);
				goto abandon_assoc;
			}

			sdata->vif.cfg.eml_cap =
				ieee80211_mle_get_eml_cap((const void *)elems->ml_basic);
			sdata->vif.cfg.eml_med_sync_delay =
				ieee80211_mle_get_eml_med_sync_delay((const void *)elems->ml_basic);
			sdata->vif.cfg.mld_capa_op =
				ieee80211_mle_get_mld_capa_op((const void *)elems->ml_basic);
		}

		sdata->vif.cfg.aid = aid;

		if (!ieee80211_assoc_success(sdata, mgmt, elems,
					     elem_start, elem_len)) {
			/* oops -- internal error -- send timeout for now */
			ieee80211_destroy_assoc_data(sdata, ASSOC_TIMEOUT);
			goto notify_driver;
		}
		event.u.mlme.status = MLME_SUCCESS;
		drv_event_callback(sdata->local, sdata, &event);
		sdata_info(sdata, "associated\n");

		info.success = 1;
	}

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct ieee80211_link_data *link;

		if (!assoc_data->link[link_id].bss)
			continue;

		resp.links[link_id].bss = assoc_data->link[link_id].bss;
		ether_addr_copy(resp.links[link_id].addr,
				assoc_data->link[link_id].addr);
		resp.links[link_id].status = assoc_data->link[link_id].status;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link)
			continue;

		/* get uapsd queues configuration - same for all links */
		resp.uapsd_queues = 0;
		for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
			if (link->tx_conf[ac].uapsd)
				resp.uapsd_queues |= ieee80211_ac_to_qos_mask[ac];
	}

	if (ieee80211_vif_is_mld(&sdata->vif)) {
		ether_addr_copy(ap_mld_addr, sdata->vif.cfg.ap_addr);
		resp.ap_mld_addr = ap_mld_addr;
	}

	ieee80211_destroy_assoc_data(sdata,
				     status_code == WLAN_STATUS_SUCCESS ?
					ASSOC_SUCCESS :
					ASSOC_REJECTED);

	resp.buf = (u8 *)mgmt;
	resp.len = len;
	resp.req_ies = ifmgd->assoc_req_ies;
	resp.req_ies_len = ifmgd->assoc_req_ies_len;
	cfg80211_rx_assoc_resp(sdata->dev, &resp);
notify_driver:
	drv_mgd_complete_tx(sdata->local, sdata, &info);
	kfree(elems);
	return;
abandon_assoc:
	ieee80211_destroy_assoc_data(sdata, ASSOC_ABANDON);
	goto notify_driver;
}

static void ieee80211_rx_bss_info(struct ieee80211_link_data *link,
				  struct ieee80211_mgmt *mgmt, size_t len,
				  struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_bss *bss;
	struct ieee80211_channel *channel;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	channel = ieee80211_get_channel_khz(local->hw.wiphy,
					ieee80211_rx_status_to_khz(rx_status));
	if (!channel)
		return;

	bss = ieee80211_bss_info_update(local, rx_status, mgmt, len, channel);
	if (bss) {
		link->conf->beacon_rate = bss->beacon_rate;
		ieee80211_rx_bss_put(local, bss);
	}
}


static void ieee80211_rx_mgmt_probe_resp(struct ieee80211_link_data *link,
					 struct sk_buff *skb)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_mgmt *mgmt = (void *)skb->data;
	struct ieee80211_if_managed *ifmgd;
	struct ieee80211_rx_status *rx_status = (void *) skb->cb;
	struct ieee80211_channel *channel;
	size_t baselen, len = skb->len;

	ifmgd = &sdata->u.mgd;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	/*
	 * According to Draft P802.11ax D6.0 clause 26.17.2.3.2:
	 * "If a 6 GHz AP receives a Probe Request frame  and responds with
	 * a Probe Response frame [..], the Address 1 field of the Probe
	 * Response frame shall be set to the broadcast address [..]"
	 * So, on 6GHz band we should also accept broadcast responses.
	 */
	channel = ieee80211_get_channel(sdata->local->hw.wiphy,
					rx_status->freq);
	if (!channel)
		return;

	if (!ether_addr_equal(mgmt->da, sdata->vif.addr) &&
	    (channel->band != NL80211_BAND_6GHZ ||
	     !is_broadcast_ether_addr(mgmt->da)))
		return; /* ignore ProbeResp to foreign address */

	baselen = (u8 *) mgmt->u.probe_resp.variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	ieee80211_rx_bss_info(link, mgmt, len, rx_status);

	if (ifmgd->associated &&
	    ether_addr_equal(mgmt->bssid, link->u.mgd.bssid))
		ieee80211_reset_ap_probe(sdata);
}

/*
 * This is the canonical list of information elements we care about,
 * the filter code also gives us all changes to the Microsoft OUI
 * (00:50:F2) vendor IE which is used for WMM which we need to track,
 * as well as the DTPC IE (part of the Cisco OUI) used for signaling
 * changes to requested client power.
 *
 * We implement beacon filtering in software since that means we can
 * avoid processing the frame here and in cfg80211, and userspace
 * will not be able to tell whether the hardware supports it or not.
 *
 * XXX: This list needs to be dynamic -- userspace needs to be able to
 *	add items it requires. It also needs to be able to tell us to
 *	look out for other vendor IEs.
 */
static const u64 care_about_ies =
	(1ULL << WLAN_EID_COUNTRY) |
	(1ULL << WLAN_EID_ERP_INFO) |
	(1ULL << WLAN_EID_CHANNEL_SWITCH) |
	(1ULL << WLAN_EID_PWR_CONSTRAINT) |
	(1ULL << WLAN_EID_HT_CAPABILITY) |
	(1ULL << WLAN_EID_HT_OPERATION) |
	(1ULL << WLAN_EID_EXT_CHANSWITCH_ANN);

static void ieee80211_handle_beacon_sig(struct ieee80211_link_data *link,
					struct ieee80211_if_managed *ifmgd,
					struct ieee80211_bss_conf *bss_conf,
					struct ieee80211_local *local,
					struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;

	/* Track average RSSI from the Beacon frames of the current AP */

	if (!link->u.mgd.tracking_signal_avg) {
		link->u.mgd.tracking_signal_avg = true;
		ewma_beacon_signal_init(&link->u.mgd.ave_beacon_signal);
		link->u.mgd.last_cqm_event_signal = 0;
		link->u.mgd.count_beacon_signal = 1;
		link->u.mgd.last_ave_beacon_signal = 0;
	} else {
		link->u.mgd.count_beacon_signal++;
	}

	ewma_beacon_signal_add(&link->u.mgd.ave_beacon_signal,
			       -rx_status->signal);

	if (ifmgd->rssi_min_thold != ifmgd->rssi_max_thold &&
	    link->u.mgd.count_beacon_signal >= IEEE80211_SIGNAL_AVE_MIN_COUNT) {
		int sig = -ewma_beacon_signal_read(&link->u.mgd.ave_beacon_signal);
		int last_sig = link->u.mgd.last_ave_beacon_signal;
		struct ieee80211_event event = {
			.type = RSSI_EVENT,
		};

		/*
		 * if signal crosses either of the boundaries, invoke callback
		 * with appropriate parameters
		 */
		if (sig > ifmgd->rssi_max_thold &&
		    (last_sig <= ifmgd->rssi_min_thold || last_sig == 0)) {
			link->u.mgd.last_ave_beacon_signal = sig;
			event.u.rssi.data = RSSI_EVENT_HIGH;
			drv_event_callback(local, sdata, &event);
		} else if (sig < ifmgd->rssi_min_thold &&
			   (last_sig >= ifmgd->rssi_max_thold ||
			   last_sig == 0)) {
			link->u.mgd.last_ave_beacon_signal = sig;
			event.u.rssi.data = RSSI_EVENT_LOW;
			drv_event_callback(local, sdata, &event);
		}
	}

	if (bss_conf->cqm_rssi_thold &&
	    link->u.mgd.count_beacon_signal >= IEEE80211_SIGNAL_AVE_MIN_COUNT &&
	    !(sdata->vif.driver_flags & IEEE80211_VIF_SUPPORTS_CQM_RSSI)) {
		int sig = -ewma_beacon_signal_read(&link->u.mgd.ave_beacon_signal);
		int last_event = link->u.mgd.last_cqm_event_signal;
		int thold = bss_conf->cqm_rssi_thold;
		int hyst = bss_conf->cqm_rssi_hyst;

		if (sig < thold &&
		    (last_event == 0 || sig < last_event - hyst)) {
			link->u.mgd.last_cqm_event_signal = sig;
			ieee80211_cqm_rssi_notify(
				&sdata->vif,
				NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW,
				sig, GFP_KERNEL);
		} else if (sig > thold &&
			   (last_event == 0 || sig > last_event + hyst)) {
			link->u.mgd.last_cqm_event_signal = sig;
			ieee80211_cqm_rssi_notify(
				&sdata->vif,
				NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH,
				sig, GFP_KERNEL);
		}
	}

	if (bss_conf->cqm_rssi_low &&
	    link->u.mgd.count_beacon_signal >= IEEE80211_SIGNAL_AVE_MIN_COUNT) {
		int sig = -ewma_beacon_signal_read(&link->u.mgd.ave_beacon_signal);
		int last_event = link->u.mgd.last_cqm_event_signal;
		int low = bss_conf->cqm_rssi_low;
		int high = bss_conf->cqm_rssi_high;

		if (sig < low &&
		    (last_event == 0 || last_event >= low)) {
			link->u.mgd.last_cqm_event_signal = sig;
			ieee80211_cqm_rssi_notify(
				&sdata->vif,
				NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW,
				sig, GFP_KERNEL);
		} else if (sig > high &&
			   (last_event == 0 || last_event <= high)) {
			link->u.mgd.last_cqm_event_signal = sig;
			ieee80211_cqm_rssi_notify(
				&sdata->vif,
				NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH,
				sig, GFP_KERNEL);
		}
	}
}

static bool ieee80211_rx_our_beacon(const u8 *tx_bssid,
				    struct cfg80211_bss *bss)
{
	if (ether_addr_equal(tx_bssid, bss->bssid))
		return true;
	if (!bss->transmitted_bss)
		return false;
	return ether_addr_equal(tx_bssid, bss->transmitted_bss->bssid);
}

static void ieee80211_ml_reconf_work(struct wiphy *wiphy,
				     struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.ml_reconf_work.work);
	u16 new_valid_links, new_active_links, new_dormant_links;
	int ret;

	if (!sdata->u.mgd.removed_links)
		return;

	sdata_info(sdata,
		   "MLO Reconfiguration: work: valid=0x%x, removed=0x%x\n",
		   sdata->vif.valid_links, sdata->u.mgd.removed_links);

	new_valid_links = sdata->vif.valid_links & ~sdata->u.mgd.removed_links;
	if (new_valid_links == sdata->vif.valid_links)
		return;

	if (!new_valid_links ||
	    !(new_valid_links & ~sdata->vif.dormant_links)) {
		sdata_info(sdata, "No valid links after reconfiguration\n");
		ret = -EINVAL;
		goto out;
	}

	new_active_links = sdata->vif.active_links & ~sdata->u.mgd.removed_links;
	if (new_active_links != sdata->vif.active_links) {
		if (!new_active_links)
			new_active_links =
				BIT(ffs(new_valid_links &
					~sdata->vif.dormant_links) - 1);

		ret = ieee80211_set_active_links(&sdata->vif, new_active_links);
		if (ret) {
			sdata_info(sdata,
				   "Failed setting active links\n");
			goto out;
		}
	}

	new_dormant_links = sdata->vif.dormant_links & ~sdata->u.mgd.removed_links;

	ret = ieee80211_vif_set_links(sdata, new_valid_links,
				      new_dormant_links);
	if (ret)
		sdata_info(sdata, "Failed setting valid links\n");

	ieee80211_vif_cfg_change_notify(sdata, BSS_CHANGED_MLD_VALID_LINKS);

out:
	if (!ret)
		cfg80211_links_removed(sdata->dev, sdata->u.mgd.removed_links);
	else
		__ieee80211_disconnect(sdata);

	sdata->u.mgd.removed_links = 0;
}

static void ieee80211_ml_reconfiguration(struct ieee80211_sub_if_data *sdata,
					 struct ieee802_11_elems *elems)
{
	const struct element *sub;
	unsigned long removed_links = 0;
	u16 link_removal_timeout[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	u8 link_id;
	u32 delay;

	if (!ieee80211_vif_is_mld(&sdata->vif) || !elems->ml_reconf)
		return;

	/* Directly parse the sub elements as the common information doesn't
	 * hold any useful information.
	 */
	for_each_mle_subelement(sub, (const u8 *)elems->ml_reconf,
				elems->ml_reconf_len) {
		struct ieee80211_mle_per_sta_profile *prof = (void *)sub->data;
		u8 *pos = prof->variable;
		u16 control;

		if (sub->id != IEEE80211_MLE_SUBELEM_PER_STA_PROFILE)
			continue;

		if (!ieee80211_mle_reconf_sta_prof_size_ok(sub->data,
							   sub->datalen))
			return;

		control = le16_to_cpu(prof->control);
		link_id = control & IEEE80211_MLE_STA_RECONF_CONTROL_LINK_ID;

		removed_links |= BIT(link_id);

		/* the MAC address should not be included, but handle it */
		if (control &
		    IEEE80211_MLE_STA_RECONF_CONTROL_STA_MAC_ADDR_PRESENT)
			pos += 6;

		/* According to Draft P802.11be_D3.0, the control should
		 * include the AP Removal Timer present. If the AP Removal Timer
		 * is not present assume immediate removal.
		 */
		if (control &
		    IEEE80211_MLE_STA_RECONF_CONTROL_AP_REM_TIMER_PRESENT)
			link_removal_timeout[link_id] = get_unaligned_le16(pos);
	}

	removed_links &= sdata->vif.valid_links;
	if (!removed_links) {
		/* In case the removal was cancelled, abort it */
		if (sdata->u.mgd.removed_links) {
			sdata->u.mgd.removed_links = 0;
			wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
						  &sdata->u.mgd.ml_reconf_work);
		}
		return;
	}

	delay = 0;
	for_each_set_bit(link_id, &removed_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_bss_conf *link_conf =
			sdata_dereference(sdata->vif.link_conf[link_id], sdata);
		u32 link_delay;

		if (!link_conf) {
			removed_links &= ~BIT(link_id);
			continue;
		}

		if (link_removal_timeout[link_id] < 1)
			link_delay = 0;
		else
			link_delay = link_conf->beacon_int *
				(link_removal_timeout[link_id] - 1);

		if (!delay)
			delay = link_delay;
		else
			delay = min(delay, link_delay);
	}

	sdata->u.mgd.removed_links = removed_links;
	wiphy_delayed_work_queue(sdata->local->hw.wiphy,
				 &sdata->u.mgd.ml_reconf_work,
				 TU_TO_JIFFIES(delay));
}

static int ieee80211_ttlm_set_links(struct ieee80211_sub_if_data *sdata,
				    u16 active_links, u16 dormant_links,
				    u16 suspended_links)
{
	u64 changed = 0;
	int ret;

	if (!active_links) {
		ret = -EINVAL;
		goto out;
	}

	/* If there is an active negotiated TTLM, it should be discarded by
	 * the new negotiated/advertised TTLM.
	 */
	if (sdata->vif.neg_ttlm.valid) {
		memset(&sdata->vif.neg_ttlm, 0, sizeof(sdata->vif.neg_ttlm));
		sdata->vif.suspended_links = 0;
		changed = BSS_CHANGED_MLD_TTLM;
	}

	if (sdata->vif.active_links != active_links) {
		/* usable links are affected when active_links are changed,
		 * so notify the driver about the status change
		 */
		changed |= BSS_CHANGED_MLD_VALID_LINKS;
		active_links &= sdata->vif.active_links;
		if (!active_links)
			active_links =
				BIT(__ffs(sdata->vif.valid_links &
				    ~dormant_links));
		ret = ieee80211_set_active_links(&sdata->vif, active_links);
		if (ret) {
			sdata_info(sdata, "Failed to set TTLM active links\n");
			goto out;
		}
	}

	ret = ieee80211_vif_set_links(sdata, sdata->vif.valid_links,
				      dormant_links);
	if (ret) {
		sdata_info(sdata, "Failed to set TTLM dormant links\n");
		goto out;
	}

	sdata->vif.suspended_links = suspended_links;
	if (sdata->vif.suspended_links)
		changed |= BSS_CHANGED_MLD_TTLM;

	ieee80211_vif_cfg_change_notify(sdata, changed);

out:
	if (ret)
		ieee80211_disconnect(&sdata->vif, false);

	return ret;
}

static void ieee80211_tid_to_link_map_work(struct wiphy *wiphy,
					   struct wiphy_work *work)
{
	u16 new_active_links, new_dormant_links;
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.ttlm_work.work);

	new_active_links = sdata->u.mgd.ttlm_info.map &
			   sdata->vif.valid_links;
	new_dormant_links = ~sdata->u.mgd.ttlm_info.map &
			    sdata->vif.valid_links;

	ieee80211_vif_set_links(sdata, sdata->vif.valid_links, 0);
	if (ieee80211_ttlm_set_links(sdata, new_active_links, new_dormant_links,
				     0))
		return;

	sdata->u.mgd.ttlm_info.active = true;
	sdata->u.mgd.ttlm_info.switch_time = 0;
}

static u16 ieee80211_get_ttlm(u8 bm_size, u8 *data)
{
	if (bm_size == 1)
		return *data;
	else
		return get_unaligned_le16(data);
}

static int
ieee80211_parse_adv_t2l(struct ieee80211_sub_if_data *sdata,
			const struct ieee80211_ttlm_elem *ttlm,
			struct ieee80211_adv_ttlm_info *ttlm_info)
{
	/* The element size was already validated in
	 * ieee80211_tid_to_link_map_size_ok()
	 */
	u8 control, link_map_presence, map_size, tid;
	u8 *pos;

	memset(ttlm_info, 0, sizeof(*ttlm_info));
	pos = (void *)ttlm->optional;
	control	= ttlm->control;

	if ((control & IEEE80211_TTLM_CONTROL_DEF_LINK_MAP) ||
	    !(control & IEEE80211_TTLM_CONTROL_SWITCH_TIME_PRESENT))
		return 0;

	if ((control & IEEE80211_TTLM_CONTROL_DIRECTION) !=
	    IEEE80211_TTLM_DIRECTION_BOTH) {
		sdata_info(sdata, "Invalid advertised T2L map direction\n");
		return -EINVAL;
	}

	link_map_presence = *pos;
	pos++;

	ttlm_info->switch_time = get_unaligned_le16(pos);

	/* Since ttlm_info->switch_time == 0 means no switch time, bump it
	 * by 1.
	 */
	if (!ttlm_info->switch_time)
		ttlm_info->switch_time = 1;

	pos += 2;

	if (control & IEEE80211_TTLM_CONTROL_EXPECTED_DUR_PRESENT) {
		ttlm_info->duration = pos[0] | pos[1] << 8 | pos[2] << 16;
		pos += 3;
	}

	if (control & IEEE80211_TTLM_CONTROL_LINK_MAP_SIZE)
		map_size = 1;
	else
		map_size = 2;

	/* According to Draft P802.11be_D3.0 clause 35.3.7.1.7, an AP MLD shall
	 * not advertise a TID-to-link mapping that does not map all TIDs to the
	 * same link set, reject frame if not all links have mapping
	 */
	if (link_map_presence != 0xff) {
		sdata_info(sdata,
			   "Invalid advertised T2L mapping presence indicator\n");
		return -EINVAL;
	}

	ttlm_info->map = ieee80211_get_ttlm(map_size, pos);
	if (!ttlm_info->map) {
		sdata_info(sdata,
			   "Invalid advertised T2L map for TID 0\n");
		return -EINVAL;
	}

	pos += map_size;

	for (tid = 1; tid < 8; tid++) {
		u16 map = ieee80211_get_ttlm(map_size, pos);

		if (map != ttlm_info->map) {
			sdata_info(sdata, "Invalid advertised T2L map for tid %d\n",
				   tid);
			return -EINVAL;
		}

		pos += map_size;
	}
	return 0;
}

static void ieee80211_process_adv_ttlm(struct ieee80211_sub_if_data *sdata,
					  struct ieee802_11_elems *elems,
					  u64 beacon_ts)
{
	u8 i;
	int ret;

	if (!ieee80211_vif_is_mld(&sdata->vif))
		return;

	if (!elems->ttlm_num) {
		if (sdata->u.mgd.ttlm_info.switch_time) {
			/* if a planned TID-to-link mapping was cancelled -
			 * abort it
			 */
			wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
						  &sdata->u.mgd.ttlm_work);
		} else if (sdata->u.mgd.ttlm_info.active) {
			/* if no TID-to-link element, set to default mapping in
			 * which all TIDs are mapped to all setup links
			 */
			ret = ieee80211_vif_set_links(sdata,
						      sdata->vif.valid_links,
						      0);
			if (ret) {
				sdata_info(sdata, "Failed setting valid/dormant links\n");
				return;
			}
			ieee80211_vif_cfg_change_notify(sdata,
							BSS_CHANGED_MLD_VALID_LINKS);
		}
		memset(&sdata->u.mgd.ttlm_info, 0,
		       sizeof(sdata->u.mgd.ttlm_info));
		return;
	}

	for (i = 0; i < elems->ttlm_num; i++) {
		struct ieee80211_adv_ttlm_info ttlm_info;
		u32 res;

		res = ieee80211_parse_adv_t2l(sdata, elems->ttlm[i],
					      &ttlm_info);

		if (res) {
			__ieee80211_disconnect(sdata);
			return;
		}

		if (ttlm_info.switch_time) {
			u16 beacon_ts_tu, st_tu, delay;
			u32 delay_jiffies;
			u64 mask;

			/* The t2l map switch time is indicated with a partial
			 * TSF value (bits 10 to 25), get the partial beacon TS
			 * as well, and calc the delay to the start time.
			 */
			mask = GENMASK_ULL(25, 10);
			beacon_ts_tu = (beacon_ts & mask) >> 10;
			st_tu = ttlm_info.switch_time;
			delay = st_tu - beacon_ts_tu;

			/*
			 * If the switch time is far in the future, then it
			 * could also be the previous switch still being
			 * announced.
			 * We can simply ignore it for now, if it is a future
			 * switch the AP will continue to announce it anyway.
			 */
			if (delay > IEEE80211_ADV_TTLM_ST_UNDERFLOW)
				return;

			delay_jiffies = TU_TO_JIFFIES(delay);

			/* Link switching can take time, so schedule it
			 * 100ms before to be ready on time
			 */
			if (delay_jiffies > IEEE80211_ADV_TTLM_SAFETY_BUFFER_MS)
				delay_jiffies -=
					IEEE80211_ADV_TTLM_SAFETY_BUFFER_MS;
			else
				delay_jiffies = 0;

			sdata->u.mgd.ttlm_info = ttlm_info;
			wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
						  &sdata->u.mgd.ttlm_work);
			wiphy_delayed_work_queue(sdata->local->hw.wiphy,
						 &sdata->u.mgd.ttlm_work,
						 delay_jiffies);
			return;
		}
	}
}

static void
ieee80211_mgd_check_cross_link_csa(struct ieee80211_sub_if_data *sdata,
				   int reporting_link_id,
				   struct ieee802_11_elems *elems)
{
	const struct element *sta_profiles[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	ssize_t sta_profiles_len[IEEE80211_MLD_MAX_NUM_LINKS] = {};
	const struct element *sub;
	const u8 *subelems;
	size_t subelems_len;
	u8 common_size;
	int link_id;

	if (!ieee80211_mle_size_ok((u8 *)elems->ml_basic, elems->ml_basic_len))
		return;

	common_size = ieee80211_mle_common_size((u8 *)elems->ml_basic);
	subelems = (u8 *)elems->ml_basic + common_size;
	subelems_len = elems->ml_basic_len - common_size;

	for_each_element_id(sub, IEEE80211_MLE_SUBELEM_PER_STA_PROFILE,
			    subelems, subelems_len) {
		struct ieee80211_mle_per_sta_profile *prof = (void *)sub->data;
		struct ieee80211_link_data *link;
		ssize_t len;

		if (!ieee80211_mle_basic_sta_prof_size_ok(sub->data,
							  sub->datalen))
			continue;

		link_id = le16_get_bits(prof->control,
					IEEE80211_MLE_STA_CONTROL_LINK_ID);
		/* need a valid link ID, but also not our own, both AP bugs */
		if (link_id == reporting_link_id ||
		    link_id >= IEEE80211_MLD_MAX_NUM_LINKS)
			continue;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link)
			continue;

		len = cfg80211_defragment_element(sub, subelems, subelems_len,
						  NULL, 0,
						  IEEE80211_MLE_SUBELEM_FRAGMENT);
		if (WARN_ON(len < 0))
			continue;

		sta_profiles[link_id] = sub;
		sta_profiles_len[link_id] = len;
	}

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct ieee80211_mle_per_sta_profile *prof;
		struct ieee802_11_elems *prof_elems;
		struct ieee80211_link_data *link;
		ssize_t len;

		if (link_id == reporting_link_id)
			continue;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link)
			continue;

		if (!sta_profiles[link_id]) {
			prof_elems = NULL;
			goto handle;
		}

		/* we can defragment in-place, won't use the buffer again */
		len = cfg80211_defragment_element(sta_profiles[link_id],
						  subelems, subelems_len,
						  (void *)sta_profiles[link_id],
						  sta_profiles_len[link_id],
						  IEEE80211_MLE_SUBELEM_FRAGMENT);
		if (WARN_ON(len != sta_profiles_len[link_id]))
			continue;

		prof = (void *)sta_profiles[link_id];
		prof_elems = ieee802_11_parse_elems(prof->variable +
						    (prof->sta_info_len - 1),
						    len -
						    (prof->sta_info_len - 1),
						    false, NULL);

		/* memory allocation failed - let's hope that's transient */
		if (!prof_elems)
			continue;

handle:
		/*
		 * FIXME: the timings here are obviously incorrect,
		 * but only older Intel drivers seem to care, and
		 * those don't have MLO. If you really need this,
		 * the problem is having to calculate it with the
		 * TSF offset etc. The device_timestamp is still
		 * correct, of course.
		 */
		ieee80211_sta_process_chanswitch(link, 0, 0, elems, prof_elems,
						 IEEE80211_CSA_SOURCE_OTHER_LINK);
		kfree(prof_elems);
	}
}

static bool ieee80211_mgd_ssid_mismatch(struct ieee80211_sub_if_data *sdata,
					const struct ieee802_11_elems *elems)
{
	struct ieee80211_vif_cfg *cfg = &sdata->vif.cfg;
	static u8 zero_ssid[IEEE80211_MAX_SSID_LEN];

	if (!elems->ssid)
		return false;

	/* hidden SSID: zero length */
	if (elems->ssid_len == 0)
		return false;

	if (elems->ssid_len != cfg->ssid_len)
		return true;

	/* hidden SSID: zeroed out */
	if (!memcmp(elems->ssid, zero_ssid, elems->ssid_len))
		return false;

	return memcmp(elems->ssid, cfg->ssid, cfg->ssid_len);
}

static void ieee80211_rx_mgmt_beacon(struct ieee80211_link_data *link,
				     struct ieee80211_hdr *hdr, size_t len,
				     struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_bss_conf *bss_conf = link->conf;
	struct ieee80211_vif_cfg *vif_cfg = &sdata->vif.cfg;
	struct ieee80211_mgmt *mgmt = (void *) hdr;
	size_t baselen;
	struct ieee802_11_elems *elems;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *chanctx_conf;
	struct ieee80211_supported_band *sband;
	struct ieee80211_channel *chan;
	struct link_sta_info *link_sta;
	struct sta_info *sta;
	u64 changed = 0;
	bool erp_valid;
	u8 erp_value = 0;
	u32 ncrc = 0;
	u8 *bssid, *variable = mgmt->u.beacon.variable;
	u8 deauth_buf[IEEE80211_DEAUTH_FRAME_LEN];
	struct ieee80211_elems_parse_params parse_params = {
		.mode = link->u.mgd.conn.mode,
		.link_id = -1,
		.from_ap = true,
	};

	lockdep_assert_wiphy(local->hw.wiphy);

	/* Process beacon from the current BSS */
	bssid = ieee80211_get_bssid(hdr, len, sdata->vif.type);
	if (ieee80211_is_s1g_beacon(mgmt->frame_control)) {
		struct ieee80211_ext *ext = (void *) mgmt;
		variable = ext->u.s1g_beacon.variable +
			   ieee80211_s1g_optional_len(ext->frame_control);
	}

	baselen = (u8 *) variable - (u8 *) mgmt;
	if (baselen > len)
		return;

	parse_params.start = variable;
	parse_params.len = len - baselen;

	rcu_read_lock();
	chanctx_conf = rcu_dereference(bss_conf->chanctx_conf);
	if (!chanctx_conf) {
		rcu_read_unlock();
		return;
	}

	if (ieee80211_rx_status_to_khz(rx_status) !=
	    ieee80211_channel_to_khz(chanctx_conf->def.chan)) {
		rcu_read_unlock();
		return;
	}
	chan = chanctx_conf->def.chan;
	rcu_read_unlock();

	if (ifmgd->assoc_data && ifmgd->assoc_data->need_beacon &&
	    !WARN_ON(ieee80211_vif_is_mld(&sdata->vif)) &&
	    ieee80211_rx_our_beacon(bssid, ifmgd->assoc_data->link[0].bss)) {
		parse_params.bss = ifmgd->assoc_data->link[0].bss;
		elems = ieee802_11_parse_elems_full(&parse_params);
		if (!elems)
			return;

		ieee80211_rx_bss_info(link, mgmt, len, rx_status);

		if (elems->dtim_period)
			link->u.mgd.dtim_period = elems->dtim_period;
		link->u.mgd.have_beacon = true;
		ifmgd->assoc_data->need_beacon = false;
		if (ieee80211_hw_check(&local->hw, TIMING_BEACON_ONLY) &&
		    !ieee80211_is_s1g_beacon(hdr->frame_control)) {
			bss_conf->sync_tsf =
				le64_to_cpu(mgmt->u.beacon.timestamp);
			bss_conf->sync_device_ts =
				rx_status->device_timestamp;
			bss_conf->sync_dtim_count = elems->dtim_count;
		}

		if (elems->mbssid_config_ie)
			bss_conf->profile_periodicity =
				elems->mbssid_config_ie->profile_periodicity;
		else
			bss_conf->profile_periodicity = 0;

		if (elems->ext_capab_len >= 11 &&
		    (elems->ext_capab[10] & WLAN_EXT_CAPA11_EMA_SUPPORT))
			bss_conf->ema_ap = true;
		else
			bss_conf->ema_ap = false;

		/* continue assoc process */
		ifmgd->assoc_data->timeout = jiffies;
		ifmgd->assoc_data->timeout_started = true;
		run_again(sdata, ifmgd->assoc_data->timeout);
		kfree(elems);
		return;
	}

	if (!ifmgd->associated ||
	    !ieee80211_rx_our_beacon(bssid, bss_conf->bss))
		return;
	bssid = link->u.mgd.bssid;

	if (!(rx_status->flag & RX_FLAG_NO_SIGNAL_VAL))
		ieee80211_handle_beacon_sig(link, ifmgd, bss_conf,
					    local, rx_status);

	if (ifmgd->flags & IEEE80211_STA_CONNECTION_POLL) {
		mlme_dbg_ratelimited(sdata,
				     "cancelling AP probe due to a received beacon\n");
		ieee80211_reset_ap_probe(sdata);
	}

	/*
	 * Push the beacon loss detection into the future since
	 * we are processing a beacon from the AP just now.
	 */
	ieee80211_sta_reset_beacon_monitor(sdata);

	/* TODO: CRC urrently not calculated on S1G Beacon Compatibility
	 * element (which carries the beacon interval). Don't forget to add a
	 * bit to care_about_ies[] above if mac80211 is interested in a
	 * changing S1G element.
	 */
	if (!ieee80211_is_s1g_beacon(hdr->frame_control))
		ncrc = crc32_be(0, (void *)&mgmt->u.beacon.beacon_int, 4);
	parse_params.bss = bss_conf->bss;
	parse_params.filter = care_about_ies;
	parse_params.crc = ncrc;
	elems = ieee802_11_parse_elems_full(&parse_params);
	if (!elems)
		return;

	if (rx_status->flag & RX_FLAG_DECRYPTED &&
	    ieee80211_mgd_ssid_mismatch(sdata, elems)) {
		sdata_info(sdata, "SSID mismatch for AP %pM, disconnect\n",
			   sdata->vif.cfg.ap_addr);
		__ieee80211_disconnect(sdata);
		return;
	}

	ncrc = elems->crc;

	if (ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK) &&
	    ieee80211_check_tim(elems->tim, elems->tim_len, vif_cfg->aid)) {
		if (local->hw.conf.dynamic_ps_timeout > 0) {
			if (local->hw.conf.flags & IEEE80211_CONF_PS) {
				local->hw.conf.flags &= ~IEEE80211_CONF_PS;
				ieee80211_hw_config(local, -1,
						    IEEE80211_CONF_CHANGE_PS);
			}
			ieee80211_send_nullfunc(local, sdata, false);
		} else if (!local->pspolling && sdata->u.mgd.powersave) {
			local->pspolling = true;

			/*
			 * Here is assumed that the driver will be
			 * able to send ps-poll frame and receive a
			 * response even though power save mode is
			 * enabled, but some drivers might require
			 * to disable power save here. This needs
			 * to be investigated.
			 */
			ieee80211_send_pspoll(local, sdata);
		}
	}

	if (sdata->vif.p2p ||
	    sdata->vif.driver_flags & IEEE80211_VIF_GET_NOA_UPDATE) {
		struct ieee80211_p2p_noa_attr noa = {};
		int ret;

		ret = cfg80211_get_p2p_attr(variable,
					    len - baselen,
					    IEEE80211_P2P_ATTR_ABSENCE_NOTICE,
					    (u8 *) &noa, sizeof(noa));
		if (ret >= 2) {
			if (link->u.mgd.p2p_noa_index != noa.index) {
				/* valid noa_attr and index changed */
				link->u.mgd.p2p_noa_index = noa.index;
				memcpy(&bss_conf->p2p_noa_attr, &noa, sizeof(noa));
				changed |= BSS_CHANGED_P2P_PS;
				/*
				 * make sure we update all information, the CRC
				 * mechanism doesn't look at P2P attributes.
				 */
				link->u.mgd.beacon_crc_valid = false;
			}
		} else if (link->u.mgd.p2p_noa_index != -1) {
			/* noa_attr not found and we had valid noa_attr before */
			link->u.mgd.p2p_noa_index = -1;
			memset(&bss_conf->p2p_noa_attr, 0, sizeof(bss_conf->p2p_noa_attr));
			changed |= BSS_CHANGED_P2P_PS;
			link->u.mgd.beacon_crc_valid = false;
		}
	}

	/*
	 * Update beacon timing and dtim count on every beacon appearance. This
	 * will allow the driver to use the most updated values. Do it before
	 * comparing this one with last received beacon.
	 * IMPORTANT: These parameters would possibly be out of sync by the time
	 * the driver will use them. The synchronized view is currently
	 * guaranteed only in certain callbacks.
	 */
	if (ieee80211_hw_check(&local->hw, TIMING_BEACON_ONLY) &&
	    !ieee80211_is_s1g_beacon(hdr->frame_control)) {
		bss_conf->sync_tsf =
			le64_to_cpu(mgmt->u.beacon.timestamp);
		bss_conf->sync_device_ts =
			rx_status->device_timestamp;
		bss_conf->sync_dtim_count = elems->dtim_count;
	}

	if ((ncrc == link->u.mgd.beacon_crc && link->u.mgd.beacon_crc_valid) ||
	    ieee80211_is_s1g_short_beacon(mgmt->frame_control))
		goto free;
	link->u.mgd.beacon_crc = ncrc;
	link->u.mgd.beacon_crc_valid = true;

	ieee80211_rx_bss_info(link, mgmt, len, rx_status);

	ieee80211_sta_process_chanswitch(link, rx_status->mactime,
					 rx_status->device_timestamp,
					 elems, elems,
					 IEEE80211_CSA_SOURCE_BEACON);

	/* note that after this elems->ml_basic can no longer be used fully */
	ieee80211_mgd_check_cross_link_csa(sdata, rx_status->link_id, elems);

	ieee80211_mgd_update_bss_param_ch_cnt(sdata, bss_conf, elems);

	if (!sdata->u.mgd.epcs.enabled &&
	    !link->u.mgd.disable_wmm_tracking &&
	    ieee80211_sta_wmm_params(local, link, elems->wmm_param,
				     elems->wmm_param_len,
				     elems->mu_edca_param_set))
		changed |= BSS_CHANGED_QOS;

	/*
	 * If we haven't had a beacon before, tell the driver about the
	 * DTIM period (and beacon timing if desired) now.
	 */
	if (!link->u.mgd.have_beacon) {
		/* a few bogus AP send dtim_period = 0 or no TIM IE */
		bss_conf->dtim_period = elems->dtim_period ?: 1;

		changed |= BSS_CHANGED_BEACON_INFO;
		link->u.mgd.have_beacon = true;

		ieee80211_recalc_ps(local);

		ieee80211_recalc_ps_vif(sdata);
	}

	if (elems->erp_info) {
		erp_valid = true;
		erp_value = elems->erp_info[0];
	} else {
		erp_valid = false;
	}

	if (!ieee80211_is_s1g_beacon(hdr->frame_control))
		changed |= ieee80211_handle_bss_capability(link,
				le16_to_cpu(mgmt->u.beacon.capab_info),
				erp_valid, erp_value);

	sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	if (WARN_ON(!sta)) {
		goto free;
	}
	link_sta = rcu_dereference_protected(sta->link[link->link_id],
					     lockdep_is_held(&local->hw.wiphy->mtx));
	if (WARN_ON(!link_sta)) {
		goto free;
	}

	if (WARN_ON(!bss_conf->chanreq.oper.chan))
		goto free;

	sband = local->hw.wiphy->bands[bss_conf->chanreq.oper.chan->band];

	changed |= ieee80211_recalc_twt_req(sdata, sband, link, link_sta, elems);

	if (ieee80211_config_bw(link, elems, true, &changed, "beacon")) {
		ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH,
				       WLAN_REASON_DEAUTH_LEAVING,
				       true, deauth_buf);
		ieee80211_report_disconnect(sdata, deauth_buf,
					    sizeof(deauth_buf), true,
					    WLAN_REASON_DEAUTH_LEAVING,
					    false);
		goto free;
	}

	if (elems->opmode_notif)
		ieee80211_vht_handle_opmode(sdata, link_sta,
					    *elems->opmode_notif,
					    rx_status->band);

	changed |= ieee80211_handle_pwr_constr(link, chan, mgmt,
					       elems->country_elem,
					       elems->country_elem_len,
					       elems->pwr_constr_elem,
					       elems->cisco_dtpc_elem);

	ieee80211_ml_reconfiguration(sdata, elems);
	ieee80211_process_adv_ttlm(sdata, elems,
				      le64_to_cpu(mgmt->u.beacon.timestamp));

	ieee80211_link_info_change_notify(sdata, link, changed);
free:
	kfree(elems);
}

static void ieee80211_apply_neg_ttlm(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_neg_ttlm neg_ttlm)
{
	u16 new_active_links, new_dormant_links, new_suspended_links, map = 0;
	u8 i;

	for (i = 0; i < IEEE80211_TTLM_NUM_TIDS; i++)
		map |= neg_ttlm.downlink[i] | neg_ttlm.uplink[i];

	/* If there is an active TTLM, unset previously suspended links */
	if (sdata->vif.neg_ttlm.valid)
		sdata->vif.dormant_links &= ~sdata->vif.suspended_links;

	/* exclude links that are already disabled by advertised TTLM */
	new_active_links =
		map & sdata->vif.valid_links & ~sdata->vif.dormant_links;
	new_suspended_links =
		(~map & sdata->vif.valid_links) & ~sdata->vif.dormant_links;
	new_dormant_links = sdata->vif.dormant_links | new_suspended_links;
	if (ieee80211_ttlm_set_links(sdata, new_active_links,
				     new_dormant_links, new_suspended_links))
		return;

	sdata->vif.neg_ttlm = neg_ttlm;
	sdata->vif.neg_ttlm.valid = true;
}

static void ieee80211_neg_ttlm_timeout_work(struct wiphy *wiphy,
					    struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.neg_ttlm_timeout_work.work);

	sdata_info(sdata,
		   "No negotiated TTLM response from AP, disconnecting.\n");

	__ieee80211_disconnect(sdata);
}

static void
ieee80211_neg_ttlm_add_suggested_map(struct sk_buff *skb,
				     struct ieee80211_neg_ttlm *neg_ttlm)
{
	u8 i, direction[IEEE80211_TTLM_MAX_CNT];

	if (memcmp(neg_ttlm->downlink, neg_ttlm->uplink,
		   sizeof(neg_ttlm->downlink))) {
		direction[0] = IEEE80211_TTLM_DIRECTION_DOWN;
		direction[1] = IEEE80211_TTLM_DIRECTION_UP;
	} else {
		direction[0] = IEEE80211_TTLM_DIRECTION_BOTH;
	}

	for (i = 0; i < ARRAY_SIZE(direction); i++) {
		u8 tid, len, map_ind = 0, *len_pos, *map_ind_pos, *pos;
		__le16 map;

		len = sizeof(struct ieee80211_ttlm_elem) + 1 + 1;

		pos = skb_put(skb, len + 2);
		*pos++ = WLAN_EID_EXTENSION;
		len_pos = pos++;
		*pos++ = WLAN_EID_EXT_TID_TO_LINK_MAPPING;
		*pos++ = direction[i];
		map_ind_pos = pos++;
		for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++) {
			map = direction[i] == IEEE80211_TTLM_DIRECTION_UP ?
				cpu_to_le16(neg_ttlm->uplink[tid]) :
				cpu_to_le16(neg_ttlm->downlink[tid]);
			if (!map)
				continue;

			len += 2;
			map_ind |= BIT(tid);
			skb_put_data(skb, &map, sizeof(map));
		}

		*map_ind_pos = map_ind;
		*len_pos = len;

		if (direction[i] == IEEE80211_TTLM_DIRECTION_BOTH)
			break;
	}
}

static void
ieee80211_send_neg_ttlm_req(struct ieee80211_sub_if_data *sdata,
			    struct ieee80211_neg_ttlm *neg_ttlm,
			    u8 dialog_token)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	int hdr_len = offsetofend(struct ieee80211_mgmt, u.action.u.ttlm_req);
	int ttlm_max_len = 2 + 1 + sizeof(struct ieee80211_ttlm_elem) + 1 +
		2 * 2 * IEEE80211_TTLM_NUM_TIDS;

	skb = dev_alloc_skb(local->tx_headroom + hdr_len + ttlm_max_len);
	if (!skb)
		return;

	skb_reserve(skb, local->tx_headroom);
	mgmt = skb_put_zero(skb, hdr_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	mgmt->u.action.u.ttlm_req.action_code =
		WLAN_PROTECTED_EHT_ACTION_TTLM_REQ;
	mgmt->u.action.u.ttlm_req.dialog_token = dialog_token;
	ieee80211_neg_ttlm_add_suggested_map(skb, neg_ttlm);
	ieee80211_tx_skb(sdata, skb);
}

int ieee80211_req_neg_ttlm(struct ieee80211_sub_if_data *sdata,
			   struct cfg80211_ttlm_params *params)
{
	struct ieee80211_neg_ttlm neg_ttlm = {};
	u8 i;

	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    !(sdata->vif.cfg.mld_capa_op &
	      IEEE80211_MLD_CAP_OP_TID_TO_LINK_MAP_NEG_SUPP))
		return -EINVAL;

	for (i = 0; i < IEEE80211_TTLM_NUM_TIDS; i++) {
		if ((params->dlink[i] & ~sdata->vif.valid_links) ||
		    (params->ulink[i] & ~sdata->vif.valid_links))
			return -EINVAL;

		neg_ttlm.downlink[i] = params->dlink[i];
		neg_ttlm.uplink[i] = params->ulink[i];
	}

	if (drv_can_neg_ttlm(sdata->local, sdata, &neg_ttlm) !=
	    NEG_TTLM_RES_ACCEPT)
		return -EINVAL;

	ieee80211_apply_neg_ttlm(sdata, neg_ttlm);
	sdata->u.mgd.dialog_token_alloc++;
	ieee80211_send_neg_ttlm_req(sdata, &sdata->vif.neg_ttlm,
				    sdata->u.mgd.dialog_token_alloc);
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &sdata->u.mgd.neg_ttlm_timeout_work);
	wiphy_delayed_work_queue(sdata->local->hw.wiphy,
				 &sdata->u.mgd.neg_ttlm_timeout_work,
				 IEEE80211_NEG_TTLM_REQ_TIMEOUT);
	return 0;
}

static void
ieee80211_send_neg_ttlm_res(struct ieee80211_sub_if_data *sdata,
			    enum ieee80211_neg_ttlm_res ttlm_res,
			    u8 dialog_token,
			    struct ieee80211_neg_ttlm *neg_ttlm)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	int hdr_len = offsetofend(struct ieee80211_mgmt, u.action.u.ttlm_res);
	int ttlm_max_len = 2 + 1 + sizeof(struct ieee80211_ttlm_elem) + 1 +
		2 * 2 * IEEE80211_TTLM_NUM_TIDS;
	u16 status_code;

	skb = dev_alloc_skb(local->tx_headroom + hdr_len + ttlm_max_len);
	if (!skb)
		return;

	skb_reserve(skb, local->tx_headroom);
	mgmt = skb_put_zero(skb, hdr_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	mgmt->u.action.u.ttlm_res.action_code =
		WLAN_PROTECTED_EHT_ACTION_TTLM_RES;
	mgmt->u.action.u.ttlm_res.dialog_token = dialog_token;
	switch (ttlm_res) {
	default:
		WARN_ON(1);
		fallthrough;
	case NEG_TTLM_RES_REJECT:
		status_code = WLAN_STATUS_DENIED_TID_TO_LINK_MAPPING;
		break;
	case NEG_TTLM_RES_ACCEPT:
		status_code = WLAN_STATUS_SUCCESS;
		break;
	case NEG_TTLM_RES_SUGGEST_PREFERRED:
		status_code = WLAN_STATUS_PREF_TID_TO_LINK_MAPPING_SUGGESTED;
		ieee80211_neg_ttlm_add_suggested_map(skb, neg_ttlm);
		break;
	}

	mgmt->u.action.u.ttlm_res.status_code = cpu_to_le16(status_code);
	ieee80211_tx_skb(sdata, skb);
}

static int
ieee80211_parse_neg_ttlm(struct ieee80211_sub_if_data *sdata,
			 const struct ieee80211_ttlm_elem *ttlm,
			 struct ieee80211_neg_ttlm *neg_ttlm,
			 u8 *direction)
{
	u8 control, link_map_presence, map_size, tid;
	u8 *pos;

	/* The element size was already validated in
	 * ieee80211_tid_to_link_map_size_ok()
	 */
	pos = (void *)ttlm->optional;

	control = ttlm->control;

	/* mapping switch time and expected duration fields are not expected
	 * in case of negotiated TTLM
	 */
	if (control & (IEEE80211_TTLM_CONTROL_SWITCH_TIME_PRESENT |
		       IEEE80211_TTLM_CONTROL_EXPECTED_DUR_PRESENT)) {
		mlme_dbg(sdata,
			 "Invalid TTLM element in negotiated TTLM request\n");
		return -EINVAL;
	}

	if (control & IEEE80211_TTLM_CONTROL_DEF_LINK_MAP) {
		for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++) {
			neg_ttlm->downlink[tid] = sdata->vif.valid_links;
			neg_ttlm->uplink[tid] = sdata->vif.valid_links;
		}
		*direction = IEEE80211_TTLM_DIRECTION_BOTH;
		return 0;
	}

	*direction = u8_get_bits(control, IEEE80211_TTLM_CONTROL_DIRECTION);
	if (*direction != IEEE80211_TTLM_DIRECTION_DOWN &&
	    *direction != IEEE80211_TTLM_DIRECTION_UP &&
	    *direction != IEEE80211_TTLM_DIRECTION_BOTH)
		return -EINVAL;

	link_map_presence = *pos;
	pos++;

	if (control & IEEE80211_TTLM_CONTROL_LINK_MAP_SIZE)
		map_size = 1;
	else
		map_size = 2;

	for (tid = 0; tid < IEEE80211_TTLM_NUM_TIDS; tid++) {
		u16 map;

		if (link_map_presence & BIT(tid)) {
			map = ieee80211_get_ttlm(map_size, pos);
			if (!map) {
				mlme_dbg(sdata,
					 "No active links for TID %d", tid);
				return -EINVAL;
			}
		} else {
			map = 0;
		}

		switch (*direction) {
		case IEEE80211_TTLM_DIRECTION_BOTH:
			neg_ttlm->downlink[tid] = map;
			neg_ttlm->uplink[tid] = map;
			break;
		case IEEE80211_TTLM_DIRECTION_DOWN:
			neg_ttlm->downlink[tid] = map;
			break;
		case IEEE80211_TTLM_DIRECTION_UP:
			neg_ttlm->uplink[tid] = map;
			break;
		default:
			return -EINVAL;
		}
		pos += map_size;
	}
	return 0;
}

void ieee80211_process_neg_ttlm_req(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_mgmt *mgmt, size_t len)
{
	u8 dialog_token, direction[IEEE80211_TTLM_MAX_CNT] = {}, i;
	size_t ies_len;
	enum ieee80211_neg_ttlm_res ttlm_res = NEG_TTLM_RES_ACCEPT;
	struct ieee802_11_elems *elems = NULL;
	struct ieee80211_neg_ttlm neg_ttlm = {};

	BUILD_BUG_ON(ARRAY_SIZE(direction) != ARRAY_SIZE(elems->ttlm));

	if (!ieee80211_vif_is_mld(&sdata->vif))
		return;

	dialog_token = mgmt->u.action.u.ttlm_req.dialog_token;
	ies_len  = len - offsetof(struct ieee80211_mgmt,
				  u.action.u.ttlm_req.variable);
	elems = ieee802_11_parse_elems(mgmt->u.action.u.ttlm_req.variable,
				       ies_len, true, NULL);
	if (!elems) {
		ttlm_res = NEG_TTLM_RES_REJECT;
		goto out;
	}

	for (i = 0; i < elems->ttlm_num; i++) {
		if (ieee80211_parse_neg_ttlm(sdata, elems->ttlm[i],
					     &neg_ttlm, &direction[i]) ||
		    (direction[i] == IEEE80211_TTLM_DIRECTION_BOTH &&
		     elems->ttlm_num != 1)) {
			ttlm_res = NEG_TTLM_RES_REJECT;
			goto out;
		}
	}

	if (!elems->ttlm_num ||
	    (elems->ttlm_num == 2 && direction[0] == direction[1])) {
		ttlm_res = NEG_TTLM_RES_REJECT;
		goto out;
	}

	for (i = 0; i < IEEE80211_TTLM_NUM_TIDS; i++) {
		if ((neg_ttlm.downlink[i] &&
		     (neg_ttlm.downlink[i] & ~sdata->vif.valid_links)) ||
		    (neg_ttlm.uplink[i] &&
		     (neg_ttlm.uplink[i] & ~sdata->vif.valid_links))) {
			ttlm_res = NEG_TTLM_RES_REJECT;
			goto out;
		}
	}

	ttlm_res = drv_can_neg_ttlm(sdata->local, sdata, &neg_ttlm);

	if (ttlm_res != NEG_TTLM_RES_ACCEPT)
		goto out;

	ieee80211_apply_neg_ttlm(sdata, neg_ttlm);
out:
	kfree(elems);
	ieee80211_send_neg_ttlm_res(sdata, ttlm_res, dialog_token, &neg_ttlm);
}

void ieee80211_process_neg_ttlm_res(struct ieee80211_sub_if_data *sdata,
				    struct ieee80211_mgmt *mgmt, size_t len)
{
	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    mgmt->u.action.u.ttlm_req.dialog_token !=
	    sdata->u.mgd.dialog_token_alloc)
		return;

	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &sdata->u.mgd.neg_ttlm_timeout_work);

	/* MLD station sends a TID to link mapping request, mainly to handle
	 * BTM (BSS transition management) request, in which case it needs to
	 * restrict the active links set.
	 * In this case it's not expected that the MLD AP will reject the
	 * negotiated TTLM request.
	 * This can be better implemented in the future, to handle request
	 * rejections.
	 */
	if (le16_to_cpu(mgmt->u.action.u.ttlm_res.status_code) != WLAN_STATUS_SUCCESS)
		__ieee80211_disconnect(sdata);
}

void ieee80211_process_ttlm_teardown(struct ieee80211_sub_if_data *sdata)
{
	u16 new_dormant_links;

	if (!sdata->vif.neg_ttlm.valid)
		return;

	memset(&sdata->vif.neg_ttlm, 0, sizeof(sdata->vif.neg_ttlm));
	new_dormant_links =
		sdata->vif.dormant_links & ~sdata->vif.suspended_links;
	sdata->vif.suspended_links = 0;
	ieee80211_vif_set_links(sdata, sdata->vif.valid_links,
				new_dormant_links);
	ieee80211_vif_cfg_change_notify(sdata, BSS_CHANGED_MLD_TTLM |
					       BSS_CHANGED_MLD_VALID_LINKS);
}

static void ieee80211_teardown_ttlm_work(struct wiphy *wiphy,
					 struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.teardown_ttlm_work);

	ieee80211_process_ttlm_teardown(sdata);
}

void ieee80211_send_teardown_neg_ttlm(struct ieee80211_vif *vif)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	int frame_len = offsetofend(struct ieee80211_mgmt,
				  u.action.u.ttlm_tear_down);
	struct ieee80211_tx_info *info;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + frame_len);
	if (!skb)
		return;

	skb_reserve(skb, local->hw.extra_tx_headroom);
	mgmt = skb_put_zero(skb, frame_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	mgmt->u.action.u.ttlm_tear_down.action_code =
		WLAN_PROTECTED_EHT_ACTION_TTLM_TEARDOWN;

	info = IEEE80211_SKB_CB(skb);
	info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;
	info->status_data = IEEE80211_STATUS_TYPE_NEG_TTLM;
	ieee80211_tx_skb(sdata, skb);
}
EXPORT_SYMBOL(ieee80211_send_teardown_neg_ttlm);

void ieee80211_sta_rx_queued_ext(struct ieee80211_sub_if_data *sdata,
				 struct sk_buff *skb)
{
	struct ieee80211_link_data *link = &sdata->deflink;
	struct ieee80211_rx_status *rx_status;
	struct ieee80211_hdr *hdr;
	u16 fc;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	rx_status = (struct ieee80211_rx_status *) skb->cb;
	hdr = (struct ieee80211_hdr *) skb->data;
	fc = le16_to_cpu(hdr->frame_control);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_S1G_BEACON:
		ieee80211_rx_mgmt_beacon(link, hdr, skb->len, rx_status);
		break;
	}
}

void ieee80211_sta_rx_queued_mgmt(struct ieee80211_sub_if_data *sdata,
				  struct sk_buff *skb)
{
	struct ieee80211_link_data *link = &sdata->deflink;
	struct ieee80211_rx_status *rx_status;
	struct ieee802_11_elems *elems;
	struct ieee80211_mgmt *mgmt;
	u16 fc;
	int ies_len;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	rx_status = (struct ieee80211_rx_status *) skb->cb;
	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = le16_to_cpu(mgmt->frame_control);

	if (rx_status->link_valid) {
		link = sdata_dereference(sdata->link[rx_status->link_id],
					 sdata);
		if (!link)
			return;
	}

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_BEACON:
		ieee80211_rx_mgmt_beacon(link, (void *)mgmt,
					 skb->len, rx_status);
		break;
	case IEEE80211_STYPE_PROBE_RESP:
		ieee80211_rx_mgmt_probe_resp(link, skb);
		break;
	case IEEE80211_STYPE_AUTH:
		ieee80211_rx_mgmt_auth(sdata, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_DEAUTH:
		ieee80211_rx_mgmt_deauth(sdata, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_DISASSOC:
		ieee80211_rx_mgmt_disassoc(sdata, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_ASSOC_RESP:
	case IEEE80211_STYPE_REASSOC_RESP:
		ieee80211_rx_mgmt_assoc_resp(sdata, mgmt, skb->len);
		break;
	case IEEE80211_STYPE_ACTION:
		if (!sdata->u.mgd.associated ||
		    !ether_addr_equal(mgmt->bssid, sdata->vif.cfg.ap_addr))
			break;

		switch (mgmt->u.action.category) {
		case WLAN_CATEGORY_SPECTRUM_MGMT:
			ies_len = skb->len -
				  offsetof(struct ieee80211_mgmt,
					   u.action.u.chan_switch.variable);

			if (ies_len < 0)
				break;

			/* CSA IE cannot be overridden, no need for BSSID */
			elems = ieee802_11_parse_elems(
					mgmt->u.action.u.chan_switch.variable,
					ies_len, true, NULL);

			if (elems && !elems->parse_error) {
				enum ieee80211_csa_source src =
					IEEE80211_CSA_SOURCE_PROT_ACTION;

				ieee80211_sta_process_chanswitch(link,
								 rx_status->mactime,
								 rx_status->device_timestamp,
								 elems, elems,
								 src);
			}
			kfree(elems);
			break;
		case WLAN_CATEGORY_PUBLIC:
		case WLAN_CATEGORY_PROTECTED_DUAL_OF_ACTION:
			ies_len = skb->len -
				  offsetof(struct ieee80211_mgmt,
					   u.action.u.ext_chan_switch.variable);

			if (ies_len < 0)
				break;

			/*
			 * extended CSA IE can't be overridden, no need for
			 * BSSID
			 */
			elems = ieee802_11_parse_elems(
					mgmt->u.action.u.ext_chan_switch.variable,
					ies_len, true, NULL);

			if (elems && !elems->parse_error) {
				enum ieee80211_csa_source src;

				if (mgmt->u.action.category ==
						WLAN_CATEGORY_PROTECTED_DUAL_OF_ACTION)
					src = IEEE80211_CSA_SOURCE_PROT_ACTION;
				else
					src = IEEE80211_CSA_SOURCE_UNPROT_ACTION;

				/* for the handling code pretend it was an IE */
				elems->ext_chansw_ie =
					&mgmt->u.action.u.ext_chan_switch.data;

				ieee80211_sta_process_chanswitch(link,
								 rx_status->mactime,
								 rx_status->device_timestamp,
								 elems, elems,
								 src);
			}

			kfree(elems);
			break;
		}
		break;
	}
}

static void ieee80211_sta_timer(struct timer_list *t)
{
	struct ieee80211_sub_if_data *sdata =
		timer_container_of(sdata, t, u.mgd.timer);

	wiphy_work_queue(sdata->local->hw.wiphy, &sdata->work);
}

void ieee80211_sta_connection_lost(struct ieee80211_sub_if_data *sdata,
				   u8 reason, bool tx)
{
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

	ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH, reason,
			       tx, frame_buf);

	ieee80211_report_disconnect(sdata, frame_buf, sizeof(frame_buf), true,
				    reason, false);
}

static int ieee80211_auth(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_auth_data *auth_data = ifmgd->auth_data;
	u32 tx_flags = 0;
	u16 trans = 1;
	u16 status = 0;
	struct ieee80211_prep_tx_info info = {
		.subtype = IEEE80211_STYPE_AUTH,
	};

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (WARN_ON_ONCE(!auth_data))
		return -EINVAL;

	auth_data->tries++;

	if (auth_data->tries > IEEE80211_AUTH_MAX_TRIES) {
		sdata_info(sdata, "authentication with %pM timed out\n",
			   auth_data->ap_addr);

		/*
		 * Most likely AP is not in the range so remove the
		 * bss struct for that AP.
		 */
		cfg80211_unlink_bss(local->hw.wiphy, auth_data->bss);

		return -ETIMEDOUT;
	}

	if (auth_data->algorithm == WLAN_AUTH_SAE)
		info.duration = jiffies_to_msecs(IEEE80211_AUTH_TIMEOUT_SAE);

	info.link_id = auth_data->link_id;
	drv_mgd_prepare_tx(local, sdata, &info);

	sdata_info(sdata, "send auth to %pM (try %d/%d)\n",
		   auth_data->ap_addr, auth_data->tries,
		   IEEE80211_AUTH_MAX_TRIES);

	auth_data->expected_transaction = 2;

	if (auth_data->algorithm == WLAN_AUTH_SAE) {
		trans = auth_data->sae_trans;
		status = auth_data->sae_status;
		auth_data->expected_transaction = trans;
	}

	if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
		tx_flags = IEEE80211_TX_CTL_REQ_TX_STATUS |
			   IEEE80211_TX_INTFL_MLME_CONN_TX;

	ieee80211_send_auth(sdata, trans, auth_data->algorithm, status,
			    auth_data->data, auth_data->data_len,
			    auth_data->ap_addr, auth_data->ap_addr,
			    NULL, 0, 0, tx_flags);

	if (tx_flags == 0) {
		if (auth_data->algorithm == WLAN_AUTH_SAE)
			auth_data->timeout = jiffies +
				IEEE80211_AUTH_TIMEOUT_SAE;
		else
			auth_data->timeout = jiffies + IEEE80211_AUTH_TIMEOUT;
	} else {
		auth_data->timeout =
			round_jiffies_up(jiffies + IEEE80211_AUTH_TIMEOUT_LONG);
	}

	auth_data->timeout_started = true;
	run_again(sdata, auth_data->timeout);

	return 0;
}

static int ieee80211_do_assoc(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_mgd_assoc_data *assoc_data = sdata->u.mgd.assoc_data;
	struct ieee80211_local *local = sdata->local;
	int ret;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	assoc_data->tries++;
	assoc_data->comeback = false;
	if (assoc_data->tries > IEEE80211_ASSOC_MAX_TRIES) {
		sdata_info(sdata, "association with %pM timed out\n",
			   assoc_data->ap_addr);

		/*
		 * Most likely AP is not in the range so remove the
		 * bss struct for that AP.
		 */
		cfg80211_unlink_bss(local->hw.wiphy,
				    assoc_data->link[assoc_data->assoc_link_id].bss);

		return -ETIMEDOUT;
	}

	sdata_info(sdata, "associate with %pM (try %d/%d)\n",
		   assoc_data->ap_addr, assoc_data->tries,
		   IEEE80211_ASSOC_MAX_TRIES);
	ret = ieee80211_send_assoc(sdata);
	if (ret)
		return ret;

	if (!ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS)) {
		assoc_data->timeout = jiffies + IEEE80211_ASSOC_TIMEOUT;
		assoc_data->timeout_started = true;
		run_again(sdata, assoc_data->timeout);
	} else {
		assoc_data->timeout =
			round_jiffies_up(jiffies +
					 IEEE80211_ASSOC_TIMEOUT_LONG);
		assoc_data->timeout_started = true;
		run_again(sdata, assoc_data->timeout);
	}

	return 0;
}

void ieee80211_mgd_conn_tx_status(struct ieee80211_sub_if_data *sdata,
				  __le16 fc, bool acked)
{
	struct ieee80211_local *local = sdata->local;

	sdata->u.mgd.status_fc = fc;
	sdata->u.mgd.status_acked = acked;
	sdata->u.mgd.status_received = true;

	wiphy_work_queue(local->hw.wiphy, &sdata->work);
}

void ieee80211_sta_work(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (ifmgd->status_received) {
		__le16 fc = ifmgd->status_fc;
		bool status_acked = ifmgd->status_acked;

		ifmgd->status_received = false;
		if (ifmgd->auth_data && ieee80211_is_auth(fc)) {
			if (status_acked) {
				if (ifmgd->auth_data->algorithm ==
				    WLAN_AUTH_SAE)
					ifmgd->auth_data->timeout =
						jiffies +
						IEEE80211_AUTH_TIMEOUT_SAE;
				else
					ifmgd->auth_data->timeout =
						jiffies +
						IEEE80211_AUTH_TIMEOUT_SHORT;
				run_again(sdata, ifmgd->auth_data->timeout);
			} else {
				ifmgd->auth_data->timeout = jiffies - 1;
			}
			ifmgd->auth_data->timeout_started = true;
		} else if (ifmgd->assoc_data &&
			   !ifmgd->assoc_data->comeback &&
			   (ieee80211_is_assoc_req(fc) ||
			    ieee80211_is_reassoc_req(fc))) {
			/*
			 * Update association timeout based on the TX status
			 * for the (Re)Association Request frame. Skip this if
			 * we have already processed a (Re)Association Response
			 * frame that indicated need for association comeback
			 * at a specific time in the future. This could happen
			 * if the TX status information is delayed enough for
			 * the response to be received and processed first.
			 */
			if (status_acked) {
				ifmgd->assoc_data->timeout =
					jiffies + IEEE80211_ASSOC_TIMEOUT_SHORT;
				run_again(sdata, ifmgd->assoc_data->timeout);
			} else {
				ifmgd->assoc_data->timeout = jiffies - 1;
			}
			ifmgd->assoc_data->timeout_started = true;
		}
	}

	if (ifmgd->auth_data && ifmgd->auth_data->timeout_started &&
	    time_after(jiffies, ifmgd->auth_data->timeout)) {
		if (ifmgd->auth_data->done || ifmgd->auth_data->waiting) {
			/*
			 * ok ... we waited for assoc or continuation but
			 * userspace didn't do it, so kill the auth data
			 */
			ieee80211_destroy_auth_data(sdata, false);
		} else if (ieee80211_auth(sdata)) {
			u8 ap_addr[ETH_ALEN];
			struct ieee80211_event event = {
				.type = MLME_EVENT,
				.u.mlme.data = AUTH_EVENT,
				.u.mlme.status = MLME_TIMEOUT,
			};

			memcpy(ap_addr, ifmgd->auth_data->ap_addr, ETH_ALEN);

			ieee80211_destroy_auth_data(sdata, false);

			cfg80211_auth_timeout(sdata->dev, ap_addr);
			drv_event_callback(sdata->local, sdata, &event);
		}
	} else if (ifmgd->auth_data && ifmgd->auth_data->timeout_started)
		run_again(sdata, ifmgd->auth_data->timeout);

	if (ifmgd->assoc_data && ifmgd->assoc_data->timeout_started &&
	    time_after(jiffies, ifmgd->assoc_data->timeout)) {
		if ((ifmgd->assoc_data->need_beacon &&
		     !sdata->deflink.u.mgd.have_beacon) ||
		    ieee80211_do_assoc(sdata)) {
			struct ieee80211_event event = {
				.type = MLME_EVENT,
				.u.mlme.data = ASSOC_EVENT,
				.u.mlme.status = MLME_TIMEOUT,
			};

			ieee80211_destroy_assoc_data(sdata, ASSOC_TIMEOUT);
			drv_event_callback(sdata->local, sdata, &event);
		}
	} else if (ifmgd->assoc_data && ifmgd->assoc_data->timeout_started)
		run_again(sdata, ifmgd->assoc_data->timeout);

	if (ifmgd->flags & IEEE80211_STA_CONNECTION_POLL &&
	    ifmgd->associated) {
		u8 *bssid = sdata->deflink.u.mgd.bssid;
		int max_tries;

		if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS))
			max_tries = max_nullfunc_tries;
		else
			max_tries = max_probe_tries;

		/* ACK received for nullfunc probing frame */
		if (!ifmgd->probe_send_count)
			ieee80211_reset_ap_probe(sdata);
		else if (ifmgd->nullfunc_failed) {
			if (ifmgd->probe_send_count < max_tries) {
				mlme_dbg(sdata,
					 "No ack for nullfunc frame to AP %pM, try %d/%i\n",
					 bssid, ifmgd->probe_send_count,
					 max_tries);
				ieee80211_mgd_probe_ap_send(sdata);
			} else {
				mlme_dbg(sdata,
					 "No ack for nullfunc frame to AP %pM, disconnecting.\n",
					 bssid);
				ieee80211_sta_connection_lost(sdata,
					WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY,
					false);
			}
		} else if (time_is_after_jiffies(ifmgd->probe_timeout))
			run_again(sdata, ifmgd->probe_timeout);
		else if (ieee80211_hw_check(&local->hw, REPORTS_TX_ACK_STATUS)) {
			mlme_dbg(sdata,
				 "Failed to send nullfunc to AP %pM after %dms, disconnecting\n",
				 bssid, probe_wait_ms);
			ieee80211_sta_connection_lost(sdata,
				WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY, false);
		} else if (ifmgd->probe_send_count < max_tries) {
			mlme_dbg(sdata,
				 "No probe response from AP %pM after %dms, try %d/%i\n",
				 bssid, probe_wait_ms,
				 ifmgd->probe_send_count, max_tries);
			ieee80211_mgd_probe_ap_send(sdata);
		} else {
			/*
			 * We actually lost the connection ... or did we?
			 * Let's make sure!
			 */
			mlme_dbg(sdata,
				 "No probe response from AP %pM after %dms, disconnecting.\n",
				 bssid, probe_wait_ms);

			ieee80211_sta_connection_lost(sdata,
				WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY, false);
		}
	}
}

static void ieee80211_sta_bcn_mon_timer(struct timer_list *t)
{
	struct ieee80211_sub_if_data *sdata =
		timer_container_of(sdata, t, u.mgd.bcn_mon_timer);

	if (WARN_ON(ieee80211_vif_is_mld(&sdata->vif)))
		return;

	if (sdata->vif.bss_conf.csa_active &&
	    !sdata->deflink.u.mgd.csa.waiting_bcn)
		return;

	if (sdata->vif.driver_flags & IEEE80211_VIF_BEACON_FILTER)
		return;

	sdata->u.mgd.connection_loss = false;
	wiphy_work_queue(sdata->local->hw.wiphy,
			 &sdata->u.mgd.beacon_connection_loss_work);
}

static void ieee80211_sta_conn_mon_timer(struct timer_list *t)
{
	struct ieee80211_sub_if_data *sdata =
		timer_container_of(sdata, t, u.mgd.conn_mon_timer);
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;
	unsigned long timeout;

	if (WARN_ON(ieee80211_vif_is_mld(&sdata->vif)))
		return;

	if (sdata->vif.bss_conf.csa_active &&
	    !sdata->deflink.u.mgd.csa.waiting_bcn)
		return;

	sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	if (!sta)
		return;

	timeout = sta->deflink.status_stats.last_ack;
	if (time_before(sta->deflink.status_stats.last_ack, sta->deflink.rx_stats.last_rx))
		timeout = sta->deflink.rx_stats.last_rx;
	timeout += IEEE80211_CONNECTION_IDLE_TIME;

	/* If timeout is after now, then update timer to fire at
	 * the later date, but do not actually probe at this time.
	 */
	if (time_is_after_jiffies(timeout)) {
		mod_timer(&ifmgd->conn_mon_timer, round_jiffies_up(timeout));
		return;
	}

	wiphy_work_queue(local->hw.wiphy, &sdata->u.mgd.monitor_work);
}

static void ieee80211_sta_monitor_work(struct wiphy *wiphy,
				       struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.monitor_work);

	ieee80211_mgd_probe_ap(sdata, false);
}

static void ieee80211_restart_sta_timer(struct ieee80211_sub_if_data *sdata)
{
	if (sdata->vif.type == NL80211_IFTYPE_STATION) {
		__ieee80211_stop_poll(sdata);

		/* let's probe the connection once */
		if (!ieee80211_hw_check(&sdata->local->hw, CONNECTION_MONITOR))
			wiphy_work_queue(sdata->local->hw.wiphy,
					 &sdata->u.mgd.monitor_work);
	}
}

#ifdef CONFIG_PM
void ieee80211_mgd_quiesce(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (ifmgd->auth_data || ifmgd->assoc_data) {
		const u8 *ap_addr = ifmgd->auth_data ?
				ifmgd->auth_data->ap_addr :
				ifmgd->assoc_data->ap_addr;

		/*
		 * If we are trying to authenticate / associate while suspending,
		 * cfg80211 won't know and won't actually abort those attempts,
		 * thus we need to do that ourselves.
		 */
		ieee80211_send_deauth_disassoc(sdata, ap_addr, ap_addr,
					       IEEE80211_STYPE_DEAUTH,
					       WLAN_REASON_DEAUTH_LEAVING,
					       false, frame_buf);
		if (ifmgd->assoc_data)
			ieee80211_destroy_assoc_data(sdata, ASSOC_ABANDON);
		if (ifmgd->auth_data)
			ieee80211_destroy_auth_data(sdata, false);
		cfg80211_tx_mlme_mgmt(sdata->dev, frame_buf,
				      IEEE80211_DEAUTH_FRAME_LEN,
				      false);
	}

	/* This is a bit of a hack - we should find a better and more generic
	 * solution to this. Normally when suspending, cfg80211 will in fact
	 * deauthenticate. However, it doesn't (and cannot) stop an ongoing
	 * auth (not so important) or assoc (this is the problem) process.
	 *
	 * As a consequence, it can happen that we are in the process of both
	 * associating and suspending, and receive an association response
	 * after cfg80211 has checked if it needs to disconnect, but before
	 * we actually set the flag to drop incoming frames. This will then
	 * cause the workqueue flush to process the association response in
	 * the suspend, resulting in a successful association just before it
	 * tries to remove the interface from the driver, which now though
	 * has a channel context assigned ... this results in issues.
	 *
	 * To work around this (for now) simply deauth here again if we're
	 * now connected.
	 */
	if (ifmgd->associated && !sdata->local->wowlan) {
		u8 bssid[ETH_ALEN];
		struct cfg80211_deauth_request req = {
			.reason_code = WLAN_REASON_DEAUTH_LEAVING,
			.bssid = bssid,
		};

		memcpy(bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);
		ieee80211_mgd_deauth(sdata, &req);
	}
}
#endif

void ieee80211_sta_restart(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!ifmgd->associated)
		return;

	if (sdata->flags & IEEE80211_SDATA_DISCONNECT_RESUME) {
		sdata->flags &= ~IEEE80211_SDATA_DISCONNECT_RESUME;
		mlme_dbg(sdata, "driver requested disconnect after resume\n");
		ieee80211_sta_connection_lost(sdata,
					      WLAN_REASON_UNSPECIFIED,
					      true);
		return;
	}

	if (sdata->flags & IEEE80211_SDATA_DISCONNECT_HW_RESTART) {
		sdata->flags &= ~IEEE80211_SDATA_DISCONNECT_HW_RESTART;
		mlme_dbg(sdata, "driver requested disconnect after hardware restart\n");
		ieee80211_sta_connection_lost(sdata,
					      WLAN_REASON_UNSPECIFIED,
					      true);
		return;
	}
}

static void ieee80211_request_smps_mgd_work(struct wiphy *wiphy,
					    struct wiphy_work *work)
{
	struct ieee80211_link_data *link =
		container_of(work, struct ieee80211_link_data,
			     u.mgd.request_smps_work);

	__ieee80211_request_smps_mgd(link->sdata, link,
				     link->u.mgd.driver_smps_mode);
}

static void ieee80211_ml_sta_reconf_timeout(struct wiphy *wiphy,
					    struct wiphy_work *work)
{
	struct ieee80211_sub_if_data *sdata =
		container_of(work, struct ieee80211_sub_if_data,
			     u.mgd.reconf.wk.work);

	if (!sdata->u.mgd.reconf.added_links &&
	    !sdata->u.mgd.reconf.removed_links)
		return;

	sdata_info(sdata,
		   "mlo: reconf: timeout: added=0x%x, removed=0x%x\n",
		   sdata->u.mgd.reconf.added_links,
		   sdata->u.mgd.reconf.removed_links);

	__ieee80211_disconnect(sdata);
}

/* interface setup */
void ieee80211_sta_setup_sdata(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	wiphy_work_init(&ifmgd->monitor_work, ieee80211_sta_monitor_work);
	wiphy_work_init(&ifmgd->beacon_connection_loss_work,
			ieee80211_beacon_connection_loss_work);
	wiphy_work_init(&ifmgd->csa_connection_drop_work,
			ieee80211_csa_connection_drop_work);
	wiphy_delayed_work_init(&ifmgd->tdls_peer_del_work,
				ieee80211_tdls_peer_del_work);
	wiphy_delayed_work_init(&ifmgd->ml_reconf_work,
				ieee80211_ml_reconf_work);
	wiphy_delayed_work_init(&ifmgd->reconf.wk,
				ieee80211_ml_sta_reconf_timeout);
	timer_setup(&ifmgd->timer, ieee80211_sta_timer, 0);
	timer_setup(&ifmgd->bcn_mon_timer, ieee80211_sta_bcn_mon_timer, 0);
	timer_setup(&ifmgd->conn_mon_timer, ieee80211_sta_conn_mon_timer, 0);
	wiphy_delayed_work_init(&ifmgd->tx_tspec_wk,
				ieee80211_sta_handle_tspec_ac_params_wk);
	wiphy_delayed_work_init(&ifmgd->ttlm_work,
				ieee80211_tid_to_link_map_work);
	wiphy_delayed_work_init(&ifmgd->neg_ttlm_timeout_work,
				ieee80211_neg_ttlm_timeout_work);
	wiphy_work_init(&ifmgd->teardown_ttlm_work,
			ieee80211_teardown_ttlm_work);

	ifmgd->flags = 0;
	ifmgd->powersave = sdata->wdev.ps;
	ifmgd->uapsd_queues = sdata->local->hw.uapsd_queues;
	ifmgd->uapsd_max_sp_len = sdata->local->hw.uapsd_max_sp_len;
	/* Setup TDLS data */
	spin_lock_init(&ifmgd->teardown_lock);
	ifmgd->teardown_skb = NULL;
	ifmgd->orig_teardown_skb = NULL;
	ifmgd->mcast_seq_last = IEEE80211_SN_MODULO;
}

static void ieee80211_recalc_smps_work(struct wiphy *wiphy,
				       struct wiphy_work *work)
{
	struct ieee80211_link_data *link =
		container_of(work, struct ieee80211_link_data,
			     u.mgd.recalc_smps);

	ieee80211_recalc_smps(link->sdata, link);
}

void ieee80211_mgd_setup_link(struct ieee80211_link_data *link)
{
	struct ieee80211_sub_if_data *sdata = link->sdata;
	struct ieee80211_local *local = sdata->local;
	unsigned int link_id = link->link_id;

	link->u.mgd.p2p_noa_index = -1;
	link->conf->bssid = link->u.mgd.bssid;
	link->smps_mode = IEEE80211_SMPS_OFF;

	wiphy_work_init(&link->u.mgd.request_smps_work,
			ieee80211_request_smps_mgd_work);
	wiphy_work_init(&link->u.mgd.recalc_smps,
			ieee80211_recalc_smps_work);
	if (local->hw.wiphy->features & NL80211_FEATURE_DYNAMIC_SMPS)
		link->u.mgd.req_smps = IEEE80211_SMPS_AUTOMATIC;
	else
		link->u.mgd.req_smps = IEEE80211_SMPS_OFF;

	wiphy_delayed_work_init(&link->u.mgd.csa.switch_work,
				ieee80211_csa_switch_work);

	ieee80211_clear_tpe(&link->conf->tpe);

	if (sdata->u.mgd.assoc_data)
		ether_addr_copy(link->conf->addr,
				sdata->u.mgd.assoc_data->link[link_id].addr);
	else if (sdata->u.mgd.reconf.add_links_data)
		ether_addr_copy(link->conf->addr,
				sdata->u.mgd.reconf.add_links_data->link[link_id].addr);
	else if (!is_valid_ether_addr(link->conf->addr))
		eth_random_addr(link->conf->addr);
}

/* scan finished notification */
void ieee80211_mlme_notify_scan_completed(struct ieee80211_local *local)
{
	struct ieee80211_sub_if_data *sdata;

	/* Restart STA timers */
	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (ieee80211_sdata_running(sdata))
			ieee80211_restart_sta_timer(sdata);
	}
	rcu_read_unlock();
}

static int ieee80211_prep_connection(struct ieee80211_sub_if_data *sdata,
				     struct cfg80211_bss *cbss, s8 link_id,
				     const u8 *ap_mld_addr, bool assoc,
				     struct ieee80211_conn_settings *conn,
				     bool override,
				     unsigned long *userspace_selectors)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_bss *bss = (void *)cbss->priv;
	struct sta_info *new_sta = NULL;
	struct ieee80211_link_data *link;
	bool have_sta = false;
	bool mlo;
	int err;

	if (link_id >= 0) {
		mlo = true;
		if (WARN_ON(!ap_mld_addr))
			return -EINVAL;
		err = ieee80211_vif_set_links(sdata, BIT(link_id), 0);
	} else {
		if (WARN_ON(ap_mld_addr))
			return -EINVAL;
		ap_mld_addr = cbss->bssid;
		err = ieee80211_vif_set_links(sdata, 0, 0);
		link_id = 0;
		mlo = false;
	}

	if (err)
		return err;

	link = sdata_dereference(sdata->link[link_id], sdata);
	if (WARN_ON(!link)) {
		err = -ENOLINK;
		goto out_err;
	}

	if (WARN_ON(!ifmgd->auth_data && !ifmgd->assoc_data)) {
		err = -EINVAL;
		goto out_err;
	}

	/* If a reconfig is happening, bail out */
	if (local->in_reconfig) {
		err = -EBUSY;
		goto out_err;
	}

	if (assoc) {
		rcu_read_lock();
		have_sta = sta_info_get(sdata, ap_mld_addr);
		rcu_read_unlock();
	}

	if (!have_sta) {
		if (mlo)
			new_sta = sta_info_alloc_with_link(sdata, ap_mld_addr,
							   link_id, cbss->bssid,
							   GFP_KERNEL);
		else
			new_sta = sta_info_alloc(sdata, ap_mld_addr, GFP_KERNEL);

		if (!new_sta) {
			err = -ENOMEM;
			goto out_err;
		}

		new_sta->sta.mlo = mlo;
	}

	/*
	 * Set up the information for the new channel before setting the
	 * new channel. We can't - completely race-free - change the basic
	 * rates bitmap and the channel (sband) that it refers to, but if
	 * we set it up before we at least avoid calling into the driver's
	 * bss_info_changed() method with invalid information (since we do
	 * call that from changing the channel - only for IDLE and perhaps
	 * some others, but ...).
	 *
	 * So to avoid that, just set up all the new information before the
	 * channel, but tell the driver to apply it only afterwards, since
	 * it might need the new channel for that.
	 */
	if (new_sta) {
		const struct cfg80211_bss_ies *ies;
		struct link_sta_info *link_sta;

		rcu_read_lock();
		link_sta = rcu_dereference(new_sta->link[link_id]);
		if (WARN_ON(!link_sta)) {
			rcu_read_unlock();
			sta_info_free(local, new_sta);
			err = -EINVAL;
			goto out_err;
		}

		err = ieee80211_mgd_setup_link_sta(link, new_sta,
						   link_sta, cbss);
		if (err) {
			rcu_read_unlock();
			sta_info_free(local, new_sta);
			goto out_err;
		}

		memcpy(link->u.mgd.bssid, cbss->bssid, ETH_ALEN);

		/* set timing information */
		link->conf->beacon_int = cbss->beacon_interval;
		ies = rcu_dereference(cbss->beacon_ies);
		if (ies) {
			link->conf->sync_tsf = ies->tsf;
			link->conf->sync_device_ts =
				bss->device_ts_beacon;

			ieee80211_get_dtim(ies,
					   &link->conf->sync_dtim_count,
					   NULL);
		} else if (!ieee80211_hw_check(&sdata->local->hw,
					       TIMING_BEACON_ONLY)) {
			ies = rcu_dereference(cbss->proberesp_ies);
			/* must be non-NULL since beacon IEs were NULL */
			link->conf->sync_tsf = ies->tsf;
			link->conf->sync_device_ts =
				bss->device_ts_presp;
			link->conf->sync_dtim_count = 0;
		} else {
			link->conf->sync_tsf = 0;
			link->conf->sync_device_ts = 0;
			link->conf->sync_dtim_count = 0;
		}
		rcu_read_unlock();
	}

	if (new_sta || override) {
		/*
		 * Only set this if we're also going to calculate the AP
		 * settings etc., otherwise this was set before in a
		 * previous call. Note override is set to %true in assoc
		 * if the settings were changed.
		 */
		link->u.mgd.conn = *conn;
		err = ieee80211_prep_channel(sdata, link, link->link_id, cbss,
					     mlo, &link->u.mgd.conn,
					     userspace_selectors);
		if (err) {
			if (new_sta)
				sta_info_free(local, new_sta);
			goto out_err;
		}
		/* pass out for use in assoc */
		*conn = link->u.mgd.conn;
	}

	if (new_sta) {
		/*
		 * tell driver about BSSID, basic rates and timing
		 * this was set up above, before setting the channel
		 */
		ieee80211_link_info_change_notify(sdata, link,
						  BSS_CHANGED_BSSID |
						  BSS_CHANGED_BASIC_RATES |
						  BSS_CHANGED_BEACON_INT);

		if (assoc)
			sta_info_pre_move_state(new_sta, IEEE80211_STA_AUTH);

		err = sta_info_insert(new_sta);
		new_sta = NULL;
		if (err) {
			sdata_info(sdata,
				   "failed to insert STA entry for the AP (error %d)\n",
				   err);
			goto out_release_chan;
		}
	} else
		WARN_ON_ONCE(!ether_addr_equal(link->u.mgd.bssid, cbss->bssid));

	/* Cancel scan to ensure that nothing interferes with connection */
	if (local->scanning)
		ieee80211_scan_cancel(local);

	return 0;

out_release_chan:
	ieee80211_link_release_channel(link);
out_err:
	ieee80211_vif_set_links(sdata, 0, 0);
	return err;
}

static bool ieee80211_mgd_csa_present(struct ieee80211_sub_if_data *sdata,
				      const struct cfg80211_bss_ies *ies,
				      u8 cur_channel, bool ignore_ecsa)
{
	const struct element *csa_elem, *ecsa_elem;
	struct ieee80211_channel_sw_ie *csa = NULL;
	struct ieee80211_ext_chansw_ie *ecsa = NULL;

	if (!ies)
		return false;

	csa_elem = cfg80211_find_elem(WLAN_EID_CHANNEL_SWITCH,
				      ies->data, ies->len);
	if (csa_elem && csa_elem->datalen == sizeof(*csa))
		csa = (void *)csa_elem->data;

	ecsa_elem = cfg80211_find_elem(WLAN_EID_EXT_CHANSWITCH_ANN,
				       ies->data, ies->len);
	if (ecsa_elem && ecsa_elem->datalen == sizeof(*ecsa))
		ecsa = (void *)ecsa_elem->data;

	if (csa && csa->count == 0)
		csa = NULL;
	if (csa && !csa->mode && csa->new_ch_num == cur_channel)
		csa = NULL;

	if (ecsa && ecsa->count == 0)
		ecsa = NULL;
	if (ecsa && !ecsa->mode && ecsa->new_ch_num == cur_channel)
		ecsa = NULL;

	if (ignore_ecsa && ecsa) {
		sdata_info(sdata,
			   "Ignoring ECSA in probe response - was considered stuck!\n");
		return csa;
	}

	return csa || ecsa;
}

static bool ieee80211_mgd_csa_in_process(struct ieee80211_sub_if_data *sdata,
					 struct cfg80211_bss *bss)
{
	u8 cur_channel;
	bool ret;

	cur_channel = ieee80211_frequency_to_channel(bss->channel->center_freq);

	rcu_read_lock();
	if (ieee80211_mgd_csa_present(sdata,
				      rcu_dereference(bss->beacon_ies),
				      cur_channel, false)) {
		ret = true;
		goto out;
	}

	if (ieee80211_mgd_csa_present(sdata,
				      rcu_dereference(bss->proberesp_ies),
				      cur_channel, bss->proberesp_ecsa_stuck)) {
		ret = true;
		goto out;
	}

	ret = false;
out:
	rcu_read_unlock();
	return ret;
}

static void ieee80211_parse_cfg_selectors(unsigned long *userspace_selectors,
					  const u8 *supported_selectors,
					  u8 supported_selectors_len)
{
	if (supported_selectors) {
		for (int i = 0; i < supported_selectors_len; i++) {
			set_bit(supported_selectors[i],
				userspace_selectors);
		}
	} else {
		/* Assume SAE_H2E support for backward compatibility. */
		set_bit(BSS_MEMBERSHIP_SELECTOR_SAE_H2E,
			userspace_selectors);
	}
}

/* config hooks */
int ieee80211_mgd_auth(struct ieee80211_sub_if_data *sdata,
		       struct cfg80211_auth_request *req)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_auth_data *auth_data;
	struct ieee80211_conn_settings conn;
	struct ieee80211_link_data *link;
	struct ieee80211_supported_band *sband;
	struct ieee80211_bss *bss;
	u16 auth_alg;
	int err;
	bool cont_auth, wmm_used;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	/* prepare auth data structure */

	switch (req->auth_type) {
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		auth_alg = WLAN_AUTH_OPEN;
		break;
	case NL80211_AUTHTYPE_SHARED_KEY:
		if (fips_enabled)
			return -EOPNOTSUPP;
		auth_alg = WLAN_AUTH_SHARED_KEY;
		break;
	case NL80211_AUTHTYPE_FT:
		auth_alg = WLAN_AUTH_FT;
		break;
	case NL80211_AUTHTYPE_NETWORK_EAP:
		auth_alg = WLAN_AUTH_LEAP;
		break;
	case NL80211_AUTHTYPE_SAE:
		auth_alg = WLAN_AUTH_SAE;
		break;
	case NL80211_AUTHTYPE_FILS_SK:
		auth_alg = WLAN_AUTH_FILS_SK;
		break;
	case NL80211_AUTHTYPE_FILS_SK_PFS:
		auth_alg = WLAN_AUTH_FILS_SK_PFS;
		break;
	case NL80211_AUTHTYPE_FILS_PK:
		auth_alg = WLAN_AUTH_FILS_PK;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (ifmgd->assoc_data)
		return -EBUSY;

	if (ieee80211_mgd_csa_in_process(sdata, req->bss)) {
		sdata_info(sdata, "AP is in CSA process, reject auth\n");
		return -EINVAL;
	}

	auth_data = kzalloc(sizeof(*auth_data) + req->auth_data_len +
			    req->ie_len, GFP_KERNEL);
	if (!auth_data)
		return -ENOMEM;

	memcpy(auth_data->ap_addr,
	       req->ap_mld_addr ?: req->bss->bssid,
	       ETH_ALEN);
	auth_data->bss = req->bss;
	auth_data->link_id = req->link_id;

	if (req->auth_data_len >= 4) {
		if (req->auth_type == NL80211_AUTHTYPE_SAE) {
			__le16 *pos = (__le16 *) req->auth_data;

			auth_data->sae_trans = le16_to_cpu(pos[0]);
			auth_data->sae_status = le16_to_cpu(pos[1]);
		}
		memcpy(auth_data->data, req->auth_data + 4,
		       req->auth_data_len - 4);
		auth_data->data_len += req->auth_data_len - 4;
	}

	/* Check if continuing authentication or trying to authenticate with the
	 * same BSS that we were in the process of authenticating with and avoid
	 * removal and re-addition of the STA entry in
	 * ieee80211_prep_connection().
	 */
	cont_auth = ifmgd->auth_data && req->bss == ifmgd->auth_data->bss &&
		    ifmgd->auth_data->link_id == req->link_id;

	if (req->ie && req->ie_len) {
		memcpy(&auth_data->data[auth_data->data_len],
		       req->ie, req->ie_len);
		auth_data->data_len += req->ie_len;
	}

	if (req->key && req->key_len) {
		auth_data->key_len = req->key_len;
		auth_data->key_idx = req->key_idx;
		memcpy(auth_data->key, req->key, req->key_len);
	}

	ieee80211_parse_cfg_selectors(auth_data->userspace_selectors,
				      req->supported_selectors,
				      req->supported_selectors_len);

	auth_data->algorithm = auth_alg;

	/* try to authenticate/probe */

	if (ifmgd->auth_data) {
		if (cont_auth && req->auth_type == NL80211_AUTHTYPE_SAE) {
			auth_data->peer_confirmed =
				ifmgd->auth_data->peer_confirmed;
		}
		ieee80211_destroy_auth_data(sdata, cont_auth);
	}

	/* prep auth_data so we don't go into idle on disassoc */
	ifmgd->auth_data = auth_data;

	/* If this is continuation of an ongoing SAE authentication exchange
	 * (i.e., request to send SAE Confirm) and the peer has already
	 * confirmed, mark authentication completed since we are about to send
	 * out SAE Confirm.
	 */
	if (cont_auth && req->auth_type == NL80211_AUTHTYPE_SAE &&
	    auth_data->peer_confirmed && auth_data->sae_trans == 2)
		ieee80211_mark_sta_auth(sdata);

	if (ifmgd->associated) {
		u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

		sdata_info(sdata,
			   "disconnect from AP %pM for new auth to %pM\n",
			   sdata->vif.cfg.ap_addr, auth_data->ap_addr);
		ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH,
				       WLAN_REASON_UNSPECIFIED,
				       false, frame_buf);

		ieee80211_report_disconnect(sdata, frame_buf,
					    sizeof(frame_buf), true,
					    WLAN_REASON_UNSPECIFIED,
					    false);
	}

	/* needed for transmitting the auth frame(s) properly */
	memcpy(sdata->vif.cfg.ap_addr, auth_data->ap_addr, ETH_ALEN);

	bss = (void *)req->bss->priv;
	wmm_used = bss->wmm_used && (local->hw.queues >= IEEE80211_NUM_ACS);

	sband = local->hw.wiphy->bands[req->bss->channel->band];

	ieee80211_determine_our_sta_mode_auth(sdata, sband, req, wmm_used,
					      &conn);

	err = ieee80211_prep_connection(sdata, req->bss, req->link_id,
					req->ap_mld_addr, cont_auth,
					&conn, false,
					auth_data->userspace_selectors);
	if (err)
		goto err_clear;

	if (req->link_id >= 0)
		link = sdata_dereference(sdata->link[req->link_id], sdata);
	else
		link = &sdata->deflink;

	if (WARN_ON(!link)) {
		err = -ENOLINK;
		goto err_clear;
	}

	sdata_info(sdata, "authenticate with %pM (local address=%pM)\n",
		   auth_data->ap_addr, link->conf->addr);

	err = ieee80211_auth(sdata);
	if (err) {
		sta_info_destroy_addr(sdata, auth_data->ap_addr);
		goto err_clear;
	}

	/* hold our own reference */
	cfg80211_ref_bss(local->hw.wiphy, auth_data->bss);
	return 0;

 err_clear:
	if (!ieee80211_vif_is_mld(&sdata->vif)) {
		eth_zero_addr(sdata->deflink.u.mgd.bssid);
		ieee80211_link_info_change_notify(sdata, &sdata->deflink,
						  BSS_CHANGED_BSSID);
		ieee80211_link_release_channel(&sdata->deflink);
	}
	ifmgd->auth_data = NULL;
	kfree(auth_data);
	return err;
}

static void
ieee80211_setup_assoc_link(struct ieee80211_sub_if_data *sdata,
			   struct ieee80211_mgd_assoc_data *assoc_data,
			   struct cfg80211_assoc_request *req,
			   struct ieee80211_conn_settings *conn,
			   unsigned int link_id)
{
	struct ieee80211_local *local = sdata->local;
	const struct cfg80211_bss_ies *bss_ies;
	struct ieee80211_supported_band *sband;
	struct ieee80211_link_data *link;
	struct cfg80211_bss *cbss;
	struct ieee80211_bss *bss;

	cbss = assoc_data->link[link_id].bss;
	if (WARN_ON(!cbss))
		return;

	bss = (void *)cbss->priv;

	sband = local->hw.wiphy->bands[cbss->channel->band];
	if (WARN_ON(!sband))
		return;

	link = sdata_dereference(sdata->link[link_id], sdata);
	if (WARN_ON(!link))
		return;

	/* for MLO connections assume advertising all rates is OK */
	if (!req->ap_mld_addr) {
		assoc_data->supp_rates = bss->supp_rates;
		assoc_data->supp_rates_len = bss->supp_rates_len;
	}

	/* copy and link elems for the STA profile */
	if (req->links[link_id].elems_len) {
		memcpy(assoc_data->ie_pos, req->links[link_id].elems,
		       req->links[link_id].elems_len);
		assoc_data->link[link_id].elems = assoc_data->ie_pos;
		assoc_data->link[link_id].elems_len = req->links[link_id].elems_len;
		assoc_data->ie_pos += req->links[link_id].elems_len;
	}

	link->u.mgd.beacon_crc_valid = false;
	link->u.mgd.dtim_period = 0;
	link->u.mgd.have_beacon = false;

	/* override HT configuration only if the AP and we support it */
	if (conn->mode >= IEEE80211_CONN_MODE_HT) {
		struct ieee80211_sta_ht_cap sta_ht_cap;

		memcpy(&sta_ht_cap, &sband->ht_cap, sizeof(sta_ht_cap));
		ieee80211_apply_htcap_overrides(sdata, &sta_ht_cap);
	}

	rcu_read_lock();
	bss_ies = rcu_dereference(cbss->beacon_ies);
	if (bss_ies) {
		u8 dtim_count = 0;

		ieee80211_get_dtim(bss_ies, &dtim_count,
				   &link->u.mgd.dtim_period);

		sdata->deflink.u.mgd.have_beacon = true;

		if (ieee80211_hw_check(&local->hw, TIMING_BEACON_ONLY)) {
			link->conf->sync_tsf = bss_ies->tsf;
			link->conf->sync_device_ts = bss->device_ts_beacon;
			link->conf->sync_dtim_count = dtim_count;
		}
	} else {
		bss_ies = rcu_dereference(cbss->ies);
	}

	if (bss_ies) {
		const struct element *elem;

		elem = cfg80211_find_ext_elem(WLAN_EID_EXT_MULTIPLE_BSSID_CONFIGURATION,
					      bss_ies->data, bss_ies->len);
		if (elem && elem->datalen >= 3)
			link->conf->profile_periodicity = elem->data[2];
		else
			link->conf->profile_periodicity = 0;

		elem = cfg80211_find_elem(WLAN_EID_EXT_CAPABILITY,
					  bss_ies->data, bss_ies->len);
		if (elem && elem->datalen >= 11 &&
		    (elem->data[10] & WLAN_EXT_CAPA11_EMA_SUPPORT))
			link->conf->ema_ap = true;
		else
			link->conf->ema_ap = false;
	}
	rcu_read_unlock();

	if (bss->corrupt_data) {
		char *corrupt_type = "data";

		if (bss->corrupt_data & IEEE80211_BSS_CORRUPT_BEACON) {
			if (bss->corrupt_data & IEEE80211_BSS_CORRUPT_PROBE_RESP)
				corrupt_type = "beacon and probe response";
			else
				corrupt_type = "beacon";
		} else if (bss->corrupt_data & IEEE80211_BSS_CORRUPT_PROBE_RESP) {
			corrupt_type = "probe response";
		}
		sdata_info(sdata, "associating to AP %pM with corrupt %s\n",
			   cbss->bssid, corrupt_type);
	}

	if (link->u.mgd.req_smps == IEEE80211_SMPS_AUTOMATIC) {
		if (sdata->u.mgd.powersave)
			link->smps_mode = IEEE80211_SMPS_DYNAMIC;
		else
			link->smps_mode = IEEE80211_SMPS_OFF;
	} else {
		link->smps_mode = link->u.mgd.req_smps;
	}
}

static int
ieee80211_mgd_get_ap_ht_vht_capa(struct ieee80211_sub_if_data *sdata,
				 struct ieee80211_mgd_assoc_data *assoc_data,
				 int link_id)
{
	struct cfg80211_bss *cbss = assoc_data->link[link_id].bss;
	enum nl80211_band band = cbss->channel->band;
	struct ieee80211_supported_band *sband;
	const struct element *elem;
	int err;

	/* neither HT nor VHT elements used on 6 GHz */
	if (band == NL80211_BAND_6GHZ)
		return 0;

	if (assoc_data->link[link_id].conn.mode < IEEE80211_CONN_MODE_HT)
		return 0;

	rcu_read_lock();
	elem = ieee80211_bss_get_elem(cbss, WLAN_EID_HT_OPERATION);
	if (!elem || elem->datalen < sizeof(struct ieee80211_ht_operation)) {
		mlme_link_id_dbg(sdata, link_id, "no HT operation on BSS %pM\n",
				 cbss->bssid);
		err = -EINVAL;
		goto out_rcu;
	}
	assoc_data->link[link_id].ap_ht_param =
		((struct ieee80211_ht_operation *)(elem->data))->ht_param;
	rcu_read_unlock();

	if (assoc_data->link[link_id].conn.mode < IEEE80211_CONN_MODE_VHT)
		return 0;

	/* some drivers want to support VHT on 2.4 GHz even */
	sband = sdata->local->hw.wiphy->bands[band];
	if (!sband->vht_cap.vht_supported)
		return 0;

	rcu_read_lock();
	elem = ieee80211_bss_get_elem(cbss, WLAN_EID_VHT_CAPABILITY);
	/* but even then accept it not being present on the AP */
	if (!elem && band == NL80211_BAND_2GHZ) {
		err = 0;
		goto out_rcu;
	}
	if (!elem || elem->datalen < sizeof(struct ieee80211_vht_cap)) {
		mlme_link_id_dbg(sdata, link_id, "no VHT capa on BSS %pM\n",
				 cbss->bssid);
		err = -EINVAL;
		goto out_rcu;
	}
	memcpy(&assoc_data->link[link_id].ap_vht_cap, elem->data,
	       sizeof(struct ieee80211_vht_cap));
	rcu_read_unlock();

	return 0;
out_rcu:
	rcu_read_unlock();
	return err;
}

int ieee80211_mgd_assoc(struct ieee80211_sub_if_data *sdata,
			struct cfg80211_assoc_request *req)
{
	unsigned int assoc_link_id = req->link_id < 0 ? 0 : req->link_id;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_assoc_data *assoc_data;
	const struct element *ssid_elem;
	struct ieee80211_vif_cfg *vif_cfg = &sdata->vif.cfg;
	struct ieee80211_link_data *link;
	struct cfg80211_bss *cbss;
	bool override, uapsd_supported;
	bool match_auth;
	int i, err;
	size_t size = sizeof(*assoc_data) + req->ie_len;

	for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++)
		size += req->links[i].elems_len;

	/* FIXME: no support for 4-addr MLO yet */
	if (sdata->u.mgd.use_4addr && req->link_id >= 0)
		return -EOPNOTSUPP;

	assoc_data = kzalloc(size, GFP_KERNEL);
	if (!assoc_data)
		return -ENOMEM;

	cbss = req->link_id < 0 ? req->bss : req->links[req->link_id].bss;

	if (ieee80211_mgd_csa_in_process(sdata, cbss)) {
		sdata_info(sdata, "AP is in CSA process, reject assoc\n");
		err = -EINVAL;
		goto err_free;
	}

	rcu_read_lock();
	ssid_elem = ieee80211_bss_get_elem(cbss, WLAN_EID_SSID);
	if (!ssid_elem || ssid_elem->datalen > sizeof(assoc_data->ssid)) {
		rcu_read_unlock();
		err = -EINVAL;
		goto err_free;
	}

	memcpy(assoc_data->ssid, ssid_elem->data, ssid_elem->datalen);
	assoc_data->ssid_len = ssid_elem->datalen;
	rcu_read_unlock();

	if (req->ap_mld_addr)
		memcpy(assoc_data->ap_addr, req->ap_mld_addr, ETH_ALEN);
	else
		memcpy(assoc_data->ap_addr, cbss->bssid, ETH_ALEN);

	assoc_data->ext_mld_capa_ops = cpu_to_le16(req->ext_mld_capa_ops);

	if (ifmgd->associated) {
		u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

		sdata_info(sdata,
			   "disconnect from AP %pM for new assoc to %pM\n",
			   sdata->vif.cfg.ap_addr, assoc_data->ap_addr);
		ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH,
				       WLAN_REASON_UNSPECIFIED,
				       false, frame_buf);

		ieee80211_report_disconnect(sdata, frame_buf,
					    sizeof(frame_buf), true,
					    WLAN_REASON_UNSPECIFIED,
					    false);
	}

	memset(sdata->u.mgd.userspace_selectors, 0,
	       sizeof(sdata->u.mgd.userspace_selectors));
	ieee80211_parse_cfg_selectors(sdata->u.mgd.userspace_selectors,
				      req->supported_selectors,
				      req->supported_selectors_len);

	memcpy(&ifmgd->ht_capa, &req->ht_capa, sizeof(ifmgd->ht_capa));
	memcpy(&ifmgd->ht_capa_mask, &req->ht_capa_mask,
	       sizeof(ifmgd->ht_capa_mask));

	memcpy(&ifmgd->vht_capa, &req->vht_capa, sizeof(ifmgd->vht_capa));
	memcpy(&ifmgd->vht_capa_mask, &req->vht_capa_mask,
	       sizeof(ifmgd->vht_capa_mask));

	memcpy(&ifmgd->s1g_capa, &req->s1g_capa, sizeof(ifmgd->s1g_capa));
	memcpy(&ifmgd->s1g_capa_mask, &req->s1g_capa_mask,
	       sizeof(ifmgd->s1g_capa_mask));

	/* keep some setup (AP STA, channel, ...) if matching */
	match_auth = ifmgd->auth_data &&
		     ether_addr_equal(ifmgd->auth_data->ap_addr,
				      assoc_data->ap_addr) &&
		     ifmgd->auth_data->link_id == req->link_id;

	if (req->ap_mld_addr) {
		uapsd_supported = true;

		if (req->flags & (ASSOC_REQ_DISABLE_HT |
				  ASSOC_REQ_DISABLE_VHT |
				  ASSOC_REQ_DISABLE_HE |
				  ASSOC_REQ_DISABLE_EHT)) {
			err = -EINVAL;
			goto err_free;
		}

		for (i = 0; i < IEEE80211_MLD_MAX_NUM_LINKS; i++) {
			struct ieee80211_supported_band *sband;
			struct cfg80211_bss *link_cbss = req->links[i].bss;
			struct ieee80211_bss *bss;

			if (!link_cbss)
				continue;

			bss = (void *)link_cbss->priv;

			if (!bss->wmm_used) {
				err = -EINVAL;
				req->links[i].error = err;
				goto err_free;
			}

			if (link_cbss->channel->band == NL80211_BAND_S1GHZ) {
				err = -EINVAL;
				req->links[i].error = err;
				goto err_free;
			}

			link = sdata_dereference(sdata->link[i], sdata);
			if (link)
				ether_addr_copy(assoc_data->link[i].addr,
						link->conf->addr);
			else
				eth_random_addr(assoc_data->link[i].addr);
			sband = local->hw.wiphy->bands[link_cbss->channel->band];

			if (match_auth && i == assoc_link_id && link)
				assoc_data->link[i].conn = link->u.mgd.conn;
			else
				assoc_data->link[i].conn =
					ieee80211_conn_settings_unlimited;
			ieee80211_determine_our_sta_mode_assoc(sdata, sband,
							       req, true, i,
							       &assoc_data->link[i].conn);
			assoc_data->link[i].bss = link_cbss;
			assoc_data->link[i].disabled = req->links[i].disabled;

			if (!bss->uapsd_supported)
				uapsd_supported = false;

			if (assoc_data->link[i].conn.mode < IEEE80211_CONN_MODE_EHT) {
				err = -EINVAL;
				req->links[i].error = err;
				goto err_free;
			}

			err = ieee80211_mgd_get_ap_ht_vht_capa(sdata,
							       assoc_data, i);
			if (err) {
				err = -EINVAL;
				req->links[i].error = err;
				goto err_free;
			}
		}

		assoc_data->wmm = true;
	} else {
		struct ieee80211_supported_band *sband;
		struct ieee80211_bss *bss = (void *)cbss->priv;

		memcpy(assoc_data->link[0].addr, sdata->vif.addr, ETH_ALEN);
		assoc_data->s1g = cbss->channel->band == NL80211_BAND_S1GHZ;

		assoc_data->wmm = bss->wmm_used &&
				  (local->hw.queues >= IEEE80211_NUM_ACS);

		if (cbss->channel->band == NL80211_BAND_6GHZ &&
		    req->flags & (ASSOC_REQ_DISABLE_HT |
				  ASSOC_REQ_DISABLE_VHT |
				  ASSOC_REQ_DISABLE_HE)) {
			err = -EINVAL;
			goto err_free;
		}

		sband = local->hw.wiphy->bands[cbss->channel->band];

		assoc_data->link[0].bss = cbss;

		if (match_auth)
			assoc_data->link[0].conn = sdata->deflink.u.mgd.conn;
		else
			assoc_data->link[0].conn =
				ieee80211_conn_settings_unlimited;
		ieee80211_determine_our_sta_mode_assoc(sdata, sband, req,
						       assoc_data->wmm, 0,
						       &assoc_data->link[0].conn);

		uapsd_supported = bss->uapsd_supported;

		err = ieee80211_mgd_get_ap_ht_vht_capa(sdata, assoc_data, 0);
		if (err)
			goto err_free;
	}

	assoc_data->spp_amsdu = req->flags & ASSOC_REQ_SPP_AMSDU;

	if (ifmgd->auth_data && !ifmgd->auth_data->done) {
		err = -EBUSY;
		goto err_free;
	}

	if (ifmgd->assoc_data) {
		err = -EBUSY;
		goto err_free;
	}

	/* Cleanup is delayed if auth_data matches */
	if (ifmgd->auth_data && !match_auth)
		ieee80211_destroy_auth_data(sdata, false);

	if (req->ie && req->ie_len) {
		memcpy(assoc_data->ie, req->ie, req->ie_len);
		assoc_data->ie_len = req->ie_len;
		assoc_data->ie_pos = assoc_data->ie + assoc_data->ie_len;
	} else {
		assoc_data->ie_pos = assoc_data->ie;
	}

	if (req->fils_kek) {
		/* should already be checked in cfg80211 - so warn */
		if (WARN_ON(req->fils_kek_len > FILS_MAX_KEK_LEN)) {
			err = -EINVAL;
			goto err_free;
		}
		memcpy(assoc_data->fils_kek, req->fils_kek,
		       req->fils_kek_len);
		assoc_data->fils_kek_len = req->fils_kek_len;
	}

	if (req->fils_nonces)
		memcpy(assoc_data->fils_nonces, req->fils_nonces,
		       2 * FILS_NONCE_LEN);

	/* default timeout */
	assoc_data->timeout = jiffies;
	assoc_data->timeout_started = true;

	assoc_data->assoc_link_id = assoc_link_id;

	if (req->ap_mld_addr) {
		/* if there was no authentication, set up the link */
		err = ieee80211_vif_set_links(sdata, BIT(assoc_link_id), 0);
		if (err)
			goto err_clear;
	}

	link = sdata_dereference(sdata->link[assoc_link_id], sdata);
	if (WARN_ON(!link)) {
		err = -EINVAL;
		goto err_clear;
	}

	override = link->u.mgd.conn.mode !=
			assoc_data->link[assoc_link_id].conn.mode ||
		   link->u.mgd.conn.bw_limit !=
			assoc_data->link[assoc_link_id].conn.bw_limit;
	link->u.mgd.conn = assoc_data->link[assoc_link_id].conn;

	ieee80211_setup_assoc_link(sdata, assoc_data, req, &link->u.mgd.conn,
				   assoc_link_id);

	if (WARN((sdata->vif.driver_flags & IEEE80211_VIF_SUPPORTS_UAPSD) &&
		 ieee80211_hw_check(&local->hw, PS_NULLFUNC_STACK),
	     "U-APSD not supported with HW_PS_NULLFUNC_STACK\n"))
		sdata->vif.driver_flags &= ~IEEE80211_VIF_SUPPORTS_UAPSD;

	if (assoc_data->wmm && uapsd_supported &&
	    (sdata->vif.driver_flags & IEEE80211_VIF_SUPPORTS_UAPSD)) {
		assoc_data->uapsd = true;
		ifmgd->flags |= IEEE80211_STA_UAPSD_ENABLED;
	} else {
		assoc_data->uapsd = false;
		ifmgd->flags &= ~IEEE80211_STA_UAPSD_ENABLED;
	}

	if (req->prev_bssid)
		memcpy(assoc_data->prev_ap_addr, req->prev_bssid, ETH_ALEN);

	if (req->use_mfp) {
		ifmgd->mfp = IEEE80211_MFP_REQUIRED;
		ifmgd->flags |= IEEE80211_STA_MFP_ENABLED;
	} else {
		ifmgd->mfp = IEEE80211_MFP_DISABLED;
		ifmgd->flags &= ~IEEE80211_STA_MFP_ENABLED;
	}

	if (req->flags & ASSOC_REQ_USE_RRM)
		ifmgd->flags |= IEEE80211_STA_ENABLE_RRM;
	else
		ifmgd->flags &= ~IEEE80211_STA_ENABLE_RRM;

	if (req->crypto.control_port)
		ifmgd->flags |= IEEE80211_STA_CONTROL_PORT;
	else
		ifmgd->flags &= ~IEEE80211_STA_CONTROL_PORT;

	sdata->control_port_protocol = req->crypto.control_port_ethertype;
	sdata->control_port_no_encrypt = req->crypto.control_port_no_encrypt;
	sdata->control_port_over_nl80211 =
					req->crypto.control_port_over_nl80211;
	sdata->control_port_no_preauth = req->crypto.control_port_no_preauth;

	/* kick off associate process */
	ifmgd->assoc_data = assoc_data;

	for (i = 0; i < ARRAY_SIZE(assoc_data->link); i++) {
		if (!assoc_data->link[i].bss)
			continue;
		if (i == assoc_data->assoc_link_id)
			continue;
		/* only calculate the mode, hence link == NULL */
		err = ieee80211_prep_channel(sdata, NULL, i,
					     assoc_data->link[i].bss, true,
					     &assoc_data->link[i].conn,
					     sdata->u.mgd.userspace_selectors);
		if (err) {
			req->links[i].error = err;
			goto err_clear;
		}
	}

	memcpy(vif_cfg->ssid, assoc_data->ssid, assoc_data->ssid_len);
	vif_cfg->ssid_len = assoc_data->ssid_len;

	/* needed for transmitting the assoc frames properly */
	memcpy(sdata->vif.cfg.ap_addr, assoc_data->ap_addr, ETH_ALEN);

	err = ieee80211_prep_connection(sdata, cbss, req->link_id,
					req->ap_mld_addr, true,
					&assoc_data->link[assoc_link_id].conn,
					override,
					sdata->u.mgd.userspace_selectors);
	if (err)
		goto err_clear;

	if (ieee80211_hw_check(&sdata->local->hw, NEED_DTIM_BEFORE_ASSOC)) {
		const struct cfg80211_bss_ies *beacon_ies;

		rcu_read_lock();
		beacon_ies = rcu_dereference(req->bss->beacon_ies);
		if (!beacon_ies) {
			/*
			 * Wait up to one beacon interval ...
			 * should this be more if we miss one?
			 */
			sdata_info(sdata, "waiting for beacon from %pM\n",
				   link->u.mgd.bssid);
			assoc_data->timeout = TU_TO_EXP_TIME(req->bss->beacon_interval);
			assoc_data->timeout_started = true;
			assoc_data->need_beacon = true;
		}
		rcu_read_unlock();
	}

	run_again(sdata, assoc_data->timeout);

	/* We are associating, clean up auth_data */
	if (ifmgd->auth_data)
		ieee80211_destroy_auth_data(sdata, true);

	return 0;
 err_clear:
	if (!ifmgd->auth_data) {
		eth_zero_addr(sdata->deflink.u.mgd.bssid);
		ieee80211_link_info_change_notify(sdata, &sdata->deflink,
						  BSS_CHANGED_BSSID);
	}
	ifmgd->assoc_data = NULL;
 err_free:
	kfree(assoc_data);
	return err;
}

int ieee80211_mgd_deauth(struct ieee80211_sub_if_data *sdata,
			 struct cfg80211_deauth_request *req)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];
	bool tx = !req->local_state_change;
	struct ieee80211_prep_tx_info info = {
		.subtype = IEEE80211_STYPE_DEAUTH,
	};

	if (ifmgd->auth_data &&
	    ether_addr_equal(ifmgd->auth_data->ap_addr, req->bssid)) {
		sdata_info(sdata,
			   "aborting authentication with %pM by local choice (Reason: %u=%s)\n",
			   req->bssid, req->reason_code,
			   ieee80211_get_reason_code_string(req->reason_code));

		info.link_id = ifmgd->auth_data->link_id;
		drv_mgd_prepare_tx(sdata->local, sdata, &info);
		ieee80211_send_deauth_disassoc(sdata, req->bssid, req->bssid,
					       IEEE80211_STYPE_DEAUTH,
					       req->reason_code, tx,
					       frame_buf);
		ieee80211_destroy_auth_data(sdata, false);
		ieee80211_report_disconnect(sdata, frame_buf,
					    sizeof(frame_buf), true,
					    req->reason_code, false);
		drv_mgd_complete_tx(sdata->local, sdata, &info);
		return 0;
	}

	if (ifmgd->assoc_data &&
	    ether_addr_equal(ifmgd->assoc_data->ap_addr, req->bssid)) {
		sdata_info(sdata,
			   "aborting association with %pM by local choice (Reason: %u=%s)\n",
			   req->bssid, req->reason_code,
			   ieee80211_get_reason_code_string(req->reason_code));

		info.link_id = ifmgd->assoc_data->assoc_link_id;
		drv_mgd_prepare_tx(sdata->local, sdata, &info);
		ieee80211_send_deauth_disassoc(sdata, req->bssid, req->bssid,
					       IEEE80211_STYPE_DEAUTH,
					       req->reason_code, tx,
					       frame_buf);
		ieee80211_destroy_assoc_data(sdata, ASSOC_ABANDON);
		ieee80211_report_disconnect(sdata, frame_buf,
					    sizeof(frame_buf), true,
					    req->reason_code, false);
		drv_mgd_complete_tx(sdata->local, sdata, &info);
		return 0;
	}

	if (ifmgd->associated &&
	    ether_addr_equal(sdata->vif.cfg.ap_addr, req->bssid)) {
		sdata_info(sdata,
			   "deauthenticating from %pM by local choice (Reason: %u=%s)\n",
			   req->bssid, req->reason_code,
			   ieee80211_get_reason_code_string(req->reason_code));

		ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DEAUTH,
				       req->reason_code, tx, frame_buf);
		ieee80211_report_disconnect(sdata, frame_buf,
					    sizeof(frame_buf), true,
					    req->reason_code, false);
		return 0;
	}

	return -ENOTCONN;
}

int ieee80211_mgd_disassoc(struct ieee80211_sub_if_data *sdata,
			   struct cfg80211_disassoc_request *req)
{
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];

	if (!sdata->u.mgd.associated ||
	    memcmp(sdata->vif.cfg.ap_addr, req->ap_addr, ETH_ALEN))
		return -ENOTCONN;

	sdata_info(sdata,
		   "disassociating from %pM by local choice (Reason: %u=%s)\n",
		   req->ap_addr, req->reason_code,
		   ieee80211_get_reason_code_string(req->reason_code));

	ieee80211_set_disassoc(sdata, IEEE80211_STYPE_DISASSOC,
			       req->reason_code, !req->local_state_change,
			       frame_buf);

	ieee80211_report_disconnect(sdata, frame_buf, sizeof(frame_buf), true,
				    req->reason_code, false);

	return 0;
}

void ieee80211_mgd_stop_link(struct ieee80211_link_data *link)
{
	wiphy_work_cancel(link->sdata->local->hw.wiphy,
			  &link->u.mgd.request_smps_work);
	wiphy_work_cancel(link->sdata->local->hw.wiphy,
			  &link->u.mgd.recalc_smps);
	wiphy_delayed_work_cancel(link->sdata->local->hw.wiphy,
				  &link->u.mgd.csa.switch_work);
}

void ieee80211_mgd_stop(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;

	/*
	 * Make sure some work items will not run after this,
	 * they will not do anything but might not have been
	 * cancelled when disconnecting.
	 */
	wiphy_work_cancel(sdata->local->hw.wiphy,
			  &ifmgd->monitor_work);
	wiphy_work_cancel(sdata->local->hw.wiphy,
			  &ifmgd->beacon_connection_loss_work);
	wiphy_work_cancel(sdata->local->hw.wiphy,
			  &ifmgd->csa_connection_drop_work);
	wiphy_delayed_work_cancel(sdata->local->hw.wiphy,
				  &ifmgd->tdls_peer_del_work);

	if (ifmgd->assoc_data)
		ieee80211_destroy_assoc_data(sdata, ASSOC_TIMEOUT);
	if (ifmgd->auth_data)
		ieee80211_destroy_auth_data(sdata, false);
	spin_lock_bh(&ifmgd->teardown_lock);
	if (ifmgd->teardown_skb) {
		kfree_skb(ifmgd->teardown_skb);
		ifmgd->teardown_skb = NULL;
		ifmgd->orig_teardown_skb = NULL;
	}
	kfree(ifmgd->assoc_req_ies);
	ifmgd->assoc_req_ies = NULL;
	ifmgd->assoc_req_ies_len = 0;
	spin_unlock_bh(&ifmgd->teardown_lock);
	timer_delete_sync(&ifmgd->timer);
}

void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif,
			       enum nl80211_cqm_rssi_threshold_event rssi_event,
			       s32 rssi_level,
			       gfp_t gfp)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	trace_api_cqm_rssi_notify(sdata, rssi_event, rssi_level);

	cfg80211_cqm_rssi_notify(sdata->dev, rssi_event, rssi_level, gfp);
}
EXPORT_SYMBOL(ieee80211_cqm_rssi_notify);

void ieee80211_cqm_beacon_loss_notify(struct ieee80211_vif *vif, gfp_t gfp)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	trace_api_cqm_beacon_loss_notify(sdata->local, sdata);

	cfg80211_cqm_beacon_loss_notify(sdata->dev, gfp);
}
EXPORT_SYMBOL(ieee80211_cqm_beacon_loss_notify);

static void _ieee80211_enable_rssi_reports(struct ieee80211_sub_if_data *sdata,
					    int rssi_min_thold,
					    int rssi_max_thold)
{
	trace_api_enable_rssi_reports(sdata, rssi_min_thold, rssi_max_thold);

	if (WARN_ON(sdata->vif.type != NL80211_IFTYPE_STATION))
		return;

	/*
	 * Scale up threshold values before storing it, as the RSSI averaging
	 * algorithm uses a scaled up value as well. Change this scaling
	 * factor if the RSSI averaging algorithm changes.
	 */
	sdata->u.mgd.rssi_min_thold = rssi_min_thold*16;
	sdata->u.mgd.rssi_max_thold = rssi_max_thold*16;
}

void ieee80211_enable_rssi_reports(struct ieee80211_vif *vif,
				    int rssi_min_thold,
				    int rssi_max_thold)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	WARN_ON(rssi_min_thold == rssi_max_thold ||
		rssi_min_thold > rssi_max_thold);

	_ieee80211_enable_rssi_reports(sdata, rssi_min_thold,
				       rssi_max_thold);
}
EXPORT_SYMBOL(ieee80211_enable_rssi_reports);

void ieee80211_disable_rssi_reports(struct ieee80211_vif *vif)
{
	struct ieee80211_sub_if_data *sdata = vif_to_sdata(vif);

	_ieee80211_enable_rssi_reports(sdata, 0, 0);
}
EXPORT_SYMBOL(ieee80211_disable_rssi_reports);

void ieee80211_process_ml_reconf_resp(struct ieee80211_sub_if_data *sdata,
				      struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	struct ieee80211_mgd_assoc_data *add_links_data =
		ifmgd->reconf.add_links_data;
	struct sta_info *sta;
	struct cfg80211_mlo_reconf_done_data done_data = {};
	u16 sta_changed_links = sdata->u.mgd.reconf.added_links |
		                sdata->u.mgd.reconf.removed_links;
	u16 link_mask, valid_links;
	unsigned int link_id;
	size_t orig_len = len;
	u8 i, group_key_data_len;
	u8 *pos;

	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    len < offsetofend(typeof(*mgmt), u.action.u.ml_reconf_resp) ||
	    mgmt->u.action.u.ml_reconf_resp.dialog_token !=
	    sdata->u.mgd.reconf.dialog_token ||
	    !sta_changed_links)
		return;

	pos = mgmt->u.action.u.ml_reconf_resp.variable;
	len -= offsetofend(typeof(*mgmt), u.action.u.ml_reconf_resp);

	/* each status duple is 3 octets */
	if (len < mgmt->u.action.u.ml_reconf_resp.count * 3) {
		sdata_info(sdata,
			   "mlo: reconf: unexpected len=%zu, count=%u\n",
			   len, mgmt->u.action.u.ml_reconf_resp.count);
		goto disconnect;
	}

	link_mask = sta_changed_links;
	for (i = 0; i < mgmt->u.action.u.ml_reconf_resp.count; i++) {
		u16 status = get_unaligned_le16(pos + 1);

		link_id = *pos;

		if (!(link_mask & BIT(link_id))) {
			sdata_info(sdata,
				   "mlo: reconf: unexpected link: %u, changed=0x%x\n",
				   link_id, sta_changed_links);
			goto disconnect;
		}

		/* clear the corresponding link, to detect the case that
		 * the same link was included more than one time
		 */
		link_mask &= ~BIT(link_id);

		/* Handle failure to remove links here. Failure to remove added
		 * links will be done later in the flow.
		 */
		if (status != WLAN_STATUS_SUCCESS) {
			sdata_info(sdata,
				   "mlo: reconf: failed on link=%u, status=%u\n",
				   link_id, status);

			/* The AP MLD failed to remove a link that was already
			 * removed locally. As this is not expected behavior,
			 * disconnect
			 */
			if (sdata->u.mgd.reconf.removed_links & BIT(link_id))
				goto disconnect;

			/* The AP MLD failed to add a link. Remove it from the
			 * added links.
			 */
			sdata->u.mgd.reconf.added_links &= ~BIT(link_id);
		}

		pos += 3;
		len -= 3;
	}

	if (link_mask) {
		sdata_info(sdata,
			   "mlo: reconf: no response for links=0x%x\n",
			   link_mask);
		goto disconnect;
	}

	if (!sdata->u.mgd.reconf.added_links)
		goto out;

	if (len < 1 || len < 1 + *pos) {
		sdata_info(sdata,
			   "mlo: reconf: invalid group key data length");
		goto disconnect;
	}

	/* The Group Key Data field must be present when links are added. This
	 * field should be processed by userland.
	 */
	group_key_data_len = *pos++;

	pos += group_key_data_len;
	len -= group_key_data_len + 1;

	/* Process the information for the added links */
	sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	if (WARN_ON(!sta))
		goto disconnect;

	valid_links = sdata->vif.valid_links;
	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		if (!add_links_data->link[link_id].bss ||
		    !(sdata->u.mgd.reconf.added_links & BIT(link_id)))

			continue;

		valid_links |= BIT(link_id);
		if (ieee80211_sta_allocate_link(sta, link_id))
			goto disconnect;
	}

	ieee80211_vif_set_links(sdata, valid_links, sdata->vif.dormant_links);
	link_mask = 0;
	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct cfg80211_bss *cbss = add_links_data->link[link_id].bss;
		struct ieee80211_link_data *link;
		struct link_sta_info *link_sta;
		u64 changed = 0;

		if (!cbss)
			continue;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (WARN_ON(!link))
			goto disconnect;

		link_info(link,
			  "mlo: reconf: local address %pM, AP link address %pM\n",
			  add_links_data->link[link_id].addr,
			  add_links_data->link[link_id].bss->bssid);

		link_sta = rcu_dereference_protected(sta->link[link_id],
						     lockdep_is_held(&local->hw.wiphy->mtx));
		if (WARN_ON(!link_sta))
			goto disconnect;

		if (!link->u.mgd.have_beacon) {
			const struct cfg80211_bss_ies *ies;

			rcu_read_lock();
			ies = rcu_dereference(cbss->beacon_ies);
			if (ies)
				link->u.mgd.have_beacon = true;
			else
				ies = rcu_dereference(cbss->ies);
			ieee80211_get_dtim(ies,
					   &link->conf->sync_dtim_count,
					   &link->u.mgd.dtim_period);
			link->conf->beacon_int = cbss->beacon_interval;
			rcu_read_unlock();
		}

		link->conf->dtim_period = link->u.mgd.dtim_period ?: 1;

		link->u.mgd.conn = add_links_data->link[link_id].conn;
		if (ieee80211_prep_channel(sdata, link, link_id, cbss,
					   true, &link->u.mgd.conn,
					   sdata->u.mgd.userspace_selectors)) {
			link_info(link, "mlo: reconf: prep_channel failed\n");
			goto disconnect;
		}

		if (ieee80211_mgd_setup_link_sta(link, sta, link_sta,
						 add_links_data->link[link_id].bss))
			goto disconnect;

		if (!ieee80211_assoc_config_link(link, link_sta,
						 add_links_data->link[link_id].bss,
						 mgmt, pos, len,
						 &changed))
			goto disconnect;

		/* The AP MLD indicated success for this link, but the station
		 * profile status indicated otherwise. Since there is an
		 * inconsistency in the ML reconfiguration response, disconnect
		 */
		if (add_links_data->link[link_id].status != WLAN_STATUS_SUCCESS)
			goto disconnect;

		ieee80211_sta_init_nss(link_sta);
		if (ieee80211_sta_activate_link(sta, link_id))
			goto disconnect;

		changed |= ieee80211_link_set_associated(link, cbss);
		ieee80211_link_info_change_notify(sdata, link, changed);

		ieee80211_recalc_smps(sdata, link);
		link_mask |= BIT(link_id);
	}

	sdata_info(sdata,
		   "mlo: reconf: current valid_links=0x%x, added=0x%x\n",
		   valid_links, link_mask);

	/* links might have changed due to rejected ones, set them again */
	ieee80211_vif_set_links(sdata, valid_links, sdata->vif.dormant_links);
	ieee80211_vif_cfg_change_notify(sdata, BSS_CHANGED_MLD_VALID_LINKS);

	ieee80211_recalc_ps(local);
	ieee80211_recalc_ps_vif(sdata);

	done_data.buf = (const u8 *)mgmt;
	done_data.len = orig_len;
	done_data.added_links = link_mask;

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		done_data.links[link_id].bss = add_links_data->link[link_id].bss;
		done_data.links[link_id].addr =
			add_links_data->link[link_id].addr;
	}

	cfg80211_mlo_reconf_add_done(sdata->dev, &done_data);
	kfree(sdata->u.mgd.reconf.add_links_data);
	sdata->u.mgd.reconf.add_links_data = NULL;
out:
	ieee80211_ml_reconf_reset(sdata);
	return;

disconnect:
	__ieee80211_disconnect(sdata);
}

static struct sk_buff *
ieee80211_build_ml_reconf_req(struct ieee80211_sub_if_data *sdata,
			      struct ieee80211_mgd_assoc_data *add_links_data,
			      u16 removed_links, __le16 ext_mld_capa_ops)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct ieee80211_multi_link_elem *ml_elem;
	struct ieee80211_mle_basic_common_info *common;
	enum nl80211_iftype iftype = ieee80211_vif_type_p2p(&sdata->vif);
	struct sk_buff *skb;
	size_t size;
	unsigned int link_id;
	__le16 eml_capa = 0, mld_capa_ops = 0;
	struct ieee80211_tx_info *info;
	u8 common_size, var_common_size;
	u8 *ml_elem_len;
	u16 capab = 0;

	size = local->hw.extra_tx_headroom + sizeof(*mgmt);

	/* Consider the maximal length of the reconfiguration ML element */
	size += sizeof(struct ieee80211_multi_link_elem);

	/* The Basic ML element and the Reconfiguration ML element have the same
	 * fixed common information fields in the context of ML reconfiguration
	 * action frame. The AP MLD MAC address must always be present
	 */
	common_size = sizeof(*common);

	/* when adding links, the MLD capabilities must be present */
	var_common_size = 0;
	if (add_links_data) {
		const struct wiphy_iftype_ext_capab *ift_ext_capa =
			cfg80211_get_iftype_ext_capa(local->hw.wiphy,
						     ieee80211_vif_type_p2p(&sdata->vif));

		if (ift_ext_capa) {
			eml_capa = cpu_to_le16(ift_ext_capa->eml_capabilities);
			mld_capa_ops =
				cpu_to_le16(ift_ext_capa->mld_capa_and_ops);
		}

		/* MLD capabilities and operation */
		var_common_size += 2;

		/* EML capabilities */
		if (eml_capa & cpu_to_le16((IEEE80211_EML_CAP_EMLSR_SUPP |
					    IEEE80211_EML_CAP_EMLMR_SUPPORT)))
			var_common_size += 2;
	}

	if (ext_mld_capa_ops)
		var_common_size += 2;

	/* Add the common information length */
	size += common_size + var_common_size;

	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		struct cfg80211_bss *cbss;
		size_t elems_len;

		if (removed_links & BIT(link_id)) {
			size += sizeof(struct ieee80211_mle_per_sta_profile) +
				ETH_ALEN;
			continue;
		}

		if (!add_links_data || !add_links_data->link[link_id].bss)
			continue;

		elems_len = add_links_data->link[link_id].elems_len;
		cbss = add_links_data->link[link_id].bss;

		/* should be the same across all BSSes */
		if (cbss->capability & WLAN_CAPABILITY_PRIVACY)
			capab |= WLAN_CAPABILITY_PRIVACY;

		size += 2 + sizeof(struct ieee80211_mle_per_sta_profile) +
			ETH_ALEN;

		/* WMM */
		size += 9;
		size += ieee80211_link_common_elems_size(sdata, iftype, cbss,
							 elems_len);
	}

	skb = alloc_skb(size, GFP_KERNEL);
	if (!skb)
		return NULL;

	skb_reserve(skb, local->hw.extra_tx_headroom);
	mgmt = skb_put_zero(skb, offsetofend(struct ieee80211_mgmt,
					     u.action.u.ml_reconf_req));

	/* Add the MAC header */
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	/* Add the action frame fixed fields */
	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	mgmt->u.action.u.ml_reconf_req.action_code =
		WLAN_PROTECTED_EHT_ACTION_LINK_RECONFIG_REQ;

	/* allocate a dialog token and store it */
	sdata->u.mgd.reconf.dialog_token = ++sdata->u.mgd.dialog_token_alloc;
	mgmt->u.action.u.ml_reconf_req.dialog_token =
		sdata->u.mgd.reconf.dialog_token;

	/* Add the ML reconfiguration element and the common information  */
	skb_put_u8(skb, WLAN_EID_EXTENSION);
	ml_elem_len = skb_put(skb, 1);
	skb_put_u8(skb, WLAN_EID_EXT_EHT_MULTI_LINK);
	ml_elem = skb_put(skb, sizeof(*ml_elem));
	ml_elem->control =
		cpu_to_le16(IEEE80211_ML_CONTROL_TYPE_RECONF |
			    IEEE80211_MLC_RECONF_PRES_MLD_MAC_ADDR);
	common = skb_put(skb, common_size);
	common->len = common_size + var_common_size;
	memcpy(common->mld_mac_addr, sdata->vif.addr, ETH_ALEN);

	if (add_links_data) {
		if (eml_capa &
		    cpu_to_le16((IEEE80211_EML_CAP_EMLSR_SUPP |
				 IEEE80211_EML_CAP_EMLMR_SUPPORT))) {
			ml_elem->control |=
				cpu_to_le16(IEEE80211_MLC_RECONF_PRES_EML_CAPA);
			skb_put_data(skb, &eml_capa, sizeof(eml_capa));
		}

		ml_elem->control |=
			cpu_to_le16(IEEE80211_MLC_RECONF_PRES_MLD_CAPA_OP);

		skb_put_data(skb, &mld_capa_ops, sizeof(mld_capa_ops));
	}

	if (ext_mld_capa_ops) {
		ml_elem->control |=
			cpu_to_le16(IEEE80211_MLC_RECONF_PRES_EXT_MLD_CAPA_OP);
		skb_put_data(skb, &ext_mld_capa_ops, sizeof(ext_mld_capa_ops));
	}

	if (sdata->u.mgd.flags & IEEE80211_STA_ENABLE_RRM)
		capab |= WLAN_CAPABILITY_RADIO_MEASURE;

	/* Add the per station profile */
	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		u8 *subelem_len = NULL;
		u16 ctrl;
		const u8 *addr;

		/* Skip links that are not changing */
		if (!(removed_links & BIT(link_id)) &&
		    (!add_links_data || !add_links_data->link[link_id].bss))
			continue;

		ctrl = link_id |
		       IEEE80211_MLE_STA_RECONF_CONTROL_STA_MAC_ADDR_PRESENT;

		if (removed_links & BIT(link_id)) {
			struct ieee80211_bss_conf *conf =
				sdata_dereference(sdata->vif.link_conf[link_id],
						  sdata);
			if (!conf)
				continue;

			addr = conf->addr;
			ctrl |= u16_encode_bits(IEEE80211_MLE_STA_RECONF_CONTROL_OPERATION_TYPE_DEL_LINK,
						IEEE80211_MLE_STA_RECONF_CONTROL_OPERATION_TYPE);
		} else {
			addr = add_links_data->link[link_id].addr;
			ctrl |= IEEE80211_MLE_STA_RECONF_CONTROL_COMPLETE_PROFILE |
				u16_encode_bits(IEEE80211_MLE_STA_RECONF_CONTROL_OPERATION_TYPE_ADD_LINK,
						IEEE80211_MLE_STA_RECONF_CONTROL_OPERATION_TYPE);
		}

		skb_put_u8(skb, IEEE80211_MLE_SUBELEM_PER_STA_PROFILE);
		subelem_len = skb_put(skb, 1);

		put_unaligned_le16(ctrl, skb_put(skb, sizeof(ctrl)));
		skb_put_u8(skb, 1 + ETH_ALEN);
		skb_put_data(skb, addr, ETH_ALEN);

		if (!(removed_links & BIT(link_id))) {
			u16 link_present_elems[PRESENT_ELEMS_MAX] = {};
			size_t extra_used;
			void *capab_pos;
			u8 qos_info;

			capab_pos = skb_put(skb, 2);

			extra_used =
				ieee80211_add_link_elems(sdata, skb, &capab, NULL,
							 add_links_data->link[link_id].elems,
							 add_links_data->link[link_id].elems_len,
							 link_id, NULL,
							 link_present_elems,
							 add_links_data);

			if (add_links_data->link[link_id].elems)
				skb_put_data(skb,
					     add_links_data->link[link_id].elems +
					     extra_used,
					     add_links_data->link[link_id].elems_len -
					     extra_used);
			if (sdata->u.mgd.flags & IEEE80211_STA_UAPSD_ENABLED) {
				qos_info = sdata->u.mgd.uapsd_queues;
				qos_info |= (sdata->u.mgd.uapsd_max_sp_len <<
					     IEEE80211_WMM_IE_STA_QOSINFO_SP_SHIFT);
			} else {
				qos_info = 0;
			}

			ieee80211_add_wmm_info_ie(skb_put(skb, 9), qos_info);
			put_unaligned_le16(capab, capab_pos);
		}

		ieee80211_fragment_element(skb, subelem_len,
					   IEEE80211_MLE_SUBELEM_FRAGMENT);
	}

	ieee80211_fragment_element(skb, ml_elem_len, WLAN_EID_FRAGMENT);

	info = IEEE80211_SKB_CB(skb);
	info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;

	return skb;
}

int ieee80211_mgd_assoc_ml_reconf(struct ieee80211_sub_if_data *sdata,
				  struct cfg80211_ml_reconf_req *req)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgd_assoc_data *data = NULL;
	struct sta_info *sta;
	struct sk_buff *skb;
	u16 added_links, new_valid_links;
	int link_id, err;

	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    !(sdata->vif.cfg.mld_capa_op &
	      IEEE80211_MLD_CAP_OP_LINK_RECONF_SUPPORT))
		return -EINVAL;

	/* No support for concurrent ML reconfiguration operation */
	if (sdata->u.mgd.reconf.added_links ||
	    sdata->u.mgd.reconf.removed_links)
		return -EBUSY;

	added_links = 0;
	for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS; link_id++) {
		if (!req->add_links[link_id].bss)
			continue;

		added_links |= BIT(link_id);
	}

	sta = sta_info_get(sdata, sdata->vif.cfg.ap_addr);
	if (WARN_ON(!sta))
		return -ENOLINK;

	/* Adding links to the set of valid link is done only after a successful
	 * ML reconfiguration frame exchange. Here prepare the data for the ML
	 * reconfiguration frame construction and allocate the required
	 * resources
	 */
	if (added_links) {
		bool uapsd_supported;

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		data->assoc_link_id = -1;
		data->wmm = true;

		uapsd_supported = true;
		for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS;
		     link_id++) {
			struct ieee80211_supported_band *sband;
			struct cfg80211_bss *link_cbss =
				req->add_links[link_id].bss;
			struct ieee80211_bss *bss;

			if (!link_cbss)
				continue;

			bss = (void *)link_cbss->priv;

			if (!bss->wmm_used) {
				err = -EINVAL;
				goto err_free;
			}

			if (link_cbss->channel->band == NL80211_BAND_S1GHZ) {
				err = -EINVAL;
				goto err_free;
			}

			eth_random_addr(data->link[link_id].addr);
			data->link[link_id].conn =
				ieee80211_conn_settings_unlimited;
			sband =
				local->hw.wiphy->bands[link_cbss->channel->band];

			ieee80211_determine_our_sta_mode(sdata, sband,
							 NULL, true, link_id,
							 &data->link[link_id].conn);

			data->link[link_id].bss = link_cbss;
			data->link[link_id].disabled =
				req->add_links[link_id].disabled;
			data->link[link_id].elems =
				(u8 *)req->add_links[link_id].elems;
			data->link[link_id].elems_len =
				req->add_links[link_id].elems_len;

			if (!bss->uapsd_supported)
				uapsd_supported = false;

			if (data->link[link_id].conn.mode <
			    IEEE80211_CONN_MODE_EHT) {
				err = -EINVAL;
				goto err_free;
			}

			err = ieee80211_mgd_get_ap_ht_vht_capa(sdata, data,
							       link_id);
			if (err) {
				err = -EINVAL;
				goto err_free;
			}
		}

		/* Require U-APSD support if we enabled it */
		if (sdata->u.mgd.flags & IEEE80211_STA_UAPSD_ENABLED &&
		    !uapsd_supported) {
			err = -EINVAL;
			sdata_info(sdata, "U-APSD on but not available on (all) new links\n");
			goto err_free;
		}

		for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS;
		     link_id++) {
			if (!data->link[link_id].bss)
				continue;

			/* only used to verify the mode, nothing is allocated */
			err = ieee80211_prep_channel(sdata, NULL, link_id,
						     data->link[link_id].bss,
						     true,
						     &data->link[link_id].conn,
						     sdata->u.mgd.userspace_selectors);
			if (err)
				goto err_free;
		}
	}

	/* link removal is done before the ML reconfiguration frame exchange so
	 * that these links will not be used between their removal by the AP MLD
	 * and before the station got the ML reconfiguration response. Based on
	 * Section 35.3.6.4 in Draft P802.11be_D7.0 the AP MLD should accept the
	 * link removal request.
	 */
	if (req->rem_links) {
		u16 new_active_links =
			sdata->vif.active_links & ~req->rem_links;

		new_valid_links = sdata->vif.valid_links & ~req->rem_links;

		/* Should not be left with no valid links to perform the
		 * ML reconfiguration
		 */
		if (!new_valid_links ||
		    !(new_valid_links & ~sdata->vif.dormant_links)) {
			sdata_info(sdata, "mlo: reconf: no valid links\n");
			err = -EINVAL;
			goto err_free;
		}

		if (new_active_links != sdata->vif.active_links) {
			if (!new_active_links)
				new_active_links =
					BIT(__ffs(new_valid_links &
						  ~sdata->vif.dormant_links));

			err = ieee80211_set_active_links(&sdata->vif,
							 new_active_links);
			if (err) {
				sdata_info(sdata,
					   "mlo: reconf: failed set active links\n");
				goto err_free;
			}
		}
	}

	/* Build the SKB before the link removal as the construction of the
	 * station info for removed links requires the local address.
	 * Invalidate the removed links, so that the transmission of the ML
	 * reconfiguration request frame would not be done using them, as the AP
	 * is expected to send the ML reconfiguration response frame on the link
	 * on which the request was received.
	 */
	skb = ieee80211_build_ml_reconf_req(sdata, data, req->rem_links,
					    cpu_to_le16(req->ext_mld_capa_ops));
	if (!skb) {
		err = -ENOMEM;
		goto err_free;
	}

	if (req->rem_links) {
		u16 new_dormant_links =
			sdata->vif.dormant_links & ~req->rem_links;

		err = ieee80211_vif_set_links(sdata, new_valid_links,
					      new_dormant_links);
		if (err) {
			sdata_info(sdata,
				   "mlo: reconf: failed set valid links\n");
			kfree_skb(skb);
			goto err_free;
		}

		for (link_id = 0; link_id < IEEE80211_MLD_MAX_NUM_LINKS;
		     link_id++) {
			if (!(req->rem_links & BIT(link_id)))
				continue;

			ieee80211_sta_remove_link(sta, link_id);
		}

		/* notify the driver and upper layers */
		ieee80211_vif_cfg_change_notify(sdata,
						BSS_CHANGED_MLD_VALID_LINKS);
		cfg80211_links_removed(sdata->dev, req->rem_links);
	}

	sdata_info(sdata, "mlo: reconf: adding=0x%x, removed=0x%x\n",
		   added_links, req->rem_links);

	ieee80211_tx_skb(sdata, skb);

	sdata->u.mgd.reconf.added_links = added_links;
	sdata->u.mgd.reconf.add_links_data = data;
	sdata->u.mgd.reconf.removed_links = req->rem_links;
	wiphy_delayed_work_queue(sdata->local->hw.wiphy,
				 &sdata->u.mgd.reconf.wk,
				 IEEE80211_ASSOC_TIMEOUT_SHORT);
	return 0;

 err_free:
	kfree(data);
	return err;
}

static bool ieee80211_mgd_epcs_supp(struct ieee80211_sub_if_data *sdata)
{
	unsigned long valid_links = sdata->vif.valid_links;
	u8 link_id;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!ieee80211_vif_is_mld(&sdata->vif))
		return false;

	for_each_set_bit(link_id, &valid_links, IEEE80211_MLD_MAX_NUM_LINKS) {
		struct ieee80211_bss_conf *bss_conf =
			sdata_dereference(sdata->vif.link_conf[link_id], sdata);

		if (WARN_ON(!bss_conf) || !bss_conf->epcs_support)
			return false;
	}

	return true;
}

int ieee80211_mgd_set_epcs(struct ieee80211_sub_if_data *sdata, bool enable)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	int frame_len = offsetofend(struct ieee80211_mgmt,
				    u.action.u.epcs) + (enable ? 1 : 0);

	if (!ieee80211_mgd_epcs_supp(sdata))
		return -EINVAL;

	if (sdata->u.mgd.epcs.enabled == enable &&
	    !sdata->u.mgd.epcs.dialog_token)
		return 0;

	/* Do not allow enabling EPCS if the AP didn't respond yet.
	 * However, allow disabling EPCS in such a case.
	 */
	if (sdata->u.mgd.epcs.dialog_token && enable)
		return -EALREADY;

	skb = dev_alloc_skb(local->hw.extra_tx_headroom + frame_len);
	if (!skb)
		return -ENOBUFS;

	skb_reserve(skb, local->hw.extra_tx_headroom);
	mgmt = skb_put_zero(skb, frame_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	memcpy(mgmt->da, sdata->vif.cfg.ap_addr, ETH_ALEN);
	memcpy(mgmt->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(mgmt->bssid, sdata->vif.cfg.ap_addr, ETH_ALEN);

	mgmt->u.action.category = WLAN_CATEGORY_PROTECTED_EHT;
	if (enable) {
		u8 *pos = mgmt->u.action.u.epcs.variable;

		mgmt->u.action.u.epcs.action_code =
			WLAN_PROTECTED_EHT_ACTION_EPCS_ENABLE_REQ;

		*pos = ++sdata->u.mgd.dialog_token_alloc;
		sdata->u.mgd.epcs.dialog_token = *pos;
	} else {
		mgmt->u.action.u.epcs.action_code =
			WLAN_PROTECTED_EHT_ACTION_EPCS_ENABLE_TEARDOWN;

		ieee80211_epcs_teardown(sdata);
		ieee80211_epcs_changed(sdata, false);
	}

	ieee80211_tx_skb(sdata, skb);
	return 0;
}

static void ieee80211_ml_epcs(struct ieee80211_sub_if_data *sdata,
			      struct ieee802_11_elems *elems)
{
	const struct element *sub;
	size_t scratch_len = elems->ml_epcs_len;
	u8 *scratch __free(kfree) = kzalloc(scratch_len, GFP_KERNEL);

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!ieee80211_vif_is_mld(&sdata->vif) || !elems->ml_epcs)
		return;

	if (WARN_ON(!scratch))
		return;

	/* Directly parse the sub elements as the common information doesn't
	 * hold any useful information.
	 */
	for_each_mle_subelement(sub, (const u8 *)elems->ml_epcs,
				elems->ml_epcs_len) {
		struct ieee80211_link_data *link;
		struct ieee802_11_elems *link_elems __free(kfree);
		u8 *pos = (void *)sub->data;
		u16 control;
		ssize_t len;
		u8 link_id;

		if (sub->id != IEEE80211_MLE_SUBELEM_PER_STA_PROFILE)
			continue;

		if (sub->datalen < sizeof(control))
			break;

		control = get_unaligned_le16(pos);
		link_id = control & IEEE80211_MLE_STA_EPCS_CONTROL_LINK_ID;

		link = sdata_dereference(sdata->link[link_id], sdata);
		if (!link)
			continue;

		len = cfg80211_defragment_element(sub, (u8 *)elems->ml_epcs,
						  elems->ml_epcs_len,
						  scratch, scratch_len,
						  IEEE80211_MLE_SUBELEM_FRAGMENT);
		if (len < (ssize_t)sizeof(control))
			continue;

		pos = scratch + sizeof(control);
		len -= sizeof(control);

		link_elems = ieee802_11_parse_elems(pos, len, false, NULL);
		if (!link_elems)
			continue;

		if (ieee80211_sta_wmm_params(sdata->local, link,
					     link_elems->wmm_param,
					     link_elems->wmm_param_len,
					     link_elems->mu_edca_param_set))
			ieee80211_link_info_change_notify(sdata, link,
							  BSS_CHANGED_QOS);
	}
}

void ieee80211_process_epcs_ena_resp(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee802_11_elems *elems __free(kfree) = NULL;
	size_t ies_len;
	u16 status_code;
	u8 *pos, dialog_token;

	if (!ieee80211_mgd_epcs_supp(sdata))
		return;

	/* Handle dialog token and status code */
	pos = mgmt->u.action.u.epcs.variable;
	dialog_token = *pos;
	status_code = get_unaligned_le16(pos + 1);

	/* An EPCS enable response with dialog token == 0 is an unsolicited
	 * notification from the AP MLD. In such a case, EPCS should already be
	 * enabled and status must be success
	 */
	if (!dialog_token &&
	    (!sdata->u.mgd.epcs.enabled ||
	     status_code != WLAN_STATUS_SUCCESS))
		return;

	if (sdata->u.mgd.epcs.dialog_token != dialog_token)
		return;

	sdata->u.mgd.epcs.dialog_token = 0;

	if (status_code != WLAN_STATUS_SUCCESS)
		return;

	pos += IEEE80211_EPCS_ENA_RESP_BODY_LEN;
	ies_len = len - offsetof(struct ieee80211_mgmt,
				 u.action.u.epcs.variable) -
		IEEE80211_EPCS_ENA_RESP_BODY_LEN;

	elems = ieee802_11_parse_elems(pos, ies_len, true, NULL);
	if (!elems)
		return;

	ieee80211_ml_epcs(sdata, elems);
	ieee80211_epcs_changed(sdata, true);
}

void ieee80211_process_epcs_teardown(struct ieee80211_sub_if_data *sdata,
				     struct ieee80211_mgmt *mgmt, size_t len)
{
	if (!ieee80211_vif_is_mld(&sdata->vif) ||
	    !sdata->u.mgd.epcs.enabled)
		return;

	ieee80211_epcs_teardown(sdata);
	ieee80211_epcs_changed(sdata, false);
}
