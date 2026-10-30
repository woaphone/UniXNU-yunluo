# DarwinBoot largest-window source reference

`DarwinBoot/` is a source-only export of the loader used to prepare the cust-v2
offline candidate. See `source-manifest.json` for byte hashes and original
local snapshot `efdc6bdff768cfe149b0a39f978ddd921fd8eb39`.

This source reference is **not yet selected by the format-2 board build**.
The candidate still uses the catalog's DarwinBoot. Complete component/feed,
ADT and archive integration before claiming a reproducible cust-v2 rebuild.

The optional little-endian u32 `/chosen/darwinboot-largest-window = 1` selects
the largest real, adjacent, non-runtime ConventionalMemory window after
kernel placement and 2 MiB alignment. It never bridges a reserved region.
Only a successful AllocatePages result updates MemorySize and MemorySizeActual.
The default fixed-window policy remains unchanged without the property.

Compile the production arithmetic helper on a host:

```sh
mkdir -p out
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I loader/DarwinBoot/DarwinBootPkg/Driver \
  tests/largest-window.c -o out/largest-window
./out/largest-window
```

Use a new output path for each run. This validates arithmetic, not successful
device allocation or XNU startup. Preserve `DarwinBoot/LICENSE`.
