# MT6789 native UFS driver

This directory publishes the previously local yunluo (Redmi Pad, MT6789)
UniXNU UFS driver. It is not the MT6989/rothko driver.

The first publication imports the preserved UniXNU checkpoint `2f7e5ed` without
changing the C++ file. Its SHA-256 is
`fa218b9dd276778dee46ed1b3505849d24bc68db4015c188d0cdbe1f0324703b`.
That identifier is a local recovery checkpoint, not an upstream release tag.

The implementation uses UFSHCI at `0x11270000`, fixed user LUN 2 and polled
completion. Historical comments at the top describe early bring-up: they are
not an accurate description of the later write policy. The OS media is
read-only; the implementation also has bounded Data/Log write paths. Do not
treat it as a completely read-only controller or use its board layout on
another device.

Earlier yunluo device logs established APFS root access. Publishing this source
does not establish that every optional path or another firmware combination is
qualified. This is not a standalone kext; it uses the matching UniXNU XNU and
storage-family ABI. The repository's qualification configuration is not the
full recovered system build.

License: BSD-2-Clause-Patent; see [LICENSE](LICENSE).
