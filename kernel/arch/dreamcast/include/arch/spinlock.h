/* KallistiOS ##version##

   arch/dreamcast/include/spinlock.h
   Copyright (C) 2001 Megan Potter

*/

/** \file    arch/spinlock.h
    \brief   Simple locking.
    \ingroup kthreads

    This file contains definitions for very simple locks. Most of the time, you
    will probably not use such low-level locking, but will opt for something
    more fully featured like mutexes, semaphores, reader-writer semaphores, or
    recursive locks.

    \author Megan Potter

    \see    kos/sem.h
    \see    kos/mutex.h
    \see    kos/rwsem.h
    \see    kos/recursive_lock.h
*/

#ifndef __ARCH_SPINLOCK_H
#define __ARCH_SPINLOCK_H

/* Defines processor specific spinlocks */

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <stdbool.h>

/* DC implementation uses threads most of the time */
#include <kos/thread.h>

/** \brief  Spinlock data type. */
typedef volatile int spinlock_t;

/** \brief  Spinlock initializer.

    All created spinlocks should be initialized with this initializer so that
    they are in a sane state, ready to be used.
*/
#define SPINLOCK_INITIALIZER 0

/** \brief  Initialize a spinlock.

    This function abstracts initializing a spinlock, in case the
    initializer is not applicable to what you are doing.

    \param  lock            A pointer to the spinlock to be initialized.
*/
static inline void spinlock_init(spinlock_t *lock) {
    *lock = SPINLOCK_INITIALIZER;
}

/* Note here that even if threads aren't enabled, we'll still set the
   lock so that it can be used for anti-IRQ protection (e.g., malloc) */

/** \brief  Try to lock, without spinning.

    This function will attempt to lock the lock, but will not spin. Instead, it
    will return whether the lock was obtained or not.

    \param  lock            A pointer to the spinlock to be locked.
    \return                 False if the lock is held by another thread. True if
                            the lock was successfully obtained.
*/
static inline bool spinlock_trylock(spinlock_t *lock) {
    bool locked = false;

    __asm__ __volatile__("tas.b @%1\n\t"
                         "movt %0\n\t"
                         : "=r"(locked)
                         : "r"(lock)
                         : "t", "memory");

    return locked;
}

/** \brief  Spin on a lock.

    This function will spin on the lock, and will not return until the lock has
    been obtained for the calling thread.

    \param  lock            A pointer to the spinlock to be locked.
*/
static inline void spinlock_lock(spinlock_t *lock) {
    while(!spinlock_trylock(lock))
        thd_pass();
}

/** \brief  Free a lock.

    This function will unlock the lock that is currently held by the calling
    thread. Do not use this function unless you actually hold the lock!

    \param  lock            A pointer to the spinlock to be unlocked.
*/
static inline void spinlock_unlock(spinlock_t *lock) {
    *lock = 0;
}

/** \brief  Determine if a lock is locked.

    This function will return whether or not the lock specified is actually locked
    when it is called. This is NOT a thread-safe way of determining if a lock
    will be locked when you get around to locking it!

    \param  lock            A pointer to the spinlock to be checked.
    \return                 True if the spinlock is locked, false otherwise.
*/
static inline bool spinlock_is_locked(spinlock_t *lock) {
    return *lock != 0;
}

__END_DECLS

#endif  /* __ARCH_SPINLOCK_H */
