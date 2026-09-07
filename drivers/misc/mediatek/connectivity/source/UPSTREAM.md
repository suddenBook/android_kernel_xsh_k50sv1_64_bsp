# MediaTek MT6755 connectivity source origins

These files were published in the following Motorola source repositories. The
initial import preserves their contents, notices and licensing. Later commits
carry the local driver fixes and Linux 3.18 Kbuild integration separately.

| Imported directory | Public source repository | Fixed revision |
| --- | --- | --- |
| `common/` | https://github.com/MotorolaMobilityLLC/vendor-mediatek-kernel_modules-connectivity-common | `038e72f831e7af86b23d6e0ab9bdeb894f7caf21` |
| `wlan/adaptor/` | https://github.com/MotorolaMobilityLLC/vendor-mediatek-kernel_modules-connectivity-wlan-adaptor | `ec5c4eed7d06db3dc24e26ec3e7ed121fe364700` |
| `wlan/core/gen2/` | https://github.com/MotorolaMobilityLLC/vendor-mediatek-kernel_modules-connectivity-wlan-core-gen2 | `11f50f99225b60b5526f96604c775e064964a324` |
| `bt/legacy/` | https://github.com/MotorolaMobilityLLC/vendor-mediatek-kernel_modules-connectivity-bt-mt66xx (legacy subdirectory) | `692b4bf6fb2c2af9dd4f57d7221597743f64f9c4` |
| `gps/` | https://github.com/MotorolaMobilityLLC/vendor-mediatek-kernel_modules-connectivity-gps | `3404c547410315b1de7ae720e12f64feb7444e85` |

The import contains 331 source, build and accompanying files. Each downloaded
file was verified against its Git blob identifier before import. The host
modules retain the existing device firmware, board calibration and vendor
user-space interfaces; those components are not supplied by these sources.
