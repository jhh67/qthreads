#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <stdlib.h>		       /* for malloc() and abort() */
#ifndef QTHREAD_NO_ASSERTS
# include <assert.h>		       /* for assert() */
#endif
#if defined(HAVE_UCONTEXT_H) && defined(HAVE_CONTEXT_FUNCS)
# include <ucontext.h>		       /* for make/get/swap-context functions */
#else
# include "taskimpl.h"
#endif
#include <stdarg.h>		       /* for va_start and va_end */
#include <stdint.h>		       /* for UINT8_MAX */
#include <string.h>		       /* for memset() */
#if !HAVE_MEMCPY
# define memcpy(d, s, n) bcopy((s), (d), (n))
# define memmove(d, s, n) bcopy((s), (d), (n))
#endif
#ifdef NEED_RLIMIT
# include <sys/time.h>
# include <sys/resource.h>
#endif

#include <cprops/mempool.h>
#include <cprops/hashtable.h>
#include <cprops/linked_list.h>
#include "qthread/qthread.h"
#include "futurelib_innards.h"

#ifndef UINT8_MAX
#define UINT8_MAX (255)
#endif

#define MACHINEMASK (~(WORDSIZE-1))

/* If __USE_FILE_OFFSET64, and NEED_RLIMIT, and we don't have __REDIRECT, we
 * #define rlimit to rlimit64, and its prototype returns a 'struct rlimit64 *',
 * but the user's code expects to be able to designate it by 'struct rlimit *'.
 */
#if defined(__USE_FILE_OFFSET64) && defined(NEED_RLIMIT) && ! defined(__REDIRECT)
# define rlimit rlimit64
#endif

#ifdef QTHREAD_DEBUG
/* for the vprintf in qthread_debug() */
/* 8MB stack */
/* unless you're doing some limit testing with very small stacks, the stack
 * size MUST be a multiple of the page size */
# define QTHREAD_DEFAULT_STACK_SIZE 4096*2048
#else
# ifdef REALLY_SMALL_STACKS
#  define QTHREAD_DEFAULT_STACK_SIZE 2048
# else
#  define QTHREAD_DEFAULT_STACK_SIZE 4096
# endif
#endif

/* internal constants */
#define QTHREAD_STATE_NEW               0
#define QTHREAD_STATE_RUNNING           1
#define QTHREAD_STATE_YIELDED           2
#define QTHREAD_STATE_BLOCKED           3
#define QTHREAD_STATE_FEB_BLOCKED       4
#define QTHREAD_STATE_TERMINATED        5
#define QTHREAD_STATE_DONE              6
#define QTHREAD_STATE_TERM_SHEP         UINT8_MAX
#define QTHREAD_THREAD                  0
#define QTHREAD_FUTURE                  1

#if defined(UNPOOLED_QTHREAD_T) || defined(UNPOOLED)
#define ALLOC_QTHREAD(shep) (qthread_t *) malloc(sizeof(qthread_t))
#define FREE_QTHREAD(shep, t) free(t)
#else
#define ALLOC_QTHREAD(shep) (qthread_t *) cp_mempool_alloc(shep?(shep->qthread_pool):generic_qthread_pool)
#define FREE_QTHREAD(shep, t) cp_mempool_free(shep?(shep->qthread_pool):generic_qthread_pool, t)
#endif

#if defined(UNPOOLED_STACKS) || defined(UNPOOLED)
#define ALLOC_STACK(shep) malloc(qlib->qthread_stack_size)
#define FREE_STACK(shep, t) free(t)
#else
#define ALLOC_STACK(shep) cp_mempool_alloc(shep?(shep->stack_pool):generic_stack_pool)
#define FREE_STACK(shep, t) cp_mempool_free(shep?(shep->stack_pool):generic_stack_pool, t)
#endif

#if defined(UNPOOLED_CONTEXTS) || defined(UNPOOLED)
#define ALLOC_CONTEXT(shep) (ucontext_t *) malloc(sizeof(ucontext_t))
#define FREE_CONTEXT(shep, t) free(t)
#else
#define ALLOC_CONTEXT(shep) (ucontext_t *) cp_mempool_alloc(shep?(shep->context_pool):generic_context_pool)
#define FREE_CONTEXT(shep, t) cp_mempool_free(shep?(shep->context_pool):generic_context_pool, t)
#endif

#if defined(UNPOOLED_QUEUES) || defined(UNPOOLED)
#define ALLOC_QUEUE(shep) (qthread_queue_t *) malloc(sizeof(qthread_queue_t))
#define FREE_QUEUE(shep, t) free(t)
#else
#define ALLOC_QUEUE(shep) (qthread_queue_t *) cp_mempool_alloc(shep?(shep->queue_pool):generic_queue_pool)
#define FREE_QUEUE(shep, t) cp_mempool_free(shep?(shep->queue_pool):generic_queue_pool, t)
#endif

#if defined(UNPOOLED_LOCKS) || defined(UNPOOLED)
#define ALLOC_LOCK(shep) (qthread_lock_t *) malloc(sizeof(qthread_lock_t))
#define FREE_LOCK(shep, t) free(t)
#else
#define ALLOC_LOCK(shep) (qthread_lock_t *) cp_mempool_alloc(shep?(shep->lock_pool):generic_lock_pool)
#define FREE_LOCK(shep, t) cp_mempool_free(shep?(shep->lock_pool):generic_lock_pool, t)
#endif

#if defined(UNPOOLED_ADDRRES) || defined(UNPOOLED)
#define ALLOC_ADDRRES(shep) (qthread_addrres_t *) malloc(sizeof(qthread_addrres_t))
#define FREE_ADDRRES(shep, t) free(t)
#else
#define ALLOC_ADDRRES(shep) (qthread_addrres_t *) cp_mempool_alloc(shep->addrres_pool)
#define FREE_ADDRRES(shep, t) cp_mempool_free(shep->addrres_pool, t)
#endif

#if defined(UNPOOLED_ADDRSTAT) || defined(UNPOOLED)
#define ALLOC_ADDRSTAT(shep) (qthread_addrstat_t *) malloc(sizeof(qthread_addrstat_t))
#define FREE_ADDRSTAT(shep, t) free(t)
#else
#define ALLOC_ADDRSTAT(shep) (qthread_addrstat_t *) cp_mempool_alloc(shep->addrstat_pool)
#define FREE_ADDRSTAT(shep, t) cp_mempool_free(shep->addrstat_pool, t)
#endif

#if defined(UNPOOLED_ADDRSTAT2) || defined(UNPOOLED)
#define ALLOC_ADDRSTAT2(shep) (qthread_addrstat2_t *) malloc(sizeof(qthread_addrstat2_t))
#define FREE_ADDRSTAT2(shep, t) free(t)
#else
#define ALLOC_ADDRSTAT2(shep) (qthread_addrstat2_t *) cp_mempool_alloc(shep->addrstat2_pool)
#define FREE_ADDRSTAT2(shep, t) cp_mempool_free(shep->addrstat2_pool, t)
#endif

#define ALIGN(d, s, f) do { \
    s = (aligned_t *) (((size_t) d) & MACHINEMASK); \
    if (s != d) { \
	fprintf(stderr, \
		"WARNING: " f ": unaligned address %p ... assuming %p\n", \
		(void *) d, (void *) s); \
    } \
} while(0)

#ifdef DEBUG_DEADLOCK
#define REPORTLOCK(m) printf("%i:%i LOCKED %p's LOCK!\n", qthread_shep(NULL), __LINE__, m)
#define REPORTUNLOCK(m) printf("%i:%i UNLOCKED %p's LOCK!\n", qthread_shep(NULL), __LINE__, m)
#else
#define REPORTLOCK(m)
#define REPORTUNLOCK(m)
#endif

/* internal data structures */
typedef struct qthread_lock_s qthread_lock_t;
typedef struct qthread_shepherd_s qthread_shepherd_t;
typedef struct qthread_queue_s qthread_queue_t;

struct qthread_s
{
    unsigned int thread_id;
    unsigned char thread_state;
    unsigned char future_flags;

    /* the shepherd (pthread) we run on */
    qthread_shepherd_t * shepherd_ptr;
    /* a pointer used for passing information back to the shepherd when
     * becoming blocked */
    struct qthread_lock_s *blockedon;

    /* the function to call (that defines this thread) */
    qthread_f f;
    void *arg;			/* user defined data */
    aligned_t *ret;		/* user defined retval location */

    ucontext_t *context;	/* the context switch info */
    void *stack;		/* the thread's stack */
    ucontext_t *return_context;	/* context of parent kthread */

    struct qthread_s *next;
};

struct qthread_queue_s
{
    qthread_t *head;
    qthread_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t notempty;
};

struct qthread_shepherd_s
{
    pthread_t kthread;
    unsigned kthread_index;
    qthread_t *current;
    qthread_queue_t *ready;
    cp_mempool *qthread_pool;
    cp_mempool *list_pool;
    cp_mempool *queue_pool;
    cp_mempool *lock_pool;
    cp_mempool *addrres_pool;
    cp_mempool *addrstat_pool;
    cp_mempool *addrstat2_pool;
    cp_mempool *stack_pool;
    cp_mempool *context_pool;
    /* round robin scheduler - can probably be smarter */
    qthread_shepherd_id_t sched_kthread;
};

struct qthread_lock_s
{
    unsigned owner;
    pthread_mutex_t lock;
    qthread_queue_t *waiting;
};

typedef struct qthread_addrres_s
{
    aligned_t *addr;		/* ptr to the memory NOT being blocked on */
    qthread_t *waiter;
    struct qthread_addrres_s *next;
} qthread_addrres_t;

typedef struct qthread_addrstat_s
{
    pthread_mutex_t lock;
    qthread_addrres_t *EFQ;
    qthread_addrres_t *FEQ;
    qthread_addrres_t *FFQ;
    unsigned int full:1;
} qthread_addrstat_t;

typedef struct qthread_addrstat2_s
{
    qthread_addrstat_t *m;
    aligned_t *addr;
    struct qthread_addrstat2_s *next;
} qthread_addrstat2_t;

typedef struct
{
    int nkthreads;
    struct qthread_shepherd_s *kthreads;

    unsigned qthread_stack_size;
    unsigned master_stack_size;
    unsigned max_stack_size;

    /* assigns a unique thread_id mostly for debugging! */
    unsigned max_thread_id;
    pthread_mutex_t max_thread_id_lock;

    /* round robin scheduler - can probably be smarter */
    qthread_shepherd_id_t sched_kthread;
    pthread_mutex_t sched_kthread_lock;

    /* this is how we manage FEB-type locks
     * NOTE: this can be a major bottleneck and we should probably create
     * multiple hashtables to improve performance. The current hashing is a bit
     * of a hack, but improves the bottleneck a bit
     */
    cp_hashtable *locks[32];
    /* these are separated out for memory reasons: if you can get away with
     * simple locks, then you can use less memory. Subject to the same
     * bottleneck concerns as the above hashtable, though these are slightly
     * better at shrinking their critical section. FEBs have more memory
     * overhead, though. */
    cp_hashtable *FEBs;
} qlib_t;

pthread_key_t shepherd_structs;

/* internal globals */
static qlib_t *qlib = NULL;
static cp_mempool *generic_qthread_pool = NULL;
static cp_mempool *generic_stack_pool = NULL;
static cp_mempool *generic_context_pool = NULL;
static cp_mempool *generic_queue_pool = NULL;
static cp_mempool *generic_lock_pool = NULL;

/* Internal functions */
static void qthread_wrapper(void *arg);

static void qthread_FEBlock_delete(qthread_addrstat_t * m);
static inline qthread_t *qthread_thread_new(const qthread_f f,
					    const void *arg, aligned_t * ret,
					    const qthread_shepherd_id_t
					    shepherd);
static inline void qthread_thread_free(qthread_t * t);
static inline qthread_queue_t *qthread_queue_new(qthread_shepherd_t *
						 shepherd);
static inline void qthread_queue_free(qthread_queue_t * q,
				      qthread_shepherd_t * shepherd);
static inline void qthread_enqueue(qthread_queue_t * q, qthread_t * t);
static inline qthread_t *qthread_dequeue(qthread_queue_t * q);
static inline qthread_t *qthread_dequeue_nonblocking(qthread_queue_t * q);
static inline void qthread_exec(qthread_t * t, ucontext_t * c);
static inline void qthread_back_to_master(qthread_t * t);
static inline void qthread_gotlock_fill(qthread_addrstat_t * m, void *maddr,
					const qthread_shepherd_t *
					threadshep, const char recursive);
static inline void qthread_gotlock_empty(qthread_addrstat_t * m, void *maddr,
					 const qthread_shepherd_t *
					 threadshep, const char recursive);

#ifdef QTHREAD_NO_ASSERTS
#define qassert(op, val) op
#define assert(foo)
#else
#define qassert(op, val) assert(op == val)
#endif

#define QTHREAD_LOCK(l) qassert(pthread_mutex_lock(l), 0)
#define QTHREAD_UNLOCK(l) qassert(pthread_mutex_unlock(l), 0)
#define QTHREAD_INITLOCK(l) qassert(pthread_mutex_init(l, NULL), 0)
#define QTHREAD_DESTROYLOCK(l) qassert(pthread_mutex_destroy(l), 0)
#define QTHREAD_INITCOND(l) qassert(pthread_cond_init(l, NULL), 0)
#define QTHREAD_DESTROYCOND(l) qassert(pthread_cond_destroy(l), 0)
#define QTHREAD_SIGNAL(l) qassert(pthread_cond_signal(l), 0)
#define QTHREAD_CONDWAIT(c, l) qassert(pthread_cond_wait(c, l), 0)

#define ATOMIC_INC(r, x, l) do { \
    QTHREAD_LOCK(l); \
    r = *(x); \
    *(x) += 1; \
    QTHREAD_UNLOCK(l); \
} while (0)

#define ATOMIC_INC_MOD(r, x, l, m) do {\
    QTHREAD_LOCK(l); \
    r = *(x); \
    if (*(x) + 1 < (m)) { \
	*(x) += 1; \
    } else { \
	*(x) = 0; \
    } \
    QTHREAD_UNLOCK(l); \
} while (0)

#if 0				       /* currently not used */
static inline unsigned qthread_internal_atomic_inc(unsigned *x,
						   pthread_mutex_t * lock)
{				       /*{{{ */
    unsigned r;

    QTHREAD_LOCK(lock);
    r = *x;
    *x++;
    QTHREAD_UNLOCK(lock);
    return (r);
}				       /*}}} */

static inline unsigned qthread_internal_atomic_inc_mod(unsigned *x,
						       pthread_mutex_t * lock,
						       int mod)
{				       /*{{{ */
    unsigned r;

    QTHREAD_LOCK(lock);
    r = *x;
    if (*x + 1 < mod) {
	*x++;
    } else {
	*x = 0;
    }
    QTHREAD_UNLOCK(lock);
    return (r);
}				       /*}}} */

static inline unsigned qthread_internal_atomic_check(unsigned *x,
						     pthread_mutex_t * lock)
{				       /*{{{ */
    unsigned r;

    QTHREAD_LOCK(lock);
    r = *x;
    QTHREAD_UNLOCK(lock);

    return (r);
}				       /*}}} */
#endif

/*#define QTHREAD_DEBUG 1*/
/* for debugging */
#ifdef QTHREAD_DEBUG
static inline void qthread_debug(char *format, ...)
{				       /*{{{ */
    static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
    va_list args;

    QTHREAD_LOCK(&output_lock);

    fprintf(stderr, "qthread_debug(): ");

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);		       /* KBW: helps keep things straight */

    QTHREAD_UNLOCK(&output_lock);
}				       /*}}} */
#define QTHREAD_NONLAZY_THREADIDS
#else
#define qthread_debug(...) do{ }while(0)
#endif

/* the qthread_shepherd() is the pthread responsible for actually
 * executing the work units
 *
 * this function is the workhorse of the library: this is the function that
 * gets spawned several times and runs all the qthreads. */
static void *qthread_shepherd(void *arg)
{				       /*{{{ */
    qthread_shepherd_t *me = (qthread_shepherd_t *) arg;
    ucontext_t my_context;
    qthread_t *t;
    int done = 0;

    qthread_debug("qthread_shepherd(%u): forked\n", me->kthread_index);

    pthread_setspecific(shepherd_structs, arg);
    while (!done) {
	t = qthread_dequeue(me->ready);

	qthread_debug
	    ("qthread_shepherd(%u): dequeued thread id %d/state %d\n",
	     me->kthread_index, t->thread_id, t->thread_state);

	if (t->thread_state == QTHREAD_STATE_TERM_SHEP) {
	    done = 1;
	    qthread_thread_free(t);
	} else {
	    assert((t->thread_state == QTHREAD_STATE_NEW) ||
		   (t->thread_state == QTHREAD_STATE_RUNNING));

	    assert(t->f != NULL);

	    /* note: there's a good argument that the following should
	     * be: (*t->f)(t), however the state management would be
	     * more complex 
	     */

	    assert(t->shepherd_ptr == me);
	    me->current = t;
	    qthread_exec(t, &my_context);
	    me->current = NULL;
	    qthread_debug("qthread_shepherd(%u): back from qthread_exec\n",
			  me->kthread_index);
	    switch (t->thread_state) {
		case QTHREAD_STATE_YIELDED:	/* reschedule it */
		    qthread_debug
			("qthread_shepherd(%u): rescheduling thread %p\n",
			 me->kthread_index, t);
		    t->thread_state = QTHREAD_STATE_RUNNING;
		    qthread_enqueue(t->shepherd_ptr->ready, t);
		    break;

		case QTHREAD_STATE_FEB_BLOCKED:	/* unlock the related FEB address locks, and re-arrange memory to be correct */
		    qthread_debug
			("qthread_shepherd(%u): unlocking FEB address locks of thread %p\n",
			 me->kthread_index, t);
		    t->thread_state = QTHREAD_STATE_BLOCKED;
		    QTHREAD_UNLOCK(&
				   (((qthread_addrstat_t *) (t->blockedon))->
				    lock));
		    REPORTUNLOCK(t->blockedon);
		    break;

		case QTHREAD_STATE_BLOCKED:	/* put it in the blocked queue */
		    qthread_debug
			("qthread_shepherd(%u): adding blocked thread %p to blocked queue\n",
			 me->kthread_index, t);
		    qthread_enqueue((qthread_queue_t *) t->blockedon->waiting,
				    t);
		    QTHREAD_UNLOCK(&(t->blockedon->lock));
		    break;

		case QTHREAD_STATE_TERMINATED:
		    qthread_debug
			("qthread_shepherd(%u): thread %p is in state terminated.\n",
			 me->kthread_index, t);
		    t->thread_state = QTHREAD_STATE_DONE;
		    /* we can remove the stack and the context... */
		    if (t->context) {
			FREE_CONTEXT(me, t->context);
			t->context = NULL;
		    }
		    if (t->stack != NULL) {
			FREE_STACK(me, t->stack);
			t->stack = NULL;
		    }
		    qthread_thread_free(t);
		    break;
	    }
	}
    }

    qthread_debug("qthread_shepherd(%u): finished\n", me->kthread_index);
    pthread_exit(NULL);
    return NULL;
}				       /*}}} */

int qthread_init(const qthread_shepherd_id_t nkthreads)
{				       /*{{{ */
    int r;
    size_t i;

#ifdef NEED_RLIMIT
    struct rlimit rlp;
#endif

    qthread_debug("qthread_init(): began.\n");

    if ((qlib = (qlib_t *) malloc(sizeof(qlib_t))) == NULL) {
	perror("qthread_init()");
	abort();
    }

    /* initialize the FEB-like locking structures */

    /* this is synchronized with read/write locks by default */
    for (i = 0; i < 32; i++) {
	if ((qlib->locks[i] =
	     cp_hashtable_create(100, cp_hash_addr,
				 cp_hash_compare_addr)) == NULL) {
	    perror("qthread_init()");
	    abort();
	}
    }
    if ((qlib->FEBs =
	 cp_hashtable_create_by_option(COLLECTION_MODE_DEEP, 100,
				       cp_hash_addr, cp_hash_compare_addr,
				       NULL, NULL, NULL, NULL)) == NULL) {
	perror("qthread_init()");
	abort();
    }

    /* initialize the kernel threads and scheduler */
    pthread_key_create(&shepherd_structs, NULL);
    qlib->nkthreads = nkthreads;
    if ((qlib->kthreads = (qthread_shepherd_t *)
	 malloc(sizeof(qthread_shepherd_t) * nkthreads)) == NULL) {
	perror("qthread_init()");
	abort();
    }

    qlib->qthread_stack_size = QTHREAD_DEFAULT_STACK_SIZE;
    qlib->max_thread_id = 0;
    qlib->sched_kthread = 0;
    QTHREAD_INITLOCK(&qlib->max_thread_id_lock);
    QTHREAD_INITLOCK(&qlib->sched_kthread_lock);

#ifdef NEED_RLIMIT
    qassert(getrlimit(RLIMIT_STACK, &rlp), 0);
    qthread_debug("stack sizes ... cur: %u max: %u\n", rlp.rlim_cur,
		  rlp.rlim_max);
    qlib->master_stack_size = rlp.rlim_cur;
    qlib->max_stack_size = rlp.rlim_max;
#endif

    /* set up the memory pools */
    for (i = 0; i < nkthreads; i++) {
	/* the following SHOULD only be accessed by one thread at a time, so
	 * should be quite safe unsynchronized. If things fail, though...
	 * resynchronize them and see if that fixes it. */
	qlib->kthreads[i].qthread_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_t),
					sizeof(qthread_t) * 100);
	qlib->kthreads[i].stack_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					qlib->qthread_stack_size,
					qlib->qthread_stack_size * 100);
#if ALIGNMENT_PROBLEMS_RETURN
	if (sizeof(ucontext_t) < 2048) {
	    qlib->kthreads[i].context_pool =
		cp_mempool_create_by_option(0, 2048, 2048 * 100);
	} else {
	    qlib->kthreads[i].context_pool =
		cp_mempool_create_by_option(0, sizeof(ucontext_t),
					    sizeof(ucontext_t) * 100);
	}
#else
	qlib->kthreads[i].context_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(ucontext_t),
					sizeof(ucontext_t) * 100);
#endif
	qlib->kthreads[i].list_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(cp_list_entry), 0);
	qlib->kthreads[i].queue_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_queue_t), 0);
	qlib->kthreads[i].lock_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_lock_t), 0);
	qlib->kthreads[i].addrres_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_addrres_t), 0);
	qlib->kthreads[i].addrstat_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_addrstat_t), 0);
	qlib->kthreads[i].addrstat2_pool =
	    cp_mempool_create_by_option(COLLECTION_MODE_NOSYNC,
					sizeof(qthread_addrstat2_t), 0);
    }
    /* these are used when qthread_fork() is called from a non-qthread. they
     * are protected by a mutex so that things don't get wonky (note: that
     * means qthread_fork is WAY faster if you called it from a qthread) */
    generic_qthread_pool =
	cp_mempool_create_by_option(0, sizeof(qthread_t),
				    sizeof(qthread_t) * 100);
    generic_stack_pool =
	cp_mempool_create_by_option(0, qlib->qthread_stack_size,
				    qlib->qthread_stack_size * 100);
    generic_context_pool =
	cp_mempool_create_by_option(0, sizeof(ucontext_t),
				    sizeof(ucontext_t) * 100);
    generic_queue_pool =
	cp_mempool_create_by_option(0, sizeof(qthread_queue_t), 0);
    generic_lock_pool =
	cp_mempool_create_by_option(0, sizeof(qthread_lock_t), 0);

    /* spawn the number of shepherd threads that were specified */
    for (i = 0; i < nkthreads; i++) {
	qlib->kthreads[i].sched_kthread = 0;
	qlib->kthreads[i].kthread_index = i;
	qlib->kthreads[i].ready =
	    qthread_queue_new(NULL);

	qthread_debug("qthread_init(): forking shepherd thread %p\n",
		      &qlib->kthreads[i]);

	if ((r =
	     pthread_create(&qlib->kthreads[i].kthread, NULL,
			    qthread_shepherd, &qlib->kthreads[i])) != 0) {
	    fprintf(stderr, "qthread_init: pthread_create() failed (%d)\n",
		    r);
	    abort();
	}
    }

    qthread_debug("qthread_init(): finished.\n");
    return 0;
}				       /*}}} */

void qthread_finalize(void)
{				       /*{{{ */
    int i, r;
    qthread_t *t;

    assert(qlib != NULL);

    qthread_debug("qthread_finalize(): began.\n");

    /* rcm - probably need to put a "turn off the library flag" here, but,
     * the programmer can ensure that no further threads are forked for now
     */

    /* enqueue the termination thread sentinal */
    for (i = 0; i < qlib->nkthreads; i++) {
	t = qthread_thread_new(NULL, NULL, (aligned_t *) NULL, i);
	t->thread_state = QTHREAD_STATE_TERM_SHEP;
	t->thread_id = (unsigned int)-1;
	qthread_enqueue(qlib->kthreads[i].ready, t);
    }

    /* wait for each thread to drain it's queue! */
    for (i = 0; i < qlib->nkthreads; i++) {
	if ((r = pthread_join(qlib->kthreads[i].kthread, NULL)) != 0) {
	    fprintf(stderr, "qthread_finalize: pthread_join() failed (%d)\n",
		    r);
	    abort();
	}
	qthread_queue_free(qlib->kthreads[i].ready,
			   NULL);
    }

    for (i = 0; i < 32; i++) {
	cp_hashtable_destroy(qlib->locks[i]);
    }
    cp_hashtable_destroy_custom(qlib->FEBs, NULL, (cp_destructor_fn)
				qthread_FEBlock_delete);

    QTHREAD_DESTROYLOCK(&qlib->max_thread_id_lock);
    QTHREAD_DESTROYLOCK(&qlib->sched_kthread_lock);

    for (i = 0; i < qlib->nkthreads; ++i) {
	cp_mempool_destroy(qlib->kthreads[i].qthread_pool);
	cp_mempool_destroy(qlib->kthreads[i].list_pool);
	cp_mempool_destroy(qlib->kthreads[i].queue_pool);
	cp_mempool_destroy(qlib->kthreads[i].lock_pool);
	cp_mempool_destroy(qlib->kthreads[i].addrres_pool);
	cp_mempool_destroy(qlib->kthreads[i].addrstat_pool);
	cp_mempool_destroy(qlib->kthreads[i].addrstat2_pool);
	cp_mempool_destroy(qlib->kthreads[i].stack_pool);
	cp_mempool_destroy(qlib->kthreads[i].context_pool);
    }
    cp_mempool_destroy(generic_qthread_pool);
    cp_mempool_destroy(generic_stack_pool);
    cp_mempool_destroy(generic_context_pool);
    cp_mempool_destroy(generic_queue_pool);
    cp_mempool_destroy(generic_lock_pool);
    free(qlib->kthreads);
    free(qlib);
    qlib = NULL;

    qthread_debug("qthread_finalize(): finished.\n");
}				       /*}}} */

qthread_t *qthread_self(void)
{				       /*{{{ */
    qthread_shepherd_t *shep;
#if 0
    /* size_t mask; */

    printf("stack size: %lu\n", qlib->qthread_stack_size);
    printf("ret is at %p\n", &ret);
    mask = qlib->qthread_stack_size - 1;	/* assuming the stack is a power of two */
    printf("mask is: %p\n", ((size_t) (qlib->qthread_stack_size) - 1));
    printf("low order bits: 0x%lx\n",
	   ((size_t) (&ret) % (size_t) (qlib->qthread_stack_size)));
    printf("low order bits: 0x%lx\n", (size_t) (&ret) & mask);
    printf("calc stack pointer is: %p\n", ((size_t) (&ret) & ~mask));
    printf("top is then: 0x%lx\n",
	   ((size_t) (&ret) & ~mask) + qlib->qthread_stack_size);
    /* printf("stack pointer should be %p\n", t->stack); */
#endif
    shep = (qthread_shepherd_t *)pthread_getspecific(shepherd_structs);
    return shep?shep->current:NULL;
}				       /*}}} */

size_t qthread_stackleft(const qthread_t * t)
{				       /*{{{ */
    if (t != NULL) {
	return (size_t) (&t) - (size_t) (t->stack);
    } else {
	return 0;
    }
}				       /*}}} */

aligned_t *qthread_retlock(const qthread_t * t)
{				       /*{{{ */
    if (t) {
	return t->ret;
    } else {
	qthread_t *me = qthread_self();

	if (me) {
	    return me->ret;
	} else {
	    return NULL;
	}
    }
}				       /*}}} */

/************************************************************/
/* functions to manage thread stack allocation/deallocation */
/************************************************************/

static inline qthread_t *qthread_thread_bare(const qthread_f f,
					     const void *arg, aligned_t * ret,
					     const qthread_shepherd_id_t
					     shepherd)
{				       /*{{{ */
    qthread_t *t;
    qthread_shepherd_t * myshep;

#ifndef UNPOOLED
    myshep = (qthread_shepherd_t *) pthread_getspecific(shepherd_structs);
#else
    myshep = NULL;
#endif

    t = ALLOC_QTHREAD(myshep);
    if (t == NULL) {
	perror("qthread_prepare()");
	abort();
    }
#ifdef QTHREAD_NONLAZY_THREADIDS
    /* give the thread an ID number */
    ATOMIC_INC(t->thread_id, &qlib->max_thread_id, &qlib->max_thread_id_lock);
#else
    t->thread_id = (unsigned int)-1;
#endif
    t->thread_state = QTHREAD_STATE_NEW;
    t->f = f;
    t->arg = (void *)arg;
    t->blockedon = NULL;
    t->shepherd_ptr = &(qlib->kthreads[shepherd]);
    t->ret = ret;

    return t;
}				       /*}}} */

static inline void qthread_thread_plush(qthread_t * t)
{				       /*{{{ */
    ucontext_t *uc;
    void *stack;
    qthread_shepherd_t * shepherd = (qthread_shepherd_t *) pthread_getspecific(shepherd_structs);

    uc = ALLOC_CONTEXT(shepherd);
    stack = ALLOC_STACK(shepherd);

    if (uc == NULL) {
	perror("qthread_thread_plush()");
	abort();
    }
    t->context = uc;
    if (stack == NULL) {
	perror("qthread_thread_plush()");
	abort();
    }
    t->stack = stack;
}				       /*}}} */

/* this could be reduced to a qthread_thread_bare() and qthread_thread_plush(),
 * but I *think* doing it this way makes it faster. maybe not, I haven't tested
 * it. */
static inline qthread_t *qthread_thread_new(const qthread_f f,
					    const void *arg, aligned_t * ret,
					    const qthread_shepherd_id_t
					    shepherd)
{				       /*{{{ */
    qthread_t *t;
    ucontext_t *uc;
    void *stack;
    qthread_shepherd_t * myshep;

#ifndef UNPOOLED
    myshep = (qthread_shepherd_t *) pthread_getspecific(shepherd_structs);
#else
    myshep = NULL;
#endif

    t = ALLOC_QTHREAD(myshep);
    uc = ALLOC_CONTEXT(myshep);
    stack = ALLOC_STACK(myshep);

    if (t == NULL) {
	perror("qthread_thread_new()");
	abort();
    }

    t->thread_state = QTHREAD_STATE_NEW;
    t->future_flags = 0;
    t->f = f;
    t->arg = (void *)arg;
    t->blockedon = NULL;
    t->shepherd_ptr = &(qlib->kthreads[shepherd]);
    t->ret = ret;

    /* re-ordered the checks to help optimization */
    if (uc == NULL) {
	perror("qthread_thread_new()");
	abort();
    }
    t->context = uc;
    if (stack == NULL) {
	perror("qthread_thread_new()");
	abort();
    }
    t->stack = stack;

#ifdef QTHREAD_NONLAZY_THREADIDS
    /* give the thread an ID number */
    ATOMIC_INC(t->thread_id, &qlib->max_thread_id, &qlib->max_thread_id_lock);
#else
    t->thread_id = (unsigned int)-1;
#endif

    return (t);
}				       /*}}} */

static inline void qthread_thread_free(qthread_t * t)
{				       /*{{{ */
#ifndef UNPOOLED
    qthread_shepherd_t * myshep = (qthread_shepherd_t*) pthread_getspecific(shepherd_structs);
#else
    qthread_shepherd_t * myshep = NULL;
#endif

    assert(t != NULL);

    if (t->context) {
	FREE_CONTEXT(myshep, t->context);
    }
    if (t->stack != NULL) {
	FREE_STACK(myshep, t->stack);
    }
    FREE_QTHREAD(myshep, t);
}				       /*}}} */


/*****************************************/
/* functions to manage the thread queues */
/*****************************************/

static inline qthread_queue_t *qthread_queue_new(qthread_shepherd_t *
						 shepherd)
{				       /*{{{ */
    qthread_queue_t *q;

    q = ALLOC_QUEUE(shepherd);
    if (q == NULL) {
	perror("qthread_queue_new()");
	abort();
    }

    q->head = NULL;
    q->tail = NULL;
    QTHREAD_INITLOCK(&q->lock);
    QTHREAD_INITCOND(&q->notempty);
    return (q);
}				       /*}}} */

static inline void qthread_queue_free(qthread_queue_t * q,
				      qthread_shepherd_t * shepherd)
{				       /*{{{ */
    assert((q->head == NULL) && (q->tail == NULL));
    QTHREAD_DESTROYLOCK(&q->lock);
    QTHREAD_DESTROYCOND(&q->notempty);
    FREE_QUEUE(shepherd, q);
}				       /*}}} */

static inline void qthread_enqueue(qthread_queue_t * q, qthread_t * t)
{				       /*{{{ */
    assert(t != NULL);
    assert(q != NULL);

    qthread_debug("qthread_enqueue(%p,%p): started\n", q, t);

    QTHREAD_LOCK(&q->lock);

    t->next = NULL;

    if (q->head == NULL) {	       /* surely then tail is also null; no need to check */
	q->head = t;
	q->tail = t;
	QTHREAD_SIGNAL(&q->notempty);
    } else {
	q->tail->next = t;
	q->tail = t;
    }

    qthread_debug("qthread_enqueue(%p,%p): finished\n", q, t);
    QTHREAD_UNLOCK(&q->lock);
}				       /*}}} */

static inline qthread_t *qthread_dequeue(qthread_queue_t * q)
{				       /*{{{ */
    qthread_t *t;

    qthread_debug("qthread_dequeue(%p): started\n", q);

    QTHREAD_LOCK(&q->lock);

    while (q->head == NULL) {	       /* if head is null, then surely tail is also null */
	QTHREAD_CONDWAIT(&q->notempty, &q->lock);
    }

    assert(q->head != NULL);

    t = q->head;
    if (q->head != q->tail) {
	q->head = q->head->next;
    } else {
	q->head = NULL;
	q->tail = NULL;
    }
    t->next = NULL;

    QTHREAD_UNLOCK(&q->lock);

    qthread_debug("qthread_dequeue(%p,%p): finished\n", q, t);
    return (t);
}				       /*}}} */

static inline qthread_t *qthread_dequeue_nonblocking(qthread_queue_t * q)
{				       /*{{{ */
    qthread_t *t = NULL;

    /* NOTE: it's up to the caller to lock/unlock the queue! */
    qthread_debug("qthread_dequeue_nonblocking(%p,%p): started\n", q, t);

    if (q->head == NULL) {
	qthread_debug
	    ("qthread_dequeue_nonblocking(%p,%p): finished (nobody in list)\n",
	     q, t);
	return (NULL);
    }

    t = q->head;
    if (q->head != q->tail) {
	q->head = q->head->next;
    } else {
	q->head = NULL;
	q->tail = NULL;
    }
    t->next = NULL;

    qthread_debug("qthread_dequeue_nonblocking(%p,%p): finished\n", q, t);
    return (t);
}				       /*}}} */

/* this function is for maintenance of the FEB hashtables. SHOULD only be
 * necessary for things left over when qthread_finalize is called */
static void qthread_FEBlock_delete(qthread_addrstat_t * m)
{				       /*{{{ */
    /* NOTE! This is only safe if this function (as part of destroying the FEB
     * hash table) is ONLY called once all other pthreads have been joined */
    FREE_ADDRSTAT((&(qlib->kthreads[0])), m);
}				       /*}}} */

/* this function runs a thread until it completes or yields */
static void qthread_wrapper(void *arg)
{				       /*{{{ */
    qthread_t *t = (qthread_t *) arg;

    qthread_debug("qthread_wrapper(): executing f=%p arg=%p.\n", t->f,
		  t->arg);
    if (t->future_flags & QTHREAD_FUTURE) {
	extern pthread_key_t future_bookkeeping;
	location_t *loc;

	loc =
	     (location_t *) pthread_getspecific(future_bookkeeping);
	if (loc != NULL) {
	    blocking_vp_incr(t, loc);
	} else {
	    fprintf(stderr, "could not find bookkeeping for %i\n", qthread_shep(t));
	    abort();
	}
    }
    if (t->ret)
	qthread_writeEF_const(t, t->ret, (t->f) (t, t->arg));
    else
	(t->f) (t, t->arg);
    t->thread_state = QTHREAD_STATE_TERMINATED;

    qthread_debug("qthread_wrapper(): f=%p arg=%p completed.\n", t->f,
		  t->arg);
#if !defined(HAVE_CONTEXT_FUNCS) || defined(NEED_RLIMIT)
    /* without a built-in make/get/swapcontext, we're relying on the portable
     * one in context.c (stolen from libtask). unfortunately, this home-made
     * context stuff does not allow us to set up a uc_link pointer that will be
     * returned to once qthread_wrapper returns, so we have to do it by hand.
     *
     * We also have to do it by hand if the context switch requires a
     * stack-size modification.
     */
    qthread_back_to_master(t);
#endif
}				       /*}}} */

static inline void qthread_exec(qthread_t * t, ucontext_t * c)
{				       /*{{{ */
#ifdef NEED_RLIMIT
    struct rlimit rlp;
#endif

    assert(t != NULL);
    assert(c != NULL);

    if (t->thread_state == QTHREAD_STATE_NEW) {

	qthread_debug("qthread_exec(%p, %p): type is QTHREAD_THREAD_NEW!\n",
		      t, c);
	t->thread_state = QTHREAD_STATE_RUNNING;

	getcontext(t->context);	       /* puts the current context into t->contextq */
	/* Several other libraries that do this reserve a few words on either
	 * end of the stack for some reason. To avoid problems, I'll also do
	 * this (even though I have no idea why they would do this). */
	/* t is cast here ONLY because the PGI compiler is idiotic about typedef's */
	t->context->uc_stack.ss_sp =
	    (char *)(((struct qthread_s *)t)->stack) + 8;
	t->context->uc_stack.ss_size = qlib->qthread_stack_size - 64;
#ifdef HAVE_CONTEXT_FUNCS
	/* the makecontext man page (Linux) says: set the uc_link FIRST
	 * why? no idea */
	t->context->uc_link = c;       /* NULL pthread_exit() */
	qthread_debug("qthread_exec(): context is {%p, %d, %p}\n",
		      t->context->uc_stack.ss_sp,
		      t->context->uc_stack.ss_size, t->context->uc_link);
#else
	qthread_debug("qthread_exec(): context is {%p, %d}\n",
		      t->context->uc_stack.ss_sp,
		      t->context->uc_stack.ss_size);
#endif
	makecontext(t->context, (void (*)(void))qthread_wrapper, 1, t);	/* the casting shuts gcc up */
#ifdef HAVE_CONTEXT_FUNCS
    } else {
	t->context->uc_link = c;       /* NULL pthread_exit() */
#endif
    }

    t->return_context = c;

#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_exec(%p): setting stack size limits... hopefully we don't currently exceed them!\n",
	 t);
    rlp.rlim_cur = qlib->qthread_stack_size;
    rlp.rlim_max = qlib->max_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif

    qthread_debug("qthread_exec(%p): executing swapcontext()...\n", t);
    /* return_context (aka "c") is being written over with the current context */
    if (swapcontext(t->return_context, t->context) != 0) {
	perror("qthread_exec: swapcontext() failed");
	abort();
    }
#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_exec(%p): setting stack size limits back to normal...\n",
	 t);
    rlp.rlim_cur = qlib->master_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif

    assert(t != NULL);
    assert(c != NULL);

    qthread_debug("qthread_exec(%p): finished\n", t);
}				       /*}}} */

/* this function yields thread t to the master kernel thread */
void qthread_yield(qthread_t * t)
{				       /*{{{ */
    qthread_debug("qthread_yield(): thread %p yielding.\n", t);
    t->thread_state = QTHREAD_STATE_YIELDED;
    qthread_back_to_master(t);
    qthread_debug("qthread_yield(): thread %p resumed.\n", t);
}				       /*}}} */

/***********************************************
 * FORKING                                     *
 ***********************************************/
/* fork a thread by putting it in somebody's work queue
 * NOTE: scheduling happens here
 */
void qthread_fork(const qthread_f f, const void *arg, aligned_t * ret)
{				       /*{{{ */
    qthread_t *t;
    qthread_shepherd_id_t shep;
    qthread_shepherd_t * myshep = (qthread_shepherd_t*) pthread_getspecific(shepherd_structs);

    if (myshep) { /* note: for forking from a qthread, NO LOCKS! */
	shep = myshep->sched_kthread;
	if (myshep->sched_kthread + 1 < qlib->nkthreads) {
	    (myshep->sched_kthread)++;
	} else {
	    myshep->sched_kthread = 0;
	}
    } else {
	ATOMIC_INC_MOD(shep, &qlib->sched_kthread, &qlib->sched_kthread_lock,
		qlib->nkthreads);
    }
    t = qthread_thread_new(f, arg, ret, shep);


    qthread_debug("qthread_fork(): tid %u shep %u\n", t->thread_id, shep);

    if (ret) {
	qthread_empty(qthread_self(), ret, 1);
    }
    qthread_enqueue(qlib->kthreads[shep].ready, t);
}				       /*}}} */

void qthread_fork_to(const qthread_f f, const void *arg, aligned_t * ret,
		     const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t;

    if (shepherd > qlib->nkthreads || f == NULL) {
	return;
    }
    t = qthread_thread_new(f, arg, ret, shepherd);
    qthread_debug("qthread_fork_to(): tid %u shep %u\n", t->thread_id,
		  shepherd);

    if (ret) {
	qthread_empty(qthread_self(), ret, 1);
    }
    qthread_enqueue(qlib->kthreads[shepherd].ready, t);
}				       /*}}} */

void qthread_fork_future_to(const qthread_f f, const void *arg,
			    aligned_t * ret,
			    const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t;

    if (shepherd > qlib->nkthreads) {
	return;
    }
    t = qthread_thread_new(f, arg, ret, shepherd);
    t->future_flags |= QTHREAD_FUTURE;
    qthread_debug("qthread_fork_future_to(): tid %u shep %u\n", t->thread_id,
		  shepherd);

    if (ret) {
	qthread_empty(qthread_self(), ret, 1);
    }
    qthread_enqueue(qlib->kthreads[shepherd].ready, t);
}				       /*}}} */

static inline void qthread_back_to_master(qthread_t * t)
{				       /*{{{ */
#ifdef NEED_RLIMIT
    struct rlimit rlp;

    qthread_debug
	("qthread_back_to_master(%p): setting stack size limits for master thread...\n",
	 t);
    rlp.rlim_cur = qlib->master_stack_size;
    rlp.rlim_max = qlib->max_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif
    /* now back to your regularly scheduled master thread */
    if (swapcontext(t->context, t->return_context) != 0) {
	perror("qthread_back_to_master(): swapcontext() failed!");
	abort();
    }
#ifdef NEED_RLIMIT
    qthread_debug
	("qthread_back_to_master(%p): setting stack size limits back to qthread size...\n",
	 t);
    rlp.rlim_cur = qlib->qthread_stack_size;
    qassert(setrlimit(RLIMIT_STACK, &rlp), 0);
#endif
}				       /*}}} */

qthread_t *qthread_prepare(const qthread_f f, const void *arg,
			   aligned_t * ret)
{				       /*{{{ */
    qthread_t *t;
    qthread_shepherd_id_t shep;
    qthread_shepherd_t *myshep = (qthread_shepherd_t*) pthread_getspecific(shepherd_structs);

    if (myshep) {
	shep = myshep->sched_kthread++;
	if (myshep->sched_kthread + 1 < qlib->nkthreads) {
	    (myshep->sched_kthread)++;
	} else {
	    myshep->sched_kthread = 0;
	}
    } else {
	ATOMIC_INC_MOD(shep, &qlib->sched_kthread, &qlib->sched_kthread_lock,
		qlib->nkthreads);
    }

    t = qthread_thread_bare(f, arg, ret, shep);
    if (ret) {
	qthread_empty(qthread_self(), ret, 1);
    }

    return t;
}				       /*}}} */

qthread_t *qthread_prepare_for(const qthread_f f, const void *arg,
			       aligned_t * ret,
			       const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_t *t = qthread_thread_bare(f, arg, ret, shepherd);

    if (ret) {
	qthread_empty(qthread_self(), ret, 1);
    }

    return t;
}				       /*}}} */

void qthread_schedule(qthread_t * t)
{				       /*{{{ */
    qthread_thread_plush(t);
    qthread_enqueue(t->shepherd_ptr->ready, t);
}				       /*}}} */

void qthread_schedule_on(qthread_t * t, const qthread_shepherd_id_t shepherd)
{				       /*{{{ */
    qthread_thread_plush(t);
    qthread_enqueue(qlib->kthreads[shepherd].ready, t);
}				       /*}}} */

/* functions to implement FEB locking/unlocking 
 *
 * NOTE: these have not been profiled, and so may need tweaking for speed
 * (e.g. multiple hash tables, shortening critical section, etc.)
 */

/* The lock ordering in these functions is very particular, and is designed to
 * reduce the impact of having only one hashtable. Don't monkey with it unless
 * you REALLY know what you're doing! If one hashtable becomes a problem, we
 * may need to move to a new mechanism.
 */

struct qthread_FEB_sub_args
{
    void *src;
    void *dest;
    pthread_mutex_t alldone;
};

/* this one is (strictly-speaking) unnecessary, but I think it helps with
 * optimization to have those consts */
struct qthread_FEB_ef_sub_args
{
    const size_t count;
    const void *dest;
    pthread_mutex_t alldone;
};

/* This is just a little function that should help in debugging */
int qthread_feb_status(const void *addr)
{				       /*{{{ */
    qthread_addrstat_t *m;
    aligned_t *alignedaddr;
    int status = 1;		/* full */

    ALIGN(addr, alignedaddr, "qthread_feb_status()");
    cp_hashtable_rdlock(qlib->FEBs); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
						    (void *)alignedaddr);
	if (m) {
	    status = m->full;
	}
    }
    cp_hashtable_unlock(qlib->FEBs);
    return status;
}				       /*}}} */

static inline qthread_addrstat_t *qthread_addrstat_new(qthread_shepherd_t *
						       shepherd)
{				       /*{{{ */
    qthread_addrstat_t *ret = ALLOC_ADDRSTAT(shepherd);

    QTHREAD_INITLOCK(&ret->lock);
    ret->full = 1;
    ret->EFQ = NULL;
    ret->FEQ = NULL;
    ret->FFQ = NULL;
    return ret;
}				       /*}}} */

static inline void qthread_FEB_remove(void *maddr,
				      const qthread_shepherd_t * threadshep)
{				       /*{{{ */
    qthread_addrstat_t *m;

    qthread_debug("qthread_FEB_remove(): attempting removal\n");
    cp_hashtable_wrlock(qlib->FEBs); {
	m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs, maddr);
	if (m) {
	    QTHREAD_LOCK(&(m->lock));
	    REPORTLOCK(m);
	    if (m->FEQ == NULL && m->EFQ == NULL && m->FFQ == NULL &&
		m->full == 1) {
		qthread_debug
		    ("qthread_FEB_remove(): all lists are empty, and status is full\n");
		cp_hashtable_remove(qlib->FEBs, maddr);
	    } else {
		QTHREAD_UNLOCK(&(m->lock));
		REPORTUNLOCK(m);
		qthread_debug
		    ("qthread_FEB_remove(): address cannot be removed; in use\n");
		m = NULL;
	    }
	}
    }
    cp_hashtable_unlock(qlib->FEBs);
    if (m != NULL) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	QTHREAD_DESTROYLOCK(&m->lock);
	FREE_ADDRSTAT(threadshep, m);
    }
}				       /*}}} */

static inline void qthread_gotlock_empty(qthread_addrstat_t * m, void *maddr,
					 const qthread_shepherd_t *
					 threadshep, const char recursive)
{				       /*{{{ */
    qthread_addrres_t *X = NULL;
    int removeable;

    m->full = 0;
    if (m->EFQ != NULL) {
	/* dQ */
	X = m->EFQ;
	m->EFQ = X->next;
	/* op */
	memcpy(maddr, X->addr, WORDSIZE);
	m->full = 1;
	/* requeue */
	X->waiter->thread_state = QTHREAD_STATE_RUNNING;
	qthread_enqueue(X->waiter->shepherd_ptr->ready, X->waiter);
	/* XXX: Note that this may not be the same mempool that this memory
	 * originally came from. This shouldn't be a big problem, but if it is,
	 * we will have to make the addrres_pool synchronized, and use the
	 * commented-out line. */
	/* FREE_ADDRRES(X->waiter->shepherd_ptr, X); */
	FREE_ADDRRES(threadshep, X);
	qthread_gotlock_fill(m, maddr, threadshep, 1);
    }
    if (m->full == 1 && m->EFQ == NULL && m->FEQ == NULL && m->FFQ == NULL)
	removeable = 1;
    else
	removeable = 0;
    if (recursive == 0) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	if (removeable) {
	    qthread_FEB_remove(maddr, threadshep);
	}
    }
}				       /*}}} */

static inline void qthread_gotlock_fill(qthread_addrstat_t * m, void *maddr,
					const qthread_shepherd_t *
					threadshep, const char recursive)
{				       /*{{{ */
    qthread_addrres_t *X = NULL;
    int removeable;

    qthread_debug("qthread_gotlock_fill(%p, %p)\n", m, maddr);
    m->full = 1;
    /* dequeue all FFQ, do their operation, and schedule them */
    qthread_debug("qthread_gotlock_fill(): dQ all FFQ\n");
    while (m->FFQ != NULL) {
	/* dQ */
	X = m->FFQ;
	m->FFQ = X->next;
	/* op */
	memcpy(X->addr, maddr, WORDSIZE);
	/* schedule */
	X->waiter->thread_state = QTHREAD_STATE_RUNNING;
	qthread_enqueue(X->waiter->shepherd_ptr->ready, X->waiter);
	/* XXX: Note that this may not be the same mempool that this memory
	 * originally came from. This shouldn't be a big problem, but if it is,
	 * we will have to make the addrres_pool synchronized, and use the
	 * commented-out line. */
	/* FREE_ADDRRES(X->waiter->shepherd_ptr, X); */
	FREE_ADDRRES(threadshep, X);
    }
    if (m->FEQ != NULL) {
	/* dequeue one FEQ, do their operation, and schedule them */
	qthread_t *waiter;

	qthread_debug("qthread_gotlock_fill(): dQ 1 FEQ\n");
	X = m->FEQ;
	m->FEQ = X->next;
	/* op */
	memcpy(X->addr, maddr, WORDSIZE);
	waiter = X->waiter;
	waiter->thread_state = QTHREAD_STATE_RUNNING;
	m->full = 0;
	qthread_enqueue(waiter->shepherd_ptr->ready, waiter);
	/* XXX: Note that this may not be the same mempool that this memory
	 * originally came from. This shouldn't be a big problem, but if it is,
	 * we will have to make the addrres_pool synchronized, and use the
	 * commented-out line. */
	/* FREE_ADDRRES(waiter->shepherd_ptr, X); */
	FREE_ADDRRES(threadshep, X);
	qthread_gotlock_empty(m, maddr, threadshep, 1);
    }
    if (m->EFQ == NULL && m->FEQ == NULL && m->full == 1)
	removeable = 1;
    else
	removeable = 1;
    if (recursive == 0) {
	QTHREAD_UNLOCK(&m->lock);
	REPORTUNLOCK(m);
	/* now, remove it if it needs to be removed */
	if (removeable) {
	    qthread_FEB_remove(maddr, threadshep);
	}
    }
}				       /*}}} */

static aligned_t qthread_empty_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_empty(me, ((struct qthread_FEB_ef_sub_args *)arg)->dest,
		  ((struct qthread_FEB_ef_sub_args *)arg)->count);
    pthread_mutex_unlock(&((struct qthread_FEB_ef_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_empty(qthread_t * me, const void *dest, const size_t count)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat_t *m;
	qthread_addrstat2_t *m_better;
	qthread_addrstat2_t *list = NULL;
	size_t i;
	aligned_t *startaddr;

	ALIGN(dest, startaddr, "qthread_empty()");
	cp_hashtable_wrlock(qlib->FEBs); {
	    for (i = 0; i < count; ++i) {
		m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							    (void *)(startaddr
								     + i));
		if (!m) {
		    m = qthread_addrstat_new(me->shepherd_ptr);
		    m->full = 0;
		    cp_hashtable_put(qlib->FEBs, (void *)(startaddr + i), m);
		} else {
		    QTHREAD_LOCK(&m->lock);
		    REPORTLOCK(m);
		    m_better = ALLOC_ADDRSTAT2(me->shepherd_ptr);
		    m_better->m = m;
		    m_better->addr = startaddr + i;
		    m_better->next = list;
		    list = m_better;
		}
	    }
	}
	cp_hashtable_unlock(qlib->FEBs);
	while (list != NULL) {
	    m_better = list;
	    list = list->next;
	    qthread_gotlock_empty(m_better->m, m_better->addr, me->shepherd_ptr,
				  0);
	    FREE_ADDRSTAT2(me->shepherd_ptr, m_better);
	}
    } else {
	struct qthread_FEB_ef_sub_args args =
	    { count, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_empty_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

static aligned_t qthread_fill_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_fill(me, ((struct qthread_FEB_ef_sub_args *)arg)->dest,
		 ((struct qthread_FEB_ef_sub_args *)arg)->count);
    pthread_mutex_unlock(&((struct qthread_FEB_ef_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_fill(qthread_t * me, const void *dest, const size_t count)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat2_t *m_better;
	qthread_addrstat_t *m;
	qthread_addrstat2_t *list = NULL;
	size_t i;
	aligned_t *startaddr;

	ALIGN(dest, startaddr, "qthread_fill()");
	/* lock hash */
	cp_hashtable_wrlock(qlib->FEBs); {
	    for (i = 0; i < count; ++i) {
		m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							    (void *)(startaddr
								     + i));
		if (m) {
		    QTHREAD_LOCK(&m->lock);
		    REPORTLOCK(m);
		    m_better = ALLOC_ADDRSTAT2(me->shepherd_ptr);
		    m_better->m = m;
		    m_better->addr = startaddr + i;
		    m_better->next = list;
		    list = m_better;
		}
	    }
	}
	cp_hashtable_unlock(qlib->FEBs);	/* unlock hash */
	while (list != NULL) {
	    m_better = list;
	    list = list->next;
	    qthread_gotlock_fill(m_better->m, m_better->addr, me->shepherd_ptr,
				 0);
	    FREE_ADDRSTAT2(me->shepherd_ptr, m_better);
	}
    } else {
	struct qthread_FEB_ef_sub_args args =
	    { count, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_fill_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

/* the way this works is that:
 * 1 - data is copies from src to destination
 * 2 - the destination's FEB state gets changed from empty to full
 */

static aligned_t qthread_writeF_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_writeF(me, ((struct qthread_FEB_sub_args *)arg)->dest,
		   ((struct qthread_FEB_sub_args *)arg)->src);
    pthread_mutex_unlock(&((struct qthread_FEB_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_writeF(qthread_t * me, void *dest, const void *src)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat_t *m;
	aligned_t *alignedaddr;

	ALIGN(dest, alignedaddr, "qthread_fill_with()");
	cp_hashtable_wrlock(qlib->FEBs); {	/* lock hash */
	    m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							(void *)alignedaddr);
	    if (!m) {
		m = qthread_addrstat_new(me->shepherd_ptr);
		cp_hashtable_put(qlib->FEBs, alignedaddr, m);
	    }
	    QTHREAD_LOCK(&m->lock);
	    REPORTLOCK(m);
	}
	cp_hashtable_unlock(qlib->FEBs);	/* unlock hash */
	/* we have the lock on m, so... */
	memcpy(dest, src, WORDSIZE);
	qthread_gotlock_fill(m, alignedaddr, me->shepherd_ptr, 0);
    } else {
	struct qthread_FEB_sub_args args =
	    { (void *)src, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_writeF_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

void qthread_writeF_const(qthread_t * me, void *dest, const aligned_t src)
{				       /*{{{ */
    qthread_writeF(me, dest, &src);
}				       /*}}} */

/* the way this works is that:
 * 1 - destination's FEB state must be "empty"
 * 2 - data is copied from src to destination
 * 3 - the destination's FEB state gets changed from empty to full
 */

static aligned_t qthread_writeEF_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_writeEF(me, ((struct qthread_FEB_sub_args *)arg)->dest,
		    ((struct qthread_FEB_sub_args *)arg)->src);
    pthread_mutex_unlock(&((struct qthread_FEB_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_writeEF(qthread_t * me, void *dest, const void *src)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat_t *m;
	qthread_addrres_t *X = NULL;
	aligned_t *alignedaddr;

	qthread_debug("qthread_writeEF(%p, %p, %p): init\n", me, dest, src);
	ALIGN(dest, alignedaddr, "qthread_writeEF()");
	cp_hashtable_wrlock(qlib->FEBs); {
	    m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							(void *)alignedaddr);
	    if (!m) {
		m = qthread_addrstat_new(me->shepherd_ptr);
		cp_hashtable_put(qlib->FEBs, alignedaddr, m);
	    }
	    QTHREAD_LOCK(&(m->lock));
	    REPORTLOCK(m);
	}
	cp_hashtable_unlock(qlib->FEBs);
	qthread_debug("qthread_writeEF(): data structure locked\n");
	/* by this point m is locked */
	qthread_debug("qthread_writeEF(): m->full == %i\n", m->full);
	if (m->full == 1) {	       /* full, thus, we must block */
	    X = ALLOC_ADDRRES(me->shepherd_ptr);
	    X->addr = (aligned_t *) src;
	    X->waiter = me;
	    X->next = m->EFQ;
	    m->EFQ = X;
	} else {
	    memcpy(dest, src, WORDSIZE);
	    qthread_gotlock_fill(m, alignedaddr, me->shepherd_ptr, 0);
	}
	/* now all the addresses are either written or queued */
	qthread_debug("qthread_writeEF(): all written/queued\n");
	if (X) {
	    qthread_debug("qthread_writeEF(): back to parent\n");
	    me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	    me->blockedon = (struct qthread_lock_s *)m;
	    qthread_back_to_master(me);
	}
    } else {
	struct qthread_FEB_sub_args args =
	    { (void *)src, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_writeEF_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

void qthread_writeEF_const(qthread_t * me, void *dest, const aligned_t src)
{				       /*{{{ */
    qthread_writeEF(me, dest, &src);
}				       /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 */

static aligned_t qthread_readFF_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_readFF(me, ((struct qthread_FEB_sub_args *)arg)->dest,
		   ((struct qthread_FEB_sub_args *)arg)->src);
    pthread_mutex_unlock(&((struct qthread_FEB_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_readFF(qthread_t * me, void *dest, void *src)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat_t *m = NULL;
	qthread_addrres_t *X = NULL;
	aligned_t *alignedaddr;

	qthread_debug("qthread_readFF(%p, %p, %p): init\n", me, dest, src);
	ALIGN(src, alignedaddr, "qthread_readFF()");
	cp_hashtable_wrlock(qlib->FEBs); {
	    m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							(void *)alignedaddr);
	    if (!m) {
		memcpy(dest, src, WORDSIZE);
	    } else {
		QTHREAD_LOCK(&m->lock);
		REPORTLOCK(m);
	    }
	}
	cp_hashtable_unlock(qlib->FEBs);
	qthread_debug("qthread_readFF(): data structure locked\n");
	/* now m, if it exists, is locked - if m is NULL, then we're done! */
	if (m == NULL)
	    return;
	if (m->full != 1) {
	    X = ALLOC_ADDRRES(me->shepherd_ptr);
	    X->addr = (aligned_t *) dest;
	    X->waiter = me;
	    X->next = m->FFQ;
	    m->FFQ = X;
	} else {
	    memcpy(dest, src, WORDSIZE);
	    QTHREAD_UNLOCK(&m->lock);
	    REPORTUNLOCK(m);
	}
	/* if X exists, we are queued, and need to block (i.e. go back to the shepherd) */
	if (X) {
	    qthread_debug("qthread_readFF(): back to parent\n");
	    me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	    me->blockedon = (struct qthread_lock_s *)m;
	    qthread_back_to_master(me);
	}
    } else {
	struct qthread_FEB_sub_args args =
	    { src, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_readFF_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

/* the way this works is that:
 * 1 - src's FEB state must be "full"
 * 2 - data is copied from src to destination
 * 3 - the src's FEB bits get changed from full to empty
 */

static aligned_t qthread_readFE_sub(qthread_t * me, void *arg)
{				       /*{{{ */
    qthread_readFE(me, ((struct qthread_FEB_sub_args *)arg)->dest,
		   ((struct qthread_FEB_sub_args *)arg)->src);
    pthread_mutex_unlock(&((struct qthread_FEB_sub_args *)arg)->alldone);
    return 0;
}				       /*}}} */

void qthread_readFE(qthread_t * me, void *dest, void *src)
{				       /*{{{ */
    if (me != NULL) {
	qthread_addrstat_t *m;
	qthread_addrres_t *X = NULL;
	aligned_t *alignedaddr;

	qthread_debug("qthread_readFE(%p, %p, %p): init\n", me, dest, src);
	ALIGN(src, alignedaddr, "qthread_readFE()");
	cp_hashtable_wrlock(qlib->FEBs); {
	    m = (qthread_addrstat_t *) cp_hashtable_get(qlib->FEBs,
							alignedaddr);
	    if (!m) {
		m = qthread_addrstat_new(me->shepherd_ptr);
		cp_hashtable_put(qlib->FEBs, alignedaddr, m);
	    }
	    QTHREAD_LOCK(&(m->lock));
	    REPORTLOCK(m);
	}
	cp_hashtable_unlock(qlib->FEBs);
	qthread_debug("qthread_readFE(): data structure locked\n");
	/* by this point m is locked */
	if (m->full == 0) {	       /* empty, thus, we must block */
	    X = ALLOC_ADDRRES(me->shepherd_ptr);
	    X->addr = (aligned_t *) dest;
	    X->waiter = me;
	    X->next = m->FEQ;
	    m->FEQ = X;
	} else {
	    memcpy(dest, src, WORDSIZE);
	    qthread_gotlock_empty(m, alignedaddr, me->shepherd_ptr, 0);
	}
	/* now all the addresses are either written or queued */
	if (X) {
	    qthread_debug("qthread_readFE(): back to parent\n");
	    me->thread_state = QTHREAD_STATE_FEB_BLOCKED;
	    me->blockedon = (struct qthread_lock_s *)m;
	    qthread_back_to_master(me);
	}
    } else {
	struct qthread_FEB_sub_args args =
	    { src, dest, PTHREAD_MUTEX_INITIALIZER };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_readFE_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
    }
}				       /*}}} */

/* functions to implement FEB-ish locking/unlocking
 *
 * These are atomic and functional, but do not have the same semantics as full
 * FEB locking/unlocking (for example, unlocking cannot block)
 *
 * NOTE: these have not been profiled, and so may need tweaking for speed
 * (e.g. multiple hash tables, shortening critical section, etc.)
 */

/* The lock ordering in these functions is very particular, and is designed to
 * reduce the impact of having only one hashtable. Don't monkey with it unless
 * you REALLY know what you're doing! If one hashtable becomes a problem, we
 * may need to move to a new mechanism.
 */

struct qthread_lock_sub_args
{
    pthread_mutex_t alldone;
    const void *addr;
};

static aligned_t qthread_lock_sub(qthread_t * t, void *arg)
{				       /*{{{ */
    qthread_lock(t, ((struct qthread_lock_sub_args *)arg)->addr);
    pthread_mutex_unlock(&(((struct qthread_lock_sub_args *)arg)->alldone));
    return 0;
}				       /*}}} */

int qthread_lock(qthread_t * t, const void *a)
{				       /*{{{ */
    qthread_lock_t *m;

    if (t != NULL) {
	const int lockbin = (((const size_t)a) >> 5) & 0x1f;	/* guaranteed to be between 0 and 32 */

	cp_hashtable_wrlock(qlib->locks[lockbin]);
	m = (qthread_lock_t *) cp_hashtable_get(qlib->locks[lockbin],
						(void *)a);
	if (m == NULL) {
	    /* by doing this lookup (which MAY go into the kernel), we avoid
	     * needing to lock a pthread_mutex on all memory pool accesses.
	     * Theoretically, this might be bad on some architectures. 99 times
	     * out of 100, though, this is a win for overall throughput. */
	    qthread_shepherd_t * myshep = pthread_getspecific(shepherd_structs);

	    if ((m = ALLOC_LOCK(myshep)) == NULL) {
		perror("qthread_lock()");
		abort();
	    }
	    /* If we have a shepherd, use it... note that we are ignoring the
	     * qthread_t that got passed in, because that is inherently
	     * untrustworthy. I tested it, actually, and this is faster than
	     * trying to guess whether the qthread_t is accurate or not.
	     */
	    m->waiting = qthread_queue_new(myshep);
	    QTHREAD_INITLOCK(&m->lock);
	    cp_hashtable_put(qlib->locks[lockbin], (void *)a, m);
	    /* since we just created it, we own it */
	    QTHREAD_LOCK(&m->lock);
	    /* can only unlock the hash after we've locked the address, because
	     * otherwise there's a race condition: the address could be removed
	     * before we have a chance to add ourselves to it */
	    cp_hashtable_unlock(qlib->locks[lockbin]);

#ifdef QTHREAD_DEBUG
	    m->owner = t->thread_id;
#endif
	    QTHREAD_UNLOCK(&m->lock);
	    qthread_debug("qthread_lock(%p, %p): returned (wasn't locked)\n",
			  t, a);
	} else {
	    /* success==failure: because it's in the hash, someone else owns the
	     * lock; dequeue this thread and yield.
	     * NOTE: it's up to the master thread to enqueue this thread and unlock
	     * the address
	     */
	    QTHREAD_LOCK(&m->lock);
	    /* for an explanation of the lock/unlock ordering here, see above */
	    cp_hashtable_unlock(qlib->locks[lockbin]);

	    t->thread_state = QTHREAD_STATE_BLOCKED;
	    t->blockedon = m;

	    qthread_back_to_master(t);

	    /* once I return to this context, I own the lock! */
	    /* conveniently, whoever unlocked me already set up everything too */
	    qthread_debug("qthread_lock(%p, %p): returned (was locked)\n", t,
			  a);
	}
	return 1;
    } else {
	struct qthread_lock_sub_args args = { PTHREAD_MUTEX_INITIALIZER, a };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_lock_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);
	return 2;
    }
}				       /*}}} */

static aligned_t qthread_unlock_sub(qthread_t * t, void *arg)
{				       /*{{{ */
    qthread_unlock(t, ((struct qthread_lock_sub_args *)arg)->addr);
    pthread_mutex_unlock(&(((struct qthread_lock_sub_args *)arg)->alldone));
    return 0;
}				       /*}}} */

int qthread_unlock(qthread_t * t, const void *a)
{				       /*{{{ */
    qthread_lock_t *m;
    qthread_t *u;

    qthread_debug("qthread_unlock(%p, %p): started\n", t, a);

    if (t != NULL) {
	const int lockbin = (((const size_t)a) >> 5) & 0x1f;	/* guaranteed to be between 0 and 32 */

	cp_hashtable_wrlock(qlib->locks[lockbin]);
	m = (qthread_lock_t *) cp_hashtable_get(qlib->locks[lockbin],
						(void *)a);
	if (m == NULL) {
	    /* unlocking an address that's already locked */
	    cp_hashtable_unlock(qlib->locks[lockbin]);
	    return 1;
	}
	QTHREAD_LOCK(&m->lock);

	/* unlock the address... if anybody's waiting for it, give them the lock
	 * and put them in a ready queue.  If not, delete the lock structure.
	 */

	QTHREAD_LOCK(&m->waiting->lock);
	u = qthread_dequeue_nonblocking(m->waiting);
	if (u == NULL) {
	    /* by doing this lookup (which MAY go into the kernel), we avoid
	     * needing to lock a pthread_mutex on all memory pool accesses.
	     * Theoretically, this might be bad on some architectures. 99 times
	     * out of 100, though, this is a win for overall throughput. */
	    qthread_shepherd_t * myshep = pthread_getspecific(shepherd_structs);

	    qthread_debug("qthread_unlock(%p,%p): deleting waiting queue\n",
			  t, a);
	    cp_hashtable_remove(qlib->locks[lockbin], (void *)a);
	    cp_hashtable_unlock(qlib->locks[lockbin]);

	    QTHREAD_UNLOCK(&m->waiting->lock);
	    qthread_queue_free(m->waiting, myshep);
	    QTHREAD_UNLOCK(&m->lock);
	    QTHREAD_DESTROYLOCK(&m->lock);
	    /* XXX: Note that this may not be the same mempool that this memory
	     * originally came from. This shouldn't be a big problem, but if it is,
	     * we may have to get creative */
	    FREE_LOCK(myshep, m);
	} else {
	    cp_hashtable_unlock(qlib->locks[lockbin]);
	    qthread_debug
		("qthread_unlock(%p,%p): pulling thread from queue (%p)\n", t,
		 a, u);
	    u->thread_state = QTHREAD_STATE_RUNNING;
#ifdef QTHREAD_DEBUG
	    m->owner = u->thread_id;
#endif

	    /* NOTE: because of the use of getcontext()/setcontext(), threads
	     * return to the shepherd that setcontext()'d into them, so they
	     * must remain in that queue.
	     */
	    qthread_enqueue(u->shepherd_ptr->ready, u);

	    QTHREAD_UNLOCK(&m->waiting->lock);
	    QTHREAD_UNLOCK(&m->lock);
	}

	qthread_debug("qthread_unlock(%p, %p): returned\n", t, a);
	return 1;
    } else {
	struct qthread_lock_sub_args args = { PTHREAD_MUTEX_INITIALIZER, a };

	QTHREAD_LOCK(&args.alldone);
	qthread_fork(qthread_unlock_sub, &args, NULL);
	QTHREAD_LOCK(&args.alldone);
	QTHREAD_UNLOCK(&args.alldone);
	QTHREAD_DESTROYLOCK(&args.alldone);

	qthread_debug("qthread_unlock(%p, %p): returned\n", t, a);
	return 2;
    }
}				       /*}}} */

/* These are just accessor functions */
unsigned qthread_id(const qthread_t * t)
{				       /*{{{ */
#ifdef QTHREAD_NONLAZY_THREADIDS
    return t ? t->thread_id : (unsigned int)-1;
#else
    if (!t) {
	return (unsigned int)-1;
    }
    if (t->thread_id != (unsigned int)-1) {
	return t->thread_id;
    }
    ATOMIC_INC(((qthread_t *)t)->thread_id, &qlib->max_thread_id, &qlib->max_thread_id_lock);
    return t->thread_id;
#endif
}				       /*}}} */

qthread_shepherd_id_t qthread_shep(const qthread_t * t)
{				       /*{{{ */
    qthread_shepherd_t *ret;
    if (t) {
	return t->shepherd_ptr->kthread_index;
    }
    ret = pthread_getspecific(shepherd_structs);
    if (ret == NULL) {
	return NO_SHEPHERD;
    } else {
	return ret->kthread_index;
    }
}				       /*}}} */

/* these two functions are helper functions for futurelib
 * (nobody else gets to have 'em!) */
unsigned int qthread_isfuture(const qthread_t * t)
{				       /*{{{ */
    return t ? (t->future_flags & QTHREAD_FUTURE) : 0;
}				       /*}}} */

void qthread_assertfuture(qthread_t * t)
{
    t->future_flags |= QTHREAD_FUTURE;
}

void qthread_assertnotfuture(qthread_t * t)
{
    t->future_flags &= ~QTHREAD_FUTURE;
}
