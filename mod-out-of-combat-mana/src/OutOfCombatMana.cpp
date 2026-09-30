/*
 * mod-out-of-combat-mana
 *
 * Multiplies the normal mana regeneration of players outside of combat by
 * OutOfCombatMana.Multiplier. Applies to real players and to bots in a group
 * or raid with a real player; free-roaming random bots stay unchanged.
 * Rate.Mana, rage, energy, runic power, pets and creatures stay unchanged as
 * well.
 *
 * Bots are recognised through WorldSession::IsBot(), which mod-playerbots sets
 * for its sessions; selfbots run on the client session and count as real
 * players. The module therefore needs no Playerbot headers. In battlegrounds
 * the original group counts, not the battleground raid.
 *
 * Why this hook: the core has no hook in Player::Regenerate. In
 * Player::Update, however, it calls OnPlayerUpdate right before
 * "if (IsAlive()) { m_regenTimer += p_time; RegenerateAll(); }". Because
 * RegenerateAll resets m_regenTimer to 0 afterwards, the core computes mana in
 * every update step with exactly p_time. The module computes in the same
 * step, with the same state and formula, and adds the share above 1.
 *
 * Five-second rule: like the core, the module reads the interrupted
 * regeneration value after spending mana (IsUnderLastManaUseEffect). It
 * multiplies exactly what the core currently grants and does not bypass the
 * rule. Auras that prevent mana regeneration apply as well.
 *
 * Cost: the configuration is atomic because map threads run in parallel. The
 * fraction and group result per player live in that player's CustomData and
 * are only touched by its own map thread, so no lock is needed. Bots without
 * a group drop out before any lookup; a bot's group is scanned at most once
 * per second. Like the core, whole points are written as a field update
 * without a separate SMSG_POWER_UPDATE; the packet comes with the core's
 * two-second tick or when mana reaches its maximum.
 */

#include "Config.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "OutOfCombatManaLogic.h"

#include <atomic>
#include <string>

namespace
{
    std::atomic<bool> g_enabled{true};
    std::atomic<float> g_multiplier{2.0f};

    std::string const STATE_KEY = "mod-out-of-combat-mana";

    struct ManaState : public DataMap::Base
    {
        float fraction = 0.0f;
        bool botApplies = false;
        Milliseconds nextBotCheck{0};
    };

    Group* RelevantGroup(Player* player)
    {
        return player->InBattleground() ? player->GetOriginalGroup() : player->GetGroup();
    }

    bool GroupHasRealPlayer(Group* group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (WorldSession* session = member->GetSession(); session && !session->IsBot())
                    return true;

        return false;
    }

    void LoadConfig()
    {
        bool enabled = sConfigMgr->GetOption<bool>("OutOfCombatMana.Enable", true);
        float multiplier = sConfigMgr->GetOption<float>("OutOfCombatMana.Multiplier", 2.0f);

        if (!OutOfCombatMana::IsValidMultiplier(multiplier))
        {
            LOG_ERROR("module", "[OutOfCombatMana] OutOfCombatMana.Multiplier = {} is invalid (minimum 1). "
                "Mana regeneration stays unchanged.", multiplier);
            multiplier = 1.0f;
        }

        g_enabled.store(enabled);
        g_multiplier.store(multiplier);

        if (!enabled)
            LOG_INFO("module", "[OutOfCombatMana] Disabled.");
        else
            LOG_INFO("module", "[OutOfCombatMana] Active: out-of-combat mana regeneration with factor {}. "
                "Applies to players and the bots in their groups.", multiplier);
    }
}

class OutOfCombatManaWorldScript : public WorldScript
{
public:
    OutOfCombatManaWorldScript() : WorldScript("OutOfCombatManaWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class OutOfCombatManaPlayerScript : public PlayerScript
{
public:
    OutOfCombatManaPlayerScript() : PlayerScript("OutOfCombatManaPlayerScript", {
        PLAYERHOOK_ON_UPDATE
    }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!g_enabled.load(std::memory_order_relaxed))
            return;

        float multiplier = g_multiplier.load(std::memory_order_relaxed);
        if (multiplier <= 1.0f)
            return;

        // Same conditions as Player::Update and Player::Regenerate, plus
        // "out of combat".
        if (!player->IsAlive() || player->IsInCombat())
            return;

        uint32 maxMana = player->GetMaxPower(POWER_MANA);
        uint32 curMana = player->GetPower(POWER_MANA);
        if (!maxMana || curMana >= maxMana)
            return;

        // The core refills mana for .cheat power anyway.
        if (player->GetCommandStatus(CHEAT_POWER))
            return;

        if (player->HasAuraTypeWithMiscvalue(SPELL_AURA_PREVENT_REGENERATE_POWER, POWER_MANA + 1))
            return;

        float regenPerSecond = player->GetFloatValue((player->IsUnderLastManaUseEffect()
            ? UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER
            : UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) + AsUnderlyingType(POWER_MANA));

        float manaRate = OutOfCombatMana::ManaRate(sWorld->getRate(RATE_POWER_MANA),
            sWorld->getBoolConfig(CONFIG_LOW_LEVEL_REGEN_BOOST), player->GetLevel());

        float bonus = OutOfCombatMana::Bonus(regenPerSecond, manaRate, diff, multiplier);
        if (bonus <= 0.0f)
            return;

        // Bots without a group (most random bots) drop out before CustomData
        // is looked up.
        bool isBot = player->GetSession()->IsBot();
        Group* group = isBot ? RelevantGroup(player) : nullptr;
        if (isBot && !group)
            return;

        ManaState* state = player->CustomData.GetDefault<ManaState>(STATE_KEY);

        if (isBot)
        {
            Milliseconds now = GameTime::GetGameTimeMS();
            if (now >= state->nextBotCheck)
            {
                state->botApplies = OutOfCombatMana::Applies(true, GroupHasRealPlayer(group));
                state->nextBotCheck = now + Milliseconds(OutOfCombatMana::BOT_RECHECK_MS);
            }

            if (!state->botApplies)
                return;
        }

        uint32 whole = OutOfCombatMana::TakeWhole(state->fraction, bonus);
        if (!whole)
            return;

        uint32 newMana = maxMana - curMana > whole ? curMana + whole : maxMana;
        if (newMana == maxMana)
            state->fraction = 0.0f;

        // Like Player::Regenerate: intermediate values only as a field update,
        // the packet only at the maximum.
        player->SetPower(POWER_MANA, newMana, newMana == maxMana, true);
    }
};

void AddOutOfCombatManaScripts()
{
    new OutOfCombatManaWorldScript();
    new OutOfCombatManaPlayerScript();
}
