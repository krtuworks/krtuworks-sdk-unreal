#include "KrtuworksNodes.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Client.h"
#include "KrtuworksCore/Crypto.h"
#include "KrtuworksPlatform.h"
#include "KrtuworksScreenshot.h"
#include "KrtuworksSubsystem.h"

#include <stdexcept>

using KrtuworksConvert::ToFString;
using KrtuworksConvert::ToStd;

namespace
{
	krtu::Json VersionJson(const krtu::FileVersion& Version)
	{
		krtu::Json Out = krtu::Json::object({{"id", Version.id}, {"slot", Version.slot}, {"generation", Version.generation}, {"sizeBytes", Version.sizeBytes}, {"sha256", Version.sha256}, {"savedAt", Version.savedAt}, {"fromMerge", Version.fromMerge}});
		if (Version.device)
		{
			Out["device"] = *Version.device;
		}
		if (Version.platformCloud)
		{
			Out["platformCloud"] = *Version.platformCloud;
		}
		return Out;
	}

	krtu::Json SlotsJson(const krtu::SlotList& List)
	{
		krtu::Json Slots = krtu::Json::array();
		for (const krtu::SlotView& Slot : List.slots)
		{
			krtu::Json Devices = krtu::Json::array();
			for (const krtu::FileVersion& Device : Slot.devices)
			{
				Devices.push(VersionJson(Device));
			}
			Slots.push(krtu::Json::object({{"key", Slot.key},
				{"kind", Slot.kind},
				{"name", Slot.name},
				{"retired", Slot.retired},
				{"platformMirror", Slot.platformMirror},
				{"file", Slot.file ? VersionJson(*Slot.file) : krtu::Json()},
				{"conflict", Slot.conflict ? VersionJson(*Slot.conflict) : krtu::Json()},
				{"devices", Devices}}));
		}
		krtu::Json Limits = krtu::Json::object();
		for (const auto& Limit : List.limits)
		{
			Limits[Limit.first] = Limit.second;
		}
		return krtu::Json::object({{"slots", Slots}, {"limits", Limits}});
	}

	/** The nodes' {platformLink: {platform, platformAccountId, state}} answer. */
	krtu::Json PlatformLinkJson(const krtu::PlatformLink& Link)
	{
		return krtu::Json::object({{"platformLink", krtu::Json::object({{"platform", Link.platform}, {"platformAccountId", Link.platformAccountId}, {"state", Link.state}})}});
	}

	std::optional<std::string> OptionalString(const krtu::Json& Arguments, const char* Name)
	{
		return Arguments.getString(Name);
	}

	std::string RequiredString(const krtu::Json& Arguments, const char* Name)
	{
		const std::optional<std::string> Value = Arguments.getString(Name);
		if (!Value)
		{
			throw std::invalid_argument(std::string("the arguments need \"") + Name + "\"");
		}
		return *Value;
	}

	/** The named call with its JSON arguments; the result as JSON. */
	krtu::Json RunOperation(krtu::Client& Client, const std::string& Operation, const krtu::Json& Arguments)
	{
		if (Operation == "listFiles")
		{
			return SlotsJson(Client.listFiles());
		}
		if (Operation == "saveSettings")
		{
			std::optional<krtu::SaveDevice> Device;
			if (const krtu::Json* Given = Arguments.find("device"); Given != nullptr && Given->isObject())
			{
				krtu::SaveDevice Described;
				Described.id = RequiredString(*Given, "id");
				for (const auto& Member : (*Given)["info"].members())
				{
					if (Member.second.isString())
					{
						Described.info[Member.first] = Member.second.asString();
					}
				}
				Device = Described;
			}
			return VersionJson(Client.saveSettings(RequiredString(Arguments, "slot"), krtu::fromBase64(RequiredString(Arguments, "base64")), Device));
		}
		if (Operation == "resolveFile")
		{
			return VersionJson(Client.resolveFile(RequiredString(Arguments, "slot"), RequiredString(Arguments, "keep"), RequiredString(Arguments, "current")));
		}
		if (Operation == "waitingSaves")
		{
			krtu::Json Saves = krtu::Json::array();
			for (const krtu::WaitingSave& Save : Client.waitingSaves(RequiredString(Arguments, "owner")))
			{
				Saves.push(krtu::Json::object({{"uploadId", Save.uploadId}, {"slot", Save.slot}, {"savedAt", Save.savedAt}}));
			}
			return Saves;
		}
		if (Operation == "adoptWaitingSave")
		{
			return VersionJson(Client.adoptWaitingSave(RequiredString(Arguments, "uploadId"), OptionalString(Arguments, "baseVersion")));
		}
		if (Operation == "discardWaitingSave")
		{
			Client.discardWaitingSave(RequiredString(Arguments, "uploadId"));
			return krtu::Json::object();
		}
		if (Operation == "getProfile" || Operation == "updateProfile")
		{
			const krtu::PlayerProfile Profile = Operation == "getProfile" ? Client.getProfile() : Client.updateProfile(Arguments["fields"]);
			krtu::Json Fields = krtu::Json::object();
			for (const auto& Field : Profile.fields)
			{
				Fields[Field.first] = Field.second;
			}
			return krtu::Json::object({{"fields", Fields}, {"updatedAt", Profile.updatedAt ? krtu::Json(*Profile.updatedAt) : krtu::Json()}});
		}
		if (Operation == "refreshExperiments")
		{
			krtu::Json List = krtu::Json::array();
			for (const krtu::Experiment& Experiment : Client.refreshExperiments())
			{
				List.push(krtu::Json::object({{"key", Experiment.key}, {"configKey", Experiment.configKey}, {"group", Experiment.group}, {"value", Experiment.value}}));
			}
			return List;
		}
		if (Operation == "getStatus")
		{
			return Client.getStatus();
		}
		if (Operation == "getTelemetryOptOut")
		{
			return krtu::Json::object({{"optedOut", Client.getTelemetryOptOut()}});
		}
		if (Operation == "setTelemetryOptOut")
		{
			const std::optional<bool> OptOut = Arguments.getBool("optOut");
			if (!OptOut)
			{
				throw std::invalid_argument("the arguments need \"optOut\" (true or false)");
			}
			Client.setTelemetryOptOut(*OptOut);
			return krtu::Json::object({{"optedOut", *OptOut}});
		}
		if (Operation == "submitScore")
		{
			const std::optional<std::int64_t> Value = Arguments.getInt("value");
			if (!Value)
			{
				throw std::invalid_argument("the arguments need \"value\" (a whole number)");
			}
			const std::optional<std::string> Details = Arguments.getString("details");
			Client.submitScore(RequiredString(Arguments, "key"), *Value, Details ? std::optional<std::string>(krtu::fromBase64(*Details)) : std::nullopt);
			return krtu::Json::object();
		}
		if (Operation == "setPresence")
		{
			std::map<std::string, std::string> Values;
			for (const auto& Member : Arguments["params"].members())
			{
				if (!Member.second.isString())
				{
					throw std::invalid_argument("the values of \"params\" are text");
				}
				Values[Member.first] = Member.second.asString();
			}
			const krtu::PresenceResult Result = Client.setPresence(RequiredString(Arguments, "key"), Values);
			return krtu::Json::object({{"applied", Result.applied}, {"reason", Result.reason}});
		}
		if (Operation == "clearPresence")
		{
			Client.clearPresence();
			return krtu::Json::object();
		}
		if (Operation == "setPresenceVisible")
		{
			const std::optional<bool> Visible = Arguments.getBool("visible");
			if (!Visible)
			{
				throw std::invalid_argument("the arguments need \"visible\" (true or false)");
			}
			Client.setPresenceVisible(*Visible);
			return krtu::Json::object({{"visible", *Visible}});
		}
		if (Operation == "entitlements" || Operation == "refreshEntitlements")
		{
			krtu::Json Items = krtu::Json::array();
			for (const krtu::Entitlement& Item : Operation == "entitlements" ? Client.entitlements() : Client.refreshEntitlements())
			{
				Items.push(krtu::Json::object({{"sku", Item.sku},
											   {"source", Item.source},
											   {"status", Item.status.empty() ? krtu::Json() : krtu::Json(Item.status)},
											   {"platformProductId", Item.platformProductId.empty() ? krtu::Json() : krtu::Json(Item.platformProductId)},
											   {"checked", Item.checked}}));
			}
			return krtu::Json::object({{"entitlements", Items}});
		}
		if (Operation == "ownsDlc")
		{
			return krtu::Json::object({{"owns", Client.ownsDlc(RequiredString(Arguments, "key"))}});
		}
		if (Operation == "getLeaderboards")
		{
			krtu::Json Boards = krtu::Json::array();
			for (const krtu::LeaderboardView& Board : Client.getLeaderboards(OptionalString(Arguments, "locale")))
			{
				krtu::Json Item = krtu::Json::object({{"key", Board.key}, {"name", Board.name}, {"sortOrder", Board.sortOrder}, {"display", Board.display}, {"entries", Board.entries}});
				Item["score"] = Board.score ? krtu::Json(*Board.score) : krtu::Json();
				Item["rank"] = Board.rank ? krtu::Json(*Board.rank) : krtu::Json();
				Item["details"] = Board.details ? krtu::Json(krtu::base64(*Board.details)) : krtu::Json();
				Boards.push(Item);
			}
			return krtu::Json::object({{"leaderboards", Boards}});
		}
		if (Operation == "getLeaderboardEntries")
		{
			const krtu::LeaderboardPage Page = Client.getLeaderboardEntries(RequiredString(Arguments, "key"), OptionalString(Arguments, "scope").value_or("global"),
				static_cast<int>(Arguments.getInt("limit").value_or(25)), static_cast<int>(Arguments.getInt("offset").value_or(0)));
			krtu::Json Rows = krtu::Json::array();
			for (const krtu::LeaderboardEntry& Entry : Page.entries)
			{
				krtu::Json Row = krtu::Json::object({{"rank", Entry.rank}, {"score", Entry.score}, {"updatedAt", Entry.updatedAt}, {"you", Entry.you}});
				Row["name"] = Entry.name ? krtu::Json(*Entry.name) : krtu::Json();
				Row["details"] = Entry.details ? krtu::Json(krtu::base64(*Entry.details)) : krtu::Json();
				Rows.push(Row);
			}
			return krtu::Json::object({{"key", Page.key}, {"scope", Page.scope}, {"total", Page.total}, {"entries", Rows}});
		}
		if (Operation == "reportAvatar")
		{
			const std::optional<std::string> Note = OptionalString(Arguments, "note");
			const bool Already = Client.reportAvatar(RequiredString(Arguments, "subject"), RequiredString(Arguments, "reason"), Note.value_or(""));
			return krtu::Json::object({{"already", Already}});
		}
		if (Operation == "syncPlatformAchievements")
		{
			const krtu::PlatformSyncResult Result = Client.syncPlatformAchievements();
			krtu::Json Here = krtu::Json::array();
			krtu::Json There = krtu::Json::array();
			krtu::Json Clear = krtu::Json::array();
			krtu::Json StatsImported = krtu::Json::array();
			krtu::Json StatsThere = krtu::Json::array();
			for (const std::string& Id : Result.unlockedHere)
			{
				Here.push(Id);
			}
			for (const std::string& Id : Result.toPlatform)
			{
				There.push(Id);
			}
			for (const std::string& Id : Result.toClear)
			{
				Clear.push(Id);
			}
			for (const std::string& Key : Result.statsImported)
			{
				StatsImported.push(Key);
			}
			for (const std::string& Id : Result.statsToPlatform)
			{
				StatsThere.push(Id);
			}
			return krtu::Json::object({{"unlockedHere", Here}, {"toPlatform", There}, {"toClear", Clear}, {"statsImported", StatsImported}, {"statsToPlatform", StatsThere}});
		}
		if (Operation == "answerPlatformConsent")
		{
			const std::optional<bool> Allow = Arguments.getBool("allow");
			if (!Allow)
			{
				throw std::invalid_argument("the arguments need \"allow\" (true or false)");
			}
			return PlatformLinkJson(Client.answerPlatformConsent(*Allow));
		}
		if (Operation == "syncPlatformFiles")
		{
			Client.syncPlatformFiles();
			return krtu::Json::object();
		}
		if (Operation == "platformLink")
		{
			const std::optional<krtu::PlatformLink> Link = Client.platformLink();
			return Link ? PlatformLinkJson(*Link) : krtu::Json::object({{"platformLink", krtu::Json()}});
		}
		if (Operation == "requestGuestDeletion")
		{
			return krtu::Json::object({{"deletionDate", Client.requestGuestDeletion()}});
		}
		if (Operation == "cancelGuestDeletion")
		{
			return krtu::Json::object({{"status", Client.cancelGuestDeletion()}});
		}
		if (Operation == "guestDataExport")
		{
			return Client.guestDataExport(Arguments.getBool("request").value_or(false));
		}
		throw std::invalid_argument("\"" + Operation + "\" is not a Krtuworks call");
	}
}

// ── UKrtuworksNode ──────────────────────────────────────────────────────

void UKrtuworksNode::Attach(const UObject* WorldContextObject)
{
	if (UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject))
	{
		Subsystem = GameInstance->GetSubsystem<UKrtuworksSubsystem>();
		RegisterWithGameInstance(GameInstance);
	}
}

void UKrtuworksNode::Fail(const FKrtuworksError& Error)
{
	OnFailure.Broadcast(Error);
	SetReadyToDestroy();
}

bool UKrtuworksNode::FailIfNotReady()
{
	if (Subsystem.IsValid() && Subsystem->IsReady())
	{
		return false;
	}
	FKrtuworksError Error;
	Error.Code = TEXT("not_ready");
	Error.Message = TEXT("The Krtuworks SDK did not start: see the log (krtuworks-config.json).");
	Fail(Error);
	return true;
}

// ── Sign-in ─────────────────────────────────────────────────────────────

UKrtuworksSignInAsGuest* UKrtuworksSignInAsGuest::SignInAsGuest(const UObject* WorldContextObject, const FString& BirthDate, const FString& AcceptedTerms)
{
	UKrtuworksSignInAsGuest* Node = NewObject<UKrtuworksSignInAsGuest>();
	Node->BirthDate = BirthDate;
	Node->AcceptedTerms = AcceptedTerms;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSignInAsGuest::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Birth;
	std::optional<std::string> Terms;
	if (!BirthDate.IsEmpty())
	{
		Birth = ToStd(BirthDate);
		Terms = AcceptedTerms.IsEmpty() ? std::string(krtu::Client::currentTerms) : ToStd(AcceptedTerms);
	}
	TWeakObjectPtr<UKrtuworksSignInAsGuest> Self(this);
	Subsystem->Run<krtu::Player>(
		[Birth, Terms](krtu::Client& C) { return C.signInAsGuest(Birth, Terms); },
		[Self](const krtu::Player& Player) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToPlayer(Player));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksSignIn* UKrtuworksSignIn::SignIn(const UObject* WorldContextObject)
{
	UKrtuworksSignIn* Node = NewObject<UKrtuworksSignIn>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSignIn::Cancel()
{
	CancelToken.cancel();
}

void UKrtuworksSignIn::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	const krtu::CancelToken Cancel = CancelToken;
	TWeakObjectPtr<UKrtuworksSignIn> Self(this);
	Subsystem->Run<krtu::Player>(
		[Cancel](krtu::Client& C) {
			if (KrtuworksPlatform::SignsInOnPhone())
			{
				return C.signInWithApp(&KrtuworksPlatform::AuthenticateOnPhone, Cancel);
			}
			return C.signInWithBrowser(
				[](const std::string& Url) {
					// The browser opens from the game thread (macOS and Android want it there).
					AsyncTask(ENamedThreads::GameThread, [Address = ToFString(Url)]() { FPlatformProcess::LaunchURL(*Address, nullptr, nullptr); });
				},
				Cancel);
		},
		[Self](const krtu::Player& Player) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToPlayer(Player));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

namespace
{
	/** The screens end in a player, a cancel or "offline": the pin of each. */
	template <class TNode>
	void FinishScreens(const TWeakObjectPtr<TNode>& Node, const FKrtuworksScreenResult& Result)
	{
		if (Result.bSignedIn)
		{
			Node->OnSignedIn.Broadcast(Result.Player);
		}
		else
		{
			Node->OnOffline.Broadcast(Result.OfflineReason);
		}
		Node->SetReadyToDestroy();
	}
}

UKrtuworksEnsurePlayer* UKrtuworksEnsurePlayer::EnsurePlayer(const UObject* WorldContextObject)
{
	UKrtuworksEnsurePlayer* Node = NewObject<UKrtuworksEnsurePlayer>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksEnsurePlayer::Activate()
{
	if (!Subsystem.IsValid() || !Subsystem->IsReady())
	{
		OnOffline.Broadcast(TEXT("unavailable"));
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UKrtuworksEnsurePlayer> Self(this);
	Subsystem->EnsurePlayer([Self](const FKrtuworksScreenResult& Result) {
		if (Self.IsValid())
		{
			FinishScreens(Self, Result);
		}
	});
}

UKrtuworksSignInWithScreens* UKrtuworksSignInWithScreens::SignInWithScreens(const UObject* WorldContextObject)
{
	UKrtuworksSignInWithScreens* Node = NewObject<UKrtuworksSignInWithScreens>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSignInWithScreens::Cancel()
{
	CancelToken.cancel();
}

void UKrtuworksSignInWithScreens::Activate()
{
	if (!Subsystem.IsValid() || !Subsystem->IsReady())
	{
		OnOffline.Broadcast(TEXT("unavailable"));
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UKrtuworksSignInWithScreens> Self(this);
	Subsystem->SignInWithScreens(CancelToken, [Self](const FKrtuworksScreenResult& Result) {
		if (!Self.IsValid())
		{
			return;
		}
		if (Result.bCancelled)
		{
			Self->OnCancelled.Broadcast();
			Self->SetReadyToDestroy();
			return;
		}
		FinishScreens(Self, Result);
	});
}

UKrtuworksSignInWithCode* UKrtuworksSignInWithCode::SignInWithCode(const UObject* WorldContextObject, const FString& DeviceName)
{
	UKrtuworksSignInWithCode* Node = NewObject<UKrtuworksSignInWithCode>();
	Node->DeviceName = DeviceName;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSignInWithCode::Cancel()
{
	CancelToken.cancel();
}

void UKrtuworksSignInWithCode::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Name;
	if (!DeviceName.IsEmpty())
	{
		Name = ToStd(DeviceName);
	}
	const krtu::CancelToken Cancel = CancelToken;
	TWeakObjectPtr<UKrtuworksSignInWithCode> Self(this);
	Subsystem->Run<krtu::Player>(
		[Name, Cancel, Self](krtu::Client& C) {
			const krtu::DeviceLink Link = C.startDeviceLink(Name);
			AsyncTask(ENamedThreads::GameThread, [Self, Shown = KrtuworksConvert::ToDeviceLink(Link)]() {
				if (Self.IsValid())
				{
					Self->OnCode.Broadcast(Shown);
				}
			});
			return C.finishDeviceLink(Link, Cancel);
		},
		[Self](const krtu::Player& Player) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToPlayer(Player));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksSignOut* UKrtuworksSignOut::SignOut(const UObject* WorldContextObject)
{
	UKrtuworksSignOut* Node = NewObject<UKrtuworksSignOut>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSignOut::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksSignOut> Self(this);
	Subsystem->Run<bool>(
		[](krtu::Client& C) {
			C.signOut();
			return true;
		},
		[Self](const bool&) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast();
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

// ── Files ───────────────────────────────────────────────────────────────

UKrtuworksSaveFile* UKrtuworksSaveFile::SaveFile(const UObject* WorldContextObject, const FString& Slot, const TArray<uint8>& Bytes, const FString& BaseVersion)
{
	UKrtuworksSaveFile* Node = NewObject<UKrtuworksSaveFile>();
	Node->Slot = Slot;
	Node->Bytes = Bytes;
	Node->BaseVersion = BaseVersion;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSaveFile::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Base;
	if (!BaseVersion.IsEmpty())
	{
		Base = ToStd(BaseVersion);
	}
	TWeakObjectPtr<UKrtuworksSaveFile> Self(this);
	Subsystem->Run<krtu::FileVersion>(
		[SlotKey = ToStd(Slot), Data = KrtuworksConvert::ToBytes(Bytes), Base](krtu::Client& C) { return C.saveFile(SlotKey, Data, Base); },
		[Self](const krtu::FileVersion& Version) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToFileVersion(Version));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksReadFile* UKrtuworksReadFile::ReadFile(const UObject* WorldContextObject, const FString& Slot, const FString& HeldVersion)
{
	UKrtuworksReadFile* Node = NewObject<UKrtuworksReadFile>();
	Node->Slot = Slot;
	Node->HeldVersion = HeldVersion;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksReadFile::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Held;
	if (!HeldVersion.IsEmpty())
	{
		Held = ToStd(HeldVersion);
	}
	TWeakObjectPtr<UKrtuworksReadFile> Self(this);
	Subsystem->Run<std::optional<krtu::FileRead>>(
		[SlotKey = ToStd(Slot), Held](krtu::Client& C) { return C.readFile(SlotKey, std::nullopt, Held); },
		[Self](const std::optional<krtu::FileRead>& Read) {
			if (!Self.IsValid())
			{
				return;
			}
			FKrtuworksFileRead Out;
			Out.bUnchanged = !Read.has_value();
			if (Read)
			{
				Out.Bytes = KrtuworksConvert::ToArray(Read->bytes);
				Out.VersionId = ToFString(Read->versionId.value_or(""));
				Out.Generation = Read->generation;
				Out.bNewerLocalSave = Read->newerLocalSave;
			}
			Self->OnSuccess.Broadcast(Out);
			Self->SetReadyToDestroy();
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

// ── Progress, sending, config ───────────────────────────────────────────

UKrtuworksGetProgress* UKrtuworksGetProgress::GetProgress(const UObject* WorldContextObject)
{
	UKrtuworksGetProgress* Node = NewObject<UKrtuworksGetProgress>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksGetProgress::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksGetProgress> Self(this);
	Subsystem->Run<krtu::ProgressView>(
		[](krtu::Client& C) { return C.getProgress(); },
		[Self](const krtu::ProgressView& View) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToProgress(View));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksSubmitScore* UKrtuworksSubmitScore::SubmitScore(const UObject* WorldContextObject, const FString& Key, int64 Value, const TArray<uint8>& Details)
{
	UKrtuworksSubmitScore* Node = NewObject<UKrtuworksSubmitScore>();
	Node->Key = Key;
	Node->Value = Value;
	Node->Details = Details;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSubmitScore::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Bytes;
	if (Details.Num() > 0)
	{
		Bytes = KrtuworksConvert::ToBytes(Details);
	}
	TWeakObjectPtr<UKrtuworksSubmitScore> Self(this);
	Subsystem->Run<bool>(
		[BoardKey = ToStd(Key), Score = Value, Bytes](krtu::Client& C) {
			C.submitScore(BoardKey, Score, Bytes);
			return true;
		},
		[Self](const bool&) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast();
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksGetLeaderboards* UKrtuworksGetLeaderboards::GetLeaderboards(const UObject* WorldContextObject, const FString& Locale)
{
	UKrtuworksGetLeaderboards* Node = NewObject<UKrtuworksGetLeaderboards>();
	Node->Locale = Locale;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksGetLeaderboards::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::optional<std::string> Language;
	if (!Locale.IsEmpty())
	{
		Language = ToStd(Locale);
	}
	TWeakObjectPtr<UKrtuworksGetLeaderboards> Self(this);
	Subsystem->Run<std::vector<krtu::LeaderboardView>>(
		[Language](krtu::Client& C) { return C.getLeaderboards(Language); },
		[Self](const std::vector<krtu::LeaderboardView>& Boards) {
			if (Self.IsValid())
			{
				TArray<FKrtuworksLeaderboard> Out;
				for (const krtu::LeaderboardView& Board : Boards)
				{
					Out.Add(KrtuworksConvert::ToLeaderboard(Board));
				}
				Self->OnSuccess.Broadcast(Out);
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksGetLeaderboardEntries* UKrtuworksGetLeaderboardEntries::GetLeaderboardEntries(const UObject* WorldContextObject, const FString& Key, const FString& Scope, int32 Limit, int32 Offset)
{
	UKrtuworksGetLeaderboardEntries* Node = NewObject<UKrtuworksGetLeaderboardEntries>();
	Node->Key = Key;
	Node->Scope = Scope;
	Node->Limit = Limit;
	Node->Offset = Offset;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksGetLeaderboardEntries::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksGetLeaderboardEntries> Self(this);
	Subsystem->Run<krtu::LeaderboardPage>(
		[BoardKey = ToStd(Key), PageScope = ToStd(Scope), PageLimit = Limit, PageOffset = Offset](krtu::Client& C) { return C.getLeaderboardEntries(BoardKey, PageScope, PageLimit, PageOffset); },
		[Self](const krtu::LeaderboardPage& Page) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(KrtuworksConvert::ToLeaderboardPage(Page));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

// ── Presence ────────────────────────────────────────────────────────────

UKrtuworksSetPresence* UKrtuworksSetPresence::SetPresence(const UObject* WorldContextObject, const FString& Key, const TMap<FString, FString>& Params)
{
	UKrtuworksSetPresence* Node = NewObject<UKrtuworksSetPresence>();
	Node->Key = Key;
	Node->Params = Params;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSetPresence::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	std::map<std::string, std::string> Values;
	for (const TPair<FString, FString>& Param : Params)
	{
		Values[ToStd(Param.Key)] = ToStd(Param.Value);
	}
	TWeakObjectPtr<UKrtuworksSetPresence> Self(this);
	Subsystem->Run<krtu::PresenceResult>(
		[StateKey = ToStd(Key), Values](krtu::Client& C) { return C.setPresence(StateKey, Values); },
		[Self](const krtu::PresenceResult& Result) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(Result.applied, ToFString(Result.reason));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksSetPresenceVisible* UKrtuworksSetPresenceVisible::SetPresenceVisible(const UObject* WorldContextObject, bool bVisible)
{
	UKrtuworksSetPresenceVisible* Node = NewObject<UKrtuworksSetPresenceVisible>();
	Node->bVisible = bVisible;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksSetPresenceVisible::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksSetPresenceVisible> Self(this);
	Subsystem->Run<bool>(
		[Visible = bVisible](krtu::Client& C) {
			C.setPresenceVisible(Visible);
			return true;
		},
		[Self](const bool&) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast();
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksPresenceVisible* UKrtuworksPresenceVisible::PresenceVisible(const UObject* WorldContextObject)
{
	UKrtuworksPresenceVisible* Node = NewObject<UKrtuworksPresenceVisible>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksPresenceVisible::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksPresenceVisible> Self(this);
	Subsystem->Run<std::optional<bool>>(
		[](krtu::Client& C) { return C.presenceVisible(); },
		[Self](const std::optional<bool>& Visible) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(Visible.has_value(), Visible.value_or(true));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksFlush* UKrtuworksFlush::Flush(const UObject* WorldContextObject)
{
	UKrtuworksFlush* Node = NewObject<UKrtuworksFlush>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksFlush::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksFlush> Self(this);
	Subsystem->Run<bool>(
		[](krtu::Client& C) {
			C.flush();
			return true;
		},
		[Self](const bool&) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast();
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksRefreshConfig* UKrtuworksRefreshConfig::RefreshConfig(const UObject* WorldContextObject)
{
	UKrtuworksRefreshConfig* Node = NewObject<UKrtuworksRefreshConfig>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksRefreshConfig::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksRefreshConfig> Self(this);
	Subsystem->Run<bool>(
		[](krtu::Client& C) {
			C.refreshConfig();
			return true;
		},
		[Self](const bool&) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast();
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksCaptureScreenshot* UKrtuworksCaptureScreenshot::CaptureScreenshot(const UObject* WorldContextObject, int32 Quality)
{
	UKrtuworksCaptureScreenshot* Node = NewObject<UKrtuworksCaptureScreenshot>();
	Node->Quality = Quality;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksCaptureScreenshot::Activate()
{
	TWeakObjectPtr<UKrtuworksCaptureScreenshot> Self(this);
	KrtuworksScreenshot::Capture(Quality, [Self](TArray<uint8> Bytes) {
		if (!Self.IsValid())
		{
			return;
		}
		if (Bytes.Num() == 0)
		{
			FKrtuworksError Error;
			Error.Code = TEXT("no_screenshot");
			Error.Message = TEXT("The engine gave no picture of the screen.");
			Self->Fail(Error);
			return;
		}
		Self->OnSuccess.Broadcast(Bytes);
		Self->SetReadyToDestroy();
	});
}

UKrtuworksReportBug* UKrtuworksReportBug::ReportBug(const UObject* WorldContextObject, const FString& Category, const FString& Description, const TArray<uint8>& Screenshot, const FString& Level)
{
	UKrtuworksReportBug* Node = NewObject<UKrtuworksReportBug>();
	Node->Category = Category;
	Node->Description = Description;
	Node->Screenshot = Screenshot;
	Node->Level = Level;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksReportBug::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	krtu::BugReportInput Input;
	Input.category = ToStd(Category);
	Input.description = ToStd(Description);
	if (Screenshot.Num() > 0)
	{
		Input.screenshot = KrtuworksConvert::ToBytes(Screenshot);
	}
	if (!Level.IsEmpty())
	{
		Input.level = ToStd(Level);
	}
	TWeakObjectPtr<UKrtuworksReportBug> Self(this);
	Subsystem->Run<std::string>(
		[Input](krtu::Client& C) { return C.reportBug(Input); },
		[Self](const std::string& ReportId) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(ToFString(ReportId));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksMyBugReports* UKrtuworksMyBugReports::MyBugReports(const UObject* WorldContextObject)
{
	UKrtuworksMyBugReports* Node = NewObject<UKrtuworksMyBugReports>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksMyBugReports::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksMyBugReports> Self(this);
	Subsystem->Run<std::vector<krtu::BugReportView>>(
		[](krtu::Client& C) { return C.myBugReports(); },
		[Self](const std::vector<krtu::BugReportView>& Reports) {
			if (!Self.IsValid())
			{
				return;
			}
			TArray<FKrtuworksBugReport> Out;
			for (const krtu::BugReportView& Report : Reports)
			{
				Out.Add(KrtuworksConvert::ToBugReport(Report));
			}
			Self->OnSuccess.Broadcast(Out);
			Self->SetReadyToDestroy();
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksCall* UKrtuworksCall::Call(const UObject* WorldContextObject, const FString& Operation, const FString& ArgumentsJson)
{
	UKrtuworksCall* Node = NewObject<UKrtuworksCall>();
	Node->Operation = Operation;
	Node->ArgumentsJson = ArgumentsJson;
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksCall::Activate()
{
	if (FailIfNotReady())
	{
		return;
	}
	TWeakObjectPtr<UKrtuworksCall> Self(this);
	Subsystem->Run<std::string>(
		[Name = ToStd(Operation), Text = ToStd(ArgumentsJson)](krtu::Client& C) {
			const std::optional<krtu::Json> Arguments = krtu::Json::parse(Text.empty() ? std::string("{}") : Text);
			if (!Arguments || !Arguments->isObject())
			{
				throw std::invalid_argument("the arguments are a JSON object");
			}
			return RunOperation(C, Name, *Arguments).dump();
		},
		[Self](const std::string& Result) {
			if (Self.IsValid())
			{
				Self->OnSuccess.Broadcast(ToFString(Result));
				Self->SetReadyToDestroy();
			}
		},
		[Self](const FKrtuworksError& Error) {
			if (Self.IsValid())
			{
				Self->Fail(Error);
			}
		});
}

UKrtuworksValidate* UKrtuworksValidate::Validate(const UObject* WorldContextObject)
{
	UKrtuworksValidate* Node = NewObject<UKrtuworksValidate>();
	Node->Attach(WorldContextObject);
	return Node;
}

void UKrtuworksValidate::Activate()
{
	if (!Subsystem.IsValid())
	{
		FKrtuworksError Error;
		Error.Code = TEXT("not_ready");
		Error.Message = TEXT("No game instance: the validator runs in Play In Editor or a game.");
		Fail(Error);
		return;
	}
	TWeakObjectPtr<UKrtuworksValidate> Self(this);
	Subsystem->Validate([Self](const FKrtuworksValidationReport& Report) {
		if (Self.IsValid())
		{
			Self->OnSuccess.Broadcast(Report);
			Self->SetReadyToDestroy();
		}
	});
}
