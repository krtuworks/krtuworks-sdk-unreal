// iOS: the secrets are Keychain items themselves (accessible after the first unlock, this device only: not in
// iCloud backups, not moved to a new iPhone, kept across a reinstall on the same one), and the sign-in is
// ASWebAuthenticationSession, which waits for the app's own scheme and shares Safari's sign-in.
#include "KrtuworksPlatform.h"

#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Store.h"

#import <AuthenticationServices/AuthenticationServices.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <UIKit/UIKit.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace
{
	NSString* IosString(const std::string& Text)
	{
		return [[NSString alloc] initWithBytes:Text.data() length:Text.size() encoding:NSUTF8StringEncoding];
	}

	NSMutableDictionary* IosQuery(NSString* Service, const std::string* Key)
	{
		NSMutableDictionary* Query = [@{
			(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
			(__bridge id)kSecAttrService: Service,
		} mutableCopy];
		if (Key != nullptr)
		{
			Query[(__bridge id)kSecAttrAccount] = IosString(*Key);
		}
		return Query;
	}

	/** One generic password per key under the app's service (com.krtuworks.sdk.<appId>). */
	class FIosKeychainStore : public krtu::KeyValueStore
	{
	public:
		explicit FIosKeychainStore(const std::string& AppId) : Service(IosString("com.krtuworks.sdk." + AppId)) {}

		std::optional<std::string> get(const std::string& Key) override
		{
			NSMutableDictionary* Query = IosQuery(Service, &Key);
			Query[(__bridge id)kSecReturnData] = @YES;
			Query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
			CFTypeRef Result = nullptr;
			if (SecItemCopyMatching((__bridge CFDictionaryRef)Query, &Result) != errSecSuccess || Result == nullptr)
			{
				return std::nullopt;
			}
			NSData* Data = (__bridge_transfer NSData*)Result;
			return std::string(static_cast<const char*>(Data.bytes), Data.length);
		}

		void set(const std::string& Key, const std::string& Value) override
		{
			NSData* Data = [NSData dataWithBytes:Value.data() length:Value.size()];
			NSMutableDictionary* Query = IosQuery(Service, &Key);
			NSDictionary* Fields = @{
				(__bridge id)kSecValueData: Data,
				(__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
			};
			OSStatus Status = SecItemUpdate((__bridge CFDictionaryRef)Query, (__bridge CFDictionaryRef)Fields);
			if (Status == errSecItemNotFound)
			{
				[Query addEntriesFromDictionary:Fields];
				Status = SecItemAdd((__bridge CFDictionaryRef)Query, nullptr);
			}
			if (Status != errSecSuccess)
			{
				throw krtu::Error("secret_storage_unavailable", "The Keychain refused the item (" + std::to_string(static_cast<int>(Status)) + ").");
			}
		}

		void remove(const std::string& Key) override
		{
			SecItemDelete((__bridge CFDictionaryRef)IosQuery(Service, &Key));
		}

		std::vector<std::string> keys(const std::string& Prefix) override
		{
			NSMutableDictionary* Query = IosQuery(Service, nullptr);
			Query[(__bridge id)kSecReturnAttributes] = @YES;
			Query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitAll;
			CFTypeRef Result = nullptr;
			std::vector<std::string> Names;
			if (SecItemCopyMatching((__bridge CFDictionaryRef)Query, &Result) != errSecSuccess || Result == nullptr)
			{
				return Names;
			}
			NSArray* Items = (__bridge_transfer NSArray*)Result;
			for (NSDictionary* Item in Items)
			{
				NSString* Account = Item[(__bridge id)kSecAttrAccount];
				if (Account == nil)
				{
					continue;
				}
				std::string Name(Account.UTF8String);
				if (Name.compare(0, Prefix.size(), Prefix) == 0)
				{
					Names.push_back(Name);
				}
			}
			return Names;
		}

	private:
		NSString* Service;
	};

	/** The one sign-in session the app has open, and the answer it is waiting for. */
	struct FIosSignIn
	{
		std::mutex Mutex;
		std::condition_variable Done;
		bool bFinished = false;
		std::string Address;
		std::string Failure;
	};
}

API_AVAILABLE(ios(13.0))
@interface KrtuworksAuthPresenter : NSObject <ASWebAuthenticationPresentationContextProviding>
@end

@implementation KrtuworksAuthPresenter
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession*)session
{
	for (UIScene* Scene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![Scene isKindOfClass:UIWindowScene.class])
		{
			continue;
		}
		for (UIWindow* Window in ((UIWindowScene*)Scene).windows)
		{
			if (Window.isKeyWindow)
			{
				return Window;
			}
		}
	}
	return UIApplication.sharedApplication.windows.firstObject;
}
@end

static ASWebAuthenticationSession* GKrtuworksSession = nil;
static id GKrtuworksPresenter = nil;

namespace KrtuworksPlatform
{
	std::string PlatformName()
	{
		return "IOS";
	}

	std::string OsName()
	{
		return "iOS";
	}

	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string&, const std::string& AppId)
	{
		return std::make_shared<FIosKeychainStore>(AppId);
	}

	bool SignsInOnPhone()
	{
		return true;
	}

	std::string AuthenticateOnPhone(const std::string& Url, const std::string& Scheme, const krtu::CancelToken& Cancel)
	{
		const std::shared_ptr<FIosSignIn> Wait = std::make_shared<FIosSignIn>();
		NSURL* Target = [NSURL URLWithString:IosString(Url)];
		NSString* CallbackScheme = IosString(Scheme);
		dispatch_async(dispatch_get_main_queue(), ^{
			[GKrtuworksSession cancel];
			__block __weak ASWebAuthenticationSession* Started = nil;
			ASWebAuthenticationSession* Session = [[ASWebAuthenticationSession alloc] initWithURL:Target callbackURLScheme:CallbackScheme completionHandler:^(NSURL* Callback, NSError* Error) {
				// A session replaced by a newer one answers late (its cancel): that answer is not the newer one's.
				if (GKrtuworksSession == nil || GKrtuworksSession != Started)
				{
					return;
				}
				GKrtuworksSession = nil;
				{
					std::lock_guard<std::mutex> Lock(Wait->Mutex);
					if (Callback != nil)
					{
						Wait->Address = Callback.absoluteString.UTF8String;
					}
					else
					{
						Wait->Failure = Error.code == ASWebAuthenticationSessionErrorCodeCanceledLogin ? "cancelled" : std::to_string(static_cast<long>(Error.code));
					}
					Wait->bFinished = true;
				}
				Wait->Done.notify_all();
			}];
			Started = Session;
			GKrtuworksSession = Session;
			if (@available(iOS 13.0, *))
			{
				GKrtuworksPresenter = [KrtuworksAuthPresenter new];
				Session.presentationContextProvider = GKrtuworksPresenter;
				Session.prefersEphemeralWebBrowserSession = NO;
			}
			if (![Session start])
			{
				GKrtuworksSession = nil;
				{
					std::lock_guard<std::mutex> Lock(Wait->Mutex);
					Wait->Failure = "start_failed";
					Wait->bFinished = true;
				}
				Wait->Done.notify_all();
			}
		});
		std::unique_lock<std::mutex> Lock(Wait->Mutex);
		while (!Wait->Done.wait_for(Lock, std::chrono::milliseconds(100), [&Wait] { return Wait->bFinished; }))
		{
			if (Cancel.cancelled())
			{
				Lock.unlock();
				dispatch_async(dispatch_get_main_queue(), ^{
					[GKrtuworksSession cancel];
					GKrtuworksSession = nil;
				});
				throw krtu::Cancelled();
			}
		}
		if (Wait->Failure == "cancelled")
		{
			throw krtu::Cancelled();
		}
		if (!Wait->Failure.empty())
		{
			throw krtu::Error("sign_in_failed", "The sign-in did not finish: " + Wait->Failure);
		}
		return Wait->Address;
	}

	std::optional<std::string> LaunchAddress()
	{
		// ASWebAuthenticationSession keeps the app running while the page is open: no cold start to finish.
		return std::nullopt;
	}

	void Resumed()
	{
	}
}
