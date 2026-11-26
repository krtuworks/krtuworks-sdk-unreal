#include "KrtuworksConvert.h"

#include "Misc/Paths.h"

#include <exception>
#include <stdexcept>

DEFINE_LOG_CATEGORY(LogKrtuworks);

namespace KrtuworksConvert
{
	std::string ToStd(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return std::string(reinterpret_cast<const char*>(Utf8.Get()), static_cast<size_t>(Utf8.Length()));
	}

	FString ToFString(const std::string& Text)
	{
		const FUTF8ToTCHAR Wide(Text.data(), static_cast<int32>(Text.size()));
		return FString(Wide.Length(), Wide.Get());
	}

	std::string ToBytes(const TArray<uint8>& Bytes)
	{
		return std::string(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	}

	TArray<uint8> ToArray(const std::string& Bytes)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	}

	FString ConfigFilePath()
	{
		return FPaths::Combine(FPaths::ProjectDir(), TEXT("krtuworks-config.json"));
	}

	FKrtuworksPlayer ToPlayer(const std::optional<krtu::Player>& Player)
	{
		FKrtuworksPlayer Out;
		if (!Player)
		{
			return Out;
		}
		Out.bSignedIn = true;
		Out.Kind = Player->kind == krtu::PlayerKind::Account ? EKrtuworksPlayerKind::Account : EKrtuworksPlayerKind::Guest;
		Out.Subject = ToFString(Player->subject);
		Out.Username = ToFString(Player->username.value_or(""));
		Out.Picture = ToFString(Player->picture.value_or(""));
		return Out;
	}

	FKrtuworksIdentityChange ToChange(const krtu::IdentityChange& Change)
	{
		FKrtuworksIdentityChange Out;
		Out.Type = ToFString(Change.type);
		Out.Player = ToPlayer(Change.player);
		Out.bPromoted = Change.promoted;
		Out.bCreated = Change.created;
		Out.Reason = ToFString(Change.reason.value_or(""));
		Out.PreviousSubject = ToFString(Change.previousSubject.value_or(""));
		Out.MergedInto = ToFString(Change.mergedInto.value_or(""));
		return Out;
	}

	krtu::Json ToJson(const FKrtuworksEventData& Data)
	{
		krtu::Json Out = krtu::Json::object();
		for (const TPair<FString, FString>& Entry : Data.Strings)
		{
			Out[ToStd(Entry.Key)] = ToStd(Entry.Value);
		}
		for (const TPair<FString, double>& Entry : Data.Numbers)
		{
			Out[ToStd(Entry.Key)] = Entry.Value;
		}
		for (const TPair<FString, bool>& Entry : Data.Booleans)
		{
			Out[ToStd(Entry.Key)] = Entry.Value;
		}
		return Out;
	}

	krtu::EventPosition ToPosition(const FKrtuworksPosition& Position)
	{
		krtu::EventPosition Out;
		Out.space = ToStd(Position.Space);
		Out.area = ToStd(Position.Area);
		Out.x = Position.Location.X;
		Out.y = Position.Location.Y;
		if (Out.space == "world")
		{
			Out.z = Position.Location.Z;
		}
		return Out;
	}

	krtu::LevelRef ToLevel(const FKrtuworksLevel& Level)
	{
		krtu::LevelRef Out;
		Out.level = ToStd(Level.Level);
		if (!Level.World.IsEmpty())
		{
			Out.world = ToStd(Level.World);
		}
		if (!Level.Part.IsEmpty())
		{
			Out.part = ToStd(Level.Part);
		}
		return Out;
	}

	FKrtuworksGate ToGate(const krtu::Gate& Gate)
	{
		FKrtuworksGate Out;
		Out.bMaintenance = Gate.maintenance;
		Out.MaintenanceMessage = ToFString(Gate.maintenanceMessage.value_or(""));
		Out.bUpdateRequired = Gate.updateRequired;
		Out.bSwitchedOff = Gate.switchedOff.has_value();
		Out.bFirstLaunchOffline = Gate.firstLaunchOffline;
		if (Gate.switchedOff)
		{
			Out.SwitchMode = ToFString(Gate.switchedOff->mode);
			Out.SwitchMessage = ToFString(Gate.switchedOff->message);
		}
		return Out;
	}

	FKrtuworksProgress ToProgress(const krtu::ProgressView& View)
	{
		FKrtuworksProgress Out;
		for (const krtu::StatView& Stat : View.stats)
		{
			FKrtuworksStat Item;
			Item.Key = ToFString(Stat.key);
			Item.Name = ToFString(Stat.name);
			Item.Description = ToFString(Stat.description.value_or(""));
			Item.Kind = ToFString(Stat.kind);
			Item.bHasValue = Stat.value.has_value();
			Item.Value = Stat.value.value_or(0);
			Out.Stats.Add(Item);
		}
		for (const krtu::AchievementView& Achievement : View.achievements)
		{
			FKrtuworksAchievement Item;
			Item.Key = ToFString(Achievement.key);
			Item.Name = ToFString(Achievement.name.value_or(""));
			Item.Description = ToFString(Achievement.description.value_or(""));
			Item.bHidden = Achievement.hidden;
			Item.bUnlocked = Achievement.unlocked;
			Item.UnlockedAt = ToFString(Achievement.unlockedAt.value_or(""));
			Item.IconUrl = ToFString(Achievement.iconUrl.value_or(""));
			Item.LockedIconUrl = ToFString(Achievement.lockedIconUrl.value_or(""));
			Out.Achievements.Add(Item);
		}
		return Out;
	}

	FKrtuworksLeaderboard ToLeaderboard(const krtu::LeaderboardView& Board)
	{
		FKrtuworksLeaderboard Out;
		Out.Key = ToFString(Board.key);
		Out.Name = ToFString(Board.name);
		Out.Description = ToFString(Board.description.value_or(""));
		Out.SortOrder = ToFString(Board.sortOrder);
		Out.Display = ToFString(Board.display);
		Out.bHasScore = Board.score.has_value();
		Out.Score = Board.score.value_or(0);
		Out.Details = ToArray(Board.details.value_or(""));
		Out.bHasRank = Board.rank.has_value();
		Out.Rank = Board.rank.value_or(0);
		Out.Entries = Board.entries;
		return Out;
	}

	FKrtuworksLeaderboardPage ToLeaderboardPage(const krtu::LeaderboardPage& Page)
	{
		FKrtuworksLeaderboardPage Out;
		Out.Key = ToFString(Page.key);
		Out.Scope = ToFString(Page.scope);
		Out.SortOrder = ToFString(Page.sortOrder);
		Out.Display = ToFString(Page.display);
		Out.Total = Page.total;
		for (const krtu::LeaderboardEntry& Entry : Page.entries)
		{
			FKrtuworksLeaderboardEntry Item;
			Item.Rank = Entry.rank;
			Item.Name = ToFString(Entry.name.value_or("Player"));
			Item.Score = Entry.score;
			Item.Details = ToArray(Entry.details.value_or(""));
			Item.UpdatedAt = ToFString(Entry.updatedAt);
			Item.bYou = Entry.you;
			Out.Entries.Add(Item);
		}
		return Out;
	}

	FKrtuworksBugReport ToBugReport(const krtu::BugReportView& Report)
	{
		FKrtuworksBugReport Out;
		Out.Id = ToFString(Report.id);
		Out.Category = ToFString(Report.category);
		Out.Description = ToFString(Report.description.value_or(""));
		Out.Build = ToFString(Report.build);
		Out.CreatedAt = ToFString(Report.createdAt);
		Out.bScreenshot = Report.screenshot;
		Out.Status = ToFString(Report.status);
		Out.FixedIn = ToFString(Report.fixedIn.value_or(""));
		Out.bDuplicate = Report.duplicate;
		return Out;
	}

	FKrtuworksValidationReport ToValidationReport(const krtu::ValidationReport& Report)
	{
		FKrtuworksValidationReport Out;
		Out.bOk = Report.ok;
		Out.Build = ToFString(Report.build);
		for (const krtu::ValidationCheck& Check : Report.checks)
		{
			FKrtuworksValidationCheck Row;
			Row.Key = ToFString(Check.key);
			Row.Title = ToFString(Check.title);
			Row.Status = ToFString(Check.status);
			Row.Detail = ToFString(Check.detail.value_or(""));
			Out.Checks.Add(Row);
		}
		return Out;
	}

	FKrtuworksFileVersion ToFileVersion(const krtu::FileVersion& Version)
	{
		FKrtuworksFileVersion Out;
		Out.Id = ToFString(Version.id);
		Out.Slot = ToFString(Version.slot);
		Out.Generation = Version.generation;
		Out.SizeBytes = Version.sizeBytes;
		Out.SavedAt = ToFString(Version.savedAt);
		Out.bFromMerge = Version.fromMerge;
		Out.PlatformCloud = ToFString(Version.platformCloud.value_or(""));
		Out.bQueued = Version.queued;
		return Out;
	}

	FKrtuworksPlatformSaveConflict ToPlatformSaveConflict(const krtu::PlatformSaveConflict& Conflict)
	{
		FKrtuworksPlatformSaveConflict Out;
		Out.Slot = ToFString(Conflict.slot);
		Out.Server = ToFileVersion(Conflict.server);
		Out.Platform = ToFileVersion(Conflict.platform);
		Out.bHasPlatformData = Conflict.platformData.has_value();
		Out.PlatformData = ToArray(Conflict.platformData.value_or(""));
		return Out;
	}

	FKrtuworksDeviceLink ToDeviceLink(const krtu::DeviceLink& Link)
	{
		FKrtuworksDeviceLink Out;
		Out.UserCode = ToFString(Link.userCode);
		Out.VerificationUri = ToFString(Link.verificationUri);
		Out.VerificationUriComplete = ToFString(Link.verificationUriComplete);
		Out.ExpiresIn = Link.expiresIn;
		return Out;
	}

	FKrtuworksError CurrentError()
	{
		FKrtuworksError Out;
		try
		{
			throw;
		}
		catch (const krtu::Error& Error)
		{
			Out.Code = ToFString(Error.code());
			Out.Message = ToFString(Error.message());
			Out.Status = Error.status();
			Out.bRetryable = Error.retryable();
			Out.DetailsJson = ToFString(Error.details().dump());
		}
		catch (const krtu::Cancelled&)
		{
			Out.Code = TEXT("cancelled");
			Out.Message = TEXT("The player cancelled.");
		}
		catch (const std::invalid_argument& Error)
		{
			Out.Code = TEXT("invalid_argument");
			Out.Message = ToFString(Error.what());
		}
		catch (const std::exception& Error)
		{
			Out.Code = TEXT("sdk_error");
			Out.Message = ToFString(Error.what());
		}
		catch (...)
		{
			Out.Code = TEXT("sdk_error");
			Out.Message = TEXT("An unknown failure.");
		}
		return Out;
	}
}
