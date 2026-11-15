#pragma once

#include "CoreMinimal.h"
#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Store.h"

#include <memory>
#include <optional>
#include <string>

/**
 * What differs per platform (Private/<Platform>/: the build takes only the running platform's folder), the
 * same choices as the Unity package's: where the secrets live and how the phone's sign-in comes back.
 */
namespace KrtuworksPlatform
{
	/** WINDOWS, MACOS, LINUX, IOS, ANDROID … (STEAM is chosen above this, when the Steam module is in). */
	std::string PlatformName();
	/** The operating system's name for the session's device description (Windows, macOS, iOS …). */
	std::string OsName();

	/**
	 * The secrets (tokens, the device key): Windows DPAPI, macOS Keychain and Android Keystore seal files whose
	 * key they keep; Linux and Steam Deck a key bound to the machine and user (owner's choice: one way for
	 * desktop and game mode); iOS the Keychain itself, this device only. `Root` is the SDK's own folder (UTF-8).
	 */
	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string& Root, const std::string& AppId);

	/** Whether this platform signs in with the phone's authentication session (iOS, Android) rather than the system browser. */
	bool SignsInOnPhone();
	/**
	 * The phone's sign-in (iOS ASWebAuthenticationSession; Android the browser and the app's scheme): opens
	 * `Url` and returns the address it came back to on `Scheme`; throws krtu::Cancelled when the player closed
	 * it or `Cancel` was set. Blocks an SDK thread.
	 */
	std::string AuthenticateOnPhone(const std::string& Url, const std::string& Scheme, const krtu::CancelToken& Cancel);
	/** The address that started the app, when the system closed it while the sign-in page was open (Android); nothing otherwise. */
	std::optional<std::string> LaunchAddress();
	/** The app came back to the foreground (Android: no address soon after = the browser was closed). */
	void Resumed();
}
