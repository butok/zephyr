/*
 * Copyright (c) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>
#include <zephyr/sys/check.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/arch/xtensa/arch.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/pm/device_runtime.h>
#include <ksched.h>

#include <soc.h>
#include <adsp_boot.h>
#include <adsp_power.h>
#include <adsp_ipc_regs.h>
#include <adsp_memory.h>
#include <adsp_interrupt.h>
#include <zephyr/irq.h>
#include <zephyr/cache.h>
#include <ipi.h>

#define CORE_POWER_CHECK_NUM 128

#define CPU_POWERUP_TIMEOUT_USEC 10000

#ifdef CONFIG_XTENSA_MMU
#include <zephyr/arch/xtensa/xtensa_mmu.h>
#endif /* CONFIG_XTENSA_MMU */

#define ACE_INTC_IRQ DT_IRQN(DT_NODELABEL(ace_intc))

#ifdef CONFIG_XTENSA_MMU
#define IPI_TLB_FLUSH 0x01
#endif

static void ipc_isr(void *arg)
{
	uint32_t cpu_id = arch_proc_id();

#if defined(CONFIG_XTENSA_MMU) && (CONFIG_MP_MAX_NUM_CPUS > 1)
	uint32_t msg = IDC[cpu_id].agents[0].ipc.tdr & ~INTEL_ADSP_IPC_BUSY;

	if (msg == IPI_TLB_FLUSH) {
		xtensa_mmu_tlb_shootdown();
	}
#endif

	/*
	 * Clearing the BUSY bits in both TDR and TDA are needed to
	 * complete an IDC message. If we do only one (and not both),
	 * the other side will not be able to send another IDC
	 * message as the hardware still thinks you are processing
	 * the IDC message (and thus will not send another one).
	 * On TDR, it is to write one to clear, while on TDA, it is
	 * to write zero to clear.
	 */
	IDC[cpu_id].agents[0].ipc.tdr = INTEL_ADSP_IPC_BUSY;
	IDC[cpu_id].agents[0].ipc.tda = 0;

#ifdef CONFIG_SMP
	void z_sched_ipi(void);
	z_sched_ipi();
#endif
}

#define DFIDCCP			0x2020
#define CAP_INST_SHIFT		24
#define CAP_INST_MASK		BIT_MASK(4)

unsigned int soc_num_cpus;

__imr void soc_num_cpus_init(void)
{
	/* Need to set soc_num_cpus early to arch_num_cpus() works properly */
	soc_num_cpus = ((sys_read32(DFIDCCP) >> CAP_INST_SHIFT) & CAP_INST_MASK) + 1;
	soc_num_cpus = MIN(CONFIG_MP_MAX_NUM_CPUS, soc_num_cpus);

}

void soc_mp_init(void)
{
#if defined(CONFIG_INTEL_ADSP_SIM_NO_SECONDARY_CORE_FLOW)
	/* BADDR stores the Xtensa LX7 AltResetVec input */
	for (int i = 0; i < soc_num_cpus; i++) {
		DSPCS.bootctl[i].baddr = (uint32_t)z_soc_mp_asm_entry;
	}
#endif

	IRQ_CONNECT(ACE_IRQ_TO_ZEPHYR(ACE_INTL_IDCA), 0, ipc_isr, 0, 0);

	irq_enable(ACE_IRQ_TO_ZEPHYR(ACE_INTL_IDCA));

	unsigned int num_cpus = arch_num_cpus();

	for (int i = 0; i < num_cpus; i++) {
		/* DINT has one bit per IPC, unmask only IPC "Ax" on core "x" */
		ACE_DINT[i].ie[ACE_INTL_IDCA] = BIT(i);

		/* Agent A should signal only BUSY interrupts */
		IDC[i].agents[0].ipc.ctl = BIT(0); /* IPCTBIE */
	}

	/* Set the core 0 active */
	soc_cpus_active[0] = true;
}

static int host_runtime_get(void)
{
	return pm_device_runtime_get(INTEL_ADSP_HST_DOMAIN_DEV);
}
SYS_INIT(host_runtime_get, POST_KERNEL, 99);

#ifdef CONFIG_ADSP_IMR_CONTEXT_SAVE
/*
 * Called after exiting D3 state when context restore is enabled.
 * Re-enables IDC interrupt again for all cores. Called once from core 0.
 */
void soc_mp_on_d3_exit(void)
{
	soc_mp_init();
}
#endif

void soc_start_core(int cpu_num)
{
#if !defined(CONFIG_INTEL_ADSP_SIM_NO_SECONDARY_CORE_FLOW)
	int retry = CORE_POWER_CHECK_NUM;

	if (cpu_num > 0) {
		/* Initialize the ROM jump address */
		uint32_t *rom_jump_vector = (uint32_t *) ROM_JUMP_ADDR;
#if CONFIG_PM
		extern void dsp_restore_vector(void);

		/* We need to find out what type of booting is taking place here. Secondary cores
		 * can be disabled and enabled multiple times during runtime. During kernel
		 * initialization, the next pm state is set to ACTIVE. This way we can determine
		 * whether the core is being turned on again or for the first time.
		 */
		if (pm_state_next_get(cpu_num)->state == PM_STATE_ACTIVE) {
			*rom_jump_vector = (uint32_t) z_soc_mp_asm_entry;
		} else {
			*rom_jump_vector = (uint32_t) dsp_restore_vector;
		}

		/* The primary core cannot enter power gating if any of the secondary cores are
		 * active.
		 */
		pm_policy_state_lock_get(PM_STATE_RUNTIME_IDLE, PM_ALL_SUBSTATES);
#else
		*rom_jump_vector = (uint32_t) z_soc_mp_asm_entry;
#endif

		sys_cache_data_flush_range(rom_jump_vector, sizeof(*rom_jump_vector));
		soc_cpu_power_up(cpu_num);

		if (!WAIT_FOR(soc_cpu_is_powered(cpu_num),
			      CPU_POWERUP_TIMEOUT_USEC, k_busy_wait(HW_STATE_CHECK_DELAY))) {
			k_panic();
		}

		/* Tell the ACE ROM that it should use secondary core flow */
		DSPCS.bootctl[cpu_num].battr |= DSPBR_BATTR_LPSCTL_BATTR_SLAVE_CORE;
	}
#endif /* !defined(CONFIG_INTEL_ADSP_SIM_NO_SECONDARY_CORE_FLOW) */

	/* Setting the Power Active bit to the off state before powering up the core. This step is
	 * required by the HW if we are starting core for a second time. Without this sequence, the
	 * core will not power on properly when doing transition D0->D3->D0.
	 */
	DSPCS.capctl[cpu_num].ctl &= ~DSPCS_CTL_SPA;

	/* Checking current power status of the core. */
	if (!WAIT_FOR((DSPCS.capctl[cpu_num].ctl & DSPCS_CTL_CPA) != DSPCS_CTL_CPA,
		      CPU_POWERUP_TIMEOUT_USEC, k_busy_wait(HW_STATE_CHECK_DELAY))) {
		k_panic();
	}

	DSPCS.capctl[cpu_num].ctl |= DSPCS_CTL_SPA;

#if !defined(CONFIG_INTEL_ADSP_SIM_NO_SECONDARY_CORE_FLOW)
	/* Waiting for power up */
	while (((DSPCS.capctl[cpu_num].ctl & DSPCS_CTL_CPA) != DSPCS_CTL_CPA) &&
	       (retry > 0)) {
		k_busy_wait(HW_STATE_CHECK_DELAY);
		retry--;
	}

	if (retry == 0) {
		__ASSERT(false, "%s secondary core has not powered up", __func__);
	}
#endif /* !defined(CONFIG_INTEL_ADSP_SIM_NO_SECONDARY_CORE_FLOW) */
}

void soc_mp_startup(uint32_t cpu)
{
#ifdef CONFIG_XTENSA_MMU
	xtensa_mmu_init();
#endif /* CONFIG_XTENSA_MMU */

	/* Must have this enabled always */
	xtensa_irq_enable(ACE_INTC_IRQ);

#if CONFIG_ADSP_IDLE_CLOCK_GATING
	/* Disable idle power gating */
	DSPCS.bootctl[cpu].bctl |= DSPBR_BCTL_WAITIPPG;
#else
	/* Disable idle power and clock gating */
	DSPCS.bootctl[cpu].bctl |= DSPBR_BCTL_WAITIPCG | DSPBR_BCTL_WAITIPPG;
#endif /* CONFIG_ADSP_IDLE_CLOCK_GATING */
}

/**
 * @brief Send a IPI to other processors.
 *
 * @note: Leave the MSB clear when passing @param msg.
 *
 * @param msg Message to be sent (31-bit integer).
 */
#ifndef CONFIG_XTENSA_MMU
ALWAYS_INLINE
#endif
static void send_ipi(uint32_t msg, uint32_t cpu_bitmap)
{
	uint32_t curr = arch_proc_id();

	/* Signal agent B[n] to cause an interrupt from agent A[n] */
	unsigned int num_cpus = arch_num_cpus();

	for (int core = 0; core < num_cpus; core++) {
		if ((core != curr) && soc_cpus_active[core] &&
		    ((cpu_bitmap & BIT(core)) != 0)) {
			IDC[core].agents[1].ipc.idr = msg | INTEL_ADSP_IPC_BUSY;
		}
	}
}

#if defined(CONFIG_XTENSA_MMU) && (CONFIG_MP_MAX_NUM_CPUS > 1)
void xtensa_mmu_tlb_ipi(void)
{
	send_ipi(IPI_TLB_FLUSH, IPI_ALL_CPUS_MASK);
}
#endif

void arch_sched_broadcast_ipi(void)
{
	send_ipi(0, IPI_ALL_CPUS_MASK);
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	send_ipi(0, cpu_bitmap);
}

#if CONFIG_MP_MAX_NUM_CPUS > 1
int soc_adsp_halt_cpu(int id)
{
	int retry = CORE_POWER_CHECK_NUM;

	CHECKIF(arch_proc_id() != 0) {
		return -EINVAL;
	}

	CHECKIF(id <= 0 || id >= arch_num_cpus()) {
		return -EINVAL;
	}

	CHECKIF(soc_cpus_active[id]) {
		return -EINVAL;
	}

	DSPCS.capctl[id].ctl &= ~DSPCS_CTL_SPA;

	/* Waiting for power off */
	while (((DSPCS.capctl[id].ctl & DSPCS_CTL_CPA) == DSPCS_CTL_CPA) &&
	       (retry > 0)) {
		k_busy_wait(HW_STATE_CHECK_DELAY);
		retry--;
	}

	if (retry == 0) {
		__ASSERT(false, "%s secondary core has not powered down", __func__);
		return -EINVAL;
	}

	soc_cpu_power_down(id);
	return 0;
}
#endif
