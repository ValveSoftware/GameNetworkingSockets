//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Portable abstract error codes.
//
//==========================================================================//

#ifndef MINBASE_COMMON_ERRORS_H
#define MINBASE_COMMON_ERRORS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

enum ECommonError
{
    k_EErrNoError,
    
    // Negative result for success cases that aren't complete success.
    k_EErrNoErrorNegative,

    k_EErrInternalError,
    k_EErrGenericError,
    k_EErrUnknownError,
    k_EErrIncompleteOperation,
    k_EErrInvalidParameter,
    k_EErrInvalidRequest,
    k_EErrInvalidState,
    k_EErrInvalidFormat,
    k_EErrAlreadyOpen,
    k_EErrAlreadyExists,
    k_EErrAccessDenied,
    k_EErrOutOfMemory,
    k_EErrEndOfFile,
    k_EErrDiskFull,
    k_EErrNotFound,
    k_EErrBufferOverflow,
    k_EErrIntegerOverflow,
    k_EErrNotSupported,
    k_EErrNotImplemented,

    k_EErrMaximumEnumValue
};

ECommonError TranslateCommonErrno();
#if defined(_WIN32)
ECommonError TranslateCommonLastError();
#endif

#endif // #ifndef MINBASE_COMMON_ERRORS_H
