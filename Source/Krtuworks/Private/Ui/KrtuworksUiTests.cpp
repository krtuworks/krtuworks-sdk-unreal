// The ready-made screens built inside the engine (headless in the editor: Automation RunTests Krtuworks). Every
// screen kind, the corner reminder, the sign-in button and the account chip construct with their buttons; the core's
// tests cover what each screen answers, these cover that the Slate side draws it.
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Framework/Application/SlateApplication.h"
#include "KrtuworksCore/Qr.h"
#include "KrtuworksCore/Ui.h"
#include "Ui/KrtuworksUiLook.h"
#include "Ui/SKrtuworksAccount.h"
#include "Ui/SKrtuworksScreen.h"

#include <cstdint>
#include <memory>

namespace
{
	constexpr auto KrtuworksUiTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** How many widgets of `Type` sit in the tree under `Widget`, the widget itself included. */
	int32 CountOf(const TSharedRef<SWidget>& Widget, const FName Type)
	{
		int32 Count = Widget->GetType() == Type ? 1 : 0;
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			Count += CountOf(Children->GetChildAt(Index), Type);
		}
		return Count;
	}

	krtu::UiScreen ScreenOf(krtu::UiScreenKind Kind)
	{
		krtu::UiScreen Screen;
		Screen.kind = Kind;
		Screen.termsUrl = "https://id.krtuworks.com/legal/terms/latest";
		Screen.privacyUrl = "https://id.krtuworks.com/legal/privacy/latest";
		switch (Kind)
		{
		case krtu::UiScreenKind::Age:
			for (int Year = 2026; Year > 1906; --Year)
			{
				Screen.years.push_back(Year);
			}
			Screen.signIn = true;
			break;
		case krtu::UiScreenKind::Conflict:
			Screen.slot = "save-1";
			Screen.cards[0].label = "This device";
			Screen.cards[0].newer = true;
			Screen.cards[0].when = "Today";
			Screen.cards[0].size = "12 KB";
			Screen.cards[1].label = "Cloud";
			Screen.cards[1].when = "Yesterday";
			Screen.cards[1].size = "11 KB";
			break;
		case krtu::UiScreenKind::Error:
			Screen.message = "Something went wrong.";
			Screen.retry = true;
			break;
		case krtu::UiScreenKind::Waiting:
			Screen.url = "https://id.krtuworks.com/oidc/auth";
			break;
		case krtu::UiScreenKind::Device:
			Screen.code = "ABCD-EFGH-JK";
			Screen.uri = "https://id.krtuworks.com/link";
			Screen.uriComplete = "https://id.krtuworks.com/link?code=ABCD-EFGH-JK";
			Screen.qr = krtu::qrModules(Screen.uriComplete);
			Screen.expiresAtMs = static_cast<std::int64_t>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds()) + 600000;
			break;
		default:
			break;
		}
		return Screen;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKrtuworksUiScreensTest, "Krtuworks.Ui.ScreensDraw", KrtuworksUiTestFlags)

bool FKrtuworksUiScreensTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Slate is running"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	const std::shared_ptr<FKrtuworksUiLook> Look = std::make_shared<FKrtuworksUiLook>();
	const FName Button(TEXT("SKrtuworksButton"));
	const struct
	{
		krtu::UiScreenKind Kind;
		const TCHAR* Name;
		int32 Buttons;
	} Screens[] = {
		{krtu::UiScreenKind::Age, TEXT("age"), 2},
		{krtu::UiScreenKind::Terms, TEXT("terms"), 1},
		{krtu::UiScreenKind::Underage, TEXT("under 13"), 1},
		{krtu::UiScreenKind::Conflict, TEXT("save conflict"), 1},
		{krtu::UiScreenKind::Error, TEXT("error"), 1},
		{krtu::UiScreenKind::Waiting, TEXT("waiting"), 2},
		{krtu::UiScreenKind::Device, TEXT("console code"), 1},
	};
	for (const auto& Entry : Screens)
	{
		const TSharedRef<SKrtuworksScreen> Screen = SNew(SKrtuworksScreen).Look(Look).Screen(ScreenOf(Entry.Kind));
		const int32 Count = CountOf(Screen, Button);
		AddInfo(FString::Printf(TEXT("KRTU-UI %s buttons=%d"), Entry.Name, Count));
		TestTrue(FString::Printf(TEXT("the %s screen draws its buttons"), Entry.Name), Count >= Entry.Buttons);
		TestTrue(FString::Printf(TEXT("the %s screen has a control to focus"), Entry.Name), Screen->GetInitialFocus().IsValid());
	}
	const TSharedRef<SKrtuworksScreen> Conflict = SNew(SKrtuworksScreen).Look(Look).Screen(ScreenOf(krtu::UiScreenKind::Conflict));
	TestEqual(TEXT("the save conflict screen offers both saves"), CountOf(Conflict, FName(TEXT("SKrtuworksChoice"))), 2);
	krtu::UiScreen Expired = ScreenOf(krtu::UiScreenKind::Device);
	Expired.expired = true;
	const TSharedRef<SKrtuworksScreen> Device = SNew(SKrtuworksScreen).Look(Look).Screen(ScreenOf(krtu::UiScreenKind::Device));
	Device->SetScreen(Expired);
	TestTrue(TEXT("an expired code redraws in place with its buttons"), CountOf(Device, Button) >= 1);

	const TSharedRef<SKrtuworksReminder> Reminder = SNew(SKrtuworksReminder).Look(Look);
	TestTrue(TEXT("the corner reminder draws Link and Later"), CountOf(Reminder, Button) >= 2);
	const TSharedRef<SKrtuworksSignIn> SignIn = SNew(SKrtuworksSignIn).Look(Look);
	TestTrue(TEXT("the sign-in button draws"), SignIn->GetChildren() && SignIn->GetChildren()->Num() > 0);
	const TSharedRef<SKrtuworksAccountChip> Chip = SNew(SKrtuworksAccountChip).Look(Look);
	TestTrue(TEXT("the account chip builds while nobody plays"), Chip->GetChildren() != nullptr);
	return true;
}

#endif
