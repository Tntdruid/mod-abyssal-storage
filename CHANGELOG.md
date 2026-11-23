# Changelog

All notable changes to this module are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Releases from 1.0.0 on follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html); older entries are grouped by date.

## [Unreleased]

## [2.1.1] - 2026-11-23

### Fixed
- Items missing from the client cache (for example after deleting the WoW `Cache` folder) no longer stay as question marks in the vault and rules windows. The addon asks the server for them a few at a time and fills them in as they arrive. Update the addon to get the fix.

## [2.1.0] - 2026-11-21

### Added
- Adding a ranked item such as Scroll of Spirit IV to the Store or Keep list asks whether to add every rank or only that one. The server commands take `all` for the same: `.ps rule store|keep|remove <itemId> all`. Update the addon to get the question.

## [2.0.0] - 2026-11-21

### Changed
- **Breaking:** the module is renamed from Abyssal Storage to Pocket Storage. Module folder `mod-pocket-storage`, config file `mod_pocket_storage.conf` with `PocketStorage.*` keys, tables `pocket_storage*`, addon folder `PocketStorage`, server commands `.ps` / `.pocket` and addon commands `/ps` / `/pocket`. To upgrade:
  1. Move the module to `modules/mod-pocket-storage` and re-run CMake. The old folder name no longer builds, because AzerothCore derives the module's loader function from it.
  2. Copy `mod_pocket_storage.conf.dist` to `mod_pocket_storage.conf` and carry over your settings under the new `PocketStorage.*` names.
  3. Start the server. It renames the existing `abyssal_storage*` tables to `pocket_storage*` once, so no vault data is lost.
  4. Replace the `AbyssalStorage` addon with the `PocketStorage` folder on every client. The old addon no longer talks to the server.

### Added
- A chat line such as "Stored [Linen Cloth] x2 (92 in storage)." each time looted items are stored automatically. Turn it off with `PocketStorage.DepositMessage = 0`.

## [1.0.0] - 2026-11-21

### Added
- `AbyssalStorage.ExtraItems` stores extra items by id, on top of trade goods and gems. It defaults to Lesser Bloodstone Ore (4278), which the game files as a quest item.
- Several accounts can share one vault. GMs link them with `.abs link <account> <mainAccount>` and undo it with `.abs unlink <account>`. Run `data/sql/db-characters/abyssal_storage_link.sql` to create the new table.
- Players can share a vault without a GM: `.abs share invite <character>`, then `.abs share accept` on the invited account. `leave`, `kick`, `decline` and `list` are also available. Turn it off with `AbyssalStorage.EnableSharing = 0`. Run `data/sql/db-characters/abyssal_storage_invite.sql` to create the invite table.
- Vault changes show up right away in the addon for every online character sharing the vault.
- Item tooltips everywhere (bags, chat links, loot, auction house, professions) show an "Abyssal Storage" line with the vault count. Update the addon to get it.
- Accepting a quest pulls the items it needs out of the vault (e.g. Murloc Fins), so the quest counts them right away.
- Store and Keep lists per account, edited from a new rules window in the addon (`/abs rules` or the **Rules** button): Store vaults any stackable item, Keep never vaults an item. The window also turns auto-store on loot off for the account. Run `data/sql/db-characters/abyssal_storage_rule.sql` to create the new tables, and update the addon.

### Fixed
- Logging out one character no longer drops the vault from memory while other characters using it are still online.
- Random playerbot accounts (`rndbot*`) are ignored, and the check no longer queries the database on every update.
- Random playerbot accounts with a custom prefix (such as `botr2`) are ignored too. The prefix follows mod-playerbots' `Playerbots.RandomBotAccountPrefix` or can be set with `AbyssalStorage.RandomBotAccountPrefix`.

## 2026-10-25

### Changed
- Profession tools (items with a totem category, such as runed enchanting rods) are no longer stored. They stay in your bags so spells that require them keep working.

## 2026-03-14

### Fixed
- Excess quest items are deposited again, while the stack your active quests need stays in your bags.

## 2026-02-20

### Changed
- Quest items are ignored entirely until the quest that needs them is done.

## 2026-02-19

### Fixed
- An infinite loop when adding the last quest item.
- Broken addon message processing.

## 2026-02-17

### Added
- Support for gems.

### Changed
- Revamped the UI to make it more intuitive.

## 2026-02-16

### Added
- First release: an account-wide interdimensional vault for trade goods.
- README.

### Fixed
- "Create All" now crafts at most one full stack.
- Item materialization logic.
- The "Missing Reagent" error message is no longer shown in the UI.

[Unreleased]: https://github.com/Tntdruid/mod-pocket-storage/compare/v2.1.1...HEAD
[2.1.1]: https://github.com/Tntdruid/mod-pocket-storage/compare/v2.1.0...v2.1.1
[2.1.0]: https://github.com/Tntdruid/mod-pocket-storage/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/Tntdruid/mod-pocket-storage/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/Tntdruid/mod-pocket-storage/releases/tag/v1.0.0
