#include "../ReadRecovery.h"
#include <cassert>
#include <initializer_list>
#include <vector>
using native_mtkufs::ReadResult;

static void scenario(bool enabled, std::initializer_list<ReadResult> replies,
                     bool resetOK, bool expected, unsigned reads, unsigned resets)
{
  std::vector<ReadResult> values(replies);
  unsigned at = 0, recoveries = 0;
  bool result = native_mtkufs::readWithRecovery(enabled,
      [&]() { assert(at < values.size()); return values[at++]; },
      [&]() { ++recoveries; return resetOK; });
  assert(result == expected && at == reads && recoveries == resets);
}

int main()
{
  using R = ReadResult;
  scenario(true, {R::success}, true, true, 1, 0);
  scenario(false, {R::transport}, true, false, 1, 0);
  scenario(true, {R::failure}, true, false, 1, 0);
  scenario(true, {R::unitAttention}, true, false, 1, 0);
  scenario(true, {R::transport}, false, false, 1, 1);
  scenario(true, {R::transport, R::success}, true, true, 2, 1);
  scenario(true, {R::transport, R::transport}, true, false, 2, 1);
  scenario(true, {R::transport, R::failure}, true, false, 2, 1);
  scenario(true, {R::transport, R::unitAttention, R::success}, true, true, 3, 1);
  scenario(true, {R::transport, R::unitAttention, R::unitAttention}, true, false, 3, 1);
}
