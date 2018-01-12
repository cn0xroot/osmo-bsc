/* Handover Logic for Inter-BTS (Intra-BSC) Handover.  This does not
 * actually implement the handover algorithm/decision, but executes a
 * handover decision */

/* (C) 2009 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>

#include <osmocom/core/msgb.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/bsc/abis_rsl.h>
#include <osmocom/bsc/chan_alloc.h>
#include <osmocom/bsc/signal.h>
#include <osmocom/core/talloc.h>
#include <osmocom/bsc/bsc_subscriber.h>
#include <osmocom/bsc/gsm_04_08_utils.h>
#include <osmocom/bsc/handover.h>
#include <osmocom/bsc/handover_cfg.h>

#define LOGPHOLCHANTOLCHAN(lchan, new_lchan, level, fmt, args...) \
	LOGP(DHODEC, level, "(BTS %u trx %u arfcn %u ts %u lchan %u %s)->(BTS %u trx %u arfcn %u ts %u lchan %u %s) (subscr %s) " fmt, \
	     lchan->ts->trx->bts->nr, \
	     lchan->ts->trx->nr, \
	     lchan->ts->trx->arfcn, \
	     lchan->ts->nr, \
	     lchan->nr, \
	     gsm_pchan_name(lchan->ts->pchan), \
	     new_lchan->ts->trx->bts->nr, \
	     new_lchan->ts->trx->nr, \
	     new_lchan->ts->trx->arfcn, \
	     new_lchan->ts->nr, \
	     new_lchan->nr, \
	     gsm_pchan_name(new_lchan->ts->pchan), \
	     bsc_subscr_name(lchan->conn->bsub), \
	     ## args)

#define LOGPHO(struct_bsc_handover, level, fmt, args ...) \
	LOGPHOLCHANTOLCHAN(struct_bsc_handover->old_lchan, struct_bsc_handover->new_lchan, level, fmt, ## args)


struct bsc_handover {
	struct llist_head list;

	struct gsm_lchan *old_lchan;
	struct gsm_lchan *new_lchan;

	struct osmo_timer_list T3103;

	uint8_t ho_ref;

	bool inter_cell;
	bool async;
};

static LLIST_HEAD(bsc_handovers);

static void handover_free(struct bsc_handover *ho)
{
	osmo_timer_del(&ho->T3103);
	llist_del(&ho->list);
	talloc_free(ho);
}

static struct bsc_handover *bsc_ho_by_new_lchan(struct gsm_lchan *new_lchan)
{
	struct bsc_handover *ho;

	llist_for_each_entry(ho, &bsc_handovers, list) {
		if (ho->new_lchan == new_lchan)
			return ho;
	}

	return NULL;
}

static struct bsc_handover *bsc_ho_by_old_lchan(struct gsm_lchan *old_lchan)
{
	struct bsc_handover *ho;

	llist_for_each_entry(ho, &bsc_handovers, list) {
		if (ho->old_lchan == old_lchan)
			return ho;
	}

	return NULL;
}

/*! Hand over the specified logical channel to the specified new BTS. */
int bsc_handover_start(struct gsm_lchan *old_lchan, struct gsm_bts *bts)
{
	return bsc_handover_start_lchan_change(old_lchan, bts, old_lchan->type);
}

/*! Hand over the specified logical channel to the specified new BTS and possibly change the lchan type.
 * This is the main entry point for the actual handover algorithm, after the decision whether to initiate
 * HO to a specific BTS. */
int bsc_handover_start_lchan_change(struct gsm_lchan *old_lchan, struct gsm_bts *new_bts,
				    enum gsm_chan_t new_lchan_type)
{
	struct gsm_network *network;
	struct gsm_lchan *new_lchan;
	struct bsc_handover *ho;
	static uint8_t ho_ref = 0;
	int rc;
	bool do_assignment = false;

	/* don't attempt multiple handovers for the same lchan at
	 * the same time */
	if (bsc_ho_by_old_lchan(old_lchan))
		return -EBUSY;

	if (!new_bts)
		new_bts = old_lchan->ts->trx->bts;
	do_assignment = (new_bts == old_lchan->ts->trx->bts);

	network = new_bts->network;

	rate_ctr_inc(&network->bsc_ctrs->ctr[BSC_CTR_HANDOVER_ATTEMPTED]);

	if (!old_lchan->conn) {
		LOGP(DHO, LOGL_ERROR, "Old lchan lacks connection data.\n");
		return -ENOSPC;
	}

	DEBUGP(DHO, "(BTS %u trx %u ts %u lchan %u %s)->(BTS %u lchan %s) Beginning with handover operation...\n",
	       old_lchan->ts->trx->bts->nr,
	       old_lchan->ts->trx->nr,
	       old_lchan->ts->nr,
	       old_lchan->nr,
	       gsm_pchan_name(old_lchan->ts->pchan),
	       new_bts->nr,
	       gsm_lchant_name(new_lchan_type));

	new_lchan = lchan_alloc(new_bts, new_lchan_type, 0);
	if (!new_lchan) {
		LOGP(DHO, LOGL_NOTICE, "No free channel for %s\n", gsm_lchant_name(new_lchan_type));
		rate_ctr_inc(&network->bsc_ctrs->ctr[BSC_CTR_HANDOVER_NO_CHANNEL]);
		return -ENOSPC;
	}

	ho = talloc_zero(tall_bsc_ctx, struct bsc_handover);
	if (!ho) {
		LOGP(DHO, LOGL_FATAL, "Out of Memory\n");
		lchan_free(new_lchan);
		return -ENOMEM;
	}
	ho->old_lchan = old_lchan;
	ho->new_lchan = new_lchan;
	ho->ho_ref = ho_ref++;
	ho->inter_cell = !do_assignment;
	ho->async = true;

	LOGPHO(ho, LOGL_INFO, "Triggering %s\n", do_assignment? "Assignment" : "Handover");

	/* copy some parameters from old lchan */
	memcpy(&new_lchan->encr, &old_lchan->encr, sizeof(new_lchan->encr));
	if (do_assignment) {
		new_lchan->ms_power = old_lchan->ms_power;
		new_lchan->rqd_ta = old_lchan->rqd_ta;
	} else {
		new_lchan->ms_power = 
			ms_pwr_ctl_lvl(new_bts->band, new_bts->ms_max_power);
		/* FIXME: do we have a better idea of the timing advance? */
		//new_lchan->rqd_ta = old_lchan->rqd_ta;
	}
	new_lchan->bs_power = old_lchan->bs_power;
	new_lchan->rsl_cmode = old_lchan->rsl_cmode;
	new_lchan->tch_mode = old_lchan->tch_mode;
	memcpy(&new_lchan->mr_ms_lv, &old_lchan->mr_ms_lv, sizeof(new_lchan->mr_ms_lv));
	memcpy(&new_lchan->mr_bts_lv, &old_lchan->mr_bts_lv, sizeof(new_lchan->mr_bts_lv));

	new_lchan->conn = old_lchan->conn;
	new_lchan->conn->ho_lchan = new_lchan;

	rc = rsl_chan_activate_lchan(new_lchan,
				     ho->async ? RSL_ACT_INTER_ASYNC : RSL_ACT_INTER_SYNC,
				     ho->ho_ref);
	if (rc < 0) {
		LOGPHO(ho, LOGL_INFO, "%s Failure: activate lchan rc = %d\n",
		       do_assignment? "Assignment" : "Handover", rc);
		new_lchan->conn->ho_lchan = NULL;
		new_lchan->conn = NULL;
		talloc_free(ho);
		lchan_free(new_lchan);
		return rc;
	}

	rsl_lchan_set_state(new_lchan, LCHAN_S_ACT_REQ);
	llist_add(&ho->list, &bsc_handovers);
	/* we continue in the SS_LCHAN handler / ho_chan_activ_ack */

	return 0;
}

/* clear any operation for this connection */
void bsc_clear_handover(struct gsm_subscriber_connection *conn, int free_lchan)
{
	struct bsc_handover *ho;

	ho = bsc_ho_by_new_lchan(conn->ho_lchan);


	if (!ho && conn->ho_lchan)
		LOGP(DHO, LOGL_ERROR, "BUG: We lost some state.\n");

	if (!ho) {
		LOGP(DHO, LOGL_ERROR, "unable to find HO record\n");
		return;
	}

	conn->ho_lchan->conn = NULL;
	conn->ho_lchan = NULL;

	if (free_lchan)
		lchan_release(ho->new_lchan, 0, RSL_REL_LOCAL_END);

	handover_free(ho);
}

/* T3103 expired: Handover has failed without HO COMPLETE or HO FAIL */
static void ho_T3103_cb(void *_ho)
{
	struct bsc_handover *ho = _ho;
	struct gsm_network *net = ho->new_lchan->ts->trx->bts->network;

	DEBUGP(DHO, "HO T3103 expired\n");
	rate_ctr_inc(&net->bsc_ctrs->ctr[BSC_CTR_HANDOVER_TIMEOUT]);

	ho->new_lchan->conn->ho_lchan = NULL;
	ho->new_lchan->conn = NULL;
	lchan_release(ho->new_lchan, 0, RSL_REL_LOCAL_END);
	handover_free(ho);
}

/* RSL has acknowledged activation of the new lchan */
static int ho_chan_activ_ack(struct gsm_lchan *new_lchan)
{
	struct bsc_handover *ho;

	/* we need to check if this channel activation is related to
	 * a handover at all (and if, which particular handover) */
	ho = bsc_ho_by_new_lchan(new_lchan);
	if (!ho)
		return -ENODEV;

	LOGPHO(ho, LOGL_INFO, "Channel Activate Ack, send %s COMMAND\n", ho->inter_cell? "HANDOVER" : "ASSIGNMENT");

	/* we can now send the 04.08 HANDOVER COMMAND to the MS
	 * using the old lchan */

	gsm48_send_ho_cmd(ho->old_lchan, new_lchan, new_lchan->ms_power, ho->ho_ref);

	/* start T3103.  We can continue either with T3103 expiration,
	 * 04.08 HANDOVER COMPLETE or 04.08 HANDOVER FAIL */
	osmo_timer_setup(&ho->T3103, ho_T3103_cb, ho);
	osmo_timer_schedule(&ho->T3103, 10, 0);

	/* create a RTP connection */
	if (is_ipaccess_bts(new_lchan->ts->trx->bts))
		rsl_ipacc_crcx(new_lchan);

	return 0;
}

/* RSL has not acknowledged activation of the new lchan */
static int ho_chan_activ_nack(struct gsm_lchan *new_lchan)
{
	struct bsc_handover *ho;
	struct gsm_bts *new_bts = new_lchan->ts->trx->bts;

	ho = bsc_ho_by_new_lchan(new_lchan);
	if (!ho) {
		LOGP(DHO, LOGL_INFO, "ACT NACK: unable to find HO record\n");
		return -ENODEV;
	}

	LOGPHO(ho, LOGL_ERROR, "Channel Activate Nack for %s, starting penalty timer\n", ho->inter_cell? "Handover" : "Assignment");

	/* if channel failed, wait 10 seconds befor allowing to retry handover */
	conn_penalty_timer_add(ho->old_lchan->conn, new_bts, 10); /* FIXME configurable */

	new_lchan->conn->ho_lchan = NULL;
	new_lchan->conn = NULL;
	handover_free(ho);

	/* FIXME: maybe we should try to allocate a new LCHAN here? */

	return 0;
}

/* GSM 04.08 HANDOVER COMPLETE has been received on new channel */
static int ho_gsm48_ho_compl(struct gsm_lchan *new_lchan)
{
	struct gsm_network *net;
	struct bsc_handover *ho;

	ho = bsc_ho_by_new_lchan(new_lchan);
	if (!ho) {
		LOGP(DHO, LOGL_ERROR, "unable to find HO record\n");
		return -ENODEV;
	}

	LOGPHO(ho, LOGL_INFO, "%s Complete\n", ho->inter_cell ? "Handover" : "Assignment");

	net = new_lchan->ts->trx->bts->network;
	rate_ctr_inc(&net->bsc_ctrs->ctr[BSC_CTR_HANDOVER_COMPLETED]);

	osmo_timer_del(&ho->T3103);

	/* Replace the ho lchan with the primary one */
	if (ho->old_lchan != new_lchan->conn->lchan)
		LOGPHO(ho, LOGL_ERROR, "Primary lchan changed during handover.\n");

	if (new_lchan != new_lchan->conn->ho_lchan)
		LOGPHO(ho, LOGL_ERROR, "Handover channel changed during this handover.\n");

	new_lchan->conn->ho_lchan = NULL;
	new_lchan->conn->lchan = new_lchan;
	new_lchan->conn->bts = new_lchan->ts->trx->bts;
	ho->old_lchan->conn = NULL;

	lchan_release(ho->old_lchan, 0, RSL_REL_LOCAL_END);

	handover_free(ho);
	return 0;
}

/* GSM 04.08 HANDOVER FAIL has been received */
static int ho_gsm48_ho_fail(struct gsm_lchan *old_lchan)
{
	struct gsm_network *net = old_lchan->ts->trx->bts->network;
	struct gsm_bts *old_bts;
	struct gsm_bts *new_bts;
	struct bsc_handover *ho;
	struct gsm_lchan *new_lchan;

	ho = bsc_ho_by_old_lchan(old_lchan);
	if (!ho) {
		LOGP(DHO, LOGL_ERROR, "unable to find HO record\n");
		return -ENODEV;
	}

	old_bts = old_lchan->ts->trx->bts;
	new_bts = ho->new_lchan->ts->trx->bts;

	if (old_lchan->conn->ho_failure >= ho_get_retries(old_bts->ho.cfg)) {
		int penalty = ho->inter_cell
			? ho_get_penalty_failed_ho(old_bts->ho.cfg)
			: ho_get_penalty_failed_as(old_bts->ho.cfg);
		LOGPHO(ho, LOGL_NOTICE, "%s failed, starting penalty timer (%d)\n",
		       ho->inter_cell ? "Handover" : "Assignment",
		       penalty);
		old_lchan->conn->ho_failure = 0;
		conn_penalty_timer_add(old_lchan->conn, new_bts, penalty);
	} else {
		old_lchan->conn->ho_failure++;
		LOGPHO(ho, LOGL_NOTICE, "%s failed, trying again (%d/%d)\n",
		       ho->inter_cell ? "Handover" : "Assignment",
		       old_lchan->conn->ho_failure, ho_get_retries(old_bts->ho.cfg));
	}

	rate_ctr_inc(&net->bsc_ctrs->ctr[BSC_CTR_HANDOVER_FAILED]);

	new_lchan = ho->new_lchan;

	/* release the channel and forget about it */
	ho->new_lchan->conn->ho_lchan = NULL;
	ho->new_lchan->conn = NULL;
	handover_free(ho);

	lchan_release(new_lchan, 0, RSL_REL_LOCAL_END);

	return 0;
}

/* GSM 08.58 HANDOVER DETECT has been received */
static int ho_rsl_detect(struct gsm_lchan *new_lchan)
{
	struct bsc_handover *ho;

	ho = bsc_ho_by_new_lchan(new_lchan);
	if (!ho) {
		LOGP(DHO, LOGL_ERROR, "unable to find HO record\n");
		return -ENODEV;
	}

	LOGPHO(ho, LOGL_DEBUG, "Handover RACH detected\n");

	/* FIXME: do we actually want to do something here ? */
	//rsl_ipacc_mdcx(new_lchan,
	//	       old_lchan->abis_ip.connect_ip,
	//	       old_lchan->abis_ip.connect_port, 0);
	LOGP(DHO, LOGL_DEBUG, "? rsl_ipacc_mdcx(%s -> %x:%u)\n",
	     gsm_lchan_name(new_lchan), 
	     ho->old_lchan->abis_ip.connect_ip,
	     ho->old_lchan->abis_ip.connect_port);

	return 0;
}

static int ho_logic_sig_cb(unsigned int subsys, unsigned int signal,
			   void *handler_data, void *signal_data)
{
	struct lchan_signal_data *lchan_data;
	struct gsm_lchan *lchan;

	lchan_data = signal_data;
	switch (subsys) {
	case SS_LCHAN:
		lchan = lchan_data->lchan;
		switch (signal) {
		case S_LCHAN_ACTIVATE_ACK:
			return ho_chan_activ_ack(lchan);
		case S_LCHAN_ACTIVATE_NACK:
			return ho_chan_activ_nack(lchan);
		case S_LCHAN_HANDOVER_DETECT:
			return ho_rsl_detect(lchan);
		case S_LCHAN_HANDOVER_COMPL:
			return ho_gsm48_ho_compl(lchan);
		case S_LCHAN_HANDOVER_FAIL:
			return ho_gsm48_ho_fail(lchan);
		}
		break;
	default:
		break;
	}

	return 0;
}

/* Return the old lchan or NULL. This is meant for audio handling */
struct gsm_lchan *bsc_handover_pending(struct gsm_lchan *new_lchan)
{
	struct bsc_handover *ho;
	ho = bsc_ho_by_new_lchan(new_lchan);
	if (!ho)
		return NULL;
	return ho->old_lchan;
}

static __attribute__((constructor)) void on_dso_load_ho_logic(void)
{
	osmo_signal_register_handler(SS_LCHAN, ho_logic_sig_cb, NULL);
}

/* Count number of currently ongoing handovers
 * inter_cell: if true, count only handovers between two cells. If false, count only handovers within one
 * cell. */
int bsc_ho_count(struct gsm_bts *bts, bool inter_cell)
{
	struct bsc_handover *ho;
	int count = 0;

	llist_for_each_entry(ho, &bsc_handovers, list) {
		if (ho->inter_cell != inter_cell)
			continue;
		if (ho->new_lchan->ts->trx->bts == bts)
			count++;
	}

	return count;
}
