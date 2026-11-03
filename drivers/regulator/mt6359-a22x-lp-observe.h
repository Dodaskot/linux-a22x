/* SPDX-License-Identifier: GPL-2.0-only */
/* Opt-in, bounded early A22x observations. No PMIC/selector/RC writes. */
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

static bool a22x_lp_probe;
module_param_named(a22x_lp_probe, a22x_lp_probe, bool, 0400);
MODULE_PARM_DESC(a22x_lp_probe, "Read A22x MT6359P LP fields before/after regulator probe");
static atomic_t a22x_lp_probe_attempts = ATOMIC_INIT(0);

struct a22x_lp_word {
	const char *name;
	u16 address;
};

/* Same ordinary-read fields as the source-qualified RF76 observer. */
static const struct a22x_lp_word a22x_lp_words[] = {
	{ "VXO22_EN", 0x1b9a }, { "VXO22_MODE", 0x1b9c },
	{ "VXO22_MON", 0x1b9e }, { "VXO22_OP_EN", 0x1ba0 },
	{ "VXO22_OP_CFG", 0x1ba6 },
	{ "VRF12_EN", 0x1bbe }, { "VRF12_MODE", 0x1bc0 },
	{ "VRF12_MON", 0x1bc2 }, { "VRF12_OP_EN", 0x1bc4 },
	{ "VRF12_OP_CFG", 0x1bca },
	{ "VA09_EN", 0x1c40 }, { "VA09_MODE", 0x1c42 },
	{ "VA09_MON", 0x1c44 }, { "VA09_OP_EN", 0x1c46 },
	{ "VA09_OP_CFG", 0x1c4c },
	{ "VA12_EN", 0x1c64 }, { "VA12_MODE", 0x1c66 },
	{ "VA12_MON", 0x1c68 }, { "VA12_OP_EN", 0x1c6a },
	{ "VA12_OP_CFG", 0x1c70 },
};

static const struct a22x_lp_word a22x_lp_rc_words[] = {
	{ "RC_CENTRAL_CFG1", 0x004 }, { "RC_RF_SRCLKEN_CFG", 0x024 },
	{ "RC_FSM_STA_0", 0x100 }, { "RC_CMD_STA_0", 0x104 },
	{ "RC_CMD_STA_1", 0x108 }, { "RC_SPI_STA_0", 0x10c },
	{ "RC_PI_PO_STA_0", 0x110 }, { "RC_RF_REQ_STA_0", 0x11c },
};

static int a22x_lp_rc_read(u32 *values)
{
	struct device_node *node, *it;
	struct platform_device *owner;
	struct resource res;
	struct resource *claim = NULL;
	void __iomem *map;
	const char *prop;
	unsigned int matches = 0, i;
	int len, ret = -ENODEV;

	for_each_compatible_node(it, NULL, "mediatek,srclken")
		matches++;
	if (matches != 1)
		return -ENODEV;
	node = of_find_node_by_path("/srclken@1000f800");
	if (!node)
		return -ENODEV;
	prop = of_get_property(node, "status", &len);
	if (!prop || len != sizeof("disabled") ||
	    memcmp(prop, "disabled", sizeof("disabled")) || of_device_is_available(node))
		goto out_node;
	prop = of_get_property(node, "compatible", &len);
	if (!prop || len != sizeof("mediatek,srclken") ||
	    memcmp(prop, "mediatek,srclken", sizeof("mediatek,srclken")))
		goto out_node;
	if (!of_get_property(node, "reg", &len) || len != 16 ||
	    of_address_to_resource(node, 0, &res) || res.start != 0x1000f800 ||
	    resource_size(&res) != 0x1000 || resource_type(&res) != IORESOURCE_MEM)
		goto out_node;
	owner = of_find_device_by_node(node);
	if (owner) {
		put_device(&owner->dev);
		ret = -EBUSY;
		goto out_node;
	}
	/* Claim only the read window; the full vendor DT range overlaps keypad. */
	claim = request_mem_region(res.start, 0x120, "a22x-lp-probe-readonly");
	if (!claim) {
		ret = -EBUSY;
		goto out_node;
	}
	map = ioremap(res.start, 0x120);
	if (!map) {
		ret = -ENOMEM;
		goto out_claim;
	}
	for (i = 0; i < ARRAY_SIZE(a22x_lp_rc_words); i++)
		values[i] = readl(map + a22x_lp_rc_words[i].address);
	iounmap(map);
	ret = 0;
out_claim:
	release_mem_region(res.start, 0x120);
out_node:
	of_node_put(node);
	return ret;
}

static unsigned int a22x_lp_observe_begin(struct mt6397_chip *chip, int hw_ver)
{
	unsigned int attempt;

	if (!a22x_lp_probe || !of_machine_is_compatible("samsung,a22x") ||
	    chip->chip_id != MT6359_CHIP_ID || hw_ver < MT6359P_CHIP_VER)
		return 0;
	attempt = atomic_inc_return(&a22x_lp_probe_attempts);
	/* Driver-core probe retries remain distinguishable and bounded. */
	return attempt <= 4 ? attempt : 0;
}

static void a22x_lp_observe(struct device *dev, struct regmap *map,
			    unsigned int attempt, const char *phase, int hw_ver)
{
	u32 values[ARRAY_SIZE(a22x_lp_words)] = {};
	u32 rc_values[ARRAY_SIZE(a22x_lp_rc_words)] = {};
	int errors[ARRAY_SIZE(a22x_lp_words)], rc_error;
	u64 begin, pmic_end, rc_begin, end;
	unsigned int i;

	if (!attempt)
		return;
	begin = ktime_get_boottime_ns();
	for (i = 0; i < ARRAY_SIZE(a22x_lp_words); i++)
		errors[i] = regmap_read(map, a22x_lp_words[i].address, &values[i]);
	pmic_end = ktime_get_boottime_ns();
	rc_begin = ktime_get_boottime_ns();
	rc_error = a22x_lp_rc_read(rc_values);
	end = ktime_get_boottime_ns();
	/* Log only after capture; the two domains are serial, not atomic. */
	dev_info(dev, "A22X_LP_PROBE v1 attempt=%u phase=%s hwcid=%04x begin=%llu pmic_end=%llu rc_begin=%llu end=%llu rc_error=%d writes=0\n",
		 attempt, phase, hw_ver, begin, pmic_end, rc_begin, end, rc_error);
	for (i = 0; i < ARRAY_SIZE(a22x_lp_words); i++)
		dev_info(dev, "A22X_LP_PMIC attempt=%u phase=%s field=%s addr=%04x value=%04x error=%d\n",
			 attempt, phase, a22x_lp_words[i].name, a22x_lp_words[i].address,
			 values[i], errors[i]);
	if (!rc_error)
		for (i = 0; i < ARRAY_SIZE(a22x_lp_rc_words); i++)
			dev_info(dev, "A22X_LP_RC attempt=%u phase=%s field=%s offset=%03x value=%08x\n",
				 attempt, phase, a22x_lp_rc_words[i].name,
				 a22x_lp_rc_words[i].address, rc_values[i]);
}
