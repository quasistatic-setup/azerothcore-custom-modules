/*
 * mod-quest-drop-rate: pure calculation logic without core dependencies, so it
 * can be tested outside the server.
 */

#ifndef MOD_QUEST_DROP_RATE_LOGIC_H
#define MOD_QUEST_DROP_RATE_LOGIC_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace QuestDropRate
{
    // From this value on the core hands out an entry without rolling.
    constexpr float MAX_CHANCE = 100.0f;

    // Invalid factors (below 1, NaN, infinite) have no effect.
    inline bool IsValidMultiplier(float multiplier)
    {
        return std::isfinite(multiplier) && multiplier >= 1.0f;
    }

    // Describes a loot entry as the OnItemRoll hook sees it.
    struct Entry
    {
        bool needsQuest;
        int32_t reference;
        uint8_t groupId;
        // Entry lies in a group that also contains normal items.
        bool inMixedGroup;
    };

    inline bool Applies(Entry const& entry)
    {
        if (!entry.needsQuest || entry.reference != 0)
            return false;

        // Within a group the core checks the entries one after another against
        // a shared roll. More chance for the quest item would mean less for
        // the others, so mixed groups are left alone.
        return entry.groupId == 0 || !entry.inMixedGroup;
    }

    inline float Scale(float chance, float multiplier)
    {
        return std::min(chance * multiplier, MAX_CHANCE);
    }

    // New chance for an entry; entries not affected stay unchanged.
    inline float Apply(bool enabled, float multiplier, Entry const& entry, float chance)
    {
        if (!enabled || !IsValidMultiplier(multiplier) || !Applies(entry))
            return chance;

        // Chance 0 marks equal distribution within groups; such entries never
        // reach the hook, but stay 0 here as well.
        if (chance <= 0.0f)
            return chance;

        return Scale(chance, multiplier);
    }
}

#endif
