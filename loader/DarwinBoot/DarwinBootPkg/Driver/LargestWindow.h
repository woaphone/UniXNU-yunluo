// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_LARGEST_WINDOW_H
#define DARWINBOOT_LARGEST_WINDOW_H
#include "WindowSize.h"

static inline int DarwinLargestWindow(
    unsigned long long start, unsigned long long end,
    unsigned long long kernel_base, unsigned long long *address,
    unsigned long long *size) {
  const unsigned long long mask = 0x1fffffULL;
  unsigned long long candidate, bytes, offset = kernel_base & mask;
  *address = 0;
  *size = 0;
  if (end <= start || start > ~0ULL - mask) return 0;
  candidate = (start + mask) & ~mask;
  if (candidate > ~0ULL - offset) return 0;
  candidate += offset;
  if (candidate < start || candidate >= end) return 0;
  bytes = (end - candidate) & ~mask;
  if (bytes > 0x200000000ULL) bytes = 0x200000000ULL;
  if (!DarwinWindowSizeValid(bytes)) return 0;
  *address = candidate;
  *size = bytes;
  return 1;
}
#endif
