/*
 * Copyright (C) 1999, 2000  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <isc/assertions.h>
#include <isc/boolean.h>
#include <isc/error.h>
#include <isc/ratelimiter.h>
#include <isc/time.h>
#include <isc/util.h>

typedef enum {
	isc_ratelimiter_ratelimited,
	isc_ratelimiter_worklimited,
	isc_ratelimiter_shuttingdown
} isc_ratelimiter_state_t;

struct isc_ratelimiter {
	isc_mem_t *		mctx;
	isc_mutex_t		lock;
	isc_task_t *		task;
	isc_timer_t *		timer;
	isc_interval_t		interval;
	isc_ratelimiter_state_t	state;
	isc_event_t		shutdownevent;
	ISC_LIST(isc_event_t)	pending;
};

#define ISC_RATELIMITEREVENT_SHUTDOWN (ISC_EVENTCLASS_RATELIMITER + 1)

static void ratelimiter_tick(isc_task_t *task, isc_event_t *event);

static void
ratelimiter_shutdowncomplete(isc_task_t *task, isc_event_t *event)
{
	isc_ratelimiter_t *rl = (isc_ratelimiter_t *)event->ev_arg;
	UNUSED(task);
	isc_mutex_destroy(&rl->lock);
	isc_mem_put(rl->mctx, rl, sizeof(*rl));
}

isc_result_t
isc_ratelimiter_create(isc_mem_t *mctx, isc_timermgr_t *timermgr,
		       isc_task_t *task, isc_ratelimiter_t **ratelimiterp)
{
	isc_result_t result;
	isc_ratelimiter_t *rl;
	INSIST(ratelimiterp != NULL && *ratelimiterp == NULL);
	
	rl = isc_mem_get(mctx, sizeof(*rl));
	if (rl == NULL)
		return ISC_R_NOMEMORY;
	rl->mctx = mctx;
	rl->task = task;
	isc_interval_set(&rl->interval, 0, 0);
	rl->timer = NULL;
	rl->state = isc_ratelimiter_worklimited;
	ISC_LIST_INIT(rl->pending);

	result = isc_mutex_init(&rl->lock);
	if (result != ISC_R_SUCCESS)
		goto free_mem;
	result = isc_timer_create(timermgr, isc_timertype_inactive,
				  NULL, NULL, rl->task, ratelimiter_tick,
				  rl, &rl->timer);
	if (result != ISC_R_SUCCESS)
		goto free_mutex;

	ISC_EVENT_INIT(&rl->shutdownevent,
		       sizeof(isc_event_t),
		       0, NULL, ISC_RATELIMITEREVENT_SHUTDOWN,
		       ratelimiter_shutdowncomplete, rl, rl, NULL, NULL);

	*ratelimiterp = rl;
	return (ISC_R_SUCCESS);

free_mutex:
	isc_mutex_destroy(&rl->lock);
free_mem:
	isc_mem_put(mctx, rl, sizeof(*rl));
	return (result);
}

isc_result_t
isc_ratelimiter_setinterval(isc_ratelimiter_t *rl, isc_interval_t *interval) {
	isc_result_t result = ISC_R_SUCCESS;
	LOCK(&rl->lock);
	rl->interval = *interval;
	/*
	 * If the timer is currently running, change its rate.
	 */
        if (rl->state == isc_ratelimiter_ratelimited) {
		result = isc_timer_reset(rl->timer, isc_timertype_ticker, NULL,
					 &rl->interval, ISC_FALSE);
	}
	UNLOCK(&rl->lock);
	return (result);
}
			
isc_result_t
isc_ratelimiter_enqueue(isc_ratelimiter_t *rl, isc_event_t **eventp) {
	isc_result_t result = ISC_R_SUCCESS;
	INSIST(eventp != NULL && *eventp != NULL);
	LOCK(&rl->lock);
        if (rl->state == isc_ratelimiter_ratelimited) {
		isc_event_t *ev = *eventp;
                ISC_LIST_APPEND(rl->pending, ev, ev_link);
		*eventp = NULL;
        } else if (rl->state == isc_ratelimiter_worklimited) {
		result = isc_timer_reset(rl->timer, isc_timertype_ticker, NULL,
					 &rl->interval, ISC_FALSE);
		if (result == ISC_R_SUCCESS)
			rl->state = isc_ratelimiter_ratelimited;
	} else {
		INSIST(rl->state == isc_ratelimiter_shuttingdown);
		result = ISC_R_SHUTTINGDOWN;
	}
	UNLOCK(&rl->lock);
	if (*eventp != NULL)
		isc_task_send(rl->task, eventp);
	ENSURE(*eventp == NULL);
	return (result);
}

static void
ratelimiter_tick(isc_task_t *task, isc_event_t *event) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_ratelimiter_t *rl = (isc_ratelimiter_t *)event->ev_arg;
	isc_event_t *p;
	UNUSED(task);
	LOCK(&rl->lock);
        p = ISC_LIST_HEAD(rl->pending);
        if (p != NULL) {
		/*
		 * There is work to do.  Let's do it after unlocking.
		 */
                ISC_LIST_UNLINK(rl->pending, p, ev_link);
	} else {
		/*
		 * No work left to do.  Stop the timer so that we don't
		 * waste resources by having it fire periodically.
		 */
		result = isc_timer_reset(rl->timer, isc_timertype_inactive,
					 NULL, NULL, ISC_FALSE);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);
		rl->state = isc_ratelimiter_worklimited;
	}
        UNLOCK(&rl->lock);
	isc_event_free(&event);
	/*
	 * If we have an event, dispatch it.
	 * There is potential for optimization here since
	 * we are already executing in the context of "task".
	 */
	if (p != NULL)
		isc_task_send(rl->task, &p);
	INSIST(p == NULL);
}

void
isc_ratelimiter_shutdown(isc_ratelimiter_t *rl) {
	isc_event_t *ev;
	LOCK(&rl->lock);
	rl->state = isc_ratelimiter_shuttingdown;
	(void) isc_timer_reset(rl->timer, isc_timertype_inactive,
			       NULL, NULL, ISC_FALSE);
	while ((ev = ISC_LIST_HEAD(rl->pending)) != NULL) {
		ISC_LIST_UNLINK(rl->pending, ev, ev_link);
		ev->ev_attributes |= ISC_EVENTATTR_CANCELED;
		isc_task_send(rl->task, &ev);
	}
	UNLOCK(&rl->lock);
}

void
isc_ratelimiter_destroy(isc_ratelimiter_t **ratelimiterp) 
{
	isc_ratelimiter_t *rl = *ratelimiterp;
	isc_event_t *ev = &rl->shutdownevent;
	isc_timer_detach(&rl->timer);
	/*
	 * Send an event to our task and wait for it to be delivered
	 * before freeing memory.  This guarantees that any timer
	 * event still in the task's queue are delivered first.
	 */
	isc_task_send(rl->task, &ev);
	*ratelimiterp = NULL;
}
