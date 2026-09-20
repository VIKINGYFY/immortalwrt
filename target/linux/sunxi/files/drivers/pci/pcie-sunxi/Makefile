# SPDX-License-Identifier: GPL-2.0
ccflags-y += -DDYNAMIC_DEBUG_MODULE

ccflags-y += -I $(srctree)/drivers/pci/
pcie_sunxi_host-objs := pcie-sunxi-rc.o pcie-sunxi-plat.o
obj-$(CONFIG_PCIE_SUN55I_RC) += pcie_sunxi_host.o
