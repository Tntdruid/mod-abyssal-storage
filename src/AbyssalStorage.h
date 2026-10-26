#ifndef ABYSSAL_STORAGE_H
#define ABYSSAL_STORAGE_H

#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <map>
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
};

AbyssalPlayerData* GetAbyssalData(Player* player);

class AbyssalStorageMgr
{
public:
    static AbyssalStorageMgr* instance();

    // Shared storage: linked accounts use the vault of their owner account.
    // All storage calls below take a storage id, not the player's own account id.
    uint32 GetStorageId(Player* player);
    uint32 GetStorageIdForAccount(uint32 accountId);
    // Reads the link table directly, bypassing the cache
    uint32 LookupStorageOwner(uint32 accountId);
    // Drop all cached account -> vault mappings (after links were changed directly in the DB)
    void ForgetAccountStorage();

    // Each online player using a vault holds a reference; it is unloaded when the last one leaves
    void LoadAccountData(Player* player);
    void UnloadAccountData(Player* player);
    bool IsAccountLoaded(uint32 storageId);

    // Point an account at another vault and move its online characters along.
    // With mergeItems, the old vault's contents move too (only safe when no other account uses it).
    void SwitchAccountStorage(uint32 accountId, uint32 newStorageId, bool mergeItems);

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
    bool IsSharingEnabled() const { return _sharingEnabled; }
    void SetSharingEnabled(bool enabled) { _sharingEnabled = enabled; }

private:
    AbyssalStorageMgr() = default;

    std::unordered_map<uint32, uint32> QueryStorageItems(uint32 storageId);

    // storageId -> (itemEntry -> count)
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _storage;
    // storageId -> (online player -> their account id)
    std::unordered_map<uint32, std::map<ObjectGuid, uint32>> _members;
    // accountId -> storageId
    std::unordered_map<uint32, uint32> _accountStorage;
    std::mutex _storageMutex;
    bool _enabled = true;
    bool _sharingEnabled = true;
};

#define sAbyssalStorageMgr AbyssalStorageMgr::instance()

#endif // ABYSSAL_STORAGE_H
