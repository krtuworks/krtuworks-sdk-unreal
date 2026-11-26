#pragma once

#include "CoreMinimal.h"

#include "KrtuworksTypes.generated.h"

UENUM(BlueprintType)
enum class EKrtuworksPlayerKind : uint8
{
	Guest,
	Account,
};

/** Who plays. Subject is the id this app knows the player by: the same on every device, never the Krtuworks ID itself. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksPlayer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bSignedIn = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	EKrtuworksPlayerKind Kind = EKrtuworksPlayerKind::Guest;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Subject;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Username;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Picture;
};

/** A failure: the server's code (or the SDK's own: network_error, signed_out, cancelled …), its message, the HTTP status (0 = no answer). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int32 Status = 0;

	/** The same call can succeed later (no answer, a busy server). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bRetryable = false;

	/** The rest of the server's answer as JSON (mergedInto, current, endsAt, terms …). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString DetailsJson;
};

/** signed_in · guest · signed_out · merged · deletion_pending · sanctioned */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksIdentityChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FKrtuworksPlayer Player;

	/** The guest playing here became this Krtuworks ID (same player). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bPromoted = false;

	/** A new guest was made on this device. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bCreated = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Reason;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString PreviousSubject;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString MergedInto;
};

/** An event's data: named text, number and true/false values. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksEventData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	TMap<FString, FString> Strings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	TMap<FString, double> Numbers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	TMap<FString, bool> Booleans;
};

/** Where an event happened: a world position in a scene (Space "world"), or a screen position (Space "screen", Z unused). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksPosition
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FString Space = TEXT("world");

	/** The scene or screen name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FString Area;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FVector Location = FVector::ZeroVector;
};

/** A level, with its world and part when the game has those layers. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksLevel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FString Level;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FString World;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks")
	FString Part;
};

/** The owner's gates for this build: maintenance, a newer minimum version, a switch (ALL stops every call, TELEMETRY the statistics). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksGate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bMaintenance = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString MaintenanceMessage;

	/** The player must update from the store. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bUpdateRequired = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bSwitchedOff = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString SwitchMode;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString SwitchMessage;

	/** The game needs the server on its very first launch (krtuworks-config.json firstLaunch: online) and it cannot be reached: show "connect to play" and try again. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bFirstLaunchOffline = false;
};

USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksStat
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Description;

	/** SUM, MAX, MIN or LATEST. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Kind;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bHasValue = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	double Value = 0;
};

USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksAchievement
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bHidden = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bUnlocked = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString UnlockedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString IconUrl;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString LockedIconUrl;
};

/** The app's stats and achievements with the player's values (names in the player's language where the app has them). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksProgress
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<FKrtuworksStat> Stats;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<FKrtuworksAchievement> Achievements;
};

/** A leaderboard of the app with the player's own standing (names in the player's language where the app has them). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksLeaderboard
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Description;

	/** ASC (the smaller score ranks first) or DESC. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString SortOrder;

	/** NUMERIC, TIME_SECONDS or TIME_MILLISECONDS: how to show a score. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Display;

	/** Whether the player has a score here (and, unless banned, a rank). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bHasScore = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Score = 0;

	/** The bytes the game kept with the player's score. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<uint8> Details;

	/** Whether the player is ranked here (0 and false while banned or a test player). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bHasRank = false;

	/** Competition rank: equal scores share one. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Rank = 0;

	/** Rows on the board. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Entries = 0;
};

/** One row of a leaderboard. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksLeaderboardEntry
{
	GENERATED_BODY()

	/** Competition rank: equal scores share one. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Rank = 0;

	/** The player's Krtuworks username ("Player" when the server gave none). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Score = 0;

	/** The bytes the game kept with the score. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<uint8> Details;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString UpdatedAt;

	/** The row is the signed-in player's. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bYou = false;
};

/** A page of a leaderboard's rows in its order. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksLeaderboardPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Key;

	/** global or around. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Scope;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString SortOrder;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Display;

	/** Rows on the board. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<FKrtuworksLeaderboardEntry> Entries;
};

/** One of the player's bug reports as the player sees it (never the notes, the assignee or the labels). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksBugReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Id;

	/** CRASH, GAMEPLAY, GRAPHICS, AUDIO, PERFORMANCE, CONTROLS, TEXT or OTHER. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Build;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bScreenshot = false;

	/** NEW, INVESTIGATING, RESOLVED, CANNOT_REPRODUCE or DUPLICATE (a duplicate shows its original's). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Status;

	/** The build the fix shipped in (empty until it is fixed). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString FixedIn;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bDuplicate = false;
};

USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksFileVersion
{
	GENERATED_BODY()

	/** The version's id: the base of the next save of the slot. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Slot;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Generation = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 SizeBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString SavedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bFromMerge = false;

	/** The platform whose cloud this copy came from (STEAM); empty for the server's own versions. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString PlatformCloud;

	/** A settings save that waits for its turn (a slot is written at most every 30 seconds): kept on the device and sent later; Id is empty until then. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bQueued = false;
};

/**
 * The platform cloud's copy of a SAVE or PROFILE slot differs from the server's and nothing tells which is newer
 * (the first time this device compares them, or both moved): it waits on the server for the player's choice.
 * Show both (times, sizes, your own summary of PlatformData) and call the Call node's resolveFile {slot, keep,
 * current} with the id to keep and Server.Id as current; the kept bytes then go to both sides.
 */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksPlatformSaveConflict
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Slot;

	/** The slot's current version. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FKrtuworksFileVersion Server;

	/** The platform cloud's copy, waiting in the slot. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FKrtuworksFileVersion Platform;

	/** Whether PlatformData holds the copy's bytes (they are still the ones on the platform). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bHasPlatformData = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<uint8> PlatformData;
};

USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksFileRead
{
	GENERATED_BODY()

	/** The slot is still the version the game held (If-None-Match): nothing was read. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bUnchanged = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<uint8> Bytes;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString VersionId;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int64 Generation = 0;

	/** A save of this slot started on this device while the read ran: the read is older than what the player has here. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bNewerLocalSave = false;
};

/** A console's or TV's sign-in: show UserCode and VerificationUri (or a QR code of VerificationUriComplete). */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksDeviceLink
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString UserCode;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString VerificationUri;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString VerificationUriComplete;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	int32 ExpiresIn = 0;
};

/** One of the validator's checks. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksValidationCheck
{
	GENERATED_BODY()

	/** config, build, platform, sign_in, session, events, remote_config, cloud_save, progress, bug_report or sentry. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Key;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Title;

	/** PASSED, FAILED or SKIPPED. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Status;

	/** Why it failed or was skipped (empty when it passed). */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Detail;
};

/** What the validator found: this build's setup, then its TEST round against the servers. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksValidationReport
{
	GENERATED_BODY()

	/** No check failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	bool bOk = false;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	FString Build;

	UPROPERTY(BlueprintReadOnly, Category = "Krtuworks")
	TArray<FKrtuworksValidationCheck> Checks;
};
