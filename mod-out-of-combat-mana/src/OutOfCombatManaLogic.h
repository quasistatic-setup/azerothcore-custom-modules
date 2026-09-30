/*
 * mod-out-of-combat-mana: pure calculation logic without core dependencies, so
 * it can be tested outside the server.
 */

#ifndef MOD_OUT_OF_COMBAT_MANA_LOGIC_H
#define MOD_OUT_OF_COMBAT_MANA_LOGIC_H

#include <cmath>
#include <cstdint>

namespace OutOfCombatMana
{
    // Invalid factors (below 1, NaN, infinite) have no effect.
    inline bool IsValidMultiplier(float multiplier)
    {
        return std::isfinite(multiplier) && multiplier >= 1.0f;
    }

    // How often the module checks per bot whether a real player is in its group.
    constexpr uint32_t BOT_RECHECK_MS = 1000;

    // Real players always; bots only while a real player is in their group or
    // raid. Free-roaming random bots stay unchanged.
    inline bool Applies(bool isBot, bool groupHasRealPlayer)
    {
        return !isBot || groupHasRealPlayer;
    }

    // Rate as in Player::Regenerate(POWER_MANA): Rate.Mana, below level 15 with
    // the optional boost from PlayerStart.LowLevelRegenBoost.
    inline float ManaRate(float rateMana, bool lowLevelBoost, uint32_t level)
    {
        if (lowLevelBoost && level < 15)
            return rateMana * (2.066f - (level * 0.066f));

        return rateMana;
    }

    // Extra mana for one update step. The core itself grants
    // regenPerSecond * manaRate * 0.001 * diffMs; the module adds the share
    // above 1 so the sum is exactly the multiplier times that.
    inline float Bonus(float regenPerSecond, float manaRate, uint32_t diffMs, float multiplier)
    {
        if (!IsValidMultiplier(multiplier) || multiplier == 1.0f || regenPerSecond <= 0.0f || manaRate <= 0.0f)
            return 0.0f;

        return regenPerSecond * manaRate * 0.001f * diffMs * (multiplier - 1.0f);
    }

    // Adds bonus to the fraction from earlier steps and returns the whole
    // points; the remainder stays for the next step.
    inline uint32_t TakeWhole(float& fraction, float bonus)
    {
        if (bonus > 0.0f)
            fraction += bonus;

        if (!(fraction >= 1.0f))
            return 0;

        uint32_t whole = static_cast<uint32_t>(fraction);
        fraction -= static_cast<float>(whole);
        return whole;
    }
}

#endif
