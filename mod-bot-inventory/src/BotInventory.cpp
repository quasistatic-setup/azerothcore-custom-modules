/*
 * mod-bot-inventory
 *
 * .botinv commands that let a player manage the bags, money and enchants of
 * the Playerbots in their party or raid without opening trade windows:
 *
 *   .botinv list                                  bags, equipment and money
 *   .botinv move <from> <to> <itemGuid>...        move whole stacks
 *   .botinv gold <from> <to> <copper>             move money
 *   .botinv sell <owner> <itemGuid>...            sell stacks at the targeted vendor
 *   .botinv destroy <owner> <itemGuid>...         destroy stacks
 *   .botinv enchants <owner> <itemGuid>           enchants available for an item
 *   .botinv enchant <caster> <spell> <owner> <itemGuid>
 *
 * Why chat commands: the core answers a command sent through the addon
 * channel ("AzerothCore\t" prefix, AddonChannelCommandHandler) with addon
 * messages instead of chat lines. A client addon therefore gets a request and
 * reply protocol for free, and the same commands still work when typed. The
 * output is line based and '~' separated for that reason.
 *
 * Who may be touched: the caller and bots in the caller's group. Bots are
 * recognised through WorldSession::IsHeadless(), which mod-playerbots sets for
 * its sessions, so the module needs no Playerbot headers. Other real players
 * are never a source or a target.
 *
 * Transfers follow the trade window: Item::CanBeTraded, no quest-bound items,
 * CanStoreItem on the receiver, and both inventories saved in one transaction.
 *
 * Selling pays the vendor price to the item's owner, as the vendor window
 * would; the caller must stand at a vendor and have it targeted, the bot may
 * be anywhere. There is no buyback.
 *
 * Enchants are applied the way Spell::EffectEnchantItemPerm does it, without
 * casting the spell. A real cast cannot target an item in somebody else's
 * bags: SpellCastTargets::Update only resolves the caster's own items or a
 * trade slot. The checks of Spell::CheckItems for enchant effects are repeated
 * here, reagents are taken from the enchanter and the skill-up roll is kept.
 */

#include "Bag.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <atomic>
#include <optional>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    std::atomic<bool> g_enabled{true};
    std::atomic<float> g_maxDistance{0.0f};

    // Addon messages are limited to 255 bytes; the core adds an 18 byte header.
    constexpr std::size_t MAX_LINE = 200;

    constexpr uint32 FLAG_MOVABLE = 0x1;

    void LoadConfig()
    {
        g_enabled.store(sConfigMgr->GetOption<bool>("BotInventory.Enable", true));
        g_maxDistance.store(sConfigMgr->GetOption<float>("BotInventory.MaxDistance", 0.0f));

        if (!g_enabled.load())
            LOG_INFO("module", "[BotInventory] Disabled.");
        else
            LOG_INFO("module", "[BotInventory] Active: .botinv moves items and money between a player "
                "and the bots in their group.");
    }

    bool Fail(ChatHandler* handler, std::string const& message)
    {
        handler->SendErrorMessage(message, false);
        return false;
    }

    bool IsBot(Player const* player)
    {
        WorldSession* session = player->GetSession();
        return session && session->IsHeadless();
    }

    // The caller first, then every bot of the caller's group that is in the world.
    std::vector<Player*> GetParticipants(Player* caller)
    {
        std::vector<Player*> result{ caller };

        if (Group* group = caller->GetGroup())
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member != caller && member->IsInWorld() && IsBot(member))
                    result.push_back(member);
            }

        return result;
    }

    Player* FindParticipant(Player* caller, std::string name)
    {
        if (!normalizePlayerName(name))
            return nullptr;

        for (Player* player : GetParticipants(caller))
            if (player->GetName() == name)
                return player;

        return nullptr;
    }

    // Reason why a character cannot give or receive right now, or nullptr.
    char const* BusyReason(Player const* player)
    {
        if (!player->IsInWorld() || player->IsBeingTeleported())
            return "is loading";
        if (player->IsInCombat())
            return "is in combat";
        if (player->GetTradeData())
            return "has a trade window open";
        return nullptr;
    }

    bool CheckPair(ChatHandler* handler, Player* a, Player* b)
    {
        for (Player* player : { a, b })
            if (char const* reason = BusyReason(player))
                return Fail(handler, player->GetName() + " " + reason + ".");

        float maxDistance = g_maxDistance.load(std::memory_order_relaxed);
        if (a != b && maxDistance > 0.0f && (!a->IsInMap(b) || a->GetDistance(b) > maxDistance))
            return Fail(handler, a->GetName() + " and " + b->GetName() + " are too far apart.");

        return true;
    }

    // Reason why an item cannot leave its owner's bags, or nullptr.
    char const* MoveBlockReason(Item const* item)
    {
        if (!Player::IsInventoryPos(item->GetPos()))
            return "is not in a bag";
        if (item->GetTemplate()->Bonding == BIND_QUEST_ITEM || item->GetTemplate()->Class == ITEM_CLASS_QUEST)
            return "is a quest item";
        if (item->IsSoulBound())
            return "is soulbound";
        if (!item->CanBeTraded())
            return "cannot be traded";
        return nullptr;
    }

    void SavePair(Player* a, Player* b)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        a->SaveInventoryAndGoldToDB(trans);
        if (b != a)
            b->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    Item* FindItem(Player* owner, std::string_view guidText)
    {
        Optional<uint32> low = Acore::StringTo<uint32>(guidText);
        if (!low || !*low)
            return nullptr;

        return owner->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(*low));
    }

    // Collects records into lines of at most MAX_LINE bytes behind a fixed head.
    class LineWriter
    {
    public:
        LineWriter(ChatHandler* handler, std::string head) : _handler(handler), _head(std::move(head)), _line(_head) { }

        ~LineWriter() { Flush(); }

        void Add(std::string const& record)
        {
            if (_line.size() > _head.size() && _line.size() + record.size() + 1 > MAX_LINE)
                Flush();

            _line += record;
            _line += ';';
        }

    private:
        void Flush()
        {
            if (_line.size() > _head.size())
                _handler->SendSysMessage(_line);
            _line = _head;
        }

        ChatHandler* _handler;
        std::string _head;
        std::string _line;
    };

    void AddItemRecord(LineWriter& writer, Item const* item)
    {
        uint32 flags = MoveBlockReason(item) ? 0 : FLAG_MOVABLE;

        writer.Add(Acore::StringFormat("{},{},{},{},{},{},{},{}",
            item->GetBagSlot(), item->GetSlot(), item->GetEntry(), item->GetCount(),
            item->GetGUID().GetCounter(), item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT),
            item->GetItemRandomPropertyId(), flags));
    }

    void SendInventory(ChatHandler* handler, Player* caller, Player* player)
    {
        handler->SendSysMessage(Acore::StringFormat("P~{}~{}~{}~{}~{}",
            player->GetName(), player->getClass(), player->GetMoney(), player->GetFreeInventorySpace(),
            player == caller ? 1 : 0));

        LineWriter writer(handler, "I~" + player->GetName() + "~");

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                AddItemRecord(writer, item);

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                AddItemRecord(writer, item);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        AddItemRecord(writer, item);
    }

    struct EnchantSpell
    {
        SpellInfo const* info;
        SpellItemEnchantmentEntry const* enchant;
        EnchantmentSlot slot;
    };

    // The permanent or socket enchant a spell applies, if it is one.
    std::optional<EnchantSpell> GetEnchantSpell(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return std::nullopt;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            EnchantmentSlot slot;
            if (info->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                slot = PERM_ENCHANTMENT_SLOT;
            else if (info->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC)
                slot = PRISMATIC_ENCHANTMENT_SLOT;
            else
                continue;

            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(info->Effects[i].MiscValue);
            if (!enchant)
                return std::nullopt;

            return EnchantSpell{ info, enchant, slot };
        }

        return std::nullopt;
    }

    // Same rules as Spell::CheckItems for enchant effects. Returns the reason
    // why the enchant does not go on this item, or nullptr.
    char const* EnchantFitError(Player const* caster, Player const* owner, Item const* item, EnchantSpell const& spell)
    {
        ItemTemplate const* proto = item->GetTemplate();

        // A vellum turns into a scroll in the enchanter's own bags; that is a
        // normal cast and needs no help from here.
        if (proto->IsWeaponVellum() || proto->IsArmorVellum())
            return "vellums are not supported";

        if (!item->IsFitToSpellRequirements(spell.info))
            return "does not fit this item";

        if (!spell.info->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF))
        {
            uint32 requiredLevel = proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel;
            if (requiredLevel < spell.info->BaseLevel)
                return "item level is too low";
        }

        bool isItemUsable = false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (proto->Spells[i].SpellId && (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE ||
                proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE))
                isItemUsable = true;

        bool addsSocket = false;
        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            if (spell.enchant->type[i] == ITEM_ENCHANTMENT_TYPE_USE_SPELL && isItemUsable)
                return "item already has a use effect";

            if (spell.enchant->type[i] == ITEM_ENCHANTMENT_TYPE_PRISMATIC_SOCKET)
            {
                addsSocket = true;

                uint32 sockets = 0;
                for (uint32 socket = 0; socket < MAX_ITEM_PROTO_SOCKETS; ++socket)
                    if (proto->Socket[socket].Color)
                        ++sockets;

                if (sockets == MAX_ITEM_PROTO_SOCKETS || item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT))
                    return "item cannot take another socket";
            }
        }

        if (spell.slot == PRISMATIC_ENCHANTMENT_SLOT && !addsSocket)
            return "unsupported enchant";

        if (owner != caster && (spell.enchant->slot & ENCHANTMENT_CAN_SOULBOUND))
            return "only works on the enchanter's own items";

        if (spell.enchant->requiredSkill &&
            owner->GetSkillValue(spell.enchant->requiredSkill) < spell.enchant->requiredSkillValue)
            return "the item's owner lacks the required profession";

        return nullptr;
    }

    std::string ItemName(uint32 itemId, int localeIndex)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return "item " + std::to_string(itemId);

        std::string name = proto->Name1;
        if (localeIndex >= 0)
            if (ItemLocale const* locale = sObjectMgr->GetItemLocale(itemId))
                ObjectMgr::GetLocaleString(locale->Name, localeIndex, name);

        return name;
    }

    // An item that provides a tool category, to name the category by. The
    // core does not load the category names of TotemCategory.dbc. The lowest
    // entry wins so the answer does not depend on the order of the store.
    uint32 GetToolItem(uint32 totemCategory)
    {
        // Commands run on the world thread only.
        static std::unordered_map<uint32, uint32> cache;

        auto cached = cache.find(totemCategory);
        if (cached != cache.end())
            return cached->second;

        uint32 found = 0;
        for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
            if (proto.TotemCategory == totemCategory && (!found || itemId < found))
                found = itemId;

        cache[totemCategory] = found;
        return found;
    }

    // Reagents and tools the enchanter is missing, in the caller's language,
    // empty when complete.
    std::string MissingMaterials(Player const* caster, SpellInfo const* info, int localeIndex)
    {
        std::string missing;
        auto add = [&](std::string const& text)
        {
            missing += (missing.empty() ? "" : " + ") + text;
        };

        if (!caster->CanNoReagentCast(info))
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (info->Reagent[i] <= 0)
                    continue;

                uint32 itemId = info->Reagent[i];
                if (!caster->HasItemCount(itemId, info->ReagentCount[i]))
                    add(std::to_string(info->ReagentCount[i]) + "x " + ItemName(itemId, localeIndex));
            }

        for (uint32 totem : info->Totem)
            if (totem && !caster->HasItemCount(totem))
                add(ItemName(totem, localeIndex));

        for (uint32 category : info->TotemCategory)
            if (category && !caster->HasItemTotemCategory(category))
            {
                uint32 tool = GetToolItem(category);
                add(tool ? ItemName(tool, localeIndex) : "the required tool");
            }

        return missing;
    }

    // Keeps free text from breaking the '~', ';' and ',' separated records.
    std::string RecordText(std::string text)
    {
        for (char& c : text)
            if (c == '~' || c == ';' || c == ',')
                c = ' ';

        return text;
    }
}

class bot_inventory_commandscript : public CommandScript
{
public:
    bot_inventory_commandscript() : CommandScript("bot_inventory_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable botInvCommandTable =
        {
            { "list",     HandleListCommand,     SEC_PLAYER, Console::No },
            { "move",     HandleMoveCommand,     SEC_PLAYER, Console::No },
            { "gold",     HandleGoldCommand,     SEC_PLAYER, Console::No },
            { "sell",     HandleSellCommand,     SEC_PLAYER, Console::No },
            { "destroy",  HandleDestroyCommand,  SEC_PLAYER, Console::No },
            { "enchants", HandleEnchantsCommand, SEC_PLAYER, Console::No },
            { "enchant",  HandleEnchantCommand,  SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "botinv", botInvCommandTable }
        };

        return commandTable;
    }

    static Player* GetCaller(ChatHandler* handler)
    {
        if (!g_enabled.load(std::memory_order_relaxed))
        {
            handler->SendErrorMessage("mod-bot-inventory is disabled.", false);
            return nullptr;
        }

        return handler->GetPlayer();
    }

    static Player* GetParticipant(ChatHandler* handler, Player* caller, std::string const& name)
    {
        Player* player = FindParticipant(caller, name);
        if (!player)
            handler->SendErrorMessage(name + " is neither you nor a bot in your group.", false);

        return player;
    }

    static bool HandleListCommand(ChatHandler* handler)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        for (Player* player : GetParticipants(caller))
            SendInventory(handler, caller, player);

        return true;
    }

    static bool HandleMoveCommand(ChatHandler* handler, std::string fromName, std::string toName, Tail guids)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        Player* from = GetParticipant(handler, caller, fromName);
        Player* to = from ? GetParticipant(handler, caller, toName) : nullptr;
        if (!from || !to)
            return false;

        if (from == to)
            return Fail(handler, "Source and target are the same character.");

        if (!CheckPair(handler, from, to))
            return false;

        std::vector<std::string_view> tokens = Acore::Tokenize(guids, ' ', false);
        if (tokens.empty())
            return Fail(handler, "No item given.");

        uint32 moved = 0;
        for (std::string_view token : tokens)
        {
            auto report = [&](char const* reason)
            {
                handler->SendSysMessage(Acore::StringFormat("M~{}~0~{}", token, reason));
            };

            Item* item = FindItem(from, token);
            if (!item)
            {
                report("not found");
                continue;
            }

            if (char const* reason = MoveBlockReason(item))
            {
                report(reason);
                continue;
            }

            ItemPosCountVec dest;
            if (to->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            {
                report("receiver has no room or may not carry it");
                continue;
            }

            uint32 entry = item->GetEntry();
            uint32 count = item->GetCount();

            from->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            to->MoveItemToInventory(dest, item, true, true);
            ++moved;

            handler->SendSysMessage(Acore::StringFormat("M~{}~1~", token));
            LOG_INFO("module", "[BotInventory] {}: moved {}x item {} from {} to {}.",
                caller->GetName(), count, entry, from->GetName(), to->GetName());
        }

        if (moved)
            SavePair(from, to);

        return true;
    }

    // Shared frame of sell and destroy: resolves the owner and walks the
    // guids. `act` handles one bag item and returns the reason it refused,
    // or nullptr when the item is gone.
    template <typename Action>
    static bool ForEachBagItem(ChatHandler* handler, std::string const& ownerName, std::string_view guids, Action&& act)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        Player* owner = GetParticipant(handler, caller, ownerName);
        if (!owner)
            return false;

        if (!CheckPair(handler, owner, owner))
            return false;

        std::vector<std::string_view> tokens = Acore::Tokenize(guids, ' ', false);
        if (tokens.empty())
            return Fail(handler, "No item given.");

        uint32 done = 0;
        for (std::string_view token : tokens)
        {
            Item* item = FindItem(owner, token);
            char const* reason = nullptr;
            if (!item)
                reason = "not found";
            else if (!Player::IsInventoryPos(item->GetPos()))
                reason = "is not in a bag";
            else if (item->IsNotEmptyBag())
                reason = "is a bag with content";
            else
                reason = act(caller, owner, item);

            if (reason)
            {
                handler->SendSysMessage(Acore::StringFormat("M~{}~0~{}", token, reason));
                continue;
            }

            handler->SendSysMessage(Acore::StringFormat("M~{}~1~", token));
            ++done;
        }

        if (done)
            SavePair(owner, owner);

        return true;
    }

    static bool HandleSellCommand(ChatHandler* handler, std::string ownerName, Tail guids)
    {
        Player* caller = handler->GetPlayer();
        Creature* vendor = caller ? caller->GetNPCIfCanInteractWith(caller->GetTarget(), UNIT_NPC_FLAG_VENDOR) : nullptr;
        if (!vendor || vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR))
            return Fail(handler, "Target a vendor next to you first.");

        uint32 total = 0;
        bool result = ForEachBagItem(handler, ownerName, guids, [&](Player* who, Player* owner, Item* item) -> char const*
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto->SellPrice)
                return "has no vendor price";

            uint32 count = item->GetCount();
            uint32 money = proto->SellPrice * count;
            if (owner->GetMoney() > MAX_MONEY_AMOUNT - money)
                return "owner cannot carry more money";

            uint32 entry = item->GetEntry();
            owner->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            owner->ModifyMoney(int32(money));
            total += money;

            LOG_INFO("module", "[BotInventory] {}: sold {}x item {} of {} for {} copper.",
                who->GetName(), count, entry, owner->GetName(), money);
            return nullptr;
        });

        if (total)
            handler->SendSysMessage(Acore::StringFormat("T~{}", total));

        return result;
    }

    static bool HandleDestroyCommand(ChatHandler* handler, std::string ownerName, Tail guids)
    {
        return ForEachBagItem(handler, ownerName, guids, [](Player* who, Player* owner, Item* item) -> char const*
        {
            uint32 entry = item->GetEntry();
            uint32 count = item->GetCount();
            owner->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

            LOG_INFO("module", "[BotInventory] {}: destroyed {}x item {} of {}.",
                who->GetName(), count, entry, owner->GetName());
            return nullptr;
        });
    }

    static bool HandleGoldCommand(ChatHandler* handler, std::string fromName, std::string toName, uint32 copper)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        Player* from = GetParticipant(handler, caller, fromName);
        Player* to = from ? GetParticipant(handler, caller, toName) : nullptr;
        if (!from || !to)
            return false;

        if (from == to)
            return Fail(handler, "Source and target are the same character.");

        if (!copper)
            return Fail(handler, "No amount given.");

        if (!CheckPair(handler, from, to))
            return false;

        if (from->GetMoney() < copper)
            return Fail(handler, from->GetName() + " does not have that much money.");

        if (copper > MAX_MONEY_AMOUNT || to->GetMoney() > MAX_MONEY_AMOUNT - copper)
            return Fail(handler, to->GetName() + " cannot carry that much money.");

        from->ModifyMoney(-int32(copper));
        to->ModifyMoney(int32(copper));
        SavePair(from, to);

        handler->SendSysMessage(Acore::StringFormat("G~{}~{}~{}", from->GetName(), to->GetName(), copper));
        LOG_INFO("module", "[BotInventory] {}: moved {} copper from {} to {}.",
            caller->GetName(), copper, from->GetName(), to->GetName());

        return true;
    }

    static bool HandleEnchantsCommand(ChatHandler* handler, std::string ownerName, std::string guid)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        Player* owner = GetParticipant(handler, caller, ownerName);
        if (!owner)
            return false;

        Item* item = FindItem(owner, guid);
        if (!item)
            return Fail(handler, "Item not found.");

        for (Player* caster : GetParticipants(caller))
        {
            LineWriter writer(handler, "C~" + caster->GetName() + "~");

            for (auto const& [spellId, playerSpell] : caster->GetSpellMap())
            {
                if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
                    continue;

                std::optional<EnchantSpell> spell = GetEnchantSpell(spellId);
                if (!spell || EnchantFitError(caster, owner, item, *spell))
                    continue;

                std::string missing = MissingMaterials(caster, spell->info, handler->GetSessionDbLocaleIndex());
                writer.Add(Acore::StringFormat("{},{},{}", spellId, missing.empty() ? 1 : 0, RecordText(missing)));
            }
        }

        return true;
    }

    static bool HandleEnchantCommand(ChatHandler* handler, std::string casterName, uint32 spellId, std::string ownerName, std::string guid)
    {
        Player* caller = GetCaller(handler);
        if (!caller)
            return false;

        Player* caster = GetParticipant(handler, caller, casterName);
        Player* owner = caster ? GetParticipant(handler, caller, ownerName) : nullptr;
        if (!caster || !owner)
            return false;

        if (!CheckPair(handler, caster, owner))
            return false;

        Item* item = FindItem(owner, guid);
        if (!item)
            return Fail(handler, "Item not found.");

        std::optional<EnchantSpell> spell = GetEnchantSpell(spellId);
        if (!spell || !caster->HasActiveSpell(spellId))
            return Fail(handler, caster->GetName() + " does not know that enchant.");

        if (char const* reason = EnchantFitError(caster, owner, item, *spell))
            return Fail(handler, std::string("Enchant ") + reason + ".");

        std::string missing = MissingMaterials(caster, spell->info, handler->GetSessionDbLocaleIndex());
        if (!missing.empty())
            return Fail(handler, caster->GetName() + " is missing " + missing + ".");

        if (!caster->CanNoReagentCast(spell->info))
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (spell->info->Reagent[i] > 0)
                    caster->DestroyItemCount(spell->info->Reagent[i], spell->info->ReagentCount[i], true);

        caster->UpdateCraftSkill(spellId);

        // As in Spell::EffectEnchantItemPerm: take the old enchant off the
        // owner's stats, replace it, put the new one on.
        owner->ApplyEnchantment(item, spell->slot, false);
        item->SetEnchantment(spell->slot, spell->enchant->ID, 0, 0, caster->GetGUID());
        owner->ApplyEnchantment(item, spell->slot, true);

        owner->RemoveTradeableItem(item);
        item->ClearSoulboundTradeable(owner);

        SavePair(caster, owner);

        handler->SendSysMessage(Acore::StringFormat("X~{}~{}~{}~{}",
            caster->GetName(), spellId, owner->GetName(), item->GetGUID().GetCounter()));
        LOG_INFO("module", "[BotInventory] {}: {} applied spell {} to item {} of {}.",
            caller->GetName(), caster->GetName(), spellId, item->GetEntry(), owner->GetName());

        return true;
    }
};

class BotInventoryWorldScript : public WorldScript
{
public:
    BotInventoryWorldScript() : WorldScript("BotInventoryWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

void AddBotInventoryScripts()
{
    new BotInventoryWorldScript();
    new bot_inventory_commandscript();
}
