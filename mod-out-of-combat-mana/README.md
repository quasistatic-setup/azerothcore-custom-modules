# mod-out-of-combat-mana

mod-out-of-combat-mana is an AzerothCore WotLK module for configurable out-of-combat mana regeneration.
It multiplies the normal mana regeneration of players outside of combat by
`OutOfCombatMana.Multiplier`, with Player and Playerbot support: real players and
bots in a group or raid with a real player get the bonus, free-roaming random bots
stay normal. It works through the `PlayerScript::OnPlayerUpdate` hook, without a
core patch.

## Behaviour

- The factor applies to what the core already grants, including `Rate.Mana` and
  the optional low-level boost below level 15. Example with `Rate.Mana = 1.5` and
  multiplier 2: out of combat twice the normal regeneration, in combat unchanged.
  Multiplier 1 is exactly the behaviour without the module.
- The five-second rule stays: after spending mana the module multiplies the
  interrupted regeneration value the core is currently using. If that is 0, nothing
  is added. Auras that prevent mana regeneration and `.cheat power` behave as in
  the core.
- Unchanged: `Rate.Mana`, rage, energy, runic power, as well as pets, guardians and
  creatures.
- Bots are recognised through `WorldSession::IsHeadless()`, without Playerbot headers.
  Selfbots count as real players. A bot gets the bonus while a real player is
  online in its group or raid; this is checked at most once per second. In
  battlegrounds the original group counts, not the battleground raid.
- `.reload config` applies switch and multiplier; the server log reports the state
  at startup and after every reload.

## Configuration

`conf/out_of_combat_mana.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `OutOfCombatMana.Enable` | `1` | Turn the module on or off |
| `OutOfCombatMana.Multiplier` | `2.0` | Factor on the out-of-combat mana regeneration the core computes (including `Rate.Mana`). Values below 1 are invalid and act like 1.0 |

## How it works

The core calls `OnPlayerUpdate` in `Player::Update` right before regeneration and
computes mana there in every step with exactly that step's duration. The module
uses the same state and formula in the same step to add the share above 1, and
keeps its own per-player fraction in `CustomData`. Whole points are written as a
field update like the core does; a separate `SMSG_POWER_UPDATE` is only sent when
mana reaches its maximum.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-out-of-combat-mana`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## Tests

`src/OutOfCombatManaLogic.h` holds the calculation without core dependencies;
`tests/run_logic_test.sh` checks it without a server (needs `g++` with C++20).

## License

MIT, see [LICENSE](../LICENSE).
