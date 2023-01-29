// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <dt-bindings/phy/phy.h>

#include "phy-qcom-qmp.h"

/* QPHY_SW_RESET bit */
#define SW_RESET				BIT(0)
/* QPHY_POWER_DOWN_CONTROL */
#define SW_PWRDN				BIT(0)
#define REFCLK_DRV_DSBL				BIT(1)
/* QPHY_START_CONTROL bits */
#define SERDES_START				BIT(0)
#define PCS_START				BIT(1)
#define PLL_READY_GATE_EN			BIT(3)
/* QPHY_PCS_STATUS bit */
#define PHYSTATUS				BIT(6)
#define PHYSTATUS_4_20				BIT(7)
/* QPHY_PCS_READY_STATUS & QPHY_COM_PCS_READY_STATUS bit */
#define PCS_READY				BIT(0)

/* QPHY_V3_DP_COM_RESET_OVRD_CTRL register bits */
/* DP PHY soft reset */
#define SW_DPPHY_RESET				BIT(0)
/* mux to select DP PHY reset control, 0:HW control, 1: software reset */
#define SW_DPPHY_RESET_MUX			BIT(1)
/* USB3 PHY soft reset */
#define SW_USB3PHY_RESET			BIT(2)
/* mux to select USB3 PHY reset control, 0:HW control, 1: software reset */
#define SW_USB3PHY_RESET_MUX			BIT(3)

/* QPHY_V3_DP_COM_PHY_MODE_CTRL register bits */
#define USB3_MODE				BIT(0) /* enables USB3 mode */
#define DP_MODE					BIT(1) /* enables DP mode */

/* QPHY_PCS_AUTONOMOUS_MODE_CTRL register bits */
#define ARCVR_DTCT_EN				BIT(0)
#define ALFPS_DTCT_EN				BIT(1)
#define ARCVR_DTCT_EVENT_SEL			BIT(4)

/* QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR register bits */
#define IRQ_CLEAR				BIT(0)

/* QPHY_PCS_LFPS_RXTERM_IRQ_STATUS register bits */
#define RCVR_DETECT				BIT(0)

/* QPHY_V3_PCS_MISC_CLAMP_ENABLE register bits */
#define CLAMP_EN				BIT(0) /* enables i/o clamp_n */

#define PHY_INIT_COMPLETE_TIMEOUT		10000
#define POWER_DOWN_DELAY_US_MIN			10
#define POWER_DOWN_DELAY_US_MAX			11

#define MAX_PROP_NAME				32

/* Define the assumed distance between lanes for underspecified device trees. */
#define QMP_PHY_LEGACY_LANE_STRIDE		0x400

struct qmp_phy_init_tbl {
	unsigned int offset;
	unsigned int val;
	/*
	 * register part of layout ?
	 * if yes, then offset gives index in the reg-layout
	 */
	bool in_layout;
	/*
	 * mask of lanes for which this register is written
	 * for cases when second lane needs different values
	 */
	u8 lane_mask;
};

#define QMP_PHY_INIT_CFG(o, v)		\
	{				\
		.offset = o,		\
		.val = v,		\
		.lane_mask = 0xff,	\
	}

#define QMP_PHY_INIT_CFG_L(o, v)	\
	{				\
		.offset = o,		\
		.val = v,		\
		.in_layout = true,	\
		.lane_mask = 0xff,	\
	}

#define QMP_PHY_INIT_CFG_LANE(o, v, l)	\
	{				\
		.offset = o,		\
		.val = v,		\
		.lane_mask = l,		\
	}

/* set of registers with offsets different per-PHY */
enum qphy_reg_layout {
	/* Common block control registers */
	QPHY_COM_SW_RESET,
	QPHY_COM_POWER_DOWN_CONTROL,
	QPHY_COM_START_CONTROL,
	QPHY_COM_PCS_READY_STATUS,
	/* PCS registers */
	QPHY_SW_RESET,
	QPHY_START_CTRL,
	QPHY_PCS_READY_STATUS,
	QPHY_PCS_STATUS,
	QPHY_PCS_AUTONOMOUS_MODE_CTRL,
	QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR,
	QPHY_PCS_LFPS_RXTERM_IRQ_STATUS,
	QPHY_PCS_POWER_DOWN_CONTROL,
	/* PCS_MISC registers */
	QPHY_PCS_MISC_TYPEC_CTRL,
	/* Keep last to ensure regs_layout arrays are properly initialized */
	QPHY_LAYOUT_SIZE
};

static const unsigned int qmp_v3_usb3phy_regs_layout[QPHY_LAYOUT_SIZE] = {
	[QPHY_SW_RESET]			= 0x00,
	[QPHY_START_CTRL]		= 0x08,
	[QPHY_PCS_STATUS]		= 0x174,
	[QPHY_PCS_AUTONOMOUS_MODE_CTRL]	= 0x0d8,
	[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR]  = 0x0dc,
	[QPHY_PCS_LFPS_RXTERM_IRQ_STATUS] = 0x170,
};

static const unsigned int qmp_v4_usb3phy_regs_layout[QPHY_LAYOUT_SIZE] = {
	[QPHY_SW_RESET]			= 0x00,
	[QPHY_START_CTRL]		= 0x44,
	[QPHY_PCS_STATUS]		= 0x14,
	[QPHY_PCS_POWER_DOWN_CONTROL]	= 0x40,

	/* In PCS_USB */
	[QPHY_PCS_AUTONOMOUS_MODE_CTRL]	= 0x008,
	[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR] = 0x014,
};

static const struct qmp_phy_init_tbl qmp_v3_usb3_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_IVCO, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYSCLK_EN_SEL, 0x14),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_BIAS_EN_CLKBUFLR_EN, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CLK_SELECT, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYS_CLK_CTRL, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_RESETSM_CNTRL2, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CMN_CONFIG, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SVS_MODE_CLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_HSCLK_SEL, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DEC_START_MODE0, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START1_MODE0, 0xab),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START2_MODE0, 0xea),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START3_MODE0, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CP_CTRL_MODE0, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_CCTRL_MODE0, 0x36),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_INTEGLOOP_GAIN1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_INTEGLOOP_GAIN0_MODE0, 0x3f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_VCO_TUNE2_MODE0, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_VCO_TUNE1_MODE0, 0xc9),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CORECLK_DIV_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP3_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP2_MODE0, 0x34),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP1_MODE0, 0x15),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_EN, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CORE_CLK_EN, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_CFG, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_VCO_TUNE_MAP, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYSCLK_BUF_ENABLE, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_EN_CENTER, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_PER1, 0x31),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_PER2, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_ADJ_PER1, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_ADJ_PER2, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_STEP_SIZE1, 0x85),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SSC_STEP_SIZE2, 0x07),
};

static const struct qmp_phy_init_tbl qmp_v3_usb3_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_HIGHZ_DRVR_EN, 0x10),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RCV_DETECT_LVL_2, 0x12),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_LANE_MODE_1, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RES_CODE_LANE_OFFSET_RX, 0x09),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RES_CODE_LANE_OFFSET_TX, 0x06),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SVS_MODE_CLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYSCLK_EN_SEL, 0x37),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYS_CLK_CTRL, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CLK_ENABLE1, 0x0e),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_SYSCLK_BUF_ENABLE, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CLK_SELECT, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CMN_CONFIG, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_INTEGLOOP_GAIN0_MODE0, 0x3f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_INTEGLOOP_GAIN1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_VCO_TUNE_MAP, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP3_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_BG_TIMER, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CORECLK_DIV_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_VCO_TUNE_CTRL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_BIAS_EN_CLKBUFLR_EN, 0x3f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CORE_CLK_EN, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_IVCO, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_CCTRL_MODE0, 0x36),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_CP_CTRL_MODE0, 0x06),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_serdes_tbl_rbr[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_HSCLK_SEL, 0x0c),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP1_MODE0, 0x6f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP2_MODE0, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_EN, 0x00),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_serdes_tbl_hbr[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_HSCLK_SEL, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP1_MODE0, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP2_MODE0, 0x0e),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_EN, 0x00),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_serdes_tbl_hbr2[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_HSCLK_SEL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DEC_START_MODE0, 0x8c),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START2_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START3_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP1_MODE0, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP2_MODE0, 0x1c),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_EN, 0x00),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_serdes_tbl_hbr3[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_HSCLK_SEL, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP1_MODE0, 0x2f),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP2_MODE0, 0x2a),
	QMP_PHY_INIT_CFG(QSERDES_V3_COM_LOCK_CMP_EN, 0x08),
};

static const struct qmp_phy_init_tbl qmp_v3_dp_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TRANSCEIVER_BIAS_EN, 0x1a),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_VMODE_CTRL1, 0x40),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_PRE_STALL_LDO_BOOST_EN, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_INTERFACE_SELECT, 0x3d),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_CLKBUF_ENABLE, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RESET_TSYNC_EN, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TRAN_DRVR_EMP_EN, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_PARRATE_REC_DETECT_IDLE_EN, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TX_INTERFACE_MODE, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TX_BAND, 0x4),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TX_POL_INV, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TX_DRV_LVL, 0x38),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_TX_EMP_POST1_LVL, 0x20),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RES_CODE_LANE_OFFSET_TX, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V3_TX_RES_CODE_LANE_OFFSET_RX, 0x07),
};

static const struct qmp_phy_init_tbl qmp_v3_usb3_rx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_UCDR_FASTLOCK_FO_GAIN, 0x0b),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL2, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL3, 0x4e),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL4, 0x18),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1, 0x77),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_RX_OFFSET_ADAPTOR_CNTRL2, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_SIGDET_CNTRL, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_SIGDET_DEGLITCH_CNTRL, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V3_RX_UCDR_SO_SATURATION_AND_ENABLE, 0x75),
};

static const struct qmp_phy_init_tbl qmp_v3_usb3_pcs_tbl[] = {
	/* FLL settings */
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_FLL_CNTRL2, 0x83),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_FLL_CNT_VAL_L, 0x09),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_FLL_CNT_VAL_H_TOL, 0xa2),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_FLL_MAN_CODE, 0x40),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_FLL_CNTRL1, 0x02),

	/* Lock Det settings */
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_LOCK_DETECT_CONFIG1, 0xd1),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_LOCK_DETECT_CONFIG2, 0x1f),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_LOCK_DETECT_CONFIG3, 0x47),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_POWER_STATE_CONFIG2, 0x1b),

	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RX_SIGDET_LVL, 0xba),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_V0, 0x9f),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_V1, 0x9f),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_V2, 0xb7),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_V3, 0x4e),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_V4, 0x65),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXMGN_LS, 0x6b),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_V0, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_V0, 0x0d),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_V1, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_V1, 0x0d),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_V2, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_V2, 0x0d),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_V3, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_V3, 0x1d),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_V4, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_V4, 0x0d),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M6DB_LS, 0x15),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TXDEEMPH_M3P5DB_LS, 0x0d),

	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RATE_SLEW_CNTRL, 0x02),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_PWRUP_RESET_DLY_TIME_AUXCLK, 0x04),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_TSYNC_RSYNC_TIME, 0x44),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_PWRUP_RESET_DLY_TIME_AUXCLK, 0x04),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RCVR_DTCT_DLY_P1U2_L, 0xe7),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RCVR_DTCT_DLY_P1U2_H, 0x03),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RCVR_DTCT_DLY_U3_L, 0x40),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RCVR_DTCT_DLY_U3_H, 0x00),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RXEQTRAINING_WAIT_TIME, 0x75),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_LFPS_TX_ECSTART_EQTLOCK, 0x86),
	QMP_PHY_INIT_CFG(QPHY_V3_PCS_RXEQTRAINING_RUN_TIME, 0x13),
};

static const struct qmp_phy_init_tbl sm8150_usb3_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_EN_CENTER, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_PER1, 0x31),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_PER2, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_STEP_SIZE1_MODE0, 0xde),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_STEP_SIZE2_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_STEP_SIZE1_MODE1, 0xde),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SSC_STEP_SIZE2_MODE1, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SYSCLK_BUF_ENABLE, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CMN_IPTRIM, 0x20),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CP_CTRL_MODE0, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CP_CTRL_MODE1, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_RCTRL_MODE1, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_CCTRL_MODE0, 0x36),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_CCTRL_MODE1, 0x36),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SYSCLK_EN_SEL, 0x1a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP_EN, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE0, 0x14),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE0, 0x34),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE1, 0x34),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE1, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE0, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE1, 0x82),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START1_MODE0, 0xab),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE0, 0xea),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE0, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE_MAP, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START1_MODE1, 0xab),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE1, 0xea),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE1, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE1_MODE0, 0x24),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE1_MODE1, 0x24),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE2_MODE1, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_HSCLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CORECLK_DIV_MODE1, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE1_MODE0, 0xca),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE2_MODE0, 0x1e),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE1_MODE1, 0xca),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIN_VCOCAL_CMP_CODE2_MODE1, 0x1e),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIN_VCOCAL_HSCLK_SEL, 0x11),
};

static const struct qmp_phy_init_tbl sm8150_usb3_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_TX, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_RX, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_LANE_MODE_1, 0xd5),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RCV_DETECT_LVL_2, 0x12),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_PI_QEC_CTRL, 0x20),
};

static const struct qmp_phy_init_tbl sm8150_usb3_rx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SO_GAIN, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_FO_GAIN, 0x2f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SO_SATURATION_AND_ENABLE, 0x7f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_COUNT_LOW, 0xff),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_COUNT_HIGH, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_PI_CONTROLS, 0x99),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_THRESH1, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_THRESH2, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_GAIN1, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_GAIN2, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VGA_CAL_CNTRL1, 0x54),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VGA_CAL_CNTRL2, 0x0e),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL2, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL3, 0x4a),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL4, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_IDAC_TSETTLE_LOW, 0xc0),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_IDAC_TSETTLE_HIGH, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1, 0x77),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_SIGDET_CNTRL, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_SIGDET_DEGLITCH_CNTRL, 0x0e),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_LOW, 0xbf),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH, 0xbf),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH2, 0x3f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH3, 0x7f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH4, 0x94),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_LOW, 0xdc),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH, 0xdc),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH2, 0x5c),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH3, 0x0b),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH4, 0xb3),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DFE_EN_TIMER, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DFE_CTLE_POST_CAL_OFFSET, 0x38),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_AUX_DATA_TCOARSE_TFINE, 0xa0),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DCC_CTRL1, 0x0c),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_GM_CAL, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VTH_CODE, 0x10),
};

static const struct qmp_phy_init_tbl sm8150_usb3_pcs_tbl[] = {
	/* Lock Det settings */
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG1, 0xd0),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG2, 0x07),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG6, 0x13),

	QMP_PHY_INIT_CFG(QPHY_V4_PCS_REFGEN_REQ_CONFIG1, 0x21),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_RX_SIGDET_LVL, 0xaa),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_CDR_RESET_TIME, 0x0a),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_ALIGN_DETECT_CONFIG1, 0x88),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_ALIGN_DETECT_CONFIG2, 0x13),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_PCS_TX_RX_CONFIG, 0x0c),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_EQ_CONFIG1, 0x4b),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_EQ_CONFIG5, 0x10),
};

static const struct qmp_phy_init_tbl sm8150_usb3_pcs_usb_tbl[] = {
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_USB3_LFPS_DET_HIGH_COUNT_VAL, 0xf8),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_USB3_RXEQTRAINING_DFE_TIME_S2, 0x07),
};

static const struct qmp_phy_init_tbl sm8250_usb3_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_TX, 0x60),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_RX, 0x60),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_OFFSET_TX, 0x11),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_OFFSET_RX, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_LANE_MODE_1, 0xd5),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RCV_DETECT_LVL_2, 0x12),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_TX_PI_QEC_CTRL, 0x40, 1),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_TX_PI_QEC_CTRL, 0x54, 2),
};

static const struct qmp_phy_init_tbl sm8250_usb3_rx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SO_GAIN, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_FO_GAIN, 0x2f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SO_SATURATION_AND_ENABLE, 0x7f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_COUNT_LOW, 0xff),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_FASTLOCK_COUNT_HIGH, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_PI_CONTROLS, 0x99),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_THRESH1, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_THRESH2, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_GAIN1, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_UCDR_SB2_GAIN2, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VGA_CAL_CNTRL1, 0x54),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VGA_CAL_CNTRL2, 0x0c),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL2, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL3, 0x4a),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQU_ADAPTOR_CNTRL4, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_IDAC_TSETTLE_LOW, 0xc0),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_IDAC_TSETTLE_HIGH, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1, 0x77),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_SIGDET_CNTRL, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_SIGDET_DEGLITCH_CNTRL, 0x0e),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_RX_RX_MODE_00_LOW, 0xff, 1),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_RX_RX_MODE_00_LOW, 0x7f, 2),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_RX_RX_MODE_00_HIGH, 0x7f, 1),
	QMP_PHY_INIT_CFG_LANE(QSERDES_V4_RX_RX_MODE_00_HIGH, 0xff, 2),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH2, 0x7f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH3, 0x7f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_00_HIGH4, 0x97),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_LOW, 0xdc),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH, 0xdc),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH2, 0x5c),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH3, 0x7b),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_RX_MODE_01_HIGH4, 0xb4),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DFE_EN_TIMER, 0x04),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DFE_CTLE_POST_CAL_OFFSET, 0x38),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_AUX_DATA_TCOARSE_TFINE, 0xa0),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_DCC_CTRL1, 0x0c),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_GM_CAL, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_V4_RX_VTH_CODE, 0x10),
};

static const struct qmp_phy_init_tbl sm8250_usb3_pcs_tbl[] = {
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG1, 0xd0),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG2, 0x07),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG3, 0x20),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_LOCK_DETECT_CONFIG6, 0x13),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_REFGEN_REQ_CONFIG1, 0x21),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_RX_SIGDET_LVL, 0xa9),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_CDR_RESET_TIME, 0x0a),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_ALIGN_DETECT_CONFIG1, 0x88),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_ALIGN_DETECT_CONFIG2, 0x13),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_PCS_TX_RX_CONFIG, 0x0c),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_EQ_CONFIG1, 0x4b),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_EQ_CONFIG5, 0x10),
};

static const struct qmp_phy_init_tbl sm8250_usb3_pcs_usb_tbl[] = {
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_USB3_LFPS_DET_HIGH_COUNT_VAL, 0xf8),
	QMP_PHY_INIT_CFG(QPHY_V4_PCS_USB3_RXEQTRAINING_DFE_TIME_S2, 0x07),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_serdes_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SVS_MODE_CLK_SEL, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SYSCLK_EN_SEL, 0x3b),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SYS_CLK_CTRL, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CLK_ENABLE1, 0x0c),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_SYSCLK_BUF_ENABLE, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CLK_SELECT, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_IVCO, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_CCTRL_MODE0, 0x36),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_PLL_RCTRL_MODE0, 0x16),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CP_CTRL_MODE0, 0x06),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CMN_CONFIG, 0x02),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_INTEGLOOP_GAIN0_MODE0, 0x3f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_INTEGLOOP_GAIN1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE_MAP, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START1_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BG_TIMER, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CORECLK_DIV_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_VCO_TUNE_CTRL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_BIAS_EN_CLKBUFLR_EN, 0x17),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_CORE_CLK_EN, 0x1f),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_serdes_tbl_rbr[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_HSCLK_SEL, 0x05),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE0, 0x6f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE0, 0x08),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP_EN, 0x04),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_serdes_tbl_hbr[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_HSCLK_SEL, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE0, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE0, 0x0e),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP_EN, 0x08),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_serdes_tbl_hbr2[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_HSCLK_SEL, 0x01),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE0, 0x8c),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE0, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE0, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE0, 0x1f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE0, 0x1c),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP_EN, 0x08),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_serdes_tbl_hbr3[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_HSCLK_SEL, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DEC_START_MODE0, 0x69),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START2_MODE0, 0x80),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_DIV_FRAC_START3_MODE0, 0x07),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP1_MODE0, 0x2f),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP2_MODE0, 0x2a),
	QMP_PHY_INIT_CFG(QSERDES_V4_COM_LOCK_CMP_EN, 0x08),
};

static const struct qmp_phy_init_tbl qmp_v4_dp_tx_tbl[] = {
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_VMODE_CTRL1, 0x40),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_PRE_STALL_LDO_BOOST_EN, 0x30),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_INTERFACE_SELECT, 0x3b),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_CLKBUF_ENABLE, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RESET_TSYNC_EN, 0x03),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TRAN_DRVR_EMP_EN, 0x0f),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_PARRATE_REC_DETECT_IDLE_EN, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TX_INTERFACE_MODE, 0x00),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_OFFSET_TX, 0x11),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_RES_CODE_LANE_OFFSET_RX, 0x11),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TX_BAND, 0x4),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TX_POL_INV, 0x0a),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TX_DRV_LVL, 0x2a),
	QMP_PHY_INIT_CFG(QSERDES_V4_TX_TX_EMP_POST1_LVL, 0x20),
};


/* list of regulators */
struct qmp_regulator_data {
	const char *name;
	unsigned int enable_load;
};

static struct qmp_regulator_data qmp_phy_vreg_l[] = {
	{ .name = "vdda-phy", .enable_load = 21800 },
	{ .name = "vdda-pll", .enable_load = 36000 },
};

struct qmp_phy;

/* struct qmp_phy_cfg - per-PHY initialization config */
struct qmp_phy_cfg {
	/* phy-type - PCIE/UFS/USB */
	unsigned int type;
	/* number of lanes provided by phy */
	int nlanes;

	/* Init sequence for PHY blocks - serdes, tx, rx, pcs */
	const struct qmp_phy_init_tbl *serdes_tbl;
	int serdes_tbl_num;
	const struct qmp_phy_init_tbl *tx_tbl;
	int tx_tbl_num;
	const struct qmp_phy_init_tbl *rx_tbl;
	int rx_tbl_num;
	const struct qmp_phy_init_tbl *pcs_tbl;
	int pcs_tbl_num;
	const struct qmp_phy_init_tbl *pcs_usb_tbl;
	int pcs_usb_tbl_num;

	/* Init sequence for DP PHY block link rates */
	const struct qmp_phy_init_tbl *serdes_tbl_rbr;
	int serdes_tbl_rbr_num;
	const struct qmp_phy_init_tbl *serdes_tbl_hbr;
	int serdes_tbl_hbr_num;
	const struct qmp_phy_init_tbl *serdes_tbl_hbr2;
	int serdes_tbl_hbr2_num;
	const struct qmp_phy_init_tbl *serdes_tbl_hbr3;
	int serdes_tbl_hbr3_num;

	/* DP PHY callbacks */
	int (*configure_dp_phy)(struct qmp_phy *qphy);
	void (*configure_dp_tx)(struct qmp_phy *qphy);
	int (*calibrate_dp_phy)(struct qmp_phy *qphy);
	void (*dp_aux_init)(struct qmp_phy *qphy);

	/* clock ids to be requested */
	const char * const *clk_list;
	int num_clks;
	/* resets to be requested */
	const char * const *reset_list;
	int num_resets;
	/* regulators to be requested */
	const struct qmp_regulator_data *vreg_list;
	int num_vregs;

	/* array of registers with different offsets */
	const unsigned int *regs;

	unsigned int start_ctrl;
	unsigned int pwrdn_ctrl;
	/* bit offset of PHYSTATUS in QPHY_PCS_STATUS register */
	unsigned int phy_status;

	/* true, if PHY needs delay after POWER_DOWN */
	bool has_pwrdn_delay;
	/* power_down delay in usec */
	int pwrdn_delay_min;
	int pwrdn_delay_max;

	/* true, if PHY has a separate DP_COM control block */
	bool has_phy_dp_com_ctrl;
	/* true, if PHY has secondary tx/rx lanes to be configured */
	bool is_dual_lane_phy;

	/* Offset from PCS to PCS_USB region */
	unsigned int pcs_usb_offset;

};

struct qmp_phy_combo_cfg {
	const struct qmp_phy_cfg *usb_cfg;
	const struct qmp_phy_cfg *dp_cfg;
};

/**
 * struct qmp_phy - per-lane phy descriptor
 *
 * @phy: generic phy
 * @cfg: phy specific configuration
 * @serdes: iomapped memory space for phy's serdes (i.e. PLL)
 * @tx: iomapped memory space for lane's tx
 * @rx: iomapped memory space for lane's rx
 * @pcs: iomapped memory space for lane's pcs
 * @tx2: iomapped memory space for second lane's tx (in dual lane PHYs)
 * @rx2: iomapped memory space for second lane's rx (in dual lane PHYs)
 * @pcs_misc: iomapped memory space for lane's pcs_misc
 * @pcs_usb: iomapped memory space for lane's pcs_usb
 * @pipe_clk: pipe clock
 * @index: lane index
 * @qmp: QMP phy to which this lane belongs
 * @lane_rst: lane's reset controller
 * @mode: current PHY mode
 * @dp_aux_cfg: Display port aux config
 * @dp_opts: Display port optional config
 * @dp_clks: Display port clocks
 */
struct qmp_phy {
	struct phy *phy;
	const struct qmp_phy_cfg *cfg;
	void __iomem *serdes;
	void __iomem *tx;
	void __iomem *rx;
	void __iomem *pcs;
	void __iomem *tx2;
	void __iomem *rx2;
	void __iomem *pcs_misc;
	void __iomem *pcs_usb;
	struct clk *pipe_clk;
	unsigned int index;
	struct qcom_qmp *qmp;
	struct reset_control *lane_rst;
	enum phy_mode mode;
	unsigned int dp_aux_cfg;
	struct phy_configure_opts_dp dp_opts;
	struct qmp_phy_dp_clks *dp_clks;
};

struct qmp_phy_dp_clks {
	struct qmp_phy *qphy;
	struct clk_hw dp_link_hw;
	struct clk_hw dp_pixel_hw;
};

/**
 * struct qcom_qmp - structure holding QMP phy block attributes
 *
 * @dev: device
 * @dp_com: iomapped memory space for phy's dp_com control block
 *
 * @clks: array of clocks required by phy
 * @resets: array of resets required by phy
 * @vregs: regulator supplies bulk data
 *
 * @phys: array of per-lane phy descriptors
 * @phy_mutex: mutex lock for PHY common block initialization
 * @init_count: phy common block initialization count
 * @ufs_reset: optional UFS PHY reset handle
 */
struct qcom_qmp {
	struct device *dev;
	void __iomem *dp_com;

	struct clk_bulk_data *clks;
	struct reset_control_bulk_data *resets;
	struct regulator_bulk_data *vregs;

	struct qmp_phy **phys;
	struct qmp_phy *usb_phy;

	struct mutex phy_mutex;
	int init_count;

	struct reset_control *ufs_reset;
};

static void qcom_qmp_v3_phy_dp_aux_init(struct qmp_phy *qphy);
static void qcom_qmp_v3_phy_configure_dp_tx(struct qmp_phy *qphy);
static int qcom_qmp_v3_phy_configure_dp_phy(struct qmp_phy *qphy);
static int qcom_qmp_v3_dp_phy_calibrate(struct qmp_phy *qphy);

static void qcom_qmp_v4_phy_dp_aux_init(struct qmp_phy *qphy);
static void qcom_qmp_v4_phy_configure_dp_tx(struct qmp_phy *qphy);
static int qcom_qmp_v4_phy_configure_dp_phy(struct qmp_phy *qphy);
static int qcom_qmp_v4_dp_phy_calibrate(struct qmp_phy *qphy);

static inline void qphy_setbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl(base + offset);
	reg |= val;
	writel(reg, base + offset);

	/* ensure that above write is through */
	readl(base + offset);
}

static inline void qphy_clrbits(void __iomem *base, u32 offset, u32 val)
{
	u32 reg;

	reg = readl(base + offset);
	reg &= ~val;
	writel(reg, base + offset);

	/* ensure that above write is through */
	readl(base + offset);
}

/* list of clocks required by phy */
static const char * const qmp_v3_phy_clk_l[] = {
	"aux", "cfg_ahb", "ref", "com_aux",
};

static const char * const qmp_v4_phy_clk_l[] = {
	"aux", "ref_clk_src", "ref", "com_aux",
};

/* the primary usb3 phy on sm8250 doesn't have a ref clock */
static const char * const qmp_v4_sm8250_usbphy_clk_l[] = {
	"aux", "ref_clk_src", "com_aux"
};

/* list of resets */
static const char * const msm8996_usb3phy_reset_l[] = {
	"phy", "common",
};

static const char * const sc7180_usb3phy_reset_l[] = {
	"phy",
};

static const struct qmp_phy_cfg sc7180_usb3phy_cfg = {
	.type			= PHY_TYPE_USB3,
	.nlanes			= 1,

	.serdes_tbl		= qmp_v3_usb3_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(qmp_v3_usb3_serdes_tbl),
	.tx_tbl			= qmp_v3_usb3_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(qmp_v3_usb3_tx_tbl),
	.rx_tbl			= qmp_v3_usb3_rx_tbl,
	.rx_tbl_num		= ARRAY_SIZE(qmp_v3_usb3_rx_tbl),
	.pcs_tbl		= qmp_v3_usb3_pcs_tbl,
	.pcs_tbl_num		= ARRAY_SIZE(qmp_v3_usb3_pcs_tbl),
	.clk_list		= qmp_v3_phy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v3_phy_clk_l),
	.reset_list		= sc7180_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(sc7180_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v3_usb3phy_regs_layout,

	.start_ctrl		= SERDES_START | PCS_START,
	.pwrdn_ctrl		= SW_PWRDN,
	.phy_status		= PHYSTATUS,

	.has_pwrdn_delay	= true,
	.pwrdn_delay_min	= POWER_DOWN_DELAY_US_MIN,
	.pwrdn_delay_max	= POWER_DOWN_DELAY_US_MAX,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,
};

static const struct qmp_phy_cfg sc7180_dpphy_cfg = {
	.type			= PHY_TYPE_DP,
	.nlanes			= 1,

	.serdes_tbl		= qmp_v3_dp_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(qmp_v3_dp_serdes_tbl),
	.tx_tbl			= qmp_v3_dp_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(qmp_v3_dp_tx_tbl),

	.serdes_tbl_rbr		= qmp_v3_dp_serdes_tbl_rbr,
	.serdes_tbl_rbr_num	= ARRAY_SIZE(qmp_v3_dp_serdes_tbl_rbr),
	.serdes_tbl_hbr		= qmp_v3_dp_serdes_tbl_hbr,
	.serdes_tbl_hbr_num	= ARRAY_SIZE(qmp_v3_dp_serdes_tbl_hbr),
	.serdes_tbl_hbr2	= qmp_v3_dp_serdes_tbl_hbr2,
	.serdes_tbl_hbr2_num	= ARRAY_SIZE(qmp_v3_dp_serdes_tbl_hbr2),
	.serdes_tbl_hbr3	= qmp_v3_dp_serdes_tbl_hbr3,
	.serdes_tbl_hbr3_num	= ARRAY_SIZE(qmp_v3_dp_serdes_tbl_hbr3),

	.clk_list		= qmp_v3_phy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v3_phy_clk_l),
	.reset_list		= sc7180_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(sc7180_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v3_usb3phy_regs_layout,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,

	.dp_aux_init = qcom_qmp_v3_phy_dp_aux_init,
	.configure_dp_tx = qcom_qmp_v3_phy_configure_dp_tx,
	.configure_dp_phy = qcom_qmp_v3_phy_configure_dp_phy,
	.calibrate_dp_phy = qcom_qmp_v3_dp_phy_calibrate,
};

static const struct qmp_phy_combo_cfg sc7180_usb3dpphy_cfg = {
	.usb_cfg		= &sc7180_usb3phy_cfg,
	.dp_cfg			= &sc7180_dpphy_cfg,
};

static const struct qmp_phy_cfg sm8150_usb3phy_cfg = {
	.type			= PHY_TYPE_USB3,
	.nlanes			= 1,

	.serdes_tbl		= sm8150_usb3_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(sm8150_usb3_serdes_tbl),
	.tx_tbl			= sm8150_usb3_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(sm8150_usb3_tx_tbl),
	.rx_tbl			= sm8150_usb3_rx_tbl,
	.rx_tbl_num		= ARRAY_SIZE(sm8150_usb3_rx_tbl),
	.pcs_tbl		= sm8150_usb3_pcs_tbl,
	.pcs_tbl_num		= ARRAY_SIZE(sm8150_usb3_pcs_tbl),
	.pcs_usb_tbl		= sm8150_usb3_pcs_usb_tbl,
	.pcs_usb_tbl_num	= ARRAY_SIZE(sm8150_usb3_pcs_usb_tbl),
	.clk_list		= qmp_v4_phy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v4_phy_clk_l),
	.reset_list		= msm8996_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(msm8996_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v4_usb3phy_regs_layout,
	.pcs_usb_offset		= 0x300,

	.start_ctrl		= SERDES_START | PCS_START,
	.pwrdn_ctrl		= SW_PWRDN,
	.phy_status		= PHYSTATUS,


	.has_pwrdn_delay	= true,
	.pwrdn_delay_min	= POWER_DOWN_DELAY_US_MIN,
	.pwrdn_delay_max	= POWER_DOWN_DELAY_US_MAX,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,
};

static const struct qmp_phy_cfg sc8180x_dpphy_cfg = {
	.type			= PHY_TYPE_DP,
	.nlanes			= 1,

	.serdes_tbl		= qmp_v4_dp_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(qmp_v4_dp_serdes_tbl),
	.tx_tbl			= qmp_v4_dp_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(qmp_v4_dp_tx_tbl),

	.serdes_tbl_rbr		= qmp_v4_dp_serdes_tbl_rbr,
	.serdes_tbl_rbr_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_rbr),
	.serdes_tbl_hbr		= qmp_v4_dp_serdes_tbl_hbr,
	.serdes_tbl_hbr_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr),
	.serdes_tbl_hbr2	= qmp_v4_dp_serdes_tbl_hbr2,
	.serdes_tbl_hbr2_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr2),
	.serdes_tbl_hbr3	= qmp_v4_dp_serdes_tbl_hbr3,
	.serdes_tbl_hbr3_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr3),

	.clk_list		= qmp_v3_phy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v3_phy_clk_l),
	.reset_list		= msm8996_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(msm8996_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v3_usb3phy_regs_layout,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,

	.dp_aux_init = qcom_qmp_v4_phy_dp_aux_init,
	.configure_dp_tx = qcom_qmp_v4_phy_configure_dp_tx,
	.configure_dp_phy = qcom_qmp_v4_phy_configure_dp_phy,
	.calibrate_dp_phy = qcom_qmp_v4_dp_phy_calibrate,
};

static const struct qmp_phy_combo_cfg sc8180x_usb3dpphy_cfg = {
	.usb_cfg		= &sm8150_usb3phy_cfg,
	.dp_cfg			= &sc8180x_dpphy_cfg,
};

static const struct qmp_phy_cfg sm8250_usb3phy_cfg = {
	.type			= PHY_TYPE_USB3,
	.nlanes			= 1,

	.serdes_tbl		= sm8150_usb3_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(sm8150_usb3_serdes_tbl),
	.tx_tbl			= sm8250_usb3_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(sm8250_usb3_tx_tbl),
	.rx_tbl			= sm8250_usb3_rx_tbl,
	.rx_tbl_num		= ARRAY_SIZE(sm8250_usb3_rx_tbl),
	.pcs_tbl		= sm8250_usb3_pcs_tbl,
	.pcs_tbl_num		= ARRAY_SIZE(sm8250_usb3_pcs_tbl),
	.pcs_usb_tbl		= sm8250_usb3_pcs_usb_tbl,
	.pcs_usb_tbl_num	= ARRAY_SIZE(sm8250_usb3_pcs_usb_tbl),
	.clk_list		= qmp_v4_sm8250_usbphy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v4_sm8250_usbphy_clk_l),
	.reset_list		= msm8996_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(msm8996_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v4_usb3phy_regs_layout,
	.pcs_usb_offset		= 0x300,

	.start_ctrl		= SERDES_START | PCS_START,
	.pwrdn_ctrl		= SW_PWRDN,
	.phy_status		= PHYSTATUS,

	.has_pwrdn_delay	= true,
	.pwrdn_delay_min	= POWER_DOWN_DELAY_US_MIN,
	.pwrdn_delay_max	= POWER_DOWN_DELAY_US_MAX,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,
};

static const struct qmp_phy_cfg sm8250_dpphy_cfg = {
	.type			= PHY_TYPE_DP,
	.nlanes			= 1,

	.serdes_tbl		= qmp_v4_dp_serdes_tbl,
	.serdes_tbl_num		= ARRAY_SIZE(qmp_v4_dp_serdes_tbl),
	.tx_tbl			= qmp_v4_dp_tx_tbl,
	.tx_tbl_num		= ARRAY_SIZE(qmp_v4_dp_tx_tbl),

	.serdes_tbl_rbr		= qmp_v4_dp_serdes_tbl_rbr,
	.serdes_tbl_rbr_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_rbr),
	.serdes_tbl_hbr		= qmp_v4_dp_serdes_tbl_hbr,
	.serdes_tbl_hbr_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr),
	.serdes_tbl_hbr2	= qmp_v4_dp_serdes_tbl_hbr2,
	.serdes_tbl_hbr2_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr2),
	.serdes_tbl_hbr3	= qmp_v4_dp_serdes_tbl_hbr3,
	.serdes_tbl_hbr3_num	= ARRAY_SIZE(qmp_v4_dp_serdes_tbl_hbr3),

	.clk_list		= qmp_v4_sm8250_usbphy_clk_l,
	.num_clks		= ARRAY_SIZE(qmp_v4_sm8250_usbphy_clk_l),
	.reset_list		= msm8996_usb3phy_reset_l,
	.num_resets		= ARRAY_SIZE(msm8996_usb3phy_reset_l),
	.vreg_list		= qmp_phy_vreg_l,
	.num_vregs		= ARRAY_SIZE(qmp_phy_vreg_l),
	.regs			= qmp_v4_usb3phy_regs_layout,

	.has_phy_dp_com_ctrl	= true,
	.is_dual_lane_phy	= true,

	.dp_aux_init = qcom_qmp_v4_phy_dp_aux_init,
	.configure_dp_tx = qcom_qmp_v4_phy_configure_dp_tx,
	.configure_dp_phy = qcom_qmp_v4_phy_configure_dp_phy,
	.calibrate_dp_phy = qcom_qmp_v4_dp_phy_calibrate,
};

static const struct qmp_phy_combo_cfg sm8250_usb3dpphy_cfg = {
	.usb_cfg		= &sm8250_usb3phy_cfg,
	.dp_cfg			= &sm8250_dpphy_cfg,
};

static void qcom_qmp_phy_combo_configure_lane(void __iomem *base,
					const unsigned int *regs,
					const struct qmp_phy_init_tbl tbl[],
					int num,
					u8 lane_mask)
{
	int i;
	const struct qmp_phy_init_tbl *t = tbl;

	if (!t)
		return;

	for (i = 0; i < num; i++, t++) {
		if (!(t->lane_mask & lane_mask))
			continue;

		if (t->in_layout)
			writel(t->val, base + regs[t->offset]);
		else
			writel(t->val, base + t->offset);
	}
}

static void qcom_qmp_phy_combo_configure(void __iomem *base,
				   const unsigned int *regs,
				   const struct qmp_phy_init_tbl tbl[],
				   int num)
{
	qcom_qmp_phy_combo_configure_lane(base, regs, tbl, num, 0xff);
}

static int qcom_qmp_phy_combo_serdes_init(struct qmp_phy *qphy)
{
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	void __iomem *serdes = qphy->serdes;
	const struct phy_configure_opts_dp *dp_opts = &qphy->dp_opts;
	const struct qmp_phy_init_tbl *serdes_tbl = cfg->serdes_tbl;
	int serdes_tbl_num = cfg->serdes_tbl_num;

	qcom_qmp_phy_combo_configure(serdes, cfg->regs, serdes_tbl, serdes_tbl_num);

	if (cfg->type == PHY_TYPE_DP) {
		switch (dp_opts->link_rate) {
		case 1620:
			qcom_qmp_phy_combo_configure(serdes, cfg->regs,
					       cfg->serdes_tbl_rbr,
					       cfg->serdes_tbl_rbr_num);
			break;
		case 2700:
			qcom_qmp_phy_combo_configure(serdes, cfg->regs,
					       cfg->serdes_tbl_hbr,
					       cfg->serdes_tbl_hbr_num);
			break;
		case 5400:
			qcom_qmp_phy_combo_configure(serdes, cfg->regs,
					       cfg->serdes_tbl_hbr2,
					       cfg->serdes_tbl_hbr2_num);
			break;
		case 8100:
			qcom_qmp_phy_combo_configure(serdes, cfg->regs,
					       cfg->serdes_tbl_hbr3,
					       cfg->serdes_tbl_hbr3_num);
			break;
		default:
			/* Other link rates aren't supported */
			return -EINVAL;
		}
	}

	return 0;
}

static void qcom_qmp_v3_phy_dp_aux_init(struct qmp_phy *qphy)
{
	writel(DP_PHY_PD_CTL_PWRDN | DP_PHY_PD_CTL_AUX_PWRDN |
	       DP_PHY_PD_CTL_PLL_PWRDN | DP_PHY_PD_CTL_DP_CLAMP_EN,
	       qphy->pcs + QSERDES_DP_PHY_PD_CTL);

	/* Turn on BIAS current for PHY/PLL */
	writel(QSERDES_V3_COM_BIAS_EN | QSERDES_V3_COM_BIAS_EN_MUX |
	       QSERDES_V3_COM_CLKBUF_L_EN | QSERDES_V3_COM_EN_SYSCLK_TX_SEL,
	       qphy->serdes + QSERDES_V3_COM_BIAS_EN_CLKBUFLR_EN);

	writel(DP_PHY_PD_CTL_PSR_PWRDN, qphy->pcs + QSERDES_DP_PHY_PD_CTL);

	writel(DP_PHY_PD_CTL_PWRDN | DP_PHY_PD_CTL_AUX_PWRDN |
	       DP_PHY_PD_CTL_LANE_0_1_PWRDN |
	       DP_PHY_PD_CTL_LANE_2_3_PWRDN | DP_PHY_PD_CTL_PLL_PWRDN |
	       DP_PHY_PD_CTL_DP_CLAMP_EN,
	       qphy->pcs + QSERDES_DP_PHY_PD_CTL);

	writel(QSERDES_V3_COM_BIAS_EN |
	       QSERDES_V3_COM_BIAS_EN_MUX | QSERDES_V3_COM_CLKBUF_R_EN |
	       QSERDES_V3_COM_CLKBUF_L_EN | QSERDES_V3_COM_EN_SYSCLK_TX_SEL |
	       QSERDES_V3_COM_CLKBUF_RX_DRIVE_L,
	       qphy->serdes + QSERDES_V3_COM_BIAS_EN_CLKBUFLR_EN);

	writel(0x00, qphy->pcs + QSERDES_DP_PHY_AUX_CFG0);
	writel(0x13, qphy->pcs + QSERDES_DP_PHY_AUX_CFG1);
	writel(0x24, qphy->pcs + QSERDES_DP_PHY_AUX_CFG2);
	writel(0x00, qphy->pcs + QSERDES_DP_PHY_AUX_CFG3);
	writel(0x0a, qphy->pcs + QSERDES_DP_PHY_AUX_CFG4);
	writel(0x26, qphy->pcs + QSERDES_DP_PHY_AUX_CFG5);
	writel(0x0a, qphy->pcs + QSERDES_DP_PHY_AUX_CFG6);
	writel(0x03, qphy->pcs + QSERDES_DP_PHY_AUX_CFG7);
	writel(0xbb, qphy->pcs + QSERDES_DP_PHY_AUX_CFG8);
	writel(0x03, qphy->pcs + QSERDES_DP_PHY_AUX_CFG9);
	qphy->dp_aux_cfg = 0;

	writel(PHY_AUX_STOP_ERR_MASK | PHY_AUX_DEC_ERR_MASK |
	       PHY_AUX_SYNC_ERR_MASK | PHY_AUX_ALIGN_ERR_MASK |
	       PHY_AUX_REQ_ERR_MASK,
	       qphy->pcs + QSERDES_V3_DP_PHY_AUX_INTERRUPT_MASK);
}

static const u8 qmp_dp_v3_pre_emphasis_hbr3_hbr2[4][4] = {
	{ 0x00, 0x0c, 0x15, 0x1a },
	{ 0x02, 0x0e, 0x16, 0xff },
	{ 0x02, 0x11, 0xff, 0xff },
	{ 0x04, 0xff, 0xff, 0xff }
};

static const u8 qmp_dp_v3_voltage_swing_hbr3_hbr2[4][4] = {
	{ 0x02, 0x12, 0x16, 0x1a },
	{ 0x09, 0x19, 0x1f, 0xff },
	{ 0x10, 0x1f, 0xff, 0xff },
	{ 0x1f, 0xff, 0xff, 0xff }
};

static const u8 qmp_dp_v3_pre_emphasis_hbr_rbr[4][4] = {
	{ 0x00, 0x0c, 0x14, 0x19 },
	{ 0x00, 0x0b, 0x12, 0xff },
	{ 0x00, 0x0b, 0xff, 0xff },
	{ 0x04, 0xff, 0xff, 0xff }
};

static const u8 qmp_dp_v3_voltage_swing_hbr_rbr[4][4] = {
	{ 0x08, 0x0f, 0x16, 0x1f },
	{ 0x11, 0x1e, 0x1f, 0xff },
	{ 0x19, 0x1f, 0xff, 0xff },
	{ 0x1f, 0xff, 0xff, 0xff }
};

static int qcom_qmp_phy_combo_configure_dp_swing(struct qmp_phy *qphy,
		unsigned int drv_lvl_reg, unsigned int emp_post_reg)
{
	const struct phy_configure_opts_dp *dp_opts = &qphy->dp_opts;
	unsigned int v_level = 0, p_level = 0;
	u8 voltage_swing_cfg, pre_emphasis_cfg;
	int i;

	for (i = 0; i < dp_opts->lanes; i++) {
		v_level = max(v_level, dp_opts->voltage[i]);
		p_level = max(p_level, dp_opts->pre[i]);
	}

	if (dp_opts->link_rate <= 2700) {
		voltage_swing_cfg = qmp_dp_v3_voltage_swing_hbr_rbr[v_level][p_level];
		pre_emphasis_cfg = qmp_dp_v3_pre_emphasis_hbr_rbr[v_level][p_level];
	} else {
		voltage_swing_cfg = qmp_dp_v3_voltage_swing_hbr3_hbr2[v_level][p_level];
		pre_emphasis_cfg = qmp_dp_v3_pre_emphasis_hbr3_hbr2[v_level][p_level];
	}

	/* TODO: Move check to config check */
	if (voltage_swing_cfg == 0xFF && pre_emphasis_cfg == 0xFF)
		return -EINVAL;

	/* Enable MUX to use Cursor values from these registers */
	voltage_swing_cfg |= DP_PHY_TXn_TX_DRV_LVL_MUX_EN;
	pre_emphasis_cfg |= DP_PHY_TXn_TX_EMP_POST1_LVL_MUX_EN;

	writel(voltage_swing_cfg, qphy->tx + drv_lvl_reg);
	writel(pre_emphasis_cfg, qphy->tx + emp_post_reg);
	writel(voltage_swing_cfg, qphy->tx2 + drv_lvl_reg);
	writel(pre_emphasis_cfg, qphy->tx2 + emp_post_reg);

	return 0;
}

static void qcom_qmp_v3_phy_configure_dp_tx(struct qmp_phy *qphy)
{
	const struct phy_configure_opts_dp *dp_opts = &qphy->dp_opts;
	u32 bias_en, drvr_en;

	if (qcom_qmp_phy_combo_configure_dp_swing(qphy,
				QSERDES_V3_TX_TX_DRV_LVL,
				QSERDES_V3_TX_TX_EMP_POST1_LVL) < 0)
		return;

	if (dp_opts->lanes == 1) {
		bias_en = 0x3e;
		drvr_en = 0x13;
	} else {
		bias_en = 0x3f;
		drvr_en = 0x10;
	}

	writel(drvr_en, qphy->tx + QSERDES_V3_TX_HIGHZ_DRVR_EN);
	writel(bias_en, qphy->tx + QSERDES_V3_TX_TRANSCEIVER_BIAS_EN);
	writel(drvr_en, qphy->tx2 + QSERDES_V3_TX_HIGHZ_DRVR_EN);
	writel(bias_en, qphy->tx2 + QSERDES_V3_TX_TRANSCEIVER_BIAS_EN);
}

static bool qcom_qmp_phy_combo_configure_dp_mode(struct qmp_phy *qphy)
{
	u32 val;
	bool reverse = false;

	val = DP_PHY_PD_CTL_PWRDN | DP_PHY_PD_CTL_AUX_PWRDN |
	      DP_PHY_PD_CTL_PLL_PWRDN | DP_PHY_PD_CTL_DP_CLAMP_EN;

	/*
	 * TODO: Assume orientation is CC1 for now and two lanes, need to
	 * use type-c connector to understand orientation and lanes.
	 *
	 * Otherwise val changes to be like below if this code understood
	 * the orientation of the type-c cable.
	 *
	 * if (lane_cnt == 4 || orientation == ORIENTATION_CC2)
	 *	val |= DP_PHY_PD_CTL_LANE_0_1_PWRDN;
	 * if (lane_cnt == 4 || orientation == ORIENTATION_CC1)
	 *	val |= DP_PHY_PD_CTL_LANE_2_3_PWRDN;
	 * if (orientation == ORIENTATION_CC2)
	 *	writel(0x4c, qphy->pcs + QSERDES_V3_DP_PHY_MODE);
	 */
	val |= DP_PHY_PD_CTL_LANE_2_3_PWRDN;
	writel(val, qphy->pcs + QSERDES_DP_PHY_PD_CTL);

	writel(0x5c, qphy->pcs + QSERDES_DP_PHY_MODE);

	return reverse;
}

static int qcom_qmp_v3_phy_configure_dp_phy(struct qmp_phy *qphy)
{
	const struct qmp_phy_dp_clks *dp_clks = qphy->dp_clks;
	const struct phy_configure_opts_dp *dp_opts = &qphy->dp_opts;
	u32 phy_vco_div, status;
	unsigned long pixel_freq;

	qcom_qmp_phy_combo_configure_dp_mode(qphy);

	writel(0x05, qphy->pcs + QSERDES_V3_DP_PHY_TX0_TX1_LANE_CTL);
	writel(0x05, qphy->pcs + QSERDES_V3_DP_PHY_TX2_TX3_LANE_CTL);

	switch (dp_opts->link_rate) {
	case 1620:
		phy_vco_div = 0x1;
		pixel_freq = 1620000000UL / 2;
		break;
	case 2700:
		phy_vco_div = 0x1;
		pixel_freq = 2700000000UL / 2;
		break;
	case 5400:
		phy_vco_div = 0x2;
		pixel_freq = 5400000000UL / 4;
		break;
	case 8100:
		phy_vco_div = 0x0;
		pixel_freq = 8100000000UL / 6;
		break;
	default:
		/* Other link rates aren't supported */
		return -EINVAL;
	}
	writel(phy_vco_div, qphy->pcs + QSERDES_V3_DP_PHY_VCO_DIV);

	clk_set_rate(dp_clks->dp_link_hw.clk, dp_opts->link_rate * 100000);
	clk_set_rate(dp_clks->dp_pixel_hw.clk, pixel_freq);

	writel(0x04, qphy->pcs + QSERDES_DP_PHY_AUX_CFG2);
	writel(0x01, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x05, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x01, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x09, qphy->pcs + QSERDES_DP_PHY_CFG);

	writel(0x20, qphy->serdes + QSERDES_V3_COM_RESETSM_CNTRL);

	if (readl_poll_timeout(qphy->serdes + QSERDES_V3_COM_C_READY_STATUS,
			status,
			((status & BIT(0)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	writel(0x19, qphy->pcs + QSERDES_DP_PHY_CFG);

	if (readl_poll_timeout(qphy->pcs + QSERDES_V3_DP_PHY_STATUS,
			status,
			((status & BIT(1)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	writel(0x18, qphy->pcs + QSERDES_DP_PHY_CFG);
	udelay(2000);
	writel(0x19, qphy->pcs + QSERDES_DP_PHY_CFG);

	return readl_poll_timeout(qphy->pcs + QSERDES_V3_DP_PHY_STATUS,
			status,
			((status & BIT(1)) > 0),
			500,
			10000);
}

/*
 * We need to calibrate the aux setting here as many times
 * as the caller tries
 */
static int qcom_qmp_v3_dp_phy_calibrate(struct qmp_phy *qphy)
{
	static const u8 cfg1_settings[] = { 0x13, 0x23, 0x1d };
	u8 val;

	qphy->dp_aux_cfg++;
	qphy->dp_aux_cfg %= ARRAY_SIZE(cfg1_settings);
	val = cfg1_settings[qphy->dp_aux_cfg];

	writel(val, qphy->pcs + QSERDES_DP_PHY_AUX_CFG1);

	return 0;
}

static void qcom_qmp_v4_phy_dp_aux_init(struct qmp_phy *qphy)
{
	writel(DP_PHY_PD_CTL_PWRDN | DP_PHY_PD_CTL_PSR_PWRDN | DP_PHY_PD_CTL_AUX_PWRDN |
	       DP_PHY_PD_CTL_PLL_PWRDN | DP_PHY_PD_CTL_DP_CLAMP_EN,
	       qphy->pcs + QSERDES_DP_PHY_PD_CTL);

	/* Turn on BIAS current for PHY/PLL */
	writel(0x17, qphy->serdes + QSERDES_V4_COM_BIAS_EN_CLKBUFLR_EN);

	writel(0x00, qphy->pcs + QSERDES_DP_PHY_AUX_CFG0);
	writel(0x13, qphy->pcs + QSERDES_DP_PHY_AUX_CFG1);
	writel(0xa4, qphy->pcs + QSERDES_DP_PHY_AUX_CFG2);
	writel(0x00, qphy->pcs + QSERDES_DP_PHY_AUX_CFG3);
	writel(0x0a, qphy->pcs + QSERDES_DP_PHY_AUX_CFG4);
	writel(0x26, qphy->pcs + QSERDES_DP_PHY_AUX_CFG5);
	writel(0x0a, qphy->pcs + QSERDES_DP_PHY_AUX_CFG6);
	writel(0x03, qphy->pcs + QSERDES_DP_PHY_AUX_CFG7);
	writel(0xb7, qphy->pcs + QSERDES_DP_PHY_AUX_CFG8);
	writel(0x03, qphy->pcs + QSERDES_DP_PHY_AUX_CFG9);
	qphy->dp_aux_cfg = 0;

	writel(PHY_AUX_STOP_ERR_MASK | PHY_AUX_DEC_ERR_MASK |
	       PHY_AUX_SYNC_ERR_MASK | PHY_AUX_ALIGN_ERR_MASK |
	       PHY_AUX_REQ_ERR_MASK,
	       qphy->pcs + QSERDES_V4_DP_PHY_AUX_INTERRUPT_MASK);
}

static void qcom_qmp_v4_phy_configure_dp_tx(struct qmp_phy *qphy)
{
	/* Program default values before writing proper values */
	writel(0x27, qphy->tx + QSERDES_V4_TX_TX_DRV_LVL);
	writel(0x27, qphy->tx2 + QSERDES_V4_TX_TX_DRV_LVL);

	writel(0x20, qphy->tx + QSERDES_V4_TX_TX_EMP_POST1_LVL);
	writel(0x20, qphy->tx2 + QSERDES_V4_TX_TX_EMP_POST1_LVL);

	qcom_qmp_phy_combo_configure_dp_swing(qphy,
			QSERDES_V4_TX_TX_DRV_LVL,
			QSERDES_V4_TX_TX_EMP_POST1_LVL);
}

static int qcom_qmp_v4_phy_configure_dp_phy(struct qmp_phy *qphy)
{
	const struct qmp_phy_dp_clks *dp_clks = qphy->dp_clks;
	const struct phy_configure_opts_dp *dp_opts = &qphy->dp_opts;
	u32 phy_vco_div, status;
	unsigned long pixel_freq;
	u32 bias0_en, drvr0_en, bias1_en, drvr1_en;
	bool reverse;

	writel(0x0f, qphy->pcs + QSERDES_V4_DP_PHY_CFG_1);

	reverse = qcom_qmp_phy_combo_configure_dp_mode(qphy);

	writel(0x13, qphy->pcs + QSERDES_DP_PHY_AUX_CFG1);
	writel(0xa4, qphy->pcs + QSERDES_DP_PHY_AUX_CFG2);

	writel(0x05, qphy->pcs + QSERDES_V4_DP_PHY_TX0_TX1_LANE_CTL);
	writel(0x05, qphy->pcs + QSERDES_V4_DP_PHY_TX2_TX3_LANE_CTL);

	switch (dp_opts->link_rate) {
	case 1620:
		phy_vco_div = 0x1;
		pixel_freq = 1620000000UL / 2;
		break;
	case 2700:
		phy_vco_div = 0x1;
		pixel_freq = 2700000000UL / 2;
		break;
	case 5400:
		phy_vco_div = 0x2;
		pixel_freq = 5400000000UL / 4;
		break;
	case 8100:
		phy_vco_div = 0x0;
		pixel_freq = 8100000000UL / 6;
		break;
	default:
		/* Other link rates aren't supported */
		return -EINVAL;
	}
	writel(phy_vco_div, qphy->pcs + QSERDES_V4_DP_PHY_VCO_DIV);

	clk_set_rate(dp_clks->dp_link_hw.clk, dp_opts->link_rate * 100000);
	clk_set_rate(dp_clks->dp_pixel_hw.clk, pixel_freq);

	writel(0x01, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x05, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x01, qphy->pcs + QSERDES_DP_PHY_CFG);
	writel(0x09, qphy->pcs + QSERDES_DP_PHY_CFG);

	writel(0x20, qphy->serdes + QSERDES_V4_COM_RESETSM_CNTRL);

	if (readl_poll_timeout(qphy->serdes + QSERDES_V4_COM_C_READY_STATUS,
			status,
			((status & BIT(0)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	if (readl_poll_timeout(qphy->serdes + QSERDES_V4_COM_CMN_STATUS,
			status,
			((status & BIT(0)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	if (readl_poll_timeout(qphy->serdes + QSERDES_V4_COM_CMN_STATUS,
			status,
			((status & BIT(1)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	writel(0x19, qphy->pcs + QSERDES_DP_PHY_CFG);

	if (readl_poll_timeout(qphy->pcs + QSERDES_V4_DP_PHY_STATUS,
			status,
			((status & BIT(0)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	if (readl_poll_timeout(qphy->pcs + QSERDES_V4_DP_PHY_STATUS,
			status,
			((status & BIT(1)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	/*
	 * At least for 7nm DP PHY this has to be done after enabling link
	 * clock.
	 */

	if (dp_opts->lanes == 1) {
		bias0_en = reverse ? 0x3e : 0x15;
		bias1_en = reverse ? 0x15 : 0x3e;
		drvr0_en = reverse ? 0x13 : 0x10;
		drvr1_en = reverse ? 0x10 : 0x13;
	} else if (dp_opts->lanes == 2) {
		bias0_en = reverse ? 0x3f : 0x15;
		bias1_en = reverse ? 0x15 : 0x3f;
		drvr0_en = 0x10;
		drvr1_en = 0x10;
	} else {
		bias0_en = 0x3f;
		bias1_en = 0x3f;
		drvr0_en = 0x10;
		drvr1_en = 0x10;
	}

	writel(drvr0_en, qphy->tx + QSERDES_V4_TX_HIGHZ_DRVR_EN);
	writel(bias0_en, qphy->tx + QSERDES_V4_TX_TRANSCEIVER_BIAS_EN);
	writel(drvr1_en, qphy->tx2 + QSERDES_V4_TX_HIGHZ_DRVR_EN);
	writel(bias1_en, qphy->tx2 + QSERDES_V4_TX_TRANSCEIVER_BIAS_EN);

	writel(0x18, qphy->pcs + QSERDES_DP_PHY_CFG);
	udelay(2000);
	writel(0x19, qphy->pcs + QSERDES_DP_PHY_CFG);

	if (readl_poll_timeout(qphy->pcs + QSERDES_V4_DP_PHY_STATUS,
			status,
			((status & BIT(1)) > 0),
			500,
			10000))
		return -ETIMEDOUT;

	writel(0x0a, qphy->tx + QSERDES_V4_TX_TX_POL_INV);
	writel(0x0a, qphy->tx2 + QSERDES_V4_TX_TX_POL_INV);

	writel(0x27, qphy->tx + QSERDES_V4_TX_TX_DRV_LVL);
	writel(0x27, qphy->tx2 + QSERDES_V4_TX_TX_DRV_LVL);

	writel(0x20, qphy->tx + QSERDES_V4_TX_TX_EMP_POST1_LVL);
	writel(0x20, qphy->tx2 + QSERDES_V4_TX_TX_EMP_POST1_LVL);

	return 0;
}

/*
 * We need to calibrate the aux setting here as many times
 * as the caller tries
 */
static int qcom_qmp_v4_dp_phy_calibrate(struct qmp_phy *qphy)
{
	static const u8 cfg1_settings[] = { 0x20, 0x13, 0x23, 0x1d };
	u8 val;

	qphy->dp_aux_cfg++;
	qphy->dp_aux_cfg %= ARRAY_SIZE(cfg1_settings);
	val = cfg1_settings[qphy->dp_aux_cfg];

	writel(val, qphy->pcs + QSERDES_DP_PHY_AUX_CFG1);

	return 0;
}

static int qcom_qmp_dp_phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	const struct phy_configure_opts_dp *dp_opts = &opts->dp;
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	const struct qmp_phy_cfg *cfg = qphy->cfg;

	memcpy(&qphy->dp_opts, dp_opts, sizeof(*dp_opts));
	if (qphy->dp_opts.set_voltages) {
		cfg->configure_dp_tx(qphy);
		qphy->dp_opts.set_voltages = 0;
	}

	return 0;
}

static int qcom_qmp_dp_phy_calibrate(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	const struct qmp_phy_cfg *cfg = qphy->cfg;

	if (cfg->calibrate_dp_phy)
		return cfg->calibrate_dp_phy(qphy);

	return 0;
}

static int qcom_qmp_phy_combo_com_init(struct qmp_phy *qphy)
{
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	struct qmp_phy *usb_phy = qmp->usb_phy;
	void __iomem *dp_com = qmp->dp_com;
	int ret;

	mutex_lock(&qmp->phy_mutex);
	if (qmp->init_count++) {
		mutex_unlock(&qmp->phy_mutex);
		return 0;
	}

	/* turn on regulator supplies */
	ret = regulator_bulk_enable(cfg->num_vregs, qmp->vregs);
	if (ret) {
		dev_err(qmp->dev, "failed to enable regulators, err=%d\n", ret);
		goto err_unlock;
	}

	ret = reset_control_bulk_assert(cfg->num_resets, qmp->resets);
	if (ret) {
		dev_err(qmp->dev, "reset assert failed\n");
		goto err_disable_regulators;
	}

	ret = reset_control_bulk_deassert(cfg->num_resets, qmp->resets);
	if (ret) {
		dev_err(qmp->dev, "reset deassert failed\n");
		goto err_disable_regulators;
	}

	ret = clk_bulk_prepare_enable(cfg->num_clks, qmp->clks);
	if (ret)
		goto err_assert_reset;

	if (cfg->has_phy_dp_com_ctrl) {
		qphy_setbits(dp_com, QPHY_V3_DP_COM_POWER_DOWN_CTRL,
			     SW_PWRDN);
		/* override hardware control for reset of qmp phy */
		qphy_setbits(dp_com, QPHY_V3_DP_COM_RESET_OVRD_CTRL,
			     SW_DPPHY_RESET_MUX | SW_DPPHY_RESET |
			     SW_USB3PHY_RESET_MUX | SW_USB3PHY_RESET);

		/* Default type-c orientation, i.e CC1 */
		qphy_setbits(dp_com, QPHY_V3_DP_COM_TYPEC_CTRL, 0x02);

		qphy_setbits(dp_com, QPHY_V3_DP_COM_PHY_MODE_CTRL,
			     USB3_MODE | DP_MODE);

		/* bring both QMP USB and QMP DP PHYs PCS block out of reset */
		qphy_clrbits(dp_com, QPHY_V3_DP_COM_RESET_OVRD_CTRL,
			     SW_DPPHY_RESET_MUX | SW_DPPHY_RESET |
			     SW_USB3PHY_RESET_MUX | SW_USB3PHY_RESET);

		qphy_clrbits(dp_com, QPHY_V3_DP_COM_SWI_CTRL, 0x03);
		qphy_clrbits(dp_com, QPHY_V3_DP_COM_SW_RESET, SW_RESET);
	}

	if (usb_phy->cfg->regs[QPHY_PCS_POWER_DOWN_CONTROL])
		qphy_setbits(usb_phy->pcs,
				usb_phy->cfg->regs[QPHY_PCS_POWER_DOWN_CONTROL],
				usb_phy->cfg->pwrdn_ctrl);
	else
		qphy_setbits(usb_phy->pcs, QPHY_V2_PCS_POWER_DOWN_CONTROL,
				usb_phy->cfg->pwrdn_ctrl);

	mutex_unlock(&qmp->phy_mutex);

	return 0;

err_assert_reset:
	reset_control_bulk_assert(cfg->num_resets, qmp->resets);
err_disable_regulators:
	regulator_bulk_disable(cfg->num_vregs, qmp->vregs);
err_unlock:
	mutex_unlock(&qmp->phy_mutex);

	return ret;
}

static int qcom_qmp_phy_combo_com_exit(struct qmp_phy *qphy)
{
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qphy->cfg;

	mutex_lock(&qmp->phy_mutex);
	if (--qmp->init_count) {
		mutex_unlock(&qmp->phy_mutex);
		return 0;
	}

	reset_control_assert(qmp->ufs_reset);

	reset_control_bulk_assert(cfg->num_resets, qmp->resets);

	clk_bulk_disable_unprepare(cfg->num_clks, qmp->clks);

	regulator_bulk_disable(cfg->num_vregs, qmp->vregs);

	mutex_unlock(&qmp->phy_mutex);

	return 0;
}

static int qcom_qmp_phy_combo_init(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	int ret;
	dev_vdbg(qmp->dev, "Initializing QMP phy\n");

	ret = qcom_qmp_phy_combo_com_init(qphy);
	if (ret)
		return ret;

	if (cfg->type == PHY_TYPE_DP)
		cfg->dp_aux_init(qphy);

	return 0;
}

static int qcom_qmp_phy_combo_power_on(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	struct qcom_qmp *qmp = qphy->qmp;
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	void __iomem *tx = qphy->tx;
	void __iomem *rx = qphy->rx;
	void __iomem *pcs = qphy->pcs;
	void __iomem *status;
	unsigned int mask, val, ready;
	int ret;

	qcom_qmp_phy_combo_serdes_init(qphy);

	ret = clk_prepare_enable(qphy->pipe_clk);
	if (ret) {
		dev_err(qmp->dev, "pipe_clk enable failed err=%d\n", ret);
		return ret;
	}

	/* Tx, Rx, and PCS configurations */
	qcom_qmp_phy_combo_configure_lane(tx, cfg->regs,
				    cfg->tx_tbl, cfg->tx_tbl_num, 1);

	/* Configuration for other LANE for USB-DP combo PHY */
	if (cfg->is_dual_lane_phy) {
		qcom_qmp_phy_combo_configure_lane(qphy->tx2, cfg->regs,
					    cfg->tx_tbl, cfg->tx_tbl_num, 2);
	}

	/* Configure special DP tx tunings */
	if (cfg->type == PHY_TYPE_DP)
		cfg->configure_dp_tx(qphy);

	qcom_qmp_phy_combo_configure_lane(rx, cfg->regs,
				    cfg->rx_tbl, cfg->rx_tbl_num, 1);

	if (cfg->is_dual_lane_phy) {
		qcom_qmp_phy_combo_configure_lane(qphy->rx2, cfg->regs,
					    cfg->rx_tbl, cfg->rx_tbl_num, 2);
	}

	/* Configure link rate, swing, etc. */
	if (cfg->type == PHY_TYPE_DP) {
		cfg->configure_dp_phy(qphy);
	} else {
		qcom_qmp_phy_combo_configure(pcs, cfg->regs, cfg->pcs_tbl, cfg->pcs_tbl_num);
	}

	ret = reset_control_deassert(qmp->ufs_reset);
	if (ret)
		goto err_disable_pipe_clk;

	if (cfg->has_pwrdn_delay)
		usleep_range(cfg->pwrdn_delay_min, cfg->pwrdn_delay_max);

	if (cfg->type != PHY_TYPE_DP) {
		/* Pull PHY out of reset state */
		qphy_clrbits(pcs, cfg->regs[QPHY_SW_RESET], SW_RESET);
		/* start SerDes and Phy-Coding-Sublayer */
		qphy_setbits(pcs, cfg->regs[QPHY_START_CTRL], cfg->start_ctrl);

		status = pcs + cfg->regs[QPHY_PCS_STATUS];
		mask = cfg->phy_status;
		ready = 0;

		ret = readl_poll_timeout(status, val, (val & mask) == ready, 10,
					 PHY_INIT_COMPLETE_TIMEOUT);
		if (ret) {
			dev_err(qmp->dev, "phy initialization timed-out\n");
			goto err_disable_pipe_clk;
		}
	}
	return 0;

err_disable_pipe_clk:
	clk_disable_unprepare(qphy->pipe_clk);

	return ret;
}

static int qcom_qmp_phy_combo_power_off(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);
	const struct qmp_phy_cfg *cfg = qphy->cfg;

	clk_disable_unprepare(qphy->pipe_clk);

	if (cfg->type == PHY_TYPE_DP) {
		/* Assert DP PHY power down */
		writel(DP_PHY_PD_CTL_PSR_PWRDN, qphy->pcs + QSERDES_DP_PHY_PD_CTL);
	} else {
		/* PHY reset */
		qphy_setbits(qphy->pcs, cfg->regs[QPHY_SW_RESET], SW_RESET);

		/* stop SerDes and Phy-Coding-Sublayer */
		qphy_clrbits(qphy->pcs, cfg->regs[QPHY_START_CTRL], cfg->start_ctrl);

		/* Put PHY into POWER DOWN state: active low */
		if (cfg->regs[QPHY_PCS_POWER_DOWN_CONTROL]) {
			qphy_clrbits(qphy->pcs, cfg->regs[QPHY_PCS_POWER_DOWN_CONTROL],
				     cfg->pwrdn_ctrl);
		} else {
			qphy_clrbits(qphy->pcs, QPHY_V2_PCS_POWER_DOWN_CONTROL,
					cfg->pwrdn_ctrl);
		}
	}

	return 0;
}

static int qcom_qmp_phy_combo_exit(struct phy *phy)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);

	qcom_qmp_phy_combo_com_exit(qphy);

	return 0;
}

static int qcom_qmp_phy_combo_enable(struct phy *phy)
{
	int ret;

	ret = qcom_qmp_phy_combo_init(phy);
	if (ret)
		return ret;

	ret = qcom_qmp_phy_combo_power_on(phy);
	if (ret)
		qcom_qmp_phy_combo_exit(phy);

	return ret;
}

static int qcom_qmp_phy_combo_disable(struct phy *phy)
{
	int ret;

	ret = qcom_qmp_phy_combo_power_off(phy);
	if (ret)
		return ret;
	return qcom_qmp_phy_combo_exit(phy);
}

static int qcom_qmp_phy_combo_set_mode(struct phy *phy,
				 enum phy_mode mode, int submode)
{
	struct qmp_phy *qphy = phy_get_drvdata(phy);

	qphy->mode = mode;

	return 0;
}

static void qcom_qmp_phy_combo_enable_autonomous_mode(struct qmp_phy *qphy)
{
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	void __iomem *pcs_usb = qphy->pcs_usb ?: qphy->pcs;
	void __iomem *pcs_misc = qphy->pcs_misc;
	u32 intr_mask;

	if (qphy->mode == PHY_MODE_USB_HOST_SS ||
	    qphy->mode == PHY_MODE_USB_DEVICE_SS)
		intr_mask = ARCVR_DTCT_EN | ALFPS_DTCT_EN;
	else
		intr_mask = ARCVR_DTCT_EN | ARCVR_DTCT_EVENT_SEL;

	/* Clear any pending interrupts status */
	qphy_setbits(pcs_usb, cfg->regs[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR], IRQ_CLEAR);
	/* Writing 1 followed by 0 clears the interrupt */
	qphy_clrbits(pcs_usb, cfg->regs[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR], IRQ_CLEAR);

	qphy_clrbits(pcs_usb, cfg->regs[QPHY_PCS_AUTONOMOUS_MODE_CTRL],
		     ARCVR_DTCT_EN | ALFPS_DTCT_EN | ARCVR_DTCT_EVENT_SEL);

	/* Enable required PHY autonomous mode interrupts */
	qphy_setbits(pcs_usb, cfg->regs[QPHY_PCS_AUTONOMOUS_MODE_CTRL], intr_mask);

	/* Enable i/o clamp_n for autonomous mode */
	if (pcs_misc)
		qphy_clrbits(pcs_misc, QPHY_V3_PCS_MISC_CLAMP_ENABLE, CLAMP_EN);
}

static void qcom_qmp_phy_combo_disable_autonomous_mode(struct qmp_phy *qphy)
{
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	void __iomem *pcs_usb = qphy->pcs_usb ?: qphy->pcs;
	void __iomem *pcs_misc = qphy->pcs_misc;

	/* Disable i/o clamp_n on resume for normal mode */
	if (pcs_misc)
		qphy_setbits(pcs_misc, QPHY_V3_PCS_MISC_CLAMP_ENABLE, CLAMP_EN);

	qphy_clrbits(pcs_usb, cfg->regs[QPHY_PCS_AUTONOMOUS_MODE_CTRL],
		     ARCVR_DTCT_EN | ARCVR_DTCT_EVENT_SEL | ALFPS_DTCT_EN);

	qphy_setbits(pcs_usb, cfg->regs[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR], IRQ_CLEAR);
	/* Writing 1 followed by 0 clears the interrupt */
	qphy_clrbits(pcs_usb, cfg->regs[QPHY_PCS_LFPS_RXTERM_IRQ_CLEAR], IRQ_CLEAR);
}

static int __maybe_unused qcom_qmp_phy_combo_runtime_suspend(struct device *dev)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	struct qmp_phy *qphy = qmp->phys[0];
	const struct qmp_phy_cfg *cfg = qphy->cfg;

	dev_vdbg(dev, "Suspending QMP phy, mode:%d\n", qphy->mode);

	/* Supported only for USB3 PHY and luckily USB3 is the first phy */
	if (cfg->type != PHY_TYPE_USB3)
		return 0;

	if (!qmp->init_count) {
		dev_vdbg(dev, "PHY not initialized, bailing out\n");
		return 0;
	}

	qcom_qmp_phy_combo_enable_autonomous_mode(qphy);

	clk_disable_unprepare(qphy->pipe_clk);
	clk_bulk_disable_unprepare(cfg->num_clks, qmp->clks);

	return 0;
}

static int __maybe_unused qcom_qmp_phy_combo_runtime_resume(struct device *dev)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	struct qmp_phy *qphy = qmp->phys[0];
	const struct qmp_phy_cfg *cfg = qphy->cfg;
	int ret = 0;

	dev_vdbg(dev, "Resuming QMP phy, mode:%d\n", qphy->mode);

	/* Supported only for USB3 PHY and luckily USB3 is the first phy */
	if (cfg->type != PHY_TYPE_USB3)
		return 0;

	if (!qmp->init_count) {
		dev_vdbg(dev, "PHY not initialized, bailing out\n");
		return 0;
	}

	ret = clk_bulk_prepare_enable(cfg->num_clks, qmp->clks);
	if (ret)
		return ret;

	ret = clk_prepare_enable(qphy->pipe_clk);
	if (ret) {
		dev_err(dev, "pipe_clk enable failed, err=%d\n", ret);
		clk_bulk_disable_unprepare(cfg->num_clks, qmp->clks);
		return ret;
	}

	qcom_qmp_phy_combo_disable_autonomous_mode(qphy);

	return 0;
}

static int qcom_qmp_phy_combo_vreg_init(struct device *dev, const struct qmp_phy_cfg *cfg)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int num = cfg->num_vregs;
	int ret, i;

	qmp->vregs = devm_kcalloc(dev, num, sizeof(*qmp->vregs), GFP_KERNEL);
	if (!qmp->vregs)
		return -ENOMEM;

	for (i = 0; i < num; i++)
		qmp->vregs[i].supply = cfg->vreg_list[i].name;

	ret = devm_regulator_bulk_get(dev, num, qmp->vregs);
	if (ret) {
		dev_err(dev, "failed at devm_regulator_bulk_get\n");
		return ret;
	}

	for (i = 0; i < num; i++) {
		ret = regulator_set_load(qmp->vregs[i].consumer,
					cfg->vreg_list[i].enable_load);
		if (ret) {
			dev_err(dev, "failed to set load at %s\n",
				qmp->vregs[i].supply);
			return ret;
		}
	}

	return 0;
}

static int qcom_qmp_phy_combo_reset_init(struct device *dev, const struct qmp_phy_cfg *cfg)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int i;
	int ret;

	qmp->resets = devm_kcalloc(dev, cfg->num_resets,
				   sizeof(*qmp->resets), GFP_KERNEL);
	if (!qmp->resets)
		return -ENOMEM;

	for (i = 0; i < cfg->num_resets; i++)
		qmp->resets[i].id = cfg->reset_list[i];

	ret = devm_reset_control_bulk_get_exclusive(dev, cfg->num_resets, qmp->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get resets\n");

	return 0;
}

static int qcom_qmp_phy_combo_clk_init(struct device *dev, const struct qmp_phy_cfg *cfg)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	int num = cfg->num_clks;
	int i;

	qmp->clks = devm_kcalloc(dev, num, sizeof(*qmp->clks), GFP_KERNEL);
	if (!qmp->clks)
		return -ENOMEM;

	for (i = 0; i < num; i++)
		qmp->clks[i].id = cfg->clk_list[i];

	return devm_clk_bulk_get(dev, num, qmp->clks);
}

static void phy_clk_release_provider(void *res)
{
	of_clk_del_provider(res);
}

/*
 * Register a fixed rate pipe clock.
 *
 * The <s>_pipe_clksrc generated by PHY goes to the GCC that gate
 * controls it. The <s>_pipe_clk coming out of the GCC is requested
 * by the PHY driver for its operations.
 * We register the <s>_pipe_clksrc here. The gcc driver takes care
 * of assigning this <s>_pipe_clksrc as parent to <s>_pipe_clk.
 * Below picture shows this relationship.
 *
 *         +---------------+
 *         |   PHY block   |<<---------------------------------------+
 *         |               |                                         |
 *         |   +-------+   |                   +-----+               |
 *   I/P---^-->|  PLL  |---^--->pipe_clksrc--->| GCC |--->pipe_clk---+
 *    clk  |   +-------+   |                   +-----+
 *         +---------------+
 */
static int phy_pipe_clk_register(struct qcom_qmp *qmp, struct device_node *np)
{
	struct clk_fixed_rate *fixed;
	struct clk_init_data init = { };
	int ret;

	ret = of_property_read_string(np, "clock-output-names", &init.name);
	if (ret) {
		dev_err(qmp->dev, "%pOFn: No clock-output-names\n", np);
		return ret;
	}

	fixed = devm_kzalloc(qmp->dev, sizeof(*fixed), GFP_KERNEL);
	if (!fixed)
		return -ENOMEM;

	init.ops = &clk_fixed_rate_ops;

	/* controllers using QMP phys use 125MHz pipe clock interface */
	fixed->fixed_rate = 125000000;
	fixed->hw.init = &init;

	ret = devm_clk_hw_register(qmp->dev, &fixed->hw);
	if (ret)
		return ret;

	ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, &fixed->hw);
	if (ret)
		return ret;

	/*
	 * Roll a devm action because the clock provider is the child node, but
	 * the child node is not actually a device.
	 */
	return devm_add_action_or_reset(qmp->dev, phy_clk_release_provider, np);
}

/*
 * Display Port PLL driver block diagram for branch clocks
 *
 *              +------------------------------+
 *              |         DP_VCO_CLK           |
 *              |                              |
 *              |    +-------------------+     |
 *              |    |   (DP PLL/VCO)    |     |
 *              |    +---------+---------+     |
 *              |              v               |
 *              |   +----------+-----------+   |
 *              |   | hsclk_divsel_clk_src |   |
 *              |   +----------+-----------+   |
 *              +------------------------------+
 *                              |
 *          +---------<---------v------------>----------+
 *          |                                           |
 * +--------v----------------+                          |
 * |    dp_phy_pll_link_clk  |                          |
 * |     link_clk            |                          |
 * +--------+----------------+                          |
 *          |                                           |
 *          |                                           |
 *          v                                           v
 * Input to DISPCC block                                |
 * for link clk, crypto clk                             |
 * and interface clock                                  |
 *                                                      |
 *                                                      |
 *      +--------<------------+-----------------+---<---+
 *      |                     |                 |
 * +----v---------+  +--------v-----+  +--------v------+
 * | vco_divided  |  | vco_divided  |  | vco_divided   |
 * |    _clk_src  |  |    _clk_src  |  |    _clk_src   |
 * |              |  |              |  |               |
 * |divsel_six    |  |  divsel_two  |  |  divsel_four  |
 * +-------+------+  +-----+--------+  +--------+------+
 *         |                 |                  |
 *         v---->----------v-------------<------v
 *                         |
 *              +----------+-----------------+
 *              |   dp_phy_pll_vco_div_clk   |
 *              +---------+------------------+
 *                        |
 *                        v
 *              Input to DISPCC block
 *              for DP pixel clock
 *
 */
static int qcom_qmp_dp_pixel_clk_determine_rate(struct clk_hw *hw,
						struct clk_rate_request *req)
{
	switch (req->rate) {
	case 1620000000UL / 2:
	case 2700000000UL / 2:
	/* 5.4 and 8.1 GHz are same link rate as 2.7GHz, i.e. div 4 and div 6 */
		return 0;
	default:
		return -EINVAL;
	}
}

static unsigned long
qcom_qmp_dp_pixel_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	const struct qmp_phy_dp_clks *dp_clks;
	const struct qmp_phy *qphy;
	const struct phy_configure_opts_dp *dp_opts;

	dp_clks = container_of(hw, struct qmp_phy_dp_clks, dp_pixel_hw);
	qphy = dp_clks->qphy;
	dp_opts = &qphy->dp_opts;

	switch (dp_opts->link_rate) {
	case 1620:
		return 1620000000UL / 2;
	case 2700:
		return 2700000000UL / 2;
	case 5400:
		return 5400000000UL / 4;
	case 8100:
		return 8100000000UL / 6;
	default:
		return 0;
	}
}

static const struct clk_ops qcom_qmp_dp_pixel_clk_ops = {
	.determine_rate = qcom_qmp_dp_pixel_clk_determine_rate,
	.recalc_rate = qcom_qmp_dp_pixel_clk_recalc_rate,
};

static int qcom_qmp_dp_link_clk_determine_rate(struct clk_hw *hw,
					       struct clk_rate_request *req)
{
	switch (req->rate) {
	case 162000000:
	case 270000000:
	case 540000000:
	case 810000000:
		return 0;
	default:
		return -EINVAL;
	}
}

static unsigned long
qcom_qmp_dp_link_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	const struct qmp_phy_dp_clks *dp_clks;
	const struct qmp_phy *qphy;
	const struct phy_configure_opts_dp *dp_opts;

	dp_clks = container_of(hw, struct qmp_phy_dp_clks, dp_link_hw);
	qphy = dp_clks->qphy;
	dp_opts = &qphy->dp_opts;

	switch (dp_opts->link_rate) {
	case 1620:
	case 2700:
	case 5400:
	case 8100:
		return dp_opts->link_rate * 100000;
	default:
		return 0;
	}
}

static const struct clk_ops qcom_qmp_dp_link_clk_ops = {
	.determine_rate = qcom_qmp_dp_link_clk_determine_rate,
	.recalc_rate = qcom_qmp_dp_link_clk_recalc_rate,
};

static struct clk_hw *
qcom_qmp_dp_clks_hw_get(struct of_phandle_args *clkspec, void *data)
{
	struct qmp_phy_dp_clks *dp_clks = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= 2) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	if (idx == 0)
		return &dp_clks->dp_link_hw;

	return &dp_clks->dp_pixel_hw;
}

static int phy_dp_clks_register(struct qcom_qmp *qmp, struct qmp_phy *qphy,
				struct device_node *np)
{
	struct clk_init_data init = { };
	struct qmp_phy_dp_clks *dp_clks;
	char name[64];
	int ret;

	dp_clks = devm_kzalloc(qmp->dev, sizeof(*dp_clks), GFP_KERNEL);
	if (!dp_clks)
		return -ENOMEM;

	dp_clks->qphy = qphy;
	qphy->dp_clks = dp_clks;

	snprintf(name, sizeof(name), "%s::link_clk", dev_name(qmp->dev));
	init.ops = &qcom_qmp_dp_link_clk_ops;
	init.name = name;
	dp_clks->dp_link_hw.init = &init;
	ret = devm_clk_hw_register(qmp->dev, &dp_clks->dp_link_hw);
	if (ret)
		return ret;

	snprintf(name, sizeof(name), "%s::vco_div_clk", dev_name(qmp->dev));
	init.ops = &qcom_qmp_dp_pixel_clk_ops;
	init.name = name;
	dp_clks->dp_pixel_hw.init = &init;
	ret = devm_clk_hw_register(qmp->dev, &dp_clks->dp_pixel_hw);
	if (ret)
		return ret;

	ret = of_clk_add_hw_provider(np, qcom_qmp_dp_clks_hw_get, dp_clks);
	if (ret)
		return ret;

	/*
	 * Roll a devm action because the clock provider is the child node, but
	 * the child node is not actually a device.
	 */
	return devm_add_action_or_reset(qmp->dev, phy_clk_release_provider, np);
}

static const struct phy_ops qcom_qmp_phy_combo_usb_ops = {
	.init		= qcom_qmp_phy_combo_enable,
	.exit		= qcom_qmp_phy_combo_disable,
	.set_mode	= qcom_qmp_phy_combo_set_mode,
	.owner		= THIS_MODULE,
};

static const struct phy_ops qcom_qmp_phy_combo_dp_ops = {
	.init		= qcom_qmp_phy_combo_init,
	.configure	= qcom_qmp_dp_phy_configure,
	.power_on	= qcom_qmp_phy_combo_power_on,
	.calibrate	= qcom_qmp_dp_phy_calibrate,
	.power_off	= qcom_qmp_phy_combo_power_off,
	.exit		= qcom_qmp_phy_combo_exit,
	.set_mode	= qcom_qmp_phy_combo_set_mode,
	.owner		= THIS_MODULE,
};

static
int qcom_qmp_phy_combo_create(struct device *dev, struct device_node *np, int id,
			void __iomem *serdes, const struct qmp_phy_cfg *cfg)
{
	struct qcom_qmp *qmp = dev_get_drvdata(dev);
	struct phy *generic_phy;
	struct qmp_phy *qphy;
	const struct phy_ops *ops;
	char prop_name[MAX_PROP_NAME];
	int ret;

	qphy = devm_kzalloc(dev, sizeof(*qphy), GFP_KERNEL);
	if (!qphy)
		return -ENOMEM;

	qphy->cfg = cfg;
	qphy->serdes = serdes;
	/*
	 * Get memory resources for each phy lane:
	 * Resources are indexed as: tx -> 0; rx -> 1; pcs -> 2.
	 * For dual lane PHYs: tx2 -> 3, rx2 -> 4, pcs_misc (optional) -> 5
	 * For single lane PHYs: pcs_misc (optional) -> 3.
	 */
	qphy->tx = of_iomap(np, 0);
	if (!qphy->tx)
		return -ENOMEM;

	qphy->rx = of_iomap(np, 1);
	if (!qphy->rx)
		return -ENOMEM;

	qphy->pcs = of_iomap(np, 2);
	if (!qphy->pcs)
		return -ENOMEM;

	if (cfg->pcs_usb_offset)
		qphy->pcs_usb = qphy->pcs + cfg->pcs_usb_offset;

	/*
	 * If this is a dual-lane PHY, then there should be registers for the
	 * second lane. Some old device trees did not specify this, so fall
	 * back to old legacy behavior of assuming they can be reached at an
	 * offset from the first lane.
	 */
	if (cfg->is_dual_lane_phy) {
		qphy->tx2 = of_iomap(np, 3);
		qphy->rx2 = of_iomap(np, 4);
		if (!qphy->tx2 || !qphy->rx2) {
			dev_warn(dev,
				 "Underspecified device tree, falling back to legacy register regions\n");

			/* In the old version, pcs_misc is at index 3. */
			qphy->pcs_misc = qphy->tx2;
			qphy->tx2 = qphy->tx + QMP_PHY_LEGACY_LANE_STRIDE;
			qphy->rx2 = qphy->rx + QMP_PHY_LEGACY_LANE_STRIDE;

		} else {
			qphy->pcs_misc = of_iomap(np, 5);
		}

	} else {
		qphy->pcs_misc = of_iomap(np, 3);
	}

	if (!qphy->pcs_misc)
		dev_vdbg(dev, "PHY pcs_misc-reg not used\n");

	/*
	 * Get PHY's Pipe clock, if any. USB3 and PCIe are PIPE3
	 * based phys, so they essentially have pipe clock. So,
	 * we return error in case phy is USB3 or PIPE type.
	 * Otherwise, we initialize pipe clock to NULL for
	 * all phys that don't need this.
	 */
	snprintf(prop_name, sizeof(prop_name), "pipe%d", id);
	qphy->pipe_clk = devm_get_clk_from_child(dev, np, prop_name);
	if (IS_ERR(qphy->pipe_clk)) {
		if (cfg->type == PHY_TYPE_USB3) {
			ret = PTR_ERR(qphy->pipe_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
					"failed to get lane%d pipe_clk, %d\n",
					id, ret);
			return ret;
		}
		qphy->pipe_clk = NULL;
	}

	if (cfg->type == PHY_TYPE_DP)
		ops = &qcom_qmp_phy_combo_dp_ops;
	else
		ops = &qcom_qmp_phy_combo_usb_ops;

	generic_phy = devm_phy_create(dev, np, ops);
	if (IS_ERR(generic_phy)) {
		ret = PTR_ERR(generic_phy);
		dev_err(dev, "failed to create qphy %d\n", ret);
		return ret;
	}

	qphy->phy = generic_phy;
	qphy->index = id;
	qphy->qmp = qmp;
	qmp->phys[id] = qphy;
	phy_set_drvdata(generic_phy, qphy);

	return 0;
}

static const struct of_device_id qcom_qmp_combo_phy_of_match_table[] = {
	{
		.compatible = "qcom,sc7180-qmp-usb3-dp-phy",
		.data = &sc7180_usb3dpphy_cfg,
	},
	{
		.compatible = "qcom,sm8250-qmp-usb3-dp-phy",
		.data = &sm8250_usb3dpphy_cfg,
	},
	{
		.compatible = "qcom,sc8180x-qmp-usb3-dp-phy",
		.data = &sc8180x_usb3dpphy_cfg,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_qmp_combo_phy_of_match_table);

static const struct dev_pm_ops qcom_qmp_phy_combo_pm_ops = {
	SET_RUNTIME_PM_OPS(qcom_qmp_phy_combo_runtime_suspend,
			   qcom_qmp_phy_combo_runtime_resume, NULL)
};

static int qcom_qmp_phy_combo_probe(struct platform_device *pdev)
{
	struct qcom_qmp *qmp;
	struct device *dev = &pdev->dev;
	struct device_node *child;
	struct phy_provider *phy_provider;
	void __iomem *serdes;
	void __iomem *usb_serdes;
	void __iomem *dp_serdes = NULL;
	const struct qmp_phy_combo_cfg *combo_cfg = NULL;
	const struct qmp_phy_cfg *cfg = NULL;
	const struct qmp_phy_cfg *usb_cfg = NULL;
	const struct qmp_phy_cfg *dp_cfg = NULL;
	int num, id, expected_phys;
	int ret;

	qmp = devm_kzalloc(dev, sizeof(*qmp), GFP_KERNEL);
	if (!qmp)
		return -ENOMEM;

	qmp->dev = dev;
	dev_set_drvdata(dev, qmp);

	/* Get the specific init parameters of QMP phy */
	combo_cfg = of_device_get_match_data(dev);
	if (!combo_cfg)
		return -EINVAL;

	usb_cfg = combo_cfg->usb_cfg;
	cfg = usb_cfg; /* Setup clks and regulators */

	/* per PHY serdes; usually located at base address */
	usb_serdes = serdes = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(serdes))
		return PTR_ERR(serdes);

	/* per PHY dp_com; if PHY has dp_com control block */
	if (cfg->has_phy_dp_com_ctrl) {
		qmp->dp_com = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(qmp->dp_com))
			return PTR_ERR(qmp->dp_com);
	}

	/* Only two serdes for combo PHY */
	dp_serdes = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(dp_serdes))
		return PTR_ERR(dp_serdes);

	dp_cfg = combo_cfg->dp_cfg;
	expected_phys = 2;

	mutex_init(&qmp->phy_mutex);

	ret = qcom_qmp_phy_combo_clk_init(dev, cfg);
	if (ret)
		return ret;

	ret = qcom_qmp_phy_combo_reset_init(dev, cfg);
	if (ret)
		return ret;

	ret = qcom_qmp_phy_combo_vreg_init(dev, cfg);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get regulator supplies: %d\n",
				ret);
		return ret;
	}

	num = of_get_available_child_count(dev->of_node);
	/* do we have a rogue child node ? */
	if (num > expected_phys)
		return -EINVAL;

	qmp->phys = devm_kcalloc(dev, num, sizeof(*qmp->phys), GFP_KERNEL);
	if (!qmp->phys)
		return -ENOMEM;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	/*
	 * Prevent runtime pm from being ON by default. Users can enable
	 * it using power/control in sysfs.
	 */
	pm_runtime_forbid(dev);

	id = 0;
	for_each_available_child_of_node(dev->of_node, child) {
		if (of_node_name_eq(child, "dp-phy")) {
			cfg = dp_cfg;
			serdes = dp_serdes;

			/* Create per-lane phy */
			ret = qcom_qmp_phy_combo_create(dev, child, id, serdes, cfg);
			if (ret) {
				dev_err(dev, "failed to create lane%d phy, %d\n",
					id, ret);
				goto err_node_put;
			}

			ret = phy_dp_clks_register(qmp, qmp->phys[id], child);
			if (ret) {
				dev_err(qmp->dev,
					"failed to register DP clock source\n");
				goto err_node_put;
			}
		} else if (of_node_name_eq(child, "usb3-phy")) {
			cfg = usb_cfg;
			serdes = usb_serdes;

			/* Create per-lane phy */
			ret = qcom_qmp_phy_combo_create(dev, child, id, serdes, cfg);
			if (ret) {
				dev_err(dev, "failed to create lane%d phy, %d\n",
					id, ret);
				goto err_node_put;
			}

			qmp->usb_phy = qmp->phys[id];

			/*
			 * Register the pipe clock provided by phy.
			 * See function description to see details of this pipe clock.
			 */
			ret = phy_pipe_clk_register(qmp, child);
			if (ret) {
				dev_err(qmp->dev,
					"failed to register pipe clock source\n");
				goto err_node_put;
			}
		}

		id++;
	}

	if (!qmp->usb_phy)
		return -EINVAL;

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (!IS_ERR(phy_provider))
		dev_info(dev, "Registered Qcom-QMP phy\n");
	else
		pm_runtime_disable(dev);

	return PTR_ERR_OR_ZERO(phy_provider);

err_node_put:
	pm_runtime_disable(dev);
	of_node_put(child);
	return ret;
}

static struct platform_driver qcom_qmp_phy_combo_driver = {
	.probe		= qcom_qmp_phy_combo_probe,
	.driver = {
		.name	= "qcom-qmp-combo-phy",
		.pm	= &qcom_qmp_phy_combo_pm_ops,
		.of_match_table = qcom_qmp_combo_phy_of_match_table,
	},
};

module_platform_driver(qcom_qmp_phy_combo_driver);

MODULE_AUTHOR("Vivek Gautam <vivek.gautam@codeaurora.org>");
MODULE_DESCRIPTION("Qualcomm QMP USB+DP combo PHY driver");
MODULE_LICENSE("GPL v2");
