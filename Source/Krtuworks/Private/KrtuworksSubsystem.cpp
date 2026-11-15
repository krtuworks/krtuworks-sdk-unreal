#include "KrtuworksSubsystem.h"

#include "Async/Async.h"
#include "GeneralProjectSettings.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Store.h"
#include "KrtuworksCore/Validator.h"
#include "KrtuworksPlatform.h"
#include "KrtuworksSettings.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
		Options.secrets = KrtuworksPlatform::MakeSecretStore(Root, Options.config.appId);
		Options.http = std::make_shared<FUnrealHttpTransport>();
		Options.device = DeviceDescription();
		Options.locale = ToStd(FInternationalization::Get().GetCurrentLanguage()->GetName());
		Options.sessions = Settings.bSessions;
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
	Client->setLinkReminderHandler([Self](const std::string& Reason) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = ToFString(Reason)]() {
			if (Self.IsValid())
			{
				Self->OnLinkReminder.Broadcast(Converted);
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
	Client->setPlatformSaveConflictHandler([Self](const krtu::PlatformSaveConflict& Conflict) {
		AsyncTask(ENamedThreads::GameThread, [Self, Converted = KrtuworksConvert::ToPlatformSaveConflict(Conflict)]() {
			if (Self.IsValid())
			{
				Self->OnPlatformSaveConflict.Broadcast(Converted);
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
