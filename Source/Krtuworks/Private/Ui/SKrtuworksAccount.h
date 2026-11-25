#pragma once

#include "CoreMinimal.h"
#include "KrtuworksTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

#include <memory>

struct FKrtuworksUiLook;
class FKrtuworksRemoteImage;
class SBox;

DECLARE_DELEGATE(FKrtuworksChosen);

/** The sign-in button: the "K" mark and the sign-in sentence on the accent colour. */
class SKrtuworksSignIn : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksSignIn)
	{
	}
	SLATE_ARGUMENT(std::shared_ptr<FKrtuworksUiLook>, Look)
	SLATE_EVENT(FKrtuworksChosen, OnPressed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};

/**
 * The account chip: the player's picture (else the first letter of the name), the name and "Krtuworks ID" under it;
 * pressing it opens a small menu below: Manage account and Sign out for an account, Link a Krtuworks ID for a guest.
 */
class SKrtuworksAccountChip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksAccountChip)
	{
	}
	SLATE_ARGUMENT(std::shared_ptr<FKrtuworksUiLook>, Look)
	SLATE_EVENT(FKrtuworksChosen, OnManage)
	SLATE_EVENT(FKrtuworksChosen, OnSignOut)
	SLATE_EVENT(FKrtuworksChosen, OnLink)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows who plays now; nothing is shown while nobody does. */
	void SetPlayer(const FKrtuworksPlayer& Player);

private:
	void Rebuild();

	std::shared_ptr<FKrtuworksUiLook> Look;
	FKrtuworksChosen OnManage;
	FKrtuworksChosen OnSignOut;
	FKrtuworksChosen OnLink;
	FKrtuworksPlayer Player;
	bool bMenuOpen = false;
	TSharedPtr<FKrtuworksRemoteImage> Picture;
};

/** The corner prompt that asks a guest to link a Krtuworks ID; it never blocks the game's input outside itself. */
class SKrtuworksReminder : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksReminder)
	{
	}
	SLATE_ARGUMENT(std::shared_ptr<FKrtuworksUiLook>, Look)
	SLATE_EVENT(FKrtuworksChosen, OnLink)
	SLATE_EVENT(FKrtuworksChosen, OnLater)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
