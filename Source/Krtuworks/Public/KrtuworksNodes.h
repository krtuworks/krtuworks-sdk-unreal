#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "KrtuworksCore/Error.h"
#include "KrtuworksTypes.h"

#include "KrtuworksNodes.generated.h"

class UKrtuworksSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksFailed, const FKrtuworksError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKrtuworksDone);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksPlayerReady, const FKrtuworksPlayer&, Player);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksScreensOffline, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksCodeReady, const FKrtuworksDeviceLink&, Link);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksFileSaved, const FKrtuworksFileVersion&, Version);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksFileReadDone, const FKrtuworksFileRead&, File);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksProgressReady, const FKrtuworksProgress&, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksLeaderboardsReady, const TArray<FKrtuworksLeaderboard>&, Leaderboards);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksLeaderboardPageReady, const FKrtuworksLeaderboardPage&, Page);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksJsonReady, const FString&, ResultJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKrtuworksPresenceSet, bool, bApplied, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FKrtuworksPresenceVisibleReady, bool, bKnown, bool, bVisible);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksBytesReady, const TArray<uint8>&, Bytes);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksBugReported, const FString&, ReportId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksBugReportsReady, const TArray<FKrtuworksBugReport>&, Reports);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKrtuworksValidated, const FKrtuworksValidationReport&, Report);

/** What every Krtuworks node shares: the subsystem it works through and the failure pin. */
UCLASS(Abstract)
class KRTUWORKS_API UKrtuworksNode : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksFailed OnFailure;

protected:
	/** Binds the node to the game instance's subsystem (and keeps the node alive until it finishes). */
	void Attach(const UObject* WorldContextObject);
	/** The failure pin, then the node is done. */
	void Fail(const FKrtuworksError& Error);
	/** No subsystem (no game instance, or the SDK did not start): the failure pin with not_ready. */
	bool FailIfNotReady();

	TWeakObjectPtr<UKrtuworksSubsystem> Subsystem;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSignInAsGuest : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPlayerReady OnSuccess;

	/**
	 * Plays as a guest on this device (on Steam, as the Steam account's player). A new guest needs the age
	 * screen's answers: the birth date (yyyy-MM-dd, 13 or older) and AcceptedTerms (empty = the terms this SDK
	 * shows, TERMS:v1 PRIVACY:v1). Leave both empty for a device that already played.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSignInAsGuest* SignInAsGuest(const UObject* WorldContextObject, const FString& BirthDate, const FString& AcceptedTerms);

	virtual void Activate() override;

private:
	FString BirthDate;
	FString AcceptedTerms;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSignIn : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPlayerReady OnSuccess;

	/**
	 * Signs the player in with Krtuworks ID the way the platform does it: the system browser on desktops, the
	 * phone's own sign-in screen on iOS and Android. A guest playing here is carried along. Consoles use
	 * Sign In With Code.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSignIn* SignIn(const UObject* WorldContextObject);

	/** Stops waiting for the browser (OnFailure with cancelled). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in")
	void Cancel();

	virtual void Activate() override;

private:
	krtu::CancelToken CancelToken;
};

/** The ready-made screens: makes sure someone is playing. */
UCLASS()
class KRTUWORKS_API UKrtuworksEnsurePlayer : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	/** Someone plays: the guest on this device or the signed-in account. */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPlayerReady OnSignedIn;

	/** No online player; the game goes on offline. Reason is "underage" (under 13) or "unavailable" (no connection, a refusal, the player chose to keep playing). */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksScreensOffline OnOffline;

	/**
	 * Makes sure someone is playing, with the SDK's own screens: a new guest gets the age screen once, an outdated terms
	 * version the terms screen, a refusal the error screen; the age screen's "Sign in with Krtuworks ID" signs in on
	 * this platform's way (browser, system sheet, or a code on a console). Never fails: without an online player
	 * OnOffline says why. Colours, corners, font and every sentence are in Project Settings > Plugins > Krtuworks SDK.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Screens", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Ensure Player"))
	static UKrtuworksEnsurePlayer* EnsurePlayer(const UObject* WorldContextObject);

	virtual void Activate() override;
};

/** The ready-made screens: signs the player in with Krtuworks ID. */
UCLASS()
class KRTUWORKS_API UKrtuworksSignInWithScreens : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPlayerReady OnSignedIn;

	/** The player closed the sign-in; they stay who they were. */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnCancelled;

	/** Signing in is not offered (sign-in mode None) or it failed (the error screen was shown). */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksScreensOffline OnOffline;

	/**
	 * Signs the player in with Krtuworks ID with the SDK's own screens: the system browser with a waiting screen on
	 * desktops, the system sheet on iOS and Android, a code and a QR code on consoles. A guest playing here is carried
	 * along.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Screens", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Sign In (Ready-made Screens)"))
	static UKrtuworksSignInWithScreens* SignInWithScreens(const UObject* WorldContextObject);

	/** Stops the sign-in (OnCancelled). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Screens")
	void Cancel();

	virtual void Activate() override;

private:
	krtu::CancelToken CancelToken;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSignInWithCode : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	/** Show the code and the address (or a QR code of the complete address) to the player. */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksCodeReady OnCode;

	UPROPERTY(BlueprintAssignable)
	FKrtuworksPlayerReady OnSuccess;

	/** A console's or TV's sign-in: the player approves a code on their phone or computer at id.krtuworks.com/link. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSignInWithCode* SignInWithCode(const UObject* WorldContextObject, const FString& DeviceName);

	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in")
	void Cancel();

	virtual void Activate() override;

private:
	FString DeviceName;
	krtu::CancelToken CancelToken;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSignOut : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnSuccess;

	/** Forgets the player on this device (a guest's key stays: signing in as the guest again reaches it). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Sign-in", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSignOut* SignOut(const UObject* WorldContextObject);

	virtual void Activate() override;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSaveFile : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksFileSaved OnSuccess;

	/**
	 * Saves a new version of a SAVE or PROFILE slot over BaseVersion (the version the game read; empty for an
	 * empty slot). OnFailure with file_conflict when another device saved first: read the slot again and let
	 * the player choose.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Files", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSaveFile* SaveFile(const UObject* WorldContextObject, const FString& Slot, const TArray<uint8>& Bytes, const FString& BaseVersion);

	virtual void Activate() override;

private:
	FString Slot;
	TArray<uint8> Bytes;
	FString BaseVersion;
};

UCLASS()
class KRTUWORKS_API UKrtuworksReadFile : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksFileReadDone OnSuccess;

	/** A slot's current version; with HeldVersion (a version id the game holds) nothing is read when it is still current. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Files", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksReadFile* ReadFile(const UObject* WorldContextObject, const FString& Slot, const FString& HeldVersion);

	virtual void Activate() override;

private:
	FString Slot;
	FString HeldVersion;
};

UCLASS()
class KRTUWORKS_API UKrtuworksGetProgress : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksProgressReady OnSuccess;

	/** The app's stats and achievements with the player's values. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Progress", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksGetProgress* GetProgress(const UObject* WorldContextObject);

	virtual void Activate() override;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSubmitScore : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnSuccess;

	/**
	 * Submits a score to a leaderboard. OnSuccess means the device has it (kept until the server does: never
	 * lost, never counted twice; the board's own rule decides what it does to the stored score). Details are 0
	 * bytes (none) or 1 to 64 bytes the game keeps with the score. OnFailure (invalid_argument) for a key that is
	 * none, a score of 10^15 or more, or details of another size.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Leaderboards", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSubmitScore* SubmitScore(const UObject* WorldContextObject, const FString& Key, int64 Value, const TArray<uint8>& Details);

	virtual void Activate() override;

private:
	FString Key;
	int64 Value = 0;
	TArray<uint8> Details;
};

UCLASS()
class KRTUWORKS_API UKrtuworksGetLeaderboards : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksLeaderboardsReady OnSuccess;

	/** The app's leaderboards with the player's own score and rank, named in Locale (empty = the player's language). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Leaderboards", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksGetLeaderboards* GetLeaderboards(const UObject* WorldContextObject, const FString& Locale);

	virtual void Activate() override;

private:
	FString Locale;
};

UCLASS()
class KRTUWORKS_API UKrtuworksGetLeaderboardEntries : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksLeaderboardPageReady OnSuccess;

	/**
	 * A leaderboard's rows in its order. Scope: global (from Offset) or around (the page centred on the player,
	 * Offset 0; a player with no score gets the top). Limit is 1 to 100. Names are Krtuworks usernames.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Leaderboards", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksGetLeaderboardEntries* GetLeaderboardEntries(const UObject* WorldContextObject, const FString& Key, const FString& Scope = TEXT("global"), int32 Limit = 25, int32 Offset = 0);

	virtual void Activate() override;

private:
	FString Key;
	FString Scope;
	int32 Limit = 25;
	int32 Offset = 0;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSetPresence : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	/** Done: bApplied says whether Steam now shows the state; when not, Reason says why. */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPresenceSet OnSuccess;

	/**
	 * Tells Steam what the player is doing: Key is one of the states the app declared, Params the values of the params
	 * that state's line uses. LEVEL and SESSION_MINUTES fill themselves when the game gives none, and a state that uses
	 * one is written again on every heartbeat. A refusal is not a failure: OnSuccess with bApplied false and Reason
	 * no_platform, not_game, module_disabled, hidden (the player turned presence off), unknown_state, param_missing,
	 * states_unknown or platform_rejected.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Presence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSetPresence* SetPresence(const UObject* WorldContextObject, const FString& Key, const TMap<FString, FString>& Params);

	virtual void Activate() override;

private:
	FString Key;
	TMap<FString, FString> Params;
};

UCLASS()
class KRTUWORKS_API UKrtuworksSetPresenceVisible : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnSuccess;

	/**
	 * The player's own switch for showing presence to friends. Off clears Steam's keys at once and keeps the state the
	 * game wanted; on writes it again. OnFailure (module_disabled) while the owner has the presence module off.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Presence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksSetPresenceVisible* SetPresenceVisible(const UObject* WorldContextObject, bool bVisible);

	virtual void Activate() override;

private:
	bool bVisible = true;
};

UCLASS()
class KRTUWORKS_API UKrtuworksPresenceVisible : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	/** bKnown is false until the server has said (or when the app is not a game); then bVisible is true. */
	UPROPERTY(BlueprintAssignable)
	FKrtuworksPresenceVisibleReady OnSuccess;

	/** The player's switch for showing presence, as the server last said. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Presence", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksPresenceVisible* PresenceVisible(const UObject* WorldContextObject);

	virtual void Activate() override;
};

UCLASS()
class KRTUWORKS_API UKrtuworksFlush : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnSuccess;

	/** Sends what waits now (progress first, then events); the SDK also does it on its own every 30 seconds. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Events", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksFlush* Flush(const UObject* WorldContextObject);

	virtual void Activate() override;
};

UCLASS()
class KRTUWORKS_API UKrtuworksRefreshConfig : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksDone OnSuccess;

	/** Asks for the owner's latest remote config now (the SDK also does it every 5 minutes); then read Get Gate and the Get Config values. */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Config", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksRefreshConfig* RefreshConfig(const UObject* WorldContextObject);

	virtual void Activate() override;
};

UCLASS()
class KRTUWORKS_API UKrtuworksCaptureScreenshot : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksBytesReady OnSuccess;

	/**
	 * A picture of the screen as the player sees it now (a JPEG, quality 1-100), for Report Bug's screenshot:
	 * take it before opening the report form, and let the player leave it out. Fails (`no_screenshot`) when the
	 * engine draws nothing (a server, -nullrhi) or gives no picture within 3 seconds.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Bugs", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksCaptureScreenshot* CaptureScreenshot(const UObject* WorldContextObject, int32 Quality = 75);

	virtual void Activate() override;

private:
	int32 Quality = 75;
};

UCLASS()
class KRTUWORKS_API UKrtuworksReportBug : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksBugReported OnSuccess;

	/**
	 * A bug report from the player: Category is one of CRASH, GAMEPLAY, GRAPHICS, AUDIO, PERFORMANCE, CONTROLS,
	 * TEXT, OTHER; Description what the player wrote (1-4000 characters); Screenshot the bytes from Capture
	 * Screenshot (empty = none); Level empty = the level in play. The SDK adds the platform, build, device,
	 * language, frame rate, the last 50 events and the log lines, keeps the report on the device and sends it
	 * when it can (OnSuccess gives the report's id on this device). Fails with module_disabled when the owner
	 * turned bug reports off, too_many_reports when 20 wait on the device, invalid_argument for input that can
	 * never be taken.
	 */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Bugs", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksReportBug* ReportBug(const UObject* WorldContextObject, const FString& Category, const FString& Description, const TArray<uint8>& Screenshot, const FString& Level);

	virtual void Activate() override;

private:
	FString Category;
	FString Description;
	TArray<uint8> Screenshot;
	FString Level;
};

UCLASS()
class KRTUWORKS_API UKrtuworksMyBugReports : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksBugReportsReady OnSuccess;

	/** The player's bug reports in this app, newest first, with their status and the build a fix shipped in (fixes not told yet go to On Bug Fixed). */
	UFUNCTION(BlueprintCallable, Category = "Krtuworks|Bugs", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksMyBugReports* MyBugReports(const UObject* WorldContextObject);

	virtual void Activate() override;
};

/**
 * The Krtuworks validator in one node (Play In Editor or a development build): checks this build's setup
 * (krtuworks-config.json, the platform, Steam's services), then runs a TEST round against the servers with the
 * game's own config, build and stores: a guest signs in, a session, an event, a save written and read back, a
 * progress operation, a test bug report, the remote config read. Each check goes to the log; the build's release
 * report on the partner console turns green with it, and a red build is not released in LIVE. Runs even when the
 * SDK did not start (the report says why).
 */
UCLASS()
class KRTUWORKS_API UKrtuworksValidate : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksValidated OnSuccess;

	UFUNCTION(BlueprintCallable, Category = "Krtuworks", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksValidate* Validate(const UObject* WorldContextObject);

	virtual void Activate() override;
};

/**
 * Every other server call, by name, with JSON arguments and a JSON result:
 * listFiles · saveSettings {slot, base64, device?: {id, info}} · resolveFile {slot, keep, current} ·
 * waitingSaves {owner} · adoptWaitingSave {uploadId, baseVersion?} · discardWaitingSave {uploadId} ·
 * setPresence {key, params?: {NAME: text}} (answers {applied, reason}) · clearPresence · setPresenceVisible {visible} ·
 * ownsDlc {key} (a DLC's product key; answers {owns}) · entitlements (the kept licences, answers {entitlements}) · refreshEntitlements
 * (asks the server now, same answer) ·
 * submitScore {key, value, details?: base64} · getLeaderboards {locale?} (answers {leaderboards}) · getLeaderboardEntries
 * {key, scope?, limit?, offset?} (answers {key, scope, total, entries}) · getProfile · updateProfile {fields} · refreshExperiments · getStatus · getTelemetryOptOut ·
 * setTelemetryOptOut {optOut} · syncPlatformAchievements (answers {unlockedHere, toPlatform, toClear, statsImported, statsToPlatform}) · platformLink (answers {platformLink}, null when the
 * session proved no platform account) · answerPlatformConsent {allow} (the player's answer for a Steam account
 * waiting in OnPlatformLinkNotice; answers {platformLink}) · syncPlatformFiles (lines Steam Cloud up with the mirrored
 * slots now; a copy waiting for the player's choice comes through OnPlatformSaveConflict) · requestGuestDeletion · cancelGuestDeletion ·
 * guestDataExport {request?} · reportAvatar {subject, reason, note?} (another player's profile picture:
 * SEXUAL, HATE, VIOLENCE, IMPERSONATION, SPAM or OTHER; answers {already}).
 */
UCLASS()
class KRTUWORKS_API UKrtuworksCall : public UKrtuworksNode
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FKrtuworksJsonReady OnSuccess;

	UFUNCTION(BlueprintCallable, Category = "Krtuworks", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UKrtuworksCall* Call(const UObject* WorldContextObject, const FString& Operation, const FString& ArgumentsJson);

	virtual void Activate() override;

private:
	FString Operation;
	FString ArgumentsJson;
};
