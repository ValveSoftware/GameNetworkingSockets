//========= Copyright ©, Valve LLC, All rights reserved. ============
//
// Purpose: declares a variety of constants 
//
// $NoKeywords: $
//=============================================================================

#ifndef T0CONSTANTS_H
#define T0CONSTANTS_H
#ifdef _WIN32
#pragma once
#endif

//-----------------------------------------------------------------------------
// numeric constants to avoid typos with wrong number of zeros
//-----------------------------------------------------------------------------
const int64 k_nBillion = 1000000000;
const int64 k_nMillion = 1000000;
const int64 k_nThousand = 1000;
const int64 k_nKiloByte = 1024;
const int64 k_nMegabyte = k_nKiloByte * k_nKiloByte;
const int64 k_nGigabyte = k_nMegabyte * k_nKiloByte;
const int64 k_nTerabyte = k_nGigabyte * k_nKiloByte;

//-----------------------------------------------------------------------------
// Timing constants
//-----------------------------------------------------------------------------

const unsigned int k_nSecondsPerHour = 60*60;
const unsigned int k_nSecondsPerDay = k_nSecondsPerHour * 24;

const int k_cSecondsPerMinute = 60;
const int k_cSecondsPerHour = k_cSecondsPerMinute * 60;
const int k_cSecondsPerDay = k_cSecondsPerHour * 24;
const int k_cSecondsPerWeek = k_cSecondsPerDay * 7;
const int k_cSecondsPerYear = k_cSecondsPerDay * 365;

#endif // T0CONSTANTS_H