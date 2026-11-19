/*
 * Copyright (C) 2019 Samsung Electronics Co., Ltd All Rights Reserved
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/cred.h>
#include <linux/crypto.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#include "tzdev_internal.h"
#include "core/cred.h"
#include "core/log.h"
#include "core/subsystem.h"
#include "core/sysdep.h"
#include "debug/trace.h"

static const uint8_t kernel_client_hash[CRED_HASH_SIZE] = {
	0x49, 0x43, 0x54, 0xd8, 0x88, 0x45, 0x37, 0xaa,
	0x95, 0xff, 0x73, 0x57, 0x22, 0x07, 0xc9, 0x01,
	0x60, 0x09, 0x57, 0x37, 0xe5, 0x42, 0x6c, 0x5f,
	0xfa, 0x94, 0x79, 0x6b, 0xc9, 0x37, 0x39, 0x7e
};

/*
 * A22x's stock SWd/root_task uses tzdev 4.2.0's caller measurement domain:
 * SHA-256 of the actual executable file, not tzdev 4.2.1's pathname digest.
 * Measure every userspace caller generically through its referenced exe_file.
 * No executable name, expected digest, UID or authorization rule is substituted.
 */
static int tz_crypto_file_sha256(uint8_t hash[SHA256_DIGEST_SIZE],
				 struct file *file)
{
	loff_t file_size, offset = 0;
	struct crypto_shash *tfm;
	char *rbuf;
	int ret;

	file_size = i_size_read(file_inode(file));
	if (file_size <= 0)
		return -EINVAL;

	rbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!rbuf)
		return -ENOMEM;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		goto out_free_buf;
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);
		sysdep_shash_desc_init(desc, tfm);

		ret = crypto_shash_init(desc);
		if (ret)
			goto out_desc;

		while (offset < file_size) {
			loff_t before = offset;
			size_t count = min_t(loff_t, PAGE_SIZE, file_size - offset);
			ssize_t nread = kernel_read(file, rbuf, count, &offset);

			if (nread < 0) {
				ret = nread;
				goto out_desc;
			}
			/* Never authenticate an incomplete or non-progressing read. */
			if (!nread || nread > count || offset != before + nread) {
				ret = -EIO;
				goto out_desc;
			}
			ret = crypto_shash_update(desc, rbuf, nread);
			if (ret)
				goto out_desc;
		}

		if (i_size_read(file_inode(file)) != file_size) {
			ret = -ESTALE;
			goto out_desc;
		}
		ret = crypto_shash_final(desc, hash);
out_desc:
		shash_desc_zero(desc);
	}

	crypto_free_shash(tfm);
out_free_buf:
	kfree(rbuf);
	return ret;
}

static int tz_format_cred_user(struct tz_cred *cred)
{
	uint8_t hash[SHA256_DIGEST_SIZE];
	struct file *exe_file;
	struct task_struct *task = current;
	int ret;

	exe_file = sysdep_get_exe_file(task);
	if (!exe_file) {
		log_error(tzdev_cred, "mm has no associated executable file\n");
		return -ENOENT;
	}

	pr_info_once("A22X_TZDEV_CRED_ELF_SHA256_V1\n");
	ret = tz_crypto_file_sha256(hash, exe_file);
	fput(exe_file);
	if (ret) {
		log_error(tzdev_cred, "Failed to measure executable file: %d\n", ret);
		goto out;
	}

	memcpy(&cred->hash, hash, SHA256_DIGEST_SIZE);
	cred->type = TZ_CRED_HASH;
	cred->pid = task->tgid;
	cred->uid = __kuid_val(current_uid());
	cred->gid = __kgid_val(current_gid());
out:
	memzero_explicit(hash, sizeof(hash));
	return ret;
}

static int tz_format_cred_kernel(struct tz_cred *cred)
{
	cred->type = TZ_CRED_KERNEL;
	cred->pid = current->real_parent->pid;
	cred->uid = __kuid_val(current_uid());
	cred->gid = __kgid_val(current_gid());
	memcpy(&cred->hash, &kernel_client_hash, SHA256_DIGEST_SIZE);
	return 0;
}

int tz_format_cred(struct tz_cred *cred, int is_kern)
{
	BUILD_BUG_ON(sizeof(pid_t) != sizeof(cred->pid));
	BUILD_BUG_ON(sizeof(uid_t) != sizeof(cred->uid));
	BUILD_BUG_ON(sizeof(gid_t) != sizeof(cred->gid));

	return (current->mm && !is_kern) ? tz_format_cred_user(cred) : tz_format_cred_kernel(cred);
}
