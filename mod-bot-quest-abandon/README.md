# mod-bot-quest-abandon

mod-bot-quest-abandon is an AzerothCore WotLK module for
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots): when you abandon a
quest, the bots in your party or raid abandon it too. It works through the
`PlayerScript::OnPlayerQuestAbandon` hook, without a core patch.

## Why

With `Playerbots.SyncQuestWithPlayer = 1` the bots take over your quests and finish
them when you hand them in. A quest you give up stays in their logs, though, and has
to be dropped from every bot one by one.

## Behaviour

- Runs once when a real player abandons a quest. Every bot of the group that holds
  the quest and is on the same map drops it.
- Bots go through the same core handler as a client abandon, so quest items, the
  quest's source item, timers and the PvP flag are cleaned up exactly as for a
  player. If the core refuses because a source item cannot be taken back, that bot
  keeps the quest.
- One system message counts and names the bots that followed, in the layout of the
  mod-playerbots quest group messages: `Quest abandoned: [Quest] - Tank` for one bot,
  `Quest abandoned: [Quest] - 4 bots: Heal, Hunt, Mage, Tank` for several. Beyond five
  names the rest is only counted (`+3`), so a raid stays within one chat line.
- Bots are recognised through `WorldSession::IsHeadless()`, without Playerbot headers.
  Selfbots count as real players. A quest a bot drops by itself is not passed on.
- A bot on another map keeps the quest: the hook runs on the player's map thread,
  and bots elsewhere are not touched from there.
- With several real players in one group, all bots of the group follow the player who
  abandons, including the bots of the other player.
- `.reload config` applies the switch; the server log reports the state at startup
  and after every reload.

## Configuration

`conf/bot_quest_abandon.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `BotQuestAbandon.Enable` | `1` | Turn the module on or off |

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-bot-quest-abandon`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
