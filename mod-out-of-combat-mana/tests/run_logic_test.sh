#!/usr/bin/env bash
# Checks src/OutOfCombatManaLogic.h without a server and without the core.
#
# The test is deliberately not a .cpp file in the module: CMake collects the
# whole module tree into the worldserver library, and a second main() would
# break the build.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat > "$work/test.cpp" <<'CPP'
#include "OutOfCombatManaLogic.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using namespace OutOfCombatMana;
static bool eq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Sum of the core share and the module share over many small steps, as the
// server delivers them; both with their own fraction like Player::Regenerate.
static uint32_t Simulate(float regen, float rate, float multiplier, uint32_t steps, uint32_t diff)
{
    float coreFraction = 0.0f, modFraction = 0.0f;
    uint32_t total = 0;
    for (uint32_t i = 0; i < steps; ++i)
    {
        float core = regen * rate * 0.001f * diff + coreFraction;
        uint32_t coreWhole = static_cast<uint32_t>(core);
        coreFraction = core - coreWhole;
        total += coreWhole + TakeWhole(modFraction, Bonus(regen, rate, diff, multiplier));
    }
    return total;
}

int main()
{
    // Factor
    assert(IsValidMultiplier(1.0f) && IsValidMultiplier(2.0f));
    assert(!IsValidMultiplier(0.5f) && !IsValidMultiplier(NAN) && !IsValidMultiplier(INFINITY));

    // Who gets the bonus
    assert(Applies(false, false));                      // real player, alone
    assert(Applies(false, true));
    assert(Applies(true, true));                        // bot with a real player in group or raid
    assert(!Applies(true, false));                      // random bot without a real player

    // Rate as in the core, with and without the low-level boost
    assert(eq(ManaRate(1.5f, false, 10), 1.5f));
    assert(eq(ManaRate(1.5f, true, 80), 1.5f));
    assert(eq(ManaRate(1.5f, true, 10), 1.5f * (2.066f - 0.66f)));
    assert(eq(ManaRate(1.5f, true, 15), 1.5f));

    // The bonus is the share above 1: 10 mana/s, rate 1.5, 1000 ms
    assert(eq(Bonus(10.0f, 1.5f, 1000, 2.0f), 15.0f));
    assert(eq(Bonus(10.0f, 1.5f, 1000, 3.0f), 30.0f));
    assert(eq(Bonus(10.0f, 1.5f, 1000, 1.0f), 0.0f));
    assert(eq(Bonus(10.0f, 1.5f, 1000, 0.5f), 0.0f));
    assert(eq(Bonus(10.0f, 1.5f, 1000, NAN), 0.0f));
    assert(eq(Bonus(0.0f, 1.5f, 1000, 2.0f), 0.0f));    // no regeneration, no bonus either
    assert(eq(Bonus(-1.0f, 1.5f, 1000, 2.0f), 0.0f));
    assert(eq(Bonus(10.0f, 1.5f, 0, 2.0f), 0.0f));

    // Fractions are not lost
    float f = 0.0f;
    assert(TakeWhole(f, 0.4f) == 0 && eq(f, 0.4f));
    assert(TakeWhole(f, 0.7f) == 1 && eq(f, 0.1f));
    assert(TakeWhole(f, 2.95f) == 3 && eq(f, 0.05f));
    assert(TakeWhole(f, 0.0f) == 0 && eq(f, 0.05f));

    // Over 60 s in 10 ms steps: factor 1 gives exactly the core value,
    // factor 2 twice that (up to rounding of the fractions)
    uint32_t base = Simulate(7.3f, 1.5f, 1.0f, 6000, 10);
    uint32_t doubled = Simulate(7.3f, 1.5f, 2.0f, 6000, 10);
    assert(base >= 656 && base <= 657);                 // 7.3 * 1.5 * 60 = 657
    assert(doubled >= 2 * base - 2 && doubled <= 2 * base + 2);
    // Five-second rule: an interrupted value of 0 stays 0
    assert(Simulate(0.0f, 1.5f, 2.0f, 6000, 10) == 0);

    std::puts("OutOfCombatManaLogic: all checks passed");
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -UNDEBUG -I "$here/../src" "$work/test.cpp" -o "$work/test"
"$work/test"
