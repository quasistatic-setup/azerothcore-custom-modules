/*
 * mod-quest-drop-rate
 *
 * Erhöht die Dropchance echter Questgegenstände (LootStoreItem::needs_quest)
 * um einen einstellbaren Faktor, höchstens auf 100 %. Normales Loot,
 * Referenzen und Gegenstände ohne Quest-Anforderung bleiben unverändert;
 * Rate.Drop.Item.* wird nicht angefasst.
 *
 * Warum ein Modul: Der Core ruft ScriptMgr::OnItemRoll für jeden Eintrag mit
 * eigener Chance auf, bevor er würfelt (LootStoreItem::Roll und
 * LootTemplate::LootGroup::Roll). Der Hook darf die Chance ändern. Damit
 * wirkt das Modul auf alle Loot-Arten gleich, Angeln eingeschlossen, ohne
 * einen Core-Patch.
 *
 * Zusammenspiel mit dem Core: Bei Einträgen außerhalb von Gruppen wendet
 * LootStoreItem::Roll nach dem Hook noch den Qualitätsfaktor
 * Rate.Drop.Item.<Qualität> an, außer die Chance hat 100 erreicht. Die
 * wirksame Chance verdoppelt sich also bei Faktor 2 genau; wer 100 erreicht,
 * fällt sicher. In Gruppen gibt es keinen Qualitätsfaktor.
 *
 * Gruppen: Einträge einer Gruppe teilen sich einen Wurf. Liegt ein
 * Questgegenstand zusammen mit normalen Gegenständen in einer Gruppe, ginge
 * jede zusätzliche Chance zu deren Lasten. Solche Einträge bleiben daher
 * unverändert. Der Hook kennt die Loot-Tabelle nicht (Einträge aus Referenzen
 * kommen mit dem Store des Aufrufers), deshalb gilt der Ausschluss für jedes
 * Paar aus Gegenstand und Gruppennummer, das irgendwo gemischt vorkommt.
 */

#include "Config.h"
#include "DatabaseEnv.h"
#include "GlobalScript.h"
#include "Log.h"
#include "LootMgr.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "QuestDropRateLogic.h"

#include <atomic>
#include <unordered_set>

namespace
{
    // OnItemRoll läuft in den Map-Threads; ein Konfigurations-Reload darf die
    // Werte gleichzeitig ändern.
    std::atomic<bool> g_enabled{true};
    std::atomic<float> g_multiplier{2.0f};

    // Wird nur beim Start gefüllt, bevor Loot erzeugt wird, danach nur gelesen.
    std::unordered_set<uint32> g_mixedGroupEntries;

    constexpr char const* LOOT_TABLES[] =
    {
        "creature_loot_template", "disenchant_loot_template", "fishing_loot_template",
        "gameobject_loot_template", "item_loot_template", "mail_loot_template",
        "milling_loot_template", "pickpocketing_loot_template", "player_loot_template",
        "prospecting_loot_template", "reference_loot_template", "skinning_loot_template",
        "spell_loot_template"
    };

    uint32 MixedKey(uint32 itemId, uint8 groupId)
    {
        // Gegenstandsnummern bleiben unter 2^24, die Gruppennummer hat 7 Bit.
        return (itemId << 8) | groupId;
    }

    void LoadConfig()
    {
        bool enabled = sConfigMgr->GetOption<bool>("QuestDropRate.Enable", true);
        float multiplier = sConfigMgr->GetOption<float>("QuestDropRate.Multiplier", 2.0f);

        if (!QuestDropRate::IsValidMultiplier(multiplier))
        {
            LOG_ERROR("module", "[QuestDropRate] QuestDropRate.Multiplier = {} ist ungültig (mindestens 1). Die Dropchancen bleiben unverändert.", multiplier);
            multiplier = 1.0f;
        }

        g_enabled.store(enabled);
        g_multiplier.store(multiplier);
    }

    void LoadMixedGroups()
    {
        g_mixedGroupEntries.clear();

        for (char const* table : LOOT_TABLES)
        {
            QueryResult result = WorldDatabase.Query(
                "SELECT q.Item, q.GroupId FROM {0} q WHERE q.QuestRequired = 1 AND q.GroupId > 0 AND q.Reference = 0 "
                "AND EXISTS (SELECT 1 FROM {0} o WHERE o.Entry = q.Entry AND o.GroupId = q.GroupId AND o.QuestRequired = 0)",
                table);
            if (!result)
                continue;

            do
            {
                Field* fields = result->Fetch();
                g_mixedGroupEntries.insert(MixedKey(fields[0].Get<uint32>(), fields[1].Get<uint8>()));
            } while (result->NextRow());
        }
    }
}

class QuestDropRateWorldScript : public WorldScript
{
public:
    QuestDropRateWorldScript() : WorldScript("QuestDropRateWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE
    }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        if (reload)
            LogState();
    }

    void OnLoadCustomDatabaseTable() override
    {
        LoadMixedGroups();
        LogState();
    }

private:
    static void LogState()
    {
        if (!g_enabled.load())
        {
            LOG_INFO("module", "[QuestDropRate] Abgeschaltet.");
            return;
        }

        LOG_INFO("module", "[QuestDropRate] Aktiv: Questgegenstände mit Faktor {}, höchstens 100 %. {} Paare aus Gegenstand und Gruppe in gemischten Gruppen bleiben unverändert.",
            g_multiplier.load(), g_mixedGroupEntries.size());
    }
};

class QuestDropRateGlobalScript : public GlobalScript
{
public:
    QuestDropRateGlobalScript() : GlobalScript("QuestDropRateGlobalScript", {
        GLOBALHOOK_ON_ITEM_ROLL
    }) { }

    bool OnItemRoll(Player const* /*player*/, LootStoreItem const* item, float& chance, Loot& /*loot*/, LootStore const& /*store*/) override
    {
        if (!item->needs_quest)
            return true;

        QuestDropRate::Entry entry
        {
            true,
            item->reference,
            item->groupid,
            item->groupid != 0 && g_mixedGroupEntries.count(MixedKey(item->itemid, item->groupid)) != 0
        };

        chance = QuestDropRate::Apply(g_enabled.load(), g_multiplier.load(), entry, chance);

        // true: der Core würfelt normal weiter.
        return true;
    }
};

void AddQuestDropRateScripts()
{
    new QuestDropRateWorldScript();
    new QuestDropRateGlobalScript();
}
