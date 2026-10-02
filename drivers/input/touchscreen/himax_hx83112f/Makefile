# SPDX-License-Identifier: GPL-2.0
ccflags-y += -D__HIMAX_HX83112_MOD__ -DCONFIG_TOUCHSCREEN_HIMAX_INCELL \
	     -DCONFIG_TOUCHSCREEN_HIMAX_IC_HX83112
obj-$(CONFIG_TOUCHSCREEN_HIMAX_HX83112F) += himax_hx83112f.o
himax_hx83112f-y := himax_platform.o himax_common.o himax_ic_incell_core.o \
		    himax_ic_HX83112.o
