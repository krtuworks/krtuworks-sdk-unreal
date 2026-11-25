#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Client.h"
#include "KrtuworksCore/Ui.h"
#include "KrtuworksSettings.h"
#include "KrtuworksTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Pipe.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "KrtuworksSubsystem.generated.h"

class FKrtuworksUiSlot;
class IKrtuworksUiHost;
struct FKrtuworksUiLook;

/** What the ready-made screens ended with: the player, the player closing the sign-in, or no online player. */
struct KRTUWORKS_API FKrtuworksScreenResult
{
	/** Someone plays: Player is the guest on this device or the signed-in account. */
	bool bSignedIn = false;
	/** The player closed the sign-in (SignInWithScreens only); they stay who they were. */
	bool bCancelled = false;
	/** With neither of the two: "underage" (under 13) or "unavailable" (no connection, a refusal, the player chose to keep playing); the game goes on offline. */
	FString OfflineReason;
	FKrtuworksPlayer Player;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksIdentityChanged, const FKrtuworksIdentityChange&, Change);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKrtuworksMergeSettled, const FString&, PreviousSubject, int32, WaitingSaves);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksLinkReminder, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksBugFixed, const FKrtuworksBugReport&, Report);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKrtuworksPlatformLinkNotice, const FString&, PlatformAccountId, const FString&, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksPlatformSaveConflictNotice, const FKrtuworksPlatformSaveConflict&, Conflict);

/**
 * The Krtuworks SDK in a game: started with the game instance from krtuworks-config.json (next to the
 * .uproject) and Project Settings > Plugins > Krtuworks SDK. Quick calls (events, levels, stats, config values)
 * are here; calls that talk to the servers are the async Blueprint nodes (Krtuworks|…) or, from C++, Run().
 * Events fire on the game thread.
 */
UCLASS()
class KRTUWORKS_API UKrtuworksSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The player changed: signed_in · guest · signed_out · merged · deletion_pending · sanctioned. */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks")
	FKrtuworksIdentityChanged OnIdentityChanged;

	/** A merged account's waiting progress moved to the signed-in player; its waiting saves need the player's choice. */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks")
	FKrtuworksMergeSettled OnMergeSettled;

	/** A guest has something to lose (first_achievement, first_save, play_time): show your own "link a Krtuworks ID" prompt. */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks")
	FKrtuworksLinkReminder OnLinkReminder;

	/** One of the player's bug reports was fixed since the game last looked: tell the player "fixed in <FixedIn>". */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks|Bugs")
	FKrtuworksBugFixed OnBugFixed;

	/**
	 * The Steam account the session proved is not mirrored and the player should hear why. State is PENDING_CONSENT
	 * (ask the player, then the Call node's answerPlatformConsent {allow}), LIMIT (a game uses at most two Steam
	 * accounts: remove one at id.krtuworks.com/apps) or MISMATCH (the account belongs to another Krtuworks ID).
	 */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks")
	FKrtuworksPlatformLinkNotice OnPlatformLinkNotice;

	/**
	 * Steam Cloud holds a copy of a mirrored SAVE or PROFILE slot that differs from the server's, and nothing tells
	 * which is newer: the game asks the player which to keep and calls the Call node's resolveFile. Told once per
	 * waiting copy and launch; nothing in that slot is mirrored until the player chose.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Krtuworks")
	FKrtuworksPlatformSaveConflictNotice OnPlatformSaveConflict;

	/** A line for the bug reports' log (kept in memory, the last 32 KB; sent only inside a report the player makes). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Bugs")
	void Log(const FString& Message);

	/** The SDK started (krtuworks-config.json read and valid). */
	UFUNCTION(BlueprintPure, Category = "Krtuworks")
	bool IsReady() const;

	UFUNCTION(BlueprintPure, Category = "Krtuworks")
	FKrtuworksPlayer GetPlayer() const;

	/** TEST or LIVE. */
	UFUNCTION(BlueprintPure, Category = "Krtuworks")
	FString GetEnvironment() const;

	/** GAME, MOBILE_APP, WEB_APP or DESKTOP_APP (levels, deaths and Steam are a game's). */
	UFUNCTION(BlueprintPure, Category = "Krtuworks")
	FString GetAppType() const;

	/**
	 * Records an event: a ready-made family (screen_view, button, economy, item, performance, settings …) or the
	 * app's own type. False when nothing was kept: its module is off, the owner switched this build's statistics
	 * off, or the player turned statistics off.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Events")
	bool Track(const FString& Type, const FKrtuworksEventData& Data);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Events")
	bool TrackAt(const FString& Type, const FKrtuworksEventData& Data, const FKrtuworksPosition& Position);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelStart(const FKrtuworksLevel& Level);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelComplete(bool bHasScore, double Score);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelFail(bool bHasScore, double Score);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelQuit();

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelCheckpoint(const FString& Name);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void LevelRestart();

	/** A death, counted toward the level in play. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Levels")
	void Death(const FString& Cause, const FString& Enemy, bool bHasPosition, const FKrtuworksPosition& Position);

	/** An error the game caught (Kind: exception, crash, assert, other). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Events")
	void ReportError(const FString& Message, const FString& Kind = TEXT("exception"), const FString& Fingerprint = TEXT(""), bool bHandled = true);

	/** Adds to a SUM stat: kept on the device until the server has it, never lost, never counted twice. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Progress")
	void IncrementStat(const FString& Key, double By = 1);

	/** Reports a value to a MAX, MIN or LATEST stat. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Progress")
	void SetStat(const FString& Key, double Value);

	/** Unlocks an achievement here and on the platform (Steam) when its id there is known. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Progress")
	void UnlockAchievement(const FString& Key);

	/**
	 * Submits a score to a leaderboard without waiting (C++; Blueprints use the Submit Score node, which also says when
	 * the input is invalid): kept on the device until the server has it, never lost, never counted twice. Details are
	 * empty (none) or 1 to 64 bytes; an invalid score is logged and dropped.
	 */
	void SubmitScore(const FString& Key, int64 Value, const TArray<uint8>& Details);

	/**
	 * Clears what Steam shows about the player (rich presence) and forgets the state, without waiting. The Set Presence
	 * node sets it. The group (SetPresenceGroup) stays.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Presence")
	void ClearPresence();

	/**
	 * The group the player is in (Steam's join and party display), written with every presence state and kept across
	 * states. An empty GroupId leaves the group; Size 0 or less means none. An invalid group is logged and dropped.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Presence")
	void SetPresenceGroup(const FString& GroupId, int32 Size = 0);

	/** The owner's gates for this build (maintenance, a required update, a switch). */
	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	FKrtuworksGate GetGate() const;

	/** The SDK modules and whether each is on. */
	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	TMap<FString, bool> GetModuleStates() const;

	/** A remote config value (the player's A/B group's when one varies it), or Fallback. */
	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	FString GetConfigString(const FString& Key, const FString& Fallback) const;

	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	double GetConfigNumber(const FString& Key, double Fallback) const;

	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	bool GetConfigBool(const FString& Key, bool Fallback) const;

	/** The value as JSON text ("null" when the owner set none). */
	UFUNCTION(BlueprintPure, Category = "Krtuworks|Config")
	FString GetConfigJson(const FString& Key) const;

	/** The core client for C++ (nullptr before IsReady()); its calls block, so make them through Run(). */
	krtu::Client* GetClient() const { return Client.get(); }

	/**
	 * Runs `Work` with the client on a thread of its own (the call may wait on the network or on the player in
	 * a browser, so it never holds one of the engine's task workers), then `OnSuccess` or `OnFailure` on the game
	 * thread. The Blueprint nodes use it; game C++ code can too.
	 */
	template <class TResult>
	void Run(TFunction<TResult(krtu::Client&)> Work, TFunction<void(const TResult&)> OnSuccess, TFunction<void(const FKrtuworksError&)> OnFailure);

	/**
	 * Run() without its result type: `Work` on a thread of its own, then `OnDone` or `OnFailure` on the game
	 * thread. The failure is caught here, inside the plugin, so a game module that calls Run() needs no exceptions.
	 */
	void RunErased(TFunction<void(krtu::Client&)> Work, TFunction<void()> OnDone, TFunction<void(const FKrtuworksError&)> OnFailure);

	/**
	 * Set by the Steam plugin (KrtuworksSteam) when it loads; asked when the game instance starts, by which time
	 * Steam is up. Its services make the platform STEAM; the core uses them for games only.
	 */
	static void SetPlatformServicesFactory(TFunction<std::shared_ptr<krtu::PlatformServices>()> Factory);

	/**
	 * The ready-made screens: makes sure someone is playing. A new guest gets the age screen once, an outdated terms
	 * version the terms screen, a refusal the error screen; "Sign in with Krtuworks ID" on the age screen runs
	 * SignInWithScreens. Never fails: without an online player the result says so and the game keeps going offline.
	 * `OnDone` runs on the game thread.
	 */
	void EnsurePlayer(TFunction<void(const FKrtuworksScreenResult&)> OnDone);

	/**
	 * Signs the player in with Krtuworks ID with the ready-made screens: the system browser with a waiting screen
	 * (desktops), the system sheet (iOS, Android), a code and a QR code (consoles), as Project Settings > Plugins >
	 * Krtuworks SDK says. `Cancel` stops it from outside. `OnDone` runs on the game thread.
	 */
	void SignInWithScreens(const krtu::CancelToken& Cancel, TFunction<void(const FKrtuworksScreenResult&)> OnDone);

	/** Whether the ready-made screens offer signing in (not with the sign-in mode None). */
	bool IsSignInOffered() const;

	/**
	 * The ready-made screens' controller for C++ (empty before IsReady()): its calls block, so make them off the game
	 * thread (Async, RunErased). Saves with the conflict screen (save), merged accounts' saves (resolveMergedSaves),
	 * the link reminder (remind) and Steam Cloud's conflict (resolvePlatformSaveConflict) are there.
	 */
	std::shared_ptr<krtu::UiController> GetUi() const;

	/** The screens' look and sentences, for the module's own widgets. */
	std::shared_ptr<FKrtuworksUiLook> GetUiLook() const { return UiLook; }

	/** The theme of the screens, from now on (it starts as Project Settings'). Screens already open keep theirs. */
	void SetUiTheme(const FKrtuworksUiTheme& Theme);

	/** A sentence of the screens, from now on (the texts start as Project Settings' overrides). An empty text brings the English one back. */
	void SetUiText(FName Key, const FText& Text);

	/**
	 * Reads a save's bytes into what its card on the conflict screen shows (chapter, play time, a picture): runs off
	 * the game thread; may return nothing.
	 */
	void SetSaveDescriber(std::function<std::optional<krtu::SaveSummary>(const std::string& Slot, const std::string& Bytes)> Describer);

	/**
	 * The Krtuworks validator (the Validate node): checks this build's setup (krtuworks-config.json, the platform,
	 * Steam's services), then runs a TEST round against the servers with the game's own config, build and stores,
	 * logs each check and hands the report on the game thread. The build's release report turns green with it.
	 */
	void Validate(TFunction<void(const FKrtuworksValidationReport&)> OnDone);

private:
	void EnqueueWrite(TFunction<void(krtu::Client&)> Write);
	void BuildUi();
	void RunUi(TFunction<krtu::UiResult(krtu::UiController&)> Work, TFunction<void(const FKrtuworksScreenResult&)> OnDone);
	void OnEnterBackground();
	void OnEnterForeground();

	std::shared_ptr<krtu::Client> Client;
	/** The game's options as the validator runs them (in TEST); what was known when the config was missing or bad. */
	std::optional<krtu::Options> ValidatorOptions;
	/** Writes to the device's queues (levels, stats, unlocks) one at a time, in the order the game made them. */
	TUniquePtr<UE::Tasks::FPipe> Writes;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	/** The screens' look and sentences, the host that draws them and the controller (replaced when a sentence changes). */
	std::shared_ptr<FKrtuworksUiLook> UiLook;
	std::shared_ptr<IKrtuworksUiHost> UiHost;
	std::shared_ptr<FKrtuworksUiSlot> UiSlot;
	std::shared_ptr<krtu::KeyValueStore> UiStore;
};

template <class TResult>
void UKrtuworksSubsystem::Run(TFunction<TResult(krtu::Client&)> Work, TFunction<void(const TResult&)> OnSuccess, TFunction<void(const FKrtuworksError&)> OnFailure)
{
	const TSharedRef<TOptional<TResult>, ESPMode::ThreadSafe> Result = MakeShared<TOptional<TResult>, ESPMode::ThreadSafe>();
	RunErased([Work = MoveTemp(Work), Result](krtu::Client& C) { Result->Emplace(Work(C)); }, [Result, OnSuccess = MoveTemp(OnSuccess)]() { OnSuccess(Result->GetValue()); }, MoveTemp(OnFailure));
}
