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
#include "sphinxexpr.h"
#include "sphinxplugin.h"

#include "sphinxutils.h"
#include "sphinxint.h"
#include "sphinxjson.h"
#include <time.h>
#include <math.h>

#if USE_RE2
#include <re2/re2.h>
#endif

#ifndef M_LOG2E
#define M_LOG2E		1.44269504088896340736
#endif

#ifndef M_LOG10E
#define M_LOG10E	0.434294481903251827651
#endif

// hack hack hack
UservarIntSet_c * ( *g_pUservarsHook )( const CSphString & sUservar );

//////////////////////////////////////////////////////////////////////////
// EVALUATION ENGINE
//////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS
	#ifndef NDEBUG
		#define EXPR_CLASS_NAME(name) \
			{\
			const char * szFuncName = __FUNCTION__; \
			const char * szClassNameEnd = strstr ( szFuncName, "::" ); \
			assert ( szClassNameEnd ); \
			const char * szTemplateNameEnd = strstr ( szFuncName, "<" ); \
			if ( szTemplateNameEnd ) szClassNameEnd = szTemplateNameEnd; \
			size_t iLen = szClassNameEnd-szFuncName; \
			assert ( strlen(name)==iLen && "Wrong expression name specified in ::GetHash" ); \
			assert ( !strncmp(name, szFuncName, iLen) && "Wrong expression name specified in ::GetHash" ); \
			}\
			const char * szClassName = name; \
			uint64_t uHash = uPrevHash;
	#else
			#define EXPR_CLASS_NAME(name) \
				const char * szClassName = name; \
				uint64_t uHash = uPrevHash;
	#endif
#else
	#define EXPR_CLASS_NAME(name) \
		const char * szClassName = name; \
		uint64_t uHash = uPrevHash;
#endif

#define EXPR_CLASS_NAME_NOCHECK(name) \
	const char * szClassName = name; \
	uint64_t uHash = uPrevHash;

#define CALC_DEP_HASHES() sphCalcExprDepHash ( szClassName, this, tSorterSchema, uHash, bDisable );
#define CALC_DEP_HASHES_EX(hash) sphCalcExprDepHash ( szClassName, this, tSorterSchema, uHash^hash, bDisable );
#define CALC_PARENT_HASH() CalcHash ( szClassName, tSorterSchema, uHash, bDisable );
#define CALC_PARENT_HASH_EX(hash) CalcHash ( szClassName, tSorterSchema, uHash^hash, bDisable );

#define CALC_POD_HASH(value) uHash = sphFNV64 ( &value, sizeof(value), uHash );
#define CALC_POD_HASHES(values) uHash = sphFNV64 ( values.Begin(), values.GetLength()*sizeof(values[0]), uHash );
#define CALC_STR_HASH(str,len) uHash = sphFNV64 ( str.cstr(), len, uHash );
#define CALC_CHILD_HASH(child) if (child) uHash = child->GetHash ( tSorterSchema, uHash, bDisable );
#define CALC_CHILD_HASHES(children) ARRAY_FOREACH ( i, children ) if (children[i]) uHash = children[i]->GetHash ( tSorterSchema, uHash, bDisable );


struct ExprLocatorTraits_t
{
	CSphAttrLocator m_tLocator;
	int m_iLocator; // used by SPH_EXPR_GET_DEPENDENT_COLS

	ExprLocatorTraits_t ( const CSphAttrLocator & tLocator, int iLocator ) : m_tLocator ( tLocator ), m_iLocator ( iLocator ) {}

	virtual void HandleCommand ( ESphExprCommand eCmd, void * pArg )
	{
		if ( eCmd==SPH_EXPR_GET_DEPENDENT_COLS && m_iLocator!=-1 )
			static_cast < CSphVector<int>* >(pArg)->Add ( m_iLocator );
	}

	virtual void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema )
	{
		sphFixupLocator ( m_tLocator, pOldSchema, pNewSchema );
	}
};


const BYTE * ISphExpr::StringEvalPacked ( const CSphMatch & tMatch ) const
{
	const BYTE * pStr = nullptr;
	int iStrLen = StringEval ( tMatch, &pStr );
	auto pRes = sphPackPtrAttr ( pStr, iStrLen );
	if ( IsDataPtrAttr () ) SafeDeleteArray ( pStr );
	return pRes;
}


struct Expr_WithLocator_c : public ISphExpr, public ExprLocatorTraits_t
{
public:
	Expr_WithLocator_c ( const CSphAttrLocator & tLocator, int iLocator )
		: ExprLocatorTraits_t ( tLocator, iLocator )
	{}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		sphFixupLocator ( m_tLocator, pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		HandleCommand ( eCmd, pArg );
	}
};


// has string expression traits, but has no locator
class Expr_StrNoLocator_c : public ISphStringExpr
{
public:
	void FixupLocator ( const ISphSchema * /*pOldSchema*/, const ISphSchema * /*pNewSchema*/ ) override {}
};


struct Expr_GetInt_c : public Expr_WithLocator_c
{
	Expr_GetInt_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & tMatch ) const final { return (float) tMatch.GetAttr ( m_tLocator ); } // FIXME! OPTIMIZE!!! we can go the short route here
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.GetAttr ( m_tLocator ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)tMatch.GetAttr ( m_tLocator ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetInt_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetBits_c : public Expr_WithLocator_c
{
	Expr_GetBits_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & tMatch ) const final { return (float) tMatch.GetAttr ( m_tLocator ); }
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.GetAttr ( m_tLocator ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)tMatch.GetAttr ( m_tLocator ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetBits_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetSint_c : public Expr_WithLocator_c
{
	Expr_GetSint_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & tMatch ) const final { return (float)(int)tMatch.GetAttr ( m_tLocator ); }
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.GetAttr ( m_tLocator ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int)tMatch.GetAttr ( m_tLocator ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetSint_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetFloat_c : public Expr_WithLocator_c
{
	Expr_GetFloat_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & tMatch ) const final { return tMatch.GetAttrFloat ( m_tLocator ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetFloat_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetString_c : public Expr_WithLocator_c
{
	const BYTE * m_pStrings = nullptr;

	Expr_GetString_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		// fixme: we should probably have an explicit flag to differentiate matches using index schema
		// and matches using result set schema
		if ( tMatch.m_pStatic )
		{
			SphAttr_t iOff = tMatch.GetAttr ( m_tLocator );
			if ( iOff>0 )
				return sphUnpackStr ( m_pStrings + iOff, ppStr );
			else
			{
				*ppStr = nullptr;
				return 0;
			}
		} else
		{
			if ( !m_tLocator.m_bDynamic )
			{
				assert ( 0 && "unexpected static locator" );
				*ppStr = nullptr;
				return 0;
			}

			auto * pStr = (const BYTE *)tMatch.GetAttr ( m_tLocator );
			return sphUnpackPtrAttr ( pStr, ppStr );
		}
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetString_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetMva_c : public Expr_WithLocator_c
{
	const DWORD * m_pMva;
	bool m_bArenaProhibit;

	Expr_GetMva_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ), m_pMva ( nullptr ), m_bArenaProhibit ( false ) {}
	float Eval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_MVA_POOL )
		{
			auto pPool = (const PoolPtrs_t *)pArg;
			assert ( pPool );
			m_pMva = pPool->m_pMva;
			m_bArenaProhibit = pPool->m_bArenaProhibit;
		}
	}
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.GetAttr ( m_tLocator ); }
	const DWORD * MvaEval ( const CSphMatch & tMatch ) const final { return tMatch.GetAttrMVA ( m_tLocator, m_pMva, m_bArenaProhibit ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetMva_c");
		CALC_POD_HASH(m_bArenaProhibit);
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetFactorsAttr_c : public Expr_WithLocator_c
{
	Expr_GetFactorsAttr_c ( const CSphAttrLocator & tLocator, int iLocator ) : Expr_WithLocator_c ( tLocator, iLocator ) {}
	float Eval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	const BYTE * FactorEval ( const CSphMatch & tMatch ) const final
	{
		auto * pPacked = (const BYTE *)tMatch.GetAttr ( m_tLocator );
		sphUnpackPtrAttr ( pPacked, &pPacked );
		return pPacked;
	}

	const BYTE * FactorEvalPacked ( const CSphMatch & tMatch ) const final
	{
		return (const BYTE *)tMatch.GetAttr ( m_tLocator );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetFactorsAttr_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetConst_c : public Expr_NoLocator_c
{
	float m_fValue;
	explicit Expr_GetConst_c ( float fValue ) : m_fValue ( fValue ) {}
	float Eval ( const CSphMatch & ) const final { return m_fValue; }
	int IntEval ( const CSphMatch & ) const final { return (int)m_fValue; }
	int64_t Int64Eval ( const CSphMatch & ) const final { return (int64_t)m_fValue; }
	bool IsConst () const final { return true; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetConst_c");
		CALC_POD_HASH(m_fValue);
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetIntConst_c : public Expr_NoLocator_c
{
	int m_iValue;
	explicit Expr_GetIntConst_c ( int iValue ) : m_iValue ( iValue ) {}
	float Eval ( const CSphMatch & ) const final { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	int IntEval ( const CSphMatch & ) const final { return m_iValue; }
	int64_t Int64Eval ( const CSphMatch & ) const final { return m_iValue; }
	bool IsConst () const final { return true; }
	
	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetIntConst_c");
		CALC_POD_HASH(m_iValue);
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetInt64Const_c : public Expr_NoLocator_c
{
	int64_t m_iValue;
	explicit Expr_GetInt64Const_c ( int64_t iValue ) : m_iValue ( iValue ) {}
	float Eval ( const CSphMatch & ) const final { return (float) m_iValue; } // no assert() here cause generic float Eval() needs to work even on int-evaluator tree
	int IntEval ( const CSphMatch & ) const final { assert ( 0 ); return (int)m_iValue; }
	int64_t Int64Eval ( const CSphMatch & ) const final { return m_iValue; }
	bool IsConst () const final { return true; }
	
	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetInt64Const_c");
		CALC_POD_HASH(m_iValue);
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetStrConst_c : public Expr_StrNoLocator_c
{
	CSphString m_sVal;
	int m_iLen;

	explicit Expr_GetStrConst_c ( const char * sVal, int iLen, bool bUnescape )
	{
		if ( iLen>0 )
		{
			if ( bUnescape )
				SqlUnescape ( m_sVal, sVal, iLen );
			else
				m_sVal.SetBinary ( sVal, iLen );
		}
		m_iLen = m_sVal.Length();
	}

	int StringEval ( const CSphMatch &, const BYTE ** ppStr ) const final
	{
		*ppStr = (const BYTE*) m_sVal.cstr();
		return m_iLen;
	}

	float Eval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	int IntEval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	int64_t Int64Eval ( const CSphMatch & ) const final { assert ( 0 ); return 0; }
	bool IsConst () const final { return true; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetStrConst_c");
		CALC_STR_HASH(m_sVal, m_iLen);
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetZonespanlist_c : public Expr_StrNoLocator_c
{
	const CSphVector<int> * m_pData = nullptr;
	mutable StringBuilder_c m_sBuilder;

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		assert ( ppStr );
		if ( !m_pData || !m_pData->GetLength() )
		{
			*ppStr = nullptr;
			return 0;
		}
		m_sBuilder.Clear();
		const CSphVector<int> & dSpans = *m_pData;
		int iStart = tMatch.m_iTag + 1; // spans[tag] contains the length, so the 1st data index is tag+1
		int iEnd = iStart + dSpans [ tMatch.m_iTag ]; // [start,end) now covers all data indexes
		for ( int i=iStart; i<iEnd; i+=2 )
			m_sBuilder.Appendf ( " %d:%d", 1+dSpans[i], 1+dSpans[i+1] ); // convert our 0-based span numbers to human 1-based ones
		auto iRes = m_sBuilder.GetLength ();
		*ppStr = m_sBuilder.Leak();
		return iRes;
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd==SPH_EXPR_SET_EXTRA_DATA )
			static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_DATA_ZONESPANS, (void**)&m_pData );
	}

	bool IsDataPtrAttr() const final
	{
		return true;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true; // disable caching for now, might add code to process if necessary
		return 0;
	}
};


struct Expr_GetRankFactors_c : public Expr_StrNoLocator_c
{
	/// hash type MUST BE IN SYNC with RankerState_Export_fn in sphinxsearch.cpp
	CSphOrderedHash < CSphString, SphDocID_t, IdentityHash_fn, 256 > * m_pFactors = nullptr;

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		assert ( ppStr );
		if ( !m_pFactors )
		{
			*ppStr = nullptr;
			return 0;
		}

		CSphString * sVal = (*m_pFactors) ( tMatch.m_uDocID );
		if ( !sVal )
		{
			*ppStr = nullptr;
			return 0;
		}
		int iLen = sVal->Length();
		*ppStr = (const BYTE*)sVal->Leak();
		m_pFactors->Delete ( tMatch.m_uDocID );
		return iLen;
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd==SPH_EXPR_SET_EXTRA_DATA )
			static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_DATA_RANKFACTORS, (void**)&m_pFactors );
	}

	bool IsDataPtrAttr() const final
	{
		return true;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true; // disable caching for now, might add code to process if necessary
		return 0;
	}
};


struct Expr_GetPackedFactors_c : public Expr_StrNoLocator_c
{
	SphFactorHash_t * m_pHash = nullptr;

	const BYTE * FactorEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pData = nullptr;
		int iDataLen = FetchHashEntry ( tMatch, pData );
		if ( !pData )
			return nullptr;

		auto * pResult = new BYTE[iDataLen];
		memcpy ( pResult, pData, iDataLen );

		return pResult;
	}

	const BYTE * FactorEvalPacked ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pData = nullptr;
		int iDataLen = FetchHashEntry ( tMatch, pData );
		if ( !pData )
			return nullptr;

		return sphPackPtrAttr( pData, iDataLen );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd==SPH_EXPR_SET_EXTRA_DATA )
			static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_DATA_PACKEDFACTORS, (void**)&m_pHash );
	}

	bool IsDataPtrAttr() const final
	{
		return true;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true; // disable caching for now, might add code to process if necessary
		return 0;
	}

private:
	int FetchHashEntry ( const CSphMatch & tMatch, const BYTE * & pData ) const
	{
		pData = nullptr;

		if ( !m_pHash || !m_pHash->GetLength() )
			return 0;

		SphFactorHashEntry_t * pEntry = (*m_pHash)[ (int)( tMatch.m_uDocID % m_pHash->GetLength() ) ];
		assert ( pEntry );

		while ( pEntry && pEntry->m_iId!=tMatch.m_uDocID )
			pEntry = pEntry->m_pNext;

		if ( !pEntry )
			return 0;

		pData = pEntry->m_pData;
		return int((BYTE *)pEntry - pEntry->m_pData);
	}
};


struct Expr_BM25F_c : public Expr_NoLocator_c
{
	SphExtraDataRankerState_t	m_tRankerState;
	float						m_fK1;
	float						m_fB;
	float						m_fWeightedAvgDocLen = 0.0f;
	CSphVector<int>				m_dWeights;		///< per field weights
	SphFactorHash_t *			m_pHash = nullptr;
	CSphVector<CSphNamedVariant>	m_dFieldWeights;

	Expr_BM25F_c ( float k1, float b, CSphVector<CSphNamedVariant> * pFieldWeights )
	{
		// bind k1, b
		m_fK1 = k1;
		m_fB = b;
		if ( pFieldWeights )
			m_dFieldWeights.SwapData ( *pFieldWeights );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		if ( !m_pHash || !m_pHash->GetLength() )
			return 0.0f;

		SphFactorHashEntry_t * pEntry = (*m_pHash)[ (int)( tMatch.m_uDocID % m_pHash->GetLength() ) ];
		assert ( pEntry );

		while ( pEntry && pEntry->m_iId!=tMatch.m_uDocID )
			pEntry = pEntry->m_pNext;

		if ( !pEntry )
			return 0.0f;

		SPH_UDF_FACTORS tUnpacked;
		sphinx_factors_init ( &tUnpacked );
#ifndef NDEBUG
		Verify ( sphinx_factors_unpack ( (const unsigned int*)pEntry->m_pData, &tUnpacked )==0 );
#else
		sphinx_factors_unpack ( (const unsigned int*)pEntry->m_pData, &tUnpacked ); // fix MSVC Release warning
#endif

		// compute document length
		// OPTIMIZE? could precompute and store total dl in attrs, but at a storage cost
		// OPTIMIZE? could at least share between multiple BM25F instances, if there are many
		float dl = 0;
		CSphAttrLocator tLoc = m_tRankerState.m_tFieldLensLoc;
		if ( tLoc.m_iBitOffset>=0 )
		{
			for ( int i=0; i<m_tRankerState.m_iFields; i++ )
			{
				dl += tMatch.GetAttr ( tLoc ) * m_dWeights[i];
				tLoc.m_iBitOffset += 32;
			}
		}

		// compute (the current instance of) BM25F
		float fRes = 0.0f;
		for ( int iWord=0; iWord<m_tRankerState.m_iMaxQpos; iWord++ )
		{
			if ( !tUnpacked.term[iWord].keyword_mask )
				continue;

			// compute weighted TF
			float tf = 0.0f;
			for ( int i=0; i<m_tRankerState.m_iFields; i++ )
			{
				tf += tUnpacked.field_tf[ iWord + 1 + i * ( 1 + m_tRankerState.m_iMaxQpos ) ] * m_dWeights[i];
			}
			float idf = tUnpacked.term[iWord].idf; // FIXME? zeroed out for dupes!
			fRes += tf / ( tf + m_fK1 * ( 1.0f - m_fB + m_fB * dl / m_fWeightedAvgDocLen ) ) * idf;
		}

		sphinx_factors_deinit ( &tUnpacked );

		return fRes + 0.5f; // map to [0..1] range
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd!=SPH_EXPR_SET_EXTRA_DATA )
			return;

		bool bGotHash = static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_DATA_PACKEDFACTORS, (void**)&m_pHash );
		if ( !bGotHash )
			return;

		bool bGotState = static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_DATA_RANKER_STATE, (void**)&m_tRankerState );
		if ( !bGotState )
			return;

		// bind weights
		m_dWeights.Resize ( m_tRankerState.m_iFields );
		m_dWeights.Fill ( 1 );
		if ( m_dFieldWeights.GetLength() )
		{
			ARRAY_FOREACH ( i, m_dFieldWeights )
			{
				// FIXME? report errors if field was not found?
				CSphString & sField = m_dFieldWeights[i].m_sKey;
				int iField = m_tRankerState.m_pSchema->GetFieldIndex ( sField.cstr() );
				if ( iField>=0 )
					m_dWeights[iField] = m_dFieldWeights[i].m_iValue;
			}
		}

		// compute weighted avgdl
		m_fWeightedAvgDocLen = 1.0f;
		if ( m_tRankerState.m_pFieldLens )
		{
			m_fWeightedAvgDocLen = 0.0f;
			ARRAY_FOREACH ( i, m_dWeights )
				m_fWeightedAvgDocLen += m_tRankerState.m_pFieldLens[i] * m_dWeights[i];
		}
		m_fWeightedAvgDocLen /= m_tRankerState.m_iTotalDocuments;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true; // disable caching for now, might add code to process if necessary
		return 0;
	}
};


struct Expr_GetId_c : public Expr_NoLocator_c
{
	float Eval ( const CSphMatch & tMatch ) const final { return (float)tMatch.m_uDocID; }
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.m_uDocID; }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)tMatch.m_uDocID; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetId_c");
		return CALC_DEP_HASHES();
	}
};


struct Expr_GetWeight_c : public Expr_NoLocator_c
{
	float Eval ( const CSphMatch & tMatch ) const final { return (float)tMatch.m_iWeight; }
	int IntEval ( const CSphMatch & tMatch ) const final { return (int)tMatch.m_iWeight; }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)tMatch.m_iWeight; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GetWeight_c");
		return CALC_DEP_HASHES();
	}
};

//////////////////////////////////////////////////////////////////////////

struct Expr_Arglist_c : public ISphExpr
{
	VecRefPtrs_t<ISphExpr*> m_dArgs;

	Expr_Arglist_c ( ISphExpr * pLeft, ISphExpr * pRight )
	{
		AddArgs ( pLeft );
		AddArgs ( pRight );
	}

	~Expr_Arglist_c () final
	{
		for ( auto& i : m_dArgs )
			SafeRelease ( i );
	}

	bool IsArglist () const final
	{
		return true;
	}

	ISphExpr * GetArg ( int i ) const final
	{
		if ( i>=m_dArgs.GetLength() )
			return nullptr;
		return m_dArgs[i];
	}

	int GetNumArgs() const final
	{
		return m_dArgs.GetLength();
	}

	float Eval ( const CSphMatch & ) const final
	{
		assert ( 0 && "internal error: Eval() must not be explicitly called on arglist" );
		return 0.0f;
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		for ( auto i : m_dArgs )
			i->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		for ( auto i : m_dArgs )
			i->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) final
	{
		assert ( 0 && "internal error: GetHash() must not be explicitly called on arglist" );
		return 0;
	}

private:
	void AddArgs ( ISphExpr * pExpr )
	{
		// not an arglist? just add it
		if ( !pExpr->IsArglist () )
		{
			m_dArgs.Add ( pExpr );
			SafeAddRef ( pExpr );
			return;
		}

		// arglist? take ownership of its args, and dismiss it
		auto * pArgs = ( Expr_Arglist_c * ) pExpr;
		m_dArgs.Append ( pArgs->m_dArgs );
		pArgs->m_dArgs.Reset ();
	}
};

//////////////////////////////////////////////////////////////////////////

struct Expr_Unary_c : public ISphExpr
{
	CSphRefcountedPtr<ISphExpr>		m_pFirst;
	const char *	m_szExprName;

	explicit Expr_Unary_c ( const char * szClassName, ISphExpr * pFirst )
		: m_pFirst ( pFirst )
		, m_szExprName ( szClassName )
	{
		SafeAddRef ( pFirst );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		if ( m_pFirst )
			m_pFirst->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( m_pFirst )
			m_pFirst->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) override
	{
		EXPR_CLASS_NAME_NOCHECK(m_szExprName);
		CALC_CHILD_HASH(m_pFirst);
		return CALC_DEP_HASHES();
	}
};


struct Expr_Binary_c : public ISphExpr
{
	CSphRefcountedPtr<ISphExpr>		m_pFirst;
	CSphRefcountedPtr<ISphExpr>		m_pSecond;
	const char *	m_szExprName;

	explicit Expr_Binary_c ( const char * szClassName, ISphExpr * pFirst, ISphExpr * pSecond )
		: m_pFirst ( pFirst )
		, m_pSecond ( pSecond )
		, m_szExprName ( szClassName )
	{
		SafeAddRef ( pFirst );
		SafeAddRef ( pSecond );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		m_pFirst->FixupLocator ( pOldSchema, pNewSchema );
		m_pSecond->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		m_pFirst->Command ( eCmd, pArg );
		m_pSecond->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) override
	{
		EXPR_CLASS_NAME_NOCHECK(m_szExprName);
		CALC_CHILD_HASH(m_pFirst);
		CALC_CHILD_HASH(m_pSecond);
		return CALC_DEP_HASHES();
	}
};

//////////////////////////////////////////////////////////////////////////

class Expr_StrLength_c : public Expr_Unary_c
{
public:
	explicit Expr_StrLength_c ( ISphExpr * pArg )
		: Expr_Unary_c ( "Expr_StrLength_c", pArg )
	{}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pStr = nullptr;
		int iLen = m_pFirst->StringEval ( tMatch, &pStr );
		if ( m_pFirst->IsDataPtrAttr () ) SafeDeleteArray ( pStr );
		return iLen;
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
};


struct Expr_Crc32_c : public Expr_Unary_c
{
	explicit Expr_Crc32_c ( ISphExpr * pFirst ) : Expr_Unary_c ( "Expr_Crc32_c", pFirst ) {}
	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pStr;
		int iLen = m_pFirst->StringEval ( tMatch, &pStr );
		DWORD uCrc = sphCRC32 ( pStr, iLen );
		if ( m_pFirst->IsDataPtrAttr() )
			SafeDeleteArray ( pStr );
		return uCrc;
	}
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)(DWORD)IntEval ( tMatch ); }
};


static inline int Fibonacci ( int i )
{
	if ( i<0 )
		return 0;
	int f0 = 0;
	int f1 = 1;
	int j = 0;
	for ( j=0; j+1<i; j+=2 )
	{
		f0 += f1; // f_j
		f1 += f0; // f_{j+1}
	}
	return ( i & 1 ) ? f1 : f0;
}


struct Expr_Fibonacci_c : public Expr_Unary_c
{
	explicit Expr_Fibonacci_c ( ISphExpr * pFirst ) : Expr_Unary_c ( "Expr_Fibonacci_c", pFirst ) {}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int IntEval ( const CSphMatch & tMatch ) const final { return Fibonacci ( m_pFirst->IntEval ( tMatch ) ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return IntEval ( tMatch ); }
};


struct Expr_ToString_c : public Expr_Unary_c
{
protected:
	ESphAttr	m_eArg;
	mutable StringBuilder_c m_sBuilder;
	const BYTE * m_pStrings = nullptr;

public:
	Expr_ToString_c ( ISphExpr * pArg, ESphAttr eArg )
		: Expr_Unary_c ( "Expr_ToString_c", pArg )
		, m_eArg ( eArg )
	{}

	float Eval ( const CSphMatch & ) const final
	{
		assert ( 0 );
		return 0.0f;
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		m_sBuilder.Clear();
		int64_t iPacked = 0;
		ESphJsonType eJson = JSON_NULL;
		DWORD uOff = 0;
		int iLen = 0;

		switch ( m_eArg )
		{
			case SPH_ATTR_INTEGER:	m_sBuilder.Appendf ( "%u", m_pFirst->IntEval ( tMatch ) ); break;
			case SPH_ATTR_BIGINT:	m_sBuilder.Appendf ( INT64_FMT, m_pFirst->Int64Eval ( tMatch ) ); break;
			case SPH_ATTR_FLOAT:	m_sBuilder.Appendf ( "%f", m_pFirst->Eval ( tMatch ) ); break;
			case SPH_ATTR_UINT32SET:
			case SPH_ATTR_INT64SET:
				{
					const DWORD * pValues = m_pFirst->MvaEval ( tMatch );
					if ( !pValues || !*pValues )
						break;

					DWORD nValues = *pValues++;
					assert (!( m_eArg==SPH_ATTR_INT64SET && ( nValues & 1 ) ));

					// OPTIMIZE? minibuffer on stack, less allocs, manual formatting vs printf, etc
					if ( m_eArg==SPH_ATTR_UINT32SET )
					{
						while ( nValues-- )
						{
							if ( m_sBuilder.GetLength() )
								m_sBuilder += ",";
							m_sBuilder.Appendf ( "%u", *pValues++ );
						}
					} else
					{
						for ( ; nValues; nValues-=2, pValues+=2 )
						{
							if ( m_sBuilder.GetLength() )
								m_sBuilder += ",";
							m_sBuilder.Appendf ( INT64_FMT, MVA_UPSIZE ( pValues ) );
						}
					}
				}
				break;
			case SPH_ATTR_STRINGPTR:
				return m_pFirst->StringEval ( tMatch, ppStr );

			case SPH_ATTR_JSON_FIELD:
				iPacked = m_pFirst->Int64Eval ( tMatch );
				eJson = ESphJsonType ( iPacked>>32 );
				uOff = (DWORD)iPacked;
				if ( !uOff || eJson==JSON_NULL )
				{
					*ppStr = nullptr;
					iLen = 0;
				} else
				{
					JsonEscapedBuilder dTmp;
					sphJsonFieldFormat ( dTmp, m_pStrings+uOff, eJson, false );
					iLen = dTmp.GetLength();
					*ppStr = dTmp.Leak();
				}
				return iLen;

			default:
				assert ( 0 && "unhandled arg type in TO_STRING()" );
				break;
		}

		if ( !m_sBuilder.GetLength() )
		{
			*ppStr = nullptr;
			return 0;
		}

		auto iRes = m_sBuilder.GetLength ();
		*ppStr = m_sBuilder.Leak();
		return iRes;
	}

	bool IsDataPtrAttr() const final
	{
		return true;
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;

		m_pFirst->Command ( eCmd, pArg );
	}
};

//////////////////////////////////////////////////////////////////////////

/// generic JSON value evaluation
/// can handle arbitrary stacks of jsoncol.key1.arr2[indexexpr3].key4[keynameexpr5]
/// m_dArgs holds the expressions that return actual accessors (either keynames or indexes)
/// m_dRetTypes holds their respective types
struct Expr_JsonField_c : public Expr_WithLocator_c
{
protected:
	const BYTE *			m_pStrings = nullptr;
	VecRefPtrs_t<ISphExpr *>	m_dArgs;
	CSphVector<ESphAttr>	m_dRetTypes;

public:
	/// takes over the expressions
	Expr_JsonField_c ( const CSphAttrLocator & tLocator, int iLocator, CSphVector<ISphExpr*> & dArgs, CSphVector<ESphAttr> & dRetTypes )
		: Expr_WithLocator_c ( tLocator, iLocator )
	{
		assert ( dArgs.GetLength()==dRetTypes.GetLength() );
		m_dArgs.SwapData ( dArgs );
		m_dRetTypes.SwapData ( dRetTypes );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
		else if ( eCmd==SPH_EXPR_GET_DEPENDENT_COLS && m_iLocator!=-1 )
			static_cast < CSphVector<int>* > ( pArg )->Add ( m_iLocator );
		for ( auto& pExpr : m_dArgs )
			if ( pExpr )
				pExpr->Command ( eCmd, pArg );
	}

	float Eval ( const CSphMatch & ) const final
	{
		assert ( 0 && "one just does not simply evaluate a JSON as float" );
		return 0;
	}

	int64_t DoEval ( ESphJsonType eJson, const BYTE * pVal, const CSphMatch & tMatch ) const
	{
		int iLen;
		const BYTE * pStr;

		ARRAY_FOREACH ( i, m_dRetTypes )
		{
			switch ( m_dRetTypes[i] )
			{
			case SPH_ATTR_INTEGER:	eJson = sphJsonFindByIndex ( eJson, &pVal, m_dArgs[i]->IntEval ( tMatch ) ); break;
			case SPH_ATTR_BIGINT:	eJson = sphJsonFindByIndex ( eJson, &pVal, (int)m_dArgs[i]->Int64Eval ( tMatch ) ); break;
			case SPH_ATTR_FLOAT:	eJson = sphJsonFindByIndex ( eJson, &pVal, (int)m_dArgs[i]->Eval ( tMatch ) ); break;
			case SPH_ATTR_STRING:
				// is this assert will fail someday it's ok
				// just remove it and add this code instead to handle possible memory leak
				// if ( m_dArgv[i]->IsDataPtrAttr() ) SafeDeleteArray ( pStr );
				assert ( !m_dArgs[i]->IsDataPtrAttr() );
				iLen = m_dArgs[i]->StringEval ( tMatch, &pStr );
				eJson = sphJsonFindByKey ( eJson, &pVal, (const void *)pStr, iLen, sphJsonKeyMask ( (const char *)pStr, iLen ) );
				break;
			case SPH_ATTR_JSON_FIELD: // handle cases like "json.a [ json.b ]"
				{
					uint64_t uValue = m_dArgs[i]->Int64Eval ( tMatch );
					const BYTE * p = m_pStrings + ( uValue & 0xffffffff );
					ESphJsonType eType = (ESphJsonType)( uValue >> 32 );

					switch ( eType )
					{
					case JSON_INT32:	eJson = sphJsonFindByIndex ( eJson, &pVal, sphJsonLoadInt ( &p ) ); break;
					case JSON_INT64:	eJson = sphJsonFindByIndex ( eJson, &pVal, (int)sphJsonLoadBigint ( &p ) ); break;
					case JSON_DOUBLE:	eJson = sphJsonFindByIndex ( eJson, &pVal, (int)sphQW2D ( sphJsonLoadBigint ( &p ) ) ); break;
					case JSON_STRING:
						iLen = sphJsonUnpackInt ( &p );
						eJson = sphJsonFindByKey ( eJson, &pVal, (const void *)p, iLen, sphJsonKeyMask ( (const char *)p, iLen ) );
						break;
					default:
						return 0;
					}
					break;
				}
			default:
				return 0;
			}

			if ( eJson==JSON_EOF )
				return 0;
		}

		// keep actual attribute type and offset to data packed
		int64_t iPacked = ( ( (int64_t)( pVal-m_pStrings ) ) | ( ( (int64_t)eJson )<<32 ) );
		return iPacked;
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const override
	{
		if ( !m_pStrings )
			return 0;

		uint64_t uOffset = tMatch.GetAttr ( m_tLocator );
		if ( !uOffset )
			return 0;

		if ( m_tLocator.m_bDynamic )
		{
			// extends precalculated (aliased) field
			const BYTE * pVal = m_pStrings + ( uOffset & 0xffffffff );
			auto eJson = (ESphJsonType)( uOffset >> 32 );
			return DoEval ( eJson, pVal, tMatch );
		}

		const BYTE * pVal = nullptr;
		sphUnpackStr ( m_pStrings + uOffset, &pVal );
		if ( !pVal )
			return 0;

		ESphJsonType eJson = sphJsonFindFirst ( &pVal );
		return DoEval ( eJson, pVal, tMatch );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonField_c");
		CALC_POD_HASHES(m_dRetTypes);
		CALC_CHILD_HASHES(m_dArgs);
		return CALC_DEP_HASHES();
	}

	virtual bool IsJson ( bool & bConverted ) const override
	{
		bConverted = false;
		return true;
	}
};


/// fastpath (instead of generic JsonField_c) for jsoncol.key access by a static key name
struct Expr_JsonFastKey_c : public Expr_WithLocator_c
{
protected:
	const BYTE *	m_pStrings = nullptr;
	CSphString		m_sKey;
	int				m_iKeyLen;
	DWORD			m_uKeyBloom;

public:
	/// takes over the expressions
	Expr_JsonFastKey_c ( const CSphAttrLocator & tLocator, int iLocator, ISphExpr * pArg )
		: Expr_WithLocator_c ( tLocator, iLocator )
	{
		assert ( ( tLocator.m_iBitOffset % ROWITEM_BITS )==0 );
		assert ( tLocator.m_iBitCount==ROWITEM_BITS );

		auto * pKey = (Expr_GetStrConst_c*)pArg;
		m_sKey = pKey->m_sVal;
		m_iKeyLen = pKey->m_iLen;
		m_uKeyBloom = sphJsonKeyMask ( m_sKey.cstr(), m_iKeyLen );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
	}

	float Eval ( const CSphMatch & ) const final
	{
		assert ( 0 && "one just does not simply evaluate a JSON as float" );
		return 0;
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		// get pointer to JSON blob data
		assert ( m_pStrings );
		DWORD uOffset = m_tLocator.m_bDynamic
			? tMatch.m_pDynamic [ m_tLocator.m_iBitOffset >> ROWITEM_SHIFT ]
			: tMatch.m_pStatic [ m_tLocator.m_iBitOffset >> ROWITEM_SHIFT ];
		if ( !uOffset )
			return 0;
		const BYTE * pJson;
		sphUnpackStr ( m_pStrings + uOffset, &pJson );

		// all root objects start with a Bloom mask; quickly check it
		if ( ( sphGetDword(pJson) & m_uKeyBloom )!=m_uKeyBloom )
			return 0;

		// OPTIMIZE? FindByKey does an extra (redundant) bloom check inside
		ESphJsonType eJson = sphJsonFindByKey ( JSON_ROOT, &pJson, m_sKey.cstr(), m_iKeyLen, m_uKeyBloom );
		if ( eJson==JSON_EOF )
			return 0;

		// keep actual attribute type and offset to data packed
		int64_t iPacked = ( ( (int64_t)( pJson-m_pStrings ) ) | ( ( (int64_t)eJson )<<32 ) );
		return iPacked;
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonFastKey_c");
		CALC_STR_HASH(m_sKey,m_iKeyLen);
		return CALC_DEP_HASHES();
	}


	virtual bool IsJson ( bool & bConverted ) const override
	{
		bConverted = false;
		return true;
	}
};


struct Expr_JsonFieldConv_c : public ISphExpr
{
public:
	explicit Expr_JsonFieldConv_c ( ISphExpr * pArg )
		: m_pArg { pArg }
	{
		SafeAddRef ( pArg );
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const override
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		if ( eJson!=JSON_STRING)
			return 0;

		//      using sphUnpackStr() is wrong, because BSON uses different store format of string length
		int iLen = sphJsonUnpackInt ( &pVal );
		*ppStr = pVal;
		return iLen;
	}

	float Eval ( const CSphMatch & tMatch ) const override { return DoEval<float> ( tMatch ); }
	int IntEval ( const CSphMatch & tMatch ) const override { return DoEval<int> ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const override { return DoEval<int64_t> ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) override
	{
		EXPR_CLASS_NAME("Expr_JsonFieldConv_c");
		return CALC_PARENT_HASH();
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
		if ( m_pArg )
			m_pArg->Command ( eCmd, pArg );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		if ( m_pArg )
			m_pArg->FixupLocator ( pOldSchema, pNewSchema );
	}

protected:
	const BYTE *	m_pStrings = nullptr;
	CSphRefcountedPtr<ISphExpr>		m_pArg;

	ESphJsonType GetKey ( const BYTE ** ppKey, const CSphMatch & tMatch ) const
	{
		assert ( ppKey );
		if ( !m_pStrings || !m_pArg )
			return JSON_EOF;
		uint64_t uValue = m_pArg->Int64Eval ( tMatch );
		*ppKey = m_pStrings + ( uValue & 0xffffffff );
		return (ESphJsonType)( uValue >> 32 );
	}

	// generic evaluate
	template < typename T >
	T DoEval ( const CSphMatch & tMatch ) const
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		switch ( eJson )
		{
		case JSON_INT32:	return (T)sphJsonLoadInt ( &pVal );
		case JSON_INT64:	return (T)sphJsonLoadBigint ( &pVal );
		case JSON_DOUBLE:	return (T)sphQW2D ( sphJsonLoadBigint ( &pVal ) );
		case JSON_TRUE:		return 1;
		case JSON_STRING:
		{
			if ( !g_bJsonAutoconvNumbers )
				return 0;
			int iLen = sphJsonUnpackInt ( &pVal );
			int64_t iVal;
			double fVal;
			ESphJsonType eType;
			if ( sphJsonStringToNumber ( (const char*)pVal, iLen, eType, iVal, fVal ) )
				return eType==JSON_DOUBLE ? (T)fVal : (T)iVal;
		}
		default:			return 0;
		}
	}

	uint64_t CalcHash ( const char * szTag, const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable )
	{
		EXPR_CLASS_NAME_NOCHECK(szTag);
		CALC_CHILD_HASH(m_pArg);
		return CALC_DEP_HASHES();
	}

	
	virtual bool IsJson ( bool & bConverted ) const override
	{
		bConverted = true;
		return true;
	}
};


template <typename T>
T JsonAggr ( ESphJsonType eJson, const BYTE * pVal, ESphAggrFunc eFunc, CSphString * pBuf )
{
	if ( !pVal || ( eFunc!=SPH_AGGR_MIN && eFunc!=SPH_AGGR_MAX ) )
		return 0;

	switch ( eJson )
	{
	case JSON_INT32_VECTOR:
		{
			int iVals = sphJsonUnpackInt ( &pVal );
			if ( iVals==0 )
				return 0;

			auto * p = (const int*) pVal;
			int iRes = *p; // first value

			switch ( eFunc )
			{
			case SPH_AGGR_MIN: while ( --iVals ) if ( *++p<iRes ) iRes = *p; break;
			case SPH_AGGR_MAX: while ( --iVals ) if ( *++p>iRes ) iRes = *p; break;
			default: return 0;
			}
			return (T)iRes;
		}
	case JSON_DOUBLE_VECTOR:
		{
			int iLen = sphJsonUnpackInt ( &pVal );
			if ( !iLen || ( eFunc!=SPH_AGGR_MIN && eFunc!=SPH_AGGR_MAX ) )
				return 0;

			double fRes = ( eFunc==SPH_AGGR_MIN ? FLT_MAX : FLT_MIN );
			const BYTE * p = pVal;
			for ( int i=0; i<iLen; i++ )
			{
				double fStored = sphQW2D ( sphJsonLoadBigint ( &p ) );
				switch ( eFunc )
				{
				case SPH_AGGR_MIN:
					fRes = Min ( fRes, fStored );
					break;
				case SPH_AGGR_MAX:
					fRes = Max ( fRes, fStored );
					break;
				default: return 0;
				}
			}
			return (T)fRes;
		}
	case JSON_STRING_VECTOR:
		{
			if ( !pBuf )
				return 0;

			sphJsonUnpackInt ( &pVal ); // skip node length

			int iVals = sphJsonUnpackInt ( &pVal );
			if ( iVals==0 )
				return 0;

			// first value
			int iLen = sphJsonUnpackInt ( &pVal );
			auto * pRes = (const char* )pVal;
			int iResLen = iLen;

			while ( --iVals )
			{
				pVal += iLen;
				iLen = sphJsonUnpackInt ( &pVal );

				// binary string comparison
				int iCmp = memcmp ( pRes, (const char*)pVal, iLen<iResLen ? iLen : iResLen );
				if ( iCmp==0 && iLen!=iResLen )
					iCmp = iResLen-iLen;

				if ( ( eFunc==SPH_AGGR_MIN && iCmp>0 ) || ( eFunc==SPH_AGGR_MAX && iCmp<0 ) )
				{
					pRes = (const char*)pVal;
					iResLen = iLen;
				}
			}

			pBuf->SetBinary ( pRes, iResLen );
			return (T)iResLen;
		}
	case JSON_MIXED_VECTOR:
		{
			sphJsonUnpackInt ( &pVal ); // skip node length
			int iLen = sphJsonUnpackInt ( &pVal );
			if ( !iLen || ( eFunc!=SPH_AGGR_MIN && eFunc!=SPH_AGGR_MAX ) )
				return 0;

			double fRes = ( eFunc==SPH_AGGR_MIN ? FLT_MAX : FLT_MIN );
			for ( int i=0; i<iLen; i++ )
			{
				double fVal = ( eFunc==SPH_AGGR_MIN ? FLT_MAX : FLT_MIN );

				auto eType = (ESphJsonType)*pVal++;
				switch (eType)
				{
					case JSON_INT32:
					case JSON_INT64:
						fVal = (double)( eType==JSON_INT32 ? sphJsonLoadInt ( &pVal ) : sphJsonLoadBigint ( &pVal ) );
					break;
					case JSON_DOUBLE:
						fVal = sphQW2D ( sphJsonLoadBigint ( &pVal ) );
						break;
					default:
						sphJsonSkipNode ( eType, &pVal );
						break; // for weird subobjects, just let min
				}

				switch ( eFunc )
				{
				case SPH_AGGR_MIN:
					fRes = Min ( fRes, fVal );
					break;
				case SPH_AGGR_MAX:
					fRes = Max ( fRes, fVal );
					break;
				default: return 0;
				}
			}
			return (T)fRes;
		}
	default: return 0;
	}
}


struct Expr_JsonFieldAggr_c : public Expr_JsonFieldConv_c
{
protected:
	ESphAggrFunc m_eFunc;

public:
	Expr_JsonFieldAggr_c ( ISphExpr * pArg, ESphAggrFunc eFunc )
		: Expr_JsonFieldConv_c ( pArg )
		, m_eFunc ( eFunc )
	{}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		return JsonAggr<int> ( eJson, pVal, m_eFunc, nullptr );
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		CSphString sBuf;
		*ppStr = nullptr;
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );

		int iLen = 0;
		int iVal = 0;
		float fVal = 0.0f;
		
		switch ( eJson )
		{
		case JSON_INT32_VECTOR:
			iVal = JsonAggr<int> ( eJson, pVal, m_eFunc, nullptr );
			sBuf.SetSprintf ( "%u", iVal );
			iLen = sBuf.Length();
			*ppStr = (const BYTE *) sBuf.Leak();
			return iLen;

		case JSON_STRING_VECTOR:
			JsonAggr<int> ( eJson, pVal, m_eFunc, &sBuf );
			iLen = sBuf.Length();
			*ppStr = (const BYTE *) sBuf.Leak();
			return iLen;

		case JSON_DOUBLE_VECTOR:
			fVal = JsonAggr<float> ( eJson, pVal, m_eFunc, nullptr );
			sBuf.SetSprintf ( "%f", fVal );
			iLen = sBuf.Length();
			*ppStr = (const BYTE *) sBuf.Leak();
			return iLen;

		case JSON_MIXED_VECTOR:
			fVal = JsonAggr<float> ( eJson, pVal, m_eFunc, nullptr );
			sBuf.SetSprintf ( "%f", fVal );
			iLen = sBuf.Length();
			*ppStr = (const BYTE *) sBuf.Leak();
			return iLen;

		default: return 0;
		}
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		return JsonAggr<float> ( eJson, pVal, m_eFunc, nullptr );
	}
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		return JsonAggr<int64_t> ( eJson, pVal, m_eFunc, nullptr );
	}

	bool IsDataPtrAttr() const final { return true; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonFieldAggr_c");
		CALC_POD_HASH(m_eFunc);
		return CALC_PARENT_HASH();
	}
};


struct Expr_JsonFieldLength_c : public Expr_JsonFieldConv_c
{
public:
	explicit Expr_JsonFieldLength_c ( ISphExpr * pArg )
		: Expr_JsonFieldConv_c ( pArg )
	{}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		return sphJsonFieldLength ( eJson, pVal );
	}

	float	Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonFieldLength_c");
		return CALC_PARENT_HASH();
	}
};


struct Expr_Now_c : public Expr_NoLocator_c
{
	explicit Expr_Now_c ( int iNow )
		: m_iNow ( iNow )
	{}

	int		IntEval ( const CSphMatch & ) const final { return m_iNow; }
	float	Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Now_c");
		CALC_POD_HASH(m_iNow);
		return CALC_DEP_HASHES();
	}

private:
	int m_iNow;
};


struct Expr_Time_c : public ISphExpr
{
	bool m_bUTC;
	bool m_bDate;

	explicit Expr_Time_c ( bool bUTC, bool bDate )
		: m_bUTC ( bUTC )
		, m_bDate ( bDate )
	{}

	int IntEval ( const CSphMatch & ) const final
	{
		struct tm s; // can't get non-UTC timestamp without mktime
		time_t t = time ( nullptr );
		if ( m_bUTC )
			gmtime_r ( &t, &s );
		else
			localtime_r ( &t, &s );
		return (int) mktime ( &s );
	}

	int StringEval ( const CSphMatch &, const BYTE ** ppStr ) const final
	{
		CSphString sVal;
		struct tm s;
		time_t t = time ( nullptr );
		if ( m_bUTC )
			gmtime_r ( &t, &s );
		else
			localtime_r ( &t, &s );
		if ( m_bDate )
			sVal.SetSprintf ( "%04d-%02d-%02d %02d:%02d:%02d", s.tm_year+1900, s.tm_mon+1, s.tm_mday, s.tm_hour, s.tm_min, s.tm_sec );
		else
			sVal.SetSprintf ( "%02d:%02d:%02d", s.tm_hour, s.tm_min, s.tm_sec );

		int iLength = sVal.Length();
		*ppStr = (const BYTE*) sVal.Leak();
		return iLength;
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }
	bool IsDataPtrAttr () const final { return true; }
	void FixupLocator ( const ISphSchema * /*pOldSchema*/, const ISphSchema * /*pNewSchema*/ ) final {}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true;
		return 0;
	}
};


struct Expr_TimeDiff_c : public Expr_Binary_c
{
	Expr_TimeDiff_c ( ISphExpr * pFirst, ISphExpr * pSecond )
		: Expr_Binary_c ( "Expr_TimeDiff_c", pFirst, pSecond )
	{}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		assert ( m_pFirst && m_pSecond );
		return m_pFirst->IntEval ( tMatch )-m_pSecond->IntEval ( tMatch );
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		int iVal = IntEval ( tMatch );
		CSphString sVal;
		int t = iVal<0 ? -iVal : iVal;
		sVal.SetSprintf ( "%s%02d:%02d:%02d", iVal<0 ? "-" : "", t/60/60, (t/60)%60, t%60 );
		int iLength = sVal.Length();
		*ppStr = (const BYTE*) sVal.Leak();
		return iLength;
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }
	bool IsDataPtrAttr () const final { return true; }
};


class Expr_SubstringIndex_c : public ISphStringExpr
{
private:
	CSphRefcountedPtr<ISphExpr> m_pArg;
	CSphString m_sDelim;
	int m_iCount = 0;
	int m_iLenDelim = 0;
	bool m_bFreeResPtr = false;

public:
	explicit Expr_SubstringIndex_c ( ISphExpr * pArg, ISphExpr * pDelim, ISphExpr * pCount )
		: m_pArg ( pArg )
		, m_iCount ( 0 )
		, m_bFreeResPtr ( false )
	{
		assert ( pArg && pDelim && pCount );
		SafeAddRef ( pArg );
		m_bFreeResPtr = m_pArg->IsDataPtrAttr();

		const BYTE * pBuf = nullptr;
		CSphMatch tTmp;
		
		m_iLenDelim = pDelim->StringEval ( tTmp, &pBuf );
		m_sDelim.SetBinary ( (const char *)pBuf, m_iLenDelim );
		if ( pDelim->IsDataPtrAttr() )
			SafeDeleteArray ( pBuf );

		m_iCount = pCount->IntEval ( tTmp );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		if ( m_pArg )
			m_pArg->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( m_pArg )
			m_pArg->Command ( eCmd, pArg );
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		const char* pDoc = nullptr;
		int iDocLen = m_pArg->StringEval ( tMatch, (const BYTE **)&pDoc );
		int iLength = 0;
		*ppStr = NULL;

		if ( pDoc && iDocLen>0 && m_iLenDelim>0 && m_iCount!=0 )
		{
			if ( m_iCount>0 )
				LeftSearch ( pDoc, iDocLen, m_iCount, false, ppStr, &iLength );
			else
				RightSearch ( pDoc, iDocLen, m_iCount, ppStr, &iLength );
		}

		if ( m_pArg->IsDataPtrAttr() )
			SafeDeleteArray ( pDoc );

		return iLength;
	}

	bool IsDataPtrAttr() const final
	{
		return m_bFreeResPtr;
	}

	//	base class does not convert string to float
	float Eval ( const CSphMatch & tMatch ) const final
	{
		float fVal = 0.f;
		const char * pBuf = nullptr;
		int  iLen = StringEval ( tMatch, (const BYTE **) &pBuf );

		const char * pMax = sphFindLastNumeric ( pBuf, iLen );
		if ( pBuf<pMax )
		{
			fVal = (float) strtod ( pBuf, NULL );
		}
		else
		{
			CSphString sBuf;
			sBuf.SetBinary ( pBuf, iLen );
			fVal = (float) strtod ( sBuf.cstr(), NULL );
		}

		if ( IsDataPtrAttr() )
			SafeDeleteArray ( pBuf );
		return fVal;
	}

	//	base class does not convert string to int
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		int iVal = 0;
		const char * pBuf = nullptr;
		int  iLen = StringEval ( tMatch, (const BYTE **) &pBuf );

		const char * pMax = sphFindLastNumeric ( pBuf, iLen );
		if ( pBuf<pMax )
		{
			iVal = strtol ( pBuf, NULL, 10 );
		}
		else
		{
			CSphString sBuf;
			sBuf.SetBinary ( pBuf, iLen );
			iVal = strtol ( sBuf.cstr(), NULL, 10 );
		}

		if ( IsDataPtrAttr() )
			SafeDeleteArray ( pBuf );
		return iVal;
	}

	//	base class does not convert string to int64
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		int64_t iVal = 0;
		const char * pBuf = nullptr;
		int  iLen = StringEval ( tMatch, (const BYTE **) &pBuf );

		const char * pMax = sphFindLastNumeric ( pBuf, iLen );
		if ( pBuf<pMax )
		{
			iVal = strtoll ( pBuf, NULL, 10 );
		}
		else
		{
			CSphString sBuf;
			sBuf.SetBinary ( pBuf, iLen );
			iVal = strtoll ( sBuf.cstr(), NULL, 10 );
		}

		if ( IsDataPtrAttr() )
			SafeDeleteArray ( pBuf );
		return iVal;
	}

	bool IsConst () const final { return true; }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_SubstringIndex_c");
		CALC_CHILD_HASH(m_pArg);
		CALC_POD_HASH(m_sDelim);
		CALC_POD_HASH(m_iCount);
		return CALC_DEP_HASHES();
	}

private:
	int SetResultString ( const char * pDoc, int iDocLen, const BYTE ** ppResStr ) const;
	int LeftSearch ( const char * pDoc, int iDocLen, int iCount, bool bGetRight, const BYTE ** ppResStr, int * pResLen ) const;
	int RightSearch ( const char * pDoc, int iDocLen, int iCount, const BYTE ** ppResStr, int * pResLen ) const;
};

//	in case of input static string, function returns only pointer and length of substring. buffer is not allocated
//	in case of input dynamic string, function allocates buffer for substring and copy substring to it
int Expr_SubstringIndex_c::SetResultString ( const char * pDoc, int iDocLen, const BYTE ** ppResStr ) const
{
	if ( IsDataPtrAttr()==false )
	{
		*ppResStr = (const BYTE *) pDoc;
	}
	else
	{
		CSphString  sRetVal;
		sRetVal.SetBinary ( pDoc, iDocLen );
		*ppResStr = (const BYTE *) sRetVal.Leak();
	}
	return iDocLen;
}

int Expr_SubstringIndex_c::LeftSearch ( const char * pDoc, int iDocLen, int iCount, bool bGetRight, const BYTE ** ppResStr, int * pResLen ) const
{
	int	  iTotalDelim = 0;
	const char * pDelBeg = m_sDelim.cstr();
	const char * pDelEnd = pDelBeg + m_iLenDelim;
	const char * pStrBeg = pDoc;
	const char * pStrEnd = (pStrBeg + iDocLen) - m_iLenDelim + 1;

	while ( pStrBeg<pStrEnd )
	{
		//	check first delimer string's char with current char from pStr
		if ( *pStrBeg==*pDelBeg )
		{
			//	first char is found, now we compare next chars in delimer string
			bool	bMatched = true;
			const char * p1 = pStrBeg + 1;
			const char * p2 = pDelBeg + 1;
			while ( bMatched && p2!=pDelEnd )
			{
				if ( *p1!=*p2 )
					bMatched = false;
				p1++;
				p2++;
			}

			//	if we found matched delimer string, then return left substring or search next delimer string
			if ( bMatched )
			{
				iTotalDelim++;
				iCount--;

				if ( iCount==0 )
				{
					if ( ppResStr && !bGetRight )
						*pResLen = SetResultString ( pDoc, pStrBeg - pDoc, ppResStr );

					if ( ppResStr && bGetRight )
					{
						pStrBeg += m_iLenDelim;
						*pResLen = SetResultString ( pStrBeg, iDocLen - (pStrBeg - pDoc), ppResStr );
					}

					return iTotalDelim;
				}
				pStrBeg += m_iLenDelim;
				continue;
			}
		}

		//	delimer string does not maatch with current ptr, goto to next char and repeat comparation
		int  iCharLen = sphUTF8Len ( pStrBeg, 1 );
		pStrBeg += ( iCharLen > 0 ) ? iCharLen : 1;
	}

	//	not found, return original string
	if ( iCount && ppResStr )
		*pResLen = SetResultString ( pDoc, iDocLen, ppResStr );

	return iTotalDelim;
}

int Expr_SubstringIndex_c::RightSearch ( const char * pDoc, int iDocLen, int iCount, const BYTE ** ppResStr, int * pResLen ) const
{
	//	find and count (iNumFoundDelim) of all delimer sub strings
	int  iNumFoundDelim = LeftSearch ( pDoc, iDocLen, iDocLen+1, false, NULL, NULL );

	//	correct iCount (which is negative) to positive index from left to right
	iCount += iNumFoundDelim + 1;

	//	if not found, return original string
	if ( iCount<=0 )
		*pResLen = SetResultString ( pDoc, iDocLen, ppResStr );

	//	find delimer sub string according to iCount and return result
	return LeftSearch ( pDoc, iDocLen, iCount, true, ppResStr, pResLen );
}

struct Expr_Iterator_c : Expr_JsonField_c
{
	SphAttr_t * m_pData;

	Expr_Iterator_c ( const CSphAttrLocator & tLocator, int iLocator, CSphVector<ISphExpr*> & dArgs, CSphVector<ESphAttr> & dRetTypes, SphAttr_t * pData )
		: Expr_JsonField_c ( tLocator, iLocator, dArgs, dRetTypes )
		, m_pData ( pData )
	{}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		uint64_t uValue = m_pData ? *m_pData : 0;
		const BYTE * p = m_pStrings + ( uValue & 0xffffffff );
		auto eType = (ESphJsonType)( uValue >> 32 );
		return DoEval ( eType, p, tMatch );
	}
};


struct Expr_ForIn_c : public Expr_JsonFieldConv_c
{
	CSphRefcountedPtr<ISphExpr> m_pExpr;
	bool m_bStrict;
	bool m_bIndex;
	mutable uint64_t m_uData = 0;

	Expr_ForIn_c ( ISphExpr * pArg, bool bStrict, bool bIndex )
		: Expr_JsonFieldConv_c ( pArg )
		, m_bStrict ( bStrict )
		, m_bIndex ( bIndex )
	{}

	SphAttr_t * GetRef ()
	{
		return (SphAttr_t*)&m_uData;
	}

	void SetExpr ( ISphExpr * pExpr )
	{
		if ( pExpr==m_pExpr )
			return;

		SafeAddRef ( pExpr );
		SafeRelease ( m_pExpr );
		m_pExpr = pExpr;
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		Expr_JsonFieldConv_c::FixupLocator ( pOldSchema, pNewSchema );
		if ( m_pExpr )
			m_pExpr->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_JsonFieldConv_c::Command ( eCmd, pArg );
		if ( m_pExpr )
			m_pExpr->Command ( eCmd, pArg );
	}

	bool ExprEval ( int * pResult, const CSphMatch & tMatch, int iIndex, ESphJsonType eType, const BYTE * pVal ) const
	{
		m_uData = ( ( (int64_t)( pVal-m_pStrings ) ) | ( ( (int64_t)eType )<<32 ) );
		bool bMatch = m_pExpr->Eval ( tMatch )!=0;
		*pResult = bMatch ? ( m_bIndex ? iIndex : 1 ) : ( m_bIndex ? -1 : 0 );
		return m_bStrict==bMatch;
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		int iResult = m_bIndex ? -1 : 0;

		if ( !m_pExpr )
			return iResult;

		const BYTE * p = nullptr;
		ESphJsonType eJson = GetKey ( &p, tMatch );

		switch ( eJson )
		{
		case JSON_INT32_VECTOR:
		case JSON_INT64_VECTOR:
		case JSON_DOUBLE_VECTOR:
			{
				int iSize = eJson==JSON_INT32_VECTOR ? 4 : 8;
				ESphJsonType eType = eJson==JSON_INT32_VECTOR ? JSON_INT32
					: eJson==JSON_INT64_VECTOR ? JSON_INT64
					: JSON_DOUBLE;
				int iLen = sphJsonUnpackInt ( &p );
				for ( int i=0; i<iLen; i++, p+=iSize )
					if ( !ExprEval ( &iResult, tMatch, i, eType, p ) )
						break;
				break;
			}
		case JSON_STRING_VECTOR:
			{
				sphJsonUnpackInt ( &p );
				int iLen = sphJsonUnpackInt ( &p );
				for ( int i=0;i<iLen;i++ )
				{
					if ( !ExprEval ( &iResult, tMatch, i, JSON_STRING, p ) )
						break;
					sphJsonSkipNode ( JSON_STRING, &p );
				}
				break;
			}
		case JSON_MIXED_VECTOR:
			{
				sphJsonUnpackInt ( &p );
				int iLen = sphJsonUnpackInt ( &p );
				for ( int i=0; i<iLen; i++ )
				{
					auto eType = (ESphJsonType)*p++;
					if ( !ExprEval ( &iResult, tMatch, i, eType, p ) )
						break;
					sphJsonSkipNode ( eType, &p );
				}
				break;
			}
		default:
			break;
		}

		return iResult;
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_ForIn_c");
		CALC_POD_HASH(m_bStrict);
		CALC_POD_HASH(m_bIndex);
		CALC_CHILD_HASH(m_pExpr);
		return CALC_PARENT_HASH();
	}
};


SphStringCmp_fn GetCollationFn ( ESphCollation eCollation )
{
	switch ( eCollation )
	{
		case SPH_COLLATION_LIBC_CS:			return sphCollateLibcCS;
		case SPH_COLLATION_UTF8_GENERAL_CI:	return sphCollateUtf8GeneralCI;
		case SPH_COLLATION_BINARY:			return sphCollateBinary;
		default:							return sphCollateLibcCI;
	}
}


struct Expr_StrEq_c : public Expr_Binary_c
{
	SphStringCmp_fn m_fnStrCmp;

	Expr_StrEq_c ( ISphExpr * pLeft, ISphExpr * pRight, ESphCollation eCollation )
		: Expr_Binary_c ( "Expr_StrEq_c", pLeft, pRight )
	{
		m_fnStrCmp = GetCollationFn ( eCollation );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pLeft = nullptr;
		const BYTE * pRight = nullptr;
		int iLeft = m_pFirst->StringEval ( tMatch, &pLeft );
		int iRight = m_pSecond->StringEval ( tMatch, &pRight );

		bool bEq = m_fnStrCmp ( pLeft, pRight, STRING_PLAIN, iLeft, iRight )==0;

		if ( m_pFirst->IsDataPtrAttr() ) SafeDeleteArray ( pLeft );
		if ( m_pSecond->IsDataPtrAttr() ) SafeDeleteArray ( pRight );

		return (int)bEq;
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_StrEq_c");
		CALC_POD_HASH(m_fnStrCmp);
		CALC_CHILD_HASH(m_pFirst);
		CALC_CHILD_HASH(m_pSecond);
		return CALC_DEP_HASHES();
	}
};


struct Expr_JsonFieldIsNull_c : public Expr_JsonFieldConv_c
{
	bool m_bEquals;

	explicit Expr_JsonFieldIsNull_c ( ISphExpr * pArg, bool bEquals )
		: Expr_JsonFieldConv_c ( pArg )
		, m_bEquals ( bEquals )
	{}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		return m_bEquals ^ ( eJson!=JSON_EOF && eJson!=JSON_NULL );
	}

	float	Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonFieldIsNull_c");
		CALC_POD_HASH(m_bEquals);
		return CALC_PARENT_HASH();
	}
};

//////////////////////////////////////////////////////////////////////////

struct Expr_MinTopWeight : public Expr_NoLocator_c
{
	int * m_pWeight = nullptr;

	int IntEval ( const CSphMatch & ) const final			{ return m_pWeight ? *m_pWeight : -INT_MAX; }
	float Eval ( const CSphMatch & ) const final			{ return m_pWeight ? (float)*m_pWeight : -FLT_MAX; }
	int64_t Int64Eval ( const CSphMatch & ) const final	{ return m_pWeight ? *m_pWeight : -LLONG_MAX; }

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		CSphMatch * pWorst;
		if ( eCmd!=SPH_EXPR_SET_EXTRA_DATA )
			return;
		if ( static_cast<ISphExtra*>(pArg)->ExtraData ( EXTRA_GET_QUEUE_WORST, (void**)&pWorst ) )
			m_pWeight = &pWorst->m_iWeight;
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true;
		return 0;
	}
};

struct Expr_MinTopSortval : public Expr_NoLocator_c
{
	CSphMatch *		m_pWorst = nullptr;
	int				m_iSortval = -1;

	float Eval ( const CSphMatch & ) const final
	{
		if ( m_pWorst && m_pWorst->m_pDynamic && m_iSortval>=0 )
			return *(float*)( m_pWorst->m_pDynamic + m_iSortval );
		return -FLT_MAX;
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd!=SPH_EXPR_SET_EXTRA_DATA )
			return;
		auto * p = (ISphExtra*)pArg;
		if ( !p->ExtraData ( EXTRA_GET_QUEUE_WORST, (void**)&m_pWorst )
			|| !p->ExtraData ( EXTRA_GET_QUEUE_SORTVAL, (void**)&m_iSortval ) )
		{
			m_pWorst = nullptr;
		}
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true;
		return 0;
	}
};


struct Expr_Rand_c : public Expr_Unary_c
{
	bool				m_bConst;
	mutable bool		m_bFirstEval = true;
	mutable uint64_t	m_uState;

	explicit Expr_Rand_c ( ISphExpr * pFirst, bool bConst )
		: Expr_Unary_c ( "Expr_Rand_c", pFirst )
		, m_bConst ( bConst )
		, m_bFirstEval ( true )
	{
		sphAutoSrand ();
		m_uState = ( (uint64_t)sphRand() << 32 ) + sphRand();
	}

	uint64_t XorShift64Star() const
	{
		m_uState ^= m_uState >> 12;
		m_uState ^= m_uState << 25;
		m_uState ^= m_uState >> 27;
		return m_uState * 2685821657736338717ULL;
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		if ( m_pFirst )
		{
			uint64_t uSeed = (uint64_t)m_pFirst->Int64Eval ( tMatch );
			if ( !m_bConst )
				m_uState = uSeed;
			else if ( m_bFirstEval )
			{
				m_uState = uSeed;
				m_bFirstEval = false;
			}
		}
		return (float)( XorShift64Star() / (double)UINT64_MAX );
	}

	int IntEval ( const CSphMatch & tMatch ) const final { return (int)Eval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t)Eval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) final
	{
		bDisable = true;
		return 0;
	}
};


//////////////////////////////////////////////////////////////////////////

#define FIRST	m_pFirst->Eval(tMatch)
#define SECOND	m_pSecond->Eval(tMatch)
#define THIRD	m_pThird->Eval(tMatch)

#define INTFIRST	m_pFirst->IntEval(tMatch)
#define INTSECOND	m_pSecond->IntEval(tMatch)
#define INTTHIRD	m_pThird->IntEval(tMatch)

#define INT64FIRST	m_pFirst->Int64Eval(tMatch)
#define INT64SECOND	m_pSecond->Int64Eval(tMatch)
#define INT64THIRD	m_pThird->Int64Eval(tMatch)

#define DECLARE_UNARY_TRAITS(_classname) \
	struct _classname : public Expr_Unary_c \
	{ \
		explicit _classname ( ISphExpr * pFirst ) : Expr_Unary_c ( #_classname, pFirst ) {}

#define DECLARE_END() };

#define DECLARE_UNARY_FLT(_classname,_expr) \
		DECLARE_UNARY_TRAITS ( _classname ) \
		float Eval ( const CSphMatch & tMatch ) const final { return _expr; } \
	};

#define DECLARE_UNARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_UNARY_TRAITS ( _classname ) \
		float Eval ( const CSphMatch & tMatch ) const final { return (float)_expr; } \
		int IntEval ( const CSphMatch & tMatch ) const final { return _expr2; } \
		int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return _expr3; } \
	};

#define IABS(_arg) ( (_arg)>0 ? (_arg) : (-_arg) )

DECLARE_UNARY_INT ( Expr_Neg_c,		-FIRST,					-INTFIRST,			-INT64FIRST )
DECLARE_UNARY_INT ( Expr_Abs_c,		fabs(FIRST),			IABS(INTFIRST),		IABS(INT64FIRST) )
DECLARE_UNARY_INT ( Expr_Ceil_c,	float(ceil(FIRST)),		int(ceil(FIRST)),	int64_t(ceil(FIRST)) )
DECLARE_UNARY_INT ( Expr_Floor_c,	float(floor(FIRST)),	int(floor(FIRST)),	int64_t(floor(FIRST)) )

DECLARE_UNARY_FLT ( Expr_Sin_c,		float(sin(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Cos_c,		float(cos(FIRST)) )
DECLARE_UNARY_FLT ( Expr_Exp_c,		float(exp(FIRST)) )

DECLARE_UNARY_INT ( Expr_NotInt_c,		(float)(INTFIRST?0:1),		INTFIRST?0:1,	INTFIRST?0:1 )
DECLARE_UNARY_INT ( Expr_NotInt64_c,	(float)(INT64FIRST?0:1),	INT64FIRST?0:1,	INT64FIRST?0:1 )
DECLARE_UNARY_INT ( Expr_Sint_c,		(float)(INTFIRST),			INTFIRST,		INTFIRST )

DECLARE_UNARY_TRAITS ( Expr_Ln_c )
       float Eval ( const CSphMatch & tMatch ) const final
       {
               float fFirst = m_pFirst->Eval ( tMatch );
               // ideally this would be SQLNULL instead of plain 0.0f
               return fFirst>0.0f ? (float)log ( fFirst ) : 0.0f;
       }
DECLARE_END()

DECLARE_UNARY_TRAITS ( Expr_Log2_c )
       float Eval ( const CSphMatch & tMatch ) const final
       {
               float fFirst = m_pFirst->Eval ( tMatch );
               // ideally this would be SQLNULL instead of plain 0.0f
               return fFirst>0.0f ? (float)( log ( fFirst )*M_LOG2E ) : 0.0f;
       }
DECLARE_END()

DECLARE_UNARY_TRAITS ( Expr_Log10_c )
       float Eval ( const CSphMatch & tMatch ) const final
       {
               float fFirst = m_pFirst->Eval ( tMatch );
               // ideally this would be SQLNULL instead of plain 0.0f
               return fFirst>0.0f ? (float)( log ( fFirst )*M_LOG10E ) : 0.0f;
       }
DECLARE_END()

DECLARE_UNARY_TRAITS ( Expr_Sqrt_c )
       float Eval ( const CSphMatch & tMatch ) const final
       {
               float fFirst = m_pFirst->Eval ( tMatch );
               // ideally this would be SQLNULL instead of plain 0.0f in case of negative argument
               // MEGA optimization: do not call sqrt for 0.0f
               return fFirst>0.0f ? (float)sqrt ( fFirst ) : 0.0f;
       }
DECLARE_END()

//////////////////////////////////////////////////////////////////////////

#define DECLARE_BINARY_TRAITS(_classname) \
	struct _classname : public Expr_Binary_c \
	{ \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond ) : Expr_Binary_c ( #_classname, pFirst, pSecond ) {}

#define DECLARE_BINARY_FLT(_classname,_expr) \
		DECLARE_BINARY_TRAITS ( _classname ) \
		float Eval ( const CSphMatch & tMatch ) const final { return _expr; } \
	};

#define DECLARE_BINARY_INT(_classname,_expr,_expr2,_expr3) \
		DECLARE_BINARY_TRAITS ( _classname ) \
		float Eval ( const CSphMatch & tMatch ) const final { return _expr; } \
		int IntEval ( const CSphMatch & tMatch ) const final { return _expr2; } \
		int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return _expr3; } \
	};

#define DECLARE_BINARY_POLY(_classname,_expr,_expr2,_expr3) \
	DECLARE_BINARY_INT ( _classname##Float_c,	_expr,						(int)Eval(tMatch),		(int64_t)Eval(tMatch ) ) \
	DECLARE_BINARY_INT ( _classname##Int_c,		(float)IntEval(tMatch),		_expr2,					(int64_t)IntEval(tMatch) ) \
	DECLARE_BINARY_INT ( _classname##Int64_c,	(float)Int64Eval(tMatch),	(int)Int64Eval(tMatch),	_expr3 )

#define IFFLT(_expr)	( (_expr) ? 1.0f : 0.0f )
#define IFINT(_expr)	( (_expr) ? 1 : 0 )

DECLARE_BINARY_INT ( Expr_Add_c,	FIRST + SECOND,						(DWORD)INTFIRST + (DWORD)INTSECOND,				(uint64_t)INT64FIRST + (uint64_t)INT64SECOND )
DECLARE_BINARY_INT ( Expr_Sub_c,	FIRST - SECOND,						(DWORD)INTFIRST - (DWORD)INTSECOND,				(uint64_t)INT64FIRST - (uint64_t)INT64SECOND )
DECLARE_BINARY_INT ( Expr_Mul_c,	FIRST * SECOND,						(DWORD)INTFIRST * (DWORD)INTSECOND,				(uint64_t)INT64FIRST * (uint64_t)INT64SECOND )
DECLARE_BINARY_INT ( Expr_BitAnd_c,	(float)(int(FIRST)&int(SECOND)),	INTFIRST & INTSECOND,				INT64FIRST & INT64SECOND )
DECLARE_BINARY_INT ( Expr_BitOr_c,	(float)(int(FIRST)|int(SECOND)),	INTFIRST | INTSECOND,				INT64FIRST | INT64SECOND )
DECLARE_BINARY_INT ( Expr_Mod_c,	(float)(int(FIRST)%int(SECOND)),	INTFIRST % INTSECOND,				INT64FIRST % INT64SECOND )

DECLARE_BINARY_TRAITS ( Expr_Div_c )
       float Eval ( const CSphMatch & tMatch ) const final
       {
               float fSecond = m_pSecond->Eval ( tMatch );
               // ideally this would be SQLNULL instead of plain 0.0f
               return fSecond ? m_pFirst->Eval ( tMatch )/fSecond : 0.0f;
       }
DECLARE_END()

DECLARE_BINARY_TRAITS ( Expr_Idiv_c )
	float Eval ( const CSphMatch & tMatch ) const final
	{
		auto iSecond = int(SECOND);
		// ideally this would be SQLNULL instead of plain 0.0f
		return iSecond ? float(int(FIRST)/iSecond) : 0.0f;
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		int iSecond = INTSECOND;
		// ideally this would be SQLNULL instead of plain 0
		return iSecond ? ( INTFIRST / iSecond ) : 0;
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		int64_t iSecond = INT64SECOND;
		// ideally this would be SQLNULL instead of plain 0
		return iSecond ? ( INT64FIRST / iSecond ) : 0;
	}
DECLARE_END()

DECLARE_BINARY_POLY ( Expr_Lt,		IFFLT ( FIRST<SECOND ),					IFINT ( INTFIRST<INTSECOND ),		IFINT ( INT64FIRST<INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Gt,		IFFLT ( FIRST>SECOND ),					IFINT ( INTFIRST>INTSECOND ),		IFINT ( INT64FIRST>INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Lte,		IFFLT ( FIRST<=SECOND ),				IFINT ( INTFIRST<=INTSECOND ),		IFINT ( INT64FIRST<=INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Gte,		IFFLT ( FIRST>=SECOND ),				IFINT ( INTFIRST>=INTSECOND ),		IFINT ( INT64FIRST>=INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Eq,		IFFLT ( fabs ( FIRST-SECOND )<=1e-6 ),	IFINT ( INTFIRST==INTSECOND ),		IFINT ( INT64FIRST==INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Ne,		IFFLT ( fabs ( FIRST-SECOND )>1e-6 ),	IFINT ( INTFIRST!=INTSECOND ),		IFINT ( INT64FIRST!=INT64SECOND ) )

DECLARE_BINARY_INT ( Expr_Min_c,	Min ( FIRST, SECOND ),					Min ( INTFIRST, INTSECOND ),		Min ( INT64FIRST, INT64SECOND ) )
DECLARE_BINARY_INT ( Expr_Max_c,	Max ( FIRST, SECOND ),					Max ( INTFIRST, INTSECOND ),		Max ( INT64FIRST, INT64SECOND ) )
DECLARE_BINARY_FLT ( Expr_Pow_c,	float ( pow ( FIRST, SECOND ) ) )

DECLARE_BINARY_POLY ( Expr_And,		FIRST!=0.0f && SECOND!=0.0f,		IFINT ( INTFIRST && INTSECOND ),	IFINT ( INT64FIRST && INT64SECOND ) )
DECLARE_BINARY_POLY ( Expr_Or,		FIRST!=0.0f || SECOND!=0.0f,		IFINT ( INTFIRST || INTSECOND ),	IFINT ( INT64FIRST || INT64SECOND ) )

DECLARE_BINARY_FLT ( Expr_Atan2_c,	float ( atan2 ( FIRST, SECOND ) ) )

//////////////////////////////////////////////////////////////////////////

/// boring base stuff
struct ExprThreeway_c : public ISphExpr
{
	CSphRefcountedPtr<ISphExpr>	m_pFirst;
	CSphRefcountedPtr<ISphExpr>	m_pSecond;
	CSphRefcountedPtr<ISphExpr>	m_pThird;
	CSphString	m_sExprName;

	ExprThreeway_c ( const char * szClassName, ISphExpr * pFirst, ISphExpr * pSecond, ISphExpr * pThird )
		: m_pFirst ( pFirst )
		, m_pSecond ( pSecond )
		, m_pThird ( pThird )
		, m_sExprName ( szClassName )
	{
		SafeAddRef ( pFirst );
		SafeAddRef ( pSecond );
		SafeAddRef ( pThird );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		m_pFirst->FixupLocator ( pOldSchema, pNewSchema );
		m_pSecond->FixupLocator ( pOldSchema, pNewSchema );
		m_pThird->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		m_pFirst->Command ( eCmd, pArg );
		m_pSecond->Command ( eCmd, pArg );
		m_pThird->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) override
	{
		EXPR_CLASS_NAME_NOCHECK(m_sExprName.cstr());
		CALC_CHILD_HASH(m_pFirst);
		CALC_CHILD_HASH(m_pSecond);
		CALC_CHILD_HASH(m_pThird);
		return CALC_DEP_HASHES();
	}
};

#define DECLARE_TERNARY(_classname,_expr,_expr2,_expr3) \
	struct _classname : public ExprThreeway_c \
	{ \
		_classname ( ISphExpr * pFirst, ISphExpr * pSecond, ISphExpr * pThird ) \
			: ExprThreeway_c ( #_classname, pFirst, pSecond, pThird ) {} \
		\
		float Eval ( const CSphMatch & tMatch ) const final { return _expr; } \
		int IntEval ( const CSphMatch & tMatch ) const final { return _expr2; } \
		int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return _expr3; } \
	};

DECLARE_TERNARY ( Expr_If_c,	( FIRST!=0.0f ) ? SECOND : THIRD,	INTFIRST ? INTSECOND : INTTHIRD,	INT64FIRST ? INT64SECOND : INT64THIRD )
DECLARE_TERNARY ( Expr_Madd_c,	FIRST*SECOND+THIRD,					INTFIRST*INTSECOND + INTTHIRD,		INT64FIRST*INT64SECOND + INT64THIRD )
DECLARE_TERNARY ( Expr_Mul3_c,	FIRST*SECOND*THIRD,					INTFIRST*INTSECOND*INTTHIRD,		INT64FIRST*INT64SECOND*INT64THIRD )

//////////////////////////////////////////////////////////////////////////

#define DECLARE_TIMESTAMP(_classname,_expr) \
	DECLARE_UNARY_TRAITS ( _classname ) \
		float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval(tMatch); } \
		int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return IntEval(tMatch); } \
		int IntEval ( const CSphMatch & tMatch ) const final \
		{ \
			time_t ts = (time_t)INTFIRST;	\
			struct tm s = {0}; \
			localtime_r ( &ts, &s ); \
			return _expr; \
		} \
	};

DECLARE_TIMESTAMP ( Expr_Day_c,				s.tm_mday )
DECLARE_TIMESTAMP ( Expr_Month_c,			s.tm_mon+1 )
DECLARE_TIMESTAMP ( Expr_Year_c,			s.tm_year+1900 )
DECLARE_TIMESTAMP ( Expr_YearMonth_c,		(s.tm_year+1900)*100+s.tm_mon+1 )
DECLARE_TIMESTAMP ( Expr_YearMonthDay_c,	(s.tm_year+1900)*10000+(s.tm_mon+1)*100+s.tm_mday )
DECLARE_TIMESTAMP ( Expr_Hour_c, s.tm_hour )
DECLARE_TIMESTAMP ( Expr_Minute_c, s.tm_min )
DECLARE_TIMESTAMP ( Expr_Second_c, s.tm_sec )

#define DECLARE_TIMESTAMP_UTC( _classname, _expr ) \
    DECLARE_UNARY_TRAITS ( _classname ) \
        float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval(tMatch); } \
        int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return IntEval(tMatch); } \
        int IntEval ( const CSphMatch & tMatch ) const final \
        { \
            time_t ts = (time_t)INTFIRST;    \
            struct tm s = {0}; \
            gmtime_r ( &ts, &s ); \
            return _expr; \
        } \
    };

DECLARE_TIMESTAMP_UTC ( Expr_Day_utc_c, s.tm_mday )
DECLARE_TIMESTAMP_UTC ( Expr_Month_utc_c, s.tm_mon + 1 )
DECLARE_TIMESTAMP_UTC ( Expr_Year_utc_c, s.tm_year + 1900 )
DECLARE_TIMESTAMP_UTC ( Expr_YearMonth_utc_c, (s.tm_year + 1900) * 100 + s.tm_mon + 1 )
DECLARE_TIMESTAMP_UTC ( Expr_YearMonthDay_utc_c, (s.tm_year + 1900) * 10000 + (s.tm_mon + 1) * 100 + s.tm_mday )

extern bool bGroupingInUtc; // defined in searchd.cpp

void setGroupingInUtc ( bool b_GroupingInUtc )
{
	bGroupingInUtc = b_GroupingInUtc;
}

Expr_Unary_c * ExprDay ( ISphExpr * pFirst )
{
	return bGroupingInUtc
		   ? (Expr_Unary_c *) new Expr_Day_utc_c ( pFirst )
		   : (Expr_Unary_c *) new Expr_Day_c ( pFirst );
}

Expr_Unary_c * ExprMonth ( ISphExpr * pFirst )
{
	return bGroupingInUtc
		   ? (Expr_Unary_c *) new Expr_Month_utc_c ( pFirst )
		   : (Expr_Unary_c *) new Expr_Month_c ( pFirst );
}

Expr_Unary_c * ExprYear ( ISphExpr * pFirst )
{
	return bGroupingInUtc
		   ? (Expr_Unary_c *) new Expr_Year_utc_c ( pFirst )
		   : (Expr_Unary_c *) new Expr_Year_c ( pFirst );
}

Expr_Unary_c * ExprYearMonth ( ISphExpr * pFirst )
{
	return bGroupingInUtc
		   ? (Expr_Unary_c *) new Expr_YearMonth_utc_c ( pFirst )
		   : (Expr_Unary_c *) new Expr_YearMonth_c ( pFirst );
}

Expr_Unary_c * ExprYearMonthDay ( ISphExpr * pFirst )
{
	return bGroupingInUtc
		   ? (Expr_Unary_c *) new Expr_YearMonthDay_utc_c ( pFirst )
		   : (Expr_Unary_c *) new Expr_YearMonthDay_c ( pFirst );
}

//////////////////////////////////////////////////////////////////////////
// UDF CALL SITE
//////////////////////////////////////////////////////////////////////////

void * UdfMalloc ( int iLen )
{
	return new BYTE [ iLen ];
}

/// UDF call site
struct UdfCall_t
{
	const PluginUDF_c *	m_pUdf = nullptr;
	SPH_UDF_INIT		m_tInit;
	SPH_UDF_ARGS		m_tArgs;
	CSphVector<int>		m_dArgs2Free; // these args should be freed explicitly

	UdfCall_t()
	{
		m_tInit.func_data = nullptr;
		m_tInit.is_const = false;
		m_tArgs.arg_count = 0;
		m_tArgs.arg_types = nullptr;
		m_tArgs.arg_values = nullptr;
		m_tArgs.arg_names = nullptr;
		m_tArgs.str_lengths = nullptr;
		m_tArgs.fn_malloc = UdfMalloc;
	}

	~UdfCall_t ()
	{
		SafeRelease ( m_pUdf );
		SafeDeleteArray ( m_tArgs.arg_types );
		SafeDeleteArray ( m_tArgs.arg_values );
		SafeDeleteArray ( m_tArgs.arg_names );
		SafeDeleteArray ( m_tArgs.str_lengths );
	}
};

//////////////////////////////////////////////////////////////////////////
// PARSER INTERNALS
//////////////////////////////////////////////////////////////////////////
class ExprParser_t;

#ifdef 	CMAKE_GENERATED_GRAMMAR
	#include "bissphinxexpr.h"
#else
	#include "yysphinxexpr.h"
#endif


/// known functions
enum Func_e
{
	FUNC_NOW=0,

	FUNC_ABS,
	FUNC_CEIL,
	FUNC_FLOOR,
	FUNC_SIN,
	FUNC_COS,
	FUNC_LN,
	FUNC_LOG2,
	FUNC_LOG10,
	FUNC_EXP,
	FUNC_SQRT,
	FUNC_BIGINT,
	FUNC_SINT,
	FUNC_CRC32,
	FUNC_FIBONACCI,

	FUNC_DAY,
	FUNC_MONTH,
	FUNC_YEAR,
	FUNC_YEARMONTH,
	FUNC_YEARMONTHDAY,
	FUNC_HOUR,
	FUNC_MINUTE,
	FUNC_SECOND,

	FUNC_MIN,
	FUNC_MAX,
	FUNC_POW,
	FUNC_IDIV,

	FUNC_IF,
	FUNC_MADD,
	FUNC_MUL3,

	FUNC_INTERVAL,
	FUNC_IN,
	FUNC_BITDOT,
	FUNC_REMAP,

	FUNC_GEODIST,
	FUNC_EXIST,
	FUNC_POLY2D,
	FUNC_GEOPOLY2D,
	FUNC_CONTAINS,
	FUNC_ZONESPANLIST,
	FUNC_TO_STRING,
	FUNC_RANKFACTORS,
	FUNC_PACKEDFACTORS,
	FUNC_FACTORS,
	FUNC_BM25F,
	FUNC_INTEGER,
	FUNC_DOUBLE,
	FUNC_LENGTH,
	FUNC_LEAST,
	FUNC_GREATEST,
	FUNC_UINT,

	FUNC_CURTIME,
	FUNC_UTC_TIME,
	FUNC_UTC_TIMESTAMP,
	FUNC_TIMEDIFF,
	FUNC_CURRENT_USER,
	FUNC_CONNECTION_ID,
	FUNC_ALL,
	FUNC_ANY,
	FUNC_INDEXOF,

	FUNC_MIN_TOP_WEIGHT,
	FUNC_MIN_TOP_SORTVAL,

	FUNC_ATAN2,
	FUNC_RAND,

	FUNC_REGEX,

	FUNC_SUBSTRING_INDEX
};


struct FuncDesc_t
{
	const char *	m_sName;
	int				m_iArgs;
	Func_e			m_eFunc;
	ESphAttr		m_eRet;
};


static FuncDesc_t g_dFuncs[] =
{
	{ "now",			0,	FUNC_NOW,			SPH_ATTR_INTEGER },

	{ "abs",			1,	FUNC_ABS,			SPH_ATTR_NONE },
	{ "ceil",			1,	FUNC_CEIL,			SPH_ATTR_BIGINT },
	{ "floor",			1,	FUNC_FLOOR,			SPH_ATTR_BIGINT },
	{ "sin",			1,	FUNC_SIN,			SPH_ATTR_FLOAT },
	{ "cos",			1,	FUNC_COS,			SPH_ATTR_FLOAT },
	{ "ln",				1,	FUNC_LN,			SPH_ATTR_FLOAT },
	{ "log2",			1,	FUNC_LOG2,			SPH_ATTR_FLOAT },
	{ "log10",			1,	FUNC_LOG10,			SPH_ATTR_FLOAT },
	{ "exp",			1,	FUNC_EXP,			SPH_ATTR_FLOAT },
	{ "sqrt",			1,	FUNC_SQRT,			SPH_ATTR_FLOAT },
	{ "bigint",			1,	FUNC_BIGINT,		SPH_ATTR_BIGINT },	// type-enforcer special as-if-function
	{ "sint",			1,	FUNC_SINT,			SPH_ATTR_BIGINT },	// type-enforcer special as-if-function
	{ "crc32",			1,	FUNC_CRC32,			SPH_ATTR_INTEGER },
	{ "fibonacci",		1,	FUNC_FIBONACCI,		SPH_ATTR_INTEGER },

	{ "day",			1,	FUNC_DAY,			SPH_ATTR_INTEGER },
	{ "month",			1,	FUNC_MONTH,			SPH_ATTR_INTEGER },
	{ "year",			1,	FUNC_YEAR,			SPH_ATTR_INTEGER },
	{ "yearmonth",		1,	FUNC_YEARMONTH,		SPH_ATTR_INTEGER },
	{ "yearmonthday",	1,	FUNC_YEARMONTHDAY,	SPH_ATTR_INTEGER },
	{ "hour",			1,	FUNC_HOUR,			SPH_ATTR_INTEGER },
	{ "minute",			1,	FUNC_MINUTE,		SPH_ATTR_INTEGER },
	{ "second",			1,	FUNC_SECOND,		SPH_ATTR_INTEGER },

	{ "min",			2,	FUNC_MIN,			SPH_ATTR_NONE },
	{ "max",			2,	FUNC_MAX,			SPH_ATTR_NONE },
	{ "pow",			2,	FUNC_POW,			SPH_ATTR_FLOAT },
	{ "idiv",			2,	FUNC_IDIV,			SPH_ATTR_NONE },

	{ "if",				3,	FUNC_IF,			SPH_ATTR_NONE },
	{ "madd",			3,	FUNC_MADD,			SPH_ATTR_NONE },
	{ "mul3",			3,	FUNC_MUL3,			SPH_ATTR_NONE },

	{ "interval",		-2,	FUNC_INTERVAL,		SPH_ATTR_INTEGER },
	{ "in",				-1, FUNC_IN,			SPH_ATTR_INTEGER },
	{ "bitdot",			-1, FUNC_BITDOT,		SPH_ATTR_NONE },
	{ "remap",			4,	FUNC_REMAP,			SPH_ATTR_INTEGER },

	{ "geodist",		-4,	FUNC_GEODIST,		SPH_ATTR_FLOAT },
	{ "exist",			2,	FUNC_EXIST,			SPH_ATTR_NONE },
	{ "poly2d",			-1,	FUNC_POLY2D,		SPH_ATTR_POLY2D },
	{ "geopoly2d",		-1,	FUNC_GEOPOLY2D,		SPH_ATTR_POLY2D },
	{ "contains",		3,	FUNC_CONTAINS,		SPH_ATTR_INTEGER },
	{ "zonespanlist",	0,	FUNC_ZONESPANLIST,	SPH_ATTR_STRINGPTR },
	{ "to_string",		1,	FUNC_TO_STRING,		SPH_ATTR_STRINGPTR },
	{ "rankfactors",	0,	FUNC_RANKFACTORS,	SPH_ATTR_STRINGPTR },
	{ "packedfactors",	0,	FUNC_PACKEDFACTORS, SPH_ATTR_FACTORS },
	{ "factors",		0,	FUNC_FACTORS,		SPH_ATTR_FACTORS }, // just an alias for PACKEDFACTORS()
	{ "bm25f",			-2,	FUNC_BM25F,			SPH_ATTR_FLOAT },
	{ "integer",		1,	FUNC_INTEGER,		SPH_ATTR_BIGINT },
	{ "double",			1,	FUNC_DOUBLE,		SPH_ATTR_FLOAT },
	{ "length",			1,	FUNC_LENGTH,		SPH_ATTR_INTEGER },
	{ "least",			1,	FUNC_LEAST,			SPH_ATTR_STRINGPTR },
	{ "greatest",		1,	FUNC_GREATEST,		SPH_ATTR_STRINGPTR },
	{ "uint",			1,	FUNC_UINT,			SPH_ATTR_INTEGER },

	{ "curtime",		0,	FUNC_CURTIME,		SPH_ATTR_STRINGPTR },
	{ "utc_time",		0,	FUNC_UTC_TIME,		SPH_ATTR_STRINGPTR },
	{ "utc_timestamp",	0,	FUNC_UTC_TIMESTAMP,	SPH_ATTR_STRINGPTR },
	{ "timediff",		2,	FUNC_TIMEDIFF,		SPH_ATTR_STRINGPTR },
	{ "current_user",	0,	FUNC_CURRENT_USER,	SPH_ATTR_INTEGER },
	{ "connection_id",	0,	FUNC_CONNECTION_ID,	SPH_ATTR_INTEGER },
	{ "all",			-1,	FUNC_ALL,			SPH_ATTR_INTEGER },
	{ "any",			-1,	FUNC_ANY,			SPH_ATTR_INTEGER },
	{ "indexof",		-1,	FUNC_INDEXOF,		SPH_ATTR_BIGINT },

	{ "min_top_weight",		0,	FUNC_MIN_TOP_WEIGHT,	SPH_ATTR_INTEGER },
	{ "min_top_sortval",	0,	FUNC_MIN_TOP_SORTVAL,	SPH_ATTR_FLOAT },

	{ "atan2",			2,	FUNC_ATAN2,			SPH_ATTR_FLOAT },
	{ "rand",			-1,	FUNC_RAND,			SPH_ATTR_FLOAT },

	{ "regex",			2,	FUNC_REGEX,			SPH_ATTR_INTEGER },

	{ "substring_index",	3,	FUNC_SUBSTRING_INDEX,	SPH_ATTR_STRINGPTR }
};


// helper to generate input data for gperf
// run this, run gperf, that will generate a C program
// copy dAsso from asso_values in that C source
// modify iHash switch according to that C source, if needed
// copy dIndexes from the program output
#if 0
int HashGen()
{
	printf ( "struct func { char *name; int num; };\n%%%%\n" );
	for ( int i=0; i<int( sizeof ( g_dFuncs )/sizeof ( g_dFuncs[0] )); i++ )
		printf ( "%s, %d\n", g_dFuncs[i].m_sName, i );
	printf ( "%%%%\n" );
	printf ( "void main()\n" );
	printf ( "{\n" );
	printf ( "\tint i;\n" );
	printf ( "\tfor ( i=0; i<=MAX_HASH_VALUE; i++ )\n" );
	printf ( "\t\tprintf ( \"%%d,%%s\", wordlist[i].name[0] ? wordlist[i].num : -1, (i%%10)==9 ? \"\\n\" : \" \" );\n" );
	printf ( "}\n" );
	printf ( "// gperf -Gt 1.p > 1.c\n" );
	exit ( 0 );
	return 0;
}

static int G_HASHGEN = HashGen();
#endif


// FIXME? can remove this by preprocessing the assoc table
static inline BYTE FuncHashLower ( BYTE u )
{
	return ( u>='A' && u<='Z' ) ? ( u | 0x20 ) : u;
}


static int FuncHashLookup ( const char * sKey )
{
	assert ( sKey && sKey[0] );

	static BYTE dAsso[] =
    {
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		 15, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139,  25, 139,	15,  25,   0,
		 75,  10,  65,	10,  15,   0, 139, 139,   5,   0,
		 10,   0,  55,	 0,  25,  35,  25,	25, 139,  80,
		 55,  40,	0, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
		139, 139, 139, 139, 139, 139
    };

	auto * s = (const BYTE*) sKey;
	auto iHash = strlen ( sKey );
	switch ( iHash )
	{
		default:		iHash += dAsso [ FuncHashLower ( s[2] ) ];
		case 2:			iHash += dAsso [ FuncHashLower ( s[1] ) ];
		case 1:			iHash += dAsso [ FuncHashLower ( s[0] ) ];
	}

	static int dIndexes[] =
	{
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, 31, 23, 2, 16, 21, 6, 38, 7,
		8, -1, 39, 56, 60, 61, -1, 34, 57, 37,
		13, 47, -1, 54, 29, 48, -1, -1, 5, 50,
		33, 11, 45, 30, 20, 44, -1, -1, 4, 12,
		64, 22, -1, 49, 63, -1, 32, 51, 52, 40,
		62, 41, 55, 53, 10, -1, 36, 27, 58, 17,
		35, -1, -1, 24, 18, 3, -1, 19, 1, 26,
		-1, -1, -1, 42, -1, -1, -1, 43, -1, -1,
		-1, -1, 59, 0, 28, -1, -1, -1, -1, 14,
		65, -1, -1, -1, -1, -1, 46, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, 9, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, 15, -1, -1, -1, -1, 25,
	};

	if ( iHash>=(int)(sizeof(dIndexes)/sizeof(dIndexes[0])) )
		return -1;

	int iFunc = dIndexes[iHash];
	if ( iFunc>=0 && strcasecmp ( g_dFuncs[iFunc].m_sName, sKey )==0 )
		return iFunc;
	return -1;
}


static int FuncHashCheck()
{
	for ( int i=0; i<(int)(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])); i++ )
	{
		CSphString sKey ( g_dFuncs[i].m_sName );
		sKey.ToLower();
		if ( FuncHashLookup ( sKey.cstr() )!=i )
			sphDie ( "INTERNAL ERROR: lookup for %s() failed, rebuild function hash", sKey.cstr() );
		sKey.ToUpper();
		if ( FuncHashLookup ( sKey.cstr() )!=i )
			sphDie ( "INTERNAL ERROR: lookup for %s() failed, rebuild function hash", sKey.cstr() );
		if ( g_dFuncs[i].m_eFunc!=i )
			sphDie ( "INTERNAL ERROR: function hash entry %s() at index %d maps to Func_e entry %d, sync Func_e and g_dFuncs",
				sKey.cstr(), i, g_dFuncs[i].m_eFunc );
	}
	if ( FuncHashLookup("A")!=-1 )
		sphDie ( "INTERNAL ERROR: lookup for A() succeeded, rebuild function hash" );
	return 1;
}

static int VARIABLE_IS_NOT_USED G_FUNC_HASH_CHECK = FuncHashCheck();

//////////////////////////////////////////////////////////////////////////

static ISphExpr * ConvertExprJson ( ISphExpr * pExpr );
static void ConvertArgsJson ( VecRefPtrs_t<ISphExpr *> & dArgs );

/// check whether the type is numeric
static inline bool IsNumeric ( ESphAttr eType )
{
	return eType==SPH_ATTR_INTEGER || eType==SPH_ATTR_BIGINT || eType==SPH_ATTR_FLOAT;
}

/// check whether the type is int or bigint
static inline bool IsInt ( ESphAttr eType )
{
	return eType==SPH_ATTR_INTEGER || eType==SPH_ATTR_BIGINT;
}

static inline bool IsJson ( ESphAttr eAttr )
{
	return ( eAttr==SPH_ATTR_JSON_FIELD );
}

/// check for type based on int value
static inline ESphAttr GetIntType ( int64_t iValue )
{
	return ( iValue>=(int64_t)INT_MIN && iValue<=(int64_t)INT_MAX ) ? SPH_ATTR_INTEGER : SPH_ATTR_BIGINT;
}

/// get the widest numeric type of the two
static inline ESphAttr WidestType ( ESphAttr a, ESphAttr b )
{
	assert ( ( IsNumeric(a) && IsNumeric(b) ) || ( IsNumeric(a) && b==SPH_ATTR_JSON_FIELD ) || ( a==SPH_ATTR_JSON_FIELD && IsNumeric(b) ) );
	if ( a==SPH_ATTR_FLOAT || b==SPH_ATTR_FLOAT )
		return SPH_ATTR_FLOAT;
	if ( a==SPH_ATTR_BIGINT || b==SPH_ATTR_BIGINT )
		return SPH_ATTR_BIGINT;
	if ( a==SPH_ATTR_JSON_FIELD || b==SPH_ATTR_JSON_FIELD )
		return SPH_ATTR_BIGINT;
	return SPH_ATTR_INTEGER;
}

/// list of constants
class ConstList_c
{
public:
	CSphVector<int64_t>		m_dInts;		///< dword/int64 storage
	CSphVector<float>		m_dFloats;		///< float storage
	ESphAttr				m_eRetType { SPH_ATTR_INTEGER };		///< SPH_ATTR_INTEGER, SPH_ATTR_BIGINT, SPH_ATTR_STRING, or SPH_ATTR_FLOAT
	CSphString				m_sExpr;		///< m_sExpr copy for TOK_CONST_STRING evaluation
	bool					m_bPackedStrings = false;

public:

	void Add ( int64_t iValue )
	{
		if ( m_eRetType==SPH_ATTR_FLOAT )
		{
			m_dFloats.Add ( (float)iValue );
		} else
		{
			m_eRetType = WidestType ( m_eRetType, GetIntType ( iValue ) );
			m_dInts.Add ( iValue );
		}
	}

	void Add ( float fValue )
	{
		if ( m_eRetType!=SPH_ATTR_FLOAT )
		{
			assert ( m_dFloats.GetLength()==0 );
			ARRAY_FOREACH ( i, m_dInts )
				m_dFloats.Add ( (float)m_dInts[i] );
			m_dInts.Reset ();
			m_eRetType = SPH_ATTR_FLOAT;
		}
		m_dFloats.Add ( fValue );
	}
};


/// {title=2, body=1}
/// {in=deg, out=mi}
/// argument to functions like BM25F() and GEODIST()
class MapArg_c
{
public:
	CSphVector<CSphNamedVariant> m_dPairs;

public:
	void Add ( const char * sKey, const char * sValue, int64_t iValue )
	{
		CSphNamedVariant & t = m_dPairs.Add();
		t.m_sKey = sKey;
		if ( sValue )
			t.m_sValue = sValue;
		else
			t.m_iValue = (int)iValue;
	}
};


/// expression tree node
/// used to build an AST (Abstract Syntax Tree)
struct ExprNode_t
{
	int				m_iToken = 0;	///< token type, including operators
	ESphAttr		m_eRetType { SPH_ATTR_NONE };	///< result type
	ESphAttr		m_eArgType { SPH_ATTR_NONE };	///< args type
	CSphAttrLocator	m_tLocator;	///< attribute locator, for TOK_ATTR type
	int				m_iLocator = -1; ///< index of attribute locator in schema

	union
	{
		int64_t			m_iConst;		///< constant value, for TOK_CONST_INT type
		float			m_fConst;		///< constant value, for TOK_CONST_FLOAT type
		int				m_iFunc;		///< built-in function id, for TOK_FUNC type
		int				m_iArgs;		///< args count, for arglist (token==',') type
		ConstList_c *	m_pConsts;		///< constants list, for TOK_CONST_LIST type
		MapArg_c	*	m_pMapArg;		///< map argument (maps name to const or name to expr), for TOK_MAP_ARG type
		const char	*	m_sIdent;		///< pointer to const char, for TOK_IDENT type
		SphAttr_t	*	m_pAttr;		///< pointer to 64-bit value, for TOK_ITERATOR type
		WIDEST<int64_t,float,int, ConstList_c *, MapArg_c *,const char*, SphAttr_t*>::T m_null = 0;
	};
	int				m_iLeft = -1;
	int				m_iRight = -1;
};

struct StackNode_t
{
	int m_iNode;
	int m_iLeft;
	int m_iRight;
};

/// expression parser
class ExprParser_t
{
	friend int				yylex ( YYSTYPE * lvalp, ExprParser_t * pParser );
	friend int				yyparse ( ExprParser_t * pParser );
	friend void				yyerror ( ExprParser_t * pParser, const char * sMessage );

public:
	ExprParser_t ( ISphExprHook * pHook, CSphQueryProfile * pProfiler, ESphCollation eCollation )
		: m_pHook ( pHook )
		, m_pProfiler ( pProfiler )
		, m_eCollation ( eCollation )
	{
		m_dGatherStack.Reserve ( 64 );
	}

							~ExprParser_t ();
	ISphExpr *				Parse ( const char * sExpr, const ISphSchema & tSchema, ESphAttr * pAttrType, bool * pUsesWeight, CSphString & sError );

protected:
	int						m_iParsed = 0;	///< filled by yyparse() at the very end
	CSphString				m_sLexerError;
	CSphString				m_sParserError;
	CSphString				m_sCreateError;
	ISphExprHook *			m_pHook;
	CSphQueryProfile *		m_pProfiler;

protected:
	ESphAttr				GetWidestRet ( int iLeft, int iRight );

	int						AddNodeInt ( int64_t iValue );
	int						AddNodeFloat ( float fValue );
	int						AddNodeString ( int64_t iValue );
	int						AddNodeAttr ( int iTokenType, uint64_t uAttrLocator );
	int						AddNodeID ();
	int						AddNodeWeight ();
	int						AddNodeOp ( int iOp, int iLeft, int iRight );
	int						AddNodeFunc0 ( int iFunc );
	int						AddNodeFunc ( int iFunc, int iArg );
	int						AddNodeFor ( int iFunc, int iExpr, int iLoop );
	int						AddNodeIn ( int iArg, int iList );
	int						AddNodeRemap ( int iExpr1, int iExpr2, int iList1, int iList2 );
	int						AddNodeRand ( int iArg );
	int						AddNodeUdf ( int iCall, int iArg );
	int						AddNodePF ( int iFunc, int iArg );
	int						AddNodeConstlist ( int64_t iValue, bool bPackedString );
	int						AddNodeConstlist ( float iValue );
	void					AppendToConstlist ( int iNode, int64_t iValue );
	void					AppendToConstlist ( int iNode, float iValue );
	int						AddNodeUservar ( int iUservar );
	int						AddNodeHookIdent ( int iID );
	int						AddNodeHookFunc ( int iID, int iLeft );
	int						AddNodeMapArg ( const char * sKey, const char * sValue, int64_t iValue );
	void					AppendToMapArg ( int iNode, const char * sKey, const char * sValue, int64_t iValue );
	const char *			Attr2Ident ( uint64_t uAttrLoc );
	int						AddNodeJsonField ( uint64_t uAttrLocator, int iLeft );
	int						AddNodeJsonSubkey ( int64_t iValue );
	int						AddNodeDotNumber ( int64_t iValue );
	int						AddNodeIdent ( const char * sKey, int iLeft );

private:
	const char *			m_sExpr = nullptr;
	const char *			m_pCur = nullptr;
	const char *			m_pLastTokenStart = nullptr;
	const ISphSchema *		m_pSchema = nullptr;
	CSphVector<ExprNode_t>	m_dNodes;
	StrVec_t				m_dUservars;
	CSphVector<char*>		m_dIdents;
	int						m_iConstNow = 0;
	CSphVector<StackNode_t>	m_dGatherStack;
	CSphVector<UdfCall_t*>	m_dUdfCalls;

public:
	bool					m_bHasZonespanlist = false;
	DWORD					m_uPackedFactorFlags { SPH_FACTOR_DISABLE };
	ESphEvalStage			m_eEvalStage { SPH_EVAL_FINAL };
	ESphCollation			m_eCollation;

private:
	int						GetToken ( YYSTYPE * lvalp );

	void					GatherArgTypes ( int iNode, CSphVector<int> & dTypes );
	void					GatherArgNodes ( int iNode, CSphVector<int> & dNodes );
	void					GatherArgRetTypes ( int iNode, CSphVector<ESphAttr> & dTypes );
	template < typename T >
	void					GatherArgT ( int iNode, T & FUNCTOR );

	bool					CheckForConstSet ( int iArgsNode, int iSkip );
	int						ParseAttr ( int iAttr, const char* sTok, YYSTYPE * lvalp );

	template < typename T >
	void					WalkTree ( int iRoot, T & FUNCTOR );

	void					Optimize ( int iNode );
	void					CanonizePass ( int iNode );
	void					ConstantFoldPass ( int iNode );
	void					VariousOptimizationsPass ( int iNode );
	void					Dump ( int iNode );

	ISphExpr *				CreateTree ( int iNode );
	ISphExpr *				CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs );
	ISphExpr *				CreateInNode ( int iNode );
	ISphExpr *				CreateLengthNode ( const ExprNode_t & tNode, ISphExpr * pLeft );
	ISphExpr *				CreateGeodistNode ( int iArgs );
	ISphExpr *				CreatePFNode ( int iArg );
	ISphExpr *				CreateBitdotNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs );
	ISphExpr *				CreateUdfNode ( int iCall, ISphExpr * pLeft );
	ISphExpr *				CreateExistNode ( const ExprNode_t & tNode );
	ISphExpr *				CreateContainsNode ( const ExprNode_t & tNode );
	ISphExpr *				CreateAggregateNode ( const ExprNode_t & tNode, ESphAggrFunc eFunc, ISphExpr * pLeft );
	ISphExpr *				CreateForInNode ( int iNode );
	ISphExpr *				CreateRegexNode ( ISphExpr * pAttr, ISphExpr * pString );
	void					FixupIterators ( int iNode, const char * sKey, SphAttr_t * pAttr );

	bool					GetError () const { return !( m_sLexerError.IsEmpty() && m_sParserError.IsEmpty() && m_sCreateError.IsEmpty() ); }
};

//////////////////////////////////////////////////////////////////////////

/// parse that numeric constant (e.g. "123", ".03")
static int ParseNumeric ( YYSTYPE * lvalp, const char ** ppStr )
{
	assert ( lvalp && ppStr && *ppStr );

	// try float route
	char * pEnd = nullptr;
	auto fRes = (float) strtod ( *ppStr, &pEnd );

	// try int route
	uint64_t uRes = 0; // unsigned overflow is better than signed overflow
	bool bInt = true;
	for ( const char * p=(*ppStr); p<pEnd; p++ && bInt )
	{
		if ( isdigit(*p) )
			uRes = uRes*10 + (int)( (*p)-'0' ); // FIXME! missing overflow check, missing octal/hex handling
		else
			bInt = false;
	}

	// choose your destiny
	*ppStr = pEnd;
	if ( bInt )
	{
		lvalp->iConst = (int64_t)uRes;
		return TOK_CONST_INT;
	} else
	{
		lvalp->fConst = fRes;
		return TOK_CONST_FLOAT;
	}
}

// used to store in 8 bytes in Bison lvalp variable
static uint64_t sphPackAttrLocator ( const CSphAttrLocator & tLoc, int iLocator )
{
	assert ( iLocator>=0 && iLocator<=0x7fff );
	uint64_t uIndex = 0;
	uIndex = ( tLoc.m_iBitOffset<<16 ) + tLoc.m_iBitCount + ( (uint64_t)iLocator<<32 );
	if ( tLoc.m_bDynamic )
		uIndex |= ( U64C(1)<<63 );

	return uIndex;
}

static void sphUnpackAttrLocator ( uint64_t uIndex, ExprNode_t * pNode )
{
	assert ( pNode );
	pNode->m_tLocator.m_iBitOffset = (int)( ( uIndex>>16 ) & 0xffff );
	pNode->m_tLocator.m_iBitCount = (int)( uIndex & 0xffff );
	pNode->m_tLocator.m_bDynamic = ( ( uIndex & ( U64C(1)<<63 ) )!=0 );

	pNode->m_iLocator = (int)( ( uIndex>>32 ) & 0x7fff );
}

int ExprParser_t::ParseAttr ( int iAttr, const char* sTok, YYSTYPE * lvalp )
{
	// check attribute type and width
	const CSphColumnInfo & tCol = m_pSchema->GetAttr ( iAttr );

	int iRes = -1;
	switch ( tCol.m_eAttrType )
	{
	case SPH_ATTR_FLOAT:			iRes = TOK_ATTR_FLOAT;	break;

	case SPH_ATTR_UINT32SET:
	case SPH_ATTR_UINT32SET_PTR:	iRes = TOK_ATTR_MVA32; break;

	case SPH_ATTR_INT64SET:
	case SPH_ATTR_INT64SET_PTR:		iRes = TOK_ATTR_MVA64; break;

	case SPH_ATTR_STRING:
	case SPH_ATTR_STRINGPTR:		iRes = TOK_ATTR_STRING; break;

	case SPH_ATTR_JSON:
	case SPH_ATTR_JSON_PTR:
	case SPH_ATTR_JSON_FIELD:
	case SPH_ATTR_JSON_FIELD_PTR:	iRes = TOK_ATTR_JSON; break;

	case SPH_ATTR_FACTORS:			iRes = TOK_ATTR_FACTORS; break;

	case SPH_ATTR_INTEGER:
	case SPH_ATTR_TIMESTAMP:
	case SPH_ATTR_BOOL:
	case SPH_ATTR_BIGINT:
	case SPH_ATTR_TOKENCOUNT:
		iRes = tCol.m_tLocator.IsBitfield() ? TOK_ATTR_BITS : TOK_ATTR_INT;
		break;
	default:
		m_sLexerError.SetSprintf ( "attribute '%s' is of unsupported type (type=%d)", sTok, tCol.m_eAttrType );
		return -1;
	}

	lvalp->iAttrLocator = sphPackAttrLocator ( tCol.m_tLocator, iAttr );
	return iRes;
}


/// a lexer of my own
/// returns token id and fills lvalp on success
/// returns -1 and fills sError on failure
int ExprParser_t::GetToken ( YYSTYPE * lvalp )
{
	// skip whitespace, check eof
	while ( isspace ( *m_pCur ) ) m_pCur++;
	m_pLastTokenStart = m_pCur;
	if ( !*m_pCur ) return 0;

	// check for constant
	if ( isdigit ( m_pCur[0] ) )
		return ParseNumeric ( lvalp, &m_pCur );

	// check for field, function, or magic name
	if ( sphIsAttr ( m_pCur[0] )
		|| ( m_pCur[0]=='@' && sphIsAttr ( m_pCur[1] ) && !isdigit ( m_pCur[1] ) ) )
	{
		// get token
		const char * pStart = m_pCur++;
		while ( sphIsAttr ( *m_pCur ) ) m_pCur++;

		CSphString sTok;
		sTok.SetBinary ( pStart, m_pCur-pStart );
		CSphString sTokMixedCase = sTok;
		sTok.ToLower ();

		// check for magic name
		if ( sTok=="@id" )			return TOK_ATID;
		if ( sTok=="@weight" )		return TOK_ATWEIGHT;
		if ( sTok=="id" )			return TOK_ID;
		if ( sTok=="weight" )		return TOK_WEIGHT;
		if ( sTok=="groupby" )		return TOK_GROUPBY;
		if ( sTok=="distinct" )		return TOK_DISTINCT;
		if ( sTok=="@geodist" )
		{
			int iGeodist = m_pSchema->GetAttrIndex("@geodist");
			if ( iGeodist==-1 )
			{
				m_sLexerError = "geoanchor is not set, @geodist expression unavailable";
				return -1;
			}
			const CSphAttrLocator & tLoc = m_pSchema->GetAttr ( iGeodist ).m_tLocator;
			lvalp->iAttrLocator = sphPackAttrLocator ( tLoc, iGeodist );
			return TOK_ATTR_FLOAT;
		}

		// check for uservar
		if ( pStart[0]=='@' )
		{
			lvalp->iNode = m_dUservars.GetLength();
			m_dUservars.Add ( sTok );
			return TOK_USERVAR;
		}

		// check for keyword
		if ( sTok=="and" )		{ return TOK_AND; }
		if ( sTok=="or" )		{ return TOK_OR; }
		if ( sTok=="not" )		{ return TOK_NOT; }
		if ( sTok=="div" )		{ return TOK_DIV; }
		if ( sTok=="mod" )		{ return TOK_MOD; }
		if ( sTok=="for" )		{ return TOK_FOR; }
		if ( sTok=="is" )		{ return TOK_IS; }
		if ( sTok=="null" )		{ return TOK_NULL; }

		// in case someone used 'count' as a name for an attribute
		if ( sTok=="count" )
		{
			int iAttr = m_pSchema->GetAttrIndex ( "count" );
			if ( iAttr>=0 )
				ParseAttr ( iAttr, sTok.cstr(), lvalp );
			return TOK_COUNT;
		}

		// check for attribute
		int iAttr = m_pSchema->GetAttrIndex ( sTok.cstr() );
		if ( iAttr>=0 )
			return ParseAttr ( iAttr, sTok.cstr(), lvalp );

		// hook might replace built-in function
		int iHookFunc = -1;
		if ( m_pHook )
			iHookFunc = m_pHook->IsKnownFunc ( sTok.cstr() );

		// check for function
		int iFunc = FuncHashLookup ( sTok.cstr() );
		if ( iFunc>=0 && iHookFunc==-1 )
		{
			assert ( !strcasecmp ( g_dFuncs[iFunc].m_sName, sTok.cstr() ) );
			lvalp->iFunc = iFunc;
			switch ( iFunc )
			{
			case FUNC_IN: return TOK_FUNC_IN;
			case FUNC_REMAP : return TOK_FUNC_REMAP;
			case FUNC_PACKEDFACTORS:
			case FUNC_FACTORS: return TOK_FUNC_PF;
			case FUNC_RAND: return TOK_FUNC_RAND;
			case FUNC_ALL:
			case FUNC_ANY:
			case FUNC_INDEXOF: return TOK_FUNC_JA; // json aggrs
			default: return TOK_FUNC;
			}
		}

		// ask hook
		if ( m_pHook )
		{
			int iID = m_pHook->IsKnownIdent ( sTok.cstr() );
			if ( iID>=0 )
			{
				lvalp->iNode = iID;
				return TOK_HOOK_IDENT;
			}

			iID = iHookFunc;
			if ( iID>=0 )
			{
				lvalp->iNode = iID;
				return TOK_HOOK_FUNC;
			}
		}

		// check for UDF
		auto * pUdf = (const PluginUDF_c *) sphPluginGet ( PLUGIN_FUNCTION, sTok.cstr() );
		if ( pUdf )
		{
			lvalp->iNode = m_dUdfCalls.GetLength();
			m_dUdfCalls.Add ( new UdfCall_t() );
			m_dUdfCalls.Last()->m_pUdf = pUdf;
			return TOK_UDF;
		}

		// arbitrary identifier, then
		m_dIdents.Add ( sTokMixedCase.Leak() );
		lvalp->sIdent = m_dIdents.Last();
		return TOK_IDENT;
	}

	// check for known operators, then
	switch ( *m_pCur )
	{
		case '+':
		case '-':
		case '*':
		case '/':
		case '(':
		case ')':
		case ',':
		case '&':
		case '|':
		case '%':
		case '{':
		case '}':
		case '[':
		case ']':
		case '`':
			return *m_pCur++;

		case '<':
			m_pCur++;
			if ( *m_pCur=='>' ) { m_pCur++; return TOK_NE; }
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_LTE; }
			return '<';

		case '>':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; return TOK_GTE; }
			return '>';

		case '=':
			m_pCur++;
			if ( *m_pCur=='=' ) m_pCur++;
			return TOK_EQ;

		// special case for leading dots (float values without leading zero, JSON key names, etc)
		case '.':
			{
				auto iBeg = (int)( m_pCur-m_sExpr+1 );
				bool bDigit = isdigit ( m_pCur[1] )!=0;

				// handle dots followed by a digit
				// aka, a float value without leading zero
				if ( bDigit )
				{
					char * pEnd = nullptr;
					auto fValue = (float) strtod ( m_pCur, &pEnd );
					lvalp->fConst = fValue;

					if ( pEnd && !sphIsAttr(*pEnd) )
						m_pCur = pEnd;
					else // fallback to subkey (e.g. ".1234a")
						bDigit = false;
				}

				// handle dots followed by a non-digit
				// for cases like jsoncol.keyname
				if ( !bDigit )
				{
					m_pCur++;
					while ( isspace ( *m_pCur ) )
						m_pCur++;
					iBeg = (int)( m_pCur-m_sExpr );
					while ( sphIsAttr(*m_pCur) )
						m_pCur++;
				}

				// return packed string after the dot
				int iLen = (int)( m_pCur-m_sExpr ) - iBeg;
				lvalp->iConst = ( int64_t(iBeg)<<32 ) + iLen;
				return bDigit ? TOK_DOT_NUMBER : TOK_SUBKEY;
			}

		case '\'':
		case '"':
			{
				const char cEnd = *m_pCur;
				for ( const char * s = m_pCur+1; *s; s++ )
				{
					if ( *s==cEnd )
					{
						auto iBeg = (int)( m_pCur-m_sExpr );
						int iLen = (int)( s-m_sExpr ) - iBeg + 1;
						lvalp->iConst = ( int64_t(iBeg)<<32 ) + iLen;
						m_pCur = s+1;
						return TOK_CONST_STRING;

					} else if ( *s=='\\' )
					{
						s++;
						if ( !*s )
							break;
					}
				}
				m_sLexerError.SetSprintf ( "unterminated string constant near '%s'", m_pCur );
				return -1;
			}
	}

	m_sLexerError.SetSprintf ( "unknown operator '%c' near '%s'", *m_pCur, m_pCur );
	return -1;
}

/// is add/sub?
static inline bool IsAddSub ( const ExprNode_t * pNode )
{
	if ( pNode )
		return pNode->m_iToken=='+' || pNode->m_iToken=='-';
	assert ( 0 && "null node passed to IsAddSub()" );
	return false;
}

/// is unary operator?
static inline bool IsUnary ( const ExprNode_t * pNode )
{
	if ( pNode )
		return pNode->m_iToken==TOK_NEG || pNode->m_iToken==TOK_NOT;
	assert ( 0 && "null node passed to IsUnary() ");
	return false;
}

/// is arithmetic?
static inline bool IsAri ( const ExprNode_t * pNode )
{
	if ( pNode )
	{
		int iTok = pNode->m_iToken;
		return iTok=='+' || iTok=='-' || iTok=='*' || iTok=='/';
	}
	assert ( 0 && "null node passed to IsAri()" );
	return false;
}

/// is constant?
static inline bool IsConst ( const ExprNode_t * pNode )
{
    if ( pNode )
		return pNode->m_iToken==TOK_CONST_INT || pNode->m_iToken==TOK_CONST_FLOAT;
	assert ( 0 && "null node passed to IsConst()" );
	return false;
}

/// float value of a constant
static inline float FloatVal ( const ExprNode_t * pNode )
{
	assert ( IsConst(pNode) );
	return pNode->m_iToken==TOK_CONST_INT
		? (float)pNode->m_iConst
		: pNode->m_fConst;
}

void ExprParser_t::CanonizePass ( int iNode )
{
	if ( iNode<0 )
		return;

	CanonizePass ( m_dNodes [ iNode ].m_iLeft );
	CanonizePass ( m_dNodes [ iNode ].m_iRight );

	ExprNode_t * pRoot = &m_dNodes [ iNode ];
	ExprNode_t * pLeft = ( pRoot->m_iLeft>=0 ) ? &m_dNodes [ pRoot->m_iLeft ] : nullptr;
	ExprNode_t * pRight = ( pRoot->m_iRight>=0 ) ? &m_dNodes [ pRoot->m_iRight ] : nullptr;

	// canonize (expr op const), move const to the left
	if ( pLeft && pRight && IsAri ( pRoot ) && !IsConst ( pLeft ) && IsConst ( pRight ) )
	{
		Swap ( pRoot->m_iLeft, pRoot->m_iRight );
		Swap ( pLeft, pRight );

		// fixup (expr-const) to ((-const)+expr)
		if ( pRoot->m_iToken=='-' )
		{
			pRoot->m_iToken = '+';
			if ( pLeft->m_iToken==TOK_CONST_INT )
				pLeft->m_iConst *= -1;
			else
				pLeft->m_fConst *= -1;
		}

		// fixup (expr/const) to ((1/const)*expr)
		if ( pRoot->m_iToken=='/' )
		{
			pRoot->m_iToken = '*';
			pLeft->m_fConst = 1.0f / FloatVal ( pLeft );
			pLeft->m_iToken = TOK_CONST_FLOAT;
		}
	}

	// promote children constants
	if ( pLeft && IsAri ( pRoot ) && IsAri ( pLeft ) && IsAddSub ( pLeft )==IsAddSub ( pRoot ) &&
		IsConst ( &m_dNodes [ pLeft->m_iLeft ] ) )
	{
		// ((const op lr) op2 right) gets replaced with (const op (lr op2/op right))
		// constant gets promoted one level up
		int iConst = pLeft->m_iLeft;
		pLeft->m_iLeft = pLeft->m_iRight;
		pLeft->m_iRight = pRoot->m_iRight; // (c op lr) -> (lr ... r)

		switch ( pLeft->m_iToken )
		{
		case '+':
		case '*':
			// (c + lr) op r -> c + (lr op r)
			// (c * lr) op r -> c * (lr op r)
			Swap ( pLeft->m_iToken, pRoot->m_iToken );
			break;

		case '-':
			// (c - lr) + r -> c - (lr - r)
			// (c - lr) - r -> c - (lr + r)
			pLeft->m_iToken = ( pRoot->m_iToken=='+' ? '-' : '+' );
			pRoot->m_iToken = '-';
			break;

		case '/':
			// (c / lr) * r -> c * (r / lr)
			// (c / lr) / r -> c / (r * lr)
			Swap ( pLeft->m_iLeft, pLeft->m_iRight );
			pLeft->m_iToken = ( pRoot->m_iToken=='*' ) ? '/' : '*';
			break;

		default:
			assert ( 0 && "internal error: unhandled op in left-const promotion" );
		}

		pRoot->m_iRight = pRoot->m_iLeft;
		pRoot->m_iLeft = iConst;
	}

	// MySQL Workbench fixup
	if ( pRoot->m_iToken==TOK_FUNC && ( pRoot->m_iFunc==FUNC_CURRENT_USER || pRoot->m_iFunc==FUNC_CONNECTION_ID ) )
	{
		pRoot->m_iToken = TOK_CONST_INT;
		pRoot->m_iConst = 0;
		return;
	}
}

void ExprParser_t::ConstantFoldPass ( int iNode )
{
	if ( iNode<0 )
		return;

	ConstantFoldPass ( m_dNodes [ iNode ].m_iLeft );
	ConstantFoldPass ( m_dNodes [ iNode ].m_iRight );

	ExprNode_t * pRoot = &m_dNodes [ iNode ];
	ExprNode_t * pLeft = ( pRoot->m_iLeft>=0 ) ? &m_dNodes [ pRoot->m_iLeft ] : nullptr;
	ExprNode_t * pRight = ( pRoot->m_iRight>=0 ) ? &m_dNodes [ pRoot->m_iRight ] : nullptr;

	// unary arithmetic expression with constant
	if ( IsUnary ( pRoot ) && pLeft && IsConst ( pLeft ) )
	{
		if ( pLeft->m_iToken==TOK_CONST_INT )
		{
			switch ( pRoot->m_iToken )
			{
				case TOK_NEG:	pRoot->m_iConst = -pLeft->m_iConst; break;
				case TOK_NOT:	pRoot->m_iConst = !pLeft->m_iConst; break;
				default:		assert ( 0 && "internal error: unhandled arithmetic token during const-int optimization" );
			}

		} else
		{
			switch ( pRoot->m_iToken )
			{
				case TOK_NEG:	pRoot->m_fConst = -pLeft->m_fConst; break;
				case TOK_NOT:	pRoot->m_fConst = !pLeft->m_fConst; break;
				default:		assert ( 0 && "internal error: unhandled arithmetic token during const-float optimization" );
			}
		}

		pRoot->m_iToken = pLeft->m_iToken;
		pRoot->m_iLeft = -1;
		return;
	}

	// arithmetic expression with constants
	if ( IsAri ( pRoot ) )
	{
		assert ( pLeft && pRight );

		// optimize fully-constant expressions
		if ( IsConst ( pLeft ) && IsConst ( pRight ) )
		{
			if ( pLeft->m_iToken==TOK_CONST_INT && pRight->m_iToken==TOK_CONST_INT && pRoot->m_iToken!='/' )
			{
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_iConst = pLeft->m_iConst + pRight->m_iConst; break;
					case '-':	pRoot->m_iConst = pLeft->m_iConst - pRight->m_iConst; break;
					case '*':	pRoot->m_iConst = pLeft->m_iConst * pRight->m_iConst; break;
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-int optimization" );
				}
				pRoot->m_iToken = TOK_CONST_INT;

			} else
			{
				float fLeft = FloatVal ( pLeft );
				float fRight = FloatVal ( pRight );
				switch ( pRoot->m_iToken )
				{
					case '+':	pRoot->m_fConst = fLeft + fRight; break;
					case '-':	pRoot->m_fConst = fLeft - fRight; break;
					case '*':	pRoot->m_fConst = fLeft * fRight; break;
					case '/':	pRoot->m_fConst = fRight ? fLeft / fRight : 0.0f; break; // FIXME! We don't have 'NULL', cant distinguish from 0.0f
					default:	assert ( 0 && "internal error: unhandled arithmetic token during const-float optimization" );
				}
				pRoot->m_iToken = TOK_CONST_FLOAT;
			}
			pRoot->m_iLeft = -1;
			pRoot->m_iRight = -1;
			return;
		}

		if ( IsConst ( pLeft ) && IsAri ( pRight ) && IsAddSub ( pRoot )==IsAddSub ( pRight ) &&
			IsConst ( &m_dNodes[pRight->m_iLeft] ) )
		{
			ExprNode_t * pConst = &m_dNodes[pRight->m_iLeft];
			assert ( !IsConst ( &m_dNodes [ pRight->m_iRight ] ) ); // must had been optimized

			// optimize (left op (const op2 expr)) to ((left op const) op*op2 expr)
			if ( IsAddSub ( pRoot ) )
			{
				// fold consts
				int iSign = ( ( pRoot->m_iToken=='+' ) ? 1 : -1 );
				if ( pLeft->m_iToken==TOK_CONST_INT && pConst->m_iToken==TOK_CONST_INT )
				{
					pLeft->m_iConst += iSign*pConst->m_iConst;
				} else
				{
					pLeft->m_fConst = FloatVal ( pLeft ) + iSign*FloatVal ( pConst );
					pLeft->m_iToken = TOK_CONST_FLOAT;
				}

				// fold ops
				pRoot->m_iToken = ( pRoot->m_iToken==pRight->m_iToken ) ? '+' : '-';

			} else
			{
				// fold consts
				if ( pRoot->m_iToken=='*' && pLeft->m_iToken==TOK_CONST_INT && pConst->m_iToken==TOK_CONST_INT )
				{
					pLeft->m_iConst *= pConst->m_iConst;
				} else
				{
					if ( pRoot->m_iToken=='*' )
						pLeft->m_fConst = FloatVal ( pLeft ) * FloatVal ( pConst );
					else
						pLeft->m_fConst = FloatVal ( pLeft ) / FloatVal ( pConst );
					pLeft->m_iToken = TOK_CONST_FLOAT;
				}

				// fold ops
				pRoot->m_iToken = ( pRoot->m_iToken==pRight->m_iToken ) ? '*' : '/';
			}

			// promote expr arg
			pRoot->m_iRight = pRight->m_iRight;
		}
	}

	// unary function from a constant
	if ( pRoot->m_iToken==TOK_FUNC && g_dFuncs [ pRoot->m_iFunc ].m_iArgs==1 && IsConst ( pLeft ) )
	{
		float fArg = pLeft->m_iToken==TOK_CONST_FLOAT ? pLeft->m_fConst : float ( pLeft->m_iConst );
		switch ( pRoot->m_iFunc )
		{
		case FUNC_ABS:
			pRoot->m_iToken = pLeft->m_iToken;
			pRoot->m_iLeft = -1;
			if ( pLeft->m_iToken==TOK_CONST_INT )
				pRoot->m_iConst = IABS ( pLeft->m_iConst );
			else
				pRoot->m_fConst = (float)fabs ( fArg );
			break;
		case FUNC_CEIL:		pRoot->m_iToken = TOK_CONST_INT; pRoot->m_iLeft = -1; pRoot->m_iConst = (int64_t)ceil ( fArg ); break;
		case FUNC_FLOOR:	pRoot->m_iToken = TOK_CONST_INT; pRoot->m_iLeft = -1; pRoot->m_iConst = (int64_t)floor ( fArg ); break;
		case FUNC_SIN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = float ( sin ( fArg) ); break;
		case FUNC_COS:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = float ( cos ( fArg ) ); break;
		case FUNC_LN:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = fArg>0.0f ? (float) log(fArg) : 0.0f; break;
		case FUNC_LOG2:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = fArg>0.0f ? (float)( log(fArg)*M_LOG2E ) : 0.0f; break;
		case FUNC_LOG10:	pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = fArg>0.0f ? (float)( log(fArg)*M_LOG10E ) : 0.0f; break;
		case FUNC_EXP:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = float ( exp ( fArg ) ); break;
		case FUNC_SQRT:		pRoot->m_iToken = TOK_CONST_FLOAT; pRoot->m_iLeft = -1; pRoot->m_fConst = fArg>0.0f ? (float)sqrt(fArg) : 0.0f; break;
		default:			break;
		}
		return;
	}
}

void ExprParser_t::VariousOptimizationsPass ( int iNode )
{
	if ( iNode<0 )
		return;

	VariousOptimizationsPass ( m_dNodes [ iNode ].m_iLeft );
	VariousOptimizationsPass ( m_dNodes [ iNode ].m_iRight );

	ExprNode_t * pRoot = &m_dNodes [ iNode ];
	int iLeft = pRoot->m_iLeft;
	int iRight = pRoot->m_iRight;
	ExprNode_t * pLeft = ( iLeft>=0 ) ? &m_dNodes [ iLeft ] : nullptr;
	ExprNode_t * pRight = ( iRight>=0 ) ? &m_dNodes [ iRight ] : nullptr;

	// madd, mul3
	// FIXME! separate pass for these? otherwise (2+(a*b))+3 won't get const folding
	if ( ( pRoot->m_iToken=='+' || pRoot->m_iToken=='*' ) && pLeft && pRight && ( pLeft->m_iToken=='*' || pRight->m_iToken=='*' ) )
	{
		if ( pLeft->m_iToken!='*' )
		{
			Swap ( pRoot->m_iLeft, pRoot->m_iRight );
			Swap ( pLeft, pRight );
			Swap ( iLeft, iRight );
		}

		pLeft->m_iToken = ',';
		pRoot->m_iFunc = ( pRoot->m_iToken=='+' ) ? FUNC_MADD : FUNC_MUL3;
		pRoot->m_iToken = TOK_FUNC;
		pRoot->m_iLeft = m_dNodes.GetLength();
		pRoot->m_iRight = -1;

		ExprNode_t & tArgs = m_dNodes.Add(); // invalidates all pointers!
		tArgs.m_iToken = ',';
		tArgs.m_iLeft = iLeft;
		tArgs.m_iRight = iRight;
		return;
	}

	// division by a constant (replace with multiplication by inverse)
	if ( pRoot->m_iToken=='/' && pRight && pRight->m_iToken==TOK_CONST_FLOAT )
	{
		pRight->m_fConst = 1.0f / pRight->m_fConst;
		pRoot->m_iToken = '*';
		return;
	}


	// SINT(int-attr)
	if ( pRoot->m_iToken==TOK_FUNC && pRoot->m_iFunc==FUNC_SINT && pLeft )
	{
		if ( pLeft->m_iToken==TOK_ATTR_INT || pLeft->m_iToken==TOK_ATTR_BITS )
		{
			pRoot->m_iToken = TOK_ATTR_SINT;
			pRoot->m_tLocator = pLeft->m_tLocator;
			pRoot->m_iLeft = -1;
		}
	}
}

/// optimize subtree
void ExprParser_t::Optimize ( int iNode )
{
	// fixme! m.b. iteratively repeat while something changes?
	CanonizePass ( iNode );
	ConstantFoldPass ( iNode );
	VariousOptimizationsPass ( iNode );
}


// debug dump
void ExprParser_t::Dump ( int iNode )
{
	if ( iNode<0 )
		return;

	ExprNode_t & tNode = m_dNodes[iNode];
	switch ( tNode.m_iToken )
	{
		case TOK_CONST_INT:
			printf ( INT64_FMT, tNode.m_iConst );
			break;

		case TOK_CONST_FLOAT:
			printf ( "%f", tNode.m_fConst );
			break;

		case TOK_ATTR_INT:
		case TOK_ATTR_SINT:
			printf ( "row[%d]", tNode.m_tLocator.m_iBitOffset/32 );
			break;

		default:
			printf ( "(" );
			Dump ( tNode.m_iLeft );
			printf ( ( tNode.m_iToken<256 ) ? " %c " : " op-%d ", tNode.m_iToken );
			Dump ( tNode.m_iRight );
			printf ( ")" );
			break;
	}
}


/// fold arglist into array
/// moves also ownership (so, 1-st param owned by dArgs on exit)
static void MoveToArgList ( ISphExpr * pLeft, VecRefPtrs_t<ISphExpr *> &dArgs )
{
	if ( !pLeft || !pLeft->IsArglist ())
	{
		dArgs.Add ( pLeft );
		return;
	}

	// do we have to pArgs->AddArgs instead?
	auto * pArgs = (Expr_Arglist_c *)pLeft;
	if ( dArgs.IsEmpty () )
		dArgs.SwapData ( pArgs->m_dArgs );
	else {
		dArgs.Append ( pArgs->m_dArgs );
		pArgs->m_dArgs.Reset();
	}
	SafeRelease ( pArgs );
}


using UdfInt_fn = sphinx_int64_t ( * ) ( SPH_UDF_INIT *, SPH_UDF_ARGS *, char * );
using UdfDouble_fn = double ( * ) ( SPH_UDF_INIT *, SPH_UDF_ARGS *, char * );
using UdfCharptr_fn = char * ( * ) ( SPH_UDF_INIT *, SPH_UDF_ARGS *, char * );

class Expr_Udf_c : public ISphExpr
{
protected:
	VecRefPtrs_t<ISphExpr*>			m_dArgs;
	CSphVector<int>					m_dArgs2Free;

	UdfCall_t *						m_pCall;
	mutable CSphVector<int64_t>		m_dArgvals;
	mutable char					m_bError = 0;
	CSphQueryProfile *				m_pProfiler;
	const BYTE *					m_pStrings = nullptr;

public:
	explicit Expr_Udf_c ( UdfCall_t * pCall, CSphQueryProfile * pProfiler )
		: m_pCall ( pCall )
		, m_pProfiler ( pProfiler )
	{
		SPH_UDF_ARGS & tArgs = m_pCall->m_tArgs;

		assert ( tArgs.arg_values==nullptr );
		tArgs.arg_values = new char * [ tArgs.arg_count ];
		tArgs.str_lengths = new int [ tArgs.arg_count ];

		m_dArgs2Free = pCall->m_dArgs2Free;
		m_dArgvals.Resize ( tArgs.arg_count );
		ARRAY_FOREACH ( i, m_dArgvals )
			tArgs.arg_values[i] = (char*) &m_dArgvals[i];
	}

	~Expr_Udf_c () override
	{
		if ( m_pCall->m_pUdf->m_fnDeinit )
			m_pCall->m_pUdf->m_fnDeinit ( &m_pCall->m_tInit );
		SafeDelete ( m_pCall );
	}

	void FillArgs ( const CSphMatch & tMatch ) const
	{
		int64_t iPacked = 0;
		ESphJsonType eJson = JSON_NULL;
		DWORD uOff = 0;
		CSphVector<BYTE> dTmp;

		// FIXME? a cleaner way to reinterpret?
		SPH_UDF_ARGS & tArgs = m_pCall->m_tArgs;
		ARRAY_FOREACH ( i, m_dArgs )
		{
			switch ( tArgs.arg_types[i] )
			{
			case SPH_UDF_TYPE_UINT32:		*(DWORD*)&m_dArgvals[i] = m_dArgs[i]->IntEval ( tMatch ); break;
			case SPH_UDF_TYPE_INT64:		m_dArgvals[i] = m_dArgs[i]->Int64Eval ( tMatch ); break;
			case SPH_UDF_TYPE_FLOAT:		*(float*)&m_dArgvals[i] = m_dArgs[i]->Eval ( tMatch ); break;
			case SPH_UDF_TYPE_STRING:		tArgs.str_lengths[i] = m_dArgs[i]->StringEval ( tMatch, (const BYTE**)&tArgs.arg_values[i] ); break;
			case SPH_UDF_TYPE_UINT32SET:	tArgs.arg_values[i] = (char*) m_dArgs[i]->MvaEval ( tMatch ); break;
			case SPH_UDF_TYPE_UINT64SET:	tArgs.arg_values[i] = (char*) m_dArgs[i]->MvaEval ( tMatch ); break;
			case SPH_UDF_TYPE_FACTORS:
				{
					tArgs.arg_values[i] = (char *)m_dArgs[i]->FactorEval ( tMatch );
					m_pCall->m_dArgs2Free.Add(i);
				}
				break;

			case SPH_UDF_TYPE_JSON:
				iPacked = m_dArgs[i]->Int64Eval ( tMatch );
				eJson = ESphJsonType ( iPacked>>32 );
				uOff = (DWORD)iPacked;
				if ( !uOff || eJson==JSON_NULL )
				{
					tArgs.arg_values[i] = nullptr;
					tArgs.str_lengths[i] = 0;
				} else
				{
					JsonEscapedBuilder sTmp;
					sphJsonFieldFormat ( sTmp, m_pStrings+uOff, eJson, false );
					tArgs.str_lengths[i] = sTmp.GetLength();
					tArgs.arg_values[i] = (char*) sTmp.Leak();
				}
			break;

			default:						assert ( 0 ); m_dArgvals[i] = 0; break;
			}
		}
	}

	void FreeArgs() const
	{
		for ( int iAttr : m_dArgs2Free )
			SafeDeleteArray ( m_pCall->m_tArgs.arg_values[iAttr] );
	}

	void AdoptArgs ( ISphExpr * pArglist )
	{
		MoveToArgList ( pArglist, m_dArgs );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		for ( auto i : m_dArgs )
			i->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( eCmd==SPH_EXPR_GET_UDF )
		{
			*((bool*)pArg) = true;
			return;
		}
		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
		for ( auto& pExpr : m_dArgs )
			pExpr->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & bDisable ) override
	{
		bDisable = true;
		return 0;
	}
};


class Expr_UdfInt_c : public Expr_Udf_c
{
public:
	explicit Expr_UdfInt_c ( UdfCall_t * pCall, CSphQueryProfile * pProfiler )
		: Expr_Udf_c ( pCall, pProfiler )
	{
		assert ( IsInt ( pCall->m_pUdf->m_eRetType ) );
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		if ( m_bError )
			return 0;

		CSphScopedProfile tProf ( m_pProfiler, SPH_QSTATE_EVAL_UDF );
		FillArgs ( tMatch );
		auto pFn = (UdfInt_fn) m_pCall->m_pUdf->m_fnFunc;
		auto iRes = (int64_t) pFn ( &m_pCall->m_tInit, &m_pCall->m_tArgs, &m_bError );
		FreeArgs();
		return iRes;
	}

	int IntEval ( const CSphMatch & tMatch ) const final { return (int) Int64Eval ( tMatch ); }
	float Eval ( const CSphMatch & tMatch ) const final { return (float) Int64Eval ( tMatch ); }
};


class Expr_UdfFloat_c : public Expr_Udf_c
{
public:
	explicit Expr_UdfFloat_c ( UdfCall_t * pCall, CSphQueryProfile * pProfiler )
		: Expr_Udf_c ( pCall, pProfiler )
	{
		assert ( pCall->m_pUdf->m_eRetType==SPH_ATTR_FLOAT );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		if ( m_bError )
			return 0;

		CSphScopedProfile tProf ( m_pProfiler, SPH_QSTATE_EVAL_UDF );
		FillArgs ( tMatch );
		auto pFn = (UdfDouble_fn) m_pCall->m_pUdf->m_fnFunc;
		auto fRes = (float) pFn ( &m_pCall->m_tInit, &m_pCall->m_tArgs, &m_bError );
		FreeArgs();
		return fRes;
	}

	int IntEval ( const CSphMatch & tMatch ) const final { return (int) Eval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const final { return (int64_t) Eval ( tMatch ); }
};


class Expr_UdfStringptr_c : public Expr_Udf_c
{
public:
	explicit Expr_UdfStringptr_c ( UdfCall_t * pCall, CSphQueryProfile * pProfiler )
		: Expr_Udf_c ( pCall, pProfiler )
	{
		assert ( pCall->m_pUdf->m_eRetType==SPH_ATTR_STRINGPTR );
	}

	float Eval ( const CSphMatch & ) const final
	{
		assert ( 0 && "internal error: stringptr udf evaluated as float" );
		return 0.0f;
	}

	int IntEval ( const CSphMatch & ) const final
	{
		assert ( 0 && "internal error: stringptr udf evaluated as int" );
		return 0;
	}

	int64_t Int64Eval ( const CSphMatch & ) const final
	{
		assert ( 0 && "internal error: stringptr udf evaluated as bigint" );
		return 0;
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		if ( m_bError )
		{
			*ppStr = NULL;
			return 0;
		}

		CSphScopedProfile tProf ( m_pProfiler, SPH_QSTATE_EVAL_UDF );
		FillArgs ( tMatch );
		auto pFn = (UdfCharptr_fn) m_pCall->m_pUdf->m_fnFunc;
		char * pRes = pFn ( &m_pCall->m_tInit, &m_pCall->m_tArgs, &m_bError ); // owned now!
		*ppStr = (const BYTE*) pRes;
		int iLen = ( pRes ? strlen(pRes) : 0 );
		FreeArgs();
		return iLen;
	}

	bool IsDataPtrAttr() const final
	{
		return true;
	}
};


ISphExpr * ExprParser_t::CreateUdfNode ( int iCall, ISphExpr * pLeft )
{
	Expr_Udf_c * pRes = nullptr;
	switch ( m_dUdfCalls[iCall]->m_pUdf->m_eRetType )
	{
		case SPH_ATTR_INTEGER:
		case SPH_ATTR_BIGINT:
			pRes = new Expr_UdfInt_c ( m_dUdfCalls[iCall], m_pProfiler );
			break;
		case SPH_ATTR_FLOAT:
			pRes = new Expr_UdfFloat_c ( m_dUdfCalls[iCall], m_pProfiler );
			break;
		case SPH_ATTR_STRINGPTR:
			pRes = new Expr_UdfStringptr_c ( m_dUdfCalls[iCall], m_pProfiler );
			break;
		default:
			m_sCreateError.SetSprintf ( "internal error: unhandled type %d in CreateUdfNode()", m_dUdfCalls[iCall]->m_pUdf->m_eRetType );
			break;
	}
	if ( pRes )
	{
		SafeAddRef ( pLeft );
		if ( pLeft )
			pRes->AdoptArgs ( pLeft );
		m_dUdfCalls[iCall] = nullptr; // evaluator owns it now
	}
	return pRes;
}


ISphExpr * ExprParser_t::CreateExistNode ( const ExprNode_t & tNode )
{
	assert ( m_dNodes[tNode.m_iLeft].m_iToken==',' );
	int iAttrName = m_dNodes[tNode.m_iLeft].m_iLeft;
	int iAttrDefault = m_dNodes[tNode.m_iLeft].m_iRight;
	assert ( iAttrName>=0 && iAttrName<m_dNodes.GetLength()
		&& iAttrDefault>=0 && iAttrDefault<m_dNodes.GetLength() );

	auto iNameStart = (int)( m_dNodes[iAttrName].m_iConst>>32 );
	auto iNameLen = (int)( m_dNodes[iAttrName].m_iConst & 0xffffffffUL );
	// skip head and tail non attribute name symbols
	while ( m_sExpr[iNameStart]!='\0' && ( m_sExpr[iNameStart]=='\'' || m_sExpr[iNameStart]==' ' ) && iNameLen )
	{
		iNameStart++;
		iNameLen--;
	}
	while ( m_sExpr[iNameStart+iNameLen-1]!='\0'
		&& ( m_sExpr[iNameStart+iNameLen-1]=='\'' || m_sExpr[iNameStart+iNameLen-1]==' ' )
		&& iNameLen )
	{
		iNameLen--;
	}

	if ( iNameLen<=0 )
	{
		m_sCreateError.SetSprintf ( "first EXIST() argument must be valid string" );
		return nullptr;
	}

	assert ( iNameStart>=0 && iNameLen>0 && iNameStart+iNameLen<=(int)strlen ( m_sExpr ) );

	CSphString sAttr ( m_sExpr+iNameStart, iNameLen );
	sphColumnToLowercase ( const_cast<char *>( sAttr.cstr() ) );
	int iLoc = m_pSchema->GetAttrIndex ( sAttr.cstr() );

	if ( iLoc>=0 )
	{
		const CSphColumnInfo & tCol = m_pSchema->GetAttr ( iLoc );
		if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET || tCol.m_eAttrType==SPH_ATTR_STRING )
		{
			m_sCreateError = "MVA and STRING in EXIST() prohibited";
			return nullptr;
		}

		const CSphAttrLocator & tLoc = tCol.m_tLocator;
		if ( tNode.m_eRetType==SPH_ATTR_FLOAT )
			return new Expr_GetFloat_c ( tLoc, iLoc );
		else
			return new Expr_GetInt_c ( tLoc, iLoc );
	} else
	{
		if ( tNode.m_eRetType==SPH_ATTR_INTEGER )
			return new Expr_GetIntConst_c ( (int)m_dNodes[iAttrDefault].m_iConst );
		else if ( tNode.m_eRetType==SPH_ATTR_BIGINT )
			return new Expr_GetInt64Const_c ( m_dNodes[iAttrDefault].m_iConst );
		else
			return new Expr_GetConst_c ( m_dNodes[iAttrDefault].m_fConst );
	}
}

//////////////////////////////////////////////////////////////////////////

class Expr_Contains_c : public ISphExpr
{
protected:
	CSphRefcountedPtr<ISphExpr>	m_pLat;
	CSphRefcountedPtr<ISphExpr>	m_pLon;

	static bool Contains ( float x, float y, int n, const float * p )
	{
		bool bIn = false;
		for ( int ii=0; ii<n; ii+=2 )
		{
			// get that edge
			float ax = p[ii];
			float ay = p[ii+1];
			float bx = ( ii==n-2 ) ? p[0] : p[ii+2];
			float by = ( ii==n-2 ) ? p[1] : p[ii+3];

			// check point vs edge
			float t1 = (x-ax)*(by-ay);
			float t2 = (y-ay)*(bx-ax);
			if ( t1==t2 && !( ax==bx && ay==by ) )
			{
				// so AP and AB are colinear
				// because (AP dot (-AB.y, AB.x)) aka (t1-t2) is 0
				// check (AP dot AB) vs (AB dot AB) then
				float t3 = (x-ax)*(bx-ax) + (y-ay)*(by-ay); // AP dot AP
				float t4 = (bx-ax)*(bx-ax) + (by-ay)*(by-ay); // AB dot AB
				if ( t3>=0 && t3<=t4 )
					return true;
			}

			// count edge crossings
			if ( ( ay>y )!=(by>y) )
				if ( ( t1<t2 ) ^ ( by<ay ) )
					bIn = !bIn;
		}
		return bIn;
	}

public:
	Expr_Contains_c ( ISphExpr * pLat, ISphExpr * pLon )
		: m_pLat ( pLat )
		, m_pLon ( pLon )
	{
		SafeAddRef ( pLat );
		SafeAddRef ( pLon );
	}

	float Eval ( const CSphMatch & tMatch ) const override
	{
		return (float)IntEval ( tMatch );
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const override
	{
		return IntEval ( tMatch );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		m_pLat->FixupLocator ( pOldSchema, pNewSchema );
		m_pLon->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		m_pLat->Command ( eCmd, pArg );
		m_pLon->Command ( eCmd, pArg );
	}

protected:
	uint64_t CalcHash ( const char * szTag, const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable )
	{
		EXPR_CLASS_NAME_NOCHECK(szTag);
		CALC_CHILD_HASH(m_pLat);
		CALC_CHILD_HASH(m_pLon);
		return CALC_DEP_HASHES();
	}

	// FIXME! implement SetStringPool?
};

//////////////////////////////////////////////////////////////////////////
// GEODISTANCE
//////////////////////////////////////////////////////////////////////////

// conversions between degrees and radians
static const double PI = 3.14159265358979323846;
static const double TO_RAD = PI / 180.0;
static const double TO_RAD2 = PI / 360.0;
static const double TO_DEG = 180.0 / PI;
static const float TO_RADF = (float)( PI / 180.0 );
static const float TO_RADF2 = (float)( PI / 360.0 );
static const float TO_DEGF = (float)( 180.0 / PI );

const int GEODIST_TABLE_COS		= 1024; // maxerr 0.00063%
const int GEODIST_TABLE_ASIN	= 512;
const int GEODIST_TABLE_K		= 1024;

static float g_GeoCos[GEODIST_TABLE_COS+1];		///< cos(x) table
static float g_GeoAsin[GEODIST_TABLE_ASIN+1];	///< asin(sqrt(x)) table
static float g_GeoFlatK[GEODIST_TABLE_K+1][2];	///< GeodistAdaptive() flat ellipsoid method k1,k2 coeffs table


void GeodistInit()
{
	for ( int i=0; i<=GEODIST_TABLE_COS; i++ )
		g_GeoCos[i] = (float)cos ( 2*PI*i/GEODIST_TABLE_COS ); // [0, 2pi] -> [0, COSTABLE]

	for ( int i=0; i<=GEODIST_TABLE_ASIN; i++ )
		g_GeoAsin[i] = (float)asin ( sqrt ( double(i)/GEODIST_TABLE_ASIN ) ); // [0, 1] -> [0, ASINTABLE]

	for ( int i=0; i<=GEODIST_TABLE_K; i++ )
	{
		double x = PI*i/GEODIST_TABLE_K - PI*0.5; // [-pi/2, pi/2] -> [0, KTABLE]
		g_GeoFlatK[i][0] = (float) sqr ( 111132.09 - 566.05*cos ( 2*x ) + 1.20*cos ( 4*x ) );
		g_GeoFlatK[i][1] = (float) sqr ( 111415.13*cos(x) - 94.55*cos ( 3*x ) + 0.12*cos ( 5*x ) );
	}
}


inline float GeodistSphereRad ( float lat1, float lon1, float lat2, float lon2 )
{
	static const double D = 2*6384000;
	double dlat2 = 0.5*( lat1 - lat2 );
	double dlon2 = 0.5*( lon1 - lon2 );
	double a = sqr ( sin(dlat2) ) + cos(lat1)*cos(lat2)*sqr ( sin(dlon2) );
	double c = asin ( Min ( 1.0, sqrt(a) ) );
	return (float)(D*c);
}


inline float GeodistSphereDeg ( float lat1, float lon1, float lat2, float lon2 )
{
	static const double D = 2*6384000;
	double dlat2 = TO_RAD2*( lat1 - lat2 );
	double dlon2 = TO_RAD2*( lon1 - lon2 );
	double a = sqr ( sin(dlat2) ) + cos ( TO_RAD*lat1 )*cos ( TO_RAD*lat2 )*sqr ( sin(dlon2) );
	double c = asin ( Min ( 1.0, sqrt(a) ) );
	return (float)(D*c);
}


static inline float GeodistDegDiff ( float f )
{
	f = (float)fabs(f);
	while ( f>360 )
		f -= 360;
	if ( f>180 )
		f = 360-f;
	return f;
}


float GeodistFlatDeg ( float fLat1, float fLon1, float fLat2, float fLon2 )
{
	double c1 = cos ( TO_RAD2*( fLat1+fLat2 ) );
	double c2 = 2*c1*c1-1; // cos(2*t)
	double c3 = c1*(2*c2-1); // cos(3*t)
	double k1 = 111132.09 - 566.05*c2;
	double k2 = 111415.13*c1 - 94.55*c3;
	float dlat = GeodistDegDiff ( fLat1-fLat2 );
	float dlon = GeodistDegDiff ( fLon1-fLon2 );
	return (float)sqrt ( k1*k1*dlat*dlat + k2*k2*dlon*dlon );
}


static inline float GeodistFastCos ( float x )
{
	auto y = (float)(fabs(x)*GEODIST_TABLE_COS/PI/2);
	auto i = int(y);
	y -= i;
	i &= ( GEODIST_TABLE_COS-1 );
	return g_GeoCos[i] + ( g_GeoCos[i+1]-g_GeoCos[i] )*y;
}


static inline float GeodistFastSin ( float x )
{
	auto y = float(fabs(x)*GEODIST_TABLE_COS/PI/2);
	auto i = int(y);
	y -= i;
	i = ( i - GEODIST_TABLE_COS/4 ) & ( GEODIST_TABLE_COS-1 ); // cos(x-pi/2)=sin(x), costable/4=pi/2
	return g_GeoCos[i] + ( g_GeoCos[i+1]-g_GeoCos[i] )*y;
}


/// fast implementation of asin(sqrt(x))
/// max error in floats 0.00369%, in doubles 0.00072%
static inline float GeodistFastAsinSqrt ( float x )
{
	if ( x<0.122 )
	{
		// distance under 4546km, Taylor error under 0.00072%
		auto y = (float)sqrt(x);
		return y + x*y*0.166666666666666f + x*x*y*0.075f + x*x*x*y*0.044642857142857f;
	}
	if ( x<0.948 )
	{
		// distance under 17083km, 512-entry LUT error under 0.00072%
		x *= GEODIST_TABLE_ASIN;
		auto i = int(x);
		return g_GeoAsin[i] + ( g_GeoAsin[i+1] - g_GeoAsin[i] )*( x-i );
	}
	return (float)asin ( sqrt(x) ); // distance over 17083km, just compute honestly
}


inline float GeodistAdaptiveDeg ( float lat1, float lon1, float lat2, float lon2 )
{
	float dlat = GeodistDegDiff ( lat1-lat2 );
	float dlon = GeodistDegDiff ( lon1-lon2 );

	if ( dlon<13 )
	{
		// points are close enough; use flat ellipsoid model
		// interpolate sqr(k1), sqr(k2) coefficients using latitudes midpoint
		float m = ( lat1+lat2+180 )*GEODIST_TABLE_K/360; // [-90, 90] degrees -> [0, KTABLE] indexes
		auto i = int(m);
		i &= ( GEODIST_TABLE_K-1 );
		float kk1 = g_GeoFlatK[i][0] + ( g_GeoFlatK[i+1][0] - g_GeoFlatK[i][0] )*( m-i );
		float kk2 = g_GeoFlatK[i][1] + ( g_GeoFlatK[i+1][1] - g_GeoFlatK[i][1] )*( m-i );
		return (float)sqrt ( kk1*dlat*dlat + kk2*dlon*dlon );
	} else
	{
		// points too far away; use haversine
		static const float D = 2*6371000;
		float a = fsqr ( GeodistFastSin ( dlat*TO_RADF2 ) ) + GeodistFastCos ( lat1*TO_RADF ) * GeodistFastCos ( lat2*TO_RADF ) * fsqr ( GeodistFastSin ( dlon*TO_RADF2 ) );
		return (float)( D*GeodistFastAsinSqrt(a) );
	}
}


inline float GeodistAdaptiveRad ( float lat1, float lon1, float lat2, float lon2 )
{
	// cut-paste-optimize, maybe?
	return GeodistAdaptiveDeg ( lat1*TO_DEGF, lon1*TO_DEGF, lat2*TO_DEGF, lon2*TO_DEGF );
}


static inline void GeoTesselate ( CSphVector<float> & dIn )
{
	// 1 minute of latitude, max
	// (it varies from 1842.9 to 1861.57 at 0 to 90 respectively)
	static const float LAT_MINUTE = 1861.57f;

	// 1 minute of longitude in metres, at different latitudes
	static const float LON_MINUTE[] =
	{
		1855.32f, 1848.31f, 1827.32f, 1792.51f, // 0, 5, 10, 15
		1744.12f, 1682.50f, 1608.10f, 1521.47f, // 20, 25, 30, 35
		1423.23f, 1314.11f, 1194.93f, 1066.57f, // 40, 45, 50, 55
		930.00f, 786.26f, 636.44f, 481.70f, // 60, 65 70, 75
		323.22f, 162.24f, 0.0f // 80, 85, 90
	};

	// tesselation threshold
	// FIXME! make this configurable?
	static const float TESSELATE_TRESH = 500000.0f; // 500 km, error under 150m or 0.03%

	CSphVector<float> dOut;
	for ( int i=0; i<dIn.GetLength(); i+=2 )
	{
		// add the current vertex in any event
		dOut.Add ( dIn[i] );
		dOut.Add ( dIn[i+1] );

		// get edge lat/lon, convert to radians
		bool bLast = ( i==dIn.GetLength()-2 );
		float fLat1 = dIn[i];
		float fLon1 = dIn[i+1];
		float fLat2 = dIn [ bLast ? 0 : (i+2) ];
		float fLon2 = dIn [ bLast ? 1 : (i+3) ];

		// quick rough geodistance estimation
		float fMinLat = Min ( fLat1, fLat2 );
		auto iLatBand = (int) floor ( fabs ( fMinLat ) / 5.0f );
		iLatBand = iLatBand % 18;

		auto d = (float) (60.0f*( LAT_MINUTE*fabs ( fLat1-fLat2 ) + LON_MINUTE [ iLatBand ]*fabs ( fLon1-fLon2 ) ) );
		if ( d<=TESSELATE_TRESH )
			continue;

		// convert to radians
		// FIXME! make units configurable
		fLat1 *= TO_RADF;
		fLon1 *= TO_RADF;
		fLat2 *= TO_RADF;
		fLon2 *= TO_RADF;

		// compute precise geodistance
		d = GeodistSphereRad ( fLat1, fLon1, fLat2, fLon2 );
		if ( d<=TESSELATE_TRESH )
			continue;
		int iSegments = (int) ceil ( d / TESSELATE_TRESH );

		// compute arc distance
		// OPTIMIZE! maybe combine with CalcGeodist?
		d = (float)acos ( sin(fLat1)*sin(fLat2) + cos(fLat1)*cos(fLat2)*cos(fLon1-fLon2) );
		const auto isd = (float)(1.0f / sin(d));
		const auto clat1 = (float)cos(fLat1);
		const auto slat1 = (float)sin(fLat1);
		const auto clon1 = (float)cos(fLon1);
		const auto slon1 = (float)sin(fLon1);
		const auto clat2 = (float)cos(fLat2);
		const auto slat2 = (float)sin(fLat2);
		const auto clon2 = (float)cos(fLon2);
		const auto slon2 = (float)sin(fLon2);

		for ( int j=1; j<iSegments; j++ )
		{
			float f = float(j) / float(iSegments); // needed distance fraction
			float a = (float)sin ( (1-f)*d ) * isd;
			float b = (float)sin ( f*d ) * isd;
			float x = a*clat1*clon1 + b*clat2*clon2;
			float y = a*clat1*slon1 + b*clat2*slon2;
			float z = a*slat1 + b*slat2;
			dOut.Add ( (float)( TO_DEG * atan2 ( z, sqrt ( x*x+y*y ) ) ) );
			dOut.Add ( (float)( TO_DEG * atan2 ( y, x ) ) );
		}
	}

	// swap 'em results
	dIn.SwapData ( dOut );
}

//////////////////////////////////////////////////////////////////////////

class Expr_ContainsConstvec_c : public Expr_Contains_c
{
protected:
	CSphVector<float> m_dPoly;
	float m_fMinX;
	float m_fMinY;
	float m_fMaxX;
	float m_fMaxY;

public:
	Expr_ContainsConstvec_c ( ISphExpr * pLat, ISphExpr * pLon, const CSphVector<int> & dNodes, const ExprNode_t * pNodes, bool bGeoTesselate )
		: Expr_Contains_c ( pLat, pLon )
	{
		// copy polygon data
		assert ( dNodes.GetLength()>=6 );
		m_dPoly.Resize ( dNodes.GetLength() );

		ARRAY_FOREACH ( i, dNodes )
			m_dPoly[i] = FloatVal ( &pNodes[dNodes[i]] );

		// handle (huge) geosphere polygons
		if ( bGeoTesselate )
			GeoTesselate ( m_dPoly );

		// compute bbox
		m_fMinX = m_fMaxX = m_dPoly[0];
		for ( int i=2; i<m_dPoly.GetLength(); i+=2 )
		{
			m_fMinX = Min ( m_fMinX, m_dPoly[i] );
			m_fMaxX = Max ( m_fMaxX, m_dPoly[i] );
		}

		m_fMinY = m_fMaxY = m_dPoly[1];
		for ( int i=3; i<m_dPoly.GetLength(); i+=2 )
		{
			m_fMinY = Min ( m_fMinY, m_dPoly[i] );
			m_fMaxY = Max ( m_fMaxY, m_dPoly[i] );
		}
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		// eval args, do bbox check
		float fLat = m_pLat->Eval(tMatch);
		if ( fLat<m_fMinX || fLat>m_fMaxX )
			return 0;

		float fLon = m_pLon->Eval(tMatch);
		if ( fLon<m_fMinY || fLon>m_fMaxY )
			return 0;

		// do the polygon check
		return Contains ( fLat, fLon, m_dPoly.GetLength(), m_dPoly.Begin() );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_ContainsConstvec_c");
		CALC_POD_HASHES(m_dPoly);
		return CALC_PARENT_HASH();
	}
};


class Expr_ContainsExprvec_c : public Expr_Contains_c
{
protected:
	mutable CSphVector<float> m_dPoly;
	VecRefPtrs_t<ISphExpr*> m_dExpr;

public:
	Expr_ContainsExprvec_c ( ISphExpr * pLat, ISphExpr * pLon, CSphVector<ISphExpr*> & dExprs )
		: Expr_Contains_c ( pLat, pLon )
	{
		m_dExpr.SwapData ( dExprs );
		m_dPoly.Resize ( m_dExpr.GetLength() );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		ARRAY_FOREACH ( i, m_dExpr )
			m_dPoly[i] = m_dExpr[i]->Eval ( tMatch );
		return Contains ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), m_dPoly.GetLength(), m_dPoly.Begin() );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		Expr_Contains_c::FixupLocator ( pOldSchema, pNewSchema );
		for ( auto i : m_dExpr )
			i->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_Contains_c::Command ( eCmd, pArg );
		ARRAY_FOREACH ( i, m_dExpr )
			m_dExpr[i]->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_ContainsExprvec_c");
		CALC_CHILD_HASHES(m_dExpr);
		return CALC_PARENT_HASH();
	}
};




class Expr_ContainsStrattr_c : public Expr_Contains_c
{
protected:
	CSphRefcountedPtr<ISphExpr> m_pStr;
	bool m_bGeo;

public:
	Expr_ContainsStrattr_c ( ISphExpr * pLat, ISphExpr * pLon, ISphExpr * pStr, bool bGeo )
		: Expr_Contains_c (pLat, pLon )
		, m_pStr ( pStr )
		, m_bGeo ( bGeo )
	{
		SafeAddRef ( pStr );
	}

	static void ParsePoly ( const char * p, int iLen, CSphVector<float> & dPoly )
	{
		const char * pBegin = p;
		const char * pMax = sphFindLastNumeric ( p, iLen );
		while ( p<pMax )
		{
			if ( isdigit(p[0]) || ( p+1<pMax && p[0]=='-' && isdigit(p[1]) ) )
				dPoly.Add ( (float)strtod ( p, (char**)&p ) );
			else
				p++;
		}

		// edge case - last numeric touches the end
		iLen -= pMax - pBegin;
		if ( iLen )
			dPoly.Add ( (float)strtod ( CSphString(pMax, iLen).cstr (), nullptr ) );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const char * pStr;
		assert ( !m_pStr->IsDataPtrAttr() ); // aware of mem leaks caused by some StringEval implementations
		int iLen = m_pStr->StringEval ( tMatch, (const BYTE **)&pStr );

		CSphVector<float> dPoly;
		ParsePoly ( pStr, iLen, dPoly );
		if ( dPoly.GetLength()<6 )
			return 0;
		// OPTIMIZE? add quick bbox check too?

		if ( m_bGeo )
			GeoTesselate ( dPoly );
		return Contains ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), dPoly.GetLength(), dPoly.Begin() );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		Expr_Contains_c::FixupLocator ( pOldSchema, pNewSchema );
		m_pStr->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_Contains_c::Command ( eCmd, pArg );
		m_pStr->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_ContainsStrattr_c");
		CALC_CHILD_HASH(m_pStr);
		return CALC_PARENT_HASH();
	}
};


ISphExpr * ExprParser_t::CreateContainsNode ( const ExprNode_t & tNode )
{
	// get and check them args
	const ExprNode_t & tArglist = m_dNodes [ tNode.m_iLeft ];
	const int iPoly = m_dNodes [ tArglist.m_iLeft ].m_iLeft;
	const int iLat = m_dNodes [ tArglist.m_iLeft ].m_iRight;
	const int iLon = tArglist.m_iRight;
	assert ( IsNumeric ( m_dNodes[iLat].m_eRetType ) || IsJson ( m_dNodes[iLat].m_eRetType )  );
	assert ( IsNumeric ( m_dNodes[iLon].m_eRetType ) || IsJson ( m_dNodes[iLon].m_eRetType ) );
	assert ( m_dNodes[iPoly].m_eRetType==SPH_ATTR_POLY2D );

	// create evaluator
	// gotta handle an optimized constant poly case
	CSphVector<int> dPolyArgs;
	GatherArgNodes ( m_dNodes[iPoly].m_iLeft, dPolyArgs );

	CSphRefcountedPtr<ISphExpr> pLat { ConvertExprJson ( CreateTree ( iLat ) ) };
	CSphRefcountedPtr<ISphExpr> pLon { ConvertExprJson ( CreateTree ( iLon ) ) };

	bool bGeoTesselate = ( m_dNodes[iPoly].m_iToken==TOK_FUNC && m_dNodes[iPoly].m_iFunc==FUNC_GEOPOLY2D );

	if ( dPolyArgs.GetLength()==1 && ( m_dNodes[dPolyArgs[0]].m_iToken==TOK_ATTR_STRING || m_dNodes[dPolyArgs[0]].m_iToken==TOK_ATTR_JSON ) )
	{
		CSphRefcountedPtr<ISphExpr> dPolyArgs0 { ConvertExprJson ( CreateTree ( dPolyArgs[0] ) ) };
		return new Expr_ContainsStrattr_c ( pLat, pLon, dPolyArgs0, bGeoTesselate );
	}

	if ( dPolyArgs.TestAll ( [&] ( int iArg ) { return IsConst ( &m_dNodes[iArg] ); } ) )
	{
		// POLY2D(numeric-consts)
		return new Expr_ContainsConstvec_c ( pLat, pLon, dPolyArgs, m_dNodes.Begin(), bGeoTesselate );
	} else
	{
		// POLY2D(generic-exprs)
		VecRefPtrs_t<ISphExpr*> dExprs;
		dExprs.Resize ( dPolyArgs.GetLength() );
		ARRAY_FOREACH ( i, dExprs )
			dExprs[i] = CreateTree ( dPolyArgs[i] );

		ConvertArgsJson ( dExprs );

		// will adopt dExprs and utilize them on d-tr
		return new Expr_ContainsExprvec_c ( pLat, pLon, dExprs );
	}
}

class Expr_Remap_c : public ISphExpr
{
	struct CondValPair_t
	{
		int64_t m_iCond;
		union
		{
			int64_t m_iVal;
			float m_fVal;
		};

		explicit CondValPair_t ( int64_t iCond=0 ) : m_iCond ( iCond ), m_iVal ( 0 ) {}
		bool operator< ( const CondValPair_t & rhs ) const { return m_iCond<rhs.m_iCond; }
		bool operator== ( const CondValPair_t & rhs ) const { return m_iCond==rhs.m_iCond; }
	};

	CSphRefcountedPtr<ISphExpr> m_pCond;
	CSphRefcountedPtr<ISphExpr> m_pVal;
	CSphVector<CondValPair_t> m_dPairs;

public:
	Expr_Remap_c ( ISphExpr * pCondExpr, ISphExpr * pValExpr, const CSphVector<int64_t> & dConds, const ConstList_c & tVals )
		: m_pCond ( pCondExpr )
		, m_pVal ( pValExpr )
		, m_dPairs ( dConds.GetLength() )
	{
		assert ( pCondExpr && pValExpr );
		assert ( dConds.GetLength() );
		assert ( dConds.GetLength()==tVals.m_dInts.GetLength() ||
				dConds.GetLength()==tVals.m_dFloats.GetLength() );

		SafeAddRef ( pCondExpr );
		SafeAddRef ( pValExpr );

		if ( tVals.m_dInts.GetLength() )
			ARRAY_FOREACH ( i, m_dPairs )
			{
				m_dPairs[i].m_iCond = dConds[i];
				m_dPairs[i].m_iVal = tVals.m_dInts[i];
			}
		else
			ARRAY_FOREACH ( i, m_dPairs )
			{
				m_dPairs[i].m_iCond = dConds[i];
				m_dPairs[i].m_fVal = tVals.m_dFloats[i];
			}

		m_dPairs.Uniq();
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		const CondValPair_t * p = m_dPairs.BinarySearch ( CondValPair_t ( m_pCond->Int64Eval ( tMatch ) ) );
		if ( p )
			return p->m_fVal;
		return m_pVal->Eval ( tMatch );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		return (int)Int64Eval ( tMatch );
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		const CondValPair_t * p = m_dPairs.BinarySearch ( CondValPair_t ( m_pCond->Int64Eval ( tMatch ) ) );
		if ( p )
			return p->m_iVal;
		return m_pVal->Int64Eval ( tMatch );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		m_pCond->FixupLocator ( pOldSchema, pNewSchema );
		m_pVal->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		m_pCond->Command ( eCmd, pArg );
		m_pVal->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Remap_c");
		CALC_POD_HASHES(m_dPairs);
		CALC_CHILD_HASH(m_pCond);
		CALC_CHILD_HASH(m_pVal);
		return CALC_DEP_HASHES();
	}
};

//////////////////////////////////////////////////////////////////////////

ISphExpr * ConvertExprJson ( ISphExpr * pExpr )
{
	if ( !pExpr )
		return nullptr;

	bool bConverted = false;
	bool bJson = pExpr->IsJson ( bConverted );
	if ( bJson && !bConverted )
	{
		ISphExpr * pConv = new Expr_JsonFieldConv_c ( pExpr );
		pExpr->Release();
		return pConv;
	} else
	{
		return pExpr;
	}
}

void ConvertArgsJson ( VecRefPtrs_t<ISphExpr *> & dArgs )
{
	ARRAY_FOREACH ( i, dArgs )
	{
		dArgs[i] = ConvertExprJson ( dArgs[i] );
	}
}

/// fold nodes subtree into opcodes
ISphExpr * ExprParser_t::CreateTree ( int iNode )
{
	if ( iNode<0 || GetError() )
		return nullptr;

	const ExprNode_t & tNode = m_dNodes[iNode];

	// avoid spawning argument node in some cases
	bool bSkipLeft = false;
	bool bSkipRight = false;
	if ( tNode.m_iToken==TOK_FUNC )
	{
		switch ( tNode.m_iFunc )
		{
		case FUNC_NOW:
		case FUNC_IN:
		case FUNC_EXIST:
		case FUNC_GEODIST:
		case FUNC_CONTAINS:
		case FUNC_ZONESPANLIST:
		case FUNC_RANKFACTORS:
		case FUNC_PACKEDFACTORS:
		case FUNC_FACTORS:
		case FUNC_BM25F:
		case FUNC_CURTIME:
		case FUNC_UTC_TIME:
		case FUNC_UTC_TIMESTAMP:
		case FUNC_ALL:
		case FUNC_ANY:
		case FUNC_INDEXOF:
		case FUNC_MIN_TOP_WEIGHT:
		case FUNC_MIN_TOP_SORTVAL:
		case FUNC_REMAP:
			bSkipLeft = true;
			bSkipRight = true;
			break;
		default:
			break;
		}
	}

	CSphRefcountedPtr<ISphExpr> pLeft ( bSkipLeft ? nullptr : CreateTree ( tNode.m_iLeft ) );
	CSphRefcountedPtr<ISphExpr> pRight ( bSkipRight ? nullptr : CreateTree ( tNode.m_iRight ) );

	if ( GetError() )
		return nullptr;

#define LOC_SPAWN_POLY(_classname) \
	if ( tNode.m_eArgType==SPH_ATTR_INTEGER )		return new _classname##Int_c ( pLeft, pRight ); \
	else if ( tNode.m_eArgType==SPH_ATTR_BIGINT )	return new _classname##Int64_c ( pLeft, pRight ); \
	else											return new _classname##Float_c ( pLeft, pRight );

	int iOp = tNode.m_iToken;
	if ( iOp=='+' || iOp=='-' || iOp=='*' || iOp=='/' || iOp=='&' || iOp=='|' || iOp=='%' || iOp=='<' || iOp=='>'
		|| iOp==TOK_LTE || iOp==TOK_GTE || iOp==TOK_EQ || iOp==TOK_NE || iOp==TOK_AND || iOp==TOK_OR || iOp==TOK_NOT )
	{
		if ( pLeft && m_dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_JSON_FIELD && m_dNodes[tNode.m_iLeft].m_iToken==TOK_ATTR_JSON )
			pLeft = new Expr_JsonFieldConv_c ( pLeft );
		if ( pRight && m_dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_JSON_FIELD && m_dNodes[tNode.m_iRight].m_iToken==TOK_ATTR_JSON )
			pRight = new Expr_JsonFieldConv_c ( pRight );
	}

	switch ( tNode.m_iToken )
	{
		case TOK_ATTR_INT:		return new Expr_GetInt_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_BITS:		return new Expr_GetBits_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_FLOAT:	return new Expr_GetFloat_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_SINT:		return new Expr_GetSint_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_STRING:	return new Expr_GetString_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_MVA64:
		case TOK_ATTR_MVA32:	return new Expr_GetMva_c ( tNode.m_tLocator, tNode.m_iLocator );
		case TOK_ATTR_FACTORS:	return new Expr_GetFactorsAttr_c ( tNode.m_tLocator, tNode.m_iLocator );

		case TOK_CONST_FLOAT:	return new Expr_GetConst_c ( tNode.m_fConst );
		case TOK_CONST_INT:
			if ( tNode.m_eRetType==SPH_ATTR_INTEGER )
				return new Expr_GetIntConst_c ( (int)tNode.m_iConst );
			else if ( tNode.m_eRetType==SPH_ATTR_BIGINT )
				return new Expr_GetInt64Const_c ( tNode.m_iConst );
			else
				return new Expr_GetConst_c ( float(tNode.m_iConst) );
			break;
		case TOK_CONST_STRING:
			return new Expr_GetStrConst_c ( m_sExpr+(int)( tNode.m_iConst>>32 ), (int)( tNode.m_iConst & 0xffffffffUL ), true );
		case TOK_SUBKEY:
			return new Expr_GetStrConst_c ( m_sExpr+(int)( tNode.m_iConst>>32 ), (int)( tNode.m_iConst & 0xffffffffUL ), false );

		case TOK_ID:			return new Expr_GetId_c ();
		case TOK_WEIGHT:		return new Expr_GetWeight_c ();

		case '+':				return new Expr_Add_c ( pLeft, pRight ); break;
		case '-':				return new Expr_Sub_c ( pLeft, pRight ); break;
		case '*':				return new Expr_Mul_c ( pLeft, pRight ); break;
		case '/':				return new Expr_Div_c ( pLeft, pRight ); break;
		case '&':				return new Expr_BitAnd_c ( pLeft, pRight ); break;
		case '|':				return new Expr_BitOr_c ( pLeft, pRight ); break;
		case '%':				return new Expr_Mod_c ( pLeft, pRight ); break;

		case '<':				LOC_SPAWN_POLY ( Expr_Lt ); break;
		case '>':				LOC_SPAWN_POLY ( Expr_Gt ); break;
		case TOK_LTE:			LOC_SPAWN_POLY ( Expr_Lte ); break;
		case TOK_GTE:			LOC_SPAWN_POLY ( Expr_Gte ); break;
		case TOK_EQ:			if ( ( m_dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_STRING ||
									m_dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_STRINGPTR ) &&
									( m_dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_STRING ||
									m_dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_STRINGPTR ) )
									return new Expr_StrEq_c ( pLeft, pRight, m_eCollation );
								else if ( ( m_dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_JSON_FIELD ) &&
									( m_dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_STRING ||
									m_dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_STRINGPTR ) )
									return new Expr_StrEq_c ( pLeft, pRight, m_eCollation );
								LOC_SPAWN_POLY ( Expr_Eq ); break;
		case TOK_NE:			LOC_SPAWN_POLY ( Expr_Ne ); break;
		case TOK_AND:			LOC_SPAWN_POLY ( Expr_And ); break;
		case TOK_OR:			LOC_SPAWN_POLY ( Expr_Or ); break;
		case TOK_NOT:
			return ( tNode.m_eArgType==SPH_ATTR_BIGINT )
				? (ISphExpr * ) new Expr_NotInt64_c ( pLeft )
				: (ISphExpr * ) new Expr_NotInt_c ( pLeft );

		case ',':
			if ( pLeft && pRight )
				return new Expr_Arglist_c ( pLeft, pRight );
			break;

		case TOK_NEG:			assert ( !pRight ); return new Expr_Neg_c ( pLeft ); break;
		case TOK_FUNC:
			{
				// fold arglist to array
				auto eFunc = (Func_e)tNode.m_iFunc;
				assert ( g_dFuncs[tNode.m_iFunc].m_eFunc==eFunc );

				VecRefPtrs_t<ISphExpr *> dArgs;
				if ( !bSkipLeft )
				{
					SafeAddRef ( pLeft );
					MoveToArgList ( pLeft, dArgs );
				}

				// spawn proper function
				assert ( tNode.m_iFunc>=0 && tNode.m_iFunc<int(sizeof(g_dFuncs)/sizeof(g_dFuncs[0])) );
				assert (
					( bSkipLeft ) || // function will handle its arglist,
					( g_dFuncs[tNode.m_iFunc].m_iArgs>=0 && g_dFuncs[tNode.m_iFunc].m_iArgs==dArgs.GetLength() ) || // arg count matches,
					( g_dFuncs[tNode.m_iFunc].m_iArgs<0 && -g_dFuncs[tNode.m_iFunc].m_iArgs<=dArgs.GetLength() ) ); // or min vararg count reached

				switch ( eFunc )
				{
				case FUNC_TO_STRING:
				case FUNC_INTERVAL:
				case FUNC_IN:
				case FUNC_LENGTH:
				case FUNC_LEAST:
				case FUNC_GREATEST:
				case FUNC_ALL:
				case FUNC_ANY:
				case FUNC_INDEXOF:
					break; // these have its own JSON converters

				// all others will get JSON auto-converter
				default:
					ConvertArgsJson ( dArgs );
				}

				switch ( eFunc )
				{
					case FUNC_NOW:		return new Expr_Now_c(m_iConstNow); break;

					case FUNC_ABS:		return new Expr_Abs_c ( dArgs[0] );
					case FUNC_CEIL:		return new Expr_Ceil_c ( dArgs[0] );
					case FUNC_FLOOR:	return new Expr_Floor_c ( dArgs[0] );
					case FUNC_SIN:		return new Expr_Sin_c ( dArgs[0] );
					case FUNC_COS:		return new Expr_Cos_c ( dArgs[0] );
					case FUNC_LN:		return new Expr_Ln_c ( dArgs[0] );
					case FUNC_LOG2:		return new Expr_Log2_c ( dArgs[0] );
					case FUNC_LOG10:	return new Expr_Log10_c ( dArgs[0] );
					case FUNC_EXP:		return new Expr_Exp_c ( dArgs[0] );
					case FUNC_SQRT:		return new Expr_Sqrt_c ( dArgs[0] );
					case FUNC_SINT:		return new Expr_Sint_c ( dArgs[0] );
					case FUNC_CRC32:	return new Expr_Crc32_c ( dArgs[0] );
					case FUNC_FIBONACCI:return new Expr_Fibonacci_c ( dArgs[0] );

					case FUNC_DAY:			return ExprDay ( dArgs[0] );
					case FUNC_MONTH:		return ExprMonth ( dArgs[0] );
					case FUNC_YEAR:			return ExprYear ( dArgs[0] );
					case FUNC_YEARMONTH:	return ExprYearMonth ( dArgs[0] );
					case FUNC_YEARMONTHDAY:	return ExprYearMonthDay ( dArgs[0] );
					case FUNC_HOUR:			return new Expr_Hour_c ( dArgs[0] );
					case FUNC_MINUTE:		return new Expr_Minute_c ( dArgs[0] );
					case FUNC_SECOND:		return new Expr_Second_c ( dArgs[0] );

					case FUNC_MIN:		return new Expr_Min_c ( dArgs[0], dArgs[1] );
					case FUNC_MAX:		return new Expr_Max_c ( dArgs[0], dArgs[1] );
					case FUNC_POW:		return new Expr_Pow_c ( dArgs[0], dArgs[1] );
					case FUNC_IDIV:		return new Expr_Idiv_c ( dArgs[0], dArgs[1] );

					case FUNC_IF:		return new Expr_If_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MADD:		return new Expr_Madd_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_MUL3:		return new Expr_Mul3_c ( dArgs[0], dArgs[1], dArgs[2] );
					case FUNC_ATAN2:	return new Expr_Atan2_c ( dArgs[0], dArgs[1] );
					case FUNC_RAND:		return new Expr_Rand_c ( dArgs.GetLength() ? dArgs[0] : nullptr,
							tNode.m_iLeft<0 ? false : IsConst ( &m_dNodes[tNode.m_iLeft] ));

					case FUNC_INTERVAL:	return CreateIntervalNode ( tNode.m_iLeft, dArgs );
					case FUNC_IN:		return CreateInNode ( iNode );
					case FUNC_LENGTH:	return CreateLengthNode ( tNode, dArgs[0] );
					case FUNC_BITDOT:	return CreateBitdotNode ( tNode.m_iLeft, dArgs );
					case FUNC_REMAP:
					{
						CSphRefcountedPtr<ISphExpr> pCond ( CreateTree ( tNode.m_iLeft ) );
						CSphRefcountedPtr<ISphExpr> pVal ( CreateTree ( tNode.m_iRight ) );
						assert ( pCond && pVal );
						// This is a hack. I know how parser fills m_dNodes and thus know where to find constlists.
						const CSphVector<int64_t> & dConds = m_dNodes [ iNode-2 ].m_pConsts->m_dInts;
						const ConstList_c & tVals = *m_dNodes [ iNode-1 ].m_pConsts;
						return new Expr_Remap_c ( pCond, pVal, dConds, tVals );
					}

					case FUNC_GEODIST:	return CreateGeodistNode ( tNode.m_iLeft );
					case FUNC_EXIST:	return CreateExistNode ( tNode );
					case FUNC_CONTAINS:	return CreateContainsNode ( tNode );

					case FUNC_POLY2D:
					case FUNC_GEOPOLY2D:break; // just make gcc happy

					case FUNC_ZONESPANLIST:
						m_bHasZonespanlist = true;
						m_eEvalStage = SPH_EVAL_PRESORT;
						return new Expr_GetZonespanlist_c ();
					case FUNC_TO_STRING:
						return new Expr_ToString_c ( dArgs[0], m_dNodes [ tNode.m_iLeft ].m_eRetType );
					case FUNC_RANKFACTORS:
						m_eEvalStage = SPH_EVAL_PRESORT;
						return new Expr_GetRankFactors_c();
					case FUNC_PACKEDFACTORS:
					case FUNC_FACTORS:
						return CreatePFNode ( tNode.m_iLeft );
					case FUNC_BM25F:
					{
						m_uPackedFactorFlags |= SPH_FACTOR_ENABLE;

						CSphVector<int> dBM25FArgs;
						GatherArgNodes ( tNode.m_iLeft, dBM25FArgs );

						const ExprNode_t & tLeft = m_dNodes [ dBM25FArgs[0] ];
						const ExprNode_t & tRight = m_dNodes [ dBM25FArgs[1] ];
						float fK1 = tLeft.m_fConst;
						float fB = tRight.m_fConst;
						fK1 = Max ( fK1, 0.001f );
						fB = Min ( Max ( fB, 0.0f ), 1.0f );

						CSphVector<CSphNamedVariant> * pFieldWeights = nullptr;
						if ( dBM25FArgs.GetLength()>2 )
							pFieldWeights = &m_dNodes [ dBM25FArgs[2] ].m_pMapArg->m_dPairs;

						return new Expr_BM25F_c ( fK1, fB, pFieldWeights );
					}

					case FUNC_BIGINT:
					case FUNC_INTEGER:
					case FUNC_DOUBLE:
					case FUNC_UINT:
						SafeAddRef ( dArgs[0] );
						return dArgs[0];

					case FUNC_LEAST:	return CreateAggregateNode ( tNode, SPH_AGGR_MIN, dArgs[0] );
					case FUNC_GREATEST:	return CreateAggregateNode ( tNode, SPH_AGGR_MAX, dArgs[0] );

					case FUNC_CURTIME:	return new Expr_Time_c ( false, false ); break;
					case FUNC_UTC_TIME: return new Expr_Time_c ( true, false ); break;
					case FUNC_UTC_TIMESTAMP: return new Expr_Time_c ( true, true ); break;
					case FUNC_TIMEDIFF: return new Expr_TimeDiff_c ( dArgs[0], dArgs[1] ); break;

					case FUNC_ALL:
					case FUNC_ANY:
					case FUNC_INDEXOF:
						return CreateForInNode ( iNode );

					case FUNC_MIN_TOP_WEIGHT:
						m_eEvalStage = SPH_EVAL_PRESORT;
						return new Expr_MinTopWeight();
						break;
					case FUNC_MIN_TOP_SORTVAL:
						m_eEvalStage = SPH_EVAL_PRESORT;
						return new Expr_MinTopSortval();
						break;
					case FUNC_REGEX:
						return CreateRegexNode ( dArgs[0], dArgs[1] );
						break;

					case FUNC_SUBSTRING_INDEX:
						return new Expr_SubstringIndex_c ( dArgs[0], dArgs[1], dArgs[2] );
						break;

					default: // just make gcc happy
						break;
				}
				assert ( 0 && "unhandled function id" );
				break;
			}

		case TOK_UDF:			return CreateUdfNode ( tNode.m_iFunc, pLeft ); break;
		case TOK_HOOK_IDENT:	return m_pHook->CreateNode ( tNode.m_iFunc, NULL, NULL, m_sCreateError ); break;
		case TOK_HOOK_FUNC:		return m_pHook->CreateNode ( tNode.m_iFunc, pLeft, &m_eEvalStage, m_sCreateError ); break;
		case TOK_MAP_ARG:
			// tricky bit
			// data gets moved (!) from node to ISphExpr at this point
			return new Expr_MapArg_c ( tNode.m_pMapArg->m_dPairs );
			break;
		case TOK_ATTR_JSON:
			if ( pLeft && m_dNodes[tNode.m_iLeft].m_iToken==TOK_SUBKEY && !tNode.m_tLocator.m_bDynamic )
			{
				// json key is a single static subkey, switch to fastpath
				return new Expr_JsonFastKey_c ( tNode.m_tLocator, tNode.m_iLocator, pLeft );
			} else
			{
				// json key is a generic expression, use generic catch-all JsonField
				VecRefPtrs_t<ISphExpr *> dArgs;
				CSphVector<ESphAttr> dTypes;
				if ( pLeft ) // may be NULL (top level array)
				{
					MoveToArgList ( pLeft.Leak (), dArgs );
					GatherArgRetTypes ( tNode.m_iLeft, dTypes );
				}
				return new Expr_JsonField_c ( tNode.m_tLocator, tNode.m_iLocator, dArgs, dTypes );
			}
			break;
		case TOK_ITERATOR:
			{
				// iterator, e.g. handles "x.gid" in SELECT ALL(x.gid=1 FOR x IN json.array)
				VecRefPtrs_t<ISphExpr *> dArgs;
				CSphVector<ESphAttr> dTypes;
				if ( pLeft )
				{
					MoveToArgList ( pLeft.Leak (), dArgs );
					GatherArgRetTypes ( tNode.m_iLeft, dTypes );
				}
				CSphRefcountedPtr<ISphExpr> pIterator { new Expr_Iterator_c ( tNode.m_tLocator, tNode.m_iLocator, dArgs, dTypes, tNode.m_pAttr ) };
				return new Expr_JsonFieldConv_c ( pIterator );
			}
		case TOK_IDENT:			m_sCreateError.SetSprintf ( "unknown column: %s", tNode.m_sIdent ); break;

		case TOK_IS_NULL:
		case TOK_IS_NOT_NULL:
			if ( m_dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_JSON_FIELD )
				return new Expr_JsonFieldIsNull_c ( pLeft, tNode.m_iToken==TOK_IS_NULL );
			else
				return new Expr_GetIntConst_c ( tNode.m_iToken!=TOK_IS_NULL );

		default:				assert ( 0 && "unhandled token type" ); break;
	}

#undef LOC_SPAWN_POLY

	// fire exit
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////

/// arg-vs-set function (currently, IN or INTERVAL) evaluator traits
template < typename T >
class Expr_ArgVsSet_c : public ISphExpr
{
public:
	explicit Expr_ArgVsSet_c ( ISphExpr * pArg ) : m_pArg ( pArg ) { SafeAddRef ( pArg ); }

	float Eval ( const CSphMatch & tMatch ) const override { return (float) IntEval ( tMatch ); }
	int64_t Int64Eval ( const CSphMatch & tMatch ) const override { return IntEval ( tMatch ); }
	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		if ( m_pArg )
			m_pArg->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( m_pArg )
			m_pArg->Command ( eCmd, pArg );
	}

protected:
	CSphRefcountedPtr<ISphExpr> m_pArg; /* { nullptr }; */

	T ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const;

	uint64_t CalcHash ( const char * szTag, const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable )
	{
		EXPR_CLASS_NAME_NOCHECK(szTag);
		CALC_CHILD_HASH(m_pArg);
		return CALC_DEP_HASHES();
	}
};

template<> int Expr_ArgVsSet_c<int>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const
{
	return pArg->IntEval ( tMatch );
}

template<> DWORD Expr_ArgVsSet_c<DWORD>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const
{
	return (DWORD)pArg->IntEval ( tMatch );
}

template<> float Expr_ArgVsSet_c<float>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const
{
	return pArg->Eval ( tMatch );
}

template<> int64_t Expr_ArgVsSet_c<int64_t>::ExprEval ( ISphExpr * pArg, const CSphMatch & tMatch ) const
{
	return pArg->Int64Eval ( tMatch );
}


/// arg-vs-constant-set
template < typename T >
class Expr_ArgVsConstSet_c : public Expr_ArgVsSet_c<T>
{
public:
	/// pre-evaluate and dismiss turn points
	Expr_ArgVsConstSet_c ( ISphExpr * pArg, const CSphVector<ISphExpr *> & dArgs, int iSkip )
		: Expr_ArgVsSet_c<T> ( pArg )
		, m_bFloat ( false )
	{
		CSphMatch tDummy;
		for ( int i=iSkip; i<dArgs.GetLength(); ++i )
			m_dValues.Add ( Expr_ArgVsSet_c<T>::ExprEval ( dArgs[i], tDummy ) );

		CalcValueHash();
	}

	/// copy that constlist
	Expr_ArgVsConstSet_c ( ISphExpr * pArg, ConstList_c * pConsts, bool bKeepFloat )
		: Expr_ArgVsSet_c<T> ( pArg )
		, m_bFloat ( false )
	{
		if ( !pConsts )
			return; // can happen on uservar path
		if ( pConsts->m_eRetType==SPH_ATTR_FLOAT )
		{
			m_dValues.Reserve ( pConsts->m_dFloats.GetLength() );
			if ( !bKeepFloat )
			{
				ARRAY_FOREACH ( i, pConsts->m_dFloats )
					m_dValues.Add ( (T)pConsts->m_dFloats[i] );
			} else
			{
				m_bFloat = true;
				ARRAY_FOREACH ( i, pConsts->m_dFloats )
					m_dValues.Add ( (T) sphF2DW ( pConsts->m_dFloats[i] ) );
			}
		} else
		{
			m_dValues.Reserve ( pConsts->m_dInts.GetLength() );
			ARRAY_FOREACH ( i, pConsts->m_dInts )
				m_dValues.Add ( (T)pConsts->m_dInts[i] );
		}

		CalcValueHash();
	}

	/// copy that uservar
	Expr_ArgVsConstSet_c ( ISphExpr * pArg, UservarIntSet_c * pUservar )
		: Expr_ArgVsSet_c<T> ( pArg )
		, m_bFloat ( false )
	{
		if ( !pUservar )
			return; // can happen on uservar path
		m_dValues.Reserve ( pUservar->GetLength() );
		for ( int i=0; i<pUservar->GetLength(); i++ )
			m_dValues.Add ( (T)*(pUservar->Begin() + i) );

		CalcValueHash();
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) override
	{
		EXPR_CLASS_NAME("Expr_ArgVsConstSet_c");
		return CALC_PARENT_HASH();
	}

protected:
	CSphVector<T>	m_dValues;
	uint64_t		m_uValueHash=0;
	bool			m_bFloat = false;

	void CalcValueHash()
	{
		ARRAY_FOREACH ( i, m_dValues )
			m_uValueHash = sphFNV64 ( &m_dValues[i], sizeof(m_dValues[i]), i ? m_uValueHash : SPH_FNV64_SEED );
	}

	uint64_t CalcHash ( const char * szTag, const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable )
	{
		return Expr_ArgVsSet_c<T>::CalcHash ( szTag, tSorterSchema, uPrevHash^m_uValueHash, bDisable );
	}
};

//////////////////////////////////////////////////////////////////////////

/// INTERVAL() evaluator for constant turn point values case
template < typename T >
class Expr_IntervalConst_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// pre-evaluate and dismiss turn points
	explicit Expr_IntervalConst_c ( CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsConstSet_c<T> ( dArgs[0], dArgs, 1 )
	{}

	/// evaluate arg, return interval id
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, this->m_dValues ) // FIXME! OPTIMIZE! perform binary search here
			if ( val<this->m_dValues[i] )
				return i;
		return this->m_dValues.GetLength();
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_IntervalConst_c");		
		return Expr_ArgVsConstSet_c<T>::CalcHash ( szClassName, tSorterSchema, uHash, bDisable );		// can't do CALC_PARENT_HASH because of gcc and templates
	}
};


/// generic INTERVAL() evaluator
template < typename T >
class Expr_Interval_c : public Expr_ArgVsSet_c<T>
{
protected:
	VecRefPtrs_t<ISphExpr *> m_dTurnPoints;

public:

	explicit Expr_Interval_c ( const CSphVector<ISphExpr *> & dArgs )
		: Expr_ArgVsSet_c<T> ( dArgs[0] )
	{
		for ( int i=1; i<dArgs.GetLength(); ++i )
		{
			SafeAddRef ( dArgs[i] );
			m_dTurnPoints.Add ( dArgs[i] );
		}
	}

	/// evaluate arg, return interval id
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		ARRAY_FOREACH ( i, m_dTurnPoints )
			if ( val < Expr_ArgVsSet_c<T>::ExprEval ( m_dTurnPoints[i], tMatch ) )
				return i;
		return m_dTurnPoints.GetLength();
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_ArgVsSet_c<T>::Command ( eCmd, pArg );
		ARRAY_FOREACH ( i, m_dTurnPoints )
			m_dTurnPoints[i]->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Interval_c");
		CALC_CHILD_HASHES(m_dTurnPoints);
		return Expr_ArgVsSet_c<T>::CalcHash ( szClassName, tSorterSchema, uHash, bDisable );		// can't do CALC_PARENT_HASH because of gcc and templates
	}
};

//////////////////////////////////////////////////////////////////////////

/// IN() evaluator, arbitrary scalar expression vs. constant values
template < typename T >
class Expr_In_c : public Expr_ArgVsConstSet_c<T>
{
public:
	/// pre-sort values for binary search
	Expr_In_c ( ISphExpr * pArg, ConstList_c * pConsts ) :
		Expr_ArgVsConstSet_c<T> ( pArg, pConsts, false )
	{
		this->m_dValues.Sort();
	}

	/// evaluate arg, check if the value is within set
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		T val = this->ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		return this->m_dValues.BinarySearch ( val )!=nullptr;
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_In_c");
		return Expr_ArgVsConstSet_c<T>::CalcHash ( szClassName, tSorterSchema, uHash, bDisable );		// can't do CALC_PARENT_HASH because of gcc and templates
	}
};


/// IN() evaluator, arbitrary scalar expression vs. uservar
/// (for the sake of evaluator, uservar is a pre-sorted, refcounted external vector)
class Expr_InUservar_c : public Expr_ArgVsSet_c<int64_t>
{
public:
	/// just get hold of args
	explicit Expr_InUservar_c ( ISphExpr * pArg, UservarIntSet_c * pConsts )
		: Expr_ArgVsSet_c<int64_t> ( pArg )
		, m_pConsts ( pConsts ) // no addref, hook should have addref'd (otherwise there'd be a race)
	{
		assert ( m_pConsts );
		m_uHash = sphFNV64 ( m_pConsts->Begin(), m_pConsts->GetLength()*sizeof((*m_pConsts)[0]) );
	}

	/// release the uservar value
	~Expr_InUservar_c() final
	{
		SafeRelease ( m_pConsts );
	}

	/// evaluate arg, check if the value is within set
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		int64_t iVal = ExprEval ( this->m_pArg, tMatch ); // 'this' fixes gcc braindamage
		return m_pConsts->BinarySearch ( iVal )!=nullptr;
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_InUservar_c");
		return CALC_PARENT_HASH_EX(m_uHash);
	}

protected:
	UservarIntSet_c *	m_pConsts;
	uint64_t			m_uHash;
};


/// IN() evaluator, MVA attribute vs. constant values
template < bool MVA64 >
class Expr_MVAIn_c : public Expr_ArgVsConstSet_c<int64_t>, public ExprLocatorTraits_t
{
public:
	/// pre-sort values for binary search
	Expr_MVAIn_c ( const CSphAttrLocator & tLoc, int iLocator, ConstList_c * pConsts, UservarIntSet_c * pUservar )
		: Expr_ArgVsConstSet_c<int64_t> ( nullptr, pConsts, false )
		, ExprLocatorTraits_t ( tLoc, iLocator )
		, m_pUservar ( pUservar )
	{
		assert ( tLoc.m_iBitOffset>=0 && tLoc.m_iBitCount>0 );
		assert ( !pConsts || !pUservar ); // either constlist or uservar, not both
		this->m_dValues.Sort();

		// consts are handled in Expr_ArgVsConstSet_c, we only need uservars
		if ( pUservar )
			m_uValueHash = sphFNV64 ( pUservar->Begin(), pUservar->GetLength()*sizeof((*pUservar)[0]) );
	}

	~Expr_MVAIn_c() final
	{
		SafeRelease ( m_pUservar );
	}

	int MvaEval ( const DWORD * pMva ) const;

	const DWORD * MvaEval ( const CSphMatch & ) const final { assert ( 0 && "not implemented" ); return nullptr; }

	/// evaluate arg, check if any values are within set
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva = tMatch.GetAttrMVA ( m_tLocator, m_pMvaPool, m_bArenaProhibit );
		if ( !pMva )
			return 0;

		return MvaEval ( pMva );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_ArgVsConstSet_c<int64_t>::Command ( eCmd, pArg );
		ExprLocatorTraits_t::HandleCommand ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_MVA_POOL )
		{
			auto * pPool = (const PoolPtrs_t *)pArg;
			assert ( pArg );
			m_pMvaPool = pPool->m_pMva;
			m_bArenaProhibit = pPool->m_bArenaProhibit;
		}
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_MVAIn_c");
		CALC_POD_HASH(m_bArenaProhibit);
		return CALC_DEP_HASHES_EX(m_uValueHash);
	}

protected:
	const DWORD *		m_pMvaPool = nullptr;
	UservarIntSet_c *	m_pUservar;
	bool				m_bArenaProhibit = false;
};


template<>
int Expr_MVAIn_c<false>::MvaEval ( const DWORD * pMva ) const
{
	// OPTIMIZE! FIXME! factor out a common function with Filter_MVAValues::Eval()
	DWORD uLen = *pMva++;
	const DWORD * pMvaMax = pMva+uLen;

	const int64_t * pFilter = m_pUservar ? m_pUservar->Begin() : m_dValues.Begin();
	const int64_t * pFilterMax = pFilter + ( m_pUservar ? m_pUservar->GetLength() : m_dValues.GetLength() );

	const DWORD * L = pMva;
	const DWORD * R = pMvaMax - 1;
	for ( ; pFilter < pFilterMax; pFilter++ )
	{
		while ( L<=R )
		{
			const DWORD * m = L + (R - L) / 2;

			if ( *pFilter > *m )
				L = m + 1;
			else if ( *pFilter < *m )
				R = m - 1;
			else
				return 1;
		}
		R = pMvaMax - 1;
	}
	return 0;
}


template<>
int Expr_MVAIn_c<true>::MvaEval ( const DWORD * pMva ) const
{
	// OPTIMIZE! FIXME! factor out a common function with Filter_MVAValues::Eval()
	DWORD uLen = *pMva++;
	assert ( ( uLen%2 )==0 );
	const DWORD * pMvaMax = pMva+uLen;

	const int64_t * pFilter = m_pUservar ? m_pUservar->Begin() : m_dValues.Begin();
	const int64_t * pFilterMax = pFilter + ( m_pUservar ? m_pUservar->GetLength() : m_dValues.GetLength() );

	auto * L = (const int64_t *)pMva;
	auto * R = (const int64_t *)( pMvaMax - 2 );
	for ( ; pFilter < pFilterMax; pFilter++ )
	{
		while ( L<=R )
		{
			const int64_t * pVal = L + (R - L) / 2;
			int64_t iMva = MVA_UPSIZE ( (const DWORD *)pVal );

			if ( *pFilter > iMva )
				L = pVal + 1;
			else if ( *pFilter < iMva )
				R = pVal - 1;
			else
				return 1;
		}
		R = (const int64_t *) ( pMvaMax - 2 );
	}
	return 0;
}

/// LENGTH() evaluator for MVAs
class Expr_MVALength_c : public Expr_WithLocator_c
{
public:
	Expr_MVALength_c ( const CSphAttrLocator & tLoc, int iLocator, bool b64 )
		: Expr_WithLocator_c ( tLoc, iLocator )
		, m_b64 ( b64 )
	{
		assert ( tLoc.m_iBitOffset>=0 && tLoc.m_iBitCount>0 );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva = tMatch.GetAttrMVA ( m_tLocator, m_pMvaPool, m_bArenaProhibit );
		if ( !pMva )
			return 0;
		return (int)( m_b64 ? *pMva/2 : *pMva );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_MVA_POOL )
		{
			auto * pPool = (const PoolPtrs_t *)pArg;
			assert ( pArg );
			m_pMvaPool = pPool->m_pMva;
			m_bArenaProhibit = pPool->m_bArenaProhibit;
		}
	}

	float Eval ( const CSphMatch & tMatch ) const final { return (float)IntEval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_MVALength_c");
		CALC_POD_HASH(m_bArenaProhibit);
		CALC_POD_HASH(m_b64);
		return CALC_DEP_HASHES();
	}

protected:
	bool				m_b64;
	const DWORD *		m_pMvaPool = nullptr;
	bool				m_bArenaProhibit = false;
};


/// aggregate functions evaluator for MVA attribute
template < bool MVA64 >
class Expr_MVAAggr_c : public Expr_WithLocator_c
{
public:
	Expr_MVAAggr_c ( const CSphAttrLocator & tLoc, int iLocator, ESphAggrFunc eFunc )
		: Expr_WithLocator_c ( tLoc, iLocator )
		, m_eFunc ( eFunc )
	{
		assert ( tLoc.m_iBitOffset>=0 && tLoc.m_iBitCount>0 );
	}

	int64_t MvaAggr ( const DWORD * pMva, ESphAggrFunc eFunc ) const;

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva = tMatch.GetAttrMVA ( m_tLocator, m_pMvaPool, m_bArenaProhibit );
		if ( !pMva )
			return 0;
		return MvaAggr ( pMva, m_eFunc );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_WithLocator_c::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_MVA_POOL )
		{
			auto * pPool = (const PoolPtrs_t *)pArg;
			assert ( pArg );
			m_pMvaPool = pPool->m_pMva;
			m_bArenaProhibit = pPool->m_bArenaProhibit;
		}
	}

	float	Eval ( const CSphMatch & tMatch ) const final { return (float)Int64Eval ( tMatch ); }
	int		IntEval ( const CSphMatch & tMatch ) const final { return (int)Int64Eval ( tMatch ); }

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_MVAAggr_c");
		CALC_POD_HASH(m_bArenaProhibit);
		CALC_POD_HASH(m_eFunc);
		return CALC_DEP_HASHES();
	}

protected:
	const DWORD *		m_pMvaPool = nullptr;
	bool				m_bArenaProhibit = false;
	ESphAggrFunc		m_eFunc;
};


template <>
int64_t Expr_MVAAggr_c<false>::MvaAggr ( const DWORD * pMva, ESphAggrFunc eFunc ) const
{
	DWORD uLen = *pMva++;
	const DWORD * pMvaMax = pMva+uLen;
	const DWORD * L = pMva;
	const DWORD * R = pMvaMax - 1;

	switch ( eFunc )
	{
		case SPH_AGGR_MIN:	return *L;
		case SPH_AGGR_MAX:	return *R;
		default:			return 0;
	}
}


template <>
int64_t Expr_MVAAggr_c<true>::MvaAggr ( const DWORD * pMva, ESphAggrFunc eFunc ) const
{
	DWORD uLen = *pMva++;
	assert ( ( uLen%2 )==0 );
	const DWORD * pMvaMax = pMva+uLen;
	auto * L = (const int64_t *)pMva;
	auto * R = (const int64_t *)( pMvaMax - 2 );

	switch ( eFunc )
	{
		case SPH_AGGR_MIN:	return *L;
		case SPH_AGGR_MAX:	return *R;
		default:			return 0;
	}
}


/// IN() evaluator, JSON array vs. constant values
class Expr_JsonFieldIn_c : public Expr_ArgVsConstSet_c<int64_t>
{
public:
	Expr_JsonFieldIn_c ( ConstList_c * pConsts, ISphExpr * pArg )
		: Expr_ArgVsConstSet_c<int64_t> ( pArg, pConsts, true )
	{
		assert ( pConsts );

		const char * sExpr = pConsts->m_sExpr.cstr();
		int iExprLen = pConsts->m_sExpr.Length();

		if ( pConsts->m_bPackedStrings )
		{
			for ( int64_t iVal : m_dValues )
			{
				auto iOfs = (int)( iVal>>32 );
				auto iLen = (int)( iVal & 0xffffffffUL );
				if ( iOfs>0 && iLen>0 && iOfs+iLen<=iExprLen )
				{
					CSphString sRes;
					SqlUnescape ( sRes, sExpr + iOfs, iLen );
					m_dHashes.Add ( sphFNV64 ( sRes.cstr(), sRes.Length() ) );
				}
			}
			m_dHashes.Sort();
		}
	}

	Expr_JsonFieldIn_c ( UservarIntSet_c * pUserVar, ISphExpr * pArg )
		: Expr_ArgVsConstSet_c<int64_t> ( pArg, pUserVar )
	{
		assert ( pUserVar );
		m_dHashes.Sort();
	}

	~Expr_JsonFieldIn_c() final = default;

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_ArgVsConstSet_c<int64_t>::Command ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
	}

	/// evaluate arg, check if any values are within set
	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		ESphJsonType eJson = GetKey ( &pVal, tMatch );
		int64_t iVal = 0;
		switch ( eJson )
		{
			case JSON_INT32_VECTOR:		return ArrayEval<int> ( pVal );
			case JSON_INT64_VECTOR:		return ArrayEval<int64_t> ( pVal );
			case JSON_STRING_VECTOR:	return StringArrayEval ( pVal, false );
			case JSON_DOUBLE_VECTOR:	return ArrayFloatEval ( pVal );
			case JSON_STRING:			return StringArrayEval ( pVal, true );
			case JSON_INT32:
			case JSON_INT64:
				iVal = ( eJson==JSON_INT32 ? sphJsonLoadInt ( &pVal ) : sphJsonLoadBigint ( &pVal ) );
				if ( m_bFloat )
					return FloatEval ( (float)iVal );
				else
					return ValueEval ( iVal  );
			case JSON_DOUBLE:
				iVal = sphJsonLoadBigint ( &pVal );
				if ( m_bFloat )
					return FloatEval ( sphQW2D ( iVal ) );
				else
					return ValueEval ( iVal  );

			case JSON_MIXED_VECTOR:
				{
					const BYTE * p = pVal;
					sphJsonUnpackInt ( &p ); // skip node length
					int iLen = sphJsonUnpackInt ( &p );
					for ( int i=0; i<iLen; i++ )
					{
						auto eType = (ESphJsonType)*p++;
						pVal = p;
						int iRes = 0;
						switch (eType)
						{
							case JSON_STRING:
								iRes =  StringArrayEval ( pVal, true );
							break;
							case JSON_INT32:
							case JSON_INT64:
								iVal = ( eType==JSON_INT32 ? sphJsonLoadInt ( &pVal ) : sphJsonLoadBigint ( &pVal ) );
								if ( m_bFloat )
									iRes = FloatEval ( (float)iVal );
								else
									iRes = ValueEval ( iVal );
							break;
							case JSON_DOUBLE:
								iVal = sphJsonLoadBigint ( &pVal );
								if ( m_bFloat )
									iRes = FloatEval ( sphQW2D ( iVal ) );
								else
									iRes = ValueEval ( iVal  );
								break;
							default: break; // for weird subobjects, just let IN() return false
						}
						if ( iRes )
							return 1;
						sphJsonSkipNode ( eType, &p );
					}
					return 0;
				}
			default:					return 0;
		}
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_JsonFieldIn_c");
		return CALC_PARENT_HASH_EX(m_uValueHash);
	}

protected:
	const BYTE *		m_pStrings = nullptr;
	CSphVector<int64_t>	m_dHashes;

	ESphJsonType GetKey ( const BYTE ** ppKey, const CSphMatch & tMatch ) const
	{
		assert ( ppKey );
		if ( !m_pStrings )
			return JSON_EOF;
		uint64_t uValue = m_pArg->Int64Eval ( tMatch );
		*ppKey = m_pStrings + ( uValue & 0xffffffff );
		return (ESphJsonType)( uValue >> 32 );
	}

	int ValueEval ( const int64_t iVal ) const
	{
		for ( int64_t iValue: m_dValues )
			if ( iVal==iValue )
				return 1;
		return 0;
	}

	int FloatEval ( const double fVal ) const
	{
		assert ( m_bFloat );
		for ( int64_t iFilterVal : m_dValues )
		{
			double fFilterVal = sphDW2F ( (DWORD)iFilterVal );
			if ( fabs ( fVal - fFilterVal )<=1e-6 )
				return 1;
		}
		return 0;
	}

	// cannot apply MvaEval() on unordered JSON arrays, using linear search
	template <typename T>
	int ArrayEval ( const BYTE * pVal ) const
	{
		int iLen = sphJsonUnpackInt ( &pVal );
		auto * pArray = (const T *)pVal;
		const T * pArrayMax = pArray+iLen;
		for ( int64_t dValue : m_dValues )
		{
			auto iVal = (T)dValue;
			for ( const T * m = pArray; m<pArrayMax; ++m )
				if ( iVal==*m )
					return 1;
		}
		return 0;
	}

	int StringArrayEval ( const BYTE * pVal, bool bValueEval ) const
	{
		if ( !bValueEval )
			sphJsonUnpackInt ( &pVal );
		int iCount = bValueEval ? 1 : sphJsonUnpackInt ( &pVal );

		while ( iCount-- )
		{
			int iLen = sphJsonUnpackInt ( &pVal );
			if ( m_dHashes.BinarySearch ( sphFNV64 ( pVal, iLen ) ) )
				return 1;
			pVal += iLen;
		}
		return 0;
	}

	int ArrayFloatEval ( const BYTE * pVal ) const
	{
		int iLen = sphJsonUnpackInt ( &pVal );

		for ( int64_t iFilterVal : m_dValues )
		{
			double fFilterVal = ( m_bFloat ? sphDW2F ( (DWORD)iFilterVal ) : iFilterVal );

			const BYTE * p = pVal;
			for ( int i=0; i<iLen; i++ )
			{
				double fStored = sphQW2D ( sphJsonLoadBigint ( &p ) );
				if ( fabs ( fStored - fFilterVal )<=1e-6 )
					return 1;
			}
		}
		return 0;
	}

	virtual bool IsJson ( bool & bConverted ) const override
	{
		bConverted = true;
		return true;
	}
};


class Expr_StrIn_c : public Expr_ArgVsConstSet_c<int64_t>, public ExprLocatorTraits_t
{
protected:
	const BYTE *			m_pStrings = nullptr;
	UservarIntSet_c *		m_pUservar;
	StrVec_t					m_dStringValues;
	SphStringCmp_fn			m_fnStrCmp;

public:
	Expr_StrIn_c ( const CSphAttrLocator & tLoc, int iLocator, ConstList_c * pConsts, UservarIntSet_c * pUservar, ESphCollation eCollation )
		: Expr_ArgVsConstSet_c<int64_t> ( nullptr, pConsts, false )
		, ExprLocatorTraits_t ( tLoc, iLocator )
		, m_pUservar ( pUservar )
	{
		assert ( tLoc.m_iBitOffset>=0 && tLoc.m_iBitCount>0 );
		assert ( !pConsts || !pUservar || !pConsts->m_bPackedStrings );

		m_fnStrCmp = GetCollationFn ( eCollation );

		const int64_t * pFilter = m_pUservar ? m_pUservar->Begin() : m_dValues.Begin();
		const int64_t * pFilterMax = pFilter + ( m_pUservar ? m_pUservar->GetLength() : m_dValues.GetLength() );

		if ( pConsts )
		{
			const char * sExpr = pConsts->m_sExpr.cstr ();
			int iExprLen = pConsts->m_sExpr.Length ();

			for ( const int64_t * pCur=pFilter; pCur<pFilterMax; pCur++ )
			{
				int64_t iVal = *pCur;
				auto iOfs = (int)( iVal>>32 );
				auto iLen = (int)( iVal & 0xffffffffUL );
				if ( iOfs>0 && iOfs+iLen<=iExprLen )
				{
					CSphString sRes;
					SqlUnescape ( sRes, sExpr + iOfs, iLen );
					m_dStringValues.Add ( sRes );
				}
			}
		}

		// consts are handled in Expr_ArgVsConstSet_c, we only need uservars
		if ( m_pUservar )
			m_uValueHash = sphFNV64 ( pFilter, (pFilterMax-pFilter)*sizeof(*pFilter) );
	}

	~Expr_StrIn_c() final
	{
		SafeRelease ( m_pUservar );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal;
		SphAttr_t iOfs = tMatch.GetAttr ( m_tLocator );
		if ( iOfs<=0 )
			return 0;
		int iLen = sphUnpackStr ( m_pStrings + iOfs, &pVal );

		ARRAY_FOREACH ( i, m_dStringValues )
			if ( m_fnStrCmp ( pVal, (const BYTE*)m_dStringValues[i].cstr(), STRING_PLAIN, iLen, m_dStringValues[i].Length() )==0 )
				return 1;

		return 0;
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_ArgVsConstSet_c<int64_t>::Command ( eCmd, pArg );
		ExprLocatorTraits_t::HandleCommand ( eCmd, pArg );

		if ( eCmd==SPH_EXPR_SET_STRING_POOL )
			m_pStrings = (const BYTE*)pArg;
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_StrIn_c");
		CALC_POD_HASH(m_fnStrCmp);
		return CALC_PARENT_HASH_EX(m_uValueHash);
	}
};

//////////////////////////////////////////////////////////////////////////

/// generic BITDOT() evaluator
/// first argument is a bit mask and the rest ones are bit weights
/// function returns sum of bits multiplied by their weights
/// BITDOT(5, 11, 33, 55) => 1*11 + 0*33 + 1*55 = 66
/// BITDOT(4, 11, 33, 55) => 0*11 + 0*33 + 1*55 = 55
template < typename T >
class Expr_Bitdot_c : public Expr_ArgVsSet_c<T>
{
public:
	/// take ownership of arg and turn points
	explicit Expr_Bitdot_c ( const CSphVector<ISphExpr *> & dArgs )
			: Expr_ArgVsSet_c<T> ( dArgs[0] )
	{
		for ( int i = 1; i<dArgs.GetLength (); ++i )
		{
			SafeAddRef ( dArgs[i] );
			m_dBitWeights.Add ( dArgs[i] );
		}
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		return (float) DoEval ( tMatch );
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		return (int) DoEval ( tMatch );
	}

	int64_t Int64Eval ( const CSphMatch & tMatch ) const final
	{
		return (int64_t) DoEval ( tMatch );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		Expr_ArgVsSet_c<T>::Command ( eCmd, pArg );
		ARRAY_FOREACH ( i, m_dBitWeights )
			m_dBitWeights[i]->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Bitdot_c");
		CALC_CHILD_HASHES(m_dBitWeights);
		return Expr_ArgVsSet_c<T>::CalcHash ( szClassName, tSorterSchema, uHash, bDisable );		// can't do CALC_PARENT_HASH because of gcc and templates
	}

protected:
	VecRefPtrs_t<ISphExpr *> m_dBitWeights;

	/// generic evaluate
	T DoEval ( const CSphMatch & tMatch ) const
	{
		int64_t uArg = this->m_pArg->Int64Eval ( tMatch ); // 'this' fixes gcc braindamage
		T tRes = 0;

		int iBit = 0;
		while ( uArg && iBit<m_dBitWeights.GetLength() )
		{
			if ( uArg & 1 )
				tRes += Expr_ArgVsSet_c<T>::ExprEval ( m_dBitWeights[iBit], tMatch );
			uArg >>= 1;
			iBit++;
		}

		return tRes;
	}
};

//////////////////////////////////////////////////////////////////////////

enum GeoFunc_e
{
	GEO_HAVERSINE,
	GEO_ADAPTIVE
};

typedef float (*Geofunc_fn)( float, float, float, float );

static Geofunc_fn GeodistFn ( GeoFunc_e eFunc, bool bDeg )
{
	switch ( 2*eFunc+bDeg )
	{
	case 2*GEO_HAVERSINE:		return &GeodistSphereRad;
	case 2*GEO_HAVERSINE+1:		return &GeodistSphereDeg;
	case 2*GEO_ADAPTIVE:		return &GeodistAdaptiveRad;
	case 2*GEO_ADAPTIVE+1:		return &GeodistAdaptiveDeg;
	default:;
	}
	return nullptr;
}

static float Geodist ( GeoFunc_e eFunc, bool bDeg, float lat1, float lon1, float lat2, float lon2 )
{
	return GeodistFn ( eFunc, bDeg ) ( lat1, lon1, lat2, lon2 );
}

/// geodist() - attr point, constant anchor
class Expr_GeodistAttrConst_c : public ISphExpr
{
public:
	Expr_GeodistAttrConst_c ( Geofunc_fn pFunc, float fOut, CSphAttrLocator tLat, CSphAttrLocator tLon, float fAnchorLat, float fAnchorLon, int iLat, int iLon )
		: m_pFunc ( pFunc )
		, m_fOut ( fOut )
		, m_tLat ( tLat )
		, m_tLon ( tLon )
		, m_fAnchorLat ( fAnchorLat )
		, m_fAnchorLon ( fAnchorLon )
		, m_iLat ( iLat )
		, m_iLon ( iLon )
	{}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		return m_fOut*m_pFunc ( tMatch.GetAttrFloat ( m_tLat ), tMatch.GetAttrFloat ( m_tLon ), m_fAnchorLat, m_fAnchorLon );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		sphFixupLocator ( m_tLat, pOldSchema, pNewSchema );
		sphFixupLocator ( m_tLon, pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		if ( eCmd==SPH_EXPR_GET_DEPENDENT_COLS )
		{
			static_cast < CSphVector<int>* > ( pArg )->Add ( m_iLat );
			static_cast < CSphVector<int>* > ( pArg )->Add ( m_iLon );
		}
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GeodistAttrConst_c");
		CALC_POD_HASH(m_fAnchorLat);
		CALC_POD_HASH(m_fAnchorLon);
		CALC_POD_HASH(m_fOut);
		CALC_POD_HASH(m_pFunc);
		return CALC_DEP_HASHES();
	}

private:
	Geofunc_fn		m_pFunc;
	float			m_fOut;
	CSphAttrLocator	m_tLat;
	CSphAttrLocator	m_tLon;
	float			m_fAnchorLat;
	float			m_fAnchorLon;
	int				m_iLat;
	int				m_iLon;
};

/// geodist() - expr point, constant anchor
class Expr_GeodistConst_c: public ISphExpr
{
public:
	Expr_GeodistConst_c ( Geofunc_fn pFunc, float fOut, ISphExpr * pLat, ISphExpr * pLon, float fAnchorLat, float fAnchorLon )
		: m_pFunc ( pFunc )
		, m_fOut ( fOut )
		, m_pLat ( pLat )
		, m_pLon ( pLon )
		, m_fAnchorLat ( fAnchorLat )
		, m_fAnchorLon ( fAnchorLon )
	{
		SafeAddRef ( pLat );
		SafeAddRef ( pLon );
	}

	~Expr_GeodistConst_c () final
	{
		SafeRelease ( m_pLon );
		SafeRelease ( m_pLat );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		return m_fOut*m_pFunc ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), m_fAnchorLat, m_fAnchorLon );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		m_pLat->FixupLocator ( pOldSchema, pNewSchema );
		m_pLon->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		m_pLat->Command ( eCmd, pArg );
		m_pLon->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_GeodistConst_c");
		CALC_POD_HASH(m_fAnchorLat);
		CALC_POD_HASH(m_fAnchorLon);
		CALC_POD_HASH(m_fOut);
		CALC_POD_HASH(m_pFunc);
		CALC_CHILD_HASH(m_pLat);
		CALC_CHILD_HASH(m_pLon);
		return CALC_DEP_HASHES();
	}

private:
	Geofunc_fn	m_pFunc;
	float		m_fOut;
	ISphExpr *	m_pLat;
	ISphExpr *	m_pLon;
	float		m_fAnchorLat;
	float		m_fAnchorLon;
};

/// geodist() - expr point, expr anchor
class Expr_Geodist_c: public ISphExpr
{
public:
	Expr_Geodist_c ( Geofunc_fn pFunc, float fOut, ISphExpr * pLat, ISphExpr * pLon, ISphExpr * pAnchorLat, ISphExpr * pAnchorLon )
		: m_pFunc ( pFunc )
		, m_fOut ( fOut )
		, m_pLat ( pLat )
		, m_pLon ( pLon )
		, m_pAnchorLat ( pAnchorLat )
		, m_pAnchorLon ( pAnchorLon )
	{
		SafeAddRef ( pLat );
		SafeAddRef ( pLon );
		SafeAddRef ( pAnchorLat );
		SafeAddRef ( pAnchorLon );
	}

	~Expr_Geodist_c () final
	{
		SafeRelease ( m_pAnchorLon );
		SafeRelease ( m_pAnchorLat );
		SafeRelease ( m_pLon );
		SafeRelease ( m_pLat );
	}

	float Eval ( const CSphMatch & tMatch ) const final
	{
		return m_fOut*m_pFunc ( m_pLat->Eval(tMatch), m_pLon->Eval(tMatch), m_pAnchorLat->Eval(tMatch), m_pAnchorLon->Eval(tMatch) );
	}

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) final
	{
		m_pLat->FixupLocator ( pOldSchema, pNewSchema );
		m_pLon->FixupLocator ( pOldSchema, pNewSchema );
		m_pAnchorLat->FixupLocator ( pOldSchema, pNewSchema );
		m_pAnchorLon->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) final
	{
		m_pLat->Command ( eCmd, pArg );
		m_pLon->Command ( eCmd, pArg );
		m_pAnchorLat->Command ( eCmd, pArg );
		m_pAnchorLon->Command ( eCmd, pArg );
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Geodist_c");
		CALC_POD_HASH(m_fOut);
		CALC_POD_HASH(m_pFunc);
		CALC_CHILD_HASH(m_pLat);
		CALC_CHILD_HASH(m_pLon);
		CALC_CHILD_HASH(m_pAnchorLat);
		CALC_CHILD_HASH(m_pAnchorLon);
		return CALC_DEP_HASHES();
	}

private:
	Geofunc_fn	m_pFunc;
	float		m_fOut;
	ISphExpr *	m_pLat;
	ISphExpr *	m_pLon;
	ISphExpr *	m_pAnchorLat;
	ISphExpr *	m_pAnchorLon;
};

class Expr_Regex_c : public Expr_ArgVsSet_c<int>
{
protected:
	uint64_t m_uFilterHash = SPH_FNV64_SEED;
#if USE_RE2
	RE2 *	m_pRE2 = nullptr;
#endif

public:
	Expr_Regex_c ( ISphExpr * pAttr, ISphExpr * pString )
		: Expr_ArgVsSet_c ( pAttr )
	{
		CSphMatch tTmp;
		const BYTE * sVal = nullptr;
		int iLen = pString->StringEval ( tTmp, &sVal );
		if ( iLen )
			m_uFilterHash = sphFNV64 ( sVal, iLen );

#if USE_RE2
		re2::StringPiece tBuf ( (const char *)sVal, iLen );
		RE2::Options tOpts;
		tOpts.set_utf8 ( true );
		m_pRE2 = new RE2 ( tBuf, tOpts );
#endif
	}

	~Expr_Regex_c() final
	{
#if USE_RE2
		SafeDelete ( m_pRE2 );
#endif
	}

	int IntEval ( const CSphMatch & tMatch ) const final
	{
		int iRes = 0;
#if USE_RE2
		if ( !m_pRE2 )
			return 0;

		const BYTE * sVal = nullptr;
		int iLen = m_pArg->StringEval ( tMatch, &sVal );

		re2::StringPiece tBuf ( (const char *)sVal, iLen );
		iRes = !!( RE2::PartialMatchN ( tBuf, *m_pRE2, nullptr, 0 ) );
		if ( m_pArg->IsDataPtrAttr () ) SafeDeleteArray ( sVal );
#endif

		return iRes;
	}

	uint64_t GetHash ( const ISphSchema & tSorterSchema, uint64_t uPrevHash, bool & bDisable ) final
	{
		EXPR_CLASS_NAME("Expr_Regex_c");
		uHash ^= m_uFilterHash;
		return CALC_DEP_HASHES();
	}
};


//////////////////////////////////////////////////////////////////////////

struct DistanceUnit_t
{
	CSphString	m_dNames[3];
	float		m_fConversion;
};


bool sphGeoDistanceUnit ( const char * szUnit, float & fCoeff )
{
	static DistanceUnit_t dUnits[] = 
	{
		{ { "mi", "miles" },				1609.34f },
		{ { "yd", "yards" },				0.9144f },
		{ { "ft", "feet" },					0.3048f },
		{ { "in", "inch" },					0.0254f },
		{ { "km", "kilometers" },			1000.0f },
		{ { "m", "meters" },				1.0f },
		{ { "cm", "centimeters" },			0.01f },
		{ { "mm", "millimeters" },			0.001f },
		{ { "NM", "nmi", "nauticalmiles" },	1852.0f }
	};

	if ( !szUnit || !*szUnit )
	{
		fCoeff = 1.0f;
		return true;
	}

	for ( const auto & i : dUnits )
		for ( const auto & j : i.m_dNames )
			if ( j==szUnit )
			{
				fCoeff = i.m_fConversion;
				return true;
			}

	return false;
}


struct GatherArgTypes_t : ISphNoncopyable
{
	CSphVector<int> & m_dTypes;
	explicit GatherArgTypes_t ( CSphVector<int> & dTypes )
		: m_dTypes ( dTypes )
	{}
	void Collect ( int , const ExprNode_t & tNode )
	{
		m_dTypes.Add ( tNode.m_iToken );
	}
};

void ExprParser_t::GatherArgTypes ( int iNode, CSphVector<int> & dTypes )
{
	GatherArgTypes_t tCollector ( dTypes );
	GatherArgT ( iNode, tCollector );
}

struct GatherArgNodes_t : ISphNoncopyable
{
	CSphVector<int> & m_dNodes;
	explicit GatherArgNodes_t ( CSphVector<int> & dNodes )
		: m_dNodes ( dNodes )
	{}
	void Collect ( int iNode, const ExprNode_t & )
	{
		m_dNodes.Add ( iNode );
	}
};

void ExprParser_t::GatherArgNodes ( int iNode, CSphVector<int> & dNodes )
{
	GatherArgNodes_t tCollector ( dNodes );
	GatherArgT ( iNode, tCollector );
}

struct GatherArgReturnTypes_t : ISphNoncopyable
{
	CSphVector<ESphAttr> & m_dTypes;
	explicit GatherArgReturnTypes_t ( CSphVector<ESphAttr> & dTypes )
		: m_dTypes ( dTypes )
	{}
	void Collect ( int , const ExprNode_t & tNode )
	{
		m_dTypes.Add ( tNode.m_eRetType );
	}
};

void ExprParser_t::GatherArgRetTypes ( int iNode, CSphVector<ESphAttr> & dTypes )
{
	GatherArgReturnTypes_t tCollector ( dTypes );
	GatherArgT ( iNode, tCollector );
}

template < typename T >
void ExprParser_t::GatherArgT ( int iNode, T & FUNCTOR )
{
	if ( iNode<0 )
		return;

	m_dGatherStack.Resize ( 0 );
	StackNode_t & tInitial = m_dGatherStack.Add();
	const ExprNode_t & tNode = m_dNodes[iNode];
	tInitial.m_iNode = iNode;
	tInitial.m_iLeft = tNode.m_iLeft;
	tInitial.m_iRight = tNode.m_iRight;

	while ( m_dGatherStack.GetLength()>0 )
	{
		StackNode_t & tCur = m_dGatherStack.Last();
		const ExprNode_t & tCurExprNode = m_dNodes[tCur.m_iNode];
		if ( tCurExprNode.m_iToken!=',' )
		{
			FUNCTOR.Collect ( tCur.m_iNode, tCurExprNode );
			m_dGatherStack.Pop();
			continue;
		}
		if ( tCur.m_iLeft==-1 && tCur.m_iRight==-1 )
		{
			m_dGatherStack.Pop();
			continue;
		}

		int iChild = -1;
		if ( tCur.m_iLeft>=0 )
		{
			iChild = tCur.m_iLeft;
			tCur.m_iLeft = -1;
		} else if ( tCur.m_iRight>=0 )
		{
			iChild = tCur.m_iRight;
			tCur.m_iRight = -1;
		} else
			continue;

		assert ( iChild>=0 );
		const ExprNode_t & tChild = m_dNodes[iChild];
		StackNode_t & tNext = m_dGatherStack.Add();
		tNext.m_iNode = iChild;
		tNext.m_iLeft = tChild.m_iLeft;
		tNext.m_iRight = tChild.m_iRight;
	}
}

bool ExprParser_t::CheckForConstSet ( int iArgsNode, int iSkip )
{
	CSphVector<int> dTypes;
	GatherArgTypes ( iArgsNode, dTypes );

	for ( int i=iSkip; i<dTypes.GetLength(); i++ )
		if ( dTypes[i]!=TOK_CONST_INT && dTypes[i]!=TOK_CONST_FLOAT && dTypes[i]!=TOK_MAP_ARG )
			return false;
	return true;
}


template < typename T >
void ExprParser_t::WalkTree ( int iRoot, T & FUNCTOR )
{
	if ( iRoot>=0 )
	{
		const ExprNode_t & tNode = m_dNodes[iRoot];
		FUNCTOR.Enter ( tNode, m_dNodes );
		WalkTree ( tNode.m_iLeft, FUNCTOR );
		WalkTree ( tNode.m_iRight, FUNCTOR );
		FUNCTOR.Exit ( tNode );
	}
}


ISphExpr * ExprParser_t::CreateIntervalNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs )
{
	assert ( dArgs.GetLength()>=2 );

	CSphVector<ESphAttr> dTypes;
	GatherArgRetTypes ( iArgsNode, dTypes );

	// force type conversion, where possible
	if ( dTypes[0]==SPH_ATTR_JSON_FIELD )
	{
		auto pConverted = new Expr_JsonFieldConv_c ( dArgs[0] );
		SafeRelease ( dArgs[0] );
		dArgs[0] = pConverted;
	}

	bool bConst = CheckForConstSet ( iArgsNode, 1 );
	ESphAttr eAttrType = m_dNodes[iArgsNode].m_eArgType;
	if ( bConst )
	{
		switch ( eAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_IntervalConst_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_IntervalConst_c<int64_t> ( dArgs ); break;
			default:				return new Expr_IntervalConst_c<float> ( dArgs ); break;
		}
	} else
	{
		switch ( eAttrType )
		{
			case SPH_ATTR_INTEGER:	return new Expr_Interval_c<int> ( dArgs ); break;
			case SPH_ATTR_BIGINT:	return new Expr_Interval_c<int64_t> ( dArgs ); break;
			default:				return new Expr_Interval_c<float> ( dArgs ); break;
		}
	}
#if !USE_WINDOWS
	return nullptr;
#endif
}


ISphExpr * ExprParser_t::CreateInNode ( int iNode )
{
	const ExprNode_t & tLeft = m_dNodes[m_dNodes[iNode].m_iLeft];
	const ExprNode_t & tRight = m_dNodes[m_dNodes[iNode].m_iRight];

	switch ( tRight.m_iToken )
	{
		// create IN(arg,constlist)
		case TOK_CONST_LIST:
			switch ( tLeft.m_iToken )
			{
				case TOK_ATTR_MVA32:
					return new Expr_MVAIn_c<false> ( tLeft.m_tLocator, tLeft.m_iLocator, tRight.m_pConsts, nullptr );
				case TOK_ATTR_MVA64:
					return new Expr_MVAIn_c<true> ( tLeft.m_tLocator, tLeft.m_iLocator, tRight.m_pConsts, nullptr );
				case TOK_ATTR_STRING:
					return new Expr_StrIn_c ( tLeft.m_tLocator, tLeft.m_iLocator, tRight.m_pConsts, nullptr, m_eCollation );
				case TOK_ATTR_JSON:
					return new Expr_JsonFieldIn_c ( tRight.m_pConsts, CSphRefcountedPtr<ISphExpr> { CreateTree ( m_dNodes [ iNode ].m_iLeft ) } );
				default:
				{
					CSphRefcountedPtr<ISphExpr> pArg ( CreateTree ( m_dNodes[iNode].m_iLeft ) );
					switch ( WidestType ( tLeft.m_eRetType, tRight.m_pConsts->m_eRetType ) )
					{
						case SPH_ATTR_INTEGER:	return new Expr_In_c<int> ( pArg, tRight.m_pConsts ); break;
						case SPH_ATTR_BIGINT:	return new Expr_In_c<int64_t> ( pArg, tRight.m_pConsts ); break;
						default:				return new Expr_In_c<float> ( pArg, tRight.m_pConsts ); break;
					}
				}
			}
			break;

		// create IN(arg,uservar)
		case TOK_USERVAR:
		{
			if ( !g_pUservarsHook )
			{
				m_sCreateError.SetSprintf ( "internal error: no uservars hook" );
				return nullptr;
			}

			UservarIntSet_c * pUservar = g_pUservarsHook ( m_dUservars[(int)tRight.m_iConst] );
			if ( !pUservar )
			{
				m_sCreateError.SetSprintf ( "undefined user variable '%s'", m_dUservars[(int)tRight.m_iConst].cstr() );
				return nullptr;
			}

			switch ( tLeft.m_iToken )
			{
				case TOK_ATTR_MVA32:
					return new Expr_MVAIn_c<false> ( tLeft.m_tLocator, tLeft.m_iLocator, nullptr, pUservar );
				case TOK_ATTR_MVA64:
					return new Expr_MVAIn_c<true> ( tLeft.m_tLocator, tLeft.m_iLocator, nullptr, pUservar );
				case TOK_ATTR_STRING:
					return new Expr_StrIn_c ( tLeft.m_tLocator, tLeft.m_iLocator, nullptr, pUservar, m_eCollation );
				case TOK_ATTR_JSON:
					return new Expr_JsonFieldIn_c ( pUservar, CSphRefcountedPtr<ISphExpr> { CreateTree ( m_dNodes[iNode].m_iLeft ) } );
				default:
					return new Expr_InUservar_c ( CSphRefcountedPtr<ISphExpr> { CreateTree ( m_dNodes[iNode].m_iLeft ) }, pUservar );
			}
			break;
		}

		// oops, unhandled case
		default:
			m_sCreateError = "IN() arguments must be constants (except the 1st one)";
			return nullptr;
	}
}


ISphExpr * ExprParser_t::CreateLengthNode ( const ExprNode_t & tNode, ISphExpr * pLeft )
{
	const ExprNode_t & tLeft = m_dNodes [ tNode.m_iLeft ];
	switch ( tLeft.m_iToken )
	{
		case TOK_FUNC:
			return new Expr_StrLength_c ( pLeft );
		case TOK_ATTR_MVA32:
		case TOK_ATTR_MVA64:
			return new Expr_MVALength_c ( tLeft.m_tLocator, tLeft.m_iLocator, tLeft.m_iToken==TOK_ATTR_MVA64 );
		case TOK_ATTR_JSON:
			return new Expr_JsonFieldLength_c ( pLeft );
		default:
			m_sCreateError = "LENGTH() argument must be MVA or JSON field";
			return nullptr;
	}
}


ISphExpr * ExprParser_t::CreateGeodistNode ( int iArgs )
{
	CSphVector<int> dArgs;
	GatherArgNodes ( iArgs, dArgs );
	assert ( dArgs.GetLength()==4 || dArgs.GetLength()==5 );

	float fOut = 1.0f; // result scale, defaults to out=meters
	bool bDeg = false; // arg units, defaults to in=radians
	GeoFunc_e eMethod = GEO_ADAPTIVE; // geodist function to use, defaults to adaptive

	if ( dArgs.GetLength()==5 )
	{
		assert ( m_dNodes [ dArgs[4] ].m_eRetType==SPH_ATTR_MAPARG );
		CSphVector<CSphNamedVariant> & dOpts = m_dNodes [ dArgs[4] ].m_pMapArg->m_dPairs;

		// FIXME! handle errors in options somehow?
		ARRAY_FOREACH ( i, dOpts )
		{
			const CSphNamedVariant & t = dOpts[i];
			if ( t.m_sKey=="in" )
			{
				if ( t.m_sValue=="deg" || t.m_sValue=="degrees" )
					bDeg = true;
				else if ( t.m_sValue=="rad" || t.m_sValue=="radians" )
					bDeg = false;

			} else if ( t.m_sKey=="out" )
			{
				float fCoeff = 1.0f;
				if ( sphGeoDistanceUnit ( t.m_sValue.cstr(), fCoeff ) )
					fOut = 1.0f / fCoeff;
			} else if ( t.m_sKey=="method" )
			{
				if ( t.m_sValue=="haversine" )
					eMethod = GEO_HAVERSINE;
				else if ( t.m_sValue=="adaptive" )
					eMethod = GEO_ADAPTIVE;
			}
		}
	}

	bool bConst1 = ( IsConst ( &m_dNodes[dArgs[0]] ) && IsConst ( &m_dNodes[dArgs[1]] ) );
	bool bConst2 = ( IsConst ( &m_dNodes[dArgs[2]] ) && IsConst ( &m_dNodes[dArgs[3]] ) );

	if ( bConst1 && bConst2 )
	{
		float t[4];
		for ( int i=0; i<4; i++ )
			t[i] = FloatVal ( &m_dNodes[dArgs[i]] );
		return new Expr_GetConst_c ( fOut*Geodist ( eMethod, bDeg, t[0], t[1], t[2], t[3] ) );
	}

	if ( bConst1 )
	{
		Swap ( dArgs[0], dArgs[2] );
		Swap ( dArgs[1], dArgs[3] );
		Swap ( bConst1, bConst2 );
	}

	if ( bConst2 )
	{
		// constant anchor
		if ( m_dNodes[dArgs[0]].m_iToken==TOK_ATTR_FLOAT && m_dNodes[dArgs[1]].m_iToken==TOK_ATTR_FLOAT )
		{
			// attr point
			return new Expr_GeodistAttrConst_c ( GeodistFn ( eMethod, bDeg ), fOut,
				m_dNodes[dArgs[0]].m_tLocator, m_dNodes[dArgs[1]].m_tLocator,
				FloatVal ( &m_dNodes[dArgs[2]] ), FloatVal ( &m_dNodes[dArgs[3]] ),
				m_dNodes[dArgs[0]].m_iLocator, m_dNodes[dArgs[1]].m_iLocator );
		} else
		{
			CSphRefcountedPtr<ISphExpr> pAttr0 { ConvertExprJson ( CreateTree ( dArgs[0] ) ) };
			CSphRefcountedPtr<ISphExpr> pAttr1 { ConvertExprJson ( CreateTree ( dArgs[1] ) ) };

			// expr point
			return new Expr_GeodistConst_c ( GeodistFn ( eMethod, bDeg ), fOut,
				pAttr0, pAttr1,
				FloatVal ( &m_dNodes[dArgs[2]] ), FloatVal ( &m_dNodes[dArgs[3]] ) );
		}
	}

	// four expressions
	VecRefPtrs_t<ISphExpr *> dExpr;
	MoveToArgList ( CreateTree ( iArgs ), dExpr );
	assert ( dExpr.GetLength()==4 );
	ConvertArgsJson ( dExpr );
	return new Expr_Geodist_c ( GeodistFn ( eMethod, bDeg ), fOut, dExpr[0], dExpr[1], dExpr[2], dExpr[3] );
}


ISphExpr * ExprParser_t::CreatePFNode ( int iArg )
{
	m_eEvalStage = SPH_EVAL_FINAL;

	DWORD uNodeFactorFlags = SPH_FACTOR_ENABLE | SPH_FACTOR_CALC_ATC;

	CSphVector<int> dArgs;
	GatherArgNodes ( iArg, dArgs );
	assert ( dArgs.GetLength()==0 || dArgs.GetLength()==1 );

	bool bNoATC = false;
	bool bJsonOut = false;

	if ( dArgs.GetLength()==1 )
	{
		assert ( m_dNodes[dArgs[0]].m_eRetType==SPH_ATTR_MAPARG );
		CSphVector<CSphNamedVariant> & dOpts = m_dNodes[dArgs[0]].m_pMapArg->m_dPairs;
	
		ARRAY_FOREACH ( i, dOpts )
		{
			if ( dOpts[i].m_sKey=="no_atc" && dOpts[i].m_iValue>0)
				bNoATC = true;
			else if ( dOpts[i].m_sKey=="json" && dOpts[i].m_iValue>0 )
				bJsonOut = true;
		}
	}

	if ( bNoATC )
		uNodeFactorFlags &= ~SPH_FACTOR_CALC_ATC;
	if ( bJsonOut )
		uNodeFactorFlags |= SPH_FACTOR_JSON_OUT;

	m_uPackedFactorFlags |= uNodeFactorFlags;

	return new Expr_GetPackedFactors_c();
}



ISphExpr * ExprParser_t::CreateBitdotNode ( int iArgsNode, CSphVector<ISphExpr *> & dArgs )
{
	assert ( dArgs.GetLength()>=1 );

	ESphAttr eAttrType = m_dNodes[iArgsNode].m_eRetType;
	switch ( eAttrType )
	{
		case SPH_ATTR_INTEGER:	return new Expr_Bitdot_c<int> ( dArgs ); break;
		case SPH_ATTR_BIGINT:	return new Expr_Bitdot_c<int64_t> ( dArgs ); break;
		default:				return new Expr_Bitdot_c<float> ( dArgs ); break;
	}
}


ISphExpr * ExprParser_t::CreateAggregateNode ( const ExprNode_t & tNode, ESphAggrFunc eFunc, ISphExpr * pLeft )
{
	const ExprNode_t & tLeft = m_dNodes [ tNode.m_iLeft ];
	switch ( tLeft.m_iToken )
	{
		case TOK_ATTR_JSON:		return new Expr_JsonFieldAggr_c ( pLeft, eFunc );
		case TOK_ATTR_MVA32:	return new Expr_MVAAggr_c<false> ( tLeft.m_tLocator, tLeft.m_iLocator, eFunc );
		case TOK_ATTR_MVA64:	return new Expr_MVAAggr_c<true> ( tLeft.m_tLocator, tLeft.m_iLocator, eFunc );
		default:				return nullptr;
	}
}


void ExprParser_t::FixupIterators ( int iNode, const char * sKey, SphAttr_t * pAttr )
{
	if ( iNode==-1 )
		return;

	ExprNode_t & tNode = m_dNodes[iNode];

	if ( tNode.m_iToken==TOK_IDENT && !strcmp ( sKey, tNode.m_sIdent ) )
	{
		tNode.m_iToken = TOK_ITERATOR;
		tNode.m_pAttr = pAttr;
	}

	FixupIterators ( tNode.m_iLeft, sKey, pAttr );
	FixupIterators ( tNode.m_iRight, sKey, pAttr );
}


ISphExpr * ExprParser_t::CreateForInNode ( int iNode )
{
	ExprNode_t & tNode = m_dNodes[iNode];

	int iFunc = tNode.m_iFunc;
	int iExprNode = tNode.m_iLeft;
	int iNameNode = tNode.m_iRight;
	int iDataNode = m_dNodes[iNameNode].m_iLeft;

	auto * pFunc = new Expr_ForIn_c ( CSphRefcountedPtr<ISphExpr> { CreateTree ( iDataNode )} , iFunc==FUNC_ALL, iFunc==FUNC_INDEXOF );

	FixupIterators ( iExprNode, m_dNodes[iNameNode].m_sIdent, pFunc->GetRef() );
	pFunc->SetExpr ( CSphRefcountedPtr<ISphExpr> { CreateTree ( iExprNode ) } );

	return pFunc;
}

ISphExpr * ExprParser_t::CreateRegexNode ( ISphExpr * pAttr, ISphExpr * pString )
{
	return new Expr_Regex_c ( pAttr, pString );
}

//////////////////////////////////////////////////////////////////////////

int yylex ( YYSTYPE * lvalp, ExprParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( ExprParser_t * pParser, const char * sMessage )
{
	pParser->m_sParserError.SetSprintf ( "Sphinx expr: %s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#if USE_WINDOWS
#pragma warning(push,1)
#endif

#ifdef CMAKE_GENERATED_GRAMMAR
	#include "bissphinxexpr.c"
#else
	#include "yysphinxexpr.c"
#endif

#if USE_WINDOWS
#pragma warning(pop)
#endif

//////////////////////////////////////////////////////////////////////////

ExprParser_t::~ExprParser_t ()
{
	// i kinda own those things
	ARRAY_FOREACH ( i, m_dNodes )
	{
		if ( m_dNodes[i].m_iToken==TOK_CONST_LIST )
			SafeDelete ( m_dNodes[i].m_pConsts );
		if ( m_dNodes[i].m_iToken==TOK_MAP_ARG )
			SafeDelete ( m_dNodes[i].m_pMapArg );
	}

	// free any UDF calls that weren't taken over
	ARRAY_FOREACH ( i, m_dUdfCalls )
		SafeDelete ( m_dUdfCalls[i] );

	// free temp map arguments storage
	ARRAY_FOREACH ( i, m_dIdents )
		SafeDeleteArray ( m_dIdents[i] );
}

ESphAttr ExprParser_t::GetWidestRet ( int iLeft, int iRight )
{
	ESphAttr uLeftType = ( iLeft<0 ) ? SPH_ATTR_INTEGER : m_dNodes[iLeft].m_eRetType;
	ESphAttr uRightType = ( iRight<0 ) ? SPH_ATTR_INTEGER : m_dNodes[iRight].m_eRetType;

	if ( uLeftType==SPH_ATTR_INTEGER && uRightType==SPH_ATTR_INTEGER )
		return SPH_ATTR_INTEGER;

	if ( IsInt ( uLeftType ) && IsInt ( uRightType ) )
		return SPH_ATTR_BIGINT;

	// if json vs numeric then return numeric type (for the autoconversion)
	if ( uLeftType==SPH_ATTR_JSON_FIELD && IsNumeric ( uRightType ) )
		return uRightType;

	if ( uRightType==SPH_ATTR_JSON_FIELD && IsNumeric ( uLeftType ) )
		return uLeftType;

	return SPH_ATTR_FLOAT;
}

int ExprParser_t::AddNodeInt ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_INT;
	tNode.m_eRetType = GetIntType ( iValue );
	tNode.m_iConst = iValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeFloat ( float fValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_FLOAT;
	tNode.m_eRetType = SPH_ATTR_FLOAT;
	tNode.m_fConst = fValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeString ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_STRING;
	tNode.m_eRetType = SPH_ATTR_STRING;
	tNode.m_iConst = iValue;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeAttr ( int iTokenType, uint64_t uAttrLocator )
{
	assert ( iTokenType==TOK_ATTR_INT || iTokenType==TOK_ATTR_BITS || iTokenType==TOK_ATTR_FLOAT
		|| iTokenType==TOK_ATTR_MVA32 || iTokenType==TOK_ATTR_MVA64 || iTokenType==TOK_ATTR_STRING
		|| iTokenType==TOK_ATTR_FACTORS || iTokenType==TOK_ATTR_JSON );
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iTokenType;
	sphUnpackAttrLocator ( uAttrLocator, &tNode );

	switch ( iTokenType )
	{
	case TOK_ATTR_FLOAT:	tNode.m_eRetType = SPH_ATTR_FLOAT;		break;
	case TOK_ATTR_MVA32:	tNode.m_eRetType = SPH_ATTR_UINT32SET;	break;
	case TOK_ATTR_MVA64:	tNode.m_eRetType = SPH_ATTR_INT64SET;	break;
	case TOK_ATTR_STRING:	tNode.m_eRetType = SPH_ATTR_STRING;		break;
	case TOK_ATTR_FACTORS:	tNode.m_eRetType = SPH_ATTR_FACTORS;	break;
	case TOK_ATTR_JSON:		tNode.m_eRetType = SPH_ATTR_JSON_FIELD;	break;
	default:
		tNode.m_eRetType = ( tNode.m_tLocator.m_iBitCount>32 ) ? SPH_ATTR_BIGINT : SPH_ATTR_INTEGER;
	}
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeID ()
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_ID;
	tNode.m_eRetType = SPH_ATTR_BIGINT;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeWeight ()
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_WEIGHT;
	tNode.m_eRetType = SPH_ATTR_BIGINT;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeOp ( int iOp, int iLeft, int iRight )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = iOp;

	// deduce type
	tNode.m_eRetType = SPH_ATTR_FLOAT; // default to float
	if ( iOp==TOK_NEG )
	{
		// NEG just inherits the type
		tNode.m_eArgType = m_dNodes[iLeft].m_eRetType;
		tNode.m_eRetType = tNode.m_eArgType;
	} else if ( iOp==TOK_NOT )
	{
		// NOT result is integer, and its argument must be integer
		tNode.m_eArgType = m_dNodes[iLeft].m_eRetType;
		tNode.m_eRetType = SPH_ATTR_INTEGER;
		if ( !IsInt ( tNode.m_eArgType ))
		{
			m_sParserError.SetSprintf ( "NOT argument must be integer" );
			return -1;
		}
	} else if ( iOp==TOK_NOT )
	{


	} else if ( iOp==TOK_LTE || iOp==TOK_GTE || iOp==TOK_EQ || iOp==TOK_NE
		|| iOp=='<' || iOp=='>' || iOp==TOK_AND || iOp==TOK_OR
		|| iOp=='+' || iOp=='-' || iOp=='*' || iOp==','
		|| iOp=='&' || iOp=='|' || iOp=='%'
		|| iOp==TOK_IS_NULL || iOp==TOK_IS_NOT_NULL )
	{
		tNode.m_eArgType = GetWidestRet ( iLeft, iRight );

		// arithmetical operations return arg type, logical return int
		tNode.m_eRetType = ( iOp=='+' || iOp=='-' || iOp=='*' || iOp==',' || iOp=='&' || iOp=='|' || iOp=='%' )
			? tNode.m_eArgType
			: SPH_ATTR_INTEGER;

		// both logical and bitwise AND/OR can only be over ints
		if ( ( iOp==TOK_AND || iOp==TOK_OR || iOp=='&' || iOp=='|' )
			&& !IsInt ( tNode.m_eArgType ))
		{
			m_sParserError.SetSprintf ( "%s arguments must be integer", ( iOp==TOK_AND || iOp=='&' ) ? "AND" : "OR" );
			return -1;
		}

		// MOD can only be over ints
		if ( iOp=='%' && !IsInt( tNode.m_eArgType ))
		{
			m_sParserError.SetSprintf ( "MOD arguments must be integer" );
			return -1;
		}

	} else
	{
		// check for unknown op
		assert ( iOp=='/' && "unknown op in AddNodeOp() type deducer" );
	}

	tNode.m_iArgs = 0;
	if ( iOp==',' )
	{
		if ( iLeft>=0 )		tNode.m_iArgs += ( m_dNodes[iLeft].m_iToken==',' ) ? m_dNodes[iLeft].m_iArgs : 1;
		if ( iRight>=0 )	tNode.m_iArgs += ( m_dNodes[iRight].m_iToken==',' ) ? m_dNodes[iRight].m_iArgs : 1;
	}

	// argument type conversion for functions like INDEXOF(), ALL() and ANY()
	// we need no conversion for operands of comma!
	if ( iOp!=',' && iLeft>=0 && iRight>=0 )
	{
		if ( m_dNodes[iRight].m_eRetType==SPH_ATTR_STRING && m_dNodes[iLeft].m_iToken==TOK_IDENT )
			m_dNodes[iLeft].m_eRetType = SPH_ATTR_STRING;
		else if ( m_dNodes[iLeft].m_eRetType==SPH_ATTR_STRING && m_dNodes[iRight].m_iToken==TOK_IDENT )
			m_dNodes[iRight].m_eRetType = SPH_ATTR_STRING;
	}

	tNode.m_iLeft = iLeft;
	tNode.m_iRight = iRight;
	return m_dNodes.GetLength()-1;
}

// functions without args
int ExprParser_t::AddNodeFunc0 ( int iFunc )
{
	// regular case, iFirst is entire arglist, iSecond is -1
	// special case for IN(), iFirst is arg, iSecond is constlist
	// special case for REMAP(), iFirst and iSecond are expressions, iThird and iFourth are constlists
	assert ( iFunc>=0 && iFunc<int ( sizeof ( g_dFuncs ) / sizeof ( g_dFuncs[0] ) ) );
	assert ( g_dFuncs[iFunc].m_eFunc==( Func_e ) iFunc );
	const char * sFuncName = g_dFuncs[iFunc].m_sName;

	// check args count
	int iExpectedArgc = g_dFuncs[iFunc].m_iArgs;
	if ( iExpectedArgc )
	{
		m_sParserError.SetSprintf ( "%s() called without args, %d args expected", sFuncName, iExpectedArgc );
		return -1;
	}
	// do add
	ExprNode_t &tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = -1;
	tNode.m_iRight = -1;
	tNode.m_eArgType = SPH_ATTR_INTEGER;
	tNode.m_eRetType = g_dFuncs[iFunc].m_eRet;

	// all ok
	assert ( tNode.m_eRetType!=SPH_ATTR_NONE );
	return m_dNodes.GetLength () - 1;
}

// functions with 1 arg
int ExprParser_t::AddNodeFunc ( int iFunc, int iArg )
{
	// regular case, iFirst is entire arglist, iSecond is -1
	// special case for IN(), iFirst is arg, iSecond is constlist
	// special case for REMAP(), iFirst and iSecond are expressions, iThird and iFourth are constlists
	assert ( iFunc>=0 && iFunc< int ( sizeof ( g_dFuncs )/sizeof ( g_dFuncs[0]) ) );
	auto eFunc = (Func_e)iFunc;
	assert ( g_dFuncs [ iFunc ].m_eFunc==eFunc );
	const char * sFuncName = g_dFuncs [ iFunc ].m_sName;

	// check args count
	int iExpectedArgc = g_dFuncs [ iFunc ].m_iArgs;
	int iArgc = 0;
	if ( iArg>=0 )
		iArgc = ( m_dNodes [ iArg ].m_iToken==',' ) ? m_dNodes [ iArg ].m_iArgs : 1;

	if ( iExpectedArgc<0 )
	{
		if ( iArgc<-iExpectedArgc )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, at least %d args expected", sFuncName, iArgc, -iExpectedArgc );
			return -1;
		}
	} else if ( iArgc!=iExpectedArgc )
	{
		m_sParserError.SetSprintf ( "%s() called with %d args, %d args expected", sFuncName, iArgc, iExpectedArgc );
		return -1;
	}

	// check arg types
	//
	// check for string args
	// most builtin functions take numeric args only
	bool bGotString = false, bGotMva = false;
	CSphVector<ESphAttr> dRetTypes;
	GatherArgRetTypes ( iArg, dRetTypes );
	ARRAY_FOREACH ( i, dRetTypes )
	{
		bGotString |= ( dRetTypes[i]==SPH_ATTR_STRING );
		bGotMva |= ( dRetTypes[i]==SPH_ATTR_UINT32SET || dRetTypes[i]==SPH_ATTR_INT64SET );
	}
	if ( bGotString && !( eFunc==FUNC_SUBSTRING_INDEX || eFunc==FUNC_CRC32 || eFunc==FUNC_EXIST || eFunc==FUNC_POLY2D || eFunc==FUNC_GEOPOLY2D || eFunc==FUNC_REGEX ) )
	{
		m_sParserError.SetSprintf ( "%s() arguments can not be string", sFuncName );
		return -1;
	}
	if ( bGotMva && !( eFunc==FUNC_TO_STRING || eFunc==FUNC_LENGTH || eFunc==FUNC_LEAST || eFunc==FUNC_GREATEST ) )
	{
		m_sParserError.SetSprintf ( "%s() arguments can not be MVA", sFuncName );
		return -1;
	}

	switch ( eFunc )
	{
	case FUNC_BITDOT:
		{    // check that first BITDOT arg is integer or bigint

			int iLeftmost = iArg;
			while ( m_dNodes[iLeftmost].m_iToken==',' )
				iLeftmost = m_dNodes[iLeftmost].m_iLeft;

			ESphAttr eArg = m_dNodes[iLeftmost].m_eRetType;
			if ( !IsInt ( eArg ) )
			{
				m_sParserError.SetSprintf ( "first %s() argument must be integer", sFuncName );
				return -1;
			}
		}
		break;
	case FUNC_EXIST:
		{
			int iExistLeft = m_dNodes[iArg].m_iLeft;
			int iExistRight = m_dNodes[iArg].m_iRight;
			bool bIsLeftGood = ( m_dNodes[iExistLeft].m_eRetType==SPH_ATTR_STRING );
			ESphAttr eRight = m_dNodes[iExistRight].m_eRetType;
			bool bIsRightGood = ( eRight==SPH_ATTR_INTEGER || eRight==SPH_ATTR_TIMESTAMP || eRight==SPH_ATTR_BOOL
				|| eRight==SPH_ATTR_FLOAT || eRight==SPH_ATTR_BIGINT );

			if ( !bIsLeftGood || !bIsRightGood )
			{
				if ( bIsRightGood )
					m_sParserError.SetSprintf ( "first %s() argument must be string", sFuncName );
				else
					m_sParserError.SetSprintf ( "ill-formed %s", sFuncName );
				return -1;
			}
		}
		break;
		// check that first SINT or timestamp family arg is integer
	case FUNC_SINT:
	case FUNC_DAY:
	case FUNC_MONTH:
	case FUNC_YEAR:
	case FUNC_YEARMONTH:
	case FUNC_YEARMONTHDAY:
	case FUNC_FIBONACCI:
	case FUNC_HOUR:
	case FUNC_MINUTE:
	case FUNC_SECOND:
		assert ( iArg>=0 );
		if ( m_dNodes[iArg].m_eRetType!=SPH_ATTR_INTEGER )
		{
			m_sParserError.SetSprintf ( "%s() argument must be integer", sFuncName );
			return -1;
		}
		break;
	case FUNC_CONTAINS: // check that CONTAINS args are poly, float, float
		assert ( dRetTypes.GetLength ()==3 );
		if ( dRetTypes[0]!=SPH_ATTR_POLY2D )
		{
			m_sParserError.SetSprintf ( "1st CONTAINS() argument must be a 2D polygon (see POLY2D)" );
			return -1;
		}
		if ( !( IsNumeric ( dRetTypes[1] ) || IsJson ( dRetTypes[1] ) ) || ! ( IsNumeric ( dRetTypes[2] ) || IsJson ( dRetTypes[2] ) ) )
		{
			m_sParserError.SetSprintf ( "2nd and 3rd CONTAINS() arguments must be numeric or JSON" );
			return -1;
		}
		break;
	case FUNC_POLY2D:
	case FUNC_GEOPOLY2D:
		if ( dRetTypes.GetLength ()==1 )
		{
			// handle 1 arg version, POLY2D(string-attr)
			if ( dRetTypes[0]!=SPH_ATTR_STRING && dRetTypes[0]!=SPH_ATTR_JSON_FIELD )
			{
				m_sParserError.SetSprintf ( "%s() argument must be a string or JSON field attribute", sFuncName );
				return -1;
			}
		} else if ( dRetTypes.GetLength ()<6 )
		{
			// handle 2..5 arg versions, invalid
			m_sParserError.SetSprintf ( "bad %s() argument count, must be either 1 (string) or 6+ (x/y pairs list)"
										, sFuncName );
			return -1;
		} else
		{
			// handle 6+ arg version, POLY2D(xy-list)
			if ( dRetTypes.GetLength () & 1 )
			{
				m_sParserError.SetSprintf ( "bad %s() argument count, must be even", sFuncName );
				return -1;
			}
			ARRAY_FOREACH ( i, dRetTypes )
				if ( !( IsNumeric ( dRetTypes[i] ) || IsJson ( dRetTypes[i] ) ) )
				{
					m_sParserError.SetSprintf ( "%s() argument %d must be numeric or JSON field", sFuncName, 1 + i );
					return -1;
				}
		}
		break;
	case FUNC_BM25F: // check that BM25F args are float, float [, {file_name=weight}]
		if ( dRetTypes.GetLength ()>3 )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, at most 3 args expected", sFuncName
										, dRetTypes.GetLength () );
			return -1;
		}

		if ( dRetTypes[0]!=SPH_ATTR_FLOAT || dRetTypes[1]!=SPH_ATTR_FLOAT )
		{
			m_sParserError.SetSprintf ( "%s() arguments 1,2 must be numeric", sFuncName );
			return -1;
		}

		if ( dRetTypes.GetLength ()==3 && dRetTypes[2]!=SPH_ATTR_MAPARG )
		{
			m_sParserError.SetSprintf ( "%s() argument 3 must be map", sFuncName );
			return -1;
		}
		break;
	case FUNC_SUBSTRING_INDEX:
		if ( dRetTypes.GetLength()!=3 )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, but 3 args expected", sFuncName
				, dRetTypes.GetLength () );
			return -1;
		}
		
		if ( dRetTypes[0]!=SPH_ATTR_STRING && dRetTypes[0]!=SPH_ATTR_JSON && dRetTypes[0]!=SPH_ATTR_JSON_FIELD )
		{
			m_sParserError.SetSprintf ( "%s() arguments 1 must be string or json", sFuncName );
			return -1;
		}

		if ( dRetTypes[1]!=SPH_ATTR_STRING )
		{
			m_sParserError.SetSprintf ( "%s() arguments 2 must be string", sFuncName );
			return -1;
		}

		if ( dRetTypes[2]!=SPH_ATTR_INTEGER )
		{
			m_sParserError.SetSprintf ( "%s() arguments 3 must be numeric", sFuncName );
			return -1;
		}
		break;
	case FUNC_GEODIST: // check GEODIST args count, and that optional arg 5 is a map argument
		if ( dRetTypes.GetLength ()>5 )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, at most 5 args expected", sFuncName
										, dRetTypes.GetLength () );
			return -1;
		}

		if ( dRetTypes.GetLength ()==5 && dRetTypes[4]!=SPH_ATTR_MAPARG )
		{
			m_sParserError.SetSprintf ( "%s() argument 5 must be map", sFuncName );
			return -1;
		}
		break;
	case FUNC_REGEX:
		{
#if USE_RE2
			int iLeft = m_dNodes[iArg].m_iLeft;
			ESphAttr eLeft = m_dNodes[iLeft].m_eRetType;
			bool bIsLeftGood = ( eLeft==SPH_ATTR_STRING || eLeft==SPH_ATTR_STRINGPTR || eLeft==SPH_ATTR_JSON_FIELD );
			if ( !bIsLeftGood )
			{
				m_sParserError.SetSprintf ( "first %s() argument must be string or JSON.field", sFuncName );
				return -1;
			}

			int iRight = m_dNodes[iArg].m_iRight;
			ESphAttr eRight = m_dNodes[iRight].m_eRetType;
			bool bIsRightGood = ( eRight==SPH_ATTR_STRING );
			if ( !bIsRightGood )
			{
				m_sParserError.SetSprintf ( "second %s() argument must be string", sFuncName );
				return -1;
			}
#else
			m_sParserError.SetSprintf ( "%s() used but no regexp support compiled", sFuncName );
			return -1;
#endif
		}
		break;
	default:;
	}

	// do add
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = iArg;
	tNode.m_iRight = -1;
	tNode.m_eArgType = ( iArg>=0 ) ? m_dNodes [ iArg ].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = g_dFuncs [ iFunc ].m_eRet;

	// fixup return type in a few special cases
	switch ( eFunc )
	{
	case FUNC_MIN:
	case FUNC_MAX:
	case FUNC_MADD:
	case FUNC_MUL3:
	case FUNC_ABS:
	case FUNC_IDIV:
		if ( IsJson ( tNode.m_eArgType ) ) // auto-converter from JSON field for universal (SPH_ATTR_NONE return type) nodes
			tNode.m_eRetType = SPH_ATTR_BIGINT;
		else
			tNode.m_eRetType = tNode.m_eArgType;
		break;

	case FUNC_EXIST:
		{
			ESphAttr eType = m_dNodes[m_dNodes[iArg].m_iRight].m_eRetType;
			tNode.m_eArgType = eType;
			tNode.m_eRetType = eType;
			break;
		}
	case FUNC_BIGINT:
		if ( tNode.m_eArgType==SPH_ATTR_FLOAT )
			tNode.m_eRetType = SPH_ATTR_FLOAT; // enforce if we can; FIXME! silently ignores BIGINT() on floats; should warn or raise an error
		break;
	case FUNC_IF:
	case FUNC_BITDOT:
		tNode.m_eRetType = tNode.m_eArgType;
		break;

	case FUNC_GREATEST:
	case FUNC_LEAST: // fixup MVA return type according to the leftmost argument
		{
			int iLeftmost = iArg;
			while ( m_dNodes [ iLeftmost ].m_iToken==',' )
				iLeftmost = m_dNodes [ iLeftmost ].m_iLeft;
			ESphAttr eArg = m_dNodes [ iLeftmost ].m_eRetType;
			if ( eArg==SPH_ATTR_INT64SET )
				tNode.m_eRetType = SPH_ATTR_BIGINT;
			if ( eArg==SPH_ATTR_UINT32SET )
				tNode.m_eRetType = SPH_ATTR_INTEGER;
		}
	default:;
	}

	// all ok
	assert ( tNode.m_eRetType!=SPH_ATTR_NONE );
	return m_dNodes.GetLength()-1;
}

// special branch for all/any/indexof ( expr for x in arglist )
int ExprParser_t::AddNodeFor ( int iFunc, int iExpr, int iLoop )
{
	assert ( iFunc>=0 && iFunc<int ( sizeof ( g_dFuncs ) / sizeof ( g_dFuncs[0] ) ) );
	assert ( g_dFuncs[iFunc].m_eFunc==( Func_e ) iFunc );
	const char * sFuncName = g_dFuncs[iFunc].m_sName;

	// check args count
	if ( iLoop<0 )
	{
		int iArgc = 0;
		if ( iExpr>=0 )
			iArgc = ( m_dNodes[iExpr].m_iToken==',' ) ? m_dNodes[iExpr].m_iArgs : 1;

		m_sParserError.SetSprintf ( "%s() called with %d args, at least 1 args expected", sFuncName, iArgc );
		return -1;
	}
	// do add
	ExprNode_t &tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = iExpr;
	tNode.m_iRight = iLoop;
	tNode.m_eArgType = ( iExpr>=0 ) ? m_dNodes[iExpr].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = g_dFuncs[iFunc].m_eRet;

	// all ok
	assert ( tNode.m_eRetType!=SPH_ATTR_NONE );
	return m_dNodes.GetLength () - 1;
}

int ExprParser_t::AddNodeIn ( int iArg, int iList )
{
	assert ( g_dFuncs[FUNC_IN].m_eFunc==FUNC_IN );
	const char * sFuncName = g_dFuncs[FUNC_IN].m_sName;

	// check args count
	if ( iList<0 )
	{
		m_sParserError.SetSprintf ( "%s() called with <2 args, at least 2 args expected", sFuncName );
		return -1;
	}

	// do add
	ExprNode_t &tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = FUNC_IN;
	tNode.m_iLeft = iArg;
	tNode.m_iRight = iList;
	tNode.m_eArgType = ( iArg>=0 ) ? m_dNodes[iArg].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = g_dFuncs[FUNC_IN].m_eRet;

	// all ok
	assert ( tNode.m_eRetType!=SPH_ATTR_NONE );
	return m_dNodes.GetLength () - 1;
}

int ExprParser_t::AddNodeRemap ( int iExpr1, int iExpr2, int iList1, int iList2 )
{
	assert ( g_dFuncs[FUNC_REMAP].m_eFunc==FUNC_REMAP );
	const char * sFuncName = g_dFuncs[FUNC_REMAP].m_sName;

	if ( m_dNodes[iExpr1].m_iToken==TOK_IDENT )
	{
		m_sParserError.SetSprintf ( "%s() incorrect first argument (not integer?)", sFuncName );
		return 1;
	}
	if ( m_dNodes[iExpr2].m_iToken==TOK_IDENT )
	{
		m_sParserError.SetSprintf ( "%s() incorrect second argument (not integer/float?)", sFuncName );
		return 1;
	}

	if ( !IsInt ( m_dNodes[iExpr1].m_eRetType ) )
	{
		m_sParserError.SetSprintf ( "%s() first argument should result in integer value", sFuncName );
		return -1;
	}

	ESphAttr eSecondRet = m_dNodes[iExpr2].m_eRetType;
	if ( !IsNumeric ( eSecondRet ) )
	{
		m_sParserError.SetSprintf ( "%s() second argument should result in integer or float value", sFuncName );
		return -1;
	}

	ConstList_c &tFirstList = *m_dNodes[iList1].m_pConsts;
	ConstList_c &tSecondList = *m_dNodes[iList2].m_pConsts;
	if ( tFirstList.m_dInts.GetLength ()==0 )
	{
		m_sParserError.SetSprintf ( "%s() first constlist should consist of integer values", sFuncName );
		return -1;
	}
	if ( tFirstList.m_dInts.GetLength ()!=tSecondList.m_dInts.GetLength () &&
		tFirstList.m_dInts.GetLength ()!=tSecondList.m_dFloats.GetLength () )
	{
		m_sParserError.SetSprintf ( "%s() both constlists should have the same length", sFuncName );
		return -1;
	}

	if ( eSecondRet==SPH_ATTR_FLOAT && tSecondList.m_dFloats.GetLength ()==0 )
	{
		m_sParserError.SetSprintf ( "%s() second argument results in float value and thus fourth argument should be a list of floats" , sFuncName );
		return -1;
	}
	if ( eSecondRet!=SPH_ATTR_FLOAT && tSecondList.m_dInts.GetLength ()==0 )
	{
		m_sParserError.SetSprintf ("%s() second argument results in integer value and thus fourth argument should be a list of integers", sFuncName );
		return -1;
	}

	// do add
	ExprNode_t &tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = FUNC_REMAP;
	tNode.m_iLeft = iExpr1;
	tNode.m_iRight = iExpr2;
	tNode.m_eArgType = m_dNodes[iExpr1].m_eRetType;
	tNode.m_eRetType = m_dNodes[iExpr2].m_eRetType;
	return m_dNodes.GetLength () - 1;
}

// functions RAND with 0 or 1 arg
int ExprParser_t::AddNodeRand ( int iArg )
{
	assert ( g_dFuncs[FUNC_RAND].m_eFunc==FUNC_RAND );
	const char * sFuncName = g_dFuncs[FUNC_RAND].m_sName;

	if ( iArg>=0 )
	{
		if ( !IsNumeric ( m_dNodes[iArg].m_eRetType ) )
		{
			m_sParserError.SetSprintf ( "%s() argument must be numeric", sFuncName );
			return -1;
		}
		int iArgc = ( m_dNodes[iArg].m_iToken==',' ) ? m_dNodes[iArg].m_iArgs : 1;
		if ( iArgc>1 )
		{
			m_sParserError.SetSprintf ( "%s() called with %d args, either 0 or 1 args expected", sFuncName, iArgc );
			return -1;
		}
	}

	// do add
	ExprNode_t &tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = FUNC_RAND;
	tNode.m_iLeft = iArg;
	tNode.m_iRight = -1;
	tNode.m_eArgType = ( iArg>=0 ) ? m_dNodes[iArg].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = g_dFuncs[FUNC_RAND].m_eRet;

	// all ok
	assert ( tNode.m_eRetType!=SPH_ATTR_NONE );
	return m_dNodes.GetLength () - 1;
}

int ExprParser_t::AddNodeUdf ( int iCall, int iArg )
{
	UdfCall_t * pCall = m_dUdfCalls[iCall];
	SPH_UDF_INIT & tInit = pCall->m_tInit;
	SPH_UDF_ARGS & tArgs = pCall->m_tArgs;

	// initialize UDF right here, at AST creation stage
	// just because it's easy to gather arg types here
	if ( iArg>=0 )
	{
		// gather arg types
		CSphVector<DWORD> dArgTypes;

		int iCur = iArg;
		while ( iCur>=0 )
		{
			if ( m_dNodes[iCur].m_iToken!=',' )
			{
				const ExprNode_t & tNode = m_dNodes[iCur];
				if ( tNode.m_iToken==TOK_FUNC && ( tNode.m_iFunc==FUNC_PACKEDFACTORS || tNode.m_iFunc==FUNC_RANKFACTORS || tNode.m_iFunc==FUNC_FACTORS ) )
					pCall->m_dArgs2Free.Add ( dArgTypes.GetLength() );
				if ( tNode.m_eRetType==SPH_ATTR_JSON || tNode.m_eRetType==SPH_ATTR_JSON_FIELD )
					pCall->m_dArgs2Free.Add ( dArgTypes.GetLength() );
				dArgTypes.Add ( tNode.m_eRetType );
				break;
			}

			int iRight = m_dNodes[iCur].m_iRight;
			if ( iRight>=0 )
			{
				const ExprNode_t & tNode = m_dNodes[iRight];
				assert ( tNode.m_iToken!=',' );
				if ( tNode.m_iToken==TOK_FUNC && ( tNode.m_iFunc==FUNC_PACKEDFACTORS || tNode.m_iFunc==FUNC_RANKFACTORS || tNode.m_iFunc==FUNC_FACTORS) )
					pCall->m_dArgs2Free.Add ( dArgTypes.GetLength() );
				if ( tNode.m_eRetType==SPH_ATTR_JSON || tNode.m_eRetType==SPH_ATTR_JSON_FIELD )
					pCall->m_dArgs2Free.Add ( dArgTypes.GetLength() );
				dArgTypes.Add ( tNode.m_eRetType );
			}

			iCur = m_dNodes[iCur].m_iLeft;
		}

		assert ( dArgTypes.GetLength() );
		tArgs.arg_count = dArgTypes.GetLength();
		tArgs.arg_types = new sphinx_udf_argtype [ tArgs.arg_count ];

		// we gathered internal type ids in right-to-left order
		// reverse and remap
		// FIXME! eliminate remap, maybe?
		ARRAY_FOREACH ( i, dArgTypes )
		{
			sphinx_udf_argtype & eRes = tArgs.arg_types [ tArgs.arg_count-1-i ];
			switch ( dArgTypes[i] )
			{
				case SPH_ATTR_INTEGER:
				case SPH_ATTR_TIMESTAMP:
				case SPH_ATTR_BOOL:
					eRes = SPH_UDF_TYPE_UINT32;
					break;
				case SPH_ATTR_FLOAT:
					eRes = SPH_UDF_TYPE_FLOAT;
					break;
				case SPH_ATTR_BIGINT:
					eRes = SPH_UDF_TYPE_INT64;
					break;
				case SPH_ATTR_STRING:
					eRes = SPH_UDF_TYPE_STRING;
					break;
				case SPH_ATTR_UINT32SET:
					eRes = SPH_UDF_TYPE_UINT32SET;
					break;
				case SPH_ATTR_INT64SET:
					eRes = SPH_UDF_TYPE_UINT64SET;
					break;
				case SPH_ATTR_FACTORS:
					eRes = SPH_UDF_TYPE_FACTORS;
					break;
				case SPH_ATTR_JSON_FIELD:
					eRes = SPH_UDF_TYPE_JSON;
					break;
				default:
					m_sParserError.SetSprintf ( "internal error: unmapped UDF argument type (arg=%d, type=%d)", i, dArgTypes[i] );
					return -1;
			}
		}

		ARRAY_FOREACH ( i, pCall->m_dArgs2Free )
			pCall->m_dArgs2Free[i] = tArgs.arg_count - 1 - pCall->m_dArgs2Free[i];
	}

	// init
	if ( pCall->m_pUdf->m_fnInit )
	{
		char sError [ SPH_UDF_ERROR_LEN ];
		if ( pCall->m_pUdf->m_fnInit ( &tInit, &tArgs, sError ) )
		{
			m_sParserError = sError;
			return -1;
		}
	}

	// do add
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_UDF;
	tNode.m_iFunc = iCall;
	tNode.m_iLeft = iArg;
	tNode.m_iRight = -1;

	// deduce type
	tNode.m_eArgType = ( iArg>=0 ) ? m_dNodes[iArg].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = pCall->m_pUdf->m_eRetType;
	return m_dNodes.GetLength()-1;
}

int	ExprParser_t::AddNodePF ( int iFunc, int iArg )
{
	assert ( iFunc>=0 && iFunc< int ( sizeof ( g_dFuncs )/sizeof ( g_dFuncs[0]) ) );
	const char * sFuncName = g_dFuncs [ iFunc ].m_sName;

	CSphVector<ESphAttr> dRetTypes;
	GatherArgRetTypes ( iArg, dRetTypes );

	assert ( dRetTypes.GetLength()==0 || dRetTypes.GetLength()==1 );

	if ( dRetTypes.GetLength()==1 && dRetTypes[0]!=SPH_ATTR_MAPARG )
	{
		m_sParserError.SetSprintf ( "%s() argument must be a map", sFuncName );
		return -1;
	}

	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_FUNC;
	tNode.m_iFunc = iFunc;
	tNode.m_iLeft = iArg;
	tNode.m_iRight = -1;
	tNode.m_eArgType = SPH_ATTR_MAPARG;
	tNode.m_eRetType = g_dFuncs[iFunc].m_eRet;

	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeConstlist ( int64_t iValue, bool bPackedString )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_CONST_LIST;
	tNode.m_pConsts = new ConstList_c();
	tNode.m_pConsts->Add ( iValue );
	tNode.m_pConsts->m_sExpr = m_sExpr;
	tNode.m_pConsts->m_bPackedStrings = bPackedString;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeConstlist ( float iValue )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_CONST_LIST;
	tNode.m_pConsts = new ConstList_c();
	tNode.m_pConsts->Add ( iValue );
	return m_dNodes.GetLength()-1;
}

void ExprParser_t::AppendToConstlist ( int iNode, int64_t iValue )
{
	m_dNodes[iNode].m_pConsts->Add ( iValue );
}

void ExprParser_t::AppendToConstlist ( int iNode, float iValue )
{
	m_dNodes[iNode].m_pConsts->Add ( iValue );
}

int ExprParser_t::AddNodeUservar ( int iUservar )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_USERVAR;
	tNode.m_iConst = iUservar;
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeHookIdent ( int iID )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_HOOK_IDENT;
	tNode.m_iFunc = iID;
	tNode.m_eRetType = m_pHook->GetIdentType ( iID );
	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeHookFunc ( int iID, int iLeft )
{
	CSphVector<ESphAttr> dArgTypes;
	GatherArgRetTypes ( iLeft, dArgTypes );

	ESphAttr eRet = m_pHook->GetReturnType ( iID, dArgTypes, CheckForConstSet ( iLeft, 0 ), m_sParserError );
	if ( eRet==SPH_ATTR_NONE )
		return -1;

	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_HOOK_FUNC;
	tNode.m_iFunc = iID;
	tNode.m_iLeft = iLeft;
	tNode.m_iRight = -1;

	// deduce type
	tNode.m_eArgType = ( iLeft>=0 ) ? m_dNodes[iLeft].m_eRetType : SPH_ATTR_INTEGER;
	tNode.m_eRetType = eRet;

	return m_dNodes.GetLength()-1;
}

int ExprParser_t::AddNodeMapArg ( const char * sKey, const char * sValue, int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add();
	tNode.m_iToken = TOK_MAP_ARG;
	tNode.m_pMapArg = new MapArg_c();
	tNode.m_pMapArg->Add ( sKey, sValue, iValue );
	tNode.m_eRetType = SPH_ATTR_MAPARG;
	return m_dNodes.GetLength()-1;
}

void ExprParser_t::AppendToMapArg ( int iNode, const char * sKey, const char * sValue, int64_t iValue )
{
	m_dNodes[iNode].m_pMapArg->Add ( sKey, sValue, iValue );
}

const char * ExprParser_t::Attr2Ident ( uint64_t uAttrLoc )
{
	ExprNode_t tAttr;
	sphUnpackAttrLocator ( uAttrLoc, &tAttr );

	CSphString sIdent;
	sIdent = m_pSchema->GetAttr ( tAttr.m_iLocator ).m_sName;
	m_dIdents.Add ( sIdent.Leak() );
	return m_dIdents.Last();
}


int ExprParser_t::AddNodeJsonField ( uint64_t uAttrLocator, int iLeft )
{
	int iNode = AddNodeAttr ( TOK_ATTR_JSON, uAttrLocator );
	m_dNodes[iNode].m_iLeft = iLeft;
	return m_dNodes.GetLength()-1;
}


int ExprParser_t::AddNodeJsonSubkey ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_SUBKEY;
	tNode.m_eRetType = SPH_ATTR_STRING;
	tNode.m_iConst = iValue;
	return m_dNodes.GetLength()-1;
}


int ExprParser_t::AddNodeDotNumber ( int64_t iValue )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_iToken = TOK_CONST_FLOAT;
	tNode.m_eRetType = SPH_ATTR_FLOAT;
	const char * pCur = m_sExpr + (int)( iValue>>32 );
	tNode.m_fConst = (float) strtod ( pCur-1, nullptr );
	return m_dNodes.GetLength()-1;
}


int ExprParser_t::AddNodeIdent ( const char * sKey, int iLeft )
{
	ExprNode_t & tNode = m_dNodes.Add ();
	tNode.m_sIdent = sKey;
	tNode.m_iLeft = iLeft;
	tNode.m_iToken = TOK_IDENT;
	tNode.m_eRetType = SPH_ATTR_JSON_FIELD;
	return m_dNodes.GetLength()-1;
}

//////////////////////////////////////////////////////////////////////////

// performs simple semantic analysis
// checks operand types for some arithmetic operators
struct TypeCheck_fn
{
	CSphString m_sError;

	void Enter ( const ExprNode_t & tNode, const CSphVector<ExprNode_t> & dNodes )
	{
		if ( !m_sError.IsEmpty() )
			return;

		bool bNumberOp = tNode.m_iToken=='+' || tNode.m_iToken=='-' || tNode.m_iToken=='*' || tNode.m_iToken=='/';
		if ( bNumberOp )
		{
			bool bLeftNumeric =	tNode.m_iLeft<0 ? false : IsNumericNode ( dNodes[tNode.m_iLeft] );
			bool bRightNumeric = tNode.m_iRight<0 ? false : IsNumericNode ( dNodes[tNode.m_iRight] );

			// if json vs numeric then let it pass (for the autoconversion)
			if ( ( bLeftNumeric && !bRightNumeric && dNodes[tNode.m_iRight].m_eRetType==SPH_ATTR_JSON_FIELD )
				|| ( bRightNumeric && !bLeftNumeric && dNodes[tNode.m_iLeft].m_eRetType==SPH_ATTR_JSON_FIELD ) )
					return;

			if ( !bLeftNumeric || !bRightNumeric )
			{
				m_sError = "numeric operation applied to non-numeric operands";
				return;
			}
		}

		if ( tNode.m_iToken==TOK_EQ )
		{
			// string equal must work with string columns only
			ESphAttr eLeftRet = tNode.m_iLeft<0 ? SPH_ATTR_NONE : dNodes[tNode.m_iLeft].m_eRetType;
			ESphAttr eRightRet = tNode.m_iRight<0 ? SPH_ATTR_NONE : dNodes[tNode.m_iRight].m_eRetType;
			bool bLeftStr = ( eLeftRet==SPH_ATTR_STRING || eLeftRet==SPH_ATTR_STRINGPTR || eLeftRet==SPH_ATTR_JSON_FIELD );
			bool bRightStr = ( eRightRet==SPH_ATTR_STRING || eRightRet==SPH_ATTR_STRINGPTR || eRightRet==SPH_ATTR_JSON_FIELD );
			if ( bLeftStr!=bRightStr && eLeftRet!=SPH_ATTR_JSON_FIELD && eRightRet!=SPH_ATTR_JSON_FIELD )
			{
				m_sError = "equal operation applied to part string operands";
				return;
			}
		}
	}

	void Exit ( const ExprNode_t & )
	{}

	bool IsNumericNode ( const ExprNode_t & tNode )
	{
		return tNode.m_eRetType==SPH_ATTR_INTEGER || tNode.m_eRetType==SPH_ATTR_BOOL || tNode.m_eRetType==SPH_ATTR_FLOAT ||
			tNode.m_eRetType==SPH_ATTR_BIGINT || tNode.m_eRetType==SPH_ATTR_TOKENCOUNT || tNode.m_eRetType==SPH_ATTR_TIMESTAMP;
	}
};


// checks whether we have a WEIGHT() in expression
struct WeightCheck_fn
{
	bool * m_pRes;

	explicit WeightCheck_fn ( bool * pRes )
		: m_pRes ( pRes )
	{
		assert ( m_pRes );
		*m_pRes = false;
	}

	void Enter ( const ExprNode_t & tNode, const CSphVector<ExprNode_t> & )
	{
		if ( tNode.m_iToken==TOK_WEIGHT )
			*m_pRes = true;
	}

	void Exit ( const ExprNode_t & )
	{}
};

// checks whether expression has functions defined not in this file like
// searchd-level function or ranker-level functions
struct HookCheck_fn
{
	ISphExprHook * m_pHook;

	explicit HookCheck_fn ( ISphExprHook * pHook )
		: m_pHook ( pHook )
	{}

	void Enter ( const ExprNode_t & tNode, const CSphVector<ExprNode_t> & )
	{
		if ( tNode.m_iToken==TOK_HOOK_IDENT || tNode.m_iToken==TOK_HOOK_FUNC )
			m_pHook->CheckEnter ( tNode.m_iFunc );
	}

	void Exit ( const ExprNode_t & tNode )
	{
		if ( tNode.m_iToken==TOK_HOOK_IDENT || tNode.m_iToken==TOK_HOOK_FUNC )
			m_pHook->CheckExit ( tNode.m_iFunc );
	}
};


ISphExpr * ExprParser_t::Parse ( const char * sExpr, const ISphSchema & tSchema,
	ESphAttr * pAttrType, bool * pUsesWeight, CSphString & sError )
{
	m_sLexerError = "";
	m_sParserError = "";
	m_sCreateError = "";

	// setup lexer
	m_sExpr = sExpr;
	m_pCur = sExpr;
	m_pSchema = &tSchema;

	// setup constant functions
	m_iConstNow = (int) time ( nullptr );

	// build abstract syntax tree
	m_iParsed = -1;
	yyparse ( this );

	// handle errors
	if ( m_iParsed<0 || !m_sLexerError.IsEmpty() || !m_sParserError.IsEmpty() )
	{
		sError = !m_sLexerError.IsEmpty() ? m_sLexerError : m_sParserError;
		if ( sError.IsEmpty() ) sError = "general parsing error";
		return nullptr;
	}

	// deduce return type
	ESphAttr eAttrType = m_dNodes[m_iParsed].m_eRetType;

	// Check expression stack to fit for mutual recursive function calls.
	// This check is an approximation, because different compilers with
	// different settings produce code which requires different stack size.
	if ( m_dNodes.GetLength()>100 )
	{
		CSphVector<int> dNodes;
		dNodes.Reserve ( m_dNodes.GetLength()/2 );
		int iMaxHeight = 1;
		int iHeight = 1;
		dNodes.Add ( m_iParsed );
		while ( dNodes.GetLength() )
		{
			const ExprNode_t & tExpr = m_dNodes[dNodes.Pop()];
			iHeight += ( tExpr.m_iLeft>=0 || tExpr.m_iRight>=0 ? 1 : -1 );
			iMaxHeight = Max ( iMaxHeight, iHeight );
			if ( tExpr.m_iRight>=0 )
				dNodes.Add ( tExpr.m_iRight );
			if ( tExpr.m_iLeft>=0 )
				dNodes.Add ( tExpr.m_iLeft );
		}

#define SPH_EXPRNODE_STACK_SIZE 160
		int64_t iExprStack = sphGetStackUsed() + iMaxHeight*SPH_EXPRNODE_STACK_SIZE;
		if ( g_iThreadStackSize<=iExprStack )
		{
			sError.SetSprintf ( "query too complex, not enough stack (thread_stack=%dK or higher required)",
				(int)( ( iExprStack + 1024 - ( iExprStack%1024 ) ) / 1024 ) );
			return nullptr;
		}
	}

	// perform optimizations (tree transformations)
	Optimize ( m_iParsed );
#if 0
	Dump ( m_iParsed );
	fflush ( stdout );
#endif

	// simple semantic analysis
	TypeCheck_fn tTypeChecker;
	WalkTree ( m_iParsed, tTypeChecker );
	if ( !tTypeChecker.m_sError.IsEmpty() )
	{
		sError.Swap ( tTypeChecker.m_sError );
		return nullptr;
	}

	// create evaluator
	CSphRefcountedPtr<ISphExpr> pRes { CreateTree ( m_iParsed ) };
	if ( !m_sCreateError.IsEmpty() )
	{
		pRes = nullptr;
		sError = m_sCreateError;
	}
	else if ( !pRes )
	{
		sError.SetSprintf ( "empty expression" );
	}

	if ( pAttrType )
		*pAttrType = eAttrType;

	if ( pUsesWeight )
	{
		WeightCheck_fn tWeightFunctor ( pUsesWeight );
		WalkTree ( m_iParsed, tWeightFunctor );
	}

	if ( m_pHook )
	{
		HookCheck_fn tHookFunctor ( m_pHook );
		WalkTree ( m_iParsed, tHookFunctor );
	}

	return pRes.Leak();
}

//////////////////////////////////////////////////////////////////////////
// PUBLIC STUFF
//////////////////////////////////////////////////////////////////////////

/// parser entry point
ISphExpr * sphExprParse ( const char * sExpr, const ISphSchema & tSchema, ESphAttr * pAttrType, bool * pUsesWeight,
	CSphString & sError, CSphQueryProfile * pProfiler, ESphCollation eCollation, ISphExprHook * pHook, bool * pZonespanlist, DWORD * pPackedFactorsFlags, ESphEvalStage * pEvalStage )
{
	// parse into opcodes
	ExprParser_t tParser ( pHook, pProfiler, eCollation );
	ISphExpr * pRes = tParser.Parse ( sExpr, tSchema, pAttrType, pUsesWeight, sError );
	if ( pZonespanlist )
		*pZonespanlist = tParser.m_bHasZonespanlist;
	if ( pEvalStage )
		*pEvalStage = tParser.m_eEvalStage;
	if ( pPackedFactorsFlags )
		*pPackedFactorsFlags = tParser.m_uPackedFactorFlags;
	return pRes;
}

/// json type autoconversion
ISphExpr * sphJsonFieldConv ( ISphExpr * pExpr )
{
	return new Expr_JsonFieldConv_c ( pExpr );
}

//
// $Id$
//
