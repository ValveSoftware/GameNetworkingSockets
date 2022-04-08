//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
//
// Some types used to interface with different ICE client implementations

#ifndef ICE_CLIENT_TYPES_H
#define ICE_CLIENT_TYPES_H

#include <stddef.h>
#include <stdint.h>

enum EICERole
{
	k_EICERole_Controlling, // usually the "client" who initiated the connection
	k_EICERole_Controlled, // usually the "server" who accepted the connection
	k_EICERole_Unknown,
};

enum EICECandidateType : int
{
	k_EICECandidate_Invalid = 0,

	k_EICECandidate_IPv4_Relay = 0x01,
	k_EICECandidate_IPv4_HostPrivate = 0x02,
	k_EICECandidate_IPv4_HostPublic = 0x04,
	k_EICECandidate_IPv4_Reflexive = 0x08,

	k_EICECandidate_IPv6_Relay = 0x100,
	k_EICECandidate_IPv6_HostPrivate_Unsupported = 0x200, // NOTE: Not currently used.  All IPv6 addresses (even fc00::/7) are considered "public"
	k_EICECandidate_IPv6_HostPublic = 0x400,
	k_EICECandidate_IPv6_Reflexive = 0x800,
};

constexpr int k_EICECandidate_Any_Relay = k_EICECandidate_IPv4_Relay | k_EICECandidate_IPv6_Relay;
constexpr int k_EICECandidate_Any_HostPrivate = k_EICECandidate_IPv4_HostPrivate | k_EICECandidate_IPv6_HostPrivate_Unsupported;
constexpr int k_EICECandidate_Any_HostPublic = k_EICECandidate_IPv4_HostPublic | k_EICECandidate_IPv6_HostPublic;
constexpr int k_EICECandidate_Any_Reflexive = k_EICECandidate_IPv4_Reflexive | k_EICECandidate_IPv6_Reflexive;
constexpr int k_EICECandidate_Any_IPv4 = 0x00ff;
constexpr int k_EICECandidate_Any_IPv6 = 0xff00;
constexpr int k_EICECandidate_Any = 0xffff;

// Different protocols that may be used to talk to TURN server.
// FIXME - rename this with more specific name
enum EProtocolType {
	k_EProtocolTypeUDP,
	k_EProtocolTypeTCP,
	k_EProtocolTypeSSLTCP,  // Pseudo-TLS.
	k_EProtocolTypeTLS,
};

struct ICESessionConfig
{
	typedef const char *StunServer;
	struct TurnServer
	{
		const char *m_pszHost = nullptr;
		const char *m_pszUsername = nullptr;
		const char *m_pszPwd = nullptr;
		const EProtocolType m_protocolType = k_EProtocolTypeUDP;
	};

	EICERole m_eRole = k_EICERole_Unknown;
	int m_nStunServers = 0;
	const StunServer *m_pStunServers = nullptr;
	int m_nTurnServers = 0;
	const TurnServer *m_pTurnServers = nullptr;
	int m_nCandidateTypes = k_EICECandidate_Any;
	const char *m_pszLocalUserFrag;
	const char *m_pszLocalPwd;
};


#endif // _H
