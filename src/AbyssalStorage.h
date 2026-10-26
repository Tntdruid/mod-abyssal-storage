#ifndef ABYSSAL_STORAGE_H
#define ABYSSAL_STORAGE_H

#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

class Player;
struct ItemTemplate;

struct PendingDeposit
{
    uint32 itemEntry;
    uint32 count;
};

// Per-player transient state stored via DataMap
struct AbyssalPlayerData : public DataMap::Base
{
    bool autoStoreEnabled = true;
    bool isMaterializing = false; // true while materializing items (suppress auto-deposit)
    std::set<uint32> materializedItems; // item GUIDs currently materialized for crafting
    std::vector<PendingDeposit> pendingDeposits; // deferred auto-deposits
    uint32 pendingCrafts = 0;    // remaining crafts in a multi-craft batch
    uint32 pendingSpellId = 0;   // spell ID for multi-craft batch
    bool randomBotChecked = false; // true once isRandomBot has been resolved
    bool isRandomBot = false;      // cached result of the rndbot account check
    uint32 storageId = 0;          // account whose vault this player uses (0 = not resolved yet)
};

AbyssalPlayerData* GetAbyssalData(Player* player);

class AbyssalStorageMgr
{
public:
    static AbyssalStorageMgr* instance();

    // Shared storage: linked accounts use the vault of their owner account.
    // All storage calls below take a storage id, not the player's own account id.
    uint32 GetStorageId(Player* player);
    uint32 LookupStorageOwner(uint32 accountId);

    // Each online player using a vault holds a reference; it is unloaded when the last one leaves
    void LoadAccountData(uint32 storageId, ObjectGuid playerGuid);
    void UnloadAccountData(uint32 storageId, ObjectGuid playerGuid);
    bool IsAccountLoaded(uint32 storageId);

    void DepositItem(uint32 accountId, uint32 itemEntry, uint32 count);
    bool WithdrawItem(uint32 accountId, uint32 itemEntry, uint32 count);
    uint32 GetItemCount(uint32 accountId, uint32 itemEntry);
    std::unordered_map<uint32, uint32> GetAllItems(uint32 accountId);

    bool ShouldAutoStore(Player* player, ItemTemplate const* itemTemplate);
    bool IsItemRequiredByActiveQuest(Player* player, uint32 itemId);
    uint32 GetQuestReservedCount(Player* player, uint32 itemId);

    // Messaging helpers
    void SendAddonMessage(Player* player, std::string const& message);
    void SendFullSync(Player* player);
    void SendItemUpdate(Player* player, uint32 itemEntry, uint32 count);
    void SendItemDelete(Player* player, uint32 itemEntry);
    // Send an item's current vault count to every online player sharing the vault
    void BroadcastItemCount(uint32 storageId, uint32 itemEntry);

    bool IsEnabled() const { return _enabled; }
    void SetEnabled(bool enabled) { _enabled = enabled; }

private:
    AbyssalStorageMgr() = default;

    // accountId -> (itemEntry -> count)
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _storage;
    // storageId -> online players using that vault
    std::unordered_map<uint32, std::set<ObjectGuid>> _members;
    std::mutex _storageMutex;
    bool _enabled = true;
};

#define sAbyssalStorageMgr AbyssalStorageMgr::instance()

#endif // ABYSSAL_STORAGE_H
