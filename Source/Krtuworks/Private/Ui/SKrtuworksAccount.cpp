#include "SKrtuworksAccount.h"

#include "KrtuworksRemoteImage.h"
#include "SKrtuworksParts.h"
#include "Styling/CoreStyle.h"
#include "Ui/KrtuworksUiLook.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** The "K" mark: the ink colour on a small rounded square in the accent colour. */
	TSharedRef<SWidget> Mark(const FKrtuworksUiTheme& Theme)
	{
		return SNew(SBox)
			.WidthOverride(22.0f)
			.HeightOverride(22.0f)
			[SNew(SBorder)
					.BorderImage_Lambda([Brush = MakeShared<FSlateBrush>(KrtuworksParts::Rounded(Theme.AccentInk, 6.0f))]() { return &*Brush; })
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(TEXT("K"))).Font(KrtuworksParts::Font(Theme, 13, true)).ColorAndOpacity(FSlateColor(Theme.Accent))]];
	}
}

void SKrtuworksSignIn::Construct(const FArguments& InArgs)
{
	const std::shared_ptr<FKrtuworksUiLook> Held = InArgs._Look;
	ChildSlot[SNew(SKrtuworksButton).Theme(Held->Theme).Label(Held->Text("signIn.button")).Leading(Mark(Held->Theme)).OnPressed(FKrtuworksPress::CreateLambda([Pressed = InArgs._OnPressed]() { Pressed.ExecuteIfBound(); }))];
}

void SKrtuworksAccountChip::Construct(const FArguments& InArgs)
{
	Look = InArgs._Look;
	OnManage = InArgs._OnManage;
	OnSignOut = InArgs._OnSignOut;
	OnLink = InArgs._OnLink;
	Rebuild();
}

void SKrtuworksAccountChip::SetPlayer(const FKrtuworksPlayer& Now)
{
	bMenuOpen = false;
	Player = Now;
	Rebuild();
}

void SKrtuworksAccountChip::Rebuild()
{
	if (!Player.bSignedIn)
	{
		ChildSlot[SNullWidget::NullWidget];
		return;
	}
	const FKrtuworksUiTheme& Theme = Look->Theme;
	const bool bGuest = Player.Kind == EKrtuworksPlayerKind::Guest;
	const FString Name = bGuest ? Look->Text("account.guest") : (Player.Username.IsEmpty() ? Look->Text("account.label") : Player.Username);

	// The picture when there is one and it has loaded, else the name's first letter.
	const TSharedRef<SOverlay> Avatar = SNew(SOverlay);
	Avatar->AddSlot().HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(Name.Left(1).ToUpper())).Font(KrtuworksParts::Font(Theme, 14, true)).ColorAndOpacity(FSlateColor(Theme.Text))];
	const TSharedRef<SImage> Face = SNew(SImage).Visibility(EVisibility::Collapsed);
	Avatar->AddSlot()[Face];
	Picture.Reset();
	if (!bGuest && !Player.Picture.IsEmpty())
	{
		const TWeakPtr<SImage> WeakFace = Face;
		const TSharedRef<FKrtuworksRemoteImage> Loading = FKrtuworksRemoteImage::Load(Player.Picture, [this, WeakFace]() {
			const TSharedPtr<SImage> Shown = WeakFace.Pin();
			if (Shown.IsValid() && Picture.IsValid() && Picture->GetBrush() != nullptr)
			{
				Shown->SetImage(Picture->GetBrush());
				Shown->SetVisibility(EVisibility::Visible);
			}
		});
		Picture = Loading;
	}

	const TSharedRef<SKrtuworksButton> Chip = SNew(SKrtuworksButton)
												  .Theme(Theme)
												  .Ghost(true)
												  .Label(FString())
												  .Leading(SNew(SBox).WidthOverride(32.0f).HeightOverride(32.0f)[Avatar])
												  .OnPressed(FKrtuworksPress::CreateLambda([this]() { bMenuOpen = !bMenuOpen; }));
	const TSharedRef<SVerticalBox> Names = SNew(SVerticalBox);
	Names->AddSlot().AutoHeight()[KrtuworksParts::Text(Theme, Name, 14, Theme.Text, true)];
	Names->AddSlot().AutoHeight()[KrtuworksParts::Text(Theme, Look->Text("account.label"), 11, Theme.Muted)];

	const TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	Column->AddSlot().AutoHeight()
		[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[Chip] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))[Names]];
	// The menu stays built and only shows and hides: pressing the chip must not replace the chip while it handles the press.
	const auto Item = [this, &Theme](const FString& Label, const FKrtuworksChosen& Run) -> TSharedRef<SWidget> {
		return SNew(SKrtuworksButton).Theme(Theme).Ghost(true).Label(Label).OnPressed(FKrtuworksPress::CreateLambda([this, Run]() {
			bMenuOpen = false;
			Run.ExecuteIfBound();
		}));
	};
	const TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	if (bGuest)
	{
		Menu->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))[Item(Look->Text("account.link"), OnLink)];
	}
	else
	{
		Menu->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))[Item(Look->Text("account.manage"), OnManage)];
		Menu->AddSlot().AutoHeight()[Item(Look->Text("account.signOut"), OnSignOut)];
	}
	Column->AddSlot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[SNew(SBox).MinDesiredWidth(200.0f).Visibility_Lambda([this]() { return bMenuOpen ? EVisibility::Visible : EVisibility::Collapsed; })[Menu]];
	ChildSlot[Column];
}

void SKrtuworksReminder::Construct(const FArguments& InArgs)
{
	const std::shared_ptr<FKrtuworksUiLook> Held = InArgs._Look;
	const FKrtuworksUiTheme& Theme = Held->Theme;
	const FKrtuworksChosen Link = InArgs._OnLink;
	const FKrtuworksChosen Later = InArgs._OnLater;
	const TSharedRef<SVerticalBox> Text = SNew(SVerticalBox);
	Text->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f))[KrtuworksParts::Text(Theme, Held->Text("reminder.title"), 14, Theme.Text, true)];
	Text->AddSlot().AutoHeight()[KrtuworksParts::Text(Theme, Held->Text("reminder.body"), 13, Theme.Muted)];
	const TSharedRef<SVerticalBox> Actions = SNew(SVerticalBox);
	Actions->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
		[SNew(SKrtuworksButton).Theme(Theme).Ghost(true).Label(Held->Text("reminder.later")).OnPressed(FKrtuworksPress::CreateLambda([Later]() { Later.ExecuteIfBound(); }))];
	Actions->AddSlot().AutoHeight()[SNew(SKrtuworksButton).Theme(Theme).Label(Held->Text("reminder.link")).OnPressed(FKrtuworksPress::CreateLambda([Link]() { Link.ExecuteIfBound(); }))];
	// Only the card takes the pointer: the rest of the screen stays the game's.
	SetVisibility(EVisibility::SelfHitTestInvisible);
	ChildSlot[SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(FMargin(16.0f))
		[SNew(SBox).WidthOverride(460.0f).Visibility(EVisibility::Visible)
			[SNew(SBorder)
					.BorderImage_Lambda([Brush = MakeShared<FSlateBrush>(KrtuworksParts::Rounded(Theme.Surface, Theme.Radius, Theme.Line, 1.0f))]() { return &*Brush; })
					.Padding(FMargin(16.0f, 14.0f))
					[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[Text] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(14.0f, 0.0f, 0.0f, 0.0f))[Actions]]]]];
}
