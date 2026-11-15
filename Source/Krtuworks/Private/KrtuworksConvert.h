#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Client.h"
#include "KrtuworksCore/Validator.h"
#include "KrtuworksTypes.h"

#include <string>

DECLARE_LOG_CATEGORY_EXTERN(LogKrtuworks, Log, All);

/** Between the engine's types (FString, TArray, Blueprint structs) and the core's (UTF-8 std::string, krtu::Json). */
namespace KrtuworksConvert
{
	std::string ToStd(const FString& Text);
	FString ToFString(const std::string& Text);
	std::string ToBytes(const TArray<uint8>& Bytes);
	TArray<uint8> ToArray(const std::string& Bytes);

	/** krtuworks-config.json next to the .uproject (staged with the game by Krtuworks.Build.cs). */
	FString ConfigFilePath();

	FKrtuworksPlayer ToPlayer(const std::optional<krtu::Player>& Player);
	FKrtuworksIdentityChange ToChange(const krtu::IdentityChange& Change);
	krtu::Json ToJson(const FKrtuworksEventData& Data);
	krtu::EventPosition ToPosition(const FKrtuworksPosition& Position);
	krtu::LevelRef ToLevel(const FKrtuworksLevel& Level);
	FKrtuworksGate ToGate(const krtu::Gate& Gate);
	FKrtuworksProgress ToProgress(const krtu::ProgressView& View);
	FKrtuworksLeaderboard ToLeaderboard(const krtu::LeaderboardView& Board);
	FKrtuworksLeaderboardPage ToLeaderboardPage(const krtu::LeaderboardPage& Page);
	FKrtuworksFileVersion ToFileVersion(const krtu::FileVersion& Version);
	FKrtuworksPlatformSaveConflict ToPlatformSaveConflict(const krtu::PlatformSaveConflict& Conflict);
	FKrtuworksDeviceLink ToDeviceLink(const krtu::DeviceLink& Link);
	FKrtuworksBugReport ToBugReport(const krtu::BugReportView& Report);
	FKrtuworksValidationReport ToValidationReport(const krtu::ValidationReport& Report);

	/** Whatever the core threw, as the error a Blueprint node hands on (a cancelled wait is `cancelled`). */
	FKrtuworksError CurrentError();
}
