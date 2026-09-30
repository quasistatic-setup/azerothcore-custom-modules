/*
 * mod-group-speed
 *
 * .group speed #rate setzt für jedes online befindliche Mitglied der eigenen
 * Party oder des eigenen Raids (Playerbots eingeschlossen) dieselben
 * Geschwindigkeiten wie .modify speed all für einen ausgewählten Spieler.
 *
 * Warum ein Modul: Der Befehlsbaum führt die Tabellen aller CommandScripts
 * über den Namen zusammen (ChatCommandNode::LoadCommandsIntoMap). "speed"
 * hängt sich damit unter das vorhandene ".group" ein, ohne cs_group.cpp zu
 * verändern. Ein Core-Update überschreibt den Befehl daher nicht.
 *
 * Semantik bewusst identisch zu HandleModifyASpeedCommand und
 * CheckModifySpeed (cs_modify.cpp): Grenzen 0.1 bis 50, Sicherheitsstufe des
 * Ziels, keine Änderung während eines Taxiflugs, SetSpeed(..., true) für
 * walk, run, swim und flight. Wie dort gilt der Wert nur bis zum Logout.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Group.h"
#include "GroupReference.h"
#include "Language.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace
{
    // Dieselben Grenzen wie CheckModifySpeed in cs_modify.cpp.
    constexpr float MIN_SPEED = 0.1f;
    constexpr float MAX_SPEED = 50.0f;

    std::string CountMembers(uint32 count)
    {
        return std::to_string(count) + (count == 1 ? " member" : " members");
    }
}

class group_speed_commandscript : public CommandScript
{
public:
    group_speed_commandscript() : CommandScript("group_speed_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        // Wirksame Stufe kommt wie bei allen Befehlen aus acore_world.command;
        // die Modul-SQL übernimmt dort die Stufe von "modify speed all".
        static ChatCommandTable groupCommandTable =
        {
            { "speed", HandleGroupSpeedCommand, rbac::RBAC_PERM_COMMAND_MODIFY_SPEED_ALL, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "group", groupCommandTable }
        };

        return commandTable;
    }

    static bool HandleGroupSpeedCommand(ChatHandler* handler, float speed)
    {
        if (speed > MAX_SPEED || speed < MIN_SPEED)
        {
            handler->SendErrorMessage(LANG_BAD_VALUE);
            return false;
        }

        Player* caller = handler->GetPlayer();
        Group* group = caller ? caller->GetGroup() : nullptr;
        if (!group)
        {
            handler->SendErrorMessage("You are not in a party or raid.");
            return false;
        }

        uint32 changed = 0;
        uint32 skippedCount = 0;
        std::string skipped;

        auto skip = [&](Player const* member, char const* reason)
        {
            skipped += (skippedCount ? ", " : "") + member->GetName() + " (" + reason + ")";
            ++skippedCount;
        };

        // Die Mitgliederliste enthält nur geladene Spieler; Offline-Mitglieder
        // haben keine Referenz. Begleiter sind keine Gruppenmitglieder.
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;

            if (!member->IsInWorld())
            {
                skip(member, "loading");
                continue;
            }

            // Meldet selbst LANG_YOURS_SECURITY_IS_LOW, wie bei .modify speed all.
            if (handler->HasLowerSecurity(member))
            {
                skip(member, "higher security");
                continue;
            }

            if (member->IsInFlight())
            {
                skip(member, "in flight");
                continue;
            }

            member->SetSpeed(MOVE_WALK, speed, true);
            member->SetSpeed(MOVE_RUN, speed, true);
            member->SetSpeed(MOVE_SWIM, speed, true);
            member->SetSpeed(MOVE_FLIGHT, speed, true);
            ++changed;

            // Menschliche Mitspieler erfahren es wie bei .modify speed all;
            // Bots nicht, dort landete die Meldung ungelesen.
            if (!member->GetSession()->IsBot() && handler->needReportToTarget(member))
                ChatHandler(member->GetSession()).PSendSysMessage(LANG_YOURS_ASPEED_CHANGED, handler->GetNameLink(), speed);
        }

        if (!changed)
        {
            handler->SendErrorMessage("Group speed not changed; {} skipped: {}.", CountMembers(skippedCount), skipped);
            return false;
        }

        if (skippedCount)
            handler->PSendSysMessage("Group speed set to {:.2f} for {}; {} skipped: {}.",
                speed, CountMembers(changed), CountMembers(skippedCount), skipped);
        else
            handler->PSendSysMessage("Group speed set to {:.2f} for {}.", speed, CountMembers(changed));

        return true;
    }
};

void AddGroupSpeedScripts()
{
    new group_speed_commandscript();
}
