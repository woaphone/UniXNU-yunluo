# Source and test scope

This clean repository imports reviewed source from the preserved local yunluo checkpoint 2551dfe8ec21d0c6711139058ef70f08edf7a220. Full legacy history remains preserved separately and is not uploaded: it contains a vendor firmware array. Whitespace-only normalization of imported source is recorded outside the publication candidate.

The PMIC key-state helper is yunluo source; its host state machine tests do not access registers or prove physical buttons/HID consumers. The reusable Novatek protocol/core tests model NT36532-family behavior. They do not qualify the yunluo sensor, firmware, SPI/IRQ or scan/download sequence. Real-firmware cases were explicitly skipped. Qualcomm TouchBoard/TLMM/GPLL/QUP/SMMU reference tests were excluded instead of treating their success as MTK proof.

The public configuration uses the ARM64 and iPadOS 17.7.11 profiles, the MT6789 common source and `woaphone/xnu-mtk` branch `yunluo`. MTK UFS, the full HID/display/ABI archive set, private inputs and a complete fresh package build remain to be reconstructed in this source layout. No firmware or image is distributed.

The preserved bring-up record confirms the physical device milestones: visible UEFI/XNU output, UFS LU2/APFS root, launchd, activation page, usable touch, eight-core MMIO IPI and a 2 GiB XNU owned-memory window. The image names are recorded in `docs/evidence/device-progress-2026-10-29.md`.

The generated common ADT lacks cpu-id and no-aic-ipi-required present in the preserved yunluo ADT generator. This is a source/host migration difference requiring consumer review, not proof of an upstream defect or a uniquely diagnosed device failure.

The intended public layout keeps Mu firmware in boot and UniXNU runtime files in cust. The tested legacy image used the earlier embedded-payload layout. No new device operation or image rewrite occurred while publishing this source tree.
