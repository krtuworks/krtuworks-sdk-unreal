#pragma once

#include "CoreMinimal.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Ui.h"
#include "KrtuworksCore/UiText.h"
#include "KrtuworksSettings.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

/**
 * What the ready-made screens look like and say. The subsystem changes it (SetUiTheme, SetUiText) and the screens
 * read it when they open, both on the game thread.
 */
struct FKrtuworksUiLook
{
	FKrtuworksUiTheme Theme;
	krtu::UiTexts Texts;
	/** The "A Select  B Back" row. */
	bool bControllerHints = false;

	/** The sentence for `Key` (the game's, else the English one) with `{name}` filled from `Vars`. */
	FString Text(const char* Key, const std::map<std::string, std::string>& Vars = {}) const
	{
		return KrtuworksConvert::ToFString(krtu::textFor(Texts, Key, Vars));
	}
};

/**
 * The controller of the screens and the game's save describer, handed to the SDK's own threads: the link reminder and
 * the platform cloud's conflict reach them from there.
 */
class FKrtuworksUiSlot
{
public:
	using FDescriber = std::function<std::optional<krtu::SaveSummary>(const std::string& Slot, const std::string& Bytes)>;

	std::shared_ptr<krtu::UiController> Get()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Controller;
	}

	void Set(std::shared_ptr<krtu::UiController> Next)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Controller = std::move(Next);
	}

	void SetDescriber(FDescriber Next)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Describer = std::move(Next);
	}

	std::optional<krtu::SaveSummary> Describe(const std::string& Slot, const std::string& Bytes)
	{
		FDescriber Held;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Held = Describer;
		}
		return Held ? Held(Slot, Bytes) : std::nullopt;
	}

private:
	std::mutex Mutex;
	std::shared_ptr<krtu::UiController> Controller;
	FDescriber Describer;
};
