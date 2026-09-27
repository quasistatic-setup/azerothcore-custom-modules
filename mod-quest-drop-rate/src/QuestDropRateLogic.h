/*
 * mod-quest-drop-rate: reine Rechenlogik ohne Core-Abhängigkeiten, damit sie
 * sich auch außerhalb des Servers prüfen lässt.
 */

#ifndef MOD_QUEST_DROP_RATE_LOGIC_H
#define MOD_QUEST_DROP_RATE_LOGIC_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace QuestDropRate
{
    // Ab diesem Wert gibt der Core einen Eintrag ohne Würfeln aus.
    constexpr float MAX_CHANCE = 100.0f;

    // Unzulässige Faktoren (unter 1, NaN, unendlich) wirken neutral.
    inline bool IsValidMultiplier(float multiplier)
    {
        return std::isfinite(multiplier) && multiplier >= 1.0f;
    }

    // Beschreibt einen Loot-Eintrag so, wie ihn der Hook OnItemRoll sieht.
    struct Entry
    {
        bool needsQuest;
        int32_t reference;
        uint8_t groupId;
        // Eintrag liegt in einer Gruppe, die auch normale Gegenstände enthält.
        bool inMixedGroup;
    };

    inline bool Applies(Entry const& entry)
    {
        if (!entry.needsQuest || entry.reference != 0)
            return false;

        // In einer Gruppe würfelt der Core die Einträge nacheinander gegen
        // einen gemeinsamen Wurf. Mehr Chance für den Questgegenstand hieße
        // weniger für die übrigen, daher bleiben gemischte Gruppen unberührt.
        return entry.groupId == 0 || !entry.inMixedGroup;
    }

    inline float Scale(float chance, float multiplier)
    {
        return std::min(chance * multiplier, MAX_CHANCE);
    }

    // Neue Chance für einen Eintrag; nicht betroffene bleiben unverändert.
    inline float Apply(bool enabled, float multiplier, Entry const& entry, float chance)
    {
        if (!enabled || !IsValidMultiplier(multiplier) || !Applies(entry))
            return chance;

        // Chance 0 kennzeichnet Gleichverteilung in Gruppen; solche Einträge
        // erreichen den Hook zwar nicht, bleiben aber auch hier sicher 0.
        if (chance <= 0.0f)
            return chance;

        return Scale(chance, multiplier);
    }
}

#endif
