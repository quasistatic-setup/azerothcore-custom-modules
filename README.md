# AzerothCore Custom Modules

Custom AzerothCore WotLK modules for quest drop rates, group speed, offline respawn handling, mana regeneration, welcome rewards and Playerbot-oriented gameplay tweaks.

Each folder in this repository is a self-contained AzerothCore module for World of
Warcraft 3.3.5a (Wrath of the Lich King). The modules use only script hooks, so the
core stays unpatched, and every module can be installed on its own.

## Modules

| Module | Purpose |
|---|---|
| [mod-quest-drop-rate](mod-quest-drop-rate/) | Configurable multiplier for quest item drop rates |
| [mod-offline-respawn-freeze](mod-offline-respawn-freeze/) | Freezes creature respawn timers while the server is offline or nobody plays |
| [mod-group-speed](mod-group-speed/) | `.group speed` command for the whole party or raid, faster ghosts |
| [mod-welcome-promos](mod-welcome-promos/) | Configurable one-time welcome mail with promotional rewards |
| [mod-out-of-combat-mana](mod-out-of-combat-mana/) | Configurable out-of-combat mana regeneration for players and their Playerbots |

### mod-quest-drop-rate

AzerothCore quest item drop rate module. Raises the drop chance of quest loot
(`QuestRequired = 1`) by a configurable multiplier, capped at 100 %, for every
loot type including fishing, pickpocketing and skinning. Quest items inside
referenced loot tables are raised as well; grouped loot is only touched when the
group consists of quest items alone, so normal items in a shared group keep their
chance. Normal loot and all `Rate.Drop.Item.*` settings stay unchanged.

### mod-offline-respawn-freeze

Freezes creature respawn timers while the server is offline. A creature that had
seven minutes left when the server stopped still has seven minutes left when it
starts again. Optionally the timers also pause while the server runs but no human
player is online, which suits small servers with Playerbots running around the
clock.

### mod-group-speed

Group movement speed command: `.group speed <rate>` sets walk, run, swim and
flight speed for every online member of your party or raid, Playerbots included,
with the same rules as `.modify speed all`. The module also makes ghosts run three
times as fast as normal.

### mod-welcome-promos

Configurable welcome mail with promotional rewards. Every newly created character
receives the configured items once, by mail. Existing characters and Playerbot
accounts are skipped.

### mod-out-of-combat-mana

Configurable out-of-combat mana regeneration. Multiplies the mana regeneration the
core already grants while a player is out of combat, keeps the five-second rule
intact and supports Players and Playerbots: bots in a group or raid with a real
player benefit, free-roaming random bots stay at normal regeneration.

## Compatibility

- AzerothCore WotLK (3.3.5a). Developed and run on the
  [mod-playerbots fork of AzerothCore](https://github.com/mod-playerbots/azerothcore-wotlk)
  (branch `Playerbot`) together with
  [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots).
- The hooks used by these modules also exist in upstream AzerothCore `master`;
  that combination is not tested regularly.
- [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) is optional.
  Bots are recognised through `WorldSession::IsBot()`, so no module includes
  Playerbot headers.

## Installation

AzerothCore expects every module as a direct subfolder of `modules/`. Do not clone
this repository itself into `modules/`; link or copy the modules you want instead.

```bash
git clone https://github.com/quasistatic-setup/azerothcore-custom-modules.git

cd azerothcore-wotlk/modules
ln -s /path/to/azerothcore-custom-modules/mod-quest-drop-rate mod-quest-drop-rate
# repeat for every module you want, or use cp -r instead of ln -s
```

Then re-run CMake (the module list is fixed at configure time), build and install
as usual:

```bash
cmake -S azerothcore-wotlk -B build
cmake --build build
cmake --install build
```

The install step places each `*.conf.dist` in `etc/modules/`. Copy it to the same
name without `.dist` to change settings. SQL files under `data/sql/` are applied
automatically by the AzerothCore database updater on the next start.

## Tests

Modules with pure calculation logic ship a standalone test that needs neither a
server nor the core sources:

```bash
mod-quest-drop-rate/tests/run_logic_test.sh
mod-out-of-combat-mana/tests/run_logic_test.sh
```

## Support

If these modules help you and you want to support their development:

- Buy Me a Coffee: https://buymeacoffee.com/quasistatic
- Ko-fi: https://ko-fi.com/quasistatic

## License

MIT, see [LICENSE](LICENSE). AzerothCore itself is licensed under the GNU GPL, version 2 or later;
a server binary built with these modules is subject to AzerothCore's license.

World of Warcraft and Blizzard Entertainment are trademarks or registered
trademarks of Blizzard Entertainment, Inc. This project is not affiliated with or
endorsed by Blizzard Entertainment. No game client files are included.
