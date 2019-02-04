/*
 * (C) 2017-2018 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 * All Rights Reserved
 *
 * Author: Philipp Maier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <osmocom/core/msgb.h>
#include <osmocom/gsm/protocol/gsm_08_08.h>
#include <osmocom/gsm/gsm0808_utils.h>
#include <osmocom/netif/rtp.h>
#include <osmocom/mgcp_client/mgcp_client.h>
#include <osmocom/mgcp_client/mgcp_client_fsm.h>
#include <osmocom/bsc/bsc_msc_data.h>
#include <osmocom/bsc/codec_pref.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/bsc/lchan_fsm.h>

/* Determine whether a permitted speech value is specifies a half rate or full
 * rate codec */
static int full_rate_from_perm_spch(bool * full_rate,
				    enum gsm0808_permitted_speech perm_spch)
{
	/* Check if the result is a half or full rate codec */
	switch (perm_spch) {
	case GSM0808_PERM_HR1:
	case GSM0808_PERM_HR2:
	case GSM0808_PERM_HR3:
	case GSM0808_PERM_HR4:
	case GSM0808_PERM_HR6:
		*full_rate = false;
		break;

	case GSM0808_PERM_FR1:
	case GSM0808_PERM_FR2:
	case GSM0808_PERM_FR3:
	case GSM0808_PERM_FR4:
	case GSM0808_PERM_FR5:
		*full_rate = true;
		break;

	default:
		LOGP(DMSC, LOGL_ERROR, "Invalid permitted-speech value: %u\n",
		     perm_spch);
		return -EINVAL;
	}

	return 0;
}

/* Helper function for match_codec_pref(), looks up a matching chan mode for
 * a given permitted speech value */
static enum gsm48_chan_mode gsm88_to_chan_mode(enum gsm0808_permitted_speech speech)
{
	switch (speech) {
	case GSM0808_PERM_HR1:
	case GSM0808_PERM_FR1:
		return GSM48_CMODE_SPEECH_V1;
		break;
	case GSM0808_PERM_FR2:
		return GSM48_CMODE_SPEECH_EFR;
		break;
	case GSM0808_PERM_HR3:
	case GSM0808_PERM_FR3:
		return GSM48_CMODE_SPEECH_AMR;
		break;
	case GSM0808_PERM_HR2:
		LOGP(DMSC, LOGL_FATAL, "Speech HR2 was never specified!\n");
		/* fall through */
	default:
		LOGP(DMSC, LOGL_FATAL, "Unsupported permitted speech %s selected, "
		     "assuming AMR as channel mode...\n",
		     gsm0808_permitted_speech_name(speech));
		return GSM48_CMODE_SPEECH_AMR;
	}
}

/* Helper function for match_codec_pref(), looks up a matching permitted speech
 * value for a given msc audio codec pref */
static enum gsm0808_permitted_speech audio_support_to_gsm88(const struct gsm_audio_support *audio)
{
	if (audio->hr) {
		switch (audio->ver) {
		case 1:
			return GSM0808_PERM_HR1;
			break;
		case 2:
			return GSM0808_PERM_HR2;
			break;
		case 3:
			return GSM0808_PERM_HR3;
			break;
		default:
			LOGP(DMSC, LOGL_ERROR, "Wrong speech mode: hr%d, using hr1 instead\n", audio->ver);
			return GSM0808_PERM_HR1;
		}
	} else {
		switch (audio->ver) {
		case 1:
			return GSM0808_PERM_FR1;
			break;
		case 2:
			return GSM0808_PERM_FR2;
			break;
		case 3:
			return GSM0808_PERM_FR3;
			break;
		default:
			LOGP(DMSC, LOGL_ERROR, "Wrong speech mode: fr%d, using fr1 instead\n", audio->ver);
			return GSM0808_PERM_FR1;
		}
	}
}

/* Helper function for match_codec_pref(), tests if a given audio support
 * matches one of the permitted speech settings of the channel type element.
 * The matched permitted speech value is then also compared against the
 * speech codec list. (optional, only relevant for AoIP) */
static bool test_codec_pref(const struct gsm0808_speech_codec **sc_match,
			    const struct gsm0808_speech_codec_list *scl,
			    const struct gsm0808_channel_type *ct,
			    uint8_t perm_spch)
{
	unsigned int i;
	bool match = false;
	struct gsm0808_speech_codec sc;
	int rc;

	*sc_match = NULL;

	/* Try to find the given permitted speech value in the
	 * codec list of the channel type element */
	for (i = 0; i < ct->perm_spch_len; i++) {
		if (ct->perm_spch[i] == perm_spch) {
			match = true;
			break;
		}
	}

	/* If we do not have a speech codec list to test against,
	 * we just exit early (will be always the case in non-AoIP networks) */
	if (!scl || !scl->len)
		return match;

	/* If we failed to match until here, there is no
	 * point in testing further */
	if (match == false)
		return false;

	/* Extrapolate speech codec data */
	rc = gsm0808_speech_codec_from_chan_type(&sc, perm_spch);
	if (rc < 0)
		return false;

	/* Try to find extrapolated speech codec data in
	 * the speech codec list */
	for (i = 0; i < scl->len; i++) {
		if (sc.type == scl->codec[i].type) {
			*sc_match = &scl->codec[i];
			return true;
		}
	}

	return false;
}

/* Helper function to check if the given permitted speech value is supported
 * by the BTS. (vty option bts->codec-support). */
static bool test_codec_support_bts(const struct gsm_bts *bts, uint8_t perm_spch)
{
	struct gsm_bts_trx *trx;
	const struct bts_codec_conf *bts_codec = &bts->codec;
	unsigned int i;
	bool full_rate;
	int rc;
	enum gsm_phys_chan_config pchan;
	bool rate_match = false;

	/* Check if the BTS provides a physical channel that matches the
	 * bandwith of the desired codec. */
	rc = full_rate_from_perm_spch(&full_rate, perm_spch);
	if (rc < 0)
		return false;
	llist_for_each_entry(trx, &bts->trx_list, list) {
		for (i = 0; i < TRX_NR_TS; i++) {
			pchan = trx->ts[i].pchan_from_config;
			if (pchan == GSM_PCHAN_TCH_F_TCH_H_PDCH)
				rate_match = true;
			else if (full_rate && pchan == GSM_PCHAN_TCH_F)
				rate_match = true;
			else if (full_rate && pchan == GSM_PCHAN_TCH_F_PDCH)
				rate_match = true;
			else if (!full_rate && pchan == GSM_PCHAN_TCH_H)
				rate_match = true;
		}
	}
	if (!rate_match)
		return false;

	/* Check codec support */
	switch (perm_spch) {
	case GSM0808_PERM_FR1:
		/* GSM-FR is always supported by all BTSs. There is also no way to
		 * selectively disable GSM-RF per BTS via VTY. */
		return true;
	case GSM0808_PERM_FR2:
		if (bts_codec->efr)
			return true;
		break;
	case GSM0808_PERM_FR3:
		if (bts_codec->amr)
			return true;
		break;
	case GSM0808_PERM_HR1:
		if (bts_codec->hr)
			return true;
		break;
	case GSM0808_PERM_HR3:
		if (bts_codec->amr)
			return true;
		break;
	default:
		return false;
	}

	return false;
}

/* Generate the bss supported amr configuration bits (S0-S15) */
static uint16_t gen_bss_supported_amr_s15_s0(const struct bsc_msc_data *msc, const struct gsm_bts *bts, bool hr)
{
	const struct gsm48_multi_rate_conf *amr_cfg_bts;
	const struct gsm48_multi_rate_conf *amr_cfg_msc;
	uint16_t amr_s15_s0_bts;
	uint16_t amr_s15_s0_msc;

	/* Lookup the BTS specific AMR rate configuration. This config is set
	 * via the VTY for each BTS individually. In cases where no configuration
	 * is set we will assume a safe default */
	if (hr) {
		amr_cfg_bts = (struct gsm48_multi_rate_conf *)&bts->mr_half.gsm48_ie;
		amr_s15_s0_bts = gsm0808_sc_cfg_from_gsm48_mr_cfg(amr_cfg_bts, false);
	} else {
		amr_cfg_bts = (struct gsm48_multi_rate_conf *)&bts->mr_full.gsm48_ie;
		amr_s15_s0_bts = gsm0808_sc_cfg_from_gsm48_mr_cfg(amr_cfg_bts, true);
	}

	/* Lookup the AMR rate configuration that is set for the MSC */
	amr_cfg_msc = &msc->amr_conf;
	amr_s15_s0_msc = gsm0808_sc_cfg_from_gsm48_mr_cfg(amr_cfg_msc, true);

	/* Calculate the intersection of the two configurations and update S0-S15
	 * in the codec list. */
	return amr_s15_s0_bts & amr_s15_s0_msc;
}

/*! Match the codec preferences from local config with a received codec preferences IEs received from the
 * MSC and the BTS' codec configuration.
 *  \param[out] chan_mode GSM 04.08 channel mode.
 *  \param[out] full_rate true if full-rate.
 *  \param[out] s15_s0 codec configuration bits S15-S0 (AMR)
 *  \param[in] ct GSM 08.08 channel type received from MSC.
 *  \param[in] scl GSM 08.08 speech codec list received from MSC (optional).
 *  \param[in] msc associated msc (current codec settings).
 *  \param[in] bts associated bts (current codec settings).
 *  \returns 0 on success, -1 in case no match was found */
int match_codec_pref(enum gsm48_chan_mode *chan_mode,
		     bool *full_rate,
		     uint16_t *s15_s0,
		     const struct gsm0808_channel_type *ct,
		     const struct gsm0808_speech_codec_list *scl,
		     const struct bsc_msc_data *msc,
		     const struct gsm_bts *bts)
{
	unsigned int i;
	uint8_t perm_spch;
	bool match = false;
	const struct gsm0808_speech_codec *sc_match = NULL;
	uint16_t amr_s15_s0_supported;
	int rc;

	/* Note: Normally the MSC should never try to advertise a codec that
	 * we did not advertise as supported before. In order to ensure that
	 * no unsupported codec is accepted, we make sure that the codec is
	 * indeed available with the current BTS and MSC configuration */
	for (i = 0; i < msc->audio_length; i++) {
		/* Pick a permitted speech value from the global codec configuration list */
		perm_spch = audio_support_to_gsm88(msc->audio_support[i]);

		/* Check this permitted speech value against the BTS specific parameters.
		 * if the BTS does not support the codec, try the next one */
		if (!test_codec_support_bts(bts, perm_spch))
			continue;

		/* Match the permitted speech value against the codec lists that were
		 * advertised by the MS and the MSC */
		if (test_codec_pref(&sc_match, scl, ct, perm_spch)) {
			match = true;
			break;
		}
	}

	/* Exit without result, in case no match can be deteched */
	if (!match) {
		*full_rate = false;
		*chan_mode = GSM48_CMODE_SIGN;
		*s15_s0 = 0;
		return -1;
	}

	/* Determine if the result is a half or full rate codec */
	rc = full_rate_from_perm_spch(full_rate, perm_spch);
	if (rc < 0)
		return -EINVAL;

	/* Lookup a channel mode for the selected codec */
	*chan_mode = gsm88_to_chan_mode(perm_spch);

	/* Special handling for AMR */
	if (perm_spch == GSM0808_PERM_HR3 || perm_spch == GSM0808_PERM_FR3) {
		/* Normally the MSC should never try to advertise an AMR codec
		 * configuration that we did not previously advertise as
		 * supported. However, to ensure that no unsupported AMR codec
		 * configuration enters the further processing steps we again
		 * lookup what we support and generate an intersection. All
		 * further processing is then done with this intersection
		 * result */
		amr_s15_s0_supported = gen_bss_supported_amr_s15_s0(msc, bts, (perm_spch == GSM0808_PERM_HR3));
		if (sc_match)
			*s15_s0 = sc_match->cfg & amr_s15_s0_supported;
		else
			*s15_s0 = amr_s15_s0_supported;

		/* NOTE: The function test_codec_pref() will populate the
		 * sc_match pointer from the searched speech codec list. For
		 * AoIP based networks, no speech codec list will be present
		 * and therefore no sc_match will be populated. For those
		 * cases only the local configuration will influence s15_s0.
		 * However s15_s0 is always populated with a meaningful value,
		 * regardless if AoIP is in use or not. */
	} else
		*s15_s0 = 0;

	return 0;
}

/*! Determine the BSS supported speech codec list that is sent to the MSC with
 * the COMPLETE LAYER 3 INFORMATION message.
 *  \param[out] scl GSM 08.08 speech codec list with BSS supported codecs.
 *  \param[in] msc associated msc (current codec settings).
 *  \param[in] bts associated bts (current codec settings). */
void gen_bss_supported_codec_list(struct gsm0808_speech_codec_list *scl,
				  const struct bsc_msc_data *msc, const struct gsm_bts *bts)
{
	uint8_t perm_spch;
	unsigned int i;
	int rc;

	memset(scl, 0, sizeof(*scl));

	for (i = 0; i < msc->audio_length; i++) {

		/* Pick a permitted speech value from the global codec configuration list */
		perm_spch = audio_support_to_gsm88(msc->audio_support[i]);

		/* Check this permitted speech value against the BTS specific parameters.
		 * if the BTS does not support the codec, try the next one */
		if (!test_codec_support_bts(bts, perm_spch))
			continue;

		/* Write item into codec list */
		rc = gsm0808_speech_codec_from_chan_type(&scl->codec[scl->len], perm_spch);
		if (rc != 0)
			continue;

		/* AMR (HR/FR version 3) is the only codec that requires a codec
		 * configuration (S0-S15). Determine the current configuration and update
		 * the cfg flag. */
		if (msc->audio_support[i]->ver == 3)
			scl->codec[scl->len].cfg = gen_bss_supported_amr_s15_s0(msc, bts, msc->audio_support[i]->hr);

		scl->len++;
	}
}

/*! Calculate the intersection of the rate configuration of two multirate configuration
 *  IE structures. The result c will be a copy of a, but the rate configuration bits
 *  will be the intersection of the rate configuration bits in a and b.
 *  \param[out] c user provided memory to store the result.
 *  \param[in] a multi rate configuration a.
 *  \param[in] b multi rate configuration b.
 *  \returns 0 on success, -1 when the result contains an empty set of modes. */
int calc_amr_rate_intersection(struct gsm48_multi_rate_conf *c,
			       const struct gsm48_multi_rate_conf *b,
			       const struct gsm48_multi_rate_conf *a)
{
	struct gsm48_multi_rate_conf res;
	uint8_t *_a = (uint8_t *) a;
	uint8_t *_b = (uint8_t *) b;
	uint8_t *_res = (uint8_t *) & res;

	memcpy(&res, a, sizeof(res));

	_res[1] = _a[1] & _b[1];

	if (_res[1] == 0x00)
		return -1;

	if (c)
		memcpy(c, &res, sizeof(*c));

	return 0;
}

/*! Visit the codec settings for the MSC and for each BTS in order to make sure
 *  that the configuration does not contain any combinations that lead into a
 *  mutually exclusive codec configuration (empty intersection).
 *  \param[in] mscs list head of the msc list.
 *  \returns 0 on success, -1 in case an invalid setting is found. */
int check_codec_pref(struct llist_head *mscs)
{
	struct bsc_msc_data *msc;
	struct gsm_bts *bts;
	struct gsm0808_speech_codec_list scl;
	int rc = 0;
	int rc_rate;
	const struct gsm48_multi_rate_conf *bts_gsm48_ie;

	llist_for_each_entry(msc, mscs, entry) {
		llist_for_each_entry(bts, &msc->network->bts_list, list) {
			gen_bss_supported_codec_list(&scl, msc, bts);
			if (scl.len <= 0) {
				LOGP(DMSC, LOGL_FATAL,
				     "codec-support/trx config of BTS %u does not intersect with codec-list of MSC %u\n",
				     bts->nr, msc->nr);
				rc = -1;
			}

			bts_gsm48_ie = (struct gsm48_multi_rate_conf *)&bts->mr_full.gsm48_ie;
			rc_rate = calc_amr_rate_intersection(NULL, &msc->amr_conf, bts_gsm48_ie);
			if (rc_rate < 0) {
				LOGP(DMSC, LOGL_FATAL,
				     "network amr tch-f mode config of BTS %u does not intersect with amr-config of MSC %u\n",
				     bts->nr, msc->nr);
				rc = -1;
			}

			bts_gsm48_ie = (struct gsm48_multi_rate_conf *)&bts->mr_half.gsm48_ie;
			rc_rate = calc_amr_rate_intersection(NULL, &msc->amr_conf, bts_gsm48_ie);
			if (rc_rate < 0) {
				LOGP(DMSC, LOGL_FATAL,
				     "network amr tch-h mode config of BTS %u does not intersect with amr-config of MSC %u\n",
				     bts->nr, msc->nr);
				rc = -1;
			}
		}
	}

	return rc;
}

/* Depending on the channel mode and rate, return the codec type that is signalled towards the MGW. */
enum mgcp_codecs chan_mode_to_mgcp_codec(enum gsm48_chan_mode chan_mode, bool full_rate)
{
	switch (chan_mode) {
	case GSM48_CMODE_SPEECH_V1:
		if (full_rate)
			return CODEC_GSM_8000_1;
		return CODEC_GSMHR_8000_1;

	case GSM48_CMODE_SPEECH_EFR:
		return CODEC_GSMEFR_8000_1;

	case GSM48_CMODE_SPEECH_AMR:
		return CODEC_AMR_8000_1;

	default:
		return -1;
	}
}

static int mgcp_codec_to_bss_pt(enum mgcp_codecs codec)
{
	switch (codec) {
	case CODEC_GSMHR_8000_1:
		return RTP_PT_GSM_HALF;

	case CODEC_GSMEFR_8000_1:
		return RTP_PT_GSM_EFR;

	case CODEC_AMR_8000_1:
		return RTP_PT_AMR;

	default:
		/* Not an error, we just leave it to libosmo-mgcp-client to
		 * decide over the PT. */
		return -1;
	}
}

void mgcp_pick_codec(struct mgcp_conn_peer *verb_info, const struct gsm_lchan *lchan, bool bss_side)
{
	enum mgcp_codecs codec = chan_mode_to_mgcp_codec(lchan->tch_mode,
							 lchan->type == GSM_LCHAN_TCH_H? false : true);
	int custom_pt;

	if (codec < 0) {
		LOG_LCHAN(lchan, LOGL_ERROR,
			  "Unable to determine MGCP codec type for %s in chan-mode %s\n",
			  gsm_lchant_name(lchan->type), gsm48_chan_mode_name(lchan->tch_mode));
		verb_info->codecs_len = 0;
		return;
	}

	verb_info->codecs[0] = codec;
	verb_info->codecs_len = 1;

	/* Setup custom payload types (only for BSS side and when required) */
	custom_pt = mgcp_codec_to_bss_pt(codec);
	if (bss_side && custom_pt > 0) {
		verb_info->ptmap[0].codec = codec;
	        verb_info->ptmap[0].pt = custom_pt;
	        verb_info->ptmap_len = 1;
	}
}
