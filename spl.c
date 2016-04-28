/*
 * (C) Copyright 2010
 * Texas Instruments, <www.ti.com>
 *
 * Aneesh V <aneesh@ti.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <common.h>
#include <spl.h>
#include <asm/u-boot.h>
#include <nand.h>
#include <fat.h>
#include <version.h>
#include <i2c.h>
#include <image.h>
#include <malloc.h>
#include <linux/compiler.h>
#ifdef CONFIG_SPL_DFU
#include <environment.h>
#include <dfu.h>
#include <g_dnl.h>
#include <usb.h>
#include <mmc.h>
#endif
#include <remoteproc.h>

#include <asm/ti-common/edma.h>
#if defined(CONFIG_PERIPHERAL_BOOT) && defined(CONFIG_CMD_FASTBOOT)
#include <usb/fastboot.h>
#include <asm/arch/sys_proto.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#ifndef CONFIG_SYS_UBOOT_START
#define CONFIG_SYS_UBOOT_START	CONFIG_SYS_TEXT_BASE
#endif
#ifndef CONFIG_SYS_MONITOR_LEN
#define CONFIG_SYS_MONITOR_LEN	(200 * 1024)
#endif

u32 *boot_params_ptr = NULL;
struct spl_image_info spl_image;

/* Define board data structure */
static bd_t bdata __attribute__ ((section(".data")));

/*
 * Default function to determine if u-boot or the OS should
 * be started. This implementation always returns 1.
 *
 * Please implement your own board specific funcion to do this.
 *
 * RETURN
 * 0 to not start u-boot
 * positive if u-boot should start
 */
#ifdef CONFIG_SPL_OS_BOOT
__weak int spl_start_uboot(void)
{
	puts("SPL: Please implement spl_start_uboot() for your board\n");
	puts("SPL: Direct Linux boot not active!\n");
	return 1;
}
#endif

/*
 * Weak default function for board specific cleanup/preparation before
 * Linux boot. Some boards/platforms might not need it, so just provide
 * an empty stub here.
 */
__weak void spl_board_prepare_for_linux(void)
{
	/* Nothing to do! */
}

#ifdef CONFIG_SPL_DFU
static int run_dfu_emmc(void)
{
	char *str_env;
	int ret = 1;
	char *interface = "mmc";
	int i = 0;

	set_default_env(0);
	str_env = getenv("dfu_alt_info_emmc_spl");
	if (!str_env) {
		error("\"dfu_alt_info_emmc_spl\" env variable not defined!\n");
		return -EINVAL;
	}

	ret = setenv("dfu_alt_info", str_env);
	if (ret) {
		error("unable to set env variable \"dfu_alt_info\"!\n");
		return -EINVAL;
	}

	ret = dfu_init_env_entities(interface, 1);
	if (ret)
		goto done;

	ret = CMD_RET_SUCCESS;

	board_usb_init(0, 1);
	g_dnl_register("usb_dnl_dfu");

	while (1) {
		if (dfu_reset())
			if (++i == 10)
				goto exit;
		if (ctrlc())
			goto exit;
		usb_gadget_handle_interrupts(0);
	}
exit:
	g_dnl_unregister();
	board_usb_cleanup(0, 1);
done:
	dfu_free_entities();
	if (dfu_reset())
		run_command("reset", 0);

	return ret;
}
#endif

void spl_parse_image_header(const struct image_header *header)
{
	u32 header_size = sizeof(struct image_header);

	if (image_get_magic(header) == IH_MAGIC) {
		if (spl_image.flags & SPL_COPY_PAYLOAD_ONLY) {
			/*
			 * On some system (e.g. powerpc), the load-address and
			 * entry-point is located at address 0. We can't load
			 * to 0-0x40. So skip header in this case.
			 */
			spl_image.load_addr = image_get_load(header);
			spl_image.entry_point = image_get_ep(header);
			spl_image.size = image_get_data_size(header);
		} else {
			spl_image.entry_point = image_get_load(header);
			/* Load including the header */
			spl_image.load_addr = spl_image.entry_point -
				header_size;
			spl_image.size = image_get_data_size(header) +
				header_size;
		}
		spl_image.os = image_get_os(header);
		spl_image.name = image_get_name(header);
		debug("spl: payload image: %.*s load addr: 0x%x size: %d\n",
			sizeof(spl_image.name), spl_image.name,
			spl_image.load_addr, spl_image.size);
#ifdef CONFIG_SPL_ANDROID_BOOT_SUPPORT
	} else if (genimg_get_format(header) == IMAGE_FORMAT_ANDROID) {
		ulong kern_start;
		android_image_get_kernel((const struct andr_img_hdr *)header,
					 true, &kern_start,
					 (ulong *)&spl_image.size);
		spl_image.entry_point = android_image_get_kload((const struct andr_img_hdr *)header);
		spl_image.load_addr = spl_image.entry_point - (kern_start - (ulong)header);
		spl_image.size += (kern_start - (ulong)header);
		spl_image.os = IH_OS_LINUX;
		spl_image.name = "Android Image";
		debug("spl: android image: %.*s load addr: 0x%x size: %d\n",
			sizeof(spl_image.name), spl_image.name,
			spl_image.load_addr, spl_image.size);
#endif
	} else {
		/* Signature not found - assume u-boot.bin */
		debug("mkimage signature not found - ih_magic = %x\n",
			header->ih_magic);
		/* Let's assume U-Boot will not be more than 200 KB */
		spl_image.size = CONFIG_SYS_MONITOR_LEN;
		spl_image.entry_point = CONFIG_SYS_UBOOT_START;
		spl_image.load_addr = CONFIG_SYS_TEXT_BASE;
		spl_image.os = IH_OS_U_BOOT;
		spl_image.name = "U-Boot";
	}
}

__weak void __noreturn jump_to_image_no_args(struct spl_image_info *spl_image)
{
	typedef void __noreturn (*image_entry_noargs_t)(void);

	image_entry_noargs_t image_entry =
			(image_entry_noargs_t) spl_image->entry_point;

	debug("image entry point: 0x%X\n", spl_image->entry_point);
	image_entry();
}

#ifdef CONFIG_LATE_ATTACH

/* Error code to indicate that SPL failed to load a remotecore */
#define SPL_CORE_LOAD_ERR_ID (0xFF00)

/*
 * Loads the remotecores specified in the cores array.
 *
 * In case of failure, the core id array is OR'ed with an error code
 * to indicate that the load has failed.
 *
 * This information can be used to indicate status of the remotecore
 * load to the kernel.
 */
void spl_load_cores(u32 *cores, u32 numcores)
{

	u32 i = 0;

	for (i = 0; i < numcores ; i++) {
		u32 core = cores[i];
		if (spl_mmc_load_core(core) || spl_boot_core(core)) {
			cores[i] = (cores[i] | SPL_CORE_LOAD_ERR_ID);
			printf
				("Error loading remotecore %s!,Continuing with boot ...\n",
				 rproc_cfg_arr[core]->core_name);
		} else {
			debug("loading remote core successful\n");
		}
	}
	return;
}
#endif

#ifdef CONFIG_SPL_RAM_DEVICE
static void spl_ram_load_image(void)
{
	const struct image_header *header;

	/*
	 * Get the header.  It will point to an address defined by handoff
	 * which will tell where the image located inside the flash. For
	 * now, it will temporary fixed to address pointed by U-Boot.
	 */
	header = (struct image_header *)
		(CONFIG_SYS_TEXT_BASE -	sizeof(struct image_header));

	spl_parse_image_header(header);
}
#endif

#if defined(CONFIG_PERIPHERAL_BOOT) && defined(CONFIG_CMD_FASTBOOT)
extern int do_fastboot(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[]);
#endif
void board_init_r(gd_t *dummy1, ulong dummy2)
{
#if defined(CONFIG_PERIPHERAL_BOOT) && defined(CONFIG_CMD_FASTBOOT)
	struct mmc *mmc;
#endif
	u32 boot_device;
#ifdef CONFIG_LATE_ATTACH
	u32 cores_to_boot[] = { IPU2, DSP1, DSP2, IPU1 };
#endif
	debug(">>spl:board_init_r()\n");

#ifdef CONFIG_SYS_SPL_MALLOC_START
#if defined(CONFIG_SPL_DMA_SUPPORT) && defined(CONFIG_TI_EDMA)
	edma_zero_memory((void *)CONFIG_SYS_SPL_MALLOC_START,
			 CONFIG_SYS_SPL_MALLOC_SIZE, 6, 1);
#endif
	mem_malloc_init(CONFIG_SYS_SPL_MALLOC_START,
			CONFIG_SYS_SPL_MALLOC_SIZE);
#endif

#ifndef CONFIG_PPC
	/*
	 * timer_init() does not exist on PPC systems. The timer is initialized
	 * and enabled (decrementer) in interrupt_init() here.
	 */
	timer_init();
#endif

#ifdef CONFIG_SPL_BOARD_INIT
	spl_board_init();
#endif

	boot_device = spl_boot_device();
	debug("boot device - %d\n", boot_device);

	switch (boot_device) {
#ifdef CONFIG_SPL_RAM_DEVICE
	case BOOT_DEVICE_RAM:
		spl_ram_load_image();
		break;
#endif
#ifdef CONFIG_SPL_MMC_SUPPORT
	case BOOT_DEVICE_MMC1:
	case BOOT_DEVICE_MMC2:
	case BOOT_DEVICE_MMC2_2:
		spl_mmc_load_image();
#ifdef CONFIG_LATE_ATTACH
		spl_load_cores(cores_to_boot, sizeof(cores_to_boot)/sizeof(u32));
#endif
		break;
#endif
#ifdef CONFIG_SPL_NAND_SUPPORT
	case BOOT_DEVICE_NAND:
		spl_nand_load_image();
		break;
#endif
#ifdef CONFIG_SPL_ONENAND_SUPPORT
	case BOOT_DEVICE_ONENAND:
		spl_onenand_load_image();
		break;
#endif
#ifdef CONFIG_SPL_NOR_SUPPORT
	case BOOT_DEVICE_NOR:
		spl_nor_load_image();
		break;
#endif
#if defined(CONFIG_SPL_YMODEM_SUPPORT) || defined(CONFIG_PERIPHERAL_BOOT)
	case BOOT_DEVICE_UART:
#ifdef CONFIG_PERIPHERAL_BOOT
#ifdef CONFIG_CMD_FASTBOOT
		puts("Device successfully booted, starting fastboot...\n");
		spl_mmc_init(&mmc);
		do_fastboot(NULL, 0, 0, NULL);
#else
		debug("SPL: booted, but fastboot not supported...\n");
		hang();
#endif
#else
		spl_ymodem_load_image();
#endif
		break;
#endif
#ifdef CONFIG_SPL_SPI_SUPPORT
	case BOOT_DEVICE_SPI:
		spl_spi_load_image();
#ifdef CONFIG_LATE_ATTACH
		spl_load_cores(cores_to_boot, sizeof(cores_to_boot)/sizeof(u32));
#endif
		break;
#endif
#ifdef CONFIG_SPL_ETH_SUPPORT
	case BOOT_DEVICE_CPGMAC:
#ifdef CONFIG_SPL_ETH_DEVICE
		spl_net_load_image(CONFIG_SPL_ETH_DEVICE);
#else
		spl_net_load_image(NULL);
#endif
		break;
#endif
#ifdef CONFIG_SPL_USBETH_SUPPORT
	case BOOT_DEVICE_USBETH:
		spl_net_load_image("usb_ether");
		break;
#endif
	case BOOT_DEVICE_USB:
#ifdef CONFIG_SPL_DFU
		mmc_initialize(gd->bd);
		spl_start_uboot();
		run_dfu_emmc();
#endif
#if defined(CONFIG_SPL_USB_HOST_SUPPORT) || defined(CONFIG_PERIPHERAL_BOOT)
#ifdef CONFIG_PERIPHERAL_BOOT
#ifdef CONFIG_CMD_FASTBOOT
		puts("SPL: successfully booted, starting fastboot...\n");
		spl_mmc_init(&mmc);
		do_fastboot(NULL, 0, 0, NULL);
#else
		debug("SPL: booted, but fastboot not supported...\n");
		hang();
#endif
#else
		spl_usb_load_image();
#endif
#endif
		break;
#ifdef CONFIG_SPL_SATA_SUPPORT
	case BOOT_DEVICE_SATA:
		spl_sata_load_image();
		break;
#endif
	default:
		debug("SPL: Un-supported Boot Device\n");
		hang();
	}

	switch (spl_image.os) {
	case IH_OS_U_BOOT:
#if defined(CONFIG_SECURE_BOOT)
		/* Authenticate loaded image before jumping execution */
		secure_boot_verify_image(
			(const void *)spl_image.entry_point,
			(size_t)(spl_image.size - sizeof(struct image_header)));
		debug("U-Boot authentication passed!\n");
#endif
		debug("Jumping to U-Boot\n");
		break;
#ifdef CONFIG_SPL_OS_BOOT
	case IH_OS_LINUX:
		debug("Jumping to Linux\n");
		spl_board_prepare_for_linux();
		jump_to_image_linux((void *)CONFIG_SYS_SPL_ARGS_ADDR);
#endif
	default:
		debug("Unsupported OS image.. Jumping nevertheless..\n");
	}
	cleanup_before_linux();
	jump_to_image_no_args(&spl_image);
}

/*
 * This requires UART clocks to be enabled.  In order for this to work the
 * caller must ensure that the gd pointer is valid.
 */
void preloader_console_init(void)
{
	gd->bd = &bdata;
	gd->baudrate = CONFIG_BAUDRATE;

	serial_init();		/* serial communications setup */

	gd->have_console = 1;

	puts("\nU-Boot SPL " PLAIN_VERSION " (" U_BOOT_DATE " - " \
			U_BOOT_TIME ")\n");
#ifdef CONFIG_SPL_DISPLAY_PRINT
	spl_display_print();
#endif
}
