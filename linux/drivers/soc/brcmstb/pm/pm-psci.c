// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom STB PSCI based system wide PM support
 *
 * Copyright © 2018 Broadcom
 */

#define pr_fmt(fmt) "brcmstb-pm-psci: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <linux/suspend.h>
#include <linux/brcmstb/brcmstb.h>
#include <linux/brcmstb/memory_api.h>
#include <soc/brcmstb/common.h>

#include <asm/psci.h>

#include <uapi/linux/psci.h>

#include <asm/suspend.h>

#include "pm-common.h"

#ifdef CONFIG_64BIT
#define PSCI_FN_NATIVE(version, name)   PSCI_##version##_FN64_##name
#else
#define PSCI_FN_NATIVE(version, name)   PSCI_##version##_FN_##name
#endif

/* Broadcom STB custom SIP function calls */
#define SIP_FUNC_INTEG_REGION_SET	\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   0)
#define SIP_FUNC_INTEG_REGION_DEL	\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   1)
#define SIP_FUNC_INTEG_REGION_RESET_ALL	\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   2)
#define SIP_FUNC_PANIC_NOTIFY		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   3)
#define SIP_FUNC_PSCI_FEATURES		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   4)

#define SIP_SVC_REVISION		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
			   IS_ENABLED(CONFIG_64BIT), \
			   ARM_SMCCC_OWNER_SIP, \
			   0xFF02)

#define SIP_MIN_REGION_SIZE	4096
#define SIP_REVISION_MAJOR	0
#define SIP_REVISION_MINOR	2

typedef unsigned long (psci_fn)(unsigned long, unsigned long,
				unsigned long, unsigned long);
static psci_fn *invoke_psci_fn;

static unsigned long __invoke_psci_fn_hvc(unsigned long function_id,
					  unsigned long arg0,
					  unsigned long arg1,
					  unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);

	return res.a0;
}

static unsigned long __invoke_psci_fn_smc(unsigned long function_id,
					  unsigned long arg0,
					  unsigned long arg1,
					  unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);

	return res.a0;
}

static int brcmstb_psci_integ_region(unsigned long function_id,
				     unsigned long base,
				     unsigned long size)
{
	unsigned long end;

	if (!size)
		return -EINVAL;

	end = DIV_ROUND_UP(base + size, SIP_MIN_REGION_SIZE);
	base /= SIP_MIN_REGION_SIZE;
	size = end - base;

	return invoke_psci_fn(function_id, base, size, 0);
}

static int brcmstb_psci_integ_region_set(unsigned long base,
					 unsigned long size)
{
	return brcmstb_psci_integ_region(SIP_FUNC_INTEG_REGION_SET, base, size);
}

static int brcmstb_psci_integ_region_del(unsigned long base,
					 unsigned long size)
{
	return brcmstb_psci_integ_region(SIP_FUNC_INTEG_REGION_DEL, base, size);
}

static int brcmstb_psci_integ_region_reset_all(void)
{
	return invoke_psci_fn(SIP_FUNC_INTEG_REGION_RESET_ALL, 0, 0, 0);
}

static int psci_system_suspend(unsigned long unused)
{
	return invoke_psci_fn(PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND),
			      virt_to_phys(cpu_resume), 0, 0);
}

int brcmstb_psci_system_mem_finish(void)
{
	struct dma_region combined_regions[MAX_EXCLUDE + MAX_REGION + MAX_EXTRA];
	const int max = ARRAY_SIZE(combined_regions);
	unsigned int i;
	int nregs, ret;

	memset(&combined_regions, 0, sizeof(combined_regions));
	nregs = configure_main_hash(combined_regions, max,
				    exclusions, num_exclusions);
	if (nregs < 0)
		return nregs;

	for (i = 0; i < num_regions && nregs + i < max; i++)
		combined_regions[nregs + i] = regions[i];
	nregs += i;

	for (i = 0; i < nregs; i++) {
		ret = brcmstb_psci_integ_region_set(combined_regions[i].addr,
						    combined_regions[i].len);
		if (ret != PSCI_RET_SUCCESS) {
			pr_err("Error setting combined region %d\n", i);
			continue;
		}
	}

	for (i = 0; i < num_exclusions; i++) {
		ret = brcmstb_psci_integ_region_del(exclusions[i].addr,
						    exclusions[i].len);
		if (ret != PSCI_RET_SUCCESS) {
			pr_err("Error removing exclusion region %d\n", i);
			continue;
		}
	}

	return cpu_suspend(0, psci_system_suspend);
}

void brcmstb_psci_sys_poweroff(void)
{
	invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);
}

static int psci_features(u32 psci_func_id)
{
	u32 features_func_id;

	switch (ARM_SMCCC_OWNER_NUM(psci_func_id)) {
	case ARM_SMCCC_OWNER_SIP:
		features_func_id = SIP_FUNC_PSCI_FEATURES;
		break;
	case ARM_SMCCC_OWNER_STANDARD:
		features_func_id = PSCI_1_0_FN_PSCI_FEATURES;
		break;
	default:
		return PSCI_RET_NOT_SUPPORTED;
	}

	return invoke_psci_fn(features_func_id, psci_func_id, 0, 0);
}

static int brcmstb_psci_enter(suspend_state_t state)
{
	/* Request a SYSTEM level power state with retention */
	struct psci_power_state pstate = {
		.type = 0,
		.affinity_level = 2,
	};
	int ret = -EINVAL;

	switch (state) {
	case PM_SUSPEND_STANDBY:
		ret = psci_ops.cpu_suspend(pstate, 0);
		break;
	case PM_SUSPEND_MEM:
		ret = brcmstb_psci_system_mem_finish();
		break;
	}

	return ret;
}

static int brcmstb_psci_valid(suspend_state_t state)
{
	switch (state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		return true;
	default:
		return false;
	}
}

static const struct platform_suspend_ops brcmstb_psci_ops = {
	.enter	= brcmstb_psci_enter,
	.valid	= brcmstb_psci_valid,
};

static int brcmstb_psci_panic_notify(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	int ret;

	ret = invoke_psci_fn(SIP_FUNC_PANIC_NOTIFY, BRCMSTB_PANIC_MAGIC, 0, 0);
	if (ret != PSCI_RET_SUCCESS)
		return NOTIFY_BAD;

	return NOTIFY_DONE;
}

static struct notifier_block brcmstb_psci_nb = {
	.notifier_call = brcmstb_psci_panic_notify,
};

int brcmstb_pm_psci_init(void)
{
	unsigned long funcs_id[] = {
		PSCI_0_2_FN_SYSTEM_OFF,
		SIP_FUNC_INTEG_REGION_SET,
		SIP_FUNC_INTEG_REGION_DEL,
		SIP_FUNC_INTEG_REGION_RESET_ALL,
	};
	struct arm_smccc_res res = { };
	unsigned int i;
	int ret;

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		invoke_psci_fn = __invoke_psci_fn_hvc;
		break;
	case PSCI_CONDUIT_SMC:
		invoke_psci_fn = __invoke_psci_fn_smc;
		break;
	default:
		return -EINVAL;
	}

	/* Check the revision of the monitor firmware */
	if (invoke_psci_fn == __invoke_psci_fn_hvc)
		arm_smccc_hvc(SIP_SVC_REVISION,
			      0, 0, 0, 0, 0, 0, 0, &res);
	else
		arm_smccc_smc(SIP_SVC_REVISION,
			      0, 0, 0, 0, 0, 0, 0, &res);

	/* Test for our supported features */
	for (i = 0; i < ARRAY_SIZE(funcs_id); i++) {
		ret = psci_features(funcs_id[i]);
		if (ret == PSCI_RET_NOT_SUPPORTED) {
			pr_err("Firmware does not support function 0x%lx\n",
			       funcs_id[i]);
			return -EOPNOTSUPP;
		}
	}

	ret = brcmstb_psci_integ_region_reset_all();
	if (ret != PSCI_RET_SUCCESS) {
		pr_err("Error resetting all integrity checking regions\n");
		return -EIO;
	}

	/* Firmware is new enough to participate in S3/S5, but does not
	 * take over all suspend operations and still needs assistance
	 * from pm-arm.c.
	 */
	if (res.a0 == SIP_REVISION_MAJOR && res.a1 < SIP_REVISION_MINOR) {
		brcmstb_pm_method = BRCMSTB_PM_PSCI_ASSISTED;
		pr_info("Using PSCI based system PM (assisted)\n");
		return 0;
	}

	/* Firmware is fully taking over the S2/S3/S5 states and requires
	 * only PSCI calls to enter those states
	 */
	ret = brcmstb_memory_get(&bm);
	if (ret)
		return ret;

	ret = brcmstb_regsave_init();
	if (ret)
		return ret;

	brcmstb_pm_method = BRCMSTB_PM_PSCI_FULL;
	pm_power_off = brcmstb_psci_sys_poweroff;
	suspend_set_ops(&brcmstb_psci_ops);
	atomic_notifier_chain_register(&panic_notifier_list,
				       &brcmstb_psci_nb);

	pr_info("Using PSCI based system PM (full featured)\n");

	return 0;
}
module_init(brcmstb_pm_psci_init);
