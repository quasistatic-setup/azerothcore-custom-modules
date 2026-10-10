# mod-welcome-promos

mod-welcome-promos is an AzerothCore WotLK module for a configurable welcome mail with promotional rewards.
Every newly created character receives, exactly once, a mail with historical
promotional and special items such as rare pets and mounts.

## Why a module

AzerothCore already ships a server mail system, `mail_server_template`. It delivers
on **every login** and records the character in `mail_server_character`
afterwards. On a server with Playerbots this hits every bot character: thousands of
bots would mean tens of thousands of items in bot mailboxes, and every newly
created bot would keep receiving more.

`OnPlayerCreate` only fires when a character is actually created. Existing
characters are left alone without having to register them anywhere first.

## How it works

| When | Hook | Action |
|---|---|---|
| Server start | `OnBeforeWorldInitialized` | creates the tracking table |
| Character creation | `OnPlayerCreate` | checks, sends, records |
| Permanent character deletion | `OnPlayerDeleteFromDB` | removes the delivery marker |

The core calls the hook after the character creation has been committed, so the
character exists in the database and sending mail is safe.

Bot accounts are excluded. The prefix is read from
`Playerbots.RandomBotAccountPrefix` so both sides cannot drift apart (default
`rndbot` when mod-playerbots is not installed); further prefixes can be configured.

## Sent only once

Two safeguards: `OnPlayerCreate` fires exactly once per character, and every
character that received the mail is recorded in `mod_welcome_promos_sent` in the
characters database. Logging in again does not send another mail.

When a character is deleted permanently, its marker is removed in the same database
transaction. If the core later reuses the freed GUID, the new character can receive
the mail.

## Collector's Edition rewards

Collector's Edition rewards keep working through the core's account flags. There is
no overlap: they use different item IDs (13582 Zergling Leash, 13583 Panda Collar,
13584 Diablo Stone), and this module does not send them.

## Configuration

`conf/welcome_promos.conf.dist`, installed to `etc/modules/`:

| Setting | Default | Meaning |
|---|---|---|
| `WelcomePromos.Enable` | `1` | Turn the module on or off |
| `WelcomePromos.Items` | ten promo pets and mounts | Comma-separated item IDs. Unknown IDs are skipped and logged, duplicates sent once; more than twelve items are split across several mails |
| `WelcomePromos.SenderEntry` | `0` | `creature_template` entry used as sender; `0` sends as a game master mail |
| `WelcomePromos.Subject` | `Special keepsakes` | Mail subject; with several mails a counter such as `(1/2)` is appended |
| `WelcomePromos.Body` | short English welcome text | Mail text |
| `WelcomePromos.ExcludedAccountPrefixes` | empty | Additional account prefixes that never receive the mail, case-insensitive |

The default items are 20371 Blue Murloc Egg, 39656 Tyrael's Hilt, 43599 Big
Blizzard Bear, 46767 Warbot Ignition Key, 46802 Heavy Murloc Egg, 49362 Onyxian
Whelpling, 49646 Core Hound Pup, 49665 Pandaren Monk, 49693 Lil' Phylactery and
54847 Lil' XT.

## Installation

Link or copy this folder to `azerothcore-wotlk/modules/mod-welcome-promos`, re-run
CMake, build and install. See the [repository README](../README.md#installation).

## License

MIT, see [LICENSE](../LICENSE).
