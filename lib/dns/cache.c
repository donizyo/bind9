/*
 * Copyright (C) 2004-2008  Internet Systems Consortium, Inc. ("ISC")
 * Copyright (C) 1999-2003  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: cache.c,v 1.76.36.2 2008/02/07 23:46:25 tbox Exp $ */

/*! \file */

#include <config.h>

#include <isc/mem.h>
#include <isc/task.h>
#include <isc/time.h>
#include <isc/timer.h>
#include <isc/util.h>

#include <dns/cache.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/events.h>
#include <dns/lib.h>
#include <dns/log.h>
#include <dns/masterdump.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/result.h>

#define CACHE_MAGIC             ISC_MAGIC('$', '$', '$', '$')
#define VALID_CACHE(cache)      ISC_MAGIC_VALID(cache, CACHE_MAGIC)

/*!
 * Control incremental cleaning.
 * DNS_CACHE_MINSIZE is how many bytes is the floor for dns_cache_setcachesize().
 * See also DNS_CACHE_CLEANERINCREMENT
 */
#define DNS_CACHE_MINSIZE               2097152 /*%< Bytes.  2097152 = 2 MB */
/*!
 * Control incremental cleaning.
 * CLEANERINCREMENT is how many nodes are examined in one pass.
 * See also DNS_CACHE_MINSIZE
 */
#define DNS_CACHE_CLEANERINCREMENT      1000U   /*%< Number of nodes. */

/***
 ***    Types
 ***/

/*
 * A cache_cleaner_t encapsulsates the state of the periodic
 * cache cleaning.
 */

typedef struct cache_cleaner cache_cleaner_t;

typedef enum {
	cleaner_s_idle, /*%< Waiting for cleaning-interval to expire. */
	cleaner_s_busy, /*%< Currently cleaning. */
	cleaner_s_done  /*%< Freed enough memory after being overmem. */
} cleaner_state_t;

/*
 * Convenience macros for comprehensive assertion checking.
 */
#define CLEANER_IDLE(c) ((c)->state == cleaner_s_idle)
#define CLEANER_BUSY(c) ((c)->state == cleaner_s_busy)

/*%
 * Accesses to a cache cleaner object are synchronized through
 * task/event serialization, or locked from the cache object.
 */
struct cache_cleaner {
	isc_mutex_t     lock;
	/*%<
	 * Locks overmem.  Note: never allocate memory
	 * while holding this lock - that could lead to deadlock since
	 * the lock is take by water() which is called from the memory
	 * allocator.
	 */

	dns_cache_t     *cache;
	isc_task_t      *task;
	unsigned int    cleaning_interval; /*% The cleaning-interval from
					      named.conf, in seconds. */
	isc_timer_t     *cleaning_timer;

	unsigned int     increment;     /*% Number of names to
					   clean in one increment */
	cleaner_state_t  state;         /*% Idle/Busy. */
	isc_boolean_t    overmem;       /*% The cache is in an overmem state. */
};

/*%
 * The actual cache object.
 */

struct dns_cache {
	/* Unlocked. */
	unsigned int            magic;
	isc_mutex_t             lock;
	isc_mutex_t             filelock;
	isc_mem_t               *mctx;

	/* Locked by 'lock'. */
	int                     references;
	int                     live_tasks;
	dns_rdataclass_t        rdclass;
	dns_db_t                *db;
	cache_cleaner_t         cleaner;
	char                    *db_type;
	int                     db_argc;
	char                    **db_argv;

	/* Locked by 'filelock'. */
	char *                  filename;
	/* Access to the on-disk cache file is also locked by 'filelock'. */

#ifdef LRU_DEBUG
#define DUMP_INTERVAL 30        /* seconds */
	isc_timer_t                    *dump_timer; /* for test */
	isc_time_t                      dump_time; /* for test */
#endif
};

/***
 ***    Functions
 ***/

static isc_result_t
cache_cleaner_init(dns_cache_t *cache, isc_taskmgr_t *taskmgr,
		   isc_timermgr_t *timermgr, cache_cleaner_t *cleaner);

static void
cleaning_timer_action(isc_task_t *task, isc_event_t *event);

static void
cleaner_shutdown_action(isc_task_t *task, isc_event_t *event);

#ifdef LRU_DEBUG
static void
timer_dump(isc_task_t *task, isc_event_t *event);
#endif

#if 0 /* This is no longer needed.  When LRU_TEST is cleaned up,
       * this should be as well.  XXXMLG */
/*%
 * Work out how many nodes can be cleaned in the time between two
 * requests to the nameserver.  Smooth the resulting number and use
 * it as a estimate for the number of nodes to be cleaned in the next
 * iteration.
 */
static void
adjust_increment(cache_cleaner_t *cleaner, unsigned int remaining,
		 isc_time_t *start)
{
	isc_time_t end;
	isc_uint64_t usecs;
	isc_uint64_t new;
	unsigned int pps = dns_pps;
	unsigned int interval;
	unsigned int names;

	/*
	 * Tune for minumum of 100 packets per second (pps).
	 */
	if (pps < 100)
		pps = 100;

	isc_time_now(&end);

	interval = 1000000 / pps; /* Interval between packets in usecs. */
	if (interval == 0)
		interval = 1;

	INSIST(cleaner->increment >= remaining);
	names = cleaner->increment - remaining;
	usecs = isc_time_microdiff(&end, start);

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_CACHE,
		      ISC_LOG_DEBUG(1), "adjust_increment interval=%u "
		      "names=%u usec=%" ISC_PLATFORM_QUADFORMAT "u",
		      interval, names, usecs);

	if (usecs == 0) {
		/*
		 * If we cleaned all the nodes in unmeasurable time
		 * double the number of nodes to be cleaned next time.
		 */
		if (names == cleaner->increment) {
			cleaner->increment *= 2;
			if (cleaner->increment > DNS_CACHE_CLEANERINCREMENT)
				cleaner->increment = DNS_CACHE_CLEANERINCREMENT;
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_CACHE, ISC_LOG_DEBUG(1),
				      "%p:new cleaner->increment = %u\n",
				      cleaner, cleaner->increment);
		}
		return;
	}

	new = (names * interval);
	new /= (usecs * 2);
	if (new == 0)
		new = 1;

	/* Smooth */
	new = (new + cleaner->increment * 7) / 8;

	if (new > DNS_CACHE_CLEANERINCREMENT)
		new = DNS_CACHE_CLEANERINCREMENT;

	cleaner->increment = (unsigned int)new;

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_CACHE,
		      ISC_LOG_DEBUG(1), "%p:new cleaner->increment = %u\n",
		      cleaner, cleaner->increment);
}
#endif

static inline isc_result_t
cache_create_db(dns_cache_t *cache, dns_db_t **db) {
	return (dns_db_create(cache->mctx, cache->db_type, dns_rootname,
			      dns_dbtype_cache, cache->rdclass,
			      cache->db_argc, cache->db_argv, db));
}

isc_result_t
dns_cache_create(isc_mem_t *mctx, isc_taskmgr_t *taskmgr,
		 isc_timermgr_t *timermgr, dns_rdataclass_t rdclass,
		 const char *db_type, unsigned int db_argc, char **db_argv,
		 dns_cache_t **cachep)
{
	isc_result_t result;
	dns_cache_t *cache;
	int i;

	REQUIRE(cachep != NULL);
	REQUIRE(*cachep == NULL);
	REQUIRE(mctx != NULL);

	cache = isc_mem_get(mctx, sizeof(*cache));
	if (cache == NULL)
		return (ISC_R_NOMEMORY);

	cache->mctx = NULL;
	isc_mem_attach(mctx, &cache->mctx);

	result = isc_mutex_init(&cache->lock);
	if (result != ISC_R_SUCCESS)
		goto cleanup_mem;

	result = isc_mutex_init(&cache->filelock);
	if (result != ISC_R_SUCCESS)
		goto cleanup_lock;

	cache->references = 1;
	cache->live_tasks = 0;
	cache->rdclass = rdclass;

	cache->db_type = isc_mem_strdup(mctx, db_type);
	if (cache->db_type == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup_filelock;
	}

	cache->db_argc = db_argc;
	if (cache->db_argc == 0)
		cache->db_argv = NULL;
	else {
		cache->db_argv = isc_mem_get(mctx,
					     cache->db_argc * sizeof(char *));
		if (cache->db_argv == NULL) {
			result = ISC_R_NOMEMORY;
			goto cleanup_dbtype;
		}
		for (i = 0; i < cache->db_argc; i++)
			cache->db_argv[i] = NULL;
		for (i = 0; i < cache->db_argc; i++) {
			cache->db_argv[i] = isc_mem_strdup(mctx, db_argv[i]);
			if (cache->db_argv[i] == NULL) {
				result = ISC_R_NOMEMORY;
				goto cleanup_dbargv;
			}
		}
	}

	cache->db = NULL;
	result = cache_create_db(cache, &cache->db);
	if (result != ISC_R_SUCCESS)
		goto cleanup_dbargv;

	cache->filename = NULL;

	cache->magic = CACHE_MAGIC;

	result = cache_cleaner_init(cache, taskmgr, timermgr, &cache->cleaner);
	if (result != ISC_R_SUCCESS)
		goto cleanup_db;

	*cachep = cache;
	return (ISC_R_SUCCESS);

 cleanup_db:
	dns_db_detach(&cache->db);
 cleanup_dbargv:
	for (i = 0; i < cache->db_argc; i++)
		if (cache->db_argv[i] != NULL)
			isc_mem_free(mctx, cache->db_argv[i]);
	if (cache->db_argv != NULL)
		isc_mem_put(mctx, cache->db_argv,
			    cache->db_argc * sizeof(char *));
 cleanup_dbtype:
	isc_mem_free(mctx, cache->db_type);
 cleanup_filelock:
	DESTROYLOCK(&cache->filelock);
 cleanup_lock:
	DESTROYLOCK(&cache->lock);
 cleanup_mem:
	isc_mem_put(mctx, cache, sizeof(*cache));
	isc_mem_detach(&mctx);
	return (result);
}

static void
cache_free(dns_cache_t *cache) {
	isc_mem_t *mctx;
	int i;

	REQUIRE(VALID_CACHE(cache));
	REQUIRE(cache->references == 0);

	isc_mem_setwater(cache->mctx, NULL, NULL, 0, 0);

	if (cache->cleaner.task != NULL)
		isc_task_detach(&cache->cleaner.task);

	DESTROYLOCK(&cache->cleaner.lock);

	if (cache->filename) {
		isc_mem_free(cache->mctx, cache->filename);
		cache->filename = NULL;
	}

	if (cache->db != NULL)
		dns_db_detach(&cache->db);

	if (cache->db_argv != NULL) {
		for (i = 0; i < cache->db_argc; i++)
			if (cache->db_argv[i] != NULL)
				isc_mem_free(cache->mctx, cache->db_argv[i]);
		isc_mem_put(cache->mctx, cache->db_argv,
			    cache->db_argc * sizeof(char *));
	}

	if (cache->db_type != NULL)
		isc_mem_free(cache->mctx, cache->db_type);

	DESTROYLOCK(&cache->lock);
	DESTROYLOCK(&cache->filelock);
	cache->magic = 0;
	mctx = cache->mctx;
	isc_mem_put(cache->mctx, cache, sizeof(*cache));
	isc_mem_detach(&mctx);
}


void
dns_cache_attach(dns_cache_t *cache, dns_cache_t **targetp) {

	REQUIRE(VALID_CACHE(cache));
	REQUIRE(targetp != NULL && *targetp == NULL);

	LOCK(&cache->lock);
	cache->references++;
	UNLOCK(&cache->lock);

	*targetp = cache;
}

void
dns_cache_detach(dns_cache_t **cachep) {
	dns_cache_t *cache;
	isc_boolean_t free_cache = ISC_FALSE;

	REQUIRE(cachep != NULL);
	cache = *cachep;
	REQUIRE(VALID_CACHE(cache));

	LOCK(&cache->lock);
	REQUIRE(cache->references > 0);
	cache->references--;
	if (cache->references == 0) {
		cache->cleaner.overmem = ISC_FALSE;
		free_cache = ISC_TRUE;
	}

	*cachep = NULL;

	if (free_cache) {
		/*
		 * When the cache is shut down, dump it to a file if one is
		 * specified.
		 */
		isc_result_t result = dns_cache_dump(cache);
		if (result != ISC_R_SUCCESS)
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_CACHE, ISC_LOG_WARNING,
				      "error dumping cache: %s ",
				      isc_result_totext(result));

		/*
		 * If the cleaner task exists, let it free the cache.
		 */
		if (cache->live_tasks > 0) {
			isc_task_shutdown(cache->cleaner.task);
			free_cache = ISC_FALSE;
		}
	}

	UNLOCK(&cache->lock);

	if (free_cache)
		cache_free(cache);
}

void
dns_cache_attachdb(dns_cache_t *cache, dns_db_t **dbp) {
	REQUIRE(VALID_CACHE(cache));
	REQUIRE(dbp != NULL && *dbp == NULL);
	REQUIRE(cache->db != NULL);

	LOCK(&cache->lock);
	dns_db_attach(cache->db, dbp);
	UNLOCK(&cache->lock);

}

isc_result_t
dns_cache_setfilename(dns_cache_t *cache, const char *filename) {
	char *newname;

	REQUIRE(VALID_CACHE(cache));
	REQUIRE(filename != NULL);

	newname = isc_mem_strdup(cache->mctx, filename);
	if (newname == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&cache->filelock);
	if (cache->filename)
		isc_mem_free(cache->mctx, cache->filename);
	cache->filename = newname;
	UNLOCK(&cache->filelock);

	return (ISC_R_SUCCESS);
}

isc_result_t
dns_cache_load(dns_cache_t *cache) {
	isc_result_t result;

	REQUIRE(VALID_CACHE(cache));

	if (cache->filename == NULL)
		return (ISC_R_SUCCESS);

	LOCK(&cache->filelock);
	result = dns_db_load(cache->db, cache->filename);
	UNLOCK(&cache->filelock);

	return (result);
}

isc_result_t
dns_cache_dump(dns_cache_t *cache) {
	isc_result_t result;

	REQUIRE(VALID_CACHE(cache));

	if (cache->filename == NULL)
		return (ISC_R_SUCCESS);

	LOCK(&cache->filelock);
	result = dns_master_dump(cache->mctx, cache->db, NULL,
				 &dns_master_style_cache, cache->filename);
	UNLOCK(&cache->filelock);

	return (result);
}

void
dns_cache_setcleaninginterval(dns_cache_t *cache, unsigned int t) {
	isc_interval_t interval;
	isc_result_t result;

	LOCK(&cache->lock);

	/*
	 * It may be the case that the cache has already shut down.
	 * If so, it has no timer.
	 */
	if (cache->cleaner.cleaning_timer == NULL)
		goto unlock;

	cache->cleaner.cleaning_interval = t;

	if (t == 0) {
		result = isc_timer_reset(cache->cleaner.cleaning_timer,
					 isc_timertype_inactive,
					 NULL, NULL, ISC_TRUE);
	} else {
		isc_interval_set(&interval, cache->cleaner.cleaning_interval,
				 0);
		result = isc_timer_reset(cache->cleaner.cleaning_timer,
					 isc_timertype_ticker,
					 NULL, &interval, ISC_FALSE);
	}
	if (result != ISC_R_SUCCESS)
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_CACHE, ISC_LOG_WARNING,
			      "could not set cache cleaning interval: %s",
			      isc_result_totext(result));

 unlock:
	UNLOCK(&cache->lock);
}

/*
 * Initialize the cache cleaner object at *cleaner.
 * Space for the object must be allocated by the caller.
 */

static isc_result_t
cache_cleaner_init(dns_cache_t *cache, isc_taskmgr_t *taskmgr,
		   isc_timermgr_t *timermgr, cache_cleaner_t *cleaner)
{
	isc_result_t result;
#ifdef LRU_DEBUG
	isc_interval_t interval;
#endif

	result = isc_mutex_init(&cleaner->lock);
	if (result != ISC_R_SUCCESS)
		goto fail;

	cleaner->increment = DNS_CACHE_CLEANERINCREMENT;
	cleaner->state = cleaner_s_idle;
	cleaner->cache = cache;
	cleaner->overmem = ISC_FALSE;

	cleaner->task = NULL;
	cleaner->cleaning_timer = NULL;

	if (taskmgr != NULL && timermgr != NULL) {
		result = isc_task_create(taskmgr, 1, &cleaner->task);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_task_create() failed: %s",
					 dns_result_totext(result));
			result = ISC_R_UNEXPECTED;
			goto cleanup;
		}
		cleaner->cache->live_tasks++;
		isc_task_setname(cleaner->task, "cachecleaner", cleaner);

		result = isc_task_onshutdown(cleaner->task,
					     cleaner_shutdown_action, cache);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "cache cleaner: "
					 "isc_task_onshutdown() failed: %s",
					 dns_result_totext(result));
			goto cleanup;
		}

		cleaner->cleaning_interval = 0; /* Initially turned off. */
		result = isc_timer_create(timermgr, isc_timertype_inactive,
					   NULL, NULL,
					   cleaner->task,
					   cleaning_timer_action, cleaner,
					   &cleaner->cleaning_timer);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_timer_create() failed: %s",
					 dns_result_totext(result));
			result = ISC_R_UNEXPECTED;
			goto cleanup;
		}

#ifdef LRU_DEBUG
		interval.seconds = DUMP_INTERVAL;
		interval.nanoseconds = 0;
		RUNTIME_CHECK(isc_time_nowplusinterval(&cache->dump_time,
						       &interval) ==
			      ISC_R_SUCCESS);
		cache->dump_timer = NULL;
		result = isc_timer_create(timermgr, isc_timertype_once,
					  &cache->dump_time, NULL,
					  cleaner->task, timer_dump,
					  cache, &cache->dump_timer);
		RUNTIME_CHECK(result == ISC_R_SUCCESS); /* for brevity */
#endif
	}

	return (ISC_R_SUCCESS);

 cleanup:
	if (cleaner->cleaning_timer != NULL)
		isc_timer_detach(&cleaner->cleaning_timer);
	if (cleaner->task != NULL)
		isc_task_detach(&cleaner->task);
	DESTROYLOCK(&cleaner->lock);
 fail:
	return (result);
}

/*
 * This is run once for every cache-cleaning-interval as defined in named.conf.
 */
static void
cleaning_timer_action(isc_task_t *task, isc_event_t *event) {
	cache_cleaner_t *cleaner = event->ev_arg;

	UNUSED(task);

	INSIST(task == cleaner->task);
	INSIST(event->ev_type == ISC_TIMEREVENT_TICK);

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_CACHE,
		      ISC_LOG_DEBUG(1), "cache cleaning timer fired, "
		      "cleaner state = %d", cleaner->state);

	isc_event_free(&event);
}

/*
 * Do immediate cleaning.
 */
isc_result_t
dns_cache_clean(dns_cache_t *cache, isc_stdtime_t now) {
	isc_result_t result;
	dns_dbiterator_t *iterator = NULL;

	REQUIRE(VALID_CACHE(cache));

	result = dns_db_createiterator(cache->db, ISC_FALSE, &iterator);
	if (result != ISC_R_SUCCESS)
		return result;

	result = dns_dbiterator_first(iterator);

	while (result == ISC_R_SUCCESS) {
		dns_dbnode_t *node = NULL;
		result = dns_dbiterator_current(iterator, &node,
						(dns_name_t *)NULL);
		if (result != ISC_R_SUCCESS)
			break;

		/*
		 * Check TTLs, mark expired rdatasets stale.
		 */
		result = dns_db_expirenode(cache->db, node, now);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "cache cleaner: dns_db_expirenode() "
					 "failed: %s",
					 dns_result_totext(result));
			/*
			 * Continue anyway.
			 */
		}

		/*
		 * This is where the actual freeing takes place.
		 */
		dns_db_detachnode(cache->db, &node);

		result = dns_dbiterator_next(iterator);
	}

	dns_dbiterator_destroy(&iterator);

	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

	return (result);
}

static void
water(void *arg, int mark) {
	dns_cache_t *cache = arg;
	isc_boolean_t overmem = ISC_TF(mark == ISC_MEM_HIWATER);

	REQUIRE(VALID_CACHE(cache));

	LOCK(&cache->cleaner.lock);

	if (overmem != cache->cleaner.overmem) {
		dns_db_overmem(cache->db, overmem);
		cache->cleaner.overmem = overmem;
		isc_mem_waterack(cache->mctx, mark);
	}

	UNLOCK(&cache->cleaner.lock);
}

void
dns_cache_setcachesize(dns_cache_t *cache, isc_uint32_t size) {
	isc_uint32_t lowater;
	isc_uint32_t hiwater;

	REQUIRE(VALID_CACHE(cache));

	/*
	 * Impose a minumum cache size; pathological things happen if there
	 * is too little room.
	 */
	if (size != 0 && size < DNS_CACHE_MINSIZE)
		size = DNS_CACHE_MINSIZE;

	hiwater = size - (size >> 3);   /* Approximately 7/8ths. */
	lowater = size - (size >> 2);   /* Approximately 3/4ths. */

	/*
	 * If the cache was overmem and cleaning, but now with the new limits
	 * it is no longer in an overmem condition, then the next
	 * isc_mem_put for cache memory will do the right thing and trigger
	 * water().
	 */

	if (size == 0 || hiwater == 0 || lowater == 0)
		/*
		 * Disable cache memory limiting.
		 */
		isc_mem_setwater(cache->mctx, water, cache, 0, 0);
	else
		/*
		 * Establish new cache memory limits (either for the first
		 * time, or replacing other limits).
		 */
		isc_mem_setwater(cache->mctx, water, cache, hiwater, lowater);
}

/*
 * The cleaner task is shutting down; do the necessary cleanup.
 */
static void
cleaner_shutdown_action(isc_task_t *task, isc_event_t *event) {
	dns_cache_t *cache = event->ev_arg;
	isc_boolean_t should_free = ISC_FALSE;

	UNUSED(task);

	INSIST(task == cache->cleaner.task);
	INSIST(event->ev_type == ISC_TASKEVENT_SHUTDOWN);

	LOCK(&cache->lock);

	cache->live_tasks--;
	INSIST(cache->live_tasks == 0);

	if (cache->references == 0)
		should_free = ISC_TRUE;

	/*
	 * By detaching the timer in the context of its task,
	 * we are guaranteed that there will be no further timer
	 * events.
	 */
	if (cache->cleaner.cleaning_timer != NULL)
		isc_timer_detach(&cache->cleaner.cleaning_timer);

#ifdef LRU_DEBUG
	isc_timer_detach(&cache->dump_timer);
#endif

	/* Make sure we don't reschedule anymore. */
	(void)isc_task_purge(task, NULL, DNS_EVENT_CACHECLEAN, NULL);

	UNLOCK(&cache->lock);

	if (should_free)
		cache_free(cache);

	isc_event_free(&event);
}

isc_result_t
dns_cache_flush(dns_cache_t *cache) {
	dns_db_t *db = NULL;
	isc_result_t result;

	result = cache_create_db(cache, &db);
	if (result != ISC_R_SUCCESS)
		return (result);

	LOCK(&cache->lock);
	LOCK(&cache->cleaner.lock);
	if (cache->cleaner.state == cleaner_s_idle) {
		/* XXXMLG do something */
	} else if (cache->cleaner.state == cleaner_s_busy) {
		/* XXXMLG do something else */
	}
	dns_db_detach(&cache->db);
	cache->db = db;
	UNLOCK(&cache->cleaner.lock);
	UNLOCK(&cache->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
dns_cache_flushname(dns_cache_t *cache, dns_name_t *name) {
	isc_result_t result;
	dns_rdatasetiter_t *iter = NULL;
	dns_dbnode_t *node = NULL;
	dns_db_t *db = NULL;

	LOCK(&cache->lock);
	if (cache->db != NULL)
		dns_db_attach(cache->db, &db);
	UNLOCK(&cache->lock);
	if (db == NULL)
		return (ISC_R_SUCCESS);
	result = dns_db_findnode(cache->db, name, ISC_FALSE, &node);
	if (result == ISC_R_NOTFOUND) {
		result = ISC_R_SUCCESS;
		goto cleanup_db;
	}
	if (result != ISC_R_SUCCESS)
		goto cleanup_db;

	result = dns_db_allrdatasets(cache->db, node, NULL,
				     (isc_stdtime_t)0, &iter);
	if (result != ISC_R_SUCCESS)
		goto cleanup_node;

	for (result = dns_rdatasetiter_first(iter);
	     result == ISC_R_SUCCESS;
	     result = dns_rdatasetiter_next(iter))
	{
		dns_rdataset_t rdataset;
		dns_rdataset_init(&rdataset);

		dns_rdatasetiter_current(iter, &rdataset);
		result = dns_db_deleterdataset(cache->db, node, NULL,
					       rdataset.type, rdataset.covers);
		dns_rdataset_disassociate(&rdataset);
		if (result != ISC_R_SUCCESS && result != DNS_R_UNCHANGED)
			break;
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

	dns_rdatasetiter_destroy(&iter);

 cleanup_node:
	dns_db_detachnode(cache->db, &node);

 cleanup_db:
	dns_db_detach(&db);
	return (result);
}

#ifdef LRU_DEBUG
static void
timer_dump(isc_task_t *task, isc_event_t *event) {
	dns_cache_t *cache;
	isc_interval_t interval;
	isc_time_t nexttime;

	UNUSED(task);

	cache = event->ev_arg;
	INSIST(VALID_CACHE(cache));

#ifdef LRU_DEBUG
	/* XXX: abuse existing overmem method */
	dns_db_overmem(cache->db, (isc_boolean_t)-1);
#endif

	interval.seconds = DUMP_INTERVAL;
	interval.nanoseconds = 0;

	RUNTIME_CHECK(isc_time_add(&cache->dump_time, &interval, &nexttime) ==
		      ISC_R_SUCCESS); /* XXX: this is not always true */
	cache->dump_time = nexttime;
	(void)isc_timer_reset(cache->dump_timer, isc_timertype_once,
			      &cache->dump_time, NULL, ISC_FALSE);

	isc_event_free(&event);
}
#endif
