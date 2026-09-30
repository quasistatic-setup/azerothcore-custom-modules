/*
 * mod-out-of-combat-mana: reine Rechenlogik ohne Core-Abhängigkeiten, damit
 * sie sich auch außerhalb des Servers prüfen lässt.
 */

#ifndef MOD_OUT_OF_COMBAT_MANA_LOGIC_H
#define MOD_OUT_OF_COMBAT_MANA_LOGIC_H

#include <cmath>
#include <cstdint>

namespace OutOfCombatMana
{
    // Unzulässige Faktoren (unter 1, NaN, unendlich) wirken neutral.
    inline bool IsValidMultiplier(float multiplier)
    {
        return std::isfinite(multiplier) && multiplier >= 1.0f;
    }

    // So oft prüft das Modul je Bot, ob ein echter Spieler in seiner Gruppe ist.
    constexpr uint32_t BOT_RECHECK_MS = 1000;

    // Echte Spieler immer; Bots nur, wenn ein echter Spieler in ihrer Gruppe
    // oder ihrem Raid ist. Frei laufende RandomBots bleiben unverändert.
    inline bool Applies(bool isBot, bool groupHasRealPlayer)
    {
        return !isBot || groupHasRealPlayer;
    }

    // Rate wie in Player::Regenerate(POWER_MANA): Rate.Mana, unter Stufe 15
    // mit der optionalen Anhebung aus PlayerStart.LowLevelRegenBoost.
    inline float ManaRate(float rateMana, bool lowLevelBoost, uint32_t level)
    {
        if (lowLevelBoost && level < 15)
            return rateMana * (2.066f - (level * 0.066f));

        return rateMana;
    }

    // Zusätzliches Mana für einen Update-Schritt. Der Core gibt selbst
    // regenPerSecond * manaRate * 0.001 * diffMs; das Modul ergänzt den Anteil
    // über 1, sodass die Summe genau das Multiplier-fache ist.
    inline float Bonus(float regenPerSecond, float manaRate, uint32_t diffMs, float multiplier)
    {
        if (!IsValidMultiplier(multiplier) || multiplier == 1.0f || regenPerSecond <= 0.0f || manaRate <= 0.0f)
            return 0.0f;

        return regenPerSecond * manaRate * 0.001f * diffMs * (multiplier - 1.0f);
    }

    // Addiert bonus zum Bruchteil aus früheren Schritten und gibt die ganzen
    // Punkte zurück; der Rest bleibt für den nächsten Schritt stehen.
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
