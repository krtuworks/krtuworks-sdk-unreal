#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "KrtuworksSettings.generated.h"

UENUM()
enum class EKrtuworksEnvironment : uint8
{
	/** TEST in the editor and development builds, LIVE in shipping builds. */
	Automatic,
	Test,
	Live,
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

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual void PostInitProperties() override;
#endif
};
