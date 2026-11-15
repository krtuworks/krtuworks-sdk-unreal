#include "UnrealHttpTransport.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "KrtuworksConvert.h"
#include "KrtuworksCore/Error.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace
{
	/** The request's end as the engine reports it, shared with its callback (which may come after a timeout). */
	struct FHttpWait
	{
		std::mutex Mutex;
		std::condition_variable Done;
		bool bFinished = false;
		bool bConnected = false;
		FHttpResponsePtr Response;
	};

	constexpr float RequestTimeoutSeconds = 30.0f;
	/** Longer than the engine's own timeout: it answers first; this only guards a request that never ends. */
	constexpr auto WaitLimit = std::chrono::seconds(45);
}

krtu::HttpAnswer FUnrealHttpTransport::send(const krtu::HttpCall& Call)
{
	if (IsInGameThread())
	{
		throw std::logic_error("the SDK's HTTP calls run off the game thread (the engine finishes requests on it)");
	}
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(KrtuworksConvert::ToFString(Call.url));
	Request->SetVerb(KrtuworksConvert::ToFString(Call.method));
	for (const auto& Header : Call.headers.all())
	{
		Request->SetHeader(KrtuworksConvert::ToFString(Header.first), KrtuworksConvert::ToFString(Header.second));
	}
	if (!Call.body.empty())
	{
		Request->SetContent(KrtuworksConvert::ToArray(Call.body));
	}
	Request->SetTimeout(RequestTimeoutSeconds);
	const std::shared_ptr<FHttpWait> Wait = std::make_shared<FHttpWait>();
	Request->OnProcessRequestComplete().BindLambda([Wait](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected) {
		{
			std::lock_guard<std::mutex> Lock(Wait->Mutex);
			Wait->Response = Response;
			Wait->bConnected = bConnected;
			Wait->bFinished = true;
		}
		Wait->Done.notify_all();
	});
	const std::string Host = Call.url.substr(0, Call.url.find('/', Call.url.find("://") + 3));
	if (!Request->ProcessRequest())
	{
		throw krtu::Error("network_error", "No answer from " + Host + ": the request did not start.");
	}
	std::unique_lock<std::mutex> Lock(Wait->Mutex);
	if (!Wait->Done.wait_for(Lock, WaitLimit, [&Wait] { return Wait->bFinished; }))
	{
		Lock.unlock();
		Request->CancelRequest();
		throw krtu::Error("network_error", "No answer from " + Host + ": timed out.");
	}
	if (!Wait->bConnected || !Wait->Response.IsValid())
	{
		throw krtu::Error("network_error", "No answer from " + Host + ".");
	}
	krtu::HttpAnswer Answer;
	Answer.status = Wait->Response->GetResponseCode();
	for (const FString& Line : Wait->Response->GetAllHeaders())
	{
		FString Name;
		FString Value;
		if (Line.Split(TEXT(":"), &Name, &Value))
		{
			Answer.headers.set(KrtuworksConvert::ToStd(Name.TrimStartAndEnd()), KrtuworksConvert::ToStd(Value.TrimStartAndEnd()));
		}
	}
	Answer.body = KrtuworksConvert::ToBytes(Wait->Response->GetContent());
	return Answer;
}
