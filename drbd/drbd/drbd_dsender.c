/*
-*- linux-c -*-
   drbd_dsender.c
   Kernel module for 2.2.x/2.4.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 1999-2003, Philipp Reisner <philipp.reisner@gmx.at>.
	main author.

   Copyright 2003 Lars Ellenberg <l.g.e@web.de>
       contributions.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#ifdef HAVE_AUTOCONF
#include <linux/autoconf.h>
#endif
#ifdef CONFIG_MODVERSIONS
#include <linux/modversions.h>
#endif

#include <asm/bitops.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/wait.h>
#define __KERNEL_SYSCALLS__
#include <linux/slab.h>

#include "drbd.h"
#include "drbd_int.h"

/* I choose to have all block layer end_io handlers defined here.

 * For all these callbacks, note the follwing:
 * The callbacks will be called in irq context by the IDE drivers,
 * and in Softirqs/Tasklets/BH context by the SCSI drivers.
 * Try to get the locking right :)
 *
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)

/* used for synchronous meta data and bitmap IO
 * submitted by FIXME (I'd say worker only, but currently this is not true...)
 */
void drbd_md_io_complete(struct buffer_head *bh, int uptodate)
{
	if (uptodate)
		set_bit(BH_Uptodate, &bh->b_state);

	complete((struct completion*)bh->b_private);
}

/* reads on behalf of the partner,
 * "submitted" by the receiver
 */
void enslaved_read_bi_end_io(drbd_bio_t *bh, int uptodate)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bh,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);

	mark_buffer_uptodate(bh, uptodate);
	clear_bit(BH_Lock, &bh->b_state);
	smp_mb__after_clear_bit();

	list_del(&e->w.list);
	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_queue_work(mdev,&mdev->data.work,&e->w);
	dec_local(mdev);
}

/* writes on behalf of the partner, or resync writes,
 * "submitted" by the receiver.
 */
void drbd_dio_end_sec(struct buffer_head *bh, int uptodate)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	mdev=bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bh,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);

	mark_buffer_uptodate(bh, uptodate);

	clear_bit(BH_Dirty, &bh->b_state);
	clear_bit(BH_Lock, &bh->b_state);
	smp_mb__after_clear_bit();

	list_del(&e->w.list);
	list_add(&e->w.list,&mdev->done_ee);

	if (waitqueue_active(&mdev->ee_wait) &&
	    (list_empty(&mdev->active_ee) ||
	     list_empty(&mdev->sync_ee)))
		wake_up(&mdev->ee_wait);

	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	if( mdev->do_panic && !uptodate) {
		drbd_panic(DEVICE_NAME": The lower-level device had an error.\n");
	}

	wake_asender(mdev);
	dec_local(mdev);
}

/* writes on Primary comming from drbd_make_request
 */
void drbd_dio_end(struct buffer_head *bh, int uptodate)
{
	struct Drbd_Conf* mdev;
	drbd_request_t *req;

	mdev = bh->b_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	req = container_of(bh,struct drbd_request,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(req));

	drbd_end_req(req, RQ_DRBD_WRITTEN, uptodate, drbd_req_get_sector(req));
	drbd_al_complete_io(mdev,drbd_req_get_sector(req));
	dec_local(mdev);
}

#else
//#warning "FIXME"
/* used for synchronous meta data and bitmap IO
 * submitted by FIXME (I'd say worker only, but currently this is not true...)
 */
int drbd_md_io_complete(struct bio *bio, unsigned int bytes_done, int error)
{
	if (bio->bi_size)
		return 1;

	complete((struct completion*)bio->bi_private);
	return 0;
}

/* reads on behalf of the partner,
 * "submitted" by the receiver
 */
int enslaved_read_bi_end_io(struct bio *bio, unsigned int bytes_done, int error)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	/* we should be called via bio_endio, so this should never be the case
	 * but "everyone else does it", and so do we ;)		-lge
	 */
	if (bio->bi_size)
		return 1;

	mdev=bio->bi_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bio,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);
	list_del(&e->w.list);
	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	drbd_queue_work(mdev,&mdev->data.work,&e->w);
	dec_local(mdev);
	return 0;
}

/* writes on behalf of the partner, or resync writes,
 * "submitted" by the receiver.
 */
int drbd_dio_end_sec(struct bio *bio, unsigned int bytes_done, int error)
{
	unsigned long flags=0;
	struct Tl_epoch_entry *e=NULL;
	struct Drbd_Conf* mdev;

	// see above
	if (bio->bi_size)
		return 1;

	mdev=bio->bi_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	e = container_of(bio,struct Tl_epoch_entry,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(e));
	D_ASSERT(e->block_id != ID_VACANT);

	spin_lock_irqsave(&mdev->ee_lock,flags);
	list_del(&e->w.list);
	list_add(&e->w.list,&mdev->done_ee);

	if (waitqueue_active(&mdev->ee_wait) &&
	    (list_empty(&mdev->active_ee) ||
	     list_empty(&mdev->sync_ee)))
		wake_up(&mdev->ee_wait);

	spin_unlock_irqrestore(&mdev->ee_lock,flags);

	if( mdev->do_panic && error) {
		drbd_panic(DEVICE_NAME": The lower-level device had an error.\n");
	}

	wake_asender(mdev);
	dec_local(mdev);
	return 0;
}

/* writes on Primary comming from drbd_make_request
 */
int drbd_dio_end(struct bio *bio, unsigned int bytes_done, int error)
{
	struct Drbd_Conf* mdev;
	drbd_request_t *req;

	// see above
	if (bio->bi_size)
		return 1;

	mdev = bio->bi_private;
	PARANOIA_BUG_ON(!IS_VALID_MDEV(mdev));

	req = container_of(bio,struct drbd_request,private_bio);
	PARANOIA_BUG_ON(!VALID_POINTER(req));

	drbd_end_req(req, RQ_DRBD_WRITTEN, (error == 0), drbd_req_get_sector(req));
	drbd_al_complete_io(mdev,drbd_req_get_sector(req));
	dec_local(mdev);
	return 0;
}
#endif

int w_resync_inactive(drbd_dev *mdev, struct drbd_work *w)
{
	ERR("resync inactive, but callback triggered??\n");
	return 0;
}

void resync_timer_fn(unsigned long data)
{
	drbd_dev* mdev = (drbd_dev*) data;

	if(unlikely(test_and_clear_bit(STOP_SYNC_TIMER,&mdev->flags))) {
		mdev->resync_work.cb = w_resync_inactive;
	} else {
		drbd_queue_work(mdev,&mdev->data.work,&mdev->resync_work);
	}
}

int w_make_resync_request(drbd_dev* mdev, struct drbd_work* w)
{
	struct Pending_read *pr;
	sector_t sector;
	int number,i,size;

	PARANOIA_BUG_ON(w != &mdev->resync_work);

	if(mdev->cstate < Connected ) return 1; // connection was lost...

	D_ASSERT(mdev->cstate == SyncTarget);

#define SLEEP_TIME (HZ/10)

        number = SLEEP_TIME*mdev->sync_conf.rate / ((BM_BLOCK_SIZE/1024)*HZ);

        if(number > 1000) number=1000;  // Remove later
	if (atomic_read(&mdev->rs_pending_cnt)>1200) {
		// INFO("pending cnt high -- throttling resync.\n");
		goto requeue;
	}

	for(i=0;i<number;i++) {
		pr = mempool_alloc(drbd_pr_mempool, GFP_USER);
		if (unlikely(pr == NULL)) goto requeue;
		SET_MAGIC(pr);

	next_sector:
		size = BM_BLOCK_SIZE;
		sector = bm_get_sector(mdev->mbds_id,&size);

		if (sector == MBDS_DONE) {
			INVALIDATE_MAGIC(pr);
			mempool_free(pr,drbd_pr_mempool);
			mdev->resync_work.cb = w_resync_inactive;
			return 1;
		}

		drbd_rs_begin_io(mdev,sector);
		if(unlikely(!bm_get_bit(mdev->mbds_id,sector,BM_BLOCK_SIZE))) {
			INFO("Block got synced while in drbd_rs_begin_io()\n");
			drbd_rs_complete_io(mdev,sector);
			goto next_sector;
		}

		pr->d.sector = sector;
		pr->cause = Resync;
		spin_lock(&mdev->pr_lock);
		list_add(&pr->w.list,&mdev->resync_reads);
		spin_unlock(&mdev->pr_lock);

		inc_rs_pending(mdev);
		ERR_IF(!drbd_send_drequest(mdev,RSDataRequest,
					   sector,size,(unsigned long)pr)) {
			dec_rs_pending(mdev,HERE);
			return 0; // FAILED. worker will abort!
		}
	}

   requeue:
	mdev->resync_timer.expires = jiffies + SLEEP_TIME;
	add_timer(&mdev->resync_timer);
	return 1;
}

int w_resync_finished(drbd_dev* mdev, struct drbd_work* w)
{
	unsigned long dt;

	PARANOIA_BUG_ON(w != &mdev->resync_work);
	D_ASSERT(mdev->rs_left == 0);

	dt = (jiffies - mdev->rs_start) / HZ + 1;
	INFO("Resync done (total %lu sec; %lu K/sec)\n",
	     dt,(mdev->rs_total/2)/dt);

	if (mdev->cstate == SyncTarget) {
		mdev->gen_cnt[Flags] |= MDF_Consistent;
		drbd_md_write(mdev);
	}
	mdev->rs_total = 0;

	// assert that all bit-map parts are cleared.
	D_ASSERT(list_empty(&mdev->resync->lru));
	// w->cb = w_resync_inactive; // look into done set_cstate()

	set_cstate(mdev,Connected);
	return 1;
}

int w_e_end_data_req(drbd_dev *mdev, struct drbd_work *w)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	int ok;

	ok=drbd_send_block(mdev, DataReply, e);
	dec_unacked(mdev,HERE); // THINK unconditional?

	spin_lock_irq(&mdev->ee_lock);
	drbd_put_ee(mdev,e);
	spin_unlock_irq(&mdev->ee_lock);

	if(unlikely(!ok)) ERR("drbd_send_block() failed\n");
	return ok;
}

int w_e_end_rsdata_req(drbd_dev *mdev, struct drbd_work *w)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry*)w;
	int ok;

	drbd_rs_complete_io(mdev,drbd_ee_get_sector(e));
	inc_rs_pending(mdev);
	ok=drbd_send_block(mdev, DataReply, e);
	dec_unacked(mdev,HERE); // THINK unconditional?

	spin_lock_irq(&mdev->ee_lock);
	drbd_put_ee(mdev,e);
	spin_unlock_irq(&mdev->ee_lock);

	if(unlikely(!ok)) ERR("drbd_send_block() failed\n");
	return ok;
}


STATIC void drbd_global_lock(void)
{
	int i;

	local_irq_disable();
	for (i=0; i < minor_count; i++) {
		spin_lock(&drbd_conf[i].req_lock);
	}
}

STATIC void drbd_global_unlock(void)
{
	int i;

	for (i=0; i < minor_count; i++) {
		spin_unlock(&drbd_conf[i].req_lock);
	}
	local_irq_enable();
}

STATIC void _drbd_rs_resume(drbd_dev *mdev)
{
	Drbd_CState ns;

	ns = mdev->cstate - (PausedSyncS - SyncSource);
	D_ASSERT(ns == SyncSource || ns == SyncTarget);

	INFO("Syncer continues.\n");
	_set_cstate(mdev,ns);

	if(mdev->cstate == SyncTarget) {
		mdev->resync_work.cb = w_make_resync_request;
		_drbd_queue_work(&mdev->data.work,&mdev->resync_work);
	}
}


STATIC void _drbd_rs_pause(drbd_dev *mdev)
{
	Drbd_CState ns;

	D_ASSERT(mdev->cstate == SyncSource || mdev->cstate == SyncTarget);
	ns = mdev->cstate + (PausedSyncS - SyncSource);

	if(mdev->cstate == SyncTarget) set_bit(STOP_SYNC_TIMER,&mdev->flags);

	_set_cstate(mdev,ns);
	INFO("Syncer waits for sync group.\n");
}

STATIC int _drbd_pause_higher_sg(drbd_dev *mdev)
{
	drbd_dev *odev;
	int i,rv=0;

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		if ( odev->sync_conf.group > mdev->sync_conf.group
		     && ( odev->cstate == SyncSource || 
			  odev->cstate == SyncTarget ) ) {
			_drbd_rs_pause(odev);
			rv = 1;
		}
	}

	return rv;
}

STATIC int _drbd_lower_sg_running(drbd_dev *mdev)
{
	drbd_dev *odev;
	int i,rv=0;

	for (i=0; i < minor_count; i++) {
		odev = drbd_conf + i;
		if ( odev->sync_conf.group < mdev->sync_conf.group
		     && ( odev->cstate == SyncSource || 
			  odev->cstate == SyncTarget ) ) {
			rv = 1;
		}
	}

	return rv;
}

int w_resume_next_sg(drbd_dev* mdev, struct drbd_work* w)
{
	drbd_dev *odev;
	int i,ng=10000;

	PARANOIA_BUG_ON(w != &mdev->resync_work);

	drbd_global_lock();

	for (i=0; i < minor_count; i++) { // find next sync group
		odev = drbd_conf + i;
		if ( odev->sync_conf.group > mdev->sync_conf.group
		     && odev->sync_conf.group < ng && 
		     (odev->cstate==PausedSyncS || odev->cstate==PausedSyncT)){
		  ng = odev->sync_conf.group;
		}
	}

	for (i=0; i < minor_count; i++) { // resume all devices in next group
		odev = drbd_conf + i;
		if ( odev->sync_conf.group == ng &&
		     (odev->cstate==PausedSyncS || odev->cstate==PausedSyncT)){
			_drbd_rs_resume(odev);
		}
	}

	drbd_global_unlock();
	w->cb = w_resync_inactive;

	return 1;
}

void drbd_alter_sg(drbd_dev *mdev, int ng)
{
	int c = 0;
	int d = (ng - mdev->sync_conf.group);

	drbd_global_lock();
	mdev->sync_conf.group = ng;

	if( ( mdev->cstate == PausedSyncS || 
	      mdev->cstate == PausedSyncT ) && ( d < 0 ) ) {
		if(_drbd_pause_higher_sg(mdev)) c=1;
		else if(!_drbd_lower_sg_running(mdev)) c=1;
		if(c) _drbd_rs_resume(mdev);
	}

	if( ( mdev->cstate == SyncSource || 
	      mdev->cstate == SyncTarget ) && ( d > 0 ) ) {
		if(_drbd_lower_sg_running(mdev)) c=1;
		if(c) _drbd_rs_pause(mdev);
	}
	drbd_global_unlock();
}

void drbd_start_resync(drbd_dev *mdev, Drbd_CState side)
{
	set_cstate(mdev,side);
	mdev->rs_left=mdev->rs_total;
	mdev->rs_start=jiffies;
	mdev->rs_mark_left=mdev->rs_left;
	mdev->rs_mark_time=mdev->rs_start;

	INFO("Resync started as %s (need to sync %lu KB).\n",
	     side == SyncTarget ? "target" : "source", mdev->rs_left/2);

	PARANOIA_BUG_ON(!list_empty(&mdev->resync_work.list));
	PARANOIA_BUG_ON(mdev->resync_work.cb != w_resync_inactive);

	if ( mdev->rs_left == 0 ) {
		mdev->resync_work.cb = w_resync_finished;
		drbd_queue_work(mdev,&mdev->data.work,&mdev->resync_work);
		return;
	}

	if(mdev->cstate == SyncTarget) {
		mdev->gen_cnt[Flags] &= ~MDF_Consistent;
		bm_reset(mdev->mbds_id);
		mdev->resync_work.cb = w_make_resync_request;
		drbd_queue_work(mdev,&mdev->data.work,&mdev->resync_work);
	} else {
		// If we are SyncSource we must be consistent :)
		mdev->gen_cnt[Flags] |= MDF_Consistent;
	}

	drbd_md_write(mdev);

	drbd_global_lock();
	_drbd_pause_higher_sg(mdev);
	if(_drbd_lower_sg_running(mdev)) {
		_drbd_rs_pause(mdev);
	}
	drbd_global_unlock();
}

int drbd_worker(struct Drbd_thread *thi)
{
	drbd_dev *mdev = thi->mdev;
	struct drbd_work *w;
	unsigned long flags;
	int intr;

	sprintf(current->comm, "drbd%d_worker", (int)(mdev-drbd_conf));

	mdev->resync_timer.function = resync_timer_fn;
	mdev->resync_timer.data = (unsigned long) mdev;
	
	for (;;) {
		intr = down_interruptible(&mdev->data.work.s);

		if (intr) {
			D_ASSERT(intr == -EINTR);
			LOCK_SIGMASK(current,flags);
			if (sigismember(&current->pending.signal, SIGTERM)) {
				sigdelset(&current->pending.signal, SIGTERM);
				RECALC_SIGPENDING(current);
			}
			UNLOCK_SIGMASK(current,flags);
			if (thi->t_state != Running )
				break;
			continue;
		}

		if (thi->t_state != Running )
			break;
		if (need_resched())
			schedule();

		w = NULL;
		spin_lock_irq(&mdev->req_lock);
		if (!list_empty(&mdev->data.work.q)) {
			w = list_entry(mdev->data.work.q.next,struct drbd_work,list);
			list_del_init(&w->list);
		}
		spin_unlock_irq(&mdev->req_lock);

		ERR_IF (!w)
			continue; // BUG()... racy up() somewhere ??

		ERR_IF ( !w->cb(mdev,w) )
			break;
	}

	del_timer_sync(&mdev->resync_timer); // just in case... 
	INFO("worker terminated\n");

	return 0;
}
