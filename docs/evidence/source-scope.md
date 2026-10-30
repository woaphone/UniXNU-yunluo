# Source and test scope

This clean repository imports reviewed source from the preserved local yunluo checkpoint 2551dfe8ec21d0c6711139058ef70f08edf7a220. Full legacy history remains preserved separately and is not uploaded: it contains a vendor firmware array. Whitespace-only normalization of imported source is recorded outside the publication candidate.

The PMIC key-state helper is yunluo source; its host state machine tests do not access registers or prove physical buttons/HID consumers. The reusable Novatek protocol/core tests model NT36532-family behavior. They do not qualify the yunluo sensor, firmware, SPI/IRQ or scan/download sequence. Real-firmware cases were explicitly skipped. Qualcomm TouchBoard/TLMM/GPLL/QUP/SMMU reference tests were excluded instead of treating their success as MTK proof.

The format-2 configuration is a core-only qualification scaffold. It has not preserved the entire working legacy runtime yet. MTK UFS, the full HID/display/ABI archive set, hand-restored XNU patches, Mu FV changes, private inputs and full build remain pending. No firmware/image is distributed.

The generated common ADT lacks cpu-id and no-aic-ipi-required present in the preserved yunluo ADT generator. This is a source/host migration difference requiring consumer review, not proof of an upstream defect or a uniquely diagnosed device failure.

User layout is mandatory: boot contains Mu firmware; cust contains UniXNU EFI/runtime/payload. No cust offset, capacity, filesystem or GPT/logical identity is assumed. The old monolithic baseline is intentionally rejected by the split-policy test. No device operation or image rewrite occurred.
