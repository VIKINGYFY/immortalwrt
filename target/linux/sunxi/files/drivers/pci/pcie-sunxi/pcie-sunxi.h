/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Allwinner PCIe controller driver
 *
 * Copyright (C) 2022 allwinner Co., Ltd.
 *
 * Author: songjundong <songjundong@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _PCIE_SUNXI_H
#define _PCIE_SUNXI_H

#include <linux/bits.h>
#include <asm/io.h>
#include <linux/irqreturn.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/irq.h>
#include <linux/msi.h>
#include <linux/pci.h>

struct platform_device;

#define PCIE_PORT_LINK_CONTROL			0x710
#define PORT_LINK_MODE_MASK			(0x3f << 16)
#define PORT_LINK_MODE_1_LANES			(0x1 << 16)
#define PORT_LINK_MODE_2_LANES			(0x3 << 16)
#define PORT_LINK_MODE_4_LANES			(0x7 << 16)
#define PCIE_LINK_WIDTH_SPEED_CONTROL		0x80C
#define PORT_LOGIC_SPEED_CHANGE			(0x1 << 17)
#define PORT_LOGIC_LINK_WIDTH_MASK		(0x1ff << 8)
#define PORT_LOGIC_LINK_WIDTH_1_LANES		(0x1 << 8)
#define PORT_LOGIC_LINK_WIDTH_2_LANES		(0x2 << 8)
#define PORT_LOGIC_LINK_WIDTH_4_LANES		(0x4 << 8)

#define PCIE_ATU_INDEX0				0x0

#define PCIE_ATU_CR1_OUTBOUND(reg)		(0x300000 + ((reg) * 0x200))
#define PCIE_ATU_TYPE_MEM			(0x0 << 0)
#define PCIE_ATU_TYPE_IO			(0x2 << 0)
#define PCIE_ATU_TYPE_CFG0			(0x4 << 0)
#define PCIE_ATU_TYPE_CFG1			(0x5 << 0)
#define PCIE_ATU_CR2_OUTBOUND(reg)		(0x300004 + ((reg) * 0x200))
#define PCIE_ATU_ENABLE				BIT(31)

#define PCIE_ATU_LOWER_BASE_OUTBOUND(reg)	(0x300008 + ((reg) * 0x200))
#define PCIE_ATU_UPPER_BASE_OUTBOUND(reg)	(0x30000c + ((reg) * 0x200))
#define PCIE_ATU_LIMIT_OUTBOUND(reg)		(0x300010 + ((reg) * 0x200))
#define PCIE_ATU_LOWER_TARGET_OUTBOUND(reg)	(0x300014 + ((reg) * 0x200))
#define PCIE_ATU_UPPER_TARGET_OUTBOUND(reg)	(0x300018 + ((reg) * 0x200))

#define PCIE_ATU_BUS(x)				(((x) & 0xff) << 24)
#define PCIE_ATU_DEV(x)				(((x) & 0x1f) << 19)
#define PCIE_ATU_FUNC(x)			(((x) & 0x7) << 16)

#define PCIE_MISC_CONTROL_1_CFG			0x8bc
#define PCIE_HIGH16_MASK			0xffff0000
#define PCIE_INTERRUPT_LINE_MASK		0xffff00ff
#define PCIE_INTERRUPT_LINE_ENABLE		0x00000100

#define PCIE_CPU_BASE				0x20000000

#define NEXT_CAP_PTR_MASK		0xff00
#define CAP_ID_MASK			0x00ff

/*
 * Maximum number of MSI IRQs can be 256 per controller. But keep
 * it 32 as of now. Probably we will never need more than 32. If needed,
 * then increment it in multiple of 32.
 */
#define INT_PCI_MSI_NR			32
#define HW_MSI_CTRLS			8
#define MAX_MSI_IRQS_PER_CTRL		32
#define MAX_MSI_CTRLS			DIV_ROUND_UP(INT_PCI_MSI_NR, MAX_MSI_IRQS_PER_CTRL)
#define MSI_REG_CTRL_BLOCK_SIZE		12

#define PCIE_LINK_WIDTH_SPEED_CONTROL	0x80C
#define PORT_LOGIC_SPEED_CHANGE		(0x1 << 17)
/* Parameters for the waiting for link up routine */
#define LINK_WAIT_MAX_RETRIE		20
#define LINK_WAIT_USLEEP_MIN		90000
#define LINK_WAIT_USLEEP_MAX		100000
#define SPEED_CHANGE_MAX_RETRIES	200
#define SPEED_CHANGE_USLEEP_MIN		100
#define SPEED_CHANGE_USLEEP_MAX		1000
#define WAIT_ATU			1

#define PCIE_MSI_ADDR_LO		0x820
#define PCIE_MSI_ADDR_HI		0x824
#define PCIE_MSI_INTR_ENABLE(reg)	(0x828 + ((reg) * 0x0c))
#define PCIE_MSI_INTR_STATUS		0x830

#define PCIE_USER_DEFINED_REGISTER	0x400000
#define PCIE_LTSSM_CTRL			0xc00
#define PCIE_LINK_TRAINING		BIT(0) /* 0:disable; 1:enable  */
#define PCIE_INT_ENABLE_CLR		0xE04  /* BIT(1):RDLH_LINK_MASK; BIT(0):SMLH_LINK_MASK  */
#define PCIE_LINK_STAT			0xE0C  /* BIT(1):RDLH_LINK;	  BIT(0):SMLH_LINK  */
#define RDLH_LINK_UP			BIT(1)
#define SMLH_LINK_UP			BIT(0)
#define PCIE_LINK_INT_EN		(BIT(0) | BIT(1))

#define SII_INT_MASK0			0x0e00
#define SII_INT_STAS0			0x0e08
	#define INTX_TX_DEASSERT_MASK	GENMASK(28, 25)
	#define INTX_TX_DEASSERT_SHIFT	25
	#define INTX_TX_DEASSERT(x)		BIT((x) + INTX_TX_DEASSERT_SHIFT)
	#define INTX_TX_ASSERT_MASK		GENMASK(24, 21)
	#define INTX_TX_ASSERT_SHIFT	21
	#define INTX_TX_ASSERT(x)		BIT((x) + INTX_TX_ASSERT_SHIFT)
	#define INTX_RX_DEASSERT_MASK	GENMASK(12, 9)
	#define INTX_RX_DEASSERT_SHIFT	9
	#define INTX_RX_DEASSERT(x)		BIT((x) + INTX_RX_DEASSERT_SHIFT)
	#define INTX_RX_ASSERT_MASK		GENMASK(8, 5)
	#define INTX_RX_ASSERT_SHIFT	5
	#define INTX_RX_ASSERT(x)		BIT((x) + INTX_RX_ASSERT_SHIFT)

struct sunxi_pcie_of_data {
	bool cpu_pcie_addr_quirk;
	bool has_pcie_slv_clk;
	bool need_pcie_rst;
	bool pcie_slv_clk_400m;
	bool has_pcie_its_clk;
};

struct sunxi_pcie_port {
	struct device			*dev;
	void __iomem			*dbi_base;
	u64				cfg0_base;
	void __iomem			*va_cfg0_base;
	u32				cfg0_size;
	resource_size_t			io_base;
	phys_addr_t			io_bus_addr;
	u32				io_size;
	u32				num_ob_windows;
	struct sunxi_pcie_host_ops	*ops;
	int				msi_irq;
	int				sii_irq;
	struct irq_domain		*intx_irq_domain;
	struct irq_domain		*irq_domain;
	u16			msi_msg;
	dma_addr_t		msi_data;
	struct pci_host_bridge		*bridge;
	raw_spinlock_t			lock;
	unsigned long			msi_map[BITS_TO_LONGS(INT_PCI_MSI_NR)];
	bool				has_its;
	bool				cpu_pcie_addr_quirk;
};

struct sunxi_pcie {
	struct device		*dev;
	void __iomem		*dbi_base;
	void __iomem		*app_base;
	int			link_gen;
	struct sunxi_pcie_port	pp;
	struct clk		*pcie_aux;
	struct clk		*pcie_slv;
	struct clk		*pcie_its;
	struct reset_control    *pcie_rst;
	struct reset_control    *pwrup_rst;
	struct reset_control    *pcie_its_rst;
	struct phy		*phy;
	const struct sunxi_pcie_of_data *drvdata;
	struct gpio_desc	*rst_gpio;
	struct gpio_desc	*wake_gpio;
	u32			lanes;
	struct regulator	*pcie3v3;
};

#define to_sunxi_pcie_from_pp(x)	\
		container_of((x), struct sunxi_pcie, pp)

struct sunxi_pcie_host_ops {
	void (*readl_rc)(struct sunxi_pcie_port *pp, void __iomem *dbi_base, u32 *val);
	void (*writel_rc)(struct sunxi_pcie_port *pp,	u32 val, void __iomem *dbi_base);
	int  (*rd_own_conf)(struct sunxi_pcie_port *pp, int where, int size, u32 *val);
	int  (*wr_own_conf)(struct sunxi_pcie_port *pp, int where, int size, u32 val);
	bool (*is_link_up)(struct sunxi_pcie_port *pp);
	int  (*host_init)(struct sunxi_pcie_port *pp);
	void (*scan_bus)(struct sunxi_pcie_port *pp);
};

void sunxi_pcie_plat_set_rate(struct sunxi_pcie *pci);
void sunxi_pcie_write_dbi(struct sunxi_pcie *pci, u32 reg, size_t size, u32 val);
u32 sunxi_pcie_read_dbi(struct sunxi_pcie *pci, u32 reg, size_t size);
void sunxi_pcie_plat_ltssm_enable(struct sunxi_pcie *pci);
void sunxi_pcie_plat_ltssm_disable(struct sunxi_pcie *pci);
int sunxi_pcie_cfg_write(void __iomem *addr, int size, u32 val);
int sunxi_pcie_cfg_read(void __iomem *addr, int size, u32 *val);

int sunxi_pcie_host_add_port(struct sunxi_pcie *pci, struct platform_device *pdev);
void sunxi_pcie_host_remove_port(struct sunxi_pcie *pci);
int sunxi_pcie_host_speed_change(struct sunxi_pcie *pci, int gen);
int sunxi_pcie_host_wr_own_conf(struct sunxi_pcie_port *pp, int where, int size, u32 val);
int sunxi_pcie_host_establish_link(struct sunxi_pcie *pci);
void sunxi_pcie_host_setup_rc(struct sunxi_pcie_port *pp);

void sunxi_pcie_writel(u32 val, struct sunxi_pcie *pcie, u32 offset);
u32 sunxi_pcie_readl(struct sunxi_pcie *pcie, u32 offset);
void sunxi_pcie_writel_dbi(struct sunxi_pcie *pci, u32 reg, u32 val);
u32 sunxi_pcie_readl_dbi(struct sunxi_pcie *pci, u32 reg);
void sunxi_pcie_writew_dbi(struct sunxi_pcie *pci, u32 reg, u16 val);
u16 sunxi_pcie_readw_dbi(struct sunxi_pcie *pci, u32 reg);
void sunxi_pcie_writeb_dbi(struct sunxi_pcie *pci, u32 reg, u8 val);
u8 sunxi_pcie_readb_dbi(struct sunxi_pcie *pci, u32 reg);
void sunxi_pcie_dbi_ro_wr_en(struct sunxi_pcie *pci);
void sunxi_pcie_dbi_ro_wr_dis(struct sunxi_pcie *pci);
u8 sunxi_pcie_plat_find_capability(struct sunxi_pcie *pci, u8 cap);
#endif /* _PCIE_SUNXI_H */
