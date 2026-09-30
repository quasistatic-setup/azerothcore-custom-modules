#!/usr/bin/env bash
# Checks src/QuestDropRateLogic.h without a server and without the core.
#
# The test is deliberately not a .cpp file in the module: CMake collects the
# whole module tree into the worldserver library, and a second main() would
# break the build.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat > "$work/test.cpp" <<'CPP'
#include "QuestDropRateLogic.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace QuestDropRate;
static bool eq(float a, float b) { return std::fabs(a - b) < 1e-4f; }
int main()
{
    Entry quest{true, 0, 0, false};
    Entry normal{false, 0, 0, false};
    Entry ref{true, 1055, 0, false};       // reference, even if flagged as quest
    Entry pureGroup{true, 0, 2, false};
    Entry mixedGroup{true, 0, 2, true};
    Entry mixedFlagUngrouped{true, 0, 0, true}; // without a group "mixed" does not matter

    assert(eq(Apply(true, 2.0f, quest, 40.0f), 80.0f));
    assert(eq(Apply(true, 2.0f, quest, 60.0f), 100.0f));
    assert(eq(Apply(true, 2.0f, quest, 100.0f), 100.0f));
    assert(eq(Apply(true, 3.5f, quest, 0.5f), 1.75f));
    assert(eq(Apply(true, 2.0f, quest, 0.0f), 0.0f));
    assert(eq(Apply(false, 2.0f, quest, 40.0f), 40.0f));
    assert(eq(Apply(true, 2.0f, normal, 40.0f), 40.0f));
    assert(eq(Apply(true, 2.0f, ref, 40.0f), 40.0f));
    assert(eq(Apply(true, 2.0f, pureGroup, 38.0f), 76.0f));
    assert(eq(Apply(true, 2.0f, mixedGroup, 50.0f), 50.0f));
    assert(eq(Apply(true, 2.0f, mixedFlagUngrouped, 40.0f), 80.0f));
    assert(eq(Apply(true, 1.0f, quest, 40.0f), 40.0f));
    assert(eq(Apply(true, 0.5f, quest, 40.0f), 40.0f));
    assert(eq(Apply(true, NAN, quest, 40.0f), 40.0f));
    assert(eq(Apply(true, INFINITY, quest, 40.0f), 40.0f));
    std::puts("QuestDropRateLogic: all checks passed");
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -UNDEBUG -I "$here/../src" "$work/test.cpp" -o "$work/test"
"$work/test"
