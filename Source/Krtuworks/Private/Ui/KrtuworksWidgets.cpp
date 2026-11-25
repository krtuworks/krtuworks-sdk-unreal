#include "KrtuworksWidgets.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "KrtuworksSubsystem.h"
#include "Ui/KrtuworksUiLook.h"
#include "Ui/SKrtuworksAccount.h"

#define LOCTEXT_NAMESPACE "Krtuworks"

namespace
{
	UKrtuworksSubsystem* SdkOf(const UWidget* Widget)
	{
		const UWorld* World = Widget->GetWorld();
		const UGameInstance* Instance = World != nullptr ? World->GetGameInstance() : nullptr;
		return Instance != nullptr ? Instance->GetSubsystem<UKrtuworksSubsystem>() : nullptr;
	}

	/** The look when the SDK runs, else one made from the plugin's settings (the widget designer has no game instance). */
	std::shared_ptr<FKrtuworksUiLook> LookOf(const UKrtuworksSubsystem* Sdk)
	{
		if (Sdk != nullptr && Sdk->GetUiLook())
		{
			return Sdk->GetUiLook();
		}
		const std::shared_ptr<FKrtuworksUiLook> Made = std::make_shared<FKrtuworksUiLook>();
		Made->Theme = GetDefault<UKrtuworksSettings>()->Theme;
		return Made;
	}
}

// ── UKrtuworksSignInButton ───────────────────────────────────────────────

TSharedRef<SWidget> UKrtuworksSignInButton::RebuildWidget()
{
	Button = SNew(SKrtuworksSignIn).Look(LookOf(SdkOf(this))).OnPressed(FKrtuworksChosen::CreateUObject(this, &UKrtuworksSignInButton::HandlePressed));
	return Button.ToSharedRef();
}

void UKrtuworksSignInButton::SynchronizeProperties()
{
	Super::SynchronizeProperties();
	const UKrtuworksSubsystem* Sdk = SdkOf(this);
	if (Button.IsValid() && Sdk != nullptr)
	{
		Button->SetVisibility(Sdk->IsSignInOffered() ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

void UKrtuworksSignInButton::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Button.Reset();
}

void UKrtuworksSignInButton::HandlePressed()
{
	UKrtuworksSubsystem* Sdk = SdkOf(this);
	if (Sdk == nullptr || bSigningIn)
	{
		return;
	}
	bSigningIn = true;
	TWeakObjectPtr<UKrtuworksSignInButton> Self(this);
	Sdk->SignInWithScreens(krtu::CancelToken(), [Self](const FKrtuworksScreenResult&) {
		if (Self.IsValid())
		{
			Self->bSigningIn = false;
		}
	});
}

#if WITH_EDITOR
const FText UKrtuworksSignInButton::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "Krtuworks");
}
#endif

// ── UKrtuworksAccountChip ────────────────────────────────────────────────

UKrtuworksSubsystem* UKrtuworksAccountChip::GetSdk() const
{
	return SdkOf(this);
}

TSharedRef<SWidget> UKrtuworksAccountChip::RebuildWidget()
{
	Chip = SNew(SKrtuworksAccountChip)
			   .Look(LookOf(GetSdk()))
			   .OnManage(FKrtuworksChosen::CreateLambda([]() { FPlatformProcess::LaunchURL(TEXT("https://id.krtuworks.com/"), nullptr, nullptr); }))
			   .OnSignOut(FKrtuworksChosen::CreateUObject(this, &UKrtuworksAccountChip::SignOut))
			   .OnLink(FKrtuworksChosen::CreateUObject(this, &UKrtuworksAccountChip::Link));
	if (UKrtuworksSubsystem* Sdk = GetSdk())
	{
		Sdk->OnIdentityChanged.AddUniqueDynamic(this, &UKrtuworksAccountChip::HandleIdentityChanged);
	}
	Refresh();
	return Chip.ToSharedRef();
}

void UKrtuworksAccountChip::SynchronizeProperties()
{
	Super::SynchronizeProperties();
	Refresh();
}

void UKrtuworksAccountChip::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	if (UKrtuworksSubsystem* Sdk = GetSdk())
	{
		Sdk->OnIdentityChanged.RemoveDynamic(this, &UKrtuworksAccountChip::HandleIdentityChanged);
	}
	Chip.Reset();
}

void UKrtuworksAccountChip::HandleIdentityChanged(const FKrtuworksIdentityChange&)
{
	Refresh();
}

void UKrtuworksAccountChip::Refresh()
{
	const UKrtuworksSubsystem* Sdk = GetSdk();
	if (Chip.IsValid() && Sdk != nullptr)
	{
		Chip->SetPlayer(Sdk->GetPlayer());
	}
}

void UKrtuworksAccountChip::SignOut()
{
	UKrtuworksSubsystem* Sdk = GetSdk();
	if (Sdk == nullptr)
	{
		return;
	}
	Sdk->RunErased([](krtu::Client& C) { C.signOut(); }, []() {}, [](const FKrtuworksError& Error) { UE_LOG(LogTemp, Warning, TEXT("Krtuworks: signing out failed: %s (%s)"), *Error.Message, *Error.Code); });
}

void UKrtuworksAccountChip::Link()
{
	UKrtuworksSubsystem* Sdk = GetSdk();
	if (Sdk == nullptr || bSigningIn)
	{
		return;
	}
	bSigningIn = true;
	TWeakObjectPtr<UKrtuworksAccountChip> Self(this);
	Sdk->SignInWithScreens(krtu::CancelToken(), [Self](const FKrtuworksScreenResult&) {
		if (Self.IsValid())
		{
			Self->bSigningIn = false;
		}
	});
}

#if WITH_EDITOR
const FText UKrtuworksAccountChip::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "Krtuworks");
}
#endif

#undef LOCTEXT_NAMESPACE
