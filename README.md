# mod-pocket-storage

Account-wide pocket vault for trade goods in AzerothCore (WoW 3.3.5).

Automatically deposits trade goods into a shared vault accessible by all characters on the account. Integrates with crafting and quests so vault materials are used seamlessly.

## Features

- **Auto-deposit**: Trade goods are automatically vaulted when picked up
- **Crafting integration**: Vault reagents appear in the TradeSkill UI and are materialized on demand when crafting
- **Quest integration**: Quest-required items are pulled from the vault automatically on turn-in
- **Multi-craft**: "Create All" uses vault materials across the full batch
- **Grid UI**: Searchable item grid with tooltips, opened via `/ps` or right-clicking the backpack
- **Item tooltips**: Every item tooltip (bags, chat links, loot, auction house, professions) shows how many you have in the vault
- **Real-time sync**: Vault state syncs on login and updates incrementally

## Commands

| Command | Description |
|---|---|
| `/ps` | Toggle the vault UI |
| `/ps deposit` | Deposit all trade goods from inventory |
| `/ps withdraw <itemId> [count]` | Withdraw items (omit count for all) |
| `/ps sync` | Force re-sync from server |
| `/ps rules` | Open the Store/Keep lists (also the **Rules** button in the vault window) |

### Store and Keep lists

Each account has its own lists, edited in the addon's rules window by dropping an item, shift-clicking a link or typing an item id. They apply to every character on the account, also when the account shares a vault with others.

- **Store**: always vaulted, even items that are not trade goods (food, potions, scrolls). Only stackable items, since the vault does not keep enchants or durability.
- **Keep**: never vaulted, not even trade goods.
- **Auto-store on loot**: turn automatic vaulting off for the account. Deposit All still works.

| Command | Description |
|---|---|
| `.ps rule store <itemId>` | Add an item to the Store list |
| `.ps rule keep <itemId>` | Add an item to the Keep list |
| `.ps rule remove <itemId>` | Remove an item from both lists |
| `.ps autostore on\|off` | Turn auto-store on loot on or off for the account |

### Sharing a vault between accounts

Accounts can share one vault, for example when one player runs a whole raid team across several accounts. Players can set this up themselves (needs `PocketStorage.EnableSharing = 1`):

| Command | Description |
|---|---|
| `.ps share invite <character>` | Invite the account of `<character>` to use your vault |
| `.ps share accept` | Accept a pending invite. Your vault's items are added to the shared vault. |
| `.ps share decline` | Decline a pending invite |
| `.ps share leave` | Stop using the shared vault. The shared items stay; you start with an empty vault. |
| `.ps share kick <character>` | Remove the account of `<character>` from your vault |
| `.ps share list` | Show who shares your vault |

All characters on an account share the same vault, so inviting one character invites their whole account. Players only see character names, never account names.

### Linking accounts as a GM

GMs can link accounts directly, without invites. These commands need administrator rights and also work from the worldserver console. All characters using the affected vaults must be logged out.

| Command | Description |
|---|---|
| `.ps link <account> <mainAccount>` | Make `<account>` use the vault of `<mainAccount>`. Its own items are merged into the shared vault. |
| `.ps unlink <account>` | Give `<account>` its own, empty vault again. The shared items stay in the shared vault. |

Example: `.ps link raider02 raider01`, `.ps link raider03 raider01`, and so on.

## Configuration

In `mod_pocket_storage.conf`:

```
PocketStorage.Enable = 1
PocketStorage.EnableSharing = 1
PocketStorage.DepositMessage = 1
PocketStorage.ExtraItems = "4278"
PocketStorage.RandomBotAccountPrefix = ""
```

`PocketStorage.RandomBotAccountPrefix` is the username prefix of random playerbot accounts, which the vault ignores. Left empty, it follows mod-playerbots' `Playerbots.RandomBotAccountPrefix` (or `rndbot`).

`PocketStorage.ExtraItems` lists item ids to store on top of trade goods and gems, for gathered items the game files as quest or misc items (4278 is Lesser Bloodstone Ore).

## Upgrading from Abyssal Storage

This module used to be called Abyssal Storage (`mod-abyssal-storage`). Move it to `modules/mod-pocket-storage`, re-run CMake, set up `mod_pocket_storage.conf` with the `PocketStorage.*` keys, and replace the `AbyssalStorage` addon with `PocketStorage`. On its first start, the server renames the old `abyssal_storage*` tables to `pocket_storage*`, so vault contents, shares and lists carry over.

## Installation

1. Clone into `modules/mod-pocket-storage`
2. Re-run CMake and build
3. Copy `conf/mod_pocket_storage.conf.dist` to your server's config directory
4. Run the files in `data/sql/db-characters/` (`pocket_storage.sql`, `pocket_storage_link.sql`, `pocket_storage_invite.sql`, `pocket_storage_rule.sql`) against your characters database
5. Copy the `addon/PocketStorage` folder into your WoW `Interface/AddOns` directory

## Credits

- Based on [mod-abyssal-storage](https://github.com/thanhtong89/mod-abyssal-storage) by thanhtong89.
- Maintained by [Tntdruid](https://github.com/Tntdruid).
- Random playerbot exclusion by [latchee](https://github.com/latchee).
- Developed together with Claude (Anthropic), credited as co-author on its commits.
