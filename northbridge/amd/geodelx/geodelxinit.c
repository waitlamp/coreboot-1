/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2007-2008 Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <types.h>
#include <console.h>
#include <msr.h>
#include <cpu.h>
#include <amd_geodelx.h>
#include "geodelink.h"


static struct msrinit clock_gating_default[] = {
	{GLIU0_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0005}},
	{MC_GLD_MSR_PM,		{.hi = 0x00,.lo = 0x0001}},
	{VG_GLD_MSR_PM,		{.hi = 0x00,.lo = 0x0015}},
	{GP_GLD_MSR_PM,		{.hi = 0x00,.lo = 0x0001}},
	{DF_GLD_MSR_PM,		{.hi = 0x00,.lo = 0x0555}},
	{GLIU1_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0005}},
	{GLCP_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0014}},
	{GLPCI_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0015}},
	{VIP_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0005}},
	{AES_GLD_MSR_PM,	{.hi = 0x00,.lo = 0x0015}},
	{CPU_BC_PMODE_MSR,	{.hi = 0x00,.lo = 0x70303}}, // TODO: Correct?
	{0xffffffff,		{0xffffffff, 0xffffffff}},
};

/** GeodeLink priority table. */
static struct msrinit geode_link_priority_table[] = {
	{CPU_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0220}},
	{DF_GLD_MSR_MASTER_CONF,	{.hi = 0x00,.lo = 0x0000}},
	{VG_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0720}},
	{GP_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0010}},
	{GLPCI_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0017}},
	{GLCP_GLD_MSR_CONF,		{.hi = 0x00,.lo = 0x0001}},
	{VIP_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0622}},
	{AES_GLD_MSR_CONFIG,		{.hi = 0x00,.lo = 0x0013}},
	{0x0FFFFFFFF,			{0x0FFFFFFFF, 0x0FFFFFFFF}},
};

/**
 * Write a GeodeLink MSR.
 *
 * @param gl A GeodeLink table descriptor.
 */
static void writeglmsr(const struct gliutable *gl)
{
	struct msr msr;

	msr.lo = gl->lo;
	msr.hi = gl->hi;
	wrmsr(gl->desc_name, msr);
	printk(BIOS_SPEW, "%s: MSR 0x%08lx, val 0x%08x:0x%08x\n",
	       __FUNCTION__, gl->desc_name, msr.hi, msr.lo);
}

/**
 * Read the MSR specified in the gl struct. If the low 32 bits are zero,
 * indicating it has not been set, set it.
 *
 * @param gl A GeodeLink table descriptor.
 */
static void ShadowInit(const struct gliutable *gl)
{
	struct msr msr;

	msr = rdmsr(gl->desc_name);
	if (msr.lo == 0)
		writeglmsr(gl);
}

/**
 * Size up ram.
 *
 * All we need to do here is read the MSR for DRAM and grab out the sizing
 * bits. Note that this code depends on initram having run. It uses the MSRs,
 * not the SPDs, and the MSRs of course are set up by initram.
 *
 * @return TODO
 */
int sizeram(void)
{
	struct msr msr;
	int sizem = 0;
	u32 dimm;

	/* Get the RAM size from the memory controller as calculated
	 * and set by auto_size_dimm().
	 */
	msr = rdmsr(MC_CF07_DATA);
	printk(BIOS_DEBUG, "sizeram: _MSR MC_CF07_DATA: %08x:%08x\n", msr.hi,
	       msr.lo);

	/* DIMM 0 */
	dimm = msr.hi;
	/* Installed? */
	if ((dimm & 7) != 7) {
		/* 1:8MB, 2:16MB, 3:32MB, 4:64MB, ... 7:512MB, 8:1GB */
		sizem = 4 << ((dimm >> 12) & 0x0F);
	}

	/* DIMM 1 */
	dimm = msr.hi >> 16;
	/* Installed? */
	if ((dimm & 7) != 7) {
		/* 1:8MB, 2:16MB, 3:32MB, 4:64MB, ... 7:512MB, 8:1GB */
		sizem += 4 << ((dimm >> 12) & 0x0F);
	}

	printk(BIOS_DEBUG, "sizeram: sizem 0x%xMB\n", sizem);

	return sizem;
}

/**
 * Set up the system memory registers, i.e. memory that can be used
 * for non-VSM (or SMM) purposes. 
 *
 * @param gl A GeodeLink table descriptor.
 */
static void sysmem_init(const struct gliutable *gl)
{
	struct msr msr;
	int sizembytes, sizebytes;

	/* Figure out how much RAM is in the machine and allocate all to the
	 * system. We will adjust for SMM now and Frame Buffer later.
	 */
	sizembytes = sizeram();
	printk(BIOS_DEBUG, "%s: enable for %dMBytes\n",
	       __FUNCTION__, sizembytes);
	sizebytes = sizembytes << 20;

	sizebytes -= ((SMM_SIZE * 1024) + 1);
	printk(BIOS_DEBUG, "Usable RAM: %d bytes\n", sizebytes);

	/* 20 bit address. The bottom 12 bits go into bits 20-31 in msr.lo.
	 * The top 8 bits go into 0-7 of msr.hi.
	 */
	sizebytes--;
	msr.hi = (gl->hi & 0xFFFFFF00) | (sizebytes >> 24);
	sizebytes <<= 8;	/* Move bits 23:12 in bits 31:20. */
	sizebytes &= 0xfff00000;
	sizebytes |= 0x100;	/* Start at 1 MB. */
	msr.lo = sizebytes;

	wrmsr(gl->desc_name, msr);
	printk(BIOS_DEBUG, "%s: MSR 0x%08lx, val 0x%08x:0x%08x\n", __FUNCTION__,
	       gl->desc_name, msr.hi, msr.lo);
}

/**
 * Set up GL0 memory mapping. Again, SMM memory is subtracted. 
 *
 * @param gl A GeodeLink table descriptor.
 */
static void SMMGL0Init(const struct gliutable *gl)
{
	struct msr msr;
	int sizebytes = sizeram() << 20;
	long offset;

	sizebytes -= (SMM_SIZE * 1024);

	printk(BIOS_DEBUG, "%s: %d bytes\n", __FUNCTION__, sizebytes);

	/* Calculate the "two's complement" offset. */
	offset = sizebytes - SMM_OFFSET;
	offset = (offset >> 12) & 0x000fffff;
	printk(BIOS_DEBUG, "%s: offset is 0x%08x\n", __FUNCTION__, SMM_OFFSET);

	msr.hi = offset << 8 | gl->hi;
	msr.hi |= SMM_OFFSET >> 24;

	msr.lo = SMM_OFFSET << 8;
	msr.lo |= ((~(SMM_SIZE * 1024) + 1) >> 12) & 0xfffff;

	wrmsr(gl->desc_name, msr);
	printk(BIOS_DEBUG, "%s: MSR 0x%08lx, val 0x%08x:0x%08x\n", __FUNCTION__,
	       gl->desc_name, msr.hi, msr.lo);
}

/**
 * Set up GL1 memory mapping. Again, SMM memory is subtracted. 
 *
 * @param gl A GeodeLink table descriptor.
 */
static void SMMGL1Init(const struct gliutable *gl)
{
	struct msr msr;
	printk(BIOS_DEBUG, "%s:\n", __FUNCTION__);

	msr.hi = gl->hi;
	/* I don't think this is needed. */
	msr.hi &= 0xffffff00;
	msr.hi |= (SMM_OFFSET >> 24);
	msr.lo = (SMM_OFFSET << 8) & 0xFFF00000;
	msr.lo |= ((~(SMM_SIZE * 1024) + 1) >> 12) & 0xfffff;

	wrmsr(gl->desc_name, msr);
	printk(BIOS_DEBUG, "%s: MSR 0x%08lx, val 0x%08x:0x%08x\n", __FUNCTION__,
	       gl->desc_name, msr.hi, msr.lo);
}

/**
 * Set up all GeodeLink interfaces. Iterate over the table until done.
 *
 * Case out on the link type, and call the appropriate function.
 *
 * @param gl A GeodeLink table descriptor.
 */
static void GLIUInit(const struct gliutable *gl)
{
	while (gl->desc_type != GL_END) {
		switch (gl->desc_type) {
		default:
			/* For unknown types: Write then read MSR. */
			writeglmsr(gl);
		case SC_SHADOW:	/* Check for a Shadow entry. */
			ShadowInit(gl);
			break;
		case R_SYSMEM:	/* Check for a SYSMEM entry. */
			sysmem_init(gl);
			break;
		case BMO_SMM:	/* Check for a SMM entry. */
			SMMGL0Init(gl);
			break;
		case BM_SMM:	/* Check for a SMM entry. */
			SMMGL1Init(gl);
			break;
		}
		gl++;
	}
}

/**
 * Set up the region config registers for the GeodeLink PCI interface.
 *
 * R0: 0 - 640 KB
 * R1: 1 MB - Top of System Memory
 * R2: SMM Memory
 * R3: Framebuffer? - not set up yet.
 */
static void GLPCI_init(void)
{
	const struct gliutable *gl = NULL;
	struct msr msr;
	int i, enable_preempt, enable_cpu_override;
	int nic_grants_control, enable_bus_parking;
	unsigned long pah, pal;

	/* R0 - GLPCI settings for Conventional Memory space. */
	msr.hi = (0x09F000 >> 12) << GLPCI_RC_UPPER_TOP_SHIFT;	/* 640 */
	msr.lo = 0;						/* 0 */
	msr.lo |= GLPCI_RC_LOWER_EN_SET + GLPCI_RC_LOWER_PF_SET +
		  GLPCI_RC_LOWER_WC_SET;
	wrmsr(GLPCI_RC0, msr);

	/* R1 - GLPCI settings for SysMem space. */
	/* Get systop from GLIU0 SYSTOP Descriptor. */
	for (i = 0; gliu0table[i].desc_name != GL_END; i++) {
		if (gliu0table[i].desc_type == R_SYSMEM) {
			gl = &gliu0table[i];
			break;
		}
	}

	if (gl) {
		msr = rdmsr(gl->desc_name);

		/* Example: R_SYSMEM value 20:00:00:0f:fb:f0:01:00
		 * translates to a base of 0x00100000 and top of 0xffbf0000
		 * base of 1M and top of around 256M.
		 */
		/* we have to create a page-aligned (4KB page) address
		 * for base and top.
		 * So we need a high page aligned addresss (pah) and
		 * low page aligned address (pal) pah is from msr.hi
		 * << 12 | msr.low >> 20. pal is msr.lo << 12
		 */
		pah = ((msr.hi & 0xFF) << 12) | ((msr.lo >> 20) & 0xFFF);

		/* We have the page address. Now make it page-aligned. */
		pah <<= 12;

		pal = msr.lo << 12;
		msr.hi = pah;
		msr.lo = pal;
		msr.lo |= GLPCI_RC_LOWER_EN_SET | GLPCI_RC_LOWER_PF_SET |
			  GLPCI_RC_LOWER_WC_SET;
		printk(BIOS_DEBUG,
		       "GLPCI R1: system msr.lo 0x%08x msr.hi 0x%08x\n",
		       msr.lo, msr.hi);
		wrmsr(GLPCI_RC1, msr);
	}

	/* R2 - GLPCI settings for SMM space. */
	msr.hi = ((SMM_OFFSET +
		 (SMM_SIZE * 1024 - 1)) >> 12) << GLPCI_RC_UPPER_TOP_SHIFT;
	msr.lo = (SMM_OFFSET >> 12) << GLPCI_RC_LOWER_BASE_SHIFT;
	msr.lo |= GLPCI_RC_LOWER_EN_SET | GLPCI_RC_LOWER_PF_SET;
	printk(BIOS_DEBUG, "GLPCI R2: system msr.lo 0x%08x msr.hi 0x%08x\n",
	       msr.lo, msr.hi);
	wrmsr(GLPCI_RC2, msr);

	/* This is done elsewhere already, but it does no harm to do
	 * it more than once.
	 */
	/* Write serialize memory hole to PCI. Need to to unWS when
	 * something is shadowed regardless of cachablility.
	 */
	msr.lo = 0x021212121;	/* Cache disabled and write serialized. */
	msr.hi = 0x021212121;	/* Cache disabled and write serialized. */
	wrmsr(CPU_RCONF_A0_BF, msr);
	wrmsr(CPU_RCONF_C0_DF, msr);
	wrmsr(CPU_RCONF_E0_FF, msr);

	/* Set Non-Cacheable Read Only for NorthBound Transactions to
	 * Memory. The Enable bit is handled in the Shadow setup.
	 */
	msr.lo = 0x35353535;
	msr.hi = 0x35353535;
	wrmsr(GLPCI_A0_BF, msr);
	wrmsr(GLPCI_C0_DF, msr);
	wrmsr(GLPCI_E0_FF, msr);

	/* Set WSREQ. */
	msr = rdmsr(CPU_DM_CONFIG0);
	msr.hi &= ~(7 << DM_CONFIG0_UPPER_WSREQ_SHIFT);
	/* Reduce to 1 for safe mode. */
	msr.hi |= 2 << DM_CONFIG0_UPPER_WSREQ_SHIFT;
	wrmsr(CPU_DM_CONFIG0, msr);

	/* The following settings will not work with a CS5530 southbridge.
	 * We are ignoring the CS5530 case for now, and perhaps forever.
	 */

	/* 553x NB Init */

	/* Arbiter setup */
	enable_preempt = GLPCI_ARB_LOWER_PRE0_SET | GLPCI_ARB_LOWER_PRE1_SET |
			 GLPCI_ARB_LOWER_PRE2_SET | GLPCI_ARB_LOWER_CPRE_SET;
	enable_cpu_override = GLPCI_ARB_LOWER_COV_SET;
	enable_bus_parking = GLPCI_ARB_LOWER_PARK_SET;
	nic_grants_control = (0x4 << GLPCI_ARB_UPPER_R2_SHIFT) |
			     (0x3 << GLPCI_ARB_UPPER_H2_SHIFT);

	msr = rdmsr(GLPCI_ARB);
	msr.hi |= nic_grants_control;
	msr.lo |= enable_cpu_override | enable_preempt | enable_bus_parking;
	wrmsr(GLPCI_ARB, msr);

	msr = rdmsr(GLPCI_CTRL);
	/* Out will be disabled in CPUBUG649 for < 2.0 parts. */
	msr.lo |= GLPCI_CTRL_LOWER_ME_SET | GLPCI_CTRL_LOWER_OWC_SET |
		  GLPCI_CTRL_LOWER_PCD_SET | GLPCI_CTRL_LOWER_LDE_SET;

	msr.lo &= ~(0x03 << GLPCI_CTRL_LOWER_IRFC_SHIFT);
	msr.lo |= 0x02 << GLPCI_CTRL_LOWER_IRFC_SHIFT;

	msr.lo &= ~(0x07 << GLPCI_CTRL_LOWER_IRFT_SHIFT);
	msr.lo |= 0x06 << GLPCI_CTRL_LOWER_IRFT_SHIFT;

	msr.hi &= ~(0x0f << GLPCI_CTRL_UPPER_FTH_SHIFT);
	msr.hi |= 0x0F << GLPCI_CTRL_UPPER_FTH_SHIFT;

	msr.hi &= ~(0x0f << GLPCI_CTRL_UPPER_RTH_SHIFT);
	msr.hi |= 0x0F << GLPCI_CTRL_UPPER_RTH_SHIFT;

	msr.hi &= ~(0x0f << GLPCI_CTRL_UPPER_SBRTH_SHIFT);
	msr.hi |= 0x0F << GLPCI_CTRL_UPPER_SBRTH_SHIFT;

	msr.hi &= ~(0x03 << GLPCI_CTRL_UPPER_WTO_SHIFT);
	msr.hi |= 0x06 << GLPCI_CTRL_UPPER_WTO_SHIFT;

	msr.hi &= ~(0x03 << GLPCI_CTRL_UPPER_ILTO_SHIFT);
	msr.hi |= 0x00 << GLPCI_CTRL_UPPER_ILTO_SHIFT;
	wrmsr(GLPCI_CTRL, msr);

	/* Set GLPCI Latency Timer */
	msr = rdmsr(GLPCI_CTRL);
	/* Change once 1.x is gone. */
	msr.hi |= 0x1F << GLPCI_CTRL_UPPER_LAT_SHIFT;
	wrmsr(GLPCI_CTRL, msr);

	/* GLPCI_SPARE */
	msr = rdmsr(GLPCI_SPARE);
	msr.lo &= ~0x7;
	msr.lo |= GLPCI_SPARE_LOWER_AILTO_SET | GLPCI_SPARE_LOWER_PPD_SET |
		  GLPCI_SPARE_LOWER_PPC_SET | GLPCI_SPARE_LOWER_MPC_SET |
		  GLPCI_SPARE_LOWER_NSE_SET | GLPCI_SPARE_LOWER_SUPO_SET;
	wrmsr(GLPCI_SPARE, msr);
}

/**
 * Enable Clock Gating in ALL MSRs which relate to clocks.
 */
static void clock_gating_init(void)
{
	struct msr msr;
	struct msrinit *gating = clock_gating_default;

	for (; gating->msrnum != 0xffffffff; gating++) {
		msr = rdmsr(gating->msrnum);
		msr.hi |= gating->msr.hi;
		msr.lo |= gating->msr.lo;
		wrmsr(gating->msrnum, msr);
	}
}

/**
 * Set all GeodeLink priority registers as determined by the TODO.
 */
static void geode_link_priority(void)
{
	struct msr msr;
	struct msrinit *prio = geode_link_priority_table;

	for (; prio->msrnum != 0xffffffff; prio++) {
		msr = rdmsr(prio->msrnum);
		msr.hi |= prio->msr.hi;
		msr.lo &= ~0xfff;
		msr.lo |= prio->msr.lo;
		wrmsr(prio->msrnum, msr);
	}
}

/**
 * Get the GLIU0 shadow register settings.
 *
 * If the set_shadow() function is used then all shadow descriptors
 * will stay sync'ed.
 *
 * @return TODO
 */
static u64 get_shadow(void)
{
	struct msr msr;

	msr = rdmsr(MSR_GLIU0_SHADOW);
	return (((u64) msr.hi) << 32) | msr.lo;
}

/**
 * Set the cache RConf registers for the memory hole.
 *
 * Keeps all cache shadow descriptors sync'ed.
 * This is part of the PCI lockup solution.
 *
 * @param shadowHi The high 32 bits of the msr setting.
 * @param shadowLo The low 32 bits of the msr setting.
 */
static void set_shadowRCONF(u32 shadowHi, u32 shadowLo)
{
	/* Ok, this is whacky bit translation time. */
	int bit;
	u8 shadowByte;
	struct msr msr = { 0, 0 };

	shadowByte = (u8) (shadowLo >> 16);

	/* Load up D000 settings in edx. */
	for (bit = 8; (bit > 4); bit--) {
		msr.hi <<= 8;
		msr.hi |= 1;	/* Cache disable PCI/Shadow memory. */
		if (shadowByte && (1 << bit))
			msr.hi |= 0x20;	/* Write serialize PCI memory. */
	}

	/* Load up C000 settings in eax. */
	for (; bit; bit--) {
		msr.lo <<= 8;
		msr.lo |= 1;	/* Cache disable PCI/Shadow memory. */
		if (shadowByte && (1 << bit))
			msr.lo |= 0x20;	/* Write serialize PCI memory. */
	}

	wrmsr(CPU_RCONF_C0_DF, msr);

	shadowByte = (u8) (shadowLo >> 24);

	/* Load up F000 settings in edx. */
	for (bit = 8; (bit > 4); bit--) {
		msr.hi <<= 8;
		msr.hi |= 1;	/* Cache disable PCI/Shadow memory. */
		if (shadowByte && (1 << bit))
			msr.hi |= 0x20;	/* Write serialize PCI memory. */
	}

	/* Load up E000 settings in eax. */
	for (; bit; bit--) {
		msr.lo <<= 8;
		msr.lo |= 1;	/* Cache disable PCI/Shadow memory. */
		if (shadowByte && (1 << bit))
			msr.lo |= 0x20;	/* write serialize PCI memory. */
	}

	wrmsr(CPU_RCONF_E0_FF, msr);
}

/**
 * Set the GLPCI registers for the memory hole.
 *
 * Keeps all cache shadow descriptors sync'ed.
 *
 * @param shadowhi The high 32 bits of the msr setting.
 * @param shadowlo The low 32 bits of the msr setting.
 */
static void set_shadowGLPCI(u32 shadowhi, u32 shadowlo)
{
	struct msr msr;

	/* Set the Enable register. */
	msr = rdmsr(GLPCI_REN);
	msr.lo &= 0xFFFF00FF;
	msr.lo |= ((shadowlo & 0xFFFF0000) >> 8);
	wrmsr(GLPCI_REN, msr);
}

/**
 * Set the GLIU SC register settings.
 *
 * Scans descriptor tables for SC_SHADOW.
 * Keeps all shadow descriptors sync'ed.
 *
 * @param shadowSettings Shadow register settings.
 */
static void set_shadow(u64 shadowSettings)
{
	int i;
	struct msr msr;
	const struct gliutable *pTable;
	u32 shadowLo, shadowHi;

	shadowLo = (u32) shadowSettings;
	shadowHi = (u32) (shadowSettings >> 32);

	set_shadowRCONF(shadowHi, shadowLo);
	set_shadowGLPCI(shadowHi, shadowLo);

	for (i = 0; gliutables[i]; i++) {
		for (pTable = gliutables[i]; pTable->desc_type != GL_END;
		     pTable++) {
			if (pTable->desc_type == SC_SHADOW) {
				msr = rdmsr(pTable->desc_name);
				msr.lo = (u32) shadowSettings;
				/* Maintain PDID in upper EDX. */
				msr.hi &= 0xFFFF0000;
				msr.hi |=
				    ((u32) (shadowSettings >> 32)) & 0x0000FFFF;
				wrmsr(pTable->desc_name, msr);
			}
		}
	}
}

/**
 * TODO.
 */
static void rom_shadow_settings(void)
{
	u64 shadowSettings = get_shadow();

	/* Disable read & writes. */
	shadowSettings &= (u64) 0xFFFF00000000FFFFULL;

	/* Enable reads for F0000-FFFFF. */
	shadowSettings |= (u64) 0x00000000F0000000ULL;

	/* Enable read & writes for C0000-CFFFF. */
	shadowSettings |= (u64) 0x0000FFFFFFFF0000ULL;

	set_shadow(shadowSettings);
}

/**
 * Set up RCONF_DEFAULT and any other RCONF registers needed.
 *
 * DEVRC_RCONF_DEFAULT:
 * ROMRC(63:56)   = 0x04     Write protect ROMBASE
 * ROMBASE(36:55) = 0x0FFFC0 Top of PCI/bottom of ROM chipselect area
 * DEVRC(35:28)   = 0x39     Cache disabled in PCI memory + WS bit on
 *                           Write Combine + write burst.
 * SYSTOP(27:8)   = top of system memory
 * SYSRC(7:0)     = 0        Writeback, can set to 0x08 to make writethrough
 */
#define SYSMEM_RCONF_WRITETHROUGH	8
#define DEVRC_RCONF_DEFAULT		0x21
#define ROMBASE_RCONF_DEFAULT		0xFFFC0000
#define ROMRC_RCONF_SAFE		0x25
#define ROMRC_RCONF_DEFAULT		0x04

/**
 * TODO.
 */
static void enable_L1_cache(void)
{
	int i;
	const struct gliutable *gl = NULL;
	struct msr msr;
	u8 SysMemCacheProp;

	/* Locate SYSMEM entry in GLIU0table. */
	for (i = 0; gliu0table[i].desc_name != GL_END; i++) {
		if (gliu0table[i].desc_type == R_SYSMEM) {
			gl = &gliu0table[i];
			break;
		}
	}
	if (gl == 0) {
		post_code(POST_RCONFInitError);
		while (1);	/* TODO: Should be hlt()? */
	}

	msr = rdmsr(gl->desc_name);

	/* 20 bit address - The bottom 12 bits go into bits 20-31 in eax, the
	 * top 8 bits go into 0-7 of edx.
	 */
	msr.lo = (msr.lo & 0xFFFFFF00) | (msr.hi & 0xFF);
	msr.lo = ((msr.lo << 12) | (msr.lo >> 20)) & 0x000FFFFF;
	msr.lo <<= RCONF_DEFAULT_LOWER_SYSTOP_SHIFT;	// 8

	/* Set Default SYSMEM region properties.
	 * NOT writethrough == writeback 8 (or ~8)
	 */
	msr.lo &= ~SYSMEM_RCONF_WRITETHROUGH;

	/* Set PCI space cache properties.
	 * Setting is split between hi and lo...
	 */
	msr.hi = (DEVRC_RCONF_DEFAULT >> 4);
	msr.lo |= (DEVRC_RCONF_DEFAULT << 28);

	/* Set the ROMBASE. This is usually 0xFFFC0000. */
	msr.hi |=
	    (ROMBASE_RCONF_DEFAULT >> 12) << RCONF_DEFAULT_UPPER_ROMBASE_SHIFT;

	/* Set ROMBASE cache properties. */
	msr.hi |= ((ROMRC_RCONF_DEFAULT >> 8) | (ROMRC_RCONF_DEFAULT << 24));

	/* Now program RCONF_DEFAULT. */
	wrmsr(CPU_RCONF_DEFAULT, msr);
	printk(BIOS_DEBUG, "CPU_RCONF_DEFAULT (1808): 0x%08X:0x%08X\n", msr.hi,
	       msr.lo);

	/* RCONF_BYPASS: Cache tablewalk properties and SMM/DMM header access
	 * properties. Set to match system memory cache properties.
	 */
	msr = rdmsr(CPU_RCONF_DEFAULT);
	SysMemCacheProp = (u8) (msr.lo & 0xFF);
	msr = rdmsr(CPU_RCONF_BYPASS);
	msr.lo =
	    (msr.lo & 0xFFFF0000) | (SysMemCacheProp << 8) | SysMemCacheProp;
	wrmsr(CPU_RCONF_BYPASS, msr);

	printk(BIOS_DEBUG, "CPU_RCONF_BYPASS (180A): 0x%08x : 0x%08x\n",
	       msr.hi, msr.lo);
}

/**
 * Enable the L2 cache MSRs.
 */
static void enable_L2_cache(void)
{
	struct msr msr;

	/* Instruction Memory Configuration register
	 * set EBE bit, required when L2 cache is enabled.
	 */
	msr = rdmsr(CPU_IM_CONFIG);
	msr.lo |= 0x400;
	wrmsr(CPU_IM_CONFIG, msr);

	/* Data Memory Subsystem Configuration register. Set EVCTONRPL bit,
	 * required when L2 cache is enabled in victim mode.
	 */
	msr = rdmsr(CPU_DM_CONFIG0);
	msr.lo |= 0x4000;
	wrmsr(CPU_DM_CONFIG0, msr);

	/* Invalidate L2 cache. */
	msr.hi = 0x00;
	msr.lo = 0x10;
	wrmsr(CPU_BC_L2_CONF, msr);

	/* Enable L2 cache. */
	msr.hi = 0x00;
	msr.lo = 0x0f;
	wrmsr(CPU_BC_L2_CONF, msr);

	printk(BIOS_DEBUG, "L2 cache enabled\n");
}


/**
 * Set up all LX cache registers, L1, L2, and x86.
 */
static void setup_lx_cache(void)
{
	struct msr msr;

	enable_L1_cache();
	enable_L2_cache();

	/* Make sure all INVD instructions are treated as WBINVD. We do this
	 * because we've found some programs which require this behavior.
	 */
	msr = rdmsr(CPU_DM_CONFIG0);
	msr.lo |= DM_CONFIG0_LOWER_WBINVD_SET;
	wrmsr(CPU_DM_CONFIG0, msr);

	enable_cache();
	__asm__("wbinvd\n");	/* TODO: Use wbinvd() function? */
}

/**
 * Do all the Nasty Bits that have to happen.
 *
 * These can be done once memory is up, but before much else is done.
 * So we do them in phase 2.
 */
void northbridge_init_early(void)
{
	int i;

	printk(BIOS_DEBUG, "Enter %s\n", __FUNCTION__);

	for (i = 0; gliutables[i]; i++)
		GLIUInit(gliutables[i]);

	/* Now that the descriptor to memory is set up, the memory controller
	 * needs one read to synch it's lines before it can be used.
	 */
	i = *(int *)0;

	geode_link_priority();
	setup_lx_cache();
	rom_shadow_settings();
	GLPCI_init();
	clock_gating_init();

	__asm__ __volatile__("FINIT\n"); /* TODO: Create finit() function? */

	printk(BIOS_DEBUG, "Exit %s\n", __FUNCTION__);
}

void geode_pre_payload(void)
{
	struct msr msr;

	/* Set ROM cache properties for runtime. */
	msr = rdmsr(CPU_RCONF_DEFAULT);
	msr.hi &= ~(0xFF << 24);        	// clear ROMRC
	msr.hi |= ROMRC_RCONF_SAFE << 24;	// set WS, CD, WP
	wrmsr(CPU_RCONF_DEFAULT, msr);
}

