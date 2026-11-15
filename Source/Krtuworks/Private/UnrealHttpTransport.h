#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Http.h"

/**
 * The core's HTTP through the engine's HTTP module. send() blocks the SDK thread that calls it until the
 * answer is complete; the engine finishes requests on the game thread, so it must never be called from there
 * (it refuses to, rather than wait forever).
 */
class FUnrealHttpTransport : public krtu::HttpTransport
{
public:
	krtu::HttpAnswer send(const krtu::HttpCall& Call) override;
};
