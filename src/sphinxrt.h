//
// Copyright (c) 2017-2018, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxrt_
#define _sphinxrt_

#include "sphinx.h"
#include "sphinxutils.h"
#include "sphinxstem.h"

struct CSphReconfigureSettings;
struct CSphReconfigureSetup;
class ISphRtAccum;


/// RAM based updateable backend interface
class ISphRtIndex : public CSphIndex
{
public:
	explicit ISphRtIndex ( const char * sIndexName, const char * sFileName ) : CSphIndex ( sIndexName, sFileName ) {}

	/// get internal schema (to use for Add calls)
	virtual const CSphSchema & GetInternalSchema () const { return m_tSchema; }

	/// insert/update document in current txn
	/// fails in case of two open txns to different indexes
	virtual bool AddDocument ( ISphTokenizer * pTokenizer, int iFields, const char ** ppFields, const CSphMatch & tDoc,
		bool bReplace, const CSphString & sTokenFilterOptions, const char ** ppStr, const CSphVector<DWORD> & dMvas,
		CSphString & sError, CSphString & sWarning, ISphRtAccum * pAccExt ) = 0;

	/// delete document in current txn
	/// fails in case of two open txns to different indexes
	virtual bool DeleteDocument ( const SphDocID_t * pDocs, int iDocs, CSphString & sError, ISphRtAccum * pAccExt ) = 0;

	/// commit pending changes
	virtual void Commit ( int * pDeleted, ISphRtAccum * pAccExt ) = 0;

	/// undo pending changes
	virtual void RollBack ( ISphRtAccum * pAccExt ) = 0;

	/// check and periodically flush RAM chunk to disk
	virtual void CheckRamFlush () = 0;

	/// forcibly flush RAM chunk to disk
	virtual void ForceRamFlush ( bool bPeriodic=false ) = 0;

	/// get time of last flush, 0 means no flush required
	virtual int64_t GetFlushAge() const = 0;

	/// forcibly save RAM chunk as a new disk chunk
	virtual void ForceDiskChunk () = 0;

	/// attach a disk chunk to current index
	virtual bool AttachDiskIndex ( CSphIndex * pIndex, CSphString & sError ) = 0;

	/// truncate index (that is, kill all data)
	virtual bool Truncate ( CSphString & sError ) = 0;

	virtual void Optimize ( ) = 0;

	/// check settings vs current and return back tokenizer and dictionary in case of difference
	virtual bool IsSameSettings ( CSphReconfigureSettings & tSettings, CSphReconfigureSetup & tSetup, CSphString & sError ) const = 0;

	/// reconfigure index by using new tokenizer, dictionary and index settings
	/// current data got saved with current settings
	virtual void Reconfigure ( CSphReconfigureSetup & tSetup ) = 0;

	/// get disk chunk
	virtual CSphIndex * GetDiskChunk ( int iChunk ) = 0;
	
	virtual ISphRtAccum * CreateAccum ( CSphString & sError ) = 0;

	// instead of cloning for each AddDocument() call we could just call this method and improve batch inserts speed
	virtual ISphTokenizer * CloneIndexingTokenizer() const = 0;

	/// acquire thread-local indexing accumulator
	/// returns NULL if another index already uses it in an open txn
	ISphRtAccum * AcquireAccum ( CSphDict * pDict, ISphRtAccum * pAccExt=nullptr,
		bool bWordDict=true, bool bSetTLS = true, CSphString * sError=nullptr );
};

/// initialize subsystem
class CSphConfigSection;
void sphRTInit ( const CSphConfigSection & hSearchd, bool bTestMode, const CSphConfigSection * pCommon );
void sphRTConfigure ( const CSphConfigSection & hSearchd, bool bTestMode );
bool sphRTSchemaConfigure ( const CSphConfigSection & hIndex, CSphSchema * pSchema, CSphString * pError, bool bSkipValidation );
void sphRTSetTestMode ();

/// deinitialize subsystem
void sphRTDone ();

/// RT index factory
ISphRtIndex * sphCreateIndexRT ( const CSphSchema & tSchema, const char * sIndexName, int64_t iRamSize, const char * sPath, bool bKeywordDict );

typedef void ProgressCallbackSimple_t ();

class ISphRtAccum
{
protected:
	ISphRtIndex * m_pIndex=nullptr;		///< my current owner in this thread
	ISphRtAccum () {} // can not create such thing outside of RT index
public:
	virtual ~ISphRtAccum () {}
	ISphRtIndex * GetIndex() const { return m_pIndex; }
};

struct PercolateQueryDesc
{
	uint64_t m_uID;
	CSphString m_sQuery;
	CSphString m_sTags;
	CSphString m_sFilters;
	bool m_bQL;

	void Swap ( PercolateQueryDesc & tOther );
};

struct PercolateMatchResult_t
{
	bool m_bGetDocs;
	bool m_bGetQuery;
	bool m_bGetFilters;

	CSphFixedVector<PercolateQueryDesc> m_dQueryDesc;
	CSphFixedVector<int> m_dDocs;
	int m_iQueriesMatched;
	int m_iQueriesFailed = 0;
	int m_iDocsMatched;
	int64_t m_tmTotal;

	// verbose data
	bool m_bVerbose;
	CSphFixedVector<int> m_dQueryDT; // microsecond time per query
	int	m_iEarlyOutQueries;
	int	m_iTotalQueries;
	int m_iOnlyTerms;
	int64_t m_tmSetup;

	PercolateMatchResult_t ();
	void Swap ( PercolateMatchResult_t & tOther );
};

class PercolateIndex_i : public ISphRtIndex
{
public:
	PercolateIndex_i ( const char * sIndexName, const char * sFileName ) : ISphRtIndex ( sIndexName, sFileName ) {}
	virtual bool	MatchDocuments ( ISphRtAccum * pAccExt, PercolateMatchResult_t & tResult ) = 0;
	virtual int		DeleteQueries ( const uint64_t * pQueries, int iCount ) = 0;
	virtual int		DeleteQueries ( const char * sTags ) = 0;
	virtual bool	Query ( const char * sQuery, const char * sTags, const CSphVector<CSphFilterSettings> * pFilters, const CSphVector<FilterTreeItem_t> * pFilterTree, bool bReplace, bool bQL, uint64_t & uId, CSphString & sError ) = 0;

	virtual void	GetQueries ( const char * sFilterTags, bool bTagsEq, const CSphFilterSettings * pUID, int iOffset, int iLimit, CSphVector<PercolateQueryDesc> & dQueries ) = 0;
};

/// percolate query index factory
PercolateIndex_i * CreateIndexPercolate ( const CSphSchema & tSchema, const char * sIndexName, const char * sPath );
void FixPercolateSchema ( CSphSchema & tSchema );

typedef const QueryParser_i * CreateQueryParser ( bool bJson );
void SetPercolateQueryParserFactory ( CreateQueryParser * pCall );
void SetPercolateThreads ( int iThreads );

//////////////////////////////////////////////////////////////////////////

enum ESphBinlogReplayFlags
{
	SPH_REPLAY_ACCEPT_DESC_TIMESTAMP = 1,
	SPH_REPLAY_IGNORE_OPEN_ERROR = 2
};

typedef void BinlogFlushWork_t ( void * pLog );

struct BinlogFlushInfo_t
{
	void * m_pLog;
	BinlogFlushWork_t * m_fnWork;

	BinlogFlushInfo_t ()
		: m_pLog ( NULL )
		, m_fnWork ( NULL )
	{}
};

/// replay stored binlog
void sphReplayBinlog ( const SmallStringHash_T<CSphIndex*> & hIndexes, DWORD uReplayFlags, ProgressCallbackSimple_t * pfnProgressCallback, BinlogFlushInfo_t & tFlush );

#endif // _sphinxrt_
