#include "PocketStorage.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <sstream>

// Returns true if the player belongs to a random playerbot account (username starts with the
// bot account prefix, "rndbot" by default). The account name lookup hits the login DB, so the
// result is cached in the player's DataMap.
static bool IsRandomBot(Player* player)
{
    if (!player)
        return false;

    PocketPlayerData* data = GetPocketData(player);
    if (data->randomBotChecked)
        return data->isRandomBot;

    data->randomBotChecked = true;
    data->isRandomBot = false;

    uint32 accountId = player->GetSession()->GetAccountId();
    std::string username;
    if (!sAccountMgr->GetName(accountId, username))
        return false;
    // Case-insensitive prefix check; the configured prefix is already lowercase
    std::string const& botPrefix = sPocketStorageMgr->GetRandomBotPrefix();
    if (botPrefix.empty() || username.size() < botPrefix.size())
        return false;
    std::string prefix = username.substr(0, botPrefix.size());
    for (char& c : prefix)
        c = std::tolower(static_cast<unsigned char>(c));
    data->isRandomBot = prefix == botPrefix;
    return data->isRandomBot;
}

// PocketStorage.RandomBotAccountPrefix wins; when it is empty, follow mod-playerbots' own
// setting (current and older option names), so realms with a custom prefix work unchanged
static std::string LoadRandomBotPrefix()
{
    std::string prefix = sConfigMgr->GetOption<std::string>("PocketStorage.RandomBotAccountPrefix", "", false);
    if (prefix.empty())
        prefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAccountPrefix", "", false);
    if (prefix.empty())
        prefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "", false);
    if (prefix.empty())
        prefix = "rndbot";

    for (char& c : prefix)
        c = std::tolower(static_cast<unsigned char>(c));
    return prefix;
}

// Build a clickable item link like "|cff1eff00|Hitem:2589:0:0:0:0:0:0:0:0:0|h[Linen Cloth]|h|r"
static std::string BuildItemLink(uint32 itemEntry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto)
        return "[Item #" + std::to_string(itemEntry) + "]";

    std::ostringstream ss;
    ss << "|c";
    ss << std::hex << ItemQualityColors[proto->Quality] << std::dec;
    ss << "|Hitem:" << itemEntry << ":0:0:0:0:0:0:0:0:0|h[";
    ss << proto->Name1 << "]|h|r";
    return ss.str();
}

// ============================================================================
// WorldScript — Config Loading, Table Migration
// ============================================================================

class PocketStorageWorldScript : public WorldScript
{
public:
    PocketStorageWorldScript() : WorldScript("PocketStorageWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sPocketStorageMgr->SetEnabled(sConfigMgr->GetOption<bool>("PocketStorage.Enable", true));
        sPocketStorageMgr->SetSharingEnabled(sConfigMgr->GetOption<bool>("PocketStorage.EnableSharing", true));
        sPocketStorageMgr->SetDepositMessageEnabled(sConfigMgr->GetOption<bool>("PocketStorage.DepositMessage", true));
        sPocketStorageMgr->SetExtraItems(sConfigMgr->GetOption<std::string>("PocketStorage.ExtraItems", "4278"));
        sPocketStorageMgr->SetRandomBotPrefix(LoadRandomBotPrefix());
        LOG_INFO("module", "PocketStorage: ignoring bot accounts starting with '{}'.", sPocketStorageMgr->GetRandomBotPrefix());
    }

    // Runs once the databases are open and before anyone can log in
    void OnStartup() override
    {
        sPocketStorageMgr->MigrateLegacyTables();
    }
};

// ============================================================================
// PlayerScript — Login/Logout, Item Acquisition
// ============================================================================

class PocketStoragePlayerScript : public PlayerScript
{
public:
    PocketStoragePlayerScript() : PlayerScript("PocketStoragePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_STORE_NEW_ITEM,
        PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE,
        PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!sPocketStorageMgr->IsEnabled())
            return;

        if (IsRandomBot(player))
            return;

        sPocketStorageMgr->LoadAccountData(player);
        sPocketStorageMgr->SendFullSync(player);
        sPocketStorageMgr->SendAccountSettings(player);

        if (sPocketStorageMgr->IsSharingEnabled())
        {
            QueryResult invite = CharacterDatabase.Query("SELECT inviter_name FROM pocket_storage_invite WHERE invitee_account_id = {}",
                player->GetSession()->GetAccountId());
            if (invite)
                ChatHandler(player->GetSession()).PSendSysMessage("Pocket Storage: {} has invited you to share their vault. Type .ps share accept or .ps share decline.",
                    invite->Fetch()[0].Get<std::string>());
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!sPocketStorageMgr->IsEnabled())
            return;

        if (IsRandomBot(player))
            return;

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);
        PocketPlayerData* data = GetPocketData(player);

        // Re-vault any materialized items still in inventory
        if (data && !data->materializedItems.empty())
        {
            for (uint32 guid : data->materializedItems)
            {
                Item* item = player->GetItemByGuid(ObjectGuid(HighGuid::Item, guid));
                if (item)
                {
                    uint32 entry = item->GetEntry();
                    uint32 count = item->GetCount();
                    player->DestroyItemCount(entry, count, true);
                    sPocketStorageMgr->DepositItem(storageId, entry, count);
                    sPocketStorageMgr->BroadcastItemCount(storageId, entry);
                }
            }
            data->materializedItems.clear();
        }

        // Only unloads once no other player sharing this vault is online
        sPocketStorageMgr->UnloadAccountData(player);
    }

    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
    {
        if (!sPocketStorageMgr->IsEnabled() || !player || !item)
            return;

        if (IsRandomBot(player))
            return;

        PocketPlayerData* data = GetPocketData(player);
        if (!data || !data->autoStoreEnabled)
            return;

        // Switched off for the account from the addon's rules window
        if (!sPocketStorageMgr->IsAutoStoreOnLoot(player->GetSession()->GetAccountId()))
            return;

        // Don't auto-store items being materialized from vault
        if (data->isMaterializing)
            return;

        // Don't auto-store materialized items
        if (data->materializedItems.count(item->GetGUID().GetCounter()))
            return;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!sPocketStorageMgr->ShouldAutoStore(player, itemTemplate))
            return;

        // Defer the deposit — destroying items inside this hook crashes the server
        // Use count (newly added) not item->GetCount() (merged stack size)
        data->pendingDeposits.push_back({ item->GetEntry(), count });
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (!sPocketStorageMgr->IsEnabled() || !player)
            return;

        if (IsRandomBot(player))
            return;

        PocketPlayerData* data = GetPocketData(player);
        if (!data || data->pendingDeposits.empty())
            return;

        // Move pending list out so we don't re-enter if DestroyItemCount triggers hooks
        std::vector<PendingDeposit> deposits = std::move(data->pendingDeposits);
        data->pendingDeposits.clear();

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);

        for (auto const& dep : deposits)
        {
            // Verify the player still has the items (they may have been used/moved)
            uint32 playerHas = player->GetItemCount(dep.itemEntry);
            uint32 toDeposit = std::min(dep.count, playerHas);
            if (toDeposit == 0)
                continue;

            // Never deposit below the quest-required threshold — guards against
            // timing races where item->GetCount() or quest status was stale when queued
            uint32 questReserved = sPocketStorageMgr->GetQuestReservedCount(player, dep.itemEntry);
            if (playerHas <= questReserved)
                continue;
            toDeposit = std::min(toDeposit, playerHas - questReserved);

            player->DestroyItemCount(dep.itemEntry, toDeposit, true);
            sPocketStorageMgr->DepositItem(storageId, dep.itemEntry, toDeposit);
            sPocketStorageMgr->BroadcastItemCount(storageId, dep.itemEntry);

            if (sPocketStorageMgr->IsDepositMessageEnabled())
                ChatHandler(player->GetSession()).PSendSysMessage("Pocket Storage: Stored {} x{} ({} in storage).",
                    BuildItemLink(dep.itemEntry), toDeposit, sPocketStorageMgr->GetItemCount(storageId, dep.itemEntry));
        }
    }

    bool OnPlayerBeforeQuestComplete(Player* player, uint32 questId) override
    {
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            MaterializeQuestItems(player, quest, true);

        return true;
    }

    // Pull required items out of the vault as soon as the quest is taken, so
    // items stored before the quest existed (e.g. Murloc Fins) count right away
    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (quest)
            MaterializeQuestItems(player, quest, false);
    }

private:
    // trackAsMaterialized: put leftovers back in the vault later (crafting/logout).
    // Off for quest accept, where the items must stay in bags until turn-in.
    static void MaterializeQuestItems(Player* player, Quest const* quest, bool trackAsMaterialized)
    {
        if (!sPocketStorageMgr->IsEnabled() || !player)
            return;

        if (IsRandomBot(player))
            return;

        PocketPlayerData* data = GetPocketData(player);

        // Prevent infinite recursion: StoreNewItem -> ItemAddedQuestCheck ->
        // CompleteQuest -> OnPlayerBeforeQuestComplete -> StoreNewItem ...
        if (data && data->isMaterializing)
            return;

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);

        if (data)
            data->isMaterializing = true;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 reqItem = quest->RequiredItemId[i];
            uint32 reqCount = quest->RequiredItemCount[i];
            if (!reqItem || !reqCount)
                continue;

            uint32 playerCount = player->GetItemCount(reqItem);
            if (playerCount >= reqCount)
                continue;

            uint32 deficit = reqCount - playerCount;
            uint32 vaultCount = sPocketStorageMgr->GetItemCount(storageId, reqItem);
            if (vaultCount == 0)
                continue;

            uint32 toMaterialize = std::min(deficit, vaultCount);

            ItemPosCountVec dest;
            InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reqItem, toMaterialize);
            if (result != EQUIP_ERR_OK)
            {
                ChatHandler(player->GetSession()).SendSysMessage("Pocket Storage: Not enough bag space to materialize quest items.");
                continue;
            }

            // Withdraw before creating the item: with a shared vault another
            // character may have taken the items since we checked the count
            if (!sPocketStorageMgr->WithdrawItem(storageId, reqItem, toMaterialize))
                continue;

            Item* newItem = player->StoreNewItem(dest, reqItem, true);
            if (!newItem)
                sPocketStorageMgr->DepositItem(storageId, reqItem, toMaterialize);
            else if (data && trackAsMaterialized)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sPocketStorageMgr->BroadcastItemCount(storageId, reqItem);
        }

        if (data)
            data->isMaterializing = false;
    }
};

// ============================================================================
// AllSpellScript — Crafting Materialization
// ============================================================================

class PocketStorageSpellScript : public AllSpellScript
{
public:
    PocketStorageSpellScript() : AllSpellScript("PocketStorageSpellScript", {
        ALLSPELLHOOK_ON_SPELL_CHECK_CAST,
        ALLSPELLHOOK_ON_CAST
    }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!sPocketStorageMgr->IsEnabled())
            return;

        if (res != SPELL_CAST_OK)
            return;

        WorldObject* caster = spell->GetCaster();
        if (!caster || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();

        if (IsRandomBot(player))
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();

        // Check if this spell uses reagents
        bool hasReagents = false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spellInfo->Reagent[i] > 0 && spellInfo->ReagentCount[i] > 0)
            {
                hasReagents = true;
                break;
            }
        }

        if (!hasReagents)
            return;

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);
        PocketPlayerData* data = GetPocketData(player);
        if (!data)
            return;

        // First pass: verify vault can cover all deficits before materializing anything
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 reagentEntry = spellInfo->Reagent[i];
            uint32 reagentCount = spellInfo->ReagentCount[i];
            if (reagentEntry <= 0 || reagentCount == 0)
                continue;

            uint32 playerHas = player->GetItemCount(reagentEntry);
            if (playerHas >= reagentCount)
                continue;

            uint32 deficit = reagentCount - playerHas;
            uint32 vaultCount = sPocketStorageMgr->GetItemCount(storageId, reagentEntry);

            if (vaultCount < deficit)
                return; // not enough even with vault
        }

        // Second pass: materialize deficits
        data->isMaterializing = true;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 reagentEntry = spellInfo->Reagent[i];
            uint32 reagentCount = spellInfo->ReagentCount[i];
            if (reagentEntry <= 0 || reagentCount == 0)
                continue;

            uint32 playerHas = player->GetItemCount(reagentEntry);
            if (playerHas >= reagentCount)
                continue;

            uint32 deficit = reagentCount - playerHas;

            ItemPosCountVec dest;
            InventoryResult invResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reagentEntry, deficit);
            if (invResult != EQUIP_ERR_OK)
            {
                data->isMaterializing = false;
                res = SPELL_FAILED_DONT_REPORT;
                ChatHandler(player->GetSession()).SendSysMessage("Pocket Storage: Not enough bag space to materialize crafting reagents.");
                return;
            }

            // Withdraw first; if another character emptied the shared vault meanwhile,
            // skip it and let the normal reagent check fail the cast
            if (!sPocketStorageMgr->WithdrawItem(storageId, reagentEntry, deficit))
                continue;

            Item* newItem = player->StoreNewItem(dest, reagentEntry, true);
            if (!newItem)
                sPocketStorageMgr->DepositItem(storageId, reagentEntry, deficit);
            else
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sPocketStorageMgr->BroadcastItemCount(storageId, reagentEntry);
        }
        data->isMaterializing = false;
    }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!sPocketStorageMgr->IsEnabled())
            return;

        if (!caster || !caster->IsPlayer())
            return;

        Player* player = caster->ToPlayer();

        if (IsRandomBot(player))
            return;

        PocketPlayerData* data = GetPocketData(player);
        if (!data || data->materializedItems.empty())
            return;

        // Multi-craft: if more crafts remain, queue the next one instead of re-vaulting
        if (data->pendingCrafts > 0 && data->pendingSpellId == spellInfo->Id)
        {
            data->pendingCrafts--;
            if (data->pendingCrafts > 0)
            {
                player->CastSpell(player, data->pendingSpellId, false);
                return; // don't re-vault yet
            }
            // Last craft done — fall through to re-vault leftovers
        }

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);

        // Re-vault any materialized items that are still in inventory (leftovers)
        data->isMaterializing = true;
        for (uint32 guid : data->materializedItems)
        {
            Item* item = player->GetItemByGuid(ObjectGuid(HighGuid::Item, guid));
            if (item)
            {
                uint32 entry = item->GetEntry();
                uint32 count = item->GetCount();
                player->DestroyItemCount(entry, count, true);
                sPocketStorageMgr->DepositItem(storageId, entry, count);
                sPocketStorageMgr->BroadcastItemCount(storageId, entry);
            }
        }
        data->materializedItems.clear();
        data->isMaterializing = false;
        data->pendingCrafts = 0;
        data->pendingSpellId = 0;
        data->autoStoreEnabled = true;
    }
};

// ============================================================================
// CommandScript — Player Commands
// ============================================================================

using namespace Acore::ChatCommands;

class PocketStorageCommandScript : public CommandScript
{
public:
    PocketStorageCommandScript() : CommandScript("PocketStorageCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable shareCommandTable =
        {
            { "list",    HandleShareListCommand,    SEC_PLAYER, Console::No },
            { "invite",  HandleShareInviteCommand,  SEC_PLAYER, Console::No },
            { "accept",  HandleShareAcceptCommand,  SEC_PLAYER, Console::No },
            { "decline", HandleShareDeclineCommand, SEC_PLAYER, Console::No },
            { "leave",   HandleShareLeaveCommand,   SEC_PLAYER, Console::No },
            { "kick",    HandleShareKickCommand,    SEC_PLAYER, Console::No },
        };
        static ChatCommandTable ruleCommandTable =
        {
            { "store",  HandleRuleStoreCommand,  SEC_PLAYER, Console::No },
            { "keep",   HandleRuleKeepCommand,   SEC_PLAYER, Console::No },
            { "remove", HandleRuleRemoveCommand, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable pocketCommandTable =
        {
            { "share",    shareCommandTable },
            { "rule",     ruleCommandTable },
            { "autostore", HandleAutoStoreCommand, SEC_PLAYER, Console::No },
            { "withdraw", HandleWithdrawCommand,  SEC_PLAYER, Console::No },
            { "deposit",  HandleDepositCommand,   SEC_PLAYER, Console::No },
            { "sync",     HandleSyncCommand,      SEC_PLAYER, Console::No },
            { "craft",    HandleCraftCommand,      SEC_PLAYER, Console::No },
            { "combine",  HandleCombineCommand,    SEC_PLAYER, Console::No },
            { "link",     HandleLinkCommand,       SEC_ADMINISTRATOR, Console::Yes },
            { "unlink",   HandleUnlinkCommand,     SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "ps",     pocketCommandTable },
            { "pocket", pocketCommandTable },
        };
        return commandTable;
    }

    static bool HandleWithdrawCommand(ChatHandler* handler, uint32 itemEntry, Optional<uint32> optCount)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);
        uint32 vaultCount = sPocketStorageMgr->GetItemCount(storageId, itemEntry);

        if (vaultCount == 0)
        {
            handler->SendSysMessage("Pocket Storage: Item not found in vault.");
            return true;
        }

        uint32 count = optCount.value_or(vaultCount);
        if (count > vaultCount)
            count = vaultCount;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemEntry);
        if (!itemTemplate)
        {
            handler->SendSysMessage("Pocket Storage: Invalid item.");
            return true;
        }

        // Disable auto-store BEFORE creating items, otherwise OnPlayerStoreNewItem
        // will immediately re-deposit them back into the vault
        PocketPlayerData* data = GetPocketData(player);
        if (data)
            data->autoStoreEnabled = false;

        // Add items to player in stacks respecting max stack size
        uint32 remaining = count;
        while (remaining > 0)
        {
            uint32 stackSize = std::min(remaining, itemTemplate->GetMaxStackSize());

            ItemPosCountVec dest;
            InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, stackSize);
            if (result != EQUIP_ERR_OK)
            {
                handler->SendSysMessage("Pocket Storage: Not enough bag space.");
                break;
            }

            // Withdraw before creating: another character sharing the vault may have taken some
            if (!sPocketStorageMgr->WithdrawItem(storageId, itemEntry, stackSize))
                break;

            if (!player->StoreNewItem(dest, itemEntry, true))
            {
                sPocketStorageMgr->DepositItem(storageId, itemEntry, stackSize);
                break;
            }
            remaining -= stackSize;
        }

        uint32 withdrawn = count - remaining;
        if (withdrawn > 0)
        {
            sPocketStorageMgr->BroadcastItemCount(storageId, itemEntry);

            handler->PSendSysMessage("Pocket Storage: Withdrew {} x{}.", BuildItemLink(itemEntry), withdrawn);
        }

        return true;
    }

    static bool HandleDepositCommand(ChatHandler* handler)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);

        // Collect totals first — DestroyItemCount searches the whole inventory,
        // so destroying during iteration can skip stacks of the same item
        std::unordered_map<uint32, uint32> toDeposit; // itemEntry -> totalCount

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            if (Bag* pBag = player->GetBagByPos(bag))
            {
                for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                {
                    Item* item = pBag->GetItemByPos(slot);
                    if (!item)
                        continue;

                    if (sPocketStorageMgr->ShouldAutoStore(player, item->GetTemplate()))
                        toDeposit[item->GetEntry()] += item->GetCount();
                }
            }
        }

        // Also scan the default backpack (slots 23-38)
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (sPocketStorageMgr->ShouldAutoStore(player, item->GetTemplate()))
                toDeposit[item->GetEntry()] += item->GetCount();
        }

        // Now destroy and deposit in one pass per item entry
        uint32 depositedCount = 0;
        for (auto const& [entry, count] : toDeposit)
        {
            player->DestroyItemCount(entry, count, true);
            sPocketStorageMgr->DepositItem(storageId, entry, count);
            sPocketStorageMgr->BroadcastItemCount(storageId, entry);
            ++depositedCount;
        }

        PocketPlayerData* data = GetPocketData(player);
        if (data)
            data->autoStoreEnabled = true;

        handler->PSendSysMessage("Pocket Storage: Deposited {} item stacks.", depositedCount);

        return true;
    }

    // Item name without a trailing rank numeral: "Scroll of Spirit IV" -> "Scroll of Spirit"
    static std::string StripRank(std::string const& name)
    {
        std::size_t space = name.rfind(' ');
        if (space == std::string::npos || space + 1 == name.size() || name.size() - space - 1 > 5)
            return name;

        for (std::size_t i = space + 1; i < name.size(); ++i)
            if (name[i] != 'I' && name[i] != 'V' && name[i] != 'X' && name[i] != 'L')
                return name;

        return name.substr(0, space);
    }

    // Every rank of an item: same class and subclass, and a name that is the base name
    // with or without a rank numeral. An item without a numeral has only itself.
    static std::vector<ItemTemplate const*> FindItemRanks(ItemTemplate const* item)
    {
        std::string baseName = StripRank(item->Name1);
        if (baseName == item->Name1)
            return { item };

        std::vector<ItemTemplate const*> ranks;
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.Class != item->Class || proto.SubClass != item->SubClass)
                continue;
            if (proto.Name1 == baseName || StripRank(proto.Name1) == baseName)
                ranks.push_back(&proto);
        }
        return ranks;
    }

    // Items that belong together with an item: the parts of what it combines into
    // (the eight Darkmoon cards of a deck), or else its ranks
    struct ItemSeries
    {
        std::vector<ItemTemplate const*> items;
        std::string description; // "8 items of Warlords Deck" or "8 ranks of Scroll of Spirit"
        char kind;               // 'C' combine set, 'R' ranks
    };

    static ItemSeries FindItemSeries(ItemTemplate const* item)
    {
        ItemSeries series;
        if (PocketCombineRecipe const* recipe = sPocketStorageMgr->GetCombineRecipe(item->ItemId))
        {
            for (auto const& [entry, count] : recipe->parts)
                if (ItemTemplate const* part = sObjectMgr->GetItemTemplate(entry))
                    series.items.push_back(part);

            ItemTemplate const* result = sObjectMgr->GetItemTemplate(recipe->resultItem);
            series.description = std::to_string(series.items.size()) + " items of " + (result ? result->Name1 : "the set");
            series.kind = 'C';
            return series;
        }

        series.items = FindItemRanks(item);
        series.description = std::to_string(series.items.size()) + " ranks of " + StripRank(item->Name1);
        series.kind = 'R';
        return series;
    }

    // .ps rule store|keep|remove <itemId> [all|ask] — the addon's Store/Keep lists, saved per account.
    // "all" gives the rule to every item of the series (Scroll of Spirit I-VIII, the cards of a
    // Darkmoon deck). "ask" is the addon's: for an item with a series it replies SERIES:<list>:
    // <item>:<C|R>:<count>:<description> so the addon can ask, and otherwise adds just the item.
    static bool SetItemRule(ChatHandler* handler, uint32 itemEntry, PocketItemRule rule, Optional<std::string> const& scope)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemEntry);
        if (!itemTemplate)
        {
            handler->SendSysMessage("Pocket Storage: Invalid item.");
            return true;
        }

        std::string word = scope.value_or("");
        for (char& c : word)
            c = std::tolower(static_cast<unsigned char>(c));
        if (!word.empty() && word != "all" && word != "ask")
        {
            handler->SendSysMessage("Pocket Storage: Add \"all\" after the item id to include every item of its set or every rank, or nothing for just this item.");
            return true;
        }

        std::vector<ItemTemplate const*> items = { itemTemplate };
        std::string seriesDescription;
        if (!word.empty())
        {
            ItemSeries series = FindItemSeries(itemTemplate);
            if (series.items.size() > 1)
            {
                if (word == "ask")
                {
                    char list = rule == PocketItemRule::Store ? 'S' : rule == PocketItemRule::Keep ? 'K' : 'N';
                    sPocketStorageMgr->SendAddonMessage(player, Acore::StringFormat("SERIES:{}:{}:{}:{}:{}",
                        list, itemEntry, series.kind, series.items.size(), series.description));
                    return true;
                }
                items = std::move(series.items);
                seriesDescription = std::move(series.description);
            }
        }

        // The vault keeps only entry and count, so unstackable items cannot be stored
        std::vector<uint32> entries;
        for (ItemTemplate const* item : items)
            if (rule != PocketItemRule::Store || item->GetMaxStackSize() > 1)
                entries.push_back(item->ItemId);

        if (entries.empty())
        {
            handler->PSendSysMessage("Pocket Storage: {} does not stack, so it cannot be stored.", BuildItemLink(itemEntry));
            return true;
        }

        uint32 accountId = player->GetSession()->GetAccountId();
        if (rule != PocketItemRule::None)
        {
            std::size_t added = 0;
            for (uint32 entry : entries)
                if (sPocketStorageMgr->GetItemRule(accountId, entry) == PocketItemRule::None)
                    ++added;

            if (sPocketStorageMgr->GetItemRuleCount(accountId) + added > POCKET_MAX_ITEM_RULES)
            {
                handler->PSendSysMessage("Pocket Storage: Your Store and Keep lists are full ({} items).", POCKET_MAX_ITEM_RULES);
                return true;
            }
        }

        for (uint32 entry : entries)
            sPocketStorageMgr->SetItemRule(accountId, entry, rule);

        // Unstackable items of a series are left out of the Store list, so count what was added
        std::string what = entries.size() == 1 ? BuildItemLink(entries.front())
            : entries.size() == items.size() ? seriesDescription
            : std::to_string(entries.size()) + " of the " + seriesDescription + " (the others do not stack)";

        if (rule == PocketItemRule::Store)
            handler->PSendSysMessage("Pocket Storage: {} {} now always stored.", what, entries.size() == 1 ? "is" : "are");
        else if (rule == PocketItemRule::Keep)
            handler->PSendSysMessage("Pocket Storage: {} {} now kept in your bags.", what, entries.size() == 1 ? "is" : "are");
        else
            handler->PSendSysMessage("Pocket Storage: Removed {} from your lists.", what);
        return true;
    }

    static bool HandleRuleStoreCommand(ChatHandler* handler, uint32 itemEntry, Optional<std::string> scope)
    {
        return SetItemRule(handler, itemEntry, PocketItemRule::Store, scope);
    }

    static bool HandleRuleKeepCommand(ChatHandler* handler, uint32 itemEntry, Optional<std::string> scope)
    {
        return SetItemRule(handler, itemEntry, PocketItemRule::Keep, scope);
    }

    static bool HandleRuleRemoveCommand(ChatHandler* handler, uint32 itemEntry, Optional<std::string> scope)
    {
        return SetItemRule(handler, itemEntry, PocketItemRule::None, scope);
    }

    // .ps autostore on|off
    static bool HandleAutoStoreCommand(ChatHandler* handler, std::string state)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        for (char& c : state)
            c = std::tolower(static_cast<unsigned char>(c));

        bool enable;
        if (state == "on" || state == "1")
            enable = true;
        else if (state == "off" || state == "0")
            enable = false;
        else
        {
            handler->SendSysMessage("Pocket Storage: Use .ps autostore on or .ps autostore off.");
            return true;
        }

        sPocketStorageMgr->SetAutoStoreOnLoot(player->GetSession()->GetAccountId(), enable);
        handler->PSendSysMessage("Pocket Storage: Auto-store on loot is {} for this account.", enable ? "on" : "off");
        return true;
    }

    static bool HandleSyncCommand(ChatHandler* handler)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        sPocketStorageMgr->SendFullSync(player);
        sPocketStorageMgr->SendAccountSettings(player);
        handler->SendSysMessage("Pocket Storage: Sync complete.");
        return true;
    }

    static uint32 GetAccountIdByName(std::string name)
    {
        if (!Utf8ToUpperOnlyLatin(name))
            return 0;
        return AccountMgr::GetId(name);
    }

    // .ps link <account> <mainAccount>
    // Makes <account> share the vault of <mainAccount>. If <account> owns a vault
    // (including one shared by other accounts), its items and members move along.
    static bool HandleLinkCommand(ChatHandler* handler, std::string accountName, std::string mainAccountName)
    {
        uint32 accountId = GetAccountIdByName(accountName);
        uint32 mainAccountId = GetAccountIdByName(mainAccountName);
        if (!accountId || !mainAccountId)
        {
            handler->SendSysMessage("Pocket Storage: Account not found.");
            return true;
        }

        uint32 currentStorage = sPocketStorageMgr->GetStorageIdForAccount(accountId);
        uint32 newStorage = sPocketStorageMgr->GetStorageIdForAccount(mainAccountId);
        if (currentStorage == newStorage)
        {
            handler->SendSysMessage("Pocket Storage: Those accounts already share a vault.");
            return true;
        }

        // The in-memory vault of online players would go stale, so require everyone to be offline
        if (sPocketStorageMgr->IsAccountLoaded(currentStorage) || sPocketStorageMgr->IsAccountLoaded(newStorage))
        {
            handler->SendSysMessage("Pocket Storage: Log out all characters using either vault first.");
            return true;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (currentStorage == accountId)
        {
            // The account owns its vault: merge the items and move any accounts linked to it
            trans->Append("INSERT INTO pocket_storage (account_id, item_entry, count) "
                "SELECT {}, src.item_entry, src.count FROM (SELECT item_entry, count FROM pocket_storage WHERE account_id = {}) AS src "
                "ON DUPLICATE KEY UPDATE count = pocket_storage.count + src.count", newStorage, accountId);
            trans->Append("DELETE FROM pocket_storage WHERE account_id = {}", accountId);
            trans->Append("UPDATE pocket_storage_link SET owner_account_id = {} WHERE owner_account_id = {}", newStorage, accountId);
        }
        // Otherwise the account was linked elsewhere; the shared items stay with that vault
        trans->Append("REPLACE INTO pocket_storage_link (account_id, owner_account_id) VALUES ({}, {})", accountId, newStorage);
        CharacterDatabase.DirectCommitTransaction(trans);
        sPocketStorageMgr->ForgetAccountStorage();

        handler->PSendSysMessage("Pocket Storage: Account {} now shares the vault of account {}.", accountName, mainAccountName);
        return true;
    }

    // .ps unlink <account>
    // Gives <account> its own, empty vault again. The shared items stay in the shared vault.
    static bool HandleUnlinkCommand(ChatHandler* handler, std::string accountName)
    {
        uint32 accountId = GetAccountIdByName(accountName);
        if (!accountId)
        {
            handler->SendSysMessage("Pocket Storage: Account not found.");
            return true;
        }

        uint32 currentStorage = sPocketStorageMgr->GetStorageIdForAccount(accountId);
        if (currentStorage == accountId)
        {
            handler->SendSysMessage("Pocket Storage: That account is not linked to another vault.");
            return true;
        }

        if (sPocketStorageMgr->IsAccountLoaded(currentStorage))
        {
            handler->SendSysMessage("Pocket Storage: Log out all characters using that vault first.");
            return true;
        }

        CharacterDatabase.DirectExecute("DELETE FROM pocket_storage_link WHERE account_id = {}", accountId);
        sPocketStorageMgr->ForgetAccountStorage();
        handler->PSendSysMessage("Pocket Storage: Account {} has its own vault again.", accountName);
        return true;
    }

    // Players only ever see character names, never account names
    static std::string GetMainCharacterName(uint32 accountId)
    {
        QueryResult result = CharacterDatabase.Query("SELECT name FROM characters WHERE account = {} ORDER BY totaltime DESC LIMIT 1", accountId);
        if (!result)
            return "#" + std::to_string(accountId);
        return result->Fetch()[0].Get<std::string>();
    }

    static uint32 GetAccountIdByCharacterName(std::string name)
    {
        if (!normalizePlayerName(name))
            return 0;
        return sCharacterCache->GetCharacterAccountIdByName(name);
    }

    static bool HasSharedMembers(uint32 accountId)
    {
        return bool(CharacterDatabase.Query("SELECT 1 FROM pocket_storage_link WHERE owner_account_id = {} LIMIT 1", accountId));
    }

    // Common checks for the .ps share commands; returns the player or nullptr
    static Player* GetSharingPlayer(ChatHandler* handler)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return nullptr;

        if (!sPocketStorageMgr->IsSharingEnabled())
        {
            handler->SendSysMessage("Pocket Storage: Vault sharing is disabled on this server.");
            return nullptr;
        }

        return handler->GetSession()->GetPlayer();
    }

    // .ps share list
    static bool HandleShareListCommand(ChatHandler* handler)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 owner = sPocketStorageMgr->GetStorageIdForAccount(accountId);
        if (owner != accountId)
        {
            handler->PSendSysMessage("Pocket Storage: You share the vault of {}.", GetMainCharacterName(owner));
            return true;
        }

        QueryResult result = CharacterDatabase.Query("SELECT account_id FROM pocket_storage_link WHERE owner_account_id = {}", accountId);
        if (!result)
        {
            handler->SendSysMessage("Pocket Storage: Your vault is not shared. Use .ps share invite <character> to share it.");
            return true;
        }

        handler->SendSysMessage("Pocket Storage: Your vault is shared with:");
        do
        {
            handler->PSendSysMessage("  {}", GetMainCharacterName(result->Fetch()[0].Get<uint32>()));
        } while (result->NextRow());
        return true;
    }

    // .ps share invite <character>
    // Invites the account of <character> to use your vault
    static bool HandleShareInviteCommand(ChatHandler* handler, std::string characterName)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        if (sPocketStorageMgr->GetStorageIdForAccount(accountId) != accountId)
        {
            handler->SendSysMessage("Pocket Storage: You use someone else's vault. Leave it with .ps share leave before inviting others.");
            return true;
        }

        uint32 inviteeId = GetAccountIdByCharacterName(characterName);
        if (!inviteeId)
        {
            handler->SendSysMessage("Pocket Storage: Character not found.");
            return true;
        }

        if (inviteeId == accountId)
        {
            handler->SendSysMessage("Pocket Storage: That character is on your own account and already uses your vault.");
            return true;
        }

        if (sPocketStorageMgr->GetStorageIdForAccount(inviteeId) != inviteeId || HasSharedMembers(inviteeId))
        {
            handler->SendSysMessage("Pocket Storage: That character already shares a vault with someone.");
            return true;
        }

        CharacterDatabase.DirectExecute("REPLACE INTO pocket_storage_invite (invitee_account_id, inviter_account_id, inviter_name) VALUES ({}, {}, '{}')",
            inviteeId, accountId, player->GetName());

        normalizePlayerName(characterName);
        if (Player* invitee = ObjectAccessor::FindPlayerByName(characterName))
            ChatHandler(invitee->GetSession()).PSendSysMessage("Pocket Storage: {} has invited you to share their vault. Type .ps share accept or .ps share decline.",
                player->GetName());

        handler->PSendSysMessage("Pocket Storage: Invited {}. They can accept with .ps share accept.", characterName);
        return true;
    }

    // .ps share accept
    // Joins the inviter's vault. Your current vault contents are merged into it.
    static bool HandleShareAcceptCommand(ChatHandler* handler)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        QueryResult invite = CharacterDatabase.Query("SELECT inviter_account_id, inviter_name FROM pocket_storage_invite WHERE invitee_account_id = {}", accountId);
        if (!invite)
        {
            handler->SendSysMessage("Pocket Storage: You have no pending invite.");
            return true;
        }

        uint32 ownerId = invite->Fetch()[0].Get<uint32>();
        std::string ownerName = invite->Fetch()[1].Get<std::string>();

        // Merging is only safe while this player is the vault's only user
        if (sPocketStorageMgr->GetStorageIdForAccount(accountId) != accountId || HasSharedMembers(accountId))
        {
            handler->SendSysMessage("Pocket Storage: You already share a vault. Leave it with .ps share leave first.");
            return true;
        }

        if (sPocketStorageMgr->GetStorageIdForAccount(ownerId) != ownerId)
        {
            CharacterDatabase.DirectExecute("DELETE FROM pocket_storage_invite WHERE invitee_account_id = {}", accountId);
            handler->SendSysMessage("Pocket Storage: The inviter no longer owns a vault. The invite was removed.");
            return true;
        }

        // Queued (not direct) so it runs after any deposits this player still has queued
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("INSERT INTO pocket_storage (account_id, item_entry, count) "
            "SELECT {}, src.item_entry, src.count FROM (SELECT item_entry, count FROM pocket_storage WHERE account_id = {}) AS src "
            "ON DUPLICATE KEY UPDATE count = pocket_storage.count + src.count", ownerId, accountId);
        trans->Append("DELETE FROM pocket_storage WHERE account_id = {}", accountId);
        trans->Append("REPLACE INTO pocket_storage_link (account_id, owner_account_id) VALUES ({}, {})", accountId, ownerId);
        trans->Append("DELETE FROM pocket_storage_invite WHERE invitee_account_id = {}", accountId);
        CharacterDatabase.CommitTransaction(trans);

        sPocketStorageMgr->SwitchAccountStorage(accountId, ownerId, true);

        handler->PSendSysMessage("Pocket Storage: You now share the vault of {}. Your items were added to it.", ownerName);
        if (Player* owner = ObjectAccessor::FindPlayerByName(ownerName))
            ChatHandler(owner->GetSession()).PSendSysMessage("Pocket Storage: {} now shares your vault.", player->GetName());
        return true;
    }

    // .ps share decline
    static bool HandleShareDeclineCommand(ChatHandler* handler)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        if (!CharacterDatabase.Query("SELECT 1 FROM pocket_storage_invite WHERE invitee_account_id = {}", accountId))
        {
            handler->SendSysMessage("Pocket Storage: You have no pending invite.");
            return true;
        }

        CharacterDatabase.DirectExecute("DELETE FROM pocket_storage_invite WHERE invitee_account_id = {}", accountId);
        handler->SendSysMessage("Pocket Storage: Invite declined.");
        return true;
    }

    // .ps share leave
    // Stops using the shared vault. The shared items stay in it; you start with an empty vault.
    static bool HandleShareLeaveCommand(ChatHandler* handler)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        if (sPocketStorageMgr->GetStorageIdForAccount(accountId) == accountId)
        {
            handler->SendSysMessage("Pocket Storage: You are not using someone else's vault.");
            return true;
        }

        CharacterDatabase.DirectExecute("DELETE FROM pocket_storage_link WHERE account_id = {}", accountId);
        sPocketStorageMgr->SwitchAccountStorage(accountId, accountId, false);

        handler->SendSysMessage("Pocket Storage: You left the shared vault. The shared items stay with its owner.");
        return true;
    }

    // .ps share kick <character>
    // Removes the account of <character> from your vault
    static bool HandleShareKickCommand(ChatHandler* handler, std::string characterName)
    {
        Player* player = GetSharingPlayer(handler);
        if (!player)
            return true;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 memberId = GetAccountIdByCharacterName(characterName);
        if (!memberId || memberId == accountId || sPocketStorageMgr->GetStorageIdForAccount(memberId) != accountId
            || sPocketStorageMgr->GetStorageIdForAccount(accountId) != accountId)
        {
            handler->SendSysMessage("Pocket Storage: That character does not share your vault.");
            return true;
        }

        CharacterDatabase.DirectExecute("DELETE FROM pocket_storage_link WHERE account_id = {} AND owner_account_id = {}", memberId, accountId);
        // Moves their online characters to their own (empty) vault right away
        sPocketStorageMgr->SwitchAccountStorage(memberId, memberId, false);

        handler->PSendSysMessage("Pocket Storage: Removed {} from your vault.", characterName);
        return true;
    }

    // .ps craft <spellId> [count]
    // Materializes reagents from vault and casts the crafting spell
    static bool HandleCraftCommand(ChatHandler* handler, uint32 spellId, Optional<uint32> optCount)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        // Only recipes the player knows; casting any other spell with reagents would skip its
        // requirements (a Darkmoon card spell, for one, would create the deck without using the card)
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || !player->HasSpell(spellId))
        {
            handler->SendSysMessage("Pocket Storage: Invalid spell.");
            return true;
        }

        // Verify spell has reagents
        bool hasReagents = false;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spellInfo->Reagent[i] > 0 && spellInfo->ReagentCount[i] > 0)
            {
                hasReagents = true;
                break;
            }
        }

        if (!hasReagents)
        {
            handler->SendSysMessage("Pocket Storage: Spell has no reagents.");
            return true;
        }

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);
        uint32 craftCount = optCount.value_or(1);
        if (craftCount == 0)
            craftCount = 1;

        // Gather reagent info: what the player has, what the vault has, per-craft need
        struct ReagentInfo
        {
            int32  entry;
            uint32 perCraft;
            uint32 playerHas;
            uint32 vaultHas;
            uint32 maxStack;
        };
        std::vector<ReagentInfo> reagents;

        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            int32 reagentEntry = spellInfo->Reagent[i];
            uint32 reagentCount = spellInfo->ReagentCount[i];
            if (reagentEntry <= 0 || reagentCount == 0)
                continue;

            uint32 playerHas = player->GetItemCount(reagentEntry);
            uint32 vaultHas = sPocketStorageMgr->GetItemCount(storageId, reagentEntry);

            if (playerHas + vaultHas < reagentCount)
            {
                handler->SendSysMessage("Pocket Storage: Not enough reagents.");
                return true;
            }

            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(reagentEntry);
            uint32 maxStack = tmpl ? tmpl->GetMaxStackSize() : 1;

            reagents.push_back({ reagentEntry, reagentCount, playerHas, vaultHas, maxStack });
        }

        // Count free bag slots
        uint32 freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                freeSlots++;
        }
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            if (Bag* pBag = player->GetBagByPos(bag))
            {
                for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
                {
                    if (!pBag->GetItemByPos(slot))
                        freeSlots++;
                }
            }
        }

        // Count how many distinct reagents need vault withdrawal
        uint32 vaultReagentSlots = 0;
        for (auto const& r : reagents)
        {
            if (r.playerHas < r.perCraft)
                vaultReagentSlots++;
        }

        // Need: 1 slot per vault reagent type + 1 for the crafted product
        if (freeSlots < vaultReagentSlots + 1)
        {
            handler->SendSysMessage("Pocket Storage: Not enough bag space (need room for reagents + product).");
            return true;
        }

        // Cap craft count by total available reagents (inventory + vault)
        uint32 maxCrafts = craftCount;
        for (auto const& r : reagents)
        {
            uint32 possible = (r.playerHas + r.vaultHas) / r.perCraft;
            maxCrafts = std::min(maxCrafts, possible);
        }

        // Cap further: each vault reagent gets at most 1 max-stack of bag space,
        // so the amount we can withdraw is limited
        for (auto const& r : reagents)
        {
            if (r.playerHas >= r.perCraft * maxCrafts)
                continue; // inventory alone covers this reagent for maxCrafts

            // Available in bags = what player already has + up to 1 max stack from vault
            uint32 availableInBags = r.playerHas + std::min(r.vaultHas, r.maxStack);
            uint32 possible = availableInBags / r.perCraft;
            maxCrafts = std::min(maxCrafts, possible);
        }

        if (maxCrafts == 0)
        {
            handler->SendSysMessage("Pocket Storage: Not enough reagents.");
            return true;
        }

        craftCount = std::min(craftCount, maxCrafts);

        PocketPlayerData* data = GetPocketData(player);
        if (data)
            data->isMaterializing = true;

        // Materialize reagents needed for craftCount crafts (at most 1 stack per type)
        for (auto const& r : reagents)
        {
            uint32 totalNeeded = r.perCraft * craftCount;
            if (r.playerHas >= totalNeeded)
                continue;

            uint32 deficit = totalNeeded - r.playerHas;
            uint32 toWithdraw = std::min(deficit, r.vaultHas);
            if (toWithdraw == 0)
                continue;

            ItemPosCountVec dest;
            InventoryResult invResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, r.entry, toWithdraw);
            if (invResult != EQUIP_ERR_OK)
            {
                if (data)
                    data->isMaterializing = false;
                handler->SendSysMessage("Pocket Storage: Not enough bag space for reagents.");
                return true;
            }

            // Withdraw before creating: another character sharing the vault may have taken some
            if (!sPocketStorageMgr->WithdrawItem(storageId, r.entry, toWithdraw))
            {
                if (data)
                    data->isMaterializing = false;
                handler->SendSysMessage("Pocket Storage: Not enough reagents.");
                return true;
            }

            Item* newItem = player->StoreNewItem(dest, r.entry, true);
            if (!newItem)
                sPocketStorageMgr->DepositItem(storageId, r.entry, toWithdraw);
            else if (data)
                data->materializedItems.insert(newItem->GetGUID().GetCounter());

            sPocketStorageMgr->BroadcastItemCount(storageId, r.entry);
        }

        if (data)
        {
            data->isMaterializing = false;
            data->autoStoreEnabled = false; // prevent re-deposit of withdrawn reagents
            data->pendingCrafts = craftCount;  // OnSpellCast will decrement and re-cast
            data->pendingSpellId = spellId;
        }

        // Cast once — OnSpellCast will chain the remaining crafts
        player->CastSpell(player, spellId, false);

        return true;
    }

    // .ps combine <itemId> — use an item that combines a set (a Darkmoon card into its deck),
    // with the parts taken from bags and vault. The item itself is the cast item, so the spell
    // uses it up as it does when the player right-clicks it; the spell check above
    // materializes the other parts from the vault.
    static bool HandleCombineCommand(ChatHandler* handler, uint32 itemEntry)
    {
        if (!sPocketStorageMgr->IsEnabled())
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        PocketCombineRecipe const* recipe = sPocketStorageMgr->GetCombineRecipe(itemEntry);
        if (!recipe)
        {
            handler->SendSysMessage("Pocket Storage: That item does not combine into anything.");
            return true;
        }

        uint32 storageId = sPocketStorageMgr->GetStorageId(player);

        std::string missing;
        for (auto const& [entry, count] : recipe->parts)
        {
            if (player->GetItemCount(entry) + sPocketStorageMgr->GetItemCount(storageId, entry) >= count)
                continue;
            if (!missing.empty())
                missing += ", ";
            missing += BuildItemLink(entry);
        }

        if (!missing.empty())
        {
            handler->PSendSysMessage("Pocket Storage: Still missing {} for {}.", missing, BuildItemLink(recipe->resultItem));
            return true;
        }

        // The cast item must be in the bags, not the bank
        Item* castItem = player->GetItemByEntry(itemEntry);
        if (castItem && !Player::IsInventoryPos(castItem->GetPos()))
            castItem = nullptr;

        PocketPlayerData* data = GetPocketData(player);
        if (!castItem)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) != EQUIP_ERR_OK)
            {
                handler->SendSysMessage("Pocket Storage: Not enough bag space.");
                return true;
            }

            // Withdraw before creating: another character sharing the vault may have taken it
            if (!sPocketStorageMgr->WithdrawItem(storageId, itemEntry, 1))
            {
                handler->PSendSysMessage("Pocket Storage: Still missing {} for {}.", BuildItemLink(itemEntry), BuildItemLink(recipe->resultItem));
                return true;
            }

            if (data)
                data->isMaterializing = true;
            castItem = player->StoreNewItem(dest, itemEntry, true);
            if (data)
            {
                data->isMaterializing = false;
                // Goes back to the vault like other leftovers if the cast does not happen
                if (castItem)
                    data->materializedItems.insert(castItem->GetGUID().GetCounter());
            }

            if (!castItem)
            {
                sPocketStorageMgr->DepositItem(storageId, itemEntry, 1);
                handler->SendSysMessage("Pocket Storage: Not enough bag space.");
                return true;
            }
            sPocketStorageMgr->BroadcastItemCount(storageId, itemEntry);
        }

        player->CastSpell(player, recipe->spellId, false, castItem);
        return true;
    }
};

// ============================================================================
// Registration
// ============================================================================

void AddSC_pocket_storage_scripts()
{
    new PocketStorageWorldScript();
    new PocketStoragePlayerScript();
    new PocketStorageSpellScript();
    new PocketStorageCommandScript();
}