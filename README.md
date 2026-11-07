# Xiaomi Redmi Pad (yunluo)

UniXNU device repository for the Xiaomi Redmi Pad (`yunluo`, MediaTek MT6789 / Helio G99).

The tested baseline runs the iPadOS 17.7.11 / 21H461 userspace on yunluo through Mu-Silicium UEFI and XNU. It reaches the activation screen, supports touch input, starts all eight CPUs and uses a 2 GiB XNU memory window.

## Tested status

| Area | Result |
| --- | --- |
| UEFI and display | UEFI output and the XNU console are visible; the activation page has been reached |
| Storage | UFS LU2, 4096-byte blocks, APFS root and launchd are working |
| Display | 1200x2000 panel presentation works, including the 180-degree UI configuration |
| Touch | Touch input is usable on the `inputfix4` baseline |
| CPU | Eight-core startup and MMIO IPI delivery are working |
| Memory | The 2 GiB XNU owned-memory profile is usable |
| Userland | Launchd, Setup and the activation UI have been reached |

Still open: Setup initialization currently ends with PID1 `SIGBUS`, GPU acceleration is not available, and audio, Wi-Fi, Bluetooth and full power management remain incomplete. Touch calibration at all four panel corners has not been recorded.

## Hardware

| Item | Value |
| --- | --- |
| Device | Xiaomi Redmi Pad |
| Codename | `yunluo` |
| SoC | MediaTek MT6789 / Helio G99 |
| CPU | 6x Cortex-A55 + 2x Cortex-A76 |
| Timer | 13 MHz |
| Storage | UFS, LU2 user data, 4096-byte logical blocks |
| Panel | 1200x2000, 2048-pixel scanline |
| Userspace donor | iPad7,1, iPadOS 17.7.11 / 21H461 |

## Source layout

| Repository | Responsibility |
| --- | --- |
| [`woaphone/xnu-mtk`](https://github.com/woaphone/xnu-mtk) | XNU source and per-device kernel branches |
| [this repository](https://github.com/woaphone/UniXNU-yunluo) | Board resources, drivers, patches, payload and image assembly |
| [`yunluo-uefi-development/uefi-yunluo`](https://github.com/yunluo-uefi-development/uefi-yunluo) | UEFI platform and DarwinBoot integration |
| [`woaphone/UniXNU-mt6789-common`](https://github.com/woaphone/UniXNU-mt6789-common) | Shared MT6789 configuration and device-tree source |

The XNU source is pinned by `feeds.toml` to branch `yunluo` at `53bc420e5acda41c1689f6690a85841393a347cf`.

## Build

The previously local MT6789 UFS implementation is now available in
[`drivers/NativeMtkUfs`](drivers/NativeMtkUfs), including bounded
READ recovery, its production-policy tests, license/provenance and an archive
build helper. Its README distinguishes the recovered device baseline from the
new recovery code's offline qualification. The default configuration below is
still a qualification scaffold, not the complete recovered system recipe.

From a UniXNU checkout:

```sh
./unixnu defconfig yunluo_defconfig -D ../UniXNU-yunluo
./unixnu fetch -D ../UniXNU-yunluo
./unixnu status --json -D ../UniXNU-yunluo
./unixnu tree -D ../UniXNU-yunluo
python3 scripts/porting-record.py check --device ../UniXNU-yunluo
```

Proprietary IPSW, firmware, Data seeds and generated images stay outside Git. Device-specific build overrides belong in the ignored `local.toml`.

## Evidence

The condensed device milestone record is in [`docs/evidence/device-progress-2026-10-29.md`](docs/evidence/device-progress-2026-10-29.md). The authoritative configuration and per-area progress are in [`porting.toml`](porting.toml).

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
