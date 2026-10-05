/*
 * mod-priest-lockpicking
 *
 * Lets priests pick locks: Pick Lock (spell 1804) and the Lockpicking skill
 * (633) at 450/450 for every priest that is not a random bot.
 *
 * Why a module is needed: SkillRaceClassInfo.dbc allows Lockpicking for
 * rogues only, and the core enforces that every time a character is loaded.
 * Player::_LoadSkills drops the skill regardless of any setting, and with
 * ValidateSkillLearnedBySpells = 1 Player::_LoadSpells deletes the stored
 * spell as well. Both are logged as errors. A priest who was taught the spell
 * by a game master therefore loses it on the next login.
 *
 * How it works: nothing is stored that the core would reject. The module
 * grants spell and skill again on every login and takes the skill back before
 * the character is saved on logout.
 *
 *  - OnPlayerLoadFromDB runs before spells and action buttons are loaded. The
 *    spell is added as a temporary spell there, the same way the core adds
 *    spells that come from a skill. Temporary spells are never written to
 *    character_spell, so the validation has no row to delete. Because the
 *    spell is known before Player::_LoadActions runs, a Pick Lock button on
 *    the action bar survives, and the client receives the spell with its
 *    initial spell list instead of a "new spell learned" message.
 *  - OnPlayerLogin sets the skill. The skill fields are rebuilt from the
 *    database during loading, so it cannot be set earlier. The hook also
 *    learns the spell again in case something removed it during loading, for
 *    example a row left over from an earlier .learn or a pending spell reset.
 *  - OnPlayerBeforeLogout removes the skill before the logout save, so
 *    character_skills keeps no row the core would complain about. After a
 *    crash such a row can remain from a periodic save; the core then logs one
 *    error on the next login and the module restores the skill as usual.
 *
 * Skill 633 has Pick Lock as its only ability and the spell teaches no skill,
 * so no other spell or skill is touched.
 *
 * Who gets it: real players, and characters that mod-playerbots logs in for a
 * player account (alt bots). Without the latter the core would delete the
 * Pick Lock button from the character's action bar whenever it is loaded as
 * a bot, for example through AiPlayerbot.BotAutologin. An alt bot with the
 * skill also unlocks lockboxes it receives, because the "unlock items" action
 * of mod-playerbots only asks for the skill. Characters on random bot
 * accounts stay unchanged. Bots are recognised through
 * WorldSession::IsHeadless(), which mod-playerbots sets for its sessions, and
 * random bot accounts through the account name prefix from
 * AiPlayerbot.RandomBotAccountPrefix. The module therefore needs no Playerbot
 * headers. Real players never cause an account lookup.
 *
 * Cost: three hooks that run once per login or logout, none per update.
 */

#include "AccountMgr.h"
#include "Config.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>

namespace
{
    constexpr uint32 SPELL_PICK_LOCK = 1804;

    // Value and maximum of the Lockpicking skill.
    constexpr uint16 LOCKPICKING_VALUE = 450;

    // The prefix is read from the Playerbots configuration so both sides
    // cannot drift apart. An account that cannot be resolved counts as a bot
    // account, so nothing is changed in case of doubt.
    bool IsRandomBotAccount(uint32 accountId)
    {
        std::string name;
        if (!AccountMgr::GetName(accountId, name))
            return true;

        std::string prefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
        if (prefix.empty())
            return false;

        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);
        return name.rfind(prefix, 0) == 0;
    }

    bool Applies(Player* player)
    {
        if (player->getClass() != CLASS_PRIEST)
            return false;

        WorldSession* session = player->GetSession();
        if (!session)
            return false;

        return !session->IsHeadless() || !IsRandomBotAccount(session->GetAccountId());
    }
}

class PriestLockpickingPlayerScript : public PlayerScript
{
public:
    PriestLockpickingPlayerScript() : PlayerScript("PriestLockpickingPlayerScript", {
        PLAYERHOOK_ON_LOAD_FROM_DB,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT
    }) { }

    void OnPlayerLoadFromDB(Player* player) override
    {
        if (Applies(player))
            player->addSpell(SPELL_PICK_LOCK, SPEC_MASK_ALL, true, true);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!Applies(player))
            return;

        if (!player->HasSpell(SPELL_PICK_LOCK))
            player->learnSpell(SPELL_PICK_LOCK, true, true);

        if (player->GetPureSkillValue(SKILL_LOCKPICKING) != LOCKPICKING_VALUE ||
            player->GetPureMaxSkillValue(SKILL_LOCKPICKING) != LOCKPICKING_VALUE)
            player->SetSkill(SKILL_LOCKPICKING, 0, LOCKPICKING_VALUE, LOCKPICKING_VALUE);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        if (Applies(player) && player->HasSkill(SKILL_LOCKPICKING))
            player->SetSkill(SKILL_LOCKPICKING, 0, 0, 0);
    }
};

void AddPriestLockpickingScripts()
{
    new PriestLockpickingPlayerScript();
}
