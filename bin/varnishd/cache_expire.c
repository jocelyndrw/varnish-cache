/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * LRU and object timer handling.
 *
 * We have two data structures, a LRU-list and a binary heap for the timers
 * and two ways to kill objects: TTL-timeouts and LRU cleanups.
 *
 * Any object on the LRU is also on the binheap and vice versa.
 *
 * We hold a single object reference for both data structures.
 *
 * An attempted overview:
 *
 *	                        EXP_Ttl()      EXP_Grace()   EXP_Keep()
 *				   |                |            |
 *      entered                    v                v            |
 *         |                       +--------------->+            |
 *         v                       |      grace                  |
 *         +---------------------->+                             |
 *                  ttl            |                             v
 *                                 +---------------------------->+
 *                                     keep
 *
 */

#include "config.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "binary_heap.h"
#include "cache.h"
#include "hash_slinger.h"
#include "stevedore.h"

static pthread_t exp_thread;
static struct binheap *exp_heap;
static struct lock exp_mtx;

/*--------------------------------------------------------------------
 * struct exp manipulations
 *
 * The Get/Set functions encapsulate the mutual magic between the
 * fields in one single place.
 */

void
EXP_Clr(struct exp *e)
{

	e->ttl = -1;
	e->grace = -1;
	e->keep = -1;
	e->age = 0;
	e->entered = 0;
}

#define EXP_ACCESS(fld, low_val, extra)				\
	double							\
	EXP_Get_##fld(const struct exp *e)			\
	{							\
		return (e->fld > 0. ? e->fld : low_val);	\
	}							\
								\
	void							\
	EXP_Set_##fld(struct exp *e, double v)			\
	{							\
		if (v > 0.)					\
			e->fld = v;				\
		else {						\
			e->fld = -1.;				\
			extra;					\
		}						\
	}							\

EXP_ACCESS(ttl, -1., (e->grace = e->keep = -1.))
EXP_ACCESS(grace, 0., )
EXP_ACCESS(keep, 0.,)

/*--------------------------------------------------------------------
 * Calculate an objects effective keep, grace or ttl time, suitably
 * adjusted for defaults and by per-session limits.
 */

static double
EXP_Keep(const struct sess *sp, const struct object *o)
{
	double r;

	r = (double)params->default_keep;
	if (o->exp.keep > 0.)
		r = o->exp.keep;
	if (sp != NULL && sp->exp.keep > 0. && sp->exp.keep < r)
		r = sp->exp.keep;
	return (EXP_Ttl(sp, o) + r);
}

double
EXP_Grace(const struct sess *sp, const struct object *o)
{
	double r;

	r = (double)params->default_grace;
	if (o->exp.grace >= 0.)
		r = o->exp.grace;
	if (sp != NULL && sp->exp.grace > 0. && sp->exp.grace < r)
		r = sp->exp.grace;
	return (EXP_Ttl(sp, o) + r);
}

double
EXP_Ttl(const struct sess *sp, const struct object *o)
{
	double r;

	r = o->exp.ttl;
	if (sp != NULL && sp->exp.ttl > 0. && sp->exp.ttl < r)
		r = sp->exp.ttl;
	return (o->exp.entered + r);
}

/*--------------------------------------------------------------------
 * When & why does the timer fire for this object ?
 */

static int
update_object_when(const struct object *o)
{
	struct objcore *oc;
	double when, w2;

	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	oc = o->objcore;
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	Lck_AssertHeld(&exp_mtx);

	when = EXP_Keep(NULL, o);
	w2 = EXP_Grace(NULL, o);
	if (w2 > when)
		when = w2;
	assert(!isnan(when));
	if (when == oc->timer_when)
		return (0);
	oc->timer_when = when;
	return (1);
}

/*--------------------------------------------------------------------*/

static void
exp_insert(struct objcore *oc, struct lru *lru)
{
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);

	Lck_AssertHeld(&lru->mtx);
	Lck_AssertHeld(&exp_mtx);
	assert(oc->timer_idx == BINHEAP_NOIDX);
	binheap_insert(exp_heap, oc);
	assert(oc->timer_idx != BINHEAP_NOIDX);
	VTAILQ_INSERT_TAIL(&lru->lru_head, oc, lru_list);
}

/*--------------------------------------------------------------------
 * Object has been added to cache, record in lru & binheap.
 *
 * The objcore comes with a reference, which we inherit.
 */

void
EXP_Inject(struct objcore *oc, struct lru *lru, double when)
{

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);

	Lck_Lock(&lru->mtx);
	Lck_Lock(&exp_mtx);
	oc->timer_when = when;
	exp_insert(oc, lru);
	Lck_Unlock(&exp_mtx);
	Lck_Unlock(&lru->mtx);
}

/*--------------------------------------------------------------------
 * Object has been added to cache, record in lru & binheap.
 *
 * We grab a reference to the object, which will keep it around until
 * we decide its time to let it go.
 */

void
EXP_Insert(struct object *o)
{
	struct objcore *oc;
	struct lru *lru;

	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	oc = o->objcore;
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	AssertObjBusy(o);
	HSH_Ref(oc);

	assert(o->exp.entered != 0 && !isnan(o->exp.entered));
	o->last_lru = o->exp.entered;

	lru = oc_getlru(oc);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Lock(&lru->mtx);
	Lck_Lock(&exp_mtx);
	(void)update_object_when(o);
	exp_insert(oc, lru);
	Lck_Unlock(&exp_mtx);
	Lck_Unlock(&lru->mtx);
	oc_updatemeta(oc);
}

/*--------------------------------------------------------------------
 * Object was used, move to tail of LRU list.
 *
 * To avoid the exp_mtx becoming a hotspot, we only attempt to move
 * objects if they have not been moved recently and if the lock is available.
 * This optimization obviously leaves the LRU list imperfectly sorted.
 */

int
EXP_Touch(struct objcore *oc)
{
	struct lru *lru;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

	/*
	 * For -spersistent we don't move objects on the lru list.  Each
	 * segment has its own LRU list, and the order on it is not material
	 * for anything.  The code below would move the objects to the
	 * LRU list of the currently open segment, which would prevent
	 * the cleaner from doing its job.
	 */
	if (oc->flags & OC_F_LRUDONTMOVE)
		return (0);

	lru = oc_getlru(oc);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);

	/*
	 * We only need the LRU lock here.  The locking order is LRU->EXP
	 * so we can trust the content of the oc->timer_idx without the
	 * EXP lock.   Since each lru list has its own lock, this should
	 * reduce contention a fair bit
	 */
	if (Lck_Trylock(&lru->mtx))
		return (0);

	if (oc->timer_idx != BINHEAP_NOIDX) {
		VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);
		VTAILQ_INSERT_TAIL(&lru->lru_head, oc, lru_list);
		VSC_C_main->n_lru_moved++;
	}
	Lck_Unlock(&lru->mtx);
	return (1);
}

/*--------------------------------------------------------------------
 * We have changed one or more of the object timers, shuffle it
 * accordingly in the binheap
 *
 * The VCL code can send us here on a non-cached object, just return.
 *
 * XXX: special case check for ttl = 0 ?
 */

void
EXP_Rearm(const struct object *o)
{
	struct objcore *oc;
	struct lru *lru;

	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	oc = o->objcore;
	if (oc == NULL)
		return;
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	lru = oc_getlru(oc);
	Lck_Lock(&lru->mtx);
	Lck_Lock(&exp_mtx);
	/*
	 * The hang-man might have this object of the binheap while
	 * tending to a timer.  If so, we do not muck with it here.
	 */
	if (oc->timer_idx != BINHEAP_NOIDX && update_object_when(o)) {
		assert(oc->timer_idx != BINHEAP_NOIDX);
		binheap_reorder(exp_heap, oc->timer_idx);
		assert(oc->timer_idx != BINHEAP_NOIDX);
	}
	Lck_Unlock(&exp_mtx);
	Lck_Unlock(&lru->mtx);
	oc_updatemeta(oc);
}

/*--------------------------------------------------------------------
 * This thread monitors the root of the binary heap and whenever an
 * object expires, accounting also for graceability, it is killed.
 */

static void * __match_proto__(void *start_routine(void *))
exp_timer(struct sess *sp, void *priv)
{
	struct objcore *oc;
	struct lru *lru;
	double t;
	struct object *o;

	(void)priv;
	t = TIM_real();
	oc = NULL;
	while (1) {
		if (oc == NULL) {
			WSL_Flush(sp->wrk, 0);
			WRK_SumStat(sp->wrk);
			TIM_sleep(params->expiry_sleep);
			t = TIM_real();
		}

		Lck_Lock(&exp_mtx);
		oc = binheap_root(exp_heap);
		if (oc == NULL) {
			Lck_Unlock(&exp_mtx);
			continue;
		}
		CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

		/*
		 * We may have expired so many objects that our timestamp
		 * got out of date, refresh it and check again.
		 */
		if (oc->timer_when > t)
			t = TIM_real();
		if (oc->timer_when > t) {
			Lck_Unlock(&exp_mtx);
			oc = NULL;
			continue;
		}

		/* If the object is busy, we have to wait for it */
		if (oc->flags & OC_F_BUSY) {
			Lck_Unlock(&exp_mtx);
			oc = NULL;
			continue;
		}

		/*
		 * It's time...
		 * Technically we should drop the exp_mtx, get the lru->mtx
		 * get the exp_mtx again and then check that the oc is still
		 * on the binheap.  We take the shorter route and try to
		 * get the lru->mtx and punt if we fail.
		 */

		lru = oc_getlru(oc);
		CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
		if (Lck_Trylock(&lru->mtx)) {
			Lck_Unlock(&exp_mtx);
			oc = NULL;
			continue;
		}

		/* Remove from binheap */
		assert(oc->timer_idx != BINHEAP_NOIDX);
		binheap_delete(exp_heap, oc->timer_idx);
		assert(oc->timer_idx == BINHEAP_NOIDX);

		/* And from LRU */
		lru = oc_getlru(oc);
		VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);

		Lck_Unlock(&exp_mtx);
		Lck_Unlock(&lru->mtx);

		VSC_C_main->n_expired++;

		CHECK_OBJ_NOTNULL(oc->objhead, OBJHEAD_MAGIC);
		o = oc_getobj(sp->wrk, oc);
		WSL(sp->wrk, SLT_ExpKill, 0, "%u %.0f",
		    o->xid, EXP_Ttl(NULL, o) - t);
		(void)HSH_Deref(sp->wrk, oc, NULL);
	}
	NEEDLESS_RETURN(NULL);
}

/*--------------------------------------------------------------------
 * Attempt to make space by nuking the oldest object on the LRU list
 * which isn't in use.
 * Returns: 1: did, 0: didn't, -1: can't
 */

int
EXP_NukeOne(struct worker *w, struct lru *lru)
{
	struct objcore *oc;
	struct object *o;

	/* Find the first currently unused object on the LRU.  */
	Lck_Lock(&lru->mtx);
	Lck_Lock(&exp_mtx);
	VTAILQ_FOREACH(oc, &lru->lru_head, lru_list) {
		CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
		assert (oc->timer_idx != BINHEAP_NOIDX);
		/*
		 * It wont release any space if we cannot release the last
		 * reference, besides, if somebody else has a reference,
		 * it's a bad idea to nuke this object anyway. Also do not
		 * touch busy objects.
		 */
		if (oc->refcnt == 1 && !(oc->flags & OC_F_BUSY))
			break;
	}
	if (oc != NULL) {
		VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);
		binheap_delete(exp_heap, oc->timer_idx);
		assert(oc->timer_idx == BINHEAP_NOIDX);
		VSC_C_main->n_lru_nuked++;
	}
	Lck_Unlock(&exp_mtx);
	Lck_Unlock(&lru->mtx);

	if (oc == NULL)
		return (-1);

	/* XXX: bad idea for -spersistent */
	o = oc_getobj(w, oc);
	WSL(w, SLT_ExpKill, 0, "%u LRU", o->xid);
	(void)HSH_Deref(w, NULL, &o);
	return (1);
}

/*--------------------------------------------------------------------
 * Nukes an entire LRU
 */

void
EXP_NukeLRU(struct worker *wrk, struct lru *lru)
{
	struct objcore *oc;
	struct objcore *oc_array[10];
	struct object *o;
	int i, n;
	double t;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);

	t = TIM_real();
	Lck_Lock(&lru->mtx);
	while (!VTAILQ_EMPTY(&lru->lru_head)) {
		Lck_Lock(&exp_mtx);
		n = 0;
		while (n < 10) {
			oc = VTAILQ_FIRST(&lru->lru_head);
			if (oc == NULL)
				break;
			CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
			assert(oc_getlru(oc) == lru);

			/* Remove from the binheap and LRU */
			binheap_delete(exp_heap, oc->timer_idx);
			VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);

			oc_array[n++] = oc;
		}
		assert(n > 0);
		Lck_Unlock(&exp_mtx);
		Lck_Unlock(&lru->mtx);

		for (i = 0; i < n; i++) {
			oc = oc_array[i];
			CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
			CHECK_OBJ_NOTNULL(oc->objhead, OBJHEAD_MAGIC);
			assert(oc->timer_idx == BINHEAP_NOIDX);
			o = oc_getobj(wrk, oc);
			WSL(wrk, SLT_ExpKill, 0, "%u %.0f LRU",
			    o->xid, EXP_Ttl(NULL, o) - t);
			EXP_Set_ttl(&o->exp, 0.);
			(void)HSH_Deref(wrk, oc, NULL);
		}
		VSC_C_main->n_lru_nuked++;

		Lck_Lock(&lru->mtx);
	}
	Lck_Unlock(&lru->mtx);

	WRK_SumStat(wrk);
}

/*--------------------------------------------------------------------
 * BinHeap helper functions for objcore.
 */

static int
object_cmp(void *priv, void *a, void *b)
{
	struct objcore *aa, *bb;

	(void)priv;
	CAST_OBJ_NOTNULL(aa, a, OBJCORE_MAGIC);
	CAST_OBJ_NOTNULL(bb, b, OBJCORE_MAGIC);
	return (aa->timer_when < bb->timer_when);
}

static void
object_update(void *priv, void *p, unsigned u)
{
	struct objcore *oc;

	(void)priv;
	CAST_OBJ_NOTNULL(oc, p, OBJCORE_MAGIC);
	oc->timer_idx = u;
}

/*--------------------------------------------------------------------*/

void
EXP_Init(void)
{

	Lck_New(&exp_mtx, lck_exp);
	exp_heap = binheap_new(NULL, object_cmp, object_update);
	XXXAN(exp_heap);
	WRK_BgThread(&exp_thread, "cache-timeout", exp_timer, NULL);
}
