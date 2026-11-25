#pragma once

#include "CoreMinimal.h"
#include "KrtuworksSettings.h"
#include "Styling/SlateBrush.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

#include <vector>

/** The small pieces the ready-made screens are built from; every colour, corner and font comes from the theme. */
namespace KrtuworksParts
{
	/** The theme's typeface at `Size` (bold or not), else the engine's default font. */
	FSlateFontInfo Font(const FKrtuworksUiTheme& Theme, int32 Size, bool bBold = false);

	/** A rounded fill with an optional outline. */
	FSlateBrush Rounded(const FLinearColor& Fill, float Radius, const FLinearColor& Outline = FLinearColor::Transparent, float OutlineWidth = 0.0f);

	/** A wrapped line of text. */
	TSharedRef<SWidget> Text(const FKrtuworksUiTheme& Theme, const FString& Text, int32 Size, const FLinearColor& Color, bool bBold = false, bool bCentered = false);

	/** The pad-button chip of the controller hints row (`A`, `B`). */
	TSharedRef<SWidget> Pad(const FKrtuworksUiTheme& Theme, const FString& Glyph);
}

DECLARE_DELEGATE(FKrtuworksPress);

/** A button: accent filled, ghost (outlined) or a bare link. Focusable by keyboard and gamepad; Accept presses it. */
class SKrtuworksButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksButton)
		: _Ghost(false)
		, _Link(false)
	{
	}
	SLATE_ARGUMENT(FKrtuworksUiTheme, Theme)
	SLATE_ARGUMENT(FString, Label)
	SLATE_ARGUMENT(bool, Ghost)
	SLATE_ARGUMENT(bool, Link)
	/** A widget before the label (the "K" mark). */
	SLATE_ARGUMENT(TSharedPtr<SWidget>, Leading)
	SLATE_EVENT(FKrtuworksPress, OnPressed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	FSlateBrush NormalBrush;
	FSlateBrush FocusBrush;
	FKrtuworksPress OnPressed;
};

/** A choice among a list (a day, a month, a year): Left and Right step it, the shoulders and Page keys step by ten. */
class SKrtuworksStepper : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksStepper)
		: _StartIndex(0)
	{
	}
	SLATE_ARGUMENT(FKrtuworksUiTheme, Theme)
	SLATE_ARGUMENT(FString, Label)
	SLATE_ARGUMENT(TArray<FString>, Items)
	/** Where the first step lands from "nothing chosen". */
	SLATE_ARGUMENT(int32, StartIndex)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The chosen item, or -1 while nothing is chosen. */
	int32 GetIndex() const { return Index; }

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	void Step(int32 By);
	FText Value() const;

	FSlateBrush NormalBrush;
	FSlateBrush FocusBrush;
	TArray<FString> Items;
	int32 StartIndex = 0;
	int32 Index = -1;
};

/** A box the player ticks (the terms), or a card the player picks (a save): focusable, Accept presses it. */
class SKrtuworksChoice : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksChoice)
		: _Card(false)
		, _Selected(false)
	{
	}
	SLATE_ARGUMENT(FKrtuworksUiTheme, Theme)
	/** A card holding `Content`, rather than a tick box beside it. */
	SLATE_ARGUMENT(bool, Card)
	SLATE_ARGUMENT(bool, Selected)
	SLATE_DEFAULT_SLOT(FArguments, Content)
	SLATE_EVENT(FKrtuworksPress, OnPressed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	bool IsSelected() const { return bSelected; }
	void SetSelected(bool bValue) { bSelected = bValue; }

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	void Press();

	FSlateBrush OffBrush;
	FSlateBrush OnBrush;
	FSlateBrush FocusOffBrush;
	FSlateBrush FocusOnBrush;
	bool bSelected = false;
	bool bCard = false;
	FKrtuworksPress OnPressed;
};

/** A QR code painted as whole-pixel boxes (crisp at any size) with its quiet zone; dark modules black on white. */
class SKrtuworksQr : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksQr)
		: _Size(200.0f)
	{
	}
	SLATE_ARGUMENT(std::vector<std::vector<bool>>, Modules)
	SLATE_ARGUMENT(float, Size)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	std::vector<std::vector<bool>> Modules;
	float Size = 200.0f;
};
