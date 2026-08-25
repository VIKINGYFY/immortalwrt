// SPDX-License-Identifier: GPL-2.0

#include <linux/gpio/driver.h>
#include <linux/version.h>

#include "rtl837x_common.h"

struct rtl837x_gpio {
	struct rtk_gsw *gsw;
	struct gpio_chip gp;
};

static int rtl837x_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct rtl837x_gpio *gpio = gpiochip_get_data(gc);
	u32 ret;

	rtl837x_sdk_lock(gpio->gsw);
	ret = gpio->gsw->pMapper->gpio_muxSel_set(offset);
	rtl837x_sdk_unlock(gpio->gsw);
	if (ret) {
		dev_err(gpio->gsw->dev, "gpio %u: failed to request gpio: %x\n", offset, ret);
		return -EINVAL;
	}

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static int rtl837x_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
#else
static void rtl837x_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
#endif
{
	struct rtl837x_gpio *gpio = gpiochip_get_data(gc);
	rtk_gpio_level_t level = value ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
	u32 ret;

	rtl837x_sdk_lock(gpio->gsw);
	ret = gpio->gsw->pMapper->gpio_pinVal_write(offset, level);
	rtl837x_sdk_unlock(gpio->gsw);
	if (ret) {
		dev_err(gpio->gsw->dev, "gpio %u: failed to write value: %x\n", offset, ret);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
		return -EIO;
#endif
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	return 0;
#endif
}

static int rtl837x_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct rtl837x_gpio *gpio = gpiochip_get_data(gc);
	rtk_gpio_level_t level = GPIO_LEVEL_LOW;
	u32 ret;

	rtl837x_sdk_lock(gpio->gsw);
	ret = gpio->gsw->pMapper->gpio_pinVal_read(offset, &level);
	rtl837x_sdk_unlock(gpio->gsw);
	if (ret) {
		dev_err(gpio->gsw->dev, "gpio %u: failed to read value: %x\n", offset, ret);
		return -EIO;
	}

	return level == GPIO_LEVEL_HIGH;
}

static int rtl837x_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct rtl837x_gpio *gpio = gpiochip_get_data(gc);
	u32 ret;

	rtl837x_sdk_lock(gpio->gsw);
	ret = gpio->gsw->pMapper->gpio_pinDir_set(offset, GPIO_DIR_INPUT);
	rtl837x_sdk_unlock(gpio->gsw);
	if (ret) {
		dev_err(gpio->gsw->dev, "gpio %u: failed to set input direction: %x\n", offset, ret);
		return -EINVAL;
	}

	return 0;
}

static int rtl837x_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct rtl837x_gpio *gpio = gpiochip_get_data(gc);
	rtk_gpio_level_t level = value ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
	u32 ret;

	rtl837x_sdk_lock(gpio->gsw);
	ret = gpio->gsw->pMapper->gpio_pinDir_set(offset, GPIO_DIR_OUTPUT);
	if (!ret)
		ret = gpio->gsw->pMapper->gpio_pinVal_write(offset, level);
	rtl837x_sdk_unlock(gpio->gsw);
	if (ret) {
		dev_err(gpio->gsw->dev, "gpio %u: failed to set output direction/value: %x\n", offset, ret);
		return -EIO;
	}

	return 0;
}

static const struct gpio_chip rtl837x_gpio_template = {
	.label = "rtl837x-gpio",
	.owner = THIS_MODULE,
	.request = rtl837x_gpio_request,
	.get = rtl837x_gpio_get,
	.set = rtl837x_gpio_set,
	.direction_input = rtl837x_gpio_direction_input,
	.direction_output = rtl837x_gpio_direction_output,
	.can_sleep = true,
	.ngpio = 63,
};

int rtl837x_gpiochip_init(struct rtk_gsw *gsw)
{
	struct device *dev = gsw->dev;
	struct device_node *np = dev->of_node;
	struct rtl837x_gpio *gpio;
	u32 base;
	bool has_base;

	has_base = !of_property_read_u32(np, "base", &base);

	gpio = devm_kmalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->gsw = gsw;
	gpio->gp = rtl837x_gpio_template;
	gpio->gp.base = has_base ? base : -1;
	gpio->gp.parent = dev;

	return devm_gpiochip_add_data(dev, &gpio->gp, gpio);
}
