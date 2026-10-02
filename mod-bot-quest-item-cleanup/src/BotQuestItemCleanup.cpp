/*
 * mod-bot-quest-item-cleanup
 *
 * Removes the item a quest hands out on accept from a bot's bags once the bot
 * is rewarded for that quest.
 *
 * Why this is needed: Player::RewardQuest takes the required items and the
 * ItemDrop items, but not the source item (quest_template.StartItem). A player
 * normally uses it up while doing the quest. A bot that completes a quest
 * without playing it, for example with AiPlayerbot.SyncQuestWithPlayer, never
 * uses it, so empty phials, sampling tubes and similar items pile up in its
 * bags for good.
 *
 * What is removed: only items of class Quest or with quest binding, so a
 * provided item with a use of its own is never touched. The item stays when
 * another quest in the bot's log still needs it, hands it out itself, or when
 * it starts a quest the bot has not been rewarded for yet.
 *
 * Bots are recognised through WorldSession::IsBot(), which mod-playerbots sets
 * for its sessions; selfbots run on the client session and count as real
 * players. The module therefore needs no Playerbot headers. Real players are
 * never touched.
 *
 * Cost: the hook runs once per rewarded quest, not per update. The
 * configuration is atomic because map threads run in parallel.
 */

#include "Config.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <atomic>

namespace
{
    std::atomic<bool> g_enabled{true};

    // Another quest in the log hands out the same item on accept.
    bool IsSourceItemOfActiveQuest(Player* player, uint32 itemId, uint32 excludeQuestId)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            if (!questId || questId == excludeQuestId)
                continue;

            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                if (quest->GetSrcItemId() == itemId)
                    return true;
        }

        return false;
    }

    void LoadConfig()
    {
        bool enabled = sConfigMgr->GetOption<bool>("BotQuestItemCleanup.Enable", true);
        g_enabled.store(enabled);

        if (!enabled)
            LOG_INFO("module", "[BotQuestItemCleanup] Disabled.");
        else
            LOG_INFO("module", "[BotQuestItemCleanup] Active: bots drop the source item of a quest "
                "when they are rewarded for it.");
    }
}

class BotQuestItemCleanupWorldScript : public WorldScript
{
public:
    BotQuestItemCleanupWorldScript() : WorldScript("BotQuestItemCleanupWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class BotQuestItemCleanupPlayerScript : public PlayerScript
{
public:
    BotQuestItemCleanupPlayerScript() : PlayerScript("BotQuestItemCleanupPlayerScript", {
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST
    }) { }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!g_enabled.load(std::memory_order_relaxed))
            return;

        WorldSession* session = player->GetSession();
        if (!session || !session->IsBot())
            return;

        uint32 itemId = quest->GetSrcItemId();
        if (!itemId)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || (proto->Class != ITEM_CLASS_QUEST && proto->Bonding != BIND_QUEST_ITEM))
            return;

        uint32 questId = quest->GetQuestId();

        // The item starts a quest the bot can still take.
        if (proto->StartQuest && proto->StartQuest != questId && !player->GetQuestRewardStatus(proto->StartQuest))
            return;

        if (player->HasQuestForItem(itemId, questId, true) || IsSourceItemOfActiveQuest(player, itemId, questId))
            return;

        uint32 count = player->GetItemCount(itemId, true);
        if (!count)
            return;

        player->DestroyItemCount(itemId, count, true);

        LOG_DEBUG("module", "[BotQuestItemCleanup] {}: removed {}x item {} after quest {}.",
            player->GetName(), count, itemId, questId);
    }
};

void AddBotQuestItemCleanupScripts()
{
    new BotQuestItemCleanupWorldScript();
    new BotQuestItemCleanupPlayerScript();
}
