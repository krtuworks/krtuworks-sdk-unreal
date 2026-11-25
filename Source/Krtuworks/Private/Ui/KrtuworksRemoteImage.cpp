#include "KrtuworksRemoteImage.h"

#include "Engine/Texture2D.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/UiFormat.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"

namespace
{
	/** A picture bigger than this on a side is left out: it is a profile picture or a save's thumbnail. */
	constexpr int32 MaxSide = 2048;
}

TSharedRef<FKrtuworksRemoteImage> FKrtuworksRemoteImage::Load(const FString& Url, TFunction<void()> OnLoaded)
{
	const TSharedRef<FKrtuworksRemoteImage> Image = MakeShared<FKrtuworksRemoteImage>();
	Image->Start(Url, MoveTemp(OnLoaded));
	return Image;
}

const FSlateBrush* FKrtuworksRemoteImage::GetBrush() const
{
	return bLoaded ? &Brush : nullptr;
}

void FKrtuworksRemoteImage::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Texture);
}

FString FKrtuworksRemoteImage::GetReferencerName() const
{
	return TEXT("FKrtuworksRemoteImage");
}

void FKrtuworksRemoteImage::Start(const FString& Url, TFunction<void()> OnLoaded)
{
	if (!Url.StartsWith(TEXT("https://")))
	{
		return;
	}
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(KrtuworksConvert::ToFString(krtu::engineImage(KrtuworksConvert::ToStd(Url))));
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(15.0f);
	TWeakPtr<FKrtuworksRemoteImage> Weak = AsShared();
	Request->OnProcessRequestComplete().BindLambda([Weak, OnLoaded = MoveTemp(OnLoaded)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected) {
		const TSharedPtr<FKrtuworksRemoteImage> Self = Weak.Pin();
		if (!Self.IsValid() || !bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			return;
		}
		Self->Decode(Response->GetContent());
		if (Self->bLoaded && OnLoaded)
		{
			OnLoaded();
		}
	});
	Request->ProcessRequest();
}

void FKrtuworksRemoteImage::Decode(const TArray<uint8>& Bytes)
{
	IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
	const EImageFormat Format = Images.DetectImageFormat(Bytes.GetData(), Bytes.Num());
	if (Format == EImageFormat::Invalid)
	{
		return;
	}
	const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(Format);
	TArray64<uint8> Raw;
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
	{
		return;
	}
	const int32 Width = Wrapper->GetWidth();
	const int32 Height = Wrapper->GetHeight();
	if (Width <= 0 || Height <= 0 || Width > MaxSide || Height > MaxSide || Raw.Num() != static_cast<int64>(Width) * Height * 4)
	{
		return;
	}
	UTexture2D* Made = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (Made == nullptr)
	{
		return;
	}
	FTexture2DMipMap& Mip = Made->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Raw.GetData(), Raw.Num());
	Mip.BulkData.Unlock();
	Made->UpdateResource();
	Texture = Made;
	Brush.SetResourceObject(Texture);
	Brush.SetImageSize(FVector2f(static_cast<float>(Width), static_cast<float>(Height)));
	Brush.DrawAs = ESlateBrushDrawType::Image;
	bLoaded = true;
}
