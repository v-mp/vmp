#include "StdInc.h"

#include <ServerInstanceBase.h>

#include <GameServer.h>

#include <ResourceManager.h>
#include <ResourceEventComponent.h>

#include "ServerEventComponent.h"

#include <NetBuffer.h>
#include <StructuredTrace.h>

#include <state/ServerGameStatePublic.h>

#include <PrintListener.h>

#include <msgpack.hpp>

#include <UvLoopManager.h>
#include <UvTcpServer.h> // for UvCallback

#include <MonoThreadAttachment.h>

#include <HttpClient.h>
#include <TcpListenManager.h>
#include <ServerLicensingComponent.h>

#include <json.hpp>

#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>

#include <KeyedRateLimiter.h>

#include <prometheus/histogram.h>
#include <ServerPerfComponent.h>

#ifdef _WIN32
#include <ResumeComponent.h>
#endif

#include <InfoHttpHandler.h>

#include "ClientDropReasons.h"

#include <packethandlers/RequestObjectIdsPacketHandler.h>

#include "ByteWriter.h"
#include "FormData.h"
#include "Frame.h"

constexpr const char kDefaultServerList[] = LICENSING_EP "server/heartbeat.php?work=heartbeat";

static fx::GameServer* g_gameServer;

extern fwEvent<> OnEnetReceive;

namespace fx
{
	DLL_EXPORT object_pool<GameServerPacket> m_packetPool;

	GameServer::GameServer()
		: m_residualTime(0), m_serverTime(msec().count()), m_nextHeartbeatTime(0), m_hasSettled(false)
	{
		g_gameServer = this;

		// TODO: re-enable this when we actually figure out threading
		//seCreateContext(&m_seContext);
		m_seContext = seGetCurrentContext();

		m_seContext->MakeCurrent();
		m_seContext->AddAccessControlEntry(se::Principal{ "system.console" }, se::Object{ "command" }, se::AccessType::Allow);
		m_seContext->AddAccessControlEntry(se::Principal{ "builtin.everyone" }, se::Object{ "command.help" }, se::AccessType::Allow);
	}

	GameServer::~GameServer()
	{
		// TODO: fix if ever multi-instancing
		m_thread.detach();
	}

	void GameServer::AttachToObject(ServerInstanceBase* instance)
	{
		m_instance = instance;

		m_gamename = std::make_shared<ConVar<GameName>>("gamename", ConVar_ServerInfo, GameName::GTA5);
		m_lastGameName = m_gamename->GetHelper()->GetValue();

#ifdef _WIN32
		OnAbnormalTermination.Connect([this](void* reason)
		{
			auto realReason = fmt::sprintf("Server shutting down: %s", (const char*)reason);

			m_clientRegistry->ForAllClients([this, realReason](const fx::ClientSharedPtr& client)
			{
				if (client->GetPeer())
				{
					auto oob = fmt::sprintf("error %s", realReason);
					m_net->SendOutOfBand(client->GetAddress(), oob);
				}
			});
		});
#endif

		m_net = fx::CreateGSNet(this);

		if (m_interceptor)
		{
			m_net->AddRawInterceptor(m_interceptor);
		}

		OnAttached(instance);

		m_rconPassword = instance->AddVariable<std::string>("rcon_password", ConVar_ReadOnly, "");
		m_playersToken = instance->AddVariable<std::string>("sv_playersToken", ConVar_None, "");
		m_profileDataToken = instance->AddVariable<std::string>("sv_profileDataToken", ConVar_None, "");
		m_hostname = instance->AddVariable<std::string>("sv_hostname", ConVar_ServerInfo, "default FXServer");
		m_masters[0] = instance->AddVariable<std::string>("sv_master1", ConVar_None, kDefaultServerList);
		m_masters[1] = instance->AddVariable<std::string>("sv_master2", ConVar_None, "");
		m_masters[2] = instance->AddVariable<std::string>("sv_master3", ConVar_None, "");
		m_listingIpOverride = instance->AddVariable<std::string>("sv_listingIpOverride", ConVar_None, "");
		m_useAccurateSendsVar = instance->AddVariable<bool>("sv_useAccurateSends", ConVar_None, true);

		// sv_forceIndirectListing will break listings if the proxy host can not be reached
		m_forceIndirectListing = instance->AddVariable<bool>("sv_forceIndirectListing", ConVar_None, false);

		// sv_listingHostOverride specifies a host to use as reverse proxy endpoint instead of the default
		// reverse TCP server
		m_listingHostOverride = instance->AddVariable<std::string>("sv_listingHostOverride", ConVar_None, "");

		m_heartbeatCommand = instance->AddCommand("heartbeat", [=]()
		{
			ForceHeartbeat();
		});

		m_mainThreadCallbacks = std::make_unique<CallbackListNng>("inproc://main_client", 0);

		instance->OnRequestQuit.Connect([this](const std::string& reason)
		{
			m_clientRegistry->ForAllClients([this, &reason](const fx::ClientSharedPtr& client)
			{
				DropClientWithReason(client, fx::serverDropResourceName, ClientDropReason::SERVER_SHUTDOWN, "Server shutting down: %s", reason);
			});
		});

		instance->OnInitialConfiguration.Connect([=]()
		{
			if (!m_net->SupportsUvUdp())
			{
				m_thread = std::thread([=]()
				{
					SetThreadName(-1, "[Cfx] Server Thread");

					m_mainThreadCallbacks->AttachToThread();

					Run();
				});
			}
			else
			{
				m_mainThreadLoop = Instance<net::UvLoopManager>::Get()->GetOrCreate("svMain");

				static std::shared_ptr<uvw::SignalHandle> sigint = m_mainThreadLoop->Get()->resource<uvw::SignalHandle>();
				sigint->start(SIGINT);

				static std::shared_ptr<uvw::SignalHandle> sighup = m_mainThreadLoop->Get()->resource<uvw::SignalHandle>();
				sighup->start(SIGHUP);

				sigint->on<uvw::SignalEvent>([this](const uvw::SignalEvent& ev, uvw::SignalHandle& sig)
				{
					se::ScopedPrincipal principalScope(se::Principal{ "system.console" });
					m_instance->GetComponent<console::Context>()->ExecuteSingleCommandDirect(ProgramArguments{ "quit", GetVariable("txAdminServerMode").empty() ? "SIGINT received" : "" });
				});

				sighup->on<uvw::SignalEvent>([this](const uvw::SignalEvent& ev, uvw::SignalHandle& sig)
				{
					se::ScopedPrincipal principalScope(se::Principal{ "system.console" });
					m_instance->GetComponent<console::Context>()->ExecuteSingleCommandDirect(ProgramArguments{ "quit", GetVariable("txAdminServerMode").empty() ? "SIGHUP received" : "" });
				});

				auto asyncInitHandle = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();;
				*asyncInitHandle = std::make_unique<UvHandleContainer<uv_async_t>>();

				uv_async_init(m_mainThreadLoop->GetLoop(), (*asyncInitHandle)->get(), UvPersistentCallback((*asyncInitHandle)->get(), [this, asyncInitHandle](uv_async_t*)
				{
					// private data for the net thread
					struct MainPersistentData
					{
						UvHandleContainer<uv_timer_t> tickTimer;

						std::shared_ptr<std::unique_ptr<UvHandleContainer<uv_async_t>>> callbackAsync;

						std::chrono::milliseconds lastTime;
					};

					auto mainData = std::make_shared<MainPersistentData>();
					mainData->lastTime = msec();

					auto loop = m_mainThreadLoop->GetLoop();

					// periodic timer for main ticks
					auto frameTime = 1000 / 20;

					auto mpd = mainData.get();

					static auto& collector = prometheus::BuildHistogram()
						.Name("tickTime")
						.Help("Time spent on server ticks")
						.Register(*m_instance->GetComponent<ServerPerfComponent>()->GetRegistry())
						.Add({ {"name", "svMain"} }, prometheus::Histogram::BucketBoundaries{
							0.001, 0.002, 0.004, 0.006, 0.008, 0.010, 0.015, 0.020, 0.030, 0.050, 0.070, 0.100, 0.150, 0.250
						});

					uv_timer_init(loop, &mainData->tickTimer);
					uv_timer_start(&mainData->tickTimer, UvPersistentCallback(&mainData->tickTimer, [this, mpd](uv_timer_t*)
					{
						auto now = msec();
						auto thisTime = now - mpd->lastTime;
						mpd->lastTime = now;

						if (thisTime > 150ms)
						{
							trace("server thread hitch warning: timer interval of %d milliseconds\n", thisTime.count());
							StructuredTrace({ "type", "hitch" }, { "thread", "svMain" }, { "time", thisTime.count() });
						}

						ProcessServerFrame(thisTime.count());

						auto atEnd = msec();
						collector.Observe((atEnd - now).count() / 1000.0);
					}), frameTime, frameTime);

					// event handle for callback list evaluation
					
					// unique_ptr wrapper is a workaround for libc++ bug (fixed in LLVM r340823)
					mainData->callbackAsync = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();
					*mainData->callbackAsync = std::make_unique<UvHandleContainer<uv_async_t>>();

					uv_async_init(loop, (*mainData->callbackAsync)->get(), UvPersistentCallback((*mainData->callbackAsync)->get(), [this](uv_async_t*)
					{
						m_mainThreadCallbacks->Run();
					}));

					// run remaining callbacks before we remove this callback list
					if (m_mainThreadCallbacks)
					{
						m_mainThreadCallbacks->Run();
					}

					m_mainThreadCallbacks = std::make_unique<CallbackListUv>(mainData->callbackAsync);
					m_mainThreadCallbacks->AttachToThread();

					// store the pointer in the class for lifetime purposes
					m_mainThreadData = std::move(mainData);
				}));

				uv_async_send((*asyncInitHandle)->get());
			}
		}, 100);

		m_clientRegistry = instance->GetComponent<ClientRegistry>().GetRef();

		std::thread([=]()
		{
			while (true)
			{
				for (auto& master : m_masters)
				{
					// if the master is set
					std::string masterName = master->GetValue();

					if (!masterName.empty() && masterName.find("https://") != 0 && masterName.find("http://") != 0)
					{
						// look up if not cached
						auto address = net::PeerAddress::FromString(masterName, 30110, net::PeerAddress::LookupType::ResolveName);

						if (address)
						{
							if (m_masterCache[masterName] != *address)
							{
								trace("Resolved %s to %s\n", masterName, address->ToString());

								m_masterCache[masterName] = *address;
							}
						}
					}
				}

				std::this_thread::sleep_for(60s);
			}
		}).detach();

		if (m_net->SupportsUvUdp())
		{
			InitializeNetUv();
		}
		else
		{
			InitializeNetThread();
		}

		InitializeSyncUv();
	}

	void GameServer::InitializeNetUv()
	{
		m_netThreadLoop = Instance<net::UvLoopManager>::Get()->GetOrCreate("svNetwork");

		auto asyncInitHandle = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();
		*asyncInitHandle = std::make_unique<UvHandleContainer<uv_async_t>>();

		uv_async_init(m_netThreadLoop->GetLoop(), (*asyncInitHandle)->get(), UvPersistentCallback((*asyncInitHandle)->get(), [this, asyncInitHandle](uv_async_t*)
		{
			// private data for the net thread
			struct NetPersistentData
			{
				UvHandleContainer<uv_timer_t> tickTimer;
				UvHandleContainer<uv_timer_t> sendTimer;

				std::shared_ptr<std::unique_ptr<UvHandleContainer<uv_async_t>>> callbackAsync;

				std::chrono::milliseconds lastTime;
			};

			auto netData = std::make_shared<NetPersistentData>();
			auto loop = m_netThreadLoop->GetLoop();

			netData->lastTime = msec();

			// periodic timer for network ticks
			auto frameTime = 1000 / 100;
			auto sendTime = 1000 / 40;

			auto mpd = netData.get();

			static auto& collector = prometheus::BuildHistogram()
				.Name("tickTime")
				.Help("Time spent on server ticks")
				.Register(*m_instance->GetComponent<ServerPerfComponent>()->GetRegistry())
				.Add({ {"name", "svNetwork"} }, prometheus::Histogram::BucketBoundaries{
					0.001, 0.002, 0.004, 0.006, 0.008, 0.010, 0.015, 0.020, 0.030, 0.050, 0.070, 0.100, 0.150, 0.250
				});

			auto processSendList = [this]()
			{
				while (auto *packet = m_netSendList.pop_until_empty(&fx::GameServerPacket::queueKey))
				{
					m_net->SendPacket(packet->peer, packet->channel, packet->buffer, packet->type);
					m_packetPool.destruct(packet);
				}
			};
			
			auto tcb = UvPersistentCallback(&netData->tickTimer, [this, mpd, processSendList](uv_timer_t*)
			{
				auto now = msec();
				auto thisTime = now - mpd->lastTime;
				mpd->lastTime = now;

				if (thisTime > 150ms)
				{
					trace("network thread hitch warning: timer interval of %d milliseconds\n", thisTime.count());
					StructuredTrace({ "type", "hitch" }, { "thread", "svNetwork" }, { "time", thisTime.count() });
				}

				OnNetworkTick();

				if (m_useAccurateSendsVar->GetValue())
				{
					// process send list in between
					processSendList();
				}

				// process game push
				m_net->Process();

				auto atEnd = msec();
				collector.Observe((atEnd - now).count() / 1000.0);
			});

			uv_timer_init(loop, &netData->tickTimer);
			uv_timer_start(&netData->tickTimer, tcb, frameTime, frameTime);

			uv_timer_init(loop, &netData->sendTimer);
			uv_timer_start(&netData->sendTimer, UvPersistentCallback(&netData->sendTimer, [this, processSendList](uv_timer_t*)
			{
				if (!m_useAccurateSendsVar->GetValue())
				{
					processSendList();
				}
			}), sendTime, sendTime);

			// event handle for callback list evaluation

			// unique_ptr wrapper is a workaround for libc++ bug (fixed in LLVM r340823)
			netData->callbackAsync = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();
			*netData->callbackAsync = std::make_unique<UvHandleContainer<uv_async_t>>();

			uv_async_init(loop, (*netData->callbackAsync)->get(), UvPersistentCallback((*netData->callbackAsync)->get(), [this](uv_async_t*)
			{
				m_netThreadCallbacks->Run();
			}));

			m_netThreadCallbacks = std::make_unique<CallbackListUv>(netData->callbackAsync);
			m_netThreadCallbacks->AttachToThread();

			// process hosts on a command
			// #TODO1SBIG: add bigmode check/convar?
			/*OnEnetReceive.Connect([this]()
			{
				m_net->Process();
			});*/

			// store the pointer in the class for lifetime purposes
			m_netThreadData = std::move(netData);
		}));

		uv_async_send((*asyncInitHandle)->get());
	}

	// #TODO: this is a weird copy of the same function above
	void GameServer::InitializeSyncUv()
	{
		m_syncThreadLoop = Instance<net::UvLoopManager>::Get()->GetOrCreate("svSync");

		// #TODO: make these actually send to loop properly using the async set *in* the loop wrapper?
		auto asyncInitHandle = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();
		*asyncInitHandle = std::make_unique<UvHandleContainer<uv_async_t>>();

		uv_async_init(m_syncThreadLoop->GetLoop(), (*asyncInitHandle)->get(), UvPersistentCallback((*asyncInitHandle)->get(), [this, asyncInitHandle](uv_async_t*)
		{
			// private data for the net thread
			struct NetPersistentData
			{
				UvHandleContainer<uv_timer_t> tickTimer;

				std::shared_ptr<std::unique_ptr<UvHandleContainer<uv_async_t>>> callbackAsync;

				std::chrono::milliseconds lastTime;
			};

			auto netData = std::make_shared<NetPersistentData>();
			auto loop = m_syncThreadLoop->GetLoop();

			netData->lastTime = msec();

			// periodic timer for sync ticks
			auto frameTime = 1000 / 120;

			auto mpd = netData.get();

			static auto& collector = prometheus::BuildHistogram()
				.Name("tickTime")
				.Help("Time spent on server ticks")
				.Register(*m_instance->GetComponent<ServerPerfComponent>()->GetRegistry())
				.Add({ {"name", "svSync"} }, prometheus::Histogram::BucketBoundaries{
					0.001, 0.002, 0.004, 0.006, 0.008, 0.010, 0.015, 0.020, 0.030, 0.050, 0.070, 0.100, 0.150, 0.250
				});

			uv_timer_init(loop, &netData->tickTimer);
			uv_timer_start(&netData->tickTimer, UvPersistentCallback(&netData->tickTimer, [this, mpd](uv_timer_t*)
			{
				auto now = msec();
				auto thisTime = now - mpd->lastTime;
				mpd->lastTime = now;

				if (thisTime > 100ms)
				{
					trace("sync thread hitch warning: timer interval of %d milliseconds\n", thisTime.count());
					StructuredTrace({ "type", "hitch" }, { "thread", "svSync" }, { "time", thisTime.count() });
				}

				OnSyncTick();

				auto atEnd = msec();
				collector.Observe((atEnd - now).count() / 1000.0);
			}), frameTime + (frameTime / 2), frameTime);

			// event handle for callback list evaluation

			// unique_ptr wrapper is a workaround for libc++ bug (fixed in LLVM r340823)
			netData->callbackAsync = std::make_shared<std::unique_ptr<UvHandleContainer<uv_async_t>>>();
			*netData->callbackAsync = std::make_unique<UvHandleContainer<uv_async_t>>();

			uv_async_init(loop, (*netData->callbackAsync)->get(), UvPersistentCallback((*netData->callbackAsync)->get(), [this](uv_async_t*)
			{
				m_syncThreadCallbacks->Run();
			}));

			m_syncThreadCallbacks = std::make_unique<CallbackListUv>(netData->callbackAsync);
			m_syncThreadCallbacks->AttachToThread();

			// store the pointer in the class for lifetime purposes
			m_syncThreadData = std::move(netData);
		}));

		uv_async_send((*asyncInitHandle)->get());
	}

	void GameServer::InitializeNetThread()
	{
		m_netThreadCallbacks = std::make_unique<CallbackListNng>("inproc://netlib_client", 1);

		std::thread([this]()
		{
			SetThreadName(-1, "[Cfx] Network Thread");

			m_netThreadCallbacks->AttachToThread();

			nng_socket netSocket;
			nng_rep0_open(&netSocket);

			nng_listener listener;
			nng_listen(netSocket, "inproc://netlib_client", &listener, NNG_FLAG_NONBLOCK);

			auto lastTime = msec().count();

			uint64_t residualTime = 0;
			auto frameTime = 1000 / 30;

			while (true)
			{
				// service enet with our remaining waits
				int rcvFd;
				nng_getopt_int(netSocket, NNG_OPT_RECVFD, &rcvFd);

				m_net->Select({ uintptr_t(rcvFd) }, frameTime);

				{
					m_seContext->MakeCurrent();
					m_net->Process();
				}

				{
					auto now = msec().count() - lastTime;

					if (now >= 150)
					{
						trace("hitch warning: net frame time of %d milliseconds\n", now);
					}

					// clamp time to 200ms to reduce effects of excessive hitches
					if (now > 200)
					{
						now = 200;
					}

					residualTime += now;

					lastTime = msec().count();

					// intervals
					if (residualTime > frameTime)
					{
						OnNetworkTick();

						residualTime = 0;
					}
				}

				{
					void* msgBuffer;
					size_t msgLen;

					while (nng_recv(netSocket, &msgBuffer, &msgLen, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC) == 0)
					{
						nng_free(msgBuffer, msgLen);

						int ok = 0;
						nng_send(netSocket, &ok, 4, NNG_FLAG_NONBLOCK);

						m_netThreadCallbacks->Run();
					}
				}
			}
		}).detach();
	}

	void GameServer::InternalRunMainThreadCbs(nng_socket socket)
	{
		void* msgBuffer;
		size_t msgLen;

		while (nng_recv(socket, &msgBuffer, &msgLen, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC) == 0)
		{
			nng_free(msgBuffer, msgLen);

			int ok = 0;
			nng_send(socket, &ok, 4, NNG_FLAG_NONBLOCK);

			m_mainThreadCallbacks->Run();
		}
	}

	void GameServer::InternalGetPeer(int peerId, fx::NetPeerStackBuffer& stackBuffer)
	{
		m_net->GetPeer(peerId, stackBuffer);
	}

	void GameServer::InternalResetPeer(int peerId)
	{
		m_net->ResetPeer(peerId);
	}

	void GameServer::InternalSendPacket(fx::Client* client, int peer, int channel, const net::Buffer& buffer, NetPacketType type)
	{
		GameServerPacket* packet = m_packetPool.construct(peer, channel, buffer, type);
		m_netSendList.push(&packet->queueKey);
	}

	void GameServer::Run()
	{
		if (m_runLoop)
		{
			m_runLoop();
		}
	}

	void GameServer::AddRawInterceptor(const std::function<bool(const uint8_t *, size_t, const net::PeerAddress &)>& interceptor)
	{
		m_interceptor = interceptor;
	}

	void GameServer::CreateUdpHost(const net::PeerAddress& address)
	{
		m_net->CreateUdpHost(address);
	}

	std::string GameServer::GetVariable(const std::string& key)
	{
		auto consoleCtx = m_instance->GetComponent<console::Context>();
		auto variable = consoleCtx->GetVariableManager()->FindEntryRaw(key);

		if (!variable)
		{
			return "";
		}

		return variable->GetValue();
	}

	void GameServer::ProcessPacket(NetPeerBase* peer, ENetPacketPtr& packet)
	{
		// create a byte reader and read the message type
		net::ByteReader msg(packet->data, packet->dataLength);
		uint32_t msgType;
		if (!msg.Field(msgType))
		{
			return;
		}

		// get the client
		auto peerId = peer->GetId();

		auto client = m_clientRegistry->GetClientByPeer(peerId);

		// handle connection handshake message
		if (msgType == 1)
		{
			if (!client)
			{
				std::string_view dataSpan;
				if (!msg.Field(dataSpan, msg.GetRemaining()))
				{
					return;
				}

				auto postMap = net::DecodeFormData(dataSpan);
				auto guid = postMap["guid"];
				auto token = postMap["token"];

				client = m_clientRegistry->GetClientByGuid(guid);

				if (client)
				{
					if (token != client->GetConnectionToken())
					{
						SendOutOfBand(peer->GetAddress(), "error Invalid connection token received.");

						m_clientRegistry->RemoveClient(client);

						return;
					}

					if (!client->GetData("passedValidation"))
					{
						SendOutOfBand(peer->GetAddress(), "error Invalid connection.");

						m_clientRegistry->RemoveClient(client);

						return;
					}

					client->Touch();

					client->SetPeer(peerId, peer->GetAddress());

					if (IsOneSync())
					{
						if (!client->HasSlotId())
						{
							SendOutOfBand(peer->GetAddress(), "error Not enough client slot IDs.");

							m_clientRegistry->RemoveClient(client);

							return;
						}
					}

					bool wasNew = false;
					auto oldNetID = client->GetNetId();

					if (!client->HasConnected())
					{
						m_clientRegistry->HandleConnectingClient(client);

						wasNew = true;
					}

					peer->OnSendConnectOK();

					// send a connectOK
					net::Buffer outMsg;
					outMsg.Write(1);

					auto host = m_clientRegistry->GetHost();

					uint32_t bigModeSlot = (m_instance->GetComponent<fx::GameServer>()->GetGameName() == fx::GameName::GTA5) ? 128 : 16;

					auto outStr = fmt::sprintf(
						" %d %d %d %d %lld",
						client->GetNetId(),
						(host) ? host->GetNetId() : -1,
						(host) ? host->GetNetBase() : -1,
						(IsOneSync())
							? ((fx::IsBigMode())
								? bigModeSlot
								: client->GetSlotId())
							: -1,
						(IsOneSync()) ? msec().count() : -1);

					outMsg.Write(outStr.c_str(), outStr.size());

					client->SendPacket(0, outMsg, NetPacketType_Reliable);

					if (wasNew)
					{
						gscomms_execute_callback_on_main_thread([this, client, oldNetID]()
						{
							m_clientRegistry->HandleConnectedClient(client, oldNetID);
						});

						if (IsOneSync())
						{
							RequestObjectIdsPacketHandler::SendObjectIds(m_instance, client, fx::IsBigMode() ? 4 : 64);
						}

						ForceHeartbeatSoon();
					}
				}
			}

			return;
		}

		// if not type 1, and no client, bail out
		if (!client)
		{
			return;
		}

		auto principalScope = client->EnterPrincipalScope();

		if (client->GetNetworkMetricsRecvCallback())
		{
			client->GetNetworkMetricsRecvCallback()(client.get(), msgType, msg);
		}

		if (m_packetHandler)
		{
			m_packetHandler(msgType, client, msg, packet);
		}

		client->Touch();
	}

	void GameServer::Broadcast(const net::Buffer& buffer)
	{
		m_clientRegistry->ForAllClients([&](const fx::ClientSharedPtr& client)
		{
			client->SendPacket(0, buffer, NetPacketType_Reliable);
		});
	}

	void GameServer::DeferCall(int inMsec, const std::function<void()>& fn)
	{
		static std::atomic<int> cbIdx;

		// find an empty slot first
		for (auto& pair : m_deferCallbacks)
		{
			if (std::get<int>(pair.second) == 0)
			{
				pair.second = { m_serverTime + inMsec, fn };
				return;
			}
		}

		m_deferCallbacks.insert({ cbIdx.fetch_add(1), { m_serverTime + inMsec, fn } });
	}

	void GameServer::CallbackListBase::Add(const std::function<void()>& fn, bool force)
	{
		if (threadId == std::this_thread::get_id() && !force)
		{
			fn();
			return;
		}

		// add to the queue
		callbacks.enqueue(fn);

		// signal the waiting thread
		SignalThread();
	}

	void GameServer::CallbackListBase::Run()
	{
		// TODO: might need memory barriers for single-consumer too?
		std::function<void()> fn;

		while (callbacks.try_dequeue(fn))
		{
			fn();
		}
	}

	void GameServer::CallbackListNng::SignalThread()
	{
		// submit to the owning thread
		static thread_local nng_socket sockets[2];
		static thread_local nng_dialer dialers[2];

		int i = m_socketIdx;

		if (!sockets[i].id)
		{
			nng_req0_open(&sockets[i]);
			nng_dial(sockets[i], m_socketName.c_str(), &dialers[i], 0);
		}

		std::vector<int> idxList(1);
		idxList[0] = 0xFEED;

		nng_send(sockets[i], &idxList[0], idxList.size() * sizeof(int), NNG_FLAG_NONBLOCK);
	}

	void GameServer::CallbackListUv::SignalThread()
	{
		auto async = m_async.lock();

		if (async)
		{
			uv_async_send((*async)->get());
		}
	}

	void GameServer::ProcessServerFrame(int frameTime)
	{
		MonoEnsureThreadAttached();
		m_seContext->MakeCurrent();

		m_serverTime += frameTime;
		m_hasSettled = true;

		// check callbacks
		for (auto& cb : m_deferCallbacks)
		{
			if (std::get<int>(cb.second) == 0)
			{
				continue;
			}

			if (m_serverTime >= std::get<int>(cb.second))
			{
				std::get<1>(cb.second)();

				cb.second = {};
			}
		}

		{
			std::vector<fx::ClientSharedPtr> toRemove;

			uint8_t syncStyle = (uint8_t)m_instance->GetComponent<fx::ServerGameStatePublic>()->GetSyncStyle();

			m_clientRegistry->ForAllClients([&](fx::ClientSharedPtr client)
			{
				auto peer = client->GetPeer();

				if (peer)
				{
					auto lm = client->GetData("lockdownMode");
					auto lf = client->GetData("lastFrame");
					auto ss = client->GetData("syncStyle");

					bool lockdownMode = m_instance->GetComponent<fx::ServerGameStatePublic>()->GetEntityLockdownMode(client) == fx::EntityLockdownMode::Strict;

					if (!lm || fx::AnyCast<bool>(lm) != lockdownMode ||
						!ss || fx::AnyCast<uint8_t>(ss) != syncStyle ||
						!lf ||
						(m_serverTime - fx::AnyCast<uint64_t>(lf)) > 1000)
					{
						net::packet::ServerFramePacket serverFrame;
						serverFrame.data.lockdownMode = lockdownMode;
						serverFrame.data.syncStyle = syncStyle;
						static const size_t kMaxFrameSize = net::SerializableComponent::GetMaxSize<net::packet::ServerFramePacket>();
						net::Buffer responseBuffer(kMaxFrameSize);
						net::ByteWriter writer(responseBuffer.GetBuffer(), kMaxFrameSize);
						serverFrame.Process(writer);
						responseBuffer.Seek(writer.GetOffset());
						client->SendPacket(0, responseBuffer, NetPacketType_Reliable);

						client->SetData("lockdownMode", lockdownMode);
						client->SetData("syncStyle", syncStyle);
						client->SetData("lastFrame", m_serverTime);
					}
				}

				// time out the client if needed
				if (client->IsDead())
				{
					toRemove.push_back(client);
				}
			});

			for (auto& client : toRemove)
			{
				auto lastSeen = (msec() - client->GetLastSeen());

				// if this happened fairly early, try to find out why
				if (lastSeen < 1500ms)
				{
					auto timeoutInfo = m_net->GatherTimeoutInfo(client->GetPeer());

					if (!timeoutInfo.bigCommandList.empty())
					{
						std::stringstream commandListFormat;
						for (const auto& bigCmd : timeoutInfo.bigCommandList)
						{
							std::string name = (bigCmd.eventName.empty()) ? fmt::sprintf("%08x", bigCmd.type) : bigCmd.eventName;

							commandListFormat << fmt::sprintf("%s (%d B, %d msec ago)\n", name, bigCmd.size, bigCmd.timeAgo);
						}

						DropClientWithReason(client, fx::serverDropResourceName, ClientDropReason::CLIENT_CONNECTION_TIMED_OUT_WITH_PENDING_COMMANDS, "Server->client connection timed out. Pending commands: %d.\nCommand list:\n%s", timeoutInfo.pendingCommands, commandListFormat.str());
						continue;
					}
				}

				DropClientWithReason(client, fx::serverDropResourceName, ClientDropReason::CLIENT_CONNECTION_TIMED_OUT, "Server->client connection timed out. Last seen %d msec ago.", lastSeen.count());
			}
		}

		// if we should heartbeat
		if (msec() >= m_nextHeartbeatTime)
		{
			auto sendHttpHeartbeat = [this](std::string_view masterName, bool isPrivate)
			{
				auto var = m_instance->GetComponent<console::Context>()->GetVariableManager()->FindEntryRaw("sv_sessionId");
				auto var2 = m_instance->GetComponent<console::Context>()->GetVariableManager()->FindEntryRaw("sv_secret");
				if (var && !var->GetValue().empty() && var2 && !var2->GetValue().empty())
				{
					console::DPrintf("citizen-server-impl", "Sending heartbeat to %s\n", masterName);

					nlohmann::json playersJson, infoJson, dynamicJson;
					auto ihh = m_instance->GetComponent<InfoHttpHandlerComponent>();
					ihh->GetJsonData(&infoJson, &dynamicJson, &playersJson);

					auto json = nlohmann::json::object({
						{ "secret", var2->GetValue() },
						{ "session_id", var->GetValue() },
						{ "port", m_instance->GetComponent<fx::TcpListenManager>()->GetPrimaryPort() },
						{ "ipOverride", m_listingIpOverride->GetValue() },
						{ "forceIndirectListing", m_forceIndirectListing->GetValue() },
						{ "private", isPrivate },
						{ "fallbackData", nlohmann::json::object({
							{ "players", playersJson },
							{ "info", infoJson },
							{ "dynamic", dynamicJson },
						})}
					});

					if (!m_listingHostOverride->GetValue().empty())
					{
						json["hostOverride"] = m_listingHostOverride->GetValue();
					}

					HttpRequestOptions ro;
					ro.ipv4 = true;
					ro.headers = std::map<std::string, std::string>{
						{ "Content-Type", "application/json; charset=utf-8" }
					};
					ro.addErrorBody = true;

					static bool firstRun = true;

					Instance<HttpClient>::Get()->DoPostRequest(std::string{ masterName }, json.dump(), ro, [](bool success, const char* d, size_t s)
					{
						if (!success)
						{
							console::DPrintf("citizen-server-impl", "error submitting to ingress: %s\n", std::string{ d, s });
							return;
						}

						try
						{
							// don't return a query error for the first run (may be residual)
							if (!firstRun)
							{
								auto j = nlohmann::json::parse(d, d + s);
								if (j.is_object() && j.contains("lastError"))
								{
									auto lastError = j["lastError"].get<std::string>();
									console::Printf("citizen-server-impl", "^1Server list query returned an error: %s^7\n", lastError.substr(0, lastError.find_first_of("\r\n")));
								}
							}
						}
						catch (...)
						{
						}

						firstRun = false;
					});
				}
			};

			bool isPrivate = true;

			// loop through each master
			for (auto& master : m_masters)
			{
				// if the master is set
				std::string masterName = master->GetValue();

				if (!masterName.empty())
				{
					if (masterName.find("https://") != 0 && masterName.find("http://") != 0)
					{
						// find a cached address
						auto it = m_masterCache.find(masterName);

						if (it != m_masterCache.end())
						{
							// send a heartbeat to the master
							SendOutOfBand(it->second, "heartbeat DarkPlaces\n");

							console::DPrintf("citizen:server:impl", "Sending (old style) heartbeat to %s\n", masterName);
						}
					}
					else if (masterName != kDefaultServerList)
					{
						sendHttpHeartbeat(masterName, isPrivate);
					}
					else
					{
						isPrivate = false;
					}
				}
			}

			sendHttpHeartbeat(kDefaultServerList, isPrivate);
			m_nextHeartbeatTime = msec() + 3min;
		}

		{
			auto ctx = GetInstance()->GetComponent<console::Context>();

			se::ScopedPrincipal principalScope(se::Principal{ "system.console" });
			ctx->ExecuteBuffer();
		}

		if (m_gamename->GetHelper()->GetValue() != m_lastGameName)
		{
			if (!m_lastGameName.empty())
			{
				console::PrintError("server", "Reverted a `gamename` change. You can't change gamename while the server is running!\n");
				m_gamename->GetHelper()->SetValue(m_lastGameName);
			}

			m_lastGameName = m_gamename->GetHelper()->GetValue();
		}

		OnTick();
	}

	void GameServer::DropClientv(const fx::ClientSharedPtr& client, const std::string& resourceName, ClientDropReason clientDropReason, const std::string& reason)
	{
		std::string realReason = reason;
		if (realReason.empty())
		{
			realReason = "Dropped.";
		}

		if (client->IsDropping())
		{
			return;
		}

		client->SetDropping();

		gscomms_execute_callback_on_main_thread([this, client, resourceName = std::move(resourceName), realReason = std::move(realReason), clientDropReason]()
		{
			DropClientInternal(client, resourceName, clientDropReason, realReason);
		});
	}

	void GameServer::DropClientInternal(const fx::ClientSharedPtr& client, const std::string& resourceName, ClientDropReason clientDropReason, const std::string& realReason)
	{
		// send an out-of-band error to the client
		if (client->GetPeer())
		{
			SendOutOfBand(client->GetAddress(), fmt::sprintf("error %s", realReason));
		}

		// force a hearbeat
		ForceHeartbeatSoon();

		// ensure mono thread attachment (if this was a worker thread)
		MonoEnsureThreadAttached();

		// verify if the client is still using a TempID
		bool isFinal = client->HasConnected();

		// trigger a event signaling the player's drop, if final
		if (isFinal)
		{
			m_instance
				->GetComponent<fx::ResourceManager>()
				->GetComponent<fx::ResourceEventManagerComponent>()
				->TriggerEvent2(
					"playerDropped",
					{ fmt::sprintf("internal-net:%d", client->GetNetId()) },
					realReason,
					resourceName,
					static_cast<uint32_t>(clientDropReason)
				);
		}

		// remove the host if this was the host
		if (m_clientRegistry->GetHost() == client)
		{
			m_clientRegistry->SetHost({});

			// broadcast the current host
			net::Buffer hostBroadcast;
			hostBroadcast.Write(0xB3EA30DE);
			hostBroadcast.Write<uint16_t>(0xFFFF);
			hostBroadcast.Write(0xFFFF);

			Broadcast(hostBroadcast);
		}

		// signal a drop
		client->OnDrop();

		{
			// for name handling, send player state
			fwRefContainer<ServerEventComponent> events = m_instance->GetComponent<ServerEventComponent>();

			if (!fx::IsBigMode() && isFinal)
			{
				// send every player information about the dropping client
				events->TriggerClientEvent("onPlayerDropped", std::optional<std::string_view>(), client->GetNetId(), client->GetName(), client->GetSlotId());
			}
		}

		// drop the client
		m_clientRegistry->RemoveClient(client);
	}

	void GameServer::ForceHeartbeat()
	{
		m_nextHeartbeatTime = 0ms;
	}

	void GameServer::ForceHeartbeatSoon()
	{
		auto now = msec();
		auto soon = (now + 15s);

		// if the next heartbeat is not sooner-than-soon...
		if (m_nextHeartbeatTime > soon)
		{
			// set it to execute soon
			m_nextHeartbeatTime = soon;
		}
	}

	void GameServer::SendOutOfBand(const net::PeerAddress& to, const std::string_view& oob, bool prefix)
	{
		std::string oobStr(oob);

		gscomms_execute_callback_on_net_thread([this, to, oobStr, prefix]()
		{
			m_net->SendOutOfBand(to, oobStr, prefix);
		});
	}

	fwRefContainer<GameServerNetBase> CreateGSNet(fx::GameServer* server)
	{
		static auto cmd = server->GetInstance()->AddVariable<std::string>("netlib", ConVar_None, "enet");

		// ignored: cmd value (we only support enet at this time)
		return CreateGSNet_ENet(server);
	}

	FxPrintListener printListener;

	namespace ServerDecorators
	{
		fwRefContainer<fx::GameServer> NewGameServer()
		{
			return new fx::GameServer();
		}

		struct ThreadWait
		{
			ThreadWait()
			{
				nng_rep0_open(&m_socket);
				nng_listen(m_socket, "inproc://main_client", &m_listener, 0);
			}

			inline void operator()(const fwRefContainer<fx::GameServer>& server, int maxTime)
			{
				int rcvFd;
				nng_getopt_int(m_socket, NNG_OPT_RECVFD, &rcvFd);

				fd_set fds;
				FD_ZERO(&fds);
				FD_SET(rcvFd, &fds);

				timeval timeout;
				timeout.tv_sec = 0;
				timeout.tv_usec = maxTime * 1000;
				int rv = select(rcvFd + 1, &fds, nullptr, nullptr, &timeout);

				if (rv >= 0 && FD_ISSET(rcvFd, &fds))
				{
					server->InternalRunMainThreadCbs(m_socket);
				}
			}

			nng_socket m_socket;
			nng_listener m_listener;
		};

		struct GameServerTick
		{
			inline void operator()(const fwRefContainer<fx::GameServer>& server, int frameTime)
			{
				server->ProcessServerFrame(frameTime);
			}
		};
	}
}

#include <decorators/WithEndpoints.h>
#include <decorators/WithOutOfBand.h>
#include <decorators/WithProcessTick.h>
#include <decorators/WithPacketHandler.h>

#include <outofbandhandlers/GetInfoOutOfBand.h>
#include <outofbandhandlers/GetStatusOutOfBand.h>
#include <outofbandhandlers/RconOutOfBand.h>

#include <packethandlers/RoutingPacketHandler.h>
#include <packethandlers/HeHostPacketHandler.h>
#include <packethandlers/IHostPacketHandler.h>
#include <packethandlers/IQuitPacketHandler.h>
#include <packethandlers/ServerEventPacketHandler.h>
#include <packethandlers/ServerCommandPacketHandler.h>
#include <packethandlers/TimeSyncReqPacketHandler.h>
#include <packethandlers/StateBagPacketHandler.h>
#include <packethandlers/NetGameEventPacketHandler.h>
#include <packethandlers/ReassembledEventPacketHandler.h>
#include <packethandlers/ArrayUpdatePacketHandler.h>
#include <packethandlers/GameStateNAckPacketHandler.h>
#include <packethandlers/GameStateAckPacketHandler.h>

DLL_EXPORT void gscomms_execute_callback_on_main_thread(const std::function<void()>& fn, bool force)
{
	if (!g_gameServer)
	{
		fn();
		return;
	}

	g_gameServer->InternalAddMainThreadCb(fn, force);
}

void gscomms_execute_callback_on_net_thread(const std::function<void()>& fn)
{
	if (!g_gameServer)
	{
		fn();
		return;
	}

	g_gameServer->InternalAddNetThreadCb(fn);
}

void gscomms_execute_callback_on_sync_thread(const std::function<void()>& fn)
{
	if (!g_gameServer)
	{
		fn();
		return;
	}

	g_gameServer->InternalAddSyncThreadCb(fn);
}

void gscomms_reset_peer(int peer)
{
	if (!g_gameServer)
	{
		return;
	}
	gscomms_execute_callback_on_net_thread([=]()
	{
		g_gameServer->InternalResetPeer(peer);
	});
}

void gscomms_send_packet(fx::Client* client, int peer, int channel, const net::Buffer& buffer, NetPacketType flags)
{
	if (!g_gameServer)
	{
		return;
	}
	g_gameServer->InternalSendPacket(client, peer, channel, buffer, flags);
}

void gscomms_get_peer(int peer, fx::NetPeerStackBuffer& stackBuffer)
{
	g_gameServer->InternalGetPeer(peer, stackBuffer);
}

static InitFunction initFunction([]()
{
	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		using namespace fx::ServerDecorators;

		instance->SetComponent(new fx::UdpInterceptor());

		instance->SetComponent(
			WithPacketHandler<RoutingPacketHandler, IHostPacketHandler, IQuitPacketHandler, HeHostPacketHandler, ServerEventPacketHandler, ServerCommandPacketHandler, TimeSyncReqPacketHandler, StateBagPacketHandler, StateBagPacketHandlerV2, NetGameEventPacketHandlerV2, ArrayUpdatePacketHandler, ReassembledEventPacketHandler, ReassembledEventV2PacketHandler, RequestObjectIdsPacketHandler, GameStateNAckPacketHandler, GameStateAckPacketHandler>(
				WithProcessTick<ThreadWait, GameServerTick>(
					WithOutOfBand<GetInfoOutOfBand, GetStatusOutOfBand, RconOutOfBand>(
						WithEndPoints(
							NewGameServer()
						)
					),
					20
				)
			)
		);

		instance->SetComponent(new fx::PeerAddressRateLimiterStore(instance->GetComponent<console::Context>().GetRef()));
		instance->SetComponent(new HostVoteCount());
	});
});
