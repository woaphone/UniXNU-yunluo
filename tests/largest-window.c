// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "LargestWindow.h"

int main(void) {
  unsigned long long address, size;
  const unsigned long long kernel = 0xfffffff004004000ULL;
  assert(DarwinLargestWindow(0x140000000ULL, 0x1be000000ULL, kernel, &address, &size));
  assert(address == 0x140004000ULL && size == 0x7de00000ULL);
  assert(address + size <= 0x1be000000ULL);
  printf("reported run: base=%llx window=%llx bytes=%llu MiB=%llu\n",
         address, size, size, size / (1024ULL * 1024));
  assert(DarwinLargestWindow(0xc0000000ULL, 0xfe000000ULL, kernel, &address, &size));
  assert(address + size <= 0xfe000000ULL);
  assert(DarwinLargestWindow(0xfffff000ULL, 0x13ffff000ULL, kernel, &address, &size));
  assert(address + size <= 0x13ffff000ULL);
  assert(DarwinLargestWindow(0x140000000ULL, 0x1be000000ULL, 0, &address, &size));
  assert(size == 0x7e000000ULL);
  assert(!DarwinLargestWindow(0, 0, kernel, &address, &size));
  assert(!DarwinLargestWindow(0x20000000ULL, 0x10000000ULL, kernel, &address, &size));
  assert(!DarwinLargestWindow(~0ULL - 0x1000, ~0ULL, kernel, &address, &size));
  assert(!DarwinLargestWindow(0, 0x10000000ULL, kernel, &address, &size));
  assert(DarwinLargestWindow(0, 0x10004000ULL, kernel, &address, &size));
  assert(size == 0x10000000ULL);
  assert(DarwinLargestWindow(0, 0x400000000ULL, kernel, &address, &size));
  assert(size == 0x200000000ULL);
  assert(!(size & 0x1fffffULL));
  puts("largest-window checks passed");
  return 0;
}
