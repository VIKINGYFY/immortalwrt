// SPDX-License-Identifier: GPL-2.0-only
/* RTL9303 external-CPU transport, as used by the Verizon CR1000A. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>
#include "rtl9303-version.h"

struct rtl9303_spi {
	struct spi_device *spi;
	/* regmap serializes access to these DMA-safe, kmalloc-backed buffers. */
	u8 tx[8] ____cacheline_aligned;
	u8 rx[8] ____cacheline_aligned;
};

static int rtl9303_read(void *context, unsigned int reg, unsigned int *value)
{
	struct rtl9303_spi *priv = context;
	struct spi_transfer xfer = {
		.tx_buf = priv->tx,
		.rx_buf = priv->rx,
		.len = sizeof(priv->tx),
	};
	int ret;

	memset(priv->tx, 0, sizeof(priv->tx));
	priv->tx[0] = 3;
	put_unaligned_be16(reg, &priv->tx[1]);
	ret = spi_sync_transfer(priv->spi, &xfer, 1);
	if (!ret)
		*value = get_unaligned_be32(&priv->rx[4]);
	return ret;
}

static int rtl9303_write(void *context, unsigned int reg, unsigned int value)
{
	struct rtl9303_spi *priv = context;

	priv->tx[0] = 2;
	put_unaligned_be16(reg, &priv->tx[1]);
	priv->tx[3] = 0;
	put_unaligned_be32(value, &priv->tx[4]);
	return spi_write(priv->spi, priv->tx, sizeof(priv->tx));
}

static const struct regmap_config rtl9303_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xfffc,
	.reg_read = rtl9303_read,
	.reg_write = rtl9303_write,
	.cache_type = REGCACHE_NONE,
};

static void rtl9303_unregister_switch(void *data)
{
	platform_device_unregister(data);
}

static int rtl9303_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct platform_device *pdev;
	struct rtl9303_spi *priv;
	struct regmap *map;
	u32 id;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->spi = spi;
	spi->bits_per_word = 8;
	spi->max_speed_hz = min(spi->max_speed_hz, 12000000U);
	ret = spi_setup(spi);
	if (ret)
		return ret;
	map = devm_regmap_init(dev, NULL, priv, &rtl9303_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);
	ret = regmap_read(map, 4, &id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read switch ID\n");
	if ((id >> 16) != 0x9303)
		return dev_err_probe(dev, -ENODEV, "unexpected switch ID %#x\n", id);

	dev_info(dev, "firmware-tag=%s switch-id=%08x\n", CR1000A_BUILD_TAG, id);

	/* Independent children allow normal deferred probing of PHYs and PCS.
	 * The DSA child shares the SPI node containing ethernet-ports; management
	 * providers are its siblings and all use the parent SPI regmap.
	 */
	ret = devm_of_platform_populate(dev);
	if (ret)
		return ret;
	pdev = platform_device_alloc("rtl9303-dsa", PLATFORM_DEVID_AUTO);
	if (!pdev)
		return -ENOMEM;
	pdev->dev.parent = dev;
	pdev->dev.of_node = of_node_get(dev->of_node);
	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		return ret;
	}
	return devm_add_action_or_reset(dev, rtl9303_unregister_switch, pdev);
}

static const struct of_device_id rtl9303_spi_of_match[] = {
	{ .compatible = "realtek,rtl9303-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9303_spi_of_match);

static const struct spi_device_id rtl9303_spi_ids[] = {
	{ "rtl9303-spi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, rtl9303_spi_ids);

static struct spi_driver rtl9303_spi_driver = {
	.driver = {
		.name = "rtl9303-spi",
		.of_match_table = rtl9303_spi_of_match,
	},
	.probe = rtl9303_spi_probe,
	.id_table = rtl9303_spi_ids,
};
module_spi_driver(rtl9303_spi_driver);

MODULE_DESCRIPTION("RTL9303 external SPI register transport");
MODULE_LICENSE("GPL");

MODULE_VERSION(CR1000A_BUILD_TAG);
