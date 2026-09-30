/*
 * mod-offline-respawn-freeze
 *
 * Pauses creature respawn timers while nobody is playing.
 *
 * Background: creature_respawn.respawnTime is an absolute Unix time, so
 * respawn timers keep running even when nobody plays. The module compensates
 * two spans:
 *
 *   1. Server offline. At startup, pending respawn times are pushed back by
 *      the downtime.
 *   2. Server running, but no human player logged in. On logout the pending
 *      respawns are parked and their remaining time is recorded; on the next
 *      login they get exactly that remaining time back. Whatever the bots kill
 *      in the meantime is left alone and respawns normally.
 *
 * GameObjects (herbs, ore, chests) are not touched, nor are auctions, mail,
 * calendar and world time.
 *
 * See the module's README.md for the full reasoning.
 */

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
    constexpr char const* STATE_TABLE    = "mod_offline_respawn_freeze";
    constexpr char const* SNAPSHOT_TABLE = "mod_offline_respawn_freeze_snapshot";
    constexpr uint32 SECONDS_PER_DAY     = 86400;

    // Parking time during the absence. Deliberately equal to the one-year marker
    // that Map::SaveCreatureRespawnTime clamps to in instances with a reset
    // period anyway, so both cases end up with the same value.
    constexpr uint32 PARK_SECONDS        = 365 * SECONDS_PER_DAY;

    // Snapshots are written in chunks, not row by row.
    constexpr std::size_t INSERT_CHUNK   = 500;

    struct Settings
    {
        bool   enabled          = true;
        bool   dryRun           = false;
        bool   includeInstances = true;
        bool   trackSession     = true;
        uint32 heartbeatMs      = 60000;
        uint32 minAbsence       = 60;
        uint32 maxDowntimeDays  = 60;
        uint32 maxFutureDays    = 30;
        uint32 graceSeconds     = 300;
        uint32 maxSpawnTimeSecs = 1800;
    };

    Settings g_cfg;

    // Formats a duration readably so log lines need no mental arithmetic.
    std::string HumanDuration(int64 seconds)
    {
        if (seconds < 0)
            return "negative";

        int64 d = seconds / SECONDS_PER_DAY;
        int64 h = (seconds % SECONDS_PER_DAY) / 3600;
        int64 m = (seconds % 3600) / 60;
        int64 s = seconds % 60;

        std::string out;
        if (d) out += std::to_string(d) + "d ";
        if (d || h) out += std::to_string(h) + "h ";
        if (d || h || m) out += std::to_string(m) + "min ";
        out += std::to_string(s) + "s";
        return out;
    }

    std::string InstanceScope(std::string const& prefix)
    {
        return g_cfg.includeInstances ? "" : Acore::StringFormat(" AND {}instanceId = 0", prefix);
    }

    // Battlegrounds and arenas are discarded after the match anyway.
    bool InScope(Map const* map)
    {
        if (map->IsBattlegroundOrArena())
            return false;

        return g_cfg.includeInstances || map->GetInstanceId() == 0;
    }

    // Long respawns keep following real time. A rare spawn with 42 hours would
    // otherwise turn calendar time into play time and stay away for weeks with
    // a few hours of play per day. The same applies to event NPCs whose event
    // follows the calendar anyway.
    bool IsLongRunner(uint32 guid)
    {
        if (!g_cfg.maxSpawnTimeSecs)
            return false;

        CreatureData const* data = sObjectMgr->GetCreatureData(guid);
        return data && data->spawntimesecs > g_cfg.maxSpawnTimeSecs;
    }

    // Result of one pass, used only for the log line.
    struct ShiftResult
    {
        uint32 shifted     = 0; // entries actually processed
        uint32 inMemory    = 0; // of which on maps already loaded
        uint32 skippedLong = 0; // excluded by MaxSpawnTimeSecs
        uint32 changed     = 0; // changed from outside during the absence
    };

    std::string SkippedNote(ShiftResult const& res)
    {
        std::string out;
        if (res.skippedLong)
            out += Acore::StringFormat(", {} long respawns excluded", res.skippedLong);
        if (res.changed)
            out += Acore::StringFormat(", {} changed in the meantime", res.changed);
        return out;
    }

    // Startup path. No map exists yet at startup, so this works on the database
    // only. The caller passes a SELECT with the columns guid, instanceId,
    // mapId, respawnTime.
    ShiftResult ShiftRespawnTimes(std::string const& select, int64 shift)
    {
        ShiftResult res;

        QueryResult rows = CharacterDatabase.Query(select);
        if (!rows)
            return res;

        bool const apply = !g_cfg.dryRun;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        do
        {
            Field* f = rows->Fetch();
            uint32 const guid       = f[0].Get<uint32>();
            uint32 const instanceId = f[1].Get<uint32>();
            uint32 const oldTime    = f[3].Get<uint32>();

            if (IsLongRunner(guid))
            {
                ++res.skippedLong;
                continue;
            }

            if (apply)
                trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {}",
                              oldTime + static_cast<uint32>(shift), guid, instanceId);

            ++res.shifted;
        }
        while (rows->NextRow());

        if (apply)
            CharacterDatabase.CommitTransaction(trans);

        return res;
    }

    // Lower bound above which a value still counts as parked. The core may
    // shorten the parked value by a few seconds: when a corpse is looted,
    // Creature::AllLootRemovedFromCorpse subtracts the faster decay from
    // m_respawnTime, and that value reaches the table when the corpse is
    // removed. Anything above "parking time plus MaxFutureDays" still comes from
    // parking; real respawns never lie that far in the future.
    uint32 ParkedFloor(uint32 parkedTime)
    {
        return parkedTime - PARK_SECONDS + g_cfg.maxFutureDays * SECONDS_PER_DAY;
    }

    // Remaining time after parking. Whatever the core cut from the parked value
    // is also taken from the remaining time, exactly as without parking.
    uint32 RemainingAfterPark(uint32 remaining, uint32 parkedTime, time_t current)
    {
        uint32 const cut = current < static_cast<time_t>(parkedTime) ? static_cast<uint32>(parkedTime - current) : 0;
        return remaining > cut ? remaining - cut : 0;
    }

    // Sets parked entries to "now plus remaining time", without loaded maps.
    // Only for startup, when the session anchor has been turned off.
    void RestoreParkedInDb(uint32 now)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(
            "UPDATE creature_respawn r JOIN `{}` s ON s.guid = r.guid AND s.instanceId = r.instanceId "
            "SET r.respawnTime = {} + GREATEST(CAST(s.remaining AS SIGNED) - GREATEST(CAST(s.parkedTime AS SIGNED) - CAST(r.respawnTime AS SIGNED), 0), 0) "
            "WHERE r.respawnTime >= s.parkedTime - {} + {}",
            SNAPSHOT_TABLE, now, PARK_SECONDS, g_cfg.maxFutureDays * SECONDS_PER_DAY);
        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
        trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
        CharacterDatabase.CommitTransaction(trans);
    }

    // Counts logged-in humans. Bot sessions have no socket and identify
    // themselves through WorldSession::IsBot(); they are not counted.
    uint32 CountHumanPlayers(Player const* excluded)
    {
        uint32 count = 0;
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session || session->IsBot())
                continue;

            Player* player = session->GetPlayer();
            if (!player || player == excluded)
                continue;

            ++count;
        }

        return count;
    }

    bool IsHuman(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }
}

class OfflineRespawnFreezeWorldScript : public WorldScript
{
public:
    OfflineRespawnFreezeWorldScript() : WorldScript("OfflineRespawnFreezeWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_AFTER_UNLOAD_ALL_MAPS
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    // This hook on purpose: it runs after the database connections are up and
    // sObjectMgr knows the spawn data, but before the first map is created.
    // Maps read their respawn times on creation (Map::LoadRespawnTimes);
    // writing later would no longer affect the loaded state.
    void OnBeforeWorldInitialized() override
    {
        if (!_enabled)
            return;

        EnsureTables();
        ApplyDowntimeShift();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_enabled || g_cfg.heartbeatMs == 0)
            return;

        _sinceHeartbeat += diff;
        if (_sinceHeartbeat < g_cfg.heartbeatMs)
            return;

        _sinceHeartbeat = 0;
        // Asynchronous: the world tick must not wait for this.
        WriteState(static_cast<uint32>(GameTime::GetGameTime().count()), false, false);
    }

    // Last hook before the process ends, running after MapMgr::UnloadAll().
    // By then the core has written all pending respawn times, so the timestamp
    // matches the stored data exactly.
    void OnAfterUnloadAllMaps() override
    {
        if (!_enabled)
            return;

        uint32 now = static_cast<uint32>(GameTime::GetGameTime().count());
        WriteState(now, true, true);
        LOG_INFO("module", "[RespawnFreeze] Clean shutdown recorded.");
    }

private:
    bool   _enabled        = true;
    uint32 _sinceHeartbeat = 0;

    void LoadConfig()
    {
        g_cfg.enabled          = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.Enable", true);
        g_cfg.dryRun           = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.DryRun", false);
        g_cfg.includeInstances = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.IncludeInstances", true);
        g_cfg.trackSession     = sConfigMgr->GetOption<bool>  ("OfflineRespawnFreeze.FreezeWhileNoPlayerOnline", true);
        g_cfg.minAbsence       = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MinDowntimeSeconds", 60);
        g_cfg.maxDowntimeDays  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxDowntimeDays", 60);
        g_cfg.maxFutureDays    = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxFutureDays", 30);
        g_cfg.graceSeconds     = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.StartupGraceSeconds", 300);
        g_cfg.maxSpawnTimeSecs = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.MaxSpawnTimeSecs", 1800);

        uint32 heartbeat  = sConfigMgr->GetOption<uint32>("OfflineRespawnFreeze.HeartbeatSeconds", 60);
        g_cfg.heartbeatMs = heartbeat * 1000;

        if (g_cfg.maxFutureDays == 0)
        {
            LOG_WARN("module", "[RespawnFreeze] MaxFutureDays is 0, which would prevent any shift. Using 30.");
            g_cfg.maxFutureDays = 30;
        }

        _enabled = g_cfg.enabled;
    }

    // MySQL 8 has no ADD COLUMN IF NOT EXISTS, so columns are added
    // explicitly for existing installations.
    void EnsureColumn(char const* table, char const* column, char const* definition)
    {
        QueryResult hasColumn = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.COLUMNS "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = '{}'",
            table, column);

        if (hasColumn && (*hasColumn)[0].Get<uint64>() == 0)
        {
            CharacterDatabase.DirectExecute("ALTER TABLE `{}` ADD COLUMN `{}` {}", table, column, definition);
            LOG_INFO("module", "[RespawnFreeze] Table {} extended by column {}.", table, column);
        }
    }

    void EnsureTables()
    {
        // In code rather than an SQL file on purpose: two tables that must
        // exist before anything else runs.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`id` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`last_seen` INT UNSIGNED NOT NULL,"
            "`clean_shutdown` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_shift_rows` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`absence_start` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: last known server time'",
            STATE_TABLE);

        EnsureColumn(STATE_TABLE, "absence_start", "INT UNSIGNED NOT NULL DEFAULT 0");

        // Parked entries between the logout of the last human and the next
        // login. respawnTime is the original time, remaining the remaining time
        // at logout, parkedTime the parked value by which the login recognises
        // an unchanged entry.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`guid` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`respawnTime` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`mapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`instanceId` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`remaining` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`parkedTime` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`guid`, `instanceId`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-offline-respawn-freeze: entries parked at the logout of the last player'",
            SNAPSHOT_TABLE);

        EnsureColumn(SNAPSHOT_TABLE, "remaining", "INT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn(SNAPSHOT_TABLE, "parkedTime", "INT UNSIGNED NOT NULL DEFAULT 0");
    }

    void WriteState(uint32 lastSeen, bool cleanShutdown, bool synchronous)
    {
        std::string sql = Acore::StringFormat(
            "INSERT INTO `{}` (id, last_seen, clean_shutdown) VALUES (1, {}, {}) "
            "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = VALUES(clean_shutdown)",
            STATE_TABLE, lastSeen, cleanShutdown ? 1 : 0);

        if (synchronous)
            CharacterDatabase.DirectExecute(sql);
        else
            CharacterDatabase.Execute(sql);
    }

    void ApplyDowntimeShift()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        QueryResult state = CharacterDatabase.Query(
            "SELECT last_seen, clean_shutdown, absence_start FROM `{}` WHERE id = 1", STATE_TABLE);

        if (!state)
        {
            LOG_INFO("module", "[RespawnFreeze] First run, no earlier timestamp. Nothing is shifted.");
            WriteState(now, false, true);
            return;
        }

        Field* fields         = state->Fetch();
        uint32 const lastSeen = fields[0].Get<uint32>();
        bool   const wasClean = fields[1].Get<uint8>() != 0;
        uint32 absenceStart   = fields[2].Get<uint32>();

        if (absenceStart > 0 && !g_cfg.dryRun)
        {
            // Record from before parking existed: without remaining times it
            // can no longer be evaluated. The entries themselves were not
            // changed back then and simply keep running.
            QueryResult legacy = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `{}` WHERE parkedTime = 0", SNAPSHOT_TABLE);

            if (legacy && (*legacy)[0].Get<uint64>() > 0)
            {
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
                trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
                CharacterDatabase.CommitTransaction(trans);
                absenceStart = 0;
                LOG_INFO("module", "[RespawnFreeze] Discarded old record without parking times.");
            }
            // If the session anchor was turned off while one was set, the parked
            // creatures would stay away for a year. They get their remaining
            // time back from now on.
            else if (!g_cfg.trackSession)
            {
                RestoreParkedInDb(now);
                absenceStart = 0;
                LOG_INFO("module", "[RespawnFreeze] Session anchor is off; parked respawns continue with their remaining time from now on.");
            }
        }

        // A set anchor means the last human had already logged out at shutdown
        // and their entries are parked. The flat grace is not needed then,
        // because the login returns the exact remaining time to every parked
        // entry.
        bool const anchored = g_cfg.trackSession && absenceStart > 0;

        // Against a system clock set back: compute signed, otherwise the
        // unsigned subtraction would wrap around.
        int64 const downtime = static_cast<int64>(now) - static_cast<int64>(lastSeen);

        if (downtime < 0)
        {
            LOG_WARN("module", "[RespawnFreeze] Last timestamp lies in the future (system clock?). Nothing is shifted.");
            WriteState(now, false, true);
            return;
        }

        if (!wasClean)
            LOG_WARN("module", "[RespawnFreeze] No clean shutdown detected. Using the last heartbeat; the error is at most {} seconds.",
                     g_cfg.heartbeatMs / 1000);

        if (downtime < static_cast<int64>(g_cfg.minAbsence))
        {
            LOG_INFO("module", "[RespawnFreeze] Downtime {} is below the threshold. Nothing is shifted.",
                     HumanDuration(downtime));
            FinishStartup(now, absenceStart, 0, 0);
            return;
        }

        int64 const maxDowntime = static_cast<int64>(g_cfg.maxDowntimeDays) * SECONDS_PER_DAY;
        if (downtime > maxDowntime)
        {
            LOG_WARN("module", "[RespawnFreeze] Downtime {} exceeds the limit of {} days. Nothing is shifted.",
                     HumanDuration(downtime), g_cfg.maxDowntimeDays);
            FinishStartup(now, absenceStart, 0, 0);
            return;
        }

        // Time passes between the server start and the moment someone actually
        // stands in the world: start the client, log in, choose a character,
        // load the zone. That span would run against the respawns unchecked,
        // and bots are already killing meanwhile. Without an anchor the grace
        // compensates this as a flat value; with an anchor the player's entries
        // are parked and need no grace. It is added only here so it does not
        // distort the plausibility thresholds above.
        int64 const shift = downtime + (anchored ? 0 : static_cast<int64>(g_cfg.graceSeconds));

        //   respawnTime > lastSeen    leaves entries that are already due alone;
        //                             they would otherwise move into the future
        //                             and appear later.
        //   respawnTime < upperBound  protects the "now + YEAR" marker that
        //                             Map::SaveCreatureRespawnTime uses for
        //                             creatures that do not return before the
        //                             instance reset, and the parked entries.
        //   NOT EXISTS                keeps parked entries out even if the
        //                             server ran without players for a while.
        uint32 const upperBound = lastSeen + g_cfg.maxFutureDays * SECONDS_PER_DAY;

        std::string const select = Acore::StringFormat(
            "SELECT r.guid, r.instanceId, r.mapId, r.respawnTime FROM creature_respawn r "
            "WHERE r.respawnTime > {} AND r.respawnTime < {}{} AND NOT EXISTS ("
            "SELECT 1 FROM `{}` s WHERE s.guid = r.guid AND s.instanceId = r.instanceId)",
            lastSeen, upperBound, InstanceScope("r."), SNAPSHOT_TABLE);

        ShiftResult const res = ShiftRespawnTimes(select, shift);

        std::string const grace = anchored
            ? std::string()
            : Acore::StringFormat(" plus {} grace", HumanDuration(g_cfg.graceSeconds));

        LOG_INFO("module", "[RespawnFreeze] Downtime {}{}, giving a shift of {}. {} entries {}{}{}.",
                 HumanDuration(downtime), grace, HumanDuration(shift), res.shifted,
                 g_cfg.dryRun ? "would be affected" : "shifted",
                 SkippedNote(res), g_cfg.includeInstances ? "" : ", open world only");

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Dry run active: nothing is written. Set OfflineRespawnFreeze.DryRun = 0 to apply.");
            return;
        }

        if (anchored)
            LOG_INFO("module", "[RespawnFreeze] Entries from the last logout are parked and get their remaining time back at login.");

        FinishStartup(now, absenceStart, shift, res.shifted);
    }

    // Updates the state. The anchor moves by the same span so the absence
    // reported at login only contains the time with a running server and
    // MaxDowntimeDays does not count twice.
    void FinishStartup(uint32 now, uint32 absenceStart, int64 shift, uint32 rows)
    {
        if (g_cfg.dryRun)
            return;

        uint32 const movedAnchor = absenceStart ? static_cast<uint32>(absenceStart + shift) : 0;

        // Without shifted entries the diagnostic fields of the last effective
        // run stay; only timestamp and anchor are updated.
        if (!rows)
        {
            CharacterDatabase.DirectExecute(
                "INSERT INTO `{}` (id, last_seen, clean_shutdown, absence_start) VALUES (1, {}, 0, {}) "
                "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = 0, "
                "absence_start = VALUES(absence_start)",
                STATE_TABLE, now, movedAnchor);
            return;
        }

        CharacterDatabase.DirectExecute(
            "INSERT INTO `{}` (id, last_seen, clean_shutdown, last_shift, last_shift_rows, absence_start) "
            "VALUES (1, {}, 0, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE last_seen = VALUES(last_seen), clean_shutdown = 0, "
            "last_shift = VALUES(last_shift), last_shift_rows = VALUES(last_shift_rows), "
            "absence_start = VALUES(absence_start)",
            STATE_TABLE, now, shift, rows, movedAnchor);
    }
};

class OfflineRespawnFreezePlayerScript : public PlayerScript
{
public:
    OfflineRespawnFreezePlayerScript() : PlayerScript("OfflineRespawnFreezePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_cfg.enabled || !g_cfg.trackSession || !IsHuman(player))
            return;

        // Only the first human unfreezes the world.
        if (CountHumanPlayers(player) > 0)
            return;

        Release();
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!g_cfg.enabled || !g_cfg.trackSession || !IsHuman(player))
            return;

        // The hook runs while the player's own session is still listed.
        if (CountHumanPlayers(player) > 0)
            return;

        Freeze();
    }

private:
    struct ParkedEntry
    {
        uint32 guid;
        uint32 instanceId;
        uint32 mapId;
        uint32 respawnTime;
        uint32 remaining;
        uint32 parkedTime;
    };

    // Parks all pending respawns far in the future and records their remaining
    // time.
    //
    // Only recording is not enough: the map's respawn queue
    // (Map::ProcessRespawns) spawns due creatures as soon as their grid is
    // loaded and deletes the entry. Grids stay loaded for minutes after
    // logout, near bots permanently, so there would be nothing left to shift
    // at login.
    //
    // Sources are the in-memory state for loaded maps and the database for all
    // others. Creatures currently lying as corpses have no entry yet; they are
    // captured through the creature object.
    void Freeze()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        if (g_cfg.dryRun)
        {
            LOG_INFO("module", "[RespawnFreeze] Dry run: last player logged out, nothing is parked.");
            return;
        }

        uint32 const upperBound = now + g_cfg.maxFutureDays * SECONDS_PER_DAY;
        uint32 const parkUntil  = now + PARK_SECONDS;

        // Collect first, then write: DoForAllMaps holds the MapMgr lock, and
        // SaveCreatureRespawnTime changes the table being iterated.
        std::vector<Map*> maps;
        sMapMgr->DoForAllMaps([&maps](Map* map)
        {
            if (InScope(map))
                maps.push_back(map);
        });

        ShiftResult res;
        std::vector<ParkedEntry> parked;
        std::set<std::pair<uint32, uint32>> loaded;

        auto isOpen = [&](uint32 guid, time_t respawnTime)
        {
            if (respawnTime <= static_cast<time_t>(now) || respawnTime >= static_cast<time_t>(upperBound))
                return false;

            if (IsLongRunner(guid))
            {
                ++res.skippedLong;
                return false;
            }

            return true;
        };

        for (Map* map : maps)
        {
            loaded.emplace(map->GetId(), map->GetInstanceId());

            std::map<uint32, time_t> open;
            for (auto const& [guid, respawnTime] : map->GetCreatureRespawnTimes())
                if (isOpen(guid, respawnTime))
                    open.emplace(guid, respawnTime);

            for (auto const& [guid, creature] : map->GetCreatureBySpawnIdStore())
            {
                if (!creature || creature->IsAlive() || open.count(guid))
                    continue;

                CreatureData const* data = sObjectMgr->GetCreatureData(guid);
                if (!data || !data->dbData)
                    continue;

                if (isOpen(guid, creature->GetRespawnTime()))
                    open.emplace(guid, creature->GetRespawnTime());
            }

            for (auto const& [guid, respawnTime] : open)
            {
                time_t stored = parkUntil;
                map->SaveCreatureRespawnTime(guid, stored);

                auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    if (Creature* creature = itr->second)
                        if (!creature->IsAlive())
                            creature->SetRespawnTime(static_cast<uint32>(stored - now));

                parked.push_back({ guid, map->GetInstanceId(), map->GetId(), static_cast<uint32>(respawnTime),
                                   static_cast<uint32>(respawnTime - now), static_cast<uint32>(stored) });
                ++res.inMemory;
            }
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        // Maps that are not loaded read their times from the database on the
        // next creation; an UPDATE there is enough.
        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT guid, instanceId, mapId, respawnTime FROM creature_respawn "
                "WHERE respawnTime > {} AND respawnTime < {}{}",
                now, upperBound, InstanceScope("")))
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const guid        = f[0].Get<uint32>();
                uint32 const instanceId  = f[1].Get<uint32>();
                uint32 const mapId       = f[2].Get<uint32>();
                uint32 const respawnTime = f[3].Get<uint32>();

                if (loaded.count({ mapId, instanceId }) || !isOpen(guid, respawnTime))
                    continue;

                trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {} AND respawnTime = {}",
                              parkUntil, guid, instanceId, respawnTime);

                parked.push_back({ guid, instanceId, mapId, respawnTime, respawnTime - now, parkUntil });
            }
            while (rows->NextRow());
        }

        // No DELETE first: if an earlier record without a login in between is
        // still there, its entries are already parked and would not show up
        // here. They must stay in the record, or they would never get their
        // remaining time back.
        for (std::size_t start = 0; start < parked.size(); start += INSERT_CHUNK)
        {
            std::string values;
            std::size_t const end = std::min(parked.size(), start + INSERT_CHUNK);
            for (std::size_t i = start; i < end; ++i)
            {
                ParkedEntry const& e = parked[i];
                if (!values.empty())
                    values += ',';
                values += Acore::StringFormat("({},{},{},{},{},{})",
                                              e.guid, e.respawnTime, e.mapId, e.instanceId, e.remaining, e.parkedTime);
            }

            trans->Append("REPLACE INTO `{}` (guid, respawnTime, mapId, instanceId, remaining, parkedTime) VALUES {}",
                          SNAPSHOT_TABLE, values);
        }

        trans->Append("UPDATE `{}` SET absence_start = IF(absence_start = 0, {}, absence_start) WHERE id = 1", STATE_TABLE, now);
        CharacterDatabase.CommitTransaction(trans);

        res.shifted = static_cast<uint32>(parked.size());
        LOG_INFO("module", "[RespawnFreeze] Last player logged out. {} respawn times parked, {} of them on loaded maps{}.",
                 res.shifted, res.inMemory, SkippedNote(res));
    }

    // Gives every parked entry its remaining time back, counted from now.
    // Entries that no longer carry the parked value were changed from outside
    // in the meantime (for example by a GM command) and are left alone.
    void Release()
    {
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        QueryResult state = CharacterDatabase.Query(
            "SELECT absence_start FROM `{}` WHERE id = 1", STATE_TABLE);

        if (!state)
            return;

        uint32 const absenceStart = (*state)[0].Get<uint32>();
        if (absenceStart == 0)
            return;

        int64 const absence = static_cast<int64>(now) - static_cast<int64>(absenceStart);

        // After a very long absence nobody expects a particular creature any
        // more; everything parked then appears immediately, as if time had
        // passed normally.
        int64 const maxAbsence = static_cast<int64>(g_cfg.maxDowntimeDays) * SECONDS_PER_DAY;
        bool const tooLong = absence > maxAbsence;

        if (tooLong)
            LOG_WARN("module", "[RespawnFreeze] Absence {} exceeds the limit of {} days. Parked respawns appear immediately.",
                     HumanDuration(absence), g_cfg.maxDowntimeDays);

        ShiftResult res;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT guid, instanceId, mapId, remaining, parkedTime FROM `{}`", SNAPSHOT_TABLE))
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 const guid       = f[0].Get<uint32>();
                uint32 const instanceId = f[1].Get<uint32>();
                uint32 const mapId      = f[2].Get<uint32>();
                uint32 const remaining  = f[3].Get<uint32>();
                uint32 const parkedTime = f[4].Get<uint32>();

                uint32 const floor = ParkedFloor(parkedTime);

                if (Map* map = sMapMgr->FindMap(mapId, instanceId))
                {
                    time_t const current = map->GetCreatureRespawnTime(guid);
                    if (current < static_cast<time_t>(floor))
                    {
                        ++res.changed;
                        continue;
                    }

                    time_t stored = tooLong ? now : now + RemainingAfterPark(remaining, parkedTime, current);
                    map->SaveCreatureRespawnTime(guid, stored);

                    // Dead creature objects, corpses included, carry the
                    // parked value themselves, slightly shortened for looted
                    // corpses. Creatures killed by bots since then carry a real
                    // time and are left alone.
                    auto bounds = map->GetCreatureBySpawnIdStore().equal_range(guid);
                    for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    {
                        Creature* creature = itr->second;
                        if (!creature || creature->IsAlive() || creature->GetRespawnTime() < static_cast<time_t>(floor))
                            continue;

                        uint32 const own = tooLong ? 0 : RemainingAfterPark(remaining, parkedTime, creature->GetRespawnTime());
                        creature->SetRespawnTime(own);
                    }

                    ++res.inMemory;
                }
                else if (tooLong)
                {
                    trans->Append("UPDATE creature_respawn SET respawnTime = {} WHERE guid = {} AND instanceId = {} AND respawnTime >= {}",
                                  now, guid, instanceId, floor);
                }
                else
                {
                    trans->Append("UPDATE creature_respawn SET respawnTime = {} + GREATEST({} - GREATEST({} - CAST(respawnTime AS SIGNED), 0), 0) "
                                  "WHERE guid = {} AND instanceId = {} AND respawnTime >= {}",
                                  now, static_cast<int64>(remaining), static_cast<int64>(parkedTime), guid, instanceId, floor);
                }

                ++res.shifted;
            }
            while (rows->NextRow());
        }

        trans->Append("DELETE FROM `{}`", SNAPSHOT_TABLE);
        if (res.shifted)
            trans->Append("UPDATE `{}` SET absence_start = 0, last_shift = {}, last_shift_rows = {} WHERE id = 1",
                          STATE_TABLE, std::max<int64>(absence, 0), res.shifted);
        else
            trans->Append("UPDATE `{}` SET absence_start = 0 WHERE id = 1", STATE_TABLE);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module", "[RespawnFreeze] Absence {}. {} respawn times continued with their remaining time, {} of them on loaded maps{}.",
                 HumanDuration(absence), res.shifted, res.inMemory, SkippedNote(res));
    }
};

void AddOfflineRespawnFreezeScripts()
{
    new OfflineRespawnFreezeWorldScript();
    new OfflineRespawnFreezePlayerScript();
}
