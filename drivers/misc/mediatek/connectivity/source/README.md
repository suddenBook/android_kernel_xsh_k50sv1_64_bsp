# MT6755 connectivity host modules

`CONFIG_MTK_CONNECTIVITY_SOURCE=m` builds the five host modules from this
directory. It defaults to `m` for MT6755 with the CONSYS_6755 selection,
`CONFIG_MTK_COMBO=y` and module support. The k50sv1 source fragment enables it
explicitly. This option only permits module mode so Android's existing `=m`
check invokes `modules` and `modules_install`. The built-in `connadp` adapter remains in the parent directory and
owns the kernel platform bridge and reserved connectivity memory.

Use the normal kernel configuration and build, including
`arch/arm64/configs/k50sv1_64_bsp_source.fragment` for the source board profile.
After preparing that configuration, the normal `Image modules` targets produce
these paths below the kernel output directory:

| Module | Output path |
| --- | --- |
| WMT/STP | `drivers/misc/mediatek/connectivity/source/common/wmt_drv.ko` |
| Wi-Fi character device | `drivers/misc/mediatek/connectivity/source/wlan/adaptor/wmt_chrdev_wifi.ko` |
| Gen2 Wi-Fi | `drivers/misc/mediatek/connectivity/source/wlan/core/gen2/wlan_drv_gen2.ko` |
| Bluetooth character device | `drivers/misc/mediatek/connectivity/source/bt/legacy/bt_drv.ko` |
| GPS character device | `drivers/misc/mediatek/connectivity/source/gps/gps_drv.ko` |

All modules take their configuration from Kbuild's generated `autoconf.h` via
`linux/kconfig.h`. `arch/arm64/Makefile` exports `MTK_PLATFORM` from
`CONFIG_MTK_PLATFORM`. No Android `TOP`, external `AUTOCONF_H`, `KERNEL_OUT` or
separate module symbol files are needed. The complete kernel/module build
resolves the kernel and inter-module exports together. `TARGET_BUILD_VARIANT`
defaults to `userdebug`; setting it to `user` or `eng` selects the corresponding
upstream diagnostic flags. STEP diagnostics remain enabled independently of
the presence of an Android product directory.

Package all five modules from the same build output as the kernel, and check
their imported symbols and CRCs against that build. Android's existing
`INSTALL_MOD_STRIP=1` removes debug sections when installing modules; validate
installed bytes against a temporary `strip --strip-debug` projection of the
corresponding unmodified Kbuild module. Load WMT first, then the
character-device modules; load Gen2 Wi-Fi after `wmt_chrdev_wifi`. The imported
Android makefiles and init files retain upstream history and are not used by
this Kbuild integration.

[UPSTREAM.md](UPSTREAM.md) records the public repositories and pinned commits.
The original files and license notices are preserved in the import commit.
Separate commits bound diagnostic buffer appends, correct MT6755 regulator
voltage units and fix compiler diagnostics. The subsequent build integration
changes Makefiles, Kconfig and the source fragment without changing C or header
files.

These are host drivers. Existing firmware, including `WIFI_RAM_CODE_6755` and
the ROM patch files, device calibration, `WMT_SOC.cfg`, `BT_FW.cfg` and vendor
user-space components are still required. Rebuilding the host modules does not
replace those components or establish hardware behavior by itself.
