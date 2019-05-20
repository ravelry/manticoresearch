//
// Copyright (c) 2017-2019, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxint.h"
#include "sphinxrt.h"
#include "sphinxpq.h"
#include "sphinxsearch.h"
#include "sphinxutils.h"
#include "sphinxjson.h"
#include "sphinxplugin.h"
#include "sphinxrlp.h"
#include "sphinxqcache.h"
#include "attribute.h"
#include "killlist.h"
#include "secondaryindex.h"

#include <sys/stat.h>
#include <fcntl.h>

#if USE_WINDOWS
#include <errno.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

//////////////////////////////////////////////////////////////////////////

#define BINLOG_WRITE_BUFFER		256*1024
#define BINLOG_AUTO_FLUSH		1000000

#define RTDICT_CHECKPOINT_V5			48
#define SPH_RT_DOUBLE_BUFFER_PERCENT	10

//////////////////////////////////////////////////////////////////////////

#ifndef NDEBUG
#define Verify(_expr) assert(_expr)
#else
#define Verify(_expr) _expr
#endif

//////////////////////////////////////////////////////////////////////////
// GLOBALS
//////////////////////////////////////////////////////////////////////////

/// publicly exposed binlog interface
ISphBinlog *			g_pBinlog				= NULL;

/// actual binlog implementation
class RtBinlog_c;
static RtBinlog_c *		g_pRtBinlog				= NULL;

/// protection from concurrent changes during binlog replay
static auto&	g_bRTChangesAllowed		= RTChangesAllowed ();

// optimize mode for disk chunks merge
static bool g_bProgressiveMerge = true;
static auto& g_bShutdown = sphGetShutdown();

//////////////////////////////////////////////////////////////////////////
volatile bool &RTChangesAllowed ()
{
	static bool bRTChangesAllowed = false;
	return bRTChangesAllowed;
}

// !COMMIT yes i am when debugging
#ifndef NDEBUG
#define PARANOID 1
#endif

//////////////////////////////////////////////////////////////////////////

// Variable Length Byte (VLB) encoding
// store int variable in as much bytes as actually needed to represent it
template < typename T, typename P >
static inline void ZipT ( CSphVector < BYTE, P > * pOut, T uValue )
{
	do
	{
		BYTE bOut = (BYTE)( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		pOut->Add ( bOut );
	} while ( uValue );
}

template < typename T >
static inline void ZipT ( BYTE * & pOut, T uValue )
{
	do
	{
		BYTE bOut = (BYTE)( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		*pOut++ = bOut;
	} while ( uValue );
}

#define SPH_MAX_KEYWORD_LEN (3*SPH_MAX_WORD_LEN+4)
STATIC_ASSERT ( SPH_MAX_KEYWORD_LEN<255, MAX_KEYWORD_LEN_SHOULD_FITS_BYTE );


// Variable Length Byte (VLB) decoding
template < typename T >
static inline const BYTE * UnzipT ( T * pValue, const BYTE * pIn )
{
	T uValue = 0;
	BYTE bIn;
	int iOff = 0;

	do
	{
		bIn = *pIn++;
		uValue += ( T ( bIn & 0x7f ) ) << iOff;
		iOff += 7;
	} while ( bIn & 0x80 );

	*pValue = uValue;
	return pIn;
}

#define ZipDword ZipT<DWORD>
#define ZipQword ZipT<uint64_t>
#define UnzipDword UnzipT<DWORD>
#define UnzipQword UnzipT<uint64_t>

#define ZipDocid ZipQword
#define ZipWordid ZipQword
#define UnzipWordid UnzipQword

//////////////////////////////////////////////////////////////////////////

struct CmpHitPlain_fn
{
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		return 	( a.m_uWordID<b.m_uWordID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_tRowID<b.m_tRowID ) ||
			( a.m_uWordID==b.m_uWordID && a.m_tRowID==b.m_tRowID && a.m_uWordPos<b.m_uWordPos );
	}
};


struct CmpHitKeywords_fn
{
	const BYTE * m_pBase;
	explicit CmpHitKeywords_fn ( const BYTE * pBase ) : m_pBase ( pBase ) {}
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		const BYTE * pPackedA = m_pBase + a.m_uWordID;
		const BYTE * pPackedB = m_pBase + b.m_uWordID;
		int iCmp = sphDictCmpStrictly ( (const char *)pPackedA+1, *pPackedA, (const char *)pPackedB+1, *pPackedB );
		return 	( iCmp<0 ) ||
			( iCmp==0 && a.m_tRowID<b.m_tRowID ) ||
			( iCmp==0 && a.m_tRowID==b.m_tRowID && a.m_uWordPos<b.m_uWordPos );
	}
};


RtSegment_t::RtSegment_t ( DWORD uDocs )
	: m_tDeadRowMap ( uDocs )
{
	m_iTag = m_iSegments.Inc();
}


int64_t RtSegment_t::GetUsedRam () const
{
	// FIXME! gonna break on vectors over 2GB
	return
		(int64_t)m_dWords.AllocatedBytes() +
		(int64_t)m_dDocs.AllocatedBytes() +
		(int64_t)m_dHits.AllocatedBytes() +
		(int64_t)m_dBlobs.AllocatedBytes() +
		(int64_t)m_dKeywordCheckpoints.AllocatedBytes() +
		(int64_t)m_dRows.AllocatedBytes() +
		(int64_t)m_dInfixFilterCP.AllocatedBytes();
}

DWORD RtSegment_t::GetMergeFactor() const
{
	return m_uRows;
}

int RtSegment_t::GetStride () const
{
	return ( m_dRows.GetLength() / m_uRows );
}

CSphAtomic RtSegment_t::m_iSegments { 0 };
const CSphRowitem * RtSegment_t::FindRow ( DocID_t tDocID ) const
{
	RowID_t * pRowID = m_tDocIDtoRowID.Find(tDocID);
	return pRowID ? GetDocinfoByRowID ( *pRowID ) : nullptr;
}


const CSphRowitem * RtSegment_t::FindAliveRow ( DocID_t tDocid ) const
{
	RowID_t tRowID = GetRowidByDocid(tDocid);
	if ( tRowID==INVALID_ROWID || m_tDeadRowMap.IsSet(tRowID) )
		return nullptr;

	return GetDocinfoByRowID(tRowID);
}


const CSphRowitem * RtSegment_t::GetDocinfoByRowID ( RowID_t tRowID ) const
{
	return &m_dRows[tRowID*GetStride()];
}


RowID_t RtSegment_t::GetRowidByDocid ( DocID_t tDocID ) const
{
	RowID_t * pRowID = m_tDocIDtoRowID.Find(tDocID);
	return pRowID ? *pRowID : INVALID_ROWID;
}


int RtSegment_t::Kill ( DocID_t tDocID )
{
	if ( m_tDeadRowMap.Set ( GetRowidByDocid ( tDocID ) ) )
	{
		assert ( m_tAliveRows>0 );
		m_tAliveRows.Dec();
		return 1;
	}

	return 0;
}


int	RtSegment_t::KillMulti ( const DocID_t * pKlist, int iKlistSize )
{
	int iTotalKilled = 0;

	// fixme: implement more efficient batch killer
	for ( int i = 0; i < iKlistSize; i++ )
		iTotalKilled += Kill ( pKlist[i] );

	return iTotalKilled;
}


void RtSegment_t::BuildDocID2RowIDMap()
{
	m_tDocIDtoRowID.Reset(m_uRows);

	int iStride = GetStride();
	RowID_t tRowID = 0;
	for ( int i=0; i<m_dRows.GetLength(); i+=iStride )
		m_tDocIDtoRowID.Add ( sphGetDocID ( &m_dRows[i] ), tRowID++ );
}


//////////////////////////////////////////////////////////////////////////

struct RtDocWriter_t
{
	CSphTightVector<BYTE> *		m_pDocs;
	RowID_t						m_tLastRowID {INVALID_ROWID};

	explicit RtDocWriter_t ( RtSegment_t * pSeg )
		: m_pDocs ( &pSeg->m_dDocs )
	{}

	void ZipDoc ( const RtDoc_t & tDoc )
	{
		CSphTightVector<BYTE> * pDocs = m_pDocs;
		BYTE * pEnd = pDocs->AddN ( 12*sizeof(DWORD) );
		const BYTE * pBegin = pDocs->Begin();

		ZipDword ( pEnd, tDoc.m_tRowID - m_tLastRowID );
		m_tLastRowID = tDoc.m_tRowID;
		ZipDword ( pEnd, tDoc.m_uDocFields );
		ZipDword ( pEnd, tDoc.m_uHits );
		if ( tDoc.m_uHits==1 )
		{
			ZipDword ( pEnd, tDoc.m_uHit & 0xffffffUL );
			ZipDword ( pEnd, tDoc.m_uHit>>24 );
		} else
			ZipDword ( pEnd, tDoc.m_uHit );

		pDocs->Resize ( pEnd-pBegin );
	}

	DWORD ZipDocPtr () const
	{
		return m_pDocs->GetLength();
	}

	void ZipRestart ()
	{
		m_tLastRowID = INVALID_ROWID;
	}
};


RtDocReader_t::RtDocReader_t ( const RtSegment_t * pSeg, const RtWord_t & tWord )
{
	m_pDocs = ( pSeg->m_dDocs.Begin() ? pSeg->m_dDocs.Begin() + tWord.m_uDoc : NULL );
	m_iLeft = tWord.m_uDocs;
	m_tDoc.m_tRowID = INVALID_ROWID;
}

const RtDoc_t * RtDocReader_t::UnzipDoc ()
{
	if ( !m_iLeft || !m_pDocs )
		return NULL;

	const BYTE * pIn = m_pDocs;
	RowID_t tDeltaID;
	pIn = UnzipDword ( &tDeltaID, pIn );
	m_tDoc.m_tRowID += tDeltaID;
	pIn = UnzipDword ( &m_tDoc.m_uDocFields, pIn );
	pIn = UnzipDword ( &m_tDoc.m_uHits, pIn );
	if ( m_tDoc.m_uHits==1 )
	{
		DWORD a, b;
		pIn = UnzipDword ( &a, pIn );
		pIn = UnzipDword ( &b, pIn );
		m_tDoc.m_uHit = a + ( b<<24 );
	} else
		pIn = UnzipDword ( &m_tDoc.m_uHit, pIn );
	m_pDocs = pIn;

	m_iLeft--;
	return &m_tDoc;
}


struct RtWordWriter_t
{
	CSphTightVector<BYTE> *				m_pWords;
	CSphVector<RtWordCheckpoint_t> *	m_pCheckpoints;
	CSphVector<BYTE> *					m_pKeywordCheckpoints;

	CSphKeywordDeltaWriter				m_tLastKeyword;
	SphWordID_t							m_uLastWordID;
	DWORD								m_uLastDoc;
	int									m_iWords;

	bool								m_bKeywordDict;
	int									m_iWordsCheckpoint;

	RtWordWriter_t ( RtSegment_t * pSeg, bool bKeywordDict, int iWordsCheckpoint )
		: m_pWords ( &pSeg->m_dWords )
		, m_pCheckpoints ( &pSeg->m_dWordCheckpoints )
		, m_pKeywordCheckpoints ( &pSeg->m_dKeywordCheckpoints )
		, m_uLastWordID ( 0 )
		, m_uLastDoc ( 0 )
		, m_iWords ( 0 )
		, m_bKeywordDict ( bKeywordDict )
		, m_iWordsCheckpoint ( iWordsCheckpoint )
	{
		assert ( !m_pWords->GetLength() );
		assert ( !m_pCheckpoints->GetLength() );
		assert ( !m_pKeywordCheckpoints->GetLength() );
	}

	void ZipWord ( const RtWord_t & tWord )
	{
		CSphTightVector<BYTE> * pWords = m_pWords;
		if ( ++m_iWords==m_iWordsCheckpoint )
		{
			RtWordCheckpoint_t & tCheckpoint = m_pCheckpoints->Add();
			if ( !m_bKeywordDict )
				tCheckpoint.m_uWordID = tWord.m_uWordID;
			else
			{
				int iLen = tWord.m_sWord[0];
				assert ( iLen && iLen-1<SPH_MAX_KEYWORD_LEN );
				tCheckpoint.m_uWordID = sphPutBytes ( m_pKeywordCheckpoints, tWord.m_sWord+1, iLen+1 );
				m_pKeywordCheckpoints->Last() = '\0'; // checkpoint is NULL terminating string

				// reset keywords delta encoding
				m_tLastKeyword.Reset();
			}
			tCheckpoint.m_iOffset = pWords->GetLength();

			m_uLastWordID = 0;
			m_uLastDoc = 0;
			m_iWords = 1;
		}

		if ( !m_bKeywordDict )
		{
			ZipWordid ( pWords, tWord.m_uWordID - m_uLastWordID );
		} else
		{
			m_tLastKeyword.PutDelta ( *this, tWord.m_sWord+1, tWord.m_sWord[0] );
		}

		BYTE * pEnd = pWords->AddN ( 4*sizeof(DWORD) );
		const BYTE * pBegin = pWords->Begin();

		ZipDword ( pEnd, tWord.m_uDocs );
		ZipDword ( pEnd, tWord.m_uHits );
		ZipDword ( pEnd, tWord.m_uDoc - m_uLastDoc );

		pWords->Resize ( pEnd-pBegin );

		m_uLastWordID = tWord.m_uWordID;
		m_uLastDoc = tWord.m_uDoc;
	}

	void PutBytes ( const BYTE * pData, int iLen )
	{
		sphPutBytes ( m_pWords, pData, iLen );
	}
};

RtWordReader_t::RtWordReader_t ( const RtSegment_t * pSeg, bool bWordDict, int iWordsCheckpoint )
	: m_bWordDict ( bWordDict )
	, m_iWordsCheckpoint ( iWordsCheckpoint )
{
	m_tWord.m_uWordID = 0;
	Reset ( pSeg );
	if ( bWordDict )
		m_tWord.m_sWord = m_tPackedWord;
}

void RtWordReader_t::Reset ( const RtSegment_t * pSeg )
{
	m_pCur = pSeg->m_dWords.Begin();
	m_pMax = m_pCur + pSeg->m_dWords.GetLength();

	m_tWord.m_uDoc = 0;
	m_iWords = 0;
}

const RtWord_t * RtWordReader_t::UnzipWord ()
{
	if ( ++m_iWords==m_iWordsCheckpoint )
	{
		m_tWord.m_uDoc = 0;
		m_iWords = 1;
		++m_iCheckpoint;
		if ( !m_bWordDict )
			m_tWord.m_uWordID = 0;
	}
	if ( m_pCur>=m_pMax )
		return nullptr;

	const BYTE * pIn = m_pCur;
	DWORD uDeltaDoc;
	if ( m_bWordDict )
	{
		BYTE iMatch, iDelta, uPacked;
		uPacked = *pIn++;
		if ( uPacked & 0x80 )
		{
			iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
			iMatch = uPacked & 15;
		} else
		{
			iDelta = uPacked & 127;
			iMatch = *pIn++;
		}
		m_tPackedWord[0] = iMatch+iDelta;
		memcpy ( m_tPackedWord+1+iMatch, pIn, iDelta );
		m_tPackedWord[1+m_tPackedWord[0]] = 0;
		pIn += iDelta;
	} else
	{
		SphWordID_t uDeltaID;
		pIn = UnzipWordid ( &uDeltaID, pIn );
		m_tWord.m_uWordID += uDeltaID;
	}
	pIn = UnzipDword ( &m_tWord.m_uDocs, pIn );
	pIn = UnzipDword ( &m_tWord.m_uHits, pIn );
	pIn = UnzipDword ( &uDeltaDoc, pIn );
	m_pCur = pIn;

	m_tWord.m_uDoc += uDeltaDoc;
	return &m_tWord;
}


struct RtHitWriter_t
{
	CSphTightVector<BYTE> *		m_pHits;
	DWORD						m_uLastHit = 0;

	explicit RtHitWriter_t ( RtSegment_t * pSeg )
		: m_pHits ( &pSeg->m_dHits )
	{}

	void ZipHit ( DWORD uValue )
	{
		ZipDword ( m_pHits, uValue - m_uLastHit );
		m_uLastHit = uValue;
	}

	void ZipRestart ()
	{
		m_uLastHit = 0;
	}

	DWORD ZipHitPtr () const
	{
		return m_pHits->GetLength();
	}
};

RtHitReader_t::RtHitReader_t ( const RtSegment_t * pSeg, const RtDoc_t * pDoc )
{
	m_pCur = pSeg->m_dHits.Begin() + pDoc->m_uHit;
	m_iLeft = pDoc->m_uHits;
	m_uLast = 0;
}

DWORD RtHitReader_t::UnzipHit ()
{
	if ( !m_iLeft )
		return 0;

	DWORD uValue;
	m_pCur = UnzipDword ( &uValue, m_pCur );
	m_uLast += uValue;
	m_iLeft--;
	return m_uLast;
}

void RtHitReader2_t::Seek ( SphOffset_t uOff, int iHits )
{
	m_pCur = m_pBase + uOff;
	m_iLeft = iHits;
	m_uLast = 0;
}

//////////////////////////////////////////////////////////////////////////

static const int PQ_META_VERSION_MAX = 255;

uint64_t MemoryReader_c::UnzipOffset()
{
	assert ( m_pCur );
	assert ( m_pCur<m_pData+m_iLen );
	uint64_t uVal = 0;
	m_pCur = UnzipQword ( &uVal, m_pCur );
	return uVal;
}

DWORD MemoryReader_c::UnzipInt()
{
	assert ( m_pCur );
	assert ( m_pCur<m_pData+m_iLen );
	DWORD uVal = 0;
	m_pCur = UnzipDword ( &uVal, m_pCur );
	return uVal;
}

void MemoryWriter_c::ZipOffset ( uint64_t uVal )
{
	ZipQword ( &m_dBuf, uVal );
}

void MemoryWriter_c::ZipInt ( DWORD uVal )
{
	ZipDword ( &m_dBuf, uVal );
}

void LoadStoredQueryV6 ( DWORD uVersion, StoredQueryDesc_t & tQuery, CSphReader & tReader )
{
	if ( uVersion>=3 )
		tQuery.m_uQUID = tReader.GetOffset();
	if ( uVersion>=4 )
		tQuery.m_bQL = ( tReader.GetDword()!=0 );

	tQuery.m_sQuery = tReader.GetString();
	if ( uVersion==1 )
		return;

	tQuery.m_sTags = tReader.GetString();

	tQuery.m_dFilters.Resize ( tReader.GetDword() );
	tQuery.m_dFilterTree.Resize ( tReader.GetDword() );
	ARRAY_FOREACH ( iFilter, tQuery.m_dFilters )
	{
		CSphFilterSettings & tFilter = tQuery.m_dFilters[iFilter];
		tFilter.m_sAttrName = tReader.GetString();
		tFilter.m_bExclude = ( tReader.GetDword()!=0 );
		tFilter.m_bHasEqualMin = ( tReader.GetDword()!=0 );
		tFilter.m_bHasEqualMax = ( tReader.GetDword()!=0 );
		tFilter.m_eType = (ESphFilter)tReader.GetDword();
		tFilter.m_eMvaFunc = (ESphMvaFunc)tReader.GetDword ();
		tReader.GetBytes ( &tFilter.m_iMinValue, sizeof(tFilter.m_iMinValue) );
		tReader.GetBytes ( &tFilter.m_iMaxValue, sizeof(tFilter.m_iMaxValue) );
		tFilter.m_dValues.Resize ( tReader.GetDword() );
		tFilter.m_dStrings.Resize ( tReader.GetDword() );
		ARRAY_FOREACH ( j, tFilter.m_dValues )
			tReader.GetBytes ( tFilter.m_dValues.Begin() + j, sizeof ( tFilter.m_dValues[j] ) );
		ARRAY_FOREACH ( j, tFilter.m_dStrings )
			tFilter.m_dStrings[j] = tReader.GetString();
	}
	ARRAY_FOREACH ( iTree, tQuery.m_dFilterTree )
	{
		FilterTreeItem_t & tItem = tQuery.m_dFilterTree[iTree];
		tItem.m_iLeft = tReader.GetDword();
		tItem.m_iRight = tReader.GetDword();
		tItem.m_iFilterItem = tReader.GetDword();
		tItem.m_bOr = ( tReader.GetDword()!=0 );
	}
}

template<typename READER>
void LoadStoredQuery ( DWORD uVersion, StoredQueryDesc_t & tQuery, READER & tReader )
{
	assert ( uVersion>=7 );
	tQuery.m_uQUID = tReader.UnzipOffset();
	tQuery.m_bQL = ( tReader.UnzipInt()!=0 );
	tQuery.m_sQuery = tReader.GetString();
	tQuery.m_sTags = tReader.GetString();

	tQuery.m_dFilters.Resize ( tReader.UnzipInt() );
	tQuery.m_dFilterTree.Resize ( tReader.UnzipInt() );
	ARRAY_FOREACH ( iFilter, tQuery.m_dFilters )
	{
		CSphFilterSettings & tFilter = tQuery.m_dFilters[iFilter];
		tFilter.m_sAttrName = tReader.GetString();
		tFilter.m_bExclude = ( tReader.UnzipInt()!=0 );
		tFilter.m_bHasEqualMin = ( tReader.UnzipInt()!=0 );
		tFilter.m_bHasEqualMax = ( tReader.UnzipInt()!=0 );
		tFilter.m_bOpenLeft = ( tReader.UnzipInt()!=0 );
		tFilter.m_bOpenRight = ( tReader.UnzipInt()!=0 );
		tFilter.m_bIsNull = ( tReader.UnzipInt()!=0 );
		tFilter.m_eType = (ESphFilter)tReader.UnzipInt();
		tFilter.m_eMvaFunc = (ESphMvaFunc)tReader.UnzipInt ();
		tFilter.m_iMinValue = tReader.UnzipOffset();
		tFilter.m_iMaxValue = tReader.UnzipOffset();
		tFilter.m_dValues.Resize ( tReader.UnzipInt() );
		tFilter.m_dStrings.Resize ( tReader.UnzipInt() );
		ARRAY_FOREACH ( j, tFilter.m_dValues )
			tFilter.m_dValues[j] = tReader.UnzipOffset();
		ARRAY_FOREACH ( j, tFilter.m_dStrings )
			tFilter.m_dStrings[j] = tReader.GetString();
	}
	ARRAY_FOREACH ( iTree, tQuery.m_dFilterTree )
	{
		FilterTreeItem_t & tItem = tQuery.m_dFilterTree[iTree];
		tItem.m_iLeft = tReader.UnzipInt();
		tItem.m_iRight = tReader.UnzipInt();
		tItem.m_iFilterItem = tReader.UnzipInt();
		tItem.m_bOr = ( tReader.UnzipInt()!=0 );
	}
}

template<typename WRITER>
void SaveStoredQuery ( const StoredQueryDesc_t & tQuery, WRITER & tWriter )
{
	tWriter.ZipOffset ( tQuery.m_uQUID );
	tWriter.ZipInt ( tQuery.m_bQL );
	tWriter.PutString ( tQuery.m_sQuery );
	tWriter.PutString ( tQuery.m_sTags );
	tWriter.ZipInt ( tQuery.m_dFilters.GetLength() );
	tWriter.ZipInt ( tQuery.m_dFilterTree.GetLength() );
	ARRAY_FOREACH ( iFilter, tQuery.m_dFilters )
	{
		const CSphFilterSettings & tFilter = tQuery.m_dFilters[iFilter];
		tWriter.PutString ( tFilter.m_sAttrName );
		tWriter.ZipInt ( tFilter.m_bExclude );
		tWriter.ZipInt ( tFilter.m_bHasEqualMin );
		tWriter.ZipInt ( tFilter.m_bHasEqualMax );
		tWriter.ZipInt ( tFilter.m_bOpenLeft );
		tWriter.ZipInt ( tFilter.m_bOpenRight );
		tWriter.ZipInt ( tFilter.m_bIsNull );
		tWriter.ZipInt ( tFilter.m_eType );
		tWriter.ZipInt ( tFilter.m_eMvaFunc );
		tWriter.ZipOffset ( tFilter.m_iMinValue );
		tWriter.ZipOffset ( tFilter.m_iMaxValue );
		tWriter.ZipInt ( tFilter.m_dValues.GetLength() );
		tWriter.ZipInt ( tFilter.m_dStrings.GetLength() );
		ARRAY_FOREACH ( j, tFilter.m_dValues )
			tWriter.ZipOffset ( tFilter.m_dValues[j] );
		ARRAY_FOREACH ( j, tFilter.m_dStrings )
			tWriter.PutString ( tFilter.m_dStrings[j] );
	}
	ARRAY_FOREACH ( iTree, tQuery.m_dFilterTree )
	{
		const FilterTreeItem_t & tItem = tQuery.m_dFilterTree[iTree];
		tWriter.ZipInt ( tItem.m_iLeft );
		tWriter.ZipInt ( tItem.m_iRight );
		tWriter.ZipInt ( tItem.m_iFilterItem );
		tWriter.ZipInt ( tItem.m_bOr );
	}
}

void LoadStoredQuery ( const BYTE * pData, int iLen, StoredQueryDesc_t & tQuery )
{
	MemoryReader_c tReader ( pData, iLen );
	LoadStoredQuery ( PQ_META_VERSION_MAX, tQuery, tReader );
}

void LoadStoredQuery ( DWORD uVersion, StoredQueryDesc_t & tQuery, CSphReader & tReader )
{
	LoadStoredQuery<CSphReader> ( uVersion, tQuery, tReader );
}

void SaveStoredQuery ( const StoredQueryDesc_t & tQuery, CSphVector<BYTE> & dOut )
{
	MemoryWriter_c tWriter ( dOut );
	SaveStoredQuery ( tQuery, tWriter );
}

void SaveStoredQuery ( const StoredQueryDesc_t & tQuery, CSphWriter & tWriter )
{
	SaveStoredQuery<CSphWriter> ( tQuery, tWriter );
}

template<typename READER>
void LoadDeleteQuery ( CSphVector<uint64_t> & dQueries, CSphString & sTags, READER & tReader )
{
	dQueries.Resize ( tReader.UnzipInt() );
	ARRAY_FOREACH ( i, dQueries )
		dQueries[i] = tReader.UnzipOffset();

	sTags = tReader.GetString();
}

void LoadDeleteQuery ( const BYTE * pData, int iLen, CSphVector<uint64_t> & dQueries, CSphString & sTags )
{
	MemoryReader_c tReader ( pData, iLen );
	LoadDeleteQuery ( dQueries, sTags, tReader );
}

template<typename WRITER>
void SaveDeleteQuery ( const uint64_t * pQueries, int iCount, const char * sTags, WRITER & tWriter )
{
	tWriter.ZipInt ( iCount );
	for ( int i=0; i<iCount; i++ )
		tWriter.ZipOffset ( pQueries[i] );

	tWriter.PutString ( sTags );
}

void SaveDeleteQuery ( const uint64_t * pQueries, int iCount, const char * sTags, CSphVector<BYTE> & dOut )
{
	MemoryWriter_c tWriter ( dOut );
	SaveDeleteQuery ( pQueries, iCount, sTags, tWriter );
}


//////////////////////////////////////////////////////////////////////////

/// forward ref
class RtIndex_c;

/// TLS indexing accumulator (we disallow two uncommitted adds within one thread; and so need at most one)
static SphThreadKey_t g_tTlsAccumKey;

/// binlog file view of the index
/// everything that a given log file needs to know about an index
struct BinlogIndexInfo_t
{
	CSphString	m_sName;				///< index name
	int64_t		m_iMinTID = INT64_MAX;	///< min TID logged by this file
	int64_t		m_iMaxTID = 0;			///< max TID logged by this file
	int64_t		m_iFlushedTID = 0;		///< last flushed TID
	int64_t		m_tmMin = INT64_MAX;	///< min TID timestamp
	int64_t		m_tmMax = 0;			///< max TID timestamp

	CSphIndex *	m_pIndex = nullptr;		///< replay only; associated index (might be NULL if we don't serve it anymore!)
	RtIndex_c *	m_pRT = nullptr;		///< replay only; RT index handle (might be NULL if N/A or non-RT)
	PercolateIndex_i *	m_pPQ = nullptr;		///< replay only; PQ index handle (might be NULL if N/A or non-PQ)
	int64_t		m_iPreReplayTID = 0;	///< replay only; index TID at the beginning of this file replay
};

/// binlog file descriptor
/// file id (aka extension), plus a list of associated index infos
struct BinlogFileDesc_t
{
	int								m_iExt = 0;
	CSphVector<BinlogIndexInfo_t>	m_dIndexInfos;
};

/// Bin Log Operation
enum Blop_e
{
	BLOP_COMMIT			= 1,
	BLOP_UPDATE_ATTRS	= 2,
	BLOP_ADD_INDEX		= 3,
	BLOP_ADD_CACHE		= 4,
	BLOP_RECONFIGURE	= 5,
	BLOP_PQ_ADD			= 6,
	BLOP_PQ_DELETE		= 7,

	BLOP_TOTAL
};

// forward declaration
class BufferReader_t;
class RtBinlog_c;


class BinlogWriter_c : public CSphWriter
{
public:
					BinlogWriter_c ();
	virtual			~BinlogWriter_c () {}

	virtual	void	Flush ();
	void			Write ();
	void			Fsync ();
	bool			HasUnwrittenData () const { return m_iPoolUsed>0; }
	bool			HasUnsyncedData () const { return m_iLastFsyncPos!=m_iLastWritePos; }

	void			ResetCrc ();	///< restart checksumming
	void			WriteCrc ();	///< finalize and write current checksum to output stream


private:
	int64_t			m_iLastWritePos;
	int64_t			m_iLastFsyncPos;
	int				m_iLastCrcPos;

	DWORD			m_uCRC;
	void			HashCollected ();
};


class BinlogReader_c : public CSphAutoreader
{
public:
					BinlogReader_c ();

	void			ResetCrc ();
	bool			CheckCrc ( const char * sOp, const char * sIndexName, int64_t iTid, int64_t iTxnPos );

private:
	DWORD			m_uCRC;
	int				m_iLastCrcPos;
	virtual void	UpdateCache ();
	void			HashCollected ();
};


class RtBinlog_c : public ISphBinlog
{
public:
	RtBinlog_c ();
	~RtBinlog_c ();

	void	BinlogCommit ( int64_t * pTID, const char * sIndexName, const RtSegment_t * pSeg, const CSphVector<DocID_t> & dKlist, bool bKeywordDict );
	void	BinlogUpdateAttributes ( int64_t * pTID, const char * sIndexName, const CSphAttrUpdate & tUpd ) final;
	void	BinlogReconfigure ( int64_t * pTID, const char * sIndexName, const CSphReconfigureSetup & tSetup ) override;
	void	NotifyIndexFlush ( const char * sIndexName, int64_t iTID, bool bShutdown ) final;
	void	BinlogPqAdd ( int64_t * pTID, const char * sIndexName, const StoredQueryDesc_t & tStored ) override;
	void	BinlogPqDelete ( int64_t * pTID, const char * sIndexName, const uint64_t * pQueries, int iCount, const char * sTags ) override;

	void	Configure ( const CSphConfigSection & hSearchd, bool bTestMode );
	void	Replay ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, ProgressCallbackSimple_t * pfnProgressCallback );

	void	GetFlushInfo ( BinlogFlushInfo_t & tFlush );
	bool	IsActive () override	{ return !m_bDisabled; }
	void	CheckPath ( const CSphConfigSection & hSearchd, bool bTestMode );

private:
	static const DWORD		BINLOG_VERSION = 7;

	static const DWORD		BINLOG_HEADER_MAGIC = 0x4c425053;	/// magic 'SPBL' header that marks binlog file
	static const DWORD		BLOP_MAGIC = 0x214e5854;			/// magic 'TXN!' header that marks binlog entry
	static const DWORD		BINLOG_META_MAGIC = 0x494c5053;		/// magic 'SPLI' header that marks binlog meta

	int64_t					m_iFlushTimeLeft;
	volatile int			m_iFlushPeriod;

	enum OnCommitAction_e
	{
		ACTION_NONE,
		ACTION_FSYNC,
		ACTION_WRITE
	};
	OnCommitAction_e		m_eOnCommit;

	CSphMutex				m_tWriteLock; // lock on operation

	int						m_iLockFD;
	CSphString				m_sWriterError;
	BinlogWriter_c			m_tWriter;

	mutable CSphVector<BinlogFileDesc_t>	m_dLogFiles; // active log files

	CSphString				m_sLogPath;

	bool					m_bReplayMode; // replay mode indicator
	bool					m_bDisabled;

	int						m_iRestartSize; // binlog size restart threshold

	// replay stats
	mutable int				m_iReplayedRows=0;

private:
	static void				DoAutoFlush ( void * pBinlog );
	int 					GetWriteIndexID ( const char * sName, int64_t iTID, int64_t tmNow );
	void					LoadMeta ();
	void					SaveMeta ();
	void					LockFile ( bool bLock );
	void					DoCacheWrite ();
	void					CheckDoRestart ();
	void					CheckDoFlush ();
	void					OpenNewLog ( int iLastState=0 );

	int						ReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, int iBinlog );
	bool					ReplayCommit ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;
	bool					ReplayUpdateAttributes ( int iBinlog, BinlogReader_c & tReader ) const;
	bool					ReplayIndexAdd ( int iBinlog, const SmallStringHash_T<CSphIndex*> & hIndexes, BinlogReader_c & tReader ) const;
	bool					ReplayCacheAdd ( int iBinlog, BinlogReader_c & tReader ) const;
	bool					ReplayReconfigure ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;
	bool					ReplayPqAdd ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;
	bool					ReplayPqDelete ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const;

	bool	PreOp ( Blop_e eOp, int64_t * pTID, const char * sIndexName );
	void	PostOp ();
	bool	CheckCrc ( const char * sOp, const CSphString & sIndex, int64_t iTID, int64_t iTxnPos, BinlogReader_c & tReader ) const;
	void	CheckTid ( const char * sOp, const BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t iTxnPos ) const;
	void	CheckTidSeq ( const char * sOp, const BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t iTxnPos ) const;
	void	CheckTime ( BinlogIndexInfo_t & tIndex, const char * sOp, int64_t tmStamp, int64_t iTID, int64_t iTxnPos, DWORD uReplayFlags ) const;
	void	UpdateIndexInfo ( BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t tmStamp ) const;
};

ISphBinlog * GetBinlog() { return g_pRtBinlog; }

struct SphChunkGuard_t
{
	CSphFixedVector<const RtSegment_t *>	m_dRamChunks { 0 };
	CSphFixedVector<const CSphIndex *>		m_dDiskChunks { 0 };
	CSphRwlock *							m_pReading = nullptr;

	~SphChunkGuard_t();
};


struct ChunkStats_t
{
	CSphSourceStats				m_Stats;
	CSphFixedVector<int64_t>	m_dFieldLens;

	explicit ChunkStats_t ( const CSphSourceStats & s, const CSphFixedVector<int64_t> & dLens )
		: m_dFieldLens ( dLens.GetLength() )
	{
		m_Stats = s;
		ARRAY_FOREACH ( i, dLens )
			m_dFieldLens[i] = dLens[i];
	}
};

template<typename VEC>
CSphFixedVector<int> GetIndexNames ( const VEC & dIndexes, bool bAddNext )
{
	CSphFixedVector<int> dNames ( dIndexes.GetLength() + ( bAddNext ? 1 : 0 ) );

	if ( !dIndexes.GetLength() )
	{
		if ( bAddNext )
			dNames[0] = 0;

		return dNames;
	}

	int iLast = 0;
	ARRAY_FOREACH ( iChunk, dIndexes )
	{
		const char * sName = dIndexes[iChunk]->GetFilename();
		assert ( sName );
		int iLen = strlen ( sName );
		assert ( iLen > 0 );

		const char * sNum = sName + iLen - 1;
		while ( sNum && *sNum && *sNum>='0' && *sNum<='9' )
			sNum--;

		iLast = atoi(sNum+1);
		dNames[iChunk] = iLast;
	}

	if ( bAddNext )
		dNames[dIndexes.GetLength()] = iLast + 1;

	return dNames;
}

/// RAM based index
struct RtQword_t;
class RtRowIterator_c;
struct SaveDiskDataContext_t;

class RtIndex_c : public RtIndex_i, public ISphNoncopyable, public ISphWordlist, public ISphWordlistSuggest, public IndexUpdateHelper_c, public IndexAlterHelper_c, public DebugCheckHelper_c
{
public:
	explicit			RtIndex_c ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict );
						~RtIndex_c () final;

	bool				AddDocument ( const VecTraits_T<VecTraits_T<const char >> &dFields,
		CSphMatch & tDoc, bool bReplace, const CSphString & sTokenFilterOptions, const char ** ppStr,
		const VecTraits_T<int64_t> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt ) override;
	virtual bool		AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace,
		const char ** ppStr, const VecTraits_T<int64_t> & dMvas, CSphString & sError, CSphString & sWarning,
		ISphRtAccum * pAccExt );
	bool				DeleteDocument ( const DocID_t * pDocs, int iDocs, CSphString & sError, ISphRtAccum * pAccExt ) final;
	void				Commit ( int * pDeleted, ISphRtAccum * pAccExt ) final;
	void				RollBack ( ISphRtAccum * pAccExt ) final;
	void				CommitReplayable ( RtSegment_t * pNewSeg, CSphVector<DocID_t> & dAccKlist, int * pTotalKilled, bool bForceDump ); // FIXME? protect?
	void				CheckRamFlush () final;
	void				ForceRamFlush ( bool bPeriodic=false ) final;
	void				ForceDiskChunk() final;
	bool				AttachDiskIndex ( CSphIndex * pIndex, bool bTruncate, CSphString & sError ) final;
	bool				Truncate ( CSphString & sError ) final;
	void				Optimize () final;
	virtual void				ProgressiveMerge ();
	CSphIndex *			GetDiskChunk ( int iChunk ) final { return m_dDiskChunks.GetLength()>iChunk ? m_dDiskChunks[iChunk] : nullptr; }
	ISphTokenizer *		CloneIndexingTokenizer() const final { return m_pTokenizerIndexing->Clone ( SPH_CLONE_INDEX ); }

public:
#if USE_WINDOWS
#pragma warning(push,1)
#pragma warning(disable:4100)
#endif

	int					Kill ( DocID_t tDocID ) final;
	int					KillMulti ( const DocID_t * pKlist, int iKlistSize ) final;

	int					Build ( const CSphVector<CSphSource*> & , int , int ) final { return 0; }
	bool				Merge ( CSphIndex * , const CSphVector<CSphFilterSettings> &, bool ) final { return false; }

	bool				Prealloc ( bool bStripPath ) final;
	void				Dealloc () final {}
	void				Preread () final;
	void				SetMemorySettings ( bool bMlock, bool bOndiskAttrs, bool bOndiskPool ) final;
	void				SetBase ( const char * ) final {}
	bool				Rename ( const char * ) final { return true; }
	bool				Lock () final { return true; }
	void				Unlock () final {}
	void				PostSetup() final;
	bool				IsRT() const final { return true; }

	int					UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, bool & bCritical, CSphString & sError, CSphString & sWarning ) final;
	bool				SaveAttributes ( CSphString & sError ) const final;
	DWORD				GetAttributeStatus () const final { return m_uDiskAttrStatus; }
	bool				AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError ) final;

	void				DebugDumpHeader ( FILE * , const char * , bool ) final {}
	void				DebugDumpDocids ( FILE * ) final {}
	void				DebugDumpHitlist ( FILE * , const char * , bool ) final {}
	void				DebugDumpDict ( FILE * ) final {}
	int					DebugCheck ( FILE * fp ) final;
#if USE_WINDOWS
#pragma warning(pop)
#endif

	bool				EarlyReject ( CSphQueryContext * pCtx, CSphMatch & ) const final;
	const CSphSourceStats &		GetStats () const final { return m_tStats; }
	int64_t *			GetFieldLens() const final { return m_tSettings.m_bIndexFieldLens ? m_dFieldLens.Begin() : nullptr; }
	void				GetStatus ( CSphIndexStatus* ) const final;

	bool				MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const final;
	bool				MultiQueryEx ( int iQueries, const CSphQuery * ppQueries, CSphQueryResult ** ppResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const final;
	bool				DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError, const SphChunkGuard_t & tGuard ) const;
	bool				GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const final;
	bool				FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const final;
	void				AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, CSphDict * pDict, bool bGetStats, int iQpos, RtQword_t * pQueryWord, CSphVector <CSphKeywordInfo> & dKeywords, const SphChunkGuard_t & tGuard ) const;

	bool				RtQwordSetup ( RtQword_t * pQword, int iSeg, const SphChunkGuard_t & tGuard ) const;
	static bool			RtQwordSetupSegment ( RtQword_t * pQword, const RtSegment_t * pSeg, bool bSetup, bool bWordDict, int iWordsCheckpoint, const CSphIndexSettings & tSettings );

	bool				IsStarDict() const final;

	int64_t				GetUsedRam () const;
	static int64_t		GetUsedRam ( const SphChunkGuard_t & tGuard );

	bool				IsWordDict () const { return m_bKeywordDict; }
	int					GetWordCheckoint() const { return m_iWordsCheckpoint; }
	int					GetMaxCodepointLength() const { return m_iMaxCodepointLength; }

	// TODO: implement me
	void				SetProgressCallback ( CSphIndexProgress::IndexingProgress_fn ) final {}

	bool				IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const final;
	void				Reconfigure ( CSphReconfigureSetup & tSetup ) final;
	int64_t				GetFlushAge() const final;
	void				ProhibitSave() final {}

	void				SetDebugCheck () final { m_bDebugCheck = true; }

protected:
	CSphSourceStats		m_tStats;
	bool				m_bDebugCheck = false;

private:
	static const DWORD			META_HEADER_MAGIC	= 0x54525053;	///< my magic 'SPRT' header
	static const DWORD			META_VERSION		= 14;			///< current version

	int							m_iStride;
	CSphVector<RtSegment_t*>	m_dRamChunks GUARDED_BY ( m_tChunkLock );
	CSphVector<const RtSegment_t*>	m_dRetired;

	CSphMutex					m_tWriting;
	mutable CSphRwlock			m_tChunkLock;
	mutable CSphRwlock			m_tReading;

	CSphVector<DocID_t>			m_dKillsWhileSaving GUARDED_BY ( m_tWriting );	///< documents killed in ram chunks while we were saving disk chunks (double-buffered)
	CSphVector<DocID_t>			m_dKillsWhileOptimizing GUARDED_BY ( m_tOptimizingLock );

	/// double buffer stuff (allows to work with RAM chunk while future disk is being saved)
	/// m_dSegments consists of two parts
	/// segments with indexes < m_iDoubleBuffer are being saved now as a disk chunk
	/// segments with indexes >= m_iDoubleBuffer are RAM chunk
	CSphMutex					m_tFlushLock;
	CSphMutex					m_tOptimizingLock;
	int							m_iDoubleBuffer = 0;

	int64_t						m_iSoftRamLimit;
	int64_t						m_iDoubleBufferLimit;
	CSphString					m_sPath;
	bool						m_bPathStripped = false;
	CSphVector<CSphIndex*>		m_dDiskChunks GUARDED_BY ( m_tChunkLock );
	int							m_iLockFD = -1;
	volatile bool				m_bOptimizing = false;
	volatile bool				m_bOptimizeStop = false;

	int64_t						m_iSavedTID;
	int64_t						m_tmSaved;
	mutable DWORD				m_uDiskAttrStatus = 0;

	bool						m_bKeywordDict;
	int							m_iWordsCheckpoint = RTDICT_CHECKPOINT_V5;
	int							m_iMaxCodepointLength = 0;
	ISphTokenizerRefPtr_c		m_pTokenizerIndexing;
	bool						m_bLoadRamPassedOk = true;

	bool						m_bMlock = false;
	bool						m_bOndiskAllAttr = false;
	bool						m_bOndiskPoolAttr = false;

	CSphFixedVector<int64_t>	m_dFieldLens { SPH_MAX_FIELDS };	///< total field lengths over entire index
	CSphFixedVector<int64_t>	m_dFieldLensRam { SPH_MAX_FIELDS };	///< field lengths summed over current RAM chunk
	CSphFixedVector<int64_t>	m_dFieldLensDisk { SPH_MAX_FIELDS };	///< field lengths summed over all disk chunks
	CSphBitvec					m_tMorphFields;

	ISphRtAccum *		CreateAccum ( CSphString & sError ) final;

	int							CompareWords ( const RtWord_t * pWord1, const RtWord_t * pWord2 ) const;
	void						MergeAttributes ( RtRowIterator_c & tIt, CSphTightVector<CSphRowitem> & dRows, CSphTightVector<BYTE> & dBlobs, const CSphTightVector<BYTE> & dOldBlobs, int nBlobs, CSphVector<RowID_t> & dRowMap, RowID_t & tNextRowID );
	void						MergeKeywords ( RtSegment_t & tSeg, const RtSegment_t & tSeg1, const RtSegment_t & tSeg2, const CSphVector<RowID_t> & dRowMap1, const CSphVector<RowID_t> & dRowMap2 );
	RtSegment_t *				MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2, bool bHasMorphology );
	void						CopyWord ( RtSegment_t & tDst, const RtSegment_t & tSrc, RtDocWriter_t & tOutDoc, RtDocReader_t & tInDoc, RtWord_t & tWord, const CSphVector<RowID_t> & tRowMap );

	void						SaveMeta ( int64_t iTID, const CSphFixedVector<int> & dChunkNames );
	void						SaveDiskHeader ( SaveDiskDataContext_t & tCtx, const ChunkStats_t & tStats ) const;
	void						SaveDiskData ( const char * sFilename, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats ) const;
	int							SaveDiskChunk ( int64_t iTID, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats, bool bMoveRetired );
	CSphIndex *					LoadDiskChunk ( const char * sChunk, CSphString & sError ) const;
	bool						LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes );
	bool						SaveRamChunk ( const VecTraits_T<const RtSegment_t *>& dSegments );

	bool						WriteAttributes ( SaveDiskDataContext_t & tCtx, CSphString & sError ) const;
	bool						WriteDocs ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict, CSphString & sError ) const;
	void						WriteCheckpoints ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict ) const;
	bool						WriteDeadRowMap ( SaveDiskDataContext_t & tCtx, CSphString & sError ) const;

	void				GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const final;
	void				GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const final;
	void				GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const final;

	void				SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const final;
	void				SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const final;
	bool				ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const final;

	void				GetReaderChunks ( SphChunkGuard_t & tGuard ) const;
	void				FreeRetired();

	int							ApplyKillList ( const CSphVector<DocID_t> & dAccKlist );
	int							KillInDiskChunk ( IndexSegment_c * pSegment, const DocID_t * pKlist, int iKlistSize );

	void						Update_CollectRowPtrs ( UpdateContext_t & tCtx, const SphChunkGuard_t & tGuard );
	bool						Update_WriteBlobRow ( UpdateContext_t & tCtx, int iUpd, CSphRowitem * pDocinfo, const BYTE * pBlob, int iLength, int nBlobAttrs, bool & bCritical, CSphString & sError ) override;
	bool						Update_DiskChunks ( UpdateContext_t & tCtx, const SphChunkGuard_t & tGuard, int & iUpdated, CSphString & sError );
};


RtIndex_c::RtIndex_c ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict )
	: RtIndex_i ( sIndexName, sPath )
	, m_iSoftRamLimit ( iRamSize )
	, m_sPath ( sPath )
	, m_iSavedTID ( m_iTID )
	, m_tmSaved ( sphMicroTimer() )
	, m_bKeywordDict ( bKeywordDict )
{
	MEMORY ( MEM_INDEX_RT );

	m_tSchema = tSchema;
	m_iStride = m_tSchema.GetRowSize();

	m_iDoubleBufferLimit = ( m_iSoftRamLimit * SPH_RT_DOUBLE_BUFFER_PERCENT ) / 100;

#ifndef NDEBUG
	// check that index cols are static
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		assert ( !m_tSchema.GetAttr(i).m_tLocator.m_bDynamic );
#endif

	Verify ( m_tChunkLock.Init() );
	Verify ( m_tReading.Init() );

	ARRAY_FOREACH ( i, m_dFieldLens )
	{
		m_dFieldLens[i] = 0;
		m_dFieldLensRam[i] = 0;
		m_dFieldLensDisk[i] = 0;
	}
}


RtIndex_c::~RtIndex_c ()
{
	int64_t tmSave = sphMicroTimer();
	bool bValid = m_pTokenizer && m_pDict && m_bLoadRamPassedOk;

	if ( bValid )
	{
		SaveRamChunk ( m_dRamChunks );
		CSphFixedVector<int> dNames = GetIndexNames ( m_dDiskChunks, false );
		SaveMeta ( m_iTID, dNames );
	}

	Verify ( m_tReading.Done() );
	Verify ( m_tChunkLock.Done() );

	for ( auto & dRamChunk : m_dRamChunks )
		SafeDelete ( dRamChunk );

	m_dRetired.Uniq();
	for ( auto & dRetired : m_dRetired )
		SafeDelete ( dRetired );

	for ( auto & dDiskChunk : m_dDiskChunks )
		SafeDelete ( dDiskChunk );

	if ( m_iLockFD>=0 )
		::close ( m_iLockFD );

	// might be NULL during startup
	if ( g_pBinlog )
		g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, g_bShutdown );

	tmSave = sphMicroTimer() - tmSave;
	if ( tmSave>=1000 && bValid )
	{
		sphInfo ( "rt: index %s: ramchunk saved in %d.%03d sec",
			m_sIndexName.cstr(), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
	}
}


static int64_t g_iRtFlushPeriod = 10*60*60; // default period is 10 hours
int64_t GetRtFlushPeriod() { return g_iRtFlushPeriod; }

void RtIndex_c::CheckRamFlush ()
{
	if ( ( sphMicroTimer()-m_tmSaved )/1000000<g_iRtFlushPeriod )
		return;
	if ( g_pRtBinlog->IsActive() && m_iTID<=m_iSavedTID )
		return;

	ForceRamFlush ( true );
}


void RtIndex_c::ForceRamFlush ( bool bPeriodic )
{
	int64_t tmSave = sphMicroTimer();

	// need this lock as could get here at same time either ways:
	// via RtFlushThreadFunc->RtIndex_c::CheckRamFlush
	// and via HandleMysqlFlushRtindex
	CSphScopedLock<CSphMutex> tLock ( m_tFlushLock );

	if ( g_pRtBinlog->IsActive() && m_iTID<=m_iSavedTID )
		return;

	int64_t iUsedRam = 0;
	int64_t iSavedTID = m_iTID;
	{
		SphChunkGuard_t tGuard;
		GetReaderChunks ( tGuard );
		iUsedRam = GetUsedRam ( tGuard );

		if ( !SaveRamChunk ( tGuard.m_dRamChunks ) )
		{
			sphWarning ( "rt: index %s: ramchunk save FAILED! (error=%s)", m_sIndexName.cstr(), m_sLastError.cstr() );
			return;
		}
		CSphFixedVector<int> dNames = GetIndexNames ( tGuard.m_dDiskChunks, false );
		SaveMeta ( iSavedTID, dNames );
		for ( auto pChunk : tGuard.m_dDiskChunks )
			pChunk->FlushDeadRowMap(true);
	}
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), iSavedTID, false );

	int64_t iWasTID = m_iSavedTID;
	int64_t tmDelta = sphMicroTimer() - m_tmSaved;
	m_iSavedTID = iSavedTID;
	m_tmSaved = sphMicroTimer();

	tmSave = sphMicroTimer() - tmSave;
	sphInfo ( "rt: index %s: ramchunk saved ok (mode=%s, last TID=" INT64_FMT ", current TID=" INT64_FMT ", "
		"ram=%d.%03d Mb, time delta=%d sec, took=%d.%03d sec)"
		, m_sIndexName.cstr(), bPeriodic ? "periodic" : "forced"
		, iWasTID, m_iTID, (int)(iUsedRam/1024/1024), (int)((iUsedRam/1024)%1000)
		, (int) (tmDelta/1000000), (int)(tmSave/1000000), (int)((tmSave/1000)%1000) );
}


int64_t RtIndex_c::GetFlushAge() const
{
	if ( m_iSavedTID==0 || m_iSavedTID==m_iTID )
		return 0;

	return m_tmSaved;
}


int64_t RtIndex_c::GetUsedRam () const
{
	int64_t iTotal = 0;
	for ( const auto &pSeg : m_dRamChunks )
		iTotal += pSeg->GetUsedRam();

	return iTotal;
}


int64_t RtIndex_c::GetUsedRam ( const SphChunkGuard_t & tGuard  )
{
	int64_t iTotal = 0;
	for ( const auto & pSeg : tGuard.m_dRamChunks )
		iTotal += pSeg->GetUsedRam();

	return iTotal;
}

//////////////////////////////////////////////////////////////////////////
// INDEXING
//////////////////////////////////////////////////////////////////////////
CSphSource_StringVector::CSphSource_StringVector ( const VecTraits_T<const char *> &dFields, const CSphSchema & tSchema )
	: CSphSource_Document ( "$stringvector" )
{
	m_tSchema = tSchema;
	m_dFieldLengths.Reserve ( dFields.GetLength () );
	m_dFields.Reserve ( dFields.GetLength() + 1 );
	for ( const char* sField : dFields )
	{
		m_dFields.Add ( (BYTE*) sField );
		m_dFieldLengths.Add ( strlen ( sField ) );
		assert ( sField );
	}
	m_dFields.Add (nullptr);

	m_iMaxHits = 0; // force all hits build
}

CSphSource_StringVector::CSphSource_StringVector ( const VecTraits_T<VecTraits_T<const char >> &dFields, const CSphSchema & tSchema )
	: CSphSource_Document ( "$blobvector" )
{
	m_tSchema = tSchema;
	m_dFieldLengths.Reserve ( dFields.GetLength () );
	m_dFields.Reserve ( dFields.GetLength() + 1 );
	for ( const auto& dField : dFields )
	{
		m_dFields.Add ( (BYTE*) dField.begin() );
		m_dFieldLengths.Add ( dField.GetLength () );
		assert ( dField.begin() || dField.IsEmpty () );
	}
	m_dFields.Add (nullptr);

	m_iMaxHits = 0; // force all hits build
}

bool CSphSource_StringVector::Connect ( CSphString & )
{
	// no AddAutoAttrs() here; they should already be in the schema
	m_tHits.m_dData.Reserve ( 1024 );
	return true;
}

void CSphSource_StringVector::Disconnect ()
{
	m_tHits.m_dData.Reset();
}

bool RtIndex_c::AddDocument ( const VecTraits_T<VecTraits_T<const char >> &dFields,
	CSphMatch & tDoc,	bool bReplace, const CSphString & sTokenFilterOptions, const char ** ppStr,
	const VecTraits_T<int64_t> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	assert ( m_tSchema.GetAttrIndex ( sphGetDocidName() )==0 );
	assert ( m_tSchema.GetAttr ( sphGetDocidName() )->m_eAttrType==SPH_ATTR_BIGINT );

	DocID_t tDocID = sphGetDocID ( tDoc.m_pDynamic );
	if ( !tDocID )
		return false;

	ISphTokenizerRefPtr_c tTokenizer { CloneIndexingTokenizer() };

	if (!tTokenizer)
	{
		sError.SetSprintf ( "internal error: no indexing tokenizer available" );
		return false;
	}

	MEMORY ( MEM_INDEX_RT );

	if ( !bReplace )
	{
		CSphScopedRLock tRLock { m_tChunkLock };
		for ( auto & pSegment : m_dRamChunks )
			if ( pSegment->FindAliveRow ( tDocID ) )
			{
				sError.SetSprintf ( "duplicate id '" INT64_FMT "'", tDocID );
				return false; // already exists and not deleted; INSERT fails
			}
	}

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	tDoc.m_tRowID = pAcc->GenerateRowID();

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( !m_tSettings.m_sIndexTokenFilter.IsEmpty() )
	{
		tTokenizer = ISphTokenizer::CreatePluginFilter ( tTokenizer, m_tSettings.m_sIndexTokenFilter, sError );
		if ( !tTokenizer )
			return false;
		if ( !tTokenizer->SetFilterSchema ( m_tSchema, sError ) )
			return false;
		if ( !sTokenFilterOptions.IsEmpty() )
			if ( !tTokenizer->SetFilterOptions ( sTokenFilterOptions.cstr(), sError ) )
				return false;
	}

	// OPTIMIZE? do not create filter on each(!) INSERT
	if ( m_tSettings.m_uAotFilterMask )
		tTokenizer = sphAotCreateFilter ( tTokenizer, m_pDict, m_tSettings.m_bIndexExactWords, m_tSettings.m_uAotFilterMask );

	CSphSource_StringVector tSrc ( dFields, m_tSchema );

	// SPZ setup
	if ( m_tSettings.m_bIndexSP && !tTokenizer->EnableSentenceIndexing ( sError ) )
		return false;

	if ( !m_tSettings.m_sZones.IsEmpty() && !tTokenizer->EnableZoneIndexing ( sError ) )
		return false;

	if ( m_tSettings.m_bHtmlStrip && !tSrc.SetStripHTML ( m_tSettings.m_sHtmlIndexAttrs.cstr(), m_tSettings.m_sHtmlRemoveElements.cstr(),
			m_tSettings.m_bIndexSP, m_tSettings.m_sZones.cstr(), sError ) )
		return false;

	// OPTIMIZE? do not clone filters on each INSERT
	ISphFieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter )
		pFieldFilter = m_pFieldFilter->Clone();

	tSrc.Setup ( m_tSettings );
	tSrc.SetTokenizer ( tTokenizer );
	tSrc.SetDict ( pAcc->m_pDict );
	tSrc.SetFieldFilter ( pFieldFilter );
	tSrc.SetMorphFields ( m_tMorphFields );
	if ( !tSrc.Connect ( m_sLastError ) )
		return false;

	m_tSchema.CloneWholeMatch ( &tSrc.m_tDocInfo, tDoc );

	bool bEOF = false;
	if ( !tSrc.IterateStart ( sError ) || !tSrc.IterateDocument ( bEOF, sError ) )
		return false;

	ISphHits * pHits = tSrc.IterateHits ( sError );
	pAcc->GrabLastWarning ( sWarning );

	if ( !AddDocument ( pHits, tDoc, bReplace, ppStr, dMvas, sError, sWarning, pAcc ) )
		return false;

	m_tStats.m_iTotalBytes += tSrc.GetStats().m_iTotalBytes;

	return true;
}

static void AccumCleanup ( void * pArg )
{
	auto pAcc = (RtAccum_t *) pArg;
	SafeDelete ( pAcc );
}


ISphRtAccum * RtIndex_i::AcquireAccum ( CSphDict * pDict, ISphRtAccum * pAccExt,
	bool bWordDict, bool bSetTLS, CSphString* sError )
{
	auto pAcc = ( RtAccum_t * ) ( pAccExt ? pAccExt : sphThreadGet ( g_tTlsAccumKey ) );

	if ( pAcc && pAcc->GetIndex() && pAcc->GetIndex()!=this )
	{
		if ( sError )
			sError->SetSprintf ( "current txn is working with another index ('%s')", pAcc->GetIndex()->GetName() );
		return nullptr;
	}

	if ( !pAcc )
	{
		pAcc = new RtAccum_t ( bWordDict );
		if ( bSetTLS )
		{
			sphThreadSet ( g_tTlsAccumKey, pAcc );
			sphThreadOnExit ( AccumCleanup, pAcc );
		}
	}

	assert ( pAcc->GetIndex()==nullptr || pAcc->GetIndex()==this );
	pAcc->SetIndex ( this );
	pAcc->SetupDict ( this, pDict, bWordDict );
	return pAcc;
}

ISphRtAccum * RtIndex_c::CreateAccum ( CSphString & sError )
{
	return AcquireAccum ( m_pDict, nullptr, m_bKeywordDict, false, &sError);
}


bool RtIndex_c::AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, const char ** ppStr,
	const VecTraits_T<int64_t> & dMvas, CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	RtAccum_t * pAcc = (RtAccum_t *)pAccExt;

	if ( pAcc )
		pAcc->AddDocument ( pHits, tDoc, bReplace, m_tSchema.GetRowSize(), ppStr, dMvas );

	return ( pAcc!=NULL );
}


RtAccum_t::RtAccum_t ( bool bKeywordDict )
	: m_bKeywordDict ( bKeywordDict )
{}

RtAccum_t::~RtAccum_t()
{
	SafeDelete ( m_pBlobWriter );
}

void RtAccum_t::SetupDict ( const RtIndex_i * pIndex, CSphDict * pDict, bool bKeywordDict )
{
	if ( pIndex!=m_pIndex || pDict!=m_pRefDict || bKeywordDict!=m_bKeywordDict )
	{
		m_bKeywordDict = bKeywordDict;
		m_pRefDict = pDict;
		m_pDict = GetStatelessDict ( pDict );
		if ( m_bKeywordDict )
		{
			m_pDict = m_pDictRt = sphCreateRtKeywordsDictionaryWrapper ( m_pDict );
			SafeAddRef ( m_pDict ); // since m_pDict and m_pDictRt are DIFFERENT types, = works via CsphDict*
		}
	}
}

void RtAccum_t::ResetDict ()
{
	assert ( !m_bKeywordDict || m_pDictRt );
	if ( m_pDictRt )
	{
		m_pDictRt->ResetKeywords();
	}
}

void RtAccum_t::Sort ()
{
	if ( !m_bKeywordDict )
		m_dAccum.Sort ( CmpHitPlain_fn() );
	else
	{
		assert ( m_pDictRt );
		const BYTE * pPackedKeywords = m_pDictRt->GetPackedKeywords();
		m_dAccum.Sort ( CmpHitKeywords_fn ( pPackedKeywords ) );
	}
}

void RtAccum_t::Cleanup ( BYTE eWhat )
{
	if ( eWhat & EPartial )
	{
		m_dAccumRows.Resize ( 0 );
		m_dBlobs.Resize ( 0 );
		m_dPerDocHitsCount.Resize ( 0 );
		ResetDict ();
		ResetRowID ();
	}
	if ( eWhat & EAccum )
		m_dAccum.Resize ( 0 );
	if ( eWhat & ERest )
	{
		SetIndex ( nullptr );
		m_uAccumDocs = 0;
		m_dAccumKlist.Reset ();
	}
}

void RtAccum_t::AddDocument ( ISphHits * pHits, const CSphMatch & tDoc, bool bReplace, int iRowSize,
	const char ** ppStr, const VecTraits_T<int64_t> & dMvas )
{
	MEMORY ( MEM_RT_ACCUM );

	// FIXME? what happens on mixed insert/replace?
	m_bReplace = bReplace;

	DocID_t tDocID = sphGetDocID ( tDoc.m_pDynamic );

	// schedule existing copies for deletion
	m_dAccumKlist.Add ( tDocID );

	// reserve some hit space on first use
	if ( pHits && pHits->Length() && !m_dAccum.GetLength() )
		m_dAccum.Reserve ( 128*1024 );

	// accumulate row data; expect fully dynamic rows
	assert ( !tDoc.m_pStatic );
	assert (!( !tDoc.m_pDynamic && iRowSize!=0 ));
	assert (!( tDoc.m_pDynamic && (int)tDoc.m_pDynamic[-1]!=iRowSize ));

	m_dAccumRows.Append ( tDoc.m_pDynamic, iRowSize );
	CSphRowitem * pRow = &m_dAccumRows [ m_dAccumRows.GetLength() - iRowSize ];
	CSphString sError;

	int iStrAttr = 0;
	int iBlobAttr = 0;
	int iMva = 0;

	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
//		bool bJsonCleanup = false;
		const CSphColumnInfo & tColumn = tSchema.GetAttr(i);

		if ( tColumn.m_eAttrType==SPH_ATTR_STRING || tColumn.m_eAttrType==SPH_ATTR_JSON )
		{
			const char * pStr = ppStr ? ppStr[iStrAttr++] : nullptr;
			int iLen = 0;
			if ( tColumn.m_eAttrType==SPH_ATTR_STRING )
				iLen = ( pStr ? strlen ( pStr ) : 0 );
			else // SPH_ATTR_JSON - packed len + data
				iLen = sphUnpackPtrAttr ( (const BYTE*)pStr, (const BYTE**)&pStr );

			assert ( m_pBlobWriter );
			m_pBlobWriter->SetAttr( iBlobAttr++, (const BYTE*)pStr, iLen, sError );
		} else if ( tColumn.m_eAttrType==SPH_ATTR_UINT32SET || tColumn.m_eAttrType==SPH_ATTR_INT64SET )
		{
			assert ( m_pBlobWriter );
			const int64_t * pMva = &dMvas[iMva];
			int nValues = *pMva++;
			iMva += nValues+1;

			m_pBlobWriter->SetAttr ( iBlobAttr++, (const BYTE*)pMva, nValues*sizeof(int64_t), sError );
		}
	}

	if ( m_pBlobWriter )
	{
		const CSphColumnInfo * pBlobLoc = tSchema.GetAttr ( sphGetBlobLocatorName() );
		assert ( pBlobLoc );

		sphSetRowAttr ( pRow, pBlobLoc->m_tLocator, m_pBlobWriter->Flush() );
	}

	// handle index_field_lengths
	DWORD * pFieldLens = NULL;
	if ( m_pIndex->GetSettings().m_bIndexFieldLens )
	{
		int iFirst = tSchema.GetAttrId_FirstFieldLen();
		assert ( tSchema.GetAttr ( iFirst ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		assert ( tSchema.GetAttr ( iFirst+tSchema.GetFieldsCount()-1 ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		pFieldLens = pRow + ( tSchema.GetAttr ( iFirst ).m_tLocator.m_iBitOffset / 32 );
		memset ( pFieldLens, 0, sizeof(int)*tSchema.GetFieldsCount() ); // NOLINT
	}

	// accumulate hits
	int iHits = 0;
	if ( pHits && pHits->Length() )
	{
		CSphWordHit tLastHit;
		tLastHit.m_tRowID = INVALID_ROWID;
		tLastHit.m_uWordID = 0;
		tLastHit.m_uWordPos = 0;

		iHits = pHits->Length();
		m_dAccum.Reserve ( m_dAccum.GetLength()+iHits );
		iHits = 0;
		for ( CSphWordHit * pHit = pHits->m_dData.Begin(); pHit<=pHits->Last(); pHit++ )
		{
			// ignore duplicate hits
			if ( pHit->m_tRowID==tLastHit.m_tRowID && pHit->m_uWordID==tLastHit.m_uWordID && pHit->m_uWordPos==tLastHit.m_uWordPos )
				continue;

			// update field lengths
			if ( pFieldLens && HITMAN::GetField ( pHit->m_uWordPos )!=HITMAN::GetField ( tLastHit.m_uWordPos ) )
				pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );

			// need original hit for duplicate removal
			tLastHit = *pHit;
			// reset field end for not very last position
			if ( HITMAN::IsEnd ( pHit->m_uWordPos ) && pHit!=pHits->Last() &&
				pHit->m_tRowID==pHit[1].m_tRowID && pHit->m_uWordID==pHit[1].m_uWordID && HITMAN::IsEnd ( pHit[1].m_uWordPos ) )
				pHit->m_uWordPos = HITMAN::GetPosWithField ( pHit->m_uWordPos );

			// accumulate
			m_dAccum.Add ( *pHit );
			iHits++;
		}
		if ( pFieldLens )
			pFieldLens [ HITMAN::GetField ( tLastHit.m_uWordPos ) ] = HITMAN::GetPos ( tLastHit.m_uWordPos );
	}
	// make sure to get real count without duplicated hits
	m_dPerDocHitsCount.Add ( iHits );

	m_uAccumDocs++;
}


// cook checkpoints - make NULL terminating strings from offsets
static void FixupSegmentCheckpoints ( RtSegment_t * pSeg )
{
	assert ( pSeg &&
		( !pSeg->m_dWordCheckpoints.GetLength() || pSeg->m_dKeywordCheckpoints.GetLength() ) );
	if ( !pSeg->m_dWordCheckpoints.GetLength() )
		return;

	const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
	assert ( pBase );
	for ( auto & dCheckpoint : pSeg->m_dWordCheckpoints )
		dCheckpoint.m_sWord = pBase + dCheckpoint.m_uWordID;
}


RtSegment_t * RtAccum_t::CreateSegment ( int iRowSize, int iWordsCheckpoint )
{
	if ( !m_uAccumDocs )
		return nullptr;

	MEMORY ( MEM_RT_ACCUM );
	auto * pSeg = new RtSegment_t ( m_uAccumDocs );

	CSphWordHit tClosingHit;
	tClosingHit.m_uWordID = WORDID_MAX;
	tClosingHit.m_tRowID = INVALID_ROWID;
	tClosingHit.m_uWordPos = EMPTY_HIT;
	m_dAccum.Add ( tClosingHit );

	RtDoc_t tDoc;
	tDoc.m_tRowID = INVALID_ROWID;
	tDoc.m_uDocFields = 0;
	tDoc.m_uHits = 0;
	tDoc.m_uHit = 0;

	RtWord_t tWord;
	RtDocWriter_t tOutDoc ( pSeg );
	RtWordWriter_t tOutWord ( pSeg, m_bKeywordDict, iWordsCheckpoint );
	RtHitWriter_t tOutHit ( pSeg );

	const BYTE * pPacketBase = m_bKeywordDict ? m_pDictRt->GetPackedKeywords() : nullptr;

	Hitpos_t uEmbeddedHit = EMPTY_HIT;
	Hitpos_t uPrevHit = EMPTY_HIT;

	for ( const CSphWordHit &tHit : m_dAccum )
	{
		// new keyword or doc; flush current doc
		if ( tHit.m_uWordID!=tWord.m_uWordID || tHit.m_tRowID!=tDoc.m_tRowID )
		{
			if ( tDoc.m_tRowID!=INVALID_ROWID )
			{
				++tWord.m_uDocs;
				tWord.m_uHits += tDoc.m_uHits;

				if ( uEmbeddedHit )
				{
					assert ( tDoc.m_uHits==1 );
					tDoc.m_uHit = uEmbeddedHit;
				}

				tOutDoc.ZipDoc ( tDoc );
				tDoc.m_uDocFields = 0;
				tDoc.m_uHits = 0;
				tDoc.m_uHit = tOutHit.ZipHitPtr();
			}

			tDoc.m_tRowID = tHit.m_tRowID;
			tOutHit.ZipRestart ();
			uEmbeddedHit = EMPTY_HIT;
			uPrevHit = EMPTY_HIT;
		}

		// new keyword; flush current keyword
		if ( tHit.m_uWordID!=tWord.m_uWordID )
		{
			tOutDoc.ZipRestart ();
			if ( tWord.m_uWordID )
			{
				if ( m_bKeywordDict )
				{
					const BYTE * pPackedWord = pPacketBase + tWord.m_uWordID;
					assert ( pPackedWord[0] && pPackedWord[0]+1<m_pDictRt->GetPackedLen() );
					tWord.m_sWord = pPackedWord;
				}
				tOutWord.ZipWord ( tWord );
			}

			tWord.m_uWordID = tHit.m_uWordID;
			tWord.m_uDocs = 0;
			tWord.m_uHits = 0;
			tWord.m_uDoc = tOutDoc.ZipDocPtr();
			uPrevHit = EMPTY_HIT;
		}

		// might be a duplicate
		if ( uPrevHit==tHit.m_uWordPos )
			continue;

		// just a new hit
		if ( !tDoc.m_uHits )
		{
			uEmbeddedHit = tHit.m_uWordPos;
		} else
		{
			if ( uEmbeddedHit )
			{
				tOutHit.ZipHit ( uEmbeddedHit );
				uEmbeddedHit = 0;
			}

			tOutHit.ZipHit ( tHit.m_uWordPos );
		}
		uPrevHit = tHit.m_uWordPos;

		const int iField = HITMAN::GetField ( tHit.m_uWordPos );
		if ( iField<32 )
			tDoc.m_uDocFields |= ( 1UL<<iField );
		++tDoc.m_uHits;
	}

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	pSeg->m_uRows = m_uAccumDocs;
	pSeg->m_tAliveRows = m_uAccumDocs;

	pSeg->m_dRows.SwapData ( m_dAccumRows );
	pSeg->m_dBlobs.SwapData ( m_dBlobs );

	pSeg->BuildDocID2RowIDMap();

	m_tNextRowID = 0;

	// done
	return pSeg;
}


struct AccumDocHits_t
{
	DocID_t	m_tDocID;
	int		m_iDocIndex;
	int		m_iHitIndex;
	int		m_iHitCount;
};


struct CmpDocHitIndex_t
{
	inline bool IsLess ( const AccumDocHits_t & a, const AccumDocHits_t & b ) const
	{
		return ( a.m_tDocID<b.m_tDocID || ( a.m_tDocID==b.m_tDocID && a.m_iDocIndex<b.m_iDocIndex ) );
	}
};


void RtAccum_t::CleanupDuplicates ( int iRowSize )
{
	if ( m_uAccumDocs<=1 )
		return;

	assert ( m_uAccumDocs==(DWORD)m_dPerDocHitsCount.GetLength() );
	CSphVector<AccumDocHits_t> dDocHits ( m_dPerDocHitsCount.GetLength() );

	int iHitIndex = 0;
	CSphRowitem * pRow = m_dAccumRows.Begin();
	for ( DWORD i=0; i<m_uAccumDocs; i++, pRow+=iRowSize )
	{
		AccumDocHits_t & tElem = dDocHits[i];
		tElem.m_tDocID = sphGetDocID ( pRow );
		tElem.m_iDocIndex = i;
		tElem.m_iHitIndex = iHitIndex;
		tElem.m_iHitCount = m_dPerDocHitsCount[i];
		iHitIndex += m_dPerDocHitsCount[i];
	}

	dDocHits.Sort ( CmpDocHitIndex_t() );


	DocID_t uPrev = 0;
	if ( !dDocHits.FindFirst ( [&] ( const AccumDocHits_t &dDoc ) {
			bool bRes = dDoc.m_tDocID==uPrev;
			uPrev = dDoc.m_tDocID;
			return bRes;
		} ) )
		return;

	CSphFixedVector<RowID_t> dRowMap(m_uAccumDocs);
	for ( auto & i : dRowMap )
		i = 0;

	// identify duplicates to kill
	if ( m_bReplace )
	{
		// replace mode, last value wins, precending values are duplicate
		for ( DWORD i=0; i<m_uAccumDocs-1; i++ )
			if ( dDocHits[i].m_tDocID==dDocHits[i+1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	} else
	{
		// insert mode, first value wins, subsequent values are duplicates
		for ( DWORD i=1; i<m_uAccumDocs; i++ )
			if ( dDocHits[i].m_tDocID==dDocHits[i-1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	}

	RowID_t tNextRowID = 0;
	for ( auto & i : dRowMap )
		if ( i!=INVALID_ROWID )
			i = tNextRowID++;

	// remove duplicate hits
	int iResultRow = 0;
	ARRAY_FOREACH ( i, m_dAccum )
	{
		RowID_t tNewRowID = dRowMap[m_dAccum[i].m_tRowID];
		if ( tNewRowID!=INVALID_ROWID )
		{
			CSphWordHit & tResultHit = m_dAccum[iResultRow];
			tResultHit = m_dAccum[i];
			tResultHit.m_tRowID = tNewRowID;
			iResultRow++;
		}
	}

	m_dAccum.Resize(iResultRow);

	// remove duplicate docinfos
	iResultRow = 0;
	ARRAY_FOREACH ( i, dRowMap )
		if ( dRowMap[i]!=INVALID_ROWID )
		{
			memcpy ( &m_dAccumRows[iResultRow*iRowSize], &m_dAccumRows[i*iRowSize], iRowSize*sizeof(CSphRowitem) );
			iResultRow++;
		}

	m_dAccumRows.Resize ( iResultRow*iRowSize );
	m_uAccumDocs = iResultRow;
}


void RtAccum_t::GrabLastWarning ( CSphString & sWarning )
{
	if ( m_pDictRt && m_pDictRt->GetLastWarning() )
	{
		sWarning = m_pDictRt->GetLastWarning();
		m_pDictRt->ResetWarning();
	}
}


void RtAccum_t::SetIndex ( RtIndex_i * pIndex )
{
	m_pIndex = pIndex;

	if ( pIndex )
	{
		if ( pIndex->GetInternalSchema().HasBlobAttrs() )
		{
			SafeDelete ( m_pBlobWriter );
			m_pBlobWriter = sphCreateBlobRowBuilder ( pIndex->GetInternalSchema(), m_dBlobs );
		}
	} else
		SafeDelete ( m_pBlobWriter );
}


RowID_t RtAccum_t::GenerateRowID()
{
	return m_tNextRowID++;
}


void RtAccum_t::ResetRowID()
{
	m_tNextRowID=0;
}


void RtIndex_c::CopyWord ( RtSegment_t & tDst, const RtSegment_t & tSrc, RtDocWriter_t & tOutDoc, RtDocReader_t & tInDoc, RtWord_t & tWord, const CSphVector<RowID_t> & dRowMap )
{
	// copy docs
	while (true)
	{
		const RtDoc_t * pDoc = tInDoc.UnzipDoc();
		if ( !pDoc )
			break;

		RowID_t tNewRowID = dRowMap[pDoc->m_tRowID];
		if ( tNewRowID==INVALID_ROWID )
			continue;

		RtDoc_t tDoc = *pDoc;
		tDoc.m_tRowID = tNewRowID;

		tWord.m_uDocs++;
		tWord.m_uHits += pDoc->m_uHits;

		if ( pDoc->m_uHits!=1 )
		{
			RtHitWriter_t tOutHit ( &tDst );
			RtHitReader_t tInHit ( &tSrc, pDoc );

			tDoc.m_uHit = tOutHit.ZipHitPtr();

			// OPTIMIZE? decode+memcpy?
			for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
				tOutHit.ZipHit ( uValue );
		}

		// copy doc
		tOutDoc.ZipDoc ( tDoc );
	}
}


class RtRowIterator_c : public ISphNoncopyable
{
public:
	RtRowIterator_c ( const RtSegment_t * pSeg, int iStride )
		: m_pRow ( pSeg->m_dRows.Begin() )
		, m_pRowMax ( pSeg->m_dRows.Begin() + pSeg->m_dRows.GetLength() )
		, m_iStride ( iStride )
		, m_tDeadRowMap ( pSeg->m_tDeadRowMap )
	{}

	const CSphRowitem * GetNextAliveRow()
	{
		// while there are rows and k-list entries
		while ( m_pRow<m_pRowMax )
		{
			if ( !m_tDeadRowMap.IsSet(m_tRowID) )
				break;

			m_pRow += m_iStride;
			m_tRowID++;
		}

		if ( m_pRow>=m_pRowMax )
			return nullptr;

		// got it, and it's alive!
		m_tRowID++;
		m_pRow += m_iStride;
		return m_pRow-m_iStride;
	}

	RowID_t GetRowID() const
	{
		return m_tRowID-1;
	}

protected:
	RowID_t						m_tRowID {0};
	const CSphRowitem *			m_pRow {nullptr};
	const CSphRowitem *			m_pRowMax {nullptr};
	const int					m_iStride {0};
	const DeadRowMap_Ram_c &	m_tDeadRowMap;
};

template <typename BLOOM_TRAITS>
inline bool BuildBloom_T ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount, BLOOM_TRAITS & tBloom )
{
	if ( iLen<iInfixCodepointCount )
		return false;
	// byte offset for each codepoints
	BYTE dOffsets [ SPH_MAX_WORD_LEN+1 ] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
		20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42 };
	assert ( iLen<=SPH_MAX_WORD_LEN || ( bUtf8 && iLen<=SPH_MAX_WORD_LEN*3 ) );
	int iCodes = iLen;
	if ( bUtf8 )
	{
		// build an offsets table into the bytestring
		iCodes = 0;
		const BYTE * s = sWord;
		const BYTE * sEnd = sWord + iLen;
		while ( s<sEnd )
		{
			int iCodepoints = sphUtf8CharBytes ( *s );
			assert ( iCodepoints>=1 && iCodepoints<=4 );
			dOffsets[iCodes+1] = dOffsets[iCodes] + (BYTE)iCodepoints;
			s += iCodepoints;
			iCodes++;
		}
	}
	if ( iCodes<iInfixCodepointCount )
		return false;

	int iKeyBytes = iKeyValCount * 64;
	for ( int i=0; i<=iCodes-iInfixCodepointCount && tBloom.IterateNext(); i++ )
	{
		int iFrom = dOffsets[i];
		int iTo = dOffsets[i+iInfixCodepointCount];
		uint64_t uHash64 = sphFNV64 ( sWord+iFrom, iTo-iFrom );

		uHash64 = ( uHash64>>32 ) ^ ( (DWORD)uHash64 );
		int iByte = (int)( uHash64 % iKeyBytes );
		int iPos = iByte/64;
		uint64_t uVal = U64C(1) << ( iByte % 64 );

		tBloom.Set ( iPos, uVal );
	}
	return true;
}

// explicit instantiations
bool BuildBloom ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount
				  , BloomCheckTraits_t &tBloom )
{
	return BuildBloom_T ( sWord, iLen, iInfixCodepointCount, bUtf8, iKeyValCount, tBloom );
}

bool BuildBloom ( const BYTE * sWord, int iLen, int iInfixCodepointCount, bool bUtf8, int iKeyValCount
				  , BloomGenTraits_t &tBloom )
{
	return BuildBloom_T ( sWord, iLen, iInfixCodepointCount, bUtf8, iKeyValCount, tBloom );
}

void BuildSegmentInfixes ( RtSegment_t * pSeg, bool bHasMorphology, bool bKeywordDict, int iMinInfixLen, int iWordsCheckpoint, bool bUtf8 )
{
	if ( !pSeg || !bKeywordDict || !iMinInfixLen )
		return;

	int iBloomSize = ( pSeg->m_dWordCheckpoints.GetLength()+1 ) * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
	pSeg->m_dInfixFilterCP.Resize ( iBloomSize );
	// reset filters
	pSeg->m_dInfixFilterCP.Fill ( 0 );

	uint64_t * pRough = pSeg->m_dInfixFilterCP.Begin();
	const RtWord_t * pWord = nullptr;
	RtWordReader_t rdDictRough ( pSeg, true, iWordsCheckpoint );
	while ( ( pWord = rdDictRough.UnzipWord () )!=nullptr )
	{
		const BYTE * pDictWord = pWord->m_sWord+1;
		if ( bHasMorphology && *pDictWord!=MAGIC_WORD_HEAD_NONSTEMMED )
			continue;

		int iLen = pWord->m_sWord[0];
		if ( *pDictWord<0x20 ) // anyway skip heading magic chars in the prefix, like NONSTEMMED maker
		{
			pDictWord++;
			iLen--;
		}

		uint64_t * pVal = pRough + rdDictRough.m_iCheckpoint * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		BloomGenTraits_t tBloom0 ( pVal );
		BloomGenTraits_t tBloom1 ( pVal+BLOOM_PER_ENTRY_VALS_COUNT );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_0, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 );
		BuildBloom ( pDictWord, iLen, BLOOM_NGRAM_1, bUtf8, BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );
	}
}


void RtIndex_c::MergeAttributes ( RtRowIterator_c & tIt, CSphTightVector<CSphRowitem> & dRows, CSphTightVector<BYTE> & dBlobs, const CSphTightVector<BYTE> & dOldBlobs, int nBlobs, CSphVector<RowID_t> & dRowMap, RowID_t & tNextRowID )
{
	const CSphRowitem * pRow;
	while ( !!(pRow=tIt.GetNextAliveRow()) )
	{
		auto pNewRow = dRows.AddN ( m_iStride );
		memcpy ( pNewRow, pRow, m_iStride*sizeof(CSphRowitem) );

		if ( nBlobs )
		{
			int64_t iOldOffset = sphGetBlobRowOffset ( pRow );
			int64_t iNewOffset = sphCopyBlobRow ( dBlobs, dOldBlobs, iOldOffset, nBlobs );
			sphSetBlobRowOffset ( pNewRow, iNewOffset );
		}

		dRowMap[tIt.GetRowID()] = tNextRowID++;
	}
}


int RtIndex_c::CompareWords ( const RtWord_t * pWord1, const RtWord_t * pWord2 ) const
{
	if ( !pWord1 )
		return pWord2 ? 1 : 0;

	if ( !pWord2 )
		return -1;

	if ( m_bKeywordDict )
		return sphDictCmpStrictly ( (const char *)pWord1->m_sWord+1, *pWord1->m_sWord, (const char *)pWord2->m_sWord+1, *pWord2->m_sWord );

	if ( pWord1->m_uWordID<pWord2->m_uWordID )
		return -1;

	if ( pWord1->m_uWordID>pWord2->m_uWordID )
		return 1;

	return 0;
}


void RtIndex_c::MergeKeywords ( RtSegment_t & tSeg, const RtSegment_t & tSeg1, const RtSegment_t & tSeg2, const CSphVector<RowID_t> & dRowMap1, const CSphVector<RowID_t> & dRowMap2 )
{
	tSeg.m_dWords.Reserve ( Max ( tSeg1.m_dWords.GetLength(), tSeg2.m_dWords.GetLength() ) );
	tSeg.m_dDocs.Reserve ( Max ( tSeg1.m_dDocs.GetLength(), tSeg2.m_dDocs.GetLength() ) );
	tSeg.m_dHits.Reserve ( Max ( tSeg1.m_dHits.GetLength(), tSeg2.m_dHits.GetLength() ) );

	RtWordWriter_t tOut ( &tSeg, m_bKeywordDict, m_iWordsCheckpoint );
	RtWordReader_t tIn1 ( &tSeg1, m_bKeywordDict, m_iWordsCheckpoint );
	RtWordReader_t tIn2 ( &tSeg2, m_bKeywordDict, m_iWordsCheckpoint );

	const RtWord_t * pWords1 = tIn1.UnzipWord ();
	const RtWord_t * pWords2 = tIn2.UnzipWord ();

	while ( pWords1 || pWords2 )
	{
		int iCmp = CompareWords ( pWords1, pWords2 );

		RtDocWriter_t tOutDoc ( &tSeg );

		RtWord_t tWord = iCmp<=0 ? *pWords1 : *pWords2;
		tWord.m_uDocs = 0;
		tWord.m_uHits = 0;
		tWord.m_uDoc = tOutDoc.ZipDocPtr();

		// if words are equal, copy both
		if ( iCmp<=0 )
		{
			RtDocReader_t tInDoc ( &tSeg1, *pWords1 );
			CopyWord ( tSeg, tSeg1, tOutDoc, tInDoc, tWord, dRowMap1 );
		}

		if ( iCmp>=0 )
		{
			RtDocReader_t tInDoc ( &tSeg2, *pWords2 );
			CopyWord ( tSeg, tSeg2, tOutDoc, tInDoc, tWord, dRowMap2 );
		}

		// append word to the dictionary
		if ( tWord.m_uDocs )
			tOut.ZipWord ( tWord );

		// move forward (both readers if words are equal)
		if ( iCmp<=0 )
			pWords1 = tIn1.UnzipWord();

		if ( iCmp>=0 )
			pWords2 = tIn2.UnzipWord();
	}
}


RtSegment_t * RtIndex_c::MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2, bool bHasMorphology )
{
	if ( pSeg1->m_iTag > pSeg2->m_iTag )
		Swap ( pSeg1, pSeg2 );

	auto * pSeg = new RtSegment_t(0);

	////////////////////
	// merge attributes
	////////////////////

	// just a shortcut
	CSphTightVector<CSphRowitem> & dRows = pSeg->m_dRows;
	CSphTightVector<BYTE> & dBlobs = pSeg->m_dBlobs;

	// we might need less because of dupes, but we can not know yet
	dRows.Reserve ( Max ( pSeg1->m_dRows.GetLength(), pSeg2->m_dRows.GetLength() ) );
	dBlobs.Reserve ( Max ( pSeg1->m_dBlobs.GetLength(), pSeg2->m_dBlobs.GetLength() ) );

	CSphVector<RowID_t> dRowMap1(pSeg1->m_uRows);
	CSphVector<RowID_t> dRowMap2(pSeg2->m_uRows);

	int nBlobAttrs = 0;
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
		if ( sphIsBlobAttr ( m_tSchema.GetAttr(i).m_eAttrType ) )
			nBlobAttrs++;

	RowID_t tNextRowID = 0;

	{
		for ( auto & i : dRowMap1 )
			i = INVALID_ROWID;

		RtRowIterator_c tIt ( pSeg1, m_iStride );
		MergeAttributes ( tIt, dRows, dBlobs, pSeg1->m_dBlobs, nBlobAttrs, dRowMap1, tNextRowID );
	}

	{
		for ( auto & i : dRowMap2 )
			i = INVALID_ROWID;

		RtRowIterator_c tIt ( pSeg2, m_iStride );
		MergeAttributes ( tIt, dRows, dBlobs, pSeg2->m_dBlobs, nBlobAttrs, dRowMap2, tNextRowID );
	}

	assert ( tNextRowID<=INT_MAX );
	pSeg->m_uRows = tNextRowID;
	pSeg->m_tAliveRows = pSeg->m_uRows;
	pSeg->m_tDeadRowMap.Reset ( pSeg->m_uRows );

	assert ( pSeg->m_uRows*m_iStride==(DWORD)pSeg->m_dRows.GetLength() );

	// merged segment might be completely killed by committed data
	if ( !pSeg->m_uRows )
	{
		SafeDelete ( pSeg );
		return NULL;
	}

	MergeKeywords ( *pSeg, *pSeg1, *pSeg2, dRowMap1, dRowMap2 );

	if ( m_bKeywordDict )
		FixupSegmentCheckpoints ( pSeg );

	BuildSegmentInfixes ( pSeg, bHasMorphology, m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );

	pSeg->BuildDocID2RowIDMap();

	assert ( pSeg->m_dRows.GetLength() );
	assert ( pSeg->m_uRows );
	assert ( pSeg->m_tAliveRows==pSeg->m_uRows );

	return pSeg;
}


struct CmpSegments_fn
{
	inline bool IsLess ( const RtSegment_t * a, const RtSegment_t * b ) const
	{
			return a->GetMergeFactor() > b->GetMergeFactor();
	}
};


void RtIndex_c::Commit ( int * pDeleted, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_INDEX_RT );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return;

	// empty txn, just ignore
	if ( !pAcc->m_uAccumDocs && !pAcc->m_dAccumKlist.GetLength() )
	{
		pAcc->SetIndex ( nullptr );
		pAcc->Cleanup ( RtAccum_t::EPartial );
		return;
	}

	// phase 0, build a new segment
	// accum and segment are thread local; so no locking needed yet
	// segment might be NULL if we're only killing rows this txn
	pAcc->CleanupDuplicates ( m_tSchema.GetRowSize() );
	pAcc->Sort();

	RtSegment_t * pNewSeg = pAcc->CreateSegment ( m_tSchema.GetRowSize(), m_iWordsCheckpoint );
	assert ( !pNewSeg || pNewSeg->m_uRows>0 );
	assert ( !pNewSeg || pNewSeg->m_tAliveRows>0 );

	BuildSegmentInfixes ( pNewSeg, m_pDict->HasMorphology(), m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );

	// clean up parts we no longer need
	pAcc->Cleanup ( RtAccum_t::EPartial | RtAccum_t::EAccum );

	// sort accum klist, too
	pAcc->m_dAccumKlist.Uniq ();

	// now on to the stuff that needs locking and recovery
	CommitReplayable ( pNewSeg, pAcc->m_dAccumKlist, pDeleted, false );

	// done; cleanup accum
	pAcc->Cleanup ( RtAccum_t::ERest );

	// reset accumulated warnings
	CSphString sWarning;
	pAcc->GrabLastWarning ( sWarning );
}


int RtIndex_c::ApplyKillList ( const CSphVector<DocID_t> & dAccKlist )
{
	if ( !dAccKlist.GetLength() )
		return 0;

	int iKilled = 0;

	if ( m_iDoubleBuffer )
	{
		DocID_t * pAdd = m_dKillsWhileSaving.AddN ( m_iDoubleBuffer ? dAccKlist.GetLength() : 0 );
		memcpy ( pAdd, dAccKlist.Begin(), dAccKlist.GetLength()*sizeof(dAccKlist[0]) );
	}

	for ( auto & pChunk : m_dDiskChunks )
		iKilled += KillInDiskChunk ( pChunk, dAccKlist.Begin(), dAccKlist.GetLength() );

	// don't touch the chunks that are being saved
	for ( int iChunk = m_iDoubleBuffer; iChunk < m_dRamChunks.GetLength(); iChunk++ )
		iKilled += m_dRamChunks[iChunk]->KillMulti ( dAccKlist.Begin(), dAccKlist.GetLength() );

	return iKilled;
}


void RtIndex_c::CommitReplayable ( RtSegment_t * pNewSeg, CSphVector<DocID_t> & dAccKlist, int * pTotalKilled, bool bForceDump )
{
	// store statistics, because pNewSeg just might get merged
	int iNewDocs = pNewSeg ? pNewSeg->m_uRows : 0;

	CSphVector<int64_t> dLens;
	int iFirstFieldLenAttr = m_tSchema.GetAttrId_FirstFieldLen();
	if ( pNewSeg && iFirstFieldLenAttr>=0 )
	{
		assert ( pNewSeg->GetStride()==m_iStride );
		int iFields = m_tSchema.GetFieldsCount(); // shortcut
		dLens.Resize ( iFields );
		dLens.Fill ( 0 );
		for ( DWORD i=0; i<pNewSeg->m_uRows; ++i )
			for ( int j=0; j<iFields; ++j )
				dLens[j] += sphGetRowAttr ( pNewSeg->GetDocinfoByRowID(i), m_tSchema.GetAttr ( j+iFirstFieldLenAttr ).m_tLocator );
	}

	// phase 1, lock out other writers (but not readers yet)
	// concurrent readers are ok during merges, as existing segments won't be modified yet
	// however, concurrent writers are not
	Verify ( m_tWriting.Lock() );

	// first of all, binlog txn data for recovery
	g_pRtBinlog->BinlogCommit ( &m_iTID, m_sIndexName.cstr(), pNewSeg, dAccKlist, m_bKeywordDict );
	int64_t iTID = m_iTID;

	// prepare new segments vector
	// create more new segments by merging as needed
	// do not (!) kill processed old segments just yet, as readers might still need them
	CSphVector<RtSegment_t*> dSegments;

	dSegments.Reserve ( m_dRamChunks.GetLength() - m_iDoubleBuffer + 1 );
	for ( int i=m_iDoubleBuffer; i<m_dRamChunks.GetLength(); ++i )
		dSegments.Add ( m_dRamChunks[i] );

	if ( pNewSeg )
		dSegments.Add ( pNewSeg );

	int iTotalKilled = ApplyKillList ( dAccKlist );

	int64_t iRamFreed = 0;
	bool bHasMorphology = m_pDict->HasMorphology();
	FreeRetired();

	// enforce RAM usage limit
	int64_t iRamLeft = m_iDoubleBuffer ? m_iDoubleBufferLimit : m_iSoftRamLimit;
	for ( const auto& dSegment : dSegments )
		iRamLeft = Max ( iRamLeft - dSegment->GetUsedRam(), 0 );
	for ( const auto& dRetired : m_dRetired )
		iRamLeft = Max ( iRamLeft - dRetired->GetUsedRam(), 0 );

	// skip merging if no rows were added or no memory left
	bool bDump = ( iRamLeft==0 || bForceDump );
	const int MAX_SEGMENTS = 32;
	const int MAX_PROGRESSION_SEGMENT = 8;
	const int64_t MAX_SEGMENT_VECTOR_LEN = INT_MAX;
	while ( pNewSeg && iRamLeft>0 )
	{
		// segments sort order: large first, smallest last
		// merge last smallest segments
		dSegments.Sort ( CmpSegments_fn() );

		// unconditionally merge if there's too much segments now
		// conditionally merge if smallest segment has grown too large
		// otherwise, we're done
		const int iLen = dSegments.GetLength();
		if ( iLen < ( MAX_SEGMENTS - MAX_PROGRESSION_SEGMENT ) )
			break;
		assert ( iLen>=2 );
		// exit if progression is kept AND lesser MAX_SEGMENTS limit
		if ( dSegments[iLen-2]->GetMergeFactor() > dSegments[iLen-1]->GetMergeFactor()*2 && iLen < MAX_SEGMENTS )
			break;

		// check whether we have enough RAM
#define LOC_ESTIMATE1(_seg,_vec) \
	(int)( ( (int64_t)_seg->_vec.GetLength() ) * _seg->m_tAliveRows / _seg->m_uRows )

#define LOC_ESTIMATE0(_vec) \
	( LOC_ESTIMATE1 ( dSegments[iLen-1], _vec ) + LOC_ESTIMATE1 ( dSegments[iLen-2], _vec ) )

#define LOC_ESTIMATE( _vec ) \
    ( dSegments[iLen-1]->_vec.Relimit( 0, LOC_ESTIMATE0 ( _vec ) ) )

		using namespace sph;
		int64_t iWordsRelimit =	LOC_ESTIMATE ( m_dWords );
		int64_t iDocsRelimit =	LOC_ESTIMATE ( m_dDocs );
		int64_t iHitsRelimit =	LOC_ESTIMATE ( m_dHits );
		int64_t iBlobsRelimit = LOC_ESTIMATE ( m_dBlobs );
		int64_t iKeywordsRelimit = LOC_ESTIMATE ( m_dKeywordCheckpoints );
		int64_t iRowsRelimit =	LOC_ESTIMATE ( m_dRows );

#undef LOC_ESTIMATE
#undef LOC_ESTIMATE0
#undef LOC_ESTIMATE1

		int64_t iEstimate = iWordsRelimit + iDocsRelimit + iHitsRelimit + iBlobsRelimit + iKeywordsRelimit + iRowsRelimit;
		if ( iEstimate>iRamLeft )
		{
			// dump case: can't merge any more AND segments count limit's reached
			bDump = ( ( iRamLeft + iRamFreed )<=iEstimate ) && ( iLen>=MAX_SEGMENTS );
			break;
		}

		// we have to dump if we can't merge even smallest segments without breaking vector constrain ( len<INT_MAX )
		// split this way to avoid superlong string after macro expansion that kills gcov
		int64_t iMaxLen = Max (
			Max ( iWordsRelimit, iDocsRelimit ),
			Max ( iHitsRelimit, iBlobsRelimit ) );
		iMaxLen = Max (
			Max ( iRowsRelimit, iKeywordsRelimit ),
			iMaxLen );

		if ( MAX_SEGMENT_VECTOR_LEN<iMaxLen )
		{
			bDump = true;
			break;
		}

		// do it
		RtSegment_t * pA = dSegments.Pop();
		RtSegment_t * pB = dSegments.Pop();
		RtSegment_t * pMerged = MergeSegments ( pA, pB, bHasMorphology );
		if ( pMerged )
		{
			int64_t iMerged = pMerged->GetUsedRam();
			iRamLeft -= Min ( iRamLeft, iMerged );
			dSegments.Add ( pMerged );
		}
		m_dRetired.Add ( pA );
		m_dRetired.Add ( pB );

		iRamFreed += pA->GetUsedRam() + pB->GetUsedRam();
	}

	ARRAY_FOREACH ( i, dSegments )
	{
		RtSegment_t * pSeg = dSegments[i];
		if ( !pSeg->m_tAliveRows )
		{
			m_dRetired.Add ( pSeg );
			dSegments.RemoveFast ( i );
			--i;
		}
	}

	// wipe out readers - now we are only using RAM segments
	m_tChunkLock.WriteLock ();

	// go live!
	// got rid of 'old' double-buffer segments then add 'new' onces
	m_dRamChunks.Resize ( m_iDoubleBuffer + dSegments.GetLength() );
	memcpy ( m_dRamChunks.Begin() + m_iDoubleBuffer, dSegments.Begin(), dSegments.GetLengthBytes() );

	// phase 3, enable readers again
	// we might need to dump data to disk now
	// but during the dump, readers can still use RAM chunk data
	Verify ( m_tChunkLock.Unlock() );

	// update stats
	m_tStats.m_iTotalDocuments += iNewDocs - iTotalKilled;

	if ( dLens.GetLength() )
		for ( int i = 0; i < m_tSchema.GetFieldsCount(); ++i )
		{
			m_dFieldLensRam[i] += dLens[i];
			m_dFieldLens[i] = m_dFieldLensRam[i] + m_dFieldLensDisk[i];
		}

	// get flag of double-buffer prior mutex unlock
	bool bDoubleBufferActive = ( m_iDoubleBuffer>0 );

	// tell about DELETE affected_rows
	if ( pTotalKilled )
		*pTotalKilled = iTotalKilled;

	// we can kill retired segments now
	FreeRetired();

	// double buffer writer stands still till save done
	// all writers waiting double buffer done
	// no need to dump or waiting for some writer
	if ( !bDump || bDoubleBufferActive )
	{
		// all done, enable other writers
		Verify ( m_tWriting.Unlock() );
		return;
	}

	// scope for guard then retired clean up
	{
		// copy stats for disk chunk
		SphChunkGuard_t tGuard;
		GetReaderChunks ( tGuard );

		ChunkStats_t tStat2Dump ( m_tStats, m_dFieldLensRam );
		m_iDoubleBuffer = m_dRamChunks.GetLength();

		// need release m_tReading lock to prevent deadlock - commit vs SaveDiskChunk
		// chunks will keep till this scope and
		// will be freed after this scope on next commit at FreeRetired under writer lock
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
			m_dRetired.Add ( tGuard.m_dRamChunks[i] );
		tGuard.m_pReading->Unlock();
		tGuard.m_pReading = nullptr;

		Verify ( m_tWriting.Unlock() );

		int iSavedChunkId = SaveDiskChunk ( iTID, tGuard, tStat2Dump, false );
		g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), iTID, false );

		// notify the disk chunk that we were saving of the documents that were killed while we were saving it
		{
			CSphScopedLock<CSphMutex> tWriteLock ( m_tWriting );

			CSphIndex * pDiskChunk = nullptr;
			for ( auto pChunk : m_dDiskChunks )
				if ( pChunk->GetIndexId()==iSavedChunkId )
				{
					pDiskChunk = pChunk;
					break;
				}

			if ( pDiskChunk )
				pDiskChunk->KillMulti ( m_dKillsWhileSaving.Begin(), m_dKillsWhileSaving.GetLength() );

			m_dKillsWhileSaving.Resize(0);
		}
	}

	// TODO - try to call FreeRetired after take writer lock again
	// to free more memory
}


void RtIndex_c::FreeRetired()
{
	m_dRetired.Uniq();
	ARRAY_FOREACH ( i, m_dRetired )
	{
		if ( m_dRetired[i]->m_tRefCount.GetValue()==0 )
		{
			SafeDelete ( m_dRetired[i] );
			m_dRetired.RemoveFast ( i );
			i--;
		}
	}
}


void RtIndex_c::RollBack ( ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict );
	if ( !pAcc )
		return;

	pAcc->Cleanup ();
}

bool RtIndex_c::DeleteDocument ( const DocID_t * pDocs, int iDocs, CSphString & sError, ISphRtAccum * pAccExt )
{
	assert ( g_bRTChangesAllowed );
	MEMORY ( MEM_RT_ACCUM );

	auto pAcc = ( RtAccum_t * ) AcquireAccum ( m_pDict, pAccExt, m_bKeywordDict, true, &sError );
	if ( !pAcc )
		return false;

	if ( !iDocs )
		return true;

	assert ( pDocs && iDocs );

	// !COMMIT should handle case when uDoc what inserted in current txn here
	while ( iDocs-- )
		pAcc->m_dAccumKlist.Add ( *pDocs++ );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// LOAD/SAVE
//////////////////////////////////////////////////////////////////////////

struct Checkpoint_t
{
	uint64_t m_uWord;
	uint64_t m_uOffset;
};


void RtIndex_c::ForceDiskChunk() NO_THREAD_SAFETY_ANALYSIS
{
	MEMORY ( MEM_INDEX_RT );

	if ( !m_dRamChunks.GetLength() )
		return;

	CSphVector<DocID_t> dTmp;
	CommitReplayable ( nullptr, dTmp, nullptr, true );
}


struct SaveDiskDataContext_t
{
	SphOffset_t						m_tDocsOffset {0};
	SphOffset_t						m_tLastDocPos {0};
	SphOffset_t						m_tCheckpointsPosition {0};
	SphOffset_t						m_tMinMaxPos {0};
	DWORD							m_uRows {0};
	int64_t							m_iDocinfoIndex {0};
	int64_t							m_iTotalDocs {0};
	int64_t							m_iInfixBlockOffset {0};
	int								m_iInfixCheckpointWordsSize {0};
	ISphInfixBuilder *				m_pInfixer {nullptr};
	CSphVector<Checkpoint_t>		m_dCheckpoints;
	CSphVector<BYTE>				m_dKeywordCheckpoints;
	CSphVector<CSphVector<RowID_t>>	m_dRowMaps;
	const char *					m_szFilename;
	const SphChunkGuard_t &			m_tGuard;

	SaveDiskDataContext_t ( const char * szFilename, const SphChunkGuard_t & tGuard )
		: m_szFilename ( szFilename )
		, m_tGuard ( tGuard )
	{
		m_dRowMaps.Resize ( tGuard.m_dRamChunks.GetLength() );

		ARRAY_FOREACH ( i, m_dRowMaps )
		{
			m_dRowMaps[i].Resize ( tGuard.m_dRamChunks[i]->m_uRows );
			for ( auto & j : m_dRowMaps[i] )
				j = INVALID_ROWID;
		}
	}

	~SaveDiskDataContext_t()
	{
		delete m_pInfixer;
	}
};


bool RtIndex_c::WriteAttributes ( SaveDiskDataContext_t & tCtx, CSphString & sError ) const
{
	CSphString sSPA, sSPB, sSPT, sSPHI;
	CSphWriter tWriterSPA;

	sSPA.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPA).cstr() );
	sSPB.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPB).cstr() );
	sSPT.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPT).cstr() );
	sSPHI.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPHI).cstr() );

	if ( !tWriterSPA.OpenFile ( sSPA.cstr(), sError ) )
		return false;

	const CSphColumnInfo * pBlobLocatorAttr = m_tSchema.GetAttr ( sphGetBlobLocatorName() );
	AttrIndexBuilder_c tMinMaxBuilder(m_tSchema);

	CSphScopedPtr<BlobRowBuilder_i> pBlobRowBuilder(nullptr);
	if ( pBlobLocatorAttr )
	{
		pBlobRowBuilder = sphCreateBlobRowBuilder ( m_tSchema, sSPB, m_tSettings.m_tBlobUpdateSpace, sError );
		if ( !pBlobRowBuilder.Ptr() )
			return false;
	}

	tCtx.m_iTotalDocs = 0;
	for ( const auto & i : tCtx.m_tGuard.m_dRamChunks )
		tCtx.m_iTotalDocs += i->m_tAliveRows.GetValue();

	CSphFixedVector<DocidRowidPair_t> dLookup ( tCtx.m_iTotalDocs );

	RowID_t tNextRowID = 0;
	int iStride = m_tSchema.GetRowSize();
	CSphFixedVector<CSphRowitem> dRow ( iStride );
	CSphRowitem * pNewRow = dRow.Begin();
	ARRAY_FOREACH ( i, tCtx.m_tGuard.m_dRamChunks )
	{
		const RtSegment_t * pSeg = tCtx.m_tGuard.m_dRamChunks[i];
		assert ( pSeg );

		RtRowIterator_c tIt ( pSeg, iStride );
		const CSphRowitem * pRow;
		while ( !!(pRow=tIt.GetNextAliveRow()) )
		{
			tMinMaxBuilder.Collect(pRow);
			if ( pBlobLocatorAttr )
			{
				SphAttr_t tBlobOffset = sphGetRowAttr ( pRow, pBlobLocatorAttr->m_tLocator );
				SphOffset_t tOffset = pBlobRowBuilder->Flush ( pSeg->m_dBlobs.Begin() + tBlobOffset );

				memcpy ( pNewRow, pRow, iStride*sizeof(CSphRowitem) );
				sphSetRowAttr ( pNewRow, pBlobLocatorAttr->m_tLocator, tOffset );
				tWriterSPA.PutBytes ( pNewRow, iStride*sizeof(CSphRowitem) );
			} else
				tWriterSPA.PutBytes ( pRow, iStride*sizeof(CSphRowitem) );

			dLookup[tNextRowID].m_tDocID = sphGetDocID(pRow);
			dLookup[tNextRowID].m_tRowID = tNextRowID;

			tCtx.m_dRowMaps[i][tIt.GetRowID()] = tNextRowID++;
		}
	}

	if ( pBlobRowBuilder.Ptr() && !pBlobRowBuilder->Done(sError) )
		return false;

	dLookup.Sort ( bind ( &DocidRowidPair_t::m_tDocID ) );

	if ( !WriteDocidLookup ( sSPT, dLookup, sError ) )
		return false;

	dLookup.Reset(0);

	tMinMaxBuilder.FinishCollect();
	const CSphTightVector<CSphRowitem> & dMinMaxRows = tMinMaxBuilder.GetCollected();
	const CSphRowitem * pMinRow = dMinMaxRows.Begin()+dMinMaxRows.GetLength()-iStride*2;
	const CSphRowitem * pMaxRow = pMinRow+iStride;

	// create the histograms
	HistogramContainer_c tHistogramContainer;
	CSphVector<Histogram_i *> dHistograms;
	CSphVector<CSphColumnInfo> dPOD;
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		Histogram_i * pHistogram = CreateHistogram ( tAttr.m_sName, tAttr.m_eAttrType );
		if ( pHistogram )
		{
			Verify ( tHistogramContainer.Add ( pHistogram ) );
			dHistograms.Add ( pHistogram );
			dPOD.Add ( tAttr );
			pHistogram->Setup ( sphGetRowAttr ( pMinRow, tAttr.m_tLocator ), sphGetRowAttr ( pMaxRow, tAttr.m_tLocator ) );
		}
	}

	// iterate one more time to collect histogram data
	ARRAY_FOREACH ( i, tCtx.m_tGuard.m_dRamChunks )
	{
		const RtSegment_t * pSeg = tCtx.m_tGuard.m_dRamChunks[i];
		assert ( pSeg );

		RtRowIterator_c tIt ( pSeg, iStride );
		const CSphRowitem * pRow;
		while ( !!(pRow=tIt.GetNextAliveRow()) )
		{
			ARRAY_FOREACH ( i, dHistograms )
				dHistograms[i]->Insert ( sphGetRowAttr ( pRow,  dPOD[i].m_tLocator ) );
		}
	}

	if ( !tHistogramContainer.Save ( sSPHI, sError ) )
		return false;

	tCtx.m_tMinMaxPos = tWriterSPA.GetPos();
	tCtx.m_uRows = tNextRowID;
	tCtx.m_iDocinfoIndex = ( dMinMaxRows.GetLength() / m_tSchema.GetRowSize() / 2 ) - 1;
	tWriterSPA.PutBytes ( dMinMaxRows.Begin(), dMinMaxRows.GetLength()*sizeof(CSphRowitem) );

	return true;
}


bool RtIndex_c::WriteDocs ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict, CSphString & sError ) const
{
	CSphWriter tWriterHits, tWriterDocs, tWriterSkips;

	CSphString sName;
	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPP).cstr() );
	if ( !tWriterHits.OpenFile ( sName.cstr(), sError ) )
		return false;

	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPD).cstr() );
	if ( !tWriterDocs.OpenFile ( sName.cstr(), sError ) )
		return false;

	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPE).cstr() );
	if ( !tWriterSkips.OpenFile ( sName.cstr(), sError ) )
		return false;

	tWriterHits.PutByte(1);
	tWriterDocs.PutByte(1);
	tWriterSkips.PutByte(1);

	int iSegments = tCtx.m_tGuard.m_dRamChunks.GetLength();

	CSphVector<RtWordReader_t*> dWordReaders(iSegments);
	CSphVector<const RtWord_t*> dWords(iSegments);

	ARRAY_FOREACH ( i, dWordReaders )
		dWordReaders[i] = new RtWordReader_t ( tCtx.m_tGuard.m_dRamChunks[i], m_bKeywordDict, m_iWordsCheckpoint );

	ARRAY_FOREACH ( i, dWords )
		dWords[i] = dWordReaders[i]->UnzipWord();

	// loop keywords
	int iWords = 0;
	CSphKeywordDeltaWriter tLastWord;
	SphWordID_t uLastWordID = 0;
	CSphVector<SkiplistEntry_t> dSkiplist;

	tCtx.m_tLastDocPos = 0;

	bool bHasMorphology = m_pDict->HasMorphology();
	int iSkiplistBlockSize = m_tSettings.m_iSkiplistBlockSize;
	assert ( iSkiplistBlockSize>0 );

	while (true)
	{
		// find keyword with min id
		const RtWord_t * pWord = nullptr;
		for ( auto & i : dWords )
			if ( CompareWords ( i, pWord ) < 0 )
				pWord = i;

		if ( !pWord )
			break;

		SphOffset_t uDocpos = tWriterDocs.GetPos();
		SphOffset_t uLastHitpos = 0;
		RowID_t tLastRowID = INVALID_ROWID;
		RowID_t tSkiplistRowID = INVALID_ROWID;
		int iDocs = 0;
		int iHits = 0;
		dSkiplist.Resize(0);

		// loop all segments that have this keyword
		CSphBitvec tSegsWithWord ( iSegments );

		ARRAY_FOREACH ( iSegment, dWords )
		{
			if ( !CompareWords ( dWords[iSegment], pWord ) )
				tSegsWithWord.BitSet(iSegment);
			else
				continue;

			RtDocReader_t tDocReader ( tCtx.m_tGuard.m_dRamChunks[iSegment], *dWords[iSegment] );

			const RtDoc_t * pDoc;
			while ( !!( pDoc = tDocReader.UnzipDoc() ) )
			{
				RowID_t tRowID = tCtx.m_dRowMaps[iSegment][pDoc->m_tRowID];
				if ( tRowID==INVALID_ROWID )
					continue;

				// build skiplist, aka save decoder state as needed
				if ( ( iDocs & ( iSkiplistBlockSize-1 ) )==0 )
				{
					SkiplistEntry_t & t = dSkiplist.Add();
					t.m_tBaseRowIDPlus1 = tSkiplistRowID+1;
					t.m_iOffset = tWriterDocs.GetPos();
					t.m_iBaseHitlistPos = uLastHitpos;
				}

				iDocs++;
				iHits += pDoc->m_uHits;
				tSkiplistRowID = tRowID;

				tWriterDocs.ZipOffset ( tRowID - tLastRowID );
				tWriterDocs.ZipInt ( pDoc->m_uHits );
				if ( pDoc->m_uHits==1 )
				{
					tWriterDocs.ZipInt ( pDoc->m_uHit & 0x7FFFFFUL );
					tWriterDocs.ZipInt ( pDoc->m_uHit >> 23 );
				} else
				{
					tWriterDocs.ZipInt ( pDoc->m_uDocFields );
					tWriterDocs.ZipOffset ( tWriterHits.GetPos() - uLastHitpos );
					uLastHitpos = tWriterHits.GetPos();
				}

				tLastRowID = tRowID;

				// loop hits from current segment
				if ( pDoc->m_uHits>1 )
				{
					DWORD uLastHit = 0;
					RtHitReader_t tInHit ( tCtx.m_tGuard.m_dRamChunks[iSegment], pDoc );
					for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
					{
						tWriterHits.ZipInt ( uValue - uLastHit );
						uLastHit = uValue;
					}

					tWriterHits.ZipInt(0);
				}
			}
		}

		// write skiplist
		int iSkiplistOff = (int)tWriterSkips.GetPos();
		for ( int i=1; i<dSkiplist.GetLength(); i++ )
		{
			const SkiplistEntry_t & tPrev = dSkiplist[i-1];
			const SkiplistEntry_t & tCur = dSkiplist[i];
			assert ( tCur.m_tBaseRowIDPlus1 - tPrev.m_tBaseRowIDPlus1>=(DWORD)iSkiplistBlockSize );
			assert ( tCur.m_iOffset - tPrev.m_iOffset>=4*iSkiplistBlockSize );
			tWriterSkips.ZipInt ( tCur.m_tBaseRowIDPlus1 - tPrev.m_tBaseRowIDPlus1 - iSkiplistBlockSize );
			tWriterSkips.ZipOffset ( tCur.m_iOffset - tPrev.m_iOffset - 4*iSkiplistBlockSize );
			tWriterSkips.ZipOffset ( tCur.m_iBaseHitlistPos - tPrev.m_iBaseHitlistPos );
		}

		// write dict entry if necessary
		if ( tWriterDocs.GetPos()!=uDocpos )
		{
			tWriterDocs.ZipInt ( 0 ); // docs over

			if ( ( iWords%SPH_WORDLIST_CHECKPOINT )==0 )
			{
				if ( iWords )
				{
					SphOffset_t uOff = m_bKeywordDict ? 0 : uDocpos - tCtx.m_tLastDocPos;
					tWriterDict.ZipInt ( 0 );
					tWriterDict.ZipOffset ( uOff ); // store last hitlist length
				}

				// restart delta coding, once per SPH_WORDLIST_CHECKPOINT entries
				tCtx.m_tLastDocPos = 0;
				uLastWordID = 0;
				tLastWord.Reset();

				// begin new wordlist entry
				Checkpoint_t & tChk = tCtx.m_dCheckpoints.Add();
				tChk.m_uOffset = tWriterDict.GetPos();
				if ( m_bKeywordDict )
					tChk.m_uWord = sphPutBytes ( &tCtx.m_dKeywordCheckpoints, pWord->m_sWord, pWord->m_sWord[0]+1 ); // copy word len + word itself to checkpoint storage
				else
					tChk.m_uWord = pWord->m_uWordID;
			}

			iWords++;

			if ( m_bKeywordDict )
			{
				tLastWord.PutDelta ( tWriterDict, pWord->m_sWord+1, pWord->m_sWord[0] );
				tWriterDict.ZipOffset ( uDocpos );
			} else
			{
				assert ( pWord->m_uWordID!=uLastWordID );
				tWriterDict.ZipOffset ( pWord->m_uWordID - uLastWordID );
				uLastWordID = pWord->m_uWordID;
				assert ( uDocpos>tCtx.m_tLastDocPos );
				tWriterDict.ZipOffset ( uDocpos - tCtx.m_tLastDocPos );
			}

			tWriterDict.ZipInt ( iDocs );
			tWriterDict.ZipInt ( iHits );

			if ( m_bKeywordDict )
			{
				BYTE uHint = sphDoclistHintPack ( iDocs, tWriterDocs.GetPos()-tCtx.m_tLastDocPos );
				if ( uHint )
					tWriterDict.PutByte ( uHint );

				// build infixes
				if ( tCtx.m_pInfixer )
					tCtx.m_pInfixer->AddWord ( pWord->m_sWord+1, pWord->m_sWord[0], tCtx.m_dCheckpoints.GetLength(), bHasMorphology );
			}

			// emit skiplist pointer
			if ( iDocs>iSkiplistBlockSize )
				tWriterDict.ZipInt ( iSkiplistOff );

			tCtx.m_tLastDocPos = uDocpos;
		}

		// read next words
		for ( int i = 0; i < tSegsWithWord.GetBits(); i++ )
			if ( tSegsWithWord.BitGet(i) )
				dWords[i] = dWordReaders[i]->UnzipWord();
	}

	tCtx.m_tDocsOffset = tWriterDocs.GetPos();

	for ( auto i : dWordReaders )
		delete i;

	return true;
}


void RtIndex_c::WriteCheckpoints ( SaveDiskDataContext_t & tCtx, CSphWriter & tWriterDict ) const
{
	// write checkpoints
	SphOffset_t uOff = m_bKeywordDict ? 0 : tCtx.m_tDocsOffset - tCtx.m_tLastDocPos;

	// FIXME!!! don't write to wrDict if iWords==0
	// however plain index becomes m_bIsEmpty and full scan does not work there
	// we'll get partly working RT ( RAM chunk works and disk chunks give empty result set )
	tWriterDict.ZipInt ( 0 ); // indicate checkpoint
	tWriterDict.ZipOffset ( uOff ); // store last doclist length

	// flush infix hash entries, if any
	if ( tCtx.m_pInfixer )
		tCtx.m_pInfixer->SaveEntries ( tWriterDict );

	tCtx.m_tCheckpointsPosition = tWriterDict.GetPos();
	if ( m_bKeywordDict )
	{
		const char * pCheckpoints = (const char *)tCtx.m_dKeywordCheckpoints.Begin();
		for ( const auto & i : tCtx.m_dCheckpoints )
		{
			const char * pPacked = pCheckpoints + i.m_uWord;
			int iLen = *pPacked;
			assert ( iLen && (int)i.m_uWord+1+iLen<=tCtx.m_dKeywordCheckpoints.GetLength() );
			tWriterDict.PutDword ( iLen );
			tWriterDict.PutBytes ( pPacked+1, iLen );
			tWriterDict.PutOffset ( i.m_uOffset );
		}
	} else
	{
		for ( const auto & i : tCtx.m_dCheckpoints )
		{
			tWriterDict.PutOffset ( i.m_uWord );
			tWriterDict.PutOffset ( i.m_uOffset );
		}
	}

	// flush infix hash blocks
	if ( tCtx.m_pInfixer )
	{
		tCtx.m_iInfixBlockOffset = tCtx.m_pInfixer->SaveEntryBlocks ( tWriterDict );
		tCtx.m_iInfixCheckpointWordsSize = tCtx.m_pInfixer->GetBlocksWordsSize();

		if ( tCtx.m_iInfixBlockOffset>UINT_MAX )
			sphWarning ( "INTERNAL ERROR: dictionary size " INT64_FMT " overflow at infix save", tCtx.m_iInfixBlockOffset );
	}

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	tWriterDict.PutBytes ( "dict-header", 11 );
	tWriterDict.ZipInt ( tCtx.m_dCheckpoints.GetLength() );
	tWriterDict.ZipOffset ( tCtx.m_tCheckpointsPosition );
	tWriterDict.ZipInt ( m_pTokenizer->GetMaxCodepointLength() );
	tWriterDict.ZipInt ( (DWORD)tCtx.m_iInfixBlockOffset );
}


bool RtIndex_c::WriteDeadRowMap ( SaveDiskDataContext_t & tContext, CSphString & sError ) const
{
	CSphString sName;
	sName.SetSprintf ( "%s%s", tContext.m_szFilename, sphGetExt(SPH_EXT_SPM).cstr() );

	return ::WriteDeadRowMap ( sName, tContext.m_uRows, sError );
}


void RtIndex_c::SaveDiskData ( const char * szFilename, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats ) const
{
	CSphString sName, sError; // FIXME!!! report collected (sError) errors

	CSphWriter tWriterDict;

	sName.SetSprintf ( "%s%s", szFilename, sphGetExt(SPH_EXT_SPI).cstr() );
	tWriterDict.OpenFile ( sName.cstr(), sError );
	tWriterDict.PutByte(1);

	SaveDiskDataContext_t tCtx ( szFilename, tGuard );

	CSphScopedPtr<ISphInfixBuilder> pInfixer(nullptr);
	if ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings().m_bWordDict )
		tCtx.m_pInfixer = sphCreateInfixBuilder ( m_pTokenizer->GetMaxCodepointLength(), &sError );

	// fixme: handle errors
	WriteAttributes ( tCtx, sError );
	WriteDocs ( tCtx, tWriterDict, sError );
	WriteCheckpoints ( tCtx, tWriterDict );
	WriteDeadRowMap ( tCtx, sError );

	SaveDiskHeader ( tCtx, tStats );
}


static void FixupIndexSettings ( CSphIndexSettings & tSettings )
{
	tSettings.m_eHitFormat = SPH_HIT_FORMAT_INLINE;
	tSettings.m_iBoundaryStep = 0;
	tSettings.m_iStopwordStep = 1;
	tSettings.m_iOvershortStep = 1;
}


void RtIndex_c::SaveDiskHeader ( SaveDiskDataContext_t & tCtx, const ChunkStats_t & tStats ) const
{
	static const DWORD RT_INDEX_FORMAT_VERSION	= 56;			///< my format version

	CSphWriter tWriter;
	CSphString sName, sError;
	sName.SetSprintf ( "%s%s", tCtx.m_szFilename, sphGetExt(SPH_EXT_SPH).cstr() );
	tWriter.OpenFile ( sName.cstr(), sError );

	// format
	tWriter.PutDword ( INDEX_MAGIC_HEADER );
	tWriter.PutDword ( RT_INDEX_FORMAT_VERSION );

	// schema
	WriteSchema ( tWriter, m_tSchema );

	// wordlist checkpoints
	tWriter.PutOffset ( tCtx.m_tCheckpointsPosition );
	tWriter.PutDword ( tCtx.m_dCheckpoints.GetLength() );

	int iInfixCodepointBytes = ( m_tSettings.m_iMinInfixLen && m_pDict->GetSettings().m_bWordDict ? m_pTokenizer->GetMaxCodepointLength() : 0 );
	tWriter.PutByte ( iInfixCodepointBytes ); // m_iInfixCodepointBytes, v.27+
	tWriter.PutDword ( tCtx.m_iInfixBlockOffset ); // m_iInfixBlocksOffset, v.27+
	tWriter.PutDword ( tCtx.m_iInfixCheckpointWordsSize ); // m_iInfixCheckpointWordsSize, v.34+

	// stats
	tWriter.PutDword ( (DWORD)tCtx.m_iTotalDocs ); // FIXME? we don't expect over 4G docs per just 1 local index
	tWriter.PutOffset ( tStats.m_Stats.m_iTotalBytes );

	// index settings
	// FIXME: remove this?
	CSphIndexSettings tSettings = m_tSettings;
	FixupIndexSettings ( tSettings );
	SaveIndexSettings ( tWriter, tSettings );

	// tokenizer
	SaveTokenizerSettings ( tWriter, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );

	// dictionary
	// can not use embedding as stopwords id differs between RT and plain dictionaries
	SaveDictionarySettings ( tWriter, m_pDict, m_bKeywordDict, 0 );

	// min-max count
	tWriter.PutOffset ( tCtx.m_uRows );
	tWriter.PutOffset ( tCtx.m_iDocinfoIndex );
	tWriter.PutOffset ( tCtx.m_tMinMaxPos/sizeof(CSphRowitem) );		// min-max count

	// field filter
	SaveFieldFilterSettings ( tWriter, m_pFieldFilter );

	// field lengths
	if ( m_tSettings.m_bIndexFieldLens )
		for ( int i=0; i <m_tSchema.GetFieldsCount(); i++ )
			tWriter.PutOffset ( tStats.m_dFieldLens[i] );

	// done
	tWriter.CloseFile ();
}

namespace sph
{
	int rename ( const char * sOld, const char * sNew )
	{
#if USE_WINDOWS
		if ( MoveFileEx ( sOld, sNew, MOVEFILE_REPLACE_EXISTING ) )
			return 0;
		errno = GetLastError();
		return -1;
#else
		return ::rename ( sOld, sNew );
#endif
	}
}

void RtIndex_c::SaveMeta ( int64_t iTID, const CSphFixedVector<int> & dChunkNames )
{
	// sanity check
	if ( m_iLockFD<0 )
		return;

	// write new meta
	CSphString sMeta, sMetaNew;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );
	sMetaNew.SetSprintf ( "%s.meta.new", m_sPath.cstr() );

	CSphString sError;
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMetaNew, sError ) )
		sphDie ( "failed to serialize meta: %s", sError.cstr() ); // !COMMIT handle this gracefully
	wrMeta.PutDword ( META_HEADER_MAGIC );
	wrMeta.PutDword ( META_VERSION );
	wrMeta.PutDword ( (DWORD)m_tStats.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	wrMeta.PutOffset ( m_tStats.m_iTotalBytes ); // FIXME? need PutQword ideally
	wrMeta.PutOffset ( iTID );

	// meta v.4, save disk index format and settings, too
	wrMeta.PutDword ( INDEX_FORMAT_VERSION );
	WriteSchema ( wrMeta, m_tSchema );
	SaveIndexSettings ( wrMeta, m_tSettings );
	SaveTokenizerSettings ( wrMeta, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrMeta, m_pDict, m_bKeywordDict, m_tSettings.m_iEmbeddedLimit );

	// meta v.5
	wrMeta.PutDword ( m_iWordsCheckpoint );

	// meta v.7
	wrMeta.PutDword ( m_iMaxCodepointLength );
	wrMeta.PutByte ( BLOOM_PER_ENTRY_VALS_COUNT );
	wrMeta.PutByte ( BLOOM_HASHES_COUNT );

	// meta v.11
	SaveFieldFilterSettings ( wrMeta, m_pFieldFilter );

	// meta v.12
	wrMeta.PutDword ( dChunkNames.GetLength () );
	wrMeta.PutBytes ( dChunkNames.Begin(), dChunkNames.GetLengthBytes() );

	wrMeta.CloseFile();

	// no need to remove old but good meta in case new meta failed to save
	if ( wrMeta.IsError() )
	{
		sphWarning ( "%s", sError.cstr() );
		return;
	}

	// rename
	if ( sph::rename ( sMetaNew.cstr(), sMeta.cstr() ) )
		sphDie ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)",
			sMetaNew.cstr(), sMeta.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully
}


int RtIndex_c::SaveDiskChunk ( int64_t iTID, const SphChunkGuard_t & tGuard, const ChunkStats_t & tStats, bool bMoveRetired )
{
	if ( !tGuard.m_dRamChunks.GetLength() )
		return -1;

	int64_t tmSave = sphMicroTimer ();

	MEMORY ( MEM_INDEX_RT );

	CSphFixedVector<int> dChunkNames = GetIndexNames ( tGuard.m_dDiskChunks, true );

	// dump it
	CSphString sNewChunk;
	sNewChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames.Last() );
	SaveDiskData ( sNewChunk.cstr(), tGuard, tStats );

	// bring new disk chunk online
	CSphIndex * pDiskChunk = LoadDiskChunk ( sNewChunk.cstr(), m_sLastError );
	if ( !pDiskChunk )
		sphDie ( "%s", m_sLastError.cstr() );

	// FIXME! add binlog cleanup here once we have binlogs

	// get exclusive lock again, gotta reset RAM chunk now
	Verify ( m_tWriting.Lock() );
	Verify ( m_tChunkLock.WriteLock() );

	// save updated meta
	SaveMeta ( iTID, dChunkNames );
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// swap double buffer data
	int iNewSegmentsCount = ( m_iDoubleBuffer ? m_dRamChunks.GetLength() - m_iDoubleBuffer : 0 );
	for ( int i=0; i<iNewSegmentsCount; i++ )
		m_dRamChunks[i] = m_dRamChunks[i+m_iDoubleBuffer];
	m_dRamChunks.Resize ( iNewSegmentsCount );

	m_dDiskChunks.Add ( pDiskChunk );

	int iChunkId = pDiskChunk->GetIndexId();

	// update field lengths
	if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
	{
		ARRAY_FOREACH ( i, m_dFieldLensRam )
			m_dFieldLensRam[i] -= tStats.m_dFieldLens[i];
		ARRAY_FOREACH ( i, m_dFieldLensDisk )
			m_dFieldLensDisk[i] += tStats.m_dFieldLens[i];
	}

	Verify ( m_tChunkLock.Unlock() );

	if ( bMoveRetired )
	{
		ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
			m_dRetired.Add ( tGuard.m_dRamChunks[i] );
	}

	// abandon .ram file
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );
	if ( sphIsReadable ( sChunk.cstr() ) && ::unlink ( sChunk.cstr() ) )
		sphWarning ( "failed to unlink ram chunk (file=%s, errno=%d, error=%s)", sChunk.cstr(), errno, strerrorm(errno) );

	FreeRetired();

	m_iDoubleBuffer = 0;
	m_iSavedTID = iTID;
	m_tmSaved = sphMicroTimer();

	Verify ( m_tWriting.Unlock() );

	tmSave = sphMicroTimer () - tmSave;
	sphInfo ( "rt: index %s: diskchunk %d saved in %d.%03d sec",
			  m_sIndexName.cstr (), iChunkId, ( int ) ( tmSave / 1000000 ), ( int ) (( tmSave / 1000 ) % 1000 ));

	return iChunkId;
}


CSphIndex * RtIndex_c::LoadDiskChunk ( const char * sChunk, CSphString & sError ) const
{
	MEMORY ( MEM_INDEX_DISK );

	// !COMMIT handle errors gracefully instead of dying
	CSphIndex * pDiskChunk = sphCreateIndexPhrase ( sChunk, sChunk );
	if ( !pDiskChunk )
	{
		sError.SetSprintf ( "disk chunk %s: alloc failed", sChunk );
		return NULL;
	}

	pDiskChunk->m_iExpansionLimit = m_iExpansionLimit;
	pDiskChunk->m_iExpandKeywords = m_iExpandKeywords;
	pDiskChunk->SetBinlog ( false );
	pDiskChunk->SetMemorySettings ( m_bMlock, m_bOndiskAllAttr, m_bOndiskPoolAttr );
	if ( m_bDebugCheck )
		pDiskChunk->SetDebugCheck();

	if ( !pDiskChunk->Prealloc ( m_bPathStripped ) )
	{
		sError.SetSprintf ( "disk chunk %s: prealloc failed: %s", sChunk, pDiskChunk->GetLastError().cstr() );
		SafeDelete ( pDiskChunk );
		return NULL;
	}
	if ( !m_bDebugCheck )
		pDiskChunk->Preread();

	return pDiskChunk;
}

bool HasMvaUpdated ( const CSphString & sIndexPath )
{
	CSphString sChunkMVP;
	sChunkMVP.SetSprintf ( "%s.mvp", sIndexPath.cstr() );
	return sphIsReadable ( sChunkMVP.cstr() );
}


bool RtIndex_c::Prealloc ( bool bStripPath )
{
	MEMORY ( MEM_INDEX_RT );

	// locking uber alles
	// in RT backend case, we just must be multi-threaded
	// so we simply lock here, and ignore Lock/Unlock hassle caused by forks
	assert ( m_iLockFD<0 );

	CSphString sLock;
	sLock.SetSprintf ( "%s.lock", m_sPath.cstr() );
	m_iLockFD = ::open ( sLock.cstr(), SPH_O_NEW, 0644 );
	if ( m_iLockFD<0 )
	{
		m_sLastError.SetSprintf ( "failed to open %s: %s", sLock.cstr(), strerrorm(errno) );
		return false;
	}
	if ( !sphLockEx ( m_iLockFD, false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock %s: %s", sLock.cstr(), strerrorm(errno) );
		::close ( m_iLockFD );
		return false;
	}

	/////////////
	// load meta
	/////////////

	// check if we have a meta file (kinda-header)
	CSphString sMeta;
	sMeta.SetSprintf ( "%s.meta", m_sPath.cstr() );

	// no readable meta? no disk part yet
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return true;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, m_sLastError ) )
		return false;

	if ( rdMeta.GetDword()!=META_HEADER_MAGIC )
	{
		m_sLastError.SetSprintf ( "invalid meta file %s", sMeta.cstr() );
		return false;
	}
	DWORD uVersion = rdMeta.GetDword();
	if ( uVersion==0 || uVersion>META_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%d, binary is v.%d", sMeta.cstr(), uVersion, META_VERSION );
		return false;
	}

	DWORD uMinFormatVer = 14;
	if ( uVersion<uMinFormatVer )
	{
		m_sLastError.SetSprintf ( "indexes with meta prior to v.%d are no longer supported (use index_converter tool); %s is v.%d", uMinFormatVer, sMeta.cstr(), uVersion );
		return false;
	}

	m_tStats.m_iTotalDocuments = rdMeta.GetDword();
	m_tStats.m_iTotalBytes = rdMeta.GetOffset();
	m_iTID = rdMeta.GetOffset();

	// tricky bit
	// we started saving settings into .meta from v.4 and up only
	// and those reuse disk format version, aka INDEX_FORMAT_VERSION
	// anyway, starting v.4, serialized settings take precedence over config
	// so different chunks can't have different settings any more
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;
	CSphString sWarning;

	// load them settings
	DWORD uSettingsVer = rdMeta.GetDword();
	ReadSchema ( rdMeta, m_tSchema );
	LoadIndexSettings ( m_tSettings, rdMeta, uSettingsVer );
	if ( !LoadTokenizerSettings ( rdMeta, tTokenizerSettings, tEmbeddedFiles, m_sLastError ) )
		return false;
	LoadDictionarySettings ( rdMeta, tDictSettings, tEmbeddedFiles, sWarning );

	m_bKeywordDict = tDictSettings.m_bWordDict;

	// initialize AOT if needed
	DWORD uPrevAot = m_tSettings.m_uAotFilterMask;
	m_tSettings.m_uAotFilterMask = sphParseMorphAot ( tDictSettings.m_sMorphology.cstr() );
	if ( m_tSettings.m_uAotFilterMask!=uPrevAot )
		sphWarning ( "index '%s': morphology option changed from config has no effect, ignoring", m_sIndexName.cstr() );

	if ( bStripPath )
	{
		StripPath ( tTokenizerSettings.m_sSynonymsFile );
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
			StripPath ( tDictSettings.m_dWordforms[i] );
	}

	// recreate tokenizer

	m_pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, m_sLastError );
	if ( !m_pTokenizer )
		return false;

	// recreate dictionary

	m_pDict = sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, m_pTokenizer, m_sIndexName.cstr(), bStripPath, m_tSettings.m_iSkiplistBlockSize, m_sLastError );
	if ( !m_pDict )
	{
		m_sLastError.SetSprintf ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );
		return false;
	}

	m_pTokenizer = ISphTokenizer::CreateMultiformFilter ( m_pTokenizer, m_pDict->GetMultiWordforms () );

	// update schema
	m_iStride = m_tSchema.GetRowSize();

	m_iWordsCheckpoint = rdMeta.GetDword();

	// check that infixes definition changed - going to rebuild infixes
	bool bRebuildInfixes = false;
	m_iMaxCodepointLength = rdMeta.GetDword();
	int iBloomKeyLen = rdMeta.GetByte();
	int iBloomHashesCount = rdMeta.GetByte();
	bRebuildInfixes = ( iBloomKeyLen!=BLOOM_PER_ENTRY_VALS_COUNT || iBloomHashesCount!=BLOOM_HASHES_COUNT );

	if ( bRebuildInfixes )
		sphWarning ( "infix definition changed (from len=%d, hashes=%d to len=%d, hashes=%d) - rebuilding...",
					(int)BLOOM_PER_ENTRY_VALS_COUNT, (int)BLOOM_HASHES_COUNT, iBloomKeyLen, iBloomHashesCount );

	ISphFieldFilterRefPtr_c pFieldFilter;
	CSphFieldFilterSettings tFieldFilterSettings;
	LoadFieldFilterSettings ( rdMeta, tFieldFilterSettings );
	if ( tFieldFilterSettings.m_dRegexps.GetLength() )
		pFieldFilter = sphCreateRegexpFilter ( tFieldFilterSettings, m_sLastError );

	if ( !sphSpawnRLPFilter ( pFieldFilter, m_tSettings, tTokenizerSettings, sMeta.cstr(), m_sLastError ) )

		return false;


	SetFieldFilter ( pFieldFilter );

	CSphFixedVector<int> dChunkNames(0);
	int iLen = (int)rdMeta.GetDword();
	dChunkNames.Reset ( iLen );
	rdMeta.GetBytes ( dChunkNames.Begin(), iLen*sizeof(int) );

	///////////////
	// load chunks
	///////////////

	m_bPathStripped = bStripPath;

	// load disk chunks, if any
	ARRAY_FOREACH ( iChunk, dChunkNames )
	{
		CSphString sChunk;
		sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames[iChunk] );
		CSphIndex * pIndex = LoadDiskChunk ( sChunk.cstr(), m_sLastError );
		if ( !pIndex )
			sphDie ( "%s", m_sLastError.cstr() );

		m_dDiskChunks.Add ( pIndex );

		// tricky bit
		// outgoing match schema on disk chunk should be identical to our internal (!) schema
		if ( !m_tSchema.CompareTo ( pIndex->GetMatchSchema(), m_sLastError ) )
			return false;

		// update field lengths
		if ( m_tSchema.GetAttrId_FirstFieldLen()>=0 )
		{
			int64_t * pLens = pIndex->GetFieldLens();
			if ( pLens )
				for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); i++ )
					m_dFieldLensDisk[i] += pLens[i];
		}
	}

	// load ram chunk
	bool bRamLoaded = LoadRamChunk ( uVersion, bRebuildInfixes );

	// field lengths
	ARRAY_FOREACH ( i, m_dFieldLens )
		m_dFieldLens[i] = m_dFieldLensDisk[i] + m_dFieldLensRam[i];

	// set up values for on timer save
	m_iSavedTID = m_iTID;
	m_tmSaved = sphMicroTimer();

	return bRamLoaded;
}


void RtIndex_c::Preread ()
{
	// !COMMIT move disk chunks prereading here
}


void RtIndex_c::SetMemorySettings ( bool bMlock, bool bOndiskAttrs, bool bOndiskPool )
{
	m_bMlock = bMlock;
	m_bOndiskAllAttr = bOndiskAttrs;
	m_bOndiskPoolAttr = ( bOndiskAttrs || bOndiskPool );
}


static bool CheckVectorLength ( int iLen, int64_t iSaneLen, const char * sAt, CSphString & sError )
{
	if ( iLen>=0 && iLen<iSaneLen )
		return true;

	sError.SetSprintf ( "broken index, %s length overflow (len=%d, max=" INT64_FMT ")", sAt, iLen, iSaneLen );
	return false;
}

template < typename T >
static void SaveVector ( CSphWriter & tWriter, const VecTraits_T < T > & tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tWriter.PutDword ( tVector.GetLength() );
	if ( tVector.GetLength() )
		tWriter.PutBytes ( tVector.Begin(), tVector.GetLengthBytes() );
}


template < typename T, typename P >
static bool LoadVector ( CSphReader & tReader, CSphVector < T, P > & tVector,
	int64_t iSaneLen, const char * sAt, CSphString & sError )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	int iSize = tReader.GetDword();
	if ( !CheckVectorLength ( iSize, iSaneLen, sAt, sError ) )
		return false;

	tVector.Resize ( iSize );
	if ( tVector.GetLength() )
		tReader.GetBytes ( tVector.Begin(), tVector.GetLengthBytes() );

	return true;
}


template < typename T, typename P >
static void SaveVector ( BinlogWriter_c &tWriter, const CSphVector<T, P> &tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tWriter.ZipOffset ( tVector.GetLength() );
	if ( tVector.GetLength() )
		tWriter.PutBytes ( tVector.Begin(), tVector.GetLengthBytes() );
}


template < typename T, typename P >
static bool LoadVector ( BinlogReader_c & tReader, CSphVector < T, P > & tVector )
{
	STATIC_ASSERT ( std::is_pod<T>::value, NON_POD_VECTORS_ARE_UNSERIALIZABLE );
	tVector.Resize ( (int) tReader.UnzipOffset() ); // FIXME? sanitize?
	if ( tVector.GetLength() )
		tReader.GetBytes ( tVector.Begin(), tVector.GetLengthBytes() );
	return !tReader.GetErrorFlag();
}


bool RtIndex_c::SaveRamChunk ( const VecTraits_T<const RtSegment_t *> &dSegments )
{
	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk, sNewChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );
	sNewChunk.SetSprintf ( "%s.ram.new", m_sPath.cstr() );

	CSphWriter wrChunk;
	if ( !wrChunk.OpenFile ( sNewChunk, m_sLastError ) )
		return false;

	wrChunk.PutDword ( RtSegment_t::m_iSegments );
	wrChunk.PutDword ( dSegments.GetLength() );

	// no locks here, because it's only intended to be called from dtor
	for ( const RtSegment_t * pSeg : dSegments )
	{
		wrChunk.PutDword ( pSeg->m_uRows );
		wrChunk.PutDword ( pSeg->m_tAliveRows );
		wrChunk.PutDword ( pSeg->m_iTag );
		SaveVector ( wrChunk, pSeg->m_dWords );
		if ( m_bKeywordDict )
			SaveVector ( wrChunk, pSeg->m_dKeywordCheckpoints );

		auto pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
		wrChunk.PutDword ( pSeg->m_dWordCheckpoints.GetLength() );
		for ( const auto& dWordCheckpoint : pSeg->m_dWordCheckpoints )
		{
			wrChunk.PutOffset ( dWordCheckpoint.m_iOffset );
			if ( m_bKeywordDict )
				wrChunk.PutOffset ( dWordCheckpoint.m_sWord-pCheckpoints );
			else
				wrChunk.PutOffset ( dWordCheckpoint.m_uWordID );
		}

		SaveVector ( wrChunk, pSeg->m_dDocs );
		SaveVector ( wrChunk, pSeg->m_dHits );
		SaveVector ( wrChunk, pSeg->m_dRows );
		pSeg->m_tDeadRowMap.Save ( wrChunk );
		SaveVector ( wrChunk, pSeg->m_dBlobs );

		// infixes
		SaveVector ( wrChunk, pSeg->m_dInfixFilterCP );
	}

	// field lengths
	wrChunk.PutDword ( m_tSchema.GetFieldsCount() );
	for ( int i=0; i < m_tSchema.GetFieldsCount(); ++i )
		wrChunk.PutOffset ( m_dFieldLensRam[i] );

	wrChunk.CloseFile();
	if ( wrChunk.IsError() )
		return false;

	// rename
	if ( sph::rename ( sNewChunk.cstr(), sChunk.cstr() ) )
		sphDie ( "failed to rename ram chunk (src=%s, dst=%s, errno=%d, error=%s)",
			sNewChunk.cstr(), sChunk.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully

	return true;
}


bool RtIndex_c::LoadRamChunk ( DWORD uVersion, bool bRebuildInfixes )
{
	MEMORY ( MEM_INDEX_RT );

	CSphString sChunk;
	sChunk.SetSprintf ( "%s.ram", m_sPath.cstr() );

	if ( !sphIsReadable ( sChunk.cstr(), &m_sLastError ) )
		return true;

	m_bLoadRamPassedOk = false;

	CSphAutoreader rdChunk;
	if ( !rdChunk.Open ( sChunk, m_sLastError ) )
		return false;

	int64_t iFileSize = rdChunk.GetFilesize();
	int64_t iSaneVecSize = Min ( iFileSize, INT_MAX / 2 );
	int64_t iSaneTightVecSize = Min ( iFileSize, int ( INT_MAX / 1.2f ) );

	bool bHasMorphology = ( m_pDict && m_pDict->HasMorphology() ); // fresh and old-format index still has no dictionary at this point
	int iSegmentSeq = rdChunk.GetDword();

	int iSegmentCount = rdChunk.GetDword();
	if ( !CheckVectorLength ( iSegmentCount, iSaneVecSize, "ram-chunks", m_sLastError ) )
		return false;

	m_dRamChunks.Resize ( iSegmentCount );
	m_dRamChunks.Fill ( NULL );

	ARRAY_FOREACH ( iSeg, m_dRamChunks )
	{
		DWORD uRows = rdChunk.GetDword();

		RtSegment_t * pSeg = new RtSegment_t(uRows);
		pSeg->m_uRows = uRows;
		pSeg->m_tAliveRows = rdChunk.GetDword();
		m_dRamChunks[iSeg] = pSeg;

		pSeg->m_iTag = rdChunk.GetDword ();
		if ( !LoadVector ( rdChunk, pSeg->m_dWords, iSaneTightVecSize, "ram-words", m_sLastError ) )
			return false;

		if ( m_bKeywordDict && !LoadVector ( rdChunk, pSeg->m_dKeywordCheckpoints, iSaneVecSize, "ram-checkpoints", m_sLastError ) )
			return false;

		auto * pCheckpoints = (const char *)pSeg->m_dKeywordCheckpoints.Begin();

		int iCheckpointCount = rdChunk.GetDword();
		if ( !CheckVectorLength ( iCheckpointCount, iSaneVecSize, "ram-checkpoints", m_sLastError ) )
			return false;

		pSeg->m_dWordCheckpoints.Resize ( iCheckpointCount );
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int)rdChunk.GetOffset();
			SphOffset_t uOff = rdChunk.GetOffset();
			if ( m_bKeywordDict )
				pSeg->m_dWordCheckpoints[i].m_sWord = pCheckpoints + uOff;
			else
				pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t)uOff;
		}

		if ( !LoadVector ( rdChunk, pSeg->m_dDocs, iSaneTightVecSize, "ram-doclist", m_sLastError ) )
			return false;

		if ( !LoadVector ( rdChunk, pSeg->m_dHits, iSaneTightVecSize, "ram-hitlist", m_sLastError ) )
			return false;

		if ( !LoadVector ( rdChunk, pSeg->m_dRows, iSaneTightVecSize, "ram-attributes", m_sLastError ) )
			return false;

		pSeg->m_tDeadRowMap.Load ( uRows, rdChunk, m_sLastError );

		if ( !LoadVector ( rdChunk, pSeg->m_dBlobs, iSaneTightVecSize, "ram-blobs", m_sLastError ) )
			return false;

		// infixes
		if ( !LoadVector ( rdChunk, pSeg->m_dInfixFilterCP, iSaneTightVecSize, "ram-infixes", m_sLastError ) )
			return false;

		if ( bRebuildInfixes )
				BuildSegmentInfixes ( pSeg, bHasMorphology, m_bKeywordDict, m_tSettings.m_iMinInfixLen, m_iWordsCheckpoint, ( m_iMaxCodepointLength>1 ) );

		pSeg->BuildDocID2RowIDMap();
	}

	// field lengths
	int iFields = rdChunk.GetDword();
	assert ( iFields==m_tSchema.GetFieldsCount() );

	for ( int i=0; i<iFields; i++ )
		m_dFieldLensRam[i] = rdChunk.GetOffset();

	// all done
	RtSegment_t::m_iSegments = iSegmentSeq;
	if ( rdChunk.GetErrorFlag() )
		return false;

	m_bLoadRamPassedOk = true;
	return true;
}


void RtIndex_c::PostSetup()
{
	RtIndex_i::PostSetup();

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();

	// bigram filter
	if ( m_tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE && m_tSettings.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		m_pTokenizer->SetBuffer ( (BYTE*)m_tSettings.m_sBigramWords.cstr(), m_tSettings.m_sBigramWords.Length() );

		BYTE * pTok = NULL;
		while ( ( pTok = m_pTokenizer->GetToken() )!=NULL )
			m_tSettings.m_dBigramWords.Add() = (const char*)pTok;

		m_tSettings.m_dBigramWords.Sort();
	}

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	ISphTokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;

	const CSphDictSettings & tDictSettings = m_pDict->GetSettings();
	if ( !ParseMorphFields ( tDictSettings.m_sMorphology, tDictSettings.m_sMorphFields, m_tSchema.GetFields(), m_tMorphFields, m_sLastError ) )
		sphWarning ( "index '%s': %s", m_sIndexName.cstr(), m_sLastError.cstr() );
}

struct MemoryDebugCheckReader_c : public DebugCheckReader_i
{
	MemoryDebugCheckReader_c ( const BYTE * pData, const BYTE * pDataEnd )
		: m_pData ( pData )
		, m_pDataEnd ( pDataEnd )
		, m_pCur ( pData )
	{}
	virtual ~MemoryDebugCheckReader_c () override
	{}
	virtual int64_t GetLengthBytes () override
	{
		return ( m_pDataEnd - m_pData );
	}
	virtual bool GetBytes ( void * pData, int iSize ) override
	{
		if ( m_pCur && m_pCur+iSize<=m_pDataEnd )
		{
			memcpy ( pData, m_pCur, iSize );
			m_pCur += iSize;
			return true;
		} else
		{
			return false;
		}
	}

	bool SeekTo ( int64_t iOff, int iHint ) final
	{
		if ( m_pData && m_pData+iOff<m_pDataEnd )
		{
			m_pCur = m_pData + iOff;
			return true;
		} else
		{
			return false;
		}
	}

	const BYTE * m_pData = nullptr;
	const BYTE * m_pDataEnd = nullptr;
	const BYTE * m_pCur = nullptr;
};

int RtIndex_c::DebugCheck ( FILE * fp )
{
	// FIXME! remove copypasted code from CSphIndex_VLN::DebugCheck

	DebugCheckError_c tReporter(fp);

	if ( m_iStride!=m_tSchema.GetRowSize() )
		tReporter.Fail ( "wrong attribute stride (current=%d, should_be=%d)", m_iStride, m_tSchema.GetRowSize() );

	if ( m_iSoftRamLimit<=0 )
		tReporter.Fail ( "wrong RAM limit (current=" INT64_FMT ")", m_iSoftRamLimit );

	if ( m_iLockFD<0 )
		tReporter.Fail ( "index lock file id < 0" );

	if ( m_iTID<0 )
		tReporter.Fail ( "index TID < 0 (current=" INT64_FMT ")", m_iTID );

	if ( m_iSavedTID<0 )
		tReporter.Fail ( "index saved TID < 0 (current=" INT64_FMT ")", m_iSavedTID );

	if ( m_iTID<m_iSavedTID )
		tReporter.Fail ( "index TID < index saved TID (current=" INT64_FMT ", saved=" INT64_FMT ")", m_iTID, m_iSavedTID );

	if ( m_iWordsCheckpoint!=RTDICT_CHECKPOINT_V5 )
		tReporter.Fail ( "unexpected number of words per checkpoint (expected 48, got %d)", m_iWordsCheckpoint );

	ARRAY_FOREACH ( iSegment, m_dRamChunks )
	{
		tReporter.Msg ( "checking RT segment %d(%d)...", iSegment, m_dRamChunks.GetLength() );

		if ( !m_dRamChunks[iSegment] )
		{
			tReporter.Fail ( "missing RT segment (segment=%d)", iSegment );
			continue;
		}

		RtSegment_t & tSegment = *m_dRamChunks[iSegment];
		if ( !tSegment.m_uRows )
		{
			tReporter.Fail ( "empty RT segment (segment=%d)", iSegment );
			continue;
		}

		const BYTE * pCurWord = tSegment.m_dWords.Begin();
		const BYTE * pMaxWord = pCurWord+tSegment.m_dWords.GetLength();
		const BYTE * pCurDoc = tSegment.m_dDocs.Begin();
		const BYTE * pMaxDoc = pCurDoc+tSegment.m_dDocs.GetLength();
		const BYTE * pCurHit = tSegment.m_dHits.Begin();
		const BYTE * pMaxHit = pCurHit+tSegment.m_dHits.GetLength();

		CSphVector<RtWordCheckpoint_t> dRefCheckpoints;
		int nWordsRead = 0;
		int nCheckpointWords = 0;
		int iCheckpointOffset = 0;
		SphWordID_t uPrevWordID = 0;
		DWORD uPrevDocOffset = 0;
		DWORD uPrevHitOffset = 0;

		RtWord_t tWord;
		memset ( &tWord, 0, sizeof(tWord) );

		BYTE sWord[SPH_MAX_KEYWORD_LEN+2], sLastWord[SPH_MAX_KEYWORD_LEN+2];
		memset ( sWord, 0, sizeof(sWord) );
		memset ( sLastWord, 0, sizeof(sLastWord) );

		int iLastWordLen = 0, iWordLen = 0;

		while ( pCurWord && pCurWord<pMaxWord )
		{
			bool bCheckpoint = ++nCheckpointWords==m_iWordsCheckpoint;
			if ( bCheckpoint )
			{
				nCheckpointWords = 1;
				iCheckpointOffset = pCurWord - tSegment.m_dWords.Begin();
				tWord.m_uDoc = 0;
				if ( !m_bKeywordDict )
					tWord.m_uWordID = 0;
			}

			const BYTE * pIn = pCurWord;
			DWORD uDeltaDoc;
			if ( m_bKeywordDict )
			{
				BYTE iMatch, iDelta, uPacked;
				uPacked = *pIn++;

				if ( pIn>=pMaxWord )
				{
					tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
					break;
				}

				if ( uPacked & 0x80 )
				{
					iDelta = ( ( uPacked>>4 ) & 7 ) + 1;
					iMatch = uPacked & 15;
				} else
				{
					iDelta = uPacked & 127;
					iMatch = *pIn++;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}

					if ( iDelta<=8 && iMatch<=15 )
					{
						sLastWord[sizeof(sLastWord)-1] = '\0';
						tReporter.Fail ( "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
							iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta );
					}
				}

				if ( iMatch+iDelta>=(int)sizeof(sWord)-2 || iMatch>iLastWordLen )
				{
					sLastWord[sizeof(sLastWord)-1] = '\0';
					tReporter.Fail ( "wrong word-delta (segment=%d, word=%d, last_word=%s, last_len=%d, match=%d, delta=%d)",
						iSegment, nWordsRead, sLastWord+1, iLastWordLen, iMatch, iDelta );

					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}
				} else
				{
					iWordLen = iMatch+iDelta;
					sWord[0] = (BYTE)iWordLen;
					memcpy ( sWord+1+iMatch, pIn, iDelta );
					sWord[1+iWordLen] = 0;
					pIn += iDelta;
					if ( pIn>=pMaxWord )
					{
						tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );
						break;
					}
				}

				int iCalcWordLen = strlen ( (const char *)sWord+1 );
				if ( iWordLen!=iCalcWordLen )
				{
					sWord[sizeof(sWord)-1] = '\0';
					tReporter.Fail ( "word length mismatch (segment=%d, word=%d, read_word=%s, read_len=%d, calc_len=%d)", iSegment, nWordsRead, sWord+1, iWordLen, iCalcWordLen );
				}

				if ( !iWordLen )
					tReporter.Fail ( "empty word in word list (segment=%d, word=%d)", iSegment, nWordsRead );

				const BYTE * pStr = sWord+1;
				const BYTE * pStringStart = pStr;
				while ( pStringStart-pStr < iWordLen )
				{
					if ( !*pStringStart )
					{
						CSphString sErrorStr;
						sErrorStr.SetBinary ( (const char*)pStr, iWordLen );
						tReporter.Fail ( "embedded zero in a word list string (segment=%d, offset=%u, string=%s)", iSegment, (DWORD)(pStringStart-pStr), sErrorStr.cstr() );
					}

					pStringStart++;
				}

				if ( iLastWordLen && iWordLen )
				{
					if ( sphDictCmpStrictly ( (const char *)sWord+1, iWordLen, (const char *)sLastWord+1, iLastWordLen )<=0 )
					{
						sWord[sizeof(sWord)-1] = '\0';
						sLastWord[sizeof(sLastWord)-1] = '\0';
						tReporter.Fail ( "word order decreased (segment=%d, word=%d, read_word=%s, last_word=%s)", iSegment, nWordsRead, sWord+1, sLastWord+1 );
					}
				}

				memcpy ( sLastWord, sWord, iWordLen+2 );
				iLastWordLen = iWordLen;
			} else
			{
				SphWordID_t uDeltaID;
				pIn = UnzipWordid ( &uDeltaID, pIn );
				if ( pIn>=pMaxWord )
					tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

				tWord.m_uWordID += uDeltaID;

				if ( tWord.m_uWordID<=uPrevWordID )
				{
					tReporter.Fail ( "wordid decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", previd=" UINT64_FMT ")", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, (uint64_t)uPrevWordID );
				}

				uPrevWordID = tWord.m_uWordID;
			}

			pIn = UnzipDword ( &tWord.m_uDocs, pIn );
			if ( pIn>=pMaxWord )
			{
				sWord[sizeof(sWord)-1] = '\0';
				tReporter.Fail ( "invalid docs/hits (segment=%d, word=%d, read_word=%s, docs=%u, hits=%u)", iSegment, nWordsRead, sWord+1, tWord.m_uDocs, tWord.m_uHits );
			}

			pIn = UnzipDword ( &tWord.m_uHits, pIn );
			if ( pIn>=pMaxWord )
				tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

			pIn = UnzipDword ( &uDeltaDoc, pIn );
			if ( pIn>pMaxWord )
				tReporter.Fail ( "reading past wordlist end (segment=%d, word=%d)", iSegment, nWordsRead );

			pCurWord = pIn;
			tWord.m_uDoc += uDeltaDoc;

			if ( !tWord.m_uDocs || !tWord.m_uHits || tWord.m_uHits<tWord.m_uDocs )
			{
				sWord[sizeof(sWord)-1] = '\0';
				tReporter.Fail ( "invalid docs/hits (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, docs=%u, hits=%u)",
					iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tWord.m_uDocs, tWord.m_uHits );
			}

			if ( bCheckpoint )
			{
				RtWordCheckpoint_t & tCP = dRefCheckpoints.Add();
				tCP.m_iOffset = iCheckpointOffset;

				if ( m_bKeywordDict )
				{
					tCP.m_sWord = new char [sWord[0]+1];
					memcpy ( (void *)tCP.m_sWord, sWord+1, sWord[0]+1 );
				} else
					tCP.m_uWordID = tWord.m_uWordID;
			}

			sWord[sizeof(sWord)-1] = '\0';

			if ( uPrevDocOffset && tWord.m_uDoc<=uPrevDocOffset )
				tReporter.Fail ( "doclist offset decreased (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, prev_doclist_offset=%u)",
					iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tWord.m_uDoc, uPrevDocOffset );

			// read doclist
			DWORD uDocOffset = pCurDoc-tSegment.m_dDocs.Begin();
			if ( tWord.m_uDoc!=uDocOffset )
			{
				tReporter.Fail ( "unexpected doclist offset (wordid=" UINT64_FMT "(%s)(%d), doclist_offset=%u, expected_offset=%u)",
					(uint64_t)tWord.m_uWordID, sWord+1, nWordsRead, tWord.m_uDoc, uDocOffset );

				if ( uDocOffset>=(DWORD)tSegment.m_dDocs.GetLength() )
				{
					tReporter.Fail ( "doclist offset pointing past doclist (segment=%d, word=%d, read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );

					nWordsRead++;
					continue;
				} else
					pCurDoc = tSegment.m_dDocs.Begin()+uDocOffset;
			}

			// read all docs from doclist
			RtDoc_t tDoc;
			RowID_t tPrevRowID = INVALID_ROWID;

			for ( DWORD uDoc=0; uDoc<tWord.m_uDocs && pCurDoc<pMaxDoc; uDoc++ )
			{
				bool bEmbeddedHit = false;
				pIn = pCurDoc;
				RowID_t tDeltaRowID;
				pIn = UnzipDword ( &tDeltaRowID, pIn );

				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				tDoc.m_tRowID += tDeltaRowID;
				DWORD uDocField;
				pIn = UnzipDword ( &uDocField, pIn );

				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				tDoc.m_uDocFields = uDocField;
				pIn = UnzipDword ( &tDoc.m_uHits, pIn );
				if ( pIn>=pMaxDoc )
				{
					tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
					break;
				}

				if ( tDoc.m_uHits==1 )
				{
					bEmbeddedHit = true;

					DWORD a, b;
					pIn = UnzipDword ( &a, pIn );
					if ( pIn>=pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}

					pIn = UnzipDword ( &b, pIn );
					if ( pIn>pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}

					tDoc.m_uHit = HITMAN::Create ( b, a );
				} else
				{
					pIn = UnzipDword ( &tDoc.m_uHit, pIn );
					if ( pIn>pMaxDoc )
					{
						tReporter.Fail ( "reading past doclist end (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, doclist_offset=%u, doclist_size=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, uDocOffset, tSegment.m_dDocs.GetLength() );
						break;
					}
				}

				pCurDoc = pIn;

				if ( uDoc && tDoc.m_tRowID<=tPrevRowID )
				{
					tReporter.Fail ( "rowid decreased (segment=%d, word=%d, read_wordid=" UINT64_FMT ", read_word=%s, rowid=%u, prev_rowid=%u)",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, sWord+1, tDoc.m_tRowID, tPrevRowID );
				}

				if ( tDoc.m_tRowID>=tSegment.m_uRows )
					tReporter.Fail ( "invalid rowid (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, tWord.m_uWordID, tDoc.m_tRowID );

				if ( bEmbeddedHit )
				{
					DWORD uFieldId = HITMAN::GetField ( tDoc.m_uHit );
					DWORD uFieldMask = tDoc.m_uDocFields;
					int iCounter = 0;
					for ( ; uFieldMask; iCounter++ )
						uFieldMask &= uFieldMask - 1;

					if ( iCounter!=1 || tDoc.m_uHits!=1 )
					{
						tReporter.Fail ( "embedded hit with multiple occurences in a document found (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );
					}

					if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
					{
						tReporter.Fail ( "invalid field id in an embedded hit (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, total_fields=%d)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, m_tSchema.GetFieldsCount() );
					}

					if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
					{
						tReporter.Fail ( "invalid field id: not in doclist mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, field_mask=%u)",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, tDoc.m_uDocFields );
					}
				} else
				{
					DWORD uExpectedHitOffset = pCurHit-tSegment.m_dHits.Begin();
					if ( tDoc.m_uHit!=uExpectedHitOffset )
					{
						tReporter.Fail ( "unexpected hitlist offset (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, offset=%u, expected_offset=%u",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uHit, uExpectedHitOffset );
					}

					if ( tDoc.m_uHit && tDoc.m_uHit<=uPrevHitOffset )
					{
						tReporter.Fail ( "hitlist offset decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, offset=%u, prev_offset=%u",
							iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uHit, uPrevHitOffset );
					}

					// check hitlist
					DWORD uHitlistEntry = 0;
					DWORD uLastPosInField = 0;
					DWORD uLastFieldId = 0;
					bool bLastInFieldFound = false;

					for ( DWORD uHit = 0; uHit < tDoc.m_uHits && pCurHit; uHit++ )
					{
						DWORD uValue = 0;
						pCurHit = UnzipDword ( &uValue, pCurHit );
						if ( pCurHit>pMaxHit )
						{
							tReporter.Fail ( "reading past hitlist end (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );
							break;
						}

						uHitlistEntry += uValue;

						DWORD uPosInField = HITMAN::GetPos ( uHitlistEntry );
						bool bLastInField = HITMAN::IsEnd ( uHitlistEntry );
						DWORD uFieldId = HITMAN::GetField ( uHitlistEntry );

						if ( (int)uFieldId>m_tSchema.GetFieldsCount() || uFieldId>SPH_MAX_FIELDS )
						{
							tReporter.Fail ( "invalid field id in a hitlist (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, total_fields=%d)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, m_tSchema.GetFieldsCount() );
						}

						if ( !( tDoc.m_uDocFields & ( 1 << uFieldId ) ) )
						{
							tReporter.Fail ( "invalid field id: not in doclist mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, field_id=%u, field_mask=%u)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uFieldId, tDoc.m_uDocFields );
						}

						if ( uLastFieldId!=uFieldId )
						{
							bLastInFieldFound = false;
							uLastPosInField = 0;
						}

						if ( uLastPosInField && uPosInField<=uLastPosInField )
						{
							tReporter.Fail ( "hit position in field decreased (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, pos=%u, last_pos=%u)",
								iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, uPosInField, uLastPosInField );
						}

						if ( bLastInField && bLastInFieldFound )
							tReporter.Fail ( "duplicate last-in-field hit found (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u)", iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID );

						uLastPosInField = uPosInField;
						uLastFieldId = uFieldId;
						bLastInFieldFound |= bLastInField;
					}

					uPrevHitOffset = tDoc.m_uHit;
				}

				DWORD uAvailFieldMask = ( 1 << m_tSchema.GetFieldsCount() ) - 1;
				if ( tDoc.m_uDocFields & ~uAvailFieldMask )
				{
					tReporter.Fail ( "wrong document field mask (segment=%d, word=%d, wordid=" UINT64_FMT ", rowid=%u, mask=%u, total_fields=%d",
						iSegment, nWordsRead, (uint64_t)tWord.m_uWordID, tDoc.m_tRowID, tDoc.m_uDocFields, m_tSchema.GetFieldsCount() );
				}

				tPrevRowID = tDoc.m_tRowID;
			}

			uPrevDocOffset = tWord.m_uDoc;
			nWordsRead++;
		}

		if ( pCurDoc!=pMaxDoc )
			tReporter.Fail ( "unused doclist entries found (segment=%d, doclist_size=%d)", iSegment, tSegment.m_dDocs.GetLength() );

		if ( pCurHit!=pMaxHit )
			tReporter.Fail ( "unused hitlist entries found (segment=%d, hitlist_size=%d)", iSegment, tSegment.m_dHits.GetLength() );

		if ( dRefCheckpoints.GetLength()!=tSegment.m_dWordCheckpoints.GetLength() )
			tReporter.Fail ( "word checkpoint count mismatch (read=%d, calc=%d)", tSegment.m_dWordCheckpoints.GetLength(), dRefCheckpoints.GetLength() );

		for ( int i=0; i < Min ( dRefCheckpoints.GetLength(), tSegment.m_dWordCheckpoints.GetLength() ); i++ )
		{
			const RtWordCheckpoint_t & tRefCP = dRefCheckpoints[i];
			const RtWordCheckpoint_t & tCP = tSegment.m_dWordCheckpoints[i];
			const int iLen = m_bKeywordDict ? strlen ( tCP.m_sWord ) : 0;
			if ( m_bKeywordDict && ( !tCP.m_sWord || ( !strlen ( tRefCP.m_sWord ) || !strlen ( tCP.m_sWord ) ) ) )
			{
				tReporter.Fail ( "empty word checkpoint %d ((segment=%d, read_word=%s, read_len=%u, readpos=%d, calc_word=%s, calc_len=%u, calcpos=%d)",
					i, iSegment, tCP.m_sWord, (DWORD)strlen ( tCP.m_sWord ), tCP.m_iOffset,
					tRefCP.m_sWord, (DWORD)strlen ( tRefCP.m_sWord ), tRefCP.m_iOffset );
			} else if ( sphCheckpointCmpStrictly ( tCP.m_sWord, iLen, tCP.m_uWordID, m_bKeywordDict, tRefCP ) || tRefCP.m_iOffset!=tCP.m_iOffset )
			{
				if ( m_bKeywordDict )
				{
					tReporter.Fail ( "word checkpoint %d differs (segment=%d, read_word=%s, readpos=%d, calc_word=%s, calcpos=%d)",
						i, iSegment, tCP.m_sWord, tCP.m_iOffset, tRefCP.m_sWord, tRefCP.m_iOffset );
				} else
				{
					tReporter.Fail ( "word checkpoint %d differs (segment=%d, readid=" UINT64_FMT ", readpos=%d, calcid=" UINT64_FMT ", calcpos=%d)",
						i, iSegment, (uint64_t)tCP.m_uWordID, tCP.m_iOffset, (int64_t)tRefCP.m_uWordID, tRefCP.m_iOffset );
				}
			}
		}

		if ( m_bKeywordDict )
			ARRAY_FOREACH ( i, dRefCheckpoints )
				SafeDeleteArray ( dRefCheckpoints[i].m_sWord );

		dRefCheckpoints.Reset ();

		MemoryDebugCheckReader_c tAttrs ( (const BYTE *)tSegment.m_dRows.begin(), (const BYTE *)tSegment.m_dRows.end() );
		MemoryDebugCheckReader_c tBlobs ( tSegment.m_dBlobs.begin(), tSegment.m_dBlobs.end() );
		DebugCheck_Attributes ( tAttrs, tBlobs, tSegment.m_uRows, 0, m_tSchema, tReporter );
		DebugCheck_DeadRowMap ( tSegment.m_tDeadRowMap.GetLengthBytes(), tSegment.m_uRows, tReporter );

		DWORD uCalcAliveRows = tSegment.m_tDeadRowMap.GetNumAlive();
		if ( tSegment.m_tAliveRows!=uCalcAliveRows )
			tReporter.Fail ( "alive row count mismatch (segment=%d, expected=%u, current=%u)", iSegment, uCalcAliveRows, tSegment.m_tAliveRows.GetValue() );
	}

	int iFailsPlain = 0;
	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		tReporter.Msg ( "checking disk chunk %d(%d)...", i, m_dDiskChunks.GetLength() );
		iFailsPlain += m_dDiskChunks[i]->DebugCheck ( fp );
	}


	tReporter.Done();

	return tReporter.GetNumFails() + iFailsPlain;
} // NOLINT function length

//////////////////////////////////////////////////////////////////////////
// SEARCHING
//////////////////////////////////////////////////////////////////////////


struct RtQwordTraits_t : public ISphQword
{
public:
	virtual bool Setup ( const RtIndex_c * pIndex, int iSegment, const SphChunkGuard_t & tGuard ) = 0;
};


//////////////////////////////////////////////////////////////////////////

struct RtQword_t : public RtQwordTraits_t
{
public:
	RtQword_t ()
	{
		m_tMatch.Reset ( 0 );
	}

	virtual ~RtQword_t ()
	{
	}

	virtual const CSphMatch & GetNextDoc()
	{
		while (true)
		{
			const RtDoc_t * pDoc = m_tDocReader.UnzipDoc();
			if ( !pDoc )
			{
				m_tMatch.m_tRowID = INVALID_ROWID;
				return m_tMatch;
			}

			if ( m_pSeg->m_tDeadRowMap.IsSet ( pDoc->m_tRowID ) )
				continue;

			m_tMatch.m_tRowID = pDoc->m_tRowID;
			m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
			m_uMatchHits = pDoc->m_uHits;
			m_iHitlistPos = (uint64_t(pDoc->m_uHits)<<32) + pDoc->m_uHit;
			m_bAllFieldsKnown = false;
			return m_tMatch;
		}
	}

	virtual void SeekHitlist ( SphOffset_t uOff )
	{
		int iHits = (int)(uOff>>32);
		if ( iHits==1 )
		{
			m_uNextHit = DWORD(uOff);
		} else
		{
			m_uNextHit = 0;
			m_tHitReader.Seek ( DWORD(uOff), iHits );
		}
	}

	virtual Hitpos_t GetNextHit ()
	{
		if ( m_uNextHit==0 )
		{
			return Hitpos_t ( m_tHitReader.UnzipHit() );

		} else if ( m_uNextHit==0xffffffffUL )
		{
			return EMPTY_HIT;

		} else
		{
			DWORD uRes = m_uNextHit;
			m_uNextHit = 0xffffffffUL;
			return Hitpos_t ( uRes );
		}
	}

	virtual bool Setup ( const RtIndex_c * pIndex, int iSegment, const SphChunkGuard_t & tGuard )
	{
		return pIndex->RtQwordSetup ( this, iSegment, tGuard );
	}

	void SetupReader ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pSeg = pSeg;
		m_tDocReader = RtDocReader_t ( pSeg, tWord );
		m_tHitReader.m_pBase = pSeg->m_dHits.Begin();
	}

private:
	RtDocReader_t		m_tDocReader;
	CSphMatch			m_tMatch;

	DWORD				m_uNextHit {0};
	RtHitReader2_t		m_tHitReader;

	const RtSegment_t * m_pSeg {nullptr};
};


//////////////////////////////////////////////////////////////////////////


struct RtSubstringPayload_t : public ISphSubstringPayload
{
	RtSubstringPayload_t ( int iSegmentCount, int iDoclists )
		: m_dSegment2Doclists ( iSegmentCount )
		, m_dDoclist ( iDoclists )
	{}
	CSphFixedVector<Slice_t>	m_dSegment2Doclists;
	CSphFixedVector<Slice_t>	m_dDoclist;
};


struct RtQwordPayload_t : public RtQwordTraits_t
{
public:
	explicit RtQwordPayload_t ( const RtSubstringPayload_t * pPayload )
		: m_pPayload ( pPayload )
	{
		m_tMatch.Reset ( 0 );
		m_iDocs = m_pPayload->m_iTotalDocs;
		m_iHits = m_pPayload->m_iTotalHits;

		m_uDoclist = 0;
		m_uDoclistLeft = 0;
		m_pSegment = NULL;
		m_uHitEmbeded = EMPTY_HIT;
	}

	virtual ~RtQwordPayload_t ()
	{}

	virtual const CSphMatch & GetNextDoc()
	{
		m_iHits = 0;
		while (true)
		{
			const RtDoc_t * pDoc = m_tDocReader.UnzipDoc();
			if ( !pDoc && !m_uDoclistLeft )
			{
				m_tMatch.m_tRowID = INVALID_ROWID;
				return m_tMatch;
			}

			if ( !pDoc && m_uDoclistLeft )
			{
				SetupReader();
				pDoc = m_tDocReader.UnzipDoc();
				assert ( pDoc );
			}

			if ( m_pSegment->m_tDeadRowMap.IsSet ( pDoc->m_tRowID ) )
				continue;

			m_tMatch.m_tRowID = pDoc->m_tRowID;
			m_dQwordFields.Assign32 ( pDoc->m_uDocFields );
			m_bAllFieldsKnown = false;

			m_iHits = pDoc->m_uHits;
			m_uHitEmbeded = pDoc->m_uHit;
			m_tHitReader = RtHitReader_t ( m_pSegment, pDoc );

			return m_tMatch;
		}
	}

	virtual void SeekHitlist ( SphOffset_t )
	{}

	virtual Hitpos_t GetNextHit ()
	{
		if ( m_iHits>1 )
			return Hitpos_t ( m_tHitReader.UnzipHit() );
		else if ( m_iHits==1 )
		{
			Hitpos_t tHit ( m_uHitEmbeded );
			m_uHitEmbeded = EMPTY_HIT;
			return tHit;
		} else
		{
			return EMPTY_HIT;
		}
	}

	virtual bool Setup ( const RtIndex_c *, int iSegment, const SphChunkGuard_t & tGuard )
	{
		m_uDoclist = 0;
		m_uDoclistLeft = 0;
		m_tDocReader = RtDocReader_t();
		m_pSegment = NULL;

		if ( iSegment<0 )
			return false;

		m_pSegment = tGuard.m_dRamChunks[iSegment];
		m_uDoclist = m_pPayload->m_dSegment2Doclists[iSegment].m_uOff;
		m_uDoclistLeft = m_pPayload->m_dSegment2Doclists[iSegment].m_uLen;

		if ( !m_uDoclistLeft )
			return false;

		SetupReader();
		return true;
	}

private:
	void SetupReader ()
	{
		assert ( m_uDoclistLeft );
		RtWord_t tWord;
		tWord.m_uDoc = m_pPayload->m_dDoclist[m_uDoclist].m_uOff;
		tWord.m_uDocs = m_pPayload->m_dDoclist[m_uDoclist].m_uLen;
		m_tDocReader = RtDocReader_t ( m_pSegment, tWord );
		m_uDoclist++;
		m_uDoclistLeft--;
	}

	const RtSubstringPayload_t *	m_pPayload;
	CSphMatch					m_tMatch;
	RtDocReader_t				m_tDocReader;
	RtHitReader_t				m_tHitReader;
	const RtSegment_t *			m_pSegment;

	DWORD						m_uDoclist;
	DWORD						m_uDoclistLeft;
	DWORD						m_uHitEmbeded;
};


//////////////////////////////////////////////////////////////////////////

class RtQwordSetup_t : public ISphQwordSetup
{
public:
	explicit RtQwordSetup_t ( const SphChunkGuard_t & tGuard );
	virtual ISphQword *	QwordSpawn ( const XQKeyword_t & ) const final;
	virtual bool		QwordSetup ( ISphQword * pQword ) const final;
	void				SetSegment ( int iSegment ) { m_iSeg = iSegment; }

private:
	const SphChunkGuard_t & m_tGuard;
	int					m_iSeg;
};


RtQwordSetup_t::RtQwordSetup_t ( const SphChunkGuard_t & tGuard )
	: m_tGuard ( tGuard )
	, m_iSeg ( -1 )
{ }


ISphQword * RtQwordSetup_t::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	if ( !tWord.m_pPayload )
		return new RtQword_t ();
	else
		return new RtQwordPayload_t ( (const RtSubstringPayload_t *)tWord.m_pPayload );
}


bool RtQwordSetup_t::QwordSetup ( ISphQword * pQword ) const
{
	// there was two dynamic_casts here once but they're not necessary
	// maybe it's worth to rewrite class hierarchy to avoid c-casts here?
	RtQwordTraits_t * pMyWord = (RtQwordTraits_t*)pQword;
	const RtIndex_c * pIndex = (const RtIndex_c *)m_pIndex;
	return pMyWord->Setup ( pIndex, m_iSeg, m_tGuard );
}


bool RtIndex_c::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	auto pSegment = (RtSegment_t*)pCtx->m_pIndexData;
	tMatch.m_pStatic = pSegment->GetDocinfoByRowID ( tMatch.m_tRowID );

	pCtx->CalcFilter ( tMatch );
	if ( !pCtx->m_pFilter )
		return false;

	if ( !pCtx->m_pFilter->Eval ( tMatch ) )
	{
		pCtx->FreeDataFilter ( tMatch );
		return true;
	}
	return false;
}


// WARNING, setup is pretty tricky
// for RT queries, we setup qwords several times
// first pass (with NULL segment arg) should sum all stats over all segments
// others passes (with non-NULL segments) should setup specific segment (including local stats)
bool RtIndex_c::RtQwordSetupSegment ( RtQword_t * pQword, const RtSegment_t * pCurSeg, bool bSetup, bool bWordDict, int iWordsCheckpoint, const CSphIndexSettings & tSettings )
{
	if ( !pCurSeg )
		return false;

	SphWordID_t uWordID = pQword->m_uWordID;
	const char * sWord = pQword->m_sDictWord.cstr();
	int iWordLen = pQword->m_sDictWord.Length();
	bool bPrefix = false;
	if ( bWordDict && iWordLen && sWord[iWordLen-1]=='*' ) // crc star search emulation
	{
		iWordLen = iWordLen-1;
		bPrefix = true;
	}

	if ( !iWordLen )
		return false;

	// prevent prefix matching for explicitly setting prohibited by config, to be on pair with plain index (or CRC kind of index)
	if ( bPrefix && ( ( tSettings.m_iMinPrefixLen && iWordLen<tSettings.m_iMinPrefixLen ) || ( tSettings.m_iMinInfixLen && iWordLen<tSettings.m_iMinInfixLen ) ) )
		return false;

	// no checkpoints - check all words
	// no checkpoints matched - check only words prior to 1st checkpoint
	// checkpoint found - check words at that checkpoint
	RtWordReader_t tReader ( pCurSeg, bWordDict, iWordsCheckpoint );

	if ( pCurSeg->m_dWordCheckpoints.GetLength() )
	{
		const RtWordCheckpoint_t * pCp = sphSearchCheckpoint ( sWord, iWordLen, uWordID, false, bWordDict
			, pCurSeg->m_dWordCheckpoints.Begin(), &pCurSeg->m_dWordCheckpoints.Last() );

		const BYTE * pWords = pCurSeg->m_dWords.Begin();

		if ( !pCp )
		{
			tReader.m_pMax = pWords + pCurSeg->m_dWordCheckpoints.Begin()->m_iOffset;
		} else
		{
			tReader.m_pCur = pWords + pCp->m_iOffset;
			// if next checkpoint exists setup reader range
			if ( ( pCp+1 )<= ( &pCurSeg->m_dWordCheckpoints.Last() ) )
				tReader.m_pMax = pWords + pCp[1].m_iOffset;
		}
	}

	// find the word between checkpoints
	const RtWord_t * pWord = NULL;
	while ( ( pWord = tReader.UnzipWord() )!=NULL )
	{
		int iCmp = 0;
		if ( bWordDict )
		{
			iCmp = sphDictCmpStrictly ( (const char *)pWord->m_sWord+1, pWord->m_sWord[0], sWord, iWordLen );
		} else
		{
			if ( pWord->m_uWordID<uWordID )
				iCmp = -1;
			else if ( pWord->m_uWordID>uWordID )
				iCmp = 1;
		}

		if ( iCmp==0 )
		{
			pQword->m_iDocs += pWord->m_uDocs;
			pQword->m_iHits += pWord->m_uHits;
			if ( bSetup )
				pQword->SetupReader ( pCurSeg, *pWord );

			return true;

		} else if ( iCmp>0 )
			return false;
	}
	return false;
}

struct RtExpandedEntry_t
{
	DWORD	m_uHash;
	int		m_iNameOff;
	int		m_iDocs;
	int		m_iHits;
};

struct RtExpandedPayload_t
{
	int		m_iDocs;
	int		m_iHits;
	DWORD	m_uDoclistOff;
};

struct RtExpandedTraits_fn
{
	inline bool IsLess ( const RtExpandedEntry_t & a, const RtExpandedEntry_t & b ) const
	{
		assert ( m_sBase );
		if ( a.m_uHash!=b.m_uHash )
		{
			return a.m_uHash<b.m_uHash;
		} else
		{
			const BYTE * pA = m_sBase + a.m_iNameOff;
			const BYTE * pB = m_sBase + b.m_iNameOff;
			if ( pA[0]!=pB[0] )
				return pA[0]<pB[0];

			return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )<0 );
		}
	}

	inline bool IsEqual ( const RtExpandedEntry_t * a, const RtExpandedEntry_t * b ) const
	{
		assert ( m_sBase );
		if ( a->m_uHash!=b->m_uHash )
			return false;

		const BYTE * pA = m_sBase + a->m_iNameOff;
		const BYTE * pB = m_sBase + b->m_iNameOff;
		if ( pA[0]!=pB[0] )
			return false;

		return ( sphDictCmp ( (const char *)pA+1, pA[0], (const char *)pB+1, pB[0] )==0 );
	}

	explicit RtExpandedTraits_fn ( const BYTE * sBase )
		: m_sBase ( sBase )
	{ }
	const BYTE * m_sBase;
};


struct DictEntryRtPayload_t
{
	DictEntryRtPayload_t ( bool bPayload, int iSegments )
	{
		m_bPayload = bPayload;
		m_iSegExpansionLimit = iSegments;
		if ( bPayload )
		{
			m_dWordPayload.Reserve ( 1000 );
			m_dSeg.Resize ( iSegments );
			ARRAY_FOREACH ( i, m_dSeg )
			{
				m_dSeg[i].m_uOff = 0;
				m_dSeg[i].m_uLen = 0;
			}
		}

		m_dWordExpand.Reserve ( 1000 );
		m_dWordBuf.Reserve ( 8096 );
	}

	void Add ( const RtWord_t * pWord, int iSegment )
	{
		if ( !m_bPayload || !sphIsExpandedPayload ( pWord->m_uDocs, pWord->m_uHits ) )
		{
			RtExpandedEntry_t & tExpand = m_dWordExpand.Add();

			int iOff = m_dWordBuf.GetLength();
			int iWordLen = pWord->m_sWord[0] + 1;
			tExpand.m_uHash = sphCRC32 ( pWord->m_sWord, iWordLen );
			tExpand.m_iNameOff = iOff;
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			m_dWordBuf.Append ( pWord->m_sWord, iWordLen );
		} else
		{
			RtExpandedPayload_t & tExpand = m_dWordPayload.Add();
			tExpand.m_iDocs = pWord->m_uDocs;
			tExpand.m_iHits = pWord->m_uHits;
			tExpand.m_uDoclistOff = pWord->m_uDoc;

			m_dSeg[iSegment].m_uOff = m_dWordPayload.GetLength();
			m_dSeg[iSegment].m_uLen++;
		}
	}

	void Convert ( ISphWordlist::Args_t & tArgs )
	{
		if ( !m_dWordExpand.GetLength() && !m_dWordPayload.GetLength() )
			return;

		int iTotalDocs = 0;
		int iTotalHits = 0;
		if ( m_dWordExpand.GetLength() )
		{
			int iRtExpansionLimit = tArgs.m_iExpansionLimit * m_iSegExpansionLimit;
			if ( tArgs.m_iExpansionLimit && m_dWordExpand.GetLength()>iRtExpansionLimit )
			{
				// sort expansions by frequency desc
				// clip the less frequent ones if needed, as they are likely misspellings
				sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), ExpandedOrderDesc_T<RtExpandedEntry_t>() );
				m_dWordExpand.Resize ( iRtExpansionLimit );
			}

			// lets merge statistics for same words from different segments as hash produce a lot tiny allocations here
			const BYTE * sBase = m_dWordBuf.Begin();
			RtExpandedTraits_fn fnCmp ( sBase );
			sphSort ( m_dWordExpand.Begin(), m_dWordExpand.GetLength(), fnCmp );

			const RtExpandedEntry_t * pLast = m_dWordExpand.Begin();
			tArgs.AddExpanded ( sBase+pLast->m_iNameOff+1, sBase[pLast->m_iNameOff], pLast->m_iDocs, pLast->m_iHits );
			for ( int i=1; i<m_dWordExpand.GetLength(); i++ )
			{
				const RtExpandedEntry_t * pCur = m_dWordExpand.Begin() + i;

				if ( fnCmp.IsEqual ( pLast, pCur ) )
				{
					tArgs.m_dExpanded.Last().m_iDocs += pCur->m_iDocs;
					tArgs.m_dExpanded.Last().m_iHits += pCur->m_iHits;
				} else
				{
					tArgs.AddExpanded ( sBase + pCur->m_iNameOff + 1, sBase[pCur->m_iNameOff], pCur->m_iDocs, pCur->m_iHits );
					pLast = pCur;
				}
				iTotalDocs += pCur->m_iDocs;
				iTotalHits += pCur->m_iHits;
			}
		}

		if ( m_dWordPayload.GetLength() )
		{
			DWORD uExpansionLimit = tArgs.m_iExpansionLimit;
			int iPayloads = 0;
			ARRAY_FOREACH ( i, m_dSeg )
			{
				Slice_t & tSeg = m_dSeg[i];

				// reverse per segment offset to payload doc-list as offset was the end instead of start
				assert ( tSeg.m_uOff>=tSeg.m_uLen );
				tSeg.m_uOff = tSeg.m_uOff - tSeg.m_uLen;

				// per segment expansion limit clip
				if ( uExpansionLimit && tSeg.m_uLen>uExpansionLimit )
				{
					// sort expansions by frequency desc
					// per segment clip the less frequent ones if needed, as they are likely misspellings
					sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen, ExpandedOrderDesc_T<RtExpandedPayload_t>() );
					tSeg.m_uLen = uExpansionLimit;
				}

				iPayloads += tSeg.m_uLen;
				// sort by ascending doc-list offset
				sphSort ( m_dWordPayload.Begin()+tSeg.m_uOff, tSeg.m_uLen, bind ( &RtExpandedPayload_t::m_uDoclistOff ) );
			}

			auto * pPayload = new RtSubstringPayload_t ( m_dSeg.GetLength(), iPayloads );

			Slice_t * pDst = pPayload->m_dDoclist.Begin();
			ARRAY_FOREACH ( i, m_dSeg )
			{
				const Slice_t & tSeg = m_dSeg[i];
				const RtExpandedPayload_t * pSrc = m_dWordPayload.Begin() + tSeg.m_uOff;
				const RtExpandedPayload_t * pEnd = pSrc + tSeg.m_uLen;
				pPayload->m_dSegment2Doclists[i].m_uOff = pDst - pPayload->m_dDoclist.Begin();
				pPayload->m_dSegment2Doclists[i].m_uLen = tSeg.m_uLen;
				while ( pSrc!=pEnd )
				{
					pDst->m_uOff = pSrc->m_uDoclistOff;
					pDst->m_uLen = pSrc->m_iDocs;
					iTotalDocs += pSrc->m_iDocs;
					iTotalHits += pSrc->m_iHits;
					pDst++;
					pSrc++;
				}
			}
			pPayload->m_iTotalDocs = iTotalDocs;
			pPayload->m_iTotalHits = iTotalHits;
			tArgs.m_pPayload = pPayload;
		}

		tArgs.m_iTotalDocs = iTotalDocs;
		tArgs.m_iTotalHits = iTotalHits;
	}

	bool							m_bPayload;
	CSphVector<RtExpandedEntry_t>	m_dWordExpand;
	CSphVector<RtExpandedPayload_t>	m_dWordPayload;
	CSphVector<BYTE>				m_dWordBuf;
	CSphVector<Slice_t>				m_dSeg;
	int								m_iSegExpansionLimit = 0;
};


void RtIndex_c::GetPrefixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
	int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : NULL;

	const CSphFixedVector<RtSegment_t*> & dSegments = *((CSphFixedVector<RtSegment_t*> *)tArgs.m_pIndexData);
	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	const int iSkipMagic = ( BYTE(*sSubstring)<0x20 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pCurSeg = dSegments[iSeg];
		RtWordReader_t tReader ( pCurSeg, true, m_iWordsCheckpoint );

		// find initial checkpoint or check words prior to 1st checkpoint
		if ( pCurSeg->m_dWordCheckpoints.GetLength() )
		{
			const RtWordCheckpoint_t * pCurCheckpoint = sphSearchCheckpoint ( sSubstring, iSubLen, 0, true, true
				, pCurSeg->m_dWordCheckpoints.Begin(), &pCurSeg->m_dWordCheckpoints.Last() );

			if ( pCurCheckpoint )
			{
				// there could be valid data prior 1st checkpoint that should be unpacked and checked
				int iCheckpointNameLen = strlen ( pCurCheckpoint->m_sWord );
				if ( pCurCheckpoint!=pCurSeg->m_dWordCheckpoints.Begin()
					|| ( sphDictCmp ( sSubstring, iSubLen, pCurCheckpoint->m_sWord, iCheckpointNameLen )==0 && iSubLen==iCheckpointNameLen ) )
				{
					tReader.m_pCur = pCurSeg->m_dWords.Begin() + pCurCheckpoint->m_iOffset;
				}
			}
		}

		// find the word between checkpoints
		const RtWord_t * pWord = NULL;
		while ( ( pWord = tReader.UnzipWord() )!=NULL )
		{
			int iCmp = sphDictCmp ( sSubstring, iSubLen, (const char *)pWord->m_sWord+1, pWord->m_sWord[0] );
			if ( iCmp<0 )
			{
				break;
			} else if ( iCmp==0 && iSubLen<=pWord->m_sWord[0] && sphWildcardMatch ( (const char *)pWord->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
			{
				tDict2Payload.Add ( pWord, iSeg );
			}
			// FIXME!!! same case 'boxi*' matches 'box' document at plain index
			// but masked by a checkpoint search
		}
	}

	tDict2Payload.Convert ( tArgs );
}


bool ExtractInfixCheckpoints ( const char * sInfix, int iBytes, int iMaxCodepointLength, int iDictCpCount, const CSphTightVector<uint64_t> & dFilter, CSphVector<DWORD> & dCheckpoints )
{
	if ( !dFilter.GetLength() )
		return false;

	int iStart = dCheckpoints.GetLength();

	uint64_t dVals[ BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT ];
	memset ( dVals, 0, sizeof(dVals) );

	BloomGenTraits_t tBloom0 ( dVals );
	BloomGenTraits_t tBloom1 ( dVals+BLOOM_PER_ENTRY_VALS_COUNT );
	if ( !BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_0, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom0 ) )
		return false;
	BuildBloom ( (const BYTE *)sInfix, iBytes, BLOOM_NGRAM_1, ( iMaxCodepointLength>1 ), BLOOM_PER_ENTRY_VALS_COUNT, tBloom1 );

	for ( int iDictCp=0; iDictCp<iDictCpCount+1; iDictCp++ )
	{
		const uint64_t * pCP = dFilter.Begin() + iDictCp * BLOOM_PER_ENTRY_VALS_COUNT * BLOOM_HASHES_COUNT;
		const uint64_t * pSuffix = dVals;

		bool bMatched = true;
		for ( int iElem=0; iElem<BLOOM_PER_ENTRY_VALS_COUNT*BLOOM_HASHES_COUNT; iElem++ )
		{
			uint64_t uFilter = *pCP++;
			uint64_t uSuffix = *pSuffix++;
			if ( ( uFilter & uSuffix )!=uSuffix )
			{
				bMatched = false;
				break;
			}
		}

		if ( bMatched )
			dCheckpoints.Add ( (DWORD)iDictCp );
	}

	return ( dCheckpoints.GetLength()!=iStart );
}


void RtIndex_c::GetInfixedWords ( const char * sSubstring, int iSubLen, const char * sWildcard, Args_t & tArgs ) const
{
	// sanity checks
	if ( !sSubstring || iSubLen<=0 )
		return;

	// find those prefixes
	CSphVector<DWORD> dPoints;
	const int iSkipMagic = ( tArgs.m_bHasExactForms ? 1 : 0 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	const CSphFixedVector<RtSegment_t*> & dSegments = *((CSphFixedVector<RtSegment_t*> *)tArgs.m_pIndexData);

	DictEntryRtPayload_t tDict2Payload ( tArgs.m_bPayload, dSegments.GetLength() );
	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength() )
			continue;

		dPoints.Resize ( 0 );
		if ( !ExtractInfixCheckpoints ( sSubstring, iSubLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dPoints ) )
			continue;

		int dWildcard [ SPH_MAX_WORD_LEN + 1 ];
		int * pWildcard = ( sphIsUTF8 ( sWildcard ) && sphUTF8ToWideChar ( sWildcard, dWildcard, SPH_MAX_WORD_LEN ) ) ? dWildcard : NULL;

		// walk those checkpoints, check all their words
		ARRAY_FOREACH ( i, dPoints )
		{
			int iNext = (int)dPoints[i];
			int iCur = iNext-1;
			RtWordReader_t tReader ( pSeg, true, m_iWordsCheckpoint );
			if ( iCur>0 )
				tReader.m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
			if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
				tReader.m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;

			const RtWord_t * pWord = NULL;
			while ( ( pWord = tReader.UnzipWord() )!=NULL )
			{
				if ( tArgs.m_bHasExactForms && pWord->m_sWord[1]!=MAGIC_WORD_HEAD_NONSTEMMED )
					continue;

				// check it
				if ( !sphWildcardMatch ( (const char*)pWord->m_sWord+1+iSkipMagic, sWildcard, pWildcard ) )
					continue;

				// matched, lets add
				tDict2Payload.Add ( pWord, iSeg );
			}
		}
	}

	tDict2Payload.Convert ( tArgs );
}

void RtIndex_c::GetSuggest ( const SuggestArgs_t & tArgs, SuggestResult_t & tRes ) const
{
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	const CSphFixedVector<const RtSegment_t*> & dSegments = tGuard.m_dRamChunks;

	// segments and disk chunks dictionaries produce duplicated entries
	tRes.m_bMergeWords = true;

	if ( dSegments.GetLength() )
	{
		assert ( !tRes.m_pWordReader && !tRes.m_pSegments );
		tRes.m_pWordReader = new RtWordReader_t ( dSegments[0], true, m_iWordsCheckpoint );
		tRes.m_pSegments = &tGuard.m_dRamChunks;
		tRes.m_bHasExactDict = m_tSettings.m_bIndexExactWords;

		// FIXME!!! cache InfixCodepointBytes as it is slow - GetMaxCodepointLength is charset_table traverse
		sphGetSuggest ( this, m_pTokenizer->GetMaxCodepointLength(), tArgs, tRes );

		auto pReader = ( RtWordReader_t * ) tRes.m_pWordReader;
		SafeDelete ( pReader );
		tRes.m_pWordReader = NULL;
		tRes.m_pSegments = NULL;
	}

	int iWorstCount = 0;
	// check disk chunks from recent to oldest
	for ( int i=tGuard.m_dDiskChunks.GetLength()-1; i>=0; i-- )
	{
		int iWorstDist = 0;
		int iWorstDocs = 0;
		if ( tRes.m_dMatched.GetLength() )
		{
			iWorstDist = tRes.m_dMatched.Last().m_iDistance;
			iWorstDocs = tRes.m_dMatched.Last().m_iDocs;
		}

		tGuard.m_dDiskChunks[i]->GetSuggest ( tArgs, tRes );

		// stop checking in case worst element is same several times during loop
		if ( tRes.m_dMatched.GetLength() && iWorstDist==tRes.m_dMatched.Last().m_iDistance && iWorstDocs==tRes.m_dMatched.Last().m_iDocs )
		{
			iWorstCount++;
			if ( iWorstCount>2 )
				break;
		} else
		{
			iWorstCount = 0;
		}
	}
}

void RtIndex_c::SuffixGetChekpoints ( const SuggestResult_t & tRes, const char * sSuffix, int iLen, CSphVector<DWORD> & dCheckpoints ) const
{
	const CSphFixedVector<const RtSegment_t*> & dSegments = *( (const CSphFixedVector<const RtSegment_t*> *)tRes.m_pSegments );
	assert ( dSegments.GetLength()<0xFF );

	ARRAY_FOREACH ( iSeg, dSegments )
	{
		const RtSegment_t * pSeg = dSegments[iSeg];
		if ( !pSeg->m_dWords.GetLength () )
			continue;

		int iStart = dCheckpoints.GetLength();
		if ( !ExtractInfixCheckpoints ( sSuffix, iLen, m_iMaxCodepointLength, pSeg->m_dWordCheckpoints.GetLength(), pSeg->m_dInfixFilterCP, dCheckpoints ) )
			continue;

		DWORD iSegPacked = (DWORD)iSeg<<24;
		for ( int i=iStart; i<dCheckpoints.GetLength(); i++ )
		{
			assert ( ( dCheckpoints[i] & 0xFFFFFF )==dCheckpoints[i] );
			dCheckpoints[i] |= iSegPacked;
		}
	}
}

void RtIndex_c::SetCheckpoint ( SuggestResult_t & tRes, DWORD iCP ) const
{
	assert ( tRes.m_pWordReader && tRes.m_pSegments );
	const CSphFixedVector<const RtSegment_t*> & dSegments = *( (const CSphFixedVector<const RtSegment_t*> *)tRes.m_pSegments );
	RtWordReader_t * pReader = (RtWordReader_t *)tRes.m_pWordReader;

	int iSeg = iCP>>24;
	assert ( iSeg>=0 && iSeg<dSegments.GetLength() );
	const RtSegment_t * pSeg = dSegments[iSeg];
	pReader->Reset ( pSeg );

	int iNext = (int)( iCP & 0xFFFFFF );
	int iCur = iNext-1;

	if ( iCur>0 )
		pReader->m_pCur = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iCur].m_iOffset;
	if ( iNext<pSeg->m_dWordCheckpoints.GetLength() )
		pReader->m_pMax = pSeg->m_dWords.Begin() + pSeg->m_dWordCheckpoints[iNext].m_iOffset;
}

bool RtIndex_c::ReadNextWord ( SuggestResult_t & tRes, DictWord_t & tWord ) const
{
	assert ( tRes.m_pWordReader );
	RtWordReader_t * pReader = (RtWordReader_t *)tRes.m_pWordReader;

	const RtWord_t * pWord = pReader->UnzipWord();

	if ( !pWord )
		return false;

	tWord.m_sWord = (const char *)( pWord->m_sWord + 1 );
	tWord.m_iLen = pWord->m_sWord[0];
	tWord.m_iDocs = pWord->m_uDocs;
	return true;
}


bool RtIndex_c::RtQwordSetup ( RtQword_t * pQword, int iSeg, const SphChunkGuard_t & tGuard ) const
{
	// segment-specific setup pass
	if ( iSeg>=0 )
		return RtQwordSetupSegment ( pQword, tGuard.m_dRamChunks[iSeg], true, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings );

	// stat-only pass
	// loop all segments, gather stats, do not setup anything
	pQword->m_iDocs = 0;
	pQword->m_iHits = 0;
	if ( !tGuard.m_dRamChunks.GetLength() )
		return true;

	// we care about the results anyway though
	// because if all (!) segments miss this word, we must notify the caller, right?
	bool bFound = false;
	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
		bFound |= RtQwordSetupSegment ( pQword, tGuard.m_dRamChunks[i], false, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings );

	// sanity check
	assert (!( bFound==true && pQword->m_iDocs==0 ) );
	return bFound;
}


bool RtIndex_c::IsStarDict() const
{
	return m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0;
}


void SetupExactDict ( CSphDictRefPtr_c &pDict, ISphTokenizer * pTokenizer, bool bAddSpecial )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '=' );
	if ( bAddSpecial )
		pTokenizer->AddSpecials ( "=" );
	pDict = new CSphDictExact ( pDict );
}


void SetupStarDict ( CSphDictRefPtr_c& pDict, ISphTokenizer * pTokenizer )
{
	assert ( pTokenizer );
	pTokenizer->AddPlainChar ( '*' );
	pDict = new CSphDictStarV8 ( pDict, true );
}

struct SphRtFinalMatchCalc_t : ISphMatchProcessor, ISphNoncopyable // fixme! that is actually class, not struct.
{
private:
	const CSphQueryContext &	m_tCtx;
	int							m_iSeg;
	int							m_iSegments;
	// count per segments matches
	// to skip iteration of matches at sorter and pool setup for segment without matches at sorter
	CSphBitvec					m_dSegments;

public:
	SphRtFinalMatchCalc_t ( int iSegments, const CSphQueryContext & tCtx )
		: m_tCtx ( tCtx )
		, m_iSeg ( 0 )
		, m_iSegments ( iSegments )
	{
		m_dSegments.Init ( iSegments );
	}

	bool NextSegment ( int iSeg )
	{
		m_iSeg = iSeg;

		bool bSegmentGotRows = m_dSegments.BitGet ( iSeg );

		// clear current row
		m_dSegments.BitClear ( iSeg );

		// also clear 0 segment as it got forced to process
		m_dSegments.BitClear ( 0 );

		// also force to process 0 segment to mark all used segments
		return ( iSeg==0 || bSegmentGotRows );
	}

	bool HasSegments () const
	{
		return ( m_iSeg==0 || m_dSegments.BitCount()>0 );
	}

	void Process ( CSphMatch * pMatch ) final
	{
		int iMatchSegment = pMatch->m_iTag-1;
		if ( iMatchSegment==m_iSeg && pMatch->m_pStatic )
			m_tCtx.CalcFinal ( *pMatch );

		// count all used segments at 0 pass
		if ( m_iSeg==0 && iMatchSegment<m_iSegments )
			m_dSegments.BitSet ( iMatchSegment );
	}
};


class RTMatchesToNewSchema_c : public MatchesToNewSchema_c
{
public:
	RTMatchesToNewSchema_c ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema, const SphChunkGuard_t & tGuard, const CSphVector<const BYTE *> & dDiskBlobPools )
		: MatchesToNewSchema_c ( pOldSchema, pNewSchema )
		, m_tGuard ( tGuard )
		, m_dDiskBlobPools ( dDiskBlobPools )
	{}

private:
	const SphChunkGuard_t &				m_tGuard;
	const CSphVector<const BYTE *> &	m_dDiskBlobPools;

	virtual const BYTE * GetBlobPool ( const CSphMatch * pMatch ) override
	{
		int nRamChunks = m_tGuard.m_dRamChunks.GetLength();
		int iChunkId = pMatch->m_iTag-1;
		if ( iChunkId < nRamChunks )
			return m_tGuard.m_dRamChunks[iChunkId]->m_dBlobs.Begin();

		return m_dDiskBlobPools[iChunkId-nRamChunks];
	}
};


static void TransformSorterSchema ( ISphMatchSorter * pSorter, const SphChunkGuard_t & tGuard, const CSphVector<const BYTE *> & dDiskBlobPools )
{
	assert ( pSorter );

	const ISphSchema * pOldSchema = pSorter->GetSchema();
	ISphSchema * pNewSchema =  sphCreateStandaloneSchema ( pOldSchema );
	assert ( pOldSchema && pNewSchema );

	RTMatchesToNewSchema_c fnFinal ( pOldSchema, pNewSchema, tGuard, dDiskBlobPools );
	pSorter->Finalize ( fnFinal, false );

	pSorter->SetSchema ( pNewSchema, true );
	SafeDelete ( pOldSchema );
}


void RtIndex_c::GetReaderChunks ( SphChunkGuard_t & tGuard ) const NO_THREAD_SAFETY_ANALYSIS
{
	if ( m_dRamChunks.IsEmpty() && m_dDiskChunks.IsEmpty() )
		return;

	m_tReading.ReadLock();
	tGuard.m_pReading = &m_tReading;

	ScRL_t tChunkRLock ( m_tChunkLock );

	tGuard.m_dRamChunks.CopyFrom ( m_dRamChunks );
	tGuard.m_dDiskChunks.CopyFrom ( m_dDiskChunks );

	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
	{
		assert ( tGuard.m_dRamChunks[i]->m_tRefCount.GetValue()>=0 );
		tGuard.m_dRamChunks[i]->m_tRefCount.Inc();
	}

}


SphChunkGuard_t::~SphChunkGuard_t()
{
	if ( m_pReading )
		m_pReading->Unlock();

	if ( !m_dRamChunks.GetLength() )
		return;

	ARRAY_FOREACH ( i, m_dRamChunks )
	{
		assert ( m_dRamChunks[i]->m_tRefCount.GetValue()>=1 );
		m_dRamChunks[i]->m_tRefCount.Dec();
	}
}


// FIXME! missing MVA, index_exact_words support
// FIXME? any chance to factor out common backend agnostic code?
// FIXME? do we need to support pExtraFilters?
bool RtIndex_c::MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters,
	ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	assert ( ppSorters );
	assert ( pResult );

	// to avoid the checking of a ppSorters's element for NULL on every next step, just filter out all nulls right here
	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( iSorters );
	for ( int i=0; i<iSorters; ++i )
		if ( ppSorters[i] )
			dSorters.Add ( ppSorters[i] );

	// if we have anything to work with
	if ( dSorters.GetLength()==0 )
	{
		pResult->m_iQueryTime = 0;
		return false;
	}

	assert ( pQuery );
	assert ( tArgs.m_iTag==0 );

	MEMORY ( MEM_RT_QUERY );

	// start counting
	pResult->m_iQueryTime = 0;
	int64_t tmQueryStart = sphMicroTimer();
	CSphQueryProfile * pProfiler = pResult->m_pProfile;
	ESphQueryState eOldState = SPH_QSTATE_UNKNOWN;

	if ( pProfiler )
		eOldState = pProfiler->Switch ( SPH_QSTATE_DICT_SETUP );

	// force ext2 mode for them
	// FIXME! eliminate this const breakage
	const_cast<CSphQuery*> ( pQuery )->m_eMode = SPH_MATCH_EXTENDED2;

	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	// wrappers
	ISphTokenizerRefPtr_c pQueryTokenizer { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
	sphSetupQueryTokenizer ( pQueryTokenizer, IsStarDict(), m_tSettings.m_bIndexExactWords, false );

	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( m_bKeywordDict && IsStarDict () )
		SetupStarDict ( pDict, pQueryTokenizer );

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pQueryTokenizer );

	// calculate local idf for RT with disk chunks
	// in case of local_idf set but no external hash no full-scan query and RT has disk chunks
	const SmallStringHash_T<int64_t> * pLocalDocs = tArgs.m_pLocalDocs;
	SmallStringHash_T<int64_t> hLocalDocs;
	int64_t iTotalDocs = ( tArgs.m_iTotalDocs ? tArgs.m_iTotalDocs : m_tStats.m_iTotalDocuments );
	bool bGotLocalDF = tArgs.m_bLocalDF;
	if ( tArgs.m_bLocalDF && !tArgs.m_pLocalDocs && !pQuery->m_sQuery.IsEmpty() && tGuard.m_dDiskChunks.GetLength() )
	{
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_LOCAL_DF );

		GetKeywordsSettings_t tSettings;
		tSettings.m_bStats = true;
		CSphVector < CSphKeywordInfo > dKeywords;
		DoGetKeywords ( dKeywords, pQuery->m_sQuery.cstr(), tSettings, false, NULL, tGuard );
		for ( auto & tKw : dKeywords )
			if ( !hLocalDocs.Exists ( tKw.m_sNormalized ) ) // skip dupes
				hLocalDocs.Add ( tKw.m_iDocs, tKw.m_sNormalized );

		pLocalDocs = &hLocalDocs;
		iTotalDocs = GetStats().m_iTotalDocuments;
		bGotLocalDF = true;
	}

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// FIXME! each result will point to its own MVA and string pools

	//////////////////////
	// search disk chunks
	//////////////////////

	pResult->m_bHasPrediction = pQuery->m_iMaxPredictedMsec>0;

	SphWordStatChecker_t tDiskStat;
	SphWordStatChecker_t tStat;
	tStat.Set ( pResult->m_hWordStats );

	int64_t tmMaxTimer = 0;
	if ( pQuery->m_uMaxQueryMsec>0 )
		tmMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time

	CSphVector<const BYTE *> dDiskBlobPools ( tGuard.m_dDiskChunks.GetLength() );

	for ( int iChunk = tGuard.m_dDiskChunks.GetLength()-1; iChunk>=0; iChunk-- )
	{
		// because disk chunk search within the loop will switch the profiler state
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_INIT );

		CSphQueryResult tChunkResult;
		tChunkResult.m_pProfile = pResult->m_pProfile;
		CSphMultiQueryArgs tMultiArgs ( tArgs.m_iIndexWeight );
		// storing index in matches tag for finding strings attrs offset later, biased against default zero and segments
		tMultiArgs.m_iTag = tGuard.m_dRamChunks.GetLength()+iChunk+1;
		tMultiArgs.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
		tMultiArgs.m_bLocalDF = bGotLocalDF;
		tMultiArgs.m_pLocalDocs = pLocalDocs;
		tMultiArgs.m_iTotalDocs = iTotalDocs;

		// we use sorters in both disk chunks and ram chunks, that's why we don't want to move to a new schema before we searched ram chunks
		tMultiArgs.m_bModifySorterSchemas = false;

		if ( !tGuard.m_dDiskChunks[iChunk]->MultiQuery ( pQuery, &tChunkResult, iSorters, ppSorters, tMultiArgs ) )
		{
			// FIXME? maybe handle this more gracefully (convert to a warning)?
			pResult->m_sError = tChunkResult.m_sError;
			return false;
		}

		// check terms inconsistency among disk chunks
		const SmallStringHash_T<CSphQueryResultMeta::WordStat_t> & hDstStats = tChunkResult.m_hWordStats;
		tStat.DumpDiffer ( hDstStats, m_sIndexName.cstr(), pResult->m_sWarning );
		if ( pResult->m_hWordStats.GetLength() )
		{
			pResult->m_hWordStats.IterateStart();
			while ( pResult->m_hWordStats.IterateNext() )
			{
				const CSphQueryResultMeta::WordStat_t * pDstStat = hDstStats ( pResult->m_hWordStats.IterateGetKey() );
				if ( pDstStat )
					pResult->AddStat ( pResult->m_hWordStats.IterateGetKey(), pDstStat->m_iDocs, pDstStat->m_iHits );
			}
		} else
		{
			pResult->m_hWordStats = hDstStats;
		}
		// keep last chunk statistics to check vs rt settings
		if ( iChunk==tGuard.m_dDiskChunks.GetLength()-1 )
			tDiskStat.Set ( hDstStats );
		if ( !iChunk )
			tStat.Set ( hDstStats );

		dDiskBlobPools[iChunk] = tChunkResult.m_pBlobPool;
		pResult->m_iBadRows += tChunkResult.m_iBadRows;

		if ( pResult->m_bHasPrediction )
			pResult->m_tStats.Add ( tChunkResult.m_tStats );

		if ( iChunk && tmMaxTimer>0 && sphMicroTimer()>=tmMaxTimer )
		{
			pResult->m_sWarning = "query time exceeded max_query_time";
			break;
		}
	}

	////////////////////
	// search RAM chunk
	////////////////////

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// select the sorter with max schema
	// uses GetAttrsCount to get working facets (was GetRowSize)
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	int iMatchPoolSize = 0;
	ARRAY_FOREACH ( i, dSorters )
	{
		iMatchPoolSize += dSorters[i]->m_iMatchCapacity;
		if ( dSorters[i]->GetSchema ()->GetAttrsCount ()>iMaxSchemaSize )
		{
			iMaxSchemaSize = dSorters[i]->GetSchema ()->GetAttrsCount ();
			iMaxSchemaIndex = i;
		}
	}

	if ( iMaxSchemaSize==-1 || iMaxSchemaIndex==-1 )
		return false;

	const ISphSchema & tMaxSorterSchema = *(dSorters[iMaxSchemaIndex]->GetSchema());

	CSphVector< const ISphSchema * > dSorterSchemas;
	SorterSchemas ( dSorters.Begin(), dSorters.GetLength(), iMaxSchemaIndex, dSorterSchemas );

	// setup calculations and result schema
	CSphQueryContext tCtx ( *pQuery );
	tCtx.m_pProfile = pProfiler;
	if ( !tCtx.SetupCalc ( pResult, tMaxSorterSchema, m_tSchema, nullptr, dSorterSchemas ) )
		return false;

	tCtx.m_uPackedFactorFlags = tArgs.m_uPackedFactorFlags;
	tCtx.m_pLocalDocs = pLocalDocs;
	tCtx.m_iTotalDocs = iTotalDocs;

	// setup search terms
	RtQwordSetup_t tTermSetup ( tGuard );
	tTermSetup.SetDict ( pDict );
	tTermSetup.m_pIndex = this;
	tTermSetup.m_iDynamicRowitems = tMaxSorterSchema.GetDynamicSize();
	if ( pQuery->m_uMaxQueryMsec>0 )
		tTermSetup.m_iMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time
	tTermSetup.m_pWarning = &pResult->m_sWarning;
	tTermSetup.SetSegment ( -1 );
	tTermSetup.m_pCtx = &tCtx;

	// setup prediction constrain
	CSphQueryStats tQueryStats;
	int64_t iNanoBudget = (int64_t)(pQuery->m_iMaxPredictedMsec) * 1000000; // from milliseconds to nanoseconds
	tQueryStats.m_pNanoBudget = &iNanoBudget;
	if ( pResult->m_bHasPrediction )
		tTermSetup.m_pStats = &tQueryStats;

	// bind weights
	tCtx.BindWeights ( pQuery, m_tSchema, pResult->m_sWarning );

	CSphVector<BYTE> dFiltered;
	const BYTE * sModifiedQuery = (BYTE *)pQuery->m_sQuery.cstr();

	ISphFieldFilterRefPtr_c pFieldFilter;
	if ( m_pFieldFilter && sModifiedQuery )
	{
		pFieldFilter = m_pFieldFilter->Clone();
		if ( pFieldFilter && pFieldFilter->Apply ( sModifiedQuery, strlen ( (char*)sModifiedQuery ), dFiltered, true ) )
			sModifiedQuery = dFiltered.Begin();
	}

	// parse query
	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_PARSE );

	XQQuery_t tParsed;
	// FIXME!!! provide segments list instead index to tTermSetup.m_pIndex

	const QueryParser_i * pQueryParser = pQuery->m_pQueryParser;
	assert ( pQueryParser );

	CSphScopedPtr<ISphRanker> pRanker ( nullptr );
	CSphScopedPayload tPayloads;

	// FIXME!!! add proper
	// - qcache invalidation after INSERT \ DELETE \ UPDATE and for plain index afte UPDATE #256
	// - qcache duplicates removal from killed document at segment #263
	tCtx.m_bSkipQCache = true;

	// no need to create ranker, etc if there's no query
	if ( !pQueryParser->IsFullscan(*pQuery) )
	{
		// OPTIMIZE! make a lightweight clone here? and/or remove double clone?
		ISphTokenizerRefPtr_c pQueryTokenizerJson { m_pTokenizer->Clone ( SPH_CLONE_QUERY ) };
		sphSetupQueryTokenizer ( pQueryTokenizerJson, IsStarDict (), m_tSettings.m_bIndexExactWords, true );

		if ( !pQueryParser->ParseQuery ( tParsed, (const char *)sModifiedQuery, pQuery, pQueryTokenizer, pQueryTokenizerJson, &m_tSchema, pDict, m_tSettings ) )
		{
			pResult->m_sError = tParsed.m_sParseError;
			return false;
		}

		if ( !tParsed.m_sParseWarning.IsEmpty() )
			pResult->m_sWarning = tParsed.m_sParseWarning;

		// transform query if needed (quorum transform, etc.)
		if ( pProfiler )
			pProfiler->Switch ( SPH_QSTATE_TRANSFORMS );

		// FIXME!!! provide segments list instead index
		sphTransformExtendedQuery ( &tParsed.m_pRoot, m_tSettings, pQuery->m_bSimplify, this );

		int iExpandKeywords = ExpandKeywords ( m_iExpandKeywords, pQuery->m_eExpandKeywords, m_tSettings );
		if ( iExpandKeywords!=KWE_DISABLED )
		{
			tParsed.m_pRoot = sphQueryExpandKeywords ( tParsed.m_pRoot, m_tSettings, iExpandKeywords );
			tParsed.m_pRoot->Check ( true );
		}

		// this should be after keyword expansion
		if ( m_tSettings.m_uAotFilterMask )
			TransformAotFilter ( tParsed.m_pRoot, pDict->GetWordforms(), m_tSettings );

		// expanding prefix in word dictionary case
		if ( m_bKeywordDict && IsStarDict() )
		{
			ExpansionContext_t tExpCtx;
			tExpCtx.m_pWordlist = this;
			tExpCtx.m_pBuf = NULL;
			tExpCtx.m_pResult = pResult;
			tExpCtx.m_iMinPrefixLen = m_tSettings.m_iMinPrefixLen;
			tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
			tExpCtx.m_iExpansionLimit = m_iExpansionLimit;
			tExpCtx.m_bHasExactForms = ( m_pDict->HasMorphology() || m_tSettings.m_bIndexExactWords );
			tExpCtx.m_bMergeSingles = ( pQuery->m_uDebugFlags & QUERY_DEBUG_NO_PAYLOAD )==0;
			tExpCtx.m_pPayloads = &tPayloads;
			tExpCtx.m_pIndexData = &tGuard.m_dRamChunks;

			tParsed.m_pRoot = sphExpandXQNode ( tParsed.m_pRoot, tExpCtx );
		}

		if ( !sphCheckQueryHeight ( tParsed.m_pRoot, pResult->m_sError ) )
			return false;

		// set zonespanlist settings
		tParsed.m_bNeedSZlist = pQuery->m_bZSlist;

		// setup query
		// must happen before index-level reject, in order to build proper keyword stats
		pRanker = sphCreateRanker ( tParsed, pQuery, pResult, tTermSetup, tCtx, tMaxSorterSchema );
		if ( !pRanker.Ptr() )
			return false;

		tCtx.SetupExtraData ( pRanker.Ptr(), iSorters==1 ? ppSorters[0] : NULL );

		// check terms inconsistency disk chunks vs rt vs previous indexes
		tDiskStat.DumpDiffer ( pResult->m_hWordStats, m_sIndexName.cstr(), pResult->m_sWarning );
		tStat.DumpDiffer ( pResult->m_hWordStats, m_sIndexName.cstr(), pResult->m_sWarning );

		pRanker->ExtraData ( EXTRA_SET_POOL_CAPACITY, (void**)&iMatchPoolSize );

		// check for the possible integer overflow in m_dPool.Resize
		int64_t iPoolSize = 0;
		if ( pRanker->ExtraData ( EXTRA_GET_POOL_SIZE, (void**)&iPoolSize ) && iPoolSize>INT_MAX )
		{
			pResult->m_sError.SetSprintf ( "ranking factors pool too big (%d Mb), reduce max_matches", (int)( iPoolSize/1024/1024 ) );
			return false;
		}
	}

	// empty index, empty result
	if ( !tGuard.m_dRamChunks.GetLength() && !tGuard.m_dDiskChunks.GetLength() )
	{
		for ( auto i : dSorters )
			TransformSorterSchema ( i, tGuard, dDiskBlobPools );

		pResult->m_iQueryTime = 0;
		return true;
	}

	// probably redundant, but just in case
	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_INIT );

	// search segments no looking to max_query_time
	// FIXME!!! move searching at segments before disk chunks as result set is safe with kill-lists
	if ( tGuard.m_dRamChunks.GetLength() )
	{
		// setup filters
		// FIXME! setup filters MVA pool
		bool bFullscan = pQuery->m_pQueryParser->IsFullscan ( *pQuery ) || pQuery->m_pQueryParser->IsFullscan ( tParsed );

		CreateFilterContext_t tFlx;
		tFlx.m_pFilters = &pQuery->m_dFilters;
		tFlx.m_pFilterTree = &pQuery->m_dFilterTree;
		tFlx.m_pSchema = &tMaxSorterSchema;
		tFlx.m_eCollation = pQuery->m_eCollation;
		tFlx.m_bScan = bFullscan;

		if ( !tCtx.CreateFilters ( tFlx, pResult->m_sError, pResult->m_sWarning ) )
			return false;

		// FIXME! OPTIMIZE! check if we can early reject the whole index

		// do searching
		bool bRandomize = dSorters[0]->m_bRandomize;
		int iCutoff = pQuery->m_iCutoff;
		if ( iCutoff<=0 )
			iCutoff = -1;

		if ( bFullscan )
		{
			if ( pProfiler )
				pProfiler->Switch ( SPH_QSTATE_FULLSCAN );

			// full scan
			// FIXME? OPTIMIZE? add shortcuts here too?
			CSphMatch tMatch;
			tMatch.Reset ( tMaxSorterSchema.GetDynamicSize() );
			tMatch.m_iWeight = tArgs.m_iIndexWeight;

			ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			{
				// set string pool for string on_sort expression fix up
				tCtx.SetBlobPool ( tGuard.m_dRamChunks[iSeg]->m_dBlobs.Begin() );
				ARRAY_FOREACH ( i, dSorters )
					dSorters[i]->SetBlobPool ( tGuard.m_dRamChunks[iSeg]->m_dBlobs.Begin() );

				RtRowIterator_c tIt ( tGuard.m_dRamChunks[iSeg], m_iStride );
				while (true)
				{
					const CSphRowitem * pRow = tIt.GetNextAliveRow();
					if ( !pRow )
						break;

					tMatch.m_tRowID = tIt.GetRowID();
					tMatch.m_pStatic = pRow;

					tCtx.CalcFilter ( tMatch );
					if ( tCtx.m_pFilter && !tCtx.m_pFilter->Eval ( tMatch ) )
					{
						tCtx.FreeDataFilter ( tMatch );
						continue;
					}

					if ( bRandomize )
						tMatch.m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

					tCtx.CalcSort ( tMatch );

					// storing segment in matches tag for finding strings attrs offset later, biased against default zero
					tMatch.m_iTag = iSeg+1;

					bool bNewMatch = false;
					ARRAY_FOREACH ( iSorter, dSorters )
						bNewMatch |= dSorters[iSorter]->Push ( tMatch );

					// stringptr expressions should be duplicated (or taken over) at this point
					tCtx.FreeDataFilter ( tMatch );
					tCtx.FreeDataSort ( tMatch );

					// handle cutoff
					if ( bNewMatch )
						if ( --iCutoff==0 )
							break;

					// handle timer
					if ( tmMaxTimer && sphMicroTimer()>=tmMaxTimer )
					{
						pResult->m_sWarning = "query time exceeded max_query_time";
						iSeg = tGuard.m_dRamChunks.GetLength() - 1;	// outer break
						break;
					}
				}

				if ( iCutoff==0 )
					break;
			}

		} else
		{
			// query matching
			ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			{
				const RtSegment_t * pSeg = tGuard.m_dRamChunks[iSeg];

				if ( pProfiler )
					pProfiler->Switch ( SPH_QSTATE_INIT_SEGMENT );

				tTermSetup.SetSegment ( iSeg );
				pRanker->Reset ( tTermSetup );

				// for lookups to work
				tCtx.m_pIndexData = pSeg;
				tCtx.m_pIndexSegment = pSeg;

				// set blob pool for string on_sort expression fix up
				tCtx.SetBlobPool ( pSeg->m_dBlobs.Begin() );
				for ( auto i : dSorters )
					i->SetBlobPool ( pSeg->m_dBlobs.Begin() );

				const BYTE * pBlobPool = pSeg->m_dBlobs.Begin();
				pRanker->ExtraData ( EXTRA_SET_BLOBPOOL, (void**)&pBlobPool );

				CSphMatch * pMatch = pRanker->GetMatchesBuffer();
				while (true)
				{
					// ranker does profile switches internally in GetMatches()
					int iMatches = pRanker->GetMatches();
					if ( iMatches<=0 )
						break;

					if ( pProfiler )
						pProfiler->Switch ( SPH_QSTATE_SORT );

					for ( int i=0; i<iMatches; i++ )
					{
						pMatch[i].m_pStatic = pSeg->GetDocinfoByRowID ( pMatch[i].m_tRowID );
						pMatch[i].m_iWeight *= tArgs.m_iIndexWeight;
						if ( bRandomize )
							pMatch[i].m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

						tCtx.CalcSort ( pMatch[i] );

						if ( tCtx.m_pWeightFilter && !tCtx.m_pWeightFilter->Eval ( pMatch[i] ) )
						{
							tCtx.FreeDataSort ( pMatch[i] );
							continue;
						}

						// storing segment in matches tag for finding strings attrs offset later, biased against default zero
						pMatch[i].m_iTag = iSeg+1;

						bool bNewMatch = false;
						ARRAY_FOREACH ( iSorter, dSorters )
						{
							bNewMatch |= dSorters[iSorter]->Push ( pMatch[i] );

							if ( tCtx.m_uPackedFactorFlags & SPH_FACTOR_ENABLE )
							{
								pRanker->ExtraData ( EXTRA_SET_MATCHPUSHED, (void**)&(dSorters[iSorter]->m_iJustPushed) );
								pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&(dSorters[iSorter]->m_dJustPopped) );
							}
						}

						// stringptr expressions should be duplicated (or taken over) at this point
						tCtx.FreeDataFilter ( pMatch[i] );
						tCtx.FreeDataSort ( pMatch[i] );

						if ( bNewMatch )
							if ( --iCutoff==0 )
								break;
					}

					if ( iCutoff==0 )
					{
						iSeg = tGuard.m_dRamChunks.GetLength();
						break;
					}
				}
			}
		}
	}

	// do final expression calculations
	if ( tCtx.m_dCalcFinal.GetLength () )
	{
		const int iSegmentsTotal = tGuard.m_dRamChunks.GetLength ();

		// at 0 pass processor also fills bitmask of segments these has matches at sorter
		// then skip sorter processing for these 'empty' segments
		SphRtFinalMatchCalc_t tFinal ( iSegmentsTotal, tCtx );

		ARRAY_FOREACH_COND ( iSeg, tGuard.m_dRamChunks, tFinal.HasSegments() )
		{
			if ( !tFinal.NextSegment ( iSeg ) )
				continue;

			// set blob pool for string on_sort expression fix up
			tCtx.SetBlobPool ( tGuard.m_dRamChunks[iSeg]->m_dBlobs.Begin() );

			for ( int iSorter = 0; iSorter<iSorters; iSorter++ )
			{
				ISphMatchSorter * pTop = ppSorters[iSorter];
				if ( pTop )
					pTop->Finalize ( tFinal, false );
			}
		}
	}


	//////////////////////
	// copying match's attributes to external storage in result set
	//////////////////////

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_FINALIZE );

	if ( pRanker.Ptr() )
		pRanker->FinalizeCache ( tMaxSorterSchema );

	MEMORY ( MEM_RT_RES_STRINGS );

	if ( pProfiler )
		pProfiler->Switch ( SPH_QSTATE_DYNAMIC );

	// create new standalone schema for sorters (independent of any external indexes/pools/storages)
	// modify matches inside the sorters to work with the new schema
	for ( auto i : dSorters )
		TransformSorterSchema ( i, tGuard, dDiskBlobPools );

	if ( pProfiler )
		pProfiler->Switch ( eOldState );

	if ( pResult->m_bHasPrediction )
		pResult->m_tStats.Add ( tQueryStats );

	// query timer
	pResult->m_iQueryTime = int ( ( sphMicroTimer()-tmQueryStart )/1000 );
	return true;
}

bool RtIndex_c::MultiQueryEx ( int iQueries, const CSphQuery * ppQueries, CSphQueryResult ** ppResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	// FIXME! OPTIMIZE! implement common subtree cache here
	bool bResult = false;
	for ( int i=0; i<iQueries; ++i )
		if ( MultiQuery ( &ppQueries[i], ppResults[i], 1, &ppSorters[i], tArgs ) )
			bResult = true;
		else
			ppResults[i]->m_iMultiplier = -1;

	return bResult;
}


void RtIndex_c::AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, CSphDict * pDict, bool bGetStats, int iQpos, RtQword_t * pQueryWord, CSphVector <CSphKeywordInfo> & dKeywords, const SphChunkGuard_t & tGuard ) const
{
	assert ( !bGetStats || pQueryWord );

	SphWordID_t iWord = pDict->GetWordID ( sWord );
	if ( !iWord )
		return;

	if ( bGetStats )
	{
		pQueryWord->Reset();
		pQueryWord->m_uWordID = iWord;
		pQueryWord->m_sWord = (const char *)sTokenized;
		pQueryWord->m_sDictWord = (const char *)sWord;
		ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
			RtQwordSetupSegment ( pQueryWord, tGuard.m_dRamChunks[iSeg], false, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings );
	}

	CSphKeywordInfo & tInfo = dKeywords.Add();
	tInfo.m_sTokenized = (const char *)sTokenized;
	tInfo.m_sNormalized = (const char*)sWord;
	tInfo.m_iDocs = bGetStats ? pQueryWord->m_iDocs : 0;
	tInfo.m_iHits = bGetStats ? pQueryWord->m_iHits : 0;
	tInfo.m_iQpos = iQpos;

	RemoveDictSpecials ( tInfo.m_sNormalized );
}


struct CSphRtQueryFilter : public ISphQueryFilter, public ISphNoncopyable
{
	const RtIndex_c *	m_pIndex;
	RtQword_t *			m_pQword;
	bool				m_bGetStats = false;
	const SphChunkGuard_t & m_tGuard;

	CSphRtQueryFilter ( const RtIndex_c * pIndex, RtQword_t * pQword, const SphChunkGuard_t & tGuard )
		: m_pIndex ( pIndex )
		, m_pQword ( pQword )
		, m_tGuard ( tGuard )
	{}

	void AddKeywordStats ( BYTE * sWord, const BYTE * sTokenized, int iQpos, CSphVector <CSphKeywordInfo> & dKeywords ) final
	{
		assert ( m_pIndex && m_pQword );
		m_pIndex->AddKeywordStats ( sWord, sTokenized, m_pDict, m_tFoldSettings.m_bStats, iQpos, m_pQword, dKeywords, m_tGuard );
	}
};

static void HashKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, SmallStringHash_T<CSphKeywordInfo> & hKeywords )
{
	for ( CSphKeywordInfo & tSrc : dKeywords )
	{
		CSphKeywordInfo & tDst = hKeywords.AddUnique ( tSrc.m_sNormalized );
		tDst.m_sTokenized = std::move ( tSrc.m_sTokenized );
		tDst.m_sNormalized = std::move ( tSrc.m_sNormalized );
		tDst.m_iQpos = tSrc.m_iQpos;
		tDst.m_iDocs += tSrc.m_iDocs;
		tDst.m_iHits += tSrc.m_iHits;
	}
}

bool RtIndex_c::DoGetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, bool bFillOnly, CSphString * pError, const SphChunkGuard_t & tGuard ) const
{
	if ( !bFillOnly )
		dKeywords.Resize ( 0 );

	if ( ( bFillOnly && !dKeywords.GetLength() ) || ( !bFillOnly && ( !sQuery || !sQuery[0] ) ) )
		return true;

	RtQword_t tQword;

	ISphTokenizerRefPtr_c pTokenizer { m_pTokenizer->Clone ( SPH_CLONE_INDEX ) };
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	CSphDictRefPtr_c pDict { GetStatelessDict ( m_pDict ) };

	if ( IsStarDict () )
	{
		if ( m_bKeywordDict )
			SetupStarDict ( pDict, pTokenizer );
		else
			pTokenizer->AddPlainChar ( '*' );
	}

	if ( m_tSettings.m_bIndexExactWords )
		SetupExactDict ( pDict, pTokenizer, false );

	// FIXME!!! missed bigram, FieldFilter

	if ( !bFillOnly )
	{
		ExpansionContext_t tExpCtx;

		// query defined options
		tExpCtx.m_iExpansionLimit = tSettings.m_iExpansionLimit ? tSettings.m_iExpansionLimit : m_iExpansionLimit;
		bool bExpandWildcards = ( m_bKeywordDict && IsStarDict() && !tSettings.m_bFoldWildcards );

		CSphRtQueryFilter tAotFilter ( this, &tQword, tGuard );
		tAotFilter.m_pTokenizer = pTokenizer;
		tAotFilter.m_pDict = pDict;
		tAotFilter.m_pSettings = &m_tSettings;
		tAotFilter.m_tFoldSettings = tSettings;
		tAotFilter.m_tFoldSettings.m_bFoldWildcards = !bExpandWildcards;

		tExpCtx.m_pWordlist = this;
		tExpCtx.m_iMinPrefixLen = m_tSettings.m_iMinPrefixLen;
		tExpCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
		tExpCtx.m_bHasExactForms = ( m_pDict->HasMorphology() || m_tSettings.m_bIndexExactWords );
		tExpCtx.m_bMergeSingles = false;
		tExpCtx.m_pIndexData = &tGuard.m_dRamChunks;

		pTokenizer->SetBuffer ( (BYTE *)sQuery, strlen ( sQuery ) );
		tAotFilter.GetKeywords ( dKeywords, tExpCtx );
	} else
	{
		BYTE sWord[SPH_MAX_KEYWORD_LEN];

		ARRAY_FOREACH ( i, dKeywords )
		{
			CSphKeywordInfo & tInfo = dKeywords[i];
			int iLen = tInfo.m_sTokenized.Length();
			memcpy ( sWord, tInfo.m_sTokenized.cstr(), iLen );
			sWord[iLen] = '\0';

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				tQword.Reset();
				tQword.m_uWordID = iWord;
				tQword.m_sWord = tInfo.m_sTokenized;
				tQword.m_sDictWord = (const char *)sWord;
				ARRAY_FOREACH ( iSeg, tGuard.m_dRamChunks )
					RtQwordSetupSegment ( &tQword, tGuard.m_dRamChunks[iSeg], false, m_bKeywordDict, m_iWordsCheckpoint, m_tSettings );

				tInfo.m_iDocs += tQword.m_iDocs;
				tInfo.m_iHits += tQword.m_iHits;
			}
		}
	}

	// get stats from disk chunks too
	if ( !tSettings.m_bStats )
		return true;

	if ( bFillOnly )
	{
		ARRAY_FOREACH ( iChunk, tGuard.m_dDiskChunks )
			tGuard.m_dDiskChunks[iChunk]->FillKeywords ( dKeywords );
	} else
	{
		// bigram and expanded might differs need to merge infos
		CSphVector<CSphKeywordInfo> dChunkKeywords;
		SmallStringHash_T<CSphKeywordInfo> hKeywords;
		ARRAY_FOREACH ( iChunk, tGuard.m_dDiskChunks )
		{
			tGuard.m_dDiskChunks[iChunk]->GetKeywords ( dChunkKeywords, sQuery, tSettings, pError );
			HashKeywords ( dChunkKeywords, hKeywords );
			dChunkKeywords.Resize ( 0 );
		}

		if ( hKeywords.GetLength() )
		{
			// merge keywords from RAM parts with disk keywords into hash
			HashKeywords ( dKeywords, hKeywords );
			dKeywords.Resize ( 0 );
			dKeywords.Reserve ( hKeywords.GetLength() );

			hKeywords.IterateStart();
			while ( hKeywords.IterateNext() )
			{
				const CSphKeywordInfo & tSrc = hKeywords.IterateGet();
				dKeywords.Add ( tSrc );
			}
			sphSort ( dKeywords.Begin(), dKeywords.GetLength(), bind ( &CSphKeywordInfo::m_iQpos ) );
		}
	}

	return true;
}


bool RtIndex_c::GetKeywords ( CSphVector<CSphKeywordInfo> & dKeywords, const char * sQuery, const GetKeywordsSettings_t & tSettings, CSphString * pError ) const
{
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );
	bool bGot = DoGetKeywords ( dKeywords, sQuery, tSettings, false, pError, tGuard );
	return bGot;
}


bool RtIndex_c::FillKeywords ( CSphVector<CSphKeywordInfo> & dKeywords ) const
{
	GetKeywordsSettings_t tSettings;
	tSettings.m_bStats = true;
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );
	bool bGot = DoGetKeywords ( dKeywords, NULL, tSettings, true, NULL, tGuard );
	return bGot;
}


static RtSegment_t * UpdateFindSegment ( const SphChunkGuard_t & tGuard, CSphRowitem * & pRow, DocID_t uDocID )
{
	assert ( uDocID );

	ARRAY_FOREACH ( i, tGuard.m_dRamChunks )
	{
		pRow = const_cast<CSphRowitem *> ( tGuard.m_dRamChunks[i]->FindAliveRow ( uDocID ) );
		if ( !pRow )
			continue;

		return const_cast<RtSegment_t *>(tGuard.m_dRamChunks[i]);
	}

	return nullptr;
}


void RtIndex_c::Update_CollectRowPtrs ( UpdateContext_t & tCtx, const SphChunkGuard_t & tGuard )
{
	for ( int iUpd = tCtx.m_iFirst; iUpd<tCtx.m_iLast; iUpd++ )
	{
		DocID_t uDocid = tCtx.m_tUpd.m_dDocids[iUpd];

		CSphRowitem * pRow = nullptr;
		RtSegment_t * pSegment = UpdateFindSegment ( tGuard, pRow, uDocid );

		UpdatedRowData_t & tNew = tCtx.GetRowData(iUpd);
		tNew.m_pRow = pRow;
		tNew.m_pAttrPool = ( pSegment && pSegment->m_dRows.GetLength() ) ? &pSegment->m_dRows[0] : nullptr;
		tNew.m_pBlobPool = ( pSegment && pSegment->m_dBlobs.GetLength() ) ? &pSegment->m_dBlobs[0] : nullptr;
		tNew.m_pSegment = pSegment;
	}
}


bool RtIndex_c::Update_DiskChunks ( UpdateContext_t & tCtx, const SphChunkGuard_t & tGuard, int & iUpdated, CSphString & sError )
{
	for ( int iUpd=tCtx.m_iFirst; iUpd<tCtx.m_iLast; iUpd++ )
	{
		if ( tCtx.GetRowData(iUpd).m_bUpdated )
			continue;

		for ( int iChunk = tGuard.m_dDiskChunks.GetLength()-1; iChunk>=0; iChunk-- )
		{
			// run just this update
			// FIXME! might be inefficient in case of big batches (redundant allocs in disk update)
			bool bCritical = false;
			CSphString sWarning;
			int iRes = const_cast<CSphIndex *>( tGuard.m_dDiskChunks[iChunk] )->UpdateAttributes ( tCtx.m_tUpd, iUpd, bCritical, sError, sWarning );

			// FIXME! need to handle critical failures here (chunk is unusable at this point)
			assert ( !bCritical );

			// FIXME! maybe emit a warning to client as well?
			if ( iRes<0 )
				return false;

			// update stats
			iUpdated += iRes;
			m_uDiskAttrStatus |= tGuard.m_dDiskChunks[iChunk]->GetAttributeStatus();

			// we only need to update the most fresh chunk
			if ( iRes>0 )
				break;
		}
	}

	return true;
}


bool RtIndex_c::Update_WriteBlobRow ( UpdateContext_t & tCtx, int iUpd, CSphRowitem * pDocinfo, const BYTE * pBlob, int iLength, int nBlobAttrs, bool & bCritical, CSphString & sError )
{
	IndexSegment_c * pSegment = tCtx.GetRowData(iUpd).m_pSegment;
	assert ( pSegment );

	bCritical = false;

	CSphTightVector<BYTE> & dBlobPool = ((RtSegment_t*)pSegment)->m_dBlobs;

	BYTE * pExistingBlob = dBlobPool.Begin() + sphGetBlobRowOffset(pDocinfo);
	DWORD uExistingBlobLen = sphGetBlobTotalLen ( pExistingBlob, nBlobAttrs );

	// overwrite old record
	if ( (DWORD)iLength<=uExistingBlobLen )
	{
		memcpy ( pExistingBlob, pBlob, iLength );
		return true;
	}

	int iPoolSize = dBlobPool.GetLength();
	dBlobPool.Resize ( iPoolSize+iLength );
	memcpy ( dBlobPool.Begin()+iPoolSize, pBlob, iLength );

	sphSetBlobRowOffset ( pDocinfo, iPoolSize );

	// update blob pool ptrs since they could have changed after the resize
	for ( auto & i : tCtx.m_dUpdatedRows )
		if ( i.m_pSegment==pSegment )
			i.m_pBlobPool = &dBlobPool[0];

	return true;
}


// FIXME! might be inconsistent in case disk chunk update fails
int RtIndex_c::UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, bool & bCritical, CSphString & sError, CSphString & sWarning )
{
	assert ( tUpd.m_dDocids.GetLength()==tUpd.m_dRowOffset.GetLength() );
	DWORD uRows = tUpd.m_dDocids.GetLength();

	if ( ( !m_dRamChunks.GetLength() && !m_dDiskChunks.GetLength() ) || !uRows )
		return 0;

	UpdateContext_t tCtx ( tUpd, m_tSchema, nullptr, ( iIndex<0 ) ? 0 : iIndex, ( iIndex<0 ) ? uRows : iIndex+1 );

	// FIXME!!! grab Writer lock to prevent segments retirement during commit(merge)
	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	Update_CollectRowPtrs ( tCtx, tGuard );
	if ( !Update_FixupData ( tCtx, sError ) )
		return -1;

	if ( tUpd.m_bStrict )
		if ( !Update_InplaceJson ( tCtx, sError, true ) )
			return -1;

	// second pass
	tCtx.m_iJsonWarnings = 0;
	Update_InplaceJson ( tCtx, sError, false );

	if ( !Update_Blobs ( tCtx, bCritical, sError ) )
		return -1;

	Update_Plain ( tCtx );

	int iUpdated = 0;
	for ( const auto & i : tCtx.m_dUpdatedRows )
		if ( i.m_bUpdated )
			iUpdated++;

	if ( !Update_DiskChunks ( tCtx, tGuard, iUpdated, sError ) )
		sphWarn ( "INTERNAL ERROR: index %s update failure: %s", m_sIndexName.cstr(), sError.cstr() );

	// bump the counter, binlog the update!
	assert ( iIndex<0 );
	g_pBinlog->BinlogUpdateAttributes ( &m_iTID, m_sIndexName.cstr(), tUpd );

	if ( !Update_HandleJsonWarnings ( tCtx, iUpdated, sWarning, sError ) )
		return -1;

	// all done
	return iUpdated;
}


bool RtIndex_c::SaveAttributes ( CSphString & sError ) const
{
	if ( !m_dDiskChunks.GetLength() )
		return true;

	DWORD uStatus = m_uDiskAttrStatus;
	bool bAllSaved = true;

	SphChunkGuard_t tGuard;
	GetReaderChunks ( tGuard );

	ARRAY_FOREACH ( i, tGuard.m_dDiskChunks )
		bAllSaved &= tGuard.m_dDiskChunks[i]->SaveAttributes ( sError );

	if ( uStatus==m_uDiskAttrStatus )
		m_uDiskAttrStatus = 0;

	return bAllSaved;
}


struct SphOptimizeGuard_t : ISphNoncopyable
{
	CSphMutex &			m_tLock;
	volatile bool &		m_bOptimizeStop;

	SphOptimizeGuard_t ( CSphMutex & tLock, volatile bool & bOptimizeStop )
		: m_tLock ( tLock )
		, m_bOptimizeStop ( bOptimizeStop )
	{
		bOptimizeStop = true;
		m_tLock.Lock();
	}

	~SphOptimizeGuard_t ()
	{
		m_bOptimizeStop = false;
		m_tLock.Unlock();
	}
};


template<typename T>
class WriteWrapper_Mem_c : public WriteWrapper_c
{
public:
	WriteWrapper_Mem_c ( CSphTightVector<T> & tBuffer )
		: m_tBuffer ( tBuffer )
	{}

	void PutBytes ( const BYTE * pData, int iSize ) override
	{
		assert ( iSize % sizeof(T) == 0 );
		T * pNew = m_tBuffer.AddN ( iSize/sizeof(T) );
		memcpy ( pNew, pData, iSize );
	}

	SphOffset_t	GetPos() const override
	{
		return m_tBuffer.GetLength()*sizeof(T);
	}

	bool IsError() const override
	{
		return false;
	}

protected:
	CSphTightVector<T> & m_tBuffer;
};


bool RtIndex_c::AddRemoveAttribute ( bool bAdd, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError )
{
	if ( m_dDiskChunks.GetLength() && !m_tSchema.GetAttrsCount() )
	{
		sError = "index must already have attributes";
		return false;
	}

	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop ); // got write-locked at daemon

	CSphSchema tOldSchema = m_tSchema;
	CSphSchema tNewSchema = m_tSchema;
	if ( !Alter_AddRemoveFromSchema ( tNewSchema, sAttrName, eAttrType, bAdd, sError ) )
		return false;

	m_tSchema = tNewSchema;

	int iOldStride = m_iStride;
	m_iStride = m_tSchema.GetRowSize();

	CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

	// modify the in-memory data of disk chunks
	// fixme: we can't rollback in-memory changes, so we just show errors here for now
	ARRAY_FOREACH ( iDiskChunk, m_dDiskChunks )
		if ( !m_dDiskChunks[iDiskChunk]->AddRemoveAttribute ( bAdd, sAttrName, eAttrType, sError ) )
			sphWarning ( "%s attribute to %s.%d: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), dChunkNames[iDiskChunk], sError.cstr() );

	// now modify the ramchunk
	for ( RtSegment_t * pSeg : m_dRamChunks )
	{
		assert ( pSeg );

		const CSphRowitem * pDocinfo = pSeg->m_dRows.Begin();
		const CSphRowitem * pDocinfoMax = pDocinfo+pSeg->m_dRows.GetLength();

		CSphTightVector<CSphRowitem> dSPA;
		dSPA.Reserve ( pSeg->m_dRows.GetLength() / iOldStride * m_iStride );

		CSphTightVector<BYTE> dSPB;
		dSPB.Reserve ( pSeg->m_dBlobs.GetLength() / 2 );		// reserve half of our current blobs, just in case

		WriteWrapper_Mem_c<CSphRowitem> tSPAWriteWrapper(dSPA);
		WriteWrapper_Mem_c<BYTE> tSPBWriteWrapper(dSPB);

		if ( !Alter_AddRemoveAttr ( tOldSchema, tNewSchema, pDocinfo, pDocinfoMax, pSeg->m_dBlobs.Begin(), tSPAWriteWrapper, tSPBWriteWrapper, bAdd, sAttrName ) )
			sphWarning ( "%s attribute to %s: %s", bAdd ? "adding" : "removing", m_sPath.cstr(), sError.cstr() );

		pSeg->m_dRows.SwapData(dSPA);
		pSeg->m_dBlobs.SwapData(dSPB);
	}

	// fixme: we can't rollback at this point
	Verify ( SaveRamChunk ( m_dRamChunks ) );

	SaveMeta ( m_iTID, dChunkNames );

	// fixme: notify that it was ALTER that caused the flush
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// MAGIC CONVERSIONS
//////////////////////////////////////////////////////////////////////////

bool RtIndex_c::AttachDiskIndex ( CSphIndex * pIndex, bool bTruncate, CSphString & sError )
{
	bool bEmptyRT = ( ( !m_dRamChunks.GetLength() && !m_dDiskChunks.GetLength() ) || bTruncate );

	// safeguards
	// we do not support some of the disk index features in RT just yet
#define LOC_ERROR(_arg) { sError = _arg; return false; }
	const CSphIndexSettings & tSettings = pIndex->GetSettings();
	if ( tSettings.m_iStopwordStep!=1 )
		LOC_ERROR ( "ATTACH currently requires stopword_step=1 in disk index (RT-side support not implemented yet)" );
	// ATTACH to exist index require these checks
	if ( !bEmptyRT )
	{
		if ( m_pTokenizer->GetSettingsFNV()!=pIndex->GetTokenizer()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same tokenizer settings (RT-side support not implemented yet)" );
		if ( m_pDict->GetSettingsFNV()!=pIndex->GetDictionary()->GetSettingsFNV() )
			LOC_ERROR ( "ATTACH currently requires same dictionary settings (RT-side support not implemented yet)" );
		if ( !GetMatchSchema().CompareTo ( pIndex->GetMatchSchema(), sError, true ) )
			LOC_ERROR ( "ATTACH currently requires same attributes declaration (RT-side support not implemented yet)" );
	}
#undef LOC_ERROR

	if ( bTruncate && !Truncate ( sError ) )
		return false;

	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop ); // got write-locked at daemon

	int iTotalKilled = 0;
	if ( !bEmptyRT )
	{
		SphAttr_t * pIndexDocList = nullptr;
		int64_t iCount = 0;
		if ( !pIndex->BuildDocList ( &pIndexDocList, &iCount, &sError ) )
		{
			sError.SetSprintf ( "ATTACH failed, %s", sError.cstr() );
			return false;
		}

		SphChunkGuard_t tGuard;
		GetReaderChunks ( tGuard );

		ChunkStats_t tStats ( m_tStats, m_dFieldLensRam );
		SaveDiskChunk ( m_iTID, tGuard, tStats, true );

		for ( const auto & pChunk : m_dDiskChunks )
		{
			auto pSegment = (IndexSegment_c*)pChunk;
			for ( int i = 0; i<iCount; i++ )
				iTotalKilled += KillInDiskChunk ( pSegment, pIndexDocList, iCount );
		}

		SafeDeleteArray ( pIndexDocList );
	}

	CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, true );

	// rename that source index to our last chunk
	CSphString sChunk;
	sChunk.SetSprintf ( "%s.%d", m_sPath.cstr(), dChunkNames.Last() );
	if ( !pIndex->Rename ( sChunk.cstr() ) )
	{
		sError.SetSprintf ( "ATTACH failed, %s", pIndex->GetLastError().cstr() );
		return false;
	}

	// copy schema from new index
	m_tSchema = pIndex->GetMatchSchema();
	m_iStride = m_tSchema.GetRowSize();
	m_tStats.m_iTotalBytes += pIndex->GetStats().m_iTotalBytes;
	m_tStats.m_iTotalDocuments += pIndex->GetStats().m_iTotalDocuments-iTotalKilled;

	// copy tokenizer, dict etc settings from new index
	m_tSettings = pIndex->GetSettings();
	m_tSettings.m_dBigramWords.Reset();

	m_pTokenizer = pIndex->GetTokenizer()->Clone ( SPH_CLONE_INDEX );
	m_pDict = pIndex->GetDictionary()->Clone ();
	PostSetup();
	CSphString sName;
	sName.SetSprintf ( "%s_%d", m_sIndexName.cstr(), m_dDiskChunks.GetLength() );
	pIndex->SetName ( sName.cstr() );
	pIndex->SetBinlog ( false );

	// FIXME? what about copying m_TID etc?

	// recreate disk chunk list, resave header file
	m_dDiskChunks.Add ( pIndex );
	SaveMeta ( m_iTID, dChunkNames );

	// FIXME? do something about binlog too?
	// g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// all done, reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}

//////////////////////////////////////////////////////////////////////////
// TRUNCATE
//////////////////////////////////////////////////////////////////////////

bool RtIndex_c::Truncate ( CSphString & )
{
	// TRUNCATE needs an exclusive lock, should be write-locked at daemon, conflicts only with optimize
	SphOptimizeGuard_t tStopOptimize ( m_tOptimizingLock, m_bOptimizeStop );

	// update and save meta
	// indicate 0 disk chunks, we are about to kill them anyway
	// current TID will be saved, so replay will properly skip preceding txns
	m_tStats.Reset();
	SaveMeta ( m_iTID, CSphFixedVector<int>(0) );

	// allow binlog to unlink now-redundant data files
	g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// kill RAM chunk file
	CSphString sFile;
	sFile.SetSprintf ( "%s.ram", m_sPath.cstr() );
	if ( ::unlink ( sFile.cstr() ) )
		if ( errno!=ENOENT )
			sphWarning ( "rt: truncate failed to unlink %s: %s", sFile.cstr(), strerrorm(errno) );

	// kill all disk chunks files
	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		StrVec_t v;
		const char * sChunkFilename = m_dDiskChunks[i]->GetFilename();
		sphSplit ( v, sChunkFilename, "." ); // split something like "rt.1"
		const char * sChunkNumber = v.Last().cstr();
		sFile.SetSprintf ( "%s.%s", m_sPath.cstr(), sChunkNumber );
		sphUnlinkIndex ( sFile.cstr(), false );
	}

	// kill in-memory data, reset stats
	ARRAY_FOREACH ( i, m_dDiskChunks )
		SafeDelete ( m_dDiskChunks[i] );
	m_dDiskChunks.Reset();

	ARRAY_FOREACH ( i, m_dRamChunks )
		SafeDelete ( m_dRamChunks[i] );
	m_dRamChunks.Reset();

	// reset cache
	QcacheDeleteIndex ( GetIndexId() );
	return true;
}


int RtIndex_c::KillInDiskChunk ( IndexSegment_c * pSegment, const DocID_t * pKlist, int iKlistSize )
{
	assert ( pSegment && pKlist );
	if ( m_bOptimizing )
	{
		DocID_t * pAdd = m_dKillsWhileOptimizing.AddN ( iKlistSize );
		memcpy ( pAdd, pKlist, iKlistSize*sizeof(pKlist[0]) );
	}

	return pSegment->KillMulti ( pKlist, iKlistSize );
}


//////////////////////////////////////////////////////////////////////////
// OPTIMIZE
//////////////////////////////////////////////////////////////////////////

void RtIndex_c::Optimize()
{
	if ( g_bProgressiveMerge )
	{
		ProgressiveMerge ( );
		return;
	}

	int64_t tmStart = sphMicroTimer();

	CSphScopedLock<CSphMutex> tOptimizing ( m_tOptimizingLock );
	m_bOptimizing = true;

	int iChunks = m_dDiskChunks.GetLength();
	CSphSchema tSchema = m_tSchema;
	CSphString sError;

	while ( m_dDiskChunks.GetLength()>1 && !g_bShutdown && !m_bOptimizeStop )
	{
		const CSphIndex * pOldest = nullptr;
		const CSphIndex * pOlder = nullptr;
		{ // m_tChunkLock scoped Readlock
			CSphScopedRLock RChunkLock { m_tChunkLock };
			// merge 'older'(pSrc) to 'oldest'(pDst) and get 'merged' that names like 'oldest'+.tmp
			// however 'merged' got placed at 'older' position and 'merged' renamed to 'older' name
			pOldest = m_dDiskChunks[0];
			pOlder = m_dDiskChunks[1];
		} // m_tChunkLock scoped Readlock

		CSphString sOlder, sOldest, sRename, sMerged;
		sOlder.SetSprintf ( "%s", pOlder->GetFilename() );
		sOldest.SetSprintf ( "%s", pOldest->GetFilename() );
		sRename.SetSprintf ( "%s.old", pOlder->GetFilename() );
		sMerged.SetSprintf ( "%s.tmp", pOldest->GetFilename() );

		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// merge data to disk ( data is constant during that phase )
		CSphIndexProgress tProgress;
		bool bMerged = sphMerge ( pOldest, pOlder, sError, tProgress, &m_bOptimizeStop, true );
		if ( !bMerged )
		{
			sphWarning ( "rt optimize: index %s: failed to merge %s to %s (error %s)", m_sIndexName.cstr(), sOlder.cstr(), sOldest.cstr(), sError.cstr() );
			break;
		}

		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		CSphScopedPtr<CSphIndex> pMerged ( LoadDiskChunk ( sMerged.cstr(), sError ) );
		if ( !pMerged.Ptr() )
		{
			sphWarning ( "rt optimize: index %s: failed to load merged chunk (error %s)", m_sIndexName.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// lets rotate indexes

		// rename older disk chunk to 'old'
		if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sRename.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: cur to old rename failed (error %s)", m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			break;
		}
		// rename merged disk chunk to 0
		if ( !pMerged->Rename ( sOlder.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: merged to cur rename failed (error %s)", m_sIndexName.cstr(), pMerged->GetLastError().cstr() );

			if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sOlder.cstr() ) )
				sphWarning ( "rt optimize: index %s: old to cur rename failed (error %s)",	m_sIndexName.cstr(), pOlder->GetLastError().cstr() );

			break;
		}

		if ( g_bShutdown || m_bOptimizeStop ) // protection
			break;

		Verify ( m_tWriting.Lock() );
		Verify ( m_tChunkLock.WriteLock() );

		// apply killlists that were collected while we were merging segments
		pMerged->KillMulti ( m_dKillsWhileOptimizing.Begin(), m_dKillsWhileOptimizing.GetLength() );

		sphLogDebug ( "optimized 0=%s, 1=%s, new=%s", m_dDiskChunks[0]->GetName(), m_dDiskChunks[1]->GetName(), pMerged->GetName() );

		m_dDiskChunks[1] = pMerged.LeakPtr();
		m_dDiskChunks.Remove ( 0 );
		CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

		Verify ( m_tChunkLock.Unlock() );
		SaveMeta ( m_iTID, dChunkNames );
		Verify ( m_tWriting.Unlock() );

		if ( g_bShutdown || m_bOptimizeStop )
		{
			sphWarning ( "rt optimize: index %s: forced to shutdown, remove old index files manually '%s', '%s'",
				m_sIndexName.cstr(), sRename.cstr(), sOldest.cstr() );
			break;
		}

		// exclusive reader (to make sure that disk chunks not used any more) and writer lock here
		// write lock goes first as with commit
		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );

		SafeDelete ( pOlder );
		SafeDelete ( pOldest );

		Verify ( m_tReading.Unlock() );
		Verify ( m_tWriting.Unlock() );

		// we might remove old index files
		sphUnlinkIndex ( sRename.cstr(), true );
		sphUnlinkIndex ( sOldest.cstr(), true );
		// FIXEME: wipe out 'merged' index files in case of error
	}

	Verify ( m_tWriting.Lock() );
	m_dKillsWhileOptimizing.Resize(0);
	Verify ( m_tWriting.Unlock() );

	m_bOptimizing = false;
	int64_t tmPass = sphMicroTimer() - tmStart;

	if ( g_bShutdown )
	{
		sphWarning ( "rt: index %s: optimization terminated chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	} else
	{
		sphInfo ( "rt: index %s: optimized chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	}
}


//////////////////////////////////////////////////////////////////////////
// PROGRESSIVE MERGE
//////////////////////////////////////////////////////////////////////////

static int64_t GetChunkSize ( const CSphVector<CSphIndex*> & dDiskChunks, int iIndex )
{
	if (iIndex<0)
		return 0;
	CSphIndexStatus tDisk;
	dDiskChunks[iIndex]->GetStatus(&tDisk);
	return tDisk.m_iDiskUse;
}


static int GetNextSmallestChunk ( const CSphVector<CSphIndex*> & dDiskChunks, int iIndex )
{
	assert (dDiskChunks.GetLength ()>1);
	int iRes = -1;
	int64_t iLastSize = INT64_MAX;
	ARRAY_FOREACH ( i, dDiskChunks )
	{
		int64_t iSize = GetChunkSize ( dDiskChunks, i );
		if ( iSize<iLastSize && iIndex!=i )
		{
			iLastSize = iSize;
			iRes = i;
		}
	}
	return iRes;
}


void RtIndex_c::ProgressiveMerge()
{
	// How does this work:
	// In order to minimize IO operations we merge chunks in order from the smallest to the largest to build a progression
	// the timeline is: [older chunks], ..., A, A+1, ..., B, ..., [younger chunks]
	// this also needs meta v.12 (chunk list with possible skips, instead of a base chunk + length as in meta v.11)

	int64_t tmStart = sphMicroTimer();

	CSphScopedLock<CSphMutex> tOptimizing ( m_tOptimizingLock );
	m_bOptimizing = true;

	int iChunks = m_dDiskChunks.GetLength();
	CSphSchema tSchema = m_tSchema;
	CSphString sError;

	while ( m_dDiskChunks.GetLength()>1 && !g_bShutdown && !m_bOptimizeStop )
	{
		const CSphIndex * pOldest = nullptr;
		const CSphIndex * pOlder = nullptr;
		int iA = -1;
		int iB = -1;

		{
			CSphScopedRLock tChunkLock { m_tChunkLock };

			// merge 'smallest' to 'smaller' and get 'merged' that names like 'A'+.tmp
			// however 'merged' got placed at 'B' position and 'merged' renamed to 'B' name

			iA = GetNextSmallestChunk ( m_dDiskChunks, 0 );
			iB = GetNextSmallestChunk ( m_dDiskChunks, iA );

			if ( iA<0 || iB<0 )
			{
				sError.SetSprintf ("Couldn't find smallest chunk");
				return;
			}

			// we need to make sure that A is the oldest one
			// indexes go from oldest to newest so A must go before B (A is always older than B)
			// this is not required by bitmap killlists, but by some other stuff (like ALTER RECONFIGURE)
			if ( iA > iB )
				Swap ( iB, iA );

			sphLogDebug ( "progressive merge - merging %d (%d kb) with %d (%d kb)", iA, (int)(GetChunkSize ( m_dDiskChunks, iA )/1024), iB, (int)(GetChunkSize ( m_dDiskChunks, iB )/1024) );

			pOldest = m_dDiskChunks[iA];
			pOlder = m_dDiskChunks[iB];
		} // m_tChunkLock scope

		CSphString sOlder, sOldest, sRename, sMerged;
		sOlder.SetSprintf ( "%s", pOlder->GetFilename() );
		sOldest.SetSprintf ( "%s", pOldest->GetFilename() );
		sRename.SetSprintf ( "%s.old", pOlder->GetFilename() );
		sMerged.SetSprintf ( "%s.tmp", pOldest->GetFilename() );

		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// merge data to disk ( data is constant during that phase )
		CSphIndexProgress tProgress;
		bool bMerged = sphMerge ( pOldest, pOlder, sError, tProgress, &m_bOptimizeStop, true );
		if ( !bMerged )
		{
			sphWarning ( "rt optimize: index %s: failed to merge %s to %s (error %s)",
				m_sIndexName.cstr(), sOlder.cstr(), sOldest.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		CSphScopedPtr<CSphIndex> pMerged ( LoadDiskChunk ( sMerged.cstr(), sError ) );
		if ( !pMerged.Ptr() )
		{
			sphWarning ( "rt optimize: index %s: failed to load merged chunk (error %s)",
				m_sIndexName.cstr(), sError.cstr() );
			break;
		}
		// check forced exit after long operation
		if ( g_bShutdown || m_bOptimizeStop )
			break;

		// lets rotate indexes

		// rename older disk chunk to 'old'
		if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sRename.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: cur to old rename failed (error %s)",
				m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			break;
		}
		// rename merged disk chunk to B
		if ( !pMerged->Rename ( sOlder.cstr() ) )
		{
			sphWarning ( "rt optimize: index %s: merged to cur rename failed (error %s)",
				m_sIndexName.cstr(), pMerged->GetLastError().cstr() );
			if ( !const_cast<CSphIndex *>( pOlder )->Rename ( sOlder.cstr() ) )
			{
				sphWarning ( "rt optimize: index %s: old to cur rename failed (error %s)",
					m_sIndexName.cstr(), pOlder->GetLastError().cstr() );
			}
			break;
		}

		if ( g_bShutdown || m_bOptimizeStop ) // protection
			break;

		// merged replaces recent chunk
		// oldest chunk got deleted

		// Writing lock - to wipe out writers
		// Reading wlock - to wipe out searches (FIXME: is this necessary with new killlists?)
		// Chunk wlock - to lock chunks as we are going to modify the chunk vector
		// order same as GetReaderChunks and SaveDiskChunk to prevent deadlock

		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );
		Verify ( m_tChunkLock.WriteLock() );

		// apply killlists that were collected while we were merging segments
		pMerged->KillMulti ( m_dKillsWhileOptimizing.Begin(), m_dKillsWhileOptimizing.GetLength() );

		sphLogDebug ( "optimized (progressive) a=%s, b=%s, new=%s", pOldest->GetName(), pOlder->GetName(), pMerged->GetName() );

		m_dDiskChunks[iB] = pMerged.LeakPtr();
		m_dDiskChunks.Remove ( iA );
		CSphFixedVector<int> dChunkNames = GetIndexNames ( m_dDiskChunks, false );

		Verify ( m_tChunkLock.Unlock() );
		Verify ( m_tReading.Unlock() );
		SaveMeta ( m_iTID, dChunkNames );
		Verify ( m_tWriting.Unlock() );

		if ( g_bShutdown || m_bOptimizeStop )
		{
			sphWarning ( "rt optimize: index %s: forced to shutdown, remove old index files manually '%s', '%s'",
				m_sIndexName.cstr(), sRename.cstr(), sOldest.cstr() );
			break;
		}

		// exclusive reader (to make sure that disk chunks not used any more) and writer lock here
		// wipe out writer then way all readers get out - to delete indexes
		// as readers might keep copy of chunks vector
		Verify ( m_tWriting.Lock() );
		Verify ( m_tReading.WriteLock() );

		SafeDelete ( pOlder );
		SafeDelete ( pOldest );

		Verify ( m_tReading.Unlock() );
		Verify ( m_tWriting.Unlock() );

		// we might remove old index files
		sphUnlinkIndex ( sRename.cstr(), true );
		sphUnlinkIndex ( sOldest.cstr(), true );
		// FIXEME: wipe out 'merged' index files in case of error
	}

	m_bOptimizing = false;

	m_tWriting.Lock();
	m_dKillsWhileOptimizing.Resize(0);
	m_tWriting.Unlock();

	int64_t tmPass = sphMicroTimer() - tmStart;

	if ( g_bShutdown )
	{
		sphWarning ( "rt: index %s: optimization terminated chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	} else
	{
		sphInfo ( "rt: index %s: optimized (progressive) chunk(s) %d ( of %d ) in %d.%03d sec",
			m_sIndexName.cstr(), iChunks-m_dDiskChunks.GetLength(), iChunks, (int)(tmPass/1000000), (int)((tmPass/1000)%1000) );
	}
}


//////////////////////////////////////////////////////////////////////////
// STATUS
//////////////////////////////////////////////////////////////////////////

void RtIndex_c::GetStatus ( CSphIndexStatus * pRes ) const
{
	assert ( pRes );
	if ( !pRes )
		return;

	Verify ( m_tChunkLock.ReadLock() );

	pRes->m_iRamChunkSize = GetUsedRam()
		+ m_dRamChunks.AllocatedBytes ()
		+ m_dRamChunks.GetLength()*int(sizeof(RtSegment_t));

	pRes->m_iRamUse = sizeof( RtIndex_c )
		+ m_dDiskChunks.AllocatedBytes ()
		+ pRes->m_iRamChunkSize;

	pRes->m_iRamRetired = 0;
	ARRAY_FOREACH ( i, m_dRetired )
		pRes->m_iRamRetired += m_dRetired[i]->GetUsedRam();

	pRes->m_iMemLimit = m_iSoftRamLimit;
	pRes->m_iDiskUse = 0;

	CSphString sError;
	char sFile [ SPH_MAX_FILENAME_LEN ];
	const char * sFiles[] = { ".meta", ".ram" };
	for ( const char * sName : sFiles )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", m_sFilename.cstr(), sName );
		CSphAutofile fdRT ( sFile, SPH_O_READ, sError );
		int64_t iFileSize = fdRT.GetSize();
		if ( iFileSize>0 )
			pRes->m_iDiskUse += iFileSize;
	}
	CSphIndexStatus tDisk;
	ARRAY_FOREACH ( i, m_dDiskChunks )
	{
		m_dDiskChunks[i]->GetStatus(&tDisk);
		pRes->m_iRamUse += tDisk.m_iRamUse;
		pRes->m_iDiskUse += tDisk.m_iDiskUse;
	}

	pRes->m_iNumChunks = m_dDiskChunks.GetLength();

	Verify ( m_tChunkLock.Unlock() );
}

//////////////////////////////////////////////////////////////////////////
// RECONFIGURE
//////////////////////////////////////////////////////////////////////////

bool CreateReconfigure ( const CSphString & sIndexName, bool bIsStarDict, const ISphFieldFilter * pFieldFilter,
	const CSphIndexSettings & tIndexSettings, uint64_t uTokHash, uint64_t uDictHash, int iMaxCodepointLength,
	bool bSame, CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError )
{
	// FIXME!!! check missed embedded files
	ISphTokenizerRefPtr_c pTokenizer { ISphTokenizer::Create ( tSettings.m_tTokenizer, NULL, sError ) };
	if ( !pTokenizer )
	{
		sError.SetSprintf ( "'%s' failed to create tokenizer, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// dict setup second
	CSphDictRefPtr_c tDict { sphCreateDictionaryCRC ( tSettings.m_tDict, NULL, pTokenizer, sIndexName.cstr(), false, tIndexSettings.m_iSkiplistBlockSize, sError ) };
	if ( !tDict )
	{
		sError.SetSprintf ( "'%s' failed to create dictionary, error '%s'", sIndexName.cstr(), sError.cstr() );
		return true;
	}

	// multiforms right after dict
	pTokenizer = ISphTokenizer::CreateMultiformFilter ( pTokenizer, tDict->GetMultiWordforms() );

	// bigram filter
	if ( tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_NONE && tSettings.m_tIndex.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		pTokenizer->SetBuffer ( (BYTE*)tSettings.m_tIndex.m_sBigramWords.cstr(), tSettings.m_tIndex.m_sBigramWords.Length() );

		BYTE * pTok = NULL;
		while ( ( pTok = pTokenizer->GetToken() )!=NULL )
			tSettings.m_tIndex.m_dBigramWords.Add() = (const char*)pTok;

		tSettings.m_tIndex.m_dBigramWords.Sort();
	}

	bool bNeedExact = ( tDict->HasMorphology() || tDict->GetWordformsFileInfos().GetLength() );
	if ( tSettings.m_tIndex.m_bIndexExactWords && !bNeedExact )
		tSettings.m_tIndex.m_bIndexExactWords = false;

	if ( tDict->GetSettings().m_bWordDict && tDict->HasMorphology() && bIsStarDict && !tSettings.m_tIndex.m_bIndexExactWords )
		tSettings.m_tIndex.m_bIndexExactWords = true;

	// field filter
	ISphFieldFilterRefPtr_c tFieldFilter;

	// re filter
	bool bReFilterSame = true;
	CSphFieldFilterSettings tFieldFilterSettings;
	if ( pFieldFilter )
		pFieldFilter->GetSettings ( tFieldFilterSettings );
	if ( tFieldFilterSettings.m_dRegexps.GetLength()!=tSettings.m_tFieldFilter.m_dRegexps.GetLength() )
	{
		bReFilterSame = false;
	} else
	{
		CSphVector<uint64_t> dFieldFilter;
		ARRAY_FOREACH ( i, tFieldFilterSettings.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tFieldFilterSettings.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uMyFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		dFieldFilter.Resize ( 0 );
		ARRAY_FOREACH ( i, tSettings.m_tFieldFilter.m_dRegexps )
			dFieldFilter.Add ( sphFNV64 ( tSettings.m_tFieldFilter.m_dRegexps[i].cstr() ) );
		dFieldFilter.Uniq();
		uint64_t uNewFF = sphFNV64 ( dFieldFilter.Begin(), sizeof(dFieldFilter[0]) * dFieldFilter.GetLength() );

		bReFilterSame = ( uMyFF==uNewFF );
	}

	if ( !bReFilterSame && tSettings.m_tFieldFilter.m_dRegexps.GetLength () )
	{
		tFieldFilter = sphCreateRegexpFilter ( tSettings.m_tFieldFilter, sError );
		if ( !tFieldFilter )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// rlp filter
	bool bRlpSame = ( tIndexSettings.m_eChineseRLP==tSettings.m_tIndex.m_eChineseRLP );
	if ( !bRlpSame )
	{
		if ( !sphSpawnRLPFilter ( tFieldFilter, tSettings.m_tIndex, tSettings.m_tTokenizer, sIndexName.cstr (), sError ) )
		{
			sError.SetSprintf ( "'%s' failed to create field filter, error '%s'", sIndexName.cstr (), sError.cstr () );
			return true;
		}
	}

	// compare options
	if ( !bSame || uTokHash!=pTokenizer->GetSettingsFNV() || uDictHash!=tDict->GetSettingsFNV() ||
		iMaxCodepointLength!=pTokenizer->GetMaxCodepointLength() || sphGetSettingsFNV ( tIndexSettings )!=sphGetSettingsFNV ( tSettings.m_tIndex ) ||
		!bReFilterSame || !bRlpSame )
	{
		tSetup.m_pTokenizer = pTokenizer.Leak();
		tSetup.m_pDict = tDict.Leak();
		tSetup.m_tIndex = tSettings.m_tIndex;
		tSetup.m_pFieldFilter = tFieldFilter.Leak();
		return false;
	} else
	{
		return true;
	}
}

bool RtIndex_c::IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const
{
	return CreateReconfigure ( m_sIndexName, IsStarDict(), m_pFieldFilter, m_tSettings,
		m_pTokenizer->GetSettingsFNV(), m_pDict->GetSettingsFNV(), m_pTokenizer->GetMaxCodepointLength(), true, tSettings, tSetup, sError );
}

void RtIndex_c::Reconfigure ( CSphReconfigureSetup & tSetup )
{
	ForceDiskChunk();

	Setup ( tSetup.m_tIndex );
	SetTokenizer ( tSetup.m_pTokenizer );
	SetDictionary ( tSetup.m_pDict );
	SetFieldFilter ( tSetup.m_pFieldFilter );

	m_iMaxCodepointLength = m_pTokenizer->GetMaxCodepointLength();
	SetupQueryTokenizer();

	// FIXME!!! handle error
	m_pTokenizerIndexing = m_pTokenizer->Clone ( SPH_CLONE_INDEX );
	ISphTokenizerRefPtr_c pIndexing { ISphTokenizer::CreateBigramFilter ( m_pTokenizerIndexing, m_tSettings.m_eBigramIndex, m_tSettings.m_sBigramWords, m_sLastError ) };
	if ( pIndexing )
		m_pTokenizerIndexing = pIndexing;
}


int	RtIndex_c::Kill ( DocID_t tDocID )
{
	assert ( 0 && "No external kills for RT");
	return 0;
}


int RtIndex_c::KillMulti ( const DocID_t * pKlist, int iKlistSize )
{
	assert ( 0 && "No external kills for RT");
	return 0;
}


uint64_t sphGetSettingsFNV ( const CSphIndexSettings & tSettings )
{
	uint64_t uHash = 0;

	DWORD uFlags = 0;
	if ( tSettings.m_bHtmlStrip )
		uFlags |= 1<<1;
	if ( tSettings.m_bIndexExactWords )
		uFlags |= 1<<2;
	if ( tSettings.m_bIndexFieldLens )
		uFlags |= 1<<3;
	if ( tSettings.m_bIndexSP )
		uFlags |= 1<<4;
	uHash = sphFNV64 ( &uFlags, sizeof(uFlags), uHash );

	uHash = sphFNV64 ( &tSettings.m_eHitFormat, sizeof(tSettings.m_eHitFormat), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlIndexAttrs.cstr(), tSettings.m_sHtmlIndexAttrs.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sHtmlRemoveElements.cstr(), tSettings.m_sHtmlRemoveElements.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sZones.cstr(), tSettings.m_sZones.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eHitless, sizeof(tSettings.m_eHitless), uHash );
	uHash = sphFNV64 ( tSettings.m_sHitlessFiles.cstr(), tSettings.m_sHitlessFiles.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_eBigramIndex, sizeof(tSettings.m_eBigramIndex), uHash );
	uHash = sphFNV64 ( tSettings.m_sBigramWords.cstr(), tSettings.m_sBigramWords.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_uAotFilterMask, sizeof(tSettings.m_uAotFilterMask), uHash );
	uHash = sphFNV64 ( &tSettings.m_eChineseRLP, sizeof(tSettings.m_eChineseRLP), uHash );
	uHash = sphFNV64 ( tSettings.m_sRLPContext.cstr(), tSettings.m_sRLPContext.Length(), uHash );
	uHash = sphFNV64 ( tSettings.m_sIndexTokenFilter.cstr(), tSettings.m_sIndexTokenFilter.Length(), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMinPrefixLen, sizeof(tSettings.m_iMinPrefixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMinInfixLen, sizeof(tSettings.m_iMinInfixLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iMaxSubstringLen, sizeof(tSettings.m_iMaxSubstringLen), uHash );
	uHash = sphFNV64 ( &tSettings.m_iBoundaryStep, sizeof(tSettings.m_iBoundaryStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iOvershortStep, sizeof(tSettings.m_iOvershortStep), uHash );
	uHash = sphFNV64 ( &tSettings.m_iStopwordStep, sizeof(tSettings.m_iStopwordStep), uHash );

	return uHash;
}

//////////////////////////////////////////////////////////////////////////
// BINLOG
//////////////////////////////////////////////////////////////////////////

extern DWORD g_dSphinxCRC32 [ 256 ];


static CSphString MakeBinlogName ( const char * sPath, int iExt )
{
	CSphString sName;
	sName.SetSprintf ( "%s/binlog.%03d", sPath, iExt );
	return sName;
}


BinlogWriter_c::BinlogWriter_c ()
{
	m_iLastWritePos = 0;
	m_iLastFsyncPos = 0;
	m_iLastCrcPos = 0;
	ResetCrc();
}


void BinlogWriter_c::ResetCrc ()
{
	m_uCRC = ~((DWORD)0);
	m_iLastCrcPos = m_iPoolUsed;
}


void BinlogWriter_c::HashCollected ()
{
	assert ( m_iLastCrcPos<=m_iPoolUsed );

	const BYTE * b = m_pBuffer + m_iLastCrcPos;
	int iSize = m_iPoolUsed - m_iLastCrcPos;
	DWORD uCRC = m_uCRC;

	for ( int i=0; i<iSize; i++ )
		uCRC = (uCRC >> 8) ^ g_dSphinxCRC32 [ (uCRC ^ *b++) & 0xff ];

	m_iLastCrcPos = m_iPoolUsed;
	m_uCRC = uCRC;
}


void BinlogWriter_c::WriteCrc ()
{
	HashCollected();
	m_uCRC = ~m_uCRC;
	CSphWriter::PutDword ( m_uCRC );
	ResetCrc();
}


void BinlogWriter_c::Flush ()
{
	Write();
	Fsync();
	m_iLastCrcPos = m_iPoolUsed;
}


void BinlogWriter_c::Write ()
{
	if ( m_iPoolUsed<=0 )
		return;

	HashCollected();
	CSphWriter::Flush();
	m_iLastWritePos = GetPos();
}


#if USE_WINDOWS
int fsync ( int iFD )
{
	// map fd to handle
	HANDLE h = (HANDLE) _get_osfhandle ( iFD );
	if ( h==INVALID_HANDLE_VALUE )
	{
		errno = EBADF;
		return -1;
	}

	// do flush
	if ( FlushFileBuffers(h) )
		return 0;

	// error handling
	errno = EIO;
	if ( GetLastError()==ERROR_INVALID_HANDLE )
		errno = EINVAL;
	return -1;
}
#endif


void BinlogWriter_c::Fsync ()
{
	if ( !HasUnsyncedData() )
		return;

	m_bError = ( fsync ( m_iFD )!=0 );
	if ( m_bError && m_pError )
		m_pError->SetSprintf ( "failed to sync %s: %s" , m_sName.cstr(), strerrorm(errno) );

	m_iLastFsyncPos = GetPos();
}

//////////////////////////////////////////////////////////////////////////

BinlogReader_c::BinlogReader_c()
{
	ResetCrc ();
}

void BinlogReader_c::ResetCrc ()
{
	m_uCRC = ~(DWORD(0));
	m_iLastCrcPos = m_iBuffPos;
}


bool BinlogReader_c::CheckCrc ( const char * sOp, const char * sIndexName, int64_t iTid, int64_t iTxnPos )
{
	HashCollected ();
	DWORD uCRC = ~m_uCRC;
	DWORD uRef = CSphAutoreader::GetDword();
	ResetCrc();
	bool bPassed = ( uRef==uCRC );
	if ( !bPassed )
		sphWarning ( "binlog: %s: CRC mismatch (index=%s, tid=" INT64_FMT ", pos=" INT64_FMT ")", sOp, sIndexName ? sIndexName : "", iTid, iTxnPos );
	return bPassed;
}


void BinlogReader_c::UpdateCache ()
{
	HashCollected();
	CSphAutoreader::UpdateCache();
	m_iLastCrcPos = m_iBuffPos;
}

void BinlogReader_c::HashCollected ()
{
	assert ( m_iLastCrcPos<=m_iBuffPos );

	const BYTE * b = m_pBuff + m_iLastCrcPos;
	int iSize = m_iBuffPos - m_iLastCrcPos;
	DWORD uCRC = m_uCRC;

	for ( int i=0; i<iSize; i++ )
		uCRC = (uCRC >> 8) ^ g_dSphinxCRC32 [ (uCRC ^ *b++) & 0xff ];

	m_iLastCrcPos = m_iBuffPos;
	m_uCRC = uCRC;
}

//////////////////////////////////////////////////////////////////////////

RtBinlog_c::RtBinlog_c ()
	: m_iFlushTimeLeft ( 0 )
	, m_iFlushPeriod ( BINLOG_AUTO_FLUSH )
	, m_eOnCommit ( ACTION_NONE )
	, m_iLockFD ( -1 )
	, m_bReplayMode ( false )
	, m_bDisabled ( true )
	, m_iRestartSize ( 268435456 )
{
	MEMORY ( MEM_BINLOG );

	m_tWriter.SetBufferSize ( BINLOG_WRITE_BUFFER );
}

RtBinlog_c::~RtBinlog_c ()
{
	if ( !m_bDisabled )
	{
		m_iFlushPeriod = 0;
		DoCacheWrite();
		m_tWriter.CloseFile();
		LockFile ( false );
	}
}


void RtBinlog_c::BinlogCommit ( int64_t * pTID, const char * sIndexName, const RtSegment_t * pSeg, const CSphVector<DocID_t> & dKlist, bool bKeywordDict )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_COMMIT );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// save txn data
	if ( !pSeg || !pSeg->m_uRows )
		m_tWriter.ZipOffset ( 0 );
	else
	{
		m_tWriter.ZipOffset ( pSeg->m_uRows );
		SaveVector ( m_tWriter, pSeg->m_dWords );
		m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints.GetLength() );
		if ( !bKeywordDict )
		{
			ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
			{
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_uWordID );
			}
		} else
		{
			const char * pBase = (const char *)pSeg->m_dKeywordCheckpoints.Begin();
			ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
			{
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_iOffset );
				m_tWriter.ZipOffset ( pSeg->m_dWordCheckpoints[i].m_sWord - pBase );
			}
		}
		SaveVector ( m_tWriter, pSeg->m_dDocs );
		SaveVector ( m_tWriter, pSeg->m_dHits );
		SaveVector ( m_tWriter, pSeg->m_dRows );
		SaveVector ( m_tWriter, pSeg->m_dBlobs );
		SaveVector ( m_tWriter, pSeg->m_dKeywordCheckpoints );
	}
	SaveVector ( m_tWriter, dKlist );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::BinlogUpdateAttributes ( int64_t * pTID, const char * sIndexName, const CSphAttrUpdate & tUpd )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_UPDATE_ATTRS );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// update data
	m_tWriter.ZipOffset ( tUpd.m_dAttributes.GetLength() );
	for ( const auto & i : tUpd.m_dAttributes )
	{
		m_tWriter.PutString ( i.m_sName );
		m_tWriter.ZipOffset ( i.m_eType );
	}

	// POD vectors
	SaveVector ( m_tWriter, tUpd.m_dPool );
	SaveVector ( m_tWriter, tUpd.m_dDocids );
	SaveVector ( m_tWriter, tUpd.m_dRowOffset );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::BinlogReconfigure ( int64_t * pTID, const char * sIndexName, const CSphReconfigureSetup & tSetup )
{
	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_RECONFIGURE );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	// reconfigure data
	SaveIndexSettings ( m_tWriter, tSetup.m_tIndex );
	SaveTokenizerSettings ( m_tWriter, tSetup.m_pTokenizer, 0 );
	SaveDictionarySettings ( m_tWriter, tSetup.m_pDict, false, 0 );
	SaveFieldFilterSettings ( m_tWriter, tSetup.m_pFieldFilter );

	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}


// here's been going binlogs with ALL closed indices removing
void RtBinlog_c::NotifyIndexFlush ( const char * sIndexName, int64_t iTID, bool bShutdown )
{
	if ( m_bReplayMode )
		sphInfo ( "index '%s': ramchunk saved. TID=" INT64_FMT "", sIndexName, iTID );

	if ( m_bReplayMode || m_bDisabled )
		return;

	MEMORY ( MEM_BINLOG );
	assert ( bShutdown || m_dLogFiles.GetLength() );

	Verify ( m_tWriteLock.Lock() );

	bool bCurrentLogShut = false;
	const int iPreflushFiles = m_dLogFiles.GetLength();

	// loop through all log files, and check if we can unlink any
	ARRAY_FOREACH ( iLog, m_dLogFiles )
	{
		BinlogFileDesc_t & tLog = m_dLogFiles[iLog];
		bool bUsed = false;

		// update index info for this log file
		ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
		{
			BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];

			// this index was just flushed, update flushed TID
			if ( tIndex.m_sName==sIndexName )
			{
				assert ( iTID>=tIndex.m_iFlushedTID );
				tIndex.m_iFlushedTID = Max ( tIndex.m_iFlushedTID, iTID );
			}

			// if max logged TID is greater than last flushed TID, log file still has needed recovery data
			if ( tIndex.m_iFlushedTID < tIndex.m_iMaxTID )
				bUsed = true;
		}

		// it's needed, keep looking
		if ( bUsed )
			continue;

		// hooray, we can remove this log!
		// if this is our current log, we have to close it first
		if ( iLog==m_dLogFiles.GetLength()-1 )
		{
			m_tWriter.CloseFile ();
			bCurrentLogShut = true;
		}

		// do unlink
		CSphString sLog = MakeBinlogName ( m_sLogPath.cstr(), tLog.m_iExt );
		if ( ::unlink ( sLog.cstr() ) )
			sphWarning ( "binlog: failed to unlink %s: %s (remove it manually)", sLog.cstr(), strerrorm(errno) );

		// we need to reset it, otherwise there might be leftover data after last Remove()
		m_dLogFiles[iLog] = BinlogFileDesc_t();
		// quit tracking it
		m_dLogFiles.Remove ( iLog-- );
	}

	if ( bCurrentLogShut && !bShutdown )
	{
		// if current log was closed, we need a new one (it will automatically save meta, too)
		OpenNewLog ();

	} else if ( iPreflushFiles!=m_dLogFiles.GetLength() )
	{
		// if we unlinked any logs, we need to save meta, too
		SaveMeta ();
	}

	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::Configure ( const CSphConfigSection & hSearchd, bool bTestMode )
{
	MEMORY ( MEM_BINLOG );

	const int iMode = hSearchd.GetInt ( "binlog_flush", 2 );
	switch ( iMode )
	{
		case 0:		m_eOnCommit = ACTION_NONE; break;
		case 1:		m_eOnCommit = ACTION_FSYNC; break;
		case 2:		m_eOnCommit = ACTION_WRITE; break;
		default:	sphDie ( "unknown binlog flush mode %d (must be 0, 1, or 2)\n", iMode );
	}

#ifndef DATADIR
#define DATADIR "."
#endif

	m_sLogPath = hSearchd.GetStr ( "binlog_path", bTestMode ? "" : DATADIR );
	m_bDisabled = m_sLogPath.IsEmpty();

	m_iRestartSize = hSearchd.GetSize ( "binlog_max_log_size", m_iRestartSize );

	if ( !m_bDisabled )
	{
		LockFile ( true );
		LoadMeta();
	}
}

void RtBinlog_c::Replay ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags,
	ProgressCallbackSimple_t * pfnProgressCallback )
{
	if ( m_bDisabled )
		return;

	// on replay started
	if ( pfnProgressCallback )
		pfnProgressCallback();

	int64_t tmReplay = sphMicroTimer();
	// do replay
	m_bReplayMode = true;
	int iLastLogState = 0;
	ARRAY_FOREACH ( i, m_dLogFiles )
	{
		iLastLogState = ReplayBinlog ( hIndexes, uReplayFlags, i );
		if ( pfnProgressCallback ) // on each replayed binlog
			pfnProgressCallback();
	}

	if ( m_dLogFiles.GetLength()>0 )
	{
		tmReplay = sphMicroTimer() - tmReplay;
		sphInfo ( "binlog: finished replaying total %d in %d.%03d sec",
			m_dLogFiles.GetLength(),
			(int)(tmReplay/1000000), (int)((tmReplay/1000)%1000) );
	}

	// FIXME?
	// in some cases, indexes might had been flushed during replay
	// and we might therefore want to update m_iFlushedTID everywhere
	// but for now, let's just wait until next flush for simplicity

	// resume normal operation
	m_bReplayMode = false;
	OpenNewLog ( iLastLogState );
}

void RtBinlog_c::GetFlushInfo ( BinlogFlushInfo_t & tFlush )
{
	if ( !m_bDisabled && m_eOnCommit!=ACTION_FSYNC )
	{
		m_iFlushTimeLeft = sphMicroTimer() + m_iFlushPeriod;
		tFlush.m_pLog = this;
		tFlush.m_fnWork = RtBinlog_c::DoAutoFlush;
	}
}

void RtBinlog_c::DoAutoFlush ( void * pBinlog )
{
	assert ( pBinlog );
	RtBinlog_c * pLog = (RtBinlog_c *)pBinlog;
	assert ( !pLog->m_bDisabled );

	if ( pLog->m_iFlushPeriod>0 && pLog->m_iFlushTimeLeft<sphMicroTimer() )
	{
		MEMORY ( MEM_BINLOG );

		pLog->m_iFlushTimeLeft = sphMicroTimer() + pLog->m_iFlushPeriod;

		if ( pLog->m_eOnCommit==ACTION_NONE || pLog->m_tWriter.HasUnwrittenData() )
		{
			Verify ( pLog->m_tWriteLock.Lock() );
			pLog->m_tWriter.Flush();
			Verify ( pLog->m_tWriteLock.Unlock() );
		}

		if ( pLog->m_tWriter.HasUnsyncedData() )
			pLog->m_tWriter.Fsync();
	}
}

int RtBinlog_c::GetWriteIndexID ( const char * sName, int64_t iTID, int64_t tmNow )
{
	MEMORY ( MEM_BINLOG );
	assert ( m_dLogFiles.GetLength() );

	// OPTIMIZE? maybe hash them?
	BinlogFileDesc_t & tLog = m_dLogFiles.Last();
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
	{
		BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];
		if ( tIndex.m_sName==sName )
		{
			tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
			tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmNow );
			return i;
		}
	}

	// create a new entry
	int iID = tLog.m_dIndexInfos.GetLength();
	BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos.Add(); // caller must hold a wlock
	tIndex.m_sName = sName;
	tIndex.m_iMinTID = iTID;
	tIndex.m_iMaxTID = iTID;
	tIndex.m_iFlushedTID = 0;
	tIndex.m_tmMin = tmNow;
	tIndex.m_tmMax = tmNow;

	// log this new entry
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_ADD_INDEX );
	m_tWriter.ZipOffset ( iID );
	m_tWriter.PutString ( sName );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );
	m_tWriter.WriteCrc ();

	// return the index
	return iID;
}

void RtBinlog_c::LoadMeta ()
{
	MEMORY ( MEM_BINLOG );

	CSphString sMeta;
	sMeta.SetSprintf ( "%s/binlog.meta", m_sLogPath.cstr() );
	if ( !sphIsReadable ( sMeta.cstr() ) )
		return;

	CSphString sError;

	// opened and locked, lets read
	CSphAutoreader rdMeta;
	if ( !rdMeta.Open ( sMeta, sError ) )
		sphDie ( "%s error: %s", sMeta.cstr(), sError.cstr() );

	if ( rdMeta.GetDword()!=BINLOG_META_MAGIC )
		sphDie ( "invalid meta file %s", sMeta.cstr() );

	// binlog meta v1 was dev only, crippled, and we don't like it anymore
	// binlog metas v2 upto current v4 (and likely up) share the same simplistic format
	// so let's support empty (!) binlogs w/ known versions and compatible metas
	DWORD uVersion = rdMeta.GetDword();
	if ( uVersion==1 || uVersion>BINLOG_VERSION )
		sphDie ( "binlog meta file %s is v.%d, binary is v.%d; recovery requires previous binary version",
			sMeta.cstr(), uVersion, BINLOG_VERSION );

	const bool bLoaded64bit = ( rdMeta.GetByte()==1 );
	m_dLogFiles.Resize ( rdMeta.UnzipInt() ); // FIXME! sanity check

	if ( !m_dLogFiles.GetLength() )
		return;

	// ok, so there is actual recovery data
	// let's require that exact version and bitness, then
	if ( uVersion!=BINLOG_VERSION )
		sphDie ( "binlog meta file %s is v.%d, binary is v.%d; recovery requires previous binary version",
			sMeta.cstr(), uVersion, BINLOG_VERSION );

	if ( !bLoaded64bit )
		sphDie ( "indexes with 32-bit docids are no longer supported; recovery requires previous binary version" );

	// load list of active log files
	ARRAY_FOREACH ( i, m_dLogFiles )
		m_dLogFiles[i].m_iExt = rdMeta.UnzipInt(); // everything else is saved in logs themselves
}

void RtBinlog_c::SaveMeta ()
{
	MEMORY ( MEM_BINLOG );

	CSphString sMeta, sMetaOld;
	sMeta.SetSprintf ( "%s/binlog.meta.new", m_sLogPath.cstr() );
	sMetaOld.SetSprintf ( "%s/binlog.meta", m_sLogPath.cstr() );

	CSphString sError;

	// opened and locked, lets write
	CSphWriter wrMeta;
	if ( !wrMeta.OpenFile ( sMeta, sError ) )
		sphDie ( "failed to open '%s': '%s'", sMeta.cstr(), sError.cstr() );

	wrMeta.PutDword ( BINLOG_META_MAGIC );
	wrMeta.PutDword ( BINLOG_VERSION );
	wrMeta.PutByte ( 1 ); // was USE_64BIT

	// save list of active log files
	wrMeta.ZipInt ( m_dLogFiles.GetLength() );
	ARRAY_FOREACH ( i, m_dLogFiles )
		wrMeta.ZipInt ( m_dLogFiles[i].m_iExt ); // everything else is saved in logs themselves

	wrMeta.CloseFile();

	if ( wrMeta.IsError() )
	{
		sphWarning ( "%s", sError.cstr() );
		return;
	}

	if ( sph::rename ( sMeta.cstr(), sMetaOld.cstr() ) )
		sphDie ( "failed to rename meta (src=%s, dst=%s, errno=%d, error=%s)",
			sMeta.cstr(), sMetaOld.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully
	sphLogDebug ( "SaveMeta: Done." );
}

void RtBinlog_c::LockFile ( bool bLock )
{
	CSphString sName;
	sName.SetSprintf ( "%s/binlog.lock", m_sLogPath.cstr() );

	if ( bLock )
	{
		assert ( m_iLockFD==-1 );
		const int iLockFD = ::open ( sName.cstr(), SPH_O_NEW, 0644 );

		if ( iLockFD<0 )
			sphDie ( "failed to open '%s': %u '%s'", sName.cstr(), errno, strerrorm(errno) );

		if ( !sphLockEx ( iLockFD, false ) )
			sphDie ( "failed to lock '%s': %u '%s'", sName.cstr(), errno, strerrorm(errno) );

		m_iLockFD = iLockFD;
	} else
	{
		if ( m_iLockFD>=0 )
			sphLockUn ( m_iLockFD );
		SafeClose ( m_iLockFD );
		::unlink ( sName.cstr()	);
	}
}

void RtBinlog_c::OpenNewLog ( int iLastState )
{
	MEMORY ( MEM_BINLOG );

	// calc new ext
	int iExt = 1;
	if ( m_dLogFiles.GetLength() )
	{
		iExt = m_dLogFiles.Last().m_iExt;
		if ( !iLastState )
			iExt++;
	}

	// create entry
	// we need to reset it, otherwise there might be leftover data after last Remove()
	BinlogFileDesc_t tLog;
	tLog.m_iExt = iExt;
	m_dLogFiles.Add ( tLog );

	// create file
	CSphString sLog = MakeBinlogName ( m_sLogPath.cstr(), tLog.m_iExt );

	if ( !iLastState ) // reuse the last binlog since it is empty or useless.
		::unlink ( sLog.cstr() );

	if ( !m_tWriter.OpenFile ( sLog.cstr(), m_sWriterError ) )
		sphDie ( "failed to create %s: errno=%d, error=%s", sLog.cstr(), errno, strerrorm(errno) );

	// emit header
	m_tWriter.PutDword ( BINLOG_HEADER_MAGIC );
	m_tWriter.PutDword ( BINLOG_VERSION );

	// update meta
	SaveMeta();
}

void RtBinlog_c::DoCacheWrite ()
{
	if ( !m_dLogFiles.GetLength() )
		return;
	const CSphVector<BinlogIndexInfo_t> & dIndexes = m_dLogFiles.Last().m_dIndexInfos;

	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( BLOP_ADD_CACHE );
	m_tWriter.ZipOffset ( dIndexes.GetLength() );
	ARRAY_FOREACH ( i, dIndexes )
	{
		m_tWriter.PutString ( dIndexes[i].m_sName.cstr() );
		m_tWriter.ZipOffset ( dIndexes[i].m_iMinTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_iMaxTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_iFlushedTID );
		m_tWriter.ZipOffset ( dIndexes[i].m_tmMin );
		m_tWriter.ZipOffset ( dIndexes[i].m_tmMax );
	}
	m_tWriter.WriteCrc ();
}

void RtBinlog_c::CheckDoRestart ()
{
	// restart on exceed file size limit
	if ( m_iRestartSize>0 && m_tWriter.GetPos()>m_iRestartSize )
	{
		MEMORY ( MEM_BINLOG );

		assert ( m_dLogFiles.GetLength() );

		DoCacheWrite();
		m_tWriter.CloseFile();
		OpenNewLog();
	}
}

void RtBinlog_c::CheckDoFlush ()
{
	if ( m_eOnCommit==ACTION_NONE )
		return;

	if ( m_eOnCommit==ACTION_WRITE && m_tWriter.HasUnwrittenData() )
		m_tWriter.Write();

	if ( m_eOnCommit==ACTION_FSYNC && m_tWriter.HasUnsyncedData() )
	{
		if ( m_tWriter.HasUnwrittenData() )
			m_tWriter.Write();

		m_tWriter.Fsync();
	}
}

int RtBinlog_c::ReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, int iBinlog )
{
	assert ( iBinlog>=0 && iBinlog<m_dLogFiles.GetLength() );
	CSphString sError;

	const CSphString sLog ( MakeBinlogName ( m_sLogPath.cstr(), m_dLogFiles[iBinlog].m_iExt ) );
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	// open, check, play
	sphInfo ( "binlog: replaying log %s", sLog.cstr() );

	BinlogReader_c tReader;
	if ( !tReader.Open ( sLog, sError ) )
	{
		if ( ( uReplayFlags & SPH_REPLAY_IGNORE_OPEN_ERROR )!=0 )
		{
			sphWarning ( "binlog: log open error: %s", sError.cstr() );
			return 0;
		}
		sphDie ( "binlog: log open error: %s", sError.cstr() );
	}

	const SphOffset_t iFileSize = tReader.GetFilesize();

	if ( !iFileSize )
	{
		sphWarning ( "binlog: empty binlog %s detected, skipping", sLog.cstr() );
		return -1;
	}

	if ( tReader.GetDword()!=BINLOG_HEADER_MAGIC )
		sphDie ( "binlog: log %s missing magic header (corrupted?)", sLog.cstr() );

	DWORD uVersion = tReader.GetDword();
	if ( uVersion!=BINLOG_VERSION || tReader.GetErrorFlag() )
		sphDie ( "binlog: log %s is v.%d, binary is v.%d; recovery requires previous binary version", sLog.cstr(), uVersion, BINLOG_VERSION );

	/////////////
	// do replay
	/////////////

	int dTotal [ BLOP_TOTAL+1 ];
	memset ( dTotal, 0, sizeof(dTotal) );

	// !COMMIT
	// instead of simply replaying everything, we should check whether this binlog is clean
	// by loading and checking the cache stored at its very end
	tLog.m_dIndexInfos.Reset();

	bool bReplayOK = true;
	bool bHaveCacheOp = false;
	int64_t iPos = -1;

	m_iReplayedRows = 0;
	int64_t tmReplay = sphMicroTimer();

	while ( iFileSize!=tReader.GetPos() && !tReader.GetErrorFlag() && bReplayOK )
	{
		iPos = tReader.GetPos();
		if ( tReader.GetDword()!=BLOP_MAGIC )
		{
			sphDie ( "binlog: log missing txn marker at pos=" INT64_FMT " (corrupted?)", iPos );
			break;
		}

		tReader.ResetCrc ();
		const uint64_t uOp = tReader.UnzipOffset ();

		if ( uOp<=0 || uOp>=BLOP_TOTAL )
			sphDie ( "binlog: unexpected entry (blop=" UINT64_FMT ", pos=" INT64_FMT ")", uOp, iPos );

		// FIXME! blop might be OK but skipped (eg. index that is no longer)
		switch ( uOp )
		{
			case BLOP_COMMIT:
				bReplayOK = ReplayCommit ( iBinlog, uReplayFlags, tReader );
				break;

			case BLOP_UPDATE_ATTRS:
				bReplayOK = ReplayUpdateAttributes ( iBinlog, tReader );
				break;

			case BLOP_ADD_INDEX:
				bReplayOK = ReplayIndexAdd ( iBinlog, hIndexes, tReader );
				break;

			case BLOP_ADD_CACHE:
				if ( bHaveCacheOp )
					sphDie ( "binlog: internal error, second BLOP_ADD_CACHE detected (corruption?)" );
				bHaveCacheOp = true;
				bReplayOK = ReplayCacheAdd ( iBinlog, tReader );
				break;

			case BLOP_RECONFIGURE:
				bReplayOK = ReplayReconfigure ( iBinlog, uReplayFlags, tReader );
				break;

			case BLOP_PQ_ADD:
				bReplayOK = ReplayPqAdd ( iBinlog, uReplayFlags, tReader );
				break;

			case BLOP_PQ_DELETE:
				bReplayOK = ReplayPqDelete ( iBinlog, uReplayFlags, tReader );
				break;

			default:
				sphDie ( "binlog: internal error, unhandled entry (blop=%d)", (int)uOp );
		}

		dTotal [ uOp ] += bReplayOK ? 1 : 0;
		dTotal [ BLOP_TOTAL ]++;
	}

	tmReplay = sphMicroTimer() - tmReplay;

	if ( tReader.GetErrorFlag() )
		sphWarning ( "binlog: log io error at pos=" INT64_FMT ": %s", iPos, sError.cstr() );

	if ( !bReplayOK )
		sphWarning ( "binlog: replay error at pos=" INT64_FMT ")", iPos );

	// show additional replay statistics
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
	{
		const BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];
		if ( !hIndexes ( tIndex.m_sName.cstr() ) )
		{
			sphWarning ( "binlog: index %s: missing; tids " INT64_FMT " to " INT64_FMT " skipped!",
				tIndex.m_sName.cstr(), tIndex.m_iMinTID, tIndex.m_iMaxTID );

		} else if ( tIndex.m_iPreReplayTID < tIndex.m_iMaxTID )
		{
			sphInfo ( "binlog: index %s: recovered from tid " INT64_FMT " to tid " INT64_FMT,
				tIndex.m_sName.cstr(), tIndex.m_iPreReplayTID, tIndex.m_iMaxTID );

		} else
		{
			sphInfo ( "binlog: index %s: skipped at tid " INT64_FMT " and max binlog tid " INT64_FMT,
				tIndex.m_sName.cstr(), tIndex.m_iPreReplayTID, tIndex.m_iMaxTID );
		}
	}

	sphInfo ( "binlog: replay stats: %d rows in %d commits; %d updates, %d reconfigure; %d pq-add; %d pq-delete; %d indexes",
		m_iReplayedRows, dTotal[BLOP_COMMIT], dTotal[BLOP_UPDATE_ATTRS], dTotal[BLOP_RECONFIGURE], dTotal[BLOP_PQ_ADD], dTotal[BLOP_PQ_DELETE], dTotal[BLOP_ADD_INDEX] );
	sphInfo ( "binlog: finished replaying %s; %d.%d MB in %d.%03d sec",
		sLog.cstr(),
		(int)(iFileSize/1048576), (int)((iFileSize*10/1048576)%10),
		(int)(tmReplay/1000000), (int)((tmReplay/1000)%1000) );

	if ( bHaveCacheOp && dTotal[BLOP_TOTAL]==1 ) // only one operation, that is Add Cache - by the fact, empty binlog
		return 1;

	return 0;
}


static BinlogIndexInfo_t & ReplayIndexID ( BinlogReader_c & tReader, BinlogFileDesc_t & tLog, const char * sPlace )
{
	const int64_t iTxnPos = tReader.GetPos();
	const int iVal = (int)tReader.UnzipOffset();

	if ( iVal<0 || iVal>=tLog.m_dIndexInfos.GetLength() )
		sphDie ( "binlog: %s: unexpected index id (id=%d, max=%d, pos=" INT64_FMT ")",
			sPlace, iVal, tLog.m_dIndexInfos.GetLength(), iTxnPos );

	return tLog.m_dIndexInfos[iVal];
}


bool RtBinlog_c::ReplayCommit ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "commit" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	CSphScopedPtr<RtSegment_t> pSeg ( NULL );
	CSphVector<DocID_t> dKlist;

	DWORD uRows = tReader.UnzipOffset();
	if ( uRows )
	{
		pSeg = new RtSegment_t(uRows);
		pSeg->m_uRows = pSeg->m_tAliveRows = uRows;
		m_iReplayedRows += (int)uRows;

		LoadVector ( tReader, pSeg->m_dWords );
		pSeg->m_dWordCheckpoints.Resize ( (int) tReader.UnzipOffset() ); // FIXME! sanity check
		ARRAY_FOREACH ( i, pSeg->m_dWordCheckpoints )
		{
			pSeg->m_dWordCheckpoints[i].m_iOffset = (int) tReader.UnzipOffset();
			pSeg->m_dWordCheckpoints[i].m_uWordID = (SphWordID_t )tReader.UnzipOffset();
		}
		LoadVector ( tReader, pSeg->m_dDocs );
		LoadVector ( tReader, pSeg->m_dHits );
		LoadVector ( tReader, pSeg->m_dRows );
		LoadVector ( tReader, pSeg->m_dBlobs );
		LoadVector ( tReader, pSeg->m_dKeywordCheckpoints );

		pSeg->BuildDocID2RowIDMap();
	}

	LoadVector ( tReader, dKlist );

	// checksum
	if ( tReader.GetErrorFlag() || !tReader.CheckCrc ( "commit", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
		return false;

	// check TID
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: commit: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );

	// check timestamp
	if ( tmStamp<tIndex.m_tmMax )
	{
		if (!( uReplayFlags & SPH_REPLAY_ACCEPT_DESC_TIMESTAMP ))
			sphDie ( "binlog: commit: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

		sphWarning ( "binlog: commit: replaying txn despite descending time "
			"(index=%s, logtid=" INT64_FMT ", lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), iTID, tIndex.m_tmMax, tmStamp, iTxnPos );
		tIndex.m_tmMax = tmStamp;
	}

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pRT && iTID > tIndex.m_pRT->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pRT->m_iTID+1 )
			sphWarning ( "binlog: commit: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos );

		// in case dict=keywords
		// + cook checkpoint
		// + build infixes
		if ( tIndex.m_pRT->IsWordDict() && pSeg.Ptr() )
		{
			FixupSegmentCheckpoints ( pSeg.Ptr() );
			BuildSegmentInfixes ( pSeg.Ptr(), tIndex.m_pRT->GetDictionary()->HasMorphology(),
				tIndex.m_pRT->IsWordDict(), tIndex.m_pRT->GetSettings().m_iMinInfixLen, tIndex.m_pRT->GetWordCheckoint(), ( tIndex.m_pRT->GetMaxCodepointLength()>1 ) );
		}

		// actually replay
		tIndex.m_pRT->CommitReplayable ( pSeg.LeakPtr(), dKlist, NULL, false );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pRT->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

bool RtBinlog_c::ReplayIndexAdd ( int iBinlog, const SmallStringHash_T<CSphIndex*> & hIndexes, BinlogReader_c & tReader ) const
{
	// load and check index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	uint64_t uVal = tReader.UnzipOffset();
	if ( (int)uVal!=tLog.m_dIndexInfos.GetLength() )
		sphDie ( "binlog: indexadd: unexpected index id (id=" UINT64_FMT ", expected=%d, pos=" INT64_FMT ")",
			uVal, tLog.m_dIndexInfos.GetLength(), iTxnPos );

	// load data
	CSphString sName = tReader.GetString();

	// FIXME? use this for double checking?
	tReader.UnzipOffset (); // TID
	tReader.UnzipOffset (); // time

	if ( !tReader.CheckCrc ( "indexadd", sName.cstr(), 0, iTxnPos ) )
		return false;

	// check for index name dupes
	ARRAY_FOREACH ( i, tLog.m_dIndexInfos )
		if ( tLog.m_dIndexInfos[i].m_sName==sName )
			sphDie ( "binlog: duplicate index name (name=%s, dupeid=%d, pos=" INT64_FMT ")",
				sName.cstr(), i, iTxnPos );

	// not a dupe, lets add
	BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos.Add();
	tIndex.m_sName = sName;

	// lookup index in the list of currently served ones
	CSphIndex ** ppIndex = hIndexes ( sName.cstr() );
	CSphIndex * pIndex = ppIndex ? (*ppIndex) : NULL;
	if ( pIndex )
	{
		tIndex.m_pIndex = pIndex;
		if ( pIndex->IsRT() )
			tIndex.m_pRT = (RtIndex_c*)pIndex;

		if ( pIndex->IsPQ() )
			tIndex.m_pPQ = (PercolateIndex_i *)pIndex;

		tIndex.m_iPreReplayTID = pIndex->m_iTID;
		tIndex.m_iFlushedTID = pIndex->m_iTID;
	}

	// all ok
	// TID ranges will be now recomputed as we replay
	return true;
}

bool RtBinlog_c::ReplayUpdateAttributes ( int iBinlog, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "update" );

	// load transaction data
	CSphAttrUpdate tUpd;
	tUpd.m_bIgnoreNonexistent = true;

	int64_t iTID = (int64_t) tReader.UnzipOffset();
	int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	int iAttrs = (int)tReader.UnzipOffset();
	tUpd.m_dAttributes.Resize ( iAttrs ); // FIXME! sanity check
	for ( auto & i : tUpd.m_dAttributes )
	{
		i.m_sName = tReader.GetString();
		i.m_eType = (ESphAttr) tReader.UnzipOffset(); // safe, we'll crc check later
	}

	if ( tReader.GetErrorFlag()
		|| !LoadVector ( tReader, tUpd.m_dPool )
		|| !LoadVector ( tReader, tUpd.m_dDocids )
		|| !LoadVector ( tReader, tUpd.m_dRowOffset )
		|| !tReader.CheckCrc ( "update", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
	{
		return false;
	}

	// check TID, time order in log
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: update: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );
	if ( tmStamp<tIndex.m_tmMax )
		sphDie ( "binlog: update: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

	if ( tIndex.m_pIndex && iTID > tIndex.m_pIndex->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pIndex->m_iTID+1 )
			sphWarning ( "binlog: update: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pIndex->m_iTID, iTID, iTxnPos );

		CSphString sError, sWarning;
		bool bCritical = false;
		tIndex.m_pIndex->UpdateAttributes ( tUpd, -1, bCritical, sError, sWarning ); // FIXME! check for errors
		assert ( !bCritical ); // fixme! handle this

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pIndex->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

bool RtBinlog_c::ReplayCacheAdd ( int iBinlog, BinlogReader_c & tReader ) const
{
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];

	// load data
	CSphVector<BinlogIndexInfo_t> dCache;
	dCache.Resize ( (int) tReader.UnzipOffset() ); // FIXME! sanity check
	ARRAY_FOREACH ( i, dCache )
	{
		dCache[i].m_sName = tReader.GetString();
		dCache[i].m_iMinTID = tReader.UnzipOffset();
		dCache[i].m_iMaxTID = tReader.UnzipOffset();
		dCache[i].m_iFlushedTID = tReader.UnzipOffset();
		dCache[i].m_tmMin = tReader.UnzipOffset();
		dCache[i].m_tmMax = tReader.UnzipOffset();
	}
	if ( !tReader.CheckCrc ( "cache", "", 0, iTxnPos ) )
		return false;

	// if we arrived here by replay, let's verify everything
	// note that cached infos just passed checksumming, so the file is supposed to be clean!
	// in any case, broken log or not, we probably managed to replay something
	// so let's just report differences as warnings

	if ( dCache.GetLength()!=tLog.m_dIndexInfos.GetLength() )
	{
		sphWarning ( "binlog: cache mismatch: %d indexes cached, %d replayed",
			dCache.GetLength(), tLog.m_dIndexInfos.GetLength() );
		return true;
	}

	ARRAY_FOREACH ( i, dCache )
	{
		BinlogIndexInfo_t & tCache = dCache[i];
		BinlogIndexInfo_t & tIndex = tLog.m_dIndexInfos[i];

		if ( tCache.m_sName!=tIndex.m_sName )
		{
			sphWarning ( "binlog: cache mismatch: index %d name mismatch (%s cached, %s replayed)",
				i, tCache.m_sName.cstr(), tIndex.m_sName.cstr() );
			continue;
		}

		if ( tCache.m_iMinTID!=tIndex.m_iMinTID || tCache.m_iMaxTID!=tIndex.m_iMaxTID )
		{
			sphWarning ( "binlog: cache mismatch: index %s tid ranges mismatch "
				"(cached " INT64_FMT " to " INT64_FMT ", replayed " INT64_FMT " to " INT64_FMT ")",
				tCache.m_sName.cstr(),
				tCache.m_iMinTID, tCache.m_iMaxTID, tIndex.m_iMinTID, tIndex.m_iMaxTID );
		}
	}

	return true;
}

bool RtBinlog_c::ReplayReconfigure ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "reconfigure" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	CSphString sError;
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	CSphReconfigureSettings tSettings;
	LoadIndexSettings ( tSettings.m_tIndex, tReader, INDEX_FORMAT_VERSION );
	if ( !LoadTokenizerSettings ( tReader, tSettings.m_tTokenizer, tEmbeddedFiles, sError ) )
		sphDie ( "binlog: reconfigure: failed to load settings (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ", error=%s)",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos, sError.cstr() );

	LoadDictionarySettings ( tReader, tSettings.m_tDict, tEmbeddedFiles, sError );
	LoadFieldFilterSettings ( tReader, tSettings.m_tFieldFilter );

	// checksum
	if ( tReader.GetErrorFlag() || !tReader.CheckCrc ( "reconfigure", tIndex.m_sName.cstr(), iTID, iTxnPos ) )
		return false;

	// check TID
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: reconfigure: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );

	// check timestamp
	if ( tmStamp<tIndex.m_tmMax )
	{
		if (!( uReplayFlags & SPH_REPLAY_ACCEPT_DESC_TIMESTAMP ))
			sphDie ( "binlog: reconfigure: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

		sphWarning ( "binlog: reconfigure: replaying txn despite descending time "
			"(index=%s, logtid=" INT64_FMT ", lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			tIndex.m_sName.cstr(), iTID, tIndex.m_tmMax, tmStamp, iTxnPos );
		tIndex.m_tmMax = tmStamp;
	}

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pRT && iTID > tIndex.m_pRT->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		if ( iTID!=tIndex.m_pRT->m_iTID+1 )
			sphWarning ( "binlog: reconfigure: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos );

		sError = "";
		CSphReconfigureSetup tSetup;
		bool bSame = tIndex.m_pRT->IsSameSettings ( tSettings, tSetup, sError );

		if ( !sError.IsEmpty() )
			sphWarning ( "binlog: reconfigure: wrong settings (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ", error=%s)",
				tIndex.m_sName.cstr(), tIndex.m_pRT->m_iTID, iTID, iTxnPos, sError.cstr() );

		if ( !bSame )
			tIndex.m_pRT->Reconfigure ( tSetup );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pRT->m_iTID = iTID;
	}

	// update info
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
	return true;
}

void RtBinlog_c::CheckPath ( const CSphConfigSection & hSearchd, bool bTestMode )
{
#ifndef DATADIR
#define DATADIR "."
#endif

	m_sLogPath = hSearchd.GetStr ( "binlog_path", bTestMode ? "" : DATADIR );
	m_bDisabled = m_sLogPath.IsEmpty();

	if ( !m_bDisabled )
	{
		LockFile ( true );
		LockFile ( false );
	}
}

bool RtBinlog_c::CheckCrc ( const char * sOp, const CSphString & sIndex, int64_t iTID, int64_t iTxnPos, BinlogReader_c & tReader ) const
{
	return ( tReader.GetErrorFlag() || !tReader.CheckCrc ( sOp, sIndex.cstr(), iTID, iTxnPos ) );
}

void RtBinlog_c::CheckTid ( const char * sOp, const BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t iTxnPos ) const
{
	if ( iTID<tIndex.m_iMaxTID )
		sphDie ( "binlog: %s: descending tid (index=%s, lasttid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			sOp, tIndex.m_sName.cstr(), tIndex.m_iMaxTID, iTID, iTxnPos );
}


void RtBinlog_c::CheckTidSeq ( const char * sOp, const BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t iTxnPos ) const
{
	if ( iTID!=tIndex.m_pPQ->m_iTID+1 )
		sphWarning ( "binlog: %s: unexpected tid (index=%s, indextid=" INT64_FMT ", logtid=" INT64_FMT ", pos=" INT64_FMT ")",
			sOp, tIndex.m_sName.cstr(), tIndex.m_pPQ->m_iTID, iTID, iTxnPos );
}

void RtBinlog_c::CheckTime ( BinlogIndexInfo_t & tIndex, const char * sOp, int64_t tmStamp, int64_t iTID, int64_t iTxnPos, DWORD uReplayFlags ) const
{
	if ( tmStamp<tIndex.m_tmMax )
	{
		if (!( uReplayFlags & SPH_REPLAY_ACCEPT_DESC_TIMESTAMP ))
			sphDie ( "binlog: %s: descending time (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
				sOp, tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos );

		sphWarning ( "binlog: %s: replaying txn despite descending time "
			"(index=%s, logtid=" INT64_FMT ", lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ")",
			sOp, tIndex.m_sName.cstr(), iTID, tIndex.m_tmMax, tmStamp, iTxnPos );
		tIndex.m_tmMax = tmStamp;
	}
}

void RtBinlog_c::UpdateIndexInfo ( BinlogIndexInfo_t & tIndex, int64_t iTID, int64_t tmStamp ) const
{
	tIndex.m_iMinTID = Min ( tIndex.m_iMinTID, iTID );
	tIndex.m_iMaxTID = Max ( tIndex.m_iMaxTID, iTID );
	tIndex.m_tmMin = Min ( tIndex.m_tmMin, tmStamp );
	tIndex.m_tmMax = Max ( tIndex.m_tmMax, tmStamp );
}

bool RtBinlog_c::PreOp ( Blop_e eOp, int64_t * pTID, const char * sIndexName )
{
	if ( m_bReplayMode || m_bDisabled )
	{
		// still need to advance TID as index flush according to it
		if ( m_bDisabled )
		{
			ScopedMutex_t tLock ( m_tWriteLock );
			(*pTID)++;
		}
		return false;
	}

	Verify ( m_tWriteLock.Lock() );

	int64_t iTID = ++(*pTID);
	const int64_t tmNow = sphMicroTimer();
	const int uIndex = GetWriteIndexID ( sIndexName, iTID, tmNow );

	// header
	m_tWriter.PutDword ( BLOP_MAGIC );
	m_tWriter.ResetCrc ();

	m_tWriter.ZipOffset ( eOp );
	m_tWriter.ZipOffset ( uIndex );
	m_tWriter.ZipOffset ( iTID );
	m_tWriter.ZipOffset ( tmNow );

	return true;
}

void RtBinlog_c::PostOp()
{
	// checksum
	m_tWriter.WriteCrc ();

	// finalize
	CheckDoFlush();
	CheckDoRestart();
	Verify ( m_tWriteLock.Unlock() );
}

void RtBinlog_c::BinlogPqAdd ( int64_t * pTID, const char * sIndexName, const StoredQueryDesc_t & tStored )
{
	MEMORY ( MEM_BINLOG );
	if ( !PreOp ( BLOP_PQ_ADD, pTID, sIndexName ) )
		return;

	// save txn data
	SaveStoredQuery ( tStored, m_tWriter );

	PostOp();
}

bool RtBinlog_c::ReplayPqAdd ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "pq-add" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	StoredQueryDesc_t tStored;
	LoadStoredQuery ( PQ_META_VERSION_MAX, tStored, tReader );

	// checksum
	if ( CheckCrc ("pq-add", tIndex.m_sName, iTID, iTxnPos, tReader ) )
		return false;

	// check TID
	CheckTid ( "pq-add", tIndex, iTID, iTxnPos );

	// check timestamp
	CheckTime ( tIndex, "pq-add", tmStamp, iTID, iTxnPos, uReplayFlags );

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pPQ && iTID>tIndex.m_pPQ->m_iTID )
	{
		// we normally expect per-index TIDs to be sequential
		// but let's be graceful about that
		CheckTidSeq ( "pq-add", tIndex, iTID, iTxnPos );

		CSphString sError;
		PercolateQueryArgs_t tArgs ( tStored );
		// at binlog query already passed replace checks
		tArgs.m_bReplace = true;

		// actually replay
		StoredQuery_i * pQuery = tIndex.m_pPQ->Query ( tArgs, sError );
		if ( !pQuery || !tIndex.m_pPQ->CommitPercolate ( pQuery, sError ) )
			sphDie ( "binlog: pq-add: apply error (index=%s, lasttime=" INT64_FMT ", logtime=" INT64_FMT ", pos=" INT64_FMT ", '%s')",
				tIndex.m_sName.cstr(), tIndex.m_tmMax, tmStamp, iTxnPos, sError.cstr() );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pPQ->m_iTID = iTID;
	}

	// update info
	UpdateIndexInfo ( tIndex, iTID, tmStamp );

	return true;
}

void RtBinlog_c::BinlogPqDelete ( int64_t * pTID, const char * sIndexName, const uint64_t * pQueries, int iCount, const char * sTags )
{
	MEMORY ( MEM_BINLOG );
	if ( !PreOp ( BLOP_PQ_DELETE, pTID, sIndexName ) )
		return;

	// save txn data
	SaveDeleteQuery ( pQueries, iCount, sTags, m_tWriter );

	PostOp();
}

bool RtBinlog_c::ReplayPqDelete ( int iBinlog, DWORD uReplayFlags, BinlogReader_c & tReader ) const
{
	// load and lookup index
	const int64_t iTxnPos = tReader.GetPos();
	BinlogFileDesc_t & tLog = m_dLogFiles[iBinlog];
	BinlogIndexInfo_t & tIndex = ReplayIndexID ( tReader, tLog, "pq-delete" );

	// load transaction data
	const int64_t iTID = (int64_t) tReader.UnzipOffset();
	const int64_t tmStamp = (int64_t) tReader.UnzipOffset();

	CSphVector<uint64_t> dQueries;
	CSphString sTags;
	LoadDeleteQuery ( dQueries, sTags, tReader );

	// checksum
	if ( CheckCrc ("pq-delete", tIndex.m_sName, iTID, iTxnPos, tReader ) )
		return false;

	// check TID
	CheckTid ( "pq-delete", tIndex, iTID, iTxnPos );

	// check timestamp
	CheckTime ( tIndex, "pq-delete", tmStamp, iTID, iTxnPos, uReplayFlags );

	// only replay transaction when index exists and does not have it yet (based on TID)
	if ( tIndex.m_pPQ && iTID>tIndex.m_pPQ->m_iTID )
	{
		CheckTidSeq ( "pq-delete", tIndex, iTID, iTxnPos );

		// actually replay
		if ( dQueries.GetLength() )
			tIndex.m_pPQ->DeleteQueries ( dQueries.Begin(), dQueries.GetLength() );
		else
			tIndex.m_pPQ->DeleteQueries ( sTags.cstr() );

		// update committed tid on replay in case of unexpected / mismatched tid
		tIndex.m_pPQ->m_iTID = iTID;
	}

	// update info
	UpdateIndexInfo ( tIndex, iTID, tmStamp );

	return true;
}


//////////////////////////////////////////////////////////////////////////

RtIndex_i * sphGetCurrentIndexRT()
{
	ISphRtAccum * pAcc = (ISphRtAccum*) sphThreadGet ( g_tTlsAccumKey );
	if ( pAcc )
		return pAcc->GetIndex();
	return NULL;
}

RtIndex_i * sphCreateIndexRT ( const CSphSchema & tSchema, const char * sIndexName,
	int64_t iRamSize, const char * sPath, bool bKeywordDict )
{
	MEMORY ( MEM_INDEX_RT );
	return new RtIndex_c ( tSchema, sIndexName, iRamSize, sPath, bKeywordDict );
}

void sphRTInit ( const CSphConfigSection & hSearchd, bool bTestMode, const CSphConfigSection * pCommon )
{
	MEMORY ( MEM_BINLOG );

	g_bRTChangesAllowed = false;
	Verify ( sphThreadKeyCreate ( &g_tTlsAccumKey ) );

	g_pRtBinlog = new RtBinlog_c();
	if ( !g_pRtBinlog )
		sphDie ( "binlog: failed to create binlog" );
	g_pBinlog = g_pRtBinlog;

	// check binlog path before detaching from the console
	g_pRtBinlog->CheckPath ( hSearchd, bTestMode );

	if ( pCommon )
		g_bProgressiveMerge = ( pCommon->GetInt ( "progressive_merge", 1 )!=0 );
}


void sphRTConfigure ( const CSphConfigSection & hSearchd, bool bTestMode )
{
	assert ( g_pBinlog );
	g_pRtBinlog->Configure ( hSearchd, bTestMode );
	g_iRtFlushPeriod = hSearchd.GetInt ( "rt_flush_period", (int)g_iRtFlushPeriod );
	g_iRtFlushPeriod = Max ( g_iRtFlushPeriod, 10 );
}


void sphRTDone ()
{
	sphThreadKeyDelete ( g_tTlsAccumKey );
	// its valid for "searchd --stop" case
	SafeDelete ( g_pBinlog );
}


void sphReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, ProgressCallbackSimple_t * pfnProgressCallback, BinlogFlushInfo_t & tFlush )
{
	MEMORY ( MEM_BINLOG );
	g_pRtBinlog->Replay ( hIndexes, uReplayFlags, pfnProgressCallback );
	g_pRtBinlog->GetFlushInfo ( tFlush );
	g_bRTChangesAllowed = true;
}

static bool g_bTestMode = false;

void sphRTSetTestMode ()
{
	g_bTestMode = true;
}

bool sphRTSchemaConfigure ( const CSphConfigSection & hIndex, CSphSchema & tSchema, CSphString & sError, bool bSkipValidation )
{
	// fields
	SmallStringHash_T<BYTE> hFields;
	for ( CSphVariant * v=hIndex("rt_field"); v; v=v->m_pNext )
	{
		CSphString sFieldName = v->cstr();
		sFieldName.ToLower();
		tSchema.AddField ( sFieldName.cstr() );
		hFields.Add ( 1, sFieldName );
	}

	if ( !tSchema.GetFieldsCount() && !bSkipValidation )
	{
		sError.SetSprintf ( "no fields configured (use rt_field directive)" );
		return false;
	}

	if ( tSchema.GetFieldsCount()>SPH_MAX_FIELDS )
	{
		sError.SetSprintf ( "too many fields (fields=%d, max=%d)", tSchema.GetFieldsCount(), SPH_MAX_FIELDS );
		return false;
	}

	// add id column
	CSphColumnInfo tCol ( sphGetDocidName() );
	tCol.m_eAttrType = SPH_ATTR_BIGINT;
	tSchema.AddAttr ( tCol, false );

	// attrs
	const int iNumTypes = 9;
	const char * sTypes[iNumTypes] = { "rt_attr_uint", "rt_attr_bigint", "rt_attr_timestamp", "rt_attr_bool", "rt_attr_float", "rt_attr_string", "rt_attr_json", "rt_attr_multi", "rt_attr_multi_64" };
	const ESphAttr iTypes[iNumTypes] = { SPH_ATTR_INTEGER, SPH_ATTR_BIGINT, SPH_ATTR_TIMESTAMP, SPH_ATTR_BOOL, SPH_ATTR_FLOAT, SPH_ATTR_STRING, SPH_ATTR_JSON, SPH_ATTR_UINT32SET, SPH_ATTR_INT64SET };

	for ( int iType=0; iType<iNumTypes; ++iType )
	{
		for ( CSphVariant * v = hIndex ( sTypes[iType] ); v; v = v->m_pNext )
		{
			StrVec_t dNameParts;
			sphSplit ( dNameParts, v->cstr(), ":");
			CSphColumnInfo tCol ( dNameParts[0].cstr(), iTypes[iType]);
			tCol.m_sName.ToLower();

			// bitcount
			tCol.m_tLocator = CSphAttrLocator();
			if ( dNameParts.GetLength ()>1 )
			{
				if ( tCol.m_eAttrType==SPH_ATTR_INTEGER )
				{
					auto iBits = strtol ( dNameParts[1].cstr(), NULL, 10 );
					if ( iBits>0 && iBits<=ROWITEM_BITS )
						tCol.m_tLocator.m_iBitCount = (int)iBits;
					else
						sError.SetSprintf ( "attribute '%s': invalid bitcount=%d (bitcount ignored)", tCol.m_sName.cstr(), (int)iBits );

				} else
					sError.SetSprintf ( "attribute '%s': bitcount is only supported for integer types (bitcount ignored)", tCol.m_sName.cstr() );
			}

			tSchema.AddAttr ( tCol, false );

			if ( tCol.m_eAttrType!=SPH_ATTR_STRING && hFields.Exists ( tCol.m_sName ) && !bSkipValidation )
			{
				sError.SetSprintf ( "can not add attribute that shadows '%s' field", tCol.m_sName.cstr () );
				return false;
			}
		}
	}

	// add blob attr locator
	if ( tSchema.HasBlobAttrs() )
	{
		CSphColumnInfo tCol ( sphGetBlobLocatorName() );
		tCol.m_eAttrType = SPH_ATTR_BIGINT;

		// should be right after docid
		tSchema.InsertAttr ( 1, tCol, false );

		// rebuild locators in the schema
		const char * szTmpColName = "$_tmp";
		CSphColumnInfo tTmpCol ( szTmpColName, SPH_ATTR_BIGINT );
		tSchema.AddAttr ( tTmpCol, false );
		tSchema.RemoveAttr ( szTmpColName, false );
	}

	if ( !tSchema.GetAttrsCount() && !g_bTestMode && !bSkipValidation )
	{
		sError.SetSprintf ( "no attribute configured (use rt_attr directive)" );
		return false;
	}

	return true;
}
