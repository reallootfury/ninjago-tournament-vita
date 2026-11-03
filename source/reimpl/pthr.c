/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

#include "reimpl/pthr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define PTHR_MAX_OBJECTS 1024

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

#define BIONIC_MUTEXATTR_TYPE_MASK   0x000f
#define BIONIC_MUTEXATTR_SHARED_MASK 0x0010

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

static void *initializedObjects[PTHR_MAX_OBJECTS] = {0};
static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = sceKernelCreateLwMutex(&pthr_mutex, "log_lock", 0, 0, NULL); \
        if (ret < 0) { \
            sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
            return 0; \
        } \
        pthr_mutex_inited = 1; \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

#define PTHR_OBJECT_TOMBSTONE ((void *)(uintptr_t)1)

_Static_assert((PTHR_MAX_OBJECTS & (PTHR_MAX_OBJECTS - 1)) == 0,
               "PTHR_MAX_OBJECTS must be a power of two");

/*
 * Every Fusion mutex lock and condition wait first verifies that its Android
 * wrapper was initialized. The previous registry took a global lock and
 * linearly scanned all 1,024 slots for each check, making synchronized cache
 * and scene construction serialize on loader bookkeeping. Readers now use an
 * exact open-addressed hash lookup with acquire loads. The rare insert/remove
 * paths retain the existing Vita lightweight mutex, while tombstones preserve
 * probe chains across destruction and later reuse.
 */
PTHR_INLINE unsigned int pthr_object_hash(const void *object) {
    uintptr_t value = (uintptr_t)object >> 2;
    value ^= value >> 11;
    value ^= value >> 21;
    return (unsigned int)value & (PTHR_MAX_OBJECTS - 1);
}

int isObjectInitialized(const void * mut) {
    if (!mut) {
        return 0;
    }

    unsigned int index = pthr_object_hash(mut);
    for (unsigned int probe = 0; probe < PTHR_MAX_OBJECTS; ++probe) {
        void *entry = __atomic_load_n(&initializedObjects[index],
                                      __ATOMIC_ACQUIRE);
        if (entry == mut) {
            return 1;
        }
        if (!entry) {
            return 0;
        }
        index = (index + 1) & (PTHR_MAX_OBJECTS - 1);
    }
    return 0;
}

int rememberObject(void * mut) {
    if (!mut) {
        return 0;
    }

    PTHR_LOCK
    unsigned int index = pthr_object_hash(mut);
    unsigned int tombstone = PTHR_MAX_OBJECTS;
    for (unsigned int probe = 0; probe < PTHR_MAX_OBJECTS; ++probe) {
        void *entry = __atomic_load_n(&initializedObjects[index],
                                      __ATOMIC_RELAXED);
        if (entry == mut) {
            PTHR_UNLOCK
            return 1;
        }
        if (entry == PTHR_OBJECT_TOMBSTONE &&
            tombstone == PTHR_MAX_OBJECTS) {
            tombstone = index;
        } else if (!entry) {
            unsigned int destination = tombstone != PTHR_MAX_OBJECTS
                ? tombstone : index;
            __atomic_store_n(&initializedObjects[destination], mut,
                             __ATOMIC_RELEASE);
            PTHR_UNLOCK
            return 1;
        }
        index = (index + 1) & (PTHR_MAX_OBJECTS - 1);
    }
    if (tombstone != PTHR_MAX_OBJECTS) {
        __atomic_store_n(&initializedObjects[tombstone], mut,
                         __ATOMIC_RELEASE);
        PTHR_UNLOCK
        return 1;
    }
    PTHR_UNLOCK
    return 0;
}

int forgetObject(const void * mut) {
    if (!mut) {
        return 0;
    }

    PTHR_LOCK
    unsigned int index = pthr_object_hash(mut);
    for (unsigned int probe = 0; probe < PTHR_MAX_OBJECTS; ++probe) {
        void *entry = __atomic_load_n(&initializedObjects[index],
                                      __ATOMIC_RELAXED);
        if (entry == mut) {
            __atomic_store_n(&initializedObjects[index],
                             PTHR_OBJECT_TOMBSTONE, __ATOMIC_RELEASE);
            PTHR_UNLOCK
            return 1;
        }
        if (!entry) {
            PTHR_UNLOCK
            return 0;
        }
        index = (index + 1) & (PTHR_MAX_OBJECTS - 1);
    }
    PTHR_UNLOCK
    return 0;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _mutex_t_static_init(
    pthread_mutex_t_bionic *mutex,
    const pthread_mutexattr_t_bionic *attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    if (isObjectInitialized(mutex)) {
        //logv_debug("mutex already initialized: %p", mutex);
        return ret;
    }

    if (attr) {
        int bionic_kind = *attr & BIONIC_MUTEXATTR_TYPE_MASK;
        switch (bionic_kind) {
            case BIONIC_PTHREAD_MUTEX_NORMAL:
                kind = PTHREAD_MUTEX_NORMAL;
                break;
            case BIONIC_PTHREAD_MUTEX_RECURSIVE:
                kind = PTHREAD_MUTEX_RECURSIVE;
                break;
            case BIONIC_PTHREAD_MUTEX_ERRORCHECK:
                kind = PTHREAD_MUTEX_ERRORCHECK;
                break;
            default:
                return EINVAL;
        }
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t mut;
    mutex->real_ptr = malloc(sizeof(pthread_mutex_t));
    sceClibMemcpy(mutex->real_ptr, &mut, sizeof(pthread_mutex_t));

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(mutex->real_ptr, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        rememberObject(mutex);
    } else {
        l_error("mutex initialization for %p has failed", mutex);
    }

    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    if (isObjectInitialized(cond)) {
        //logv_debug("cond already initialized: %p", cond);
        return ret;
    }

    pthread_cond_t c;
    cond->real_ptr = malloc(sizeof(pthread_cond_t));
    sceClibMemcpy(cond->real_ptr, &c, sizeof(pthread_cond_t));

    ret = pthread_cond_init(cond->real_ptr, attr);

    if (ret == 0) {
        rememberObject(cond);
    } else {
        l_error("cond initialization for %p has failed", cond);
    }

    return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    l_info("pthread_create requested: start=%p param=%p attr=%p.",
           start, param, attr);
    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    if (ret == 0) {
        l_success("pthread_create returned successfully: thread=%u.",
                  thread ? (unsigned int)*thread : 0);
    } else {
        l_error("pthread_create failed: %d.", ret);
    }
    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t_bionic *attr)
{
    if (!attr) return EINVAL;
    *attr = BIONIC_PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t_bionic *attr)
{
    if (!attr) return EINVAL;
    *attr = -1;
    return 0;
}

int pthread_mutexattr_gettype_soloader(
    const pthread_mutexattr_t_bionic *attr, int *type)
{
    if (!attr || !type) return EINVAL;
    int value = *attr & BIONIC_MUTEXATTR_TYPE_MASK;
    if (value < BIONIC_PTHREAD_MUTEX_NORMAL ||
        value > BIONIC_PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    *type = value;
    return 0;
}

int pthread_mutexattr_settype_soloader(
    pthread_mutexattr_t_bionic *attr, int type)
{
    if (!attr || type < BIONIC_PTHREAD_MUTEX_NORMAL ||
        type > BIONIC_PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    *attr = (*attr & ~BIONIC_MUTEXATTR_TYPE_MASK) | type;
    return 0;
}

int pthread_mutexattr_getpshared_soloader(
    const pthread_mutexattr_t_bionic *attr, int *pshared)
{
    if (!attr || !pshared) return EINVAL;
    *pshared = (*attr & BIONIC_MUTEXATTR_SHARED_MASK)
        ? PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_mutexattr_setpshared_soloader(
    pthread_mutexattr_t_bionic *attr, int pshared)
{
    if (!attr) return EINVAL;
    if (pshared == PTHREAD_PROCESS_PRIVATE) {
        *attr &= ~BIONIC_MUTEXATTR_SHARED_MASK;
        return 0;
    }
    if (pshared == PTHREAD_PROCESS_SHARED) {
        *attr |= BIONIC_MUTEXATTR_SHARED_MASK;
        return 0;
    }
    return EINVAL;
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(
    pthread_mutex_t_bionic *uid, const pthread_mutexattr_t_bionic *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    forgetObject(mutex);
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    if (mutex->real_ptr) free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!mutex->real_ptr) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    forgetObject(cond);
    int ret = pthread_cond_destroy(cond->real_ptr);
    if (cond->real_ptr) free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    l_info("pthread_attr_destroy entered: attr=%p real=%p.",
           attr, attr->real_ptr);
    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    l_success("pthread_attr_destroy returned: %d.", ret);
    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC...
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
    l_info("pthread_setschedparam entered: thread=%u policy=%d priority=%d.",
           (unsigned int)thread, policy,
           param ? param->sched_priority : -1);
    int ret = pthread_setschedparam(thread, policy, param);
    l_success("pthread_setschedparam returned: %d.", ret);
    return ret;
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    l_info("pthread_getschedparam entered: thread=%u.",
           (unsigned int)thread);
    int ret = pthread_getschedparam(thread, policy, param);
    l_success("pthread_getschedparam returned: %d policy=%d priority=%d.",
              ret, policy ? *policy : -1,
              param ? param->sched_priority : -1);
    return ret;
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    l_info("pthread_setname_np: thread=%u name=%s.",
           (unsigned int)thread, thread_name);

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
