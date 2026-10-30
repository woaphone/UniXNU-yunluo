# Initial public source scope

This publication contains the clean format-2 configuration/host-model history,
a public MT6789 SoC feed, a local override example and the source-only
DarwinBoot largest-window reference. No recovery archive, Apple input, device
Data, private touch firmware array or prebuilt image is published.

The loader reference comes from local snapshot
`efdc6bdff768cfe149b0a39f978ddd921fd8eb39`, not the selected catalog revision.
Its `LargestWindow.h` is the production arithmetic helper; the accompanying
test covers placement, alignment, minimum size, overflow and reserved edges.
The format-2 candidate has not yet integrated this reference into its resolved
component build or ADT policy.

The firmware overlay references Mu local snapshot
`0b68d714579ed4bb366ddad4b951b810d3b58955` and paired Basecore snapshot
`9e56daa9e6a5cb32e4aff7b9c0f70b602d7887b5`. These are local archive-import
identities, not published upstream ancestry. The overlay's source manifest
records the archive SHA-256 and exclusions. ACPI/private input recipes and
complete public dependency pins remain outstanding.

Historical user reports establish the v1 menu and fixed-window allocation
failure, not v2 XNU entry or a desktop. The source and offline cust-v2 readback
support the largest-contiguous-window policy, but do not establish its new
device allocation. Do not bridge firmware reservations or inflate boot_args.

All end-to-end areas remain planned. Host arithmetic/protocol models are not
hardware tests. The build-readiness gate is intentionally retained; a complete
runtime migration, fresh build and matching device evidence are future work.
