// SPDX-License-Identifier: GPL-2.0-only
/* MT6833 boot-loaded firmware coordination and guarded suspend bring-up. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/psci.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6833-lpm.h>
#include <linux/suspend.h>

/* Samsung MT6833's mtk_lpm_module.h and MCUPM mailbox 3 ABI. */
#define MT6833_LPM_SMC		0xc2000507
#define LPM_USER_CPU_PM		0xda000000
#define LPM_USER_SPM		0xda020000
#define LPM_USER_CPU_PM_LP	0xda030000
#define LPM_USER_SPM_DEBUG	0xda010000
#define LPM_ACTION_SET		BIT(0)
#define LPM_ACTION_CLEAR	BIT(1)
#define LPM_ACTION_GET		BIT(2)
#define LPM_ACTION_PUSH		BIT(3)
#define LPM_ACTION_SUBMIT	BIT(5)
#define LPM_MCUSYS_STATUS	0
#define LPM_SPM_STATUS		0
#define LPM_SPM_FW_TYPE		3

#define MCUPM_AP_READY		0
#define MCUPM_PWR_CTRL		4
#define MCUPM_L3_MODE		5
#define MCUPM_BUCK_MODE		6
#define MCUPM_PLL_MODE		7
#define MCUPM_TASK_STATE	8
#define MCUPM_WAKE_CPU		12
#define MCUPM_MBOX_SIZE		0xa0
#define MCUPM_PLATFORM_ACK	0x50
#define MCUPM_PLATFORM_BIT	BIT(0)
#define MCUPM_PLATFORM_INIT	0x504c5401
#define MCUPM_LOGGER_INIT	0x504c5402
#define MCUPM_PLATFORM_MEM_SIZE	0x100100
#define MCUPM_EEM_MEM_OFFSET	0x110000
/* MCUPM firmware and GPL struct eemsn_log agree on size and flag offsets. */
#define MCUPM_EEM_LOG_SIZE	0x568
#define MCUPM_EEM_ENABLE_OFFSET	0x4fe
#define MCUPM_SN_ENABLE_OFFSET	0x4ff

static const char * const boot_peripheral_clock_names[] = {
	"boot-uart1", "boot-spi0", "boot-spi3", "boot-spi4",
	"boot-spi5", "boot-spi6", "boot-spi7",
	"boot-mdp-rdma0", "boot-mdp-rdma1", "boot-mdp-smi0",
	"boot-mdp-wrot0", "boot-mdp-wrot1", "boot-mdp-fake0",
	"boot-mmpll",
};

static const char * const boot_ufs_clock_names[] = {
	"boot-ufs", "boot-unipro", "boot-ufs-bclk", "boot-ufs-aes",
};

static const char * const boot_ufs_source_names[] = {
	"boot-ufs-source", "boot-ufs-aes-source",
};

struct mt6833_lpm {
	struct device *dev;
	void __iomem *mbox;
	void __iomem *platform_mbox;
	void __iomem *eem_mbox;
	void __iomem *tx_set;
	void __iomem *tx_status;
	void __iomem *rx_clear;
	void __iomem *rx_status;
	void __iomem *shared;
	void __iomem *eem_shared;
	phys_addr_t shared_capacity;
	struct resource shared_res;
	struct mutex lock;
	struct pm_qos_request latency;
	struct clk_bulk_data boot_clocks[ARRAY_SIZE(boot_peripheral_clock_names)];
	bool boot_clocks_held;
	bool boot_peripherals_gated;
	struct clk_bulk_data ufs_clocks[ARRAY_SIZE(boot_ufs_clock_names)];
	struct clk_bulk_data ufs_sources[ARRAY_SIZE(boot_ufs_source_names)];
	bool ufs_unused;
	bool ufs_sources_held;
	bool ufs_clocks_held;
	bool boot_ufs_gated;
	bool idle_blocked;
	bool platform_initialized;
	bool ready;
	bool wake_irqs_ready;
	bool resources_ready;
	bool eem_fixed_mode;
	bool deep_enabled;
	bool stage_only;
	bool s2idle_enabled;
	bool s2idle_active;
	atomic_t *s2idle_attempts;
	atomic_t *s2idle_resumes;
	atomic_t *s2idle_errors;
	unsigned int suspend_entries;
	int suspend_result;
	u32 wake_irq_count;
	u32 power_policy;
	u32 last_ack;
	unsigned long mcusys_status;
	unsigned long spm_status;
	unsigned long fw_type;
};

static unsigned long mt6833_lpm_call(u32 user, u32 id, u32 act, u32 arg, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MT6833_LPM_SMC, user | id, act, arg, val, 0, 0, 0, &res);
	/* Some firmware returns a 32-bit SIP error without sign extension. */
	if (res.a0 >= 0xfffffffcUL && res.a0 <= 0xffffffffUL)
		res.a0 = (long)(s32)res.a0;
	if ((long)res.a0 < 0)
		pr_warn("mt6833-lpm: SMC uid=0x%x args=(0x%x,0x%x,0x%x) error=%ld\n",
			user | id, act, arg, val, (long)res.a0);
	return res.a0;
}

static unsigned long mt6833_lpm_get(u32 user, u32 id)
{
	return mt6833_lpm_call(user, id, LPM_ACTION_GET, 0, 0);
}

static u32 mt6833_mbox_read(struct mt6833_lpm *lpm, unsigned int slot)
{
	return readl(lpm->mbox + slot * sizeof(u32));
}

static bool mt6833_lpm_prepared(struct mt6833_lpm *lpm)
{
	return lpm->platform_initialized && lpm->ready && lpm->wake_irqs_ready &&
		lpm->resources_ready && lpm->eem_fixed_mode &&
		mt6833_mbox_read(lpm, MCUPM_TASK_STATE) == 3;
}

/* CPU_SUSPEND is intentionally not exported for modules. The real backend
 * is built in and its device cannot be unbound, so the PM ops stay valid.
 * External discovery modules still support transport bring-up without ops.
 */
#ifndef MODULE
static struct mt6833_lpm *mt6833_suspend_lpm;

bool mt6833_lpm_s2idle_active(void)
{
	struct mt6833_lpm *lpm = READ_ONCE(mt6833_suspend_lpm);

	return lpm && READ_ONCE(lpm->s2idle_active);
}

void mt6833_lpm_s2idle_enter(u32 state)
{
	struct mt6833_lpm *lpm = READ_ONCE(mt6833_suspend_lpm);

	if (lpm && state == MT6833_LPM_SUSPEND_STATE)
		atomic_inc(&lpm->s2idle_attempts[smp_processor_id()]);
}

void mt6833_lpm_s2idle_exit(u32 state, int result)
{
	struct mt6833_lpm *lpm = READ_ONCE(mt6833_suspend_lpm);
	unsigned int cpu = smp_processor_id();

	if (!lpm || state != MT6833_LPM_SUSPEND_STATE)
		return;
	if (result)
		atomic_inc(&lpm->s2idle_errors[cpu]);
	else
		atomic_inc(&lpm->s2idle_resumes[cpu]);
}

static int mt6833_suspend_begin(suspend_state_t state)
{
	struct mt6833_lpm *lpm = mt6833_suspend_lpm;

	mutex_lock(&lpm->lock);
	/* PM also falls back to these begin/end callbacks for s2idle. Its
	 * ordinary path needs no MCUPM initialization or deep opt-in.
	 */
	if (state == PM_SUSPEND_TO_IDLE)
		return 0;
	/* PM invokes end even when begin rejects the request. */
	if (!lpm->stage_only && (!lpm->deep_enabled || !mt6833_lpm_prepared(lpm)))
		return -EAGAIN;
	/* Hold the lock until end: sysfs cannot change firmware policy while
	 * devices/secondary CPUs are being suspended or restored.
	 */
	pm_set_suspend_via_firmware();
	return 0;
}

static int mt6833_suspend_enter(suspend_state_t state)
{
	struct mt6833_lpm *lpm = mt6833_suspend_lpm;
	int ret;

	/* Permit isolated generic PM diagnostics before firmware initialization.
	 * Even a mistaken non-test request cannot enter CPU_SUSPEND in this mode.
	 */
	if (lpm->stage_only)
		return -EOPNOTSUPP;
	if (!mt6833_lpm_prepared(lpm))
		return -EAGAIN;
	writel(smp_processor_id(), lpm->mbox + MCUPM_WAKE_CPU * sizeof(u32));
	/* Publish the selected wake CPU before firmware takes over. */
	wmb();
	/* Generic PM has already suspended syscore and parked secondary CPUs.
	 * CPU_PM supplies the GIC CPU-interface and architectural notifiers.
	 */
	ret = cpu_pm_enter();
	if (ret)
		return ret;
	lpm->suspend_entries++;
	pr_info("mt6833-lpm: entering vendor CPU_SUSPEND 0x1010100\n");
	pm_set_resume_via_firmware();
	ret = psci_cpu_suspend_enter(0x1010100);
	cpu_pm_exit();
	lpm->suspend_result = ret;
	if (ret)
		lpm->deep_enabled = false;
	pr_info("mt6833-lpm: vendor CPU_SUSPEND returned %d, task=%u\n",
		ret, mt6833_mbox_read(lpm, MCUPM_TASK_STATE));
	return ret;
}

static void mt6833_suspend_end(void)
{
	mutex_unlock(&mt6833_suspend_lpm->lock);
}

static const struct platform_suspend_ops mt6833_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.begin = mt6833_suspend_begin,
	.enter = mt6833_suspend_enter,
	.end = mt6833_suspend_end,
};

static int mt6833_s2idle_begin(void)
{
	return mt6833_suspend_begin(PM_SUSPEND_TO_IDLE);
}

static int mt6833_s2idle_prepare_late(void)
{
	struct mt6833_lpm *lpm = mt6833_suspend_lpm;

	if (!lpm->s2idle_enabled)
		return 0;
	if (!mt6833_lpm_prepared(lpm) || !lpm->idle_blocked)
		return -EAGAIN;
	writel(0, lpm->mbox + MCUPM_WAKE_CPU * sizeof(u32));
	/* Select the preferred wake CPU before permitting vendor idle entry. */
	wmb();
	WRITE_ONCE(lpm->s2idle_active, true);
	/* Devices are suspended and sysfs is frozen. Let the existing deepest
	 * cpuidle state be selected, with tick_freeze handling timekeeping once.
	 */
	cpu_latency_qos_update_request(&lpm->latency, PM_QOS_DEFAULT_VALUE);
	dev_info(lpm->dev, "vendor s2idle armed; CPUs stay online\n");
	return 0;
}

static void mt6833_s2idle_restore_early(void)
{
	struct mt6833_lpm *lpm = mt6833_suspend_lpm;

	if (!lpm->s2idle_active)
		return;
	WRITE_ONCE(lpm->s2idle_active, false);
	/* Restore the runtime WFI clamp before devices and userspace resume. */
	cpu_latency_qos_update_request(&lpm->latency, 0);
}

static const struct platform_s2idle_ops mt6833_s2idle_ops = {
	.begin = mt6833_s2idle_begin,
	.prepare_late = mt6833_s2idle_prepare_late,
	.restore_early = mt6833_s2idle_restore_early,
	.end = mt6833_suspend_end,
};
#endif

static ssize_t s2idle_enabled_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "%u\n", lpm->s2idle_enabled);
}

static ssize_t s2idle_enabled_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
#ifdef MODULE
	if (enable)
		return -EOPNOTSUPP;
#endif
	guard(mutex)(&lpm->lock);
	if (enable && (!mt6833_lpm_prepared(lpm) || lpm->stage_only ||
		       lpm->deep_enabled))
		return -EAGAIN;
	lpm->s2idle_enabled = enable;
	return count;
}
static DEVICE_ATTR_RW(s2idle_enabled);

static ssize_t s2idle_counters_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	unsigned int cpu;
	int len = 0;

	for_each_possible_cpu(cpu) {
		if (len > PAGE_SIZE - 80)
			break;
		len += sysfs_emit_at(buf, len, "cpu%u attempts=%d resumes=%d errors=%d\n",
			cpu, atomic_read(&lpm->s2idle_attempts[cpu]),
			atomic_read(&lpm->s2idle_resumes[cpu]),
			atomic_read(&lpm->s2idle_errors[cpu]));
	}
	return len;
}
static DEVICE_ATTR_ADMIN_RO(s2idle_counters);

static ssize_t deep_enabled_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "%u\n", lpm->deep_enabled);
}

static ssize_t deep_enabled_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
#ifdef MODULE
	if (enable)
		return -EOPNOTSUPP;
#endif
	guard(mutex)(&lpm->lock);
	if (enable && (lpm->stage_only || lpm->s2idle_enabled))
		return -EBUSY;
	if (enable && !mt6833_lpm_prepared(lpm))
		return -EAGAIN;
	lpm->deep_enabled = enable;
	return count;
}
static DEVICE_ATTR_RW(deep_enabled);

static ssize_t stage_only_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "%u\n", lpm->stage_only);
}

static ssize_t stage_only_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
#ifdef MODULE
	if (enable)
		return -EOPNOTSUPP;
#endif
	guard(mutex)(&lpm->lock);
	if (lpm->deep_enabled || lpm->s2idle_enabled)
		return -EBUSY;
	lpm->stage_only = enable;
	return count;
}
static DEVICE_ATTR_RW(stage_only);

static ssize_t counters_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	unsigned int id;
	int len = 0;

	guard(mutex)(&lpm->lock);
	for (id = 0; id < 3; id++)
		len += sysfs_emit_at(buf, len, "rc%u_count=%ld\n", id,
			(long)mt6833_lpm_call(LPM_USER_SPM_DEBUG, 6,
					     LPM_ACTION_GET, id, 0));
	len += sysfs_emit_at(buf, len, "suspend_entries=%u\nsuspend_result=%d\n",
			    lpm->suspend_entries, lpm->suspend_result);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(counters);

static ssize_t boot_peripherals_gated_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "%u\n", lpm->boot_peripherals_gated);
}

static ssize_t boot_peripherals_gated_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool gate;
	int ret;

	ret = kstrtobool(buf, &gate);
	if (ret)
		return ret;
	guard(mutex)(&lpm->lock);
	if (!lpm->idle_blocked || lpm->deep_enabled || lpm->stage_only ||
	    lpm->s2idle_enabled)
		return -EBUSY;
	if (gate == lpm->boot_peripherals_gated)
		return count;
	/* Only explicitly described board-unused peripheral clocks are managed. Taking
	 * and releasing a CCF reference removes inherited hardware enables;
	 * any real consumer's references continue to protect its clock.
	 * Do not globally clean up boot clocks until all dependencies are known.
	 */
	if (!lpm->boot_clocks_held) {
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(lpm->boot_clocks),
					      lpm->boot_clocks);
		if (ret)
			return ret;
		lpm->boot_clocks_held = true;
	}
	if (gate) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(lpm->boot_clocks),
					   lpm->boot_clocks);
		lpm->boot_clocks_held = false;
	}
	lpm->boot_peripherals_gated = gate;
	dev_info(dev, "board-unused peripheral clock cleanup %s\n",
		 gate ? "enabled" : "reverted");
	return count;
}
static DEVICE_ATTR_RW(boot_peripherals_gated);

static ssize_t boot_ufs_gated_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "%u\n", lpm->boot_ufs_gated);
}

static ssize_t boot_ufs_gated_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool gate;
	int ret;

	ret = kstrtobool(buf, &gate);
	if (ret)
		return ret;
	guard(mutex)(&lpm->lock);
	if (!lpm->ufs_unused)
		return -ENODEV;
	if (!lpm->idle_blocked || lpm->deep_enabled || lpm->stage_only ||
	    lpm->s2idle_enabled)
		return -EBUSY;
	if (gate == lpm->boot_ufs_gated)
		return count;
	/* Hold both source muxes throughout this scoped gate experiment. This
	 * prevents releasing inherited parent PLL enables shared with eMMC and
	 * other peripherals. Only the four board-unused UFS leaf gates change.
	 * The source references remain owned until a clean reboot.
	 */
	if (!lpm->ufs_sources_held) {
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(lpm->ufs_sources),
					      lpm->ufs_sources);
		if (ret)
			return ret;
		lpm->ufs_sources_held = true;
	}
	if (!lpm->ufs_clocks_held) {
		ret = clk_bulk_prepare_enable(ARRAY_SIZE(lpm->ufs_clocks),
					      lpm->ufs_clocks);
		if (ret)
			return ret;
		lpm->ufs_clocks_held = true;
	}
	if (gate) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(lpm->ufs_clocks),
					   lpm->ufs_clocks);
		lpm->ufs_clocks_held = false;
	}
	lpm->boot_ufs_gated = gate;
	dev_info(dev, "board-unused UFS leaf clocks %s; sources retained\n",
		 gate ? "gated" : "restored");
	return count;
}
static DEVICE_ATTR_RW(boot_ufs_gated);

static struct device_node *mt6833_firmware_config(struct device *dev)
{
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "mediatek,firmware-config", 0);
	/* Temporary platform device used for live transport tests has no OF node. */
	if (!np && !dev->of_node)
		np = of_find_compatible_node(NULL, NULL, "mediatek,mtk-lpm");
	return np;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	/* These are firmware-owned data slots, not read-to-clear IRQ registers. */
	return sysfs_emit(buf,
		"mode=guarded-bringup\nap_ready=0x%x\npower_control=0x%x\n"
		"l3_mode=%u\nbuck_mode=%u\npll_mode=%u\ntask_state=%u\n"
		"wake_cpu=%u\nmcusys_status=0x%lx\nspm_status=0x%lx\n"
		"spm_fw_type=0x%lx\ntransport=%u\nplatform_initialized=%u\n"
		"last_ack=0x%x\nready=%u\nwake_irqs_ready=%u\nwake_irq_count=%u\n"
		"idle_blocked=%u\nresources_ready=%u\neem_fixed_mode=%u\n"
		"s2idle_enabled=%u\ns2idle_active=%u\n",
		mt6833_mbox_read(lpm, MCUPM_AP_READY),
		mt6833_mbox_read(lpm, MCUPM_PWR_CTRL),
		mt6833_mbox_read(lpm, MCUPM_L3_MODE),
		mt6833_mbox_read(lpm, MCUPM_BUCK_MODE),
		mt6833_mbox_read(lpm, MCUPM_PLL_MODE),
		mt6833_mbox_read(lpm, MCUPM_TASK_STATE),
		mt6833_mbox_read(lpm, MCUPM_WAKE_CPU),
		lpm->mcusys_status, lpm->spm_status, lpm->fw_type,
		!!lpm->platform_mbox, lpm->platform_initialized, lpm->last_ack,
		lpm->ready, lpm->wake_irqs_ready, lpm->wake_irq_count,
		lpm->idle_blocked && !lpm->s2idle_active,
		lpm->resources_ready, lpm->eem_fixed_mode,
		lpm->s2idle_enabled, lpm->s2idle_active);
}
static DEVICE_ATTR_RO(status);

static void mt6833_power_policy_apply(struct mt6833_lpm *lpm)
{
	/* Runtime CPUs are held in WFI. Disable controls before changing modes. */
	writel(0, lpm->mbox + MCUPM_PWR_CTRL * sizeof(u32));
	/* Complete disabling controls before publishing replacement modes. */
	wmb();
	writel(lpm->power_policy & BIT(1) ? 2 : 0,
	       lpm->mbox + MCUPM_BUCK_MODE * sizeof(u32));
	writel(lpm->power_policy & BIT(2) ? 2 : 0,
	       lpm->mbox + MCUPM_PLL_MODE * sizeof(u32));
	/* Publish both mode fields before enabling their firmware controls. */
	wmb();
	writel(lpm->power_policy, lpm->mbox + MCUPM_PWR_CTRL * sizeof(u32));
	/* Complete policy publication before AP_READY or further PM operations. */
	wmb();
}

static ssize_t power_policy_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);

	guard(mutex)(&lpm->lock);
	return sysfs_emit(buf, "0x%x\n", lpm->power_policy);
}

static ssize_t power_policy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	u32 policy;
	int ret;

	ret = kstrtou32(buf, 0, &policy);
	/* Disabled, MCUSYS only, MCUSYS/buck/PLL, or stock with CM_MGR. */
	if (ret || (policy != 0 && policy != 1 && policy != 7 && policy != 15))
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (lpm->deep_enabled || lpm->s2idle_enabled)
		return -EBUSY;
	if (lpm->ready && (!lpm->idle_blocked ||
			   mt6833_mbox_read(lpm, MCUPM_TASK_STATE) != 3))
		return -EBUSY;
	lpm->power_policy = policy;
	if (lpm->ready)
		mt6833_power_policy_apply(lpm);
	return count;
}
static DEVICE_ATTR_RW(power_policy);

/* The caller holds lock. Only the platform channel's bit/data are touched. */
static int mt6833_channel_call(struct mt6833_lpm *lpm, void __iomem *mbox,
			      u32 bit, const u32 *data, unsigned int words)
{
	u32 pending;
	unsigned int i;
	int ret;

	if (!mbox)
		return -EOPNOTSUPP;
	if ((readl(lpm->tx_status) | readl(lpm->rx_status)) & bit)
		return -EBUSY;
	for (i = 0; i < words; i++)
		writel(data[i], mbox + i * sizeof(u32));
	/* Firmware must see the complete payload before the doorbell. */
	wmb();
	writel(bit, lpm->tx_set);
	ret = readl_poll_timeout(lpm->rx_status, pending,
				pending & bit, 100, 2000000);
	if (ret)
		return ret;
	lpm->last_ack = readl(mbox + MCUPM_PLATFORM_ACK);
	writel(bit, lpm->rx_clear);
	return 0;
}

static int mt6833_platform_call(struct mt6833_lpm *lpm, u32 cmd, u32 arg, u32 size)
{
	u32 data[] = { cmd, arg, size };

	return mt6833_channel_call(lpm, lpm->platform_mbox, MCUPM_PLATFORM_BIT,
				    data, ARRAY_SIZE(data));
}

static ssize_t alive_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&lpm->lock);
	ret = mt6833_platform_call(lpm, 0xdead, 0, 0);
	if (ret)
		return ret;
	if (!lpm->last_ack)
		return -EREMOTEIO;
	return sysfs_emit(buf, "ack=0x%x\n", lpm->last_ack);
}
static DEVICE_ATTR_ADMIN_RO(alive);

static ssize_t initialize_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	/* Exact vendor platform/logger headers; logging remains disabled. */
	__le32 header[] = {
		cpu_to_le32(MCUPM_PLATFORM_INIT), cpu_to_le32(16),
		cpu_to_le32(MCUPM_PLATFORM_MEM_SIZE), cpu_to_le32(16),
		cpu_to_le32(MCUPM_LOGGER_INIT), cpu_to_le32(24), 0,
		cpu_to_le32(24), cpu_to_le32(32), cpu_to_le32(SZ_1M), 0, 0,
	};
	bool initialize;
	int ret;

	ret = kstrtobool(buf, &initialize);
	if (ret || !initialize)
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (lpm->platform_initialized)
		return count;
	if (!lpm->shared_res.start || !lpm->platform_mbox)
		return -EOPNOTSUPP;
	if (mt6833_mbox_read(lpm, MCUPM_TASK_STATE) != 1 ||
	    mt6833_mbox_read(lpm, MCUPM_PWR_CTRL))
		return -EBUSY;
	if (!lpm->shared) {
		/* This firmware-shared DRAM is already reserved by memblock, not
		 * an unclaimed MMIO resource. Use the vendor's WC mapping type.
		 */
		lpm->shared = devm_ioremap_wc(dev, lpm->shared_res.start,
					 MCUPM_PLATFORM_MEM_SIZE);
		if (!lpm->shared)
			return -ENOMEM;
	}
	memcpy_toio(lpm->shared, header, sizeof(header));
	writel(MCUPM_PLATFORM_INIT, lpm->shared + MCUPM_PLATFORM_MEM_SIZE - 4);
	/* Publish the reserved-DRAM headers before the firmware receives its address. */
	wmb();
	ret = mt6833_platform_call(lpm, MCUPM_PLATFORM_INIT,
				  lower_32_bits(lpm->shared_res.start),
				  MCUPM_PLATFORM_MEM_SIZE);
	if (ret)
		return ret;
	if (!lpm->last_ack)
		return -EREMOTEIO;
	lpm->platform_initialized = true;
	dev_info(dev, "MCUPM platform service initialized, ack=0x%x\n", lpm->last_ack);
	return count;
}
static DEVICE_ATTR_WO(initialize);

static ssize_t prepare_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	u32 state;
	u32 ready_value;
	int ret;

	/* Both markers are defined by the vendor ABI for its PMIC variants. */
	ret = kstrtou32(buf, 0, &ready_value);
	if (ret || (ready_value != 1 && ready_value != 0x6360))
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (lpm->ready)
		return count;
	state = mt6833_mbox_read(lpm, MCUPM_TASK_STATE);
	if (!lpm->platform_initialized || !lpm->eem_fixed_mode ||
	    !lpm->wake_irqs_ready || !lpm->resources_ready ||
	    state < 1 || state > 3)
		return -EBUSY;

	/* Hold CPUs in WFI while publishing the exact vendor power policy.
	 * Keep that restriction until a suspend backend has been validated.
	 */
	if (!lpm->idle_blocked) {
		cpu_latency_qos_add_request(&lpm->latency, 0);
		lpm->idle_blocked = true;
	}
	/* Readiness must not implicitly enable unvalidated buck/PLL control.
	 * Stock mask 15 is available separately for explicitly armed tests.
	 */
	mt6833_power_policy_apply(lpm);
	writel(ready_value, lpm->mbox + MCUPM_AP_READY * sizeof(u32));
	ret = readl_poll_timeout(lpm->mbox + MCUPM_TASK_STATE * sizeof(u32), state,
				state == 2 || state == 3, 1000, 2000000);
	if (!ret)
		lpm->ready = true;
	if (ret)
		return ret;
	dev_info(dev, "MCUPM ready, task=%u; runtime CPUs restricted to WFI\n", state);
	return count;
}
static DEVICE_ATTR_WO(prepare);

static ssize_t log_enable_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;
	guard(mutex)(&lpm->lock);
	if (!lpm->platform_initialized || !lpm->shared)
		return -EBUSY;
	ret = mt6833_platform_call(lpm, MCUPM_LOGGER_INIT, enable ? 0x101 : 1, 0);
	if (ret)
		return ret;
	if (lpm->last_ack != enable)
		return -EREMOTEIO;
	writel(enable, lpm->shared + 16 + 8);
	return count;
}
static DEVICE_ATTR_WO(log_enable);

static ssize_t log_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	u32 read_pos, write_pos, length, tail;
	int header;

	guard(mutex)(&lpm->lock);
	if (!lpm->shared || !lpm->platform_initialized)
		return -EBUSY;
	read_pos = readl(lpm->shared + 40);
	write_pos = readl(lpm->shared + 44);
	if (read_pos >= SZ_1M || write_pos >= SZ_1M)
		return -EIO;
	header = sysfs_emit(buf, "read=%u write=%u\n", read_pos, write_pos);
	length = (write_pos + SZ_1M - read_pos) % SZ_1M;
	length = min_t(u32, length, PAGE_SIZE - header);
	tail = min(length, SZ_1M - read_pos);
	memcpy_fromio(buf + header, lpm->shared + 48 + read_pos, tail);
	if (tail < length)
		memcpy_fromio(buf + header + tail, lpm->shared + 48, length - tail);
	if (length)
		writel((read_pos + length) % SZ_1M, lpm->shared + 40);
	return header + length;
}
static DEVICE_ATTR_ADMIN_RO(log);

static ssize_t mailbox_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	unsigned int i;
	int length = 0;

	guard(mutex)(&lpm->lock);
	for (i = 0; i < MCUPM_MBOX_SIZE / sizeof(u32); i++)
		length += sysfs_emit_at(buf, length, "%02u: 0x%08x\n", i,
					mt6833_mbox_read(lpm, i));
	return length;
}
static DEVICE_ATTR_ADMIN_RO(mailbox);

struct mt6833_wake_irq {
	u32 hwirq;
	u32 category;
	u32 source;
};

static int mt6833_wake_irqs_parse(struct device *dev, struct mt6833_wake_irq *irqs,
				unsigned int *nr)
{
	struct device_node *parent, *np, *target;
	struct irq_data *data;
	u32 value[4];
	int i, j, n, irq, ret = 0;

	parent = mt6833_firmware_config(dev);
	if (!parent)
		return -ENODEV;
	*nr = 0;
	for (i = 0; (np = of_parse_phandle(parent, "irq-remain", i)); i++) {
		target = of_parse_phandle(np, "target", 0);
		n = of_property_count_u32_elems(np, "value");
		if (!target || n <= 0 || n % 4) {
			ret = -EINVAL;
			goto put_nodes;
		}
		for (j = 0; j < n; j += 4) {
			unsigned int k;

			if (*nr >= 32) {
				ret = -E2BIG;
				goto put_nodes;
			}
			for (k = 0; k < ARRAY_SIZE(value); k++)
				of_property_read_u32_index(np, "value", j + k, &value[k]);
			irq = of_irq_get(target, value[1]);
			if (irq <= 0) {
				/* The inherited vendor list contains absent camera IRQ
				 * indices. Vendor code skips these; active devices must
				 * still have a valid IRQ before we enable deep sleep.
				 */
				if (irq == -EOVERFLOW && !of_device_is_available(target)) {
					dev_info(dev, "skipping absent IRQ %u on inactive %pOF\n",
						 value[1], target);
					continue;
				}
				ret = irq ?: -EINVAL;
				goto put_nodes;
			}
			data = irq_get_irq_data(irq);
			/* Firmware needs the interrupt number in the root GIC domain. */
			while (data && data->parent_data)
				data = data->parent_data;
			if (!data || data->hwirq < 32 || data->hwirq > 1019) {
				ret = -EINVAL;
				goto put_nodes;
			}
			irqs[*nr] = (struct mt6833_wake_irq) {
				.hwirq = data->hwirq,
				.category = value[2],
				.source = value[3],
			};
			(*nr)++;
		}
		of_node_put(target);
		of_node_put(np);
	}
	ret = *nr ? 0 : -EINVAL;
	goto put_parent;
put_nodes:
	of_node_put(target);
	of_node_put(np);
put_parent:
	of_node_put(parent);
	return ret;
}

static ssize_t wake_irqs_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	struct mt6833_wake_irq irqs[32];
	unsigned int nr, i;
	bool configure;
	long ret;

	ret = kstrtobool(buf, &configure);
	if (ret || !configure)
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (!lpm->platform_initialized)
		return -EBUSY;
	if (lpm->wake_irqs_ready)
		return count;
	ret = mt6833_wake_irqs_parse(dev, irqs, &nr);
	if (ret)
		return ret;

	/* Legacy CPU_PM passes count/hwirq directly in the action slot.
	 * CPU_PM_LP uses explicit actions. Do not interchange those ABIs.
	 */
	ret = mt6833_lpm_call(LPM_USER_CPU_PM, 2, nr, 0, 0);
	if (ret < 0)
		return ret;
	ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 1, LPM_ACTION_SET, nr, 0);
	if (ret < 0)
		return ret;
	for (i = 0; i < nr; i++) {
		ret = mt6833_lpm_call(LPM_USER_CPU_PM, 3, irqs[i].hwirq,
				      irqs[i].category, irqs[i].source);
		if (ret < 0)
			return ret;
		ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 3, LPM_ACTION_SET,
				      irqs[i].hwirq, 0);
		if (ret < 0)
			return ret;
		ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 4, LPM_ACTION_SET,
				      irqs[i].category, 0);
		if (ret < 0)
			return ret;
		ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 5, LPM_ACTION_SET,
				      irqs[i].source, 0);
		if (ret < 0)
			return ret;
		ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 2, LPM_ACTION_PUSH, 0, 0);
		if (ret < 0)
			return ret;
		dev_info(dev, "wake IRQ: GIC %u category=0x%x source=0x%x\n",
			 irqs[i].hwirq, irqs[i].category, irqs[i].source);
	}
	ret = mt6833_lpm_call(LPM_USER_CPU_PM, 4, 0, 0, 0);
	if (ret < 0)
		return ret;
	ret = mt6833_lpm_call(LPM_USER_CPU_PM_LP, 2, LPM_ACTION_SUBMIT, 0, 0);
	if (ret < 0)
		return ret;
	lpm->wake_irqs_ready = true;
	lpm->wake_irq_count = nr;
	return count;
}
static DEVICE_ATTR_WO(wake_irqs);

static ssize_t resources_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	struct device_node *parent, *np;
	u32 id[8], value[8];
	unsigned int nr[2] = {}, i, start = 0, j;
	const char * const lists[] = { "resource-ctrl", "constraints" };
	bool configure;
	long ret;

	ret = kstrtobool(buf, &configure);
	if (ret || !configure)
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (!lpm->platform_initialized)
		return -EBUSY;
	if (lpm->resources_ready)
		return count;
	parent = mt6833_firmware_config(dev);
	if (!parent)
		return -ENODEV;
	for (j = 0; j < ARRAY_SIZE(lists); j++) {
		for (i = 0; (np = of_parse_phandle(parent, lists[j], i)); i++) {
			if (start >= ARRAY_SIZE(id)) {
				ret = -E2BIG;
				goto put_nodes;
			}
			ret = of_property_read_u32(np, "id", &id[start]);
			if (ret)
				goto put_nodes;
			ret = of_property_read_u32(np, "value", &value[start]);
			if (ret || id[start] >= 32) {
				ret = -EINVAL;
				goto put_nodes;
			}
			start++;
			nr[j]++;
			of_node_put(np);
		}
	}
	of_node_put(parent);
	if (!nr[0] || !nr[1])
		return -EINVAL;
	for (i = 0; i < start; i++) {
		if (i < nr[0])
			ret = mt6833_lpm_call(LPM_USER_SPM_DEBUG, 17,
					      LPM_ACTION_SET, id[i], value[i]);
		else
			ret = mt6833_lpm_call(LPM_USER_SPM_DEBUG, 18,
					      value[i] ? LPM_ACTION_SET : LPM_ACTION_CLEAR,
					      id[i], 0);
		if (ret < 0)
			return ret;
	}
	lpm->resources_ready = true;
	return count;
put_nodes:
	of_node_put(np);
	of_node_put(parent);
	return ret;
}
static DEVICE_ATTR_WO(resources);

static ssize_t fixed_voltage_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6833_lpm *lpm = dev_get_drvdata(dev);
	u32 data[4] = {};
	phys_addr_t address;
	bool initialize;
	int ret;

	ret = kstrtobool(buf, &initialize);
	if (ret || !initialize)
		return -EINVAL;
	guard(mutex)(&lpm->lock);
	if (lpm->eem_fixed_mode)
		return count;
	if (!lpm->platform_initialized || !lpm->eem_mbox ||
	    lpm->shared_capacity < MCUPM_EEM_MEM_OFFSET + MCUPM_EEM_LOG_SIZE)
		return -EOPNOTSUPP;
	if (!lpm->idle_blocked) {
		cpu_latency_qos_add_request(&lpm->latency, 0);
		lpm->idle_blocked = true;
	}
	address = lpm->shared_res.start + MCUPM_EEM_MEM_OFFSET;
	if (address > U32_MAX)
		return -EINVAL;
	if (!lpm->eem_shared) {
		lpm->eem_shared = devm_ioremap_wc(dev, address, MCUPM_EEM_LOG_SIZE);
		if (!lpm->eem_shared)
			return -ENOMEM;
	}
	/* The vendor's disabled-EEM fallback uses zero features and both
	 * enable bytes clear. No OPP voltages or calibration are supplied.
	 */
	memset_io(lpm->eem_shared, 0, MCUPM_EEM_LOG_SIZE);
	/* Publish disabled metadata before handing its address to firmware. */
	wmb();
	data[0] = 0; /* IPI_EEMSN_SHARERAM_INIT */
	data[1] = lower_32_bits(address);
	data[2] = MCUPM_EEM_LOG_SIZE;
	ret = mt6833_channel_call(lpm, lpm->eem_mbox, BIT(7), data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	if ((s32)lpm->last_ack < 0)
		return -EREMOTEIO;
	memset(data, 0, sizeof(data));
	data[0] = 5; /* IPI_EEMSN_INIT02, also sent when EEM/SN are disabled. */
	ret = mt6833_channel_call(lpm, lpm->eem_mbox, BIT(7), data, ARRAY_SIZE(data));
	if (ret)
		return ret;
	if ((s32)lpm->last_ack < 0)
		return -EREMOTEIO;
	if (readb(lpm->eem_shared + MCUPM_EEM_ENABLE_OFFSET) ||
	    readb(lpm->eem_shared + MCUPM_SN_ENABLE_OFFSET))
		return -EPROTO;
	lpm->eem_fixed_mode = true;
	dev_info(dev, "EEM/SN disabled-mode metadata accepted; existing OPPs retained\n");
	return count;
}
static DEVICE_ATTR_WO(fixed_voltage);

static struct attribute *mt6833_lpm_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_alive.attr,
	&dev_attr_initialize.attr,
	&dev_attr_prepare.attr,
	&dev_attr_wake_irqs.attr,
	&dev_attr_log_enable.attr,
	&dev_attr_log.attr,
	&dev_attr_mailbox.attr,
	&dev_attr_resources.attr,
	&dev_attr_fixed_voltage.attr,
	&dev_attr_power_policy.attr,
	&dev_attr_deep_enabled.attr,
	&dev_attr_stage_only.attr,
	&dev_attr_s2idle_enabled.attr,
	&dev_attr_s2idle_counters.attr,
	&dev_attr_counters.attr,
	&dev_attr_boot_peripherals_gated.attr,
	&dev_attr_boot_ufs_gated.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6833_lpm);

static int mt6833_lpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6833_lpm *lpm;
	struct resource *res;
	unsigned int i;
	int ret;

	/* The shared EEM ABI and policy below are verified for A226B firmware. */
	if (!of_machine_is_compatible("samsung,a22x"))
		return -ENODEV;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lpm-mailbox");
	if (!res || resource_size(res) != MCUPM_MBOX_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid MCUPM LPM mailbox\n");
	lpm = devm_kzalloc(dev, sizeof(*lpm), GFP_KERNEL);
	if (!lpm)
		return -ENOMEM;
	lpm->dev = dev;
	for (i = 0; i < ARRAY_SIZE(lpm->boot_clocks); i++)
		lpm->boot_clocks[i].id = boot_peripheral_clock_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(lpm->boot_clocks), lpm->boot_clocks);
	if (ret)
		return dev_err_probe(dev, ret, "missing board peripheral clocks\n");
	/* A board must explicitly identify a disabled UFS controller before any
	 * unused-storage gating is offered. There is no automatic boot cleanup.
	 */
	{
		struct device_node *ufs;

		ufs = of_parse_phandle(dev->of_node, "mediatek,unused-ufs", 0);
		if (ufs) {
			lpm->ufs_unused = !of_device_is_available(ufs) &&
				of_device_is_compatible(ufs, "mediatek,ufshci");
			of_node_put(ufs);
			if (!lpm->ufs_unused)
				return dev_err_probe(dev, -EINVAL, "UFS is not board-unused\n");
			for (i = 0; i < ARRAY_SIZE(lpm->ufs_clocks); i++)
				lpm->ufs_clocks[i].id = boot_ufs_clock_names[i];
			ret = devm_clk_bulk_get(dev, ARRAY_SIZE(lpm->ufs_clocks),
						lpm->ufs_clocks);
			if (ret)
				return dev_err_probe(dev, ret, "missing UFS leaf clocks\n");
			for (i = 0; i < ARRAY_SIZE(lpm->ufs_sources); i++)
				lpm->ufs_sources[i].id = boot_ufs_source_names[i];
			ret = devm_clk_bulk_get(dev, ARRAY_SIZE(lpm->ufs_sources),
						lpm->ufs_sources);
			if (ret)
				return dev_err_probe(dev, ret, "missing UFS source clocks\n");
		}
	}
	lpm->s2idle_attempts = devm_kcalloc(dev, nr_cpu_ids,
					 sizeof(*lpm->s2idle_attempts), GFP_KERNEL);
	lpm->s2idle_resumes = devm_kcalloc(dev, nr_cpu_ids,
					sizeof(*lpm->s2idle_resumes), GFP_KERNEL);
	lpm->s2idle_errors = devm_kcalloc(dev, nr_cpu_ids,
				       sizeof(*lpm->s2idle_errors), GFP_KERNEL);
	if (!lpm->s2idle_attempts || !lpm->s2idle_resumes || !lpm->s2idle_errors)
		return -ENOMEM;

	/* Stock MCUPM has no clock/power-domain dependency for this CPU SRAM. */
	lpm->mbox = devm_ioremap_resource(dev, res);
	if (IS_ERR(lpm->mbox))
		return PTR_ERR(lpm->mbox);
	platform_set_drvdata(pdev, lpm);
	mutex_init(&lpm->lock);
	/* Optional transport resources permit staged bring-up on the live kernel. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "platform-mailbox");
	if (res) {
		if (resource_size(res) != MCUPM_MBOX_SIZE)
			return -EINVAL;
		lpm->platform_mbox = devm_ioremap_resource(dev, res);
		if (IS_ERR(lpm->platform_mbox))
			return PTR_ERR(lpm->platform_mbox);
		lpm->tx_set = devm_platform_ioremap_resource_byname(pdev, "tx-set");
		if (IS_ERR(lpm->tx_set))
			return PTR_ERR(lpm->tx_set);
		lpm->tx_status = devm_platform_ioremap_resource_byname(pdev, "tx-status");
		if (IS_ERR(lpm->tx_status))
			return PTR_ERR(lpm->tx_status);
		lpm->rx_clear = devm_platform_ioremap_resource_byname(pdev, "rx-clear");
		if (IS_ERR(lpm->rx_clear))
			return PTR_ERR(lpm->rx_clear);
		lpm->rx_status = devm_platform_ioremap_resource_byname(pdev, "rx-status");
		if (IS_ERR(lpm->rx_status))
			return PTR_ERR(lpm->rx_status);
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "platform-memory");
	if (res || dev->of_node) {
		struct device_node *np;
		struct reserved_mem *rmem;

		if (dev->of_node)
			np = of_parse_phandle(dev->of_node, "memory-region", 0);
		else
			np = of_find_compatible_node(NULL, NULL,
						    "mediatek,reserve-memory-mcupm_share");
		rmem = np ? of_reserved_mem_lookup(np) : NULL;
		of_node_put(np);
		if (!rmem || rmem->base > U32_MAX ||
		    rmem->size < MCUPM_EEM_MEM_OFFSET + MCUPM_EEM_LOG_SIZE ||
		    rmem->size - 1 > U32_MAX - rmem->base)
			return -EINVAL;
		if (res && (rmem->base != res->start ||
			    resource_size(res) != rmem->size))
			return -EINVAL;
		lpm->shared_res = (struct resource) {
			.start = rmem->base,
			.end = rmem->base + MCUPM_PLATFORM_MEM_SIZE - 1,
			.flags = IORESOURCE_MEM,
		};
		lpm->shared_capacity = rmem->size;
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eem-mailbox");
	if (res) {
		if (resource_size(res) != MCUPM_MBOX_SIZE)
			return -EINVAL;
		lpm->eem_mbox = devm_ioremap_resource(dev, res);
		if (IS_ERR(lpm->eem_mbox))
			return PTR_ERR(lpm->eem_mbox);
	}
	lpm->mcusys_status = mt6833_lpm_get(LPM_USER_CPU_PM, LPM_MCUSYS_STATUS);
	lpm->spm_status = mt6833_lpm_get(LPM_USER_SPM, LPM_SPM_STATUS);
	lpm->fw_type = mt6833_lpm_get(LPM_USER_SPM, LPM_SPM_FW_TYPE);
	if (lpm->fw_type != 4)
		return dev_err_probe(dev, -EOPNOTSUPP, "unsupported SPM firmware\n");
	dev_info(dev, "MCUPM task=%u AP_READY=0x%x SPM firmware=0x%lx; initialization is opt-in\n",
		 mt6833_mbox_read(lpm, MCUPM_TASK_STATE),
		 mt6833_mbox_read(lpm, MCUPM_AP_READY), lpm->fw_type);
#ifndef MODULE
	/* Replace the advertised but unsuitable SYSTEM_SUSPEND path even
	 * before bring-up. Deep requests fail safely until explicitly armed.
	 */
	if (mt6833_suspend_lpm)
		return -EBUSY;
	if (!psci_ops.cpu_suspend || !psci_power_state_is_valid(0x1010100))
		return -EOPNOTSUPP;
	mt6833_suspend_lpm = lpm;
	suspend_set_ops(&mt6833_suspend_ops);
	s2idle_set_ops(&mt6833_s2idle_ops);
#endif
	return 0;
}

static const struct of_device_id mt6833_lpm_of_match[] = {
	{ .compatible = "mediatek,mt6833-lpm" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6833_lpm_of_match);

static void mt6833_lpm_remove(struct platform_device *pdev)
{
	struct mt6833_lpm *lpm = platform_get_drvdata(pdev);

	if (lpm->ufs_clocks_held)
		clk_bulk_disable_unprepare(ARRAY_SIZE(lpm->ufs_clocks),
					   lpm->ufs_clocks);
	if (lpm->ufs_sources_held)
		clk_bulk_disable_unprepare(ARRAY_SIZE(lpm->ufs_sources),
					   lpm->ufs_sources);
	if (lpm->idle_blocked) {
		writel(0, lpm->mbox + MCUPM_PWR_CTRL * sizeof(u32));
		writel(0, lpm->mbox + MCUPM_BUCK_MODE * sizeof(u32));
		writel(0, lpm->mbox + MCUPM_PLL_MODE * sizeof(u32));
		/* Disable optional firmware power controls before releasing WFI. */
		wmb();
		cpu_latency_qos_remove_request(&lpm->latency);
	}
}

static struct platform_driver mt6833_lpm_driver = {
	.probe = mt6833_lpm_probe,
	.remove = mt6833_lpm_remove,
	.driver = {
		.name = "mt6833-lpm",
		.of_match_table = mt6833_lpm_of_match,
		.dev_groups = mt6833_lpm_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6833_lpm_driver);

MODULE_DESCRIPTION("MT6833 firmware low-power coordination");
MODULE_LICENSE("GPL");
