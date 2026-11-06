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

The preserved device record establishes visible UEFI/XNU output, UFS LU2 and
APFS root, launchd, the activation page, usable touch, eight-core MMIO IPI and
a 2 GiB XNU owned-memory window. Setup initialization later reached PID1
SIGBUS, and four-corner touch calibration, GPU, audio, networking and complete
power management remain open.

The public source tree still has to reproduce the full runtime archive set and
the current split boot/cust layout. Host arithmetic and protocol models remain
host evidence; the named device milestones are documented separately in
`device-progress-2026-10-29.md`.
