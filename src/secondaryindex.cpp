//
// Copyright (c) 2018-2021, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "secondaryindex.h"

#include "histogram.h"
#include "sphinxint.h"
#include "killlist.h"
#include "attribute.h"


static bool HaveIndex ( const CSphString & sAttr )
{
	return sAttr=="id";
}

//////////////////////////////////////////////////////////////////////////

bool ReturnIteratorResult ( RowID_t * pRowID, RowID_t * pRowIdStart, RowIdBlock_t & dRowIdBlock )
{
	if ( pRowID==pRowIdStart )
		return false;

	dRowIdBlock = RowIdBlock_t(pRowIdStart, pRowID-pRowIdStart);
	return true;
}

//////////////////////////////////////////////////////////////////////////

class SecondaryIndexIterator_c : public RowidIterator_i
{
protected:
	static const int MAX_COLLECTED = 128;
	CSphFixedVector<RowID_t> m_dCollected {MAX_COLLECTED};
};


class RowidIterator_LookupValues_c : public SecondaryIndexIterator_c
{
public:
						RowidIterator_LookupValues_c ( const SphAttr_t * pValues, int nValues, const BYTE * pDocidLookup );

	bool				HintRowID ( RowID_t tRowID ) override;
	bool				GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock ) override;
	int64_t				GetNumProcessed() const override;

private:
	int64_t				m_iProcessed {0};
	LookupReaderIterator_c m_tLookupReader;
	DocidListReader_c	m_tFilterReader;
	bool				m_bHaveFilterDocs {false};
	bool				m_bHaveLookupDocs {false};
	DocID_t				m_tFilterDocID {0};
	DocID_t				m_tLookupDocID {0};
	RowID_t				m_tLookupRowID {INVALID_ROWID};
};


RowidIterator_LookupValues_c::RowidIterator_LookupValues_c ( const SphAttr_t * pValues, int nValues, const BYTE * pDocidLookup )
	: m_tLookupReader ( pDocidLookup )
	, m_tFilterReader ( pValues, nValues )
{
	// warmup
	m_bHaveFilterDocs = m_tFilterReader.ReadDocID ( m_tFilterDocID );
	m_bHaveLookupDocs = m_tLookupReader.Read ( m_tLookupDocID, m_tLookupRowID );
	m_iProcessed += m_bHaveFilterDocs ? 1 : 0;
	m_iProcessed += m_bHaveLookupDocs ? 1 : 0;
}


bool RowidIterator_LookupValues_c::HintRowID ( RowID_t tRowID )
{
	// can't rewind lookup reader
	return true;
}


bool RowidIterator_LookupValues_c::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	RowID_t * pRowIdStart = m_dCollected.Begin();
	RowID_t * pRowIdMax = pRowIdStart + m_dCollected.GetLength()-1;
	RowID_t * pRowID = pRowIdStart;

	while ( m_bHaveFilterDocs && m_bHaveLookupDocs && ( pRowID < pRowIdMax ) )
	{
		if ( m_tFilterDocID < m_tLookupDocID )
		{
			m_tFilterReader.HintDocID ( m_tLookupDocID );
			m_bHaveFilterDocs = m_tFilterReader.ReadDocID ( m_tFilterDocID );
		}
		else if ( m_tFilterDocID > m_tLookupDocID )
		{
			m_tLookupReader.HintDocID ( m_tFilterDocID );
			m_bHaveLookupDocs = m_tLookupReader.Read ( m_tLookupDocID, m_tLookupRowID );
		}
		else
		{
			// lookup reader can have duplicates; filter reader can't have duplicates
			// advance only the lookup reader
			*pRowID++ = m_tLookupRowID;
			m_bHaveLookupDocs = m_tLookupReader.Read ( m_tLookupDocID, m_tLookupRowID );
		}

		m_iProcessed++;
	}

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}


int64_t	RowidIterator_LookupValues_c::GetNumProcessed() const
{
	return m_iProcessed;
}

//////////////////////////////////////////////////////////////////////////

template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
class RowidIterator_LookupRange_T : public SecondaryIndexIterator_c
{
public:
						RowidIterator_LookupRange_T ( DocID_t tMinValue, DocID_t tMaxValue, const BYTE * pDocidLookup );

	bool				HintRowID ( RowID_t tRowID ) override;
	bool				GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock ) override;
	int64_t				GetNumProcessed() const override;

protected:
	int64_t				m_iProcessed {0};
	LookupReaderIterator_c m_tLookupReader;
	DocID_t				m_tMinValue {0};
	DocID_t				m_tMaxValue {0};
};


template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::RowidIterator_LookupRange_T ( DocID_t tMinValue, DocID_t tMaxValue, const BYTE * pDocidLookup )
	: m_tLookupReader ( pDocidLookup )
	, m_tMinValue ( tMinValue )
	, m_tMaxValue ( tMaxValue )
{
	if_const ( !OPEN_LEFT )
		m_tLookupReader.HintDocID ( tMinValue );
}

template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
bool RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::HintRowID ( RowID_t tRowID )
{
	// can't rewind lookup reader
	return true;
}

template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
bool RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	DocID_t tLookupDocID = 0;
	RowID_t tLookupRowID = INVALID_ROWID;

	RowID_t * pRowIdStart = m_dCollected.Begin();
	RowID_t * pRowIdMax = pRowIdStart + m_dCollected.GetLength()-1;
	RowID_t * pRowID = pRowIdStart;

	while ( pRowID<pRowIdMax && m_tLookupReader.Read ( tLookupDocID, tLookupRowID ) )
	{
		m_iProcessed++;

		if ( !OPEN_LEFT && ( tLookupDocID < m_tMinValue || ( !HAS_EQUAL_MIN && tLookupDocID==m_tMinValue ) ) )
			continue;

		if ( !OPEN_RIGHT && ( tLookupDocID > m_tMaxValue || ( !HAS_EQUAL_MAX && tLookupDocID==m_tMaxValue ) ) )
			return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );

		*pRowID++ = tLookupRowID;
	}

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}


template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
int64_t RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::GetNumProcessed() const
{
	return m_iProcessed;
}


//////////////////////////////////////////////////////////////////////////

template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
class RowidIterator_LookupRangeExclude_T : public RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>
{
	using BASE = RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>;

public:
			RowidIterator_LookupRangeExclude_T ( DocID_t tMinValue, DocID_t tMaxValue, const BYTE * pDocidLookup );

	bool	GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock ) override;

private:
	bool	m_bLeft {true};
};


template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
RowidIterator_LookupRangeExclude_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::RowidIterator_LookupRangeExclude_T ( DocID_t tMinValue, DocID_t tMaxValue, const BYTE * pDocidLookup )
	: RowidIterator_LookupRange_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT> ( tMinValue, tMaxValue, pDocidLookup )
{
	if ( OPEN_LEFT && !OPEN_RIGHT )
		this->m_tLookupReader.HintDocID ( this->m_tMaxValue );

	m_bLeft = OPEN_RIGHT || ( !OPEN_LEFT && !OPEN_RIGHT ); 
}


template <bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT>
bool RowidIterator_LookupRangeExclude_T<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT>::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	if_const ( OPEN_LEFT && OPEN_RIGHT )
		return false;

	DocID_t tLookupDocID = 0;
	RowID_t tLookupRowID = INVALID_ROWID;

	RowID_t * pRowIdStart = BASE::m_dCollected.Begin();
	RowID_t * pRowIdMax = pRowIdStart + BASE::m_dCollected.GetLength()-1;
	RowID_t * pRowID = pRowIdStart;

	while ( pRowID<pRowIdMax && BASE::m_tLookupReader.Read ( tLookupDocID, tLookupRowID ) )
	{
		BASE::m_iProcessed++;

		if ( m_bLeft )
		{
			// use everything ending with m_tMinValue
			if ( tLookupDocID > BASE::m_tMinValue || ( HAS_EQUAL_MIN && tLookupDocID==BASE::m_tMinValue ) )
			{
				// switch to right interval
				if_const ( !OPEN_LEFT && !OPEN_RIGHT )
				{
					m_bLeft = false;
					this->m_tLookupReader.HintDocID ( this->m_tMaxValue );
					continue;
				}

				return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
			}
		}
		else
		{
			// use everything starting from m_tMaxValue
			if ( tLookupDocID < BASE::m_tMaxValue || ( HAS_EQUAL_MAX && tLookupDocID==BASE::m_tMaxValue ) )
				continue;
		}

		*pRowID++ = tLookupRowID;
	}

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}

//////////////////////////////////////////////////////////////////////////

template <typename T>
class RowidIterator_Intersect_T : public SecondaryIndexIterator_c
{
public:
				RowidIterator_Intersect_T ( T ** ppIterators, int iNumIterators );
				~RowidIterator_Intersect_T() override;

	bool		HintRowID ( RowID_t tRowID ) override;
	bool		GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock ) override;
	int64_t		GetNumProcessed() const override;

private:
	struct IteratorState_t
	{
		T *					m_pIterator = nullptr;
		RowID_t				m_tMinRowID = INVALID_ROWID;

		const RowID_t *		m_pRowID = nullptr;
		const RowID_t *		m_pRowIDMax = nullptr;

		FORCE_INLINE bool	RewindTo  ( RowID_t tRowID );
		FORCE_INLINE bool	WarmupDocs();
		FORCE_INLINE bool	WarmupDocs ( RowID_t tRowID );
	};

	CSphFixedVector<IteratorState_t>	m_dIterators {0};

	FORCE_INLINE bool	AdvanceIterators();
	FORCE_INLINE bool	Advance ( int iIterator, RowID_t tRowID );
};

template <typename T>
bool RowidIterator_Intersect_T<T>::IteratorState_t::WarmupDocs()
{
	assert(m_pIterator);

	RowIdBlock_t dRowIdBlock;
	if ( !m_pIterator->GetNextRowIdBlock(dRowIdBlock) )
	{
		m_pRowID = m_pRowIDMax = nullptr;
		return false;
	}

	m_pRowID = dRowIdBlock.Begin();
	m_pRowIDMax = m_pRowID+dRowIdBlock.GetLength();

	return true;
}

#if USE_COLUMNAR
template <>
bool RowidIterator_Intersect_T<columnar::BlockIterator_i>::IteratorState_t::WarmupDocs()
{
	assert(m_pIterator);

	columnar::Span_T<uint32_t> dRowIdBlock;
	if ( !m_pIterator->GetNextRowIdBlock(dRowIdBlock) )
	{
		m_pRowID = m_pRowIDMax = nullptr;
		return false;
	}

	m_pRowID = (const RowID_t *)dRowIdBlock.begin();
	m_pRowIDMax = (const RowID_t *)dRowIdBlock.end();

	return true;
}
#endif

template <typename T>
bool RowidIterator_Intersect_T<T>::IteratorState_t::WarmupDocs ( RowID_t tRowID )
{
	if ( !m_pIterator->HintRowID(tRowID) )
		return false;

	return WarmupDocs();
}

template <typename T>
bool RowidIterator_Intersect_T<T>::IteratorState_t::RewindTo ( RowID_t tRowID )
{
	if ( m_pRowID>=m_pRowIDMax || tRowID>*(m_pRowIDMax-1) )
	{
		if ( !WarmupDocs(tRowID) )
			return false;
	}

	const RowID_t * pRowID = m_pRowID;

	while ( true )
	{
		while ( pRowID < m_pRowIDMax && *pRowID < tRowID )
			pRowID++;

		if ( pRowID<m_pRowIDMax )
			break;

		if ( !WarmupDocs() )
			return false;

		pRowID = m_pRowID;
	}

	m_pRowID = pRowID;

	return true;
}

template<typename T>
RowidIterator_Intersect_T<T>::RowidIterator_Intersect_T ( T ** ppIterators, int iNumIterators )
	: m_dIterators(iNumIterators)
{
	ARRAY_FOREACH ( i, m_dIterators )
	{
		auto & tIterator = m_dIterators[i];
		tIterator.m_pIterator = ppIterators[i];
	}

	m_dIterators[0].WarmupDocs();
}

template<typename T>
RowidIterator_Intersect_T<T>::~RowidIterator_Intersect_T()
{
	for ( auto & i : m_dIterators )
		SafeDelete ( i.m_pIterator );
}

template<typename T>
bool RowidIterator_Intersect_T<T>::HintRowID ( RowID_t tRowID )
{
	return m_dIterators[0].RewindTo(tRowID);
}

template<typename T>
bool RowidIterator_Intersect_T<T>::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	RowID_t * pRowIdStart = m_dCollected.Begin();
	RowID_t * pRowIdMax = pRowIdStart + m_dCollected.GetLength()-1;
	RowID_t * pRowID = pRowIdStart;

	IteratorState_t & tFirst = m_dIterators[0];

	// we assume that iterators are sorted from most selective to least selective
	while ( pRowID<pRowIdMax )
	{
		if ( !tFirst.m_pRowID )
			break;

		if ( !AdvanceIterators() )
		{
			tFirst.m_pRowID = nullptr;
			break;
		}

		*pRowID++ = *tFirst.m_pRowID;

		tFirst.m_pRowID++;
		if ( tFirst.m_pRowID>=tFirst.m_pRowIDMax && !tFirst.WarmupDocs() )
		{
			tFirst.m_pRowID = nullptr;
			break;
		}
	}

	return ReturnIteratorResult ( pRowID, pRowIdStart, dRowIdBlock );
}

template<typename T>
bool RowidIterator_Intersect_T<T>::AdvanceIterators()
{
	IteratorState_t & tFirst = m_dIterators[0];
	RowID_t tMaxRowID = *tFirst.m_pRowID;
	for ( int i=1; i < m_dIterators.GetLength(); i++ )
	{
		auto & tState = m_dIterators[i];
		if ( !tState.m_pRowID && !tState.WarmupDocs(tMaxRowID) )
			return false;

		if ( *tState.m_pRowID==tMaxRowID )
			continue;

		if ( !tState.RewindTo(tMaxRowID) )
			return false;

		if ( *tState.m_pRowID>tMaxRowID )
		{
			if ( !tFirst.RewindTo ( *tState.m_pRowID ) )
				return false;

			tMaxRowID = *tFirst.m_pRowID;
			i = 0;
		}
	}

	return true;
}

template<typename T>
int64_t RowidIterator_Intersect_T<T>::GetNumProcessed() const
{
	int64_t iTotal = 0;
	for ( auto i : m_dIterators )
		iTotal += i.m_pIterator->GetNumProcessed();

	return iTotal;
}

#if USE_COLUMNAR
class RowidIterator_Wrapper_c : public RowidIterator_i
{
public:
			RowidIterator_Wrapper_c ( columnar::BlockIterator_i * pIterator ) : m_pIterator ( pIterator ) {}

	bool	HintRowID ( RowID_t tRowID ) { return m_pIterator->HintRowID(tRowID); }
	bool	GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock );
	int64_t	GetNumProcessed() const { return m_pIterator->GetNumProcessed(); }

private:
	CSphScopedPtr<columnar::BlockIterator_i> m_pIterator;
};

bool RowidIterator_Wrapper_c::GetNextRowIdBlock ( RowIdBlock_t & dRowIdBlock )
{
	columnar::Span_T<uint32_t> dSpan;
	if ( !m_pIterator->GetNextRowIdBlock(dSpan) )
		return false;

	dRowIdBlock = { (RowID_t *)dSpan.begin(), (int64_t)dSpan.size() };
	return true;
}
#endif

//////////////////////////////////////////////////////////////////////////

#define CREATE_ITERATOR_WITH_OPEN(ITERATOR,SETTINGS,MIN,MAX,LOOKUP) \
{ \
	if ( SETTINGS.m_bOpenLeft ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new ITERATOR<true,true,true,false>(MIN,MAX,LOOKUP); \
		else \
			return new ITERATOR<true,false,true,false>(MIN,MAX,LOOKUP); \
	} else if ( SETTINGS.m_bOpenRight ) \
	{ \
		if ( SETTINGS.m_bHasEqualMin ) \
			return new ITERATOR<true,true,false,true>(MIN,MAX,LOOKUP); \
		else \
			return new ITERATOR<false,true,false,true>(MIN,MAX,LOOKUP); \
	} \
	assert ( !SETTINGS.m_bOpenLeft && !SETTINGS.m_bOpenRight ); \
	if ( SETTINGS.m_bHasEqualMin ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new ITERATOR<true,true,false,false>(MIN,MAX,LOOKUP); \
		else \
			return new ITERATOR<true,false,false,false>(MIN,MAX,LOOKUP); \
	} else \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new ITERATOR<false,true,false,false>(MIN,MAX,LOOKUP); \
		else \
			return new ITERATOR<false,false,false,false>(MIN,MAX,LOOKUP); \
	} \
}


static RowidIterator_i * CreateIterator ( const CSphFilterSettings & tFilter, const BYTE * pDocidLookup )
{
	if ( !HaveIndex ( tFilter.m_sAttrName ) )
		return nullptr;

	switch ( tFilter.m_eType )
	{
	case SPH_FILTER_VALUES:
		return new RowidIterator_LookupValues_c ( tFilter.GetValueArray(), tFilter.GetNumValues(), pDocidLookup );

	case SPH_FILTER_RANGE:
		if ( tFilter.m_bExclude )
		{
			CREATE_ITERATOR_WITH_OPEN ( RowidIterator_LookupRangeExclude_T, tFilter, tFilter.m_iMinValue, tFilter.m_iMaxValue, pDocidLookup );
		}
		else
		{
			CREATE_ITERATOR_WITH_OPEN ( RowidIterator_LookupRange_T, tFilter, tFilter.m_iMinValue, tFilter.m_iMaxValue, pDocidLookup );
		}
		break;
		
	default:
		break;
	}

	return nullptr;
}


struct IndexWithEstimate_t : public SecondaryIndexInfo_t
{
	bool	m_bEnabled {false};
	bool	m_bForce {false};
	int64_t	m_iRsetEstimate {0};
};


bool NextSet ( CSphBitvec & dSet, const CSphVector<IndexWithEstimate_t> & dSecondaryIndexes )
{
	for ( int i = 0; i < dSet.GetBits(); i++ )
	{
		if ( dSecondaryIndexes[i].m_bForce )
			continue;

		bool bState = dSet.BitGet(i);
		if ( bState )
			dSet.BitClear(i);
		else
		{
			dSet.BitSet(i);
			return true;
		}
	}

	return false;
}


class CostEstimate_c
{
public:
	float LookupRead ( int64_t iDocs ) const
	{
		return 10.0f*float(iDocs)*SCALE;
	}

	float Filter ( int64_t iDocs, int nFilters ) const
	{
		return 5.0f*nFilters*float(iDocs)*SCALE;
	}

	float Fullscan ( int64_t iDocs ) const
	{
		return 5.0f*float(iDocs)*SCALE;
	}

	float IndexIntersect ( int64_t iDocs ) const
	{
		return 8.0f*float(iDocs)*SCALE;
	}

private:
	static constexpr float SCALE = 1.0f/1000000.0f;
};


float CalcQueryCost ( const CSphVector<CSphFilterSettings> & dFilters, const CSphVector<IndexWithEstimate_t> & dSecondaryIndexes, int64_t iTotalDocs )
{
	float fCost = 0.0f;
	int64_t iDocsProcessedByIndexes = 0;
	float fTotalIndexProbability = 1.0f;

	int nEnabled = 0;
	for ( const auto & tIndex : dSecondaryIndexes )
	{
		if ( !tIndex.m_bEnabled )
			continue;

		nEnabled++;
		iDocsProcessedByIndexes += tIndex.m_iRsetEstimate;

		float fIndexProbability = float(tIndex.m_iRsetEstimate) / iTotalDocs;
		fTotalIndexProbability *= fIndexProbability;
	}


	CostEstimate_c tCost;

	if ( !nEnabled )
	{
		fCost += tCost.Fullscan ( iTotalDocs );
		fCost += tCost.Filter ( iTotalDocs, dFilters.GetLength() );
	}
	else
	{
		fCost += tCost.LookupRead ( iDocsProcessedByIndexes );

		if ( nEnabled>1 )
		{
			fCost += tCost.IndexIntersect ( iDocsProcessedByIndexes );
			fCost += tCost.Filter ( uint64_t(fTotalIndexProbability*iTotalDocs), dFilters.GetLength()-nEnabled );
		}
		else
			fCost += tCost.Filter ( iDocsProcessedByIndexes, dFilters.GetLength()-nEnabled );
	}

	return fCost;
}


void SelectIterators ( const CSphVector<CSphFilterSettings> & dFilters, const CSphVector<IndexHint_t> & dHints, CSphVector<SecondaryIndexInfo_t> & dEnabledIndexes, const HistogramContainer_c & tHistograms )
{
	dEnabledIndexes.Resize(0);

	bool bHaveUseHint = false;
	ARRAY_FOREACH_COND ( i, dHints, !bHaveUseHint )
		bHaveUseHint = dHints[i].m_eHint==INDEX_HINT_USE;

	CSphVector<IndexWithEstimate_t> dSecondaryIndexes;
	ARRAY_FOREACH ( i, dFilters )
	{
		const CSphFilterSettings & tFilter = dFilters[i];

		if ( !HaveIndex ( tFilter.m_sAttrName.cstr() ) )
			continue;

		const Histogram_i * pHistogram = tHistograms.Get ( tFilter.m_sAttrName );
		if ( !pHistogram )
			continue;

		IndexHint_e * pHint = nullptr;
		ARRAY_FOREACH_COND ( i, dHints, !pHint )
			if ( dHints[i].m_sIndex==tFilter.m_sAttrName )
				pHint = &dHints[i].m_eHint;

		if ( pHint && *pHint==INDEX_HINT_IGNORE )
			continue;

		if ( bHaveUseHint && !pHint )
			continue;

		IndexWithEstimate_t tNewIndex;

		bool bUsable = pHistogram->EstimateRsetSize ( tFilter, tNewIndex.m_iRsetEstimate );
		if ( bUsable )
		{
			tNewIndex.m_iFilterId = i;
			tNewIndex.m_bForce = pHint && *pHint==INDEX_HINT_FORCE;

			dSecondaryIndexes.Add ( tNewIndex );
		}
	}

	int nAvailableIndexes = dSecondaryIndexes.GetLength();
	if ( !nAvailableIndexes )
		return;

	CSphBitvec dEnabled ( nAvailableIndexes );
	CSphBitvec dBestEnabled ( nAvailableIndexes );

	for ( int i = 0; i < dEnabled.GetBits(); i++ )
		if ( dSecondaryIndexes[i].m_bForce )
			dEnabled.BitSet(i);

	float fBestCost = FLT_MAX;

	const int MAX_TRIES = 1024;
	for ( int iTry = 0; iTry < MAX_TRIES; iTry++ )
	{
		for ( int i = 0; i < dEnabled.GetBits(); i++ )
			dSecondaryIndexes[i].m_bEnabled = dEnabled.BitGet(i);

		float fCost = CalcQueryCost ( dFilters, dSecondaryIndexes, tHistograms.GetNumValues() );
		if ( fCost < fBestCost )
		{
			dBestEnabled = dEnabled;
			fBestCost = fCost;
		}

		if ( !NextSet ( dEnabled, dSecondaryIndexes ) )
			break;
	}

	for ( int i = 0; i < dBestEnabled.GetBits(); i++ )
		if ( dBestEnabled.BitGet(i) )
		{
			SecondaryIndexInfo_t & tInfo = dEnabledIndexes.Add();
			tInfo.m_iFilterId = dSecondaryIndexes[i].m_iFilterId;
		}
}


RowidIterator_i * CreateFilteredIterator ( const CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphFilterSettings> & dModifiedFilters, bool & bFiltersChanged, const CSphVector<FilterTreeItem_t> & dFilterTree,
	const CSphVector<IndexHint_t> & dHints, const HistogramContainer_c & tHistograms, const BYTE * pDocidLookup )
{
	bFiltersChanged = false;

	// no iterators with OR queries
	if ( dFilterTree.GetLength() )
		return nullptr;

	CSphVector<SecondaryIndexInfo_t> dEnabledIndexes;
	SelectIterators ( dFilters, dHints, dEnabledIndexes, tHistograms );

	CSphVector<RowidIterator_i *> dIterators;
	for ( auto & i : dEnabledIndexes )
	{
		RowidIterator_i * pIterator = CreateIterator ( dFilters[i.m_iFilterId], pDocidLookup );
		if ( pIterator )
			dIterators.Add ( pIterator );
	}

	if ( !dIterators.GetLength() )
		return nullptr;

	dEnabledIndexes.Sort ( bind ( &SecondaryIndexInfo_t::m_iFilterId ) );
	ARRAY_FOREACH ( i, dFilters )
	{
		if ( !dEnabledIndexes.any_of ( [i] ( const SecondaryIndexInfo_t & tInfo ) { return tInfo.m_iFilterId==i; } ) )
			dModifiedFilters.Add ( dFilters[i] );
	}

	bFiltersChanged = dFilters.GetLength()!=dModifiedFilters.GetLength();

	if ( dIterators.GetLength()==1 )
		return dIterators[0];

	return new RowidIterator_Intersect_T<RowidIterator_i> ( dIterators.Begin(), dIterators.GetLength() );
}


RowidIterator_i * CreateIteratorIntersect ( CSphVector<RowidIterator_i*> & dIterators )
{
	return new RowidIterator_Intersect_T<RowidIterator_i> ( dIterators.Begin(), dIterators.GetLength() );
}

#if USE_COLUMNAR
RowidIterator_i * CreateIteratorWrapper ( columnar::BlockIterator_i * pIterator )
{
	return new RowidIterator_Wrapper_c(pIterator);
}


RowidIterator_i * CreateIteratorIntersect ( std::vector<columnar::BlockIterator_i *> & dIterators )
{
	return new RowidIterator_Intersect_T<columnar::BlockIterator_i> ( &dIterators[0], (int)dIterators.size() );
}
#endif


//////////////////////////////////////////////////////////////////////////

DocidLookupWriter_c::DocidLookupWriter_c ( DWORD nDocs )
	: m_nDocs ( nDocs )
{}


DocidLookupWriter_c::~DocidLookupWriter_c()
{
	SafeDelete ( m_pWriter );
}


bool DocidLookupWriter_c::Open ( const CSphString & sFilename, CSphString & sError )
{
	assert ( !m_pWriter );
	m_pWriter = new CSphWriter;

	if ( !m_pWriter->OpenFile ( sFilename, sError ) )
		return false;

	m_pWriter->PutDword ( m_nDocs );
	m_pWriter->PutDword ( DOCS_PER_LOOKUP_CHECKPOINT );

	m_tCheckpointStart = m_pWriter->GetPos();
	m_pWriter->PutOffset ( 0 );	// reserve space for max docid

	int nCheckpoints = (m_nDocs+DOCS_PER_LOOKUP_CHECKPOINT-1)/DOCS_PER_LOOKUP_CHECKPOINT;
	m_dCheckpoints.Reset ( nCheckpoints );
	for ( int i = 0; i < nCheckpoints; i++ )
	{
		// reserve space for checkpoints
		m_pWriter->PutOffset(0);
		m_pWriter->PutOffset(0);
	}

	return true;
}


void DocidLookupWriter_c::AddPair ( const DocidRowidPair_t & tPair )
{
	assert ( tPair.m_tDocID>=m_tLastDocID );

	if ( !(m_iProcessed % DOCS_PER_LOOKUP_CHECKPOINT) )
	{
		m_dCheckpoints[m_iCheckpoint].m_tBaseDocID = tPair.m_tDocID;
		m_dCheckpoints[m_iCheckpoint].m_tOffset = m_pWriter->GetPos();
		m_iCheckpoint++;

		// no need to store docid for 1st entry
		m_pWriter->PutDword ( tPair.m_tRowID );
	}
	else
	{
		m_pWriter->ZipOffset ( tPair.m_tDocID-m_tLastDocID );
		m_pWriter->PutDword ( tPair.m_tRowID );
	}

	m_tLastDocID = tPair.m_tDocID;
	m_iProcessed++;
}


bool DocidLookupWriter_c::Finalize ( CSphString & sError )
{
	m_pWriter->Flush();
	m_pWriter->SeekTo ( m_tCheckpointStart );
	m_pWriter->PutOffset ( m_tLastDocID );
	for ( const auto & i : m_dCheckpoints )
	{
		m_pWriter->PutOffset ( i.m_tBaseDocID );
		m_pWriter->PutOffset ( i.m_tOffset );
	}

	m_pWriter->CloseFile();
	if ( m_pWriter->IsError() )
	{
		sError = "error writing .SPT";
		return false;
	}

	return true;
}


CSphWriter & DocidLookupWriter_c::GetWriter()
{
	assert ( m_pWriter );
	return *m_pWriter;
}


bool WriteDocidLookup ( const CSphString & sFilename, const CSphFixedVector<DocidRowidPair_t> & dLookup, CSphString & sError )
{
	DocidLookupWriter_c tWriter ( dLookup.GetLength() );
	if ( !tWriter.Open ( sFilename, sError ) )
		return false;

	for ( const auto & i : dLookup )
		tWriter.AddPair(i);

	if ( !tWriter.Finalize ( sError ) )
		return false;

	return true;
}

//////////////////////////////////////////////////////////////////////////

LookupReader_c::LookupReader_c ( const BYTE * pData )
{
	SetData ( pData );
}


void LookupReader_c::SetData ( const BYTE * pData )
{
	m_pData = pData;

	if ( !pData )
		return;

	const BYTE * p = pData;
	m_nDocs = *(DWORD*)p;
	p += sizeof(DWORD);
	m_nDocsPerCheckpoint = *(DWORD*)p;
	p += sizeof(DWORD);
	m_tMaxDocID = *(DocID_t*)p;
	p += sizeof(DocID_t);

	m_nCheckpoints = (m_nDocs+m_nDocsPerCheckpoint-1)/m_nDocsPerCheckpoint;
	m_pCheckpoints = (DocidLookupCheckpoint_t *)p;
}

//////////////////////////////////////////////////////////////////////////

LookupReaderIterator_c::LookupReaderIterator_c ( const BYTE * pData )
{
	SetData(pData);
}


void LookupReaderIterator_c::SetData ( const BYTE * pData )
{
	LookupReader_c::SetData(pData);
	SetCheckpoint ( m_pCheckpoints );
}
