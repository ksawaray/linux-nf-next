// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_address.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/if_vlan.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <linux/interrupt.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/phylink.h>
#include <linux/pcs/pcs-mtk-lynxi.h>
#include <linux/jhash.h>
#include <linux/bitfield.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <net/page_pool/helpers.h>
#include <linux/genalloc.h>

#include "mtk_eth_soc.h"
#include "mtk_wed.h"

static int mtk_msg_level = -1;
module_param_named(msg_level, mtk_msg_level, int, 0);
MODULE_PARM_DESC(msg_level, "Message level (-1=defaults,0=none,...,16=all)");

#define MTK_ETHTOOL_STAT(x) { #x, \
			      offsetof(struct mtk_hw_stats, x) / sizeof(u64) }

#define MTK_ETHTOOL_XDP_STAT(x) { #x, \
				  offsetof(struct mtk_hw_stats, xdp_stats.x) / \
				  sizeof(u64) }

static const struct mtk_reg_map mtk_reg_map = {
	.tx_irq_mask		= 0x1a1c,
	.tx_irq_status		= 0x1a18,
	.pdma = {
		.rx_ptr		= 0x0900,
		.rx_cnt_cfg	= 0x0904,
		.pcrx_ptr	= 0x0908,
		.glo_cfg	= 0x0a04,
		.rst_idx	= 0x0a08,
		.delay_irq	= 0x0a0c,
		.irq_status	= 0x0a20,
		.irq_mask	= 0x0a28,
		.adma_rx_dbg0	= 0x0a38,
		.int_grp	= 0x0a50,
	},
	.qdma = {
		.qtx_cfg	= 0x1800,
		.qtx_sch	= 0x1804,
		.rx_ptr		= 0x1900,
		.rx_cnt_cfg	= 0x1904,
		.qcrx_ptr	= 0x1908,
		.glo_cfg	= 0x1a04,
		.rst_idx	= 0x1a08,
		.delay_irq	= 0x1a0c,
		.fc_th		= 0x1a10,
		.tx_sch_rate	= 0x1a14,
		.int_grp	= 0x1a20,
		.hred		= 0x1a44,
		.ctx_ptr	= 0x1b00,
		.dtx_ptr	= 0x1b04,
		.crx_ptr	= 0x1b10,
		.drx_ptr	= 0x1b14,
		.fq_head	= 0x1b20,
		.fq_tail	= 0x1b24,
		.fq_count	= 0x1b28,
		.fq_blen	= 0x1b2c,
	},
	.gdm1_cnt		= 0x2400,
	.gdma_to_ppe	= {
		[0]		= 0x4444,
	},
	.ppe_base		= 0x0c00,
	.wdma_base = {
		[0]		= 0x2800,
		[1]		= 0x2c00,
	},
	.pse_iq_sta		= 0x0110,
	.pse_oq_sta		= 0x0118,
};

static const struct mtk_reg_map mt7628_reg_map = {
	.tx_irq_mask		= 0x0a28,
	.tx_irq_status		= 0x0a20,
	.pdma = {
		.rx_ptr		= 0x0900,
		.rx_cnt_cfg	= 0x0904,
		.pcrx_ptr	= 0x0908,
		.glo_cfg	= 0x0a04,
		.rst_idx	= 0x0a08,
		.delay_irq	= 0x0a0c,
		.irq_status	= 0x0a20,
		.irq_mask	= 0x0a28,
		.int_grp	= 0x0a50,
	},
};

static const struct mtk_reg_map mt7986_reg_map = {
	.tx_irq_mask		= 0x461c,
	.tx_irq_status		= 0x4618,
	.pdma = {
		.rx_ptr		= 0x4100,
		.rx_cnt_cfg	= 0x4104,
		.pcrx_ptr	= 0x4108,
		.glo_cfg	= 0x4204,
		.rst_idx	= 0x4208,
		.delay_irq	= 0x420c,
		.irq_status	= 0x4220,
		.irq_mask	= 0x4228,
		.adma_rx_dbg0	= 0x4238,
		.int_grp	= 0x4250,
	},
	.qdma = {
		.qtx_cfg	= 0x4400,
		.qtx_sch	= 0x4404,
		.rx_ptr		= 0x4500,
		.rx_cnt_cfg	= 0x4504,
		.qcrx_ptr	= 0x4508,
		.glo_cfg	= 0x4604,
		.rst_idx	= 0x4608,
		.delay_irq	= 0x460c,
		.fc_th		= 0x4610,
		.int_grp	= 0x4620,
		.hred		= 0x4644,
		.ctx_ptr	= 0x4700,
		.dtx_ptr	= 0x4704,
		.crx_ptr	= 0x4710,
		.drx_ptr	= 0x4714,
		.fq_head	= 0x4720,
		.fq_tail	= 0x4724,
		.fq_count	= 0x4728,
		.fq_blen	= 0x472c,
		.tx_sch_rate	= 0x4798,
	},
	.gdm1_cnt		= 0x1c00,
	.gdma_to_ppe	= {
		[0]		= 0x3333,
		[1]		= 0x4444,
	},
	.ppe_base		= 0x2000,
	.wdma_base = {
		[0]		= 0x4800,
		[1]		= 0x4c00,
	},
	.pse_iq_sta		= 0x0180,
	.pse_oq_sta		= 0x01a0,
};

static const struct mtk_reg_map mt7988_reg_map = {
	.tx_irq_mask		= 0x461c,
	.tx_irq_status		= 0x4618,
	.pdma = {
		.rx_ptr		= 0x6900,
		.rx_cnt_cfg	= 0x6904,
		.pcrx_ptr	= 0x6908,
		.glo_cfg	= 0x6a04,
		.rst_idx	= 0x6a08,
		.delay_irq	= 0x6a0c,
		.irq_status	= 0x6a20,
		.irq_mask	= 0x6a28,
		.adma_rx_dbg0	= 0x6a38,
		.int_grp	= 0x6a50,
	},
	.qdma = {
		.qtx_cfg	= 0x4400,
		.qtx_sch	= 0x4404,
		.rx_ptr		= 0x4500,
		.rx_cnt_cfg	= 0x4504,
		.qcrx_ptr	= 0x4508,
		.glo_cfg	= 0x4604,
		.rst_idx	= 0x4608,
		.delay_irq	= 0x460c,
		.fc_th		= 0x4610,
		.int_grp	= 0x4620,
		.hred		= 0x4644,
		.ctx_ptr	= 0x4700,
		.dtx_ptr	= 0x4704,
		.crx_ptr	= 0x4710,
		.drx_ptr	= 0x4714,
		.fq_head	= 0x4720,
		.fq_tail	= 0x4724,
		.fq_count	= 0x4728,
		.fq_blen	= 0x472c,
		.tx_sch_rate	= 0x4798,
	},
	.gdm1_cnt		= 0x1c00,
	.gdma_to_ppe	= {
		[0]		= 0x3333,
		[1]		= 0x4444,
		[2]		= 0xcccc,
	},
	.ppe_base		= 0x2000,
	.wdma_base = {
		[0]		= 0x4800,
		[1]		= 0x4c00,
		[2]		= 0x5000,
	},
	.pse_iq_sta		= 0x0180,
	.pse_oq_sta		= 0x01a0,
};

/* strings used by ethtool */
static const struct mtk_ethtool_stats {
	char str[ETH_GSTRING_LEN];
	u32 offset;
} mtk_ethtool_stats[] = {
	MTK_ETHTOOL_STAT(tx_bytes),
	MTK_ETHTOOL_STAT(tx_packets),
	MTK_ETHTOOL_STAT(tx_skip),
	MTK_ETHTOOL_STAT(tx_collisions),
	MTK_ETHTOOL_STAT(rx_bytes),
	MTK_ETHTOOL_STAT(rx_packets),
	MTK_ETHTOOL_STAT(rx_overflow),
	MTK_ETHTOOL_STAT(rx_fcs_errors),
	MTK_ETHTOOL_STAT(rx_short_errors),
	MTK_ETHTOOL_STAT(rx_long_errors),
	MTK_ETHTOOL_STAT(rx_checksum_errors),
	MTK_ETHTOOL_STAT(rx_flow_control_packets),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_redirect),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_pass),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_drop),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_tx),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_tx_errors),
	MTK_ETHTOOL_XDP_STAT(tx_xdp_xmit),
	MTK_ETHTOOL_XDP_STAT(tx_xdp_xmit_errors),
};

static const char * const mtk_clks_source_name[] = {
	"ethif",
	"sgmiitop",
	"esw",
	"gp0",
	"gp1",
	"gp2",
	"gp3",
	"xgp1",
	"xgp2",
	"xgp3",
	"crypto",
	"fe",
	"trgpll",
	"sgmii_tx250m",
	"sgmii_rx250m",
	"sgmii_cdr_ref",
	"sgmii_cdr_fb",
	"sgmii2_tx250m",
	"sgmii2_rx250m",
	"sgmii2_cdr_ref",
	"sgmii2_cdr_fb",
	"sgmii_ck",
	"eth2pll",
	"wocpu0",
	"wocpu1",
	"netsys0",
	"netsys1",
	"ethwarp_wocpu2",
	"ethwarp_wocpu1",
	"ethwarp_wocpu0",
	"top_sgm0_sel",
	"top_sgm1_sel",
	"top_eth_gmii_sel",
	"top_eth_refck_50m_sel",
	"top_eth_sys_200m_sel",
	"top_eth_sys_sel",
	"top_eth_xgmii_sel",
	"top_eth_mii_sel",
	"top_netsys_sel",
	"top_netsys_500m_sel",
	"top_netsys_pao_2x_sel",
	"top_netsys_sync_250m_sel",
	"top_netsys_ppefb_250m_sel",
	"top_netsys_warp_sel",
};

void mtk_w32(struct mtk_eth *eth, u32 val, unsigned reg)
{
	__raw_writel(val, eth->base + reg);
}

u32 mtk_r32(struct mtk_eth *eth, unsigned reg)
{
	return __raw_readl(eth->base + reg);
}

u32 mtk_m32(struct mtk_eth *eth, u32 mask, u32 set, unsigned int reg)
{
	u32 val;

	val = mtk_r32(eth, reg);
	val &= ~mask;
	val |= set;
	mtk_w32(eth, val, reg);
	return reg;
}

static int mtk_mdio_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;

	while (1) {
		if (!(mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_ACCESS))
			return 0;
		if (time_after(jiffies, t_start + PHY_IAC_TIMEOUT))
			break;
		cond_resched();
	}

	dev_err(eth->dev, "mdio: MDIO timeout\n");
	return -ETIMEDOUT;
}

static int _mtk_mdio_write_c22(struct mtk_eth *eth, u32 phy_addr, u32 phy_reg,
			       u32 write_data)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C22 |
		PHY_IAC_CMD_WRITE |
		PHY_IAC_REG(phy_reg) |
		PHY_IAC_ADDR(phy_addr) |
		PHY_IAC_DATA(write_data),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return 0;
}

static int _mtk_mdio_write_c45(struct mtk_eth *eth, u32 phy_addr,
			       u32 devad, u32 phy_reg, u32 write_data)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C45 |
		PHY_IAC_CMD_C45_ADDR |
		PHY_IAC_REG(devad) |
		PHY_IAC_ADDR(phy_addr) |
		PHY_IAC_DATA(phy_reg),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C45 |
		PHY_IAC_CMD_WRITE |
		PHY_IAC_REG(devad) |
		PHY_IAC_ADDR(phy_addr) |
		PHY_IAC_DATA(write_data),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return 0;
}

static int _mtk_mdio_read_c22(struct mtk_eth *eth, u32 phy_addr, u32 phy_reg)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C22 |
		PHY_IAC_CMD_C22_READ |
		PHY_IAC_REG(phy_reg) |
		PHY_IAC_ADDR(phy_addr),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_DATA_MASK;
}

static int _mtk_mdio_read_c45(struct mtk_eth *eth, u32 phy_addr,
			      u32 devad, u32 phy_reg)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C45 |
		PHY_IAC_CMD_C45_ADDR |
		PHY_IAC_REG(devad) |
		PHY_IAC_ADDR(phy_addr) |
		PHY_IAC_DATA(phy_reg),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth, PHY_IAC_ACCESS |
		PHY_IAC_START_C45 |
		PHY_IAC_CMD_C45_READ |
		PHY_IAC_REG(devad) |
		PHY_IAC_ADDR(phy_addr),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_DATA_MASK;
}

static int mtk_mdio_write_c22(struct mii_bus *bus, int phy_addr,
			      int phy_reg, u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write_c22(eth, phy_addr, phy_reg, val);
}

static int mtk_mdio_write_c45(struct mii_bus *bus, int phy_addr,
			      int devad, int phy_reg, u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write_c45(eth, phy_addr, devad, phy_reg, val);
}

static int mtk_mdio_read_c22(struct mii_bus *bus, int phy_addr, int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read_c22(eth, phy_addr, phy_reg);
}

static int mtk_mdio_read_c45(struct mii_bus *bus, int phy_addr, int devad,
			     int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read_c45(eth, phy_addr, devad, phy_reg);
}

static int mt7621_gmac0_rgmii_adjust(struct mtk_eth *eth,
				     phy_interface_t interface)
{
	u32 val;

	val = (interface == PHY_INTERFACE_MODE_TRGMII) ?
		ETHSYS_TRGMII_MT7621_DDR_PLL : 0;

	regmap_update_bits(eth->ethsys, ETHSYS_CLKCFG0,
			   ETHSYS_TRGMII_MT7621_MASK, val);

	return 0;
}

static void mtk_gmac0_rgmii_adjust(struct mtk_eth *eth,
				   phy_interface_t interface)
{
	int ret;

	if (interface == PHY_INTERFACE_MODE_TRGMII) {
		mtk_w32(eth, TRGMII_MODE, INTF_MODE);
		ret = clk_set_rate(eth->clks[MTK_CLK_TRGPLL], 500000000);
		if (ret)
			dev_err(eth->dev, "Failed to set trgmii pll: %d\n", ret);
		return;
	}

	dev_err(eth->dev, "Missing PLL configuration, ethernet may not work\n");
}

static void mtk_setup_bridge_switch(struct mtk_eth *eth)
{
	/* Force Port1 XGMAC Link Up */
	mtk_m32(eth, 0, MTK_XGMAC_FORCE_MODE(MTK_GMAC1_ID),
		MTK_XGMAC_STS(MTK_GMAC1_ID));

	/* Adjust GSW bridge IPG to 11 */
	mtk_m32(eth, GSWTX_IPG_MASK | GSWRX_IPG_MASK,
		(GSW_IPG_11 << GSWTX_IPG_SHIFT) |
		(GSW_IPG_11 << GSWRX_IPG_SHIFT),
		MTK_GSW_CFG);
}

static struct phylink_pcs *mtk_mac_select_pcs(struct phylink_config *config,
					      phy_interface_t interface)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;
	unsigned int sid;

	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_8023z(interface)) {
		sid = (MTK_HAS_CAPS(eth->soc->caps, MTK_SHARED_SGMII)) ?
		       0 : mac->id;

		return eth->sgmii_pcs[sid];
	}

	return NULL;
}

static int mtk_mac_prepare(struct phylink_config *config, unsigned int mode,
			   phy_interface_t iface)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;

	if (mtk_interface_mode_is_xgmii(eth, iface) &&
	    mac->id != MTK_GMAC1_ID) {
		mtk_m32(mac->hw, XMAC_MCR_TRX_DISABLE,
			XMAC_MCR_TRX_DISABLE, MTK_XMAC_MCR(mac->id));

		mtk_m32(mac->hw, MTK_XGMAC_FORCE_MODE(mac->id) |
				 MTK_XGMAC_FORCE_LINK(mac->id),
			MTK_XGMAC_FORCE_MODE(mac->id), MTK_XGMAC_STS(mac->id));
	}

	return 0;
}

static void mtk_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;
	int val, ge_mode, err = 0;
	u32 i;

	/* MT76x8 has no hardware settings between for the MAC */
	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628) &&
	    mac->interface != state->interface) {
		/* Setup soc pin functions */
		switch (state->interface) {
		case PHY_INTERFACE_MODE_TRGMII:
		case PHY_INTERFACE_MODE_RGMII_TXID:
		case PHY_INTERFACE_MODE_RGMII_RXID:
		case PHY_INTERFACE_MODE_RGMII_ID:
		case PHY_INTERFACE_MODE_RGMII:
		case PHY_INTERFACE_MODE_MII:
			if (MTK_HAS_CAPS(eth->soc->caps, MTK_RGMII)) {
				err = mtk_gmac_rgmii_path_setup(eth, mac->id);
				if (err)
					goto init_err;
			}
			break;
		case PHY_INTERFACE_MODE_1000BASEX:
		case PHY_INTERFACE_MODE_2500BASEX:
		case PHY_INTERFACE_MODE_SGMII:
			err = mtk_gmac_sgmii_path_setup(eth, mac->id);
			if (err)
				goto init_err;
			break;
		case PHY_INTERFACE_MODE_GMII:
			if (MTK_HAS_CAPS(eth->soc->caps, MTK_GEPHY)) {
				err = mtk_gmac_gephy_path_setup(eth, mac->id);
				if (err)
					goto init_err;
			}
			break;
		case PHY_INTERFACE_MODE_INTERNAL:
			if (mac->id == MTK_GMAC2_ID &&
			    MTK_HAS_CAPS(eth->soc->caps, MTK_2P5GPHY)) {
				err = mtk_gmac_2p5gphy_path_setup(eth, mac->id);
				if (err)
					goto init_err;
			}
			break;
		default:
			goto err_phy;
		}

		/* Setup clock for 1st gmac */
		if (!mac->id && state->interface != PHY_INTERFACE_MODE_SGMII &&
		    !phy_interface_mode_is_8023z(state->interface) &&
		    MTK_HAS_CAPS(mac->hw->soc->caps, MTK_GMAC1_TRGMII)) {
			if (MTK_HAS_CAPS(mac->hw->soc->caps,
					 MTK_TRGMII_MT7621_CLK)) {
				if (mt7621_gmac0_rgmii_adjust(mac->hw,
							      state->interface))
					goto err_phy;
			} else {
				mtk_gmac0_rgmii_adjust(mac->hw,
						       state->interface);

				/* mt7623_pad_clk_setup */
				for (i = 0 ; i < NUM_TRGMII_CTRL; i++)
					mtk_w32(mac->hw,
						TD_DM_DRVP(8) | TD_DM_DRVN(8),
						TRGMII_TD_ODT(i));

				/* Assert/release MT7623 RXC reset */
				mtk_m32(mac->hw, 0, RXC_RST | RXC_DQSISEL,
					TRGMII_RCK_CTRL);
				mtk_m32(mac->hw, RXC_RST, 0, TRGMII_RCK_CTRL);
			}
		}

		switch (state->interface) {
		case PHY_INTERFACE_MODE_MII:
		case PHY_INTERFACE_MODE_GMII:
			ge_mode = 1;
			break;
		default:
			ge_mode = 0;
			break;
		}

		/* put the gmac into the right mode */
		regmap_read(eth->ethsys, ETHSYS_SYSCFG0, &val);
		val &= ~SYSCFG0_GE_MODE(SYSCFG0_GE_MASK, mac->id);
		val |= SYSCFG0_GE_MODE(ge_mode, mac->id);
		regmap_write(eth->ethsys, ETHSYS_SYSCFG0, val);

		mac->interface = state->interface;
	}

	/* SGMII */
	if (state->interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_8023z(state->interface)) {
		/* The path GMAC to SGMII will be enabled once the SGMIISYS is
		 * being setup done.
		 */
		regmap_read(eth->ethsys, ETHSYS_SYSCFG0, &val);

		regmap_update_bits(eth->ethsys, ETHSYS_SYSCFG0,
				   SYSCFG0_SGMII_MASK,
				   ~(u32)SYSCFG0_SGMII_MASK);

		/* Save the syscfg0 value for mac_finish */
		mac->syscfg0 = val;
	} else if (phylink_autoneg_inband(mode)) {
		dev_err(eth->dev,
			"In-band mode not supported in non SGMII mode!\n");
		return;
	}

	/* Setup gmac */
	if (mtk_interface_mode_is_xgmii(eth, state->interface)) {
		mtk_w32(mac->hw, MTK_GDMA_XGDM_SEL, MTK_GDMA_EG_CTRL(mac->id));
		mtk_w32(mac->hw, MAC_MCR_FORCE_LINK_DOWN, MTK_MAC_MCR(mac->id));

		if (mac->id == MTK_GMAC1_ID)
			mtk_setup_bridge_switch(eth);
	}

	return;

err_phy:
	dev_err(eth->dev, "%s: GMAC%d mode %s not supported!\n", __func__,
		mac->id, phy_modes(state->interface));
	return;

init_err:
	dev_err(eth->dev, "%s: GMAC%d mode %s err: %d!\n", __func__,
		mac->id, phy_modes(state->interface), err);
}

static int mtk_mac_finish(struct phylink_config *config, unsigned int mode,
			  phy_interface_t interface)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;
	u32 mcr_cur, mcr_new;

	/* Enable SGMII */
	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_8023z(interface))
		regmap_update_bits(eth->ethsys, ETHSYS_SYSCFG0,
				   SYSCFG0_SGMII_MASK, mac->syscfg0);

	/* Setup gmac */
	mcr_cur = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr_new = mcr_cur;
	mcr_new |= MAC_MCR_IPG_CFG | MAC_MCR_FORCE_MODE |
		   MAC_MCR_BACKOFF_EN | MAC_MCR_BACKPR_EN | MAC_MCR_RX_FIFO_CLR_DIS;

	/* Only update control register when needed! */
	if (mcr_new != mcr_cur)
		mtk_w32(mac->hw, mcr_new, MTK_MAC_MCR(mac->id));

	return 0;
}

static void mtk_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);

	if (!mtk_interface_mode_is_xgmii(mac->hw, interface)) {
		/* GMAC modes */
		mtk_m32(mac->hw,
			MAC_MCR_TX_EN | MAC_MCR_RX_EN | MAC_MCR_FORCE_LINK, 0,
			MTK_MAC_MCR(mac->id));
	} else if (mac->id != MTK_GMAC1_ID) {
		/* XGMAC except for built-in switch */
		mtk_m32(mac->hw, XMAC_MCR_TRX_DISABLE, XMAC_MCR_TRX_DISABLE,
			MTK_XMAC_MCR(mac->id));
		mtk_m32(mac->hw, MTK_XGMAC_FORCE_LINK(mac->id), 0,
			MTK_XGMAC_STS(mac->id));
	}
}

static void mtk_set_queue_speed(struct mtk_eth *eth, unsigned int idx,
				int speed)
{
	const struct mtk_soc_data *soc = eth->soc;
	u32 ofs, val;

	if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA))
		return;

	val = MTK_QTX_SCH_MIN_RATE_EN |
	      /* minimum: 10 Mbps */
	      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_MAN, 1) |
	      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_EXP, 4) |
	      MTK_QTX_SCH_LEAKY_BUCKET_SIZE;
	if (mtk_is_netsys_v1(eth))
		val |= MTK_QTX_SCH_LEAKY_BUCKET_EN;

	if (IS_ENABLED(CONFIG_SOC_MT7621)) {
		switch (speed) {
		case SPEED_10:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 103) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 2) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
			break;
		case SPEED_100:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 103) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 3) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
			break;
		case SPEED_1000:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 105) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 4) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 10);
			break;
		default:
			break;
		}
	} else {
		switch (speed) {
		case SPEED_10:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 1) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 4) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
			break;
		case SPEED_100:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 1) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 5) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
			break;
		case SPEED_1000:
			val |= MTK_QTX_SCH_MAX_RATE_EN |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 1) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 6) |
			       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 10);
			break;
		default:
			break;
		}
	}

	ofs = MTK_QTX_OFFSET * idx;
	mtk_w32(eth, val, soc->reg_map->qdma.qtx_sch + ofs);
}

static void mtk_gdm_mac_link_up(struct mtk_mac *mac,
				struct phy_device *phy,
				unsigned int mode, phy_interface_t interface,
				int speed, int duplex, bool tx_pause,
				bool rx_pause)
{
	u32 mcr;

	mcr = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr &= ~(MAC_MCR_SPEED_100 | MAC_MCR_SPEED_1000 |
		 MAC_MCR_FORCE_DPX | MAC_MCR_FORCE_TX_FC |
		 MAC_MCR_FORCE_RX_FC);

	/* Configure speed */
	mac->speed = speed;
	switch (speed) {
	case SPEED_2500:
	case SPEED_1000:
		mcr |= MAC_MCR_SPEED_1000;
		break;
	case SPEED_100:
		mcr |= MAC_MCR_SPEED_100;
		break;
	}

	/* Configure duplex */
	if (duplex == DUPLEX_FULL)
		mcr |= MAC_MCR_FORCE_DPX;

	/* Configure pause modes - phylink will avoid these for half duplex */
	if (tx_pause)
		mcr |= MAC_MCR_FORCE_TX_FC;
	if (rx_pause)
		mcr |= MAC_MCR_FORCE_RX_FC;

	mcr |= MAC_MCR_TX_EN | MAC_MCR_RX_EN | MAC_MCR_FORCE_LINK;
	mtk_w32(mac->hw, mcr, MTK_MAC_MCR(mac->id));
}

static void mtk_xgdm_mac_link_up(struct mtk_mac *mac,
				 struct phy_device *phy,
				 unsigned int mode, phy_interface_t interface,
				 int speed, int duplex, bool tx_pause,
				 bool rx_pause)
{
	u32 mcr;

	if (mac->id == MTK_GMAC1_ID)
		return;

	/* Eliminate the interference(before link-up) caused by PHY noise */
	mtk_m32(mac->hw, XMAC_LOGIC_RST, 0, MTK_XMAC_LOGIC_RST(mac->id));
	mdelay(20);
	mtk_m32(mac->hw, XMAC_GLB_CNTCLR, XMAC_GLB_CNTCLR,
		MTK_XMAC_CNT_CTRL(mac->id));

	mtk_m32(mac->hw, MTK_XGMAC_FORCE_LINK(mac->id),
		MTK_XGMAC_FORCE_LINK(mac->id), MTK_XGMAC_STS(mac->id));

	mcr = mtk_r32(mac->hw, MTK_XMAC_MCR(mac->id));
	mcr &= ~(XMAC_MCR_FORCE_TX_FC | XMAC_MCR_FORCE_RX_FC |
		 XMAC_MCR_TRX_DISABLE);
	/* Configure pause modes -
	 * phylink will avoid these for half duplex
	 */
	if (tx_pause)
		mcr |= XMAC_MCR_FORCE_TX_FC;
	if (rx_pause)
		mcr |= XMAC_MCR_FORCE_RX_FC;

	mtk_w32(mac->hw, mcr, MTK_XMAC_MCR(mac->id));
}

static void mtk_mac_link_up(struct phylink_config *config,
			    struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause, bool rx_pause)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);

	if (mtk_interface_mode_is_xgmii(mac->hw, interface))
		mtk_xgdm_mac_link_up(mac, phy, mode, interface, speed, duplex,
				     tx_pause, rx_pause);
	else
		mtk_gdm_mac_link_up(mac, phy, mode, interface, speed, duplex,
				    tx_pause, rx_pause);
}

static void mtk_mac_disable_tx_lpi(struct phylink_config *config)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;

	mtk_m32(eth, MAC_MCR_EEE100M | MAC_MCR_EEE1G, 0, MTK_MAC_MCR(mac->id));
}

static int mtk_mac_enable_tx_lpi(struct phylink_config *config, u32 timer,
				 bool tx_clk_stop)
{
	struct mtk_mac *mac = container_of(config, struct mtk_mac,
					   phylink_config);
	struct mtk_eth *eth = mac->hw;
	u32 val;

	if (mtk_interface_mode_is_xgmii(eth, mac->interface))
		return -EOPNOTSUPP;

	/* Tx idle timer in ms */
	timer = DIV_ROUND_UP(timer, 1000);

	/* If the timer is zero, then set LPI_MODE, which allows the
	 * system to enter LPI mode immediately rather than waiting for
	 * the LPI threshold.
	 */
	if (!timer)
		val = MAC_EEE_LPI_MODE;
	else if (FIELD_FIT(MAC_EEE_LPI_TXIDLE_THD, timer))
		val = FIELD_PREP(MAC_EEE_LPI_TXIDLE_THD, timer);
	else
		val = MAC_EEE_LPI_TXIDLE_THD;

	if (tx_clk_stop)
		val |= MAC_EEE_CKG_TXIDLE;

	/* PHY Wake-up time, this field does not have a reset value, so use the
	 * reset value from MT7531 (36us for 100M and 17us for 1000M).
	 */
	val |= FIELD_PREP(MAC_EEE_WAKEUP_TIME_1000, 17) |
	       FIELD_PREP(MAC_EEE_WAKEUP_TIME_100, 36);

	mtk_w32(eth, val, MTK_MAC_EEECR(mac->id));
	mtk_m32(eth, 0, MAC_MCR_EEE100M | MAC_MCR_EEE1G, MTK_MAC_MCR(mac->id));

	return 0;
}

static const struct phylink_mac_ops mtk_phylink_ops = {
	.mac_prepare = mtk_mac_prepare,
	.mac_select_pcs = mtk_mac_select_pcs,
	.mac_config = mtk_mac_config,
	.mac_finish = mtk_mac_finish,
	.mac_link_down = mtk_mac_link_down,
	.mac_link_up = mtk_mac_link_up,
	.mac_disable_tx_lpi = mtk_mac_disable_tx_lpi,
	.mac_enable_tx_lpi = mtk_mac_enable_tx_lpi,
};

static void mtk_mdio_config(struct mtk_eth *eth)
{
	u32 val;

	/* Configure MDC Divider */
	val = FIELD_PREP(PPSC_MDC_CFG, eth->mdc_divider);

	/* Configure MDC Turbo Mode */
	if (mtk_is_netsys_v3_or_greater(eth))
		mtk_m32(eth, 0, MISC_MDC_TURBO, MTK_MAC_MISC_V3);
	else
		val |= PPSC_MDC_TURBO;

	mtk_m32(eth, PPSC_MDC_CFG, val, MTK_PPSC);
}

static int mtk_mdio_init(struct mtk_eth *eth)
{
	unsigned int max_clk = 2500000;
	struct device_node *mii_np;
	int ret;
	u32 val;

	mii_np = of_get_available_child_by_name(eth->dev->of_node, "mdio-bus");
	if (!mii_np) {
		dev_err(eth->dev, "no %s child node found", "mdio-bus");
		return -ENODEV;
	}

	eth->mii_bus = devm_mdiobus_alloc(eth->dev);
	if (!eth->mii_bus) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	eth->mii_bus->name = "mdio";
	eth->mii_bus->read = mtk_mdio_read_c22;
	eth->mii_bus->write = mtk_mdio_write_c22;
	eth->mii_bus->read_c45 = mtk_mdio_read_c45;
	eth->mii_bus->write_c45 = mtk_mdio_write_c45;
	eth->mii_bus->priv = eth;
	eth->mii_bus->parent = eth->dev;

	snprintf(eth->mii_bus->id, MII_BUS_ID_SIZE, "%pOFn", mii_np);

	if (!of_property_read_u32(mii_np, "clock-frequency", &val)) {
		if (val > MDC_MAX_FREQ || val < MDC_MAX_FREQ / MDC_MAX_DIVIDER) {
			dev_err(eth->dev, "MDIO clock frequency out of range");
			ret = -EINVAL;
			goto err_put_node;
		}
		max_clk = val;
	}
	eth->mdc_divider = min_t(unsigned int, DIV_ROUND_UP(MDC_MAX_FREQ, max_clk), 63);
	mtk_mdio_config(eth);
	dev_dbg(eth->dev, "MDC is running on %d Hz\n", MDC_MAX_FREQ / eth->mdc_divider);
	ret = of_mdiobus_register(eth->mii_bus, mii_np);

err_put_node:
	of_node_put(mii_np);
	return ret;
}

static void mtk_mdio_cleanup(struct mtk_eth *eth)
{
	if (!eth->mii_bus)
		return;

	mdiobus_unregister(eth->mii_bus);
}

static inline void mtk_tx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->tx_irq_mask);
	mtk_w32(eth, val & ~mask, eth->soc->reg_map->tx_irq_mask);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

static inline void mtk_tx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->tx_irq_mask);
	mtk_w32(eth, val | mask, eth->soc->reg_map->tx_irq_mask);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

static inline void mtk_rx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->pdma.irq_mask);
	mtk_w32(eth, val & ~mask, eth->soc->reg_map->pdma.irq_mask);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

static inline void mtk_rx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->pdma.irq_mask);
	mtk_w32(eth, val | mask, eth->soc->reg_map->pdma.irq_mask);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

static int mtk_set_mac_address(struct net_device *dev, void *p)
{
	int ret = eth_mac_addr(dev, p);
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const char *macaddr = dev->dev_addr;

	if (ret)
		return ret;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	spin_lock_bh(&mac->hw->page_lock);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		mtk_w32(mac->hw, (macaddr[0] << 8) | macaddr[1],
			MT7628_SDM_MAC_ADRH);
		mtk_w32(mac->hw, (macaddr[2] << 24) | (macaddr[3] << 16) |
			(macaddr[4] << 8) | macaddr[5],
			MT7628_SDM_MAC_ADRL);
	} else {
		mtk_w32(mac->hw, (macaddr[0] << 8) | macaddr[1],
			MTK_GDMA_MAC_ADRH(mac->id));
		mtk_w32(mac->hw, (macaddr[2] << 24) | (macaddr[3] << 16) |
			(macaddr[4] << 8) | macaddr[5],
			MTK_GDMA_MAC_ADRL(mac->id));
	}
	spin_unlock_bh(&mac->hw->page_lock);

	return 0;
}

void mtk_stats_update_mac(struct mtk_mac *mac)
{
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	struct mtk_eth *eth = mac->hw;

	u64_stats_update_begin(&hw_stats->syncp);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		hw_stats->tx_packets += mtk_r32(mac->hw, MT7628_SDM_TPCNT);
		hw_stats->tx_bytes += mtk_r32(mac->hw, MT7628_SDM_TBCNT);
		hw_stats->rx_packets += mtk_r32(mac->hw, MT7628_SDM_RPCNT);
		hw_stats->rx_bytes += mtk_r32(mac->hw, MT7628_SDM_RBCNT);
		hw_stats->rx_checksum_errors +=
			mtk_r32(mac->hw, MT7628_SDM_CS_ERR);
	} else {
		const struct mtk_reg_map *reg_map = eth->soc->reg_map;
		unsigned int offs = hw_stats->reg_offset;
		u64 stats;

		hw_stats->rx_bytes += mtk_r32(mac->hw, reg_map->gdm1_cnt + offs);
		stats = mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x4 + offs);
		if (stats)
			hw_stats->rx_bytes += (stats << 32);
		hw_stats->rx_packets +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x8 + offs);
		hw_stats->rx_overflow +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x10 + offs);
		hw_stats->rx_fcs_errors +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x14 + offs);
		hw_stats->rx_short_errors +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x18 + offs);
		hw_stats->rx_long_errors +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x1c + offs);
		hw_stats->rx_checksum_errors +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x20 + offs);
		hw_stats->rx_flow_control_packets +=
			mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x24 + offs);

		if (mtk_is_netsys_v3_or_greater(eth)) {
			hw_stats->tx_skip +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x50 + offs);
			hw_stats->tx_collisions +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x54 + offs);
			hw_stats->tx_bytes +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x40 + offs);
			stats =  mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x44 + offs);
			if (stats)
				hw_stats->tx_bytes += (stats << 32);
			hw_stats->tx_packets +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x48 + offs);
		} else {
			hw_stats->tx_skip +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x28 + offs);
			hw_stats->tx_collisions +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x2c + offs);
			hw_stats->tx_bytes +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x30 + offs);
			stats =  mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x34 + offs);
			if (stats)
				hw_stats->tx_bytes += (stats << 32);
			hw_stats->tx_packets +=
				mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x38 + offs);
		}
	}

	u64_stats_update_end(&hw_stats->syncp);
}

static void mtk_stats_update(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->mac[i] || !eth->mac[i]->hw_stats)
			continue;
		if (spin_trylock(&eth->mac[i]->hw_stats->stats_lock)) {
			mtk_stats_update_mac(eth->mac[i]);
			spin_unlock(&eth->mac[i]->hw_stats->stats_lock);
		}
	}
}

static void mtk_get_stats64(struct net_device *dev,
			    struct rtnl_link_stats64 *storage)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	unsigned int start;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock_bh(&hw_stats->stats_lock)) {
			mtk_stats_update_mac(mac);
			spin_unlock_bh(&hw_stats->stats_lock);
		}
	}

	do {
		start = u64_stats_fetch_begin(&hw_stats->syncp);
		storage->rx_packets = hw_stats->rx_packets;
		storage->tx_packets = hw_stats->tx_packets;
		storage->rx_bytes = hw_stats->rx_bytes;
		storage->tx_bytes = hw_stats->tx_bytes;
		storage->collisions = hw_stats->tx_collisions;
		storage->rx_length_errors = hw_stats->rx_short_errors +
			hw_stats->rx_long_errors;
		storage->rx_over_errors = hw_stats->rx_overflow;
		storage->rx_crc_errors = hw_stats->rx_fcs_errors;
		storage->rx_errors = hw_stats->rx_checksum_errors;
		storage->tx_aborted_errors = hw_stats->tx_skip;
	} while (u64_stats_fetch_retry(&hw_stats->syncp, start));

	storage->tx_errors = dev->stats.tx_errors;
	storage->rx_dropped = dev->stats.rx_dropped;
	storage->tx_dropped = dev->stats.tx_dropped;
}

static inline int mtk_max_frag_size(int mtu)
{
	/* make sure buf_size will be at least MTK_MAX_RX_LENGTH */
	if (mtu + MTK_RX_ETH_HLEN < MTK_MAX_RX_LENGTH_2K)
		mtu = MTK_MAX_RX_LENGTH_2K - MTK_RX_ETH_HLEN;

	return SKB_DATA_ALIGN(MTK_RX_HLEN + mtu) +
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
}

static inline int mtk_max_buf_size(int frag_size)
{
	int buf_size = frag_size - NET_SKB_PAD - NET_IP_ALIGN -
		       SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	WARN_ON(buf_size < MTK_MAX_RX_LENGTH_2K);

	return buf_size;
}

static bool mtk_rx_get_desc(struct mtk_eth *eth, struct mtk_rx_dma_v2 *rxd,
			    struct mtk_rx_dma_v2 *dma_rxd)
{
	rxd->rxd2 = READ_ONCE(dma_rxd->rxd2);
	if (!(rxd->rxd2 & RX_DMA_DONE))
		return false;

	rxd->rxd1 = READ_ONCE(dma_rxd->rxd1);
	rxd->rxd3 = READ_ONCE(dma_rxd->rxd3);
	rxd->rxd4 = READ_ONCE(dma_rxd->rxd4);
	if (mtk_is_netsys_v3_or_greater(eth)) {
		rxd->rxd5 = READ_ONCE(dma_rxd->rxd5);
		rxd->rxd6 = READ_ONCE(dma_rxd->rxd6);
	}

	return true;
}

static void *mtk_max_lro_buf_alloc(gfp_t gfp_mask)
{
	unsigned int size = mtk_max_frag_size(MTK_MAX_LRO_RX_LENGTH);
	unsigned long data;

	data = __get_free_pages(gfp_mask | __GFP_COMP | __GFP_NOWARN,
				get_order(size));

	return (void *)data;
}

static void *mtk_dma_ring_alloc(struct mtk_eth *eth, size_t size,
				dma_addr_t *dma_handle, bool use_sram)
{
	void *dma_ring;

	if (use_sram && eth->sram_pool) {
		dma_ring = (void *)gen_pool_alloc(eth->sram_pool, size);
		if (!dma_ring)
			return dma_ring;
		*dma_handle = gen_pool_virt_to_phys(eth->sram_pool,
						    (unsigned long)dma_ring);
	} else {
		dma_ring = dma_alloc_coherent(eth->dma_dev, size, dma_handle,
					      GFP_KERNEL);
	}

	return dma_ring;
}

static void mtk_dma_ring_free(struct mtk_eth *eth, size_t size, void *dma_ring,
			      dma_addr_t dma_handle, bool in_sram)
{
	if (in_sram && eth->sram_pool)
		gen_pool_free(eth->sram_pool, (unsigned long)dma_ring, size);
	else
		dma_free_coherent(eth->dma_dev, size, dma_ring, dma_handle);
}

/* the qdma core needs scratch memory to be setup */
static int mtk_init_fq_dma(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	dma_addr_t phy_ring_tail;
	int cnt = soc->tx.fq_dma_size;
	dma_addr_t dma_addr;
	int i, j, len;

	eth->scratch_ring = mtk_dma_ring_alloc(eth, cnt * soc->tx.desc_size,
					       &eth->phy_scratch_ring, true);

	if (unlikely(!eth->scratch_ring))
		return -ENOMEM;

	phy_ring_tail = eth->phy_scratch_ring + soc->tx.desc_size * (cnt - 1);

	for (j = 0; j < DIV_ROUND_UP(soc->tx.fq_dma_size, MTK_FQ_DMA_LENGTH); j++) {
		len = min_t(int, cnt - j * MTK_FQ_DMA_LENGTH, MTK_FQ_DMA_LENGTH);
		eth->scratch_head[j] = kcalloc(len, MTK_QDMA_PAGE_SIZE, GFP_KERNEL);

		if (unlikely(!eth->scratch_head[j]))
			return -ENOMEM;

		dma_addr = dma_map_single(eth->dma_dev,
					  eth->scratch_head[j], len * MTK_QDMA_PAGE_SIZE,
					  DMA_FROM_DEVICE);

		if (unlikely(dma_mapping_error(eth->dma_dev, dma_addr)))
			return -ENOMEM;

		for (i = 0; i < len; i++) {
			struct mtk_tx_dma_v2 *txd;

			txd = eth->scratch_ring + (j * MTK_FQ_DMA_LENGTH + i) * soc->tx.desc_size;
			txd->txd1 = dma_addr + i * MTK_QDMA_PAGE_SIZE;
			if (j * MTK_FQ_DMA_LENGTH + i < cnt)
				txd->txd2 = eth->phy_scratch_ring +
					    (j * MTK_FQ_DMA_LENGTH + i + 1) * soc->tx.desc_size;

			txd->txd3 = TX_DMA_PLEN0(MTK_QDMA_PAGE_SIZE);
			if (MTK_HAS_CAPS(soc->caps, MTK_36BIT_DMA))
				txd->txd3 |= TX_DMA_PREP_ADDR64(dma_addr + i * MTK_QDMA_PAGE_SIZE);

			txd->txd4 = 0;
			if (mtk_is_netsys_v2_or_greater(eth)) {
				txd->txd5 = 0;
				txd->txd6 = 0;
				txd->txd7 = 0;
				txd->txd8 = 0;
			}
		}
	}

	mtk_w32(eth, eth->phy_scratch_ring, soc->reg_map->qdma.fq_head);
	mtk_w32(eth, phy_ring_tail, soc->reg_map->qdma.fq_tail);
	mtk_w32(eth, (cnt << 16) | cnt, soc->reg_map->qdma.fq_count);
	mtk_w32(eth, MTK_QDMA_PAGE_SIZE << 16, soc->reg_map->qdma.fq_blen);

	return 0;
}

static void *mtk_qdma_phys_to_virt(struct mtk_tx_ring *ring, u32 desc)
{
	return ring->dma + (desc - ring->phys);
}

static struct mtk_tx_buf *mtk_desc_to_tx_buf(struct mtk_tx_ring *ring,
					     void *txd, u32 txd_size)
{
	int idx = (txd - ring->dma) / txd_size;

	return &ring->buf[idx];
}

static struct mtk_tx_dma *qdma_to_pdma(struct mtk_tx_ring *ring,
				       struct mtk_tx_dma *dma)
{
	return ring->dma_pdma - (struct mtk_tx_dma *)ring->dma + dma;
}

static int txd_to_idx(struct mtk_tx_ring *ring, void *dma, u32 txd_size)
{
	return (dma - ring->dma) / txd_size;
}

static void mtk_tx_unmap(struct mtk_eth *eth, struct mtk_tx_buf *tx_buf,
			 struct xdp_frame_bulk *bq, bool napi)
{
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		if (tx_buf->flags & MTK_TX_FLAGS_SINGLE0) {
			dma_unmap_single(eth->dma_dev,
					 dma_unmap_addr(tx_buf, dma_addr0),
					 dma_unmap_len(tx_buf, dma_len0),
					 DMA_TO_DEVICE);
		} else if (tx_buf->flags & MTK_TX_FLAGS_PAGE0) {
			dma_unmap_page(eth->dma_dev,
				       dma_unmap_addr(tx_buf, dma_addr0),
				       dma_unmap_len(tx_buf, dma_len0),
				       DMA_TO_DEVICE);
		}
	} else {
		if (dma_unmap_len(tx_buf, dma_len0)) {
			dma_unmap_page(eth->dma_dev,
				       dma_unmap_addr(tx_buf, dma_addr0),
				       dma_unmap_len(tx_buf, dma_len0),
				       DMA_TO_DEVICE);
		}

		if (dma_unmap_len(tx_buf, dma_len1)) {
			dma_unmap_page(eth->dma_dev,
				       dma_unmap_addr(tx_buf, dma_addr1),
				       dma_unmap_len(tx_buf, dma_len1),
				       DMA_TO_DEVICE);
		}
	}

	if (tx_buf->data && tx_buf->data != (void *)MTK_DMA_DUMMY_DESC) {
		if (tx_buf->type == MTK_TYPE_SKB) {
			struct sk_buff *skb = tx_buf->data;

			if (napi)
				napi_consume_skb(skb, napi);
			else
				dev_kfree_skb_any(skb);
		} else {
			struct xdp_frame *xdpf = tx_buf->data;

			if (napi && tx_buf->type == MTK_TYPE_XDP_TX)
				xdp_return_frame_rx_napi(xdpf);
			else if (bq)
				xdp_return_frame_bulk(xdpf, bq);
			else
				xdp_return_frame(xdpf);
		}
	}
	tx_buf->flags = 0;
	tx_buf->data = NULL;
}

static void setup_tx_buf(struct mtk_eth *eth, struct mtk_tx_buf *tx_buf,
			 struct mtk_tx_dma *txd, dma_addr_t mapped_addr,
			 size_t size, int idx)
{
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
		dma_unmap_len_set(tx_buf, dma_len0, size);
	} else {
		if (idx & 1) {
			txd->txd3 = mapped_addr;
			txd->txd2 |= TX_DMA_PLEN1(size);
			dma_unmap_addr_set(tx_buf, dma_addr1, mapped_addr);
			dma_unmap_len_set(tx_buf, dma_len1, size);
		} else {
			tx_buf->data = (void *)MTK_DMA_DUMMY_DESC;
			txd->txd1 = mapped_addr;
			txd->txd2 = TX_DMA_PLEN0(size);
			dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
			dma_unmap_len_set(tx_buf, dma_len0, size);
		}
	}
}

static void mtk_tx_set_dma_desc_v1(struct net_device *dev, void *txd,
				   struct mtk_tx_dma_desc_info *info)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_tx_dma *desc = txd;
	u32 data;

	WRITE_ONCE(desc->txd1, info->addr);

	data = TX_DMA_SWC | TX_DMA_PLEN0(info->size) |
	       FIELD_PREP(TX_DMA_PQID, info->qid);
	if (info->last)
		data |= TX_DMA_LS0;
	WRITE_ONCE(desc->txd3, data);

	data = (mac->id + 1) << TX_DMA_FPORT_SHIFT; /* forward port */
	if (info->first) {
		if (info->gso)
			data |= TX_DMA_TSO;
		/* tx checksum offload */
		if (info->csum)
			data |= TX_DMA_CHKSUM;
		/* vlan header offload */
		if (info->vlan)
			data |= TX_DMA_INS_VLAN | info->vlan_tci;
	}
	WRITE_ONCE(desc->txd4, data);
}

static void mtk_tx_set_dma_desc_v2(struct net_device *dev, void *txd,
				   struct mtk_tx_dma_desc_info *info)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_tx_dma_v2 *desc = txd;
	struct mtk_eth *eth = mac->hw;
	u32 data;

	WRITE_ONCE(desc->txd1, info->addr);

	data = TX_DMA_PLEN0(info->size);
	if (info->last)
		data |= TX_DMA_LS0;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA))
		data |= TX_DMA_PREP_ADDR64(info->addr);

	WRITE_ONCE(desc->txd3, data);

	 /* set forward port */
	switch (mac->id) {
	case MTK_GMAC1_ID:
		data = PSE_GDM1_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	case MTK_GMAC2_ID:
		data = PSE_GDM2_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	case MTK_GMAC3_ID:
		data = PSE_GDM3_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	}

	data |= TX_DMA_SWC_V2 | QID_BITS_V2(info->qid);
	WRITE_ONCE(desc->txd4, data);

	data = 0;
	if (info->first) {
		if (info->gso)
			data |= TX_DMA_TSO_V2;
		/* tx checksum offload */
		if (info->csum)
			data |= TX_DMA_CHKSUM_V2;
		if (mtk_is_netsys_v3_or_greater(eth) && netdev_uses_dsa(dev))
			data |= TX_DMA_SPTAG_V3;
	}
	WRITE_ONCE(desc->txd5, data);

	data = 0;
	if (info->first && info->vlan)
		data |= TX_DMA_INS_VLAN_V2 | info->vlan_tci;
	WRITE_ONCE(desc->txd6, data);

	WRITE_ONCE(desc->txd7, 0);
	WRITE_ONCE(desc->txd8, 0);
}

static void mtk_tx_set_dma_desc(struct net_device *dev, void *txd,
				struct mtk_tx_dma_desc_info *info)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	if (mtk_is_netsys_v2_or_greater(eth))
		mtk_tx_set_dma_desc_v2(dev, txd, info);
	else
		mtk_tx_set_dma_desc_v1(dev, txd, info);
}

static int mtk_tx_map(struct sk_buff *skb, struct net_device *dev,
		      int tx_num, struct mtk_tx_ring *ring, bool gso)
{
	struct mtk_tx_dma_desc_info txd_info = {
		.size = skb_headlen(skb),
		.gso = gso,
		.csum = skb->ip_summed == CHECKSUM_PARTIAL,
		.vlan = skb_vlan_tag_present(skb),
		.qid = skb_get_queue_mapping(skb),
		.vlan_tci = skb_vlan_tag_get(skb),
		.first = true,
		.last = !skb_is_nonlinear(skb),
	};
	struct netdev_queue *txq;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_dma *itxd, *txd;
	struct mtk_tx_dma *itxd_pdma, *txd_pdma;
	struct mtk_tx_buf *itx_buf, *tx_buf;
	int i, n_desc = 1;
	int queue = skb_get_queue_mapping(skb);
	int k = 0;

	txq = netdev_get_tx_queue(dev, queue);
	itxd = ring->next_free;
	itxd_pdma = qdma_to_pdma(ring, itxd);
	if (itxd == ring->last_free)
		return -ENOMEM;

	itx_buf = mtk_desc_to_tx_buf(ring, itxd, soc->tx.desc_size);
	memset(itx_buf, 0, sizeof(*itx_buf));

	txd_info.addr = dma_map_single(eth->dma_dev, skb->data, txd_info.size,
				       DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(eth->dma_dev, txd_info.addr)))
		return -ENOMEM;

	mtk_tx_set_dma_desc(dev, itxd, &txd_info);

	itx_buf->flags |= MTK_TX_FLAGS_SINGLE0;
	itx_buf->mac_id = mac->id;
	setup_tx_buf(eth, itx_buf, itxd_pdma, txd_info.addr, txd_info.size,
		     k++);

	/* TX SG offload */
	txd = itxd;
	txd_pdma = qdma_to_pdma(ring, txd);

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		unsigned int offset = 0;
		int frag_size = skb_frag_size(frag);

		while (frag_size) {
			bool new_desc = true;

			if (MTK_HAS_CAPS(soc->caps, MTK_QDMA) ||
			    (i & 0x1)) {
				txd = mtk_qdma_phys_to_virt(ring, txd->txd2);
				txd_pdma = qdma_to_pdma(ring, txd);
				if (txd == ring->last_free)
					goto err_dma;

				n_desc++;
			} else {
				new_desc = false;
			}

			memset(&txd_info, 0, sizeof(struct mtk_tx_dma_desc_info));
			txd_info.size = min_t(unsigned int, frag_size,
					      soc->tx.dma_max_len);
			txd_info.qid = queue;
			txd_info.last = i == skb_shinfo(skb)->nr_frags - 1 &&
					!(frag_size - txd_info.size);
			txd_info.addr = skb_frag_dma_map(eth->dma_dev, frag,
							 offset, txd_info.size,
							 DMA_TO_DEVICE);
			if (unlikely(dma_mapping_error(eth->dma_dev, txd_info.addr)))
				goto err_dma;

			mtk_tx_set_dma_desc(dev, txd, &txd_info);

			tx_buf = mtk_desc_to_tx_buf(ring, txd,
						    soc->tx.desc_size);
			if (new_desc)
				memset(tx_buf, 0, sizeof(*tx_buf));
			tx_buf->data = (void *)MTK_DMA_DUMMY_DESC;
			tx_buf->flags |= MTK_TX_FLAGS_PAGE0;
			tx_buf->mac_id = mac->id;

			setup_tx_buf(eth, tx_buf, txd_pdma, txd_info.addr,
				     txd_info.size, k++);

			frag_size -= txd_info.size;
			offset += txd_info.size;
		}
	}

	/* store skb to cleanup */
	itx_buf->type = MTK_TYPE_SKB;
	itx_buf->data = skb;

	if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		if (k & 0x1)
			txd_pdma->txd2 |= TX_DMA_LS0;
		else
			txd_pdma->txd2 |= TX_DMA_LS1;
	}

	netdev_tx_sent_queue(txq, skb->len);
	skb_tx_timestamp(skb);

	ring->next_free = mtk_qdma_phys_to_virt(ring, txd->txd2);
	atomic_sub(n_desc, &ring->free_count);

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		if (netif_xmit_stopped(txq) || !netdev_xmit_more())
			mtk_w32(eth, txd->txd2, soc->reg_map->qdma.ctx_ptr);
	} else {
		int next_idx;

		next_idx = NEXT_DESP_IDX(txd_to_idx(ring, txd, soc->tx.desc_size),
					 ring->dma_size);
		mtk_w32(eth, next_idx, MT7628_TX_CTX_IDX0);
	}

	return 0;

err_dma:
	do {
		tx_buf = mtk_desc_to_tx_buf(ring, itxd, soc->tx.desc_size);

		/* unmap dma */
		mtk_tx_unmap(eth, tx_buf, NULL, false);

		itxd->txd3 = TX_DMA_LS0 | TX_DMA_OWNER_CPU;
		if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA))
			itxd_pdma->txd2 = TX_DMA_DESP2_DEF;

		itxd = mtk_qdma_phys_to_virt(ring, itxd->txd2);
		itxd_pdma = qdma_to_pdma(ring, itxd);
	} while (itxd != txd);

	return -ENOMEM;
}

static int mtk_cal_txd_req(struct mtk_eth *eth, struct sk_buff *skb)
{
	int i, nfrags = 1;
	skb_frag_t *frag;

	if (skb_is_gso(skb)) {
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			frag = &skb_shinfo(skb)->frags[i];
			nfrags += DIV_ROUND_UP(skb_frag_size(frag),
					       eth->soc->tx.dma_max_len);
		}
	} else {
		nfrags += skb_shinfo(skb)->nr_frags;
	}

	return nfrags;
}

static int mtk_queue_stopped(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		if (netif_queue_stopped(eth->netdev[i]))
			return 1;
	}

	return 0;
}

static void mtk_wake_queue(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		netif_tx_wake_all_queues(eth->netdev[i]);
	}
}

static netdev_tx_t mtk_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct net_device_stats *stats = &dev->stats;
	bool gso = false;
	int tx_num;

	/* normally we can rely on the stack not calling this more than once,
	 * however we have 2 queues running on the same ring so we need to lock
	 * the ring access
	 */
	spin_lock(&eth->page_lock);

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		goto drop;

	tx_num = mtk_cal_txd_req(eth, skb);
	if (unlikely(atomic_read(&ring->free_count) <= tx_num)) {
		netif_tx_stop_all_queues(dev);
		netif_err(eth, tx_queued, dev,
			  "Tx Ring full when queue awake!\n");
		spin_unlock(&eth->page_lock);
		return NETDEV_TX_BUSY;
	}

	/* TSO: fill MSS info in tcp checksum field */
	if (skb_is_gso(skb)) {
		if (skb_cow_head(skb, 0)) {
			netif_warn(eth, tx_err, dev,
				   "GSO expand head fail.\n");
			goto drop;
		}

		if (skb_shinfo(skb)->gso_type &
				(SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) {
			gso = true;
			tcp_hdr(skb)->check = htons(skb_shinfo(skb)->gso_size);
		}
	}

	if (mtk_tx_map(skb, dev, tx_num, ring, gso) < 0)
		goto drop;

	if (unlikely(atomic_read(&ring->free_count) <= ring->thresh))
		netif_tx_stop_all_queues(dev);

	spin_unlock(&eth->page_lock);

	return NETDEV_TX_OK;

drop:
	spin_unlock(&eth->page_lock);
	stats->tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static struct mtk_rx_ring *mtk_get_rx_ring(struct mtk_eth *eth)
{
	int i;
	struct mtk_rx_ring *ring;
	int idx;

	if (!eth->hwlro)
		return &eth->rx_ring[0];

	for (i = 0; i < MTK_MAX_RX_RING_NUM; i++) {
		struct mtk_rx_dma *rxd;

		ring = &eth->rx_ring[i];
		idx = NEXT_DESP_IDX(ring->calc_idx, ring->dma_size);
		rxd = ring->dma + idx * eth->soc->rx.desc_size;
		if (rxd->rxd2 & RX_DMA_DONE) {
			ring->calc_idx_update = true;
			return ring;
		}
	}

	return NULL;
}

static void mtk_update_rx_cpu_idx(struct mtk_eth *eth)
{
	struct mtk_rx_ring *ring;
	int i;

	if (!eth->hwlro) {
		ring = &eth->rx_ring[0];
		mtk_w32(eth, ring->calc_idx, ring->crx_idx_reg);
	} else {
		for (i = 0; i < MTK_MAX_RX_RING_NUM; i++) {
			ring = &eth->rx_ring[i];
			if (ring->calc_idx_update) {
				ring->calc_idx_update = false;
				mtk_w32(eth, ring->calc_idx, ring->crx_idx_reg);
			}
		}
	}
}

static bool mtk_page_pool_enabled(struct mtk_eth *eth)
{
	return mtk_is_netsys_v2_or_greater(eth);
}

static struct page_pool *mtk_create_page_pool(struct mtk_eth *eth,
					      struct xdp_rxq_info *xdp_q,
					      int id, int size)
{
	struct page_pool_params pp_params = {
		.order = 0,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = size,
		.nid = NUMA_NO_NODE,
		.dev = eth->dma_dev,
		.offset = MTK_PP_HEADROOM,
		.max_len = MTK_PP_MAX_BUF_SIZE,
	};
	struct page_pool *pp;
	int err;

	pp_params.dma_dir = rcu_access_pointer(eth->prog) ? DMA_BIDIRECTIONAL
							  : DMA_FROM_DEVICE;
	pp = page_pool_create(&pp_params);
	if (IS_ERR(pp))
		return pp;

	err = __xdp_rxq_info_reg(xdp_q, eth->dummy_dev, id,
				 eth->rx_napi.napi_id, PAGE_SIZE);
	if (err < 0)
		goto err_free_pp;

	err = xdp_rxq_info_reg_mem_model(xdp_q, MEM_TYPE_PAGE_POOL, pp);
	if (err)
		goto err_unregister_rxq;

	return pp;

err_unregister_rxq:
	xdp_rxq_info_unreg(xdp_q);
err_free_pp:
	page_pool_destroy(pp);

	return ERR_PTR(err);
}

static void *mtk_page_pool_get_buff(struct page_pool *pp, dma_addr_t *dma_addr,
				    gfp_t gfp_mask)
{
	struct page *page;

	page = page_pool_alloc_pages(pp, gfp_mask | __GFP_NOWARN);
	if (!page)
		return NULL;

	*dma_addr = page_pool_get_dma_addr(page) + MTK_PP_HEADROOM;
	return page_address(page);
}

static void mtk_rx_put_buff(struct mtk_rx_ring *ring, void *data, bool napi)
{
	if (ring->page_pool)
		page_pool_put_full_page(ring->page_pool,
					virt_to_head_page(data), napi);
	else
		skb_free_frag(data);
}

static int mtk_xdp_frame_map(struct mtk_eth *eth, struct net_device *dev,
			     struct mtk_tx_dma_desc_info *txd_info,
			     struct mtk_tx_dma *txd, struct mtk_tx_buf *tx_buf,
			     void *data, u16 headroom, int index, bool dma_map)
{
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_tx_dma *txd_pdma;

	if (dma_map) {  /* ndo_xdp_xmit */
		txd_info->addr = dma_map_single(eth->dma_dev, data,
						txd_info->size, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(eth->dma_dev, txd_info->addr)))
			return -ENOMEM;

		tx_buf->flags |= MTK_TX_FLAGS_SINGLE0;
	} else {
		struct page *page = virt_to_head_page(data);

		txd_info->addr = page_pool_get_dma_addr(page) +
				 sizeof(struct xdp_frame) + headroom;
		dma_sync_single_for_device(eth->dma_dev, txd_info->addr,
					   txd_info->size, DMA_BIDIRECTIONAL);
	}
	mtk_tx_set_dma_desc(dev, txd, txd_info);

	tx_buf->mac_id = mac->id;
	tx_buf->type = dma_map ? MTK_TYPE_XDP_NDO : MTK_TYPE_XDP_TX;
	tx_buf->data = (void *)MTK_DMA_DUMMY_DESC;

	txd_pdma = qdma_to_pdma(ring, txd);
	setup_tx_buf(eth, tx_buf, txd_pdma, txd_info->addr, txd_info->size,
		     index);

	return 0;
}

static int mtk_xdp_submit_frame(struct mtk_eth *eth, struct xdp_frame *xdpf,
				struct net_device *dev, bool dma_map)
{
	struct skb_shared_info *sinfo = xdp_get_shared_info_from_frame(xdpf);
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_tx_dma_desc_info txd_info = {
		.size	= xdpf->len,
		.first	= true,
		.last	= !xdp_frame_has_frags(xdpf),
		.qid	= mac->id,
	};
	int err, index = 0, n_desc = 1, nr_frags;
	struct mtk_tx_buf *htx_buf, *tx_buf;
	struct mtk_tx_dma *htxd, *txd;
	void *data = xdpf->data;

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		return -EBUSY;

	nr_frags = unlikely(xdp_frame_has_frags(xdpf)) ? sinfo->nr_frags : 0;
	if (unlikely(atomic_read(&ring->free_count) <= 1 + nr_frags))
		return -EBUSY;

	spin_lock(&eth->page_lock);

	txd = ring->next_free;
	if (txd == ring->last_free) {
		spin_unlock(&eth->page_lock);
		return -ENOMEM;
	}
	htxd = txd;

	tx_buf = mtk_desc_to_tx_buf(ring, txd, soc->tx.desc_size);
	memset(tx_buf, 0, sizeof(*tx_buf));
	htx_buf = tx_buf;

	for (;;) {
		err = mtk_xdp_frame_map(eth, dev, &txd_info, txd, tx_buf,
					data, xdpf->headroom, index, dma_map);
		if (err < 0)
			goto unmap;

		if (txd_info.last)
			break;

		if (MTK_HAS_CAPS(soc->caps, MTK_QDMA) || (index & 0x1)) {
			txd = mtk_qdma_phys_to_virt(ring, txd->txd2);
			if (txd == ring->last_free)
				goto unmap;

			tx_buf = mtk_desc_to_tx_buf(ring, txd,
						    soc->tx.desc_size);
			memset(tx_buf, 0, sizeof(*tx_buf));
			n_desc++;
		}

		memset(&txd_info, 0, sizeof(struct mtk_tx_dma_desc_info));
		txd_info.size = skb_frag_size(&sinfo->frags[index]);
		txd_info.last = index + 1 == nr_frags;
		txd_info.qid = mac->id;
		data = skb_frag_address(&sinfo->frags[index]);

		index++;
	}
	/* store xdpf for cleanup */
	htx_buf->data = xdpf;

	if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		struct mtk_tx_dma *txd_pdma = qdma_to_pdma(ring, txd);

		if (index & 1)
			txd_pdma->txd2 |= TX_DMA_LS0;
		else
			txd_pdma->txd2 |= TX_DMA_LS1;
	}

	ring->next_free = mtk_qdma_phys_to_virt(ring, txd->txd2);
	atomic_sub(n_desc, &ring->free_count);

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		mtk_w32(eth, txd->txd2, soc->reg_map->qdma.ctx_ptr);
	} else {
		int idx;

		idx = txd_to_idx(ring, txd, soc->tx.desc_size);
		mtk_w32(eth, NEXT_DESP_IDX(idx, ring->dma_size),
			MT7628_TX_CTX_IDX0);
	}

	spin_unlock(&eth->page_lock);

	return 0;

unmap:
	while (htxd != txd) {
		tx_buf = mtk_desc_to_tx_buf(ring, htxd, soc->tx.desc_size);
		mtk_tx_unmap(eth, tx_buf, NULL, false);

		htxd->txd3 = TX_DMA_LS0 | TX_DMA_OWNER_CPU;
		if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
			struct mtk_tx_dma *txd_pdma = qdma_to_pdma(ring, htxd);

			txd_pdma->txd2 = TX_DMA_DESP2_DEF;
		}

		htxd = mtk_qdma_phys_to_virt(ring, htxd->txd2);
	}

	spin_unlock(&eth->page_lock);

	return err;
}

static int mtk_xdp_xmit(struct net_device *dev, int num_frame,
			struct xdp_frame **frames, u32 flags)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	struct mtk_eth *eth = mac->hw;
	int i, nxmit = 0;

	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK))
		return -EINVAL;

	for (i = 0; i < num_frame; i++) {
		if (mtk_xdp_submit_frame(eth, frames[i], dev, true))
			break;
		nxmit++;
	}

	u64_stats_update_begin(&hw_stats->syncp);
	hw_stats->xdp_stats.tx_xdp_xmit += nxmit;
	hw_stats->xdp_stats.tx_xdp_xmit_errors += num_frame - nxmit;
	u64_stats_update_end(&hw_stats->syncp);

	return nxmit;
}

static u32 mtk_xdp_run(struct mtk_eth *eth, struct mtk_rx_ring *ring,
		       struct xdp_buff *xdp, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	u64 *count = &hw_stats->xdp_stats.rx_xdp_drop;
	struct bpf_prog *prog;
	u32 act = XDP_PASS;

	rcu_read_lock();

	prog = rcu_dereference(eth->prog);
	if (!prog)
		goto out;

	act = bpf_prog_run_xdp(prog, xdp);
	switch (act) {
	case XDP_PASS:
		count = &hw_stats->xdp_stats.rx_xdp_pass;
		goto update_stats;
	case XDP_REDIRECT:
		if (unlikely(xdp_do_redirect(dev, xdp, prog))) {
			act = XDP_DROP;
			break;
		}

		count = &hw_stats->xdp_stats.rx_xdp_redirect;
		goto update_stats;
	case XDP_TX: {
		struct xdp_frame *xdpf = xdp_convert_buff_to_frame(xdp);

		if (!xdpf || mtk_xdp_submit_frame(eth, xdpf, dev, false)) {
			count = &hw_stats->xdp_stats.rx_xdp_tx_errors;
			act = XDP_DROP;
			break;
		}

		count = &hw_stats->xdp_stats.rx_xdp_tx;
		goto update_stats;
	}
	default:
		bpf_warn_invalid_xdp_action(dev, prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(dev, prog, act);
		fallthrough;
	case XDP_DROP:
		break;
	}

	page_pool_put_full_page(ring->page_pool,
				virt_to_head_page(xdp->data), true);

update_stats:
	u64_stats_update_begin(&hw_stats->syncp);
	*count = *count + 1;
	u64_stats_update_end(&hw_stats->syncp);
out:
	rcu_read_unlock();

	return act;
}

static int mtk_poll_rx(struct napi_struct *napi, int budget,
		       struct mtk_eth *eth)
{
	struct dim_sample dim_sample = {};
	struct mtk_rx_ring *ring;
	bool xdp_flush = false;
	int idx;
	struct sk_buff *skb;
	u64 addr64 = 0;
	u8 *data, *new_data;
	struct mtk_rx_dma_v2 *rxd, trxd;
	int done = 0, bytes = 0;
	dma_addr_t dma_addr = DMA_MAPPING_ERROR;
	int ppe_idx = 0;

	while (done < budget) {
		unsigned int pktlen, *rxdcsum;
		struct net_device *netdev;
		u32 hash, reason;
		int mac = 0;

		ring = mtk_get_rx_ring(eth);
		if (unlikely(!ring))
			goto rx_done;

		idx = NEXT_DESP_IDX(ring->calc_idx, ring->dma_size);
		rxd = ring->dma + idx * eth->soc->rx.desc_size;
		data = ring->data[idx];

		if (!mtk_rx_get_desc(eth, &trxd, rxd))
			break;

		/* find out which mac the packet come from. values start at 1 */
		if (mtk_is_netsys_v3_or_greater(eth)) {
			u32 val = RX_DMA_GET_SPORT_V2(trxd.rxd5);

			switch (val) {
			case PSE_GDM1_PORT:
			case PSE_GDM2_PORT:
				mac = val - 1;
				break;
			case PSE_GDM3_PORT:
				mac = MTK_GMAC3_ID;
				break;
			default:
				break;
			}
		} else if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628) &&
			   !(trxd.rxd4 & RX_DMA_SPECIAL_TAG)) {
			mac = RX_DMA_GET_SPORT(trxd.rxd4) - 1;
		}

		if (unlikely(mac < 0 || mac >= MTK_MAX_DEVS ||
			     !eth->netdev[mac]))
			goto release_desc;

		netdev = eth->netdev[mac];
		ppe_idx = eth->mac[mac]->ppe_idx;

		if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
			goto release_desc;

		pktlen = RX_DMA_GET_PLEN0(trxd.rxd2);

		/* alloc new buffer */
		if (ring->page_pool) {
			struct page *page = virt_to_head_page(data);
			struct xdp_buff xdp;
			u32 ret, metasize;

			new_data = mtk_page_pool_get_buff(ring->page_pool,
							  &dma_addr,
							  GFP_ATOMIC);
			if (unlikely(!new_data)) {
				netdev->stats.rx_dropped++;
				goto release_desc;
			}

			dma_sync_single_for_cpu(eth->dma_dev,
				page_pool_get_dma_addr(page) + MTK_PP_HEADROOM,
				pktlen, page_pool_get_dma_dir(ring->page_pool));

			xdp_init_buff(&xdp, PAGE_SIZE, &ring->xdp_q);
			xdp_prepare_buff(&xdp, data, MTK_PP_HEADROOM, pktlen,
					 true);
			xdp_buff_clear_frags_flag(&xdp);

			ret = mtk_xdp_run(eth, ring, &xdp, netdev);
			if (ret == XDP_REDIRECT)
				xdp_flush = true;

			if (ret != XDP_PASS)
				goto skip_rx;

			skb = build_skb(data, PAGE_SIZE);
			if (unlikely(!skb)) {
				page_pool_put_full_page(ring->page_pool,
							page, true);
				netdev->stats.rx_dropped++;
				goto skip_rx;
			}

			skb_reserve(skb, xdp.data - xdp.data_hard_start);
			skb_put(skb, xdp.data_end - xdp.data);
			metasize = xdp.data - xdp.data_meta;
			if (metasize)
				skb_metadata_set(skb, metasize);
			skb_mark_for_recycle(skb);
		} else {
			if (ring->frag_size <= PAGE_SIZE)
				new_data = napi_alloc_frag(ring->frag_size);
			else
				new_data = mtk_max_lro_buf_alloc(GFP_ATOMIC);

			if (unlikely(!new_data)) {
				netdev->stats.rx_dropped++;
				goto release_desc;
			}

			dma_addr = dma_map_single(eth->dma_dev,
				new_data + NET_SKB_PAD + eth->ip_align,
				ring->buf_size, DMA_FROM_DEVICE);
			if (unlikely(dma_mapping_error(eth->dma_dev,
						       dma_addr))) {
				skb_free_frag(new_data);
				netdev->stats.rx_dropped++;
				goto release_desc;
			}

			if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA))
				addr64 = RX_DMA_GET_ADDR64(trxd.rxd2);

			dma_unmap_single(eth->dma_dev, ((u64)trxd.rxd1 | addr64),
					 ring->buf_size, DMA_FROM_DEVICE);

			skb = build_skb(data, ring->frag_size);
			if (unlikely(!skb)) {
				netdev->stats.rx_dropped++;
				skb_free_frag(data);
				goto skip_rx;
			}

			skb_reserve(skb, NET_SKB_PAD + NET_IP_ALIGN);
			skb_put(skb, pktlen);
		}

		skb->dev = netdev;
		bytes += skb->len;

		if (mtk_is_netsys_v3_or_greater(eth)) {
			reason = FIELD_GET(MTK_RXD5_PPE_CPU_REASON, trxd.rxd5);
			hash = trxd.rxd5 & MTK_RXD5_FOE_ENTRY;
			if (hash != MTK_RXD5_FOE_ENTRY)
				skb_set_hash(skb, jhash_1word(hash, 0),
					     PKT_HASH_TYPE_L4);
			rxdcsum = &trxd.rxd3;
		} else {
			reason = FIELD_GET(MTK_RXD4_PPE_CPU_REASON, trxd.rxd4);
			hash = trxd.rxd4 & MTK_RXD4_FOE_ENTRY;
			if (hash != MTK_RXD4_FOE_ENTRY)
				skb_set_hash(skb, jhash_1word(hash, 0),
					     PKT_HASH_TYPE_L4);
			rxdcsum = &trxd.rxd4;
		}

		if (*rxdcsum & eth->soc->rx.dma_l4_valid)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb_checksum_none_assert(skb);
		skb->protocol = eth_type_trans(skb, netdev);

		/* When using VLAN untagging in combination with DSA, the
		 * hardware treats the MTK special tag as a VLAN and untags it.
		 */
		if (mtk_is_netsys_v1(eth) && (trxd.rxd2 & RX_DMA_VTAG) &&
		    netdev_uses_dsa(netdev)) {
			unsigned int port = RX_DMA_VPID(trxd.rxd3) & GENMASK(2, 0);

			if (port < ARRAY_SIZE(eth->dsa_meta) &&
			    eth->dsa_meta[port])
				skb_dst_set_noref(skb, &eth->dsa_meta[port]->dst);
		}

		if (reason == MTK_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
			mtk_ppe_check_skb(eth->ppe[ppe_idx], skb, hash);

		skb_record_rx_queue(skb, 0);
		napi_gro_receive(napi, skb);

skip_rx:
		ring->data[idx] = new_data;
		rxd->rxd1 = (unsigned int)dma_addr;
release_desc:
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA)) {
			if (unlikely(dma_addr == DMA_MAPPING_ERROR))
				addr64 = FIELD_GET(RX_DMA_ADDR64_MASK,
						   rxd->rxd2);
			else
				addr64 = RX_DMA_PREP_ADDR64(dma_addr);
		}

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
			rxd->rxd2 = RX_DMA_LSO;
		else
			rxd->rxd2 = RX_DMA_PREP_PLEN0(ring->buf_size) | addr64;

		ring->calc_idx = idx;
		done++;
	}

rx_done:
	if (done) {
		/* make sure that all changes to the dma ring are flushed before
		 * we continue
		 */
		wmb();
		mtk_update_rx_cpu_idx(eth);
	}

	eth->rx_packets += done;
	eth->rx_bytes += bytes;
	dim_update_sample(eth->rx_events, eth->rx_packets, eth->rx_bytes,
			  &dim_sample);
	net_dim(&eth->rx_dim, &dim_sample);

	if (xdp_flush)
		xdp_do_flush();

	return done;
}

struct mtk_poll_state {
    struct netdev_queue *txq;
    unsigned int total;
    unsigned int done;
    unsigned int bytes;
};

static void
mtk_poll_tx_done(struct mtk_eth *eth, struct mtk_poll_state *state, u8 mac,
		 struct sk_buff *skb)
{
	struct netdev_queue *txq;
	struct net_device *dev;
	unsigned int bytes = skb->len;

	state->total++;
	eth->tx_packets++;
	eth->tx_bytes += bytes;

	dev = eth->netdev[mac];
	if (!dev)
		return;

	txq = netdev_get_tx_queue(dev, skb_get_queue_mapping(skb));
	if (state->txq == txq) {
		state->done++;
		state->bytes += bytes;
		return;
	}

	if (state->txq)
		netdev_tx_completed_queue(state->txq, state->done, state->bytes);

	state->txq = txq;
	state->done = 1;
	state->bytes = bytes;
}

static int mtk_poll_tx_qdma(struct mtk_eth *eth, int budget,
			    struct mtk_poll_state *state)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_tx_buf *tx_buf;
	struct xdp_frame_bulk bq;
	struct mtk_tx_dma *desc;
	u32 cpu, dma;

	cpu = ring->last_free_ptr;
	dma = mtk_r32(eth, reg_map->qdma.drx_ptr);

	desc = mtk_qdma_phys_to_virt(ring, cpu);
	xdp_frame_bulk_init(&bq);

	while ((cpu != dma) && budget) {
		u32 next_cpu = desc->txd2;

		desc = mtk_qdma_phys_to_virt(ring, desc->txd2);
		if ((desc->txd3 & TX_DMA_OWNER_CPU) == 0)
			break;

		tx_buf = mtk_desc_to_tx_buf(ring, desc,
					    eth->soc->tx.desc_size);
		if (!tx_buf->data)
			break;

		if (tx_buf->data != (void *)MTK_DMA_DUMMY_DESC) {
			if (tx_buf->type == MTK_TYPE_SKB)
				mtk_poll_tx_done(eth, state, tx_buf->mac_id,
						 tx_buf->data);

			budget--;
		}
		mtk_tx_unmap(eth, tx_buf, &bq, true);

		ring->last_free = desc;
		atomic_inc(&ring->free_count);

		cpu = next_cpu;
	}
	xdp_flush_frame_bulk(&bq);

	ring->last_free_ptr = cpu;
	mtk_w32(eth, cpu, reg_map->qdma.crx_ptr);

	return budget;
}

static int mtk_poll_tx_pdma(struct mtk_eth *eth, int budget,
			    struct mtk_poll_state *state)
{
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_tx_buf *tx_buf;
	struct xdp_frame_bulk bq;
	struct mtk_tx_dma *desc;
	u32 cpu, dma;

	cpu = ring->cpu_idx;
	dma = mtk_r32(eth, MT7628_TX_DTX_IDX0);
	xdp_frame_bulk_init(&bq);

	while ((cpu != dma) && budget) {
		tx_buf = &ring->buf[cpu];
		if (!tx_buf->data)
			break;

		if (tx_buf->data != (void *)MTK_DMA_DUMMY_DESC) {
			if (tx_buf->type == MTK_TYPE_SKB)
				mtk_poll_tx_done(eth, state, 0, tx_buf->data);
			budget--;
		}
		mtk_tx_unmap(eth, tx_buf, &bq, true);

		desc = ring->dma + cpu * eth->soc->tx.desc_size;
		ring->last_free = desc;
		atomic_inc(&ring->free_count);

		cpu = NEXT_DESP_IDX(cpu, ring->dma_size);
	}
	xdp_flush_frame_bulk(&bq);

	ring->cpu_idx = cpu;

	return budget;
}

static int mtk_poll_tx(struct mtk_eth *eth, int budget)
{
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct dim_sample dim_sample = {};
	struct mtk_poll_state state = {};

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		budget = mtk_poll_tx_qdma(eth, budget, &state);
	else
		budget = mtk_poll_tx_pdma(eth, budget, &state);

	if (state.txq)
		netdev_tx_completed_queue(state.txq, state.done, state.bytes);

	dim_update_sample(eth->tx_events, eth->tx_packets, eth->tx_bytes,
			  &dim_sample);
	net_dim(&eth->tx_dim, &dim_sample);

	if (mtk_queue_stopped(eth) &&
	    (atomic_read(&ring->free_count) > ring->thresh))
		mtk_wake_queue(eth);

	return state.total;
}

static void mtk_handle_status_irq(struct mtk_eth *eth)
{
	u32 status2 = mtk_r32(eth, MTK_INT_STATUS2);

	if (unlikely(status2 & (MTK_GDM1_AF | MTK_GDM2_AF))) {
		mtk_stats_update(eth);
		mtk_w32(eth, (MTK_GDM1_AF | MTK_GDM2_AF),
			MTK_INT_STATUS2);
	}
}

static int mtk_napi_tx(struct napi_struct *napi, int budget)
{
	struct mtk_eth *eth = container_of(napi, struct mtk_eth, tx_napi);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int tx_done = 0;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		mtk_handle_status_irq(eth);
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->tx_irq_status);
	tx_done = mtk_poll_tx(eth, budget);

	if (unlikely(netif_msg_intr(eth))) {
		dev_info(eth->dev,
			 "done tx %d, intr 0x%08x/0x%x\n", tx_done,
			 mtk_r32(eth, reg_map->tx_irq_status),
			 mtk_r32(eth, reg_map->tx_irq_mask));
	}

	if (tx_done == budget)
		return budget;

	if (mtk_r32(eth, reg_map->tx_irq_status) & MTK_TX_DONE_INT)
		return budget;

	if (napi_complete_done(napi, tx_done))
		mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);

	return tx_done;
}

static int mtk_napi_rx(struct napi_struct *napi, int budget)
{
	struct mtk_eth *eth = container_of(napi, struct mtk_eth, rx_napi);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int rx_done_total = 0;

	mtk_handle_status_irq(eth);

	do {
		int rx_done;

		mtk_w32(eth, eth->soc->rx.irq_done_mask,
			reg_map->pdma.irq_status);
		rx_done = mtk_poll_rx(napi, budget - rx_done_total, eth);
		rx_done_total += rx_done;

		if (unlikely(netif_msg_intr(eth))) {
			dev_info(eth->dev,
				 "done rx %d, intr 0x%08x/0x%x\n", rx_done,
				 mtk_r32(eth, reg_map->pdma.irq_status),
				 mtk_r32(eth, reg_map->pdma.irq_mask));
		}

		if (rx_done_total == budget)
			return budget;

	} while (mtk_r32(eth, reg_map->pdma.irq_status) &
		 eth->soc->rx.irq_done_mask);

	if (napi_complete_done(napi, rx_done_total))
		mtk_rx_irq_enable(eth, eth->soc->rx.irq_done_mask);

	return rx_done_total;
}

static int mtk_tx_alloc(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	int i, sz = soc->tx.desc_size;
	struct mtk_tx_dma_v2 *txd;
	int ring_size;
	u32 ofs, val;

	if (MTK_HAS_CAPS(soc->caps, MTK_QDMA))
		ring_size = MTK_QDMA_RING_SIZE;
	else
		ring_size = soc->tx.dma_size;

	ring->buf = kcalloc(ring_size, sizeof(*ring->buf),
			       GFP_KERNEL);
	if (!ring->buf)
		goto no_tx_mem;

	ring->dma = mtk_dma_ring_alloc(eth, ring_size * sz, &ring->phys, true);
	if (!ring->dma)
		goto no_tx_mem;

	for (i = 0; i < ring_size; i++) {
		int next = (i + 1) % ring_size;
		u32 next_ptr = ring->phys + next * sz;

		txd = ring->dma + i * sz;
		txd->txd2 = next_ptr;
		txd->txd3 = TX_DMA_LS0 | TX_DMA_OWNER_CPU;
		txd->txd4 = 0;
		if (mtk_is_netsys_v2_or_greater(eth)) {
			txd->txd5 = 0;
			txd->txd6 = 0;
			txd->txd7 = 0;
			txd->txd8 = 0;
		}
	}

	/* On MT7688 (PDMA only) this driver uses the ring->dma structs
	 * only as the framework. The real HW descriptors are the PDMA
	 * descriptors in ring->dma_pdma.
	 */
	if (!MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		ring->dma_pdma = dma_alloc_coherent(eth->dma_dev, ring_size * sz,
						    &ring->phys_pdma, GFP_KERNEL);
		if (!ring->dma_pdma)
			goto no_tx_mem;

		for (i = 0; i < ring_size; i++) {
			ring->dma_pdma[i].txd2 = TX_DMA_DESP2_DEF;
			ring->dma_pdma[i].txd4 = 0;
		}
	}

	ring->dma_size = ring_size;
	atomic_set(&ring->free_count, ring_size - 2);
	ring->next_free = ring->dma;
	ring->last_free = (void *)txd;
	ring->last_free_ptr = (u32)(ring->phys + ((ring_size - 1) * sz));
	ring->thresh = MAX_SKB_FRAGS;

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (MTK_HAS_CAPS(soc->caps, MTK_QDMA)) {
		mtk_w32(eth, ring->phys, soc->reg_map->qdma.ctx_ptr);
		mtk_w32(eth, ring->phys, soc->reg_map->qdma.dtx_ptr);
		mtk_w32(eth,
			ring->phys + ((ring_size - 1) * sz),
			soc->reg_map->qdma.crx_ptr);
		mtk_w32(eth, ring->last_free_ptr, soc->reg_map->qdma.drx_ptr);

		for (i = 0, ofs = 0; i < MTK_QDMA_NUM_QUEUES; i++) {
			val = (QDMA_RES_THRES << 8) | QDMA_RES_THRES;
			mtk_w32(eth, val, soc->reg_map->qdma.qtx_cfg + ofs);

			val = MTK_QTX_SCH_MIN_RATE_EN |
			      /* minimum: 10 Mbps */
			      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_MAN, 1) |
			      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_EXP, 4) |
			      MTK_QTX_SCH_LEAKY_BUCKET_SIZE;
			if (mtk_is_netsys_v1(eth))
				val |= MTK_QTX_SCH_LEAKY_BUCKET_EN;
			mtk_w32(eth, val, soc->reg_map->qdma.qtx_sch + ofs);
			ofs += MTK_QTX_OFFSET;
		}
		val = MTK_QDMA_TX_SCH_MAX_WFQ | (MTK_QDMA_TX_SCH_MAX_WFQ << 16);
		mtk_w32(eth, val, soc->reg_map->qdma.tx_sch_rate);
		if (mtk_is_netsys_v2_or_greater(eth))
			mtk_w32(eth, val, soc->reg_map->qdma.tx_sch_rate + 4);
	} else {
		mtk_w32(eth, ring->phys_pdma, MT7628_TX_BASE_PTR0);
		mtk_w32(eth, ring_size, MT7628_TX_MAX_CNT0);
		mtk_w32(eth, 0, MT7628_TX_CTX_IDX0);
		mtk_w32(eth, MT7628_PST_DTX_IDX0, soc->reg_map->pdma.rst_idx);
	}

	return 0;

no_tx_mem:
	return -ENOMEM;
}

static void mtk_tx_clean(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	int i;

	if (ring->buf) {
		for (i = 0; i < ring->dma_size; i++)
			mtk_tx_unmap(eth, &ring->buf[i], NULL, false);
		kfree(ring->buf);
		ring->buf = NULL;
	}

	if (ring->dma) {
		mtk_dma_ring_free(eth, ring->dma_size * soc->tx.desc_size,
				  ring->dma, ring->phys, true);
		ring->dma = NULL;
	}

	if (ring->dma_pdma) {
		dma_free_coherent(eth->dma_dev,
				  ring->dma_size * soc->tx.desc_size,
				  ring->dma_pdma, ring->phys_pdma);
		ring->dma_pdma = NULL;
	}
}

static int mtk_rx_alloc(struct mtk_eth *eth, int ring_no, int rx_flag)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_rx_ring *ring;
	int rx_data_len, rx_dma_size;
	int i;

	if (rx_flag == MTK_RX_FLAGS_QDMA) {
		if (ring_no)
			return -EINVAL;
		ring = &eth->rx_ring_qdma;
	} else {
		ring = &eth->rx_ring[ring_no];
	}

	if (rx_flag == MTK_RX_FLAGS_HWLRO) {
		rx_data_len = MTK_MAX_LRO_RX_LENGTH;
		rx_dma_size = MTK_HW_LRO_DMA_SIZE;
	} else {
		rx_data_len = ETH_DATA_LEN;
		rx_dma_size = soc->rx.dma_size;
	}

	ring->frag_size = mtk_max_frag_size(rx_data_len);
	ring->buf_size = mtk_max_buf_size(ring->frag_size);
	ring->data = kcalloc(rx_dma_size, sizeof(*ring->data),
			     GFP_KERNEL);
	if (!ring->data)
		return -ENOMEM;

	if (mtk_page_pool_enabled(eth)) {
		struct page_pool *pp;

		pp = mtk_create_page_pool(eth, &ring->xdp_q, ring_no,
					  rx_dma_size);
		if (IS_ERR(pp))
			return PTR_ERR(pp);

		ring->page_pool = pp;
	}

	ring->dma = mtk_dma_ring_alloc(eth,
				       rx_dma_size * eth->soc->rx.desc_size,
				       &ring->phys,
				       rx_flag == MTK_RX_FLAGS_NORMAL);
	if (!ring->dma)
		return -ENOMEM;

	for (i = 0; i < rx_dma_size; i++) {
		struct mtk_rx_dma_v2 *rxd;
		dma_addr_t dma_addr;
		void *data;

		rxd = ring->dma + i * eth->soc->rx.desc_size;
		if (ring->page_pool) {
			data = mtk_page_pool_get_buff(ring->page_pool,
						      &dma_addr, GFP_KERNEL);
			if (!data)
				return -ENOMEM;
		} else {
			if (ring->frag_size <= PAGE_SIZE)
				data = netdev_alloc_frag(ring->frag_size);
			else
				data = mtk_max_lro_buf_alloc(GFP_KERNEL);

			if (!data)
				return -ENOMEM;

			dma_addr = dma_map_single(eth->dma_dev,
				data + NET_SKB_PAD + eth->ip_align,
				ring->buf_size, DMA_FROM_DEVICE);
			if (unlikely(dma_mapping_error(eth->dma_dev,
						       dma_addr))) {
				skb_free_frag(data);
				return -ENOMEM;
			}
		}
		rxd->rxd1 = (unsigned int)dma_addr;
		ring->data[i] = data;

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
			rxd->rxd2 = RX_DMA_LSO;
		else
			rxd->rxd2 = RX_DMA_PREP_PLEN0(ring->buf_size);

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA))
			rxd->rxd2 |= RX_DMA_PREP_ADDR64(dma_addr);

		rxd->rxd3 = 0;
		rxd->rxd4 = 0;
		if (mtk_is_netsys_v3_or_greater(eth)) {
			rxd->rxd5 = 0;
			rxd->rxd6 = 0;
			rxd->rxd7 = 0;
			rxd->rxd8 = 0;
		}
	}

	ring->dma_size = rx_dma_size;
	ring->calc_idx_update = false;
	ring->calc_idx = rx_dma_size - 1;
	if (rx_flag == MTK_RX_FLAGS_QDMA)
		ring->crx_idx_reg = reg_map->qdma.qcrx_ptr +
				    ring_no * MTK_QRX_OFFSET;
	else
		ring->crx_idx_reg = reg_map->pdma.pcrx_ptr +
				    ring_no * MTK_QRX_OFFSET;
	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (rx_flag == MTK_RX_FLAGS_QDMA) {
		mtk_w32(eth, ring->phys,
			reg_map->qdma.rx_ptr + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, rx_dma_size,
			reg_map->qdma.rx_cnt_cfg + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, MTK_PST_DRX_IDX_CFG(ring_no),
			reg_map->qdma.rst_idx);
	} else {
		mtk_w32(eth, ring->phys,
			reg_map->pdma.rx_ptr + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, rx_dma_size,
			reg_map->pdma.rx_cnt_cfg + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, MTK_PST_DRX_IDX_CFG(ring_no),
			reg_map->pdma.rst_idx);
	}
	mtk_w32(eth, ring->calc_idx, ring->crx_idx_reg);

	return 0;
}

static void mtk_rx_clean(struct mtk_eth *eth, struct mtk_rx_ring *ring, bool in_sram)
{
	u64 addr64 = 0;
	int i;

	if (ring->data && ring->dma) {
		for (i = 0; i < ring->dma_size; i++) {
			struct mtk_rx_dma *rxd;

			if (!ring->data[i])
				continue;

			rxd = ring->dma + i * eth->soc->rx.desc_size;
			if (!rxd->rxd1)
				continue;

			if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA))
				addr64 = RX_DMA_GET_ADDR64(rxd->rxd2);

			dma_unmap_single(eth->dma_dev, ((u64)rxd->rxd1 | addr64),
					 ring->buf_size, DMA_FROM_DEVICE);
			mtk_rx_put_buff(ring, ring->data[i], false);
		}
		kfree(ring->data);
		ring->data = NULL;
	}

	if (ring->dma) {
		mtk_dma_ring_free(eth, ring->dma_size * eth->soc->rx.desc_size,
				  ring->dma, ring->phys, in_sram);
		ring->dma = NULL;
	}

	if (ring->page_pool) {
		if (xdp_rxq_info_is_reg(&ring->xdp_q))
			xdp_rxq_info_unreg(&ring->xdp_q);
		page_pool_destroy(ring->page_pool);
		ring->page_pool = NULL;
	}
}

static int mtk_hwlro_rx_init(struct mtk_eth *eth)
{
	int i;
	u32 ring_ctrl_dw1 = 0, ring_ctrl_dw2 = 0, ring_ctrl_dw3 = 0;
	u32 lro_ctrl_dw0 = 0, lro_ctrl_dw3 = 0;

	/* set LRO rings to auto-learn modes */
	ring_ctrl_dw2 |= MTK_RING_AUTO_LERAN_MODE;

	/* validate LRO ring */
	ring_ctrl_dw2 |= MTK_RING_VLD;

	/* set AGE timer (unit: 20us) */
	ring_ctrl_dw2 |= MTK_RING_AGE_TIME_H;
	ring_ctrl_dw1 |= MTK_RING_AGE_TIME_L;

	/* set max AGG timer (unit: 20us) */
	ring_ctrl_dw2 |= MTK_RING_MAX_AGG_TIME;

	/* set max LRO AGG count */
	ring_ctrl_dw2 |= MTK_RING_MAX_AGG_CNT_L;
	ring_ctrl_dw3 |= MTK_RING_MAX_AGG_CNT_H;

	for (i = 1; i < MTK_MAX_RX_RING_NUM; i++) {
		mtk_w32(eth, ring_ctrl_dw1, MTK_LRO_CTRL_DW1_CFG(i));
		mtk_w32(eth, ring_ctrl_dw2, MTK_LRO_CTRL_DW2_CFG(i));
		mtk_w32(eth, ring_ctrl_dw3, MTK_LRO_CTRL_DW3_CFG(i));
	}

	/* IPv4 checksum update enable */
	lro_ctrl_dw0 |= MTK_L3_CKS_UPD_EN;

	/* switch priority comparison to packet count mode */
	lro_ctrl_dw0 |= MTK_LRO_ALT_PKT_CNT_MODE;

	/* bandwidth threshold setting */
	mtk_w32(eth, MTK_HW_LRO_BW_THRE, MTK_PDMA_LRO_CTRL_DW2);

	/* auto-learn score delta setting */
	mtk_w32(eth, MTK_HW_LRO_REPLACE_DELTA, MTK_PDMA_LRO_ALT_SCORE_DELTA);

	/* set refresh timer for altering flows to 1 sec. (unit: 20us) */
	mtk_w32(eth, (MTK_HW_LRO_TIMER_UNIT << 16) | MTK_HW_LRO_REFRESH_TIME,
		MTK_PDMA_LRO_ALT_REFRESH_TIMER);

	/* set HW LRO mode & the max aggregation count for rx packets */
	lro_ctrl_dw3 |= MTK_ADMA_MODE | (MTK_HW_LRO_MAX_AGG_CNT & 0xff);

	/* the minimal remaining room of SDL0 in RXD for lro aggregation */
	lro_ctrl_dw3 |= MTK_LRO_MIN_RXD_SDL;

	/* enable HW LRO */
	lro_ctrl_dw0 |= MTK_LRO_EN;

	mtk_w32(eth, lro_ctrl_dw3, MTK_PDMA_LRO_CTRL_DW3);
	mtk_w32(eth, lro_ctrl_dw0, MTK_PDMA_LRO_CTRL_DW0);

	return 0;
}

static void mtk_hwlro_rx_uninit(struct mtk_eth *eth)
{
	int i;
	u32 val;

	/* relinquish lro rings, flush aggregated packets */
	mtk_w32(eth, MTK_LRO_RING_RELINQUISH_REQ, MTK_PDMA_LRO_CTRL_DW0);

	/* wait for relinquishments done */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, MTK_PDMA_LRO_CTRL_DW0);
		if (val & MTK_LRO_RING_RELINQUISH_DONE) {
			msleep(20);
			continue;
		}
		break;
	}

	/* invalidate lro rings */
	for (i = 1; i < MTK_MAX_RX_RING_NUM; i++)
		mtk_w32(eth, 0, MTK_LRO_CTRL_DW2_CFG(i));

	/* disable HW LRO */
	mtk_w32(eth, 0, MTK_PDMA_LRO_CTRL_DW0);
}

static void mtk_hwlro_val_ipaddr(struct mtk_eth *eth, int idx, __be32 ip)
{
	u32 reg_val;

	reg_val = mtk_r32(eth, MTK_LRO_CTRL_DW2_CFG(idx));

	/* invalidate the IP setting */
	mtk_w32(eth, (reg_val & ~MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));

	mtk_w32(eth, ip, MTK_LRO_DIP_DW0_CFG(idx));

	/* validate the IP setting */
	mtk_w32(eth, (reg_val | MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));
}

static void mtk_hwlro_inval_ipaddr(struct mtk_eth *eth, int idx)
{
	u32 reg_val;

	reg_val = mtk_r32(eth, MTK_LRO_CTRL_DW2_CFG(idx));

	/* invalidate the IP setting */
	mtk_w32(eth, (reg_val & ~MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));

	mtk_w32(eth, 0, MTK_LRO_DIP_DW0_CFG(idx));
}

static int mtk_hwlro_get_ip_cnt(struct mtk_mac *mac)
{
	int cnt = 0;
	int i;

	for (i = 0; i < MTK_MAX_LRO_IP_CNT; i++) {
		if (mac->hwlro_ip[i])
			cnt++;
	}

	return cnt;
}

static int mtk_hwlro_add_ipaddr(struct net_device *dev,
				struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int hwlro_idx;

	if ((fsp->flow_type != TCP_V4_FLOW) ||
	    (!fsp->h_u.tcp_ip4_spec.ip4dst) ||
	    (fsp->location > 1))
		return -EINVAL;

	mac->hwlro_ip[fsp->location] = htonl(fsp->h_u.tcp_ip4_spec.ip4dst);
	hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + fsp->location;

	mac->hwlro_ip_cnt = mtk_hwlro_get_ip_cnt(mac);

	mtk_hwlro_val_ipaddr(eth, hwlro_idx, mac->hwlro_ip[fsp->location]);

	return 0;
}

static int mtk_hwlro_del_ipaddr(struct net_device *dev,
				struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int hwlro_idx;

	if (fsp->location > 1)
		return -EINVAL;

	mac->hwlro_ip[fsp->location] = 0;
	hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + fsp->location;

	mac->hwlro_ip_cnt = mtk_hwlro_get_ip_cnt(mac);

	mtk_hwlro_inval_ipaddr(eth, hwlro_idx);

	return 0;
}

static void mtk_hwlro_netdev_disable(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int i, hwlro_idx;

	for (i = 0; i < MTK_MAX_LRO_IP_CNT; i++) {
		mac->hwlro_ip[i] = 0;
		hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + i;

		mtk_hwlro_inval_ipaddr(eth, hwlro_idx);
	}

	mac->hwlro_ip_cnt = 0;
}

static int mtk_hwlro_get_fdir_entry(struct net_device *dev,
				    struct ethtool_rxnfc *cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;

	if (fsp->location >= ARRAY_SIZE(mac->hwlro_ip))
		return -EINVAL;

	/* only tcp dst ipv4 is meaningful, others are meaningless */
	fsp->flow_type = TCP_V4_FLOW;
	fsp->h_u.tcp_ip4_spec.ip4dst = ntohl(mac->hwlro_ip[fsp->location]);
	fsp->m_u.tcp_ip4_spec.ip4dst = 0;

	fsp->h_u.tcp_ip4_spec.ip4src = 0;
	fsp->m_u.tcp_ip4_spec.ip4src = 0xffffffff;
	fsp->h_u.tcp_ip4_spec.psrc = 0;
	fsp->m_u.tcp_ip4_spec.psrc = 0xffff;
	fsp->h_u.tcp_ip4_spec.pdst = 0;
	fsp->m_u.tcp_ip4_spec.pdst = 0xffff;
	fsp->h_u.tcp_ip4_spec.tos = 0;
	fsp->m_u.tcp_ip4_spec.tos = 0xff;

	return 0;
}

static int mtk_hwlro_get_fdir_all(struct net_device *dev,
				  struct ethtool_rxnfc *cmd,
				  u32 *rule_locs)
{
	struct mtk_mac *mac = netdev_priv(dev);
	int cnt = 0;
	int i;

	for (i = 0; i < MTK_MAX_LRO_IP_CNT; i++) {
		if (cnt == cmd->rule_cnt)
			return -EMSGSIZE;

		if (mac->hwlro_ip[i]) {
			rule_locs[cnt] = i;
			cnt++;
		}
	}

	cmd->rule_cnt = cnt;

	return 0;
}

static netdev_features_t mtk_fix_features(struct net_device *dev,
					  netdev_features_t features)
{
	if (!(features & NETIF_F_LRO)) {
		struct mtk_mac *mac = netdev_priv(dev);
		int ip_cnt = mtk_hwlro_get_ip_cnt(mac);

		if (ip_cnt) {
			netdev_info(dev, "RX flow is programmed, LRO should keep on\n");

			features |= NETIF_F_LRO;
		}
	}

	return features;
}

static int mtk_set_features(struct net_device *dev, netdev_features_t features)
{
	netdev_features_t diff = dev->features ^ features;

	if ((diff & NETIF_F_LRO) && !(features & NETIF_F_LRO))
		mtk_hwlro_netdev_disable(dev);

	return 0;
}

/* wait for DMA to finish whatever it is doing before we start using it again */
static int mtk_dma_busy_wait(struct mtk_eth *eth)
{
	unsigned int reg;
	int ret;
	u32 val;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		reg = eth->soc->reg_map->qdma.glo_cfg;
	else
		reg = eth->soc->reg_map->pdma.glo_cfg;

	ret = readx_poll_timeout_atomic(__raw_readl, eth->base + reg, val,
					!(val & (MTK_RX_DMA_BUSY | MTK_TX_DMA_BUSY)),
					5, MTK_DMA_BUSY_TIMEOUT_US);
	if (ret)
		dev_err(eth->dev, "DMA init timeout\n");

	return ret;
}

static int mtk_dma_init(struct mtk_eth *eth)
{
	int err;
	u32 i;

	if (mtk_dma_busy_wait(eth))
		return -EBUSY;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		/* QDMA needs scratch memory for internal reordering of the
		 * descriptors
		 */
		err = mtk_init_fq_dma(eth);
		if (err)
			return err;
	}

	err = mtk_tx_alloc(eth);
	if (err)
		return err;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		err = mtk_rx_alloc(eth, 0, MTK_RX_FLAGS_QDMA);
		if (err)
			return err;
	}

	err = mtk_rx_alloc(eth, 0, MTK_RX_FLAGS_NORMAL);
	if (err)
		return err;

	if (eth->hwlro) {
		for (i = 1; i < MTK_MAX_RX_RING_NUM; i++) {
			err = mtk_rx_alloc(eth, i, MTK_RX_FLAGS_HWLRO);
			if (err)
				return err;
		}
		err = mtk_hwlro_rx_init(eth);
		if (err)
			return err;
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		/* Enable random early drop and set drop threshold
		 * automatically
		 */
		mtk_w32(eth, FC_THRES_DROP_MODE | FC_THRES_DROP_EN |
			FC_THRES_MIN, eth->soc->reg_map->qdma.fc_th);
		mtk_w32(eth, 0x0, eth->soc->reg_map->qdma.hred);
	}

	return 0;
}

static void mtk_dma_free(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	int i, j, txqs = 1;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		txqs = MTK_QDMA_NUM_QUEUES;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		for (j = 0; j < txqs; j++)
			netdev_tx_reset_subqueue(eth->netdev[i], j);
	}

	if (eth->scratch_ring) {
		mtk_dma_ring_free(eth, soc->tx.fq_dma_size * soc->tx.desc_size,
				  eth->scratch_ring, eth->phy_scratch_ring,
				  true);
		eth->scratch_ring = NULL;
		eth->phy_scratch_ring = 0;
	}

	mtk_tx_clean(eth);
	mtk_rx_clean(eth, &eth->rx_ring[0], true);
	mtk_rx_clean(eth, &eth->rx_ring_qdma, false);

	if (eth->hwlro) {
		mtk_hwlro_rx_uninit(eth);
		for (i = 1; i < MTK_MAX_RX_RING_NUM; i++)
			mtk_rx_clean(eth, &eth->rx_ring[i], false);
	}

	for (i = 0; i < DIV_ROUND_UP(soc->tx.fq_dma_size, MTK_FQ_DMA_LENGTH); i++) {
		kfree(eth->scratch_head[i]);
		eth->scratch_head[i] = NULL;
	}
}

static bool mtk_hw_reset_check(struct mtk_eth *eth)
{
	u32 val = mtk_r32(eth, MTK_INT_STATUS2);

	return (val & MTK_FE_INT_FQ_EMPTY) || (val & MTK_FE_INT_RFIFO_UF) ||
	       (val & MTK_FE_INT_RFIFO_OV) || (val & MTK_FE_INT_TSO_FAIL) ||
	       (val & MTK_FE_INT_TSO_ALIGN) || (val & MTK_FE_INT_TSO_ILLEGAL);
}

static void mtk_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	if (test_bit(MTK_RESETTING, &eth->state))
		return;

	if (!mtk_hw_reset_check(eth))
		return;

	eth->netdev[mac->id]->stats.tx_errors++;
	netif_err(eth, tx_err, dev, "transmit timed out\n");

	schedule_work(&eth->pending_work);
}

static int mtk_get_irqs(struct platform_device *pdev, struct mtk_eth *eth)
{
	int i;

	/* future SoCs beginning with MT7988 should use named IRQs in dts */
	eth->irq[MTK_FE_IRQ_TX] = platform_get_irq_byname_optional(pdev, "fe1");
	eth->irq[MTK_FE_IRQ_RX] = platform_get_irq_byname_optional(pdev, "fe2");
	if (eth->irq[MTK_FE_IRQ_TX] >= 0 && eth->irq[MTK_FE_IRQ_RX] >= 0)
		return 0;

	/* only use legacy mode if platform_get_irq_byname_optional returned -ENXIO */
	if (eth->irq[MTK_FE_IRQ_TX] != -ENXIO)
		return dev_err_probe(&pdev->dev, eth->irq[MTK_FE_IRQ_TX],
				     "Error requesting FE TX IRQ\n");

	if (eth->irq[MTK_FE_IRQ_RX] != -ENXIO)
		return dev_err_probe(&pdev->dev, eth->irq[MTK_FE_IRQ_RX],
				     "Error requesting FE RX IRQ\n");

	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SHARED_INT))
		dev_warn(&pdev->dev, "legacy DT: missing interrupt-names.");

	/* legacy way:
	 * On MTK_SHARED_INT SoCs (MT7621 + MT7628) the first IRQ is taken
	 * from devicetree and used for both RX and TX - it is shared.
	 * On SoCs with non-shared IRQs the first entry is not used,
	 * the second is for TX, and the third is for RX.
	 */
	for (i = 0; i < MTK_FE_IRQ_NUM; i++) {
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_SHARED_INT)) {
			if (i == MTK_FE_IRQ_SHARED)
				eth->irq[MTK_FE_IRQ_SHARED] = platform_get_irq(pdev, i);
			else
				eth->irq[i] = eth->irq[MTK_FE_IRQ_SHARED];
		} else {
			eth->irq[i] = platform_get_irq(pdev, i + 1);
		}

		if (eth->irq[i] < 0) {
			dev_err(&pdev->dev, "no IRQ%d resource found\n", i);
			return -ENXIO;
		}
	}

	return 0;
}

static irqreturn_t mtk_handle_irq_rx(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;

	eth->rx_events++;
	if (likely(napi_schedule_prep(&eth->rx_napi))) {
		mtk_rx_irq_disable(eth, eth->soc->rx.irq_done_mask);
		__napi_schedule(&eth->rx_napi);
	}

	return IRQ_HANDLED;
}

static irqreturn_t mtk_handle_irq_tx(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;

	eth->tx_events++;
	if (likely(napi_schedule_prep(&eth->tx_napi))) {
		mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
		__napi_schedule(&eth->tx_napi);
	}

	return IRQ_HANDLED;
}

static irqreturn_t mtk_handle_irq(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;

	if (mtk_r32(eth, reg_map->pdma.irq_mask) &
	    eth->soc->rx.irq_done_mask) {
		if (mtk_r32(eth, reg_map->pdma.irq_status) &
		    eth->soc->rx.irq_done_mask)
			mtk_handle_irq_rx(irq, _eth);
	}
	if (mtk_r32(eth, reg_map->tx_irq_mask) & MTK_TX_DONE_INT) {
		if (mtk_r32(eth, reg_map->tx_irq_status) & MTK_TX_DONE_INT)
			mtk_handle_irq_tx(irq, _eth);
	}

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void mtk_poll_controller(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_disable(eth, eth->soc->rx.irq_done_mask);
	mtk_handle_irq_rx(eth->irq[MTK_FE_IRQ_RX], dev);
	mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_enable(eth, eth->soc->rx.irq_done_mask);
}
#endif

static int mtk_start_dma(struct mtk_eth *eth)
{
	u32 val, rx_2b_offset = (NET_IP_ALIGN == 2) ? MTK_RX_2B_OFFSET : 0;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int err;

	err = mtk_dma_init(eth);
	if (err) {
		mtk_dma_free(eth);
		return err;
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		val = mtk_r32(eth, reg_map->qdma.glo_cfg);
		val |= MTK_TX_DMA_EN | MTK_RX_DMA_EN |
		       MTK_TX_BT_32DWORDS | MTK_NDP_CO_PRO |
		       MTK_RX_2B_OFFSET | MTK_TX_WB_DDONE;

		if (mtk_is_netsys_v2_or_greater(eth))
			val |= MTK_MUTLI_CNT | MTK_RESV_BUF |
			       MTK_WCOMP_EN | MTK_DMAD_WR_WDONE |
			       MTK_CHK_DDONE_EN;
		else
			val |= MTK_RX_BT_32DWORDS;
		mtk_w32(eth, val, reg_map->qdma.glo_cfg);

		mtk_w32(eth,
			MTK_RX_DMA_EN | rx_2b_offset |
			MTK_RX_BT_32DWORDS | MTK_MULTI_EN,
			reg_map->pdma.glo_cfg);
	} else {
		mtk_w32(eth, MTK_TX_WB_DDONE | MTK_TX_DMA_EN | MTK_RX_DMA_EN |
			MTK_MULTI_EN | MTK_PDMA_SIZE_8DWORDS,
			reg_map->pdma.glo_cfg);
	}

	return 0;
}

static void mtk_gdm_config(struct mtk_eth *eth, u32 id, u32 config)
{
	u32 val;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		return;

	val = mtk_r32(eth, MTK_GDMA_FWD_CFG(id));

	/* default setup the forward port to send frame to PDMA */
	val &= ~0xffff;

	/* Enable RX checksum */
	val |= MTK_GDMA_ICS_EN | MTK_GDMA_TCS_EN | MTK_GDMA_UCS_EN;

	val |= config;

	if (eth->netdev[id] && netdev_uses_dsa(eth->netdev[id]))
		val |= MTK_GDMA_SPECIAL_TAG;

	mtk_w32(eth, val, MTK_GDMA_FWD_CFG(id));
}


static bool mtk_uses_dsa(struct net_device *dev)
{
#if IS_ENABLED(CONFIG_NET_DSA)
	return netdev_uses_dsa(dev) &&
	       dev->dsa_ptr->tag_ops->proto == DSA_TAG_PROTO_MTK;
#else
	return false;
#endif
}

static int mtk_device_event(struct notifier_block *n, unsigned long event, void *ptr)
{
	struct mtk_mac *mac = container_of(n, struct mtk_mac, device_notifier);
	struct mtk_eth *eth = mac->hw;
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct ethtool_link_ksettings s;
	struct net_device *ldev;
	struct list_head *iter;
	struct dsa_port *dp;

	if (event != NETDEV_CHANGE)
		return NOTIFY_DONE;

	netdev_for_each_lower_dev(dev, ldev, iter) {
		if (netdev_priv(ldev) == mac)
			goto found;
	}

	return NOTIFY_DONE;

found:
	if (!dsa_user_dev_check(dev))
		return NOTIFY_DONE;

	if (__ethtool_get_link_ksettings(dev, &s))
		return NOTIFY_DONE;

	if (s.base.speed == 0 || s.base.speed == ((__u32)-1))
		return NOTIFY_DONE;

	dp = dsa_port_from_netdev(dev);
	if (dp->index >= MTK_QDMA_NUM_QUEUES)
		return NOTIFY_DONE;

	if (mac->speed > 0 && mac->speed <= s.base.speed)
		s.base.speed = 0;

	mtk_set_queue_speed(eth, dp->index + 3, s.base.speed);

	return NOTIFY_DONE;
}

static int mtk_open(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_mac *target_mac;
	int i, err, ppe_num;

	ppe_num = eth->soc->ppe_num;

	err = phylink_of_phy_connect(mac->phylink, mac->of_node, 0);
	if (err) {
		netdev_err(dev, "%s: could not attach PHY: %d\n", __func__,
			   err);
		return err;
	}

	/* we run 2 netdevs on the same dma ring so we only bring it up once */
	if (!refcount_read(&eth->dma_refcnt)) {
		const struct mtk_soc_data *soc = eth->soc;
		u32 gdm_config;
		int i;

		err = mtk_start_dma(eth);
		if (err) {
			phylink_disconnect_phy(mac->phylink);
			return err;
		}

		for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
			mtk_ppe_start(eth->ppe[i]);

		for (i = 0; i < MTK_MAX_DEVS; i++) {
			if (!eth->netdev[i])
				continue;

			target_mac = netdev_priv(eth->netdev[i]);
			if (!soc->offload_version) {
				target_mac->ppe_idx = 0;
				gdm_config = MTK_GDMA_TO_PDMA;
			} else if (ppe_num >= 3 && target_mac->id == 2) {
				target_mac->ppe_idx = 2;
				gdm_config = soc->reg_map->gdma_to_ppe[2];
			} else if (ppe_num >= 2 && target_mac->id == 1) {
				target_mac->ppe_idx = 1;
				gdm_config = soc->reg_map->gdma_to_ppe[1];
			} else {
				target_mac->ppe_idx = 0;
				gdm_config = soc->reg_map->gdma_to_ppe[0];
			}
			mtk_gdm_config(eth, target_mac->id, gdm_config);
		}

		napi_enable(&eth->tx_napi);
		napi_enable(&eth->rx_napi);
		mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);
		mtk_rx_irq_enable(eth, soc->rx.irq_done_mask);
		refcount_set(&eth->dma_refcnt, 1);
	} else {
		refcount_inc(&eth->dma_refcnt);
	}

	phylink_start(mac->phylink);
	netif_tx_start_all_queues(dev);

	if (mtk_is_netsys_v2_or_greater(eth))
		return 0;

	if (mtk_uses_dsa(dev) && !eth->prog) {
		for (i = 0; i < ARRAY_SIZE(eth->dsa_meta); i++) {
			struct metadata_dst *md_dst = eth->dsa_meta[i];

			if (md_dst)
				continue;

			md_dst = metadata_dst_alloc(0, METADATA_HW_PORT_MUX,
						    GFP_KERNEL);
			if (!md_dst)
				return -ENOMEM;

			md_dst->u.port_info.port_id = i;
			eth->dsa_meta[i] = md_dst;
		}
	} else {
		/* Hardware DSA untagging and VLAN RX offloading need to be
		 * disabled if at least one MAC does not use DSA.
		 */
		u32 val = mtk_r32(eth, MTK_CDMP_IG_CTRL);

		val &= ~MTK_CDMP_STAG_EN;
		mtk_w32(eth, val, MTK_CDMP_IG_CTRL);

		mtk_w32(eth, 0, MTK_CDMP_EG_CTRL);
	}

	return 0;
}

static void mtk_stop_dma(struct mtk_eth *eth, u32 glo_cfg)
{
	u32 val;
	int i;

	/* stop the dma engine */
	spin_lock_bh(&eth->page_lock);
	val = mtk_r32(eth, glo_cfg);
	mtk_w32(eth, val & ~(MTK_TX_WB_DDONE | MTK_RX_DMA_EN | MTK_TX_DMA_EN),
		glo_cfg);
	spin_unlock_bh(&eth->page_lock);

	/* wait for dma stop */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, glo_cfg);
		if (val & (MTK_TX_DMA_BUSY | MTK_RX_DMA_BUSY)) {
			msleep(20);
			continue;
		}
		break;
	}
}

static int mtk_stop(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int i;

	phylink_stop(mac->phylink);

	netif_tx_disable(dev);

	phylink_disconnect_phy(mac->phylink);

	/* only shutdown DMA if this is the last user */
	if (!refcount_dec_and_test(&eth->dma_refcnt))
		return 0;

	for (i = 0; i < MTK_MAX_DEVS; i++)
		mtk_gdm_config(eth, i, MTK_GDMA_DROP_ALL);

	mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_disable(eth, eth->soc->rx.irq_done_mask);
	napi_disable(&eth->tx_napi);
	napi_disable(&eth->rx_napi);

	cancel_work_sync(&eth->rx_dim.work);
	cancel_work_sync(&eth->tx_dim.work);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		mtk_stop_dma(eth, eth->soc->reg_map->qdma.glo_cfg);
	mtk_stop_dma(eth, eth->soc->reg_map->pdma.glo_cfg);

	mtk_dma_free(eth);

	for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
		mtk_ppe_stop(eth->ppe[i]);

	return 0;
}

static int mtk_xdp_setup(struct net_device *dev, struct bpf_prog *prog,
			 struct netlink_ext_ack *extack)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct bpf_prog *old_prog;
	bool need_update;

	if (eth->hwlro) {
		NL_SET_ERR_MSG_MOD(extack, "XDP not supported with HWLRO");
		return -EOPNOTSUPP;
	}

	if (dev->mtu > MTK_PP_MAX_BUF_SIZE) {
		NL_SET_ERR_MSG_MOD(extack, "MTU too large for XDP");
		return -EOPNOTSUPP;
	}

	need_update = !!eth->prog != !!prog;
	if (netif_running(dev) && need_update)
		mtk_stop(dev);

	old_prog = rcu_replace_pointer(eth->prog, prog, lockdep_rtnl_is_held());
	if (old_prog)
		bpf_prog_put(old_prog);

	if (netif_running(dev) && need_update)
		return mtk_open(dev);

	return 0;
}

static int mtk_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return mtk_xdp_setup(dev, xdp->prog, xdp->extack);
	default:
		return -EINVAL;
	}
}

static void ethsys_reset(struct mtk_eth *eth, u32 reset_bits)
{
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL,
			   reset_bits,
			   reset_bits);

	usleep_range(1000, 1100);
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL,
			   reset_bits,
			   ~reset_bits);
	mdelay(10);
}

static void mtk_clk_disable(struct mtk_eth *eth)
{
	int clk;

	for (clk = MTK_CLK_MAX - 1; clk >= 0; clk--)
		clk_disable_unprepare(eth->clks[clk]);
}

static int mtk_clk_enable(struct mtk_eth *eth)
{
	int clk, ret;

	for (clk = 0; clk < MTK_CLK_MAX ; clk++) {
		ret = clk_prepare_enable(eth->clks[clk]);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(eth->clks[clk]);

	return ret;
}

static void mtk_dim_rx(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct mtk_eth *eth = container_of(dim, struct mtk_eth, rx_dim);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct dim_cq_moder cur_profile;
	u32 val, cur;

	cur_profile = net_dim_get_rx_moderation(eth->rx_dim.mode,
						dim->profile_ix);
	spin_lock_bh(&eth->dim_lock);

	val = mtk_r32(eth, reg_map->pdma.delay_irq);
	val &= MTK_PDMA_DELAY_TX_MASK;
	val |= MTK_PDMA_DELAY_RX_EN;

	cur = min_t(u32, DIV_ROUND_UP(cur_profile.usec, 20), MTK_PDMA_DELAY_PTIME_MASK);
	val |= cur << MTK_PDMA_DELAY_RX_PTIME_SHIFT;

	cur = min_t(u32, cur_profile.pkts, MTK_PDMA_DELAY_PINT_MASK);
	val |= cur << MTK_PDMA_DELAY_RX_PINT_SHIFT;

	mtk_w32(eth, val, reg_map->pdma.delay_irq);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		mtk_w32(eth, val, reg_map->qdma.delay_irq);

	spin_unlock_bh(&eth->dim_lock);

	dim->state = DIM_START_MEASURE;
}

static void mtk_dim_tx(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct mtk_eth *eth = container_of(dim, struct mtk_eth, tx_dim);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct dim_cq_moder cur_profile;
	u32 val, cur;

	cur_profile = net_dim_get_tx_moderation(eth->tx_dim.mode,
						dim->profile_ix);
	spin_lock_bh(&eth->dim_lock);

	val = mtk_r32(eth, reg_map->pdma.delay_irq);
	val &= MTK_PDMA_DELAY_RX_MASK;
	val |= MTK_PDMA_DELAY_TX_EN;

	cur = min_t(u32, DIV_ROUND_UP(cur_profile.usec, 20), MTK_PDMA_DELAY_PTIME_MASK);
	val |= cur << MTK_PDMA_DELAY_TX_PTIME_SHIFT;

	cur = min_t(u32, cur_profile.pkts, MTK_PDMA_DELAY_PINT_MASK);
	val |= cur << MTK_PDMA_DELAY_TX_PINT_SHIFT;

	mtk_w32(eth, val, reg_map->pdma.delay_irq);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		mtk_w32(eth, val, reg_map->qdma.delay_irq);

	spin_unlock_bh(&eth->dim_lock);

	dim->state = DIM_START_MEASURE;
}

static void mtk_set_mcr_max_rx(struct mtk_mac *mac, u32 val)
{
	struct mtk_eth *eth = mac->hw;
	u32 mcr_cur, mcr_new;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		return;

	mcr_cur = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr_new = mcr_cur & ~MAC_MCR_MAX_RX_MASK;

	if (val <= 1518)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1518);
	else if (val <= 1536)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1536);
	else if (val <= 1552)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1552);
	else
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_2048);

	if (mcr_new != mcr_cur)
		mtk_w32(mac->hw, mcr_new, MTK_MAC_MCR(mac->id));
}

static void mtk_hw_reset(struct mtk_eth *eth)
{
	u32 val;

	if (mtk_is_netsys_v2_or_greater(eth))
		regmap_write(eth->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN, 0);

	if (mtk_is_netsys_v3_or_greater(eth)) {
		val = RSTCTRL_PPE0_V3;

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val |= RSTCTRL_PPE1_V3;

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			val |= RSTCTRL_PPE2;

		val |= RSTCTRL_WDMA0 | RSTCTRL_WDMA1 | RSTCTRL_WDMA2;
	} else if (mtk_is_netsys_v2_or_greater(eth)) {
		val = RSTCTRL_PPE0_V2;

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val |= RSTCTRL_PPE1;
	} else {
		val = RSTCTRL_PPE0;
	}

	ethsys_reset(eth, RSTCTRL_ETH | RSTCTRL_FE | val);

	if (mtk_is_netsys_v3_or_greater(eth))
		regmap_write(eth->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN,
			     0x6f8ff);
	else if (mtk_is_netsys_v2_or_greater(eth))
		regmap_write(eth->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN,
			     0x3ffffff);
}

static u32 mtk_hw_reset_read(struct mtk_eth *eth)
{
	u32 val;

	regmap_read(eth->ethsys, ETHSYS_RSTCTRL, &val);
	return val;
}

static void mtk_hw_warm_reset(struct mtk_eth *eth)
{
	u32 rst_mask, val;

	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, RSTCTRL_FE,
			   RSTCTRL_FE);
	if (readx_poll_timeout_atomic(mtk_hw_reset_read, eth, val,
				      val & RSTCTRL_FE, 1, 1000)) {
		dev_err(eth->dev, "warm reset failed\n");
		mtk_hw_reset(eth);
		return;
	}

	if (mtk_is_netsys_v3_or_greater(eth)) {
		rst_mask = RSTCTRL_ETH | RSTCTRL_PPE0_V3;
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			rst_mask |= RSTCTRL_PPE1_V3;
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			rst_mask |= RSTCTRL_PPE2;

		rst_mask |= RSTCTRL_WDMA0 | RSTCTRL_WDMA1 | RSTCTRL_WDMA2;
	} else if (mtk_is_netsys_v2_or_greater(eth)) {
		rst_mask = RSTCTRL_ETH | RSTCTRL_PPE0_V2;
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			rst_mask |= RSTCTRL_PPE1;
	} else {
		rst_mask = RSTCTRL_ETH | RSTCTRL_PPE0;
	}

	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, rst_mask, rst_mask);

	udelay(1);
	val = mtk_hw_reset_read(eth);
	if (!(val & rst_mask))
		dev_err(eth->dev, "warm reset stage0 failed %08x (%08x)\n",
			val, rst_mask);

	rst_mask |= RSTCTRL_FE;
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, rst_mask, ~rst_mask);

	udelay(1);
	val = mtk_hw_reset_read(eth);
	if (val & rst_mask)
		dev_err(eth->dev, "warm reset stage1 failed %08x (%08x)\n",
			val, rst_mask);
}

static bool mtk_hw_check_dma_hang(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	bool gmac1_tx, gmac2_tx, gdm1_tx, gdm2_tx;
	bool oq_hang, cdm1_busy, adma_busy;
	bool wtx_busy, cdm_full, oq_free;
	u32 wdidx, val, gdm1_fc, gdm2_fc;
	bool qfsm_hang, qfwd_hang;
	bool ret = false;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		return false;

	/* WDMA sanity checks */
	wdidx = mtk_r32(eth, reg_map->wdma_base[0] + 0xc);

	val = mtk_r32(eth, reg_map->wdma_base[0] + 0x204);
	wtx_busy = FIELD_GET(MTK_TX_DMA_BUSY, val);

	val = mtk_r32(eth, reg_map->wdma_base[0] + 0x230);
	cdm_full = !FIELD_GET(MTK_CDM_TXFIFO_RDY, val);

	oq_free  = (!(mtk_r32(eth, reg_map->pse_oq_sta) & GENMASK(24, 16)) &&
		    !(mtk_r32(eth, reg_map->pse_oq_sta + 0x4) & GENMASK(8, 0)) &&
		    !(mtk_r32(eth, reg_map->pse_oq_sta + 0x10) & GENMASK(24, 16)));

	if (wdidx == eth->reset.wdidx && wtx_busy && cdm_full && oq_free) {
		if (++eth->reset.wdma_hang_count > 2) {
			eth->reset.wdma_hang_count = 0;
			ret = true;
		}
		goto out;
	}

	/* QDMA sanity checks */
	qfsm_hang = !!mtk_r32(eth, reg_map->qdma.qtx_cfg + 0x234);
	qfwd_hang = !mtk_r32(eth, reg_map->qdma.qtx_cfg + 0x308);

	gdm1_tx = FIELD_GET(GENMASK(31, 16), mtk_r32(eth, MTK_FE_GDM1_FSM)) > 0;
	gdm2_tx = FIELD_GET(GENMASK(31, 16), mtk_r32(eth, MTK_FE_GDM2_FSM)) > 0;
	gmac1_tx = FIELD_GET(GENMASK(31, 24), mtk_r32(eth, MTK_MAC_FSM(0))) != 1;
	gmac2_tx = FIELD_GET(GENMASK(31, 24), mtk_r32(eth, MTK_MAC_FSM(1))) != 1;
	gdm1_fc = mtk_r32(eth, reg_map->gdm1_cnt + 0x24);
	gdm2_fc = mtk_r32(eth, reg_map->gdm1_cnt + 0x64);

	if (qfsm_hang && qfwd_hang &&
	    ((gdm1_tx && gmac1_tx && gdm1_fc < 1) ||
	     (gdm2_tx && gmac2_tx && gdm2_fc < 1))) {
		if (++eth->reset.qdma_hang_count > 2) {
			eth->reset.qdma_hang_count = 0;
			ret = true;
		}
		goto out;
	}

	/* ADMA sanity checks */
	oq_hang = !!(mtk_r32(eth, reg_map->pse_oq_sta) & GENMASK(8, 0));
	cdm1_busy = !!(mtk_r32(eth, MTK_FE_CDM1_FSM) & GENMASK(31, 16));
	adma_busy = !(mtk_r32(eth, reg_map->pdma.adma_rx_dbg0) & GENMASK(4, 0)) &&
		    !(mtk_r32(eth, reg_map->pdma.adma_rx_dbg0) & BIT(6));

	if (oq_hang && cdm1_busy && adma_busy) {
		if (++eth->reset.adma_hang_count > 2) {
			eth->reset.adma_hang_count = 0;
			ret = true;
		}
		goto out;
	}

	eth->reset.wdma_hang_count = 0;
	eth->reset.qdma_hang_count = 0;
	eth->reset.adma_hang_count = 0;
out:
	eth->reset.wdidx = wdidx;

	return ret;
}

static void mtk_hw_reset_monitor_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct mtk_eth *eth = container_of(del_work, struct mtk_eth,
					   reset.monitor_work);

	if (test_bit(MTK_RESETTING, &eth->state))
		goto out;

	/* DMA stuck checks */
	if (mtk_hw_check_dma_hang(eth))
		schedule_work(&eth->pending_work);

out:
	schedule_delayed_work(&eth->reset.monitor_work,
			      MTK_DMA_MONITOR_TIMEOUT);
}

static int mtk_hw_init(struct mtk_eth *eth, bool reset)
{
	u32 dma_mask = ETHSYS_DMA_AG_MAP_PDMA | ETHSYS_DMA_AG_MAP_QDMA |
		       ETHSYS_DMA_AG_MAP_PPE;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i, val, ret;

	if (!reset && test_and_set_bit(MTK_HW_INIT, &eth->state))
		return 0;

	if (!reset) {
		pm_runtime_enable(eth->dev);
		pm_runtime_get_sync(eth->dev);

		ret = mtk_clk_enable(eth);
		if (ret)
			goto err_disable_pm;
	}

	if (eth->ethsys)
		regmap_update_bits(eth->ethsys, ETHSYS_DMA_AG_MAP, dma_mask,
				   of_dma_is_coherent(eth->dma_dev->of_node) * dma_mask);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		ret = device_reset(eth->dev);
		if (ret) {
			dev_err(eth->dev, "MAC reset failed!\n");
			goto err_disable_pm;
		}

		/* set interrupt delays based on current Net DIM sample */
		mtk_dim_rx(&eth->rx_dim.work);
		mtk_dim_tx(&eth->tx_dim.work);

		/* disable delay and normal interrupt */
		mtk_tx_irq_disable(eth, ~0);
		mtk_rx_irq_disable(eth, ~0);

		return 0;
	}

	msleep(100);

	if (reset)
		mtk_hw_warm_reset(eth);
	else
		mtk_hw_reset(eth);

	/* No MT7628/88 support yet */
	if (reset && !MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		mtk_mdio_config(eth);

	if (mtk_is_netsys_v3_or_greater(eth)) {
		/* Set FE to PDMAv2 if necessary */
		val = mtk_r32(eth, MTK_FE_GLO_MISC);
		mtk_w32(eth,  val | BIT(4), MTK_FE_GLO_MISC);
	}

	if (eth->pctl) {
		/* Set GE2 driving and slew rate */
		regmap_write(eth->pctl, GPIO_DRV_SEL10, 0xa00);

		/* set GE2 TDSEL */
		regmap_write(eth->pctl, GPIO_OD33_CTRL8, 0x5);

		/* set GE2 TUNE */
		regmap_write(eth->pctl, GPIO_BIAS_CTRL, 0x0);
	}

	/* Set linkdown as the default for each GMAC. Its own MCR would be set
	 * up with the more appropriate value when mtk_mac_config call is being
	 * invoked.
	 */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		struct net_device *dev = eth->netdev[i];

		if (!dev)
			continue;

		mtk_w32(eth, MAC_MCR_FORCE_LINK_DOWN, MTK_MAC_MCR(i));
		mtk_set_mcr_max_rx(netdev_priv(dev),
				   dev->mtu + MTK_RX_ETH_HLEN);
	}

	/* Indicates CDM to parse the MTK special tag from CPU
	 * which also is working out for untag packets.
	 */
	val = mtk_r32(eth, MTK_CDMQ_IG_CTRL);
	mtk_w32(eth, val | MTK_CDMQ_STAG_EN, MTK_CDMQ_IG_CTRL);
	if (mtk_is_netsys_v1(eth)) {
		val = mtk_r32(eth, MTK_CDMP_IG_CTRL);
		mtk_w32(eth, val | MTK_CDMP_STAG_EN, MTK_CDMP_IG_CTRL);

		mtk_w32(eth, 1, MTK_CDMP_EG_CTRL);
	}

	/* set interrupt delays based on current Net DIM sample */
	mtk_dim_rx(&eth->rx_dim.work);
	mtk_dim_tx(&eth->tx_dim.work);

	/* disable delay and normal interrupt */
	mtk_tx_irq_disable(eth, ~0);
	mtk_rx_irq_disable(eth, ~0);

	/* FE int grouping */
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->pdma.int_grp);
	mtk_w32(eth, eth->soc->rx.irq_done_mask, reg_map->pdma.int_grp + 4);
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->qdma.int_grp);
	mtk_w32(eth, eth->soc->rx.irq_done_mask, reg_map->qdma.int_grp + 4);
	mtk_w32(eth, 0x21021000, MTK_FE_INT_GRP);

	if (mtk_is_netsys_v3_or_greater(eth)) {
		/* PSE dummy page mechanism */
		mtk_w32(eth, PSE_DUMMY_WORK_GDM(1) | PSE_DUMMY_WORK_GDM(2) |
			PSE_DUMMY_WORK_GDM(3) | DUMMY_PAGE_THR, PSE_DUMY_REQ);

		/* PSE free buffer drop threshold */
		mtk_w32(eth, 0x00600009, PSE_IQ_REV(8));

		/* PSE should not drop port8, port9 and port13 packets from
		 * WDMA Tx
		 */
		mtk_w32(eth, 0x00002300, PSE_DROP_CFG);

		/* PSE should drop packets to port8, port9 and port13 on WDMA Rx
		 * ring full
		 */
		mtk_w32(eth, 0x00002300, PSE_PPE_DROP(0));
		mtk_w32(eth, 0x00002300, PSE_PPE_DROP(1));
		mtk_w32(eth, 0x00002300, PSE_PPE_DROP(2));

		/* GDM and CDM Threshold */
		mtk_w32(eth, 0x08000707, MTK_CDMW0_THRES);
		mtk_w32(eth, 0x00000077, MTK_CDMW1_THRES);

		/* Disable GDM1 RX CRC stripping */
		mtk_m32(eth, MTK_GDMA_STRP_CRC, 0, MTK_GDMA_FWD_CFG(0));

		/* PSE GDM3 MIB counter has incorrect hw default values,
		 * so the driver ought to read clear the values beforehand
		 * in case ethtool retrieve wrong mib values.
		 */
		for (i = 0; i < 0x80; i += 0x4)
			mtk_r32(eth, reg_map->gdm1_cnt + 0x100 + i);
	} else if (!mtk_is_netsys_v1(eth)) {
		/* PSE should not drop port8 and port9 packets from WDMA Tx */
		mtk_w32(eth, 0x00000300, PSE_DROP_CFG);

		/* PSE should drop packets to port 8/9 on WDMA Rx ring full */
		mtk_w32(eth, 0x00000300, PSE_PPE_DROP(0));

		/* PSE Free Queue Flow Control  */
		mtk_w32(eth, 0x01fa01f4, PSE_FQFC_CFG2);

		/* PSE config input queue threshold */
		mtk_w32(eth, 0x001a000e, PSE_IQ_REV(1));
		mtk_w32(eth, 0x01ff001a, PSE_IQ_REV(2));
		mtk_w32(eth, 0x000e01ff, PSE_IQ_REV(3));
		mtk_w32(eth, 0x000e000e, PSE_IQ_REV(4));
		mtk_w32(eth, 0x000e000e, PSE_IQ_REV(5));
		mtk_w32(eth, 0x000e000e, PSE_IQ_REV(6));
		mtk_w32(eth, 0x000e000e, PSE_IQ_REV(7));
		mtk_w32(eth, 0x000e000e, PSE_IQ_REV(8));

		/* PSE config output queue threshold */
		mtk_w32(eth, 0x000f000a, PSE_OQ_TH(1));
		mtk_w32(eth, 0x001a000f, PSE_OQ_TH(2));
		mtk_w32(eth, 0x000f001a, PSE_OQ_TH(3));
		mtk_w32(eth, 0x01ff000f, PSE_OQ_TH(4));
		mtk_w32(eth, 0x000f000f, PSE_OQ_TH(5));
		mtk_w32(eth, 0x0006000f, PSE_OQ_TH(6));
		mtk_w32(eth, 0x00060006, PSE_OQ_TH(7));
		mtk_w32(eth, 0x00060006, PSE_OQ_TH(8));

		/* GDM and CDM Threshold */
		mtk_w32(eth, 0x00000004, MTK_GDM2_THRES);
		mtk_w32(eth, 0x00000004, MTK_CDMW0_THRES);
		mtk_w32(eth, 0x00000004, MTK_CDMW1_THRES);
		mtk_w32(eth, 0x00000004, MTK_CDME0_THRES);
		mtk_w32(eth, 0x00000004, MTK_CDME1_THRES);
		mtk_w32(eth, 0x00000004, MTK_CDMM_THRES);
	}

	return 0;

err_disable_pm:
	if (!reset) {
		pm_runtime_put_sync(eth->dev);
		pm_runtime_disable(eth->dev);
	}

	return ret;
}

static int mtk_hw_deinit(struct mtk_eth *eth)
{
	if (!test_and_clear_bit(MTK_HW_INIT, &eth->state))
		return 0;

	mtk_clk_disable(eth);

	pm_runtime_put_sync(eth->dev);
	pm_runtime_disable(eth->dev);

	return 0;
}

static void mtk_uninit(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	phylink_disconnect_phy(mac->phylink);
	mtk_tx_irq_disable(eth, ~0);
	mtk_rx_irq_disable(eth, ~0);
}

static int mtk_change_mtu(struct net_device *dev, int new_mtu)
{
	int length = new_mtu + MTK_RX_ETH_HLEN;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	if (rcu_access_pointer(eth->prog) &&
	    length > MTK_PP_MAX_BUF_SIZE) {
		netdev_err(dev, "Invalid MTU for XDP mode\n");
		return -EINVAL;
	}

	mtk_set_mcr_max_rx(mac, length);
	WRITE_ONCE(dev->mtu, new_mtu);

	return 0;
}

static int mtk_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return phylink_mii_ioctl(mac->phylink, ifr, cmd);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static void mtk_prepare_for_reset(struct mtk_eth *eth)
{
	u32 val;
	int i;

	/* set FE PPE ports link down */
	for (i = MTK_GMAC1_ID;
	     i <= (mtk_is_netsys_v3_or_greater(eth) ? MTK_GMAC3_ID : MTK_GMAC2_ID);
	     i += 2) {
		val = mtk_r32(eth, MTK_FE_GLO_CFG(i)) | MTK_FE_LINK_DOWN_P(PSE_PPE0_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val |= MTK_FE_LINK_DOWN_P(PSE_PPE1_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			val |= MTK_FE_LINK_DOWN_P(PSE_PPE2_PORT);
		mtk_w32(eth, val, MTK_FE_GLO_CFG(i));
	}

	/* adjust PPE configurations to prepare for reset */
	for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
		mtk_ppe_prepare_reset(eth->ppe[i]);

	/* disable NETSYS interrupts */
	mtk_w32(eth, 0, MTK_FE_INT_ENABLE);

	/* force link down GMAC */
	for (i = 0; i < 2; i++) {
		val = mtk_r32(eth, MTK_MAC_MCR(i)) & ~MAC_MCR_FORCE_LINK;
		mtk_w32(eth, val, MTK_MAC_MCR(i));
	}
}

static void mtk_pending_work(struct work_struct *work)
{
	struct mtk_eth *eth = container_of(work, struct mtk_eth, pending_work);
	unsigned long restart = 0;
	u32 val;
	int i;

	rtnl_lock();
	set_bit(MTK_RESETTING, &eth->state);

	mtk_prepare_for_reset(eth);
	mtk_wed_fe_reset();
	/* Run again reset preliminary configuration in order to avoid any
	 * possible race during FE reset since it can run releasing RTNL lock.
	 */
	mtk_prepare_for_reset(eth);

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i] || !netif_running(eth->netdev[i]))
			continue;

		mtk_stop(eth->netdev[i]);
		__set_bit(i, &restart);
	}

	usleep_range(15000, 16000);

	if (eth->dev->pins)
		pinctrl_select_state(eth->dev->pins->p,
				     eth->dev->pins->default_state);
	mtk_hw_init(eth, true);

	/* restart DMA and enable IRQs */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i] || !test_bit(i, &restart))
			continue;

		if (mtk_open(eth->netdev[i])) {
			netif_alert(eth, ifup, eth->netdev[i],
				    "Driver up/down cycle failed\n");
			dev_close(eth->netdev[i]);
		}
	}

	/* set FE PPE ports link up */
	for (i = MTK_GMAC1_ID;
	     i <= (mtk_is_netsys_v3_or_greater(eth) ? MTK_GMAC3_ID : MTK_GMAC2_ID);
	     i += 2) {
		val = mtk_r32(eth, MTK_FE_GLO_CFG(i)) & ~MTK_FE_LINK_DOWN_P(PSE_PPE0_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val &= ~MTK_FE_LINK_DOWN_P(PSE_PPE1_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			val &= ~MTK_FE_LINK_DOWN_P(PSE_PPE2_PORT);

		mtk_w32(eth, val, MTK_FE_GLO_CFG(i));
	}

	clear_bit(MTK_RESETTING, &eth->state);

	mtk_wed_fe_reset_complete();

	rtnl_unlock();
}

static int mtk_free_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		free_netdev(eth->netdev[i]);
	}

	for (i = 0; i < ARRAY_SIZE(eth->dsa_meta); i++) {
		if (!eth->dsa_meta[i])
			break;
		metadata_dst_free(eth->dsa_meta[i]);
	}

	return 0;
}

static int mtk_unreg_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		struct mtk_mac *mac;
		if (!eth->netdev[i])
			continue;
		mac = netdev_priv(eth->netdev[i]);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
			unregister_netdevice_notifier(&mac->device_notifier);
		unregister_netdev(eth->netdev[i]);
	}

	return 0;
}

static void mtk_sgmii_destroy(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++)
		mtk_pcs_lynxi_destroy(eth->sgmii_pcs[i]);
}

static int mtk_cleanup(struct mtk_eth *eth)
{
	mtk_sgmii_destroy(eth);
	mtk_unreg_dev(eth);
	mtk_free_dev(eth);
	cancel_work_sync(&eth->pending_work);
	cancel_delayed_work_sync(&eth->reset.monitor_work);

	return 0;
}

static int mtk_get_link_ksettings(struct net_device *ndev,
				  struct ethtool_link_ksettings *cmd)
{
	struct mtk_mac *mac = netdev_priv(ndev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	return phylink_ethtool_ksettings_get(mac->phylink, cmd);
}

static int mtk_set_link_ksettings(struct net_device *ndev,
				  const struct ethtool_link_ksettings *cmd)
{
	struct mtk_mac *mac = netdev_priv(ndev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	return phylink_ethtool_ksettings_set(mac->phylink, cmd);
}

static void mtk_get_drvinfo(struct net_device *dev,
			    struct ethtool_drvinfo *info)
{
	struct mtk_mac *mac = netdev_priv(dev);

	strscpy(info->driver, mac->hw->dev->driver->name, sizeof(info->driver));
	strscpy(info->bus_info, dev_name(mac->hw->dev), sizeof(info->bus_info));
	info->n_stats = ARRAY_SIZE(mtk_ethtool_stats);
}

static u32 mtk_get_msglevel(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return mac->hw->msg_enable;
}

static void mtk_set_msglevel(struct net_device *dev, u32 value)
{
	struct mtk_mac *mac = netdev_priv(dev);

	mac->hw->msg_enable = value;
}

static int mtk_nway_reset(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	if (!mac->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_nway_reset(mac->phylink);
}

static void mtk_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	int i;

	switch (stringset) {
	case ETH_SS_STATS: {
		struct mtk_mac *mac = netdev_priv(dev);

		for (i = 0; i < ARRAY_SIZE(mtk_ethtool_stats); i++)
			ethtool_puts(&data, mtk_ethtool_stats[i].str);
		if (mtk_page_pool_enabled(mac->hw))
			page_pool_ethtool_stats_get_strings(data);
		break;
	}
	default:
		break;
	}
}

static int mtk_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS: {
		int count = ARRAY_SIZE(mtk_ethtool_stats);
		struct mtk_mac *mac = netdev_priv(dev);

		if (mtk_page_pool_enabled(mac->hw))
			count += page_pool_ethtool_stats_get_count();
		return count;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static void mtk_ethtool_pp_stats(struct mtk_eth *eth, u64 *data)
{
	struct page_pool_stats stats = {};
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->rx_ring); i++) {
		struct mtk_rx_ring *ring = &eth->rx_ring[i];

		if (!ring->page_pool)
			continue;

		page_pool_get_stats(ring->page_pool, &stats);
	}
	page_pool_ethtool_stats_get(data, &stats);
}

static void mtk_get_ethtool_stats(struct net_device *dev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hwstats = mac->hw_stats;
	u64 *data_src, *data_dst;
	unsigned int start;
	int i;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock_bh(&hwstats->stats_lock)) {
			mtk_stats_update_mac(mac);
			spin_unlock_bh(&hwstats->stats_lock);
		}
	}

	data_src = (u64 *)hwstats;

	do {
		data_dst = data;
		start = u64_stats_fetch_begin(&hwstats->syncp);

		for (i = 0; i < ARRAY_SIZE(mtk_ethtool_stats); i++)
			*data_dst++ = *(data_src + mtk_ethtool_stats[i].offset);
		if (mtk_page_pool_enabled(mac->hw))
			mtk_ethtool_pp_stats(mac->hw, data_dst);
	} while (u64_stats_fetch_retry(&hwstats->syncp, start));
}

static int mtk_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd,
			 u32 *rule_locs)
{
	int ret = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		if (dev->hw_features & NETIF_F_LRO) {
			cmd->data = MTK_MAX_RX_RING_NUM;
			ret = 0;
		}
		break;
	case ETHTOOL_GRXCLSRLCNT:
		if (dev->hw_features & NETIF_F_LRO) {
			struct mtk_mac *mac = netdev_priv(dev);

			cmd->rule_cnt = mac->hwlro_ip_cnt;
			ret = 0;
		}
		break;
	case ETHTOOL_GRXCLSRULE:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_get_fdir_entry(dev, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_get_fdir_all(dev, cmd,
						     rule_locs);
		break;
	default:
		break;
	}

	return ret;
}

static int mtk_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	int ret = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_add_ipaddr(dev, cmd);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_del_ipaddr(dev, cmd);
		break;
	default:
		break;
	}

	return ret;
}

static void mtk_get_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct mtk_mac *mac = netdev_priv(dev);

	phylink_ethtool_get_pauseparam(mac->phylink, pause);
}

static int mtk_set_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return phylink_ethtool_set_pauseparam(mac->phylink, pause);
}

static int mtk_get_eee(struct net_device *dev, struct ethtool_keee *eee)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return phylink_ethtool_get_eee(mac->phylink, eee);
}

static int mtk_set_eee(struct net_device *dev, struct ethtool_keee *eee)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return phylink_ethtool_set_eee(mac->phylink, eee);
}

static u16 mtk_select_queue(struct net_device *dev, struct sk_buff *skb,
			    struct net_device *sb_dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	unsigned int queue = 0;

	if (netdev_uses_dsa(dev))
		queue = skb_get_queue_mapping(skb) + 3;
	else
		queue = mac->id;

	if (queue >= dev->num_tx_queues)
		queue = 0;

	return queue;
}

static const struct ethtool_ops mtk_ethtool_ops = {
	.get_link_ksettings	= mtk_get_link_ksettings,
	.set_link_ksettings	= mtk_set_link_ksettings,
	.get_drvinfo		= mtk_get_drvinfo,
	.get_msglevel		= mtk_get_msglevel,
	.set_msglevel		= mtk_set_msglevel,
	.nway_reset		= mtk_nway_reset,
	.get_link		= ethtool_op_get_link,
	.get_strings		= mtk_get_strings,
	.get_sset_count		= mtk_get_sset_count,
	.get_ethtool_stats	= mtk_get_ethtool_stats,
	.get_pauseparam		= mtk_get_pauseparam,
	.set_pauseparam		= mtk_set_pauseparam,
	.get_rxnfc		= mtk_get_rxnfc,
	.set_rxnfc		= mtk_set_rxnfc,
	.get_eee		= mtk_get_eee,
	.set_eee		= mtk_set_eee,
};

static const struct net_device_ops mtk_netdev_ops = {
	.ndo_uninit		= mtk_uninit,
	.ndo_open		= mtk_open,
	.ndo_stop		= mtk_stop,
	.ndo_start_xmit		= mtk_start_xmit,
	.ndo_set_mac_address	= mtk_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= mtk_do_ioctl,
	.ndo_change_mtu		= mtk_change_mtu,
	.ndo_tx_timeout		= mtk_tx_timeout,
	.ndo_get_stats64        = mtk_get_stats64,
	.ndo_fix_features	= mtk_fix_features,
	.ndo_set_features	= mtk_set_features,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= mtk_poll_controller,
#endif
	.ndo_setup_tc		= mtk_eth_setup_tc,
	.ndo_bpf		= mtk_xdp,
	.ndo_xdp_xmit		= mtk_xdp_xmit,
	.ndo_select_queue	= mtk_select_queue,
};

static int mtk_add_mac(struct mtk_eth *eth, struct device_node *np)
{
	const __be32 *_id = of_get_property(np, "reg", NULL);
	phy_interface_t phy_mode;
	struct phylink *phylink;
	struct mtk_mac *mac;
	int id, err;
	int txqs = 1;
	u32 val;

	if (!_id) {
		dev_err(eth->dev, "missing mac id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(_id);
	if (id >= MTK_MAX_DEVS) {
		dev_err(eth->dev, "%d is not a valid mac id\n", id);
		return -EINVAL;
	}

	if (eth->netdev[id]) {
		dev_err(eth->dev, "duplicate mac id found: %d\n", id);
		return -EINVAL;
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA))
		txqs = MTK_QDMA_NUM_QUEUES;

	eth->netdev[id] = alloc_etherdev_mqs(sizeof(*mac), txqs, 1);
	if (!eth->netdev[id]) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return -ENOMEM;
	}
	mac = netdev_priv(eth->netdev[id]);
	eth->mac[id] = mac;
	mac->id = id;
	mac->hw = eth;
	mac->of_node = np;

	err = of_get_ethdev_address(mac->of_node, eth->netdev[id]);
	if (err == -EPROBE_DEFER)
		return err;

	if (err) {
		/* If the mac address is invalid, use random mac address */
		eth_hw_addr_random(eth->netdev[id]);
		dev_err(eth->dev, "generated random MAC address %pM\n",
			eth->netdev[id]->dev_addr);
	}

	memset(mac->hwlro_ip, 0, sizeof(mac->hwlro_ip));
	mac->hwlro_ip_cnt = 0;

	mac->hw_stats = devm_kzalloc(eth->dev,
				     sizeof(*mac->hw_stats),
				     GFP_KERNEL);
	if (!mac->hw_stats) {
		dev_err(eth->dev, "failed to allocate counter memory\n");
		err = -ENOMEM;
		goto free_netdev;
	}
	spin_lock_init(&mac->hw_stats->stats_lock);
	u64_stats_init(&mac->hw_stats->syncp);

	if (mtk_is_netsys_v3_or_greater(eth))
		mac->hw_stats->reg_offset = id * 0x80;
	else
		mac->hw_stats->reg_offset = id * 0x40;

	/* phylink create */
	err = of_get_phy_mode(np, &phy_mode);
	if (err) {
		dev_err(eth->dev, "incorrect phy-mode\n");
		goto free_netdev;
	}

	/* mac config is not set */
	mac->interface = PHY_INTERFACE_MODE_NA;
	mac->speed = SPEED_UNKNOWN;

	mac->phylink_config.dev = &eth->netdev[id]->dev;
	mac->phylink_config.type = PHYLINK_NETDEV;
	mac->phylink_config.mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000 | MAC_2500FD;
	mac->phylink_config.lpi_capabilities = MAC_100FD | MAC_1000FD |
		MAC_2500FD;
	mac->phylink_config.lpi_timer_default = 1000;

	/* MT7623 gmac0 is now missing its speed-specific PLL configuration
	 * in its .mac_config method (since state->speed is not valid there.
	 * Disable support for MII, GMII and RGMII.
	 */
	if (!mac->hw->soc->disable_pll_modes || mac->id != 0) {
		__set_bit(PHY_INTERFACE_MODE_MII,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  mac->phylink_config.supported_interfaces);

		if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_RGMII))
			phy_interface_set_rgmii(mac->phylink_config.supported_interfaces);
	}

	if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_TRGMII) && !mac->id)
		__set_bit(PHY_INTERFACE_MODE_TRGMII,
			  mac->phylink_config.supported_interfaces);

	/* TRGMII is not permitted on MT7621 if using DDR2 */
	if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_GMAC1_TRGMII) &&
	    MTK_HAS_CAPS(mac->hw->soc->caps, MTK_TRGMII_MT7621_CLK)) {
		regmap_read(eth->ethsys, ETHSYS_SYSCFG, &val);
		if (val & SYSCFG_DRAM_TYPE_DDR2)
			__clear_bit(PHY_INTERFACE_MODE_TRGMII,
				    mac->phylink_config.supported_interfaces);
	}

	if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_SGMII)) {
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  mac->phylink_config.supported_interfaces);
	}

	if (mtk_is_netsys_v3_or_greater(mac->hw) &&
	    MTK_HAS_CAPS(mac->hw->soc->caps, MTK_ESW) &&
	    id == MTK_GMAC1_ID) {
		mac->phylink_config.mac_capabilities = MAC_ASYM_PAUSE |
						       MAC_SYM_PAUSE |
						       MAC_10000FD;
		phy_interface_zero(mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  mac->phylink_config.supported_interfaces);
	}

	phylink = phylink_create(&mac->phylink_config,
				 of_fwnode_handle(mac->of_node),
				 phy_mode, &mtk_phylink_ops);
	if (IS_ERR(phylink)) {
		err = PTR_ERR(phylink);
		goto free_netdev;
	}

	mac->phylink = phylink;

	if (MTK_HAS_CAPS(mac->hw->soc->caps, MTK_2P5GPHY) &&
	    id == MTK_GMAC2_ID)
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  mac->phylink_config.supported_interfaces);

	SET_NETDEV_DEV(eth->netdev[id], eth->dev);
	eth->netdev[id]->watchdog_timeo = 5 * HZ;
	eth->netdev[id]->netdev_ops = &mtk_netdev_ops;
	eth->netdev[id]->base_addr = (unsigned long)eth->base;

	eth->netdev[id]->hw_features = eth->soc->hw_features;
	if (eth->hwlro)
		eth->netdev[id]->hw_features |= NETIF_F_LRO;

	eth->netdev[id]->vlan_features = eth->soc->hw_features &
		~NETIF_F_HW_VLAN_CTAG_TX;
	eth->netdev[id]->features |= eth->soc->hw_features;
	eth->netdev[id]->ethtool_ops = &mtk_ethtool_ops;

	eth->netdev[id]->irq = eth->irq[MTK_FE_IRQ_SHARED];
	eth->netdev[id]->dev.of_node = np;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		eth->netdev[id]->max_mtu = MTK_MAX_RX_LENGTH - MTK_RX_ETH_HLEN;
	else
		eth->netdev[id]->max_mtu = MTK_MAX_RX_LENGTH_2K - MTK_RX_ETH_HLEN;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_QDMA)) {
		mac->device_notifier.notifier_call = mtk_device_event;
		register_netdevice_notifier(&mac->device_notifier);
	}

	if (mtk_page_pool_enabled(eth))
		eth->netdev[id]->xdp_features = NETDEV_XDP_ACT_BASIC |
						NETDEV_XDP_ACT_REDIRECT |
						NETDEV_XDP_ACT_NDO_XMIT |
						NETDEV_XDP_ACT_NDO_XMIT_SG;

	return 0;

free_netdev:
	free_netdev(eth->netdev[id]);
	return err;
}

void mtk_eth_set_dma_device(struct mtk_eth *eth, struct device *dma_dev)
{
	struct net_device *dev, *tmp;
	LIST_HEAD(dev_list);
	int i;

	rtnl_lock();

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		dev = eth->netdev[i];

		if (!dev || !(dev->flags & IFF_UP))
			continue;

		list_add_tail(&dev->close_list, &dev_list);
	}

	dev_close_many(&dev_list, false);

	eth->dma_dev = dma_dev;

	list_for_each_entry_safe(dev, tmp, &dev_list, close_list) {
		list_del_init(&dev->close_list);
		dev_open(dev, NULL);
	}

	rtnl_unlock();
}

static int mtk_sgmii_init(struct mtk_eth *eth)
{
	struct device_node *np;
	struct regmap *regmap;
	u32 flags;
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		np = of_parse_phandle(eth->dev->of_node, "mediatek,sgmiisys", i);
		if (!np)
			break;

		regmap = syscon_node_to_regmap(np);
		flags = 0;
		if (of_property_read_bool(np, "mediatek,pnswap"))
			flags |= MTK_SGMII_FLAG_PN_SWAP;

		of_node_put(np);

		if (IS_ERR(regmap))
			return PTR_ERR(regmap);

		eth->sgmii_pcs[i] = mtk_pcs_lynxi_create(eth->dev, regmap,
							 eth->soc->ana_rgc3,
							 flags);
	}

	return 0;
}

static int mtk_setup_legacy_sram(struct mtk_eth *eth, struct resource *res)
{
	dev_warn(eth->dev, "legacy DT: using hard-coded SRAM offset.\n");

	if (res->start + MTK_ETH_SRAM_OFFSET + MTK_ETH_NETSYS_V2_SRAM_SIZE - 1 >
	    res->end)
		return -EINVAL;

	eth->sram_pool = devm_gen_pool_create(eth->dev,
					      const_ilog2(MTK_ETH_SRAM_GRANULARITY),
					      NUMA_NO_NODE, dev_name(eth->dev));

	if (IS_ERR(eth->sram_pool))
		return PTR_ERR(eth->sram_pool);

	return gen_pool_add_virt(eth->sram_pool,
				 (unsigned long)eth->base + MTK_ETH_SRAM_OFFSET,
				 res->start + MTK_ETH_SRAM_OFFSET,
				 MTK_ETH_NETSYS_V2_SRAM_SIZE, NUMA_NO_NODE);
}

static int mtk_probe(struct platform_device *pdev)
{
	struct resource *res = NULL;
	struct device_node *mac_np;
	struct mtk_eth *eth;
	int err, i;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->soc = of_device_get_match_data(&pdev->dev);

	eth->dev = &pdev->dev;
	eth->dma_dev = &pdev->dev;
	eth->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628))
		eth->ip_align = NET_IP_ALIGN;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_36BIT_DMA)) {
		err = dma_set_mask(&pdev->dev, DMA_BIT_MASK(36));
		if (!err)
			err = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));

		if (err) {
			dev_err(&pdev->dev, "Wrong DMA config\n");
			return -EINVAL;
		}
	}

	spin_lock_init(&eth->page_lock);
	spin_lock_init(&eth->tx_irq_lock);
	spin_lock_init(&eth->rx_irq_lock);
	spin_lock_init(&eth->dim_lock);

	eth->rx_dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_EQE;
	INIT_WORK(&eth->rx_dim.work, mtk_dim_rx);
	INIT_DELAYED_WORK(&eth->reset.monitor_work, mtk_hw_reset_monitor_work);

	eth->tx_dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_EQE;
	INIT_WORK(&eth->tx_dim.work, mtk_dim_tx);

	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		eth->ethsys = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							      "mediatek,ethsys");
		if (IS_ERR(eth->ethsys)) {
			dev_err(&pdev->dev, "no ethsys regmap found\n");
			return PTR_ERR(eth->ethsys);
		}
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_INFRA)) {
		eth->infra = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							     "mediatek,infracfg");
		if (IS_ERR(eth->infra)) {
			dev_err(&pdev->dev, "no infracfg regmap found\n");
			return PTR_ERR(eth->infra);
		}
	}

	if (of_dma_is_coherent(pdev->dev.of_node)) {
		struct regmap *cci;

		cci = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "cci-control-port");
		/* enable CPU/bus coherency */
		if (!IS_ERR(cci))
			regmap_write(cci, 0, 3);
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SGMII)) {
		err = mtk_sgmii_init(eth);

		if (err)
			return err;
	}

	if (eth->soc->required_pctl) {
		eth->pctl = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							    "mediatek,pctl");
		if (IS_ERR(eth->pctl)) {
			dev_err(&pdev->dev, "no pctl regmap found\n");
			err = PTR_ERR(eth->pctl);
			goto err_destroy_sgmii;
		}
	}

	if (mtk_is_netsys_v2_or_greater(eth)) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res) {
			err = -EINVAL;
			goto err_destroy_sgmii;
		}

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_SRAM)) {
			eth->sram_pool = of_gen_pool_get(pdev->dev.of_node,
							 "sram", 0);
			if (!eth->sram_pool) {
				if (!mtk_is_netsys_v3_or_greater(eth)) {
					err = mtk_setup_legacy_sram(eth, res);
					if (err)
						goto err_destroy_sgmii;
				} else {
					dev_err(&pdev->dev,
						"Could not get SRAM pool\n");
					err = -EINVAL;
					goto err_destroy_sgmii;
				}
			}
		}
	}

	if (eth->soc->offload_version) {
		for (i = 0;; i++) {
			struct device_node *np;
			phys_addr_t wdma_phy;
			u32 wdma_base;

			if (i >= ARRAY_SIZE(eth->soc->reg_map->wdma_base))
				break;

			np = of_parse_phandle(pdev->dev.of_node,
					      "mediatek,wed", i);
			if (!np)
				break;

			wdma_base = eth->soc->reg_map->wdma_base[i];
			wdma_phy = res ? res->start + wdma_base : 0;
			mtk_wed_add_hw(np, eth, eth->base + wdma_base,
				       wdma_phy, i);
		}
	}

	err = mtk_get_irqs(pdev, eth);
	if (err)
		goto err_wed_exit;

	for (i = 0; i < ARRAY_SIZE(eth->clks); i++) {
		eth->clks[i] = devm_clk_get(eth->dev,
					    mtk_clks_source_name[i]);
		if (IS_ERR(eth->clks[i])) {
			if (PTR_ERR(eth->clks[i]) == -EPROBE_DEFER) {
				err = -EPROBE_DEFER;
				goto err_wed_exit;
			}
			if (eth->soc->required_clks & BIT(i)) {
				dev_err(&pdev->dev, "clock %s not found\n",
					mtk_clks_source_name[i]);
				err = -EINVAL;
				goto err_wed_exit;
			}
			eth->clks[i] = NULL;
		}
	}

	eth->msg_enable = netif_msg_init(mtk_msg_level, MTK_DEFAULT_MSG_ENABLE);
	INIT_WORK(&eth->pending_work, mtk_pending_work);

	err = mtk_hw_init(eth, false);
	if (err)
		goto err_wed_exit;

	eth->hwlro = MTK_HAS_CAPS(eth->soc->caps, MTK_HWLRO);

	for_each_child_of_node(pdev->dev.of_node, mac_np) {
		if (!of_device_is_compatible(mac_np,
					     "mediatek,eth-mac"))
			continue;

		if (!of_device_is_available(mac_np))
			continue;

		err = mtk_add_mac(eth, mac_np);
		if (err) {
			of_node_put(mac_np);
			goto err_deinit_hw;
		}
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SHARED_INT)) {
		err = devm_request_irq(eth->dev, eth->irq[MTK_FE_IRQ_SHARED],
				       mtk_handle_irq, 0,
				       dev_name(eth->dev), eth);
	} else {
		err = devm_request_irq(eth->dev, eth->irq[MTK_FE_IRQ_TX],
				       mtk_handle_irq_tx, 0,
				       dev_name(eth->dev), eth);
		if (err)
			goto err_free_dev;

		err = devm_request_irq(eth->dev, eth->irq[MTK_FE_IRQ_RX],
				       mtk_handle_irq_rx, 0,
				       dev_name(eth->dev), eth);
	}
	if (err)
		goto err_free_dev;

	/* No MT7628/88 support yet */
	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SOC_MT7628)) {
		err = mtk_mdio_init(eth);
		if (err)
			goto err_free_dev;
	}

	if (eth->soc->offload_version) {
		u8 ppe_num = eth->soc->ppe_num;

		ppe_num = min_t(u8, ARRAY_SIZE(eth->ppe), ppe_num);
		for (i = 0; i < ppe_num; i++) {
			u32 ppe_addr = eth->soc->reg_map->ppe_base;

			ppe_addr += (i == 2 ? 0xc00 : i * 0x400);
			eth->ppe[i] = mtk_ppe_init(eth, eth->base + ppe_addr, i);

			if (!eth->ppe[i]) {
				err = -ENOMEM;
				goto err_deinit_ppe;
			}
			err = mtk_eth_offload_init(eth, i);

			if (err)
				goto err_deinit_ppe;
		}
	}

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = register_netdev(eth->netdev[i]);
		if (err) {
			dev_err(eth->dev, "error bringing up device\n");
			goto err_deinit_ppe;
		} else
			netif_info(eth, probe, eth->netdev[i],
				   "mediatek frame engine at 0x%08lx, irq %d\n",
				   eth->netdev[i]->base_addr, eth->irq[MTK_FE_IRQ_SHARED]);
	}

	/* we run 2 devices on the same DMA ring so we need a dummy device
	 * for NAPI to work
	 */
	eth->dummy_dev = alloc_netdev_dummy(0);
	if (!eth->dummy_dev) {
		err = -ENOMEM;
		dev_err(eth->dev, "failed to allocated dummy device\n");
		goto err_unreg_netdev;
	}
	netif_napi_add(eth->dummy_dev, &eth->tx_napi, mtk_napi_tx);
	netif_napi_add(eth->dummy_dev, &eth->rx_napi, mtk_napi_rx);

	platform_set_drvdata(pdev, eth);
	schedule_delayed_work(&eth->reset.monitor_work,
			      MTK_DMA_MONITOR_TIMEOUT);

	return 0;

err_unreg_netdev:
	mtk_unreg_dev(eth);
err_deinit_ppe:
	mtk_ppe_deinit(eth);
	mtk_mdio_cleanup(eth);
err_free_dev:
	mtk_free_dev(eth);
err_deinit_hw:
	mtk_hw_deinit(eth);
err_wed_exit:
	mtk_wed_exit();
err_destroy_sgmii:
	mtk_sgmii_destroy(eth);

	return err;
}

static void mtk_remove(struct platform_device *pdev)
{
	struct mtk_eth *eth = platform_get_drvdata(pdev);
	struct mtk_mac *mac;
	int i;

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		mtk_stop(eth->netdev[i]);
		mac = netdev_priv(eth->netdev[i]);
		phylink_disconnect_phy(mac->phylink);
	}

	mtk_wed_exit();
	mtk_hw_deinit(eth);

	netif_napi_del(&eth->tx_napi);
	netif_napi_del(&eth->rx_napi);
	mtk_cleanup(eth);
	free_netdev(eth->dummy_dev);
	mtk_mdio_cleanup(eth);
}

static const struct mtk_soc_data mt2701_data = {
	.reg_map = &mtk_reg_map,
	.caps = MT7623_CAPS | MTK_HWLRO,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7623_CLKS_BITMAP,
	.required_pctl = true,
	.version = 1,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID,
		.dma_size = MTK_DMA_SIZE(2K),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
	},
};

static const struct mtk_soc_data mt7621_data = {
	.reg_map = &mtk_reg_map,
	.caps = MT7621_CAPS,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7621_CLKS_BITMAP,
	.required_pctl = false,
	.version = 1,
	.offload_version = 1,
	.ppe_num = 1,
	.hash_offset = 2,
	.foe_entry_size = MTK_FOE_ENTRY_V1_SIZE,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID,
		.dma_size = MTK_DMA_SIZE(2K),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
	},
};

static const struct mtk_soc_data mt7622_data = {
	.reg_map = &mtk_reg_map,
	.ana_rgc3 = 0x2028,
	.caps = MT7622_CAPS | MTK_HWLRO,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7622_CLKS_BITMAP,
	.required_pctl = false,
	.version = 1,
	.offload_version = 2,
	.ppe_num = 1,
	.hash_offset = 2,
	.has_accounting = true,
	.foe_entry_size = MTK_FOE_ENTRY_V1_SIZE,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID,
		.dma_size = MTK_DMA_SIZE(2K),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
	},
};

static const struct mtk_soc_data mt7623_data = {
	.reg_map = &mtk_reg_map,
	.caps = MT7623_CAPS | MTK_HWLRO,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7623_CLKS_BITMAP,
	.required_pctl = true,
	.version = 1,
	.offload_version = 1,
	.ppe_num = 1,
	.hash_offset = 2,
	.foe_entry_size = MTK_FOE_ENTRY_V1_SIZE,
	.disable_pll_modes = true,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID,
		.dma_size = MTK_DMA_SIZE(2K),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
	},
};

static const struct mtk_soc_data mt7629_data = {
	.reg_map = &mtk_reg_map,
	.ana_rgc3 = 0x128,
	.caps = MT7629_CAPS | MTK_HWLRO,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7629_CLKS_BITMAP,
	.required_pctl = false,
	.has_accounting = true,
	.version = 1,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID,
		.dma_size = MTK_DMA_SIZE(2K),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
	},
};

static const struct mtk_soc_data mt7981_data = {
	.reg_map = &mt7986_reg_map,
	.ana_rgc3 = 0x128,
	.caps = MT7981_CAPS,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7981_CLKS_BITMAP,
	.required_pctl = false,
	.version = 2,
	.offload_version = 2,
	.ppe_num = 2,
	.hash_offset = 4,
	.has_accounting = true,
	.foe_entry_size = MTK_FOE_ENTRY_V2_SIZE,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma_v2),
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID_V2,
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
	},
};

static const struct mtk_soc_data mt7986_data = {
	.reg_map = &mt7986_reg_map,
	.ana_rgc3 = 0x128,
	.caps = MT7986_CAPS,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7986_CLKS_BITMAP,
	.required_pctl = false,
	.version = 2,
	.offload_version = 2,
	.ppe_num = 2,
	.hash_offset = 4,
	.has_accounting = true,
	.foe_entry_size = MTK_FOE_ENTRY_V2_SIZE,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma_v2),
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID_V2,
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
	},
};

static const struct mtk_soc_data mt7988_data = {
	.reg_map = &mt7988_reg_map,
	.ana_rgc3 = 0x128,
	.caps = MT7988_CAPS,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7988_CLKS_BITMAP,
	.required_pctl = false,
	.version = 3,
	.offload_version = 2,
	.ppe_num = 3,
	.hash_offset = 4,
	.has_accounting = true,
	.foe_entry_size = MTK_FOE_ENTRY_V3_SIZE,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma_v2),
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(2K),
		.fq_dma_size = MTK_DMA_SIZE(4K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma_v2),
		.irq_done_mask = MTK_RX_DONE_INT_V2,
		.dma_l4_valid = RX_DMA_L4_VALID_V2,
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(2K),
	},
};

static const struct mtk_soc_data rt5350_data = {
	.reg_map = &mt7628_reg_map,
	.caps = MT7628_CAPS,
	.hw_features = MTK_HW_FEATURES_MT7628,
	.required_clks = MT7628_CLKS_BITMAP,
	.required_pctl = false,
	.version = 1,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma),
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma),
		.irq_done_mask = MTK_RX_DONE_INT,
		.dma_l4_valid = RX_DMA_L4_VALID_PDMA,
		.dma_max_len = MTK_TX_DMA_BUF_LEN,
		.dma_len_offset = 16,
		.dma_size = MTK_DMA_SIZE(2K),
	},
};

const struct of_device_id of_mtk_match[] = {
	{ .compatible = "mediatek,mt2701-eth", .data = &mt2701_data },
	{ .compatible = "mediatek,mt7621-eth", .data = &mt7621_data },
	{ .compatible = "mediatek,mt7622-eth", .data = &mt7622_data },
	{ .compatible = "mediatek,mt7623-eth", .data = &mt7623_data },
	{ .compatible = "mediatek,mt7629-eth", .data = &mt7629_data },
	{ .compatible = "mediatek,mt7981-eth", .data = &mt7981_data },
	{ .compatible = "mediatek,mt7986-eth", .data = &mt7986_data },
	{ .compatible = "mediatek,mt7988-eth", .data = &mt7988_data },
	{ .compatible = "ralink,rt5350-eth", .data = &rt5350_data },
	{},
};
MODULE_DEVICE_TABLE(of, of_mtk_match);

static struct platform_driver mtk_driver = {
	.probe = mtk_probe,
	.remove = mtk_remove,
	.driver = {
		.name = "mtk_soc_eth",
		.of_match_table = of_mtk_match,
	},
};

module_platform_driver(mtk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("Ethernet driver for MediaTek SoC");
