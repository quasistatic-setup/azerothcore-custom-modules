/*
 * mod-out-of-combat-mana
 *
 * Vervielfacht die normale Mana-Regeneration von Spielern außerhalb des
 * Kampfes um OutOfCombatMana.Multiplier. Gilt für echte Spieler und für Bots
 * in einer Gruppe oder einem Raid mit einem echten Spieler; frei laufende
 * RandomBots bleiben unverändert. Rate.Mana, Wut, Energie, Runenmacht,
 * Begleiter und Kreaturen bleiben ebenfalls unverändert.
 *
 * Bots erkennt das Modul an WorldSession::IsBot(), das mod-playerbots für
 * seine Sitzungen setzt; Selfbots laufen über die Client-Sitzung und zählen
 * als echte Spieler. So braucht das Modul keine Playerbots-Header. Im
 * Schlachtfeld zählt die eigentliche Gruppe, nicht der Schlachtfeld-Raid.
 *
 * Warum dieser Hook: Der Core hat keinen Hook in Player::Regenerate. Er ruft
 * in Player::Update aber OnPlayerUpdate unmittelbar vor
 * "if (IsAlive()) { m_regenTimer += p_time; RegenerateAll(); }" auf. Weil
 * RegenerateAll m_regenTimer danach auf 0 setzt, rechnet der Core Mana in
 * jedem Update-Schritt mit genau p_time. Das Modul rechnet im selben Schritt,
 * mit demselben Zustand und derselben Formel, und ergänzt den Anteil über 1.
 *
 * Fünf-Sekunden-Regel: Wie der Core liest das Modul nach einem Manaverbrauch
 * (IsUnderLastManaUseEffect) den unterbrochenen Regenerationswert. Es
 * vervielfacht damit genau das, was der Core gerade gibt, und umgeht die
 * Regel nicht. Auren, die Mana-Regeneration verhindern, gelten ebenso.
 *
 * Kosten: Die Konfiguration liegt atomar vor, weil Map-Threads parallel
 * laufen. Bruchteil und Gruppenergebnis je Spieler liegen in dessen
 * CustomData und werden nur vom eigenen Map-Thread berührt, daher ohne
 * Sperre. Bots ohne Gruppe scheiden vor jedem Nachschlagen aus; die Gruppe
 * eines Bots wird höchstens einmal je Sekunde durchsucht. Ganze Punkte schreibt
 * das Modul wie der Core als Feldänderung ohne eigenes SMSG_POWER_UPDATE;
 * das Paket kommt mit dem 2-Sekunden-Takt des Cores oder beim Erreichen des
 * Maximums.
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
            LOG_ERROR("module", "[OutOfCombatMana] OutOfCombatMana.Multiplier = {} ist ungültig (mindestens 1). "
                "Die Mana-Regeneration bleibt unverändert.", multiplier);
            multiplier = 1.0f;
        }

        g_enabled.store(enabled);
        g_multiplier.store(multiplier);

        if (!enabled)
            LOG_INFO("module", "[OutOfCombatMana] Abgeschaltet.");
        else
            LOG_INFO("module", "[OutOfCombatMana] Aktiv: Mana-Regeneration außerhalb des Kampfes mit Faktor {}. "
                "Gilt für Spieler und Bots in deren Gruppen.", multiplier);
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

        // Dieselben Bedingungen wie Player::Update und Player::Regenerate,
        // dazu "außerhalb des Kampfes".
        if (!player->IsAlive() || player->IsInCombat())
            return;

        uint32 maxMana = player->GetMaxPower(POWER_MANA);
        uint32 curMana = player->GetPower(POWER_MANA);
        if (!maxMana || curMana >= maxMana)
            return;

        // .cheat power füllt der Core ohnehin auf.
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

        // Bots ohne Gruppe (die meisten RandomBots) scheiden aus, bevor
        // CustomData nachgeschlagen wird.
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

        // Wie Player::Regenerate: Zwischenstände nur als Feldänderung, das
        // Paket erst beim Maximum.
        player->SetPower(POWER_MANA, newMana, newMana == maxMana, true);
    }
};

void AddOutOfCombatManaScripts()
{
    new OutOfCombatManaWorldScript();
    new OutOfCombatManaPlayerScript();
}
