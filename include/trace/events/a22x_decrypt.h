/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM a22x_decrypt
#if !defined(_TRACE_A22X_DECRYPT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_A22X_DECRYPT_H
#include <linux/tracepoint.h>
/* Only fixed operation identity and errno category; never argument contents. */
TRACE_EVENT(a22x_decrypt_stage,
 TP_PROTO(unsigned int stage, unsigned int command, unsigned int phase,
          unsigned int result_class, long error),
 TP_ARGS(stage, command, phase, result_class, error),
 TP_STRUCT__entry(
  __field(unsigned int, stage)
  __field(unsigned int, command)
  __field(unsigned int, phase)
  __field(unsigned int, result_class)
  __field(long, error)
 ),
 TP_fast_assign(
  __entry->stage = stage;
  __entry->command = command;
  __entry->phase = phase;
  __entry->result_class = result_class;
  __entry->error = error;
 ),
 TP_printk("stage=%u command=%u phase=%u class=%u errno=%ld",
           __entry->stage, __entry->command, __entry->phase,
           __entry->result_class, __entry->error)
);
/* Same scalar schema/operation classes as the stock diagnostic candidate. */
TRACE_EVENT(a22x_decrypt_pre_smc,
 TP_PROTO(unsigned int operation),
 TP_ARGS(operation),
 TP_STRUCT__entry(__field(unsigned int, operation)),
 TP_fast_assign(__entry->operation = operation;),
 TP_printk("operation=%u", __entry->operation)
);
TRACE_EVENT(a22x_decrypt_post_smc,
 TP_PROTO(unsigned int operation, int wrapper_rc,
          unsigned int secure_class, u64 duration_ns),
 TP_ARGS(operation, wrapper_rc, secure_class, duration_ns),
 TP_STRUCT__entry(
  __field(unsigned int, operation)
  __field(int, wrapper_rc)
  __field(unsigned int, secure_class)
  __field(unsigned int, reserved)
  __field(u64, duration_ns)
 ),
 TP_fast_assign(
  __entry->operation = operation;
  __entry->wrapper_rc = wrapper_rc;
  __entry->secure_class = secure_class;
  __entry->reserved = 0;
  __entry->duration_ns = duration_ns;
 ),
 TP_printk("operation=%u wrapper_rc=%d secure_class=%u duration_ns=%llu",
           __entry->operation, __entry->wrapper_rc, __entry->secure_class,
           (unsigned long long)__entry->duration_ns)
);

#endif
#include <trace/define_trace.h>
