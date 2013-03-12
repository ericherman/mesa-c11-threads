/*
 * C11 <threads.h> emulation library
 *
 * (C) Copyright yohhoy 2012.
 * Distributed under the Boost Software License, Version 1.0.
 * (See copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h> /* for intptr_t */

/*
Configuration macro:

  EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
    Use pthread_mutex_timedlock() for `mtx_timedlock()'
    Otherwise use mtx_trylock() + *busy loop* emulation.
*/
#if !defined(__CYGWIN__)
#define EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
#endif


#include <pthread.h>

/*---------------------------- macros ----------------------------*/
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#ifdef INIT_ONCE_STATIC_INIT
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS
#else
#define TSS_DTOR_ITERATIONS 1  // assume TSS dtor MAY be called at least once.
#endif

// FIXME: temporary non-standard hack to ease transition
#define _MTX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER

/*---------------------------- types ----------------------------*/
typedef pthread_cond_t  cnd_t;
typedef pthread_t       thrd_t;
typedef pthread_key_t   tss_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_once_t  once_flag;


/*
Implementation limits:
  - Conditionally emulation for "mutex with timeout"
    (see EMULATED_THREADS_USE_NATIVE_TIMEDLOCK macro)
*/
struct impl_thrd_param {
    thrd_start_t func;
    void *arg;
};

static inline void *
impl_thrd_routine(void *p)
{
    struct impl_thrd_param pack = *((struct impl_thrd_param *)p);
    free(p);
    return (void*)(intptr_t)pack.func(pack.arg);
}


/*--------------- 7.25.2 Initialization functions ---------------*/
// 7.25.2.1
static inline void
call_once(once_flag *flag, void (*func)(void))
{
    pthread_once(flag, func);
}


/*------------- 7.25.3 Condition variable functions -------------*/
// 7.25.3.1
static inline int
cnd_broadcast(cnd_t *cond)
{
    if (!cond) return thrd_error;
    pthread_cond_broadcast(cond);
    return thrd_success;
}

// 7.25.3.2
static inline void
cnd_destroy(cnd_t *cond)
{
    assert(cond);
    pthread_cond_destroy(cond);
}

// 7.25.3.3
static inline int
cnd_init(cnd_t *cond)
{
    if (!cond) return thrd_error;
    pthread_cond_init(cond, NULL);
    return thrd_success;
}

// 7.25.3.4
static inline int
cnd_signal(cnd_t *cond)
{
    if (!cond) return thrd_error;
    pthread_cond_signal(cond);
    return thrd_success;
}

// 7.25.3.5
static inline int
cnd_timedwait(cnd_t *cond, mtx_t *mtx, const xtime *xt)
{
    struct timespec abs_time;
    int rt;
    if (!cond || !mtx || !xt) return thrd_error;
    rt = pthread_cond_timedwait(cond, mtx, &abs_time);
    if (rt == ETIMEDOUT)
        return thrd_busy;
    return (rt == 0) ? thrd_success : thrd_error;
}

// 7.25.3.6
static inline int
cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    if (!cond || !mtx) return thrd_error;
    pthread_cond_wait(cond, mtx);
    return thrd_success;
}


/*-------------------- 7.25.4 Mutex functions --------------------*/
// 7.25.4.1
static inline void
mtx_destroy(mtx_t *mtx)
{
    assert(mtx);
    pthread_mutex_destroy(mtx);
}

// 7.25.4.2
static inline int
mtx_init(mtx_t *mtx, int type)
{
    pthread_mutexattr_t attr;
    if (!mtx) return thrd_error;
    if (type != mtx_plain && type != mtx_timed && type != mtx_try
      && type != (mtx_plain|mtx_recursive)
      && type != (mtx_timed|mtx_recursive)
      && type != (mtx_try|mtx_recursive))
        return thrd_error;
    pthread_mutexattr_init(&attr);
    if ((type & mtx_recursive) != 0) {
#if defined(__linux__) || defined(__linux)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
#else
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#endif
    }
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    return thrd_success;
}

// 7.25.4.3
static inline int
mtx_lock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    pthread_mutex_lock(mtx);
    return thrd_success;
}

// 7.25.4.4
static inline int
mtx_timedlock(mtx_t *mtx, const xtime *xt)
{
    if (!mtx || !xt) return thrd_error;
    {
#ifdef EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
    struct timespec ts;
    int rt;
    ts.tv_sec = xt->sec;
    ts.tv_nsec = xt->nsec;
    rt = pthread_mutex_timedlock(mtx, &ts);
    if (rt == 0)
        return thrd_success;
    return (rt == ETIMEDOUT) ? thrd_busy : thrd_error;
#else
    time_t expire = time(NULL);
    expire += xt->sec;
    while (mtx_trylock(mtx) != thrd_success) {
        time_t now = time(NULL);
        if (expire < now)
            return thrd_busy;
        // busy loop!
        thrd_yield();
    }
    return thrd_success;
#endif
    }
}

// 7.25.4.5
static inline int
mtx_trylock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    return (pthread_mutex_trylock(mtx) == 0) ? thrd_success : thrd_busy;
}

// 7.25.4.6
static inline int
mtx_unlock(mtx_t *mtx)
{
    if (!mtx) return thrd_error;
    pthread_mutex_unlock(mtx);
    return thrd_success;
}


/*------------------- 7.25.5 Thread functions -------------------*/
// 7.25.5.1
static inline int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    struct impl_thrd_param *pack;
    if (!thr) return thrd_error;
    pack = (struct impl_thrd_param *)malloc(sizeof(struct impl_thrd_param));
    if (!pack) return thrd_nomem;
    pack->func = func;
    pack->arg = arg;
    if (pthread_create(thr, NULL, impl_thrd_routine, pack) != 0) {
        free(pack);
        return thrd_error;
    }
    return thrd_success;
}

// 7.25.5.2
static inline thrd_t
thrd_current(void)
{
    return pthread_self();
}

// 7.25.5.3
static inline int
thrd_detach(thrd_t thr)
{
    return (pthread_detach(thr) == 0) ? thrd_success : thrd_error;
}

// 7.25.5.4
static inline int
thrd_equal(thrd_t thr0, thrd_t thr1)
{
    return pthread_equal(thr0, thr1);
}

// 7.25.5.5
static inline void
thrd_exit(int res)
{
    pthread_exit((void*)(intptr_t)res);
}

// 7.25.5.6
static inline int
thrd_join(thrd_t thr, int *res)
{
    void *code;
    if (pthread_join(thr, &code) != 0)
        return thrd_error;
    if (res)
        *res = (int)(intptr_t)code;
    return thrd_success;
}

// 7.25.5.7
static inline void
thrd_sleep(const xtime *xt)
{
    struct timespec req;
    assert(xt);
    req.tv_sec = xt->sec;
    req.tv_nsec = xt->nsec;
    nanosleep(&req, NULL);
}

// 7.25.5.8
static inline void
thrd_yield(void)
{
    sched_yield();
}


/*----------- 7.25.6 Thread-specific storage functions -----------*/
// 7.25.6.1
static inline int
tss_create(tss_t *key, tss_dtor_t dtor)
{
    if (!key) return thrd_error;
    return (pthread_key_create(key, dtor) == 0) ? thrd_success : thrd_error;
}

// 7.25.6.2
static inline void
tss_delete(tss_t key)
{
    pthread_key_delete(key);
}

// 7.25.6.3
static inline void *
tss_get(tss_t key)
{
    return pthread_getspecific(key);
}

// 7.25.6.4
static inline int
tss_set(tss_t key, void *val)
{
    return (pthread_setspecific(key, val) == 0) ? thrd_success : thrd_error;
}


/*-------------------- 7.25.7 Time functions --------------------*/
// 7.25.6.1
static inline int
xtime_get(xtime *xt, int base)
{
    if (!xt) return 0;
    if (base == TIME_UTC) {
        xt->sec = time(NULL);
        xt->nsec = 0;
        return base;
    }
    return 0;
}
