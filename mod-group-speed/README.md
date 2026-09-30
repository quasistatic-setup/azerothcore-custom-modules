# mod-group-speed

mod-group-speed is an AzerothCore WotLK module that adds a group movement speed command.
`.group speed <rate>` sets the speed of your whole party or raid in one step, and
ghosts run three times as fast as normal.

```
.group speed 2     double speed
.group speed 1     normal
```

## Behaviour

For every online member of the caller's party or raid, the caller and Playerbots
included, the command does the same as `.modify speed all` on a selected player:

| Aspect | Value |
|---|---|
| Movement types | walk, run, swim, flight via `SetSpeed(..., true)` |
| Limits | 0.1 to 50, otherwise `Incorrect values.` |
| Skipped | members on a taxi flight, members with a higher account level, members still loading |
| Not affected | pets, NPCs, players outside the group, offline members |
| Duration | until logout, like `.modify speed all` |

The current target does not matter. Without a group the command reports an error
and changes nothing. The reply is a single line, for example
`Group speed set to 2.00 for 4 members; 1 member skipped: Hunt (in flight).`

## Ghost speed

An AuraScript on the ghost aura 8326 (`src/GhostSpeed.cpp`) raises its bonus from
+50 % to +200 % run and swim speed, so ghosts run three times as fast as normal.
This applies to all players and Playerbots, also after relogging or teleporting as
a ghost, because the value is part of the aura and is not lost on the next speed
recalculation like `.group speed`. The Night Elf aura 20584 (+75 %) does not
stack; the higher value wins. `data/sql/db-world/mod_group_speed_ghost.sql` binds
the script to the aura through `spell_script_names`.

## Permission

The command is registered with `RBAC_PERM_COMMAND_MODIFY_SPEED_ALL`, like
`.modify speed all`. As for every command, the effective level comes from
`acore_world.command`. `data/sql/db-world/mod_group_speed_command.sql` creates the
row `group speed` with the level of `modify speed all` (2 by default). If you later
change that level, adjust `group speed` as well.

## Why a module

AzerothCore merges the command tables of all `CommandScript`s by name, so `speed`
attaches below the existing `.group` without touching `cs_group.cpp`, and core
updates do not overwrite it. There is no configuration file.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-group-speed`, re-run
CMake, build and install. The SQL files are applied by the database updater on the
next start. See the [repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
