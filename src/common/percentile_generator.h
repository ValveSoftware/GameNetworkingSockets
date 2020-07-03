//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef PERCENTILE_GENERATOR_H
#define PERCENTILE_GENERATOR_H
#pragma once

#include <tier0/dbg.h>
#include <vstdlib/random.h>

#include <tier0/memdbgoff.h>
#include <algorithm>
#include <tier0/memdbgon.h>


/// Used to collect samples and then get a percentile breakdown of the data.
/// This class can be used even if the number of data points you collect grows
/// beyond the number you want to store in memory, by keeping a random
/// subsample.  Note that if the table is filled and we have to resort
/// to sub-sampling, that the resulting sample will be based on the entire
/// data set you provide.  It will not be biased towards the first or last samples.
template < typename T, int MAX_SAMPLES = 1000 >
class PercentileGenerator
{
public:
	PercentileGenerator() { Clear(); }

	/// Throw away all samples and restart collection
	void Clear() { m_nSamples = m_nSamplesTotal = 0; m_bNeedSort = false; }

	/// Add a sample
	void AddSample( T x );

	/// Return number of samples we have right now.  This is always <=
	/// MAX_SAMPLES
	int NumSamples() const { return m_nSamples; }

	/// Total number samples we have ever received
	int NumSamplesTotal() const { return m_nSamplesTotal; }

	/// Max number of samples we can accept.
	static int MaxSamples() { return MAX_SAMPLES; }

	/// Fetch an estimate of the Nth percentile.
	/// The percentile should in the range (0,1).  (exclusive)
	///
	/// Before using this blindly, you should ensure that you have a
	/// sufficient number of samples for the percentile you are asking for.
	/// You only need a handful of samples to get a reasonable estimate of the
	/// median, but you need more samples to get a quality estimate for the
	/// percentile further away from the median.
	T GetPercentile( float flPct ) const;

private:
	int m_nSamples;
	int m_nSamplesTotal;
	mutable bool m_bNeedSort;

	/// Raw sample data
	T m_arSamples[ MAX_SAMPLES ];
};


template < typename T, int MAX_SAMPLES >
void PercentileGenerator<T,MAX_SAMPLES>::AddSample( T x )
{

	// Still have room to keep all the samples?
	if ( m_nSamples < MAX_SAMPLES )
	{
		// Just store it
		m_arSamples[ m_nSamples ] = x;
		++m_nSamples;
		m_bNeedSort = true;
	}
	else
	{
		// We're full.  The goal here is to get a random subset of the overall sample.
		// We don't want it to be biased towards older samples or newer samples.
		//
		// Imagine we had the full list of all samples, and then we randomly
		// scrambled it, and then truncated that list to the first N.  This code
		// achieves the same effect.
		int r = WeakRandomInt( 0, m_nSamplesTotal );
		if ( r < MAX_SAMPLES )
		{
			m_arSamples[r] = x;
			m_bNeedSort = true;
		}
	}

	++m_nSamplesTotal;
}

template < typename T, int MAX_SAMPLES >
T PercentileGenerator<T,MAX_SAMPLES>::GetPercentile( float flPct )const
{
	// Make sure percentile is reasonable.  If you want the min or
	// max, don't use this method.
	Assert( 0 < flPct && flPct < 1.0f );

	// We have to have collected at least one sample!
	if ( m_nSamples < 1 )
	{
		Assert( m_nSamples > 0 );
		return T();
	}

	// Sort samples if necessary
	if ( m_bNeedSort )
	{
		T *pBegin = const_cast<T*>( m_arSamples );
		std::sort( pBegin, pBegin+m_nSamples );
		m_bNeedSort = false;
	}

	// Interpolate between adjacent samples
	float flIdx = flPct * float(m_nSamples-1);
	if ( flIdx <= 0.0f )
		return m_arSamples[ 0 ];
	int idx = (int)flIdx;
	if ( idx >= m_nSamples-1 )
		return m_arSamples[m_nSamples-1];

	// Cast to float first, so that we don't blow up if the type is unsigned, etc
	float l = (float)m_arSamples[idx];
	float r = (float)m_arSamples[idx+1];

	// Lerp and cast back to T
	return T( l + (r-l)*(flIdx-idx) );
}

#endif // #ifndef PERCENTILE_GENERATOR_H
