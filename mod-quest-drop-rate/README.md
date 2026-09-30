# mod-quest-drop-rate

mod-quest-drop-rate is an AzerothCore WotLK module for configurable quest item drop rates.
It multiplies the drop chance of real quest items (`QuestRequired = 1` in the
`*_loot_template` tables, `LootStoreItem::needs_quest` in the core) by
`QuestDropRate.Multiplier`, capped at 100 %. It works through the
`GlobalScript::OnItemRoll` hook, without a core patch.

## Behaviour

- New chance is `min(table chance × multiplier, 100)` for every loot type,
  including fishing, pickpocketing and skinning.
- Outside of loot groups the core applies `Rate.Drop.Item.<quality>` afterwards.
  Example with multiplier 2 and `Rate.Drop.Item.Normal = 0.8`: a white quest item
  with 40 % drops at 80 × 0.8 = 64 % instead of 32 %. Once the chance reaches 100,
  the item always drops.
- Quest items inside referenced loot tables (`reference_loot_template`) are raised
  like any other quest item. The reference entry that pulls such a table in keeps
  its own chance.
- Grouped loot: entries of one group share a single roll, so more chance for a
  quest item would be taken from the other items. Quest items in groups that also
  contain normal items therefore stay unchanged. The module reads these
  item/group pairs from the world database at startup and logs their number.
- Groups made up of quest items only are raised; several quest items of such a
  group share at most 100 %.
- Unchanged: normal loot, reference entries and all `Rate.Drop.Item.*` values.
- `.reload config` applies switch and multiplier. Changed loot tables affect the
  grouped-loot exclusion only after a restart.

## Configuration

`conf/quest_drop_rate.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `QuestDropRate.Enable` | `1` | Turn the module on or off |
| `QuestDropRate.Multiplier` | `2.0` | Factor for quest item drop chances, capped at 100 %. Values below 1 are invalid and leave chances unchanged |

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-quest-drop-rate`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## Tests

`src/QuestDropRateLogic.h` holds the calculation without core dependencies;
`tests/run_logic_test.sh` checks it without a server (needs `g++` with C++20).

## License

MIT, see [LICENSE](../LICENSE).
