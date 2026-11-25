#include "KrtuworksUiHost.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "KrtuworksConvert.h"
#include "SKrtuworksAccount.h"
#include "SKrtuworksScreen.h"
#include "Ui/KrtuworksUiLook.h"

#include <chrono>
#include <future>
#include <mutex>

namespace
{
	/** Above the game's own widgets. */
	constexpr int32 ScreenOrder = 20000;
	constexpr int32 ReminderOrder = 19000;
	/** A reminder nobody answers means "later". */
	constexpr float ReminderSeconds = 20.0f;

	/** One question waiting for its answer: set once, by whoever comes first. */
	template <class TAnswer>
	class TPending
	{
	public:
		TPending() : Future(Promise.get_future()) {}

		void Set(const TAnswer& Answer)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (!bSet)
			{
				bSet = true;
				Promise.set_value(Answer);
			}
		}

		/** True once the answer is there. */
		bool Wait(std::chrono::milliseconds Duration) { return Future.wait_for(Duration) == std::future_status::ready; }

		TAnswer Get() { return Future.get(); }

	private:
		std::mutex Mutex;
		bool bSet = false;
		std::promise<TAnswer> Promise;
		std::future<TAnswer> Future;
	};

	using FPendingAnswer = TPending<krtu::UiAnswer>;
	using FPendingReminder = TPending<krtu::UiReminderAnswer>;

	class FSlateUiHost final : public IKrtuworksUiHost, public std::enable_shared_from_this<FSlateUiHost>
	{
	public:
		explicit FSlateUiHost(std::shared_ptr<FKrtuworksUiLook> InLook) : Look(std::move(InLook)) {}

		krtu::UiAnswer show(const krtu::UiScreen& Screen, const krtu::CancelToken& Dismiss) override
		{
			if (IsInGameThread())
			{
				UE_LOG(LogKrtuworks, Error, TEXT("Krtuworks: a ready-made screen was asked for on the game thread; the screens are only for the SDK's own threads."));
				return krtu::UiAnswer();
			}
			const std::shared_ptr<FPendingAnswer> Pending = std::make_shared<FPendingAnswer>();
			AsyncTask(ENamedThreads::GameThread, [Self = shared_from_this(), Screen, Pending]() { Self->Open(Screen, Pending); });
			while (!Pending->Wait(std::chrono::milliseconds(50)))
			{
				if (Dismiss.cancelled())
				{
					Pending->Set(krtu::UiAnswer());
				}
			}
			return Pending->Get();
		}

		void update(const krtu::UiScreen& Screen) override
		{
			AsyncTask(ENamedThreads::GameThread, [Self = shared_from_this(), Screen]() {
				if (Self->Overlay.IsValid())
				{
					Self->Overlay->SetScreen(Screen);
					Self->FocusOverlay();
				}
			});
		}

		void close() override
		{
			AsyncTask(ENamedThreads::GameThread, [Self = shared_from_this()]() { Self->RemoveOverlay(); });
		}

		krtu::UiReminderAnswer remind() override
		{
			if (IsInGameThread())
			{
				return krtu::UiReminderAnswer::Later;
			}
			const std::shared_ptr<FPendingReminder> Pending = std::make_shared<FPendingReminder>();
			AsyncTask(ENamedThreads::GameThread, [Self = shared_from_this(), Pending]() { Self->OpenReminder(Pending); });
			return Pending->Get();
		}

		void Shutdown() override
		{
			RemoveOverlay();
			CloseReminder(krtu::UiReminderAnswer::Later);
			if (Waiting)
			{
				Waiting->Set(krtu::UiAnswer());
				Waiting.reset();
			}
		}

	private:
		/** Game thread: puts `Screen` up, or replaces what the open screen of its kind shows. */
		void Open(const krtu::UiScreen& Screen, const std::shared_ptr<FPendingAnswer>& Pending)
		{
			if (Waiting && Waiting != Pending)
			{
				Waiting->Set(krtu::UiAnswer());
			}
			Waiting = Pending;
			if (GEngine == nullptr || GEngine->GameViewport == nullptr)
			{
				UE_LOG(LogKrtuworks, Warning, TEXT("Krtuworks: no game viewport to show a screen on."));
				Pending->Set(krtu::UiAnswer());
				return;
			}
			if (Overlay.IsValid() && OverlayKind == Screen.kind)
			{
				Overlay->SetScreen(Screen);
				FocusOverlay();
				return;
			}
			RemoveOverlay();
			OverlayKind = Screen.kind;
			std::weak_ptr<FSlateUiHost> Weak = weak_from_this();
			Overlay = SNew(SKrtuworksScreen).Look(Look).Screen(Screen).OnAnswer(FKrtuworksScreenAnswer::CreateLambda([Weak](const krtu::UiAnswer& Answer) {
				if (const std::shared_ptr<FSlateUiHost> Self = Weak.lock())
				{
					// The question being asked now; once answered, a second press before the controller closes the screen is not another answer.
					if (Self->Waiting)
					{
						Self->Waiting->Set(Answer);
					}
				}
			}));
			GEngine->GameViewport->AddViewportWidgetContent(Overlay.ToSharedRef(), ScreenOrder);
			TakeInput();
		}

		/** Game thread: the screen has the keyboard and the gamepad (and the cursor) until it closes. */
		void TakeInput()
		{
			if (UWorld* World = GEngine->GameViewport->GetWorld())
			{
				if (APlayerController* Controller = GEngine->GetFirstLocalPlayerController(World))
				{
					Player = Controller;
					bCursorBefore = Controller->bShowMouseCursor;
					Controller->SetShowMouseCursor(true);
					FInputModeUIOnly Mode;
					Mode.SetWidgetToFocus(FocusTarget());
					Controller->SetInputMode(Mode);
				}
			}
			FocusOverlay();
		}

		TSharedPtr<SWidget> FocusTarget() const
		{
			return Overlay->GetInitialFocus().IsValid() ? Overlay->GetInitialFocus() : TSharedPtr<SWidget>(Overlay);
		}

		/** Game thread: focus goes to the screen's first control, now and again once the widget has joined the window's layout on the next frame. */
		void FocusOverlay()
		{
			if (!Overlay.IsValid() || !FSlateApplication::IsInitialized())
			{
				return;
			}
			FSlateApplication::Get().SetUserFocus(0, FocusTarget(), EFocusCause::SetDirectly);
			const std::weak_ptr<FSlateUiHost> Weak = weak_from_this();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float) {
				if (const std::shared_ptr<FSlateUiHost> Self = Weak.lock())
				{
					if (Self->Overlay.IsValid() && FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().SetUserFocus(0, Self->FocusTarget(), EFocusCause::SetDirectly);
					}
				}
				return false;
			}));
		}

		void RemoveOverlay()
		{
			if (!Overlay.IsValid())
			{
				return;
			}
			if (GEngine != nullptr && GEngine->GameViewport != nullptr)
			{
				GEngine->GameViewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
			}
			Overlay.Reset();
			if (APlayerController* Controller = Player.Get())
			{
				Controller->SetShowMouseCursor(bCursorBefore);
				Controller->SetInputMode(FInputModeGameOnly());
			}
			Player.Reset();
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetAllUserFocusToGameViewport();
			}
		}

		void OpenReminder(const std::shared_ptr<FPendingReminder>& Pending)
		{
			CloseReminder(krtu::UiReminderAnswer::Later);
			if (GEngine == nullptr || GEngine->GameViewport == nullptr)
			{
				Pending->Set(krtu::UiReminderAnswer::Later);
				return;
			}
			Reminding = Pending;
			const std::weak_ptr<FSlateUiHost> Weak = weak_from_this();
			Reminder = SNew(SKrtuworksReminder)
						   .Look(Look)
						   .OnLink(FKrtuworksChosen::CreateLambda([Weak]() { AnswerReminder(Weak, krtu::UiReminderAnswer::Link); }))
						   .OnLater(FKrtuworksChosen::CreateLambda([Weak]() { AnswerReminder(Weak, krtu::UiReminderAnswer::Later); }));
			GEngine->GameViewport->AddViewportWidgetContent(Reminder.ToSharedRef(), ReminderOrder);
			ReminderTimer = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, Pending](float) {
				if (const std::shared_ptr<FSlateUiHost> Self = Weak.lock())
				{
					if (Self->Reminding == Pending)
					{
						Self->CloseReminder(krtu::UiReminderAnswer::Later);
					}
				}
				return false;
			}), ReminderSeconds);
		}

		/** The prompt's button was pressed: it closes on the next turn, not while the button is still handling the press. */
		static void AnswerReminder(const std::weak_ptr<FSlateUiHost>& Weak, krtu::UiReminderAnswer Answer)
		{
			AsyncTask(ENamedThreads::GameThread, [Weak, Answer]() {
				if (const std::shared_ptr<FSlateUiHost> Self = Weak.lock())
				{
					Self->CloseReminder(Answer);
				}
			});
		}

		void CloseReminder(krtu::UiReminderAnswer Answer)
		{
			if (Reminder.IsValid() && GEngine != nullptr && GEngine->GameViewport != nullptr)
			{
				GEngine->GameViewport->RemoveViewportWidgetContent(Reminder.ToSharedRef());
			}
			Reminder.Reset();
			if (Reminding)
			{
				FTSTicker::GetCoreTicker().RemoveTicker(ReminderTimer);
				Reminding->Set(Answer);
				Reminding.reset();
			}
		}

		std::shared_ptr<FKrtuworksUiLook> Look;
		// Game thread only, from here on.
		TSharedPtr<SKrtuworksScreen> Overlay;
		krtu::UiScreenKind OverlayKind = krtu::UiScreenKind::Error;
		std::shared_ptr<FPendingAnswer> Waiting;
		TWeakObjectPtr<APlayerController> Player;
		bool bCursorBefore = false;
		TSharedPtr<SKrtuworksReminder> Reminder;
		std::shared_ptr<FPendingReminder> Reminding;
		FTSTicker::FDelegateHandle ReminderTimer;
	};
}

namespace KrtuworksUi
{
	std::shared_ptr<IKrtuworksUiHost> MakeSlateHost(std::shared_ptr<FKrtuworksUiLook> Look)
	{
		return std::make_shared<FSlateUiHost>(std::move(Look));
	}
}
