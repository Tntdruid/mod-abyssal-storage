#include "PocketStorage.h"
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

PocketPlayerData* GetPocketData(Player* player)
{
    if (!player)
        return nullptr;
    return player->CustomData.GetDefault<PocketPlayerData>("PocketData");
}

PocketStorageMgr* PocketStorageMgr::instance()
{
    static PocketStorageMgr instance;
    return &instance;
}

static bool CharacterTableExists(std::string const& table)
{
    return bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table));
}

void PocketStorageMgr::MigrateLegacyTables()
{
    // The module used to be called Abyssal Storage; carry its data over to the new table names
    static std::pair<char const*, char const*> const tables[] =
    {
        { "abyssal_storage",          "pocket_storage" },
        { "abyssal_storage_link",     "pocket_storage_link" },
        { "abyssal_storage_invite",   "pocket_storage_invite" },
        { "abyssal_storage_rule",     "pocket_storage_rule" },
        { "abyssal_storage_settings", "pocket_storage_settings" },
    };

    for (auto const& [oldName, newName] : tables)
    {
        if (!CharacterTableExists(oldName))
            continue;

        if (CharacterTableExists(newName))
        {
            // The new SQL files may already have created an empty table; never overwrite real data
            if (CharacterDatabase.Query("SELECT 1 FROM `{}` LIMIT 1", newName))
            {
                LOG_ERROR("module", "PocketStorage: both `{}` and `{}` hold data, so `{}` was not migrated. Merge or drop one of them by hand.",
                    oldName, newName, oldName);
                continue;
            }
            CharacterDatabase.DirectExecute("DROP TABLE `{}`", newName);
        }

        CharacterDatabase.DirectExecute("RENAME TABLE `{}` TO `{}`", oldName, newName);
        LOG_INFO("module", "PocketStorage: renamed table `{}` to `{}`.", oldName, newName);
    }
}

uint32 PocketStorageMgr::LookupStorageOwner(uint32 accountId)
{
    QueryResult result = CharacterDatabase.Query("SELECT owner_account_id FROM pocket_storage_link WHERE account_id = {}", accountId);
    if (!result)
        return accountId;

    return result->Fetch()[0].Get<uint32>();
}

uint32 PocketStorageMgr::GetStorageId(Player* player)
{
    return GetStorageIdForAccount(player->GetSession()->GetAccountId());
}

uint32 PocketStorageMgr::GetStorageIdForAccount(uint32 accountId)
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

void PocketStorageMgr::ForgetAccountStorage()
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    _accountStorage.clear();
}

std::unordered_map<uint32, uint32> PocketStorageMgr::QueryStorageItems(uint32 storageId)
{
    QueryResult result = CharacterDatabase.Query("SELECT item_entry, count FROM pocket_storage WHERE account_id = {}", storageId);

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

PocketStorageMgr::AccountSettings PocketStorageMgr::QueryAccountSettings(uint32 accountId)
{
    AccountSettings settings;

    if (QueryResult result = CharacterDatabase.Query("SELECT item_entry, rule FROM pocket_storage_rule WHERE account_id = {}", accountId))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 rule = fields[1].Get<uint8>();
            if (rule == uint8(PocketItemRule::Store) || rule == uint8(PocketItemRule::Keep))
                settings.rules[fields[0].Get<uint32>()] = PocketItemRule(rule);
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT auto_store FROM pocket_storage_settings WHERE account_id = {}", accountId))
        settings.autoStoreOnLoot = result->Fetch()[0].Get<uint8>() != 0;

    return settings;
}

void PocketStorageMgr::LoadAccountData(Player* player)
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

void PocketStorageMgr::UnloadAccountData(Player* player)
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

bool PocketStorageMgr::IsAccountLoaded(uint32 storageId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    return _storage.find(storageId) != _storage.end();
}

void PocketStorageMgr::SwitchAccountStorage(uint32 accountId, uint32 newStorageId, bool mergeItems)
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

void PocketStorageMgr::DepositItem(uint32 accountId, uint32 itemEntry, uint32 count)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _storage[accountId][itemEntry] += count;
    }

    CharacterDatabase.Execute("INSERT INTO pocket_storage (account_id, item_entry, count) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE count = count + {}", accountId, itemEntry, count, count);
}

bool PocketStorageMgr::WithdrawItem(uint32 accountId, uint32 itemEntry, uint32 count)
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
        CharacterDatabase.Execute("DELETE FROM pocket_storage WHERE account_id = {} AND item_entry = {}", accountId, itemEntry);
    }
    else
    {
        CharacterDatabase.Execute("UPDATE pocket_storage SET count = {} WHERE account_id = {} AND item_entry = {}",
            itemIt->second, accountId, itemEntry);
    }

    return true;
}

uint32 PocketStorageMgr::GetItemCount(uint32 accountId, uint32 itemEntry)
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

std::unordered_map<uint32, uint32> PocketStorageMgr::GetAllItems(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _storage.find(accountId);
    if (accIt == _storage.end())
        return {};

    return accIt->second;
}

uint32 PocketStorageMgr::GetQuestReservedCount(Player* player, uint32 itemId)
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

bool PocketStorageMgr::IsItemRequiredByActiveQuest(Player* player, uint32 itemId)
{
    uint32 reserved = GetQuestReservedCount(player, itemId);
    return reserved > 0 && player->GetItemCount(itemId) <= reserved;
}

void PocketStorageMgr::SetExtraItems(std::string const& itemList)
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
            LOG_ERROR("module", "PocketStorage.ExtraItems: '{}' is not a valid item id, ignored.", token);
    }
}

bool PocketStorageMgr::ShouldAutoStore(Player* player, ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return false;

    PocketItemRule rule = GetItemRule(player->GetSession()->GetAccountId(), itemTemplate->ItemId);
    if (rule == PocketItemRule::Keep)
        return false;

    // The vault keeps only entry and count, so a Store rule never applies to unstackable
    // items, whose enchants, gems and durability would be lost
    if (rule != PocketItemRule::Store || itemTemplate->GetMaxStackSize() <= 1)
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

PocketItemRule PocketStorageMgr::GetItemRule(uint32 accountId, uint32 itemEntry)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    if (accIt == _accountSettings.end())
        return PocketItemRule::None;

    auto ruleIt = accIt->second.rules.find(itemEntry);
    return ruleIt == accIt->second.rules.end() ? PocketItemRule::None : ruleIt->second;
}

std::size_t PocketStorageMgr::GetItemRuleCount(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    return accIt == _accountSettings.end() ? 0 : accIt->second.rules.size();
}

void PocketStorageMgr::SetItemRule(uint32 accountId, uint32 itemEntry, PocketItemRule rule)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto& rules = _accountSettings[accountId].rules;
        if (rule == PocketItemRule::None)
            rules.erase(itemEntry);
        else
            rules[itemEntry] = rule;
    }

    if (rule == PocketItemRule::None)
        CharacterDatabase.Execute("DELETE FROM pocket_storage_rule WHERE account_id = {} AND item_entry = {}", accountId, itemEntry);
    else
        CharacterDatabase.Execute("REPLACE INTO pocket_storage_rule (account_id, item_entry, rule) VALUES ({}, {}, {})",
            accountId, itemEntry, uint32(rule)); // uint8 would be formatted as a character

    char const* code = rule == PocketItemRule::Store ? "S" : rule == PocketItemRule::Keep ? "K" : "N";
    SendToAccount(accountId, std::string("RULE:") + code + ":" + std::to_string(itemEntry));
}

bool PocketStorageMgr::IsAutoStoreOnLoot(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto accIt = _accountSettings.find(accountId);
    return accIt == _accountSettings.end() || accIt->second.autoStoreOnLoot;
}

void PocketStorageMgr::SetAutoStoreOnLoot(uint32 accountId, bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _accountSettings[accountId].autoStoreOnLoot = enabled;
    }

    CharacterDatabase.Execute("REPLACE INTO pocket_storage_settings (account_id, auto_store) VALUES ({}, {})",
        accountId, enabled ? 1 : 0);

    SendToAccount(accountId, std::string("AUTO:") + (enabled ? "1" : "0"));
}

void PocketStorageMgr::SendToAccount(uint32 accountId, std::string const& message)
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
void PocketStorageMgr::SendAccountSettings(Player* player)
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
                (rule == PocketItemRule::Store ? store : keep).push_back(entry);
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

void PocketStorageMgr::SendAddonMessage(Player* player, std::string const& message)
{
    // Prefix with "PCKT\t" so client receives arg1="PCKT", arg2=message
    const size_t MAX_MSG_LEN = 240; // leave room for prefix
    std::string fullMsg = "PCKT\t" + message;

    if (fullMsg.length() <= MAX_MSG_LEN)
    {
        SendOnePacket(player, fullMsg);
        return;
    }

    // For SYNC messages, split by semicolons to create valid chunks
    size_t prefixEnd = message.find(':');
    if (prefixEnd == std::string::npos)
    {
        SendOnePacket(player, ("PCKT\t" + message.substr(0, MAX_MSG_LEN - 5)));
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
            SendOnePacket(player, "PCKT\t" + chunk);
            chunk = prefix;
        }

        if (chunk.length() > prefix.length())
            chunk += ";";
        chunk += entry;
    }

    if (chunk.length() > prefix.length())
        SendOnePacket(player, "PCKT\t" + chunk);
}

void PocketStorageMgr::SendFullSync(Player* player)
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

void PocketStorageMgr::SendItemUpdate(Player* player, uint32 itemEntry, uint32 count)
{
    std::string msg = "UPD:" + std::to_string(itemEntry) + "," + std::to_string(count);
    SendAddonMessage(player, msg);
}

void PocketStorageMgr::SendItemDelete(Player* player, uint32 itemEntry)
{
    std::string msg = "DEL:" + std::to_string(itemEntry);
    SendAddonMessage(player, msg);
}

void PocketStorageMgr::BroadcastItemCount(uint32 storageId, uint32 itemEntry)
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
