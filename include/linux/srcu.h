/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion
 *
 * Copyright (C) IBM Corporation, 2006
 * Copyright (C) Fujitsu, 2012
 *
 * Author: Paul McKenney <paulmck@linux.ibm.com>
 *	   Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/RCU/ *.txt
 *
 */

#ifndef _LINUX_SRCU_H
#define _LINUX_SRCU_H

#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/rcu_segcblist.h>

struct srcu_struct;

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_srcu_struct(struct srcu_struct *ssp, const char *name,
		       struct lock_class_key *key);

#define init_srcu_struct(ssp) \
({ \
	static struct lock_class_key __srcu_key; \
	\
	__init_srcu_struct((ssp), #ssp, &__srcu_key); \
})

#define __SRCU_DEP_MAP_INIT(srcu_name)	.dep_map = { .name = #srcu_name },
#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

int init_srcu_struct(struct srcu_struct *ssp);

#define __SRCU_DEP_MAP_INIT(srcu_name)
#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/* Values for SRCU Tree srcu_data ->srcu_reader_flavor, but also used by rcutorture. */
#define SRCU_READ_FLAVOR_NORMAL	0x1		// srcu_read_lock().
#define SRCU_READ_FLAVOR_NMI	0x2		// srcu_read_lock_nmisafe().
//				0x4		// SRCU-lite is no longer with us.
#define SRCU_READ_FLAVOR_FAST	0x8		// srcu_read_lock_fast().
#define SRCU_READ_FLAVOR_ALL   (SRCU_READ_FLAVOR_NORMAL | SRCU_READ_FLAVOR_NMI | \
				SRCU_READ_FLAVOR_FAST) // All of the above.
#define SRCU_READ_FLAVOR_SLOWGP	SRCU_READ_FLAVOR_FAST
						// Flavors requiring synchronize_rcu()
						// instead of smp_mb().
void __srcu_read_unlock(struct srcu_struct *ssp, int idx) __releases(ssp);

#ifdef CONFIG_TINY_SRCU
#include <linux/srcutiny.h>
#elif defined(CONFIG_TREE_SRCU)
#include <linux/srcutree.h>
#else
#error "Unknown SRCU implementation specified to kernel configuration"
#endif

void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
		void (*func)(struct rcu_head *head));
void cleanup_srcu_struct(struct srcu_struct *ssp);
void synchronize_srcu(struct srcu_struct *ssp);

#define SRCU_GET_STATE_COMPLETED 0x1

/**
 * get_completed_synchronize_srcu - Return a pre-completed polled state cookie
 *
 * Returns a value that poll_state_synchronize_srcu() will always treat
 * as a cookie whose grace period has already completed.
 */
static inline unsigned long get_completed_synchronize_srcu(void)
{
	return SRCU_GET_STATE_COMPLETED;
}

unsigned long get_state_synchronize_srcu(struct srcu_struct *ssp);
unsigned long start_poll_synchronize_srcu(struct srcu_struct *ssp);
bool poll_state_synchronize_srcu(struct srcu_struct *ssp, unsigned long cookie);

// Maximum number of unsigned long values corresponding to
// not-yet-completed SRCU grace periods.
#define NUM_ACTIVE_SRCU_POLL_OLDSTATE 2

/**
 * same_state_synchronize_srcu - Are two old-state values identical?
 * @oldstate1: First old-state value.
 * @oldstate2: Second old-state value.
 *
 * The two old-state values must have been obtained from either
 * get_state_synchronize_srcu(), start_poll_synchronize_srcu(), or
 * get_completed_synchronize_srcu().  Returns @true if the two values are
 * identical and @false otherwise.  This allows structures whose lifetimes
 * are tracked by old-state values to push these values to a list header,
 * allowing those structures to be slightly smaller.
 */
static inline bool same_state_synchronize_srcu(unsigned long oldstate1, unsigned long oldstate2)
{
	return oldstate1 == oldstate2;
}

#ifdef CONFIG_NEED_SRCU_NMI_SAFE
int __srcu_read_lock_nmisafe(struct srcu_struct *ssp) __acquires(ssp);
void __srcu_read_unlock_nmisafe(struct srcu_struct *ssp, int idx) __releases(ssp);
#else
static inline int __srcu_read_lock_nmisafe(struct srcu_struct *ssp)
{
	return __srcu_read_lock(ssp);
}
static inline void __srcu_read_unlock_nmisafe(struct srcu_struct *ssp, int idx)
{
	__srcu_read_unlock(ssp, idx);
}
#endif /* CONFIG_NEED_SRCU_NMI_SAFE */

void srcu_init(void);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

/**
 * srcu_read_lock_held - might we be in SRCU read-side critical section?
 * @ssp: The srcu_struct structure to check
 *
 * If CONFIG_DEBUG_LOCK_ALLOC is selected, returns nonzero iff in an SRCU
 * read-side critical section.  In absence of CONFIG_DEBUG_LOCK_ALLOC,
 * this assumes we are in an SRCU read-side critical section unless it can
 * prove otherwise.
 *
 * Checks debug_lockdep_rcu_enabled() to prevent false positives during boot
 * and while lockdep is disabled.
 *
 * Note that SRCU is based on its own statemachine and it doesn't
 * relies on normal RCU, it can be called from the CPU which
 * is in the idle loop from an RCU point of view or offline.
 */
static inline int srcu_read_lock_held(const struct srcu_struct *ssp)
{
	if (!debug_lockdep_rcu_enabled())
		return 1;
	return lock_is_held(&ssp->dep_map);
}

/*
 * Annotations provide deadlock detection for SRCU.
 *
 * Similar to other lockdep annotations, except there is an additional
 * srcu_lock_sync(), which is basically an empty *write*-side critical section,
 * see lock_sync() for more information.
 */

/* Annotates a srcu_read_lock() */
static inline void srcu_lock_acquire(struct lockdep_map *map)
{
	lock_map_acquire_read(map);
}

/* Annotates a srcu_read_lock() */
static inline void srcu_lock_release(struct lockdep_map *map)
{
	lock_map_release(map);
}

/* Annotates a synchronize_srcu() */
static inline void srcu_lock_sync(struct lockdep_map *map)
{
	lock_map_sync(map);
}

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

static inline int srcu_read_lock_held(const struct srcu_struct *ssp)
{
	return 1;
}

#define srcu_lock_acquire(m) do { } while (0)
#define srcu_lock_release(m) do { } while (0)
#define srcu_lock_sync(m) do { } while (0)

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */


/**
 * srcu_dereference_check - fetch SRCU-protected pointer for later dereferencing
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the srcu_struct, which is used to check that we
 *	really are in an SRCU read-side critical section.
 * @c: condition to check for update-side use
 *
 * If PROVE_RCU is enabled, invoking this outside of an RCU read-side
 * critical section will result in an RCU-lockdep splat, unless @c evaluates
 * to 1.  The @c argument will normally be a logical expression containing
 * lockdep_is_held() calls.
 */
#define srcu_dereference_check(p, ssp, c) \
	__rcu_dereference_check((p), __UNIQUE_ID(rcu), \
				(c) || srcu_read_lock_held(ssp), __rcu)

/**
 * srcu_dereference - fetch SRCU-protected pointer for later dereferencing
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the srcu_struct, which is used to check that we
 *	really are in an SRCU read-side critical section.
 *
 * Makes rcu_dereference_check() do the dirty work.  If PROVE_RCU
 * is enabled, invoking this outside of an RCU read-side critical
 * section will result in an RCU-lockdep splat.
 */
#define srcu_dereference(p, ssp) srcu_dereference_check((p), (ssp), 0)

/**
 * srcu_dereference_notrace - no tracing and no lockdep calls from here
 * @p: the pointer to fetch and protect for later dereferencing
 * @ssp: pointer to the srcu_struct, which is used to check that we
 *	really are in an SRCU read-side critical section.
 */
#define srcu_dereference_notrace(p, ssp) srcu_dereference_check((p), (ssp), 1)

/**
 * srcu_read_lock - register a new reader for an SRCU-protected structure.
 * @ssp: srcu_struct in which to register the new reader.
 *
 * Enter an SRCU read-side critical section.  Note that SRCU read-side
 * critical sections may be nested.  However, it is illegal to
 * call anything that waits on an SRCU grace period for the same
 * srcu_struct, whether directly or indirectly.  Please note that
 * one way to indirectly wait on an SRCU grace period is to acquire
 * a mutex that is held elsewhere while calling synchronize_srcu() or
 * synchronize_srcu_expedited().
 *
 * The return value from srcu_read_lock() is guaranteed to be
 * non-negative.  This value must be passed unaltered to the matching
 * srcu_read_unlock().  Note that srcu_read_lock() and the matching
 * srcu_read_unlock() must occur in the same context, for example, it is
 * illegal to invoke srcu_read_unlock() in an irq handler if the matching
 * srcu_read_lock() was invoked in process context.  Or, for that matter to
 * invoke srcu_read_unlock() from one task and the matching srcu_read_lock()
 * from another.
 */
static inline int srcu_read_lock(struct srcu_struct *ssp) __acquires(ssp)
{
	int retval;

	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	retval = __srcu_read_lock(ssp);
	srcu_lock_acquire(&ssp->dep_map);
	return retval;
}

/**
 * srcu_read_lock_fast - register a new reader for an SRCU-protected structure.
 * @ssp: srcu_struct in which to register the new reader.
 *
 * Enter an SRCU read-side critical section, but for a light-weight
 * smp_mb()-free reader.  See srcu_read_lock() for more information.
 *
 * If srcu_read_lock_fast() is ever used on an srcu_struct structure,
 * then none of the other flavors may be used, whether before, during,
 * or after.  Note that grace-period auto-expediting is disabled for _fast
 * srcu_struct structures because auto-expedited grace periods invoke
 * synchronize_rcu_expedited(), IPIs and all.
 *
 * Note that srcu_read_lock_fast() can be invoked only from those contexts
 * where RCU is watching, that is, from contexts where it would be legal
 * to invoke rcu_read_lock().  Otherwise, lockdep will complain.
 */
static inline struct srcu_ctr __percpu *srcu_read_lock_fast(struct srcu_struct *ssp) __acquires(ssp)
{
	struct srcu_ctr __percpu *retval;

	srcu_check_read_flavor_force(ssp, SRCU_READ_FLAVOR_FAST);
	retval = __srcu_read_lock_fast(ssp);
	rcu_try_lock_acquire(&ssp->dep_map);
	return retval;
}

/**
 * srcu_down_read_fast - register a new reader for an SRCU-protected structure.
 * @ssp: srcu_struct in which to register the new reader.
 *
 * Enter a semaphore-like SRCU read-side critical section, but for
 * a light-weight smp_mb()-free reader.  See srcu_read_lock_fast() and
 * srcu_down_read() for more information.
 *
 * The same srcu_struct may be used concurrently by srcu_down_read_fast()
 * and srcu_read_lock_fast().
 */
static inline struct srcu_ctr __percpu *srcu_down_read_fast(struct srcu_struct *ssp) __acquires(ssp)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) && in_nmi());
	srcu_check_read_flavor_force(ssp, SRCU_READ_FLAVOR_FAST);
	return __srcu_read_lock_fast(ssp);
}

/**
 * srcu_read_lock_nmisafe - register a new reader for an SRCU-protected structure.
 * @ssp: srcu_struct in which to register the new reader.
 *
 * Enter an SRCU read-side critical section, but in an NMI-safe manner.
 * See srcu_read_lock() for more information.
 *
 * If srcu_read_lock_nmisafe() is ever used on an srcu_struct structure,
 * then none of the other flavors may be used, whether before, during,
 * or after.
 */
static inline int srcu_read_lock_nmisafe(struct srcu_struct *ssp) __acquires(ssp)
{
	int retval;

	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NMI);
	retval = __srcu_read_lock_nmisafe(ssp);
	rcu_try_lock_acquire(&ssp->dep_map);
	return retval;
}

/* Used by tracing, cannot be traced and cannot invoke lockdep. */
static inline notrace int
srcu_read_lock_notrace(struct srcu_struct *ssp) __acquires(ssp)
{
	int retval;

	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	retval = __srcu_read_lock(ssp);
	return retval;
}

/**
 * srcu_down_read - register a new reader for an SRCU-protected structure.
 * @ssp: srcu_struct in which to register the new reader.
 *
 * Enter a semaphore-like SRCU read-side critical section.  Note that
 * SRCU read-side critical sections may be nested.  However, it is
 * illegal to call anything that waits on an SRCU grace period for the
 * same srcu_struct, whether directly or indirectly.  Please note that
 * one way to indirectly wait on an SRCU grace period is to acquire
 * a mutex that is held elsewhere while calling synchronize_srcu() or
 * synchronize_srcu_expedited().  But if you want lockdep to help you
 * keep this stuff straight, you should instead use srcu_read_lock().
 *
 * The semaphore-like nature of srcu_down_read() means that the matching
 * srcu_up_read() can be invoked from some other context, for example,
 * from some other task or from an irq handler.  However, neither
 * srcu_down_read() nor srcu_up_read() may be invoked from an NMI handler.
 *
 * Calls to srcu_down_read() may be nested, similar to the manner in
 * which calls to down_read() may be nested.  The same srcu_struct may be
 * used concurrently by srcu_down_read() and srcu_read_lock().
 */
static inline int srcu_down_read(struct srcu_struct *ssp) __acquires(ssp)
{
	WARN_ON_ONCE(in_nmi());
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	return __srcu_read_lock(ssp);
}

/**
 * srcu_read_unlock - unregister a old reader from an SRCU-protected structure.
 * @ssp: srcu_struct in which to unregister the old reader.
 * @idx: return value from corresponding srcu_read_lock().
 *
 * Exit an SRCU read-side critical section.
 */
static inline void srcu_read_unlock(struct srcu_struct *ssp, int idx)
	__releases(ssp)
{
	WARN_ON_ONCE(idx & ~0x1);
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	srcu_lock_release(&ssp->dep_map);
	__srcu_read_unlock(ssp, idx);
}

/**
 * srcu_read_unlock_fast - unregister a old reader from an SRCU-protected structure.
 * @ssp: srcu_struct in which to unregister the old reader.
 * @scp: return value from corresponding srcu_read_lock_fast().
 *
 * Exit a light-weight SRCU read-side critical section.
 */
static inline void srcu_read_unlock_fast(struct srcu_struct *ssp, struct srcu_ctr __percpu *scp)
	__releases(ssp)
{
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_FAST);
	srcu_lock_release(&ssp->dep_map);
	__srcu_read_unlock_fast(ssp, scp);
}

/**
 * srcu_up_read_fast - unregister a old reader from an SRCU-protected structure.
 * @ssp: srcu_struct in which to unregister the old reader.
 * @scp: return value from corresponding srcu_read_lock_fast().
 *
 * Exit an SRCU read-side critical section, but not necessarily from
 * the same context as the maching srcu_down_read_fast().
 */
static inline void srcu_up_read_fast(struct srcu_struct *ssp, struct srcu_ctr __percpu *scp)
	__releases(ssp)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PROVE_RCU) && in_nmi());
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_FAST);
	__srcu_read_unlock_fast(ssp, scp);
}

/**
 * srcu_read_unlock_nmisafe - unregister a old reader from an SRCU-protected structure.
 * @ssp: srcu_struct in which to unregister the old reader.
 * @idx: return value from corresponding srcu_read_lock_nmisafe().
 *
 * Exit an SRCU read-side critical section, but in an NMI-safe manner.
 */
static inline void srcu_read_unlock_nmisafe(struct srcu_struct *ssp, int idx)
	__releases(ssp)
{
	WARN_ON_ONCE(idx & ~0x1);
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NMI);
	rcu_lock_release(&ssp->dep_map);
	__srcu_read_unlock_nmisafe(ssp, idx);
}

/* Used by tracing, cannot be traced and cannot call lockdep. */
static inline notrace void
srcu_read_unlock_notrace(struct srcu_struct *ssp, int idx) __releases(ssp)
{
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	__srcu_read_unlock(ssp, idx);
}

/**
 * srcu_up_read - unregister a old reader from an SRCU-protected structure.
 * @ssp: srcu_struct in which to unregister the old reader.
 * @idx: return value from corresponding srcu_read_lock().
 *
 * Exit an SRCU read-side critical section, but not necessarily from
 * the same context as the maching srcu_down_read().
 */
static inline void srcu_up_read(struct srcu_struct *ssp, int idx)
	__releases(ssp)
{
	WARN_ON_ONCE(idx & ~0x1);
	WARN_ON_ONCE(in_nmi());
	srcu_check_read_flavor(ssp, SRCU_READ_FLAVOR_NORMAL);
	__srcu_read_unlock(ssp, idx);
}

/**
 * smp_mb__after_srcu_read_unlock - ensure full ordering after srcu_read_unlock
 *
 * Converts the preceding srcu_read_unlock into a two-way memory barrier.
 *
 * Call this after srcu_read_unlock, to guarantee that all memory operations
 * that occur after smp_mb__after_srcu_read_unlock will appear to happen after
 * the preceding srcu_read_unlock.
 */
static inline void smp_mb__after_srcu_read_unlock(void)
{
	/* __srcu_read_unlock has smp_mb() internally so nothing to do here. */
}

/**
 * smp_mb__after_srcu_read_lock - ensure full ordering after srcu_read_lock
 *
 * Converts the preceding srcu_read_lock into a two-way memory barrier.
 *
 * Call this after srcu_read_lock, to guarantee that all memory operations
 * that occur after smp_mb__after_srcu_read_lock will appear to happen after
 * the preceding srcu_read_lock.
 */
static inline void smp_mb__after_srcu_read_lock(void)
{
	/* __srcu_read_lock has smp_mb() internally so nothing to do here. */
}

DEFINE_LOCK_GUARD_1(srcu, struct srcu_struct,
		    _T->idx = srcu_read_lock(_T->lock),
		    srcu_read_unlock(_T->lock, _T->idx),
		    int idx)

DEFINE_LOCK_GUARD_1(srcu_fast, struct srcu_struct,
		    _T->scp = srcu_read_lock_fast(_T->lock),
		    srcu_read_unlock_fast(_T->lock, _T->scp),
		    struct srcu_ctr __percpu *scp)

#endif
