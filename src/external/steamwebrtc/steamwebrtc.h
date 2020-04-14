//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
//
// Simplified interface to a WebRTC session
//
// This wrapper is designed to provide the WebRTC peer connection functionality in a separate
// shared library to encapsulate the build environment and C++ runtime requirements of WebRTC.
//
//==================================================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>


//-----------------------------------------------------------------------------
// Increment this if the delegate classes below change interfaces
//-----------------------------------------------------------------------------
#define STEAMWEBRTC_INTERFACE_VERSION	1


//-----------------------------------------------------------------------------
// Class to encode a video as H.264
//-----------------------------------------------------------------------------
class IWebRTCH264Encoder
{
public:
	// Supported profiles
	enum EProfile
	{
		k_EProfileBaseline,
		k_EProfileMain,
		k_EProfileHigh,
	};

	struct EncoderConfig_t
	{
		EProfile m_eProfile;
		int m_nLevel;			// 10 * level, e.g. level 4.1 = 41
		int m_nWidth;
		int m_nHeight;
		int m_nKeyFrameInterval;
		uint32_t m_nBitrate;
		uint32_t m_nMaxBitrate;
		float m_flFramerate;
	};

	struct Picture_t
	{
		bool m_bKeyframe;			// On input, whether a keyframe is requested, on output, whether it's an IDR frame
		uint8_t *m_pData;
		size_t m_nSize;
	};

public:
	virtual unsigned int AddRef() = 0;
	virtual unsigned int Release() = 0;

	virtual bool BEncodePicture( Picture_t &picture ) = 0;
	virtual bool BUpdateBitrate( int nBitrate ) = 0;
	virtual bool BUpdateFramerate( float flFramerate ) = 0;
};
typedef IWebRTCH264Encoder *(*WebRTCH264EncoderFactoryFunc_t)( const IWebRTCH264Encoder::EncoderConfig_t &config );


//-----------------------------------------------------------------------------
// The current state of the WebRTC session
//-----------------------------------------------------------------------------
enum EWebRTCSessionState
{
	k_EWebRTCSessionStateNew,
	k_EWebRTCSessionStateConnecting,
	k_EWebRTCSessionStateConnected,
	k_EWebRTCSessionStateDisconnected,
	k_EWebRTCSessionStateFailed,
	k_EWebRTCSessionStateClosed,
};


//-----------------------------------------------------------------------------
// Class to handle state changes in the WebRTC connection
//-----------------------------------------------------------------------------
class IWebRTCSessionDelegate
{
public:
	enum ELogPriority
	{
		k_ELogPriorityDebug,
		k_ELogPriorityVerbose,
		k_ELogPriorityInfo,
		k_ELogPriorityWarning,
		k_ELogPriorityError
	};

public:
	virtual void Log( ELogPriority ePriority, const char *pszMessageFormat, ... ) = 0;

	// Called during initialization, return any STUN servers that should be used (default is stun:stun.l.google.com:19302)
	virtual int GetNumStunServers() { return 0; }
	virtual const char *GetStunServer( int iIndex ) { return nullptr; }

	// Called during initialization, return any TURN servers that should be used
	virtual int GetNumTurnServers() { return 0; }
	virtual const char *GetTurnServer( int iIndex ) { return nullptr; }
	virtual const char *GetTurnServerUsername() { return nullptr; }
	virtual const char *GetTurnServerPassword() { return nullptr; }

	// Called during initialization, return true if you only want TURN relay candidates
	virtual bool BUseOnlyRelay() { return false; }

	// Called when the connection state changes
	virtual void OnSessionStateChanged( EWebRTCSessionState eState ) { }

	// Called with the result of CreateOffer()
	virtual void OnOfferReady( bool bSuccess, const char *pszOffer ) = 0;

	// Called with the result of CreateAnswer()
	virtual void OnAnswerReady( bool bSuccess, const char *pszAnswer ) = 0;

	// Called when an ICE candidate becomes available
	virtual void OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) { }

	// Called when the ICE candidate list is complete
	// The parameter is a JSON encoded array of candidates
	virtual void OnIceCandidatesComplete( const char *pszCandidates ) { }

	// Called when data can be sent on the data channel
	virtual void OnSendPossible() { }

	// Called when data is received on the data channel
	virtual void OnData( const uint8_t *pData, size_t nSize ) { }
};


//-----------------------------------------------------------------------------
// Class to represent a WebRTC connection
//-----------------------------------------------------------------------------
class IWebRTCSession
{
public:
	virtual unsigned int AddRef() = 0;
	virtual unsigned int Release() = 0;

	virtual EWebRTCSessionState GetState() = 0;

	// These must be called before calling CreateOffer() or CreateAnswer()
	virtual void SetH264EncoderFactory( WebRTCH264EncoderFactoryFunc_t pVideoEncoderFactoryFunc ) = 0;
	virtual bool BAddVideoChannel( int nWidth, int nHeight, float flFrameRate ) = 0;
	virtual bool BAddAudioChannel( int nChannels, int nFrequency ) = 0;
	virtual bool BAddDataChannel( bool bReliable ) = 0;

	virtual bool BCreateOffer() = 0;
	virtual bool BCreateAnswer( const char *pszOffer ) = 0;
	virtual bool BSetAnswer( const char *pszAnswer ) = 0;
	virtual bool BSetRemoteIceCandidates( const char *pszIceCandidates ) = 0;
	virtual bool BAddRemoteIceCandidate( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) = 0;

	// Send a frame of NV12 video data, nSize is in bytes
	virtual bool BSendVideo( const uint8_t *pData, size_t nSize ) = 0;

	// Send 16-bit audio data, nSize is in bytes
	virtual bool BSendAudio( const uint8_t *pData, size_t nSize ) = 0;

	// Send arbitrary binary data over the data channel
	virtual bool BSendData( const uint8_t *pData, size_t nSize ) = 0;
};


//-----------------------------------------------------------------------------
// The default port used by the STUN protocol
//-----------------------------------------------------------------------------
#define DEFAULT_TURN_PROTOCOL_PORT	3478


//-----------------------------------------------------------------------------
// Class to handle logging in the WebRTC TURN server
//-----------------------------------------------------------------------------
class IWebRTCTURNServerDelegate
{
public:
	enum ELogPriority
	{
		k_ELogPriorityDebug,
		k_ELogPriorityVerbose,
		k_ELogPriorityInfo,
		k_ELogPriorityWarning,
		k_ELogPriorityError
	};

public:
	virtual void Log( ELogPriority ePriority, const char *pszMessageFormat, ... ) = 0;

	// Called during initialization, return the IP address of the network interface to use for TURN requests
	virtual const char *GetBindInterfaceAddress() = 0;

	// Called during initialization, return the network port to use to listen for TURN requests
	virtual int GetBindInterfacePort() = 0;

	// Called when a TURN allocation (relay candidate) is created
	virtual void OnTurnAllocationCreated() { }

	// Called when a TURN allocation is destroyed
	virtual void OnTurnAllocationDestroyed() { }

	// Called when STUN or TURN protocol packets arrive
	virtual void OnProtocolPacket( size_t nPacketSize ) { }

	// Called when relay data packets arrive
	virtual void OnDataPacket( size_t nPacketSize ) { }
};


//-----------------------------------------------------------------------------
// Class to represent a TURN server
//-----------------------------------------------------------------------------
class IWebRTCTURNServer
{
public:
	virtual unsigned int AddRef() = 0;
	virtual unsigned int Release() = 0;

	// Add valid TURN username and password credentials
	virtual void AddCredentials( const char *pszUsername, const char *pszPassword ) = 0;
	virtual void DelCredentials( const char *pszUsername ) = 0;
};


// Some compilers use a special export keyword
#ifndef DECLSPEC
# if defined(_WIN32)
#  define DECLSPEC __declspec(dllexport)
# else
#  if defined(__GNUC__) && __GNUC__ >= 4
#   define DECLSPEC __attribute__ ((visibility("default")))
#  else
#   define DECLSPEC
#  endif
# endif
#endif

extern "C"
{
DECLSPEC IWebRTCSession *CreateWebRTCSession( IWebRTCSessionDelegate *pDelegate, int nInterfaceVersion = STEAMWEBRTC_INTERFACE_VERSION );
DECLSPEC IWebRTCTURNServer *CreateWebRTCTURNServer( IWebRTCTURNServerDelegate *pDelegate, int nInterfaceVersion = STEAMWEBRTC_INTERFACE_VERSION );
}
