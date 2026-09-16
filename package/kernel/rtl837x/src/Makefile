ccflags-y += -DMDC_MDIO_OPERATION

ccflags-y += -I$(src)/rtk-api -I$(src)/rtk-api/dal/rtl8373 -Iinclude/linux/

obj-$(CONFIG_RTL837X_DSA) += rtl837x_dsa.o

rtl837x_dsa-objs := rtl837x_mdio.o
rtl837x_dsa-objs += rtl837x_dsa_ops.o
rtl837x_dsa-objs += rtl837x_dbg.o

ifeq ($(CONFIG_GPIOLIB),y)
rtl837x_dsa-objs += rtl837x_gpio.o
endif

rtl837x_dsa-objs += rtk-api/dal/dal_mgmt.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_cpuTag.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_drv.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_eee.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_gpio.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_igmp.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_isolation.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_led.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_lut.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_mapper.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_mib.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_phy.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_port.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_rma.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_stp.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_svlan.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_switch.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/dal_rtl8373_vlan.o
rtl837x_dsa-objs += rtk-api/dal/rtl8373/rtl8373_asicdrv.o
rtl837x_dsa-objs += rtk-api/chip.o
rtl837x_dsa-objs += rtk-api/cpuTag.o
rtl837x_dsa-objs += rtk-api/eee.o
rtl837x_dsa-objs += rtk-api/identify.o
rtl837x_dsa-objs += rtk-api/igmp.o
rtl837x_dsa-objs += rtk-api/isolation.o
rtl837x_dsa-objs += rtk-api/l2.o
rtl837x_dsa-objs += rtk-api/mib.o
rtl837x_dsa-objs += rtk-api/phy.o
rtl837x_dsa-objs += rtk-api/port.o
rtl837x_dsa-objs += rtk-api/rma.o
rtl837x_dsa-objs += rtk-api/rtk_switch.o
rtl837x_dsa-objs += rtk-api/vlan.o
