#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Ui.h"

#include <memory>

struct FKrtuworksUiLook;

/** The screens' host: draws them on the game thread, answers the controller's blocking calls from there. */
class IKrtuworksUiHost : public krtu::UiHost
{
public:
	/** Closes whatever is open and answers every waiting call as dismissed. Game thread. */
	virtual void Shutdown() = 0;
};

namespace KrtuworksUi
{
	/** The Slate host: a viewport overlay, gamepad and keyboard focus, `Look` read whenever a screen opens. */
	std::shared_ptr<IKrtuworksUiHost> MakeSlateHost(std::shared_ptr<FKrtuworksUiLook> Look);
}
