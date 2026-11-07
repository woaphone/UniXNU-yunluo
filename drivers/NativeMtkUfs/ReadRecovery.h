// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded READ-only recovery, informed by HelioDarwin patch 0186.
#pragma once

namespace native_mtkufs {
enum class ReadResult { success, transport, unitAttention, failure };

// A failed write/query never enters this policy. Reissuing READ is permitted
// only after Recover proves quiescence and re-establishes the original LUN.
template <typename Read, typename Recover>
bool readWithRecovery(bool enabled, Read read, Recover recover)
{
  ReadResult result = read();
  if (result == ReadResult::success) return true;
  if (!enabled || result != ReadResult::transport || !recover()) return false;
  result = read();
  if (result == ReadResult::unitAttention) result = read();
  return result == ReadResult::success;
}
}
