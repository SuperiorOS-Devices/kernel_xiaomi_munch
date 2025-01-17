// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2011, 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/unaligned.h>

/* RTC_CTRL register bit fields */
#define PM8xxx_RTC_ENABLE		BIT(7)
#define PM8xxx_RTC_ALARM_CLEAR		BIT(0)
#define PM8xxx_RTC_ALARM_ENABLE		BIT(7)
#define NUM_8_BIT_RTC_REGS		0x4

/**
 * struct pm8xxx_rtc_regs - describe RTC registers per PMIC versions
 * @ctrl: base address of control register
 * @write: base address of write register
 * @read: base address of read register
 * @alarm_ctrl: base address of alarm control register
 * @alarm_ctrl2: base address of alarm control2 register
 * @alarm_rw: base address of alarm read-write register
 * @alarm_en: alarm enable mask
 */
struct pm8xxx_rtc_regs {
	unsigned int ctrl;
	unsigned int write;
	unsigned int read;
	unsigned int alarm_ctrl;
	unsigned int alarm_ctrl2;
	unsigned int alarm_rw;
	unsigned int alarm_en;
};

/**
 * struct pm8xxx_rtc -  rtc driver internal structure
 * @rtc:		rtc device for this driver.
 * @regmap:		regmap used to access RTC registers
 * @allow_set_time:	indicates whether writing to the RTC is allowed
 * @rtc_alarm_irq:	rtc alarm irq number.
 * @ctrl_reg:		rtc control register.
 * @rtc_dev:		device structure.
 */
struct pm8xxx_rtc {
	struct rtc_device *rtc;
	struct regmap *regmap;
	bool allow_set_time;
	int rtc_alarm_irq;
	const struct pm8xxx_rtc_regs *regs;
	struct device *rtc_dev;
};

static int pm8xxx_rtc_read_rtc_data(struct pm8xxx_rtc *rtc_dd, unsigned long *rtc_data)
{
	int rc;
	u8 value[NUM_8_BIT_RTC_REGS];
	unsigned int reg;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = regmap_bulk_read(rtc_dd->regmap, regs->read, value, sizeof(value));
	if (rc)
		return rc;

	/*
	 * Read the LSB again and check if there has been a carry over.
	 * If there is, redo the read operation.
	 */
	rc = regmap_read(rtc_dd->regmap, regs->read, &reg);
	if (rc < 0)
		return rc;

	if (unlikely(reg < value[0])) {
		rc = regmap_bulk_read(rtc_dd->regmap, regs->read,
				      value, sizeof(value));
		if (rc)
			return rc;
	}

	*rtc_data = get_unaligned_le32(value);

	return 0;
}

static int pm8xxx_rtc_read_alarm_data(struct pm8xxx_rtc *rtc_dd, unsigned long *alarm_data)
{
	int rc;
	u8 value[NUM_8_BIT_RTC_REGS];

	rc = regmap_bulk_read(rtc_dd->regmap, rtc_dd->regs->alarm_rw, value,
			      sizeof(value));
	if (rc)
		return rc;

	*alarm_data = get_unaligned_le32(value);

	return 0;
}

/*
 * Steps to write the RTC registers.
 * 1. Disable alarm if enabled.
 * 2. Disable rtc if enabled.
 * 3. Write 0x00 to LSB.
 * 4. Write Byte[1], Byte[2], Byte[3] then Byte[0].
 * 5. Enable rtc if disabled in step 2.
 * 6. Enable alarm if disabled in step 1.
 */
static int pm8xxx_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	u8 value[NUM_8_BIT_RTC_REGS];
	bool alarm_enabled;
	unsigned long secs;
	int rc;

	if (!rtc_dd->allow_set_time)
		return -EACCES;

	secs = rtc_tm_to_time64(tm);
	put_unaligned_le32(secs, value);

	dev_dbg(dev, "Seconds value to be written to RTC = %lu\n", secs);

	rc = regmap_update_bits_check(rtc_dd->regmap, regs->alarm_ctrl,
				      regs->alarm_en, 0, &alarm_enabled);
	if (rc)
		return rc;

	/* Disable RTC H/w before writing on RTC register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE, 0);
	if (rc)
		return rc;

	/* Write 0 to Byte[0] */
	rc = regmap_write(rtc_dd->regmap, regs->write, 0);
	if (rc)
		return rc;

	/* Write Byte[1], Byte[2], Byte[3] */
	rc = regmap_bulk_write(rtc_dd->regmap, regs->write + 1,
			       &value[1], sizeof(value) - 1);
	if (rc)
		return rc;

	/* Write Byte[0] */
	rc = regmap_write(rtc_dd->regmap, regs->write, value[0]);
	if (rc)
		return rc;

	/* Enable RTC H/w after writing on RTC register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE,
				PM8xxx_RTC_ENABLE);
	if (rc)
		return rc;

	if (alarm_enabled) {
		rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
					regs->alarm_en, regs->alarm_en);
		if (rc)
			return rc;
	}

	return 0;
}

static int pm8xxx_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	int rc;
	unsigned long secs;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	rc = pm8xxx_rtc_read_rtc_data(rtc_dd, &secs);
	if (rc)
		return rc;

	rtc_time64_to_tm(secs, tm);

	dev_dbg(dev, "secs = %lu, h:m:s == %ptRt, y-m-d = %ptRdr\n", secs, tm, tm);

	return 0;
}

static int pm8xxx_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	u8 value[NUM_8_BIT_RTC_REGS];
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	unsigned long secs;
	int rc;
	secs = rtc_tm_to_time64(&alarm->time);

	put_unaligned_le32(secs, value);

	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, 0);
	if (rc)
		return rc;

	rc = regmap_bulk_write(rtc_dd->regmap, regs->alarm_rw, value,
			       sizeof(value));
	if (rc)
		return rc;

	if (alarm->enabled) {
		rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
					regs->alarm_en, regs->alarm_en);
		if (rc)
			return rc;
	}

	dev_dbg(dev, "Alarm Set for h:m:s=%ptRt, y-m-d=%ptRdr\n",
		&alarm->time, &alarm->time);

	return 0;
}

static int pm8xxx_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	int rc;
	unsigned int ctrl_reg;
	unsigned long secs;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = pm8xxx_rtc_read_alarm_data(rtc_dd, &secs);
	if (rc)
		return rc;

	rtc_time64_to_tm(secs, &alarm->time);

	rc = regmap_read(rtc_dd->regmap, regs->alarm_ctrl, &ctrl_reg);
	if (rc)
		return rc;

	alarm->enabled = !!(ctrl_reg & PM8xxx_RTC_ALARM_ENABLE);

	dev_dbg(dev, "Alarm set for - h:m:s=%ptRt, y-m-d=%ptRdr\n",
		&alarm->time, &alarm->time);

	return 0;
}

static int pm8xxx_rtc_alarm_irq_enable(struct device *dev, unsigned int enable)
{
	int rc;
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	u8 value[NUM_8_BIT_RTC_REGS] = {0};
	unsigned int val;

	if (enable)
		val = regs->alarm_en;
	else
		val = 0;

	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, val);
	if (rc)
		return rc;

	/* Clear Alarm register */
	if (!enable) {
		rc = regmap_bulk_write(rtc_dd->regmap, regs->alarm_rw, value,
					sizeof(value));
		if (rc)
			return rc;
	}

	return 0;
}

static const struct rtc_class_ops pm8xxx_rtc_ops = {
	.read_time	= pm8xxx_rtc_read_time,
	.set_time	= pm8xxx_rtc_set_time,
	.set_alarm	= pm8xxx_rtc_set_alarm,
	.read_alarm	= pm8xxx_rtc_read_alarm,
	.alarm_irq_enable = pm8xxx_rtc_alarm_irq_enable,
};

static irqreturn_t pm8xxx_alarm_trigger(int irq, void *dev_id)
{
	struct pm8xxx_rtc *rtc_dd = dev_id;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;
	int rc;

	rtc_update_irq(rtc_dd->rtc, 1, RTC_IRQF | RTC_AF);

	/* Clear the alarm enable bit */
	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl,
				regs->alarm_en, 0);
	if (rc)
		return IRQ_NONE;

	/* Clear RTC alarm register */
	rc = regmap_update_bits(rtc_dd->regmap, regs->alarm_ctrl2,
				PM8xxx_RTC_ALARM_CLEAR, 0);
	if (rc)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

/*
 * Trigger the alarm event and clear the alarm settings
 * if the alarm data has been behind the RTC data which
 * means the alarm has been triggered before the driver
 * is probed.
 */
static int pm8xxx_rtc_init_alarm(struct pm8xxx_rtc *rtc_dd)
{
	int rc;
	unsigned long rtc_data, alarm_data;
	unsigned int ctrl_reg, alarm_en;
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	rc = pm8xxx_rtc_read_rtc_data(rtc_dd, &rtc_data);
	if (rc)
		return rc;

	rc = pm8xxx_rtc_read_alarm_data(rtc_dd, &alarm_data);
	if (rc)
		return rc;

	rc = regmap_read(rtc_dd->regmap, regs->alarm_ctrl, &ctrl_reg);
	if (rc)
		return rc;

	alarm_en = !!(ctrl_reg & PM8xxx_RTC_ALARM_ENABLE);

	if (alarm_en && rtc_data >= alarm_data)
		pm8xxx_alarm_trigger(0, rtc_dd);

	return 0;
}

static int pm8xxx_rtc_enable(struct pm8xxx_rtc *rtc_dd)
{
	const struct pm8xxx_rtc_regs *regs = rtc_dd->regs;

	return regmap_update_bits(rtc_dd->regmap, regs->ctrl, PM8xxx_RTC_ENABLE,
				  PM8xxx_RTC_ENABLE);
}

static const struct pm8xxx_rtc_regs pm8921_regs = {
	.ctrl		= 0x11d,
	.write		= 0x11f,
	.read		= 0x123,
	.alarm_rw	= 0x127,
	.alarm_ctrl	= 0x11d,
	.alarm_ctrl2	= 0x11e,
	.alarm_en	= BIT(1),
};

static const struct pm8xxx_rtc_regs pm8058_regs = {
	.ctrl		= 0x1e8,
	.write		= 0x1ea,
	.read		= 0x1ee,
	.alarm_rw	= 0x1f2,
	.alarm_ctrl	= 0x1e8,
	.alarm_ctrl2	= 0x1e9,
	.alarm_en	= BIT(1),
};

static const struct pm8xxx_rtc_regs pm8941_regs = {
	.ctrl		= 0x6046,
	.write		= 0x6040,
	.read		= 0x6048,
	.alarm_rw	= 0x6140,
	.alarm_ctrl	= 0x6146,
	.alarm_ctrl2	= 0x6148,
	.alarm_en	= BIT(7),
};

static const struct pm8xxx_rtc_regs pmk8350_regs = {
	.ctrl		= 0x6146,
	.write		= 0x6140,
	.read		= 0x6148,
	.alarm_rw	= 0x6240,
	.alarm_ctrl	= 0x6246,
	.alarm_ctrl2	= 0x6248,
	.alarm_en	= BIT(7),
};

static const struct pm8xxx_rtc_regs pm8916_regs = {
	.ctrl		= 0x6046,
	.write		= 0x6040,
	.read		= 0x6048,
	.alarm_rw	= 0x6140,
	.alarm_ctrl	= 0x6146,
	.alarm_ctrl2	= 0x6148,
	.alarm_en	= BIT(7),
};

/*
 * Hardcoded RTC bases until IORESOURCE_REG mapping is figured out
 */
static const struct of_device_id pm8xxx_id_table[] = {
	{ .compatible = "qcom,pm8921-rtc", .data = &pm8921_regs },
	{ .compatible = "qcom,pm8018-rtc", .data = &pm8921_regs },
	{ .compatible = "qcom,pm8058-rtc", .data = &pm8058_regs },
	{ .compatible = "qcom,pm8941-rtc", .data = &pm8941_regs },
	{ .compatible = "qcom,pmk8350-rtc", .data = &pmk8350_regs },
	{ .compatible = "qcom,pm8916-rtc", .data = &pm8916_regs },
	{ },
};
MODULE_DEVICE_TABLE(of, pm8xxx_id_table);

static int pm8xxx_rtc_probe(struct platform_device *pdev)
{
	int rc;
	struct pm8xxx_rtc *rtc_dd;
	const struct of_device_id *match;

	match = of_match_node(pm8xxx_id_table, pdev->dev.of_node);
	if (!match)
		return -ENXIO;

	rtc_dd = devm_kzalloc(&pdev->dev, sizeof(*rtc_dd), GFP_KERNEL);
	if (rtc_dd == NULL)
		return -ENOMEM;

	rtc_dd->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!rtc_dd->regmap)
		return -ENXIO;

	rtc_dd->rtc_alarm_irq = platform_get_irq(pdev, 0);
	if (rtc_dd->rtc_alarm_irq < 0)
		return -ENXIO;

	rtc_dd->allow_set_time = of_property_read_bool(pdev->dev.of_node,
						      "allow-set-time");

	rtc_dd->regs = match->data;
	rtc_dd->rtc_dev = &pdev->dev;

	rc = pm8xxx_rtc_enable(rtc_dd);
	if (rc)
		return rc;

	platform_set_drvdata(pdev, rtc_dd);

	device_init_wakeup(&pdev->dev, 1);

	/* Register the RTC device */
	rtc_dd->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc_dd->rtc))
		return PTR_ERR(rtc_dd->rtc);

	rtc_dd->rtc->ops = &pm8xxx_rtc_ops;
	rtc_dd->rtc->range_max = U32_MAX;

	/* Request the alarm IRQ */
	rc = devm_request_any_context_irq(&pdev->dev, rtc_dd->rtc_alarm_irq,
					  pm8xxx_alarm_trigger,
					  IRQF_TRIGGER_RISING,
					  "pm8xxx_rtc_alarm", rtc_dd);
	if (rc < 0)
		return rc;

	rc =  rtc_register_device(rtc_dd->rtc);
	if (rc < 0)
		return rc;

	if (of_property_read_bool(pdev->dev.of_node, "disable-alarm-wakeup"))
		device_set_wakeup_capable(&pdev->dev, false);

	return pm8xxx_rtc_init_alarm(rtc_dd);
}

static int pm8xxx_rtc_restore(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);
	int rc;

	/* Request the alarm IRQ */
	rc = devm_request_any_context_irq(rtc_dd->rtc_dev,
					  rtc_dd->rtc_alarm_irq,
					  pm8xxx_alarm_trigger,
					  IRQF_TRIGGER_RISING,
					  "pm8xxx_rtc_alarm", rtc_dd);
	if (rc < 0) {
		return rc;
	}

	return pm8xxx_rtc_enable(rtc_dd);
}

static int pm8xxx_rtc_freeze(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	devm_free_irq(rtc_dd->rtc_dev, rtc_dd->rtc_alarm_irq, rtc_dd);

	return 0;
}

static int pm8xxx_rtc_resume(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc_dd->rtc_alarm_irq);

	return 0;
}

static int pm8xxx_rtc_suspend(struct device *dev)
{
	struct pm8xxx_rtc *rtc_dd = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc_dd->rtc_alarm_irq);

	return 0;
}

static const struct dev_pm_ops pm8xxx_rtc_pm_ops = {
	.freeze = pm8xxx_rtc_freeze,
	.restore = pm8xxx_rtc_restore,
	.suspend = pm8xxx_rtc_suspend,
	.resume = pm8xxx_rtc_resume,
};

static void pm8xxx_rtc_shutdown(struct platform_device *pdev)
{
	struct pm8xxx_rtc *rtc_dd = platform_get_drvdata(pdev);

	devm_free_irq(rtc_dd->rtc_dev, rtc_dd->rtc_alarm_irq, rtc_dd);
}

static struct platform_driver pm8xxx_rtc_driver = {
	.probe		= pm8xxx_rtc_probe,
	.driver	= {
		.name		= "rtc-pm8xxx",
		.pm		= &pm8xxx_rtc_pm_ops,
		.of_match_table	= pm8xxx_id_table,
	},
	.shutdown	= pm8xxx_rtc_shutdown,
};

module_platform_driver(pm8xxx_rtc_driver);

MODULE_ALIAS("platform:rtc-pm8xxx");
MODULE_DESCRIPTION("PMIC8xxx RTC driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anirudh Ghayal <aghayal@codeaurora.org>");
