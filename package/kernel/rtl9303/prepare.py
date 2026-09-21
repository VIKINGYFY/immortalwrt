#!/usr/bin/env python3
"""Adapt the in-tree Otto PCS/MDIO providers to the sleeping SPI regmap.

The source files remain shared with the Realtek target. Assert each source
anchor so upstream changes fail the build instead of silently losing a fix.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])


def replace(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


for filename in ('mdio-rtl9303.c', 'mdio-rtl9303-serdes.c', 'pcs-rtl9303.c'):
    path = root / filename
    text = path.read_text()
    if filename == 'mdio-rtl9303.c':
        text = replace(text, 'syscon_node_to_regmap(pdev->dev.of_node->parent)',
                       'dev_get_regmap(dev->parent, NULL)')
        text = replace(text, 'if (IS_ERR(ctrl->map))\n\t\treturn PTR_ERR(ctrl->map);',
                       'if (!ctrl->map)\n\t\treturn -ENODEV;')
        text = replace(text, '"realtek,rtl9301-mdio"', '"realtek,rtl9303-spi-mdio"')
        text = replace(text, '.name = "mdio-rtl-otto"', '.name = "mdio-rtl9303"')
        text = replace(text, 'snprintf(bus->id, MII_BUS_ID_SIZE, "realtek-mdio-%d", smi_bus);',
                       'snprintf(bus->id, MII_BUS_ID_SIZE, "%s-%d", dev_name(dev), smi_bus);')
        # Unlike an integrated SoC, the external switch has an independently
        # initialized MoCA port. Touch only ports described by this device.
        text = replace(text, '\tret = rtmd_disable_polling(ctrl);\n\tif (ret)\n\t\treturn ret;\n\n'
                       '\tret = rtmd_map_ports(dev);', '\tret = rtmd_map_ports(dev);')
        text = replace(text, '\tret = rtmd_setup_smi_topology(ctrl);',
                       '\tret = rtmd_disable_polling(ctrl);\n\tif (ret)\n\t\treturn ret;\n\n'
                       '\tret = rtmd_setup_smi_topology(ctrl);')
        text = text.replace('for (pn = 0; pn < ctrl->cfg->num_ports; pn++) {',
                            'for (pn = 0; pn < ctrl->cfg->num_ports; pn++) {\n'
                            '\t\tif (!test_bit(pn, ctrl->phy_ports) &&\n'
                            '\t\t    !test_bit(pn, ctrl->sds_ports))\n\t\t\tcontinue;')
        # The three programmable 10G registers are GLOBAL, not per bus.
        # C22 RTL8221B ports must not overwrite the C45 Aquantia descriptors.
        # The same comment exists for RTL931x; only change the RTL930x function.
        start = text.index('static int rtmd_930x_setup_polling(')
        end = text.index('static int rtmd_931x_setup_ctrl(', start)
        part = text[start:end]
        part = replace(part, '\t\t/* Unique 10G polling setup enforced by hardware design. Always same 10G PHYs. */',
                       '\t\tif (!ctrl->bus[ctrl->port[pn].smi_bus].is_c45)\n'
                       '\t\t\tcontinue;\n\n'
                       '\t\t/* Global Clause 45 polling descriptors. */')
        text = text[:start] + part + text[end:]
        text = replace(text, '#include <linux/fwnode.h>', '#include <linux/jiffies.h>\n#include <linux/fwnode.h>')
        text = replace(text, 'struct rtmd_port {',
                       'struct rtmd_port {\n\tunsigned long diag_next;\n\tu16 diag_an;\n\tu8 diag_count;')
        anchor = '\treturn ret ? ret : val;\n}\n\nstatic int rtmd_read_c22'
        debug = """\t/* Observe phylib's reads; no extra latch-clearing PHY status reads. */
\tif (!ret && pn == 8 && devnum == MDIO_MMD_AN && regnum == MDIO_STAT1 &&
\t    ctrl->port[pn].diag_count < 16 &&
\t    (!ctrl->port[pn].diag_count ||
\t     (ctrl->port[pn].diag_an != val &&
\t      time_after_eq(jiffies, ctrl->port[pn].diag_next)))) {
\t\tdev_info(&bus->dev, "cr1000a-diag PHY port8 AN-status=%04x link=%u complete=%u (%u/16)\\n",
\t\t\t val, !!(val & MDIO_STAT1_LSTATUS), !!(val & MDIO_AN_STAT1_COMPLETE),
\t\t\t ++ctrl->port[pn].diag_count);
\t\tctrl->port[pn].diag_an = val;
\t\tctrl->port[pn].diag_next = jiffies + 5 * HZ;
\t}

"""
        text = replace(text, anchor, debug + anchor)
        anchor = '\tret = rtmd_enable_polling(ctrl);\n\tif (ret)\n\t\treturn ret;'
        text = replace(text, anchor, anchor + """

\tdev_info(dev, "cr1000a-diag Otto MDIO: Aquantia C45 polling, RTL8221B C22 polling\\n");""")
    else:
        text = replace(text, 'syscon_node_to_regmap(np->parent)',
                       'dev_get_regmap(dev->parent, NULL)')
        text = replace(text, 'if (IS_ERR(ctrl->map))\n\t\treturn PTR_ERR(ctrl->map);',
                       'if (!ctrl->map)\n\t\treturn -ENODEV;')
        text = text.replace('\tstruct device_node *np = pdev->dev.of_node;\n', '')
        if filename == 'mdio-rtl9303-serdes.c':
            text = replace(text, '"realtek,rtl9301-serdes-mdio"', '"realtek,rtl9303-spi-serdes"')
            text = replace(text, '.name = "realtek-otto-serdes-mdio"', '.name = "rtl9303-serdes-mdio"')
            text = replace(text, 'snprintf(bus->id, MII_BUS_ID_SIZE, "realtek-serdes-mdio");',
                           'snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));')
            text = replace(text, '\tregmap_write(ctrl->map, ctrl->cfg->base, op);',
                           '\tret = regmap_write(ctrl->map, ctrl->cfg->base, op);\n\tif (ret)\n\t\treturn ret;')
        else:
            text = replace(text, '"realtek,rtl9301-pcs"', '"realtek,rtl9303-spi-pcs"')
            text = replace(text, '.name = "realtek-otto-pcs"', '.name = "rtl9303-pcs"')
            text = replace(text, 'struct rtpcs_link {',
                           'struct rtpcs_link {\n\tunsigned long diag_next;\n'
                           '\tu32 diag_state;\n\tu8 diag_count;')
            text = replace(text, '/* Decode the Clause 37 modes directly and use the MAC-side mirror otherwise. */',
                           '#include "rtl9303-pcs-debug.h"\n\n'
                           '/* Decode the Clause 37 modes directly and use the MAC-side mirror otherwise. */')
            text = replace(text, '\t\trtpcs_pcs_get_state_mac(link, state);\n\t\tbreak;\n\t}\n',
                           '\t\trtpcs_pcs_get_state_mac(link, state);\n\t\tbreak;\n\t}\n'
                           '\trtl9303_pcs_debug(link, state);\n')
    path.write_text(text)
