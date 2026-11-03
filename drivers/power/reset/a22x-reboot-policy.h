/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A22X_REBOOT_POLICY_H
#define A22X_REBOOT_POLICY_H

#define A22X_RESET_MAGIC		0x12345678U
#define A22X_REASON_NORMAL	0x12345670U
#define A22X_REASON_DOWNLOAD	0x12345671U
#define A22X_REASON_RECOVERY	0x12345674U
#define A22X_REASON_BOOTLOADER	0x1234567dU

/* False leaves both existing SRAM words untouched. */
static inline bool a22x_reboot_reason(const char *cmd, unsigned int marker,
				      unsigned int reason, unsigned int *next_reason)
{
	if (!cmd || !*cmd) {
		/* Preserve the existing raw-SRAM Download/Recovery helpers. */
		if (marker == A22X_RESET_MAGIC &&
		    (reason == A22X_REASON_DOWNLOAD ||
		     reason == A22X_REASON_RECOVERY ||
		     reason == A22X_REASON_BOOTLOADER))
			return false;
		*next_reason = A22X_REASON_NORMAL;
	} else if (!strcmp(cmd, "normal") || !strcmp(cmd, "reboot")) {
		/* An explicit normal request overrides every stale reason. */
		*next_reason = A22X_REASON_NORMAL;
	} else if (!strcmp(cmd, "download")) {
		*next_reason = A22X_REASON_DOWNLOAD;
	} else if (!strcmp(cmd, "recovery")) {
		*next_reason = A22X_REASON_RECOVERY;
	} else if (!strcmp(cmd, "bootloader")) {
		*next_reason = A22X_REASON_BOOTLOADER;
	} else {
		/* Other explicit requests belong to their existing handlers. */
		return false;
	}

	return true;
}

#endif
