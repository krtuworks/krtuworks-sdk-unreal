#include "KrtuworksScreenshot.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

namespace KrtuworksScreenshot
{
	namespace
	{
		constexpr float CaptureWaitSeconds = 3.0f;

		/** One capture: the engine's answer and the time limit race; whichever comes first finishes it. */
		struct FPending
		{
			TFunction<void(TArray<uint8>)> Done;
			FDelegateHandle Captured;
			FTSTicker::FDelegateHandle Timeout;
			bool bFinished = false;

			void Finish(TArray<uint8> Bytes)
			{
				if (bFinished)
				{
					return;
				}
				bFinished = true;
				UGameViewportClient::OnScreenshotCaptured().Remove(Captured);
				FTSTicker::GetCoreTicker().RemoveTicker(Timeout);
				Done(MoveTemp(Bytes));
			}
		};

		TArray<uint8> Jpeg(int32 Width, int32 Height, const TArray<FColor>& Colors, int32 Quality)
		{
			TArray<uint8> Bytes;
			IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
			const TSharedPtr<IImageWrapper> Writer = Images.CreateImageWrapper(EImageFormat::JPEG);
			if (Writer.IsValid() && Writer->SetRaw(Colors.GetData(), static_cast<int64>(Colors.Num()) * static_cast<int64>(sizeof(FColor)), Width, Height, ERGBFormat::BGRA, 8))
			{
				const TArray64<uint8> Compressed = Writer->GetCompressed(Quality);
				Bytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
			}
			return Bytes;
		}
	}

	void Capture(int32 Quality, TFunction<void(TArray<uint8>)> Done)
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || GEngine->GameViewport->Viewport == nullptr)
		{
			Done(TArray<uint8>());
			return;
		}
		const TSharedRef<FPending> Pending = MakeShared<FPending>();
		Pending->Done = MoveTemp(Done);
		const int32 Clamped = FMath::Clamp(Quality, 1, 100);
		Pending->Captured = UGameViewportClient::OnScreenshotCaptured().AddLambda([Pending, Clamped](int32 Width, int32 Height, const TArray<FColor>& Colors) {
			Pending->Finish(Jpeg(Width, Height, Colors, Clamped));
		});
		Pending->Timeout = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Pending](float) {
			Pending->Finish(TArray<uint8>());
			return false;
		}), CaptureWaitSeconds);
		FScreenshotRequest::RequestScreenshot(false);
	}
}
