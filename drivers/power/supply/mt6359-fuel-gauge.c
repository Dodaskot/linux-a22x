// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6359(P) fuel gauge (FGADC coulomb counter).
 *
 * The bootloader leaves the FGADC running (FG_ON=1, clocks ungated); this
 * driver only reads it. The state of charge is taken from the OCV table once
 * at probe (cell voltage corrected by I*R with the measured current), then
 * followed by the hardware charge counter, which keeps counting in suspend.
 * A full charge re-anchors it to 100 %; near empty the count is pulled down
 * towards the OCV estimate, so a worn cell does not die at "10 %".
 *
 * One reading of the voltage under the load of a starting system is a poor
 * witness: between 20 % and 50 % the curve of this cell rises 3 mV per percent,
 * so 100 mV of sag is 30 points (seen: 15 % shown at 3.79 V, which is 30 %).
 * Two things mend that. The state of charge that was last shown is kept where
 * Android's gauge keeps its own, in the RTC domain of the PMIC, and is taken
 * as the start while the voltage does not contradict it; every system on the
 * phone then goes on from the same number. And while little current flows, the
 * count is drawn one percent per poll towards the smoothed OCV estimate. A cell
 * that is drained for minutes sags more than its resistance explains, and one
 * that is charged stands higher (seen: ten points in six minutes at 0.2-0.5 A),
 * so the estimate is heard at four percent apart only while the cell rests.
 * Under a current it is heard at six percent against the current (while
 * draining, that the cell holds more; while charging, that it holds less) and
 * at fifteen the other way: the vendor's gauge also counts charge from the
 * stored number and lets the voltage speak only at the ends of the curve.
 * The attribute soc_start tells user space where the count began.
 *
 * Register layout and units from the vendor mt6359_gauge.c (GM3.0):
 *   current: 610.352 uA/LSB, CAR: 0.085 uAh/LSB, both for a 5 mOhm sense
 *   resistor and scaled by 5 mOhm / R_sense; positive = charging.
 */
#include <linux/bitfield.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MT6359_FGADC_CON0		0xd08
#define MT6359_FGADC_CON1		0xd0a
#define MT6359_FGADC_CAR_CON0		0xd16
#define MT6359_FGADC_CAR_CON1		0xd18
#define MT6359_FGADC_CUR_CON0		0xd8a

/*
 * The state of charge the user was last shown: RTC_AL_HOU[15:8], bit 7 valid,
 * bits 6:0 percent (vendor mt6359_gauge.c fgauge_set_rtc_ui_soc, RTC_FGSOC).
 * The low byte is the alarm hour of the RTC driver and is never touched here.
 */
#define MT6359_RTC_BBPU			0x588
#define MT6359_RTC_AL_HOU		0x5a4
#define MT6359_RTC_PROT			0x5bc
#define MT6359_RTC_WRTGR		0x5c2
#define RTC_PROT_UNLOCK1		0x586a
#define RTC_PROT_UNLOCK2		0x9136
#define RTC_BBPU_CBUSY			BIT(6)
#define RTC_BBPU_RELOAD			BIT(5)
#define RTC_BBPU_KEY			0x4300
#define RTC_AL_HOU_FG_MASK		GENMASK(15, 8)
#define RTC_FG_SOC_VALID		BIT(7)
#define RTC_FG_SOC_MASK			GENMASK(6, 0)

#define FGADC_CON0_FG_ON		BIT(0)
#define FGADC_CON1_READ_PRE		BIT(0)
#define FGADC_CON1_SW_CLEAR		BIT(3)
#define FGADC_CON1_LATCHDATA_ST		BIT(15)

#define FG_CUR_NA_PER_LSB		610352	/* at 5 mOhm */
#define FG_CAR_PAH_PER_LSB		85000	/* pico-Ah (0.085 uAh) at 5 mOhm */
#define FG_DEFAULT_RSENSE_UOHM		5000
#define FG_DEFAULT_RBAT_UOHM		120000
#define FG_POLL_INTERVAL		(30 * HZ)
#define FG_LOW_OCV_CAPACITY		15	/* % below which OCV is trusted */
#define FG_LOW_MAX_ERROR		5	/* % allowed above the OCV estimate */
#define FG_STORED_MAX_UV		70000	/* the stored charge may lie this far from the voltage */
#define FG_RELAXED_UA			400000	/* below this the OCV estimate is heard */
#define FG_OCV_SETTLE			4	/* OCV readings before any correction */
#define FG_OCV_APART			4	/* % apart at rest before the count is corrected */
#define FG_OCV_AGAINST			6	/* the same against the current */
#define FG_OCV_WITH			15	/* the same along the current */
#define FG_STEADY_UA			60000	/* beyond this the current biases the OCV */

struct mt6359_fg {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;			/* serialises latch/read */
	struct power_supply *psy;
	struct power_supply_battery_info *info;
	struct iio_channel *vbat;
	struct delayed_work work;

	u32 rsense_uohm;
	u32 rbat_uohm;
	s64 full_uah;			/* charge-full-design */
	s64 car0_uah;			/* counter value at the anchor point */
	s64 charge0_uah;		/* charge at the anchor point */
	bool anchored;
	int ocv_avg;			/* smoothed OCV estimate, percent * 16 */
	unsigned int ocv_samples;
	int stored;			/* percent last written to the RTC, -1: none */
	bool rtc_unlocked;		/* the RTC write interface was opened */
	int rtc_error;			/* what the last write to the RTC ended with */
	unsigned int stored_raw;	/* RTC_AL_HOU as it was found */
	int start_cap;			/* percent the count began at, -1: not yet */
	int start_ocv;			/* what the voltage said then */
	bool start_stored;		/* it began at the stored percent */
};

/* Latch the counters, run @read, release the latch (vendor sequence). */
static int mt6359_fg_latched_read(struct mt6359_fg *fg, unsigned int reg,
				  unsigned int *val, unsigned int *val_hi)
{
	unsigned int st;
	int ret, err;

	mutex_lock(&fg->lock);
	ret = regmap_update_bits(fg->regmap, MT6359_FGADC_CON1,
				 FGADC_CON1_READ_PRE, FGADC_CON1_READ_PRE);
	if (ret)
		goto out;
	ret = regmap_read_poll_timeout(fg->regmap, MT6359_FGADC_CON1, st,
				       st & FGADC_CON1_LATCHDATA_ST, 10, 10000);
	if (!ret) {
		ret = regmap_read(fg->regmap, reg, val);
		if (!ret && val_hi)
			ret = regmap_read(fg->regmap, reg + 2, val_hi);
	}

	err = regmap_update_bits(fg->regmap, MT6359_FGADC_CON1,
				 FGADC_CON1_SW_CLEAR | FGADC_CON1_READ_PRE,
				 FGADC_CON1_SW_CLEAR);
	if (!err)
		err = regmap_read_poll_timeout(fg->regmap, MT6359_FGADC_CON1,
					       st, !(st & FGADC_CON1_LATCHDATA_ST),
					       10, 10000);
	regmap_update_bits(fg->regmap, MT6359_FGADC_CON1,
			   FGADC_CON1_SW_CLEAR, 0);
	if (!ret)
		ret = err;
out:
	mutex_unlock(&fg->lock);
	return ret;
}

static s64 mt6359_fg_scale(struct mt6359_fg *fg, s64 v)
{
	return div_s64(v * FG_DEFAULT_RSENSE_UOHM, fg->rsense_uohm);
}

static int mt6359_fg_current_ua(struct mt6359_fg *fg, int *ua)
{
	unsigned int raw;
	s64 lsb;
	int ret;

	ret = mt6359_fg_latched_read(fg, MT6359_FGADC_CUR_CON0, &raw, NULL);
	if (ret)
		return ret;
	/* vendor convention: > 0x7fff is discharging, magnitude 0xffff - raw */
	lsb = raw > 0x7fff ? -(s64)(0xffff - raw) : raw;
	*ua = mt6359_fg_scale(fg, div_s64(lsb * FG_CUR_NA_PER_LSB, 1000));
	return 0;
}

static int mt6359_fg_car_uah(struct mt6359_fg *fg, s64 *uah)
{
	unsigned int lo, hi;
	s64 lsb;
	int ret;

	ret = mt6359_fg_latched_read(fg, MT6359_FGADC_CAR_CON0, &lo, &hi);
	if (ret)
		return ret;
	lsb = (s64)(lo & 0xffff) | (s64)(hi & 0x7fff) << 16;
	if (hi & BIT(15))
		lsb = -(0x7fffffffLL - lsb);
	*uah = mt6359_fg_scale(fg, div_s64(lsb * FG_CAR_PAH_PER_LSB, 1000000));
	return 0;
}

static int mt6359_fg_voltage_uv(struct mt6359_fg *fg, int *uv)
{
	int ret = iio_read_channel_processed_scale(fg->vbat, uv, 1000);

	return ret < 0 ? ret : 0;
}

static int mt6359_fg_ocv_uv(struct mt6359_fg *fg, int *ocv)
{
	int uv, ua, ret;

	ret = mt6359_fg_voltage_uv(fg, &uv);
	if (ret)
		return ret;
	ret = mt6359_fg_current_ua(fg, &ua);
	if (ret)
		return ret;
	/* charging current raises the terminal voltage above OCV */
	*ocv = uv - (int)div_s64((s64)ua * fg->rbat_uohm, 1000000);
	return 0;
}

static int mt6359_fg_ocv_capacity(struct mt6359_fg *fg, int *cap)
{
	int uv, ret = mt6359_fg_ocv_uv(fg, &uv);

	if (ret)
		return ret;
	*cap = power_supply_batinfo_ocv2cap(fg->info, uv, 25);
	if (*cap < 0)
		return *cap;
	return 0;
}

/* The open-circuit voltage the cell's curve gives for a percent. */
static int mt6359_fg_capacity_uv(struct mt6359_fg *fg, int cap)
{
	const struct power_supply_battery_ocv_table *t = fg->info->ocv_table[0];
	int n = fg->info->ocv_table_size[0], i;

	if (cap >= t[0].capacity)
		return t[0].ocv;
	for (i = 1; i < n; i++)
		if (cap >= t[i].capacity)
			return t[i].ocv + (t[i - 1].ocv - t[i].ocv) *
			       (cap - t[i].capacity) /
			       max(t[i - 1].capacity - t[i].capacity, 1);
	return t[n - 1].ocv;
}

static int mt6359_fg_anchor(struct mt6359_fg *fg, int capacity)
{
	s64 car;
	int ret = mt6359_fg_car_uah(fg, &car);

	if (ret)
		return ret;
	fg->car0_uah = car;
	fg->charge0_uah = div_s64(fg->full_uah * capacity, 100);
	fg->anchored = true;
	dev_dbg(fg->dev, "anchored at %d%%\n", capacity);
	return 0;
}

/* The state of charge last shown by this driver or by Android's gauge. */
static int mt6359_fg_stored_capacity(struct mt6359_fg *fg, int *cap)
{
	unsigned int val;
	int ret = regmap_read(fg->regmap, MT6359_RTC_AL_HOU, &val);

	if (ret)
		return ret;
	fg->stored_raw = val;
	val = FIELD_GET(RTC_AL_HOU_FG_MASK, val);
	if (!(val & RTC_FG_SOC_VALID) || !(val & RTC_FG_SOC_MASK) ||
	    (val & RTC_FG_SOC_MASK) > 100)
		return -ENODATA;
	*cap = val & RTC_FG_SOC_MASK;
	return 0;
}

/* What was written to the RTC's registers takes effect with the write trigger. */
static int mt6359_fg_rtc_trigger(struct mt6359_fg *fg)
{
	unsigned int busy;
	int ret = regmap_write(fg->regmap, MT6359_RTC_WRTGR, 1);

	if (ret)
		return ret;
	/* the RTC runs from 32 kHz: the vendor waits up to half a second */
	return regmap_read_poll_timeout(fg->regmap, MT6359_RTC_BBPU, busy,
					!(busy & RTC_BBPU_CBUSY), 200, 500000);
}

/*
 * The bootloader leaves the RTC's write interface closed, and a write trigger
 * then changes nothing (seen: 63 % written, 96 % of days before read after the
 * restart). The vendor kernel opens it once in its RTC driver's probe
 * (rtc_writeif_unlock), with the same two keys.
 */
static int mt6359_fg_rtc_unlock(struct mt6359_fg *fg)
{
	int ret;

	if (fg->rtc_unlocked)
		return 0;
	ret = regmap_write(fg->regmap, MT6359_RTC_PROT, RTC_PROT_UNLOCK1);
	if (!ret)
		ret = mt6359_fg_rtc_trigger(fg);
	if (!ret)
		ret = regmap_write(fg->regmap, MT6359_RTC_PROT, RTC_PROT_UNLOCK2);
	if (!ret)
		ret = mt6359_fg_rtc_trigger(fg);
	fg->rtc_unlocked = !ret;
	if (ret)
		dev_warn_ratelimited(fg->dev, "RTC write interface not opened: %d\n", ret);
	return ret;
}

static void mt6359_fg_store_capacity(struct mt6359_fg *fg, int cap)
{
	unsigned int back = 0;

	cap = clamp(cap, 1, 100);	/* 0 reads as "nothing stored" */
	if (cap == fg->stored)
		return;
	fg->rtc_error = mt6359_fg_rtc_unlock(fg);
	if (fg->rtc_error)
		return;
	fg->rtc_error = regmap_update_bits(fg->regmap, MT6359_RTC_AL_HOU,
					   RTC_AL_HOU_FG_MASK,
					   FIELD_PREP(RTC_AL_HOU_FG_MASK,
						      RTC_FG_SOC_VALID | cap));
	if (!fg->rtc_error)
		fg->rtc_error = mt6359_fg_rtc_trigger(fg);
	if (fg->rtc_error)
		return;
	regmap_read(fg->regmap, MT6359_RTC_AL_HOU, &back);
	if (fg->stored < 0) {
		unsigned int bbpu = 0, core = 0;

		/*
		 * What reads back is the write buffer. A reload brings the
		 * registers of the RTC core into it (the vendor's rtc_reload):
		 * what reads then is what a restart will find.
		 */
		regmap_read(fg->regmap, MT6359_RTC_BBPU, &bbpu);
		regmap_write(fg->regmap, MT6359_RTC_BBPU,
			     (bbpu & 0xff) | RTC_BBPU_KEY | RTC_BBPU_RELOAD);
		mt6359_fg_rtc_trigger(fg);
		regmap_read(fg->regmap, MT6359_RTC_AL_HOU, &core);
		dev_info(fg->dev, "kept %d%% in the RTC: reads %#x, after a reload %#x (BBPU %#x)\n",
			 cap, back, core, bbpu);
	}
	fg->stored = cap;
}

static int mt6359_fg_charge_now(struct mt6359_fg *fg, s64 *uah)
{
	s64 car;
	int ret, cap, stored, uv = 0;

	if (!fg->anchored) {
		ret = mt6359_fg_ocv_capacity(fg, &cap);
		if (!ret)
			ret = mt6359_fg_ocv_uv(fg, &uv);
		/*
		 * What was last shown is the better start while the voltage
		 * does not contradict it. How far the two may lie apart is
		 * measured in volts, not in percent: in the flat middle of the
		 * curve 70 mV are twenty points, near its ends three (seen:
		 * 96 % kept from days before taken at 3.97 V, which is 66 %).
		 */
		if (!ret && !mt6359_fg_stored_capacity(fg, &stored) &&
		    abs(mt6359_fg_capacity_uv(fg, stored) - uv) <= FG_STORED_MAX_UV) {
			dev_info(fg->dev, "start from the stored %d%% (OCV says %d%%)\n",
				 stored, cap);
			fg->start_ocv = cap;
			fg->start_stored = true;
			cap = stored;
		} else if (!ret) {
			fg->start_ocv = cap;
			dev_info(fg->dev, "start from OCV at %d%% (the RTC keeps %#x)\n",
				 cap, fg->stored_raw);
		}
		if (!ret)
			ret = mt6359_fg_anchor(fg, cap);
		if (ret)
			return ret;
		fg->start_cap = cap;
	}
	ret = mt6359_fg_car_uah(fg, &car);
	if (ret)
		return ret;
	*uah = clamp_t(s64, fg->charge0_uah + car - fg->car0_uah, 0, fg->full_uah);
	return 0;
}

static int mt6359_fg_supplier_status(struct mt6359_fg *fg)
{
	union power_supply_propval val;

	if (power_supply_get_property_from_supplier(fg->psy,
						    POWER_SUPPLY_PROP_STATUS,
						    &val))
		return POWER_SUPPLY_STATUS_UNKNOWN;
	return val.intval;
}

static void mt6359_fg_work(struct work_struct *work)
{
	struct mt6359_fg *fg = container_of(work, struct mt6359_fg, work.work);

	int status = mt6359_fg_supplier_status(fg);
	int ocv_cap, cap, ua, avg, up, down;
	s64 uah;

	/* the charger ended the charge cycle: the cell is full */
	if (status == POWER_SUPPLY_STATUS_FULL) {
		mt6359_fg_anchor(fg, 100);
		fg->ocv_avg = 100 * 16;
	} else if (status != POWER_SUPPLY_STATUS_CHARGING &&
		   !mt6359_fg_charge_now(fg, &uah) &&
		   !mt6359_fg_ocv_capacity(fg, &ocv_cap) &&
		   ocv_cap < FG_LOW_OCV_CAPACITY) {
		/* 1 % per poll, so the reading never jumps */
		cap = div_s64(uah * 100, fg->full_uah);
		if (cap > ocv_cap + FG_LOW_MAX_ERROR)
			mt6359_fg_anchor(fg, cap - 1);
	} else if (!mt6359_fg_current_ua(fg, &ua) && abs(ua) <= FG_RELAXED_UA &&
		   !mt6359_fg_ocv_capacity(fg, &ocv_cap) &&
		   !mt6359_fg_charge_now(fg, &uah)) {
		/*
		 * Little current: the corrected voltage says where the charge
		 * stands. Smoothed, and heard only once it has settled.
		 */
		if (fg->ocv_samples++)
			fg->ocv_avg += (ocv_cap * 16 - fg->ocv_avg) / 4;
		else
			fg->ocv_avg = ocv_cap * 16;
		avg = (fg->ocv_avg + 8) / 16;
		cap = div_s64(uah * 100 + fg->full_uah / 2, fg->full_uah);
		up = down = FG_OCV_APART;
		if (ua > FG_STEADY_UA) {
			up = FG_OCV_WITH;
			down = FG_OCV_AGAINST;
		} else if (ua < -FG_STEADY_UA) {
			up = FG_OCV_AGAINST;
			down = FG_OCV_WITH;
		}
		if (fg->ocv_samples > FG_OCV_SETTLE) {
			if (avg - cap >= up)
				mt6359_fg_anchor(fg, min(cap + 1, 100));
			else if (cap - avg >= down)
				mt6359_fg_anchor(fg, max(cap - 1, 0));
		}
	}

	if (!mt6359_fg_charge_now(fg, &uah))
		mt6359_fg_store_capacity(fg, div_s64(uah * 100 + fg->full_uah / 2,
						     fg->full_uah));

	power_supply_changed(fg->psy);
	/* freezable: no wakeups (and no ADC reads) while the system sleeps */
	queue_delayed_work(system_freezable_wq, &fg->work, FG_POLL_INTERVAL);
}

static int mt6359_fg_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct mt6359_fg *fg = power_supply_get_drvdata(psy);
	s64 uah;
	int ret = 0, ua;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mt6359_fg_supplier_status(fg);
		if (val->intval == POWER_SUPPLY_STATUS_UNKNOWN) {
			ret = mt6359_fg_current_ua(fg, &ua);
			val->intval = ua > 0 ? POWER_SUPPLY_STATUS_CHARGING :
					       POWER_SUPPLY_STATUS_DISCHARGING;
		}
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = mt6359_fg_voltage_uv(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = fg->info->voltage_min_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = fg->info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = mt6359_fg_current_ua(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = fg->full_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = mt6359_fg_charge_now(fg, &uah);
		val->intval = uah;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = mt6359_fg_charge_now(fg, &uah);
		val->intval = div_s64(uah * 100 + fg->full_uah / 2, fg->full_uah);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static void mt6359_fg_external_power_changed(struct power_supply *psy)
{
	struct mt6359_fg *fg = power_supply_get_drvdata(psy);

	mod_delayed_work(system_freezable_wq, &fg->work, HZ);
}

static const enum power_supply_property mt6359_fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

static const struct power_supply_desc mt6359_fg_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6359_fg_props,
	.num_properties = ARRAY_SIZE(mt6359_fg_props),
	.get_property = mt6359_fg_get_property,
	.external_power_changed = mt6359_fg_external_power_changed,
};

static struct regmap *mt6359_fg_get_regmap(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pmic;
	struct regmap *map;

	np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!np)
		return ERR_PTR(-ENODEV);
	pmic = of_find_device_by_node(np);
	of_node_put(np);
	if (!pmic)
		return ERR_PTR(-EPROBE_DEFER);
	/* the MT6359 MFD uses the regmap of its parent, the PMIC wrapper */
	map = pmic->dev.parent ? dev_get_regmap(pmic->dev.parent, NULL) : NULL;
	put_device(&pmic->dev);
	return map ?: ERR_PTR(-EPROBE_DEFER);
}

/* Where the count began: "stored" or "ocv", the percent, and what the voltage said. */
static ssize_t soc_start_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct mt6359_fg *fg = dev_get_drvdata(dev);

	if (!fg || fg->start_cap < 0)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%s %d %d found=%#x kept=%d error=%d\n",
			  fg->start_stored ? "stored" : "ocv", fg->start_cap,
			  fg->start_ocv, fg->stored_raw, fg->stored, fg->rtc_error);
}
static DEVICE_ATTR_RO(soc_start);

static struct attribute *mt6359_fg_attrs[] = {
	&dev_attr_soc_start.attr,
	NULL
};
ATTRIBUTE_GROUPS(mt6359_fg);

static void mt6359_fg_cancel_work(void *data)
{
	cancel_delayed_work_sync(data);
}

static int mt6359_fg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct mt6359_fg *fg;
	unsigned int con0;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->dev = dev;
	fg->stored = -1;
	fg->start_cap = -1;
	platform_set_drvdata(pdev, fg);
	ret = devm_mutex_init(dev, &fg->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&fg->work, mt6359_fg_work);

	fg->regmap = mt6359_fg_get_regmap(dev);
	if (IS_ERR(fg->regmap))
		return dev_err_probe(dev, PTR_ERR(fg->regmap), "no PMIC regmap\n");

	ret = regmap_read(fg->regmap, MT6359_FGADC_CON0, &con0);
	if (ret)
		return ret;
	if (!(con0 & FGADC_CON0_FG_ON))
		return dev_err_probe(dev, -ENODEV, "FGADC not enabled by the bootloader\n");

	fg->vbat = devm_iio_channel_get(dev, "voltage");
	if (IS_ERR(fg->vbat))
		return dev_err_probe(dev, PTR_ERR(fg->vbat), "no voltage channel\n");

	fg->rsense_uohm = FG_DEFAULT_RSENSE_UOHM;
	of_property_read_u32(dev->of_node, "mediatek,sense-resistor-micro-ohms",
			     &fg->rsense_uohm);
	if (!fg->rsense_uohm)
		return -EINVAL;

	cfg.drv_data = fg;
	cfg.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &mt6359_fg_desc, &cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy), "register failed\n");

	ret = power_supply_get_battery_info(fg->psy, &fg->info);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	if (fg->info->charge_full_design_uah <= 0 || !fg->info->ocv_table_size[0])
		return dev_err_probe(dev, -EINVAL, "battery needs capacity and OCV table\n");
	fg->full_uah = fg->info->charge_full_design_uah;
	fg->rbat_uohm = fg->info->factory_internal_resistance_uohm > 0 ?
			fg->info->factory_internal_resistance_uohm :
			FG_DEFAULT_RBAT_UOHM;

	ret = devm_add_action_or_reset(dev, mt6359_fg_cancel_work, &fg->work);
	if (ret)
		return ret;
	queue_delayed_work(system_freezable_wq, &fg->work, 0);
	return 0;
}

static const struct of_device_id mt6359_fg_of_match[] = {
	{ .compatible = "mediatek,mt6359-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6359_fg_of_match);

static struct platform_driver mt6359_fg_driver = {
	.probe = mt6359_fg_probe,
	.driver = {
		.name = "mt6359-fuel-gauge",
		.of_match_table = mt6359_fg_of_match,
		.dev_groups = mt6359_fg_groups,
	},
};
module_platform_driver(mt6359_fg_driver);

MODULE_DESCRIPTION("MediaTek MT6359 fuel gauge (coulomb counter)");
MODULE_LICENSE("GPL");
