//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_ICE_CLIENT_H
#define STEAMNETWORKINGSOCKETS_ICE_CLIENT_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include "../steamnetworkingsockets_thinker.h"
#include "steamnetworkingsockets_p2p_ice.h"
#include <memory>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

namespace SteamNetworkingSocketsLib {
    class CSteamNetworkingSocketsSTUNRequest;

    /// Represents one local network interface used for ICE candidate gathering.
    /// Owns its socket and tracks at most one in-flight server-reflexive STUN request.
    struct ICESessionInterface
    {
        // Local-preference component of the ICE priority formula
        // (RFC 8445 §5.1.2).  Assigned as a countdown from 65535 in
        // enumeration order so the first adapter returned by the OS gets
        // the highest preference.
        uint32 m_nPriority;

        // Subnet prefix length from the OS adapter enumeration (e.g. 24
        // for a /24 network).  Used to detect same-LAN peers.  0 if the
        // OS did not provide this information.
        int m_nPrefixLen;

        // Raw UDP socket bound to this interface for sending and receiving
        // ICE traffic.  Always non-NULL, owned by this object.
        // m_pSocket->m_boundAddr is the local IP:port for this interface.
        IRawUDPSocket * const m_pSocket;

        // In-flight STUN bind request for server-reflexive discovery or
        // keepalive, or null if none is active.  At most one per interface.
        CSteamNetworkingSocketsSTUNRequest *m_pPendingSTUNRequest = nullptr;

        ICESessionInterface( uint32 p, int nPrefixLen, IRawUDPSocket *pSocket )
            : m_nPriority( p ), m_nPrefixLen( nPrefixLen ), m_pSocket( pSocket ) {}
        ~ICESessionInterface() { m_pSocket->Close(); }

        ICESessionInterface( const ICESessionInterface& ) = delete;
        ICESessionInterface& operator=( const ICESessionInterface& ) = delete;
    };

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
        // The local interface this request was sent from.  Set at construction, never null.
        ICESessionInterface * const m_pInterface;
        SteamNetworkingIPAddr m_remoteAddr;
        int m_nRetryCount;
        int m_nMaxRetries;
        CRecvSTUNPktCallback m_callback;
        uint32 m_nTransactionID[3];
        int m_nEncoding;
        CUtlVector< STUNAttribute > m_vecExtraAttrs;
        std::string m_strPassword;
		SteamNetworkingMicroseconds m_usecLastSentTime;

        static CSteamNetworkingSocketsSTUNRequest *SendBindRequest( ICESessionInterface *pIntf, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding );

        static CSteamNetworkingSocketsSTUNRequest *CreatePeerConnectivityCheckRequest( ICESessionInterface *pIntf, SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb, int nEncoding );
        void Send( SteamNetworkingIPAddr remoteAddr, CRecvSTUNPktCallback cb );
        void Cancel();

        constexpr static bool kPacketNotProcessed = true;
        constexpr static bool kPacketProcessed = true;
        bool OnPacketReceived( const RecvPktInfo_t &info );

    protected:
        void Think( SteamNetworkingMicroseconds usecNow ) override;
        friend class CSteamNetworkingSocketsSTUN;

    private:
        explicit CSteamNetworkingSocketsSTUNRequest( ICESessionInterface *pInterface );
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
        void AddPeerCandidate( const ICECandidate& peerCandidate, const char* pszFoundation );
        void SetRemoteUsername( const char *pszUsername );
		void SetRemotePassword( const char *pszPassword );

        const char* GetLocalPassword() { return m_strLocalPassword.c_str(); }
        IRawUDPSocket *GetSelectedSocket() { return m_pSelectedSocket; }
        SteamNetworkingIPAddr GetSelectedDestination();
		int GetPing() const;

        // Returns the subnet prefix length for the local interface matching addr,
        // or 0 if the interface was not found or has no valid prefix data.
        int GetLocalCandidatePrefixLen( const SteamNetworkingIPAddr &addr ) const;

    protected:
        void Think( SteamNetworkingMicroseconds usecNow ) override;

    private:
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

        // ICE defines two asymmetric roles: the controlling agent drives candidate
        // nomination (picks which path wins), the controlled agent follows its lead.
        // Roles are assigned by the signaling layer before ICE starts and stay fixed
        // unless a role-conflict packet forces a swap.
        EICERole m_role;

        // Random 64-bit value generated fresh each session, included in every
        // outgoing connectivity check.  When both sides accidentally claim the same
        // role, whichever has the numerically larger tiebreaker keeps that role and
        // the other side switches.
        uint64 m_nRoleTiebreaker;

        // Set to true whenever the local network topology might have changed (e.g.
        // at startup, or on a network-change notification).  The next Think() pass
        // will re-enumerate interfaces and rebuild candidates, then clear this flag.
        bool m_bInterfaceListStale;

        // Bitmask of kSTUNPacketEncodingFlags_* controlling STUN wire format quirks:
        // whether to include a fingerprint, whether to use legacy MappedAddress vs
        // XOR-MappedAddress, and whether to sign with HMAC-SHA1 (MessageIntegrity)
        // vs HMAC-SHA256.  Set to MessageIntegrity at construction for compatibility
        // with peers that don't support SHA256.
        int m_nEncoding;

        // Our ICE credentials for this session, generated once at startup from the
        // connection ID and a random block.  The username fragment identifies us;
        // the password is the HMAC key used to sign and verify connectivity checks.
        std::string m_strLocalUsernameFragment;
        std::string m_strLocalPassword;

        // The remote peer's ICE credentials, delivered via signaling.  Empty until
        // the first auth message arrives; connectivity checks cannot be validated
        // or sent until both are populated.
        std::string m_strRemoteUsernameFragment;
        std::string m_strRemotePassword;

        // Pre-built USERNAME attribute strings derived from the two ufrag values above.
        // ICE mandates the format "recipient:sender" — so the direction reverses
        // depending on whether a packet is inbound or outbound.  Caching them avoids
        // repeated string concatenation in the hot path.
        // Both are empty until remote credentials have arrived via signaling.
        std::string m_strIncomingUsername;  // local:remote — expected in packets we receive
        std::string m_strOutgoingUsername;  // remote:local — placed in packets we send

        // Dirty flag set whenever a local or remote candidate is added or removed.
        // The next Think() pass rebuilds m_vecCandidatePairs from the current
        // cross-product, then clears this flag.
        bool m_bCandidatePairsNeedUpdate;

        // Bitmask of k_EICECandidate_* types the caller has authorized us to gather
        // and use.  Host-only vs reflexive vs relay, IPv4 vs IPv6, are all controlled
        // here.  A candidate whose type bit is absent is silently dropped during
        // gathering rather than advertised to the peer.
        int m_nPermittedCandidateTypes;

        // Timestamp of the next keepalive to send on the selected path.  Zero means
        // "send immediately on the next Think()."  Only meaningful once
        // m_pSelectedCandidatePair is non-null.
        SteamNetworkingMicroseconds m_nextKeepalive;

        // The candidate pair currently in use for sending and receiving application
        // data.  Null until ICE nominates a winner.  Once set, changes only if the
        // selected path fails and a new one is nominated.
        // m_pSelectedSocket is the pre-looked-up raw socket for the local candidate
        // in that pair, kept here to avoid the per-send lookup overhead.
        ICECandidatePair *m_pSelectedCandidatePair;
        IRawUDPSocket    *m_pSelectedSocket;

        // Local network interfaces discovered during the most recent enumeration.
        // Each entry represents one usable local address.  Rebuilt whenever
        // m_bInterfaceListStale is set.
        std_vector< std::unique_ptr<ICESessionInterface> > m_vecInterfaces;

        // Resolved addresses of STUN servers, populated once at construction from the
        // config string.  Used to discover server-reflexive candidates and to dispatch
        // STUN responses back to the correct server.
        std_vector< SteamNetworkingIPAddr > m_vecSTUNServers;

        // All local candidates gathered so far (host and server-reflexive), advertised
        // to the peer via signaling.  Rebuilt on every interface re-enumeration and
        // grows as STUN responses arrive.
        std_vector< ICECandidate > m_vecCandidates;

        // Candidates received from the remote peer via signaling.  Paired with
        // m_vecCandidates to form m_vecCandidatePairs.
        std_vector< ICEPeerCandidate > m_vecPeerCandidates;

        // In-flight STUN Binding requests being used as ICE connectivity checks,
        // one per candidate pair currently under active test.
        std_vector< CSteamNetworkingSocketsSTUNRequest* > m_vecPendingPeerRequests;

        // All formed candidate pairs (cross-product of local × remote candidates,
        // pruned for duplicates).  Checked in priority order during Think(); each
        // pair tracks its own state (waiting, in-progress, succeeded, failed).
        std_vector< ICECandidatePair* > m_vecCandidatePairs;

        // Pairs needing an immediate out-of-turn connectivity check, bypassing the
        // normal paced schedule.  Used when the controlling agent nominates a pair
        // (one final check with USE-CANDIDATE must be sent) or when an incoming check
        // reveals a new valid pair that should be verified promptly.  Drained LIFO on
        // each Think() pass before the regular check list.
        std_vector< ICECandidatePair* > m_vecTriggeredCheckQueue;

        ICESessionInterface *FindInterfaceForCandidate( const SteamNetworkingIPAddr& addr );
        CSteamNetworkingSocketsSTUNRequest *FindPendingRequestByTransactionID( const uint32 nTransactionID[3] ) const;
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
