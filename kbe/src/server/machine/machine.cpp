#include "machine.hpp"
#include "machine_interface.hpp"
#include "network/common.hpp"
#include "network/tcp_packet.hpp"
#include "network/udp_packet.hpp"
#include "network/message_handler.hpp"
#include "network/bundle_broadcast.hpp"
#include "thread/threadpool.hpp"
#include "server/componentbridge.hpp"

namespace KBEngine{
	
ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Machine);

//-------------------------------------------------------------------------------------
Machine::Machine(Mercury::EventDispatcher& dispatcher, 
				 Mercury::NetworkInterface& ninterface, COMPONENT_TYPE componentType):
	ServerApp(dispatcher, ninterface, componentType),
	broadcastAddr_(0),
	ep_(),
	epBroadcast_(),
	epLocal_(),
	pEPPacketReceiver_(NULL),
	pEBPacketReceiver_(NULL),
	pEPLocalPacketReceiver_(NULL)

{
}

//-------------------------------------------------------------------------------------
Machine::~Machine()
{
	SAFE_RELEASE(pEPPacketReceiver_);
	SAFE_RELEASE(pEBPacketReceiver_);
	SAFE_RELEASE(pEPLocalPacketReceiver_);
}

//-------------------------------------------------------------------------------------
void Machine::onBroadcastInterface(int32 uid, std::string& username,
								   int8 componentType, int32 componentID, 
								   uint32 addr, uint16 port)
{
	if(componentType == MACHINE_TYPE)
	{
		if(uid == getUserUID() && 
			addr == this->getNetworkInterface().addr().ip)
		{
			return;
		}
	}
	else if(componentType == DBMGR_TYPE) // 如果是dbmgr重启了则清空所有这个uid的记录。
	{
		Componentbridge::getComponents().clear(uid);
	}

	INFO_MSG("Machine::onBroadcastInterface: uid:%d, username:%s, componentType:%s, "
			"componentID:%d, addr:%s, port:%u\n", 
		uid, username.c_str(), COMPONENT_NAME[componentType], componentID, inet_ntoa((struct in_addr&)addr), ntohs(port));

	Componentbridge::getComponents().addComponent(uid, username.c_str(), 
		(KBEngine::COMPONENT_TYPE)componentType, componentID, addr, port);
}

//-------------------------------------------------------------------------------------
void Machine::onFindInterfaceAddr(int32 uid, std::string& username, int8 componentType, 
								  int8 findComponentType, uint32 finderAddr, uint16 finderRecvPort)
{
	INFO_MSG("Machine::onFindInterfaceAddr: uid:%d, username:%s, componentType:%s, "
		"find:%s, finderaddr:%s, finderRecvPort:%u\n", 
		uid, username.c_str(), COMPONENT_NAME[componentType],  COMPONENT_NAME[findComponentType], 
		inet_ntoa((struct in_addr&)finderAddr), ntohs(finderRecvPort));

	Mercury::EndPoint ep;
	ep.socket(SOCK_DGRAM);

	if (!ep.good())
	{
		ERROR_MSG("Machine::onFindInterfaceAddr: Failed to create socket.\n");
		return;
	}
	
	const Components::ComponentInfos* pinfos = 
		Componentbridge::getComponents().findComponent((KBEngine::COMPONENT_TYPE)findComponentType, uid, 0);
	
	Mercury::Bundle bundle;

	if(pinfos == NULL)
	{
		WARNING_MSG("Machine::onFindInterfaceAddr: %s not found %s.\n", COMPONENT_NAME[componentType], 
			COMPONENT_NAME[findComponentType]);

		MachineInterface::onBroadcastInterfaceArgs6::staticAddToBundle(bundle, 0, 
			"", UNKNOWN_COMPONENT_TYPE, 0, 0, 0);
	}
	else
		MachineInterface::onBroadcastInterfaceArgs6::staticAddToBundle(bundle, pinfos->uid, 
			pinfos->username, findComponentType, 0, pinfos->addr, pinfos->port);

	bundle.sendto(ep, finderRecvPort, finderAddr);
}

//-------------------------------------------------------------------------------------
bool Machine::findBroadcastInterface()
{

	std::map<u_int32_t, std::string> interfaces;
	Mercury::BundleBroadcast bhandler(networkInterface_, KBE_PORT_BROADCAST_DISCOVERY);

	// Perform a discovery of all network interfaces on this host
	if (!bhandler.epListen().getInterfaces(interfaces))
	{
		ERROR_MSG("Failed to discover network interfaces\n");
		return false;
	}


	uint8 data = 1;
	bhandler << data;
	if (!bhandler.broadcast(KBE_PORT_BROADCAST_DISCOVERY))
	{
		ERROR_MSG("Failed to send broadcast discovery message. error:%s\n", kbe_strerror());
		return false;
	}
	
	sockaddr_in	sin;

	if(bhandler.receive(NULL, &sin))
	{
		INFO_MSG("Broadcast discovery receipt from %s.\n",
					inet_ntoa((struct in_addr&)sin.sin_addr.s_addr) );

		std::map< u_int32_t, std::string >::iterator iter;

		iter = interfaces.find( (u_int32_t &)sin.sin_addr.s_addr );
		if (iter != interfaces.end())
		{
			INFO_MSG("Confirmed %s (%s) as default broadcast route interface.\n",
				inet_ntoa((struct in_addr&)sin.sin_addr.s_addr),
				iter->second.c_str() );
			broadcastAddr_ = sin.sin_addr.s_addr;
			return true;
		}
	}
	
	ERROR_MSG("Broadcast discovery %s not a valid interface.\n",
			inet_ntoa((struct in_addr&)sin.sin_addr.s_addr) );

	return false;
}

//-------------------------------------------------------------------------------------
bool Machine::initNetwork()
{
	epBroadcast_.socket(SOCK_DGRAM);
	ep_.socket(SOCK_DGRAM);
	epLocal_.socket(SOCK_DGRAM); 

	Mercury::Address address;
	address.ip = 0;
	address.port = 0;

	if (broadcastAddr_ == 0 && !this->findBroadcastInterface())
	{
		ERROR_MSG("Failed to determine default broadcast interface. "
				"Make sure that your broadcast route is set correctly. "
				"e.g. /sbin/ip route add broadcast 255.255.255.255 dev eth0\n" );
		return false;
	}

	if (!ep_.good() ||
		 ep_.bind(htons(KBE_MACHINE_BRAODCAST_PORT), broadcastAddr_) == -1)
	{
		ERROR_MSG("Failed to bind socket to '%s'. %s.\n",
							inet_ntoa((struct in_addr &)broadcastAddr_),
							kbe_strerror());
		return false;
	}

	address.ip = broadcastAddr_;
	address.port = htons(KBE_MACHINE_BRAODCAST_PORT);
	ep_.setbroadcast( true );
	ep_.setnonblocking(true);
	ep_.addr(address);
	pEPPacketReceiver_ = new Mercury::UDPPacketReceiver(ep_, this->getNetworkInterface());

	if(!this->getMainDispatcher().registerFileDescriptor(ep_, pEPPacketReceiver_))
	{
		ERROR_MSG("registerFileDescriptor ep is failed!\n");
		return false;
	}

	if (!epBroadcast_.good() ||
		epBroadcast_.bind(htons(KBE_MACHINE_BRAODCAST_PORT), Mercury::BROADCAST) == -1)
	{
		ERROR_MSG("Failed to bind socket to '%s'. %s.\n",
							inet_ntoa((struct in_addr &)Mercury::BROADCAST),
							kbe_strerror());
//		return false;
	}
	else
	{
		address.ip = Mercury::BROADCAST;
		address.port = htons(KBE_MACHINE_BRAODCAST_PORT);
		epBroadcast_.setnonblocking(true);
		epBroadcast_.addr(address);
		pEBPacketReceiver_ = new Mercury::UDPPacketReceiver(epBroadcast_, this->getNetworkInterface());
	
		if(!this->getMainDispatcher().registerFileDescriptor(epBroadcast_, pEBPacketReceiver_))
		{
			ERROR_MSG("registerFileDescriptor epBroadcast is failed!\n");
			return false;
		}

	}

	if (!epLocal_.good() ||
		 epLocal_.bind(htons(KBE_MACHINE_BRAODCAST_PORT), Mercury::LOCALHOST) == -1)
	{
		ERROR_MSG("Failed to bind socket to (lo). %s.\n",
							kbe_strerror() );
		return false;
	}

	address.ip = Mercury::LOCALHOST;
	address.port = htons(KBE_MACHINE_BRAODCAST_PORT);
	epLocal_.setnonblocking(true);
	epLocal_.addr(address);
	pEPLocalPacketReceiver_ = new Mercury::UDPPacketReceiver(epLocal_, this->getNetworkInterface());

	if(!this->getMainDispatcher().registerFileDescriptor(epLocal_, pEPLocalPacketReceiver_))
	{
		ERROR_MSG("registerFileDescriptor epLocal is failed!\n");
		return false;
	}

	INFO_MSG("bind broadcast successfully! addr:%s\n", ep_.addr().c_str());
	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::run()
{
	bool ret = true;

	while(!this->getMainDispatcher().isBreakProcessing())
	{
		this->getMainDispatcher().processOnce(false);
		getNetworkInterface().handleChannels(&MachineInterface::messageHandlers);
		KBEngine::sleep(100);
	};

	return ret;
}

//-------------------------------------------------------------------------------------
void Machine::handleTimeout(TimerHandle handle, void * arg)
{
}

//-------------------------------------------------------------------------------------
bool Machine::initializeBegin()
{
	if(!initNetwork())
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::inInitialize()
{
	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::initializeEnd()
{
	return true;
}

//-------------------------------------------------------------------------------------
void Machine::finalise()
{
}

//-------------------------------------------------------------------------------------

}
