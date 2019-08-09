/*
 * libwebsockets - lib/core-net/sequencer.c
 *
 * Copyright (C) 2019 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "core/private.h"

/*
 * per pending event
 */
typedef struct lws_seq_event {
	struct lws_dll2			seq_event_list;

	void				*data;
	void				*aux;
	lws_seq_events_t		e;
} lws_seq_event_t;

/*
 * per sequencer
 */
typedef struct lws_sequencer {
	struct lws_dll2			seq_list;
	struct lws_dll2			seq_pend_list;

	lws_sorted_usec_list_t		sul;

	struct lws_dll2_owner		seq_event_owner;
	struct lws_context_per_thread	*pt;
	lws_seq_event_cb		cb;
	const char			*name;
	const lws_retry_bo_t		*retry;

	lws_usec_t			time_created;
	lws_usec_t			timeout; /* 0 or time we timeout */

	char				going_down;
} lws_seq_t;

#define QUEUE_SANITY_LIMIT 10

lws_seq_t *
lws_seq_create(lws_seq_info_t *i)
{
	struct lws_context_per_thread *pt = &i->context->pt[i->tsi];
	lws_seq_t *seq = lws_zalloc(sizeof(*seq) + i->user_size, __func__);

	if (!seq)
		return NULL;

	seq->cb = i->cb;
	seq->pt = pt;
	seq->name = i->name;
	seq->retry = i->retry;

	*i->puser = (void *)&seq[1];

	/* add the sequencer to the pt */

	lws_pt_lock(pt, __func__); /* ---------------------------------- pt { */

	lws_dll2_add_tail(&seq->seq_list, &pt->seq_owner);

	lws_pt_unlock(pt); /* } pt ------------------------------------------ */

	seq->time_created = lws_now_usecs();

	/* try to queue the creation cb */

	if (lws_seq_queue_event(seq, LWSSEQ_CREATED, NULL, NULL)) {
		lws_dll2_remove(&seq->seq_list);
		lws_free(seq);

		return NULL;
	}

	return seq;
}

static int
seq_ev_destroy(struct lws_dll2 *d, void *user)
{
	lws_seq_event_t *seqe = lws_container_of(d, lws_seq_event_t,
						 seq_event_list);

	lws_dll2_remove(&seqe->seq_event_list);
	lws_free(seqe);

	return 0;
}

void
lws_seq_destroy(lws_seq_t **pseq)
{
	lws_seq_t *seq = *pseq;

	/* defeat another thread racing to add events while we are destroying */
	seq->going_down = 1;

	seq->cb(seq, (void *)&seq[1], LWSSEQ_DESTROYED, NULL, NULL);

	lws_pt_lock(seq->pt, __func__); /* -------------------------- pt { */

	lws_dll2_remove(&seq->seq_list);
	lws_dll2_remove(&seq->sul.list);
	lws_dll2_remove(&seq->seq_pend_list);
	/* remove and destroy any pending events */
	lws_dll2_foreach_safe(&seq->seq_event_owner, NULL, seq_ev_destroy);

	lws_pt_unlock(seq->pt); /* } pt ---------------------------------- */


	lws_free_set_NULL(seq);
}

void
lws_seq_destroy_all_on_pt(struct lws_context_per_thread *pt)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, p, tp,
				   pt->seq_owner.head) {
		lws_seq_t *s = lws_container_of(p, lws_seq_t,
						      seq_list);

		lws_seq_destroy(&s);

	} lws_end_foreach_dll_safe(p, tp);
}

int
lws_seq_queue_event(lws_seq_t *seq, lws_seq_events_t e, void *data,
			  void *aux)
{
	lws_seq_event_t *seqe;

	if (!seq || seq->going_down)
		return 1;

	seqe = lws_zalloc(sizeof(*seqe), __func__);
	if (!seqe)
		return 1;

	seqe->e = e;
	seqe->data = data;
	seqe->aux = aux;

	// lwsl_notice("%s: seq %s: event %d\n", __func__, seq->name, e);

	lws_pt_lock(seq->pt, __func__); /* ----------------------------- pt { */

	if (seq->seq_event_owner.count > QUEUE_SANITY_LIMIT) {
		lwsl_err("%s: more than %d events queued\n", __func__,
			 QUEUE_SANITY_LIMIT);
	}

	lws_dll2_add_tail(&seqe->seq_event_list, &seq->seq_event_owner);

	/* if not already on the pending list, add us */
	if (lws_dll2_is_detached(&seq->seq_pend_list))
		lws_dll2_add_tail(&seq->seq_pend_list, &seq->pt->seq_pend_owner);

	lws_pt_unlock(seq->pt); /* } pt ------------------------------------- */

	return 0;
}

/*
 * Check if wsi still extant, by peeking in the message queue for a
 * LWSSEQ_WSI_CONN_CLOSE message about wsi.  (Doesn't need to do the same for
 * CONN_FAIL since that will never have produced any messages prior to that).
 *
 * Use this to avoid trying to perform operations on wsi that have already
 * closed but we didn't get to that message yet.
 *
 * Returns 0 if not closed yet or 1 if it has closed but we didn't process the
 * close message yet.
 */

int
lws_seq_check_wsi(lws_seq_t *seq, struct lws *wsi)
{
	lws_seq_event_t *seqe;
	struct lws_dll2 *dh;

	lws_pt_lock(seq->pt, __func__); /* ----------------------------- pt { */

	dh = lws_dll2_get_head(&seq->seq_event_owner);
	while (dh) {
		seqe = lws_container_of(dh, lws_seq_event_t, seq_event_list);

		if (seqe->e == LWSSEQ_WSI_CONN_CLOSE && seqe->data == wsi)
			break;

		dh = dh->next;
	}

	lws_pt_unlock(seq->pt); /* } pt ------------------------------------- */

	return !!dh;
}


/*
 * seq should have at least one pending event (he was on the pt's list of
 * sequencers with pending events).  Send the top event in the queue.
 */

static int
lws_seq_next_event(struct lws_dll2 *d, void *user)
{
	lws_seq_t *seq = lws_container_of(d, lws_seq_t,
						seq_pend_list);
	lws_seq_event_t *seqe;
	struct lws_dll2 *dh;
	int n;

	/* we should be on the pending list, right? */
	assert(seq->seq_event_owner.count);

	/* events are only added at tail, so no race possible yet... */

	dh = lws_dll2_get_head(&seq->seq_event_owner);
	seqe = lws_container_of(dh, lws_seq_event_t, seq_event_list);

	n = seq->cb(seq, (void *)&seq[1], seqe->e, seqe->data, seqe->aux);

	/* ... have to lock here though, because we will change the list */

	lws_pt_lock(seq->pt, __func__); /* ----------------------------- pt { */

	/* detach event from sequencer event list and free it */
	lws_dll2_remove(&seqe->seq_event_list);
	lws_free(seqe);

	/*
	 * if seq has no more pending, remove from pt's list of sequencers
	 * with pending events
	 */
	if (!seq->seq_event_owner.count)
		lws_dll2_remove(&seq->seq_pend_list);

	lws_pt_unlock(seq->pt); /* } pt ------------------------------------- */

	if (n) {
		lwsl_info("%s: destroying seq '%s' by request\n", __func__,
				seq->name);
		lws_seq_destroy(&seq);

		return LWSSEQ_RET_DESTROY;
	}

	return LWSSEQ_RET_CONTINUE;
}

/*
 * nonpublic helper for the pt to call one event per pending sequencer, if any
 * are pending
 */

int
lws_pt_do_pending_sequencer_events(struct lws_context_per_thread *pt)
{
	if (!pt->seq_pend_owner.count)
		return 0;

	return lws_dll2_foreach_safe(&pt->seq_pend_owner, NULL,
				     lws_seq_next_event);
}

/* set secs to zero to remove timeout */

int
lws_seq_timeout_us(lws_seq_t *seq, lws_usec_t us)
{
	/* list is always at the very top of the sul */
	return __lws_sul_insert(&seq->pt->seq_to_owner,
				(lws_sorted_usec_list_t *)&seq->sul.list, us);
}

/*
 * nonpublic helper to check for and handle sequencer timeouts for a whole pt
 * returns either 0 or number of us until next event (which cannot be 0 or we
 * would have serviced it)
 */

static void
lws_seq_sul_check_cb(lws_sorted_usec_list_t *sul)
{
	lws_seq_t *s = lws_container_of(sul, lws_seq_t, sul);

	lws_seq_queue_event(s, LWSSEQ_TIMED_OUT, NULL, NULL);
}

lws_usec_t
__lws_seq_timeout_check(struct lws_context_per_thread *pt, lws_usec_t usnow)
{
	lws_usec_t future_us = __lws_sul_check(&pt->seq_to_owner,
					       lws_seq_sul_check_cb, usnow);

	if (usnow - pt->last_heartbeat < LWS_US_PER_SEC)
		return future_us;

	pt->last_heartbeat = usnow;

	/* send every sequencer a heartbeat message... it can ignore it */

	lws_start_foreach_dll_safe(struct lws_dll2 *, p, tp,
				   lws_dll2_get_head(&pt->seq_owner)) {
		lws_seq_t *s = lws_container_of(p, lws_seq_t, seq_list);

		/* queue the message to inform the sequencer */
		lws_seq_queue_event(s, LWSSEQ_HEARTBEAT, NULL, NULL);

	} lws_end_foreach_dll_safe(p, tp);

	return future_us;
}

lws_seq_t *
lws_seq_from_user(void *u)
{
	return &((lws_seq_t *)u)[-1];
}

const char *
lws_seq_name(lws_seq_t *seq)
{
	return seq->name;
}

int
lws_seq_secs_since_creation(lws_seq_t *seq)
{
	time_t now;

	time(&now);

	return now - seq->time_created;
}

struct lws_context *
lws_seq_get_context(lws_seq_t *seq)
{
	return seq->pt->context;
}
