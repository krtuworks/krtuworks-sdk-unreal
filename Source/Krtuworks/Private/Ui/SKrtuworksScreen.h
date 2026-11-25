#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Ui.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

#include <memory>

struct FKrtuworksUiLook;
class FKrtuworksRemoteImage;

DECLARE_DELEGATE_OneParam(FKrtuworksScreenAnswer, const krtu::UiAnswer&);

/**
 * One modal screen of the ready-made set (age, terms, under 13, save conflict, error, the browser's waiting screen, a
 * console's code) over a dimmed backdrop. Everything is reachable with a keyboard or a gamepad: Up and Down move
 * between controls, Accept presses, the back key (Escape, the gamepad's B) is the screen's back action.
 */
class SKrtuworksScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SKrtuworksScreen)
	{
	}
	SLATE_ARGUMENT(std::shared_ptr<FKrtuworksUiLook>, Look)
	SLATE_ARGUMENT(krtu::UiScreen, Screen)
	SLATE_EVENT(FKrtuworksScreenAnswer, OnAnswer)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows another screen of the same kind in place (a new code, an expired one). */
	void SetScreen(const krtu::UiScreen& Screen);

	/** The control that takes focus first. */
	TSharedPtr<SWidget> GetInitialFocus() const { return InitialFocus; }

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void Build();
	void Answer(const krtu::UiAnswer& Answer) const;
	TSharedRef<SWidget> Hints() const;

	std::shared_ptr<FKrtuworksUiLook> Look;
	krtu::UiScreen Screen;
	FKrtuworksScreenAnswer OnAnswer;
	TSharedPtr<SWidget> InitialFocus;
	/** What the back key answers; nothing on a screen that has no back action. */
	TOptional<krtu::UiAnswerKind> BackAnswer;
	/** The pictures of a conflict screen's saves, kept while the screen shows them. */
	TArray<TSharedPtr<FKrtuworksRemoteImage>> Images;
};
