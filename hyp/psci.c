// SPDX-License-Identifier: GPL-2.0-only
/*
 * PSCI 1.0 for MSM8994, running at EL2 inside the Qualcomm hypervisor.
 *
 * The power sequences are those of the vendor kernel
 * (drivers/soc/qcom/cpu_pwr_ctl.c, spm_devices.c, drivers/power/qcom/msm-pm.c):
 *
 *  Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 */

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define BIT(n)			(1UL << (n))

#define HCR_FMO			BIT(3)
#define HCR_IMO			BIT(4)

#define PSCI_SUCCESS		0UL
#define PSCI_NOT_SUPPORTED	((u64)-1)
#define PSCI_INVALID_PARAMS	((u64)-2)
#define PSCI_ALREADY_ON		((u64)-4)

#define PSCI_VERSION_1_0	0x00010000
#define PSCI_STATE_TYPE_PD	BIT(16)
#define PSCI_STATE_ID_RET	1

/* Data page, zeroed by the loader, which also marks the boot CPU as on */
#define DATA_PAGE		0x6c17000UL
#define CPU_STATE		((volatile u8 *)(DATA_PAGE + 0x300))
#define SPM_READY		((volatile u8 *)(DATA_PAGE + 0x310))

enum { CPU_NEVER_ON, CPU_ON, CPU_OFF };

/* One cache line per core, shared with el1_entry which runs uncached */
struct mbox {
	u64 entry;
	u64 context;
	u32 spm_ctl_stby;
	u32 q2s_stby;
	u32 spm_ready;
};
#define MBOX(i)			((volatile struct mbox *)(DATA_PAGE + 0x40 + (i) * 0x40))

#define SCM_SET_SEC_IO_READ	0x02000501
#define SCM_SET_SEC_IO_WRITE	0x02000502
#define SCM_TERMINATE_PC	0x02000102
#define SCM_CORE_HOTPLUGGED	0x10

#define GICD			0xf9000000UL
#define GICD_ISENABLER0		(GICD + 0x100)
#define GICD_ICENABLER0		(GICD + 0x180)
#define GICC_CTLR		(0xf9002000UL)
#define GICC_HPPIR		(0xf9002000UL + 0x18)
#define GICD_SGIR		(GICD + 0xf00)
#define GICD_CPENDSGIR3		(GICD + 0xf1c)
/*
 * Linux uses SGIs 0-7 and TrustZone keeps 15 secure: 14 is free, and stays
 * disabled while the OS runs on the core.
 */
#define WAKE_SGI		14

/* Per core: SAW2 (SPM), ACC and LDO/BHS controller */
#define SAW(i)			(0xf9089000UL + (i) * 0x10000UL)
#define ACC(i)			(0xf908b000UL + (i) * 0x10000UL)
#define LDO(i)			(0xf9070000UL + (i) * 0x1000UL)

#define SAW2_CFG		0x08
#define SAW2_SPM_CTL		0x30
#define SAW2_SPM_DLY		0x34
#define SAW2_SEQ_ENTRY		0x400
#define SPM_CTL_EN		BIT(0)
#define SPM_CTL_PC_MODE		BIT(16)

#define CPU_PWR_CTL		0x04
#define CPU_PWR_GATE_CTL	0x14
#define CPU_Q2S_CTL		0x60
#define Q2S_QCHANNEL_IGNORE	BIT(1)
#define Q2S_SPM_LEGACY_MODE	BIT(2)

#define APC_LDO_VREF_CFG	0x04
#define APC_LDO_CFG1		0x0c
#define APC_LDO_CFG2		0x10
#define APC_LDO_BHS_PWR_CTL	0x28

/* Per cluster: L2 clock/power controller and L2 SAW */
#define L2CCC(cl)		(0xf900d000UL + (cl) * 0x2000UL)
#define L2_SPM(cl)		(0xf9012000UL + (cl) * 0x1000UL)
#define L2_PWR_CTL_OVERRIDE	0x0c
#define L2_PWR_CTL		0x14
#define L2_MUX			0x54
#define L2_Q2S_CTL		0x210
#define L1_RST_DIS		0x284
#define L2_SPM_STS		0x0c
#define L2_VREG_CTL		0x1c
#define L2_PWR_ON_MASK		(BIT(9) | BIT(28))
#define APCS_L2_SEC_REG		0xf911210cUL

#define C1_PLL			0xf9016000UL
#define PLL_MODE		0x00
#define PLL_L_VAL		0x04
#define PLL_ALPHA		0x08
#define PLL_USER_CTL		0x10
#define PLL_CONFIG_CTL		0x14
#define PLL_TEST_CTL_LO		0x20
#define PLL_TEST_CTL_HI		0x24
#define PLL_MODE_OUTCTRL	BIT(0)
#define PLL_MODE_BYPASSNL	BIT(1)
#define PLL_MODE_RESET_N	BIT(2)
#define PLL_MODE_APC_PDN	BIT(24)
#define PLL_MODE_LOCK		BIT(31)

enum spm_mode { SPM_STBY, SPM_RET, SPM_PC, SPM_NR };

/*
 * SPM sequences (WFI, retention, power collapse) of msm8994-v2-pm.dtsi, as
 * the little-endian words of the sequence registers.
 */
static const u32 seq_a53[] = {
	0x0f0b2f03, 0xc0002b50, 0xe2b03b14, 0x501b92a0, 0x500b2f03,
	0xe21b922b, 0x14b0a01b, 0xc070703b, 0x000f5000, 0x2b503040,
	0x243b14c0, 0xa0e2803b, 0x03501b92, 0x2b500b2f, 0x1be21b92,
	0x3b2480a0, 0x50c03b14, 0x0f304000,
};
static const u8 start_a53[SPM_NR] = { 0, 4, 35 };

static const u32 seq_a57[] = {
	0x542b5003, 0x002b500f, 0x1f3b14c0, 0x1f1f1f1f, 0x0b2f0350,
	0x1f1f2b50, 0x1f1f1f1f, 0x70703b14, 0x0f5000c0, 0x50304000,
	0x3b14c02b, 0xe2803b24, 0x501b92a0, 0x500b2f03, 0xe21b922b,
	0x2480a01b, 0xc03b143b, 0x30400050, 0x0000000f,
};
static const u8 start_a57[SPM_NR] = { 0, 5, 36 };

u64 smc(u64 fn, u64 arginfo, u64 arg0, u64 arg1, u64 *res);

static inline u32 rd(u64 addr)
{
	return *(volatile u32 *)addr;
}

static inline void wr(u32 val, u64 addr)
{
	*(volatile u32 *)addr = val;
	__asm__ volatile("dsb sy");
}

static u64 ticks(void)
{
	u64 t;

	__asm__ volatile("mrs %0, cntpct_el0" : "=r" (t));
	return t;
}

static void udelay(u32 us)
{
	u64 hz, end;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r" (hz));
	end = ticks() + (hz * us + 999999) / 1000000;
	while (ticks() < end)
		;
}

/* The mailbox as el1_entry left it in memory, and back out to memory */
static void mbox_fetch(volatile struct mbox *m)
{
	__asm__ volatile("dc ivac, %0\n dsb sy" : : "r" (m));
}

static void mbox_flush(volatile struct mbox *m)
{
	__asm__ volatile("dc civac, %0\n dsb sy" : : "r" (m));
}

static unsigned int this_cpu(void)
{
	u64 mpidr;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));
	return (mpidr & 0x3) | ((mpidr >> 8) & 0x1) << 2;
}

static u32 sec_io_read(u64 addr)
{
	u64 val = 0;

	if (smc(SCM_SET_SEC_IO_READ, 1, addr, 0, &val))
		return 0;
	return val;
}

static void sec_io_write(u64 addr, u32 val)
{
	smc(SCM_SET_SEC_IO_WRITE, 2, addr, val, 0);
}

static void spm_set_mode(unsigned int cpu, enum spm_mode mode)
{
	const u8 *start = cpu < 4 ? start_a53 : start_a57;
	u32 ctl = 0x8 | SPM_CTL_EN | start[mode] << 4, q2s = 0;
	int retry = 100;

	if (mode == SPM_PC) {
		ctl |= SPM_CTL_PC_MODE;
		/* v2 parts do not use the Q-channel for the power collapse */
		q2s = Q2S_SPM_LEGACY_MODE | Q2S_QCHANNEL_IGNORE;
	} else if (mode == SPM_STBY && cpu < 4) {
		/* Only the Cortex-A57 SPM takes the Q-channel for WFI */
		q2s = Q2S_QCHANNEL_IGNORE;
	}

	do {
		wr(ctl, SAW(cpu) + SAW2_SPM_CTL);
	} while (rd(SAW(cpu) + SAW2_SPM_CTL) != ctl && retry--);
	wr(q2s, ACC(cpu) + CPU_Q2S_CTL);
}

/*
 * The bootloader leaves the Cortex-A57 cluster on XO with its PLL powered
 * down: run it from PLL0 / 2 at 768 MHz, as the Cortex-A53 cluster.
 */
static void a57_clock_init(void)
{
	u64 pll = C1_PLL, mux = L2CCC(1) + L2_MUX;
	u32 mode;
	int timeout = 1000;

	if ((rd(pll + PLL_MODE) & (PLL_MODE_LOCK | PLL_MODE_OUTCTRL)) ==
	    (PLL_MODE_LOCK | PLL_MODE_OUTCTRL))
		return;

	wr(0x11f, pll + PLL_USER_CTL);
	wr(rd(pll + PLL_MODE) & ~PLL_MODE_APC_PDN, pll + PLL_MODE);
	wr(0, pll + PLL_ALPHA);
	wr(0x004d6968, pll + PLL_CONFIG_CTL);
	wr(0x80000000, pll + PLL_TEST_CTL_LO);
	wr(0x1, pll + PLL_TEST_CTL_HI);
	wr(0x50, pll + PLL_L_VAL);

	mode = rd(pll + PLL_MODE) | PLL_MODE_BYPASSNL;
	wr(mode, pll + PLL_MODE);
	udelay(10);
	mode |= PLL_MODE_RESET_N;
	wr(mode, pll + PLL_MODE);
	udelay(200);

	while (!(rd(pll + PLL_MODE) & PLL_MODE_LOCK)) {
		if (!timeout--)
			return;
		udelay(1);
	}
	wr(mode | PLL_MODE_OUTCTRL, pll + PLL_MODE);

	/* Low frequency mux to PLL0 / 2, high frequency mux to the former */
	wr((rd(mux) & ~((0x3 << 1) | (0x3 << 3))) | (2 << 1), mux);
	udelay(1);
}

static void power_on_l2(unsigned int cl)
{
	u64 l2 = L2CCC(cl), spm = L2_SPM(cl);
	int timeout = 100;

	if ((rd(l2 + L2_PWR_CTL) & L2_PWR_ON_MASK) == L2_PWR_ON_MASK) {
		/* On, but its SPM may be in a sleep sequence: wake it up */
		if (!(rd(spm + L2_SPM_STS) & 0xffff0000))
			return;
		sec_io_write(APCS_L2_SEC_REG, sec_io_read(APCS_L2_SEC_REG) | BIT(0));
		while ((rd(spm + L2_SPM_STS) & 0xffff0000) && timeout--)
			udelay(100);
		return;
	}

	/* Only the Cortex-A57 L2 is ever found off; 0xb8 is its rail value */
	wr(0x2, l2 + L2_Q2S_CTL);
	wr(0xb8, spm + L2_VREG_CTL);
	udelay(2000);
	wr(0x30080, spm + L2_VREG_CTL);
	udelay(2000);

	wr(0x00000000, l2 + L1_RST_DIS);
	wr(0x00400000, l2 + L2_PWR_CTL_OVERRIDE);
	wr(0x00029716, l2 + L2_PWR_CTL);
	udelay(8);
	wr(0x00023716, l2 + L2_PWR_CTL);
	wr(0x0002371e, l2 + L2_PWR_CTL);
	udelay(8);
	wr(0x0002371c, l2 + L2_PWR_CTL);
	udelay(4);
	wr(0x0002361c, l2 + L2_PWR_CTL);
	udelay(2);
	wr(0x00022218, l2 + L2_PWR_CTL);
	udelay(4);
	wr(0x10022218, l2 + L2_PWR_CTL);
	wr(0x00000000, l2 + L2_PWR_CTL_OVERRIDE);
}

static void ldo_config(unsigned int cpu)
{
	u64 ldo = LDO(cpu);
	u32 vref_ret = cpu < 4 ? 0x2a : 0x3e;

	wr((rd(ldo + APC_LDO_BHS_PWR_CTL) & 0xffffff00) | 0x12,
	   ldo + APC_LDO_BHS_PWR_CTL);
	wr((rd(ldo + APC_LDO_CFG1) & 0xffffff00) | 0xc2, ldo + APC_LDO_CFG1);
	wr((rd(ldo + APC_LDO_CFG1) & 0xffff00ff) | (0xf8 << 8), ldo + APC_LDO_CFG1);
	wr((rd(ldo + APC_LDO_CFG2) & 0xffffff00) | 0x60, ldo + APC_LDO_CFG2);
	wr((rd(ldo + APC_LDO_CFG2) & 0xff00ffff) | (0x4a << 16), ldo + APC_LDO_CFG2);

	/* LDO out of reset, retention voltage, latch it */
	wr(rd(ldo + APC_LDO_VREF_CFG) & ~BIT(16), ldo + APC_LDO_VREF_CFG);
	wr((rd(ldo + APC_LDO_VREF_CFG) & 0xffff80ff) | (vref_ret << 8),
	   ldo + APC_LDO_VREF_CFG);
	wr(0x0, ldo);
	wr(0x1, ldo);
	udelay(1);
	wr(0x0, ldo);
}

/* On the core itself, the first time it asks for a low power state */
static void spm_init(unsigned int cpu)
{
	volatile struct mbox *m = MBOX(cpu);
	const u32 *seq = cpu < 4 ? seq_a53 : seq_a57;
	unsigned int i, n = cpu < 4 ? sizeof(seq_a53) / 4 : sizeof(seq_a57) / 4;

	if (SPM_READY[cpu])
		return;

	wr(rd(SAW(cpu) + SAW2_SPM_CTL) & ~SPM_CTL_EN, SAW(cpu) + SAW2_SPM_CTL);
	for (i = 0; i < n; i++)
		wr(seq[i], SAW(cpu) + SAW2_SEQ_ENTRY + 4 * i);
	wr(0x1, SAW(cpu) + SAW2_CFG);
	wr(0x3c100c00, SAW(cpu) + SAW2_SPM_DLY);

	/* The others got their retention LDO set up when they were started */
	if (!cpu)
		ldo_config(cpu);

	mbox_fetch(m);
	m->spm_ctl_stby = 0x8 | SPM_CTL_EN;
	m->q2s_stby = cpu < 4 ? Q2S_QCHANNEL_IGNORE : 0;
	m->spm_ready = 1;
	mbox_flush(m);

	spm_set_mode(cpu, SPM_STBY);
	SPM_READY[cpu] = 1;
}

/* First power-up of a core since reset */
static void cold_boot(unsigned int cpu)
{
	unsigned int cl = cpu >> 2;
	u64 acc = ACC(cpu);

	if (cl == 1)
		a57_clock_init();
	power_on_l2(cl);

	/*
	 * No SPM sequence on WFI and no Q-channel until the core programs
	 * its SPM itself: it would wait in WFI for an SPM that never answers.
	 */
	wr(Q2S_QCHANNEL_IGNORE, acc + CPU_Q2S_CTL);
	wr(rd(SAW(cpu) + SAW2_SPM_CTL) & ~SPM_CTL_EN, SAW(cpu) + SAW2_SPM_CTL);

	wr(0x00000001, acc + CPU_PWR_GATE_CTL);
	udelay(1);
	wr(0x00000003, acc + CPU_PWR_GATE_CTL);
	udelay(1);
	wr(0x00000079, acc + CPU_PWR_CTL);
	udelay(2);
	wr(0x0000007d, acc + CPU_PWR_CTL);
	udelay(2);
	wr(0x0000003d, acc + CPU_PWR_CTL);
	wr(0x0000003c, acc + CPU_PWR_CTL);
	udelay(1);
	wr(0x0000000c, acc + CPU_PWR_CTL);
	wr(0x0000008c, acc + CPU_PWR_CTL);

	sec_io_write(APCS_L2_SEC_REG, sec_io_read(APCS_L2_SEC_REG) & ~BIT(0));

	ldo_config(cpu);
}

/*
 * An interrupt for EL1 does not end a WFI at EL2: route them to EL2 for its
 * duration.  They stay masked here and are taken at EL1 once back there.
 */
static void wfi_el2(void)
{
	u64 hcr;

	__asm__ volatile("mrs %0, hcr_el2" : "=r" (hcr));
	__asm__ volatile("msr hcr_el2, %0\n"
			 "isb\n"
			 "dsb sy\n"
			 "wfi\n"
			 "msr hcr_el2, %1\n"
			 "isb"
			 : : "r" (hcr | HCR_IMO | HCR_FMO), "r" (hcr) : "memory");
}

/* Leave the hypervisor at @entry with the MMU off, as after a reset */
static u64 enter_el1(u64 *frame, u64 entry, u64 context)
{
	frame[1] = 0;
	frame[2] = 0;
	frame[3] = 0;

	__asm__ volatile("msr sctlr_el1, %0\n"
			 "tlbi vmalle1\n"
			 "ic iallu\n"
			 "dsb sy\n"
			 "isb\n"
			 "msr spsr_el2, %1\n"
			 "msr elr_el2, %2\n"
			 : : "r" (0x00c00800UL), "r" (0x3c5UL), "r" (entry));
	return context;
}

extern u64 a57_pd_smc(void);

static u64 cpu_suspend(u64 *frame)
{
	unsigned int cpu = this_cpu();
	volatile struct mbox *m = MBOX(cpu);
	u32 state = frame[1];

	spm_init(cpu);
	if (!(state & PSCI_STATE_TYPE_PD)) {
		if ((state & 0xffff) == PSCI_STATE_ID_RET)
			spm_set_mode(cpu, SPM_RET);
		wfi_el2();
		spm_set_mode(cpu, SPM_STBY);
		return PSCI_SUCCESS;
	}

	mbox_fetch(m);
	m->entry = frame[2];
	m->context = frame[3];
	mbox_flush(m);

	/* An interrupt is already pending: the SPM would miss it. Take it at the resume point. */
	if ((rd(GICC_HPPIR) & 0x3ff) != 0x3ff) {
		mbox_fetch(m);
		m->entry = 0;
		mbox_flush(m);
		return enter_el1(frame, frame[2], frame[3]);
	}

	spm_set_mode(cpu, SPM_PC);
	if (cpu >= 4)
		a57_pd_smc();	/* the A57 needs its power-down sequence done by hand, see entry.S */
	else
		smc(SCM_TERMINATE_PC, 1, SCM_CORE_HOTPLUGGED, 0, 0);

	/*
	 * An interrupt was pending: no power collapse.  Resume as if woken up,
	 * which is what the caller has prepared for.
	 */
	spm_set_mode(cpu, SPM_STBY);
	mbox_fetch(m);
	m->entry = 0;
	mbox_flush(m);
	return enter_el1(frame, frame[2], frame[3]);
}

static u64 cpu_off(u64 *frame)
{
	unsigned int cpu = this_cpu();
	volatile struct mbox *m = MBOX(cpu);
	u64 entry, context;

	spm_init(cpu);

	mbox_fetch(m);
	m->entry = 0;
	mbox_flush(m);
	CPU_STATE[cpu] = CPU_OFF;

	/*
	 * Only an interrupt gets a core out of the power collapse, and the OS
	 * has disabled all those of a core it takes offline.
	 */
	wr(BIT(WAKE_SGI), GICD_ISENABLER0);

	spm_set_mode(cpu, SPM_PC);
	do {
		smc(SCM_TERMINATE_PC, 1, SCM_CORE_HOTPLUGGED, 0, 0);
		/* Still here: CPU_ON came before the power collapse, or noise */
		mbox_fetch(m);
	} while (!m->entry);

	entry = m->entry;
	context = m->context;
	m->entry = 0;
	mbox_flush(m);
	spm_set_mode(cpu, SPM_STBY);
	wr(BIT(WAKE_SGI), GICD_ICENABLER0);
	wr(0xffU << 16, GICD_CPENDSGIR3);

	return enter_el1(frame, entry, context);
}

static int target_cpu(u64 mpidr)
{
	if (mpidr & ~0x103UL)
		return -1;
	return (mpidr & 0x3) | ((mpidr >> 8) & 0x1) << 2;
}

static u64 cpu_on(u64 *frame)
{
	int cpu = target_cpu(frame[1]);
	volatile struct mbox *m;
	u8 state;

	if (cpu < 0)
		return PSCI_INVALID_PARAMS;

	state = CPU_STATE[cpu];
	if (state == CPU_ON)
		return PSCI_ALREADY_ON;

	m = MBOX(cpu);
	mbox_fetch(m);
	m->entry = frame[2];
	m->context = frame[3];
	mbox_flush(m);

	CPU_STATE[cpu] = CPU_ON;
	if (state == CPU_NEVER_ON)
		cold_boot(cpu);
	else
		/* Power collapsed: any interrupt wakes it up, to el1_entry */
		wr(BIT(16 + cpu) | WAKE_SGI, GICD_SGIR);

	return PSCI_SUCCESS;
}

/*
 * The Cortex-A57 cores come out of reset (and out of power collapse) with
 * CPUECTLR_EL1.SMPEN clear: they are not in the coherency domain although
 * Linux runs SMP on them.  Join it.  (The Cortex-A53 cores come up with it set.)
 */
static void smpen(unsigned int cpu)
{
	u64 e;

	if (cpu < 4)
		return;
	__asm__ volatile("mrs %0, S3_1_C15_C2_1" : "=r" (e));
	if (!(e & 0x40))
		__asm__ volatile("msr S3_1_C15_C2_1, %0\n isb" : : "r" (e | 0x40));
}

u64 psci_handler(u64 *frame, u32 fid)
{
	int cpu;

	smpen(this_cpu());

	if ((fid & 0xbfffff00) != 0x84000000)
		return PSCI_NOT_SUPPORTED;

	switch (fid & 0xff) {
	case 0x00:	/* PSCI_VERSION */
		return PSCI_VERSION_1_0;
	case 0x01:	/* CPU_SUSPEND */
		return cpu_suspend(frame);
	case 0x02:	/* CPU_OFF */
		return cpu_off(frame);
	case 0x03:	/* CPU_ON */
		return cpu_on(frame);
	case 0x04:	/* AFFINITY_INFO */
		cpu = target_cpu(frame[1]);
		if (cpu < 0)
			return PSCI_INVALID_PARAMS;
		return CPU_STATE[cpu] == CPU_ON ? 0 : 1;
	case 0x06:	/* MIGRATE_INFO_TYPE: no trusted OS to migrate */
		return 2;
	case 0x0a:	/* PSCI_FEATURES */
		fid = frame[1];
		if ((fid & 0xbfffff00) != 0x84000000)
			return PSCI_NOT_SUPPORTED;
		switch (fid & 0xff) {
		case 0x00 ... 0x04:
		case 0x06:
		case 0x0a:
			return 0;
		}
	}

	return PSCI_NOT_SUPPORTED;
}
