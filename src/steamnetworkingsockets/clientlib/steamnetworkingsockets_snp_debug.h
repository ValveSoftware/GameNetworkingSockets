//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

bool SNPDebugWindowActive();
void InitSNPDebugWindow();
void ShutdownSNPDebugWindow();
void RunFrameSNPDebugWindow();
void SetSNPDebugText( int nCols, const char **ppszTextArray );

} // namespace SteamNetworkingSocketsLib

