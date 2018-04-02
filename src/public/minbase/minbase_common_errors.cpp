//===== Copyright © 2012-2012, Valve Corporation, All rights reserved. =====//
//
// Common error mapping functions.
// This is intended to be included in an implementation file instead
// of being standalone.
//
//==========================================================================//

#include "minbase_identify.h"
#include "minbase_common_errors.h"
#include "errno.h"
#include "winlite.h"

ECommonError TranslateCommonErrno()
{
    switch(errno)
    {
    case EPERM:
    case EACCES:
        return k_EErrAccessDenied;
    case ENOENT:
    case ESRCH:
        return k_EErrNotFound;
    case EBADF:
        return k_EErrInvalidParameter;
    case ENOMEM:
        return k_EErrOutOfMemory;
    case EEXIST:
        return k_EErrAlreadyExists;
    default:
        return k_EErrUnknownError;
    }
}

#if defined(_WIN32)

ECommonError TranslateCommonLastError()
{
    switch(GetLastError())
    {
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return k_EErrOutOfMemory;
    case ERROR_ACCESS_DENIED:
        return k_EErrAccessDenied;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return k_EErrNotFound;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        return k_EErrInvalidParameter;
    case ERROR_ALREADY_EXISTS:
        return k_EErrAlreadyExists;
    case ERROR_HANDLE_EOF:
        return k_EErrEndOfFile;
    case ERROR_HANDLE_DISK_FULL:
        return k_EErrDiskFull;
    case ERROR_NOT_SUPPORTED:
        return k_EErrNotSupported;
    default:
        return k_EErrUnknownError;
    }
}

#endif // #if defined(_WIN32)
