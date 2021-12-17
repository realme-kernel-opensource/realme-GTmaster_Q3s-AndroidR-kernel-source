// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/uaccess.h>
#include <linux/soc/qcom/smem.h>
#include <asm/arch_timer.h>
#include "rpmh_master_stat.h"

#define UNIT_DIST 0x14
#define REG_VALID 0x0
#define REG_DATA_LO 0x4
#define REG_DATA_HI 0x8

#define GET_ADDR(REG, UNIT_NO) (REG + (UNIT_DIST * UNIT_NO))

enum master_smem_id {
	MPSS = 605,
	ADSP,
	CDSP,
	SLPI,
	GPU,
	DISPLAY,
	SLPI_ISLAND = 613,
	APSS = 631,
};

enum master_pid {
	PID_APSS = 0,
	PID_MPSS = 1,
	PID_ADSP = 2,
	PID_SLPI = 3,
	PID_CDSP = 5,
	PID_WPSS = 13,
	PID_GPU = PID_APSS,
	PID_DISPLAY = PID_APSS,
};

enum profile_data {
	POWER_DOWN_START,
	POWER_UP_END,
	POWER_DOWN_END,
	POWER_UP_START,
	NUM_UNIT,
};

struct msm_rpmh_master_data {
	char *master_name;
	enum master_smem_id smem_id;
	enum master_pid pid;
};

static const struct msm_rpmh_master_data rpmh_masters[] = {
	{"APSS", APSS, QCOM_SMEM_HOST_ANY},
	{"MPSS", MPSS, PID_MPSS},
	{"WPSS", MPSS, PID_WPSS},
	{"ADSP", ADSP, PID_ADSP},
	{"ADSP_ISLAND", SLPI_ISLAND, PID_ADSP},
	{"CDSP", CDSP, PID_CDSP},
	{"SLPI", SLPI, PID_SLPI},
	{"SLPI_ISLAND", SLPI_ISLAND, PID_SLPI},
	{"GPU", GPU, PID_GPU},
	{"DISPLAY", DISPLAY, PID_DISPLAY},
};

struct msm_rpmh_master_stats {
	uint32_t version_id;
	uint32_t counts;
	uint64_t last_entered;
	uint64_t last_exited;
	uint64_t accumulated_duration;
};

struct msm_rpmh_profile_unit {
	uint64_t value;
	uint64_t valid;
};

struct rpmh_master_stats_prv_data {
	struct kobj_attribute ka;
	#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
	//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
	struct kobj_attribute oplus_ka;
	#endif
	struct kobject *kobj;
};

static struct msm_rpmh_master_stats apss_master_stats;
static void __iomem *rpmh_unit_base;

static DEFINE_MUTEX(rpmh_stats_mutex);

#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
#define MSM_ARCH_TIMER_FREQ 19200000
static inline u64 get_time_in_msec(u64 counter)
{
	do_div(counter, (MSM_ARCH_TIMER_FREQ/MSEC_PER_SEC));
	return counter;
}
#endif

static ssize_t msm_rpmh_master_stats_print_data(char *prvbuf, ssize_t length,
				struct msm_rpmh_master_stats *record,
				const char *name)
{
	uint64_t accumulated_duration = record->accumulated_duration;
	/*
	 * If a master is in sleep when reading the sleep stats from SMEM
	 * adjust the accumulated sleep duration to show actual sleep time.
	 * This ensures that the displayed stats are real when used for
	 * the purpose of computing battery utilization.
	 */
	if (record->last_entered > record->last_exited)
		accumulated_duration +=
				(__arch_counter_get_cntvct()
				- record->last_entered);
#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
	return snprintf(prvbuf, length, "%s\n\tVersion:0x%x\n"
			"\tSleep Count:0x%x\n"
			"\tSleep Last Entered At:0x%llx\n"
			"\tSleep Last Exited At:0x%llx\n"
			"\tSleep Accumulated Duration:0x%llx\n"
			"\tSleep Accumulated Duration(mS):0x%llx\n"
			"\tSleep Accumulated Duration(mS):%llu\n\n",
			name, record->version_id, record->counts,
			record->last_entered, record->last_exited,
			accumulated_duration, get_time_in_msec(accumulated_duration), get_time_in_msec(accumulated_duration));
#else
	return scnprintf(prvbuf, length, "%s\n\tVersion:0x%x\n"
			"\tSleep Count:0x%x\n"
			"\tSleep Last Entered At:0x%llx\n"
			"\tSleep Last Exited At:0x%llx\n"
			"\tSleep Accumulated Duration:0x%llx\n\n",
			name, record->version_id, record->counts,
			record->last_entered, record->last_exited,
			accumulated_duration);
#endif
}

static ssize_t msm_rpmh_master_stats_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	ssize_t length = 0;
	int i = 0;
	struct msm_rpmh_master_stats *record = NULL;
	bool skip_apss = false;

	mutex_lock(&rpmh_stats_mutex);

	/* First Read APSS master stats */

	if (rpmh_unit_base) {
		length = msm_rpmh_master_stats_print_data(buf, PAGE_SIZE,
							  &apss_master_stats,
							  "APSS");
		skip_apss = true;
	}
	/* Read SMEM data written by other masters */

	for (i = 0; i < ARRAY_SIZE(rpmh_masters); i++) {
		if (skip_apss && i == 0)
			continue;

		record = (struct msm_rpmh_master_stats *) qcom_smem_get(
					rpmh_masters[i].pid,
					rpmh_masters[i].smem_id, NULL);
		if (!IS_ERR_OR_NULL(record) && (PAGE_SIZE - length > 0))
			length += msm_rpmh_master_stats_print_data(
					buf + length, PAGE_SIZE - length,
					record,
					rpmh_masters[i].master_name);
	}

	mutex_unlock(&rpmh_stats_mutex);

	return length;
}

#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
int rpmh_modem_sleepinfo_buffer_clear(void)
{
	pr_info("%s: wr_offset restart\n", __func__);
	return 0;
}
EXPORT_SYMBOL(rpmh_modem_sleepinfo_buffer_clear);

static ssize_t oplus_msm_rpmh_master_stats_print_data(char *prvbuf, ssize_t length,
				struct msm_rpmh_master_stats *record,
				const char *name)
{
	uint64_t accumulated_duration = record->accumulated_duration;
	/*
	 * If a master is in sleep when reading the sleep stats from SMEM
	 * adjust the accumulated sleep duration to show actual sleep time.
	 * This ensures that the displayed stats are real when used for
	 * the purpose of computing battery utilization.
	 */
	if (record->last_entered > record->last_exited)
		accumulated_duration +=
				(__arch_counter_get_cntvct()
				- record->last_entered);

	return scnprintf(prvbuf, length, "%s:%x:%llx\n",
			name,record->counts,
			get_time_in_msec(accumulated_duration));
}

static ssize_t oplus_msm_rpmh_master_stats_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	ssize_t length;
	int i = 0;
	struct msm_rpmh_master_stats *record = NULL;

	mutex_lock(&rpmh_stats_mutex);

	/* First Read APSS master stats */

	length = oplus_msm_rpmh_master_stats_print_data(buf, PAGE_SIZE,
						&apss_master_stats, "APSS");

	/* Read SMEM data written by other masters */

	for (i = 0; i < ARRAY_SIZE(rpmh_masters); i++) {
		record = (struct msm_rpmh_master_stats *) qcom_smem_get(
					rpmh_masters[i].pid,
					rpmh_masters[i].smem_id, NULL);
		if (!IS_ERR_OR_NULL(record) && (PAGE_SIZE - length > 0))
			length += oplus_msm_rpmh_master_stats_print_data(
					buf + length, PAGE_SIZE - length,
					record,
					rpmh_masters[i].master_name);
	}

	mutex_unlock(&rpmh_stats_mutex);

	return length;
}
#endif


static inline void msm_rpmh_apss_master_stats_update(
				struct msm_rpmh_profile_unit *profile_unit)
{
	apss_master_stats.counts++;
	apss_master_stats.last_entered = profile_unit[POWER_DOWN_END].value;
	apss_master_stats.last_exited = profile_unit[POWER_UP_START].value;
	apss_master_stats.accumulated_duration +=
					(apss_master_stats.last_exited
					- apss_master_stats.last_entered);
}

void msm_rpmh_master_stats_update(void)
{
	int i;
	struct msm_rpmh_profile_unit profile_unit[NUM_UNIT];

	if (!rpmh_unit_base)
		return;

	for (i = POWER_DOWN_END; i < NUM_UNIT; i++) {
		profile_unit[i].valid = readl_relaxed(rpmh_unit_base +
						GET_ADDR(REG_VALID, i));

		/*
		 * Do not update APSS stats if valid bit is not set.
		 * It means APSS did not execute cx-off sequence.
		 * This can be due to fall through at some point.
		 */

		if (!(profile_unit[i].valid & BIT(REG_VALID)))
			return;

		profile_unit[i].value = readl_relaxed(rpmh_unit_base +
						GET_ADDR(REG_DATA_LO, i));
		profile_unit[i].value |= ((uint64_t)
					readl_relaxed(rpmh_unit_base +
					GET_ADDR(REG_DATA_HI, i)) << 32);
	}
	msm_rpmh_apss_master_stats_update(profile_unit);
}
EXPORT_SYMBOL(msm_rpmh_master_stats_update);

static int msm_rpmh_master_stats_probe(struct platform_device *pdev)
{
	struct rpmh_master_stats_prv_data *prvdata = NULL;
	struct kobject *rpmh_master_stats_kobj = NULL;
	int ret = -ENOMEM;

	prvdata = devm_kzalloc(&pdev->dev, sizeof(*prvdata), GFP_KERNEL);
	if (!prvdata)
		return ret;

	rpmh_master_stats_kobj = kobject_create_and_add(
					"rpmh_stats",
					power_kobj);
	if (!rpmh_master_stats_kobj)
		return ret;

	prvdata->kobj = rpmh_master_stats_kobj;

	sysfs_attr_init(&prvdata->ka.attr);
	prvdata->ka.attr.mode = 0444;
	prvdata->ka.attr.name = "master_stats";
	prvdata->ka.show = msm_rpmh_master_stats_show;
	prvdata->ka.store = NULL;

	ret = sysfs_create_file(prvdata->kobj, &prvdata->ka.attr);
	if (ret) {
		pr_err("sysfs_create_file failed\n");
		goto fail_sysfs;
	}

	#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
	//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
	sysfs_attr_init(&prvdata->oplus_ka.attr);
	prvdata->oplus_ka.attr.mode = 0444;
	prvdata->oplus_ka.attr.name = "oplus_rpmh_master_stats";
	prvdata->oplus_ka.show = oplus_msm_rpmh_master_stats_show;
	prvdata->oplus_ka.store = NULL;

	ret = sysfs_create_file(prvdata->kobj, &prvdata->oplus_ka.attr);
	if (ret) {
		pr_err("sysfs_create_file failed\n");
		goto fail_sysfs;
	}
	#endif

	rpmh_unit_base = of_iomap(pdev->dev.of_node, 0);
	if (!rpmh_unit_base) {
		pr_err("Failed to get rpmh_unit_base\n");
		ret = -ENOMEM;
		goto fail_iomap;
	}

	apss_master_stats.version_id = 0x1;
	platform_set_drvdata(pdev, prvdata);
	return ret;

fail_iomap:
	sysfs_remove_file(prvdata->kobj, &prvdata->ka.attr);
	#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
	//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
	sysfs_remove_file(prvdata->kobj, &prvdata->oplus_ka.attr);
	#endif
fail_sysfs:
	kobject_put(prvdata->kobj);
	return ret;
}

static int msm_rpmh_master_stats_remove(struct platform_device *pdev)
{
	struct rpmh_master_stats_prv_data *prvdata;

	prvdata = (struct rpmh_master_stats_prv_data *)
				platform_get_drvdata(pdev);

	sysfs_remove_file(prvdata->kobj, &prvdata->ka.attr);
	#if defined(OPLUS_FEATURE_POWERINFO_RPMH) && defined(CONFIG_OPLUS_POWERINFO_RPMH)
	//Yunqing.Zeng@BSP.Power.Basic, 2020/09/08, add for feature rpmh info.
	sysfs_remove_file(prvdata->kobj, &prvdata->oplus_ka.attr);
	#endif
	kobject_put(prvdata->kobj);
	platform_set_drvdata(pdev, NULL);
	if (rpmh_unit_base)
		iounmap(rpmh_unit_base);
	rpmh_unit_base = NULL;

	return 0;
}

static const struct of_device_id rpmh_master_table[] = {
	{.compatible = "qcom,rpmh-master-stats-v1"},
	{},
};

static struct platform_driver msm_rpmh_master_stats_driver = {
	.probe	= msm_rpmh_master_stats_probe,
	.remove = msm_rpmh_master_stats_remove,
	.driver = {
		.name = "msm_rpmh_master_stats",
		.of_match_table = rpmh_master_table,
	},
};

module_platform_driver(msm_rpmh_master_stats_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM RPMH Master Statistics driver");
MODULE_ALIAS("platform:msm_rpmh_master_stat_log");
