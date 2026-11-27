# Changelog

All notable changes to the Krtuworks plugin for Unreal Engine. Versions follow semantic versioning; a breaking change is marked **Breaking**.

## Unreleased

## 1.0.13 - 2026-11-27

### Fixed
- Switching the `sessions` module on or off takes effect on the next config read, with no new build: on opens the player's session, off ends it and its heartbeats.

## 1.0.12 - 2026-11-26

### Changed
- No change in this package: the SDK packages share one version, and this one fixes the JavaScript SDK in the browser.

## 1.0.11 - 2026-11-26

### Added
- `samplePosition` (C++, Blueprint `Sample Position`): the game reports where the player is as often as it likes; at most one `position_sample` per `PositionSampleSeconds` (default 10) of active play goes out with the seconds since the one before, for the time heat map.

### Fixed
- The `position_sample` event belongs to its own module switch, `positions`, as on the server, and only a game sends it. Before, the SDK counted it as one of the app's own events: with `customEvents` off it was dropped, and with `customEvents` on it was sent even while the server's switch was off.

## 1.0.10 - 2026-11-26

### Fixed
- Phone sign-in on Android: a late return from an earlier sign-in is no longer taken as the answer to the sign-in now waiting. Before, only the app's return scheme was checked, so the earlier sign-in's return could complete the new one with the wrong result.

## 1.0.9 - 2026-11-26

### Fixed
- An A/B group's value (`value`) is used only while the `abTests` module is on; switching the module off now gives the config's own value.
- A new player starts without the last player's A/B values and statistics choice (an answer that arrives after the player changed is not applied); before, `value` and `track` kept the previous player's until fresh answers came.
- `shutdown()` while a session open is still on its way: the late answer no longer brings the session back, and the session it names is ended.

## 1.0.8 - 2026-11-26

### Fixed
- A queued write is never dropped while the refusal can lift. An error from getting or refreshing the player's token (for example a frozen app answering `app_suspended` to the refresh) and the codes `app_suspended`, `account_merged`, `account_merging`, `sanctioned`, `account_deletion_pending`, `switched_off` and `module_disabled` keep the event batch, the bug report and its screenshot, and the save upload on the device; the next try waits for Retry-After, else 15 minutes. Before, a refused refresh deleted them. Only a refusal of the body itself drops it.
- `account_deleted` (401) signs the player out (`signed_out`, reason `account_deleted`) and drops that player's queued events, progress, bug reports and saves.
- A 200 answer for the remote config that is not a JSON object (a captive portal's page) is a failed fetch: the kept copy stays and nothing is stored.
- A 409 `app_transferring` or `account_merging` on a bug report's screenshot keeps the screenshot for a later try. A stored screenshot that cannot be read is let go and the report is sent without it; an unexpected error in the report flush no longer stops the events after it.
- A failed session open is tried again (growing wait, at once when the network is back), never given up for the rest of the run.
- An open level attempt belongs to its player: after a player change `levelComplete`, `levelFail`, `levelCheckpoint` and `death` no longer count under the new player.
- A score refused with `module_disabled` is parked for the rest of the flush instead of being sent again every round.
- Events the store could not take stay in memory until it can; the event age limit and the Retry-After date use the server's clock (the offset the SDK keeps), not the device clock; the conflict screen's "Newer" mark too.
- The Steam Cloud sync is bound to the player it started for: a player change mid-sync sends nothing under the new player.
- A Steam ticket that was asked for and never sent (the call failed before it left) is cancelled. A Steam guest on a new PC is found by the ticket without the age screen; the server asks for the birth date only for a new Steam account.
- A bug in the guest sign-in: the stored age answers are written under the lock; an age the server refused for good (`too_young`, `invalid_birth_date`) is no longer retried at every launch, and newer accepted terms replace the stored ones. Stale sign-in records are swept; a sign-out by another instance is reported.
- A replayed `file_conflict` carries the slot's current version (`details()["current"]`), so the choice screen can open.
- On shutdown the events kept in memory are written first, before waiting for the background loop.
- Android: a device with no browser no longer crashes the game when sign-in opens the browser; the sign-in ends with `browser_unavailable`. A build from a clean checkout stops when `AppSlug` is missing from `DefaultGame.ini` instead of shipping a sign-in that cannot return.
- iOS: a Keychain delete that fails is reported, so a sign-out cannot leave the tokens behind unnoticed.

### Added
- `IdentityChange` types `merging` and `merging_ended`: told once when the server is merging the player's account and once when calls go through again.
- Settings saves (`saveSettings`) are written at most once every 30 seconds per slot; the newest content wins and goes when the app pauses or stops (`FileVersion::queued` while it waits). A `429 too_many_writes` is retried after Retry-After.
- `firstLaunch: "online"` in `krtuworks-config.json`: `Gate::firstLaunchOffline` is set while the very first launch cannot reach the server, and `EnsurePlayer` shows "connect to play" with Try again.
- `Client::serverOffsetMs()`, `PlatformServices::cancelWebApiTicket`, `Error::authLayer()` and `Error::waitsForLater()`.

## 1.0.7 - 2026-11-25

### Fixed
- A bug report carries only its own player's recent events, log lines and frame rate. Before, after a switch to another account in the same run, the new player's report took the previous player's events and log lines too. What was recorded before anyone signed in goes to the first player, like the events queue.
- A different player signed in by another tab or instance sharing the same storage is now taken up like a sign-in here: the identity change is reported and a session opens for that player. Before, the SDK switched to that player silently. A plain token refresh by another tab stays silent.
- The log a bug report attaches is now also limited by its size in the request, so a log full of control characters can no longer push a report over the server's size limit.
- On iOS, a second sign-in started while the first was still open ends the first one as cancelled. Before, the first call could wait forever, and its later cancel could close the second sign-in's page.

## 1.0.6 - 2026-11-25

### Fixed
- The save screens answer only for the player who opened them. Before, when the player signed out or another account signed in while the "which save to keep" screen was open, the choice and the save could reach the new account. Now `save`, `chooseSave` and `resolveMergedSaves` end with `context_changed` and nothing is saved, and the Steam Cloud screen is dropped: the copy waits for its own player.
- On Android, a key kept unsealed because the Android Keystore failed once stayed unsealed for good. Now every launch seals the same key again once the Keystore works, and the saved sign-in stays. Before Android 6.0 there is no Keystore and the key stays as it was.
- An error while applying a Steam Cloud save choice no longer ends the game; it is written to the log.

## 1.0.5 - 2026-11-25

### Added
- Ready-made screens, built in Slate (no assets to import): the `Ensure Player` node (C++ `UKrtuworksSubsystem::EnsurePlayer`) shows the age and terms screen before a new guest, a screen for players under 13 (the game goes on offline and the screen is not shown again until their 13th birthday), the screen for newer terms, and an error screen with Try again and Keep playing. Signing in (`Sign In (Ready-made Screens)`, or "Sign in with Krtuworks ID" on the age screen) follows the platform: the system browser with a waiting screen (Open the page again, Cancel) on desktops, the system sheet on iOS and Android, a code, the address and a QR code on consoles (a new code replaces the old on the same screen; an expired code asks for a new one). `GetUi()->save(...)` shows both saves when another device saved first and does what the player picks (`SetSaveDescriber` adds the game's own chapter, play time or picture to each card), and the same screen asks which save to keep when Steam Cloud holds a different copy. A guest gets the "link a Krtuworks ID" prompt in a corner when the SDK says it is time. The `Krtuworks Sign-in Button` and `Krtuworks Account Chip` widgets go in any UMG layout. Every screen works with a gamepad and a keyboard (focus, Accept, Back). Colours, corners and font are `Theme` in Project Settings > Plugins > Krtuworks SDK (`SetUiTheme` at runtime), every sentence can be replaced (`TextOverrides`, localizable), and a game that draws its own screens gets the same models through `krtu::UiController` and its own `krtu::UiHost`.

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
