// SPDX-License-Identifier: GPL-2.0-only
/* MT6833 boot-loaded firmware coordination and guarded suspend bring-up. */
#include <linux/arm-smccc.h>
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

struct mt6833_lpm {
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
	bool idle_blocked;
	bool platform_initialized;
	bool ready;
	bool wake_irqs_ready;
	bool resources_ready;
	bool eem_fixed_mode;
	bool deep_enabled;
	bool stage_only;
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
#endif

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
	if (enable && lpm->stage_only)
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
	if (lpm->deep_enabled)
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
		"idle_blocked=%u\nresources_ready=%u\neem_fixed_mode=%u\n",
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
		lpm->idle_blocked, lpm->resources_ready, lpm->eem_fixed_mode);
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
	if (lpm->deep_enabled)
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
	&dev_attr_counters.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6833_lpm);

static int mt6833_lpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6833_lpm *lpm;
	struct resource *res;

	/* The shared EEM ABI and policy below are verified for A226B firmware. */
	if (!of_machine_is_compatible("samsung,a22x"))
		return -ENODEV;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lpm-mailbox");
	if (!res || resource_size(res) != MCUPM_MBOX_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid MCUPM LPM mailbox\n");
	lpm = devm_kzalloc(dev, sizeof(*lpm), GFP_KERNEL);
	if (!lpm)
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
