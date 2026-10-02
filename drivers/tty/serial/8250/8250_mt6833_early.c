// SPDX-License-Identifier: GPL-2.0
/*
 * Early console for MediaTek MT6833 boards whose bootloader leaves the AP
 * UART0 pads (GPIO61 URXD0 / GPIO62 UTXD0) in a non-UART pin function
 * before jumping to the kernel (seen on Samsung SM-A226B "a22x").
 *
 * There is no mainline MT6833 pinctrl driver yet, so the two pin mode
 * fields are switched back to function 1 (UART0) directly, then the port is
 * handled by the generic 8250 early console code.
 *
 * Usage: earlycon=mt6833uart,0x11002000
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>
#include <asm/early_ioremap.h>

#define MT6833_GPIO_BASE	0x10005000
#define MT6833_GPIO_MODE7	0x370	/* pins 56..63, 4 bits per pin */
#define MT6833_URXD0_SHIFT	((61 - 56) * 4)
#define MT6833_UTXD0_SHIFT	((62 - 56) * 4)
#define MT6833_FUNC_UART0	1

static void __init mt6833_uart0_pinmux(void)
{
	void __iomem *gpio;
	u32 mask, val;

	gpio = early_ioremap(MT6833_GPIO_BASE, SZ_4K);
	if (!gpio)
		return;

	mask = GENMASK(MT6833_URXD0_SHIFT + 3, MT6833_URXD0_SHIFT) |
	       GENMASK(MT6833_UTXD0_SHIFT + 3, MT6833_UTXD0_SHIFT);
	val = readl(gpio + MT6833_GPIO_MODE7) & ~mask;
	val |= MT6833_FUNC_UART0 << MT6833_URXD0_SHIFT;
	val |= MT6833_FUNC_UART0 << MT6833_UTXD0_SHIFT;
	writel(val, gpio + MT6833_GPIO_MODE7);

	early_iounmap(gpio, SZ_4K);
}

static int __init early_mt6833_uart_setup(struct earlycon_device *device,
					  const char *options)
{
	if (!device->port.membase)
		return -ENODEV;

	mt6833_uart0_pinmux();

	device->port.iotype = UPIO_MEM32;
	device->port.regshift = 2;

	return early_serial8250_setup(device, NULL);
}
EARLYCON_DECLARE(mt6833uart, early_mt6833_uart_setup);
