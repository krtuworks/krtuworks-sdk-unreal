// Android: the secrets are sealed files in the app's no-backup folder (never in Auto Backup or a
// device-to-device transfer), their key wrapped by an AES key that never leaves the Android Keystore; the
// sign-in opens the browser and comes back as a deep link on the app's scheme (Krtuworks_UPL.xml adds it to
// GameActivity and hands the address to KrtuworksAndroid.java, which calls nativeAddress below).
#include "KrtuworksPlatform.h"

#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Store.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace
{
	/** Back in the app with no address this long after it came back = the player closed the browser. */
	constexpr auto NoAddressAfterResume = std::chrono::seconds(2);

	struct FAndroidSignIn
	{
		std::mutex Mutex;
		std::condition_variable Arrived;
		std::string Scheme;
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

	/** Marks a key kept as it is: Android before 6.0 has no Keystore AES (the folder is still private and never backed up). */
	const std::string PlainPrefix = "plain:";

	/**
	 * The sealing key encrypted by the Keystore (KrtuworksKeystore.java), kept in the no-backup folder. Restored
	 * onto another phone it cannot be opened, so the SDK starts as a new device.
	 */
	class FKeystoreMasterKey : public krtu::MasterKey
	{
	public:
		explicit FKeystoreMasterKey(std::shared_ptr<krtu::FileSystem> InFolder) : Folder(MoveTemp(InFolder)) {}

		std::optional<std::string> getOrCreate() override
		{
			if (const std::optional<std::string> Kept = Folder->read("secrets.key"))
			{
				std::optional<std::string> Opened;
				if (Kept->compare(0, PlainPrefix.size(), PlainPrefix) == 0)
				{
					Opened = Kept->substr(PlainPrefix.size());
				}
				else
				{
					Opened = CallStringWithText(KeystoreClass(), "open", *Kept);
				}
				// Nothing: the Keystore failed just now. Making a new key would orphan every sealed secret; wait instead.
				if (!Opened)
				{
					return std::nullopt;
				}
				if (!Opened->empty())
				{
					return krtu::fromBase64(*Opened);
				}
			}
			const std::string Key = krtu::randomBytes(32);
			std::optional<std::string> Sealed = CallStringWithText(KeystoreClass(), "seal", krtu::base64(Key));
			if (!Sealed || Sealed->empty())
			{
				Sealed = PlainPrefix + krtu::base64(Key);
			}
			Folder->writeAtomic("secrets.key", *Sealed, true);
			return Key;
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
		if (!State.bWaiting || Text.compare(0, State.Scheme.size() + 1, State.Scheme + ":") != 0)
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
		{
			std::lock_guard<std::mutex> Lock(State.Mutex);
			State.Scheme = Scheme;
			State.Address.clear();
			State.bWaiting = true;
			State.bResumed = false;
		}
		{
			JNIEnv* Env = FAndroidApplication::GetJavaEnv();
			const jmethodID Open = Env->GetStaticMethodID(HelperClass(), "openBrowser", "(Landroid/app/Activity;Ljava/lang/String;)V");
			jstring Address = Env->NewStringUTF(Url.c_str());
			Env->CallStaticVoidMethod(HelperClass(), Open, FJavaWrapper::GameActivityThis, Address);
			Env->DeleteLocalRef(Address);
		}
		std::unique_lock<std::mutex> Lock(State.Mutex);
		while (State.Address.empty())
		{
			State.Arrived.wait_for(Lock, std::chrono::milliseconds(100));
			const bool bClosed = State.bResumed && std::chrono::steady_clock::now() - State.ResumedAt > NoAddressAfterResume;
			if (State.Address.empty() && (Cancel.cancelled() || bClosed))
			{
				State.bWaiting = false;
				throw krtu::Cancelled();
			}
		}
		State.bWaiting = false;
		return State.Address;
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
