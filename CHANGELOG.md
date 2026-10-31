# Changelog

All notable changes to this module are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has no version tags yet, so entries are grouped by date.

## [Unreleased]

### Added
- Several accounts can share one vault. GMs link them with `.abs link <account> <mainAccount>` and undo it with `.abs unlink <account>`. Run `data/sql/db-characters/abyssal_storage_link.sql` to create the new table.
- Players can share a vault without a GM: `.abs share invite <character>`, then `.abs share accept` on the invited account. `leave`, `kick`, `decline` and `list` are also available. Turn it off with `AbyssalStorage.EnableSharing = 0`. Run `data/sql/db-characters/abyssal_storage_invite.sql` to create the invite table.
- Vault changes show up right away in the addon for every online character sharing the vault.
- Item tooltips everywhere (bags, chat links, loot, auction house, professions) show an "Abyssal Storage" line with the vault count. Update the addon to get it.
- Accepting a quest pulls the items it needs out of the vault (e.g. Murloc Fins), so the quest counts them right away.

### Fixed
- Logging out one character no longer drops the vault from memory while other characters using it are still online.
- Random playerbot accounts (`rndbot*`) are ignored, and the check no longer queries the database on every update.

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
