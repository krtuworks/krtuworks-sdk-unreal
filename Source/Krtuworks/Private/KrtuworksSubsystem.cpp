#include "KrtuworksSubsystem.h"

#include "Async/Async.h"
#include "GeneralProjectSettings.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Kismet/GameplayStatics.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Store.h"
#include "KrtuworksCore/Validator.h"
#include "KrtuworksPlatform.h"
#include "KrtuworksSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ui/KrtuworksUiHost.h"
#include "Ui/KrtuworksUiLook.h"
#include "UnrealHttpTransport.h"

#include <chrono>
#include <future>
#include <thread>

using KrtuworksConvert::ToFString;
using KrtuworksConvert::ToStd;

namespace
{
	TFunction<std::shared_ptr<krtu::PlatformServices>()>& PlatformServicesFactory()
	{
		static TFunction<std::shared_ptr<krtu::PlatformServices>()> Factory;
		return Factory;
	}

	/** Quitting waits this long for the session's end and the queues' last write (then the game goes anyway). */
	constexpr auto ShutdownWait = std::chrono::seconds(2);

	std::optional<std::string> Clip(const FString& Text, int32 Max)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return std::nullopt;
		}
		return ToStd(Trimmed.Left(Max));
	}

	krtu::DeviceInfo DeviceDescription()
	{
		krtu::DeviceInfo Device;
		Device.os = KrtuworksPlatform::OsName();
		Device.osVersion = Clip(FPlatformMisc::GetOSVersion(), 40);
		Device.model = Clip(FPlatformMisc::GetDeviceMakeAndModel(), 120);
		Device.gpu = Clip(FPlatformMisc::GetPrimaryGPUBrand(), 120);
		const uint32 RamGb = FPlatformMemory::GetPhysicalGBRam();
		if (RamGb > 0)
		{
			Device.ramMb = static_cast<int>(RamGb) * 1024;
		}
		FDisplayMetrics Metrics;
		FDisplayMetrics::RebuildDisplayMetrics(Metrics);
		if (Metrics.PrimaryDisplayWidth > 0 && Metrics.PrimaryDisplayHeight > 0)
		{
			Device.screenWidth = Metrics.PrimaryDisplayWidth;
			Device.screenHeight = Metrics.PrimaryDisplayHeight;
		}
		return Device;
	}

	std::string EnvironmentOf(const UKrtuworksSettings& Settings)
	{
		switch (Settings.Environment)
		{
		case EKrtuworksEnvironment::Test:
			return "TEST";
		case EKrtuworksEnvironment::Live:
			return "LIVE";
		default:
#if UE_BUILD_SHIPPING
			return "LIVE";
#else
			return "TEST";
#endif
		}
	}

	bool IsConsole()
	{
		const FString Name = UGameplayStatics::GetPlatformName();
		return Name == FString(TEXT("PS4")) || Name == FString(TEXT("PS5")) || Name == FString(TEXT("XboxOne")) || Name == FString(TEXT("XSX")) || Name == FString(TEXT("Switch"));
	}

	/** The mode the settings say; consoles sign in with a code, iOS and Android with the system sheet, everything else with the browser. */
	krtu::UiSignInMode ResolveSignInMode(EKrtuworksSignInMode Mode)
	{
		switch (Mode)
		{
		case EKrtuworksSignInMode::Browser:
			return krtu::UiSignInMode::Browser;
		case EKrtuworksSignInMode::App:
			return krtu::UiSignInMode::App;
		case EKrtuworksSignInMode::Device:
			return krtu::UiSignInMode::Device;
		case EKrtuworksSignInMode::None:
			return krtu::UiSignInMode::None;
		default:
			if (KrtuworksPlatform::SignsInOnPhone())
			{
				return krtu::UiSignInMode::App;
			}
			return IsConsole() ? krtu::UiSignInMode::Device : krtu::UiSignInMode::Browser;
		}
	}

	FKrtuworksScreenResult ToScreenResult(const krtu::UiResult& Result)
	{
		FKrtuworksScreenResult Out;
		Out.bSignedIn = Result.status == krtu::UiStatus::SignedIn;
		Out.bCancelled = Result.status == krtu::UiStatus::Cancelled;
		if (Result.status == krtu::UiStatus::Offline)
		{
			Out.OfflineReason = Result.reason == krtu::UiOfflineReason::Underage ? TEXT("underage") : TEXT("unavailable");
		}
		Out.Player = KrtuworksConvert::ToPlayer(Result.player);
		return Out;
	}

	FKrtuworksScreenResult Unavailable()
	{
		FKrtuworksScreenResult Out;
		Out.OfflineReason = TEXT("unavailable");
		return Out;
	}

	/** The SDK's own folder under the game's saved data, as an absolute UTF-8 path. */
	std::string SdkFolder()
	{
		const FString Relative = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Krtuworks"));
		return ToStd(IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*Relative));
	}
}

void UKrtuworksSubsystem::SetPlatformServicesFactory(TFunction<std::shared_ptr<krtu::PlatformServices>()> Factory)
{
	PlatformServicesFactory() = MoveTemp(Factory);
}

void UKrtuworksSubsystem::RunErased(TFunction<void(krtu::Client&)> Work, TFunction<void()> OnDone, TFunction<void(const FKrtuworksError&)> OnFailure)
{
	std::shared_ptr<krtu::Client> Held = Client;
	if (!Held)
	{
		FKrtuworksError Error;
		Error.Code = TEXT("not_ready");
		Error.Message = TEXT("The Krtuworks SDK did not start: see the log (krtuworks-config.json).");
		OnFailure(Error);
		return;
	}
	Async(EAsyncExecution::Thread, [Held, Work = MoveTemp(Work), OnDone = MoveTemp(OnDone), OnFailure = MoveTemp(OnFailure)]() mutable {
		bool bDone = false;
		FKrtuworksError Error;
		try
		{
			Work(*Held);
			bDone = true;
		}
		catch (...)
		{
			Error = KrtuworksConvert::CurrentError();
		}
		AsyncTask(ENamedThreads::GameThread, [bDone, Error = MoveTemp(Error), OnDone = MoveTemp(OnDone), OnFailure = MoveTemp(OnFailure)]() {
			if (bDone)
			{
				OnDone();
			}
			else
			{
				OnFailure(Error);
			}
		});
	});
}

void UKrtuworksSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Writes = MakeUnique<UE::Tasks::FPipe>(TEXT("KrtuworksWrites"));
	const UKrtuworksSettings& Settings = *GetDefault<UKrtuworksSettings>();
	FString ConfigText;
	const bool bHasConfig = FFileHelper::LoadFileToString(ConfigText, *KrtuworksConvert::ConfigFilePath());
	if (!bHasConfig)
	{
		UE_LOG(LogKrtuworks, Error, TEXT("Krtuworks SDK: no krtuworks-config.json at %s; download it from the partner console's Credentials page."), *KrtuworksConvert::ConfigFilePath());
	}
	try
	{
		krtu::Options Options;
		if (PlatformServicesFactory())
		{
			Options.platformServices = PlatformServicesFactory()();
		}
		Options.platform = Options.platformServices ? "STEAM" : KrtuworksPlatform::PlatformName();
		Options.build = ToStd(GetDefault<UGeneralProjectSettings>()->ProjectVersion);
		// What the validator checks, even when the config is missing or does not read.
		ValidatorOptions = Options;
		if (!bHasConfig)
		{
			return;
		}
		Options.config = krtu::AppConfig::fromJson(ToStd(ConfigText));
		ValidatorOptions = Options;
		Options.environment = EnvironmentOf(Settings);
		const std::string Root = SdkFolder();
		Options.store = std::make_shared<krtu::FileStore>(std::make_shared<krtu::StdFileSystem>(Root + "/store"));
		UiStore = Options.store;
		Options.secrets = KrtuworksPlatform::MakeSecretStore(Root, Options.config.appId);
		Options.http = std::make_shared<FUnrealHttpTransport>();
		Options.device = DeviceDescription();
		Options.locale = ToStd(FInternationalization::Get().GetCurrentLanguage()->GetName());
		Options.sessions = Settings.bSessions;
		Options.positionSampleSeconds = Settings.PositionSampleSeconds;
		ValidatorOptions = Options;
		Client = std::shared_ptr<krtu::Client>(krtu::Client::initialize(MoveTemp(Options)));
	}
	catch (...)
	{
		const FKrtuworksError Error = KrtuworksConvert::CurrentError();
		UE_LOG(LogKrtuworks, Error, TEXT("Krtuworks SDK did not start: %s (%s)"), *Error.Message, *Error.Code);
		Client.reset();
		return;
	}
	UE_LOG(LogKrtuworks, Log, TEXT("Krtuworks SDK started (%s, %s)."), *ToFString(Client->appType()), *ToFString(Client->environment()));

	BuildUi();

	TWeakObjectPtr<UKrtuworksSubsystem> Self(this);
	Client->setIdentityChangedHandler([Self](const krtu::IdentityChange& Change) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = KrtuworksConvert::ToChange(Change)]() {
			if (Self.IsValid())
			{
				Self->OnIdentityChanged.Broadcast(Converted);
			}
		});
	});
	Client->setMergeSettledHandler([Self](const krtu::MergeSettlement& Settled) {
		AsyncTask(ENamedThreads::GameThread, [Self, Previous = ToFString(Settled.previousSubject), Waiting = static_cast<int32>(Settled.waitingSaves.size())]() {
			if (Self.IsValid())
			{
				Self->OnMergeSettled.Broadcast(Previous, Waiting);
			}
		});
	});
	const std::shared_ptr<FKrtuworksUiSlot> Slot = UiSlot;
	Client->setLinkReminderHandler([Self, Slot](const std::string& Reason) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = ToFString(Reason)]() {
			if (Self.IsValid())
			{
				Self->OnLinkReminder.Broadcast(Converted);
			}
		});
		// The ready-made corner prompt waits for the player's answer: not on the SDK's thread.
		Async(EAsyncExecution::Thread, [Slot]() {
			if (const std::shared_ptr<krtu::UiController> Ui = Slot->Get())
			{
				Ui->remind();
			}
		});
	});
	Client->setBugFixedHandler([Self](const krtu::BugReportView& Report) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = KrtuworksConvert::ToBugReport(Report)]() {
			if (Self.IsValid())
			{
				Self->OnBugFixed.Broadcast(Converted);
			}
		});
	});
	Client->setPlatformLinkNoticeHandler([Self](const krtu::PlatformLink& Link) {
		AsyncTask(ENamedThreads::GameThread, [Self, AccountId = ToFString(Link.platformAccountId), State = ToFString(Link.state)]() {
			if (Self.IsValid())
			{
				Self->OnPlatformLinkNotice.Broadcast(AccountId, State);
			}
		});
	});
	Client->setPlatformSaveConflictHandler([Self, Slot](const krtu::PlatformSaveConflict& Conflict) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = KrtuworksConvert::ToPlatformSaveConflict(Conflict)]() {
			if (Self.IsValid())
			{
				Self->OnPlatformSaveConflict.Broadcast(Converted);
			}
		});
		Async(EAsyncExecution::Thread, [Slot, Conflict]() {
			if (const std::shared_ptr<krtu::UiController> Ui = Slot->Get())
			{
				// Nobody waits on this thread: an error escaping it would end the game.
				try
				{
					Ui->resolvePlatformSaveConflict(Conflict);
				}
				catch (const std::exception& Err)
				{
					UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks SDK: the Steam Cloud save choice did not go through: %s"), UTF8_TO_TCHAR(Err.what()));
				}
			}
		});
	});
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UKrtuworksSubsystem::OnEnterBackground);
	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UKrtuworksSubsystem::OnEnterForeground);

	// The system closed the app while the sign-in page was open; the page's return address started it again.
	if (const std::optional<std::string> Launch = KrtuworksPlatform::LaunchAddress())
	{
		if (Launch->rfind(Client->appScheme() + ":", 0) == 0)
		{
			const std::string Address = *Launch;
			Run<bool>(
				[Address](krtu::Client& C) {
					C.completeSignIn(Address);
					return true;
				},
				[](const bool&) {},
				[](const FKrtuworksError& Error) { UE_LOG(LogKrtuworks, Log, TEXT("Krtuworks: the sign-in that started the app did not finish (%s)."), *Error.Code); });
		}
	}
}

void UKrtuworksSubsystem::BuildUi()
{
	if (!Client || !UiStore)
	{
		return;
	}
	const UKrtuworksSettings& Settings = *GetDefault<UKrtuworksSettings>();
	if (!UiSlot)
	{
		UiSlot = std::make_shared<FKrtuworksUiSlot>();
	}
	if (!UiLook)
	{
		UiLook = std::make_shared<FKrtuworksUiLook>();
		UiLook->Theme = Settings.Theme;
		for (const TPair<FName, FText>& Override : Settings.TextOverrides)
		{
			UiLook->Texts[ToStd(Override.Key.ToString())] = ToStd(Override.Value.ToString());
		}
		UiLook->bControllerHints = Settings.ControllerHints == EKrtuworksControllerHints::Always || (Settings.ControllerHints == EKrtuworksControllerHints::Automatic && IsConsole());
	}
	if (!UiHost)
	{
		UiHost = KrtuworksUi::MakeSlateHost(UiLook);
	}
	krtu::UiOptions Options;
	Options.texts = UiLook->Texts;
	Options.locale = ToStd(FInternationalization::Get().GetCurrentLanguage()->GetName());
	Options.guests = Settings.bGuests;
	Options.linkReminder = Settings.bLinkReminder;
	Options.platformSaveConflicts = Settings.bPlatformSaveConflicts;
	Options.signInMode = ResolveSignInMode(Settings.SignInMode);
	Options.openUrl = [](const std::string& Url) {
		// The browser opens from the game thread (macOS and Android want it there).
		AsyncTask(ENamedThreads::GameThread, [Address = ToFString(Url)]() { FPlatformProcess::LaunchURL(*Address, nullptr, nullptr); });
	};
	Options.authenticate = &KrtuworksPlatform::AuthenticateOnPhone;
	Options.deviceName = Clip(FPlatformMisc::GetDeviceMakeAndModel(), 60);
	const std::shared_ptr<FKrtuworksUiSlot> Slot = UiSlot;
	Options.describeSave = [Slot](const std::string& SlotKey, const std::string& Bytes) { return Slot->Describe(SlotKey, Bytes); };
	Options.memory = UiStore;
	UiSlot->Set(std::make_shared<krtu::UiController>(Client, UiHost, std::move(Options)));
}

void UKrtuworksSubsystem::RunUi(TFunction<krtu::UiResult(krtu::UiController&)> Work, TFunction<void(const FKrtuworksScreenResult&)> OnDone)
{
	const std::shared_ptr<krtu::UiController> Ui = UiSlot ? UiSlot->Get() : nullptr;
	if (!Ui)
	{
		OnDone(Unavailable());
		return;
	}
	Run<krtu::UiResult>(
		[Ui, Work = MoveTemp(Work)](krtu::Client&) { return Work(*Ui); },
		[OnDone](const krtu::UiResult& Result) { OnDone(ToScreenResult(Result)); },
		[OnDone](const FKrtuworksError& Error) {
			UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks: the ready-made screens stopped: %s (%s)"), *Error.Message, *Error.Code);
			OnDone(Unavailable());
		});
}

void UKrtuworksSubsystem::EnsurePlayer(TFunction<void(const FKrtuworksScreenResult&)> OnDone)
{
	RunUi([](krtu::UiController& Ui) { return Ui.ensurePlayer(); }, MoveTemp(OnDone));
}

void UKrtuworksSubsystem::SignInWithScreens(const krtu::CancelToken& Cancel, TFunction<void(const FKrtuworksScreenResult&)> OnDone)
{
	RunUi([Cancel](krtu::UiController& Ui) { return Ui.signIn(Cancel); }, MoveTemp(OnDone));
}

bool UKrtuworksSubsystem::IsSignInOffered() const
{
	const std::shared_ptr<krtu::UiController> Ui = UiSlot ? UiSlot->Get() : nullptr;
	return Ui && Ui->signInOffered();
}

std::shared_ptr<krtu::UiController> UKrtuworksSubsystem::GetUi() const
{
	return UiSlot ? UiSlot->Get() : nullptr;
}

void UKrtuworksSubsystem::SetUiTheme(const FKrtuworksUiTheme& Theme)
{
	if (UiLook)
	{
		UiLook->Theme = Theme;
	}
}

void UKrtuworksSubsystem::SetUiText(FName Key, const FText& Text)
{
	if (!UiLook)
	{
		return;
	}
	const std::string Name = ToStd(Key.ToString());
	const std::string Value = ToStd(Text.ToString());
	if (Value.empty())
	{
		UiLook->Texts.erase(Name);
	}
	else
	{
		UiLook->Texts[Name] = Value;
	}
	// The controller words its own sentences (a ban's end, a save's time) from the same table: a new one takes over, a call in flight finishes with the old.
	BuildUi();
}

void UKrtuworksSubsystem::SetSaveDescriber(std::function<std::optional<krtu::SaveSummary>(const std::string&, const std::string&)> Describer)
{
	if (UiSlot)
	{
		UiSlot->SetDescriber(std::move(Describer));
	}
}

void UKrtuworksSubsystem::Validate(TFunction<void(const FKrtuworksValidationReport&)> OnDone)
{
	const krtu::Options Options = ValidatorOptions.value_or(krtu::Options());
	// The round waits on the network: a thread of its own, then the report on the game thread.
	Async(EAsyncExecution::Thread, [Options, OnDone = MoveTemp(OnDone)]() mutable {
		FKrtuworksValidationReport Report = KrtuworksConvert::ToValidationReport(krtu::validate(Options));
		AsyncTask(ENamedThreads::GameThread, [Report = MoveTemp(Report), OnDone = MoveTemp(OnDone)]() {
			for (const FKrtuworksValidationCheck& Check : Report.Checks)
			{
				const FString Why = Check.Detail.IsEmpty() ? FString() : FString::Printf(TEXT(": %s"), *Check.Detail);
				const FString Line = FString::Printf(TEXT("Krtuworks validator %s: %s %s (%s)%s"), *Report.Build, *Check.Status, *Check.Key, *Check.Title, *Why);
				if (Check.Status == TEXT("FAILED"))
				{
					UE_LOG(LogKrtuworks, Error, TEXT("%s"), *Line);
				}
				else if (Check.Status == TEXT("SKIPPED"))
				{
					UE_LOG(LogKrtuworks, Warning, TEXT("%s"), *Line);
				}
				else
				{
					UE_LOG(LogKrtuworks, Log, TEXT("%s"), *Line);
				}
			}
			UE_LOG(LogKrtuworks, Log, TEXT("Krtuworks validator: %s %s."), *Report.Build, Report.bOk ? TEXT("passed") : TEXT("has failed checks"));
			OnDone(Report);
		});
	});
}

void UKrtuworksSubsystem::Deinitialize()
{
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	if (Writes)
	{
		Writes->WaitUntilEmpty();
	}
	if (UiHost)
	{
		// Anyone waiting on a screen is let go before the client shuts down.
		UiHost->Shutdown();
	}
	if (UiSlot)
	{
		UiSlot->Set(nullptr);
	}
	if (const std::shared_ptr<krtu::Client> Held = MoveTemp(Client))
	{
		Held->setIdentityChangedHandler(nullptr);
		Held->setMergeSettledHandler(nullptr);
		Held->setLinkReminderHandler(nullptr);
		Held->setBugFixedHandler(nullptr);
		Held->setPlatformLinkNoticeHandler(nullptr);
		Held->setPlatformSaveConflictHandler(nullptr);
		// The shutdown ends the session over the network: it goes on its own thread, which keeps the client
		// alive until it is done, and quitting waits for it at most a moment.
		const std::shared_ptr<std::promise<void>> Done = std::make_shared<std::promise<void>>();
		std::future<void> Finished = Done->get_future();
		std::thread([Held, Done]() {
			try
			{
				Held->shutdown();
			}
			catch (...)
			{
				// Quitting anyway: what waits stays in the store for the next launch.
			}
			Done->set_value();
		}).detach();
		Finished.wait_for(ShutdownWait);
	}
	Super::Deinitialize();
}

bool UKrtuworksSubsystem::IsReady() const
{
	return Client != nullptr;
}

FKrtuworksPlayer UKrtuworksSubsystem::GetPlayer() const
{
	return Client ? KrtuworksConvert::ToPlayer(Client->player()) : FKrtuworksPlayer();
}

FString UKrtuworksSubsystem::GetEnvironment() const
{
	return Client ? ToFString(Client->environment()) : FString();
}

FString UKrtuworksSubsystem::GetAppType() const
{
	return Client ? ToFString(Client->appType()) : FString();
}

bool UKrtuworksSubsystem::Track(const FString& Type, const FKrtuworksEventData& Data)
{
	if (!Client)
	{
		return false;
	}
	try
	{
		return Client->track(ToStd(Type), KrtuworksConvert::ToJson(Data));
	}
	catch (...)
	{
		UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks: event \"%s\" not kept: %s"), *Type, *KrtuworksConvert::CurrentError().Message);
		return false;
	}
}

bool UKrtuworksSubsystem::TrackAt(const FString& Type, const FKrtuworksEventData& Data, const FKrtuworksPosition& Position)
{
	if (!Client)
	{
		return false;
	}
	try
	{
		return Client->track(ToStd(Type), KrtuworksConvert::ToJson(Data), KrtuworksConvert::ToPosition(Position));
	}
	catch (...)
	{
		UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks: event \"%s\" not kept: %s"), *Type, *KrtuworksConvert::CurrentError().Message);
		return false;
	}
}

void UKrtuworksSubsystem::EnqueueWrite(TFunction<void(krtu::Client&)> Write)
{
	std::shared_ptr<krtu::Client> Held = Client;
	if (!Held || !Writes)
	{
		return;
	}
	Writes->Launch(TEXT("KrtuworksWrite"), [Held, Write = MoveTemp(Write)]() {
		try
		{
			Write(*Held);
		}
		catch (...)
		{
			const FKrtuworksError Error = KrtuworksConvert::CurrentError();
			UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks: %s (%s)"), *Error.Message, *Error.Code);
		}
	});
}

void UKrtuworksSubsystem::LevelStart(const FKrtuworksLevel& Level)
{
	EnqueueWrite([Ref = KrtuworksConvert::ToLevel(Level)](krtu::Client& C) { C.levelStart(Ref); });
}

void UKrtuworksSubsystem::LevelComplete(bool bHasScore, double Score)
{
	EnqueueWrite([bHasScore, Score](krtu::Client& C) { C.levelComplete(bHasScore ? std::optional<double>(Score) : std::nullopt); });
}

void UKrtuworksSubsystem::LevelFail(bool bHasScore, double Score)
{
	EnqueueWrite([bHasScore, Score](krtu::Client& C) { C.levelFail(bHasScore ? std::optional<double>(Score) : std::nullopt); });
}

void UKrtuworksSubsystem::LevelQuit()
{
	EnqueueWrite([](krtu::Client& C) { C.levelQuit(); });
}

void UKrtuworksSubsystem::LevelCheckpoint(const FString& Name)
{
	EnqueueWrite([Checkpoint = ToStd(Name)](krtu::Client& C) { C.levelCheckpoint(Checkpoint); });
}

void UKrtuworksSubsystem::LevelRestart()
{
	EnqueueWrite([](krtu::Client& C) { C.levelRestart(); });
}

void UKrtuworksSubsystem::Death(const FString& Cause, const FString& Enemy, bool bHasPosition, const FKrtuworksPosition& Position)
{
	std::optional<std::string> EnemyName;
	if (!Enemy.IsEmpty())
	{
		EnemyName = ToStd(Enemy);
	}
	std::optional<krtu::EventPosition> Place;
	if (bHasPosition)
	{
		Place = KrtuworksConvert::ToPosition(Position);
	}
	EnqueueWrite([CauseName = ToStd(Cause), EnemyName, Place](krtu::Client& C) { C.death(CauseName, EnemyName, Place); });
}

void UKrtuworksSubsystem::SamplePosition(const FKrtuworksPosition& Position)
{
	// Called every frame: the core's active clock moves in whole seconds, so one look a second is all it can use.
	const double Now = FPlatformTime::Seconds();
	if (Now < NextPositionLook)
	{
		return;
	}
	NextPositionLook = Now + 1.0;
	EnqueueWrite([Place = KrtuworksConvert::ToPosition(Position)](krtu::Client& C) { C.samplePosition(Place); });
}

void UKrtuworksSubsystem::ReportError(const FString& Message, const FString& Kind, const FString& Fingerprint, bool bHandled)
{
	std::optional<std::string> Print;
	if (!Fingerprint.IsEmpty())
	{
		Print = ToStd(Fingerprint);
	}
	EnqueueWrite([Text = ToStd(Message), KindName = ToStd(Kind), Print, bHandled](krtu::Client& C) { C.error(Text, KindName, Print, bHandled); });
}

void UKrtuworksSubsystem::Log(const FString& Message)
{
	EnqueueWrite([Text = ToStd(Message)](krtu::Client& C) { C.log(Text); });
}

void UKrtuworksSubsystem::IncrementStat(const FString& Key, double By)
{
	EnqueueWrite([StatKey = ToStd(Key), By](krtu::Client& C) { C.incrementStat(StatKey, By); });
}

void UKrtuworksSubsystem::SetStat(const FString& Key, double Value)
{
	EnqueueWrite([StatKey = ToStd(Key), Value](krtu::Client& C) { C.setStat(StatKey, Value); });
}

void UKrtuworksSubsystem::UnlockAchievement(const FString& Key)
{
	EnqueueWrite([AchievementKey = ToStd(Key)](krtu::Client& C) { C.unlockAchievement(AchievementKey); });
}

void UKrtuworksSubsystem::SubmitScore(const FString& Key, int64 Value, const TArray<uint8>& Details)
{
	std::optional<std::string> Bytes;
	if (Details.Num() > 0)
	{
		Bytes = KrtuworksConvert::ToBytes(Details);
	}
	EnqueueWrite([BoardKey = ToStd(Key), Value, Bytes](krtu::Client& C) { C.submitScore(BoardKey, Value, Bytes); });
}

void UKrtuworksSubsystem::ClearPresence()
{
	EnqueueWrite([](krtu::Client& C) { C.clearPresence(); });
}

void UKrtuworksSubsystem::SetPresenceGroup(const FString& GroupId, int32 Size)
{
	std::optional<std::string> Group;
	std::optional<int> Count;
	if (!GroupId.IsEmpty())
	{
		Group = ToStd(GroupId);
		if (Size > 0)
		{
			Count = static_cast<int>(Size);
		}
	}
	EnqueueWrite([Group, Count](krtu::Client& C) { C.setPresenceGroup(Group, Count); });
}

FKrtuworksGate UKrtuworksSubsystem::GetGate() const
{
	return Client ? KrtuworksConvert::ToGate(Client->getGate()) : FKrtuworksGate();
}

TMap<FString, bool> UKrtuworksSubsystem::GetModuleStates() const
{
	TMap<FString, bool> States;
	if (Client)
	{
		for (const auto& Entry : Client->moduleStates())
		{
			States.Add(ToFString(Entry.first), Entry.second);
		}
	}
	return States;
}

FString UKrtuworksSubsystem::GetConfigString(const FString& Key, const FString& Fallback) const
{
	return Client ? ToFString(Client->value<std::string>(ToStd(Key), ToStd(Fallback))) : Fallback;
}

double UKrtuworksSubsystem::GetConfigNumber(const FString& Key, double Fallback) const
{
	return Client ? Client->value<double>(ToStd(Key), Fallback) : Fallback;
}

bool UKrtuworksSubsystem::GetConfigBool(const FString& Key, bool Fallback) const
{
	return Client ? Client->value<bool>(ToStd(Key), Fallback) : Fallback;
}

FString UKrtuworksSubsystem::GetConfigJson(const FString& Key) const
{
	return Client ? ToFString(Client->value(ToStd(Key)).dump()) : FString(TEXT("null"));
}

void UKrtuworksSubsystem::OnEnterBackground()
{
	if (Client)
	{
		Client->setForeground(false);
	}
}

void UKrtuworksSubsystem::OnEnterForeground()
{
	if (Client)
	{
		Client->setForeground(true);
	}
	KrtuworksPlatform::Resumed();
}
