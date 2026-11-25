#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "UObject/GCObject.h"

class UTexture2D;

/**
 * A picture from an https address (the player's picture, a save's picture): fetched with the engine's web request,
 * decoded and held as a texture for a Slate brush. Nothing shows while it loads or when it fails.
 */
class FKrtuworksRemoteImage : public FGCObject, public TSharedFromThis<FKrtuworksRemoteImage>
{
public:
	/** Starts loading `Url`; `OnLoaded` runs on the game thread when the picture is ready (never when it fails). */
	static TSharedRef<FKrtuworksRemoteImage> Load(const FString& Url, TFunction<void()> OnLoaded);

	/** The picture, or nothing until it has loaded. */
	const FSlateBrush* GetBrush() const;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

private:
	void Start(const FString& Url, TFunction<void()> OnLoaded);
	void Decode(const TArray<uint8>& Bytes);

	UTexture2D* Texture = nullptr;
	FSlateBrush Brush;
	bool bLoaded = false;
};
