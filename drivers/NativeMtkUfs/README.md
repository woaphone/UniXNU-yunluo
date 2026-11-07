# MT6789 native UFS driver

This directory publishes the previously local yunluo (Redmi Pad, MT6789)
UniXNU UFS driver. It is not the MT6989/rothko driver.

The first publication imports the preserved UniXNU checkpoint `2f7e5ed` without
changing the C++ file. Its SHA-256 is
`fa218b9dd276778dee46ed1b3505849d24bc68db4015c188d0cdbe1f0324703b`.
That identifier is a local recovery checkpoint, not an upstream release tag.

The implementation uses UFSHCI at `0x11270000`, fixed user LUN 2 and polled
completion. The OS media is
read-only; the implementation also has bounded Data/Log write paths. Do not
treat it as a completely read-only controller or use its board layout on
another device.

Earlier yunluo device logs established APFS root access. Publishing this source
does not establish that every optional path or another firmware combination is
qualified. This is not a standalone kext; it uses the matching UniXNU XNU and
storage-family ABI. The repository's qualification configuration is not the
full recovered system build.

## Bounded READ recovery

The second publication adds the locally qualified change from checkpoint
`ba139a18248b65dc7c890bfe35e4bc5e87623112`. Apart from correcting the historical
header comments, the C++ implementation matches the source used by the
2026-11-07 `cust-yunluo-heliodarwin-v1` candidate.

Recovery is disabled by default; the candidate enables it with
`yunluo-ufs-read-recover=1`. Only READ transport failures enter recovery. The
driver confirms that the host has stopped and old requests are quiescent
before reusing DMA buffers, re-establishes the host/link/NOP path, validates
the original LU geometry and write protection, then retries the read. The
driver limits recovery to two attempts per instance. WRITE requests are not
blindly replayed.

The host test exercises the production `ReadRecovery.h` policy, including
disabled recovery, non-transport failures, failed recovery, success, repeated
transport failure and the bounded UNIT ATTENTION retry:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer drivers/NativeMtkUfs/Test/read-recovery.cpp \
  -o /tmp/yunluo-ufs-read-recovery-test
/tmp/yunluo-ufs-read-recovery-test
```

Use a new output filename when preserving a previous test executable. These
tests do not emulate UFS hardware or prove recovery on a physical device.

## Build and integration

The repository now provides `scripts/build-native-mtk-ufs.py`, derived from the
archive builder used for the recovered system. It replays a real
`IOPlatformExpert.cpo.json` from a matching XNU build, adds the prepared storage
ABI headers, compiles with `-Werror`, and verifies an ARM64/iOS 17 Mach-O object
before creating the archive. It does not download or redistribute the SDK,
IPSW, original storage-family binary or generated private ABI headers.

```sh
python3 scripts/build-native-mtk-ufs.py \
  --tree /absolute/path/to/prepared-yunluo-tree \
  --recipe /absolute/path/to/IOPlatformExpert.cpo.json \
  --output-dir /absolute/path/to/new-ufs-build
```

The matching toolchain must provide the compiler named in that recipe and
`xcrun ar`. The output contains `libNativeMtkUfs.a`, its object, the
compiler command/log and a SHA-256 report. Existing output directories are
refused so a new run cannot silently replace a qualified archive.

Link that archive through the recovered kernel's `XNU_NATIVE_MTKUFS` path,
with its matching XNU personalities, APFS/Data/log hooks and original storage
bindings. See [`configs/yunluo_ufs.config`](../../configs/yunluo_ufs.config) for
the optional selection fragment. Rebuild the archive whenever its source
changes; do not select an old archive merely because its filename matches.

The public default `yunluo_defconfig` remains a qualification scaffold. This
source publication does **not** enable UFS automatically in that incomplete
profile or claim a clean full-system Format 2 build. The helper qualifies the
component against an already prepared matching tree; it is not a substitute
for preparing and verifying the complete kernel and firmware build.

## Attribution and verification boundary

The recovery work was informed by
[HelioDarwin](https://github.com/squeeeezy/HelioDarwin), especially patch 0186
at revision `e04035ebe905a7d41926ee4582a601cb114f8bc4`. See [NOTICE](NOTICE) and
[LICENSE.HelioDarwin](LICENSE.HelioDarwin) for attribution. Same-SoC reference
code is not evidence that another board layout or iOS version works here.

The source and recovery policy have local compiler/host-test qualification.
The recovery addition has no new device-run qualification. OS and bounded
Data/Log writes retain their existing behavior; do not use tests or a
successful archive build as permission to flash a device.

License: BSD-2-Clause-Patent; see [LICENSE](LICENSE).
