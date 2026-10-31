# mod-abyssal-storage

Account-wide interdimensional vault for trade goods in AzerothCore (WoW 3.3.5).

Automatically deposits trade goods into a shared vault accessible by all characters on the account. Integrates with crafting and quests so vault materials are used seamlessly.

## Features

- **Auto-deposit**: Trade goods are automatically vaulted when picked up
- **Crafting integration**: Vault reagents appear in the TradeSkill UI and are materialized on demand when crafting
- **Quest integration**: Quest-required items are pulled from the vault automatically on turn-in
- **Multi-craft**: "Create All" uses vault materials across the full batch
- **Grid UI**: Searchable item grid with tooltips, opened via `/abs` or right-clicking the backpack
- **Item tooltips**: Every item tooltip (bags, chat links, loot, auction house, professions) shows how many you have in the vault
- **Real-time sync**: Vault state syncs on login and updates incrementally

## Commands

| Command | Description |
|---|---|
| `/abs` | Toggle the vault UI |
| `/abs deposit` | Deposit all trade goods from inventory |
| `/abs withdraw <itemId> [count]` | Withdraw items (omit count for all) |
| `/abs sync` | Force re-sync from server |

### Sharing a vault between accounts

Accounts can share one vault, for example when one player runs a whole raid team across several accounts. Players can set this up themselves (needs `AbyssalStorage.EnableSharing = 1`):

| Command | Description |
|---|---|
| `.abs share invite <character>` | Invite the account of `<character>` to use your vault |
| `.abs share accept` | Accept a pending invite. Your vault's items are added to the shared vault. |
| `.abs share decline` | Decline a pending invite |
| `.abs share leave` | Stop using the shared vault. The shared items stay; you start with an empty vault. |
| `.abs share kick <character>` | Remove the account of `<character>` from your vault |
| `.abs share list` | Show who shares your vault |

All characters on an account share the same vault, so inviting one character invites their whole account. Players only see character names, never account names.

### Linking accounts as a GM

GMs can link accounts directly, without invites. These commands need administrator rights and also work from the worldserver console. All characters using the affected vaults must be logged out.

| Command | Description |
|---|---|
| `.abs link <account> <mainAccount>` | Make `<account>` use the vault of `<mainAccount>`. Its own items are merged into the shared vault. |
| `.abs unlink <account>` | Give `<account>` its own, empty vault again. The shared items stay in the shared vault. |

Example: `.abs link raider02 raider01`, `.abs link raider03 raider01`, and so on.

## Configuration

In `mod_abyssal_storage.conf`:

```
AbyssalStorage.Enable = 1
AbyssalStorage.EnableSharing = 1
```

## Installation

1. Clone into `modules/mod-abyssal-storage`
2. Re-run CMake and build
3. Copy `conf/mod_abyssal_storage.conf.dist` to your server's config directory
4. Run the files in `data/sql/db-characters/` (`abyssal_storage.sql`, `abyssal_storage_link.sql`, `abyssal_storage_invite.sql`) against your characters database
5. Copy the `addon/AbyssalStorage` folder into your WoW `Interface/AddOns` directory
