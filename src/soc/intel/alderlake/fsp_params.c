/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <cbfs.h>
#include <console/console.h>
#include <device/device.h>
#include <device/pci.h>
#include <fsp/api.h>
#include <fsp/ppi/mp_service_ppi.h>
#include <fsp/util.h>
#include <intelblocks/lpss.h>
#include <intelblocks/xdci.h>
#include <intelpch/lockdown.h>
#include <intelblocks/mp_init.h>
#include <intelblocks/tcss.h>
#include <soc/gpio_soc_defs.h>
#include <soc/intel/common/vbt.h>
#include <soc/pci_devs.h>
#include <soc/pcie.h>
#include <soc/ramstage.h>
#include <soc/soc_chip.h>
#include <string.h>

/* THC assignment definition */
#define THC_NONE	0
#define THC_0		1
#define THC_1		2

/* SATA DEVSLP idle timeout default values */
#define DEF_DMVAL	15
#define DEF_DITOVAL	625

/*
 * Chip config parameter PcieRpL1Substates uses (UPD value + 1)
 * because UPD value of 0 for PcieRpL1Substates means disabled for FSP.
 * In order to ensure that mainboard setting does not disable L1 substates
 * incorrectly, chip config parameter values are offset by 1 with 0 meaning
 * use FSP UPD default. get_l1_substate_control() ensures that the right UPD
 * value is set in fsp_params.
 * 0: Use FSP UPD default
 * 1: Disable L1 substates
 * 2: Use L1.1
 * 3: Use L1.2 (FSP UPD default)
 */
static int get_l1_substate_control(enum L1_substates_control ctl)
{
	if ((ctl > L1_SS_L1_2) || (ctl == L1_SS_FSP_DEFAULT))
		ctl = L1_SS_L1_2;
	return ctl - 1;
}

static void parse_devicetree(FSP_S_CONFIG *params)
{
	const struct soc_intel_alderlake_config *config;
	config = config_of_soc();

	for (int i = 0; i < CONFIG_SOC_INTEL_I2C_DEV_MAX; i++)
		params->SerialIoI2cMode[i] = config->SerialIoI2cMode[i];

	for (int i = 0; i < CONFIG_SOC_INTEL_COMMON_BLOCK_GSPI_MAX; i++) {
		params->SerialIoSpiMode[i] = config->SerialIoGSpiMode[i];
		params->SerialIoSpiCsMode[i] = config->SerialIoGSpiCsMode[i];
		params->SerialIoSpiCsState[i] = config->SerialIoGSpiCsState[i];
	}

	for (int i = 0; i < CONFIG_SOC_INTEL_UART_DEV_MAX; i++)
		params->SerialIoUartMode[i] = config->SerialIoUartMode[i];
}

__weak void mainboard_update_soc_chip_config(struct soc_intel_alderlake_config *config)
{
	/* Override settings per board. */
}

/* UPD parameters to be initialized before SiliconInit */
void platform_fsp_silicon_init_params_cb(FSPS_UPD *supd)
{
	int i;
	const struct microcode *microcode_file;
	size_t microcode_len;
	FSP_S_CONFIG *params = &supd->FspsConfig;
	FSPS_ARCH_UPD *pfsps_arch_upd = &supd->FspsArchUpd;
	uint32_t enable_mask;

	struct device *dev;
	struct soc_intel_alderlake_config *config;
	config = config_of_soc();
	mainboard_update_soc_chip_config(config);

	/* Parse device tree and enable/disable Serial I/O devices */
	parse_devicetree(params);

	microcode_file = cbfs_map("cpu_microcode_blob.bin", &microcode_len);

	if ((microcode_file != NULL) && (microcode_len != 0)) {
		/* Update CPU Microcode patch base address/size */
		params->MicrocodeRegionBase = (uint32_t)microcode_file;
		params->MicrocodeRegionSize = (uint32_t)microcode_len;
	}

	/* Load VBT before devicetree-specific config. */
	params->GraphicsConfigPtr = (uintptr_t)vbt_get();

	/* Check if IGD is present and fill Graphics init param accordingly */
	params->PeiGraphicsPeimInit = CONFIG(RUN_FSP_GOP) && is_devfn_enabled(SA_DEVFN_IGD);
	params->LidStatus = CONFIG(RUN_FSP_GOP);

	/* Use coreboot MP PPI services if Kconfig is enabled */
	if (CONFIG(USE_INTEL_FSP_TO_CALL_COREBOOT_PUBLISH_MP_PPI))
		params->CpuMpPpi = (uintptr_t) mp_fill_ppi_services_data();

	/* D3Hot and D3Cold for TCSS */
	params->D3HotEnable = !config->TcssD3HotDisable;
	params->D3ColdEnable = !config->TcssD3ColdDisable;

	params->TcssAuxOri = config->TcssAuxOri;

	/* Explicitly clear this field to avoid using defaults */
	memset(params->IomTypeCPortPadCfg, 0, sizeof(params->IomTypeCPortPadCfg));

	/*
	 * Set FSPS UPD ITbtConnectTopologyTimeoutInMs with value 0. FSP will
	 * evaluate this UPD value and skip sending command. There will be no
	 * delay for command completion.
	 */
	params->ITbtConnectTopologyTimeoutInMs = 0;

	/* Chipset Lockdown */
	if (get_lockdown_config() == CHIPSET_LOCKDOWN_COREBOOT) {
		params->PchLockDownGlobalSmi = 0;
		params->PchLockDownBiosInterface = 0;
		params->PchUnlockGpioPads = 1;
		params->RtcMemoryLock = 0;
	} else {
		params->PchLockDownGlobalSmi = 1;
		params->PchLockDownBiosInterface = 1;
		params->PchUnlockGpioPads = 0;
		params->RtcMemoryLock = 1;
	}

	/* USB */
	for (i = 0; i < ARRAY_SIZE(config->usb2_ports); i++) {
		params->PortUsb20Enable[i] = config->usb2_ports[i].enable;
		params->Usb2PhyPetxiset[i] = config->usb2_ports[i].pre_emp_bias;
		params->Usb2PhyTxiset[i] = config->usb2_ports[i].tx_bias;
		params->Usb2PhyPredeemp[i] = config->usb2_ports[i].tx_emp_enable;
		params->Usb2PhyPehalfbit[i] = config->usb2_ports[i].pre_emp_bit;

		if (config->usb2_ports[i].enable)
			params->Usb2OverCurrentPin[i] = config->usb2_ports[i].ocpin;
		else
			params->Usb2OverCurrentPin[i] = OC_SKIP;
	}

	for (i = 0; i < ARRAY_SIZE(config->usb3_ports); i++) {
		params->PortUsb30Enable[i] = config->usb3_ports[i].enable;
		if (config->usb3_ports[i].enable)
			params->Usb3OverCurrentPin[i] = config->usb3_ports[i].ocpin;
		else
			params->Usb3OverCurrentPin[i] = OC_SKIP;

		if (config->usb3_ports[i].tx_de_emp) {
			params->Usb3HsioTxDeEmphEnable[i] = 1;
			params->Usb3HsioTxDeEmph[i] = config->usb3_ports[i].tx_de_emp;
		}
		if (config->usb3_ports[i].tx_downscale_amp) {
			params->Usb3HsioTxDownscaleAmpEnable[i] = 1;
			params->Usb3HsioTxDownscaleAmp[i] =
				config->usb3_ports[i].tx_downscale_amp;
		}
	}

	for (i = 0; i < ARRAY_SIZE(config->tcss_ports); i++) {
		if (config->tcss_ports[i].enable)
			params->CpuUsb3OverCurrentPin[i] = config->tcss_ports[i].ocpin;
	}

	/* EnableMultiPhaseSiliconInit for running MultiPhaseSiInit */
	pfsps_arch_upd->EnableMultiPhaseSiliconInit = 1;

	/* Enable xDCI controller if enabled in devicetree and allowed */
	dev = pcidev_path_on_root(PCH_DEVFN_USBOTG);
	if (dev) {
		if (!xdci_can_enable())
			dev->enabled = 0;
		params->XdciEnable = dev->enabled;
	} else {
		params->XdciEnable = 0;
	}

	/* PCH UART selection for FSP Debug */
	params->SerialIoDebugUartNumber = CONFIG_UART_FOR_CONSOLE;
	ASSERT(ARRAY_SIZE(params->SerialIoUartAutoFlow) > CONFIG_UART_FOR_CONSOLE);
	params->SerialIoUartAutoFlow[CONFIG_UART_FOR_CONSOLE] = 0;

	/* SATA */
	params->SataEnable = is_devfn_enabled(PCH_DEVFN_SATA);
	if (params->SataEnable) {
		params->SataMode = config->SataMode;
		params->SataSalpSupport = config->SataSalpSupport;
		memcpy(params->SataPortsEnable, config->SataPortsEnable,
			sizeof(params->SataPortsEnable));
		memcpy(params->SataPortsDevSlp, config->SataPortsDevSlp,
			sizeof(params->SataPortsDevSlp));
	}

	/*
	 * Power Optimizer for DMI and SATA.
	 * DmiPwrOptimizeDisable and SataPwrOptimizeDisable is default to 0.
	 * Boards not needing the optimizers explicitly disables them by setting
	 * these disable variables to 1 in devicetree overrides.
	 */
	params->PchPwrOptEnable = !(config->DmiPwrOptimizeDisable);
	params->SataPwrOptEnable = !(config->SataPwrOptimizeDisable);

	/*
	 *  Enable DEVSLP Idle Timeout settings DmVal and DitoVal.
	 *  SataPortsDmVal is the DITO multiplier. Default is 15.
	 *  SataPortsDitoVal is the DEVSLP Idle Timeout (DITO), Default is 625ms.
	 *  The default values can be changed from devicetree.
	 */
	for (i = 0; i < ARRAY_SIZE(config->SataPortsEnableDitoConfig); i++) {
		if (config->SataPortsEnableDitoConfig[i]) {
			params->SataPortsDmVal[i] = config->SataPortsDmVal[i];
			params->SataPortsDitoVal[i] = config->SataPortsDitoVal[i];
		}
	}

	/* Enable TCPU for processor thermal control */
	params->Device4Enable = is_devfn_enabled(SA_DEVFN_DPTF);

	/* Set TccActivationOffset */
	params->TccActivationOffset = config->tcc_offset;

	/* LAN */
	params->PchLanEnable = is_devfn_enabled(PCH_DEVFN_GBE);

	/* CNVi */
	params->CnviMode = is_devfn_enabled(PCH_DEVFN_CNVI_WIFI);
	params->CnviBtCore = config->CnviBtCore;
	params->CnviBtAudioOffload = config->CnviBtAudioOffload;
	/* Assert if CNVi BT is enabled without CNVi being enabled. */
	assert(params->CnviMode || !params->CnviBtCore);
	/* Assert if CNVi BT offload is enabled without CNVi BT being enabled. */
	assert(params->CnviBtCore || !params->CnviBtAudioOffload);

	/* VMD */
	params->VmdEnable = is_devfn_enabled(SA_DEVFN_VMD);

	/* THC */
	params->ThcPort0Assignment = is_devfn_enabled(PCH_DEVFN_THC0) ? THC_0 : THC_NONE;
	params->ThcPort1Assignment = is_devfn_enabled(PCH_DEVFN_THC1) ? THC_1 : THC_NONE;

	/* USB4/TBT */
	for (i = 0; i < ARRAY_SIZE(params->ITbtPcieRootPortEn); i++)
		params->ITbtPcieRootPortEn[i] = is_devfn_enabled(SA_DEVFN_TBT(i));

	/* Legacy 8254 timer support */
	params->Enable8254ClockGating = !CONFIG(USE_LEGACY_8254_TIMER);
	params->Enable8254ClockGatingOnS3 = !CONFIG(USE_LEGACY_8254_TIMER);

	/* Enable Hybrid storage auto detection */
	params->HybridStorageMode = config->HybridStorageMode;

	enable_mask = pcie_rp_enable_mask(get_pch_pcie_rp_table());
	for (i = 0; i < CONFIG_MAX_PCH_ROOT_PORTS; i++) {
		if (!(enable_mask & BIT(i)))
			continue;
		const struct pcie_rp_config *rp_cfg = &config->pch_pcie_rp[i];
		params->PcieRpL1Substates[i] =
				get_l1_substate_control(rp_cfg->PcieRpL1Substates);
		params->PcieRpLtrEnable[i] = !!(rp_cfg->flags & PCIE_RP_LTR);
		params->PcieRpAdvancedErrorReporting[i] = !!(rp_cfg->flags & PCIE_RP_AER);
		params->PcieRpHotPlug[i] = !!(rp_cfg->flags & PCIE_RP_HOTPLUG);
		params->PcieRpClkReqDetect[i] = !!(rp_cfg->flags & PCIE_RP_CLK_REQ_DETECT);
	}

	params->PmSupport = 1;
	params->Hwp = 1;
	params->Cx = 1;
	params->PsOnEnable = 1;

	mainboard_silicon_init_params(params);
}

/*
 * Callbacks for SoC/Mainboard specific overrides for FspMultiPhaseSiInit
 * This platform supports below MultiPhaseSIInit Phase(s):
 * Phase   |  FSP return point                                |  Purpose
 * ------- + ------------------------------------------------ + -------------------------------
 *   1     |  After TCSS initialization completed             |  for TCSS specific init
 */
void platform_fsp_multi_phase_init_cb(uint32_t phase_index)
{
	switch (phase_index) {
	case 1:
		/* TCSS specific initialization here */
		printk(BIOS_DEBUG, "FSP MultiPhaseSiInit %s/%s called\n",
			__FILE__, __func__);

		if (CONFIG(SOC_INTEL_COMMON_BLOCK_TCSS)) {
			const config_t *config = config_of_soc();
			tcss_configure(config->typec_aux_bias_pads);
		}
		break;
	default:
		break;
	}
}

/* Mainboard GPIO Configuration */
__weak void mainboard_silicon_init_params(FSP_S_CONFIG *params)
{
	printk(BIOS_DEBUG, "WEAK: %s/%s called\n", __FILE__, __func__);
}
