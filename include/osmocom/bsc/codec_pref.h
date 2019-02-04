#pragma once

#include <stdbool.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>

struct gsm0808_channel_type;
struct gsm0808_speech_codec_list;
struct gsm_audio_support;
struct bts_codec_conf;
struct bsc_msc_data;
struct gsm_bts;
struct mgcp_conn_peer;

int match_codec_pref(enum gsm48_chan_mode *chan_mode,
		     bool *full_rate,
		     uint16_t *s15_s0,
		     const struct gsm0808_channel_type *ct,
		     const struct gsm0808_speech_codec_list *scl,
		     const struct bsc_msc_data *msc,
		     const struct gsm_bts *bts);

void gen_bss_supported_codec_list(struct gsm0808_speech_codec_list *scl,
				  const struct bsc_msc_data *msc,
				  const struct gsm_bts *bts);

int calc_amr_rate_intersection(struct gsm48_multi_rate_conf *c,
			       const struct gsm48_multi_rate_conf *b,
			       const struct gsm48_multi_rate_conf *a);

int check_codec_pref(struct llist_head *mscs);

enum mgcp_codecs chan_mode_to_mgcp_codec(enum gsm48_chan_mode chan_mode, bool full_rate);
void mgcp_pick_codec(struct mgcp_conn_peer *verb_info, const struct gsm_lchan *lchan, bool bss_side);
