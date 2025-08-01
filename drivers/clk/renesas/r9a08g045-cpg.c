// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G3S CPG driver
 *
 * Copyright (C) 2023 Renesas Electronics Corp.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pm_domain.h>

#include <dt-bindings/clock/r9a08g045-cpg.h>

#include "rzg2l-cpg.h"

/* RZ/G3S Specific registers. */
#define G3S_CPG_PL2_DDIV		(0x204)
#define G3S_CPG_SDHI_DDIV		(0x218)
#define G3S_CPG_PLL_DSEL		(0x240)
#define G3S_CPG_SDHI_DSEL		(0x244)
#define G3S_CLKDIVSTATUS		(0x280)
#define G3S_CLKSELSTATUS		(0x284)

/* RZ/G3S Specific division configuration.  */
#define G3S_DIVPL2B		DDIV_PACK(G3S_CPG_PL2_DDIV, 4, 3)
#define G3S_DIV_SDHI0		DDIV_PACK(G3S_CPG_SDHI_DDIV, 0, 1)
#define G3S_DIV_SDHI1		DDIV_PACK(G3S_CPG_SDHI_DDIV, 4, 1)
#define G3S_DIV_SDHI2		DDIV_PACK(G3S_CPG_SDHI_DDIV, 8, 1)

/* RZ/G3S Clock status configuration. */
#define G3S_DIVPL1A_STS		DDIV_PACK(G3S_CLKDIVSTATUS, 0, 1)
#define G3S_DIVPL2B_STS		DDIV_PACK(G3S_CLKDIVSTATUS, 5, 1)
#define G3S_DIVPL3A_STS		DDIV_PACK(G3S_CLKDIVSTATUS, 8, 1)
#define G3S_DIVPL3B_STS		DDIV_PACK(G3S_CLKDIVSTATUS, 9, 1)
#define G3S_DIVPL3C_STS		DDIV_PACK(G3S_CLKDIVSTATUS, 10, 1)
#define G3S_DIV_SDHI0_STS	DDIV_PACK(G3S_CLKDIVSTATUS, 24, 1)
#define G3S_DIV_SDHI1_STS	DDIV_PACK(G3S_CLKDIVSTATUS, 25, 1)
#define G3S_DIV_SDHI2_STS	DDIV_PACK(G3S_CLKDIVSTATUS, 26, 1)

#define G3S_SEL_PLL4_STS	SEL_PLL_PACK(G3S_CLKSELSTATUS, 6, 1)
#define G3S_SEL_SDHI0_STS	SEL_PLL_PACK(G3S_CLKSELSTATUS, 16, 1)
#define G3S_SEL_SDHI1_STS	SEL_PLL_PACK(G3S_CLKSELSTATUS, 17, 1)
#define G3S_SEL_SDHI2_STS	SEL_PLL_PACK(G3S_CLKSELSTATUS, 18, 1)

/* RZ/G3S Specific clocks select. */
#define G3S_SEL_PLL4		SEL_PLL_PACK(G3S_CPG_PLL_DSEL, 6, 1)
#define G3S_SEL_SDHI0		SEL_PLL_PACK(G3S_CPG_SDHI_DSEL, 0, 2)
#define G3S_SEL_SDHI1		SEL_PLL_PACK(G3S_CPG_SDHI_DSEL, 4, 2)
#define G3S_SEL_SDHI2		SEL_PLL_PACK(G3S_CPG_SDHI_DSEL, 8, 2)

/* PLL 1/4/6 configuration registers macro. */
#define G3S_PLL146_CONF(clk1, clk2, setting)	((clk1) << 22 | (clk2) << 12 | (setting))

#define DEF_G3S_MUX(_name, _id, _conf, _parent_names, _mux_flags, _clk_flags) \
	DEF_TYPE(_name, _id, CLK_TYPE_MUX, .conf = (_conf), \
		 .parent_names = (_parent_names), \
		 .num_parents = ARRAY_SIZE((_parent_names)), \
		 .mux_flags = CLK_MUX_HIWORD_MASK | (_mux_flags), \
		 .flag = (_clk_flags))

enum clk_ids {
	/* Core Clock Outputs exported to DT */
	LAST_DT_CORE_CLK = R9A08G045_SWD,

	/* External Input Clocks */
	CLK_EXTAL,

	/* Internal Core Clocks */
	CLK_OSC_DIV1000,
	CLK_PLL1,
	CLK_PLL2,
	CLK_PLL2_DIV2,
	CLK_PLL2_DIV2_8,
	CLK_PLL2_DIV6,
	CLK_PLL3,
	CLK_PLL3_DIV2,
	CLK_PLL3_DIV2_4,
	CLK_PLL3_DIV2_8,
	CLK_PLL3_DIV6,
	CLK_PLL4,
	CLK_PLL6,
	CLK_PLL6_DIV2,
	CLK_SEL_SDHI0,
	CLK_SEL_SDHI1,
	CLK_SEL_SDHI2,
	CLK_SEL_PLL4,
	CLK_P1_DIV2,
	CLK_P3_DIV2,
	CLK_SD0_DIV4,
	CLK_SD1_DIV4,
	CLK_SD2_DIV4,

	/* Module Clocks */
	MOD_CLK_BASE,
};

/* Divider tables */
static const struct clk_div_table dtable_1_2[] = {
	{ 0, 1 },
	{ 1, 2 },
	{ 0, 0 },
};

static const struct clk_div_table dtable_1_8[] = {
	{ 0, 1 },
	{ 1, 2 },
	{ 2, 4 },
	{ 3, 8 },
	{ 0, 0 },
};

static const struct clk_div_table dtable_1_32[] = {
	{ 0, 1 },
	{ 1, 2 },
	{ 2, 4 },
	{ 3, 8 },
	{ 4, 32 },
	{ 0, 0 },
};

/* Mux clock names tables. */
static const char * const sel_sdhi[] = { ".pll2_div2", ".pll6", ".pll2_div6" };
static const char * const sel_pll4[] = { ".osc_div1000", ".pll4" };

/* Mux clock indices tables. */
static const u32 mtable_sd[] = { 0, 2, 3 };
static const u32 mtable_pll4[] = { 0, 1 };

static const struct cpg_core_clk r9a08g045_core_clks[] __initconst = {
	/* External Clock Inputs */
	DEF_INPUT("extal", CLK_EXTAL),

	/* Internal Core Clocks */
	DEF_FIXED(".osc_div1000", CLK_OSC_DIV1000, CLK_EXTAL, 1, 1000),
	DEF_G3S_PLL(".pll1", CLK_PLL1, CLK_EXTAL, G3S_PLL146_CONF(0x4, 0x8, 0x100),
		    1100000000UL),
	DEF_FIXED(".pll2", CLK_PLL2, CLK_EXTAL, 200, 3),
	DEF_FIXED(".pll3", CLK_PLL3, CLK_EXTAL, 200, 3),
	DEF_FIXED(".pll4", CLK_PLL4, CLK_EXTAL, 100, 3),
	DEF_FIXED(".pll6", CLK_PLL6, CLK_EXTAL, 125, 6),
	DEF_FIXED(".pll2_div2", CLK_PLL2_DIV2, CLK_PLL2, 1, 2),
	DEF_FIXED(".pll2_div2_8", CLK_PLL2_DIV2_8, CLK_PLL2_DIV2, 1, 8),
	DEF_FIXED(".pll2_div6", CLK_PLL2_DIV6, CLK_PLL2, 1, 6),
	DEF_FIXED(".pll3_div2", CLK_PLL3_DIV2, CLK_PLL3, 1, 2),
	DEF_FIXED(".pll3_div2_4", CLK_PLL3_DIV2_4, CLK_PLL3_DIV2, 1, 4),
	DEF_FIXED(".pll3_div2_8", CLK_PLL3_DIV2_8, CLK_PLL3_DIV2, 1, 8),
	DEF_FIXED(".pll3_div6", CLK_PLL3_DIV6, CLK_PLL3, 1, 6),
	DEF_FIXED(".pll6_div2", CLK_PLL6_DIV2, CLK_PLL6, 1, 2),
	DEF_SD_MUX(".sel_sd0", CLK_SEL_SDHI0, G3S_SEL_SDHI0, G3S_SEL_SDHI0_STS, sel_sdhi,
		   mtable_sd, 0, NULL),
	DEF_SD_MUX(".sel_sd1", CLK_SEL_SDHI1, G3S_SEL_SDHI1, G3S_SEL_SDHI1_STS, sel_sdhi,
		   mtable_sd, 0, NULL),
	DEF_SD_MUX(".sel_sd2", CLK_SEL_SDHI2, G3S_SEL_SDHI2, G3S_SEL_SDHI2_STS, sel_sdhi,
		   mtable_sd, 0, NULL),
	DEF_SD_MUX(".sel_pll4", CLK_SEL_PLL4, G3S_SEL_PLL4, G3S_SEL_PLL4_STS, sel_pll4,
		   mtable_pll4, CLK_SET_PARENT_GATE, NULL),

	/* Core output clk */
	DEF_G3S_DIV("I", R9A08G045_CLK_I, CLK_PLL1, DIVPL1A, G3S_DIVPL1A_STS, dtable_1_8,
		    0, 0, 0, NULL),
	DEF_G3S_DIV("P0", R9A08G045_CLK_P0, CLK_PLL2_DIV2_8, G3S_DIVPL2B, G3S_DIVPL2B_STS,
		    dtable_1_32, 0, 0, 0, NULL),
	DEF_G3S_DIV("SD0", R9A08G045_CLK_SD0, CLK_SEL_SDHI0, G3S_DIV_SDHI0, G3S_DIV_SDHI0_STS,
		    dtable_1_2, 800000000UL, 500000000UL, CLK_SET_RATE_PARENT,
		    rzg3s_cpg_div_clk_notifier),
	DEF_G3S_DIV("SD1", R9A08G045_CLK_SD1, CLK_SEL_SDHI1, G3S_DIV_SDHI1, G3S_DIV_SDHI1_STS,
		    dtable_1_2, 800000000UL, 500000000UL, CLK_SET_RATE_PARENT,
		    rzg3s_cpg_div_clk_notifier),
	DEF_G3S_DIV("SD2", R9A08G045_CLK_SD2, CLK_SEL_SDHI2, G3S_DIV_SDHI2, G3S_DIV_SDHI2_STS,
		    dtable_1_2, 800000000UL, 500000000UL, CLK_SET_RATE_PARENT,
		    rzg3s_cpg_div_clk_notifier),
	DEF_FIXED(".sd0_div4", CLK_SD0_DIV4, R9A08G045_CLK_SD0, 1, 4),
	DEF_FIXED(".sd1_div4", CLK_SD1_DIV4, R9A08G045_CLK_SD1, 1, 4),
	DEF_FIXED(".sd2_div4", CLK_SD2_DIV4, R9A08G045_CLK_SD2, 1, 4),
	DEF_FIXED("M0", R9A08G045_CLK_M0, CLK_PLL3_DIV2_4, 1, 1),
	DEF_G3S_DIV("P1", R9A08G045_CLK_P1, CLK_PLL3_DIV2_4, DIVPL3A, G3S_DIVPL3A_STS,
		    dtable_1_32, 0, 0, 0, NULL),
	DEF_FIXED("P1_DIV2", CLK_P1_DIV2, R9A08G045_CLK_P1, 1, 2),
	DEF_G3S_DIV("P2", R9A08G045_CLK_P2, CLK_PLL3_DIV2_8, DIVPL3B, G3S_DIVPL3B_STS,
		    dtable_1_32, 0, 0, 0, NULL),
	DEF_G3S_DIV("P3", R9A08G045_CLK_P3, CLK_PLL3_DIV2_4, DIVPL3C, G3S_DIVPL3C_STS,
		    dtable_1_32, 0, 0, 0, NULL),
	DEF_FIXED("P3_DIV2", CLK_P3_DIV2, R9A08G045_CLK_P3, 1, 2),
	DEF_FIXED("ZT", R9A08G045_CLK_ZT, CLK_PLL3_DIV2_8, 1, 1),
	DEF_FIXED("S0", R9A08G045_CLK_S0, CLK_SEL_PLL4, 1, 2),
	DEF_FIXED("OSC", R9A08G045_OSCCLK, CLK_EXTAL, 1, 1),
	DEF_FIXED("OSC2", R9A08G045_OSCCLK2, CLK_EXTAL, 1, 3),
	DEF_FIXED("HP", R9A08G045_CLK_HP, CLK_PLL6, 1, 2),
	DEF_FIXED("TSU", R9A08G045_CLK_TSU, CLK_PLL2_DIV2, 1, 8),
};

static const struct rzg2l_mod_clk r9a08g045_mod_clks[] = {
	DEF_MOD("gic_gicclk",		R9A08G045_GIC600_GICCLK, R9A08G045_CLK_P1, 0x514, 0,
					MSTOP(BUS_ACPU, BIT(3))),
	DEF_MOD("ia55_pclk",		R9A08G045_IA55_PCLK, R9A08G045_CLK_P2, 0x518, 0,
					MSTOP(BUS_PERI_CPU, BIT(13))),
	DEF_MOD("ia55_clk",		R9A08G045_IA55_CLK, R9A08G045_CLK_P1, 0x518, 1,
					MSTOP(BUS_PERI_CPU, BIT(13))),
	DEF_MOD("dmac_aclk",		R9A08G045_DMAC_ACLK, R9A08G045_CLK_P3, 0x52c, 0,
					MSTOP(BUS_REG1, BIT(2))),
	DEF_MOD("dmac_pclk",		R9A08G045_DMAC_PCLK, CLK_P3_DIV2, 0x52c, 1,
					MSTOP(BUS_REG1, BIT(3))),
	DEF_MOD("wdt0_pclk",		R9A08G045_WDT0_PCLK, R9A08G045_CLK_P0, 0x548, 0,
					MSTOP(BUS_REG0, BIT(0))),
	DEF_MOD("wdt0_clk",		R9A08G045_WDT0_CLK, R9A08G045_OSCCLK, 0x548, 1,
					MSTOP(BUS_REG0, BIT(0))),
	DEF_MOD("sdhi0_imclk",		R9A08G045_SDHI0_IMCLK, CLK_SD0_DIV4, 0x554, 0,
					MSTOP(BUS_PERI_COM, BIT(0))),
	DEF_MOD("sdhi0_imclk2",		R9A08G045_SDHI0_IMCLK2, CLK_SD0_DIV4, 0x554, 1,
					MSTOP(BUS_PERI_COM, BIT(0))),
	DEF_MOD("sdhi0_clk_hs",		R9A08G045_SDHI0_CLK_HS, R9A08G045_CLK_SD0, 0x554, 2,
					MSTOP(BUS_PERI_COM, BIT(0))),
	DEF_MOD("sdhi0_aclk",		R9A08G045_SDHI0_ACLK, R9A08G045_CLK_P1, 0x554, 3,
					MSTOP(BUS_PERI_COM, BIT(0))),
	DEF_MOD("sdhi1_imclk",		R9A08G045_SDHI1_IMCLK, CLK_SD1_DIV4, 0x554, 4,
					MSTOP(BUS_PERI_COM, BIT(1))),
	DEF_MOD("sdhi1_imclk2",		R9A08G045_SDHI1_IMCLK2, CLK_SD1_DIV4, 0x554, 5,
					MSTOP(BUS_PERI_COM, BIT(1))),
	DEF_MOD("sdhi1_clk_hs",		R9A08G045_SDHI1_CLK_HS, R9A08G045_CLK_SD1, 0x554, 6,
					MSTOP(BUS_PERI_COM, BIT(1))),
	DEF_MOD("sdhi1_aclk",		R9A08G045_SDHI1_ACLK, R9A08G045_CLK_P1, 0x554, 7,
					MSTOP(BUS_PERI_COM, BIT(1))),
	DEF_MOD("sdhi2_imclk",		R9A08G045_SDHI2_IMCLK, CLK_SD2_DIV4, 0x554, 8,
					MSTOP(BUS_PERI_COM, BIT(11))),
	DEF_MOD("sdhi2_imclk2",		R9A08G045_SDHI2_IMCLK2, CLK_SD2_DIV4, 0x554, 9,
					MSTOP(BUS_PERI_COM, BIT(11))),
	DEF_MOD("sdhi2_clk_hs",		R9A08G045_SDHI2_CLK_HS, R9A08G045_CLK_SD2, 0x554, 10,
					MSTOP(BUS_PERI_COM, BIT(11))),
	DEF_MOD("sdhi2_aclk",		R9A08G045_SDHI2_ACLK, R9A08G045_CLK_P1, 0x554, 11,
					MSTOP(BUS_PERI_COM, BIT(11))),
	DEF_MOD("ssi0_pclk2",		R9A08G045_SSI0_PCLK2, R9A08G045_CLK_P0, 0x570, 0,
					MSTOP(BUS_MCPU1, BIT(10))),
	DEF_MOD("ssi0_sfr",		R9A08G045_SSI0_PCLK_SFR, R9A08G045_CLK_P0, 0x570, 1,
					MSTOP(BUS_MCPU1, BIT(10))),
	DEF_MOD("ssi1_pclk2",		R9A08G045_SSI1_PCLK2, R9A08G045_CLK_P0, 0x570, 2,
					MSTOP(BUS_MCPU1, BIT(11))),
	DEF_MOD("ssi1_sfr",		R9A08G045_SSI1_PCLK_SFR, R9A08G045_CLK_P0, 0x570, 3,
					MSTOP(BUS_MCPU1, BIT(11))),
	DEF_MOD("ssi2_pclk2",		R9A08G045_SSI2_PCLK2, R9A08G045_CLK_P0, 0x570, 4,
					MSTOP(BUS_MCPU1, BIT(12))),
	DEF_MOD("ssi2_sfr",		R9A08G045_SSI2_PCLK_SFR, R9A08G045_CLK_P0, 0x570, 5,
					MSTOP(BUS_MCPU1, BIT(12))),
	DEF_MOD("ssi3_pclk2",		R9A08G045_SSI3_PCLK2, R9A08G045_CLK_P0, 0x570, 6,
					MSTOP(BUS_MCPU1, BIT(13))),
	DEF_MOD("ssi3_sfr",		R9A08G045_SSI3_PCLK_SFR, R9A08G045_CLK_P0, 0x570, 7,
					MSTOP(BUS_MCPU1, BIT(13))),
	DEF_MOD("usb0_host",		R9A08G045_USB_U2H0_HCLK, R9A08G045_CLK_P1, 0x578, 0,
					MSTOP(BUS_PERI_COM, BIT(5))),
	DEF_MOD("usb1_host",		R9A08G045_USB_U2H1_HCLK, R9A08G045_CLK_P1, 0x578, 1,
					MSTOP(BUS_PERI_COM, BIT(7))),
	DEF_MOD("usb0_func",		R9A08G045_USB_U2P_EXR_CPUCLK, R9A08G045_CLK_P1, 0x578, 2,
					MSTOP(BUS_PERI_COM, BIT(6))),
	DEF_MOD("usb_pclk",		R9A08G045_USB_PCLK, R9A08G045_CLK_P1, 0x578, 3,
					MSTOP(BUS_PERI_COM, BIT(4))),
	DEF_COUPLED("eth0_axi",		R9A08G045_ETH0_CLK_AXI, R9A08G045_CLK_M0, 0x57c, 0,
					MSTOP(BUS_PERI_COM, BIT(2))),
	DEF_COUPLED("eth0_chi",		R9A08G045_ETH0_CLK_CHI, R9A08G045_CLK_ZT, 0x57c, 0,
					MSTOP(BUS_PERI_COM, BIT(2))),
	DEF_MOD("eth0_refclk",		R9A08G045_ETH0_REFCLK, R9A08G045_CLK_HP, 0x57c, 8, 0),
	DEF_COUPLED("eth1_axi",		R9A08G045_ETH1_CLK_AXI, R9A08G045_CLK_M0, 0x57c, 1,
					MSTOP(BUS_PERI_COM, BIT(3))),
	DEF_COUPLED("eth1_chi",		R9A08G045_ETH1_CLK_CHI, R9A08G045_CLK_ZT, 0x57c, 1,
					MSTOP(BUS_PERI_COM, BIT(3))),
	DEF_MOD("eth1_refclk",		R9A08G045_ETH1_REFCLK, R9A08G045_CLK_HP, 0x57c, 9, 0),
	DEF_MOD("i2c0_pclk",		R9A08G045_I2C0_PCLK, R9A08G045_CLK_P0, 0x580, 0,
					MSTOP(BUS_MCPU2, BIT(10))),
	DEF_MOD("i2c1_pclk",		R9A08G045_I2C1_PCLK, R9A08G045_CLK_P0, 0x580, 1,
					MSTOP(BUS_MCPU2, BIT(11))),
	DEF_MOD("i2c2_pclk",		R9A08G045_I2C2_PCLK, R9A08G045_CLK_P0, 0x580, 2,
					MSTOP(BUS_MCPU2, BIT(12))),
	DEF_MOD("i2c3_pclk",		R9A08G045_I2C3_PCLK, R9A08G045_CLK_P0, 0x580, 3,
					MSTOP(BUS_MCPU2, BIT(13))),
	DEF_MOD("scif0_clk_pck",	R9A08G045_SCIF0_CLK_PCK, R9A08G045_CLK_P0, 0x584, 0,
					MSTOP(BUS_MCPU2, BIT(1))),
	DEF_MOD("scif1_clk_pck",	R9A08G045_SCIF1_CLK_PCK, R9A08G045_CLK_P0, 0x584, 1,
					MSTOP(BUS_MCPU2, BIT(2))),
	DEF_MOD("scif2_clk_pck",	R9A08G045_SCIF2_CLK_PCK, R9A08G045_CLK_P0, 0x584, 2,
					MSTOP(BUS_MCPU2, BIT(3))),
	DEF_MOD("scif3_clk_pck",	R9A08G045_SCIF3_CLK_PCK, R9A08G045_CLK_P0, 0x584, 3,
					MSTOP(BUS_MCPU2, BIT(4))),
	DEF_MOD("scif4_clk_pck",	R9A08G045_SCIF4_CLK_PCK, R9A08G045_CLK_P0, 0x584, 4,
					MSTOP(BUS_MCPU2, BIT(5))),
	DEF_MOD("scif5_clk_pck",	R9A08G045_SCIF5_CLK_PCK, R9A08G045_CLK_P0, 0x584, 5,
					MSTOP(BUS_MCPU3, BIT(4))),
	DEF_MOD("gpio_hclk",		R9A08G045_GPIO_HCLK, R9A08G045_OSCCLK, 0x598, 0, 0),
	DEF_MOD("adc_adclk",		R9A08G045_ADC_ADCLK, R9A08G045_CLK_TSU, 0x5a8, 0,
					MSTOP(BUS_MCPU2, BIT(14))),
	DEF_MOD("adc_pclk",		R9A08G045_ADC_PCLK, R9A08G045_CLK_TSU, 0x5a8, 1,
					MSTOP(BUS_MCPU2, BIT(14))),
	DEF_MOD("tsu_pclk",		R9A08G045_TSU_PCLK, R9A08G045_CLK_TSU, 0x5ac, 0,
					MSTOP(BUS_MCPU2, BIT(15))),
	DEF_MOD("vbat_bclk",		R9A08G045_VBAT_BCLK, R9A08G045_OSCCLK, 0x614, 0,
					MSTOP(BUS_MCPU3, GENMASK(8, 7))),
};

static const struct rzg2l_reset r9a08g045_resets[] = {
	DEF_RST(R9A08G045_GIC600_GICRESET_N, 0x814, 0),
	DEF_RST(R9A08G045_GIC600_DBG_GICRESET_N, 0x814, 1),
	DEF_RST(R9A08G045_IA55_RESETN, 0x818, 0),
	DEF_RST(R9A08G045_DMAC_ARESETN, 0x82c, 0),
	DEF_RST(R9A08G045_DMAC_RST_ASYNC, 0x82c, 1),
	DEF_RST(R9A08G045_WDT0_PRESETN, 0x848, 0),
	DEF_RST(R9A08G045_SDHI0_IXRST, 0x854, 0),
	DEF_RST(R9A08G045_SDHI1_IXRST, 0x854, 1),
	DEF_RST(R9A08G045_SDHI2_IXRST, 0x854, 2),
	DEF_RST(R9A08G045_SSI0_RST_M2_REG, 0x870, 0),
	DEF_RST(R9A08G045_SSI1_RST_M2_REG, 0x870, 1),
	DEF_RST(R9A08G045_SSI2_RST_M2_REG, 0x870, 2),
	DEF_RST(R9A08G045_SSI3_RST_M2_REG, 0x870, 3),
	DEF_RST(R9A08G045_USB_U2H0_HRESETN, 0x878, 0),
	DEF_RST(R9A08G045_USB_U2H1_HRESETN, 0x878, 1),
	DEF_RST(R9A08G045_USB_U2P_EXL_SYSRST, 0x878, 2),
	DEF_RST(R9A08G045_USB_PRESETN, 0x878, 3),
	DEF_RST(R9A08G045_ETH0_RST_HW_N, 0x87c, 0),
	DEF_RST(R9A08G045_ETH1_RST_HW_N, 0x87c, 1),
	DEF_RST(R9A08G045_I2C0_MRST, 0x880, 0),
	DEF_RST(R9A08G045_I2C1_MRST, 0x880, 1),
	DEF_RST(R9A08G045_I2C2_MRST, 0x880, 2),
	DEF_RST(R9A08G045_I2C3_MRST, 0x880, 3),
	DEF_RST(R9A08G045_SCIF0_RST_SYSTEM_N, 0x884, 0),
	DEF_RST(R9A08G045_SCIF1_RST_SYSTEM_N, 0x884, 1),
	DEF_RST(R9A08G045_SCIF2_RST_SYSTEM_N, 0x884, 2),
	DEF_RST(R9A08G045_SCIF3_RST_SYSTEM_N, 0x884, 3),
	DEF_RST(R9A08G045_SCIF4_RST_SYSTEM_N, 0x884, 4),
	DEF_RST(R9A08G045_SCIF5_RST_SYSTEM_N, 0x884, 5),
	DEF_RST(R9A08G045_GPIO_RSTN, 0x898, 0),
	DEF_RST(R9A08G045_GPIO_PORT_RESETN, 0x898, 1),
	DEF_RST(R9A08G045_GPIO_SPARE_RESETN, 0x898, 2),
	DEF_RST(R9A08G045_ADC_PRESETN, 0x8a8, 0),
	DEF_RST(R9A08G045_ADC_ADRST_N, 0x8a8, 1),
	DEF_RST(R9A08G045_TSU_PRESETN, 0x8ac, 0),
	DEF_RST(R9A08G045_VBAT_BRESETN, 0x914, 0),
};

static const unsigned int r9a08g045_crit_mod_clks[] __initconst = {
	MOD_CLK_BASE + R9A08G045_GIC600_GICCLK,
	MOD_CLK_BASE + R9A08G045_IA55_PCLK,
	MOD_CLK_BASE + R9A08G045_IA55_CLK,
	MOD_CLK_BASE + R9A08G045_DMAC_ACLK,
	MOD_CLK_BASE + R9A08G045_VBAT_BCLK,
};

const struct rzg2l_cpg_info r9a08g045_cpg_info = {
	/* Core Clocks */
	.core_clks = r9a08g045_core_clks,
	.num_core_clks = ARRAY_SIZE(r9a08g045_core_clks),
	.last_dt_core_clk = LAST_DT_CORE_CLK,
	.num_total_core_clks = MOD_CLK_BASE,

	/* Critical Module Clocks */
	.crit_mod_clks = r9a08g045_crit_mod_clks,
	.num_crit_mod_clks = ARRAY_SIZE(r9a08g045_crit_mod_clks),

	/* Module Clocks */
	.mod_clks = r9a08g045_mod_clks,
	.num_mod_clks = ARRAY_SIZE(r9a08g045_mod_clks),
	.num_hw_mod_clks = R9A08G045_VBAT_BCLK + 1,

	/* Resets */
	.resets = r9a08g045_resets,
	.num_resets = R9A08G045_VBAT_BRESETN + 1, /* Last reset ID + 1 */

	.has_clk_mon_regs = true,
};
