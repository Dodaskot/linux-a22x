/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/mutex.h>
#include "mt6359-a22x-lp-init-core.h"

static bool a22x_lp_stock_init;
module_param_named(a22x_lp_stock_init, a22x_lp_stock_init, bool, 0400);
MODULE_PARM_DESC(a22x_lp_stock_init, "Opt-in A22x MT6359P four-LDO stock LP init before regulator registration");
static DEFINE_MUTEX(a22x_lp_init_lock);
static bool a22x_lp_init_attempted;
static struct a22x_lp_transaction a22x_lp_init_receipt;

static int a22x_lp_init_read(void *ctx, unsigned int reg, unsigned int *value)
{
	return regmap_read(ctx, reg, value);
}

static int a22x_lp_init_update(void *ctx, unsigned int reg,
			       unsigned int mask, unsigned int value)
{
	return regmap_update_bits(ctx, reg, mask, value);
}

static u64 a22x_lp_init_now(void *ctx)
{
	return ktime_get_boottime_ns();
}

static bool a22x_lp_init_provider(struct device *dev, struct mt6397_chip *chip,
				 int hw_ver)
{
	struct device *wrapper;

	if (!of_machine_is_compatible("samsung,a22x") ||
	    !of_machine_is_compatible("mediatek,mt6833") || hw_ver != 0x5930 ||
	    !chip || chip->chip_id != MT6359_CHIP_ID || !chip->regmap ||
	    chip->dev != dev->parent || !chip->dev->of_node)
		return false;
	wrapper = chip->dev->parent;
	if (!wrapper || !wrapper->of_node ||
	    !of_device_is_available(chip->dev->of_node) ||
	    !of_device_is_available(wrapper->of_node) ||
	    strcmp(of_node_full_name(chip->dev->of_node),
		   "/pmic-wrapper@10026000/pmic") ||
	    strcmp(of_node_full_name(wrapper->of_node), "/pmic-wrapper@10026000") ||
	    !of_device_is_compatible(chip->dev->of_node, "mediatek,mt6359") ||
	    !of_device_is_compatible(wrapper->of_node, "mediatek,mt6833-pwrap"))
		return false;
	return dev_get_regmap(wrapper, NULL) == chip->regmap &&
	       regmap_get_device(chip->regmap) == wrapper &&
	       regmap_get_val_bytes(chip->regmap) == 2;
}

static int a22x_lp_init(struct device *dev, struct mt6397_chip *chip, int hw_ver)
{
	struct a22x_lp_transaction *t = &a22x_lp_init_receipt;
	struct a22x_lp_io io = {
		.read = a22x_lp_init_read, .update = a22x_lp_init_update,
		.now = a22x_lp_init_now,
	};
	unsigned int i, values[12];
	int ret;
	u64 begin, end;

	if (!a22x_lp_stock_init)
		return 0;
	if (!a22x_lp_init_provider(dev, chip, hw_ver))
		return dev_err_probe(dev, -ENODEV, "A22X_LP_INIT provider rejected; writes=0\n");
	io.ctx = chip->regmap;

	mutex_lock(&a22x_lp_init_lock);
	if (a22x_lp_init_attempted) {
		/* Probe retries may verify success, never reapply or retry a failure. */
		ret = t->complete ? a22x_lp_verify(&io, values) : -ECANCELED;
		dev_info(dev, "A22X_LP_INIT recheck result=%d writes=0\n", ret);
		goto out;
	}
	a22x_lp_init_attempted = true;
	begin = ktime_get_boottime_ns();
	ret = a22x_lp_apply(&io, t);
	end = ktime_get_boottime_ns();
	/* Print after the transaction, so console latency does not split writes. */
	dev_info(dev, "A22X_LP_INIT v1 hwcid=%04x begin=%llu end=%llu saved=%u attempted=%u verified=%u complete=%u error=%d undo_attempted=%u undo_verified=%u rollback_error=%d\n",
		 hw_ver, begin, end, t->saved, t->attempted, t->verified,
		 t->complete, t->error, t->undo_attempted, t->undo_verified,
		 t->rollback_error);
	for (i = 0; i < t->saved; i++)
		dev_info(dev, "A22X_LP_INIT_FIELD i=%u addr=%04x mask=%04x pre=%04x final=%04x rollback=%04x\n",
			 i, a22x_lp_init_fields[i].address, a22x_lp_init_fields[i].mask,
			 t->pre[i], t->final[i], t->rollback[i]);
	for (i = 0; i < t->attempted; i++) {
		const struct a22x_lp_update *u = &a22x_lp_init_updates[i];
		const struct a22x_lp_step *s = &t->step[i], *b = &t->undo[i];

		dev_info(dev, "A22X_LP_INIT_STEP i=%u addr=%04x mask=%04x value=%04x begin=%llu end=%llu write_error=%d read_error=%d readback=%04x\n",
			 i, a22x_lp_init_fields[u->field].address, u->mask, u->mask,
			 s->begin, s->end, s->write_error, s->read_error, s->readback);
		if (t->undo_attempted)
			dev_info(dev, "A22X_LP_INIT_UNDO i=%u begin=%llu end=%llu write_error=%d read_error=%d readback=%04x\n",
				 i, b->begin, b->end, b->write_error, b->read_error,
				 b->readback);
	}
out:
	mutex_unlock(&a22x_lp_init_lock);
	return ret;
}
