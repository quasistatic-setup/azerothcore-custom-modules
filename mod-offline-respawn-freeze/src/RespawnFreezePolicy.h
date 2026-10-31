/*
 * mod-offline-respawn-freeze: respawn selection without core dependencies.
 */

#ifndef MOD_OFFLINE_RESPAWN_FREEZE_POLICY_H
#define MOD_OFFLINE_RESPAWN_FREEZE_POLICY_H

#include <cstdint>

namespace OfflineRespawnFreeze
{
    // Rare open-world spawns follow real time. Instanced creatures are exempt:
    // ordinary dungeon trash can have a respawn delay of several hours.
    constexpr bool IsLongRunner(uint32_t instanceId, uint32_t spawnTimeSecs, uint32_t maxSpawnTimeSecs)
    {
        return instanceId == 0 && maxSpawnTimeSecs != 0 && spawnTimeSecs > maxSpawnTimeSecs;
    }
}

#endif
