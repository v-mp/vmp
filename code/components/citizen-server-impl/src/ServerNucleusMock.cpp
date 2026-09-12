#include <StdInc.h>

#include <CoreConsole.h>

#include <GameServer.h>
#include <HttpClient.h>

#include <ResourceEventComponent.h>
#include <ResourceManager.h>

#include <ServerInstanceBase.h>
#include <ServerLicensingComponent.h>

#include <StructuredTrace.h>

#include <TcpListenManager.h>
#include <NoticeLogicProcessor.h>

#include <chrono>

#define VMP_ALLOW_CFX_NUCLEUS 0

#if VMP_ALLOW_CFX_NUCLEUS

// 100KiB cap, conditional notices should fit more than comfortably under this limit
#define MAX_NOTICE_FILESIZE 102400

static void DownloadAndProcessNotices(fx::ServerInstanceBase* server, HttpClient* httpClient)
{
	HttpRequestOptions options;
	options.maxFilesize = MAX_NOTICE_FILESIZE;
	httpClient->DoGetRequest("https://cdn.vmp.ir/promotions_targeting.json", options, [server, httpClient](bool success, const char* data, size_t length)
	{
		// Double checking received size because CURL will let bigger files through if the server doesn't specify Content-Length outright
		if (success && length <= MAX_NOTICE_FILESIZE)
		{
			try
			{
				auto noticesBlob = nlohmann::json::parse(data, data + length);
				fx::NoticeLogicProcessor::BeginProcessingNotices(server, noticesBlob);
			}
			catch (std::exception& e)
			{
				trace("Notice error: %s\n", e.what());
			}
		}
	});
}

static InitFunction initFunction([]()
{
	static auto httpClient = new HttpClient();

	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		using namespace std::chrono_literals;

		static bool registerSent = false;
		static bool registerSuccess = false;
		static std::chrono::milliseconds registerTimeout{};
		static auto registerDelay = 15s;

		instance->GetComponent<fx::GameServer>()->OnTick.Connect([instance]()
		{
			if (registerSuccess)
			{
				return;
			}

			if (registerSent && msec() < registerTimeout)
			{
				return;
			}

			auto licenseTokenVar = instance->GetComponent<console::Context>()->GetVariableManager()->FindEntryRaw("sv_licenseKeyToken");

			if (!licenseTokenVar || licenseTokenVar->GetValue().empty())
			{
				return;
			}

			auto licensingComponent = instance->GetComponent<ServerLicensingComponent>();
			auto nucleusToken = licensingComponent->GetNucleusToken();

			if (nucleusToken.empty())
			{
				return;
			}

			auto tlm = instance->GetComponent<fx::TcpListenManager>();

			auto reqJson = nlohmann::json::object({
				{ "token", nucleusToken },
				{ "port", fmt::sprintf("%d", tlm->GetPrimaryPort()) },
				{ "ipOverride", instance->GetComponent<fx::GameServer>()->GetIpOverrideVar()->GetValue() },
			});

			registerTimeout = msec() + registerDelay;
			registerSent = true;

			HttpRequestOptions opts;
			opts.ipv4 = true;

			httpClient->DoPostRequest("https://cfx.re/api/register/?v=2", reqJson.dump(), opts, [instance](bool success, const char* data, size_t length)
			{
				auto backoff = [&]()
				{
					if (registerDelay < 15min)
					{
						registerDelay *= 2;
					}
					registerTimeout = msec() + registerDelay;
				};

				if (!success)
				{
					backoff();
					return;
				}

				std::string host;
				try
				{
					auto resp = nlohmann::json::parse(std::string(data, length));
					host = resp.value("host", "");
				}
				catch (const std::exception&)
				{
					backoff();
					return;
				}

				if (host.empty())
				{
					backoff();
					return;
				}

				auto url = fmt::sprintf("https://%s/", host);

				instance->GetComponent<fx::ResourceManager>()
					->GetComponent<fx::ResourceEventManagerComponent>()
					->QueueEvent2(
						"_cfx_internal:nucleusConnected",
						{},
						url
					);

				StructuredTrace({ "type", "nucleus_connected" }, { "url", url });

				static auto webVar = instance->AddVariable<std::string>("web_baseUrl", ConVar_None, host);

				registerSuccess = true;

				DownloadAndProcessNotices(instance, httpClient);
			});
		});
	}, INT32_MAX);
});

#endif // VMP_ALLOW_CFX_NUCLEUS
