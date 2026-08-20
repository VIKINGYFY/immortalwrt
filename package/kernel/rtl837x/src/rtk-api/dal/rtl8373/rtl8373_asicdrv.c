/*
 * Copyright (C) 2019 Realtek Semiconductor Corp.
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
 * Purpose : RTL8373 switch high-level API for RTL8373
 * Feature :
 *
 */

#include <linux/bitops.h>
#include <linux/regmap.h>
#include <rtl8373_asicdrv.h>

#include "../../../rtl837x_common.h"

// we need refactoring here to avoid global variable
struct rtk_gsw *rtl_gbl_priv;

/* Function Name:
 *      rtl8373_setAsicRegBit
 * Description:
 *      Set a bit value of a specified register
 * Input:
 *      reg     - register's address
 *      offset     - offset location
 *      value   - value to set. It can be value 0 or 1.
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 *      RT_ERR_INPUT    - Invalid input parameter
 * Note:
 *      Set a bit of a specified register to 1 or 0.
 */
ret_t rtl8373_setAsicRegBit(rtk_uint32 reg, rtk_uint32 offset, rtk_uint32 value)
{
    int ret;

    if(value)
        ret = regmap_set_bits(rtl_gbl_priv->map, reg, BIT(offset));
    else
        ret = regmap_clear_bits(rtl_gbl_priv->map, reg, BIT(offset));

    if (ret)
        return RT_ERR_SMI;
    return RT_ERR_OK;
}
/* Function Name:
 *      rtl8373_getAsicRegBit
 * Description:
 *      Get a bit value of a specified register
 * Input:
 *      reg     - register's address
 *      offset     - bit location
 *      value   - value to get.
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 *      RT_ERR_INPUT    - Invalid input parameter
 * Note:
 *      None
 */
ret_t rtl8373_getAsicRegBit(rtk_uint32 reg, rtk_uint32 offset, rtk_uint32 *pValue)
{
    int ret;
    u32 val;

    ret = regmap_read(rtl_gbl_priv->map, reg, &val);

    if (ret)
        return RT_ERR_SMI;

    *pValue = !!(val & (0x1 << offset));

    return RT_ERR_OK;
}
/* Function Name:
 *      rtl8373_setAsicRegBits
 * Description:
 *      Set bits value of a specified register
 * Input:
 *      reg     - register's address
 *      bits    - bits mask for setting
 *      value   - bits value for setting
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 *      RT_ERR_INPUT    - Invalid input parameter
 * Note:
 *      Set bits of a specified register to value. Both bits and value are be treated as bit-mask
 */
ret_t rtl8373_setAsicRegBits(rtk_uint32 reg, rtk_uint32 bitsMask, rtk_uint32 value)
{
    int ret;

    ret = regmap_update_bits(rtl_gbl_priv->map, reg, bitsMask, (value << __ffs(bitsMask)) & bitsMask);
    if (ret)
        return RT_ERR_SMI;
    return RT_ERR_OK;
}
/* Function Name:
 *      rtl8373_getAsicRegBits
 * Description:
 *      Get bits value of a specified register
 * Input:
 *      reg     - register's address
 *      bits    - bits mask for setting
 *      value   - bits value for setting
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 *      RT_ERR_INPUT    - Invalid input parameter
 * Note:
 *      None
 */
ret_t rtl8373_getAsicRegBits(rtk_uint32 reg, rtk_uint32 bitsMask, rtk_uint32 *pValue)
{
    int ret;
    u32 val;

    ret = regmap_read(rtl_gbl_priv->map, reg, &val);

    if (ret)
        return RT_ERR_SMI;

    *pValue = (val & bitsMask) >> __ffs(bitsMask);
    return RT_ERR_OK;
}
/* Function Name:
 *      rtl8373_setAsicReg
 * Description:
 *      Set content of asic register
 * Input:
 *      reg     - register's address
 *      value   - Value setting to register
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 * Note:
 *      The value will be set to ASIC mapping address only and it is always return RT_ERR_OK while setting un-mapping address registers
 */
ret_t rtl8373_setAsicReg(rtk_uint32 reg, rtk_uint32 value)
{
    int ret;

    ret = regmap_write(rtl_gbl_priv->map, reg, value);

    if (ret)
        return RT_ERR_SMI;
    return RT_ERR_OK;
}
/* Function Name:
 *      rtl8373_getAsicReg
 * Description:
 *      Get content of asic register
 * Input:
 *      reg     - register's address
 *      value   - Value setting to register
 * Output:
 *      None
 * Return:
 *      RT_ERR_OK       - Success
 *      RT_ERR_SMI      - SMI access error
 * Note:
 *      Value 0x0000 will be returned for ASIC un-mapping address
 */
ret_t rtl8373_getAsicReg(rtk_uint32 reg, rtk_uint32 *pValue)
{
    int ret;
    u32 val;

    ret = regmap_read(rtl_gbl_priv->map, reg, &val);

    if (ret)
        return RT_ERR_SMI;

    *pValue = val;
    return RT_ERR_OK;
}
