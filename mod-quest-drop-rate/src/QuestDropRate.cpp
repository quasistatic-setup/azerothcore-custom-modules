/*
 * mod-quest-drop-rate
 *
 * Raises the drop chance of real quest items (LootStoreItem::needs_quest) by a
 * configurable factor, capped at 100 %. Normal loot, references and items
 * without a quest requirement stay unchanged; Rate.Drop.Item.* is not touched.
 *
 * Why a module: the core calls ScriptMgr::OnItemRoll for every entry with its
 * own chance before rolling (LootStoreItem::Roll and
 * LootTemplate::LootGroup::Roll), and the hook may change the chance. The
 * module therefore works the same for every loot type, fishing included,
 * without a core patch.
 *
 * Interplay with the core: for entries outside of groups, LootStoreItem::Roll
 * applies the quality factor Rate.Drop.Item.<quality> after the hook unless
 * the chance has reached 100. With factor 2 the effective chance thus doubles
 * exactly; anything reaching 100 always drops. Groups have no quality factor.
 *
 * Groups: entries of one group share a single roll. If a quest item sits in a
 * group together with normal items, any extra chance would be taken from
 * them, so such entries stay unchanged. The hook does not know the loot table
 * (entries from references arrive with the caller's store), so the exclusion
 * applies to every item/group pair that appears mixed anywhere.
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
    // OnItemRoll runs in the map threads; a config reload may change the
    // values at the same time.
    std::atomic<bool> g_enabled{true};
    std::atomic<float> g_multiplier{2.0f};

    // Filled only at startup, before any loot is generated; read-only afterwards.
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
        // Item IDs stay below 2^24, the group ID has 7 bits.
        return (itemId << 8) | groupId;
    }

    void LoadConfig()
    {
        bool enabled = sConfigMgr->GetOption<bool>("QuestDropRate.Enable", true);
        float multiplier = sConfigMgr->GetOption<float>("QuestDropRate.Multiplier", 2.0f);

        if (!QuestDropRate::IsValidMultiplier(multiplier))
        {
            LOG_ERROR("module", "[QuestDropRate] QuestDropRate.Multiplier = {} is invalid (minimum 1). Drop chances stay unchanged.", multiplier);
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
            LOG_INFO("module", "[QuestDropRate] Disabled.");
            return;
        }

        LOG_INFO("module", "[QuestDropRate] Active: quest items with factor {}, capped at 100 %. {} item/group pairs in mixed groups stay unchanged.",
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

        // true: the core continues rolling as usual.
        return true;
    }
};

void AddQuestDropRateScripts()
{
    new QuestDropRateWorldScript();
    new QuestDropRateGlobalScript();
}
