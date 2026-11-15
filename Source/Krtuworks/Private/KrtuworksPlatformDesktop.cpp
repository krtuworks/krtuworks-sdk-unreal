// What desktops share: they sign in through the system browser (the core's loopback listener), so the phone
// functions are never reached there; and Linux (Steam Deck included) seals its secrets with the machine-bound key.
#include "KrtuworksPlatform.h"

#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX

#include "KrtuworksCore/Store.h"

#include <stdexcept>

namespace KrtuworksPlatform
{
	bool SignsInOnPhone()
	{
		return false;
	}

	std::string AuthenticateOnPhone(const std::string&, const std::string&, const krtu::CancelToken&)
	{
		throw std::logic_error("desktops sign in through the system browser");
	}

	std::optional<std::string> LaunchAddress()
	{
		return std::nullopt;
	}

	void Resumed()
	{
	}

#if PLATFORM_LINUX
	std::string PlatformName()
	{
		return "LINUX";
	}

	std::string OsName()
	{
		return "Linux";
	}

	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string& Root, const std::string& AppId)
	{
		const auto Folder = std::make_shared<krtu::StdFileSystem>(Root);
		return std::make_shared<krtu::SealedFileStore>(std::make_shared<krtu::StdFileSystem>(Root + "/secrets"), std::make_shared<krtu::MachineBoundMasterKey>(AppId, Folder, "secrets.key"));
	}
#endif
}

#endif
