# mod-priest-lockpicking

mod-priest-lockpicking is an AzerothCore WotLK module that lets priests pick locks.
Every priest that is not a random bot knows Pick Lock (spell 1804) and has the
Lockpicking skill (633) at 450/450, without a core patch, without DBC or database
changes and with `ValidateSkillLearnedBySpells = 1` left on.

## Why a module

`SkillRaceClassInfo.dbc` allows Lockpicking for rogues only, and the core enforces
that every time a character is loaded:

- `Player::_LoadSkills` drops a stored skill that does not fit the class. No setting
  turns this off.
- With `ValidateSkillLearnedBySpells = 1`, `Player::_LoadSpells` deletes a stored
  spell that belongs to such a skill.
- `Player::_LoadActions` then deletes every action bar button of the missing spell.

A priest who gets the spell with `.learn 1804` and the skill with `.setskill 633`
therefore loses spell, skill and button on the next login, and the server log
shows three errors. Turning the validation off would keep the spell, but not the
skill, and it would stop the check for every other spell too.

## How it works

Nothing is stored that the core would reject. The module grants spell and skill on
every login and takes the skill back before the character is saved on logout.

| When | Hook | Action |
|---|---|---|
| Before spells and action buttons are loaded | `OnPlayerLoadFromDB` | adds Pick Lock as a temporary spell |
| Login, after loading | `OnPlayerLogin` | sets Lockpicking to 450/450; learns the spell again if loading removed it |
| Logout, before the save | `OnPlayerBeforeLogout` | removes the skill |

The core adds spells that come from a skill as temporary spells and never writes
them to `character_spell`. The module uses the same mechanism, so the validation
finds no row to delete. Because the spell is known before the action buttons are
loaded, a Pick Lock button on the action bar survives relogging, and the client
gets the spell with its initial spell list instead of a "new spell learned" message
on every login.

The skill cannot be set that early, because the skill fields are rebuilt from the
database during loading. It is removed again before the logout save so that
`character_skills` keeps no row the core would report as an error.

## Behaviour

- Applies to real players and to characters that
  [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) logs in for a
  player account (alt bots, for example through `AiPlayerbot.BotAutologin`). Without
  the latter the core would delete the Pick Lock button whenever the priest is
  loaded as a bot. An alt bot with the skill also unlocks lockboxes it receives.
- Characters on random bot accounts stay unchanged. They are recognised through
  `WorldSession::IsHeadless()` and the account name prefix from
  `AiPlayerbot.RandomBotAccountPrefix`, without Playerbot headers. Real players
  never cause an account lookup.
- Other classes, spells and skills are never touched. Lockpicking has Pick Lock as
  its only ability, and the spell teaches no skill.
- The skill stays at 450/450. It does not rise with use and does not follow the
  character level.
- A spell row left over from an earlier `.learn 1804` is deleted by the core on the
  first login with the module, together with a Pick Lock button; the server log
  shows the usual errors once. The spell is there again right away, the button has
  to be placed once more.
- After a crash a skill row can remain from a periodic save. The core then logs one
  error on the next login and the module restores the skill as usual.
- The module stores nothing itself. After removing it, the core deletes a remaining
  Pick Lock button on the next login.

There is no configuration file. Spell, skill value and class are constants in
`src/PriestLockpicking.cpp`.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-priest-lockpicking`,
re-run CMake, build and install. See the
[repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
