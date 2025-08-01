// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R-Car GPIO Support
 *
 *  Copyright (C) 2014 Renesas Electronics Corporation
 *  Copyright (C) 2013 Magnus Damm
 */

#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

struct gpio_rcar_bank_info {
	u32 iointsel;
	u32 inoutsel;
	u32 outdt;
	u32 posneg;
	u32 edglevel;
	u32 bothedge;
	u32 intmsk;
};

struct gpio_rcar_info {
	bool has_outdtsel;
	bool has_both_edge_trigger;
	bool has_always_in;
	bool has_inen;
};

struct gpio_rcar_priv {
	void __iomem *base;
	raw_spinlock_t lock;
	struct device *dev;
	struct gpio_chip gpio_chip;
	unsigned int irq_parent;
	atomic_t wakeup_path;
	struct gpio_rcar_info info;
	struct gpio_rcar_bank_info bank_info;
};

#define IOINTSEL	0x00	/* General IO/Interrupt Switching Register */
#define INOUTSEL	0x04	/* General Input/Output Switching Register */
#define OUTDT		0x08	/* General Output Register */
#define INDT		0x0c	/* General Input Register */
#define INTDT		0x10	/* Interrupt Display Register */
#define INTCLR		0x14	/* Interrupt Clear Register */
#define INTMSK		0x18	/* Interrupt Mask Register */
#define MSKCLR		0x1c	/* Interrupt Mask Clear Register */
#define POSNEG		0x20	/* Positive/Negative Logic Select Register */
#define EDGLEVEL	0x24	/* Edge/level Select Register */
#define FILONOFF	0x28	/* Chattering Prevention On/Off Register */
#define OUTDTSEL	0x40	/* Output Data Select Register */
#define BOTHEDGE	0x4c	/* One Edge/Both Edge Select Register */
#define INEN		0x50	/* General Input Enable Register */

#define RCAR_MAX_GPIO_PER_BANK		32

static inline u32 gpio_rcar_read(struct gpio_rcar_priv *p, int offs)
{
	return ioread32(p->base + offs);
}

static inline void gpio_rcar_write(struct gpio_rcar_priv *p, int offs,
				   u32 value)
{
	iowrite32(value, p->base + offs);
}

static void gpio_rcar_modify_bit(struct gpio_rcar_priv *p, int offs,
				 int bit, bool value)
{
	u32 tmp = gpio_rcar_read(p, offs);

	if (value)
		tmp |= BIT(bit);
	else
		tmp &= ~BIT(bit);

	gpio_rcar_write(p, offs, tmp);
}

static void gpio_rcar_irq_disable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct gpio_rcar_priv *p = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpio_rcar_write(p, INTMSK, ~BIT(hwirq));
	gpiochip_disable_irq(gc, hwirq);
}

static void gpio_rcar_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct gpio_rcar_priv *p = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_enable_irq(gc, hwirq);
	gpio_rcar_write(p, MSKCLR, BIT(hwirq));
}

static void gpio_rcar_config_interrupt_input_mode(struct gpio_rcar_priv *p,
						  unsigned int hwirq,
						  bool active_high_rising_edge,
						  bool level_trigger,
						  bool both)
{
	unsigned long flags;

	/* follow steps in the GPIO documentation for
	 * "Setting Edge-Sensitive Interrupt Input Mode" and
	 * "Setting Level-Sensitive Interrupt Input Mode"
	 */

	raw_spin_lock_irqsave(&p->lock, flags);

	/* Configure positive or negative logic in POSNEG */
	gpio_rcar_modify_bit(p, POSNEG, hwirq, !active_high_rising_edge);

	/* Configure edge or level trigger in EDGLEVEL */
	gpio_rcar_modify_bit(p, EDGLEVEL, hwirq, !level_trigger);

	/* Select one edge or both edges in BOTHEDGE */
	if (p->info.has_both_edge_trigger)
		gpio_rcar_modify_bit(p, BOTHEDGE, hwirq, both);

	/* Select "Interrupt Input Mode" in IOINTSEL */
	gpio_rcar_modify_bit(p, IOINTSEL, hwirq, true);

	/* Write INTCLR in case of edge trigger */
	if (!level_trigger)
		gpio_rcar_write(p, INTCLR, BIT(hwirq));

	raw_spin_unlock_irqrestore(&p->lock, flags);
}

static int gpio_rcar_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct gpio_rcar_priv *p = gpiochip_get_data(gc);
	unsigned int hwirq = irqd_to_hwirq(d);

	dev_dbg(p->dev, "sense irq = %d, type = %d\n", hwirq, type);

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
		gpio_rcar_config_interrupt_input_mode(p, hwirq, true, true,
						      false);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		gpio_rcar_config_interrupt_input_mode(p, hwirq, false, true,
						      false);
		break;
	case IRQ_TYPE_EDGE_RISING:
		gpio_rcar_config_interrupt_input_mode(p, hwirq, true, false,
						      false);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		gpio_rcar_config_interrupt_input_mode(p, hwirq, false, false,
						      false);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		if (!p->info.has_both_edge_trigger)
			return -EINVAL;
		gpio_rcar_config_interrupt_input_mode(p, hwirq, true, false,
						      true);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int gpio_rcar_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct gpio_rcar_priv *p = gpiochip_get_data(gc);
	int error;

	if (p->irq_parent) {
		error = irq_set_irq_wake(p->irq_parent, on);
		if (error) {
			dev_dbg(p->dev, "irq %u doesn't support irq_set_wake\n",
				p->irq_parent);
			p->irq_parent = 0;
		}
	}

	if (on)
		atomic_inc(&p->wakeup_path);
	else
		atomic_dec(&p->wakeup_path);

	return 0;
}

static const struct irq_chip gpio_rcar_irq_chip = {
	.name		= "gpio-rcar",
	.irq_mask	= gpio_rcar_irq_disable,
	.irq_unmask	= gpio_rcar_irq_enable,
	.irq_set_type	= gpio_rcar_irq_set_type,
	.irq_set_wake	= gpio_rcar_irq_set_wake,
	.flags		= IRQCHIP_IMMUTABLE | IRQCHIP_SET_TYPE_MASKED |
			  IRQCHIP_MASK_ON_SUSPEND,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static irqreturn_t gpio_rcar_irq_handler(int irq, void *dev_id)
{
	struct gpio_rcar_priv *p = dev_id;
	u32 pending;
	unsigned int offset, irqs_handled = 0;

	while ((pending = gpio_rcar_read(p, INTDT) &
			  gpio_rcar_read(p, INTMSK))) {
		offset = __ffs(pending);
		gpio_rcar_write(p, INTCLR, BIT(offset));
		generic_handle_domain_irq(p->gpio_chip.irq.domain,
					  offset);
		irqs_handled++;
	}

	return irqs_handled ? IRQ_HANDLED : IRQ_NONE;
}

static void gpio_rcar_config_general_input_output_mode(struct gpio_chip *chip,
						       unsigned int gpio,
						       bool output)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	unsigned long flags;

	/* follow steps in the GPIO documentation for
	 * "Setting General Output Mode" and
	 * "Setting General Input Mode"
	 */

	raw_spin_lock_irqsave(&p->lock, flags);

	/* Configure positive logic in POSNEG */
	gpio_rcar_modify_bit(p, POSNEG, gpio, false);

	/* Select "General Input/Output Mode" in IOINTSEL */
	gpio_rcar_modify_bit(p, IOINTSEL, gpio, false);

	/* Select Input Mode or Output Mode in INOUTSEL */
	gpio_rcar_modify_bit(p, INOUTSEL, gpio, output);

	/* Select General Output Register to output data in OUTDTSEL */
	if (p->info.has_outdtsel && output)
		gpio_rcar_modify_bit(p, OUTDTSEL, gpio, false);

	raw_spin_unlock_irqrestore(&p->lock, flags);
}

static int gpio_rcar_request(struct gpio_chip *chip, unsigned offset)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	int error;

	error = pm_runtime_get_sync(p->dev);
	if (error < 0) {
		pm_runtime_put(p->dev);
		return error;
	}

	error = pinctrl_gpio_request(chip, offset);
	if (error)
		pm_runtime_put(p->dev);

	return error;
}

static void gpio_rcar_free(struct gpio_chip *chip, unsigned offset)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);

	pinctrl_gpio_free(chip, offset);

	/*
	 * Set the GPIO as an input to ensure that the next GPIO request won't
	 * drive the GPIO pin as an output.
	 */
	gpio_rcar_config_general_input_output_mode(chip, offset, false);

	pm_runtime_put(p->dev);
}

static int gpio_rcar_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);

	if (gpio_rcar_read(p, INOUTSEL) & BIT(offset))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int gpio_rcar_direction_input(struct gpio_chip *chip, unsigned offset)
{
	gpio_rcar_config_general_input_output_mode(chip, offset, false);
	return 0;
}

static int gpio_rcar_get(struct gpio_chip *chip, unsigned offset)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	u32 bit = BIT(offset);

	/*
	 * Before R-Car Gen3, INDT does not show correct pin state when
	 * configured as output, so use OUTDT in case of output pins
	 */
	if (!p->info.has_always_in && (gpio_rcar_read(p, INOUTSEL) & bit))
		return !!(gpio_rcar_read(p, OUTDT) & bit);
	else
		return !!(gpio_rcar_read(p, INDT) & bit);
}

static int gpio_rcar_get_multiple(struct gpio_chip *chip, unsigned long *mask,
				  unsigned long *bits)
{
	u32 bankmask = mask[0] & GENMASK(chip->ngpio - 1, 0);
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	u32 outputs, m, val = 0;
	unsigned long flags;

	if (p->info.has_always_in) {
		bits[0] = gpio_rcar_read(p, INDT) & bankmask;
		return 0;
	}

	raw_spin_lock_irqsave(&p->lock, flags);
	outputs = gpio_rcar_read(p, INOUTSEL);
	m = outputs & bankmask;
	if (m)
		val |= gpio_rcar_read(p, OUTDT) & m;

	m = ~outputs & bankmask;
	if (m)
		val |= gpio_rcar_read(p, INDT) & m;
	raw_spin_unlock_irqrestore(&p->lock, flags);

	bits[0] = val;
	return 0;
}

static int gpio_rcar_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	unsigned long flags;

	raw_spin_lock_irqsave(&p->lock, flags);
	gpio_rcar_modify_bit(p, OUTDT, offset, value);
	raw_spin_unlock_irqrestore(&p->lock, flags);

	return 0;
}

static int gpio_rcar_set_multiple(struct gpio_chip *chip, unsigned long *mask,
				  unsigned long *bits)
{
	u32 bankmask = mask[0] & GENMASK(chip->ngpio - 1, 0);
	struct gpio_rcar_priv *p = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&p->lock, flags);
	val = gpio_rcar_read(p, OUTDT);
	val &= ~bankmask;
	val |= (bankmask & bits[0]);
	gpio_rcar_write(p, OUTDT, val);
	raw_spin_unlock_irqrestore(&p->lock, flags);

	return 0;
}

static int gpio_rcar_direction_output(struct gpio_chip *chip, unsigned offset,
				      int value)
{
	/* write GPIO value to output before selecting output mode of pin */
	gpio_rcar_set(chip, offset, value);
	gpio_rcar_config_general_input_output_mode(chip, offset, true);
	return 0;
}

static const struct gpio_rcar_info gpio_rcar_info_gen1 = {
	.has_outdtsel = false,
	.has_both_edge_trigger = false,
	.has_always_in = false,
	.has_inen = false,
};

static const struct gpio_rcar_info gpio_rcar_info_gen2 = {
	.has_outdtsel = true,
	.has_both_edge_trigger = true,
	.has_always_in = false,
	.has_inen = false,
};

static const struct gpio_rcar_info gpio_rcar_info_gen3 = {
	.has_outdtsel = true,
	.has_both_edge_trigger = true,
	.has_always_in = true,
	.has_inen = false,
};

static const struct gpio_rcar_info gpio_rcar_info_gen4 = {
	.has_outdtsel = true,
	.has_both_edge_trigger = true,
	.has_always_in = true,
	.has_inen = true,
};

static const struct of_device_id gpio_rcar_of_table[] = {
	{
		.compatible = "renesas,gpio-r8a779a0",
		.data = &gpio_rcar_info_gen4,
	}, {
		.compatible = "renesas,rcar-gen1-gpio",
		.data = &gpio_rcar_info_gen1,
	}, {
		.compatible = "renesas,rcar-gen2-gpio",
		.data = &gpio_rcar_info_gen2,
	}, {
		.compatible = "renesas,rcar-gen3-gpio",
		.data = &gpio_rcar_info_gen3,
	}, {
		.compatible = "renesas,rcar-gen4-gpio",
		.data = &gpio_rcar_info_gen4,
	}, {
		.compatible = "renesas,gpio-rcar",
		.data = &gpio_rcar_info_gen1,
	}, {
		/* Terminator */
	},
};

MODULE_DEVICE_TABLE(of, gpio_rcar_of_table);

static int gpio_rcar_parse_dt(struct gpio_rcar_priv *p, unsigned int *npins)
{
	struct device_node *np = p->dev->of_node;
	const struct gpio_rcar_info *info;
	struct of_phandle_args args;
	int ret;

	info = of_device_get_match_data(p->dev);
	p->info = *info;

	ret = of_parse_phandle_with_fixed_args(np, "gpio-ranges", 3, 0, &args);
	if (ret) {
		*npins = RCAR_MAX_GPIO_PER_BANK;
	} else {
		*npins = args.args[2];
		of_node_put(args.np);
	}

	if (*npins == 0 || *npins > RCAR_MAX_GPIO_PER_BANK) {
		dev_warn(p->dev, "Invalid number of gpio lines %u, using %u\n",
			 *npins, RCAR_MAX_GPIO_PER_BANK);
		*npins = RCAR_MAX_GPIO_PER_BANK;
	}

	return 0;
}

static void gpio_rcar_enable_inputs(struct gpio_rcar_priv *p)
{
	u32 mask = GENMASK(p->gpio_chip.ngpio - 1, 0);
	const unsigned long *valid_mask;

	valid_mask = gpiochip_query_valid_mask(&p->gpio_chip);

	/* Select "Input Enable" in INEN */
	if (valid_mask)
		mask &= valid_mask[0];
	if (mask)
		gpio_rcar_write(p, INEN, gpio_rcar_read(p, INEN) | mask);
}

static int gpio_rcar_probe(struct platform_device *pdev)
{
	struct gpio_rcar_priv *p;
	struct gpio_chip *gpio_chip;
	struct gpio_irq_chip *girq;
	struct device *dev = &pdev->dev;
	const char *name = dev_name(dev);
	unsigned int npins;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = dev;
	raw_spin_lock_init(&p->lock);

	/* Get device configuration from DT node */
	ret = gpio_rcar_parse_dt(p, &npins);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, p);

	pm_runtime_enable(dev);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		goto err0;
	p->irq_parent = ret;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base)) {
		ret = PTR_ERR(p->base);
		goto err0;
	}

	gpio_chip = &p->gpio_chip;
	gpio_chip->request = gpio_rcar_request;
	gpio_chip->free = gpio_rcar_free;
	gpio_chip->get_direction = gpio_rcar_get_direction;
	gpio_chip->direction_input = gpio_rcar_direction_input;
	gpio_chip->get = gpio_rcar_get;
	gpio_chip->get_multiple = gpio_rcar_get_multiple;
	gpio_chip->direction_output = gpio_rcar_direction_output;
	gpio_chip->set_rv = gpio_rcar_set;
	gpio_chip->set_multiple_rv = gpio_rcar_set_multiple;
	gpio_chip->label = name;
	gpio_chip->parent = dev;
	gpio_chip->owner = THIS_MODULE;
	gpio_chip->base = -1;
	gpio_chip->ngpio = npins;

	girq = &gpio_chip->irq;
	gpio_irq_chip_set_chip(girq, &gpio_rcar_irq_chip);
	/* This will let us handle the parent IRQ in the driver */
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_level_irq;

	ret = gpiochip_add_data(gpio_chip, p);
	if (ret) {
		dev_err(dev, "failed to add GPIO controller\n");
		goto err0;
	}

	irq_domain_set_pm_device(gpio_chip->irq.domain, dev);
	ret = devm_request_irq(dev, p->irq_parent, gpio_rcar_irq_handler,
			       IRQF_SHARED, name, p);
	if (ret) {
		dev_err(dev, "failed to request IRQ\n");
		goto err1;
	}

	if (p->info.has_inen) {
		pm_runtime_get_sync(dev);
		gpio_rcar_enable_inputs(p);
		pm_runtime_put(dev);
	}

	dev_info(dev, "driving %d GPIOs\n", npins);

	return 0;

err1:
	gpiochip_remove(gpio_chip);
err0:
	pm_runtime_disable(dev);
	return ret;
}

static void gpio_rcar_remove(struct platform_device *pdev)
{
	struct gpio_rcar_priv *p = platform_get_drvdata(pdev);

	gpiochip_remove(&p->gpio_chip);

	pm_runtime_disable(&pdev->dev);
}

static int gpio_rcar_suspend(struct device *dev)
{
	struct gpio_rcar_priv *p = dev_get_drvdata(dev);

	p->bank_info.iointsel = gpio_rcar_read(p, IOINTSEL);
	p->bank_info.inoutsel = gpio_rcar_read(p, INOUTSEL);
	p->bank_info.outdt = gpio_rcar_read(p, OUTDT);
	p->bank_info.intmsk = gpio_rcar_read(p, INTMSK);
	p->bank_info.posneg = gpio_rcar_read(p, POSNEG);
	p->bank_info.edglevel = gpio_rcar_read(p, EDGLEVEL);
	if (p->info.has_both_edge_trigger)
		p->bank_info.bothedge = gpio_rcar_read(p, BOTHEDGE);

	if (atomic_read(&p->wakeup_path))
		device_set_wakeup_path(dev);

	return 0;
}

static int gpio_rcar_resume(struct device *dev)
{
	struct gpio_rcar_priv *p = dev_get_drvdata(dev);
	unsigned int offset;
	u32 mask;

	for (offset = 0; offset < p->gpio_chip.ngpio; offset++) {
		if (!gpiochip_line_is_valid(&p->gpio_chip, offset))
			continue;

		mask = BIT(offset);
		/* I/O pin */
		if (!(p->bank_info.iointsel & mask)) {
			if (p->bank_info.inoutsel & mask)
				gpio_rcar_direction_output(
					&p->gpio_chip, offset,
					!!(p->bank_info.outdt & mask));
			else
				gpio_rcar_direction_input(&p->gpio_chip,
							  offset);
		} else {
			/* Interrupt pin */
			gpio_rcar_config_interrupt_input_mode(
				p,
				offset,
				!(p->bank_info.posneg & mask),
				!(p->bank_info.edglevel & mask),
				!!(p->bank_info.bothedge & mask));

			if (p->bank_info.intmsk & mask)
				gpio_rcar_write(p, MSKCLR, mask);
		}
	}

	if (p->info.has_inen)
		gpio_rcar_enable_inputs(p);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(gpio_rcar_pm_ops, gpio_rcar_suspend,
				gpio_rcar_resume);

static struct platform_driver gpio_rcar_device_driver = {
	.probe		= gpio_rcar_probe,
	.remove		= gpio_rcar_remove,
	.driver		= {
		.name	= "gpio_rcar",
		.pm     = pm_sleep_ptr(&gpio_rcar_pm_ops),
		.of_match_table = gpio_rcar_of_table,
	}
};

module_platform_driver(gpio_rcar_device_driver);

MODULE_AUTHOR("Magnus Damm");
MODULE_DESCRIPTION("Renesas R-Car GPIO Driver");
MODULE_LICENSE("GPL v2");
