# mod-bot-quest-item-cleanup

mod-bot-quest-item-cleanup is an AzerothCore WotLK module that keeps Playerbot bags
free of leftover quest items. When a bot is rewarded for a quest, the module removes
the item that quest handed out on accept (`quest_template.StartItem`), such as empty
phials, sampling tubes or clue notes. It works through the
`PlayerScript::OnPlayerCompleteQuest` hook, without a core patch.

## Why

`Player::RewardQuest` takes the required items and the `ItemDrop` items of a quest,
but not its source item. A player normally uses that item up while doing the quest.
A bot that completes a quest without playing it never does. This happens with
`AiPlayerbot.SyncQuestWithPlayer = 1` in
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), where bots finish
a quest the moment their master hands it in. The items cannot be sold and stay in
the bags for good.

## Behaviour

- Runs once when a bot is rewarded for a quest. Every stack of the quest's source
  item is removed from bags and bank.
- Only items of class Quest or with quest binding are removed. A provided item with
  a use of its own is never touched.
- The item stays when another quest in the bot's log still needs it or hands it out
  itself, and when it starts a quest the bot has not been rewarded for yet.
- Bots are recognised through `WorldSession::IsBot()`, without Playerbot headers.
  Real players and selfbots are never touched.
- Items that are already in the bags from earlier quests are not cleaned up
  retroactively; the module only acts at the moment of the reward.
- `.reload config` applies the switch; the server log reports the state at startup
  and after every reload.

## Configuration

`conf/bot_quest_item_cleanup.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `BotQuestItemCleanup.Enable` | `1` | Turn the module on or off |

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-bot-quest-item-cleanup`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
