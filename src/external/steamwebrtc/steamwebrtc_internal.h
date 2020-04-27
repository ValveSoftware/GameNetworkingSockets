//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
#pragma once

#if defined(_WIN32)
#  define STEAMWEBRTC_DECLSPEC __declspec(dllexport)
# else
#  if defined(__GNUC__) && __GNUC__ >= 4
#   define STEAMWEBRTC_DECLSPEC __attribute__ ((visibility("default")))
#  else
#   define STEAMWEBRTC_DECLSPEC
#  endif
#endif

