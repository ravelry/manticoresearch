//
// Copyright (c) 2017-2020, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxsort_
#define _sphinxsort_

#include "sphinx.h"
#include "sortsetup.h"

#if USE_COLUMNAR
namespace columnar
{
	class Columnar_i;
}
#endif


class MatchProcessor_i
{
public:
	virtual			~MatchProcessor_i () {}
	virtual void	Process ( CSphMatch * pMatch ) = 0;
	virtual void	Process ( VecTraits_T<CSphMatch *> & dMatches ) = 0;
	virtual bool	ProcessInRowIdOrder() const = 0;
};

using fnGetBlobPoolFromMatch = std::function< const BYTE* ( const CSphMatch * )>;

/// generic match sorter interface
class ISphMatchSorter
{
public:
	bool				m_bRandomize = false;
	int64_t				m_iTotal = 0;

	RowID_t				m_iJustPushed {INVALID_ROWID};
	int					m_iMatchCapacity = 0;
	CSphTightVector<RowID_t> m_dJustPopped;

	virtual				~ISphMatchSorter() {}

	/// check if this sorter does groupby
	virtual bool		IsGroupby() const = 0;

	/// set match comparator state
	void				SetState ( const CSphMatchComparatorState & tState );

	/// get match comparator stat
	const CSphMatchComparatorState & GetState() const { return m_tState; }

	/// set group comparator state
	virtual void		SetGroupState ( const CSphMatchComparatorState & ) {}

	/// set blob pool pointer (for string+groupby sorters)
	virtual void		SetBlobPool ( const BYTE * ) {}

#if USE_COLUMNAR
	/// set columnar (to work with columnar attributes)
	virtual void		SetColumnar ( columnar::Columnar_i * pColumnar );
#endif

	/// set sorter schema
	virtual void		SetSchema ( ISphSchema * pSchema, bool bRemapCmp );

	/// get incoming schema
	const ISphSchema *	GetSchema() const { return ( ISphSchema *) m_pSchema; }

	/// base push
	/// returns false if the entry was rejected as duplicate
	/// returns true otherwise (even if it was not actually inserted)
	virtual bool		Push ( const CSphMatch & tEntry ) = 0;

	/// submit pre-grouped match. bNewSet indicates that the match begins the bunch of matches got from one source
	virtual bool		PushGrouped ( const CSphMatch & tEntry, bool bNewSet ) = 0;

	/// get	rough entries count, due of aggregate filtering phase
	virtual int			GetLength() const = 0;

	/// get total count of non-duplicates Push()ed through this queue
	int64_t				GetTotalCount() const { return m_iTotal; }

	/// process collected entries up to length count
	virtual void		Finalize ( MatchProcessor_i & tProcessor, bool bCallProcessInResultSetOrder ) = 0;

	/// store all entries into specified location and remove them from the queue
	/// entries are stored in properly sorted order
	/// return sorted entries count, might be less than length due of aggregate filtering phase
	virtual int			Flatten ( CSphMatch * pTo ) = 0;

	/// get a pointer to the worst element, NULL if there is no fixed location
	virtual const CSphMatch * GetWorst() const { return nullptr; }


	/// returns whether the sorter can be cloned to distribute processing over multi threads
	/// (delete and update sorters are too complex by side effects and can't be cloned)
	virtual bool		CanBeCloned() const { return true; }

	/// make same sorter (for MT processing)
	virtual ISphMatchSorter * Clone() const = 0;

	/// move resultset into target
	virtual void		MoveTo ( ISphMatchSorter * pRhs ) = 0;

	/// makes the same sorter
	void				CloneTo ( ISphMatchSorter * pTrg ) const;

	const CSphMatchComparatorState & GetComparatorState() const { return m_tState; }

	/// set attributes list these should copied into result set \ final matches
	void				SetFilteredAttrs ( const sph::StringSet & hAttrs, bool bAddDocid );

	/// transform collected matches into standalone (copy all pooled attrs to ptrs, drop unused)
	/// param fnBlobPoolFromMatch provides pool pointer from currently processed match pointer.
	void				TransformPooled2StandalonePtrs ( fnGetBlobPoolFromMatch fnBlobPoolFromMatch );

protected:
	SharedPtr_t<ISphSchema*>	m_pSchema;	///< sorter schema (adds dynamic attributes on top of index schema)
	CSphMatchComparatorState	m_tState;		///< protected to set m_iNow automatically on SetState() calls
	StrVec_t					m_dTransformed;

#if USE_COLUMNAR
	columnar::Columnar_i *		m_pColumnar;
#endif
};


struct CmpPSortersByRandom_fn
{
	inline static bool IsLess ( const ISphMatchSorter * a, const ISphMatchSorter * b )
	{
		assert ( a );
		assert ( b );
		return a->m_bRandomize<b->m_bRandomize;
	}
};


class BlobPool_c
{
public:
	virtual void	SetBlobPool ( const BYTE * pBlobPool ) { m_pBlobPool = pBlobPool; }
	const BYTE *	GetBlobPool () const { return m_pBlobPool; }

protected:
	const BYTE *	m_pBlobPool {nullptr};
};

/// groupby key type
typedef int64_t SphGroupKey_t;

/// base grouper (class that computes groupby key)
class CSphGrouper : public BlobPool_c, public ISphRefcountedMT
{
public:
	virtual SphGroupKey_t	KeyFromValue ( SphAttr_t uValue ) const = 0;
	virtual SphGroupKey_t	KeyFromMatch ( const CSphMatch & tMatch ) const = 0;
	virtual void			GetLocator ( CSphAttrLocator & tOut ) const = 0;
	virtual ESphAttr		GetResultType () const = 0;
	virtual CSphGrouper *	Clone() const = 0;

#if USE_COLUMNAR
	virtual void			SetColumnar ( const columnar::Columnar_i * pColumnar ) {}
#endif

protected:
	virtual					~CSphGrouper () {}; // =default causes bunch of errors building on wheezy
};

const char *	GetInternalAttrPrefix();
int 			GetStringRemapCount ( const ISphSchema & tDstSchema, const ISphSchema & tSrcSchema );
bool			IsSortStringInternal ( const CSphString & sColumnName );
bool			IsSortJsonInternal ( const CSphString & sColumnName );
CSphString		SortJsonInternalSet ( const CSphString & sColumnName );

/// creates proper queue for given query
/// may return NULL on error; in this case, error message is placed in sError
/// if the pUpdate is given, creates the updater's queue and perform the index update
/// instead of searching
ISphMatchSorter * sphCreateQueue ( const SphQueueSettings_t & tQueue, const CSphQuery & tQuery, CSphString & sError, SphQueueRes_t & tRes, StrVec_t * pExtra = nullptr );

void sphCreateMultiQueue ( const SphQueueSettings_t & tQueue, const VecTraits_T<CSphQuery> & dQueries, VecTraits_T<ISphMatchSorter *> & dSorters, VecTraits_T<CSphString> & dErrors,
	SphQueueRes_t & tRes, StrVec_t * pExtra );


#endif // _sphinxsort_