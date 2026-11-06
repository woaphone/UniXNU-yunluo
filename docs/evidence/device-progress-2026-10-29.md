# Yunluo device progress

Source: `D:\漏洞\UNIXNU\docs\YUNLUO_FULL_STORY_2026-10-29.md` and the images named by that record.

## Tested milestones

| Image or version | Confirmed result |
| --- | --- |
| `boot-yunluo-unixnu-fb46` | UEFI video output became visible and XNU text was shown on the panel |
| `boot-yunluo-unixnu-fb47` | UFS LU2 was accepted and SCSI READ(10) completed |
| `boot-yunluo-unixnu-fb50` | GPT presentation exposed the APFS container |
| `boot-yunluo-unixnu-fb70` and later | APFS root, cryptex and launchd reached |
| `boot-yunluo-unixnu-fb198` | Activation page reached |
| `boot-yunluo-unixnu-inputfix4-expdb1029.img` | Touch input confirmed usable |
| `boot-yunluo-unixnu-inputfix5-display-aks.img` | 180-degree display and AKS compatibility layer |
| `boot-yunluo-unixnu-inputfix6-smp8-mmioipi.img` | Eight-core startup and MMIO IPI delivery |
| `boot-yunluo-unixnu-inputfix6-smp8-mmioipi-2g.img` | 2 GiB XNU memory profile confirmed usable |
| `boot-yunluo-unixnu-smp8-2g-setup-init1.img` | Setup initialization reached PID1 `SIGBUS` |

## Current limits

- Setup initialization ends with PID1 `SIGBUS`.
- Full four-corner touch calibration has not been recorded.
- Mali GPU, audio, Wi-Fi, Bluetooth and complete power management remain incomplete.
