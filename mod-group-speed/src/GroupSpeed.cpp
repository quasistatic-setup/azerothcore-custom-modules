/*
 * mod-group-speed
 *
 * .group speed #rate sets, for every online member of the caller's party or
 * raid (Playerbots included), the same speeds as .modify speed all does for a
 * selected player.
 *
 * Why a module: the command tree merges the tables of all CommandScripts by
 * name (ChatCommandNode::LoadCommandsIntoMap). "speed" therefore attaches
 * below the existing ".group" without changing cs_group.cpp, and core updates
 * do not overwrite the command.
 *
 * Semantics deliberately identical to HandleModifyASpeedCommand and
 * CheckModifySpeed (cs_modify.cpp): limits 0.1 to 50, target security level,
 * no change during a taxi flight, SetSpeed(..., true) for walk, run, swim and
 * flight. As there, the value only lasts until logout.
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
    // Same limits as CheckModifySpeed in cs_modify.cpp.
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
        // As for all commands, the effective level comes from
        // acore_world.command; the module SQL copies the level of
        // "modify speed all" there.
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

        // The member list only contains loaded players; offline members have
        // no reference. Pets are not group members.
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

            // Reports LANG_YOURS_SECURITY_IS_LOW itself, like .modify speed all.
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

            // Human members are told, like with .modify speed all; bots are
            // not, the message would go unread there.
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
