// Windows: the secrets are sealed files whose key DPAPI protects for this Windows user (and this app).
#include "KrtuworksPlatform.h"

#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Store.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <dpapi.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
	/** DPAPI for this user, with the app's id as extra entropy; nothing when Windows refuses. */
	std::optional<std::string> Dpapi(const std::string& Input, const std::string& Entropy, bool bProtect)
	{
		DATA_BLOB In{static_cast<DWORD>(Input.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Input.data()))};
		DATA_BLOB Extra{static_cast<DWORD>(Entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Entropy.data()))};
		DATA_BLOB Out{};
		const BOOL bOk = bProtect ? CryptProtectData(&In, L"Krtuworks SDK", &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out)
								  : CryptUnprotectData(&In, nullptr, &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out);
		if (!bOk)
		{
			return std::nullopt;
		}
		std::string Result(reinterpret_cast<const char*>(Out.pbData), Out.cbData);
		LocalFree(Out.pbData);
		return Result;
	}

	/** The sealing key, protected with DPAPI in a file next to the secrets. */
	class FDpapiMasterKey : public krtu::MasterKey
	{
	public:
		FDpapiMasterKey(std::shared_ptr<krtu::FileSystem> InFolder, std::string InAppId) : Folder(MoveTemp(InFolder)), Entropy("krtuworks-sdk:" + InAppId) {}

		std::optional<std::string> getOrCreate() override
		{
			if (const std::optional<std::string> Kept = Folder->read("secrets.key"))
			{
				const std::optional<std::string> Opened = Dpapi(*Kept, Entropy, false);
				if (Opened && Opened->size() == 32)
				{
					return Opened;
				}
			}
			// None yet, or sealed for another user or machine (copied here): a new key; the old files no longer open.
			const std::string Key = krtu::randomBytes(32);
			const std::optional<std::string> Sealed = Dpapi(Key, Entropy, true);
			if (!Sealed)
			{
				return std::nullopt;
			}
			Folder->writeAtomic("secrets.key", *Sealed, true);
			return Key;
		}

	private:
		std::shared_ptr<krtu::FileSystem> Folder;
		std::string Entropy;
	};
}

namespace KrtuworksPlatform
{
	std::string PlatformName()
	{
		return "WINDOWS";
	}

	std::string OsName()
	{
		return "Windows";
	}

	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string& Root, const std::string& AppId)
	{
		return std::make_shared<krtu::SealedFileStore>(std::make_shared<krtu::StdFileSystem>(Root + "/secrets"), std::make_shared<FDpapiMasterKey>(std::make_shared<krtu::StdFileSystem>(Root), AppId));
	}
}
