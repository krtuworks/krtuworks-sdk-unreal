# Changelog

All notable changes to the Krtuworks plugin for Unreal Engine. Versions follow semantic versioning; a breaking change is marked **Breaking**.

## Unreleased

## 1.0.4 - 2026-11-22

### Fixed
- A late `account_merged` answer for a player who already left no longer signs out the player who plays now. Before, when a call made for one account was answered after the player had switched to another, the new player's tokens were dropped and the app was told of a merge that was not theirs. Now only the merged player is signed out; its queues still wait for the merge check.
- A session reports only its own time in use. Before, a session opened after another one (a new player, a session the server closed after 30 minutes) started from the whole launch's time, so its first heartbeat could claim an hour of play the session never had. `SESSION_MINUTES` in rich presence counts the session too.

## 1.0.3 - 2026-11-22

### Fixed
- A player's call is sent only with that player's token. Before, a call refused as expired while the player changed (a sign-in in another tab, a switch of accounts) could be sent again with the new player's token, and a queued save, stat, event or bug report could go out under whoever was signed in when it was sent. Now each call carries the player it was made for, and when the token in hand is another player's nothing is sent (`context_changed`); the work waits for its own player.
- Moving waiting progress to another account (the first sign-in after playing without one, a merge) adds nothing twice when it is cut short (the app closed between its two writes) and run again. The events queue move is safe the same way.
- One waiting save per slot: a newer save takes the place of an older one not sent yet (the older one's call ends with `save_replaced`), and the saves of a slot go in the order they were made, so an older copy never lands after a newer one. Before, every save made offline waited on its own and they went in no set order.

## 1.0.2 - 2026-11-15

### Fixed
- The plugin did not compile on Unreal Engine 5.6 and later: the language lookup used an engine type without its header, and the in-engine tests used an automation flag those versions no longer have.
- A game's own module could not call the C++ API (`krtu::Client`, the types, the validator): the plugin did not export it, so modular builds (the editor on every platform) failed to link. The whole public C++ API is exported now.

## 1.0.1 - 2026-11-15

### Changed
- No change in this package: the SDK packages share one version, and 1.0.1 fixes the Unity package.

## 1.0.0 - 2026-11-15

First release. Unreal Engine 5.3 or later.

### Added
- The Krtuworks game instance subsystem, started with the game from `krtuworks-config.json` next to the `.uproject`; TEST in development builds, LIVE in Shipping builds; the app's type.
- Blueprint nodes for sign-in, saves, progress, sending, remote config, bug reports and the validator, and a C++ core with no dependencies.
- Guests with a device key, Krtuworks ID sign-in on desktops (system browser, loopback return), phones (iOS ASWebAuthenticationSession, Android browser and the app's scheme) and consoles (a code to enter at id.krtuworks.com/link).
- Secrets in the platform's secure storage: Windows DPAPI, macOS Keychain, iOS Keychain (this device only), Android Keystore with a no-backup folder, Linux and Steam Deck sealed to the machine.
- Steam (the KrtuworksSteam plugin): the Steam account's player, each session's licence check, achievements both ways with the Steam accounts the player allowed: the session's Steam account (`platformLink`), `OnPlatformLinkNotice` for a second account waiting for the player's answer (`answerPlatformConsent` Call operation), a third one or another Krtuworks ID's.
- Steam Cloud mirrors the slots the console marks for it (the platform services' `cloud()`; SAVE, PROFILE and SETTINGS, never DEVICE_SETTINGS): the server is the record and Steam holds each slot's current version as `krtuworks-<slot>.bin`; whichever side moved alone goes to the other; SETTINGS take the latest; a copy nothing can order waits for the player (`OnPlatformSaveConflict`, then the `resolveFile` Call operation); a slot Steam has no room for is left alone. The `syncPlatformFiles` Call operation lines them up on demand. Only with the session's Steam account allowed and Steam Cloud on.
- Steam stats mirror the server (the platform services' stat calls): stats with a Steam API name get the server's value, never an increase; the launch's sync imports the player's existing Steam values where the stat's kind allows; values are stored on Steam at most every 2 minutes and on pause or exit. Only with the session's Steam account allowed; achievements unlock on Steam only then too.
- Leaderboards (the `leaderboards` module): the `SubmitScore` node (C++ `SubmitScore`, `Call` operation `submitScore`) keeps a score on the device until the server has it, like a stat (same durable queue, one id per op, a lost answer is sent again unchanged); waiting scores of a board fold by its rule (KEEP_BEST the better, FORCE_UPDATE the last); a score the server refuses for good is dropped, and one sent while the module is off waits. `GetLeaderboards` (the player's own score, details and rank) and `GetLeaderboardEntries` (global or around the player) show Krtuworks usernames. On Steam, a board that takes the platform's row in is read once per board and Steam account (the player's own row only, with the session's Steam account allowed) and sent as a score from the platform; the SDK never writes a score to Steam, the server does.
- Steam rich presence (the `presence` module, games only): the app declares its states on the console and the game says which one is true (`SetPresence` node with a state key and its params, `Call` operation `setPresence`). When nothing is shown the answer is not a failure: `applied` is false with a `reason` (no_platform, not_game, module_disabled, hidden, unknown_state, param_missing, states_unknown or platform_rejected). LEVEL and SESSION_MINUTES fill themselves, `ClearPresence` and `SetPresenceGroup` (Steam's party display) are on the subsystem, and the player's own switch is the `SetPresenceVisible` and `PresenceVisible` nodes (off clears Steam at once and remembers the state).
- DLC ownership (games only): C++ `ownsDlc(key)`, `entitlements()` and `refreshEntitlements()` (`Call` operations `ownsDlc {key}`, `entitlements`, `refreshEntitlements`). The server's answer for this launch comes first (its check of the session's Steam ticket); when the server could not check, the Steam client's own answer (the platform services' `ownership()`); without that, the last server answer kept on the device per player and environment.
- Sessions, events, levels and deaths for games, stats and achievements kept on the device until the server has them, cloud saves with conflict handling, remote config, modules, A/B tests, player profile card.
- Bug reports with a screenshot (`CaptureScreenshot`) and the "fixed" notice; profile picture reports.
- A guest's account deletion, its cancellation and data copy from inside the game.
- The validator (`Validate`) for the release report.
