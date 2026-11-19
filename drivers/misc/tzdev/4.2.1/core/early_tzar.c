// SPDX-License-Identifier: GPL-2.0
/* A226BXXSBDZC3 LK-equivalent bootstrap, explicitly before any tzdev init.
 *
 * This is a different wire format from USERBOOT_UPLOAD_CONTAINER.  It takes
 * the exact mkimg payload: compressed startup archive plus root certificates.
 * No boot-time call, physical-address parameter, or second attempt is exposed.
 * A zero SMC result proves handoff only; secure "bootstrap method: attach"
 * and a working TA are the separate completion evidence.
 */
#include <linux/arm-smccc.h>
#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/smp.h>
#include <linux/unaligned.h>
#include <crypto/sha2.h>
#include <asm/cacheflush.h>

#include "core/early_tzar.h"

#define TZAR_BOOTSTRAP_SMC 0xb2000014U
#define TZAR_PAYLOAD_BYTES 3037544U
#define TZAR_BUFFER_BYTES SZ_4M

static const u8 payload_digest[SHA256_DIGEST_SIZE] = {
	0x33, 0x7d, 0xce, 0x4d, 0xb8, 0x69, 0x87, 0xb2,
	0xde, 0x26, 0x33, 0xf6, 0x5e, 0xc8, 0x7b, 0xdb,
	0xc5, 0xd1, 0x9b, 0x90, 0xed, 0x42, 0xc6, 0x01,
	0x01, 0x00, 0x0d, 0x61, 0xf6, 0x03, 0x45, 0xb2,
};

/* Keep these owned pages until reset, even after a returned SMC.  This bounded
 * allocation avoids returning any possibly retained secure reference to the
 * Linux allocator; it never claims or maps a historical LK address.
 */
static struct page *bootstrap_pages;
static atomic_t bootstrap_attempted = ATOMIC_INIT(0);
static int early_bootstrap_result = -EAGAIN;
module_param(early_bootstrap_result, int, 0444);
MODULE_PARM_DESC(early_bootstrap_result,
	"SMC .14 handoff only: -EAGAIN before start, 0 after returned success; not TA readiness");

static int bootstrap_call(void *unused)
{
	struct arm_smccc_res result;
	void *buffer = page_address(bootstrap_pages);
	phys_addr_t pa = page_to_phys(bootstrap_pages);

	/* The work is bound to CPU0; all unused ABI argument registers are zero. */
	if (raw_smp_processor_id() != 0)
		return -EXDEV;
	pr_info("tzdev: early TZAR handoff cpu=%u pa=%pa bytes=%u allocated=%u\n",
		raw_smp_processor_id(), &pa, TZAR_PAYLOAD_BYTES, TZAR_BUFFER_BYTES);
	preempt_disable();
	dcache_clean_inval_poc((unsigned long)buffer,
		(unsigned long)buffer + TZAR_BUFFER_BYTES);
	arm_smccc_smc(TZAR_BOOTSTRAP_SMC, (u32)pa, TZAR_PAYLOAD_BYTES,
		0, 0, 0, 0, 0, &result);
	preempt_enable();
	pr_info("tzdev: early TZAR SMC raw=0x%lx signed32=%d r1=0x%lx r2=0x%lx r3=0x%lx\n",
		result.a0, (s32)result.a0, result.a1, result.a2, result.a3);
	return (s32)result.a0;
}

int tzdev_early_tzar_bootstrap(void)
{
	const struct firmware *fw;
	struct device_node *chosen;
	const void *atag;
	u8 digest[SHA256_DIGEST_SIZE];
	phys_addr_t pa;
	int len, ret;
	bool recovery;

	BUILD_BUG_ON(PAGE_SIZE != SZ_4K);
	if (atomic_cmpxchg(&bootstrap_attempted, 0, 1))
		return -EALREADY;
	WRITE_ONCE(early_bootstrap_result, -EINPROGRESS);
	if (!of_machine_is_compatible("samsung,a22x")) {
		ret = -ENODEV;
		goto out;
	}
	chosen = of_find_node_by_path("/chosen");
	atag = chosen ? of_get_property(chosen, "atag,boot", &len) : NULL;
	recovery = atag && len >= 12 && get_unaligned_le32(atag + 8) == 2;
	of_node_put(chosen);
	if (!recovery) {
		ret = -EPERM;
		goto out;
	}
	ret = request_firmware(&fw, "a22x-tzar.bin", NULL);
	if (ret)
		goto out;
	if (fw->size != TZAR_PAYLOAD_BYTES) {
		ret = -EINVAL;
		goto release;
	}
	sha256(fw->data, fw->size, digest);
	if (memcmp(digest, payload_digest, sizeof(digest))) {
		ret = -EBADMSG;
		goto release;
	}
	bootstrap_pages = alloc_pages(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO |
		__GFP_NOWARN | __GFP_NORETRY, get_order(TZAR_BUFFER_BYTES));
	if (!bootstrap_pages) {
		ret = -ENOMEM;
		goto release;
	}
	pa = page_to_phys(bootstrap_pages);
	if (!IS_ALIGNED(pa, SZ_4K) || pa + TZAR_BUFFER_BYTES > SZ_4G) {
		ret = -ERANGE;
		__free_pages(bootstrap_pages, get_order(TZAR_BUFFER_BYTES));
		bootstrap_pages = NULL;
		goto release;
	}
	memcpy(page_address(bootstrap_pages), fw->data, fw->size);
	/* Hash the actual contiguous copy, not only the firmware-loader buffer. */
	sha256(page_address(bootstrap_pages), fw->size, digest);
	if (memcmp(digest, payload_digest, sizeof(digest))) {
		ret = -EIO;
		goto release;
	}
	release_firmware(fw);
	cpus_read_lock();
	ret = smp_call_on_cpu(0, bootstrap_call, NULL, false);
	cpus_read_unlock();
	goto out;
release:
	release_firmware(fw);
out:
	WRITE_ONCE(early_bootstrap_result, ret);
	pr_info("tzdev: early TZAR handoff result=%d; no retry this boot\n", ret);
	return ret;
}

bool tzdev_early_tzar_handed_off(void)
{
	return READ_ONCE(early_bootstrap_result) == 0;
}
