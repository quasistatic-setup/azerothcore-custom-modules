# mod-bot-inventory

mod-bot-inventory is an AzerothCore WotLK module for managing the inventories of the
Playerbots in your party or raid without trade windows. Its `.botinv` commands list
the bags, equipment and money of you and your bots, move items and gold directly
between them, and apply profession enchants to any of their items. It is a plain
`CommandScript`, without a core patch and without Playerbot headers.

## Why

Handing gear, materials and money back and forth between a player and a group of
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) bots takes one
trade window per bot and per direction. Enchanting a bot's gear takes a trade window
per item. The commands here do the same things in one step, and their output is
built for a client addon to turn into an inventory window.

## Commands

All commands are available to players (`SEC_PLAYER`). Characters are named; valid
names are your own character and bots in your group.

| Command | Effect |
|---|---|
| `.botinv list` | Bags, equipment, money and free slots of you and every bot in your group |
| `.botinv move <from> <to> <itemGuid> [<itemGuid> ...]` | Moves whole stacks from one character's bags to another's |
| `.botinv gold <from> <to> <copper>` | Moves money |
| `.botinv enchants <owner> <itemGuid>` | Lists, per group member, the enchants they know that fit this item |
| `.botinv enchant <caster> <spellId> <owner> <itemGuid>` | Applies that enchant |

`itemGuid` is the low GUID of the item as printed by `.botinv list`.

## Rules

- Only you and bots in your party or raid take part. Bots are recognised through
  `WorldSession::IsHeadless()`; other real players are never a source or a target.
  Every real player in a group can manage every bot in that group.
- Nobody involved may be in combat, loading or in an open trade.
- Items move under the rules of the trade window: no soulbound items, no quest
  items, no bags with content, only from bags (not from equipment or bank), and the
  receiver must have room and be allowed to carry the item. Both inventories are
  saved in one database transaction.
- Enchants follow the checks of a normal cast: the enchanter must know the spell,
  the item must fit (class, slot, level), reagents and tools must be in the
  enchanter's bags. Reagents are consumed and the skill-up roll happens as usual.
  Enchants limited to the enchanter's own gear stay limited to it. Vellums are not
  supported; enchant those the normal way.
- An enchant is applied instantly, without cast time. A real cast cannot target an
  item in another character's bags, so the module applies the enchantment itself.
- Every transfer and enchant is written to the server log (`module` logger).

## Output format

Lines are `~` separated so they can be parsed; records inside a line end with `;`.
Long lists are split over several lines with the same head.

| Line | Meaning |
|---|---|
| `P~<name>~<classId>~<copper>~<freeSlots>~<isCaller>` | One character, followed by its item lines |
| `I~<name>~<bag>,<slot>,<entry>,<count>,<itemGuid>,<enchantId>,<randomPropertyId>,<flags>;...` | Items; bag 255 with slot 0 to 18 is equipment; flag 1 means the item can be moved |
| `M~<itemGuid>~<1 or 0>~<reason>` | Result of one item of a move |
| `G~<from>~<to>~<copper>` | Money was moved |
| `C~<caster>~<spellId>,<1 or 0>;...` | Enchants a caster knows for the item; 1 means reagents and tools are complete |
| `X~<caster>~<spellId>~<owner>~<itemGuid>` | Enchant was applied |

Errors are plain sentences.

### Use from an addon

AzerothCore answers a command sent through the addon channel with addon messages
instead of chat lines. Send the command without the leading dot as an addon whisper
to your own character:

```lua
SendAddonMessage("AzerothCore", "i0001botinv list", "WHISPER", UnitName("player"))
```

`i` issues a command and `0001` is any four-character id. The replies arrive as
`CHAT_MSG_ADDON` with prefix `AzerothCore`: `a0001` (acknowledged), `m0001<line>`
for every output line, then `o0001` (done) or `f0001` (failed). This needs
`AddonChannel = 1` in `worldserver.conf`, which is the default.

## Configuration

`conf/bot_inventory.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `BotInventory.Enable` | `1` | Turn the commands on or off |
| `BotInventory.MaxDistance` | `0` | Largest distance in yards between the two characters of a transfer or enchant; `0` means no limit |

`.reload config` applies both settings.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-bot-inventory`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
