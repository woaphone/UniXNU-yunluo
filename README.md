# yunluo upstream candidate

This is a clean, separately initialized publication candidate, not a rewrite of the preserved legacy history. Reviewed source is imported from the local yunluo checkpoint 2551dfe8ec21d0c6711139058ef70f08edf7a220; the full original Git remains preserved outside this repository. No proprietary firmware array, Apple input, image, Data seed or original recovery history is imported.

The port is not yet build-qualified or device-verified. Read porting.toml for authoritative scope and limits. Do not publish until the format-2, source, build and device acceptance gates pass.

## Required partition layout

Mu-Silicium UEFI belongs in boot. UniXNU EFI/kit, file-loaded DarwinBoot, XNU kernel and payload inputs belong separately in cust. The old payload-FAT-in-boot-ramdisk route is not the target. No device write is authorized by these files. See partition-layout.toml and the firmware/storage/packaging areas in porting.toml.

## Current scope and dependencies

This is a format-2 qualification scaffold, not the full recovered functional system. It uses the official ARM64 and ipados-17.7.11-21H461 profiles and an explicitly recorded local MT6789 common source. A public pinned SoC feed, complete MTK runtime/component and archive migration, private-input recipes, Mu/cust loading and image builds are still required. Qualcomm-only test helpers and the old embedded vendor firmware array are not included.

## Reproducible host checks

From this repository, use a fresh retained output directory:

```sh
python3 scripts/run-host-tests.py --output out/host-NEW
python3 scripts/check-partition-layout.py --self-test out/split-fixtures-NEW
python3 scripts/check-partition-layout.py --require-build-ready
```

The last command intentionally refuses until cust geometry/filesystem/load-path evidence exists. From the pinned UniXNU checkout run the normal porting-record checker and --base porting-import-base. See committed docs/evidence for exact tested source revisions and commands.

Host protocol/key-state models, DTS/ADT generation and configuration validation have passed. Real sensor-firmware tests were skipped. No kernel/prelink/split-image build or new device boot has passed; every end-to-end area stays planned rather than being promoted by a narrow host test. Do not submit or claim usability yet.
