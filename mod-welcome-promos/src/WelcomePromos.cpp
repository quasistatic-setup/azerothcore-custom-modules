/*
 * mod-welcome-promos
 *
 * Schickt jedem neu erstellten Charakter einmalig eine Willkommensmail mit
 * historischen Aktions- und Sondergegenstaenden.
 *
 * Warum ein Modul und nicht mail_server_template: Jenes stellt bei jedem
 * Login zu und traegt den Charakter danach in mail_server_character ein.
 * Auf einem Server mit Playerbots betrifft das saemtliche Bot-Charaktere,
 * oft Tausende gegenueber wenigen echten. Das waeren zehntausende erzeugte
 * Gegenstaende in Bot-Postfaechern. OnPlayerCreate loest dagegen nur bei
 * tatsaechlicher Neuerstellung aus; bestehende Charaktere bleiben damit
 * unberuehrt, ohne dass man sie vorher eintragen muesste.
 *
 * Die Belohnungen der Sammleredition laufen weiter ueber die Kontokennzeichen
 * des Cores. Es gibt keine Ueberschneidung: Jene verwenden andere
 * Gegenstandsnummern (13582 Zergling Leash, 13583 Panda Collar,
 * 13584 Diablo Stone).
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

    // Wird nach dem erfolgreichen Festschreiben der Charaktererstellung
    // aufgerufen (CharacterHandler.cpp). Der Charakter steht zu diesem
    // Zeitpunkt in der Datenbank, der Mailversand ist also sicher.
    void OnPlayerCreate(Player* player) override
    {
        if (!sConfigMgr->GetOption<bool>("WelcomePromos.Enable", true) || !player)
            return;

        ObjectGuid::LowType guid = player->GetGUID().GetCounter();

        if (IsExcludedAccount(player->GetSession() ? player->GetSession()->GetAccountId() : 0))
            return;

        // Sicherheitsnetz: Der Hook feuert je Charakter genau einmal. Sollte
        // sich das je aendern, verhindert die Marke eine zweite Sendung.
        if (AlreadyDelivered(guid))
            return;

        uint32 mails = SendPromoMails(player);
        if (!mails)
            return;

        MarkDelivered(guid, mails);
        LOG_INFO("module", "[WelcomePromos] {} ({}) hat {} Willkommensmail(s) erhalten.",
                 player->GetName(), guid, mails);
    }

    // Charakter-GUIDs koennen nach einer endgueltigen Loeschung erneut
    // vergeben werden. Deshalb muss die Versandmarke in derselben Transaktion
    // wie der Charakter entfernt werden.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `{}` WHERE guid = {}", SENT_TABLE, guid);
    }

private:
    // Bot-Konten aussen vor lassen. Der Praefix wird aus der Playerbots-
    // Konfiguration gelesen, damit beide Seiten nicht auseinanderlaufen.
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

        // Zusaetzliche Praefixe aus der eigenen Konfiguration, kommagetrennt.
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

            // Nie einen Gegenstand verschicken, den es nicht gibt: Der
            // Mailversand wuerde ihn stillschweigend auslassen.
            if (!sObjectMgr->GetItemTemplate(*entry))
            {
                LOG_ERROR("module", "[WelcomePromos] Gegenstand {} existiert nicht und wird uebersprungen.", *entry);
                continue;
            }

            if (std::find(items.begin(), items.end(), *entry) != items.end())
            {
                LOG_WARN("module", "[WelcomePromos] Gegenstand {} ist doppelt konfiguriert und wird nur einmal verschickt.", *entry);
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
            LOG_WARN("module", "[WelcomePromos] Keine gueltigen Gegenstaende konfiguriert, es wird nichts verschickt.");
            return 0;
        }

        uint32 senderEntry = sConfigMgr->GetOption<uint32>("WelcomePromos.SenderEntry", 0);
        std::string subject = sConfigMgr->GetOption<std::string>("WelcomePromos.Subject", "Besondere Erinnerungsstuecke");
        std::string body    = sConfigMgr->GetOption<std::string>("WelcomePromos.Body",
            "Willkommen in Azeroth! Im Anhang findest du einige besondere Erinnerungsstuecke.");

        // Ein Brief fasst hoechstens MAX_MAIL_ITEMS Anhaenge. Bei mehr
        // Gegenstaenden wird automatisch auf mehrere Briefe aufgeteilt.
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
                    LOG_ERROR("module", "[WelcomePromos] Gegenstand {} liess sich nicht erzeugen.", items[i]);
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
        // Eine einzige Tabelle; im Code angelegt, damit sie sicher vorhanden
        // ist, bevor der erste Charakter erstellt werden kann.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `{}` ("
            "`guid` INT UNSIGNED NOT NULL,"
            "`mail_count` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`sent_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci "
            "COMMENT='mod-welcome-promos: bereits belieferte Charaktere'",
            SENT_TABLE);
    }
};

void AddWelcomePromosScripts()
{
    new WelcomePromosPlayerScript();
    new WelcomePromosWorldScript();
}
