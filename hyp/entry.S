/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PSCI for MSM8994 phones whose TrustZone has none: a payload for the
 * Qualcomm hypervisor, entered at EL2 from its path for unhandled HVCs.
 *
 *   x19 = frame with the caller's x0..x30, w20 = function ID
 *
 * Return through the epilogue of the hypervisor's HVC dispatcher, which
 * restores the frame and goes back to EL1 with ELR_EL2/SPSR_EL2.
 */

#define HYP_EPILOGUE	0x6c01580
#define DATA_PAGE	0x6c17000
#define MBOX		(DATA_PAGE + 0x40)

#define SCM_TERMINATE_PC	0x02000102
#define SCM_CORE_HOTPLUGGED	0x10

	.section .head, "ax"
	.globl	_start
_start:
	b	hvc_entry
	.word	0
	.ascii	"qhypstub"

/*
 * Offset 0x10: cold and warm boot address of all cores, entered at EL1 with
 * the MMU off.  Go where the mailbox says, or back to sleep when nobody
 * asked for this core.
 */
	.globl	el1_entry
el1_entry:
	mrs	x1, mpidr_el1
	ubfx	x2, x1, #8, #8
	and	x3, x1, #0xff
	orr	x2, x3, x2, lsl #2		/* core index */
	movz	x4, #(MBOX & 0xffff)
	movk	x4, #(MBOX >> 16), lsl #16
	add	x1, x4, x2, lsl #6
	ldr	x5, [x1]			/* entry point */
	cbz	x5, 2f
	ldr	x0, [x1, #8]			/* context */
	str	xzr, [x1]
	/* The SGI that CPU_ON woke this core with has done its job */
	movz	x7, #0xf900, lsl #16
	mov	w6, #0x4000			/* SGI 14 */
	str	w6, [x7, #0x180]
	mov	w6, #0x00ff0000
	str	w6, [x7, #0xf1c]
	ldr	w6, [x1, #0x18]
	cbz	w6, 1f
	/* SPM back to clock gating on WFI */
	movz	x7, #0x9000
	movk	x7, #0xf908, lsl #16
	add	x7, x7, x2, lsl #16		/* SAW of this core */
	ldr	w6, [x1, #0x10]
	str	w6, [x7, #0x30]
	add	x7, x7, #0x2000			/* ACC */
	ldr	w6, [x1, #0x14]
	str	w6, [x7, #0x60]
1:	dsb	sy
	br	x5

2:	movz	x0, #(SCM_TERMINATE_PC & 0xffff)
	movk	x0, #(SCM_TERMINATE_PC >> 16), lsl #16
	mov	x1, #1
	mov	x2, #SCM_CORE_HOTPLUGGED
	mov	x3, #0
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	smc	#0
	b	el1_entry

hvc_entry:
	mov	x0, x19
	mov	w1, w20
	bl	psci_handler
	str	x0, [x19]
	movz	x30, #(HYP_EPILOGUE & 0xffff)
	movk	x30, #(HYP_EPILOGUE >> 16), lsl #16
	br	x30

	.text
/* u64 smc(u64 fn, u64 arginfo, u64 arg0, u64 arg1, u64 *res) */
	.globl	smc
smc:
	stp	x29, x30, [sp, #-112]!
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	str	x4, [sp, #96]
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	smc	#0
	ldr	x4, [sp, #96]
	cbz	x4, 1f
	str	x1, [x4]
1:	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp], #112
	ret

/*
 * Cortex-A57 power-down sequence around TERMINATE_PC, no stack, only x0-x17:
 * stop allocating data cache lines at EL2, clean+invalidate the L1D by set/way,
 * leave the coherency domain (CPUECTLR_EL1.SMPEN = 0), call TZ.  If TZ returns the
 * core did not collapse: join the coherency domain again and re-enable the cache.
 * (After a real collapse the warm boot path sets SMPEN again.)
 *
 * u64 a57_pd_smc(void)
 */
	.globl	a57_pd_smc
a57_pd_smc:
	mrs	x9, sctlr_el2
	bic	x9, x9, #4			/* C */
	msr	sctlr_el2, x9
	isb
	dsb	sy
	msr	csselr_el1, xzr
	isb
	mrs	x1, ccsidr_el1
	and	x2, x1, #7
	add	x2, x2, #4			/* log2(line size) */
	ubfx	x3, x1, #3, #10			/* ways - 1 */
	ubfx	x4, x1, #13, #15		/* sets - 1 */
	clz	w5, w3				/* way shift */
	mov	x10, x3
1:	mov	x7, x4
2:	lsl	x8, x10, x5
	lsl	x6, x7, x2
	orr	x8, x8, x6
	dc	cisw, x8
	subs	x7, x7, #1
	b.ge	2b
	subs	x10, x10, #1
	b.ge	1b
	dsb	sy
	isb
	mrs	x1, S3_1_C15_C2_1
	bic	x1, x1, #0x40			/* SMPEN */
	msr	S3_1_C15_C2_1, x1
	isb
	dsb	sy

	movz	x0, #(SCM_TERMINATE_PC & 0xffff)
	movk	x0, #(SCM_TERMINATE_PC >> 16), lsl #16
	mov	x1, #1
	mov	x2, #SCM_CORE_HOTPLUGGED
	mov	x3, #0
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	smc	#0
	mov	x11, x0

	mrs	x1, S3_1_C15_C2_1
	orr	x1, x1, #0x40
	msr	S3_1_C15_C2_1, x1
	isb
	mrs	x9, sctlr_el2
	orr	x9, x9, #4
	msr	sctlr_el2, x9
	isb
	dsb	sy
	mov	x0, x11
	ret
