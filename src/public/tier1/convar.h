#pragma once
#include <stdlib.h>
#include <string>

// Stub

struct ConVar
{
	int m_nIntVal;
	float m_flFloatVal;
	std::string m_sStringVal;
	ConVar( const char * /* name */, const char *pszDefault, int /* flags */, const char * /* comment */ )
	{
		m_nIntVal = atoi( pszDefault );
		m_flFloatVal = (float)atof( pszDefault );
		m_sStringVal = pszDefault;
	}
	inline int GetInt() const { return m_nIntVal; }
	inline float GetFloat() const { return m_flFloatVal; }
	inline const char *GetString() const { return m_sStringVal.c_str(); }
	inline bool GetBool() const { return m_nIntVal != 0; }
};

