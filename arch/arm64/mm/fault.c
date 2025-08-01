// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/fault.c
 *
 * Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1995-2004 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/extable.h>
#include <linux/kfence.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/page-flags.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/highmem.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/preempt.h>
#include <linux/hugetlb.h>

#include <asm/acpi.h>
#include <asm/bug.h>
#include <asm/cmpxchg.h>
#include <asm/cpufeature.h>
#include <asm/efi.h>
#include <asm/exception.h>
#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/esr.h>
#include <asm/kprobes.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/system_misc.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>

struct fault_info {
	int	(*fn)(unsigned long far, unsigned long esr,
		      struct pt_regs *regs);
	int	sig;
	int	code;
	const char *name;
};

static const struct fault_info fault_info[];

static inline const struct fault_info *esr_to_fault_info(unsigned long esr)
{
	return fault_info + (esr & ESR_ELx_FSC);
}

static void data_abort_decode(unsigned long esr)
{
	unsigned long iss2 = ESR_ELx_ISS2(esr);

	pr_alert("Data abort info:\n");

	if (esr & ESR_ELx_ISV) {
		pr_alert("  Access size = %u byte(s)\n",
			 1U << ((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT));
		pr_alert("  SSE = %lu, SRT = %lu\n",
			 (esr & ESR_ELx_SSE) >> ESR_ELx_SSE_SHIFT,
			 (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT);
		pr_alert("  SF = %lu, AR = %lu\n",
			 (esr & ESR_ELx_SF) >> ESR_ELx_SF_SHIFT,
			 (esr & ESR_ELx_AR) >> ESR_ELx_AR_SHIFT);
	} else {
		pr_alert("  ISV = 0, ISS = 0x%08lx, ISS2 = 0x%08lx\n",
			 esr & ESR_ELx_ISS_MASK, iss2);
	}

	pr_alert("  CM = %lu, WnR = %lu, TnD = %lu, TagAccess = %lu\n",
		 (esr & ESR_ELx_CM) >> ESR_ELx_CM_SHIFT,
		 (esr & ESR_ELx_WNR) >> ESR_ELx_WNR_SHIFT,
		 (iss2 & ESR_ELx_TnD) >> ESR_ELx_TnD_SHIFT,
		 (iss2 & ESR_ELx_TagAccess) >> ESR_ELx_TagAccess_SHIFT);

	pr_alert("  GCS = %ld, Overlay = %lu, DirtyBit = %lu, Xs = %llu\n",
		 (iss2 & ESR_ELx_GCS) >> ESR_ELx_GCS_SHIFT,
		 (iss2 & ESR_ELx_Overlay) >> ESR_ELx_Overlay_SHIFT,
		 (iss2 & ESR_ELx_DirtyBit) >> ESR_ELx_DirtyBit_SHIFT,
		 (iss2 & ESR_ELx_Xs_MASK) >> ESR_ELx_Xs_SHIFT);
}

static void mem_abort_decode(unsigned long esr)
{
	pr_alert("Mem abort info:\n");

	pr_alert("  ESR = 0x%016lx\n", esr);
	pr_alert("  EC = 0x%02lx: %s, IL = %u bits\n",
		 ESR_ELx_EC(esr), esr_get_class_string(esr),
		 (esr & ESR_ELx_IL) ? 32 : 16);
	pr_alert("  SET = %lu, FnV = %lu\n",
		 (esr & ESR_ELx_SET_MASK) >> ESR_ELx_SET_SHIFT,
		 (esr & ESR_ELx_FnV) >> ESR_ELx_FnV_SHIFT);
	pr_alert("  EA = %lu, S1PTW = %lu\n",
		 (esr & ESR_ELx_EA) >> ESR_ELx_EA_SHIFT,
		 (esr & ESR_ELx_S1PTW) >> ESR_ELx_S1PTW_SHIFT);
	pr_alert("  FSC = 0x%02lx: %s\n", (esr & ESR_ELx_FSC),
		 esr_to_fault_info(esr)->name);

	if (esr_is_data_abort(esr))
		data_abort_decode(esr);
}

static inline unsigned long mm_to_pgd_phys(struct mm_struct *mm)
{
	/* Either init_pg_dir or swapper_pg_dir */
	if (mm == &init_mm)
		return __pa_symbol(mm->pgd);

	return (unsigned long)virt_to_phys(mm->pgd);
}

/*
 * Dump out the page tables associated with 'addr' in the currently active mm.
 */
static void show_pte(unsigned long addr)
{
	struct mm_struct *mm;
	pgd_t *pgdp;
	pgd_t pgd;

	if (is_ttbr0_addr(addr)) {
		/* TTBR0 */
		mm = current->active_mm;
		if (mm == &init_mm) {
			pr_alert("[%016lx] user address but active_mm is swapper\n",
				 addr);
			return;
		}
	} else if (is_ttbr1_addr(addr)) {
		/* TTBR1 */
		mm = &init_mm;
	} else {
		pr_alert("[%016lx] address between user and kernel address ranges\n",
			 addr);
		return;
	}

	pr_alert("%s pgtable: %luk pages, %llu-bit VAs, pgdp=%016lx\n",
		 mm == &init_mm ? "swapper" : "user", PAGE_SIZE / SZ_1K,
		 vabits_actual, mm_to_pgd_phys(mm));
	pgdp = pgd_offset(mm, addr);
	pgd = READ_ONCE(*pgdp);
	pr_alert("[%016lx] pgd=%016llx", addr, pgd_val(pgd));

	do {
		p4d_t *p4dp, p4d;
		pud_t *pudp, pud;
		pmd_t *pmdp, pmd;
		pte_t *ptep, pte;

		if (pgd_none(pgd) || pgd_bad(pgd))
			break;

		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		pr_cont(", p4d=%016llx", p4d_val(p4d));
		if (p4d_none(p4d) || p4d_bad(p4d))
			break;

		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		pr_cont(", pud=%016llx", pud_val(pud));
		if (pud_none(pud) || pud_bad(pud))
			break;

		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		pr_cont(", pmd=%016llx", pmd_val(pmd));
		if (pmd_none(pmd) || pmd_bad(pmd))
			break;

		ptep = pte_offset_map(pmdp, addr);
		if (!ptep)
			break;

		pte = __ptep_get(ptep);
		pr_cont(", pte=%016llx", pte_val(pte));
		pte_unmap(ptep);
	} while(0);

	pr_cont("\n");
}

/*
 * This function sets the access flags (dirty, accessed), as well as write
 * permission, and only to a more permissive setting.
 *
 * It needs to cope with hardware update of the accessed/dirty state by other
 * agents in the system and can safely skip the __sync_icache_dcache() call as,
 * like __set_ptes(), the PTE is never changed from no-exec to exec here.
 *
 * Returns whether or not the PTE actually changed.
 */
int __ptep_set_access_flags(struct vm_area_struct *vma,
			    unsigned long address, pte_t *ptep,
			    pte_t entry, int dirty)
{
	pteval_t old_pteval, pteval;
	pte_t pte = __ptep_get(ptep);

	if (pte_same(pte, entry))
		return 0;

	/* only preserve the access flags and write permission */
	pte_val(entry) &= PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;

	/*
	 * Setting the flags must be done atomically to avoid racing with the
	 * hardware update of the access/dirty state. The PTE_RDONLY bit must
	 * be set to the most permissive (lowest value) of *ptep and entry
	 * (calculated as: a & b == ~(~a | ~b)).
	 */
	pte_val(entry) ^= PTE_RDONLY;
	pteval = pte_val(pte);
	do {
		old_pteval = pteval;
		pteval ^= PTE_RDONLY;
		pteval |= pte_val(entry);
		pteval ^= PTE_RDONLY;
		pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
	} while (pteval != old_pteval);

	/* Invalidate a stale read-only entry */
	if (dirty)
		flush_tlb_page(vma, address);
	return 1;
}

static bool is_el1_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_CUR;
}

static bool is_el1_data_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_DABT_CUR;
}

static inline bool is_el1_permission_fault(unsigned long addr, unsigned long esr,
					   struct pt_regs *regs)
{
	if (!is_el1_data_abort(esr) && !is_el1_instruction_abort(esr))
		return false;

	if (esr_fsc_is_permission_fault(esr))
		return true;

	if (is_ttbr0_addr(addr) && system_uses_ttbr0_pan())
		return esr_fsc_is_translation_fault(esr) &&
			(regs->pstate & PSR_PAN_BIT);

	return false;
}

static bool __kprobes is_spurious_el1_translation_fault(unsigned long addr,
							unsigned long esr,
							struct pt_regs *regs)
{
	unsigned long flags;
	u64 par, dfsc;

	if (!is_el1_data_abort(esr) || !esr_fsc_is_translation_fault(esr))
		return false;

	local_irq_save(flags);
	asm volatile("at s1e1r, %0" :: "r" (addr));
	isb();
	par = read_sysreg_par();
	local_irq_restore(flags);

	/*
	 * If we now have a valid translation, treat the translation fault as
	 * spurious.
	 */
	if (!(par & SYS_PAR_EL1_F))
		return true;

	/*
	 * If we got a different type of fault from the AT instruction,
	 * treat the translation fault as spurious.
	 */
	dfsc = FIELD_GET(SYS_PAR_EL1_FST, par);
	return !esr_fsc_is_translation_fault(dfsc);
}

static void die_kernel_fault(const char *msg, unsigned long addr,
			     unsigned long esr, struct pt_regs *regs)
{
	bust_spinlocks(1);

	pr_alert("Unable to handle kernel %s at virtual address %016lx\n", msg,
		 addr);

	kasan_non_canonical_hook(addr);

	mem_abort_decode(esr);

	show_pte(addr);
	die("Oops", regs, esr);
	bust_spinlocks(0);
	make_task_dead(SIGKILL);
}

#ifdef CONFIG_KASAN_HW_TAGS
static void report_tag_fault(unsigned long addr, unsigned long esr,
			     struct pt_regs *regs)
{
	/*
	 * SAS bits aren't set for all faults reported in EL1, so we can't
	 * find out access size.
	 */
	bool is_write = !!(esr & ESR_ELx_WNR);
	kasan_report((void *)addr, 0, is_write, regs->pc);
}
#else
/* Tag faults aren't enabled without CONFIG_KASAN_HW_TAGS. */
static inline void report_tag_fault(unsigned long addr, unsigned long esr,
				    struct pt_regs *regs) { }
#endif

static void do_tag_recovery(unsigned long addr, unsigned long esr,
			   struct pt_regs *regs)
{

	report_tag_fault(addr, esr, regs);

	/*
	 * Disable MTE Tag Checking on the local CPU for the current EL.
	 * It will be done lazily on the other CPUs when they will hit a
	 * tag fault.
	 */
	sysreg_clear_set(sctlr_el1, SCTLR_EL1_TCF_MASK,
			 SYS_FIELD_PREP_ENUM(SCTLR_EL1, TCF, NONE));
	isb();
}

static bool is_el1_mte_sync_tag_check_fault(unsigned long esr)
{
	unsigned long fsc = esr & ESR_ELx_FSC;

	if (!is_el1_data_abort(esr))
		return false;

	if (fsc == ESR_ELx_FSC_MTE)
		return true;

	return false;
}

static void __do_kernel_fault(unsigned long addr, unsigned long esr,
			      struct pt_regs *regs)
{
	const char *msg;

	/*
	 * Are we prepared to handle this kernel fault?
	 * We are almost certainly not prepared to handle instruction faults.
	 */
	if (!is_el1_instruction_abort(esr) && fixup_exception(regs, esr))
		return;

	if (WARN_RATELIMIT(is_spurious_el1_translation_fault(addr, esr, regs),
	    "Ignoring spurious kernel translation fault at virtual address %016lx\n", addr))
		return;

	if (is_el1_mte_sync_tag_check_fault(esr)) {
		do_tag_recovery(addr, esr, regs);

		return;
	}

	if (is_el1_permission_fault(addr, esr, regs)) {
		if (esr & ESR_ELx_WNR)
			msg = "write to read-only memory";
		else if (is_el1_instruction_abort(esr))
			msg = "execute from non-executable memory";
		else
			msg = "read from unreadable memory";
	} else if (addr < PAGE_SIZE) {
		msg = "NULL pointer dereference";
	} else {
		if (esr_fsc_is_translation_fault(esr) &&
		    kfence_handle_page_fault(addr, esr & ESR_ELx_WNR, regs))
			return;

		msg = "paging request";
	}

	if (efi_runtime_fixup_exception(regs, msg))
		return;

	die_kernel_fault(msg, addr, esr, regs);
}

static void set_thread_esr(unsigned long address, unsigned long esr)
{
	current->thread.fault_address = address;

	/*
	 * If the faulting address is in the kernel, we must sanitize the ESR.
	 * From userspace's point of view, kernel-only mappings don't exist
	 * at all, so we report them as level 0 translation faults.
	 * (This is not quite the way that "no mapping there at all" behaves:
	 * an alignment fault not caused by the memory type would take
	 * precedence over translation fault for a real access to empty
	 * space. Unfortunately we can't easily distinguish "alignment fault
	 * not caused by memory type" from "alignment fault caused by memory
	 * type", so we ignore this wrinkle and just return the translation
	 * fault.)
	 */
	if (!is_ttbr0_addr(current->thread.fault_address)) {
		switch (ESR_ELx_EC(esr)) {
		case ESR_ELx_EC_DABT_LOW:
			/*
			 * These bits provide only information about the
			 * faulting instruction, which userspace knows already.
			 * We explicitly clear bits which are architecturally
			 * RES0 in case they are given meanings in future.
			 * We always report the ESR as if the fault was taken
			 * to EL1 and so ISV and the bits in ISS[23:14] are
			 * clear. (In fact it always will be a fault to EL1.)
			 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL |
				ESR_ELx_CM | ESR_ELx_WNR;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		case ESR_ELx_EC_IABT_LOW:
			/*
			 * Claim a level 0 translation fault.
			 * All other bits are architecturally RES0 for faults
			 * reported with that DFSC value, so we clear them.
			 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		default:
			/*
			 * This should never happen (entry.S only brings us
			 * into this code for insn and data aborts from a lower
			 * exception level). Fail safe by not providing an ESR
			 * context record at all.
			 */
			WARN(1, "ESR 0x%lx is not DABT or IABT from EL0\n", esr);
			esr = 0;
			break;
		}
	}

	current->thread.fault_code = esr;
}

static void do_bad_area(unsigned long far, unsigned long esr,
			struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	if (user_mode(regs)) {
		const struct fault_info *inf = esr_to_fault_info(esr);

		set_thread_esr(addr, esr);
		arm64_force_sig_fault(inf->sig, inf->code, far, inf->name);
	} else {
		__do_kernel_fault(addr, esr, regs);
	}
}

static bool fault_from_pkey(struct vm_area_struct *vma, unsigned int mm_flags)
{
	if (!system_supports_poe())
		return false;

	/*
	 * We do not check whether an Overlay fault has occurred because we
	 * cannot make a decision based solely on its value:
	 *
	 * - If Overlay is set, a fault did occur due to POE, but it may be
	 *   spurious in those cases where we update POR_EL0 without ISB (e.g.
	 *   on context-switch). We would then need to manually check POR_EL0
	 *   against vma_pkey(vma), which is exactly what
	 *   arch_vma_access_permitted() does.
	 *
	 * - If Overlay is not set, we may still need to report a pkey fault.
	 *   This is the case if an access was made within a mapping but with no
	 *   page mapped, and POR_EL0 forbids the access (according to
	 *   vma_pkey()). Such access will result in a SIGSEGV regardless
	 *   because core code checks arch_vma_access_permitted(), but in order
	 *   to report the correct error code - SEGV_PKUERR - we must handle
	 *   that case here.
	 */
	return !arch_vma_access_permitted(vma,
			mm_flags & FAULT_FLAG_WRITE,
			mm_flags & FAULT_FLAG_INSTRUCTION,
			false);
}

static bool is_gcs_fault(unsigned long esr)
{
	if (!esr_is_data_abort(esr))
		return false;

	return ESR_ELx_ISS2(esr) & ESR_ELx_GCS;
}

static bool is_el0_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW;
}

/*
 * Note: not valid for EL1 DC IVAC, but we never use that such that it
 * should fault. EL0 cannot issue DC IVAC (undef).
 */
static bool is_write_abort(unsigned long esr)
{
	return (esr & ESR_ELx_WNR) && !(esr & ESR_ELx_CM);
}

static bool is_invalid_gcs_access(struct vm_area_struct *vma, u64 esr)
{
	if (!system_supports_gcs())
		return false;

	if (unlikely(is_gcs_fault(esr))) {
		/* GCS accesses must be performed on a GCS page */
		if (!(vma->vm_flags & VM_SHADOW_STACK))
			return true;
	} else if (unlikely(vma->vm_flags & VM_SHADOW_STACK)) {
		/* Only GCS operations can write to a GCS page */
		return esr_is_data_abort(esr) && is_write_abort(esr);
	}

	return false;
}

static int __kprobes do_page_fault(unsigned long far, unsigned long esr,
				   struct pt_regs *regs)
{
	const struct fault_info *inf;
	struct mm_struct *mm = current->mm;
	vm_fault_t fault;
	vm_flags_t vm_flags;
	unsigned int mm_flags = FAULT_FLAG_DEFAULT;
	unsigned long addr = untagged_addr(far);
	struct vm_area_struct *vma;
	int si_code;
	int pkey = -1;

	if (kprobe_page_fault(regs, esr))
		return 0;

	/*
	 * If we're in an interrupt or have no user context, we must not take
	 * the fault.
	 */
	if (faulthandler_disabled() || !mm)
		goto no_context;

	if (user_mode(regs))
		mm_flags |= FAULT_FLAG_USER;

	/*
	 * vm_flags tells us what bits we must have in vma->vm_flags
	 * for the fault to be benign, __do_page_fault() would check
	 * vma->vm_flags & vm_flags and returns an error if the
	 * intersection is empty
	 */
	if (is_el0_instruction_abort(esr)) {
		/* It was exec fault */
		vm_flags = VM_EXEC;
		mm_flags |= FAULT_FLAG_INSTRUCTION;
	} else if (is_gcs_fault(esr)) {
		/*
		 * The GCS permission on a page implies both read and
		 * write so always handle any GCS fault as a write fault,
		 * we need to trigger CoW even for GCS reads.
		 */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else if (is_write_abort(esr)) {
		/* It was write fault */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else {
		/* It was read fault */
		vm_flags = VM_READ;
		/* Write implies read */
		vm_flags |= VM_WRITE;
		/* If EPAN is absent then exec implies read */
		if (!alternative_has_cap_unlikely(ARM64_HAS_EPAN))
			vm_flags |= VM_EXEC;
	}

	if (is_ttbr0_addr(addr) && is_el1_permission_fault(addr, esr, regs)) {
		if (is_el1_instruction_abort(esr))
			die_kernel_fault("execution of user memory",
					 addr, esr, regs);

		if (!insn_may_access_user(regs->pc, esr))
			die_kernel_fault("access to user memory outside uaccess routines",
					 addr, esr, regs);
	}

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, addr);

	if (!(mm_flags & FAULT_FLAG_USER))
		goto lock_mmap;

	vma = lock_vma_under_rcu(mm, addr);
	if (!vma)
		goto lock_mmap;

	if (is_invalid_gcs_access(vma, esr)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_PKUERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags | FAULT_FLAG_VMA_LOCK, regs);
	if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
		vma_end_read(vma);

	if (!(fault & VM_FAULT_RETRY)) {
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto done;
	}
	count_vm_vma_lock_event(VMA_LOCK_RETRY);
	if (fault & VM_FAULT_MAJOR)
		mm_flags |= FAULT_FLAG_TRIED;

	/* Quick path to respond to signals */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}
lock_mmap:

retry:
	vma = lock_mm_and_find_vma(mm, addr, regs);
	if (unlikely(!vma)) {
		fault = 0;
		si_code = SEGV_MAPERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_PKUERR;
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags, regs);

	/* Quick path to respond to signals */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}

	/* The fault is fully completed (including releasing mmap lock) */
	if (fault & VM_FAULT_COMPLETED)
		return 0;

	if (fault & VM_FAULT_RETRY) {
		mm_flags |= FAULT_FLAG_TRIED;
		goto retry;
	}
	mmap_read_unlock(mm);

done:
	/* Handle the "normal" (no error) case first. */
	if (likely(!(fault & VM_FAULT_ERROR)))
		return 0;

	si_code = SEGV_MAPERR;
bad_area:
	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	if (!user_mode(regs))
		goto no_context;

	if (fault & VM_FAULT_OOM) {
		/*
		 * We ran out of memory, call the OOM killer, and return to
		 * userspace (which will retry the fault, or kill us if we got
		 * oom-killed).
		 */
		pagefault_out_of_memory();
		return 0;
	}

	inf = esr_to_fault_info(esr);
	set_thread_esr(addr, esr);
	if (fault & VM_FAULT_SIGBUS) {
		/*
		 * We had some memory, but were unable to successfully fix up
		 * this page fault.
		 */
		arm64_force_sig_fault(SIGBUS, BUS_ADRERR, far, inf->name);
	} else if (fault & (VM_FAULT_HWPOISON_LARGE | VM_FAULT_HWPOISON)) {
		unsigned int lsb;

		lsb = PAGE_SHIFT;
		if (fault & VM_FAULT_HWPOISON_LARGE)
			lsb = hstate_index_to_shift(VM_FAULT_GET_HINDEX(fault));

		arm64_force_sig_mceerr(BUS_MCEERR_AR, far, lsb, inf->name);
	} else {
		/*
		 * The pkey value that we return to userspace can be different
		 * from the pkey that caused the fault.
		 *
		 * 1. T1   : mprotect_key(foo, PAGE_SIZE, pkey=4);
		 * 2. T1   : set POR_EL0 to deny access to pkey=4, touches, page
		 * 3. T1   : faults...
		 * 4.    T2: mprotect_key(foo, PAGE_SIZE, pkey=5);
		 * 5. T1   : enters fault handler, takes mmap_lock, etc...
		 * 6. T1   : reaches here, sees vma_pkey(vma)=5, when we really
		 *	     faulted on a pte with its pkey=4.
		 */
		/* Something tried to access memory that out of memory map */
		if (si_code == SEGV_PKUERR)
			arm64_force_sig_fault_pkey(far, inf->name, pkey);
		else
			arm64_force_sig_fault(SIGSEGV, si_code, far, inf->name);
	}

	return 0;

no_context:
	__do_kernel_fault(addr, esr, regs);
	return 0;
}

static int __kprobes do_translation_fault(unsigned long far,
					  unsigned long esr,
					  struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	if (is_ttbr0_addr(addr))
		return do_page_fault(far, esr, regs);

	do_bad_area(far, esr, regs);
	return 0;
}

static int do_alignment_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_COMPAT_ALIGNMENT_FIXUPS) &&
	    compat_user_mode(regs))
		return do_compat_alignment_fixup(far, regs);
	do_bad_area(far, esr, regs);
	return 0;
}

static int do_bad(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	return 1; /* "fault" */
}

static int do_sea(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf;
	unsigned long siaddr;

	inf = esr_to_fault_info(esr);

	if (user_mode(regs) && apei_claim_sea(regs) == 0) {
		/*
		 * APEI claimed this as a firmware-first notification.
		 * Some processing deferred to task_work before ret_to_user().
		 */
		return 0;
	}

	if (esr & ESR_ELx_FnV) {
		siaddr = 0;
	} else {
		/*
		 * The architecture specifies that the tag bits of FAR_EL1 are
		 * UNKNOWN for synchronous external aborts. Mask them out now
		 * so that userspace doesn't see them.
		 */
		siaddr  = untagged_addr(far);
	}
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, siaddr, esr);

	return 0;
}

static int do_tag_check_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	/*
	 * The architecture specifies that bits 63:60 of FAR_EL1 are UNKNOWN
	 * for tag check faults. Set them to corresponding bits in the untagged
	 * address if ARM64_MTE_FAR isn't supported.
	 * Otherwise, bits 63:60 of FAR_EL1 are not UNKNOWN.
	 */
	if (!cpus_have_cap(ARM64_MTE_FAR))
		far = (__untagged_addr(far) & ~MTE_TAG_MASK) | (far & MTE_TAG_MASK);

	do_bad_area(far, esr, regs);
	return 0;
}

static const struct fault_info fault_info[] = {
	{ do_bad,		SIGKILL, SI_KERNEL,	"ttbr address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 2 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 3 address size fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 0 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 1 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 2 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 3 translation fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 permission fault"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous external abort"	},
	{ do_tag_check_fault,	SIGSEGV, SEGV_MTESERR,	"synchronous tag check fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 18"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 (translation table walk)"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous parity or ECC error" },	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 25"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 26"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 32"			},
	{ do_alignment_fault,	SIGBUS,  BUS_ADRALN,	"alignment fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 34"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 35"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 36"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 37"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 38"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 39"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 40"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level -1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 42"			},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level -1 translation fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 44"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 45"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 46"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 47"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"TLB conflict abort"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"Unsupported atomic hardware update fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 50"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 51"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"implementation fault (lockdown abort)" },
	{ do_bad,		SIGBUS,  BUS_OBJERR,	"implementation fault (unsupported exclusive)" },
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 54"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 55"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 56"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 57"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 58" 			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 59"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 60"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"section domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"page domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 63"			},
};

void do_mem_abort(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf = esr_to_fault_info(esr);
	unsigned long addr = untagged_addr(far);

	if (!inf->fn(far, esr, regs))
		return;

	if (!user_mode(regs))
		die_kernel_fault(inf->name, addr, esr, regs);

	/*
	 * At this point we have an unrecognized fault type whose tag bits may
	 * have been defined as UNKNOWN. Therefore we only expose the untagged
	 * address to the signal handler.
	 */
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, addr, esr);
}
NOKPROBE_SYMBOL(do_mem_abort);

void do_sp_pc_abort(unsigned long addr, unsigned long esr, struct pt_regs *regs)
{
	arm64_notify_die("SP/PC alignment exception", regs, SIGBUS, BUS_ADRALN,
			 addr, esr);
}
NOKPROBE_SYMBOL(do_sp_pc_abort);

/*
 * Used during anonymous page fault handling.
 */
struct folio *vma_alloc_zeroed_movable_folio(struct vm_area_struct *vma,
						unsigned long vaddr)
{
	gfp_t flags = GFP_HIGHUSER_MOVABLE | __GFP_ZERO;

	/*
	 * If the page is mapped with PROT_MTE, initialise the tags at the
	 * point of allocation and page zeroing as this is usually faster than
	 * separate DC ZVA and STGM.
	 */
	if (vma->vm_flags & VM_MTE)
		flags |= __GFP_ZEROTAGS;

	return vma_alloc_folio(flags, 0, vma, vaddr);
}

void tag_clear_highpage(struct page *page)
{
	/* Newly allocated page, shouldn't have been tagged yet */
	WARN_ON_ONCE(!try_page_mte_tagging(page));
	mte_zero_clear_page_tags(page_address(page));
	set_page_mte_tagged(page);
}
