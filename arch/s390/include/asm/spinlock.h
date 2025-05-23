/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  S390 version
 *    Copyright IBM Corp. 1999
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Derived from "include/asm-i386/spinlock.h"
 */

#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#include <linux/smp.h>
#include <asm/atomic_ops.h>
#include <asm/barrier.h>
#include <asm/processor.h>
#include <asm/alternative.h>

static __always_inline unsigned int spinlock_lockval(void)
{
	unsigned long lc_lockval;
	unsigned int lockval;

	BUILD_BUG_ON(sizeof_field(struct lowcore, spinlock_lockval) != sizeof(lockval));
	lc_lockval = offsetof(struct lowcore, spinlock_lockval);
	asm_inline(
		ALTERNATIVE("   ly      %[lockval],%[offzero](%%r0)\n",
			    "   ly      %[lockval],%[offalt](%%r0)\n",
			    ALT_FEATURE(MFEATURE_LOWCORE))
		: [lockval] "=d" (lockval)
		: [offzero] "i" (lc_lockval),
		  [offalt] "i" (lc_lockval + LOWCORE_ALT_ADDRESS),
		  "m" (((struct lowcore *)0)->spinlock_lockval));
	return lockval;
}

extern int spin_retry;

bool arch_vcpu_is_preempted(int cpu);

#define vcpu_is_preempted arch_vcpu_is_preempted

/*
 * Simple spin lock operations.  There are two variants, one clears IRQ's
 * on the local processor, one does not.
 *
 * We make no fairness assumptions. They have a cost.
 *
 * (the type definitions are in asm/spinlock_types.h)
 */

void arch_spin_relax(arch_spinlock_t *lock);
#define arch_spin_relax	arch_spin_relax

void arch_spin_lock_wait(arch_spinlock_t *);
int arch_spin_trylock_retry(arch_spinlock_t *);
void arch_spin_lock_setup(int cpu);

static inline u32 arch_spin_lockval(int cpu)
{
	return cpu + 1;
}

static inline int arch_spin_value_unlocked(arch_spinlock_t lock)
{
	return lock.lock == 0;
}

static inline int arch_spin_is_locked(arch_spinlock_t *lp)
{
	return READ_ONCE(lp->lock) != 0;
}

static inline int arch_spin_trylock_once(arch_spinlock_t *lp)
{
	int old = 0;

	barrier();
	return likely(arch_try_cmpxchg(&lp->lock, &old, spinlock_lockval()));
}

static inline void arch_spin_lock(arch_spinlock_t *lp)
{
	if (!arch_spin_trylock_once(lp))
		arch_spin_lock_wait(lp);
}

static inline int arch_spin_trylock(arch_spinlock_t *lp)
{
	if (!arch_spin_trylock_once(lp))
		return arch_spin_trylock_retry(lp);
	return 1;
}

static inline void arch_spin_unlock(arch_spinlock_t *lp)
{
	typecheck(int, lp->lock);
	kcsan_release();
	asm_inline volatile(
		ALTERNATIVE("nop", ".insn rre,0xb2fa0000,7,0", ALT_FACILITY(49)) /* NIAI 7 */
		"	mvhhi	%[lock],0\n"
		: [lock] "=Q" (((unsigned short *)&lp->lock)[1])
		:
		: "memory");
}

/*
 * Read-write spinlocks, allowing multiple readers
 * but only one writer.
 *
 * NOTE! it is quite common to have readers in interrupts
 * but no interrupt writers. For those circumstances we
 * can "mix" irq-safe locks - any writer needs to get a
 * irq-safe write-lock, but readers can get non-irqsafe
 * read-locks.
 */

#define arch_read_relax(rw) barrier()
#define arch_write_relax(rw) barrier()

void arch_read_lock_wait(arch_rwlock_t *lp);
void arch_write_lock_wait(arch_rwlock_t *lp);

static inline void arch_read_lock(arch_rwlock_t *rw)
{
	int old;

	old = __atomic_add(1, &rw->cnts);
	if (old & 0xffff0000)
		arch_read_lock_wait(rw);
}

static inline void arch_read_unlock(arch_rwlock_t *rw)
{
	__atomic_add_const_barrier(-1, &rw->cnts);
}

static inline void arch_write_lock(arch_rwlock_t *rw)
{
	int old = 0;

	if (!arch_try_cmpxchg(&rw->cnts, &old, 0x30000))
		arch_write_lock_wait(rw);
}

static inline void arch_write_unlock(arch_rwlock_t *rw)
{
	__atomic_add_barrier(-0x30000, &rw->cnts);
}


static inline int arch_read_trylock(arch_rwlock_t *rw)
{
	int old;

	old = READ_ONCE(rw->cnts);
	return (!(old & 0xffff0000) && arch_try_cmpxchg(&rw->cnts, &old, old + 1));
}

static inline int arch_write_trylock(arch_rwlock_t *rw)
{
	int old;

	old = READ_ONCE(rw->cnts);
	return !old && arch_try_cmpxchg(&rw->cnts, &old, 0x30000);
}

#endif /* __ASM_SPINLOCK_H */
