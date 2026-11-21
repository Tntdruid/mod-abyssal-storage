#include "AbyssalStorage.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Log.h"
#include <algorithm>
#include <sstream>

AbyssalPlayerData* GetAbyssalData(Player* player)
{
    if (!player)
        return nullptr;
    return player->CustomData.GetDefault<AbyssalPlayerData>("AbyssalData");
}

AbyssalStorageMgr* AbyssalStorageMgr::instance()
{
    static AbyssalStorageMgr instance;
    return &instance;
}

uint32 AbyssalStorageMgr::LookupStorageOwner(uint32 accountId)
{
    QueryResult result = CharacterDatabase.Query("SELECT owner_account_id FROM abyssal_storage_link WHERE account_id = {}", accountId);
    if (!result)
        return accountId;

    return result->Fetch()[0].Get<uint32>();
}

uint32 AbyssalStorageMgr::GetStorageId(Player* player)
{
    return GetStorageIdForAccount(player->GetSession()->GetAccountId());
}

uint32 AbyssalStorageMgr::GetStorageIdForAccount(uint32 accountId)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto it = _accountStorage.find(accountId);
        if (it != _accountStorage.end())
            return it->second;
    }

    // Cached after the first lookup, since it needs a DB query
    uint32 storageId = LookupStorageOwner(accountId);

    std::lock_guard<std::mutex> lock(_storageMutex);
    return _accountStorage.emplace(accountId, storageId).first->second;
}

void AbyssalStorageMgr::ForgetAccountStorage()
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _accountStorage.clear();
}

std::unordered_map<uint32, uint32> AbyssalStorageMgr::QueryStorageItems(uint32 storageId)
{
    QueryResult result = CharacterDatabase.Query("SELECT item_entry, count FROM abyssal_storage WHERE account_id = {}", storageId);

    std::unordered_map<uint32, uint32> items;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 itemEntry = fields[0].Get<uint32>();
            uint32 count = fields[1].Get<uint32>();
            items[itemEntry] = count;
        } while (result->NextRow());
    }

    return items;
}

AbyssalStorageMgr::AccountSettings AbyssalStorageMgr::QueryAccountSettings(uint32 accountId)
{
    AccountSettings settings;

    if (QueryResult result = CharacterDatabase.Query("SELECT item_entry, rule FROM abyssal_storage_rule WHERE account_id = {}", accountId))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 rule = fields[1].Get<uint8>();
            if (rule == uint8(AbyssalItemRule::Store) || rule == uint8(AbyssalItemRule::Keep))
                settings.rules[fields[0].Get<uint32>()] = AbyssalItemRule(rule);
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT auto_store FROM abyssal_storage_settings WHERE account_id = {}", accountId))
        settings.autoStoreOnLoot = result->Fetch()[0].Get<uint8>() != 0;

    return settings;
}

void AbyssalStorageMgr::LoadAccountData(Player* player)
{
    uint32 accountId = player->GetSession()->GetAccountId();
    uint32 storageId = GetStorageIdForAccount(accountId);

    bool storageLoaded;
    bool settingsLoaded;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _members[storageId][player->GetGUID()] = accountId;
        storageLoaded = _storage.find(storageId) != _storage.end();
        settingsLoaded = _accountSettings.find(accountId) != _accountSettings.end();
    }

    // DB queries run outside the lock; another player on the same vault or account
    // may load the same data meanwhile, and emplace keeps whichever came first
    if (!storageLoaded)
    {
        auto items = QueryStorageItems(storageId);
        std::lock_guard<std::mutex> lock(_storageMutex);
        _storage.emplace(storageId, std::move(items));
    }

    if (!settingsLoaded)
    {
        AccountSettings settings = QueryAccountSettings(accountId);
        std::lock_guard<std::mutex> lock(_storageMutex);
        _accountSettings.emplace(accountId, std::move(settings));
    }
}

void AbyssalStorageMgr::UnloadAccountData(Player* player)
{
    uint32 accountId = player->GetSession()->GetAccountId();
    uint32 storageId = GetStorageId(player);

    std::lock_guard<std::mutex> lock(_storageMutex);

    auto it = _members.find(storageId);
    if (it != _members.end())
    {
        it->second.erase(player->GetGUID());
        if (it->second.empty())
            _members.erase(it);
    }

    // Keep the vault while other players sharing it are online
    if (_members.find(storageId) == _members.end())
        _storage.erase(storageId);

    // Keep the account's rules while another of its characters is online
    for (auto const& [id, members] : _members)
        for (auto const& [guid, memberAccountId] : members)
            if (memberAccountId == accountId)
                return;

    _accountSettings.erase(accountId);
}

bool AbyssalStorageMgr::IsAccountLoaded(uint32 storageId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    return _storage.find(storageId) != _storage.end();
}

void AbyssalStorageMgr::SwitchAccountStorage(uint32 accountId, uint32 newStorageId, bool mergeItems)
{
    uint32 oldStorageId = GetStorageIdForAccount(accountId);
    if (oldStorageId == newStorageId)
        return;

    // Load the new vault up front if it is not in memory; the DB query must run outside the lock
    std::unordered_map<uint32, uint32> newItems;
    bool newLoaded = IsAccountLoaded(newStorageId);
    if (!newLoaded)
        newItems = QueryStorageItems(newStorageId);

    std::vector<ObjectGuid> movedPlayers;
    std::vector<uint32> mergedEntries;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _accountStorage[accountId] = newStorageId;

        // Every online character of the account moves, including playerbots on the same account
        auto oldMembers = _members.find(oldStorageId);
        if (oldMembers != _members.end())
        {
            for (auto it = oldMembers->second.begin(); it != oldMembers->second.end();)
            {
                if (it->second == accountId)
                {
                    movedPlayers.push_back(it->first);
                    _members[newStorageId][it->first] = accountId;
                    it = oldMembers->second.erase(it);
                }
                else
                    ++it;
            }
        }

        if (!movedPlayers.empty())
            _storage.emplace(newStorageId, std::move(newItems));

        auto oldStorage = _storage.find(oldStorageId);
        if (mergeItems && oldStorage != _storage.end())
        {
            auto newStorage = _storage.find(newStorageId);
            if (newStorage != _storage.end())
            {
                for (auto const& [entry, count] : oldStorage->second)
                {
                    newStorage->second[entry] += count;
                    mergedEntries.push_back(entry);
                }
            }
            oldStorage->second.clear();
        }

        oldMembers = _members.find(oldStorageId);
        if (oldMembers != _members.end() && oldMembers->second.empty())
        {
            _members.erase(oldMembers);
            _storage.erase(oldStorageId);
        }
    }

    for (ObjectGuid const& guid : movedPlayers)
        if (Player* moved = ObjectAccessor::FindConnectedPlayer(guid))
            SendFullSync(moved);

    // Let the others already sharing the new vault see the merged items
    for (uint32 entry : mergedEntries)
        BroadcastItemCount(newStorageId, entry);
}

void AbyssalStorageMgr::DepositItem(uint32 accountId, uint32 itemEntry, uint32 count)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _storage[accountId][itemEntry] += count;
    }

    CharacterDatabase.Execute("INSERT INTO abyssal_storage (account_id, item_entry, count) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE count = count + {}", accountId, itemEntry, count, count);
}

bool AbyssalStorageMgr::WithdrawItem(uint32 accountId, uint32 itemEntry, uint32 count)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _storage.find(accountId);
    if (accIt == _storage.end())
        return false;

    auto itemIt = accIt->second.find(itemEntry);
    if (itemIt == accIt->second.end() || itemIt->second < count)
        return false;

    itemIt->second -= count;

    if (itemIt->second == 0)
    {
        accIt->second.erase(itemIt);
        CharacterDatabase.Execute("DELETE FROM abyssal_storage WHERE account_id = {} AND item_entry = {}", accountId, itemEntry);
    }
    else
    {
        CharacterDatabase.Execute("UPDATE abyssal_storage SET count = {} WHERE account_id = {} AND item_entry = {}",
            itemIt->second, accountId, itemEntry);
    }

    return true;
}

uint32 AbyssalStorageMgr::GetItemCount(uint32 accountId, uint32 itemEntry)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _storage.find(accountId);
    if (accIt == _storage.end())
        return 0;

    auto itemIt = accIt->second.find(itemEntry);
    if (itemIt == accIt->second.end())
        return 0;

    return itemIt->second;
}

std::unordered_map<uint32, uint32> AbyssalStorageMgr::GetAllItems(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _storage.find(accountId);
    if (accIt == _storage.end())
        return {};

    return accIt->second;
}

uint32 AbyssalStorageMgr::GetQuestReservedCount(Player* player, uint32 itemId)
{
    uint32 reserved = 0;

    for (uint8 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = player->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        QuestStatus status = player->GetQuestStatus(questId);
        if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint8 j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
        {
            if (quest->RequiredItemId[j] == itemId)
                reserved += quest->RequiredItemCount[j];
        }
    }

    return reserved;
}

bool AbyssalStorageMgr::IsItemRequiredByActiveQuest(Player* player, uint32 itemId)
{
    uint32 reserved = GetQuestReservedCount(player, itemId);
    return reserved > 0 && player->GetItemCount(itemId) <= reserved;
}

void AbyssalStorageMgr::SetExtraItems(std::string const& itemList)
{
    _extraItems.clear();

    // Accept "4278, 1234" as well as "4278 1234"
    std::string list = itemList;
    std::replace(list.begin(), list.end(), ',', ' ');

    std::istringstream stream(list);
    std::string token;
    while (stream >> token)
    {
        // Item templates are not loaded yet when the config is first read, so only check the number
        uint32 itemId = static_cast<uint32>(std::strtoul(token.c_str(), nullptr, 10));
        if (itemId)
            _extraItems.insert(itemId);
        else
            LOG_ERROR("module", "AbyssalStorage.ExtraItems: '{}' is not a valid item id, ignored.", token);
    }
}

bool AbyssalStorageMgr::ShouldAutoStore(Player* player, ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return false;

    AbyssalItemRule rule = GetItemRule(player->GetSession()->GetAccountId(), itemTemplate->ItemId);
    if (rule == AbyssalItemRule::Keep)
        return false;

    // The vault keeps only entry and count, so a Store rule never applies to unstackable
    // items, whose enchants, gems and durability would be lost
    if (rule != AbyssalItemRule::Store || itemTemplate->GetMaxStackSize() <= 1)
    {
        if (itemTemplate->Class != ITEM_CLASS_TRADE_GOODS && itemTemplate->Class != ITEM_CLASS_GEM
            && _extraItems.find(itemTemplate->ItemId) == _extraItems.end())
            return false;

        // Profession tools (runed enchanting rods, etc.) must stay in bags to satisfy spell totem requirements
        if (itemTemplate->TotemCategory)
            return false;
    }

    if (IsItemRequiredByActiveQuest(player, itemTemplate->ItemId))
        return false;

    return true;
}

AbyssalItemRule AbyssalStorageMgr::GetItemRule(uint32 accountId, uint32 itemEntry)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    if (accIt == _accountSettings.end())
        return AbyssalItemRule::None;

    auto ruleIt = accIt->second.rules.find(itemEntry);
    return ruleIt == accIt->second.rules.end() ? AbyssalItemRule::None : ruleIt->second;
}

std::size_t AbyssalStorageMgr::GetItemRuleCount(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    return accIt == _accountSettings.end() ? 0 : accIt->second.rules.size();
}

void AbyssalStorageMgr::SetItemRule(uint32 accountId, uint32 itemEntry, AbyssalItemRule rule)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto& rules = _accountSettings[accountId].rules;
        if (rule == AbyssalItemRule::None)
            rules.erase(itemEntry);
        else
            rules[itemEntry] = rule;
    }

    if (rule == AbyssalItemRule::None)
        CharacterDatabase.Execute("DELETE FROM abyssal_storage_rule WHERE account_id = {} AND item_entry = {}", accountId, itemEntry);
    else
        CharacterDatabase.Execute("REPLACE INTO abyssal_storage_rule (account_id, item_entry, rule) VALUES ({}, {}, {})",
            accountId, itemEntry, uint32(rule)); // uint8 would be formatted as a character

    char const* code = rule == AbyssalItemRule::Store ? "S" : rule == AbyssalItemRule::Keep ? "K" : "N";
    SendToAccount(accountId, std::string("RULE:") + code + ":" + std::to_string(itemEntry));
}

bool AbyssalStorageMgr::IsAutoStoreOnLoot(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    return accIt == _accountSettings.end() || accIt->second.autoStoreOnLoot;
}

void AbyssalStorageMgr::SetAutoStoreOnLoot(uint32 accountId, bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _accountSettings[accountId].autoStoreOnLoot = enabled;
    }

    CharacterDatabase.Execute("REPLACE INTO abyssal_storage_settings (account_id, auto_store) VALUES ({}, {})",
        accountId, enabled ? 1 : 0);

    SendToAccount(accountId, std::string("AUTO:") + (enabled ? "1" : "0"));
}

void AbyssalStorageMgr::SendToAccount(uint32 accountId, std::string const& message)
{
    std::vector<ObjectGuid> members;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        for (auto const& [storageId, storageMembers] : _members)
            for (auto const& [guid, memberAccountId] : storageMembers)
                if (memberAccountId == accountId)
                    members.push_back(guid);
    }

    for (ObjectGuid const& guid : members)
        if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
            SendAddonMessage(member, message);
}

// RULES:BEGIN, then the ids in short RULES:S:/RULES:K: lines, then RULES:END and AUTO:<0|1>.
// The lines are built here, since SendAddonMessage only knows how to split SYNC-style lists.
void AbyssalStorageMgr::SendAccountSettings(Player* player)
{
    uint32 accountId = player->GetSession()->GetAccountId();

    std::vector<uint32> store;
    std::vector<uint32> keep;
    bool autoStore = true;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto accIt = _accountSettings.find(accountId);
        if (accIt != _accountSettings.end())
        {
            for (auto const& [entry, rule] : accIt->second.rules)
                (rule == AbyssalItemRule::Store ? store : keep).push_back(entry);
            autoStore = accIt->second.autoStoreOnLoot;
        }
    }

    SendAddonMessage(player, "RULES:BEGIN");

    auto sendList = [&](char const* prefix, std::vector<uint32> const& ids)
    {
        std::string line;
        for (uint32 id : ids)
        {
            std::string idText = std::to_string(id);
            if (!line.empty() && line.length() + 1 + idText.length() > 180)
            {
                SendAddonMessage(player, prefix + line);
                line.clear();
            }
            if (!line.empty())
                line += ",";
            line += idText;
        }
        if (!line.empty())
            SendAddonMessage(player, prefix + line);
    };
    sendList("RULES:S:", store);
    sendList("RULES:K:", keep);

    SendAddonMessage(player, "RULES:END");
    SendAddonMessage(player, std::string("AUTO:") + (autoStore ? "1" : "0"));
}

// Send a raw LANG_ADDON packet. The WoW 3.3.5 client splits on '\t':
//   arg1 (prefix) = everything before first \t
//   arg2 (body)   = everything after first \t
// Client fires CHAT_MSG_ADDON event.
static void SendOnePacket(Player* player, std::string const& msg)
{
    WorldPacket data;
    std::size_t len = msg.length();
    data.Initialize(SMSG_MESSAGECHAT, 1 + 4 + 8 + 4 + 8 + 4 + 1 + len + 1);
    data << uint8(CHAT_MSG_WHISPER);
    data << uint32(LANG_ADDON);
    data << uint64(0);
    data << uint32(0);
    data << uint64(0);
    data << uint32(len + 1);
    data << msg;
    data << uint8(0);
    player->GetSession()->SendPacket(&data);
}

void AbyssalStorageMgr::SendAddonMessage(Player* player, std::string const& message)
{
    // Prefix with "ABYS\t" so client receives arg1="ABYS", arg2=message
    const size_t MAX_MSG_LEN = 240; // leave room for prefix
    std::string fullMsg = "ABYS\t" + message;

    if (fullMsg.length() <= MAX_MSG_LEN)
    {
        SendOnePacket(player, fullMsg);
        return;
    }

    // For SYNC messages, split by semicolons to create valid chunks
    size_t prefixEnd = message.find(':');
    if (prefixEnd == std::string::npos)
    {
        SendOnePacket(player, ("ABYS\t" + message.substr(0, MAX_MSG_LEN - 5)));
        return;
    }
    prefixEnd++;

    std::string prefix = message.substr(0, prefixEnd);
    std::string payload = message.substr(prefixEnd);

    std::string chunk = prefix;
    size_t pos = 0;
    while (pos < payload.length())
    {
        size_t nextSep = payload.find(';', pos);
        std::string entry;
        if (nextSep == std::string::npos)
        {
            entry = payload.substr(pos);
            pos = payload.length();
        }
        else
        {
            entry = payload.substr(pos, nextSep - pos);
            pos = nextSep + 1;
        }

        if (chunk.length() > prefix.length() && chunk.length() + 1 + entry.length() + 5 > MAX_MSG_LEN)
        {
            SendOnePacket(player, "ABYS\t" + chunk);
            chunk = prefix;
        }

        if (chunk.length() > prefix.length())
            chunk += ";";
        chunk += entry;
    }

    if (chunk.length() > prefix.length())
        SendOnePacket(player, "ABYS\t" + chunk);
}

void AbyssalStorageMgr::SendFullSync(Player* player)
{
    auto items = GetAllItems(GetStorageId(player));

    if (items.empty())
    {
        SendAddonMessage(player, "SYNC:");
        return;
    }

    std::string msg = "SYNC:";
    bool first = true;
    for (auto const& pair : items)
    {
        if (!first)
            msg += ";";
        msg += std::to_string(pair.first) + "," + std::to_string(pair.second);
        first = false;
    }

    SendAddonMessage(player, msg);
}

void AbyssalStorageMgr::SendItemUpdate(Player* player, uint32 itemEntry, uint32 count)
{
    std::string msg = "UPD:" + std::to_string(itemEntry) + "," + std::to_string(count);
    SendAddonMessage(player, msg);
}

void AbyssalStorageMgr::SendItemDelete(Player* player, uint32 itemEntry)
{
    std::string msg = "DEL:" + std::to_string(itemEntry);
    SendAddonMessage(player, msg);
}

void AbyssalStorageMgr::BroadcastItemCount(uint32 storageId, uint32 itemEntry)
{
    uint32 count = GetItemCount(storageId, itemEntry);

    std::vector<ObjectGuid> members;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto it = _members.find(storageId);
        if (it == _members.end())
            return;
        for (auto const& [guid, accountId] : it->second)
            members.push_back(guid);
    }

    // The addon treats an update with count 0 as a delete
    for (ObjectGuid const& guid : members)
        if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
            SendItemUpdate(member, itemEntry, count);
}
