#include "KrtuworksSettings.h"

#include "KrtuworksConvert.h"
#include "KrtuworksCore/Types.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UKrtuworksSettings::UKrtuworksSettings()
{
	SectionName = TEXT("Krtuworks SDK");
}

#if WITH_EDITOR
void UKrtuworksSettings::PostInitProperties()
{
	Super::PostInitProperties();
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *KrtuworksConvert::ConfigFilePath()))
	{
		return;
	}
	try
	{
		const FString Slug = KrtuworksConvert::ToFString(krtu::AppConfig::fromJson(KrtuworksConvert::ToStd(Text)).slug);
		if (Slug != AppSlug)
		{
			AppSlug = Slug;
			TryUpdateDefaultConfigFile();
		}
	}
	catch (const std::exception&)
	{
		// Not a krtuworks-config.json: the game's start says so in the log.
	}
}
#endif
