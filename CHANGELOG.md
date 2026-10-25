# Changelog

All notable changes to this module are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has no version tags yet, so entries are grouped by date.

## [Unreleased]

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
