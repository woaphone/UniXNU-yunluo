# yunluo upstream candidate

This is a clean, separately initialized publication candidate, not a rewrite of the preserved legacy history. Reviewed source is imported from the local yunluo checkpoint 2551dfe8ec21d0c6711139058ef70f08edf7a220; the full original Git remains preserved outside this repository. No proprietary firmware array, Apple input, image, Data seed or original recovery history is imported.

The port is not yet build-qualified or device-verified. This initial public WIP
is for source review and collaboration, not a usable-system release. Read
porting.toml for authoritative scope and limits. A complete runtime build and
the corresponding device observations remain required for a completed-port claim.

## Required partition layout

Mu-Silicium UEFI belongs in boot. UniXNU EFI/kit, file-loaded DarwinBoot, XNU kernel and payload inputs belong separately in cust. The old payload-FAT-in-boot-ramdisk route is not the target. No device write is authorized by these files. See partition-layout.toml and the firmware/storage/packaging areas in porting.toml.

## Current scope and dependencies

This is a format-2 qualification scaffold, not the full recovered functional system.
It uses the ARM64 and ipados-17.7.11-21H461 profiles and pins
[MT6789 common](https://github.com/woaphone/UniXNU-mt6789-common) in feeds.toml.
The reviewed firmware source overlay is maintained in
[uefi-yunluo](https://github.com/yunluo-uefi-development/uefi-yunluo).
Complete MTK runtime/archive integration, private-input recipes, CPU ADT
consumer checks and a clean split-image build remain outstanding.
Qualcomm-only helpers and the old embedded vendor firmware array are excluded.

The cust-v2 largest-window loader is published as a source reference in
`loader/`, with a production-helper test. It is not yet selected by the
format-2 build; publishing this reference does not silently change the board's
loader or establish a new device result.

## Reproducible host checks

From this repository, use a fresh retained output directory:

```sh
python3 scripts/run-host-tests.py --output out/host-NEW
python3 scripts/check-partition-layout.py --self-test out/split-fixtures-NEW
python3 scripts/check-partition-layout.py --require-build-ready
```

The last command intentionally refuses until cust geometry/filesystem/load-path evidence exists. From the pinned UniXNU checkout run the normal porting-record checker and --base porting-import-base. See committed docs/evidence for exact tested source revisions and commands.

Host protocol/key-state models, DTS/ADT generation and configuration validation
have passed on the recorded source. Real sensor-firmware tests were skipped.
No complete format-2 kernel/prelink/split-image build or new device boot is
qualified; every end-to-end area stays planned. See
`docs/evidence/publication-scope.md` for the initial publication boundary.
