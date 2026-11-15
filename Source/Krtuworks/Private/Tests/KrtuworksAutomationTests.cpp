// The engine layer's own checks, run inside the engine (headless in the editor: Automation RunTests Krtuworks).
// They need no Krtuworks server: what they prove is what the core's tests cannot, that the engine's pieces
// behave as the layer assumes (text conversion, the platform's secret store, the engine's HTTP module finishing
// a request while the game thread keeps ticking, the system browser's loopback return reached through it).
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Crypto.h"
#include "KrtuworksCore/Error.h"
#include "KrtuworksCore/Http.h"
#include "KrtuworksPlatform.h"
#include "Loopback.h"
#include "Misc/Paths.h"
#include "UnrealHttpTransport.h"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	// The four contexts by name: from UE 5.6 on EAutomationTestFlags has no ApplicationContextMask member
	constexpr auto KrtuworksTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext
		| EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	template <class T>
	bool IsReady(const std::future<T>& Future)
	{
		return Future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}

	krtu::HttpCall LoopbackCall(int Port, const std::string& Path)
	{
		krtu::HttpCall Call;
		Call.url = "http://127.0.0.1:" + std::to_string(Port) + Path;
		return Call;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKrtuworksConvertTest, "Krtuworks.Convert", KrtuworksTestFlags)

bool FKrtuworksConvertTest::RunTest(const FString& Parameters)
{
	// Turkish letters and an emoji (a surrogate pair where TCHAR is 16 bits) cross to UTF-8 and back unchanged.
	const FString Text(TEXT("Şğüİı \U0001F3AE"));
	const std::string Utf8 = KrtuworksConvert::ToStd(Text);
	TestTrue(TEXT("the text is UTF-8 for the core"), Utf8 == "\xC5\x9E\xC4\x9F\xC3\xBC\xC4\xB0\xC4\xB1 \xF0\x9F\x8E\xAE");
	TestTrue(TEXT("and comes back as it was"), KrtuworksConvert::ToFString(Utf8) == Text);
	const std::string Bytes("\x00\x01\x7F\x80\xFF", 5);
	TestTrue(TEXT("bytes cross unchanged, a zero byte included"), KrtuworksConvert::ToBytes(KrtuworksConvert::ToArray(Bytes)) == Bytes);
	TestTrue(TEXT("the config is read next to the .uproject"), KrtuworksConvert::ConfigFilePath().EndsWith(TEXT("/krtuworks-config.json")));
	return true;
}

#if PLATFORM_LINUX || PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKrtuworksSecretStoreTest, "Krtuworks.Platform.SecretStore", KrtuworksTestFlags)

bool FKrtuworksSecretStoreTest::RunTest(const FString& Parameters)
{
	// The platform's sealed store in a folder of its own: what one launch writes the next one reads; another
	// app's key does not open it (Linux: the machine-bound key; Windows: DPAPI).
	const FString Folder = FPaths::Combine(FPaths::AutomationTransientDir(), KrtuworksConvert::ToFString("krtuworks-secrets-" + krtu::toHex(krtu::randomBytes(6))));
	const std::string Root = KrtuworksConvert::ToStd(IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*Folder));
	{
		const auto First = KrtuworksPlatform::MakeSecretStore(Root, "app_automation_one");
		First->set("krtu:test:tokens", "{\"refreshToken\":\"r1\"}");
		TestTrue(TEXT("a secret reads back"), First->get("krtu:test:tokens") == std::optional<std::string>("{\"refreshToken\":\"r1\"}"));
	}
	const auto Next = KrtuworksPlatform::MakeSecretStore(Root, "app_automation_one");
	TestTrue(TEXT("the next launch reads it"), Next->get("krtu:test:tokens") == std::optional<std::string>("{\"refreshToken\":\"r1\"}"));
	const auto Other = KrtuworksPlatform::MakeSecretStore(Root, "app_automation_two");
	TestFalse(TEXT("another app's key does not open it"), Other->get("krtu:test:tokens").has_value());
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKrtuworksHttpGameThreadTest, "Krtuworks.Http.RefusedOnTheGameThread", KrtuworksTestFlags)

bool FKrtuworksHttpGameThreadTest::RunTest(const FString& Parameters)
{
	// The engine finishes requests on the game thread, so waiting there would never end: the transport refuses.
	bool bRefused = false;
	try
	{
		FUnrealHttpTransport().send(LoopbackCall(9, "/"));
	}
	catch (const std::logic_error&)
	{
		bRefused = true;
	}
	TestTrue(TEXT("a call from the game thread is refused, not waited on"), bRefused);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKrtuworksHttpLoopbackTest, "Krtuworks.Http.LoopbackThroughTheEngine", KrtuworksTestFlags)

bool FKrtuworksHttpLoopbackTest::RunTest(const FString& Parameters)
{
	// An SDK thread asks through the engine's HTTP module while the game thread keeps ticking: a stray path gets
	// the listener's 404 and it keeps waiting, the sign-in's return gets the page and hands the address over; a
	// port nobody listens on is a network_error.
	struct FState
	{
		std::unique_ptr<krtu::detail::LoopbackReceiver> Receiver;
		krtu::CancelToken Cancel;
		int Port = 0;
		int ClosedPort = 0;
		std::future<std::string> Received;
		std::future<std::pair<krtu::HttpAnswer, krtu::HttpAnswer>> Answers;
		std::future<std::string> Closed;
		std::chrono::steady_clock::time_point Deadline;
	};
	const auto State = std::make_shared<FState>();
	State->Receiver = std::make_unique<krtu::detail::LoopbackReceiver>();
	State->Port = State->Receiver->port();
	{
		const krtu::detail::LoopbackReceiver Gone;
		State->ClosedPort = Gone.port();
	}
	State->Received = std::async(std::launch::async, [State] { return State->Receiver->wait(State->Cancel); });
	State->Answers = std::async(std::launch::async, [State] {
		FUnrealHttpTransport Transport;
		auto Stray = Transport.send(LoopbackCall(State->Port, "/favicon.ico"));
		auto Return = Transport.send(LoopbackCall(State->Port, "/callback?code=abc&state=xyz"));
		return std::make_pair(Stray, Return);
	});
	State->Closed = std::async(std::launch::async, [State] {
		try
		{
			FUnrealHttpTransport().send(LoopbackCall(State->ClosedPort, "/callback"));
			return std::string("answered");
		}
		catch (const krtu::Error& Failure)
		{
			return Failure.code();
		}
	});
	State->Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() {
		if (!IsReady(State->Received) || !IsReady(State->Answers) || !IsReady(State->Closed))
		{
			if (std::chrono::steady_clock::now() < State->Deadline)
			{
				return false;
			}
			State->Cancel.cancel();
			AddError(TEXT("The loopback round trip did not finish within 60 s"));
			return true;
		}
		try
		{
			const auto Answers = State->Answers.get();
			const std::string Received = State->Received.get();
			TestTrue(TEXT("a stray path gets the listener's 404"), Answers.first.status == 404);
			TestTrue(TEXT("the sign-in's return gets the page"), Answers.second.status == 200 && Answers.second.body.find("You are signed in") != std::string::npos);
			TestTrue(TEXT("the page comes as UTF-8 HTML"), Answers.second.header("content-type").value_or("").find("text/html") != std::string::npos);
			TestTrue(TEXT("the listener hands over the address the browser came back to"),
				Received == "http://127.0.0.1:" + std::to_string(State->Port) + "/callback?code=abc&state=xyz");
			TestTrue(TEXT("a port nobody listens on is a network_error"), State->Closed.get() == "network_error");
		}
		catch (const std::exception& Failure)
		{
			AddError(KrtuworksConvert::ToFString(std::string("The loopback round trip failed: ") + Failure.what()));
		}
		return true;
	}));
	return true;
}

#endif
