/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A22X_DECRYPT_DIAG_H
#define _LINUX_A22X_DECRYPT_DIAG_H
#include <linux/kconfig.h>
#include <linux/err.h>
#include <linux/types.h>
enum a22x_diag_stage {
 A22X_DIAG_TZDEV_IOCTL = 1,
 A22X_DIAG_UIWSOCK_IOCTL = 2,
 A22X_DIAG_UIWSOCK_COMPAT = 3,
 A22X_DIAG_DM_DEFAULT_KEY_CTR = 4,
 A22X_DIAG_FSCRYPT_ADD_KEY = 5,
};
#if IS_ENABLED(CONFIG_A22X_DECRYPT_DIAG)
#include <trace/events/a22x_decrypt.h>
static inline void a22x_diag_emit(unsigned int stage, unsigned int command,
                                 unsigned int phase, long result)
{
 /* Positive syscall results may be byte counts or descriptors. Do not emit. */
 trace_a22x_decrypt_stage(stage, command, phase,
                         phase == 0 ? 3 : result < -MAX_ERRNO ? 4 :
                         result < 0 ? 2 : result > 0 ? 1 : 0,
                         phase != 0 && result < 0 && result >= -MAX_ERRNO ? result : 0);
}
static inline void a22x_diag_pre_smc(unsigned int operation)
{
 trace_a22x_decrypt_pre_smc(operation);
}
static inline void a22x_diag_post_smc(unsigned int operation, int wrapper_rc,
                                     unsigned int secure_class, u64 duration_ns)
{
 trace_a22x_decrypt_post_smc(operation, wrapper_rc, secure_class, duration_ns);
}

#else
static inline void a22x_diag_emit(unsigned int stage, unsigned int command,
                                 unsigned int phase, long result)
{
 (void)stage; (void)command; (void)phase; (void)result;
}
static inline void a22x_diag_pre_smc(unsigned int operation)
{
 (void)operation;
}
static inline void a22x_diag_post_smc(unsigned int operation, int wrapper_rc,
                                     unsigned int secure_class, u64 duration_ns)
{
 (void)operation; (void)wrapper_rc; (void)secure_class; (void)duration_ns;
}

#endif
#endif
