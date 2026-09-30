# mod-offline-respawn-freeze

mod-offline-respawn-freeze is an AzerothCore WotLK module that freezes creature respawn timers while the server is offline.
Optionally the timers also pause while the server runs but no human player is
online, so the world looks the way you left it.

## Purpose

`creature_respawn.respawnTime` is an absolute Unix time in AzerothCore. A creature
with ten minutes left until respawn therefore reappears even if nobody played in
between. For a server that only runs from time to time this is unwanted: the world
looks fully reset on every start.

The module compensates two spans:

1. **Server offline.** At startup, pending respawn times are pushed back by exactly
   the downtime. A creature with seven minutes left still has seven minutes left
   after twenty hours of downtime.
2. **Server running, nobody logged in.** When the last human player logs out, the
   pending respawns are parked and their remaining time is recorded; when the first
   human logs in, they get exactly that remaining time back. This covers the time a
   server typically runs before logging in and after logging out.

The second part can be turned off with
`OfflineRespawnFreeze.FreezeWhileNoPlayerOnline`.

## Why Playerbots do not interfere

With `AiPlayerbot.RandomBotAutologin = 1` random bots play as soon as the
worldserver runs, including while nobody sits at the computer. A global freeze
would also freeze their kills and depopulate the world.

So the module does not freeze globally; it only parks what the human player left
behind. On logout, pending respawns are set to one year in the future ("parked")
and their remaining time is stored in a separate table. On login, every entry that
still carries the parked value gets its remaining time back from now on. Whatever
the bots kill in the meantime is not parked and respawns normally.

No attribution by killer is needed. Bot sessions do not count as players
(`WorldSession::IsBot()`); only the last logout and the first login of a human
count. The approach therefore suits a single player or a single player group.

## Not affected

GameObjects (herbs, ore, chests) stay untouched; they live in a separate table the
module does not change. Auctions, mail, calendar, seasonal events and world time
are untouched as well. No virtual clock is introduced.

Instance binds (`instance.resettime`) are deliberately not moved. They expire by
real calendar time so weekly resets stay in sync with the real week.

Long respawns are excluded as well, see `MaxSpawnTimeSecs`.

## How it works

| When | Hook | Action |
|---|---|---|
| Running | `OnUpdate` | writes a heartbeat every 60 seconds |
| Logout of the last human | `OnPlayerLogout` | parks pending respawns, records remaining times |
| Shutdown | `OnAfterUnloadAllMaps` | records the time as a clean shutdown |
| Startup | `OnBeforeWorldInitialized` | shifts by the downtime |
| Login of the first human | `OnPlayerLogin` | sets every parked entry to now plus remaining time |

The heartbeat covers crashes: without a clean shutdown there would be no clue when
the server stopped. The error is at most one heartbeat interval.

The startup shift happens in `OnBeforeWorldInitialized` because maps read their
respawn times when they are created (`Map::LoadRespawnTimes`); at that point no map
exists yet.

Logout and login are different: maps are loaded and work with their in-memory
state. The module therefore writes through `Map::SaveCreatureRespawnTime`, which
updates map, respawn queue and database together, and also sets the timer of dead
creature objects (`Creature::SetRespawnTime`). Creatures still lying as corpses at
logout have no entry yet; they are captured through the creature object. Maps that
are not loaded are parked directly in the database.

Parking instead of only recording is necessary because the map's respawn queue
(`Map::ProcessRespawns`) spawns due creatures as soon as their grid is loaded and
deletes the entry. Grids stay loaded for minutes after logout, near bots
permanently, so shifting only at login would find nothing left.

At startup, parked entries stay untouched. The flat startup grace is not needed
then, because the login returns the exact remaining time.

## Edge cases

Respawn times that are already due are not shifted; otherwise they would move into
the future and appear later than without the module.

In instances with a reset period AzerothCore sets `respawnTime` to "now plus one
year" to mark that a creature does not return before the next reset. That is a
marker, not a time. `MaxFutureDays` keeps such values out of the shift.

If the server crashes while someone is logged in, the logout anchor is empty. The
startup path with the flat grace applies as before.

If you disable the module while respawns are parked, log in once beforehand.
Otherwise the parked creatures stay away for up to a year.
`FreezeWhileNoPlayerOnline = 0` is harmless: the next start returns the remaining
time to parked entries.

If a parked corpse is still looted, the core shortens its time by a few seconds
(`Creature::AllLootRemovedFromCorpse`). Everything further than `MaxFutureDays` in
the future therefore counts as parked; the reduction is taken from the remaining
time, exactly as without parking.

If the logout lies further back than `MaxDowntimeDays`, everything parked appears
immediately at login, as if time had passed normally.

## Long respawns

The freeze turns calendar time into play time. For ordinary creatures this is not
noticeable: in a sample of 889 pending entries, 817 had a respawn time of five
minutes or less, so they are back shortly after login anyway.

For rare elite spawns the effect reverses. Grunter with 42 hours or Kurmokk with 35
hours would not return after two days with two hours of play per evening, but after
weeks. The same applies to event NPCs whose event follows the real calendar.
`MaxSpawnTimeSecs` excludes such spawns from the shift; with the default of 1800
seconds this affected 11 of the 889 entries in the same sample.

## Configuration

`conf/offline_respawn_freeze.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `OfflineRespawnFreeze.Enable` | `1` | Turn the module on or off |
| `OfflineRespawnFreeze.DryRun` | `0` | Compute and log everything without writing; recommended for the first run |
| `OfflineRespawnFreeze.FreezeWhileNoPlayerOnline` | `1` | Also pause respawns while the server runs without a human player |
| `OfflineRespawnFreeze.HeartbeatSeconds` | `60` | Heartbeat interval, maximum error after a crash; `0` disables it |
| `OfflineRespawnFreeze.StartupGraceSeconds` | `300` | Extra seconds added to the downtime when no logout anchor exists |
| `OfflineRespawnFreeze.MinDowntimeSeconds` | `60` | Below this downtime nothing is shifted at startup |
| `OfflineRespawnFreeze.MaxDowntimeDays` | `60` | Above this downtime only log, do not change anything |
| `OfflineRespawnFreeze.MaxFutureDays` | `30` | Respawn times further in the future stay untouched |
| `OfflineRespawnFreeze.MaxSpawnTimeSecs` | `1800` | Spawns with a longer own respawn time follow real time; `0` shifts everything |
| `OfflineRespawnFreeze.IncludeInstances` | `1` | Also shift respawn times inside instances |

## Own tables

The module creates both tables in the characters database itself.

`mod_offline_respawn_freeze` holds a single row with the last known server time,
whether the server shut down cleanly, the anchor of the last logout and the extent
and time of the last shift.

`mod_offline_respawn_freeze_snapshot` holds the parked entries between logout and
login: original time, remaining time and parked value. Outside that span it is
empty.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-offline-respawn-freeze`,
re-run CMake, build and install. See the
[repository README](../README.md#installation). For a clean shutdown use
`server shutdown` in the worldserver console; after a crash the heartbeat is used.

## License

MIT, see [LICENSE](../LICENSE).
