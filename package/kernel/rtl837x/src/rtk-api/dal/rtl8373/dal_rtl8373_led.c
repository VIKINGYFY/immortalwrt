/*
 * Copyright (C) 2013 Realtek Semiconductor Corp.
 * All Rights Reserved.
 *
 * This program is the proprietary software of Realtek Semiconductor
 * Corporation and/or its licensors, and only be used, duplicated,
 * modified or distributed under the authorized license from Realtek.
 *
 * ANY USE OF THE SOFTWARE OTHER THAN AS AUTHORIZED UNDER
 * THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 *
 * $Revision$
 * $Date$
 *
 * Purpose : RTK switch high-level API for RTL8373
 * Feature : Here is a list of all functions and variables in LED module.
 *
 */

#include <rtk_switch.h>
#include <rtk_error.h>
#include <dal_rtl8373_led.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <rtl8373_reg_definition.h>
#include <rtl8373_asicdrv.h>

struct rtl8373_led_reg_step {
	rtk_uint32 addr;
	rtk_uint32 mask;
	rtk_uint32 value;
};

rtk_api_ret_t dal_rtl8373_led_default_init(void)
{
	static const struct rtl8373_led_reg_step steps[] = {
		{ RTL8373_LED_GLB_MUX_1_ADDR, 0x0000003f, 0x00000000 },
		{ RTL8373_LED_GLB_MUX_1_ADDR, 0x00000fc0, 0x00000040 },
		{ RTL8373_LED_GLB_MUX_1_ADDR, 0x0003f000, 0x00004000 },
		{ RTL8373_LED_GLB_MUX_1_ADDR, 0x00fc0000, 0x00140000 },
		{ RTL8373_LED_GLB_MUX_1_ADDR, 0x3f000000, 0x10000000 },
		{ RTL8373_LED_GLB_MUX_2_ADDR, 0x0000003f, 0x00000009 },
		{ RTL8373_LED_GLB_MUX_2_ADDR, 0x00000fc0, 0x00000300 },
		{ RTL8373_LED_GLB_MUX_2_ADDR, 0x00fc0000, 0x00500000 },
		{ RTL8373_LED_GLB_MUX_2_ADDR, 0x3f000000, 0x10000000 },
		{ RTL8373_LED_GLB_MUX_3_ADDR, 0x0000003f, 0x00000018 },
		{ RTL8373_LED_GLB_MUX_3_ADDR, 0x0003f000, 0x00014000 },
		{ RTL8373_LED_GLB_MUX_4_ADDR, 0x0000003f, 0x00000015 },
		{ RTL8373_LED_GLB_MUX_4_ADDR, 0x00fc0000, 0x00600000 },
		{ RTL8373_LED_GLB_MUX_5_ADDR, 0x00000fc0, 0x00000700 },
		{ RTL8373_LED_GLB_MUX_5_ADDR, 0x3f000000, 0x1c000000 },
		{ RTL8373_LED_GLB_MUX_6_ADDR, 0x0000003f, 0x0000001d },
		{ RTL8373_LED_GLB_MUX_6_ADDR, 0x00000fc0, 0x00000800 },
		{ RTL8373_LED_GLB_MUX_6_ADDR, 0x0003f000, 0x00021000 },
		{ RTL8373_LED_GLB_ACTIVE_ADDR, BIT(4), BIT(4) },
		{ RTL8373_LED_GLB_ACTIVE_ADDR, BIT(8), BIT(8) },
		{ RTL8373_LED_GLB_ACTIVE_ADDR, BIT(10), BIT(10) },
		{ RTL8373_LED_GLB_ACTIVE_ADDR, BIT(21), BIT(21) },
		{ RTL8373_LED_GLB_CTRL_ADDR, 0x00001e00, 0x00000400 },
		{ RTL8373_LED_GLB_CTRL_ADDR, 0x000001e0, 0x00000020 },
		{ RTL8373_LED_GLB_CTRL_ADDR, 0x0000000c, 0x00000004 },
		{ RTL8373_LED_RLDP_CTRL_1_ADDR, 0x00000003, 0x00000003 },
		{ RTL8373_LED_RLDP_CTRL_2_ADDR, 0xffffffff, 0x33333000 },
		{ RTL8373_LED_RLDP_CTRL_3_ADDR, 0x0000000f, 0x00000003 },
		{ RTL8373_LED_GLB_IO_EN_ADDR, 0x3fffffff, 0x30200510 },
		{ RTL8373_IO_MUX_SEL_0_ADDR, 0x3fffffff, 0x3fdffaef },
		{ RTL8373_LED_PORT_SET_SEL_CTRL_ADDR(0), 0x0002ffff, 0x00000000 },
		{ RTL8373_LED1_0_SET0_CTRL0_ADDR, 0x0000ffff, 0x00000175 },
		{ RTL8373_LED1_0_SET0_CTRL0_ADDR, 0xffff0000, 0x01400000 },
		{ RTL8373_LED3_0_SET1_0_CTRL1_ADDR, 0x000000ff, 0x00000000 },
		{ RTL8373_LED_GLB_IO_EN_ADDR, RTL8373_LED_GLB_IO_EN_LED_PAD_EN_MASK, RTL8373_LED_GLB_IO_EN_LED_PAD_EN_MASK },
	};
	rtk_api_ret_t ret;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		/* This table stores raw register values recovered from a masked
		 * read-modify-write sequence. The SDK helper expects an unshifted
		 * field value and applies the mask shift itself.
		 */
		ret = rtl8373_setAsicRegBits(steps[i].addr, steps[i].mask, steps[i].value >> __ffs(steps[i].mask));
		if (ret != RT_ERR_OK)
			return ret;
	}

	return RT_ERR_OK;
}

/* Function Name:
 *      dal_rtl8373_led_config_set
 * Description:
 *      Set Led congiuration
 * Input:
 *      setid       -  4 groups led setting 0 ~ 3
 *      ledid       -  4 leds, 0 ~ 3
 *      pConfig  .
 *                      led_2p5g;
 *                      led_two_pair_1g;
 *                      led_1g;
 *                      led_500m;
 *                      led_100m;
 *                      led_10m;
 *                      led_link;
 *                      led_link_flash;
 *                      led_act;
 *                      led_rx;
 *                      led_tx;
 *                      led_col;
 *                      led_duplex;
 *                      led_training;
 *                      led_master;
 *                      led_10g;
 *                      led_two_pair_5g;
 *                      led_5g;
 *                      led_two_pair_2p5g;
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK           - OK
 *      RT_ERR_FAILED       - Failed
 *      RT_ERR_SMI          - SMI access error
 *      RT_ERR_INPUT        - Invalid input parameters.
 *      RT_ERR_PORT_MASK    - Error portmask
 * Note:
 *      The API can be used to enable LED per port per group.
 */
rtk_api_ret_t dal_rtl8373_led_config_set(rtk_led_set_t setid, rtk_uint32 ledid, rtk_led_config_t *pConfig)
{
	rtk_api_ret_t retVal;
	rtk_uint32 sel0, sel1;

	/* Check initialization state */
	RTK_CHK_INIT_STATE();

	if (pConfig == NULL)
		return RT_ERR_NULL_POINTER;
	sel0 = 0;
	sel1 = 0;
	if (pConfig->led_2p5g)
		sel0 |= 1;
	if (pConfig->led_two_pair_1g)
		sel0 |= (1 << 1);
	if (pConfig->led_1g)
		sel0 |= (1 << 2);
	if (pConfig->led_500m)
		sel0 |= (1 << 3);
	if (pConfig->led_100m)
		sel0 |= (1 << 4);
	if (pConfig->led_10m)
		sel0 |= (1 << 5);
	if (pConfig->led_link)
		sel0 |= (1 << 6);
	if (pConfig->led_link_flash)
		sel0 |= (1 << 7);
	if (pConfig->led_act)
		sel0 |= (1 << 8);
	if (pConfig->led_rx)
		sel0 |= (1 << 9);
	if (pConfig->led_tx)
		sel0 |= (1 << 10);
	if (pConfig->led_col)
		sel0 |= (1 << 11);
	if (pConfig->led_duplex)
		sel0 |= (1 << 12);
	if (pConfig->led_training)
		sel0 |= (1 << 13);
	if (pConfig->led_master)
		sel0 |= (1 << 14);
	if (pConfig->led_10g)
		sel1 |= (1 << 0);
	if (pConfig->led_two_pair_5g)
		sel1 |= (1 << 1);
	if (pConfig->led_5g)
		sel1 |= (1 << 2);
	if (pConfig->led_two_pair_2p5g)
		sel1 |= (1 << 3);

	if (setid == LED_SET_0) {
		if (ledid == 0) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET0_CTRL0_ADDR, RTL8373_LED1_0_SET0_CTRL0_SET0_LED0_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET0_LED0_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 1) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET0_CTRL0_ADDR, RTL8373_LED1_0_SET0_CTRL0_SET0_LED1_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET0_LED1_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;

		} else if (ledid == 2) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET0_CTRL0_ADDR, RTL8373_LED3_2_SET0_CTRL0_SET0_LED2_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET0_LED2_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;

		} else if (ledid == 3) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET0_CTRL0_ADDR, RTL8373_LED3_2_SET0_CTRL0_SET0_LED3_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET0_LED3_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;

		} else
			return RT_ERR_INPUT;
	} else if (setid == LED_SET_1) {
		if (ledid == 0) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET1_CTRL0_ADDR, RTL8373_LED1_0_SET1_CTRL0_SET1_LED0_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET1_LED0_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 1) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET1_CTRL0_ADDR, RTL8373_LED1_0_SET1_CTRL0_SET1_LED1_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET1_LED1_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 2) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET1_CTRL0_ADDR, RTL8373_LED3_2_SET1_CTRL0_SET1_LED2_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET1_LED2_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 3) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET1_CTRL0_ADDR, RTL8373_LED3_2_SET1_CTRL0_SET1_LED3_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET1_0_CTRL1_ADDR, RTL8373_LED3_0_SET1_0_CTRL1_SET1_LED3_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else
			return RT_ERR_INPUT;
	} else if (setid == LED_SET_2) {
		if (ledid == 0) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET2_CTRL0_ADDR, RTL8373_LED1_0_SET2_CTRL0_SET2_LED0_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET2_LED0_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 1) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET2_CTRL0_ADDR, RTL8373_LED1_0_SET2_CTRL0_SET2_LED1_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET2_LED1_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 2) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET2_CTRL0_ADDR, RTL8373_LED3_2_SET2_CTRL0_SET2_LED2_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET2_LED2_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 3) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET2_CTRL0_ADDR, RTL8373_LED3_2_SET2_CTRL0_SET2_LED3_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET2_LED3_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else
			return RT_ERR_INPUT;
	} else if (setid == LED_SET_3) {
		if (ledid == 0) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET3_CTRL0_ADDR, RTL8373_LED1_0_SET3_CTRL0_SET3_LED0_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET3_LED0_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 1) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED1_0_SET3_CTRL0_ADDR, RTL8373_LED1_0_SET3_CTRL0_SET3_LED1_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET3_LED1_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 2) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET3_CTRL0_ADDR, RTL8373_LED3_2_SET3_CTRL0_SET3_LED2_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET3_LED2_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else if (ledid == 3) {
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_2_SET3_CTRL0_ADDR, RTL8373_LED3_2_SET3_CTRL0_SET3_LED3_SEL0_MASK, sel0)) != RT_ERR_OK)
				return retVal;
			if ((retVal = rtl8373_setAsicRegBits(RTL8373_LED3_0_SET3_2_CTRL1_ADDR, RTL8373_LED3_0_SET3_2_CTRL1_SET3_LED3_SEL1_MASK, sel1)) != RT_ERR_OK)
				return retVal;
		} else
			return RT_ERR_INPUT;
	} else
		return RT_ERR_INPUT;

	return RT_ERR_OK;
}

/* Function Name:
 *      dal_rtl8373_portLedConfig_set
 * Description:
 *      Select led config for per port
 * Input:
 *      port       -  port 0 ~ 8
 *      setid       -  4 leds, 0 ~ 3
 *      None
 * Return:
 *      RT_ERR_OK           - OK
 *      RT_ERR_FAILED       - Failed
 *      RT_ERR_SMI          - SMI access error
 *      RT_ERR_INPUT        - Invalid input parameters.
 *      RT_ERR_PORT_MASK    - Error portmask
 * Note:
 *      The API can be used to set LED per port config.
 */
rtk_api_ret_t dal_rtl8373_portLedConfig_set(rtk_port_t port, rtk_led_set_t setid)
{
	return rtl8373_setAsicRegBits(RTL8373_LED_PORT_SET_SEL_CTRL_ADDR(port), RTL8373_LED_PORT_SET_SEL_CTRL_LED_SET_PSEL_MASK(port), setid);
}

/* Function Name:
 *      dal_rtl8373_portLedConfig_get
 * Description:
 *      Get led config for per port
 * Input:
 *      port       -  port 0 ~ 8
 *      setid       -  4 leds, 0 ~ 3
 *      None
 * Return:
 *      RT_ERR_OK           - OK
 *      RT_ERR_FAILED       - Failed
 *      RT_ERR_SMI          - SMI access error
 *      RT_ERR_INPUT        - Invalid input parameters.
 *      RT_ERR_PORT_MASK    - Error portmask
 * Note:
 *      The API can be used to get  LED per port config.
 */
rtk_api_ret_t dal_rtl8373_portLedConfig_get(rtk_port_t port, rtk_led_set_t *pSetid)
{
	return rtl8373_getAsicRegBits(RTL8373_LED_PORT_SET_SEL_CTRL_ADDR(port), RTL8373_LED_PORT_SET_SEL_CTRL_LED_SET_PSEL_MASK(port), pSetid);
}

/* Function Name:
 *      dal_rtl8373_led_blinkRate_set
 * Description:
 *      Set Led blink rate
 * Input:
 *      rate      -
 *                   LED_BLINKRATE_32MS=1,
 *                   LED_BLINKRATE_64MS,
 *                   LED_BLINKRATE_128MS,
 *                   LED_BLINKRATE_256MS,
 *                   LED_BLINKRATE_512MS,
 *                   LED_BLINKRATE_1024MS,.
 *                   LED_BLINKRATE_END, *      None
 * Return:
 *      RT_ERR_OK           - OK
 *      RT_ERR_FAILED       - Failed
 *      RT_ERR_SMI          - SMI access error
 *      RT_ERR_INPUT        - Invalid input parameters.
 *      RT_ERR_PORT_MASK    - Error portmask
 * Note:
 *      The API can be used to enable LED per port per group.
 */
rtk_api_ret_t dal_rtl8373_led_blinkRate_set(rtk_led_blink_rate_t rate)
{
	return rtl8373_setAsicRegBits(RTL8373_LED_GLB_CTRL_ADDR, RTL8373_LED_GLB_CTRL_BLINK_TIME_SEL_MASK, rate);
}

/* Function Name:
 *      dal_rtl8373_led_blinkRate_get
 * Description:
 *      Set Led blink rate
 * Input:
 *      pRate      -
 *                   LED_BLINKRATE_32MS=1,
 *                   LED_BLINKRATE_64MS,
 *                   LED_BLINKRATE_128MS,
 *                   LED_BLINKRATE_256MS,
 *                   LED_BLINKRATE_512MS,
 *                   LED_BLINKRATE_1024MS,.
 *                   LED_BLINKRATE_END, *      None
 * Return:
 *      RT_ERR_OK           - OK
 *      RT_ERR_FAILED       - Failed
 *      RT_ERR_SMI          - SMI access error
 *      RT_ERR_INPUT        - Invalid input parameters.
 *      RT_ERR_PORT_MASK    - Error portmask
 * Note:
 *      The API can be used to enable LED per port per group.
 */
rtk_api_ret_t dal_rtl8373_led_blinkRate_get(rtk_led_blink_rate_t *pRate)
{
	return rtl8373_getAsicRegBits(RTL8373_LED_GLB_CTRL_ADDR, RTL8373_LED_GLB_CTRL_BLINK_TIME_SEL_MASK, pRate);
}
