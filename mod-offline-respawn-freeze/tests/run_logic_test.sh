#!/usr/bin/env bash
# Tests the production respawn policy without a server or database.
# Keep the test main outside the module tree, which CMake collects recursively.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat > "$work/test.cpp" <<'CPP'
#include "RespawnFreezePolicy.h"
#include <cassert>
#include <cstdio>
#include <limits>

using OfflineRespawnFreeze::IsLongRunner;

int main()
{
    // Ordinary outdoor mobs and the configured boundary still freeze.
    assert(!IsLongRunner(0, 300, 1800));
    assert(!IsLongRunner(0, 1800, 1800));
    assert(IsLongRunner(0, 1801, 1800));

    // Outdoor rare and event spawns keep their real-time respawns.
    assert(IsLongRunner(0, 7200, 1800));
    assert(IsLongRunner(0, 42 * 3600, 1800));
    assert(IsLongRunner(0, 12 * 3600, 1800));

    // Blackrock Depths spawn delays: 2 h, 12 h and 24 h.
    assert(!IsLongRunner(1, 7200, 1800));
    assert(!IsLongRunner(1, 43200, 1800));
    assert(!IsLongRunner(1, 86400, 1800));
    assert(!IsLongRunner(99, 7 * 86400, 1800));

    // Zero disables the outdoor limit; custom limits retain the same boundary.
    assert(!IsLongRunner(0, 42 * 3600, 0));
    assert(!IsLongRunner(1, 7200, 0));
    assert(!IsLongRunner(0, 7200, 7200));
    assert(IsLongRunner(0, 7201, 7200));
    assert(!IsLongRunner(1, 7201, 7200));
    assert(!IsLongRunner(std::numeric_limits<uint32_t>::max(), 7200, 1800));
    std::puts("RespawnFreezePolicy: 16 checks passed");
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -UNDEBUG -I "$here/../src" "$work/test.cpp" -o "$work/test"
"$work/test"
