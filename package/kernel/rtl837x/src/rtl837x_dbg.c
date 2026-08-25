/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 StarField Xu <air_jinkela@163.com>
 */

#include <linux/uaccess.h>
#include <linux/trace_seq.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/u64_stats_sync.h>
#include <linux/slab.h>

#include "./rtl837x_common.h"

#include "./rtk-api/isolation.h"
#include "./rtk-api/rtk_error.h"
#include "./rtk-api/rtk_switch.h"
#include "./rtk-api/dal/rtl8373/rtl8373_asicdrv.h"

#define TO_FOPS(name) _##name##_rw_fops
#define RTL837X_DEBUG_REG 0
#define RTL837X_DEBUG_PHY 1
#define RTL837X_DEBUG_SDS 2
#define RTL837X_CONTEXT_BUFSIZE 2048

#define REGRWFUNC(name, slot)                                                                                              \
	static ssize_t _##name##_rw_read(struct file *filep, char __user *ubuf, size_t count, loff_t *offp)                \
	{                                                                                                                  \
		struct rtk_gsw *gsw = filep->private_data;                                                                 \
		return simple_read_from_buffer(ubuf, count, offp, gsw->debug_reply[slot], strlen(gsw->debug_reply[slot])); \
	}                                                                                                                  \
	extern ssize_t _##name##_rw_write(struct file *filep, const char __user *ubuf, size_t count, loff_t *offp);        \
	static const struct file_operations _##name##_rw_fops = { .owner = THIS_MODULE, .open = simple_open, .write = _##name##_rw_write, .read = _##name##_rw_read };

REGRWFUNC(reg, RTL837X_DEBUG_REG)
REGRWFUNC(phyreg_mmd, RTL837X_DEBUG_PHY)
REGRWFUNC(sdsreg, RTL837X_DEBUG_SDS)

ssize_t _sdsreg_rw_write(struct file *filep, const char __user *ubuf, size_t count, loff_t *offp)
{
	struct rtk_gsw *gsw = filep->private_data;
	char *buf;
	uint32_t sds_id, page, reg, val;
	ssize_t ret = count;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (buf[0] == 'w') {
		if (sscanf(buf, "w %d %x %x %x", &sds_id, &page, &reg, &val) != 4) {
			ret = -EINVAL;
			goto out;
		} else {
			if (sds_id > 1) {
				ret = -EINVAL;
				goto out;
			}
			rtl837x_sdk_lock(gsw);
			rtk_rtl8373_sds_reg_write(sds_id, page, reg, val);
			rtl837x_sdk_unlock(gsw);
		}
	} else if (buf[0] == 'r') {
		if (sscanf(buf, "r %d %x %x", &sds_id, &page, &reg) != 3) {
			ret = -EINVAL;
			goto out;
		} else {
			rtl837x_sdk_lock(gsw);
			rtk_rtl8373_sds_reg_read(sds_id, page, reg, &val);
			rtl837x_sdk_unlock(gsw);
			snprintf(gsw->debug_reply[RTL837X_DEBUG_SDS], sizeof(gsw->debug_reply[RTL837X_DEBUG_SDS]), "sds_id: %d, page: 0x%08x, reg: 0x%08x, val: 0x%08x\n", sds_id, page, reg, val);
		}
	} else {
		snprintf(gsw->debug_reply[RTL837X_DEBUG_SDS], sizeof(gsw->debug_reply[RTL837X_DEBUG_SDS]), "echo \"w/r <sds_id> <page> <reg> [<val>]\" > sdsreg\n");
	}

out:
	kfree(buf);
	return ret;
}

ssize_t _phyreg_mmd_rw_write(struct file *filep, const char __user *ubuf, size_t count, loff_t *offp)
{
	struct rtk_gsw *gsw = filep->private_data;
	char *buf;
	uint32_t port, devad, reg, val;
	ssize_t ret = count;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (buf[0] == 'w') {
		if (sscanf(buf, "w %d %x %x %x", &port, &devad, &reg, &val) != 4) {
			ret = -EINVAL;
			goto out;
		} else {
			if (port > 9) {
				ret = -EINVAL;
				goto out;
			}
			rtl837x_sdk_lock(gsw);
			rtk_port_phyReg_set(1 << port, devad, reg, val);
			rtl837x_sdk_unlock(gsw);
		}
	} else if (buf[0] == 'r') {
		if (sscanf(buf, "r %d %x %x", &port, &devad, &reg) != 3) {
			ret = -EINVAL;
			goto out;
		} else {
			rtl837x_sdk_lock(gsw);
			rtk_port_phyReg_get(port, devad, reg, &val);
			rtl837x_sdk_unlock(gsw);
			snprintf(gsw->debug_reply[RTL837X_DEBUG_PHY], sizeof(gsw->debug_reply[RTL837X_DEBUG_PHY]), "port: %d, devad: 0x%08x, reg: 0x%08x, val: 0x%08x\n", port, devad, reg, val);
		}
	} else {
		snprintf(gsw->debug_reply[RTL837X_DEBUG_PHY], sizeof(gsw->debug_reply[RTL837X_DEBUG_PHY]), "echo \"w/r <real_port_index> <devad> <reg> [<val>]\" > phy_mmd\n");
	}

out:
	kfree(buf);
	return ret;
}

ssize_t _reg_rw_write(struct file *filep, const char __user *ubuf, size_t count, loff_t *offp)
{
	struct rtk_gsw *gsw = filep->private_data;
	char *buf;
	uint32_t reg, val;
	ssize_t ret = count;

	if (*offp)
		return 0;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (buf[0] == 'w') {
		if (sscanf(buf, "w %x %x", &reg, &val) != 2) {
			ret = -EINVAL;
			goto out;
		} else {
			rtl837x_sdk_lock(gsw);
			rtk_rtl8373_setAsicReg(reg, val);
			rtl837x_sdk_unlock(gsw);
		}
	} else if (buf[0] == 'r') {
		if (sscanf(buf, "r %x", &reg) != 1) {
			ret = -EINVAL;
			goto out;
		} else {
			rtl837x_sdk_lock(gsw);
			rtk_rtl8373_getAsicReg(reg, &val);
			rtl837x_sdk_unlock(gsw);
			snprintf(gsw->debug_reply[RTL837X_DEBUG_REG], sizeof(gsw->debug_reply[RTL837X_DEBUG_REG]), "reg: 0x%08x, val: 0x%08x\n", reg, val);
		}
	} else {
		snprintf(gsw->debug_reply[RTL837X_DEBUG_REG], sizeof(gsw->debug_reply[RTL837X_DEBUG_REG]), "echo \"w/r <reg> [<val>]\" > reg\n");
	}

out:
	kfree(buf);
	return ret;
}

static ssize_t _sds_page_dump_read(struct file *filep, char __user *ubuf, size_t count, loff_t *offp)
{
	struct rtk_gsw *gsw = filep->private_data;
	char *buf;
	int len = 0;
	ssize_t ret;
	unsigned int v3;

	buf = kzalloc(4096, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rtl837x_sdk_lock(gsw);
	rtk_rtl8373_getAsicReg(RTL8373_SDS_MODE_SEL_ADDR, &v3);
	len += snprintf(buf + len, 4096 - len, "reg 0x7b20: %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x21, 0x10, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x21  reg 0x10; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x21, 0x13, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x21  reg 0x13; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x21, 0x18, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x21  reg 0x18; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x21, 0x1B, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x21  reg 0x1b; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x21, 0x1D, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x21  reg 0x1d; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x36, 0x1C, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x36  reg 0x1c; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x36, 0x14, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x36  reg 0x14; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x36, 0x10, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x36  reg 0x10; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 4, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x04; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 6, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x06; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 7, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x07; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 9, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x09; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0xB, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x0b; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0xC, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x0c; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0xD, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x0d; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0x15, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x15; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0x16, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x16; data = %#x\n", v3);
	rtk_rtl8373_sds_reg_read(0, 0x2E, 0x1D, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 0x2e  reg 0x1d; data = %#x\n", v3);

	rtk_rtl8373_sds_regbits_read(0, 5, 0, 1, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 5  reg 0; bit0 = %#x\n", v3);
	rtk_rtl8373_sds_regbits_read(0, 5, 1, 255, &v3);
	len += snprintf(buf + len, 4096 - len, "sds page 5  reg 1; bit7:0 = %#x\n", v3);
	rtl837x_sdk_unlock(gsw);

	ret = simple_read_from_buffer(ubuf, count, offp, buf, len);
	kfree(buf);

	return ret;
}

static const struct file_operations _sds_page_dump_fops = { .owner = THIS_MODULE, .open = simple_open, .read = _sds_page_dump_read };

static ssize_t rtl837x_context_read(struct file *filep, char __user *ubuf, size_t count, loff_t *offp)
{
	struct rtk_gsw *gsw = filep->private_data;
	char *buf;
	rtk_cpuTag_insertMode_t insert_mode = CPU_INSERT_END;
	rtk_enable_t cpu_tag = RTK_ENABLE_END;
	rtk_enable_t egr_filter = RTK_ENABLE_END;
	rtk_enable_t igr_filter;
	rtk_vlan_entry_t vlan1 = { 0 };
	rtk_portmask_t aware = { 0 };
	rtk_vlan_t pvid;
	u32 chip_id = 0;
	u32 ext_cpu = U32_MAX;
	u32 isolation;
	u32 sds_mode = 0;
	u32 tpid = U32_MAX;
	int port, ret;
	int len;
	ssize_t read;

	buf = kmalloc(RTL837X_CONTEXT_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rtl837x_sdk_lock(gsw);
	rtk_rtl8373_getAsicReg(0x4, &chip_id);
	rtk_rtl8373_getAsicReg(RTL8373_SDS_MODE_SEL_ADDR, &sds_mode);
	rtk_cpu_externalCpuPort_get(&ext_cpu);
	rtk_cpuTag_tpid_get(&tpid);
	rtk_cpuTag_enable_get(EXTERNAL_CPU, &cpu_tag);
	rtk_cpuTag_insertMode_get(EXTERNAL_CPU, &insert_mode);
	rtk_cpuTag_awarePort_get(&aware);
	rtk_vlan_egrFilterEnable_get(&egr_filter);
	rtk_vlan_get(1, &vlan1);
	rtl837x_sdk_unlock(gsw);

	len = scnprintf(buf, RTL837X_CONTEXT_BUFSIZE,
			"device=%s mdio=%u chip=%s raw-id=0x%08x cpu=%u cpu-sds=%d valid-ports=0x%03x configured-ports=0x%03x sds0=0x%x "
			"sds1=0x%x sds-ctrl=0x%08x selections=%lu\n"
			"mdio-reads=%llu mdio-writes=%llu timeouts=%llu last-op=%s last-reg=0x%04x last-value=0x%08x "
			"ctrl-before=0x%04x ctrl-after=0x%04x data-hi=0x%04x data-lo=0x%04x "
			"last-error=%d reset-gpio=%s reset-us=%u/%u preserve-boot=%u rtl8372n-led-init=%u quarantine=%u reinit-cpu-serdes=%u\n"
			"conduit=%s ready=%u dsa-registered=%u probe-attempts=%u last-probe-error=%d last-probe-id=0x%08x "
			"port-enable=%u port-disable=%u phy-status=%u\n",
			dev_name(gsw->dev), gsw->mdio_addr, gsw->chip_name ? gsw->chip_name : "unknown", chip_id, gsw->cpu_port, gsw->cpu_sds, gsw->valid_port_mask, gsw->configured_port_mask, gsw->sds0mode, gsw->sds1mode, sds_mode,
			gsw->sdk_select_count, (unsigned long long)gsw->mdio_reads, (unsigned long long)gsw->mdio_writes, (unsigned long long)gsw->mdio_timeouts, gsw->mdio_last_write ? "write" : "read", gsw->mdio_last_reg,
			gsw->mdio_last_value, gsw->mdio_last_ctrl_before, gsw->mdio_last_ctrl_after, gsw->mdio_last_data_high, gsw->mdio_last_data_low, gsw->mdio_last_error, gsw->reset_pin ? "yes" : "no", gsw->reset_assert_us,
			gsw->reset_deassert_us, gsw->preserve_boot_config, gsw->init_rtl8372n_leds, gsw->quarantine_before_conduit, gsw->reinit_cpu_serdes, gsw->conduit_name[0] ? gsw->conduit_name : "none", gsw->conduit_ready,
			gsw->dsa_registered, gsw->probe_attempts, gsw->last_probe_error, gsw->last_probe_id, gsw->port_enable_count, gsw->port_disable_count, gsw->phy_status_count);

	len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, "tagger=8021q ext-cpu=%u private-tag=%u insert=%u tpid=0x%04x\n", ext_cpu, cpu_tag, insert_mode, tpid);
	len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, "tagger-state aware=0x%03x egr-filter=%u vlan1=0x%03x/0x%03x\n", aware.bits[0], egr_filter, vlan1.mbr.bits[0], vlan1.untag.bits[0]);

	len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, "ports:");
	rtl837x_sdk_lock(gsw);
	for (port = 0; port < RTK_MAX_NUM_OF_PORT; port++) {
		if (!(gsw->configured_port_mask & BIT(port)))
			continue;

		pvid = U16_MAX;
		igr_filter = RTK_ENABLE_END;
		ret = rtk_port_isolation_get(port, &isolation);
		if (ret) {
			len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, " p%d=iso-error:%d", port, ret);
			continue;
		}
		rtk_vlan_portPvid_get(port, &pvid);
		rtk_vlan_portIgrFilterEnable_get(port, &igr_filter);

		len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, " p%d=%s,iso:0x%03x,pvid:%u,tag:%u/%u,bridge:%u/%u,igr:%u", port, port == gsw->cpu_port ? "cpu" : "user", isolation, pvid, gsw->tag8021q_pvid[port],
				 gsw->tag8021q_pvid_valid[port], gsw->bridge_pvid[port], gsw->bridge_pvid_valid[port], igr_filter);
	}
	rtl837x_sdk_unlock(gsw);
	len += scnprintf(buf + len, RTL837X_CONTEXT_BUFSIZE - len, "\n");

	read = simple_read_from_buffer(ubuf, count, offp, buf, len);
	kfree(buf);

	return read;
}

static const struct file_operations rtl837x_context_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = rtl837x_context_read,
};

int rtl837x_debug_proc_init(struct rtk_gsw *gsw)
{
	gsw->debugfs_parent = debugfs_create_dir(dev_name(gsw->dev), NULL);
	debugfs_create_file("reg", 0600, gsw->debugfs_parent, gsw, &TO_FOPS(reg));

	debugfs_create_file("phy_mmd", 0600, gsw->debugfs_parent, gsw, &TO_FOPS(phyreg_mmd));

	debugfs_create_file("sdsreg", 0600, gsw->debugfs_parent, gsw, &TO_FOPS(sdsreg));

	debugfs_create_file("sds_page_dump", 0400, gsw->debugfs_parent, gsw, &_sds_page_dump_fops);

	debugfs_create_file("context", 0400, gsw->debugfs_parent, gsw, &rtl837x_context_fops);

	return 0;
}

int rtl837x_debug_proc_deinit(struct rtk_gsw *gsw)
{
	debugfs_remove_recursive(gsw->debugfs_parent);
	return 0;
}
