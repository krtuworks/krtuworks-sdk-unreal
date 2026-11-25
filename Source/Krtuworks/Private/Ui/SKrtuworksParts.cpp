#include "SKrtuworksParts.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** Pressing a focused control: Enter, Space and the gamepad's accept button. */
	bool IsAccept(const FKey& Key)
	{
		return Key == EKeys::Enter || Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::Virtual_Accept;
	}

	bool IsLeft(const FKey& Key)
	{
		return Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left || Key == EKeys::Gamepad_LeftStick_Left;
	}

	bool IsRight(const FKey& Key)
	{
		return Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right || Key == EKeys::Gamepad_LeftStick_Right;
	}
}

namespace KrtuworksParts
{
	FSlateFontInfo Font(const FKrtuworksUiTheme& Theme, int32 Size, bool bBold)
	{
		if (Theme.Font.HasValidFont())
		{
			FSlateFontInfo Made = Theme.Font;
			Made.Size = Size;
			return Made;
		}
		return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}

	FSlateBrush Rounded(const FLinearColor& Fill, float Radius, const FLinearColor& Outline, float OutlineWidth)
	{
		return FSlateRoundedBoxBrush(Fill, Radius, Outline, OutlineWidth);
	}

	TSharedRef<SWidget> Text(const FKrtuworksUiTheme& Theme, const FString& Text, int32 Size, const FLinearColor& Color, bool bBold, bool bCentered)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Text))
			.Font(Font(Theme, Size, bBold))
			.ColorAndOpacity(FSlateColor(Color))
			.AutoWrapText(true)
			.Justification(bCentered ? ETextJustify::Center : ETextJustify::Left);
	}

	TSharedRef<SWidget> Pad(const FKrtuworksUiTheme& Theme, const FString& Glyph)
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Theme.Raised)
			.Padding(FMargin(6.0f, 2.0f))
			[SNew(STextBlock).Text(FText::FromString(Glyph)).Font(Font(Theme, 11, true)).ColorAndOpacity(FSlateColor(Theme.Text))];
	}
}

// ── SKrtuworksButton ─────────────────────────────────────────────────────

void SKrtuworksButton::Construct(const FArguments& InArgs)
{
	const FKrtuworksUiTheme& Theme = InArgs._Theme;
	const bool bFilled = !InArgs._Ghost && !InArgs._Link;
	const FLinearColor Fill = bFilled ? Theme.Accent : FLinearColor::Transparent;
	const FLinearColor Ink = InArgs._Link ? Theme.Accent : (InArgs._Ghost ? Theme.Text : Theme.AccentInk);
	NormalBrush = KrtuworksParts::Rounded(Fill, Theme.RadiusSmall, Theme.Line, InArgs._Ghost ? 1.0f : 0.0f);
	// The focus ring is the text colour on the accent fill, the accent colour everywhere else.
	FocusBrush = KrtuworksParts::Rounded(Fill, Theme.RadiusSmall, bFilled ? Theme.Text : Theme.Accent, 3.0f);
	OnPressed = InArgs._OnPressed;

	const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	if (InArgs._Leading.IsValid())
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))[InArgs._Leading.ToSharedRef()];
	}
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(InArgs._Label)).Font(KrtuworksParts::Font(Theme, 15, true)).ColorAndOpacity(FSlateColor(Ink))];

	ChildSlot[SNew(SBox).MinDesiredHeight(InArgs._Link ? 0.0f : 46.0f)
		[SNew(SBorder)
				.BorderImage_Lambda([this]() { return HasKeyboardFocus() || IsHovered() ? &FocusBrush : &NormalBrush; })
				.Padding(InArgs._Link ? FMargin(2.0f) : FMargin(16.0f, 0.0f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)[Row]]];
}

FReply SKrtuworksButton::OnKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	if (IsAccept(InKeyEvent.GetKey()))
	{
		OnPressed.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SKrtuworksButton::OnMouseButtonDown(const FGeometry&, const FPointerEvent& MouseEvent)
{
	return MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton ? FReply::Handled().SetUserFocus(AsShared(), EFocusCause::Mouse) : FReply::Unhandled();
}

FReply SKrtuworksButton::OnMouseButtonUp(const FGeometry&, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsHovered())
	{
		OnPressed.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

// ── SKrtuworksStepper ────────────────────────────────────────────────────

void SKrtuworksStepper::Construct(const FArguments& InArgs)
{
	const FKrtuworksUiTheme& Theme = InArgs._Theme;
	Items = InArgs._Items;
	StartIndex = FMath::Clamp(InArgs._StartIndex, 0, FMath::Max(0, Items.Num() - 1));
	NormalBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Line, 1.0f);
	FocusBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Accent, 3.0f);
	const FSlateFontInfo Small = KrtuworksParts::Font(Theme, 13);
	ChildSlot[SNew(SBox).MinDesiredHeight(44.0f)
		[SNew(SBorder)
				.BorderImage_Lambda([this]() { return HasKeyboardFocus() || IsHovered() ? &FocusBrush : &NormalBrush; })
				.Padding(FMargin(12.0f, 0.0f))
				.VAlign(VAlign_Center)
				[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(0.4f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(InArgs._Label)).Font(Small).ColorAndOpacity(FSlateColor(Theme.Muted))]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(TEXT("<"))).Font(KrtuworksParts::Font(Theme, 16, true)).ColorAndOpacity(FSlateColor(Theme.Muted))]
					+ SHorizontalBox::Slot().FillWidth(0.6f).VAlign(VAlign_Center).HAlign(HAlign_Center)
						[SNew(STextBlock).Text_Lambda([this]() { return Value(); }).Font(KrtuworksParts::Font(Theme, 16)).ColorAndOpacity(FSlateColor(Theme.Text))]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(TEXT(">"))).Font(KrtuworksParts::Font(Theme, 16, true)).ColorAndOpacity(FSlateColor(Theme.Muted))]]]];
}

FText SKrtuworksStepper::Value() const
{
	return FText::FromString(Items.IsValidIndex(Index) ? Items[Index] : FString(TEXT("--")));
}

void SKrtuworksStepper::Step(int32 By)
{
	if (Items.Num() == 0)
	{
		return;
	}
	// From nothing chosen the first step lands on the start item whichever way it goes.
	Index = Index < 0 ? StartIndex : FMath::Clamp(Index + By, 0, Items.Num() - 1);
}

FReply SKrtuworksStepper::OnKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (IsLeft(Key))
	{
		Step(-1);
	}
	else if (IsRight(Key))
	{
		Step(1);
	}
	else if (Key == EKeys::PageUp || Key == EKeys::Gamepad_LeftShoulder)
	{
		Step(-10);
	}
	else if (Key == EKeys::PageDown || Key == EKeys::Gamepad_RightShoulder)
	{
		Step(10);
	}
	else
	{
		return FReply::Unhandled();
	}
	return FReply::Handled();
}

FReply SKrtuworksStepper::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	const float Across = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()).X / FMath::Max(1.0f, MyGeometry.GetLocalSize().X);
	if (Across < 0.3f)
	{
		Step(-1);
	}
	else if (Across > 0.7f)
	{
		Step(1);
	}
	return FReply::Handled().SetUserFocus(AsShared(), EFocusCause::Mouse);
}

FReply SKrtuworksStepper::OnMouseWheel(const FGeometry&, const FPointerEvent& MouseEvent)
{
	Step(MouseEvent.GetWheelDelta() > 0.0f ? -1 : 1);
	return FReply::Handled();
}

// ── SKrtuworksChoice ─────────────────────────────────────────────────────

void SKrtuworksChoice::Construct(const FArguments& InArgs)
{
	const FKrtuworksUiTheme& Theme = InArgs._Theme;
	bCard = InArgs._Card;
	bSelected = InArgs._Selected;
	OnPressed = InArgs._OnPressed;
	if (bCard)
	{
		OffBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Line, 1.0f);
		OnBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Accent, 2.0f);
		FocusOffBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Text, 3.0f);
		FocusOnBrush = KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Accent, 3.0f);
		ChildSlot[SNew(SBorder)
				.BorderImage_Lambda([this]() {
					const bool bFocused = HasKeyboardFocus() || IsHovered();
					return bSelected ? (bFocused ? &FocusOnBrush : &OnBrush) : (bFocused ? &FocusOffBrush : &OffBrush);
				})
				.Padding(FMargin(14.0f))[InArgs._Content.Widget]];
		return;
	}
	OffBrush = KrtuworksParts::Rounded(Theme.Raised, 6.0f, Theme.Line, 1.0f);
	OnBrush = KrtuworksParts::Rounded(Theme.Accent, 6.0f, Theme.Accent, 1.0f);
	FocusOffBrush = KrtuworksParts::Rounded(Theme.Raised, 6.0f, Theme.Accent, 3.0f);
	FocusOnBrush = KrtuworksParts::Rounded(Theme.Accent, 6.0f, Theme.Text, 3.0f);
	ChildSlot[SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(FMargin(0.0f, 1.0f, 10.0f, 0.0f))
			[SNew(SBox).WidthOverride(22.0f).HeightOverride(22.0f)
				[SNew(SBorder).BorderImage_Lambda([this]() {
					const bool bFocused = HasKeyboardFocus() || IsHovered();
					return bSelected ? (bFocused ? &FocusOnBrush : &OnBrush) : (bFocused ? &FocusOffBrush : &OffBrush);
				})]]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[InArgs._Content.Widget]];
}

void SKrtuworksChoice::Press()
{
	bSelected = bCard ? true : !bSelected;
	OnPressed.ExecuteIfBound();
}

FReply SKrtuworksChoice::OnKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	if (IsAccept(InKeyEvent.GetKey()))
	{
		Press();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SKrtuworksChoice::OnMouseButtonDown(const FGeometry&, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	Press();
	return FReply::Handled().SetUserFocus(AsShared(), EFocusCause::Mouse);
}

// ── SKrtuworksQr ─────────────────────────────────────────────────────────

void SKrtuworksQr::Construct(const FArguments& InArgs)
{
	Modules = InArgs._Modules;
	Size = InArgs._Size;
}

FVector2D SKrtuworksQr::ComputeDesiredSize(float) const
{
	return FVector2D(Size, Size);
}

int32 SKrtuworksQr::OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry, const FSlateRect&, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle&, bool) const
{
	const int32 Count = static_cast<int32>(Modules.size());
	if (Count == 0)
	{
		return LayerId;
	}
	// The code and its four-module quiet zone, each module a whole number of device pixels so every edge is sharp.
	const int32 Total = Count + 8;
	const FVector2f Local = AllottedGeometry.GetLocalSize();
	const float Scale = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const float SidePixels = FMath::Min(Local.X, Local.Y) * Scale;
	const float ModulePixels = FMath::Max(1.0f, FMath::FloorToFloat(SidePixels / Total));
	const float CodePixels = ModulePixels * Total;
	const float OriginPixels = FMath::FloorToFloat((SidePixels - CodePixels) * 0.5f);
	const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	const auto Box = [&](float XPixels, float YPixels, float WidthPixels, float HeightPixels, const FLinearColor& Color, int32 Layer) {
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(WidthPixels / Scale, HeightPixels / Scale), FSlateLayoutTransform(FVector2f(XPixels / Scale, YPixels / Scale))), White, ESlateDrawEffect::None, Color);
	};
	Box(OriginPixels, OriginPixels, CodePixels, CodePixels, FLinearColor::White, LayerId);
	for (int32 Row = 0; Row < Count; ++Row)
	{
		const std::vector<bool>& Line = Modules[static_cast<size_t>(Row)];
		int32 Column = 0;
		while (Column < Count)
		{
			if (!Line[static_cast<size_t>(Column)])
			{
				++Column;
				continue;
			}
			int32 End = Column;
			while (End < Count && Line[static_cast<size_t>(End)])
			{
				++End;
			}
			Box(OriginPixels + (Column + 4) * ModulePixels, OriginPixels + (Row + 4) * ModulePixels, (End - Column) * ModulePixels, ModulePixels, FLinearColor::Black, LayerId + 1);
			Column = End;
		}
	}
	return LayerId + 1;
}
