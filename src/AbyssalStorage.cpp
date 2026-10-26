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
    // Resolved once per login and cached, since it needs a DB lookup
    AbyssalPlayerData* data = GetAbyssalData(player);
    if (!data->storageId)
        data->storageId = LookupStorageOwner(player->GetSession()->GetAccountId());

    return data->storageId;
}

void AbyssalStorageMgr::LoadAccountData(uint32 storageId, ObjectGuid playerGuid)
{
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        _members[storageId].insert(playerGuid);
        if (_storage.find(storageId) != _storage.end())
            return; // already loaded
    }

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

    std::lock_guard<std::mutex> lock(_storageMutex);
    // Another player on the same vault may have loaded it while we queried
    _storage.emplace(storageId, std::move(items));
}

void AbyssalStorageMgr::UnloadAccountData(uint32 storageId, ObjectGuid playerGuid)
{
    std::lock_guard<std::mutex> lock(_storageMutex);

    auto it = _members.find(storageId);
    if (it != _members.end())
    {
        it->second.erase(playerGuid);
        if (!it->second.empty())
            return; // still in use by other players sharing this vault
        _members.erase(it);
    }

    _storage.erase(storageId);
}

bool AbyssalStorageMgr::IsAccountLoaded(uint32 storageId)
{
    std::lock_guard<std::mutex> lock(_storageMutex);
    return _storage.find(storageId) != _storage.end();
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

bool AbyssalStorageMgr::ShouldAutoStore(Player* player, ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return false;

    if (itemTemplate->Class != ITEM_CLASS_TRADE_GOODS && itemTemplate->Class != ITEM_CLASS_GEM)
        return false;

    // Profession tools (runed enchanting rods, etc.) must stay in bags to satisfy spell totem requirements
    if (itemTemplate->TotemCategory)
        return false;

    if (IsItemRequiredByActiveQuest(player, itemTemplate->ItemId))
        return false;

    return true;
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

    std::set<ObjectGuid> members;
    {
        std::lock_guard<std::mutex> lock(_storageMutex);
        auto it = _members.find(storageId);
        if (it == _members.end())
            return;
        members = it->second;
    }

    // The addon treats an update with count 0 as a delete
    for (ObjectGuid const& guid : members)
        if (Player* member = ObjectAccessor::FindConnectedPlayer(guid))
            SendItemUpdate(member, itemEntry, count);
}
