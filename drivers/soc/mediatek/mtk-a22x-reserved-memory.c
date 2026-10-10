// SPDX-License-Identifier: GPL-2.0-only
/* Samsung A22 5G: discard an unused Android dynamic memory pool. */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/printk.h>
#include <linux/sizes.h>

static int __init a22x_zmc_validate(unsigned long node, phys_addr_t *align)
{
	const __be32 *size;
	int len;

	if (!of_flat_dt_is_compatible(of_get_flat_dt_root(), "samsung,a22x"))
		return 0;

	/* Never discard a firmware-placed or protected reservation. */
	if (of_get_flat_dt_prop(node, "reg", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL))
		return 0;

	size = of_get_flat_dt_prop(node, "size", &len);
	if (!size || len != 2 * sizeof(*size) ||
	    of_read_number(size, 2) != 848 * SZ_1M)
		return 0;

	/*
	 * LK adds this size-only Android ZMC pool. The vendor driver turns
	 * it into movable/CMA memory, but mainline has no ZMC clients and
	 * otherwise strands all 848 MiB. Reject it before memblock allocates
	 * anything. Keep the separate display CMA and firmware regions.
	 */
	pr_info("A22x: skipping unused Android ZMC pool (848 MiB)\n");
	return -EOPNOTSUPP;
}

static int __init a22x_zmc_init(unsigned long node, struct reserved_mem *rmem)
{
	/* Preserve the normal reservation for every case not rejected above. */
	return -ENODEV;
}

static const struct reserved_mem_ops a22x_zmc_ops = {
	.node_validate = a22x_zmc_validate,
	.node_init = a22x_zmc_init,
};
RESERVEDMEM_OF_DECLARE(a22x_zmc, "mediatek,zone_movable_cma", &a22x_zmc_ops);
