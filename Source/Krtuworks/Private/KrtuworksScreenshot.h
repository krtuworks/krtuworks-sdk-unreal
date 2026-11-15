#pragma once

#include "CoreMinimal.h"

/** A picture of the screen for a bug report, taken by the engine at the end of a frame. */
namespace KrtuworksScreenshot
{
	/**
	 * Asks the engine for the next frame as a JPEG (quality 1-100) and calls `Done` on the game thread with its
	 * bytes, or with nothing when the engine draws no picture (a server, -nullrhi) or gives none within 3 seconds.
	 */
	void Capture(int32 Quality, TFunction<void(TArray<uint8>)> Done);
}
