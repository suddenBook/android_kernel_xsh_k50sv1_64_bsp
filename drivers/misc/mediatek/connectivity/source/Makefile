# Build all five host drivers as modules against this kernel's configuration.
# Kbuild supplies generated/autoconf.h through linux/kconfig.h; the arm64
# Makefile exports MTK_PLATFORM from CONFIG_MTK_PLATFORM.
TARGET_BUILD_VARIANT ?= userdebug
export TARGET_BUILD_VARIANT

obj-m += common/
obj-m += wlan/adaptor/
obj-m += wlan/core/gen2/
obj-m += bt/legacy/
obj-m += gps/
