// macOS: the secrets are sealed files whose key is a generic password in the user's login Keychain (service
// com.krtuworks.sdk, account = the app). The Keychain trusts the app that made the item by its code signature:
// a build signed with a Developer ID keeps that trust across updates; an unsigned build asks again after each.
#include "KrtuworksPlatform.h"

#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Store.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace
{
	CFStringRef MacString(const std::string& Text)
	{
		return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(Text.data()), static_cast<CFIndex>(Text.size()), kCFStringEncodingUTF8, false);
	}

	/** The query naming the app's item; the caller releases it. */
	CFMutableDictionaryRef MacQuery(const std::string& Account)
	{
		CFMutableDictionaryRef Query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFStringRef Service = MacString("com.krtuworks.sdk");
		CFStringRef Name = MacString(Account);
		CFDictionarySetValue(Query, kSecClass, kSecClassGenericPassword);
		CFDictionarySetValue(Query, kSecAttrService, Service);
		CFDictionarySetValue(Query, kSecAttrAccount, Name);
		CFRelease(Service);
		CFRelease(Name);
		return Query;
	}

	class FKeychainMasterKey : public krtu::MasterKey
	{
	public:
		explicit FKeychainMasterKey(std::string InAppId) : AppId(MoveTemp(InAppId)) {}

		std::optional<std::string> getOrCreate() override
		{
			if (const std::optional<std::string> Found = Find())
			{
				return Found;
			}
			const std::string Key = krtu::randomBytes(32);
			CFMutableDictionaryRef Query = MacQuery(AppId);
			CFDataRef Data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(Key.data()), static_cast<CFIndex>(Key.size()));
			CFDictionarySetValue(Query, kSecValueData, Data);
			const OSStatus Status = SecItemAdd(Query, nullptr);
			CFRelease(Data);
			CFRelease(Query);
			if (Status == errSecDuplicateItem)
			{
				// Made by another launch between the two calls: that one is the key.
				return Find();
			}
			if (Status != errSecSuccess)
			{
				// Locked or refused now: nothing is made, nothing is lost; the store waits.
				return std::nullopt;
			}
			return Key;
		}

	private:
		std::optional<std::string> Find() const
		{
			CFMutableDictionaryRef Query = MacQuery(AppId);
			CFDictionarySetValue(Query, kSecReturnData, kCFBooleanTrue);
			CFDictionarySetValue(Query, kSecMatchLimit, kSecMatchLimitOne);
			CFTypeRef Result = nullptr;
			const OSStatus Status = SecItemCopyMatching(Query, &Result);
			CFRelease(Query);
			if (Status != errSecSuccess || Result == nullptr)
			{
				return std::nullopt;
			}
			CFDataRef Data = static_cast<CFDataRef>(Result);
			std::string Key(reinterpret_cast<const char*>(CFDataGetBytePtr(Data)), static_cast<size_t>(CFDataGetLength(Data)));
			CFRelease(Result);
			if (Key.size() != 32)
			{
				return std::nullopt;
			}
			return Key;
		}

		std::string AppId;
	};
}

namespace KrtuworksPlatform
{
	std::string PlatformName()
	{
		return "MACOS";
	}

	std::string OsName()
	{
		return "macOS";
	}

	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string& Root, const std::string& AppId)
	{
		return std::make_shared<krtu::SealedFileStore>(std::make_shared<krtu::StdFileSystem>(Root + "/secrets"), std::make_shared<FKeychainMasterKey>(AppId));
	}
}
