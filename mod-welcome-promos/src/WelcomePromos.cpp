/*
 * mod-welcome-promos
 *
 * Sends every newly created character a one-time welcome mail with historical
 * promotional and special items.
 *
 * Why a module and not mail_server_template: that system delivers on every
 * login and then records the character in mail_server_character. On a server
 * with Playerbots this hits every bot character, often thousands against a
 * few real ones, which would mean tens of thousands of items in bot
 * mailboxes. OnPlayerCreate only fires when a character is actually created,
 * so existing characters are left alone without registering them first.
 *
 * Collector's Edition rewards keep working through the core's account flags.
 * There is no overlap: they use different item IDs (13582 Zergling Leash,
 * 13583 Panda Collar, 13584 Diablo Stone).
 */

#include "AccountMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"

namespace
{
    constexpr char const* SENT_TABLE = "mod_welcome_promos_sent";
}

class WelcomePromosPlayerScript : public PlayerScript
{
public:
    WelcomePromosPlayerScript() : PlayerScript("WelcomePromosPlayerScript", {
        PLAYERHOOK_ON_CREATE,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // Called after the character creation has been committed
    // (CharacterHandler.cpp). The character exists in the database at this
    // point, so sending mail is safe.
    void OnPlayerCreate(Player* player) override
    {
        if (!sConfigMgr->GetOption<bool>("WelcomePromos.Enable", true) || !player)
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        if (IsExcludedAccount(player->GetSession() ? player->GetSession()->GetAccountId() : 0))
            return;

        // Safety net: the hook fires exactly once per character. Should that
        // ever change, the marker prevents a second delivery.
        if (AlreadyDelivered(guid))
            return;

        uint32 mails = SendPromoMails(player);
        if (!mails)
            return;

        MarkDelivered(guid, mails);
        LOG_INFO("module", "[WelcomePromos] {} ({}) received {} welcome mail(s).",
                 player->GetName(), guid, mails);
    }

    // Character GUIDs can be reused after a permanent deletion, so the delivery
    // marker has to be removed in the same transaction as the character.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `{}` WHERE guid = {}", SENT_TABLE, guid);
    }

private:
    // Skip bot accounts. The prefix is read from the Playerbots configuration
    // so both sides cannot drift apart.
    bool IsExcludedAccount(uint32 accountId) const
    {
        if (!accountId)
            return true;

        std::string name;
        if (!AccountMgr::GetName(accountId, name))
            return true;

        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        std::string prefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
        std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::toupper);
        if (!prefix.empty() && upper.rfind(prefix, 0) == 0)
            return true;

        // Additional comma-separated prefixes from this module's configuration.
        std::string extra = sConfigMgr->GetOption<std::string>("WelcomePromos.ExcludedAccountPrefixes", "");
        for (std::string_view part : Acore::Tokenize(extra, ',', false))
        {
            std::string p{ part };
            p.erase(0, p.find_first_not_of(" \t"));
            p.erase(p.find_last_not_of(" \t") + 1);
            if (p.empty())
                continue;
            std::transform(p.begin(), p.end(), p.begin(), ::toupper);
            if (upper.rfind(p, 0) == 0)
                return true;
        }

        return false;
    }

    bool AlreadyDelivered(ObjectGuid::LowType guid) const
    {
        QueryResult r = CharacterDatabase.Query("SELECT 1 FROM `{}` WHERE guid = {}", SENT_TABLE, guid);
        return static_cast<bool>(r);
    }

    void MarkDelivered(ObjectGuid::LowType guid, uint32 mails) const
    {
        CharacterDatabase.Execute(
            "INSERT INTO `{}` (guid, mail_count) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE mail_count = VALUES(mail_count)",
            SENT_TABLE, guid, mails);
    }

    std::vector<uint32> ConfiguredItems() const
    {
        std::vector<uint32> items;
        std::string raw = sConfigMgr->GetOption<std::string>("WelcomePromos.Items", "");

        for (std::string_view part : Acore::Tokenize(raw, ',', false))
        {
            Optional<uint32> entry = Acore::StringTo<uint32>(part);
            if (!entry || !*entry)
                continue;

            // Never send an item that does not exist: the mail would silently
            // leave it out.
            if (!sObjectMgr->GetItemTemplate(*entry))
            {
                LOG_ERROR("module", "[WelcomePromos] Item {} does not exist and is skipped.", *entry);
                continue;
            }

            if (std::find(items.begin(), items.end(), *entry) != items.end())
            {
                LOG_WARN("module", "[WelcomePromos] Item {} is configured twice and is sent only once.", *entry);
                continue;
            }

            items.push_back(*entry);
        }
        return items;
    }

    uint32 SendPromoMails(Player* player) const
    {
        std::vector<uint32> items = ConfiguredItems();
        if (items.empty())
        {
            LOG_WARN("module", "[WelcomePromos] No valid items configured, nothing is sent.");
            return 0;
        }

        uint32 senderEntry = sConfigMgr->GetOption<uint32>("WelcomePromos.SenderEntry", 0);
        std::string subject = sConfigMgr->GetOption<std::string>("WelcomePromos.Subject", "Special keepsakes");
        std::string body    = sConfigMgr->GetOption<std::string>("WelcomePromos.Body",
            "Welcome to Azeroth! Attached you will find some special keepsakes.");

        // One mail holds at most MAX_MAIL_ITEMS attachments. More items are
        // split across several mails automatically.
        std::size_t const perMail = MAX_MAIL_ITEMS;
        std::size_t const total   = items.size();
        uint32 const mailCount = static_cast<uint32>((total + perMail - 1) / perMail);

        uint32 sent = 0;
        for (std::size_t offset = 0; offset < total; offset += perMail)
        {
            std::size_t end = std::min(offset + perMail, total);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            MailSender sender = senderEntry
                ? MailSender(MAIL_CREATURE, senderEntry, MAIL_STATIONERY_DEFAULT)
                : MailSender(MAIL_NORMAL, player->GetGUID().GetCounter(), MAIL_STATIONERY_GM);

            std::string thisSubject = subject;
            if (mailCount > 1)
                thisSubject += " (" + std::to_string(sent + 1) + "/" + std::to_string(mailCount) + ")";

            MailDraft draft(thisSubject, body);

            bool any = false;
            for (std::size_t i = offset; i < end; ++i)
            {
                if (Item* item = Item::CreateItem(items[i], 1))
                {
                    item->SaveToDB(trans);
                    draft.AddItem(item);
                    any = true;
                }
                else
                    LOG_ERROR("module", "[WelcomePromos] Item {} could not be created.", items[i]);
            }

            if (!any)
            {
                CharacterDatabase.CommitTransaction(trans);
                continue;
            }

            draft.SendMailTo(trans, MailReceiver(player, player->GetGUID().GetCounter()), sender);
            CharacterDatabase.CommitTransaction(trans);
            ++sent;
        }

        return sent;
    }
};

class WelcomePromosWorldScript : public WorldScript
{
public:
    WelcomePromosWorldScript() : WorldScript("WelcomePromosWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED
    }) { }

    void OnBeforeWorldInitialized() override
    {
        // A single table, created in code so it is guaranteed to exist before
        // the first character can be created.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`guid` INT UNSIGNED NOT NULL,"
            "`mail_count` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`sent_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-welcome-promos: characters already served'",
            SENT_TABLE);
    }
};

void AddWelcomePromosScripts()
{
    new WelcomePromosPlayerScript();
    new WelcomePromosWorldScript();
}
