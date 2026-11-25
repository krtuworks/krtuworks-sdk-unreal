#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Fonts/SlateFontInfo.h"

#include "KrtuworksSettings.generated.h"

UENUM()
enum class EKrtuworksEnvironment : uint8
{
	/** TEST in the editor and development builds, LIVE in shipping builds. */
	Automatic,
	Test,
	Live,
};

UENUM()
enum class EKrtuworksSignInMode : uint8
{
	/** Consoles use a code and an address on another device, iOS and Android the system sheet, everything else the system browser. */
	Automatic,
	/** The system browser and a loopback address. */
	Browser,
	/** The phone's own authentication session (iOS, Android). */
	App,
	/** A code and an address on another device (consoles, TVs). */
	Device,
	/** Signing in is not offered: the sign-in button and the age screen's "Sign in with Krtuworks ID" are hidden. */
	None,
};

UENUM()
enum class EKrtuworksControllerHints : uint8
{
	/** Shown on consoles. */
	Automatic,
	Always,
	Never,
};

/** The colours, corners and font of the ready-made screens: the same tokens as the web package's CSS variables. */
USTRUCT(BlueprintType)
struct KRTUWORKS_API FKrtuworksUiTheme
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Accent = FLinearColor::FromSRGBColor(FColor(0xFF, 0xBF, 0x23));

	/** Text and marks on the accent colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor AccentInk = FLinearColor::FromSRGBColor(FColor(0x1D, 0x1D, 0x1F));

	/** Cards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Surface = FLinearColor::FromSRGBColor(FColor(0x1B, 0x1B, 0x1E));

	/** Fields and buttons on a card. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Raised = FLinearColor::FromSRGBColor(FColor(0x26, 0x26, 0x2B));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Line = FLinearColor::FromSRGBColor(FColor(0x34, 0x34, 0x3A));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Text = FLinearColor::FromSRGBColor(FColor(0xF5, 0xF5, 0xF7));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Muted = FLinearColor::FromSRGBColor(FColor(0xA1, 0xA1, 0xA6));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Danger = FLinearColor::FromSRGBColor(FColor(0xFF, 0x6B, 0x5E));

	/** Behind a screen that blocks the game. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FLinearColor Backdrop = FLinearColor(0.0f, 0.0f, 0.0f, 0.55f);

	/** Cards and the account chip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme", meta = (ClampMin = "0"))
	float Radius = 16.0f;

	/** Buttons and fields. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme", meta = (ClampMin = "0"))
	float RadiusSmall = 10.0f;

	/** The typeface of every screen; the engine's default font while none is set. The size comes from the screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Krtuworks|Theme")
	FSlateFontInfo Font;
};

/**
 * Project Settings > Plugins > Krtuworks SDK. The app itself is described by krtuworks-config.json next to the
 * .uproject (downloaded from the partner console's Credentials page); these are the game's own choices.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Krtuworks SDK"))
class KRTUWORKS_API UKrtuworksSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UKrtuworksSettings();

	/** Which environment's players, data and switches the game uses. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks")
	EKrtuworksEnvironment Environment = EKrtuworksEnvironment::Automatic;

	/** Opens a play session and keeps it alive (heartbeats, active time). */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks")
	bool bSessions = true;

	/**
	 * The app's slug from krtuworks-config.json, copied here by the editor whenever the file changes: the
	 * Android build reads it from the project's config to register the sign-in's return scheme
	 * (com.krtuworks.<slug>) on the game's activity.
	 */
	UPROPERTY(Config, VisibleAnywhere, Category = "Krtuworks")
	FString AppSlug;

	/** How the player signs in with Krtuworks ID on the ready-made screens. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	EKrtuworksSignInMode SignInMode = EKrtuworksSignInMode::Automatic;

	/** Plays as a guest when nobody signs in; off starts the sign-in instead of the age screen. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	bool bGuests = true;

	/** Shows the "link a Krtuworks ID" prompt in the corner when the SDK says a guest has something to lose. Off when the game handles OnLinkReminder itself. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	bool bLinkReminder = true;

	/** Asks the player which save to keep when Steam Cloud holds a different copy than the server. Off when the game handles OnPlatformSaveConflict itself. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	bool bPlatformSaveConflicts = true;

	/** The "A Select  B Back" row at the bottom of each screen. */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	EKrtuworksControllerHints ControllerHints = EKrtuworksControllerHints::Automatic;

	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	FKrtuworksUiTheme Theme;

	/**
	 * The game's own sentences for the screens, by key (age.title, device.go, error.offline …): the English text
	 * stays for a key not listed. FText, so Unreal's localization tools translate them. {name} marks a value
	 * filled in when the screen is shown.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Krtuworks|Screens")
	TMap<FName, FText> TextOverrides;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual void PostInitProperties() override;
#endif
};
