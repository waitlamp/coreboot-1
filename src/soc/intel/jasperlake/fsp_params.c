/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <console/console.h>
#include <device/device.h>
#include <fsp/api.h>
#include <fsp/ppi/mp_service_ppi.h>
#include <fsp/util.h>
#include <intelblocks/lpss.h>
#include <intelblocks/mp_init.h>
#include <intelblocks/pmclib.h>
#include <intelblocks/xdci.h>
#include <intelpch/lockdown.h>
#include <soc/intel/common/vbt.h>
#include <soc/pci_devs.h>
#include <soc/ramstage.h>
#include <soc/soc_chip.h>
#include <string.h>

/*
 * ME End of Post configuration
 * 0 - Disable EOP.
 * 1 - Send in PEI (Applicable for FSP in API mode)
 * 2 - Send in DXE (Not applicable for FSP in API mode)
 */
enum {
	EOP_DISABLE,
	EOP_PEI,
	EOP_DXE,
} EndOfPost;

static void parse_devicetree(FSP_S_CONFIG *params)
{
	const struct soc_intel_jasperlake_config *config = config_of_soc();

	/* LPSS controllers configuration */

	/* I2C */
	FSP_ARRAY_LOAD(params->SerialIoI2cMode, config->SerialIoI2cMode);

	/* GSPI */
	FSP_ARRAY_LOAD(params->SerialIoSpiMode, config->SerialIoGSpiMode);
	FSP_ARRAY_LOAD(params->SerialIoSpiCsMode, config->SerialIoGSpiCsMode);
	FSP_ARRAY_LOAD(params->SerialIoSpiCsState, config->SerialIoGSpiCsState);

	/* UART */
	FSP_ARRAY_LOAD(params->SerialIoUartMode, config->SerialIoUartMode);
}

/* UPD parameters to be initialized before SiliconInit */
void platform_fsp_silicon_init_params_cb(FSPS_UPD *supd)
{
	unsigned int i;
	struct device *dev;
	FSP_S_CONFIG *params = &supd->FspsConfig;
	struct soc_intel_jasperlake_config *config = config_of_soc();

	/* Parse device tree and fill in FSP UPDs */
	parse_devicetree(params);

	/* Load VBT before devicetree-specific config. */
	params->GraphicsConfigPtr = (uintptr_t)vbt_get();

	/* Check if IGD is present and fill Graphics init param accordingly */
	params->PeiGraphicsPeimInit = CONFIG(RUN_FSP_GOP) && is_devfn_enabled(SA_DEVFN_IGD);

	params->PavpEnable = CONFIG(PAVP);

	/* Use coreboot MP PPI services if Kconfig is enabled */
	if (CONFIG(USE_INTEL_FSP_TO_CALL_COREBOOT_PUBLISH_MP_PPI))
		params->CpuMpPpi = (uintptr_t) mp_fill_ppi_services_data();

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

	/* Enable End of Post in PEI phase */
	params->EndOfPostMessage = EOP_PEI;

	/* Legacy 8254 timer support */
	params->Enable8254ClockGating = !CONFIG(USE_LEGACY_8254_TIMER);
	params->Enable8254ClockGatingOnS3 = 1;

	/* disable Legacy PME */
	memset(params->PcieRpPmSci, 0, sizeof(params->PcieRpPmSci));

	/* Enable ClkReqDetect for enabled port */
	memcpy(params->PcieRpClkReqDetect, config->PcieRpClkReqDetect,
		sizeof(config->PcieRpClkReqDetect));

	/* USB configuration */
	for (i = 0; i < ARRAY_SIZE(config->usb2_ports); i++) {
		params->PortUsb20Enable[i] = config->usb2_ports[i].enable;
		params->Usb2PhyPetxiset[i] = config->usb2_ports[i].pre_emp_bias;
		params->Usb2PhyTxiset[i] = config->usb2_ports[i].tx_bias;
		params->Usb2PhyPredeemp[i] = config->usb2_ports[i].tx_emp_enable;
		params->Usb2PhyPehalfbit[i] = config->usb2_ports[i].pre_emp_bit;

		if (config->usb2_ports[i].enable)
			params->Usb2OverCurrentPin[i] = config->usb2_ports[i].ocpin;
		else
			params->Usb2OverCurrentPin[i] = 0xff;
	}

	for (i = 0; i < ARRAY_SIZE(config->usb3_ports); i++) {
		params->PortUsb30Enable[i] = config->usb3_ports[i].enable;
		if (config->usb3_ports[i].enable) {
			params->Usb3OverCurrentPin[i] = config->usb3_ports[i].ocpin;
		} else {
			params->Usb3OverCurrentPin[i] = 0xff;
		}
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

	/* SATA */
	params->SataEnable = is_devfn_enabled(PCH_DEVFN_SATA);
	if (params->SataEnable) {
		params->SataMode = config->SataMode;
		params->SataSalpSupport = config->SataSalpSupport;

		FSP_ARRAY_LOAD(params->SataPortsEnable, config->SataPortsEnable);
		FSP_ARRAY_LOAD(params->SataPortsDevSlp, config->SataPortsDevSlp);
	}

	/* VR Configuration */
	params->ImonSlope[0] = config->ImonSlope;
	params->ImonOffset[0] = config->ImonOffset;

	/* SDCard related configuration */
	params->ScsSdCardEnabled = is_devfn_enabled(PCH_DEVFN_SDCARD);
	if (params->ScsSdCardEnabled)
		params->SdCardPowerEnableActiveHigh = config->SdCardPowerEnableActiveHigh;

	/* Enable Processor Thermal Control */
	params->Device4Enable = is_devfn_enabled(SA_DEVFN_DPTF);

	/* Set TccActivationOffset */
	params->TccActivationOffset = config->tcc_offset;

	/* eMMC configuration */
	params->ScsEmmcEnabled = is_devfn_enabled(PCH_DEVFN_EMMC);
	if (params->ScsEmmcEnabled)
		params->ScsEmmcHs400Enabled = config->ScsEmmcHs400Enabled;

	/* Enable xDCI controller if enabled in devicetree and allowed */
	dev = pcidev_path_on_root(PCH_DEVFN_USBOTG);
	if (dev) {
		if (!xdci_can_enable())
			dev->enabled = 0;
		params->XdciEnable = dev->enabled;
	} else {
		params->XdciEnable = 0;
	}

	/* Provide correct UART number for FSP debug logs */
	params->SerialIoDebugUartNumber = CONFIG_UART_FOR_CONSOLE;

	/* Configure FIVR RFI related settings */
	params->FivrRfiFrequency = config->FivrRfiFrequency;
	params->FivrSpreadSpectrum = config->FivrSpreadSpectrum;

	/* Apply minimum assertion width settings if non-zero */
	if (config->PchPmSlpS3MinAssert)
		params->PchPmSlpS3MinAssert = config->PchPmSlpS3MinAssert;
	if (config->PchPmSlpS4MinAssert)
		params->PchPmSlpS4MinAssert = config->PchPmSlpS4MinAssert;
	if (config->PchPmSlpSusMinAssert)
		params->PchPmSlpSusMinAssert = config->PchPmSlpSusMinAssert;
	if (config->PchPmSlpAMinAssert)
		params->PchPmSlpAMinAssert = config->PchPmSlpAMinAssert;

	/* Set Power Cycle Duration */
	if (config->PchPmPwrCycDur)
		params->PchPmPwrCycDur = get_pm_pwr_cyc_dur(config->PchPmSlpS4MinAssert,
				config->PchPmSlpS3MinAssert, config->PchPmSlpAMinAssert,
				config->PchPmPwrCycDur);

	/*
	 * Fill Acoustic noise mitigation related configuration
	 * JSL only has single VR domain (VCCIN VR), thus filling only index 0 for
	 * Slew rate and FastPkgCRamp for VR0 only.
	 */
	params->AcousticNoiseMitigation = config->AcousticNoiseMitigation;

	if (params->AcousticNoiseMitigation) {
		params->FastPkgCRampDisable[0] = config->FastPkgCRampDisable;
		params->SlowSlewRate[0] = config->SlowSlewRate;
		params->PreWake = config->PreWake;
		params->RampUp = config->RampUp;
		params->RampDown = config->RampDown;
	}

	/* Override/Fill FSP Silicon Param for mainboard */
	mainboard_silicon_init_params(params);
}

/* Disable Multiphase Si init */
int soc_fsp_multi_phase_init_is_enable(void)
{
	return 0;
}

/* Mainboard GPIO Configuration */
__weak void mainboard_silicon_init_params(FSP_S_CONFIG *params)
{
	printk(BIOS_DEBUG, "WEAK: %s/%s called\n", __FILE__, __func__);
}
