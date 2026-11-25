#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "KrtuworksTypes.h"

#include "KrtuworksWidgets.generated.h"

class SWidget;
class SKrtuworksAccountChip;
class UKrtuworksSubsystem;

/**
 * The sign-in button for a menu or a HUD: the "K" mark and the sign-in sentence on the accent colour (Project
 * Settings > Plugins > Krtuworks SDK for the theme and the text). Pressing it signs the player in with Krtuworks
 * ID with the ready-made screens. Hidden while signing in is not offered (sign-in mode None).
 */
UCLASS(meta = (DisplayName = "Krtuworks Sign-in Button"))
class KRTUWORKS_API UKrtuworksSignInButton : public UWidget
{
	GENERATED_BODY()

public:
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void SynchronizeProperties() override;

private:
	void HandlePressed();

	TSharedPtr<SWidget> Button;
	bool bSigningIn = false;
};

/**
 * The account chip: the player's picture (else the first letter of the name), the name and "Krtuworks ID" under it.
 * Pressing it opens a small menu: Manage account (id.krtuworks.com in the system browser) and Sign out for an
 * account, Link a Krtuworks ID for a guest. It follows who plays and shows nothing while nobody does.
 */
UCLASS(meta = (DisplayName = "Krtuworks Account Chip"))
class KRTUWORKS_API UKrtuworksAccountChip : public UWidget
{
	GENERATED_BODY()

public:
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void SynchronizeProperties() override;

private:
	UFUNCTION()
	void HandleIdentityChanged(const FKrtuworksIdentityChange& Change);

	void Refresh();
	void SignOut();
	void Link();

	UKrtuworksSubsystem* GetSdk() const;

	TSharedPtr<SKrtuworksAccountChip> Chip;
	bool bSigningIn = false;
};
