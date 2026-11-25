# Krtuworks SDK for Unreal Engine

The Krtuworks plugin connects an Unreal Engine game or app (UE 5.3 or later) to Krtuworks: Krtuworks ID sign-in (guest, desktop browser, phone, console code, Steam), sessions, events and dashboards, stats, achievements and leaderboards, cloud saves, remote config and A/B tests, bug reports and the release report. Its core is plain C++ with no dependencies; the engine layer adds a game instance subsystem and Blueprint nodes.

Put this repository at the tag of the version you use (or the zip from [Releases](https://github.com/krtuworks/krtuworks-sdk-unreal/releases)) in your project's `Plugins/Krtuworks` folder and enable the plugin. Save your app's `krtuworks-config.json` (the app's Credentials page in the [partner console](https://partner.krtuworks.com)) next to the `.uproject` file. The SDK starts with the game.

Ready-made screens for the age question, the sign-in button, the account chip and save conflicts, built in Slate: call the `Ensure Player` node (or, from C++, `Subsystem->EnsurePlayer([](const FKrtuworksScreenResult& Result) { ... })`) before the game starts and place the `Krtuworks Sign-in Button` and `Krtuworks Account Chip` widgets in a menu. Theme and sentences are in Project Settings > Plugins > Krtuworks SDK.

For Steam, also add [KrtuworksSteam](https://github.com/krtuworks/krtuworks-sdk-unreal-steam) in the same version.

- Documentation: [partner.krtuworks.com/docs](https://partner.krtuworks.com/docs)
- Coding agents: [`AGENTS.md`](AGENTS.md)
- Changes: [`CHANGELOG.md`](CHANGELOG.md)
- Support: support@krtuworks.com

Licensed under the [Apache License 2.0](LICENSE). The Krtuworks name and logo are not covered by the licence ([`NOTICE`](NOTICE)).
