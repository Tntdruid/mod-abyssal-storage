#ifndef ABYSSAL_STORAGE_H
#define ABYSSAL_STORAGE_H

#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <unordered_set>
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

// Per-account item rule, set from the addon's Store/Keep lists
enum class AbyssalItemRule : uint8
{
    None  = 0,
    Store = 1, // always vault the item, whatever its class
    Keep  = 2  // never vault the item
};

// Upper bound on Store + Keep rules per account
constexpr std::size_t ABYSSAL_MAX_ITEM_RULES = 500;

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
    // Item ids stored on top of trade goods and gems (AbyssalStorage.ExtraItems)
    void SetExtraItems(std::string const& itemList);
    bool IsItemRequiredByActiveQuest(Player* player, uint32 itemId);
    uint32 GetQuestReservedCount(Player* player, uint32 itemId);

    // Store/Keep rules and the auto-store switch belong to the player's own account,
    // not the shared vault. Loaded while a character of the account is online.
    AbyssalItemRule GetItemRule(uint32 accountId, uint32 itemEntry);
    std::size_t GetItemRuleCount(uint32 accountId);
    // Saves the rule and updates the addon of every online character on the account
    void SetItemRule(uint32 accountId, uint32 itemEntry, AbyssalItemRule rule);
    bool IsAutoStoreOnLoot(uint32 accountId);
    void SetAutoStoreOnLoot(uint32 accountId, bool enabled);
    void SendAccountSettings(Player* player);

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
    // Lowercase username prefix of random playerbot accounts, which the module ignores
    std::string const& GetRandomBotPrefix() const { return _randomBotPrefix; }
    void SetRandomBotPrefix(std::string const& prefix) { _randomBotPrefix = prefix; }

private:
    AbyssalStorageMgr() = default;

    struct AccountSettings
    {
        std::unordered_map<uint32, AbyssalItemRule> rules; // itemEntry -> rule
        bool autoStoreOnLoot = true;
    };

    std::unordered_map<uint32, uint32> QueryStorageItems(uint32 storageId);
    AccountSettings QueryAccountSettings(uint32 accountId);
    // Send a message to every online character of the account
    void SendToAccount(uint32 accountId, std::string const& message);

    // storageId -> (itemEntry -> count)
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _storage;
    // storageId -> (online player -> their account id)
    std::unordered_map<uint32, std::map<ObjectGuid, uint32>> _members;
    // accountId -> storageId
    std::unordered_map<uint32, uint32> _accountStorage;
    // accountId -> Store/Keep rules and settings, for accounts with a character online
    std::unordered_map<uint32, AccountSettings> _accountSettings;
    std::unordered_set<uint32> _extraItems;
    std::mutex _storageMutex;
    bool _enabled = true;
    bool _sharingEnabled = true;
    std::string _randomBotPrefix = "rndbot";
};

#define sAbyssalStorageMgr AbyssalStorageMgr::instance()

#endif // ABYSSAL_STORAGE_H
