//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_STUN_H
#define STEAMNETWORKINGSOCKETS_STUN_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include "../steamnetworkingsockets_thinker.h"
#include "steamnetworkingsockets_p2p_ice.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

namespace SteamNetworkingSocketsLib {
    class CSharedSocket;
    class CSteamNetworkingSocketsSTUNRequest;

    const uint32 k_nSTUN_CookieValue = 0x2112A442;
    const uint32 k_nSTUN_BindingRequest = 0x0001;
    const uint32 k_nSTUN_BindingResponse = 0x0101;
    const uint32 k_nSTUN_BindingErrorResponse = 0x0111;
    const uint32 k_nSTUN_Attr_MappedAddress = 0x0001;
    const uint32 k_nSTUN_Attr_UserName = 0x0006;
    const uint32 k_nSTUN_Attr_MessageIntegrity = 0x0008;
    const uint32 k_nSTUN_Attr_MessageIntegrity_SHA256 = 0x001C;
    const uint32 k_nSTUN_Attr_XORMappedAddress = 0x0020;
    const uint32 k_nSTUN_Attr_Priority = 0x0024;
    const uint32 k_nSTUN_Attr_UseCandidate = 0x0025;
    const uint32 k_nSTUN_Attr_Fingerprint = 0x8028;
    const uint32 k_nSTUN_Attr_ICEControlled = 0x8029;
    const uint32 k_nSTUN_Attr_ICEControlling = 0x802A;

    struct STUNHeader
    {
        uint32 m_nZeroPad;
        uint32 m_nMessageType;
        uint32 m_nMessageLength;
        uint32 m_nCookie;
        uint32 m_nTransactionID[3];
    };

    struct STUNAttribute
    {
        uint32 m_nType;
        uint32 m_nLength;
        const uint32 *m_pData;
    };

    /// Info about an incoming packet passed to the CRecvSTUNPktCallback
    struct RecvSTUNPktInfo_t
    {
        CSteamNetworkingSocketsSTUNRequest *m_pRequest;
		SteamNetworkingMicroseconds m_usecNow;
        STUNHeader *m_pHeader;
        uint32 m_nAttributes;
        STUNAttribute *m_pAttributes;
    };

    /// Store the callback and its context together
    class CRecvSTUNPktCallback
    {
    public:
        /// Prototype of the callback
        typedef void (*FCallbackRecvSTUNPkt)( const RecvSTUNPktInfo_t &info, void *pContext );

        /// Default constructor sets stuff to null
        inline CRecvSTUNPktCallback() : m_fnCallback( nullptr ), m_pContext( nullptr ) {}

        /// A template constructor so you can use type safe context and avoid messy casting
        template< typename T >
        inline CRecvSTUNPktCallback( void (*fnCallback)( const RecvSTUNPktInfo_t &info, T context ), T context )
        : m_fnCallback ( reinterpret_cast< FCallbackRecvSTUNPkt>( fnCallback ) )
        , m_pContext( reinterpret_cast< void * >( context ) )
        {
            COMPILE_TIME_ASSERT( sizeof(T) == sizeof(void*) );
        }

        FCallbackRecvSTUNPkt m_fnCallback;
        void *m_pContext;

        /// Shortcut notation to execute the callback
        inline void operator()( const RecvSTUNPktInfo_t &info ) const
        {
            if ( m_fnCallback )
                m_fnCallback( info, m_pContext );
        }
    };

    enum STUNPacketEncodingFlags
    {
        kSTUNPacketEncodingFlags_None = 0,
        kSTUNPacketEncodingFlags_NoFingerprint = 1,  // Do not emit a fingerprint attr 
        kSTUNPacketEncodingFlags_MappedAddress = 2,  // Use MappedAddress, not XORMappedAddress
        kSTUNPacketEncodingFlags_NoMappedAddress = 4, // Do not emit *any* address attribute at all.
        kSTUNPacketEncodingFlags_MessageIntegrity = 8, // Use MessageIntegrity, not MessageIntegrity_SHA256
    };

	/// Track an in-flight STUN request.  The thinker interface is used to handle
	/// retry and timeout.  Note, that there is no list of in-flight requests,
	/// we use the thinker system to extant requests.  All read and write access
	/// to these objects require holding the global lock
    class CSteamNetworkingSocketsSTUNRequest : private IThinker
    {
    public:
        IBoundUDPSocket *m_pSocket = nullptr;
        SteamNetworkingIPAddr m_localAddr;
        SteamNetworkingIPAddr m_remoteAddr;
        int m_nRetryCount;
        int m_nMaxRetries;
        CRecvSTUNPktCallback m_callback;
        uint32 m_nTransactionID[3];
        int m_nEncoding;
        CUtlVector< STUNAttribute > m_vecExtraAttrs;
        std::string m_strPassword;
		SteamNetworkingMicroseconds m_usecLastSentTime;

        static CSteamNetworkingSocketsSTUNRequest *SendBindRequest( CSharedSocket *pSharedSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding );   
        static CSteamNetworkingSocketsSTUNRequest *SendBindRequest( IBoundUDPSocket *pBoundSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding );   
        
        static CSteamNetworkingSocketsSTUNRequest *CreatePeerConnectivityCheckRequest( CSharedSocket *pSharedSock, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding );
        void Send( SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb );
        void Cancel();

        constexpr static bool kPacketNotProcessed = true;
        constexpr static bool kPacketProcessed = true;
        bool OnPacketReceived( const RecvPktInfo_t &info );

    protected:
        void Think( SteamNetworkingMicroseconds usecNow ) override;
        friend class CSteamNetworkingSocketsSTUN;

    private:
        CSteamNetworkingSocketsSTUNRequest();
        ~CSteamNetworkingSocketsSTUNRequest();

        static void StaticPacketReceived( const RecvPktInfo_t &info, CSteamNetworkingSocketsSTUNRequest *pContext );

        CSteamNetworkingSocketsSTUNRequest( const CSteamNetworkingSocketsSTUNRequest& );
        CSteamNetworkingSocketsSTUNRequest& operator=( const CSteamNetworkingSocketsSTUNRequest& );
    };
    
    class CSteamNetworkingICESessionCallbacks;

	/// Main logic of establishing an ICE session with a peer.  In real-world
	/// uses cases this is always associated one-to-one with a CConnectionTransportP2PICE_Valve.
	/// But breaking it out into a separate object helps with testing.
	/// Also, this object is only protected by the global lock, and accessing
	/// the transport also requires the connection lock.
    class CSteamNetworkingICESession : private IThinker
    {
    public:       
        CSteamNetworkingICESession( EICERole role, CSteamNetworkingICESessionCallbacks *pCallbacks, int nEncoding );
		CSteamNetworkingICESession( const ICESessionConfig& cfg, CSteamNetworkingICESessionCallbacks *pCallbacks );
		~CSteamNetworkingICESession();

        void StartSession();
        void InvalidateInterfaceList();

        enum ICECandidateType
        {
            kICECandidateType_Host,
            kICECandidateType_ServerReflexive,
            //kICECandidateType_Relayed,
            kICECandidateType_PeerReflexive,
            kICECandidateType_None
        };
        struct ICECandidate
        {
            ICECandidateType m_type;
            SteamNetworkingIPAddr m_addr;
            SteamNetworkingIPAddr m_base;
            SteamNetworkingIPAddr m_stunServer;
            uint32 m_nPriority;
            ICECandidate();
            ICECandidate( ICECandidateType t, const SteamNetworkingIPAddr& addr, const SteamNetworkingIPAddr& base );
            ICECandidate( ICECandidateType t, const SteamNetworkingIPAddr& addr, const SteamNetworkingIPAddr& base, const SteamNetworkingIPAddr& stunServer );
            uint32 CalcPriority( uint32 nLocalPreference );
            void CalcCandidateAttribute( char *pszBuffer, size_t nBufferSize ) const;
			EICECandidateType CalcType() const;
        };
        EICERole GetRole() { return m_role; }
        enum ICESessionState
        {
            kICESessionState_Idle,
            kICESessionState_GatheringCandidates,
            kICESessionState_TestingPeerConnectivity
        };
        ICESessionState GetSessionState();
        void AddPeerCandidate( const ICECandidate& peerCandidate, const char* pszFoundation );
        void SetRemoteUsername( const char *pszUsername );
		void SetRemotePassword( const char *pszPassword );

        bool GetCandidates( CUtlVector< ICECandidate >* pOutVecCandidates );
        const char* GetLocalPassword() { return m_strLocalPassword.c_str(); }
        CSharedSocket *GetSelectedSocket() { return m_pSelectedSocket; }
        SteamNetworkingIPAddr GetSelectedDestination();
		int GetPing() const;

    protected:
        void Think( SteamNetworkingMicroseconds usecNow ) override;

    private:
        struct Interface
        {
            SteamNetworkingIPAddr m_localaddr;
            uint32 m_nPriority;
            Interface( const SteamNetworkingIPAddr& ipAddr, uint32 p ) : m_localaddr( ipAddr ), m_nPriority( p ) {}
        };

        struct ICEPeerCandidate : public ICECandidate
        {
            std::string m_sFoundation;
            ICEPeerCandidate( const ICECandidate& c, const char *pszFoundation ) : ICECandidate( c ), m_sFoundation( pszFoundation ) {}
        };

        enum ICECandidatePairState
        {
            kICECandidatePairState_Frozen,
            kICECandidatePairState_Waiting,
            kICECandidatePairState_InProgress,
            kICECandidatePairState_Succeeded,
            kICECandidatePairState_Failed,
            kICECandidatePairState_None

        };
        struct ICECandidatePair
        {
            ICECandidatePairState m_nState;
            bool m_bNominated;
            uint64 m_nPriority;
            ICECandidate m_localCandidate;
            ICEPeerCandidate m_remoteCandidate;
            CSteamNetworkingSocketsSTUNRequest *m_pPeerRequest;
			int m_nLastRecordedPing;
            ICECandidatePair( const ICECandidate& localCandidate, const ICEPeerCandidate& remoteCandidate, EICERole role );
        };

        CSteamNetworkingICESessionCallbacks *m_pCallbacks;
        EICERole m_role;
        uint64 m_nRoleTiebreaker;
        ICESessionState m_sessionState;
        bool m_bInterfaceListStale;
        int m_nEncoding;
        std::string m_strLocalUsernameFragment;
        std::string m_strLocalPassword;
        std::string m_strRemoteUsernameFragment;
        std::string m_strRemotePassword;
        std::string m_strIncomingUsername;
        std::string m_strOutgoingUsername;
        bool m_bCandidatePairsNeedUpdate;
		int m_nPermittedCandidateTypes;

        SteamNetworkingMicroseconds m_nextKeepalive;
        ICECandidatePair *m_pSelectedCandidatePair;
        CSharedSocket *m_pSelectedSocket;
        std_vector< Interface > m_vecInterfaces;
        std_vector< CSharedSocket* > m_vecSharedSockets;
        std_vector< SteamNetworkingIPAddr > m_vecSTUNServers;
        std_vector< ICECandidate > m_vecCandidates;
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingServerReflexiveRequests;
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingServerReflexiveKeepAliveRequests;       
        std_vector< ICEPeerCandidate > m_vecPeerCandidates;
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingPeerRequests;
        std_vector< ICECandidatePair* > m_vecCandidatePairs;
        std_vector< ICECandidatePair* > m_vecTriggeredCheckQueue;
       
        CSharedSocket* FindSharedSocketForCandidate( const SteamNetworkingIPAddr& addr );
        void GatherInterfaces();
        void UpdateHostCandidates();
        void UpdateKeepalive( const ICECandidate& c );
        uint32 GetInterfaceLocalPreference( const SteamNetworkingIPAddr& addr );
		bool IsCandidatePermitted( const ICECandidate& localCandidate );

        void Think_KeepAliveOnCandidates( SteamNetworkingMicroseconds usecNow );
        void Think_DiscoverServerReflexiveCandidates();
        void Think_TestPeerConnectivity();

        void SetSelectedCandidatePair( ICECandidatePair *pPair );

        void STUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info );
        static void StaticSTUNRequestCallback_ServerReflexiveCandidate( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext );
        void STUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info );
        static void StaticSTUNRequestCallback_ServerReflexiveKeepAlive( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext );
        void STUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info );
        static void StaticSTUNRequestCallback_PeerConnectivityCheck( const RecvSTUNPktInfo_t &info, CSteamNetworkingICESession* pContext );
    
        void OnPacketReceived( const RecvPktInfo_t &info );
        static void StaticPacketReceived( const RecvPktInfo_t &info, CSteamNetworkingICESession *pContext );
    };

    class CSteamNetworkingICESessionCallbacks
    {
    public:
        virtual void OnLocalCandidateDiscovered( const CSteamNetworkingICESession::ICECandidate& candidate ) {}
        virtual void OnPacketReceived( const RecvPktInfo_t &info ) {}
        virtual void OnConnectionSelected( const CSteamNetworkingICESession::ICECandidate& localCandidate, const CSteamNetworkingICESession::ICECandidate& remoteCandidate ) {}
    };


	/// Connection transport that sends datagrams using the route discovered
	/// by our ICE client, CSteamNetworkingICESession
    class CConnectionTransportP2PICE_Valve final
        : public CConnectionTransportP2PICE, public CSteamNetworkingICESessionCallbacks
    {
    public:
        CConnectionTransportP2PICE_Valve( CSteamNetworkConnectionP2P &connection );
    	void Init( const ICESessionConfig& cfg );

    private:
        CSteamNetworkingICESession *m_pICESession;

    	virtual bool BCanSendEndToEndData() const override;
		virtual void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow ) override;
    	virtual void TransportFreeResources() override;

        // Implements CConnectionTransportUDPBase
        virtual bool SendPacket( const void *pkt, int cbPkt ) override;
        virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;

    protected:
        virtual void OnLocalCandidateDiscovered( const CSteamNetworkingICESession::ICECandidate& candidate ) override;
        virtual void OnPacketReceived( const RecvPktInfo_t &info ) override;
        virtual void OnConnectionSelected( const CSteamNetworkingICESession::ICECandidate& localCandidate, const CSteamNetworkingICESession::ICECandidate& remoteCandidate ) override;
    };

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // _H
