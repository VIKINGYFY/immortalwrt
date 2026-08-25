/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#ifndef __RTL8372_COMMON_H__
#define __RTL8372_COMMON_H__

#include <linux/of_mdio.h>
#include <linux/regmap.h>
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
#include "./rtk-api/dal/rtl8373/rtl8373_reg_definition.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_led.h"
#include "./rtk-api/dal/dal_mapper.h"
#include "./rtk-api/dal/rtl8373/dal_rtl8373_mapper.h"

#define MDC_MDIO_CTRL_REG 21
#define MDC_MDIO_ADDR_REG 22
#define MDC_MDIO_DATA_LOW 23
#define MDC_MDIO_DATA_HIGH 24
#define MDC_MDIO_READ_CMD 0x1B
#define MDC_MDIO_WRITE_CMD 0x19

#define PORT_MAPPED(portx) (gsw->port_map[portx])
#define USE_SERDESMODE(sds_index, _mode)         \
	do {                                     \
		if ((sds_index) == 0)            \
			gsw->sds0mode = (_mode); \
		else if ((sds_index) == 1)       \
			gsw->sds1mode = (_mode); \
	} while (0)

struct rtl837x_mib_counter {
	uint16_t base;
	const char *name;
};

struct rtl837x_sdsmode_map {
	rtk_sds_mode_t mode;
	const char *name;
};

struct rtl837x_vlan_entry {
	u16 mbr;
	u16 untag;
};

typedef struct rtl837x_pnswap_cfg_s {
	uint8_t sds0_rx_swap : 1;
	uint8_t sds0_tx_swap : 1;

	uint8_t sds1_rx_swap : 1;
	uint8_t sds1_tx_swap : 1;

	uint8_t phy_mdi_reverse : 1;
	uint8_t phy_tx_polarity_swap : 1;
	uint8_t RESERVED : 2;
} rtl837x_pnswap_cfg_t;

struct rtk_gsw {
	struct device *dev;
	struct mii_bus *bus;

	struct regmap *map;
	struct mutex map_lock;

	struct gpio_desc *reset_pin;
	u32 reset_assert_us;
	u32 reset_deassert_us;
	int mdio_addr;
	u64 mdio_reads;
	u64 mdio_writes;
	u64 mdio_timeouts;
	u32 mdio_last_reg;
	u32 mdio_last_value;
	u16 mdio_last_data_low;
	u16 mdio_last_data_high;
	u16 mdio_last_ctrl_before;
	u16 mdio_last_ctrl_after;
	int mdio_last_error;
	bool mdio_last_write;
	bool preserve_boot_config;
	bool init_rtl8372n_leds;
	bool quarantine_before_conduit;
	bool reinit_cpu_serdes;
	bool conduit_ready;
	char conduit_name[IFNAMSIZ];
	u32 probe_attempts;
	u32 last_probe_id;
	int last_probe_error;

	const char *chip_name;
	switch_chip_t chip_id;
	const uint8_t *port_map;
	unsigned int num_ports;

	struct dentry *debugfs_parent;

	rtk_sds_mode_t sds0mode;
	rtk_sds_mode_t sds1mode;
	rtl837x_pnswap_cfg_t swap_cfg;

	unsigned int cpu_port;
	int cpu_sds;
	unsigned int legacy_cpu_port;
	bool cpu_port_from_dsa;
	u32 valid_port_mask;
	u32 configured_port_mask;
	unsigned int dsa_num_ports;
	bool dsa_registered;
	unsigned long sdk_select_count;
	u32 port_enable_count;
	u32 port_disable_count;
	u32 phy_status_count;
	char debug_reply[3][128];
	struct dsa_switch ds;
	struct net_device *ethernet_master;
	struct sfp_bus *sfp_bus;

	struct rtl837x_mib_counter *mib_counters;
	unsigned int num_mib_counters;

	dal_mapper_t *pMapper;

	struct rtl837x_vlan_entry vlan_table[4096];

	uint16_t port_pvid[RTK_MAX_NUM_OF_PORT]; // 端口PVID配置

	u16 tag8021q_pvid[RTK_MAX_NUM_OF_PORT];
	bool tag8021q_pvid_valid[RTK_MAX_NUM_OF_PORT];
	uint16_t bridge_pvid[RTK_MAX_NUM_OF_PORT];
	bool bridge_pvid_valid[RTK_MAX_NUM_OF_PORT];
};

extern int rtl8372n_hw_init(struct rtk_gsw *gsw, rtl837x_pnswap_cfg_t swap_cfg);

void rtl837x_sdk_lock(struct rtk_gsw *gsw);
void rtl837x_sdk_unlock(struct rtk_gsw *gsw);

extern int rtl837x_debug_proc_init(struct rtk_gsw *gsw);
extern int rtl837x_debug_proc_deinit(struct rtk_gsw *gsw);

extern int rtl837x_dsa_register(struct rtk_gsw *gsw);
extern void rtl837x_dsa_unregister(struct rtk_gsw *gsw);
extern void rtl837x_dsa_shutdown(struct rtk_gsw *gsw);
extern int rtl837x_gpiochip_init(struct rtk_gsw *gsw);

unsigned int mii_mgr_read(unsigned int phy_addr, unsigned int phy_register, unsigned int *read_data);

unsigned int mii_mgr_write(unsigned int phy_addr, unsigned int phy_register, unsigned int write_data);

#endif
