#include "SKrtuworksScreen.h"

#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "KrtuworksConvert.h"
#include "KrtuworksRemoteImage.h"
#include "SKrtuworksParts.h"
#include "Styling/CoreStyle.h"
#include "Ui/KrtuworksUiLook.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using KrtuworksConvert::ToFString;

namespace
{
	krtu::UiAnswer MakeAnswer(krtu::UiAnswerKind Kind, int32 Card = 0)
	{
		krtu::UiAnswer Out;
		Out.kind = Kind;
		Out.card = Card;
		return Out;
	}

	/** Milliseconds since 1970 on this device's clock. */
	int64 UnixMillisecondsNow()
	{
		return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
	}

	void Open(const FString& Url)
	{
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
}

void SKrtuworksScreen::Construct(const FArguments& InArgs)
{
	Look = InArgs._Look;
	Screen = InArgs._Screen;
	OnAnswer = InArgs._OnAnswer;
	Build();
}

void SKrtuworksScreen::SetScreen(const krtu::UiScreen& Next)
{
	Screen = Next;
	Build();
}

void SKrtuworksScreen::Answer(const krtu::UiAnswer& Given) const
{
	OnAnswer.ExecuteIfBound(Given);
}

FReply SKrtuworksScreen::OnKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (BackAnswer.IsSet() && (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right || Key == EKeys::Virtual_Back))
	{
		Answer(MakeAnswer(BackAnswer.GetValue()));
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

TSharedRef<SWidget> SKrtuworksScreen::Hints() const
{
	const FKrtuworksUiTheme& Theme = Look->Theme;
	const auto Hint = [&](const char* Glyph, const char* Label) -> TSharedRef<SWidget> {
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))[KrtuworksParts::Pad(Theme, Look->Text(Glyph))]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[KrtuworksParts::Text(Theme, Look->Text(Label), 12, Theme.Muted)];
	};
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))[Hint("hint.selectButton", "hint.select")]
		+ SHorizontalBox::Slot().AutoWidth()[Hint("hint.backButton", "hint.back")];
}

void SKrtuworksScreen::Build()
{
	const FKrtuworksUiTheme& Theme = Look->Theme;
	const auto Say = [this](const char* Key, const std::map<std::string, std::string>& Vars = {}) { return Look->Text(Key, Vars); };
	const auto Press = [this](krtu::UiAnswerKind Kind, int32 Card = 0) {
		return FKrtuworksPress::CreateLambda([this, Kind, Card]() { Answer(MakeAnswer(Kind, Card)); });
	};
	const auto Title = [&](const FString& Text) -> TSharedRef<SWidget> { return KrtuworksParts::Text(Theme, Text, 19, Theme.Text, true); };
	const auto Body = [&](const FString& Text) -> TSharedRef<SWidget> { return KrtuworksParts::Text(Theme, Text, 14, Theme.Muted); };
	const auto Problem = [&](const FString& Text) -> TSharedRef<SWidget> {
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(3.0f)[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(Theme.Danger)]]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(12.0f, 2.0f, 0.0f, 2.0f))[KrtuworksParts::Text(Theme, Text, 14, Theme.Text)];
	};
	const auto Button = [&](const FString& Label, bool bGhost, krtu::UiAnswerKind Kind, TSharedPtr<SWidget>* FocusOut = nullptr) -> TSharedRef<SWidget> {
		const TSharedRef<SKrtuworksButton> Made = SNew(SKrtuworksButton).Theme(Theme).Label(Label).Ghost(bGhost).OnPressed(Press(Kind));
		if (FocusOut != nullptr && !FocusOut->IsValid())
		{
			*FocusOut = Made;
		}
		return Made;
	};
	// The two links under a sentence that names the terms and the privacy notice.
	const auto Links = [&]() -> TSharedRef<SWidget> {
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 14.0f, 0.0f))
				[SNew(SKrtuworksButton).Theme(Theme).Label(Say("age.termsLink")).Link(true).OnPressed(FKrtuworksPress::CreateLambda([Url = ToFString(Screen.termsUrl)]() { Open(Url); }))]
			+ SHorizontalBox::Slot().AutoWidth()
				[SNew(SKrtuworksButton).Theme(Theme).Label(Say("age.privacyLink")).Link(true).OnPressed(FKrtuworksPress::CreateLambda([Url = ToFString(Screen.privacyUrl)]() { Open(Url); }))];
	};

	InitialFocus.Reset();
	BackAnswer.Reset();
	Images.Reset();
	const TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	const auto Add = [&Column](const TSharedRef<SWidget>& Widget, float Below = 0.0f) { Column->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, Below))[Widget]; };
	bool bWide = false;

	switch (Screen.kind)
	{
	case krtu::UiScreenKind::Age:
	{
		Add(Title(Say("age.title")), 14.0f);
		if (Screen.problem != krtu::AgeProblem::None)
		{
			Add(Problem(Say(Screen.problem == krtu::AgeProblem::Invalid ? "age.invalid" : "age.acceptNeeded")), 14.0f);
		}
		TArray<FString> Days;
		for (int32 Day = 1; Day <= 31; ++Day)
		{
			Days.Add(FString::FromInt(Day));
		}
		TArray<FString> Months;
		for (int32 Month = 1; Month <= 12; ++Month)
		{
			Months.Add(Say(("month." + std::to_string(Month)).c_str()));
		}
		TArray<FString> Years;
		for (const int Year : Screen.years)
		{
			Years.Add(FString::FromInt(Year));
		}
		const TSharedRef<SKrtuworksStepper> DayStepper = SNew(SKrtuworksStepper).Theme(Theme).Label(Say("age.day")).Items(Days);
		const TSharedRef<SKrtuworksStepper> MonthStepper = SNew(SKrtuworksStepper).Theme(Theme).Label(Say("age.month")).Items(Months);
		const TSharedRef<SKrtuworksStepper> YearStepper = SNew(SKrtuworksStepper).Theme(Theme).Label(Say("age.year")).Items(Years).StartIndex(FMath::Min(18, Years.Num() - 1));
		TArray<TSharedRef<SKrtuworksStepper>> InOrder;
		switch (Screen.order)
		{
		case krtu::DateOrder::Mdy:
			InOrder = {MonthStepper, DayStepper, YearStepper};
			break;
		case krtu::DateOrder::Ymd:
			InOrder = {YearStepper, MonthStepper, DayStepper};
			break;
		default:
			InOrder = {DayStepper, MonthStepper, YearStepper};
			break;
		}
		for (const TSharedRef<SKrtuworksStepper>& Stepper : InOrder)
		{
			Add(Stepper, 8.0f);
		}
		InitialFocus = InOrder[0];
		const TSharedRef<SKrtuworksChoice> Accept = SNew(SKrtuworksChoice).Theme(Theme)[KrtuworksParts::Text(Theme, ToFString(krtu::textFor(Look->Texts, "age.accept", {{"terms", krtu::textFor(Look->Texts, "age.termsLink")}, {"privacy", krtu::textFor(Look->Texts, "age.privacyLink")}})), 14, Theme.Text)];
		Add(Accept, 4.0f);
		Add(Links(), 18.0f);
		const int32 YearCount = Years.Num();
		const TArray<int> YearValues(Screen.years.data(), YearCount);
		Add(SNew(SKrtuworksButton).Theme(Theme).Label(Say("age.continue")).OnPressed(FKrtuworksPress::CreateLambda([this, DayStepper, MonthStepper, YearStepper, Accept, YearValues]() {
			krtu::UiAnswer Given = MakeAnswer(krtu::UiAnswerKind::Birth);
			Given.day = DayStepper->GetIndex() + 1;
			Given.month = MonthStepper->GetIndex() + 1;
			Given.year = YearValues.IsValidIndex(YearStepper->GetIndex()) ? YearValues[YearStepper->GetIndex()] : 0;
			Given.accepted = Accept->IsSelected();
			Answer(Given);
		})), 8.0f);
		if (Screen.signIn)
		{
			Add(Button(Say("age.signIn"), true, krtu::UiAnswerKind::SignIn));
		}
		break;
	}
	case krtu::UiScreenKind::Terms:
	{
		Add(Title(Say("terms.title")), 6.0f);
		Add(Body(ToFString(krtu::textFor(Look->Texts, "terms.body", {{"terms", krtu::textFor(Look->Texts, "age.termsLink")}, {"privacy", krtu::textFor(Look->Texts, "age.privacyLink")}}))), 8.0f);
		Add(Links(), 16.0f);
		Add(Button(Say("terms.accept"), false, krtu::UiAnswerKind::Accept, &InitialFocus));
		break;
	}
	case krtu::UiScreenKind::Underage:
	{
		Add(Title(Say("underage.title")), 6.0f);
		Add(Body(Say("underage.body")), 16.0f);
		Add(Button(Say("underage.play"), false, krtu::UiAnswerKind::Offline, &InitialFocus));
		BackAnswer = krtu::UiAnswerKind::Offline;
		break;
	}
	case krtu::UiScreenKind::Conflict:
	{
		bWide = true;
		Add(Title(Say("conflict.title", {{"slot", Screen.slot}})), 6.0f);
		Add(Body(Say("conflict.body")), 16.0f);
		const TSharedRef<int32> Picked = MakeShared<int32>(Screen.cards[0].newer ? 0 : 1);
		const TSharedRef<SHorizontalBox> Cards = SNew(SHorizontalBox);
		const TSharedRef<TArray<TSharedPtr<SKrtuworksChoice>>> Made = MakeShared<TArray<TSharedPtr<SKrtuworksChoice>>>();
		for (int32 At = 0; At < 2; ++At)
		{
			const krtu::SaveCard& Save = Screen.cards[static_cast<size_t>(At)];
			const TSharedRef<SVerticalBox> Inside = SNew(SVerticalBox);
			TSharedPtr<SImage> Picture;
			if (Save.summary && Save.summary->image)
			{
				Picture = SNew(SImage).Visibility(EVisibility::Collapsed);
				Inside->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))[SNew(SBox).MaxDesiredHeight(110.0f)[Picture.ToSharedRef()]];
				const TWeakPtr<SImage> WeakPicture = Picture;
				const TSharedRef<FKrtuworksRemoteImage> Image = FKrtuworksRemoteImage::Load(ToFString(*Save.summary->image), [WeakPicture, At, this]() {
					if (const TSharedPtr<SImage> Shown = WeakPicture.Pin())
					{
						Shown->SetImage(Images.IsValidIndex(At) && Images[At].IsValid() ? Images[At]->GetBrush() : nullptr);
						Shown->SetVisibility(EVisibility::Visible);
					}
				});
				Images.SetNum(FMath::Max(Images.Num(), At + 1));
				Images[At] = Image;
			}
			FString Name = ToFString(Save.label);
			if (Save.newer)
			{
				Name += TEXT("  [") + Say("conflict.newer") + TEXT("]");
			}
			Inside->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))[KrtuworksParts::Text(Theme, Name, 14, Theme.Text, true)];
			FString Meta;
			const auto Line = [&Meta](const FString& Text) {
				if (!Text.IsEmpty())
				{
					Meta += (Meta.IsEmpty() ? TEXT("") : TEXT("\n")) + Text;
				}
			};
			if (Save.summary)
			{
				Line(Save.summary->title ? ToFString(*Save.summary->title) : FString());
				for (const std::string& Extra : Save.summary->lines)
				{
					Line(ToFString(Extra));
				}
			}
			Line(ToFString(Save.when));
			Line(ToFString(Save.size));
			Inside->AddSlot().AutoHeight()[KrtuworksParts::Text(Theme, Meta, 12, Theme.Muted)];
			const TSharedRef<SKrtuworksChoice> Card = SNew(SKrtuworksChoice)
														  .Theme(Theme)
														  .Card(true)
														  .Selected(*Picked == At)
														  .OnPressed(FKrtuworksPress::CreateLambda([Picked, Made, At]() {
															  *Picked = At;
															  for (int32 Other = 0; Other < Made->Num(); ++Other)
															  {
																  (*Made)[Other]->SetSelected(Other == At);
															  }
														  }))[Inside];
			Made->Add(Card);
			Cards->AddSlot().FillWidth(1.0f).Padding(FMargin(At == 0 ? 0.0f : 5.0f, 0.0f, At == 0 ? 5.0f : 0.0f, 0.0f))[Card];
		}
		Add(Cards, 14.0f);
		InitialFocus = (*Made)[*Picked];
		const TSharedRef<SKrtuworksButton> KeepButton = SNew(SKrtuworksButton).Theme(Theme).Label(Say("conflict.keep")).OnPressed(FKrtuworksPress::CreateLambda([this, Picked]() { Answer(MakeAnswer(krtu::UiAnswerKind::Keep, *Picked)); }));
		Add(KeepButton);
		break;
	}
	case krtu::UiScreenKind::Error:
	{
		Add(Title(Say("error.title")), 10.0f);
		Add(Problem(ToFString(Screen.message)), 14.0f);
		if (Screen.retry)
		{
			Add(Button(Say("error.retry"), false, krtu::UiAnswerKind::Retry, &InitialFocus), 8.0f);
		}
		Add(Button(Say("error.continue"), Screen.retry, krtu::UiAnswerKind::Continue, &InitialFocus));
		BackAnswer = krtu::UiAnswerKind::Continue;
		break;
	}
	case krtu::UiScreenKind::Waiting:
	{
		Add(SNew(SBox).HAlign(HAlign_Center).Padding(FMargin(0.0f, 4.0f, 0.0f, 14.0f))[SNew(SCircularThrobber).Radius(16.0f)], 0.0f);
		Add(KrtuworksParts::Text(Theme, Say("waiting.title"), 19, Theme.Text, true, true), 6.0f);
		Add(KrtuworksParts::Text(Theme, Say("waiting.body"), 14, Theme.Muted, false, true), 16.0f);
		Add(Button(Say("waiting.reopen"), true, krtu::UiAnswerKind::Reopen, &InitialFocus), 8.0f);
		Add(Button(Say("waiting.cancel"), true, krtu::UiAnswerKind::Cancel));
		BackAnswer = krtu::UiAnswerKind::Cancel;
		break;
	}
	case krtu::UiScreenKind::Device:
	{
		bWide = true;
		const TSharedRef<SVerticalBox> Left = SNew(SVerticalBox);
		Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))[Title(Say("device.title"))];
		if (Screen.expired)
		{
			Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 16.0f))[Problem(Say("device.expired"))];
		}
		else
		{
			Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f))[Body(Say("device.go", {{"uri", Screen.uri}}))];
			Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))[Body(Say("device.enter"))];
			Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))
				[SNew(SBorder).BorderImage_Lambda([Brush = MakeShared<FSlateBrush>(KrtuworksParts::Rounded(Theme.Raised, Theme.RadiusSmall, Theme.Line, 1.0f))]() { return &*Brush; }).Padding(FMargin(16.0f, 12.0f)).HAlign(HAlign_Center)
					[KrtuworksParts::Text(Theme, ToFString(Screen.code), 30, Theme.Text, true, true)]];
			const int64 ExpiresAt = Screen.expiresAtMs;
			Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 16.0f))
				[SNew(STextBlock)
						.Text_Lambda([Held = Look, ExpiresAt]() { return FText::FromString(Held->Text("device.waiting", {{"time", krtu::countdown(ExpiresAt - UnixMillisecondsNow())}})); })
						.Font(KrtuworksParts::Font(Theme, 13))
						.ColorAndOpacity(FSlateColor(Theme.Muted))];
		}
		Left->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))[Button(Say("device.newCode"), !Screen.expired, krtu::UiAnswerKind::NewCode, &InitialFocus)];
		Left->AddSlot().AutoHeight()[Button(Say("device.cancel"), true, krtu::UiAnswerKind::Cancel)];
		const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		Row->AddSlot().FillWidth(1.0f)[Left];
		if (!Screen.expired && !Screen.qr.empty())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(22.0f, 0.0f, 0.0f, 0.0f))[SNew(SBox).WidthOverride(200.0f).HeightOverride(200.0f)[SNew(SKrtuworksQr).Modules(Screen.qr).Size(200.0f)]];
		}
		Add(Row);
		BackAnswer = krtu::UiAnswerKind::Cancel;
		break;
	}
	}

	if (Look->bControllerHints)
	{
		Column->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.0f, 14.0f, 0.0f, 0.0f))[Hints()];
	}

	const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	const TSharedRef<FSlateBrush> CardBrush = MakeShared<FSlateBrush>(KrtuworksParts::Rounded(Theme.Surface, Theme.Radius, Theme.Line, 1.0f));
	ChildSlot[SNew(SBorder)
			.BorderImage(White)
			.BorderBackgroundColor(Theme.Backdrop)
			.Padding(FMargin(16.0f))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)[SNew(SBox).WidthOverride(bWide ? 560.0f : 420.0f)
				[SNew(SScrollBox)
					+ SScrollBox::Slot()[SNew(SBorder).BorderImage_Lambda([CardBrush]() { return &*CardBrush; }).Padding(FMargin(22.0f))[Column]]]]];
}
