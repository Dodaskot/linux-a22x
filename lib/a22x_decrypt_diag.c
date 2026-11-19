// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#define CREATE_TRACE_POINTS
#include <trace/events/a22x_decrypt.h>
EXPORT_TRACEPOINT_SYMBOL_GPL(a22x_decrypt_stage);
EXPORT_TRACEPOINT_SYMBOL_GPL(a22x_decrypt_pre_smc);
EXPORT_TRACEPOINT_SYMBOL_GPL(a22x_decrypt_post_smc);
