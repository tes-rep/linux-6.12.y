/*
 * Battery power supply driver for X-Powers AXP20X and AXP22X PMICs
 *
 * Copyright 2016 Free Electrons NextThing Co.
 *	Quentin Schulz <quentin.schulz@free-electrons.com>
 *
 * This driver is based on a previous upstreaming attempt by:
 *	Bruno Prémont <bonbons@linux-vserver.org>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/iio/iio.h>
#include <linux/iio/consumer.h>
#include <linux/mfd/axp20x.h>

#define AXP20X_PWR_STATUS_BAT_CHARGING	BIT(2)
#define AXP717_PWR_STATUS_MASK		GENMASK(6, 5)
#define AXP717_PWR_STATUS_BAT_STANDBY	0
#define AXP717_PWR_STATUS_BAT_CHRG	1
#define AXP717_PWR_STATUS_BAT_DISCHRG	2

#define AXP20X_PWR_OP_BATT_PRESENT	BIT(5)
#define AXP20X_PWR_OP_BATT_ACTIVATED	BIT(3)
#define AXP717_PWR_OP_BATT_PRESENT	BIT(3)

#define AXP717_BATT_PMU_FAULT_MASK	GENMASK(2, 0)
#define AXP717_BATT_UVLO_2_5V		BIT(2)
#define AXP717_BATT_OVER_TEMP		BIT(1)
#define AXP717_BATT_UNDER_TEMP		BIT(0)

#define AXP209_FG_PERCENT		GENMASK(6, 0)
#define AXP22X_FG_VALID			BIT(7)

#define AXP20X_CHRG_CTRL1_ENABLE	BIT(7)
#define AXP20X_CHRG_CTRL1_TGT_VOLT	GENMASK(6, 5)
#define AXP20X_CHRG_CTRL1_TGT_4_1V	(0 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_15V	(1 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_2V	(2 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_36V	(3 << 5)

#define AXP22X_CHRG_CTRL1_TGT_4_22V	(1 << 5)
#define AXP22X_CHRG_CTRL1_TGT_4_24V	(3 << 5)

#define AXP717_CHRG_ENABLE		BIT(1)
#define AXP717_CHRG_CV_VOLT_MASK	GENMASK(2, 0)
#define AXP717_CHRG_CV_4_0V		0
#define AXP717_CHRG_CV_4_1V		1
#define AXP717_CHRG_CV_4_2V		2
#define AXP717_CHRG_CV_4_35V		3
#define AXP717_CHRG_CV_4_4V		4
/* Values 5 and 6 reserved. */
#define AXP717_CHRG_CV_5_0V		7

#define AXP813_CHRG_CTRL1_TGT_4_35V	(3 << 5)

#define AXP20X_CHRG_CTRL1_TGT_CURR	GENMASK(3, 0)
#define AXP717_ICC_CHARGER_LIM_MASK	GENMASK(5, 0)

#define AXP717_ITERM_CHG_LIM_MASK	GENMASK(3, 0)
#define AXP717_ITERM_CC_STEP		64000

#define AXP20X_V_OFF_MASK		GENMASK(2, 0)
#define AXP717_V_OFF_MASK		GENMASK(6, 4)

#define AXP717_BAT_VMIN_MIN_UV		2600000
#define AXP717_BAT_VMIN_MAX_UV		3300000
#define AXP717_BAT_VMIN_STEP		100000
#define AXP717_BAT_CV_MIN_UV		4000000
#define AXP717_BAT_CV_MAX_UV		5000000
#define AXP717_BAT_CC_MIN_UA		0
#define AXP717_BAT_CC_MAX_UA		3008000

#define AXP717_TS_PIN_DISABLE		BIT(4)

struct axp20x_batt_ps;

struct axp_data {
	int		ccc_scale;
	int		ccc_offset;
	unsigned int	ccc_reg;
	unsigned int	ccc_mask;
	bool		has_fg_valid;
	const struct	power_supply_desc *bat_ps_desc;
	int	(*get_max_voltage)(struct axp20x_batt_ps *batt, int *val);
	int	(*set_max_voltage)(struct axp20x_batt_ps *batt, int val);
	int	(*cfg_iio_chan)(struct platform_device *pdev,
				struct axp20x_batt_ps *axp_batt);
	void	(*set_bat_info)(struct platform_device *pdev,
				struct axp20x_batt_ps *axp_batt,
				struct power_supply_battery_info *info);
};

struct axp20x_batt_ps {
	struct regmap *regmap;
	struct power_supply *batt;
	struct device *dev;
	struct iio_channel *batt_chrg_i;
	struct iio_channel *batt_dischrg_i;
	struct iio_channel *batt_v;
	/* Maximum constant charge current */
	unsigned int max_ccc;
	const struct axp_data	*data;
	bool ts_disable;
};

static int axp20x_battery_get_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int *val)
{
	int ret, reg;

	ret = regmap_read(axp20x_batt->regmap, AXP20X_CHRG_CTRL1, &reg);
	if (ret)
		return ret;

	switch (reg & AXP20X_CHRG_CTRL1_TGT_VOLT) {
	case AXP20X_CHRG_CTRL1_TGT_4_1V:
		*val = 4100000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_15V:
		*val = 4150000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_2V:
		*val = 4200000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_36V:
		*val = 4360000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int axp22x_battery_get_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int *val)
{
	int ret, reg;

	ret = regmap_read(axp20x_batt->regmap, AXP20X_CHRG_CTRL1, &reg);
	if (ret)
		return ret;

	switch (reg & AXP20X_CHRG_CTRL1_TGT_VOLT) {
	case AXP20X_CHRG_CTRL1_TGT_4_1V:
		*val = 4100000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_2V:
		*val = 4200000;
		break;
	case AXP22X_CHRG_CTRL1_TGT_4_22V:
		*val = 4220000;
		break;
	case AXP22X_CHRG_CTRL1_TGT_4_24V:
		*val = 4240000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int axp717_battery_get_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int *val)
{
	int ret, reg;

	ret = regmap_read(axp20x_batt->regmap, AXP717_CV_CHG_SET, &reg);
	if (ret)
		return ret;

	switch (reg & AXP717_CHRG_CV_VOLT_MASK) {
	case AXP717_CHRG_CV_4_0V:
		*val = 4000000;
		return 0;
	case AXP717_CHRG_CV_4_1V:
		*val = 4100000;
		return 0;
	case AXP717_CHRG_CV_4_2V:
		*val = 4200000;
		return 0;
	case AXP717_CHRG_CV_4_35V:
		*val = 4350000;
		return 0;
	case AXP717_CHRG_CV_4_4V:
		*val = 4400000;
		return 0;
	case AXP717_CHRG_CV_5_0V:
		*val = 5000000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int axp813_battery_get_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int *val)
{
	int ret, reg;

	ret = regmap_read(axp20x_batt->regmap, AXP20X_CHRG_CTRL1, &reg);
	if (ret)
		return ret;

	switch (reg & AXP20X_CHRG_CTRL1_TGT_VOLT) {
	case AXP20X_CHRG_CTRL1_TGT_4_1V:
		*val = 4100000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_15V:
		*val = 4150000;
		break;
	case AXP20X_CHRG_CTRL1_TGT_4_2V:
		*val = 4200000;
		break;
	case AXP813_CHRG_CTRL1_TGT_4_35V:
		*val = 4350000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int axp20x_get_constant_charge_current(struct axp20x_batt_ps *axp,
					      int *val)
{
	int ret;

	ret = regmap_read(axp->regmap, AXP20X_CHRG_CTRL1, val);
	if (ret)
		return ret;

	*val &= AXP20X_CHRG_CTRL1_TGT_CURR;

	*val = *val * axp->data->ccc_scale + axp->data->ccc_offset;

	return 0;
}

static int axp717_get_constant_charge_current(struct axp20x_batt_ps *axp,
					      int *val)
{
	int ret;

	ret = regmap_read(axp->regmap, AXP717_ICC_CHG_SET, val);
	if (ret)
		return ret;

	*val = FIELD_GET(AXP717_ICC_CHARGER_LIM_MASK, *val) *
		axp->data->ccc_scale;

	return 0;
}

static int axp20x_battery_get_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct axp20x_batt_ps *axp20x_batt = power_supply_get_drvdata(psy);
	int ret = 0, reg, val1;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(axp20x_batt->regmap, AXP20X_PWR_OP_MODE,
				  &reg);
		if (ret)
			return ret;

		val->intval = !!(reg & AXP20X_PWR_OP_BATT_PRESENT);
		break;

	case POWER_SUPPLY_PROP_STATUS:
		ret = regmap_read(axp20x_batt->regmap, AXP20X_PWR_INPUT_STATUS,
				  &reg);
		if (ret)
			return ret;

		if (reg & AXP20X_PWR_STATUS_BAT_CHARGING) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}

		ret = iio_read_channel_processed(axp20x_batt->batt_dischrg_i,
						 &val1);
		if (ret)
			return ret;

		if (val1) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			return 0;
		}

		ret = regmap_read(axp20x_batt->regmap, AXP20X_FG_RES, &val1);
		if (ret)
			return ret;

		/*
		 * Fuel Gauge data takes 7 bits but the stored value seems to be
		 * directly the raw percentage without any scaling to 7 bits.
		 */
		if ((val1 & AXP209_FG_PERCENT) == 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		ret = regmap_read(axp20x_batt->regmap, AXP20X_PWR_OP_MODE,
				  &val1);
		if (ret)
			return ret;

		if (val1 & AXP20X_PWR_OP_BATT_ACTIVATED) {
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
			return 0;
		}

		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = axp20x_get_constant_charge_current(axp20x_batt,
							 &val->intval);
		if (ret)
			return ret;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = axp20x_batt->max_ccc;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = regmap_read(axp20x_batt->regmap, AXP20X_PWR_INPUT_STATUS,
				  &reg);
		if (ret)
			return ret;

		if (reg & AXP20X_PWR_STATUS_BAT_CHARGING) {
			ret = iio_read_channel_processed(axp20x_batt->batt_chrg_i, &val->intval);
		} else {
			ret = iio_read_channel_processed(axp20x_batt->batt_dischrg_i, &val1);
			val->intval = -val1;
		}
		if (ret)
			return ret;

		/* IIO framework gives mA but Power Supply framework gives uA */
		val->intval *= 1000;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		/* When no battery is present, return capacity is 100% */
		ret = regmap_read(axp20x_batt->regmap, AXP20X_PWR_OP_MODE,
				  &reg);
		if (ret)
			return ret;

		if (!(reg & AXP20X_PWR_OP_BATT_PRESENT)) {
			val->intval = 100;
			return 0;
		}

		ret = regmap_read(axp20x_batt->regmap, AXP20X_FG_RES, &reg);
		if (ret)
			return ret;

		if (axp20x_batt->data->has_fg_valid && !(reg & AXP22X_FG_VALID))
			return -EINVAL;

		/*
		 * Fuel Gauge data takes 7 bits but the stored value seems to be
		 * directly the raw percentage without any scaling to 7 bits.
		 */
		val->intval = reg & AXP209_FG_PERCENT;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return axp20x_batt->data->get_max_voltage(axp20x_batt,
							  &val->intval);

	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		ret = regmap_read(axp20x_batt->regmap, AXP20X_V_OFF, &reg);
		if (ret)
			return ret;

		val->intval = 2600000 + 100000 * (reg & AXP20X_V_OFF_MASK);
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = iio_read_channel_processed(axp20x_batt->batt_v,
						 &val->intval);
		if (ret)
			return ret;

		/* IIO framework gives mV but Power Supply framework gives uV */
		val->intval *= 1000;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int axp717_battery_get_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct axp20x_batt_ps *axp20x_batt = power_supply_get_drvdata(psy);
	int ret = 0, reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(axp20x_batt->regmap, AXP717_ON_INDICATE,
				  &reg);
		if (ret)
			return ret;

		val->intval = FIELD_GET(AXP717_PWR_OP_BATT_PRESENT, reg);
		return 0;

	case POWER_SUPPLY_PROP_STATUS:
		ret = regmap_read(axp20x_batt->regmap, AXP717_PMU_STATUS_2,
				  &reg);
		if (ret)
			return ret;

		switch (FIELD_GET(AXP717_PWR_STATUS_MASK, reg)) {
		case AXP717_PWR_STATUS_BAT_STANDBY:
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			return 0;

		case AXP717_PWR_STATUS_BAT_CHRG:
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;

		case AXP717_PWR_STATUS_BAT_DISCHRG:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			return 0;

		default:
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			return 0;
		}

	/*
	 * If a fault is detected it must also be cleared; if the
	 * condition persists it should reappear. A restart was not
	 * sufficient to clear the bit in testing despite the register
	 * listed as POR.
	 */
	case POWER_SUPPLY_PROP_HEALTH:
		ret = regmap_read(axp20x_batt->regmap, AXP717_PMU_FAULT,
				  &reg);
		if (ret)
			return ret;

		switch (reg & AXP717_BATT_PMU_FAULT_MASK) {
		case AXP717_BATT_UVLO_2_5V:
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
			regmap_write_bits(axp20x_batt->regmap,
					  AXP717_PMU_FAULT,
					  AXP717_BATT_UVLO_2_5V,
					  AXP717_BATT_UVLO_2_5V);
			return 0;

		case AXP717_BATT_OVER_TEMP:
			val->intval = POWER_SUPPLY_HEALTH_HOT;
			regmap_write_bits(axp20x_batt->regmap,
					  AXP717_PMU_FAULT,
					  AXP717_BATT_OVER_TEMP,
					  AXP717_BATT_OVER_TEMP);
			return 0;

		case AXP717_BATT_UNDER_TEMP:
			val->intval = POWER_SUPPLY_HEALTH_COLD;
			regmap_write_bits(axp20x_batt->regmap,
					  AXP717_PMU_FAULT,
					  AXP717_BATT_UNDER_TEMP,
					  AXP717_BATT_UNDER_TEMP);
			return 0;

		default:
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
			return 0;
		}

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = axp717_get_constant_charge_current(axp20x_batt,
							 &val->intval);
		if (ret)
			return ret;
		return 0;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/*
		 * The offset of this value is currently unknown and is
		 * not documented in the datasheet. Based on
		 * observation it's assumed to be somewhere around
		 * 450ma. I will leave the value raw for now.
		 */
		ret = iio_read_channel_processed(axp20x_batt->batt_chrg_i, &val->intval);
		if (ret)
			return ret;
		/* IIO framework gives mA but Power Supply framework gives uA */
		val->intval *= 1000;
		return 0;

	case POWER_SUPPLY_PROP_CAPACITY:
		ret = regmap_read(axp20x_batt->regmap, AXP717_ON_INDICATE,
				  &reg);
		if (ret)
			return ret;

		if (!FIELD_GET(AXP717_PWR_OP_BATT_PRESENT, reg))
			return -ENODEV;

		ret = regmap_read(axp20x_batt->regmap,
				  AXP717_BATT_PERCENT_DATA, &reg);
		if (ret)
			return ret;

		/*
		 * Fuel Gauge data takes 7 bits but the stored value seems to be
		 * directly the raw percentage without any scaling to 7 bits.
		 */
		val->intval = reg & AXP209_FG_PERCENT;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return axp20x_batt->data->get_max_voltage(axp20x_batt,
							  &val->intval);

	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		ret = regmap_read(axp20x_batt->regmap,
				  AXP717_VSYS_V_POWEROFF, &reg);
		if (ret)
			return ret;

		val->intval = AXP717_BAT_VMIN_MIN_UV + AXP717_BAT_VMIN_STEP *
			(reg & AXP717_V_OFF_MASK);
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = iio_read_channel_processed(axp20x_batt->batt_v,
						 &val->intval);
		if (ret)
			return ret;

		/* IIO framework gives mV but Power Supply framework gives uV */
		val->intval *= 1000;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = regmap_read(axp20x_batt->regmap,
				  AXP717_ITERM_CHG_SET, &reg);
		if (ret)
			return ret;

		val->intval = (reg & AXP717_ITERM_CHG_LIM_MASK) * AXP717_ITERM_CC_STEP;
		return 0;

	default:
		return -EINVAL;
	}
}

static int axp22x_battery_set_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int val)
{
	switch (val) {
	case 4100000:
		val = AXP20X_CHRG_CTRL1_TGT_4_1V;
		break;

	case 4200000:
		val = AXP20X_CHRG_CTRL1_TGT_4_2V;
		break;

	default:
		/*
		 * AXP20x max voltage can be set to 4.36V and AXP22X max voltage
		 * can be set to 4.22V and 4.24V, but these voltages are too
		 * high for Lithium based batteries (AXP PMICs are supposed to
		 * be used with these kinds of battery).
		 */
		return -EINVAL;
	}

	return regmap_update_bits(axp20x_batt->regmap, AXP20X_CHRG_CTRL1,
				  AXP20X_CHRG_CTRL1_TGT_VOLT, val);
}

static int axp20x_battery_set_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int val)
{
	switch (val) {
	case 4100000:
		val = AXP20X_CHRG_CTRL1_TGT_4_1V;
		break;

	case 4150000:
		val = AXP20X_CHRG_CTRL1_TGT_4_15V;
		break;

	case 4200000:
		val = AXP20X_CHRG_CTRL1_TGT_4_2V;
		break;

	default:
		/*
		 * AXP20x max voltage can be set to 4.36V and AXP22X max voltage
		 * can be set to 4.22V and 4.24V, but these voltages are too
		 * high for Lithium based batteries (AXP PMICs are supposed to
		 * be used with these kinds of battery).
		 */
		return -EINVAL;
	}

	return regmap_update_bits(axp20x_batt->regmap, AXP20X_CHRG_CTRL1,
				  AXP20X_CHRG_CTRL1_TGT_VOLT, val);
}

static int axp717_battery_set_max_voltage(struct axp20x_batt_ps *axp20x_batt,
					  int val)
{
	switch (val) {
	case 4000000:
		val = AXP717_CHRG_CV_4_0V;
		break;

	case 4100000:
		val = AXP717_CHRG_CV_4_1V;
		break;

	case 4200000:
		val = AXP717_CHRG_CV_4_2V;
		break;

	default:
		/*
		 * AXP717 can go up to 4.35, 4.4, and 5.0 volts which
		 * seem too high for lithium batteries, so do not allow.
		 */
		return -EINVAL;
	}

	return regmap_update_bits(axp20x_batt->regmap,
				  AXP717_CV_CHG_SET,
				  AXP717_CHRG_CV_VOLT_MASK, val);
}

static int axp20x_set_constant_charge_current(struct axp20x_batt_ps *axp_batt,
					      int charge_current)
{
	if (charge_current > axp_batt->max_ccc)
		return -EINVAL;

	charge_current = (charge_current - axp_batt->data->ccc_offset) /
		axp_batt->data->ccc_scale;

	if (charge_current > AXP20X_CHRG_CTRL1_TGT_CURR || charge_current < 0)
		return -EINVAL;

	return regmap_update_bits(axp_batt->regmap, AXP20X_CHRG_CTRL1,
				  AXP20X_CHRG_CTRL1_TGT_CURR, charge_current);
}

static int axp717_set_constant_charge_current(struct axp20x_batt_ps *axp,
					      int charge_current)
{
	int val;

	if (charge_current > axp->max_ccc)
		return -EINVAL;

	if (charge_current > AXP717_BAT_CC_MAX_UA || charge_current < 0)
		return -EINVAL;

	val = (charge_current - axp->data->ccc_offset) /
		axp->data->ccc_scale;

	return regmap_update_bits(axp->regmap, AXP717_ICC_CHG_SET,
				  AXP717_ICC_CHARGER_LIM_MASK, val);
}

static int axp20x_set_max_constant_charge_current(struct axp20x_batt_ps *axp,
						  int charge_current)
{
	bool lower_max = false;

	charge_current = (charge_current - axp->data->ccc_offset) /
		axp->data->ccc_scale;

	if (charge_current > AXP20X_CHRG_CTRL1_TGT_CURR || charge_current < 0)
		return -EINVAL;

	charge_current = charge_current * axp->data->ccc_scale +
		axp->data->ccc_offset;

	if (charge_current > axp->max_ccc)
		dev_warn(axp->dev,
			 "Setting max constant charge current higher than previously defined. Note that increasing the constant charge current may damage your battery.\n");
	else
		lower_max = true;

	axp->max_ccc = charge_current;

	if (lower_max) {
		int current_cc;

		axp20x_get_constant_charge_current(axp, &current_cc);
		if (current_cc > charge_current)
			axp20x_set_constant_charge_current(axp, charge_current);
	}

	return 0;
}
static int axp20x_set_voltage_min_design(struct axp20x_batt_ps *axp_batt,
					 int min_voltage)
{
	int val1 = (min_voltage - 2600000) / 100000;

	if (val1 < 0 || val1 > AXP20X_V_OFF_MASK)
		return -EINVAL;

	return regmap_update_bits(axp_batt->regmap, AXP20X_V_OFF,
				  AXP20X_V_OFF_MASK, val1);
}

static int axp717_set_voltage_min_design(struct axp20x_batt_ps *axp_batt,
					 int min_voltage)
{
	int val1 = (min_voltage - AXP717_BAT_VMIN_MIN_UV) / AXP717_BAT_VMIN_STEP;

	if (val1 < 0 || val1 > AXP717_V_OFF_MASK)
		return -EINVAL;

	return regmap_update_bits(axp_batt->regmap,
				  AXP717_VSYS_V_POWEROFF,
				  AXP717_V_OFF_MASK, val1);
}

static int axp20x_battery_set_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct axp20x_batt_ps *axp20x_batt = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		return axp20x_set_voltage_min_design(axp20x_batt, val->intval);

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return axp20x_batt->data->set_max_voltage(axp20x_batt, val->intval);

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return axp20x_set_constant_charge_current(axp20x_batt,
							  val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		return axp20x_set_max_constant_charge_current(axp20x_batt,
							      val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		switch (val->intval) {
		case POWER_SUPPLY_STATUS_CHARGING:
			return regmap_update_bits(axp20x_batt->regmap, AXP20X_CHRG_CTRL1,
				AXP20X_CHRG_CTRL1_ENABLE, AXP20X_CHRG_CTRL1_ENABLE);

		case POWER_SUPPLY_STATUS_DISCHARGING:
		case POWER_SUPPLY_STATUS_NOT_CHARGING:
			return regmap_update_bits(axp20x_batt->regmap, AXP20X_CHRG_CTRL1,
				AXP20X_CHRG_CTRL1_ENABLE, 0);
		}
		fallthrough;
	default:
		return -EINVAL;
	}
}

static int axp717_battery_set_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct axp20x_batt_ps *axp20x_batt = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MIN:
		return axp717_set_voltage_min_design(axp20x_batt, val->intval);

	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return axp20x_batt->data->set_max_voltage(axp20x_batt, val->intval);

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		return axp717_set_constant_charge_current(axp20x_batt,
							  val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		switch (val->intval) {
		case POWER_SUPPLY_STATUS_CHARGING:
			return regmap_update_bits(axp20x_batt->regmap,
						  AXP717_MODULE_EN_CONTROL_2,
						  AXP717_CHRG_ENABLE,
						  AXP717_CHRG_ENABLE);

		case POWER_SUPPLY_STATUS_DISCHARGING:
		case POWER_SUPPLY_STATUS_NOT_CHARGING:
			return regmap_update_bits(axp20x_batt->regmap,
						  AXP717_MODULE_EN_CONTROL_2,
						  AXP717_CHRG_ENABLE, 0);
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property axp20x_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_CAPACITY,
};

static enum power_supply_property axp717_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MIN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
};

static int axp20x_battery_prop_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_STATUS ||
	       psp == POWER_SUPPLY_PROP_VOLTAGE_MIN ||
	       psp == POWER_SUPPLY_PROP_VOLTAGE_MAX ||
	       psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
	       psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX;
}

static int axp717_battery_prop_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_STATUS ||
	       psp == POWER_SUPPLY_PROP_VOLTAGE_MIN ||
	       psp == POWER_SUPPLY_PROP_VOLTAGE_MAX ||
	       psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX;
}

static const struct power_supply_desc axp209_batt_ps_desc = {
	.name = "axp20x-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp20x_battery_props,
	.num_properties = ARRAY_SIZE(axp20x_battery_props),
	.property_is_writeable = axp20x_battery_prop_writeable,
	.get_property = axp20x_battery_get_prop,
	.set_property = axp20x_battery_set_prop,
};

static const struct power_supply_desc axp717_batt_ps_desc = {
	.name = "axp20x-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp717_battery_props,
	.num_properties = ARRAY_SIZE(axp717_battery_props),
	.property_is_writeable = axp717_battery_prop_writeable,
	.get_property = axp717_battery_get_prop,
	.set_property = axp717_battery_set_prop,
};

static int axp209_bat_cfg_iio_channels(struct platform_device *pdev,
				       struct axp20x_batt_ps *axp_batt)
{
	axp_batt->batt_v = devm_iio_channel_get(&pdev->dev, "batt_v");
	if (IS_ERR(axp_batt->batt_v)) {
		if (PTR_ERR(axp_batt->batt_v) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(axp_batt->batt_v);
	}

	axp_batt->batt_chrg_i = devm_iio_channel_get(&pdev->dev,
							"batt_chrg_i");
	if (IS_ERR(axp_batt->batt_chrg_i)) {
		if (PTR_ERR(axp_batt->batt_chrg_i) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(axp_batt->batt_chrg_i);
	}

	axp_batt->batt_dischrg_i = devm_iio_channel_get(&pdev->dev,
							   "batt_dischrg_i");
	if (IS_ERR(axp_batt->batt_dischrg_i)) {
		if (PTR_ERR(axp_batt->batt_dischrg_i) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(axp_batt->batt_dischrg_i);
	}

	return 0;
}

static int axp717_bat_cfg_iio_channels(struct platform_device *pdev,
				       struct axp20x_batt_ps *axp_batt)
{
	axp_batt->batt_v = devm_iio_channel_get(&pdev->dev, "batt_v");
	if (IS_ERR(axp_batt->batt_v)) {
		if (PTR_ERR(axp_batt->batt_v) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(axp_batt->batt_v);
	}

	axp_batt->batt_chrg_i = devm_iio_channel_get(&pdev->dev,
							"batt_chrg_i");
	if (IS_ERR(axp_batt->batt_chrg_i)) {
		if (PTR_ERR(axp_batt->batt_chrg_i) == -ENODEV)
			return -EPROBE_DEFER;
		return PTR_ERR(axp_batt->batt_chrg_i);
	}

	return 0;
}

static void axp209_set_battery_info(struct platform_device *pdev,
				    struct axp20x_batt_ps *axp_batt,
				    struct power_supply_battery_info *info)
{
	int vmin = info->voltage_min_design_uv;
	int ccc = info->constant_charge_current_max_ua;

	if (vmin > 0 && axp20x_set_voltage_min_design(axp_batt, vmin))
		dev_err(&pdev->dev,
			"couldn't set voltage_min_design\n");

	/* Set max to unverified value to be able to set CCC */
	axp_batt->max_ccc = ccc;

	if (ccc <= 0 || axp20x_set_constant_charge_current(axp_batt, ccc)) {
		dev_err(&pdev->dev,
			"couldn't set ccc from DT: fallback to min value\n");
		ccc = 300000;
		axp_batt->max_ccc = ccc;
		axp20x_set_constant_charge_current(axp_batt, ccc);
	}
}

static void axp717_set_battery_info(struct platform_device *pdev,
				    struct axp20x_batt_ps *axp_batt,
				    struct power_supply_battery_info *info)
{
	int vmin = info->voltage_min_design_uv;
	int vmax = info->voltage_max_design_uv;
	int ccc = info->constant_charge_current_max_ua;
	int val;

	axp_batt->ts_disable = (device_property_read_bool(axp_batt->dev,
							  "x-powers,no-thermistor"));

	/*
	 * Under rare conditions an incorrectly programmed efuse for
	 * the temp sensor on the PMIC may trigger a fault condition.
	 * Allow users to hard-code if the ts pin is not used to work
	 * around this problem. Note that this requires the battery
	 * be correctly defined in the device tree with a monitored
	 * battery node.
	 */
	if (axp_batt->ts_disable) {
		regmap_update_bits(axp_batt->regmap,
				   AXP717_TS_PIN_CFG,
				   AXP717_TS_PIN_DISABLE,
				   AXP717_TS_PIN_DISABLE);
	}

	if (vmin > 0 && axp717_set_voltage_min_design(axp_batt, vmin))
		dev_err(&pdev->dev,
			"couldn't set voltage_min_design\n");

	if (vmax > 0 && axp717_battery_set_max_voltage(axp_batt, vmax))
		dev_err(&pdev->dev,
			"couldn't set voltage_max_design\n");

	axp717_get_constant_charge_current(axp_batt, &val);
	axp_batt->max_ccc = ccc;
	if (ccc <= 0 || axp717_set_constant_charge_current(axp_batt, ccc)) {
		dev_err(&pdev->dev,
			"couldn't set ccc from DT: current ccc is %d\n",
			val);
	}
}

static const struct axp_data axp209_data = {
	.ccc_scale = 100000,
	.ccc_offset = 300000,
	.ccc_reg = AXP20X_CHRG_CTRL1,
	.ccc_mask = AXP20X_CHRG_CTRL1_TGT_CURR,
	.bat_ps_desc = &axp209_batt_ps_desc,
	.get_max_voltage = axp20x_battery_get_max_voltage,
	.set_max_voltage = axp20x_battery_set_max_voltage,
	.cfg_iio_chan = axp209_bat_cfg_iio_channels,
	.set_bat_info = axp209_set_battery_info,
};

static const struct axp_data axp221_data = {
	.ccc_scale = 150000,
	.ccc_offset = 300000,
	.ccc_reg = AXP20X_CHRG_CTRL1,
	.ccc_mask = AXP20X_CHRG_CTRL1_TGT_CURR,
	.has_fg_valid = true,
	.bat_ps_desc = &axp209_batt_ps_desc,
	.get_max_voltage = axp22x_battery_get_max_voltage,
	.set_max_voltage = axp22x_battery_set_max_voltage,
	.cfg_iio_chan = axp209_bat_cfg_iio_channels,
	.set_bat_info = axp209_set_battery_info,
};

static const struct axp_data axp717_data = {
	.ccc_scale = 64000,
	.ccc_offset = 0,
	.ccc_reg = AXP717_ICC_CHG_SET,
	.ccc_mask = AXP717_ICC_CHARGER_LIM_MASK,
	.bat_ps_desc = &axp717_batt_ps_desc,
	.get_max_voltage = axp717_battery_get_max_voltage,
	.set_max_voltage = axp717_battery_set_max_voltage,
	.cfg_iio_chan = axp717_bat_cfg_iio_channels,
	.set_bat_info = axp717_set_battery_info,
};

static const struct axp_data axp813_data = {
	.ccc_scale = 200000,
	.ccc_offset = 200000,
	.ccc_reg = AXP20X_CHRG_CTRL1,
	.ccc_mask = AXP20X_CHRG_CTRL1_TGT_CURR,
	.has_fg_valid = true,
	.bat_ps_desc = &axp209_batt_ps_desc,
	.get_max_voltage = axp813_battery_get_max_voltage,
	.set_max_voltage = axp20x_battery_set_max_voltage,
	.cfg_iio_chan = axp209_bat_cfg_iio_channels,
	.set_bat_info = axp209_set_battery_info,
};

static const struct of_device_id axp20x_battery_ps_id[] = {
	{
		.compatible = "x-powers,axp209-battery-power-supply",
		.data = (void *)&axp209_data,
	}, {
		.compatible = "x-powers,axp221-battery-power-supply",
		.data = (void *)&axp221_data,
	}, {
		.compatible = "x-powers,axp717-battery-power-supply",
		.data = (void *)&axp717_data,
	}, {
		.compatible = "x-powers,axp813-battery-power-supply",
		.data = (void *)&axp813_data,
	}, { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, axp20x_battery_ps_id);

static int axp20x_power_probe(struct platform_device *pdev)
{
	struct axp20x_batt_ps *axp20x_batt;
	struct power_supply_config psy_cfg = {};
	struct power_supply_battery_info *info;
	struct device *dev = &pdev->dev;
	int ret;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	axp20x_batt = devm_kzalloc(&pdev->dev, sizeof(*axp20x_batt),
				   GFP_KERNEL);
	if (!axp20x_batt)
		return -ENOMEM;

	axp20x_batt->dev = &pdev->dev;

	axp20x_batt->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	platform_set_drvdata(pdev, axp20x_batt);

	psy_cfg.drv_data = axp20x_batt;
	psy_cfg.of_node = pdev->dev.of_node;

	axp20x_batt->data = (struct axp_data *)of_device_get_match_data(dev);

	ret = axp20x_batt->data->cfg_iio_chan(pdev, axp20x_batt);
	if (ret)
		return ret;

	axp20x_batt->batt = devm_power_supply_register(&pdev->dev,
						       axp20x_batt->data->bat_ps_desc,
						       &psy_cfg);
	if (IS_ERR(axp20x_batt->batt)) {
		dev_err(&pdev->dev, "failed to register power supply: %ld\n",
			PTR_ERR(axp20x_batt->batt));
		return PTR_ERR(axp20x_batt->batt);
	}

	if (!power_supply_get_battery_info(axp20x_batt->batt, &info)) {
		axp20x_batt->data->set_bat_info(pdev, axp20x_batt, info);
		power_supply_put_battery_info(axp20x_batt->batt, info);
	}

	/*
	 * Update max CCC to a valid value if battery info is present or set it
	 * to current register value by default.
	 */
	axp20x_get_constant_charge_current(axp20x_batt, &axp20x_batt->max_ccc);

	return 0;
}

static struct platform_driver axp20x_batt_driver = {
	.probe    = axp20x_power_probe,
	.driver   = {
		.name  = "axp20x-battery-power-supply",
		.of_match_table = axp20x_battery_ps_id,
	},
};

module_platform_driver(axp20x_batt_driver);

MODULE_DESCRIPTION("Battery power supply driver for AXP20X and AXP22X PMICs");
MODULE_AUTHOR("Quentin Schulz <quentin.schulz@free-electrons.com>");
MODULE_LICENSE("GPL");
