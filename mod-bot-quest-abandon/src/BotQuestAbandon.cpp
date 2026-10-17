/*
 * mod-bot-quest-abandon
 *
 * When a real player abandons a quest, the bots in their party or raid abandon
 * it as well.
 *
 * Why this is needed: with Playerbots.SyncQuestWithPlayer the bots take over
 * the quests of their player and finish them when the player hands them in. A
 * quest the player gives up stays in the bots' logs, though, and has to be
 * removed from every bot by hand.
 *
 * How: the PlayerScript::OnPlayerQuestAbandon hook fires after the core has
 * removed the quest from the player. Every bot of the group that holds the
 * quest then runs the same core handler a client abandon goes through, so
 * quest items, source items, timers and PvP state are cleaned up exactly as
 * for a player.
 *
 * Bots are recognised through WorldSession::IsHeadless(), which mod-playerbots
 * sets for its sessions; selfbots run on the client session and count as real
 * players. The module therefore needs no Playerbot headers. A bot that
 * abandons a quest itself does not pass it on.
 *
 * Thread safety: the hook runs on the map thread of the player. Only bots on
 * the same map are changed; a bot on another map keeps the quest.
 */

#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    std::atomic<bool> g_enabled{true};

    // Names listed in one message; the rest is only counted, so a raid stays
    // within one chat line. Same layout as the quest group messages of
    // mod-playerbots.
    constexpr std::size_t MaxNamedBots = 5;

    // Clickable quest link, built like ChatHelper::FormatQuest in mod-playerbots.
    std::string QuestLink(uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return "[" + std::to_string(questId) + "]";

        std::string title;
        if (QuestLocale const* locale = sObjectMgr->GetQuestLocale(questId))
            if (locale->Title.size() > sWorld->GetDefaultDbcLocale())
                title = locale->Title[sWorld->GetDefaultDbcLocale()];
        if (title.empty())
            title = quest->GetTitle();

        std::ostringstream out;
        out << "|cFFFFFF00|Hquest:" << questId << ':' << quest->GetQuestLevel() << "|h[" << title << "]|h|r";
        return out.str();
    }

    // "Quest abandoned: [Quest] - Tank" for one bot,
    // "Quest abandoned: [Quest] - 4 bots: Heal, Hunt, Mage, Tank" for several,
    // names beyond MaxNamedBots as "+N".
    std::string FormatMessage(uint32 questId, std::vector<std::string> names)
    {
        std::sort(names.begin(), names.end());
        std::string message = "Quest abandoned: " + QuestLink(questId);

        if (names.size() == 1)
            return message + " - " + names.front();

        std::size_t const named = std::min(names.size(), MaxNamedBots);
        message += " - " + std::to_string(names.size()) + " bots";
        for (std::size_t i = 0; i < named; ++i)
            message += (i == 0 ? ": " : ", ") + names[i];
        if (names.size() > named)
            message += " +" + std::to_string(names.size() - named);

        return message;
    }

    bool IsBot(Player const* player)
    {
        WorldSession* session = player->GetSession();
        return session && session->IsHeadless();
    }

    // Runs the client abandon handler for the bot, so the bot loses the quest
    // the same way the player did.
    bool AbandonOnBot(Player* bot, uint32 questId)
    {
        uint16 slot = bot->FindQuestSlot(questId);
        if (slot >= MAX_QUEST_LOG_SIZE)
            return false;

        WorldPacket data(CMSG_QUESTLOG_REMOVE_QUEST, 1);
        data << uint8(slot);
        WorldPackets::Quest::QuestLogRemoveQuest packet(std::move(data));
        packet.Read();
        bot->GetSession()->HandleQuestLogRemoveQuest(packet);

        // The core refuses when a source item cannot be taken back.
        return bot->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE;
    }

    void LoadConfig()
    {
        bool enabled = sConfigMgr->GetOption<bool>("BotQuestAbandon.Enable", true);
        g_enabled.store(enabled);

        if (!enabled)
            LOG_INFO("module", "[BotQuestAbandon] Disabled.");
        else
            LOG_INFO("module", "[BotQuestAbandon] Active: group bots abandon the quests their player abandons.");
    }
}

class BotQuestAbandonWorldScript : public WorldScript
{
public:
    BotQuestAbandonWorldScript() : WorldScript("BotQuestAbandonWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class BotQuestAbandonPlayerScript : public PlayerScript
{
public:
    BotQuestAbandonPlayerScript() : PlayerScript("BotQuestAbandonPlayerScript", {
        PLAYERHOOK_ON_QUEST_ABANDON
    }) { }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (!g_enabled.load(std::memory_order_relaxed) || IsBot(player))
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        std::vector<std::string> names;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !member->IsInWorld() || !IsBot(member))
                continue;

            if (member->GetMap() != player->GetMap())
                continue;

            if (!AbandonOnBot(member, questId))
                continue;

            names.push_back(member->GetName());
        }

        if (names.empty())
            return;

        std::string const message = FormatMessage(questId, names);
        ChatHandler(player->GetSession()).SendSysMessage(message);

        LOG_DEBUG("module", "[BotQuestAbandon] {} abandoned quest {}, {} bots followed.",
            player->GetName(), questId, names.size());
    }
};

void AddBotQuestAbandonScripts()
{
    new BotQuestAbandonWorldScript();
    new BotQuestAbandonPlayerScript();
}
