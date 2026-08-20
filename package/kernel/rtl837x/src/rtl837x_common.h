/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#ifndef __RTL8372_COMMON_H__
#define __RTL8372_COMMON_H__

#include <linux/of_mdio.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <net/dsa.h>

#include "./rtk-api/rtk_error.h"
#include "./rtk-api/rtk_types.h"
#include "./rtk-api/rtk_switch.h"
#include "./rtk-api/phy.h"
#include "./rtk-api/port.h"
#include "./rtk-api/vlan.h"
#include "./rtk-api/chip.h"
#include "./rtk-api/eee.h"
#include "./rtk-api/rma.h"
#include "./rtk-api/cpuTag.h"
#include "./rtk-api/mib.h"
#include "./rtk-api/isolation.h"
#include "./rtk-api/igmp.h"
#include "./rtk-api/gpio.h"
#include "./rtk-api/dal/rtl8373/rtl8373_asicdrv.h"
#include "./rtk-api/dal/dal_mapper.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_mapper.h"

#define MDC_MDIO_CTRL_REG           21
#define MDC_MDIO_ADDR_REG           22
#define MDC_MDIO_DATA_LOW           23
#define MDC_MDIO_DATA_HIGH          24
#define MDC_MDIO_READ_CMD           0x1B
#define MDC_MDIO_WRITE_CMD          0x19

#define PORT_MAPPED(portx) (gsw->port_map[portx])
#define USE_SERDESMODE(sds_index, _mode)                    \
	do {                                                    \
		if ((sds_index) == 0 && !gsw->force_set_serdes0_mode)       \
			gsw->sds0mode = (_mode);                        \
		else if ((sds_index) == 1 && !gsw->force_set_serdes1_mode)  \
			gsw->sds1mode = (_mode);                        \
	} while (0)

struct rtl837x_mib_counter {
	uint16_t	base;
	const char	*name;
};

struct rtl837x_sdsmode_map {
	rtk_sds_mode_t mode;
	const char *name;
};

typedef struct rtl837x_pnswap_cfg_s {
	uint8_t sds0_rx_swap:1;
	uint8_t sds0_tx_swap:1;

	uint8_t sds1_rx_swap:1;
	uint8_t sds1_tx_swap:1;

	uint8_t phy_mdi_reverse:1;
	uint8_t phy_tx_polarity_swap:1;
	uint8_t RESERVED:2;
} rtl837x_pnswap_cfg_t;

struct rtk_gsw {
 	struct device *dev;
 	struct mii_bus *bus;

	struct regmap		*map;
	struct regmap		*map_nolock;
	struct mutex		map_lock;

	struct gpio_desc *reset_pin;
	int mdio_addr;

	const char *chip_name;
	switch_chip_t chip_id;
	const uint8_t *port_map;
	unsigned int num_ports;

	struct dentry *debugfs_parent;

	bool force_set_serdes0_mode;
	bool force_set_serdes1_mode;
	rtk_sds_mode_t sds0mode;
	rtk_sds_mode_t sds1mode;
	rtl837x_pnswap_cfg_t swap_cfg;

	unsigned int cpu_port;
	unsigned int legacy_cpu_port;
	bool cpu_port_from_dsa;
	u32 valid_port_mask;
	unsigned int dsa_num_ports;
	bool dsa_registered;
	struct dsa_switch ds;
	struct net_device *bridge_dev[RTK_MAX_NUM_OF_PORT];
	bool port_enabled[RTK_MAX_NUM_OF_PORT];
	struct net_device *ethernet_master;
	struct sfp_bus *sfp_bus;

	struct rtl837x_mib_counter *mib_counters;
	unsigned int num_mib_counters;

	dal_mapper_t *pMapper;

	struct {
		uint8_t valid;  // 条目是否有效
		uint16_t vid;   // VLAN ID
		uint16_t mbr;   // 成员端口位图
		uint16_t untag; // 未标记端口位图
	} vlan_table[4096]; // VLAN 配置表

	char buf[4096];

	uint16_t port_pvid[RTK_MAX_NUM_OF_PORT];  // 端口PVID配置

	uint16_t tag8021q_pvid[RTK_MAX_NUM_OF_PORT];
	bool tag8021q_pvid_valid[RTK_MAX_NUM_OF_PORT];
	uint16_t bridge_pvid[RTK_MAX_NUM_OF_PORT];
	bool bridge_pvid_valid[RTK_MAX_NUM_OF_PORT];

	uint16_t flow_control_map; // 流控配置位图
	bool global_vlan_enable;

	int default_work_delay_ms;
	struct delayed_work status_check_work;
};

extern int rtl8372n_hw_init(struct rtk_gsw *gsw, rtl837x_pnswap_cfg_t swap_cfg);

extern int rtl837x_debug_proc_init(struct rtk_gsw *gsw);
extern int rtl837x_debug_proc_deinit(struct rtk_gsw *gsw);

extern int rtl837x_dsa_register(struct rtk_gsw *gsw);
extern void rtl837x_dsa_unregister(struct rtk_gsw *gsw);
extern void rtl837x_dsa_shutdown(struct rtk_gsw *gsw);
extern int rtl837x_gpiochip_init(struct rtk_gsw *gsw);

unsigned int mii_mgr_read(unsigned int phy_addr, 
		unsigned int phy_register, unsigned int *read_data);

unsigned int mii_mgr_write(unsigned int phy_addr, 
		unsigned int phy_register, unsigned int write_data);

#endif
