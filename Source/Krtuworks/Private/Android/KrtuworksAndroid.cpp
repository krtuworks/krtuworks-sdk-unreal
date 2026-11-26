// Android: the secrets are sealed files in the app's no-backup folder (never in Auto Backup or a
// device-to-device transfer), their key wrapped by an AES key that never leaves the Android Keystore; the
// sign-in opens the browser and comes back as a deep link on the app's scheme (Krtuworks_UPL.xml adds it to
// GameActivity and hands the address to KrtuworksAndroid.java, which calls nativeAddress below).
#include "KrtuworksPlatform.h"

#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Identity.h"
#include "KrtuworksCore/Store.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace
{
	/** Back in the app with no address this long after it came back = the player closed the browser. */
	constexpr auto NoAddressAfterResume = std::chrono::seconds(2);

	/**
	 * The one open sign-in page. Each call takes a new generation; a newer sign-in replaces the open one,
	 * and the replaced call ends as cancelled instead of taking the newer call's address.
	 */
	struct FAndroidSignIn
	{
		std::mutex Mutex;
		std::condition_variable Arrived;
		std::uint64_t Generation = 0;
		std::string Scheme;
		/** The address the waiting sign-in opened: a return carrying another sign-in's state is not its. */
		std::string OpenedUrl;
		std::string Address;
		bool bWaiting = false;
		std::chrono::steady_clock::time_point ResumedAt{};
		bool bResumed = false;
	};

	FAndroidSignIn& SignIn()
	{
		static FAndroidSignIn State;
		return State;
	}

	jclass HelperClass()
	{
		static jclass Class = FAndroidApplication::FindJavaClassGlobalRef("com/krtuworks/sdk/KrtuworksAndroid");
		return Class;
	}

	jclass KeystoreClass()
	{
		static jclass Class = FAndroidApplication::FindJavaClassGlobalRef("com/krtuworks/sdk/KrtuworksKeystore");
		return Class;
	}

	/** A Java string as UTF-8; nothing for null. Frees the local reference. */
	std::optional<std::string> AndroidString(JNIEnv* Env, jstring Text)
	{
		if (Text == nullptr)
		{
			return std::nullopt;
		}
		const char* Chars = Env->GetStringUTFChars(Text, nullptr);
		std::string Out(Chars);
		Env->ReleaseStringUTFChars(Text, Chars);
		Env->DeleteLocalRef(Text);
		return Out;
	}

	/** A static helper taking (Activity) or (String) and answering a String. */
	std::optional<std::string> CallString(jclass Class, const char* Method, const char* Signature, jobject Argument)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		const jmethodID Id = Env->GetStaticMethodID(Class, Method, Signature);
		jstring Result = static_cast<jstring>(Env->CallStaticObjectMethod(Class, Id, Argument));
		if (Env->ExceptionCheck())
		{
			Env->ExceptionClear();
			return std::nullopt;
		}
		return AndroidString(Env, Result);
	}

	std::optional<std::string> CallStringWithText(jclass Class, const char* Method, const std::string& Text)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jstring Argument = Env->NewStringUTF(Text.c_str());
		std::optional<std::string> Result = CallString(Class, Method, "(Ljava/lang/String;)Ljava/lang/String;", Argument);
		Env->DeleteLocalRef(Argument);
		return Result;
	}

	/**
	 * The sealing key encrypted by the Keystore (KrtuworksKeystore.java), kept in the no-backup folder. Restored
	 * onto another phone it cannot be opened, so the SDK starts as a new device. Where sealing fails the key is
	 * kept as `plain:` text and sealed on a later launch (krtu::resolveKeystoreKey).
	 */
	class FKeystoreMasterKey : public krtu::MasterKey
	{
	public:
		explicit FKeystoreMasterKey(std::shared_ptr<krtu::FileSystem> InFolder) : Folder(MoveTemp(InFolder)) {}

		std::optional<std::string> getOrCreate() override
		{
			return krtu::resolveKeystoreKey(
				*Folder, "secrets.key",
				[](const std::string& Text) { return CallStringWithText(KeystoreClass(), "open", Text); },
				[](const std::string& Secret) { return CallStringWithText(KeystoreClass(), "seal", Secret); });
		}

	private:
		std::shared_ptr<krtu::FileSystem> Folder;
	};
}

JNI_METHOD void Java_com_krtuworks_sdk_KrtuworksAndroid_nativeAddress(JNIEnv* Env, jclass, jstring Address)
{
	const char* Chars = Env->GetStringUTFChars(Address, nullptr);
	const std::string Text(Chars);
	Env->ReleaseStringUTFChars(Address, Chars);
	FAndroidSignIn& State = SignIn();
	{
		std::lock_guard<std::mutex> Lock(State.Mutex);
		if (!State.bWaiting || Text.compare(0, State.Scheme.size() + 1, State.Scheme + ":") != 0 || !krtu::detail::returnBelongsTo(State.OpenedUrl, Text))
		{
			return;
		}
		State.Address = Text;
	}
	State.Arrived.notify_all();
}

namespace KrtuworksPlatform
{
	std::string PlatformName()
	{
		return "ANDROID";
	}

	std::string OsName()
	{
		return "Android";
	}

	std::shared_ptr<krtu::KeyValueStore> MakeSecretStore(const std::string&, const std::string&)
	{
		const std::optional<std::string> NoBackup = CallString(HelperClass(), "noBackupDir", "(Landroid/app/Activity;)Ljava/lang/String;", FJavaWrapper::GameActivityThis);
		if (!NoBackup)
		{
			throw std::runtime_error("the app's no-backup folder is not available");
		}
		const std::string Root = *NoBackup + "/krtuworks";
		return std::make_shared<krtu::SealedFileStore>(std::make_shared<krtu::StdFileSystem>(Root + "/secrets"), std::make_shared<FKeystoreMasterKey>(std::make_shared<krtu::StdFileSystem>(Root)));
	}

	bool SignsInOnPhone()
	{
		return true;
	}

	std::string AuthenticateOnPhone(const std::string& Url, const std::string& Scheme, const krtu::CancelToken& Cancel)
	{
		FAndroidSignIn& State = SignIn();
		std::uint64_t Mine = 0;
		{
			std::lock_guard<std::mutex> Lock(State.Mutex);
			Mine = ++State.Generation;
			State.Scheme = Scheme;
			State.OpenedUrl = Url;
			State.Address.clear();
			State.bWaiting = true;
			State.bResumed = false;
		}
		bool bOpened = false;
		{
			JNIEnv* Env = FAndroidApplication::GetJavaEnv();
			const jmethodID Open = Env->GetStaticMethodID(HelperClass(), "openBrowser", "(Landroid/app/Activity;Ljava/lang/String;)Z");
			jstring Address = Env->NewStringUTF(Url.c_str());
			bOpened = Env->CallStaticBooleanMethod(HelperClass(), Open, FJavaWrapper::GameActivityThis, Address) == JNI_TRUE;
			Env->DeleteLocalRef(Address);
			if (Env->ExceptionCheck())
			{
				Env->ExceptionClear();
				bOpened = false;
			}
		}
		if (!bOpened)
		{
			std::lock_guard<std::mutex> Lock(State.Mutex);
			if (State.Generation == Mine)
			{
				State.bWaiting = false;
			}
			throw krtu::Error("browser_unavailable", "No browser could be opened for the sign-in.");
		}
		std::unique_lock<std::mutex> Lock(State.Mutex);
		while (true)
		{
			if (State.Generation != Mine)
			{
				throw krtu::Cancelled();
			}
			if (!State.Address.empty())
			{
				break;
			}
			State.Arrived.wait_for(Lock, std::chrono::milliseconds(100));
			if (State.Generation != Mine)
			{
				throw krtu::Cancelled();
			}
			const bool bClosed = State.bResumed && std::chrono::steady_clock::now() - State.ResumedAt > NoAddressAfterResume;
			if (State.Address.empty() && (Cancel.cancelled() || bClosed))
			{
				State.bWaiting = false;
				throw krtu::Cancelled();
			}
		}
		State.bWaiting = false;
		std::string Out = State.Address;
		State.Address.clear();
		return Out;
	}

	std::optional<std::string> LaunchAddress()
	{
		return CallString(HelperClass(), "launchAddress", "(Landroid/app/Activity;)Ljava/lang/String;", FJavaWrapper::GameActivityThis);
	}

	void Resumed()
	{
		FAndroidSignIn& State = SignIn();
		std::lock_guard<std::mutex> Lock(State.Mutex);
		if (State.bWaiting)
		{
			State.bResumed = true;
			State.ResumedAt = std::chrono::steady_clock::now();
		}
	}
}
