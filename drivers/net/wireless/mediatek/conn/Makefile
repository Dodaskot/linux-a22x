# SPDX-License-Identifier: GPL-2.0-only

# The MT6833 A22X board uses the SOC2_1X1 AXI connectivity device.
MTK_CONN_HIF := axi
MTK_CONN_PLATFORM := mt6833
MTK_CONN_CHIP := SOC2_1X1
MTK_CONN_WLAN_CHIP_ID := 6833
MTK_WLAN_SERVICE ?= yes
MTK_ANDROID_WMT ?= y
MTK_ANDROID_EMI ?= y
TARGET_BUILD_VARIANT ?= userdebug

subdir-ccflags-y += -I$(src)/compat/include
subdir-ccflags-y += -include $(src)/compat/include/mtk_compat.h
subdir-ccflags-y += -I$(src)/common/common_main/include
subdir-ccflags-y += -I$(src)/common/common_main/linux/include
subdir-ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT
subdir-ccflags-y += -DWMT_IDC_SUPPORT=0
subdir-ccflags-y += -DLIST_SEG_MAX=100
subdir-ccflags-y += -DBTIF_DBG_SUPPORT=0
subdir-ccflags-y += -Wno-error -Wno-strict-prototypes
subdir-ccflags-y += -Wno-error=strict-prototypes -Wno-null-pointer-subtraction
subdir-ccflags-y += -Wno-cast-function-type-strict -Wno-array-bounds
subdir-ccflags-y += -Wno-missing-prototypes
subdir-ccflags-y += -Wno-incompatible-pointer-types-discards-qualifiers
subdir-ccflags-y += -Wno-visibility -Wno-unused-but-set-variable
subdir-ccflags-y += -Wno-unused-variable -Wno-unused-function -ferror-limit=0

obj-m += btif/common/
obj-m += common/
obj-m += bt/hci/
obj-m += wlan/adaptor/
obj-m += wlan/core/gen4m/
