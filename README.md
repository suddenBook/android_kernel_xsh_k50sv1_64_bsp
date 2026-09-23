# Android kernel for k50sv1_64_bsp

Linux kernel source for the `k50sv1_64_bsp` board, used with **LineageOS 17.1
(Android 10)**. This tree is based on
[nokia-mt6750/android_kernel_nokia_mt6755](https://github.com/nokia-mt6750/android_kernel_nokia_mt6755),
with adaptations for this board's fitted hardware and Android userspace.

| Item | Configuration |
| --- | --- |
| Linux version | 3.18.140 |
| MediaTek BSP platform | MT6755 (`CONFIG_ARCH_MT6755`, `CONFIG_MTK_PLATFORM="mt6755"`) |
| Board hardware | MT6750 E2, project `k50sv1_64_bsp` |
| Kernel architecture | ARM64, with 32-bit ARM userspace compatibility (`CONFIG_COMPAT`) |
| Android platform | LineageOS 17.1 / Android 10 |
| Kernel image | `Image.gz` |

## Source layout

The Android source checkout expects this repository at
`kernel/xsh/k50sv1_64_bsp`.

The active configuration is defined by the device tree's
[BoardConfig.mk](https://github.com/suddenBook/android_device_xsh_k50sv1_64_bsp/blob/lineage-17.1/BoardConfig.mk):

- Base configuration:
  [arch/arm64/configs/k50sv1_64_bsp_stock_defconfig](arch/arm64/configs/k50sv1_64_bsp_stock_defconfig)
- Additional board configuration:
  [arch/arm64/configs/k50sv1_64_bsp_source.fragment](arch/arm64/configs/k50sv1_64_bsp_source.fragment)
- Board device tree:
  [arch/arm64/boot/dts/k50sv1_64_bsp.dts](arch/arm64/boot/dts/k50sv1_64_bsp.dts)
- MediaTek DCT board description:
  [drivers/misc/mediatek/dws/mt6755/k50sv1_64_bsp.dws](drivers/misc/mediatek/dws/mt6755/k50sv1_64_bsp.dws)

The base configuration preserves the stock kernel configuration, including
module versioning. The additional fragment selects the fitted board drivers,
source connectivity modules and built-in F2FS support.

Only the front GC5025 camera remains fitted. The active source fragment builds
that sensor and disables the removed rear camera's lens driver. The ten-entry
vendor sensor table is preserved: GC5025 stays at slot 6, while the removed
IMX145 at slot 1 uses the absent-sensor placeholder. The separate MT6353 rear
flash driver remains enabled.

## Build integration

The current Android integration uses the **AArch64 Android GCC 4.9** toolchain
with the `aarch64-linux-android-` prefix. Clang compilation is disabled in
`BoardConfig.mk`. The expected GCC prebuilt is
`prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/` in the Android tree.

The legacy MediaTek DCT generator requires Android's pinned Python 2.7
prebuilt at `prebuilts/python/linux-x86/2.7.5/bin/python2.7`. The board
configuration also selects the Android prebuilt `flex`, `bison` and Bison data
directory, and passes `LOCALVERSION=` and `KBUILD_SYMTYPES=1` to Kbuild.

Boot and recovery use the same source-built `Image.gz`. Their current
packaging retains the stock DTB and recovery DTBO assets supplied by the
device tree. A generated source DTB is therefore not a drop-in replacement
for those packaging inputs.

Current diagnostic builds use the matching `lineage-17.1` device/vendor trees
and the owner's sibling `bringup/` workspace. The build compiles kernel and all
five connectivity modules together; validate their symbol versions before
packaging. The selected 1.807 GHz, startup PPM-thermal removal and screen-on/off
CPU policy are owner requirements and must survive stable-kernel upgrades.

The canonical tree includes the individually reviewed 3.18.120–3.18.140
increments, the screen-off single-core correction, and the official mainline
VTI cleanup supplement. Both diagnostic products at source `12a5158f` passed
build, five-module ABI and actual boot/recovery/vendor image checks. The
owner's sibling workspace records the per-version handset results in
`bringup/k50sv1-bringup/evidence/kernel-stable-20260909/` and the source decisions
in `bringup/k50sv1-bringup/notes/kernel-upgrade.md`. Validation targets this
ARM64 product; it does not certify disabled drivers or other architectures.

## First release source

The v1.0.0 images were built from local kernel revision
`ef8238aca4c6f3d3eba764313320e30b24250e4e`, with Git tree
`9703c5b1204c94c66b4937dee43e6e5d2f42e971`. This public repository starts with a
complete snapshot of that source plus this publication README. The original
local repository had missing historical Git objects; its history was retained
locally. The source snapshot was exported successfully and includes all tracked
files. The upstream project's public history remains available through the
source link above.

## Related repositories

| Repository | Android checkout path |
| --- | --- |
| [android_device_xsh_k50sv1_64_bsp](https://github.com/suddenBook/android_device_xsh_k50sv1_64_bsp) | `device/xsh/k50sv1_64_bsp` |
| [android_vendor_xsh_k50sv1_64_bsp](https://github.com/suddenBook/android_vendor_xsh_k50sv1_64_bsp) | `vendor/xsh/k50sv1_64_bsp` |
| [android_kernel_xsh_k50sv1_64_bsp](https://github.com/suddenBook/android_kernel_xsh_k50sv1_64_bsp) | `kernel/xsh/k50sv1_64_bsp` |

## License and credits

See [COPYING](COPYING) for the kernel's GPLv2 license and the notices in
individual files for their applicable terms. The original Linux
[README](README) is retained. Credit belongs to the Linux kernel contributors,
MediaTek, the Nokia kernel tree maintainers, and the authors recorded in the
source files and upstream history.
