/*
* This file is part of FiveM: https://fivem.net/
*
* See LICENSE in the root of the source tree for information
* regarding licensing.
*/

#include "StdInc.h"
#include <ServerIdentityProvider.h>

#include <tbb/concurrent_unordered_map.h>

#include <HttpClient.h>
#include <HttpServer.h>

#include <TcpListenManager.h>

static InitFunction initFunction([]()
{
	static struct EndPointIdProvider : public fx::ServerIdentityProviderBase
	{
		virtual std::string GetIdentifierPrefix() override
		{
			return "ip";
		}

		virtual int GetVarianceLevel() override
		{
			return 5;
		}

		virtual int GetTrustLevel() override
		{
			return 5;
		}

		virtual void RunAuthentication(const fx::ClientSharedPtr& clientPtr, const std::map<std::string, std::string>& postMap, const std::function<void(boost::optional<std::string>)>& cb) override
		{
			const auto& ep = clientPtr->GetTcpEndPoint();
			clientPtr->AddIdentifier("ip:" + ep);

			cb({});
		}

		void RunRealIPAuthentication(const fx::ClientSharedPtr& clientPtr, const fwRefContainer<net::HttpRequest>& request, const std::map<std::string, std::string>& postMap, const std::function<void(boost::optional<std::string>)>& cb, const std::string& realIP)
		{
			const auto& ep = clientPtr->GetTcpEndPoint();
			bool found = fx::IsProxyAddress(ep);

			if (!found)
			{
				clientPtr->AddIdentifier("ip:" + ep);
			}
			else
			{
				clientPtr->AddIdentifier("ip:" + realIP);
				clientPtr->SetTcpEndPoint(realIP);
			}

			cb({});
		}

		virtual void RunAuthentication(const fx::ClientSharedPtr& clientPtr, const fwRefContainer<net::HttpRequest>& request, const std::map<std::string, std::string>& postMap, const std::function<void(boost::optional<std::string>)>& cb) override
		{
			auto realIP = request->GetHeader("X-Real-Ip", "");

			if (!realIP.empty())
			{
				return RunRealIPAuthentication(clientPtr, request, postMap, cb, realIP);
			}

			return RunAuthentication(clientPtr, postMap, cb);
		}
	} idp;

	fx::RegisterServerIdentityProvider(&idp);
}, 150);
