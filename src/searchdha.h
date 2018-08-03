//
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// Copyright (c) 2017-2018, Manticore Software LTD (http://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

/// @file searchdha.h
/// Declarations for the stuff specifically needed by searchd to work with remote agents
/// and high availability funcs


#ifndef _searchdha_
#define _searchdha_

#include <utility>

#if HAVE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif

#if USE_WINDOWS

bool LoadExFunctions ();

#endif

/////////////////////////////////////////////////////////////////////////////
// SOME SHARED GLOBAL VARIABLES
/////////////////////////////////////////////////////////////////////////////

extern int				g_iReadTimeout; // defined in searchd.cpp

extern int				g_iPingInterval;		// by default ping HA agents every 1 second
extern DWORD			g_uHAPeriodKarma;		// by default use the last 1 minute statistic to determine the best HA agent
extern int				g_iPersistentPoolSize;

extern int				g_iAgentConnectTimeout;
extern int				g_iAgentQueryTimeout;	// global (default). May be override by index-scope values, if one specified
extern bool				g_bHostnameLookup;

const int	STATS_DASH_PERIODS = 15;	///< store the history for last periods

/////////////////////////////////////////////////////////////////////////////
// MISC GLOBALS
/////////////////////////////////////////////////////////////////////////////

/// known default sphinx ports
/// (assigned by IANA, see http://www.iana.org/assignments/port-numbers for details)
enum IanaPorts_e
{
	IANA_PORT_SPHINXQL	= 9306,
	IANA_PORT_SPHINXAPI	= 9312
};

/// known status return codes
enum SearchdStatus_e : WORD
{
	SEARCHD_OK		= 0,	///< general success, command-specific reply follows
	SEARCHD_ERROR	= 1,	///< general failure, error message follows
	SEARCHD_RETRY	= 2,	///< temporary failure, error message follows, client should retry later
	SEARCHD_WARNING	= 3		///< general success, warning message and command-specific reply follow
};

/// remote agent state
enum class Agent_e
{
	HEALTHY,			///< agent is in normal state
	CONNECTING,		///< connecting to agent in progress, write handshake on socket ready
	RETRY,			///< should retry, but after scheduled timeout
};

const char * Agent_e_Name ( Agent_e eState );

/// per-agent query stats (raw, filled atomically on-the-fly)
enum AgentStats_e
{
	eTimeoutsQuery = 0,	///< number of time-outed queries
	eTimeoutsConnect,	///< number of time-outed connections
	eConnectFailures,	///< failed to connect
	eNetworkErrors,		///< network error
	eWrongReplies,		///< incomplete reply
	eUnexpectedClose,	///< agent closed the connection
	eNetworkCritical,		///< agent answered, but with warnings
	eNetworkNonCritical,	///< successful queries, no errors
	eMaxAgentStat
};

/// per-host query stats (calculated)
enum HostStats_e
{
	ehTotalMsecs=0,		///< number of microseconds in queries, total
	ehConnTries,		///< total number of connect tries
	ehAverageMsecs,		///< average connect time
	ehMaxMsecs,			///< maximal connect time
	ehMaxStat
};

enum HAStrategies_e {
	HA_RANDOM,
	HA_ROUNDROBIN,
	HA_AVOIDDEAD,
	HA_AVOIDERRORS,
	HA_AVOIDDEADTM,			///< the same as HA_AVOIDDEAD, but uses just min timeout instead of weighted random
	HA_AVOIDERRORSTM,		///< the same as HA_AVOIDERRORS, but uses just min timeout instead of weighted random

	HA_DEFAULT = HA_RANDOM
};

// manages persistent connections to a host
// serves a FIFO queue.
// I.e. if we have 2 connections to a host, and one task rent the connection,
// we will return 1-st socket. And the next rent request will definitely 2-nd socket
// whenever 1-st socket already released or not.
// (previous code used LIFO strategy)
class PersistentConnectionsPool_c
{
	mutable CSphMutex	 m_dDataLock;
	bool			m_bShutdown = false;			// will cause ReturnConnection to close the socket instead of returning it
	CSphTightVector<int>	m_dSockets GUARDED_BY ( m_dDataLock );
	int				m_iRit GUARDED_BY ( m_dDataLock ) = 0; // pos where we take the next socket to rent.
	int				m_iWit GUARDED_BY ( m_dDataLock ) = 0; // pos where we will put returned socket.
	int				m_iFreeWindow GUARDED_BY ( m_dDataLock ) = 0; // # of free sockets in the existing ring
	int				m_iLimit GUARDED_BY ( m_dDataLock ) = 0; // exact limit (embedded vector's limit is not exact)

	int Step ( int* ) REQUIRES ( m_dDataLock ); // step over the ring

public:
	~PersistentConnectionsPool_c ()	{ Shutdown (); };
	void	ReInit ( int iPoolSize ) REQUIRES ( !m_dDataLock );
	int		RentConnection () REQUIRES ( !m_dDataLock );
	void	ReturnConnection ( int iSocket ) REQUIRES ( !m_dDataLock );
	void	Shutdown () REQUIRES ( !m_dDataLock );
};

void ClosePersistentSockets();

struct AgentDash_t : ISphRefcountedMT
{
	// was uint64_t, but for atomic it creates extra tmpl instantiation without practical difference
	CSphAtomicL m_dCounters[eMaxAgentStat];	// event counters
	uint64_t m_dMetrics[ehMaxStat];			// calculated metrics

	AgentDash_t()
	{
		for ( auto& dMetric : m_dMetrics )
			dMetric = 0;
	}

	void Reset ()
	{
		for ( auto &iCounter : m_dCounters )
			iCounter = 0;
		for ( auto &uMetric : m_dMetrics )
			uMetric = 0;
	}

	void Add ( const AgentDash_t &rhs )
	{
		for ( int i = 0; i<eMaxAgentStat; ++i )
			m_dCounters[i] += rhs.m_dCounters[i];

		if ( m_dMetrics[ehConnTries] )
			m_dMetrics[ehAverageMsecs] =
				( m_dMetrics[ehAverageMsecs] * m_dMetrics[ehConnTries]
					+ rhs.m_dMetrics[ehAverageMsecs] * rhs.m_dMetrics[ehConnTries] )
					/ ( m_dMetrics[ehConnTries] + rhs.m_dMetrics[ehConnTries] );
		else
			m_dMetrics[ehAverageMsecs] = rhs.m_dMetrics[ehAverageMsecs];
		m_dMetrics[ehMaxMsecs] = Max ( m_dMetrics[ehMaxMsecs], rhs.m_dMetrics[ehMaxMsecs] );
		m_dMetrics[ehConnTries] += rhs.m_dMetrics[ehConnTries];
	}
private:
	~AgentDash_t() = default;
	friend struct HostDashboard_t; // the only struct allowed to directly declare/destroy me.
};

using AgentDashPtr_t = CSphRefcountedPtr<AgentDash_t>;

struct HostDashboard_t;
using HostDashboardPtr_t = CSphRefcountedPtr<HostDashboard_t>;

/// generic descriptor of remote host
struct HostDesc_t : ISphNoncopyable
{
	int m_iFamily = AF_INET;	///< TCP or UNIX socket
	CSphString m_sAddr;			///< remote searchd host (used to update m_uAddr with resolver)
	int m_iPort = -1;			///< remote searchd port, 0 if local
	DWORD m_uAddr = 0;			///< IP address
	bool m_bNeedResolve = false;	///< whether we keep m_uAddr, or call GetAddrInfo each time.

	bool m_bBlackhole = false;	///< blackhole agent flag
	bool m_bPersistent = false;	///< whether to keep the persistent connection to the agent.

	mutable HostDashboardPtr_t m_pDash;	///< ha dashboard of the host

	HostDesc_t &CloneFromHost ( const HostDesc_t &tOther );
	CSphString GetMyUrl () const;
};

/// generic descriptor of the mirror: host + indexes.
struct AgentDesc_t : HostDesc_t
{
	CSphString			m_sIndexes;		///< remote index names to query
	mutable AgentDashPtr_t m_pStats;	///< source for ShowStatus (one copy shared over all clones, refcounted).
	AgentDesc_t &CloneFrom ( const AgentDesc_t &tOther );
};

extern int g_iAgentRetryCount;
extern int g_iAgentRetryDelay;

struct IReporter_t : ISphNoncopyable
{
	virtual ~IReporter_t () {};
	virtual void SetTotal ( int iTasks ) = 0;
	virtual void Report ( bool bSuccess ) = 0;
};

#if USE_WINDOWS
	struct SingleOverlapped_t : public OVERLAPPED
	{
		ULONG_PTR	m_uParentOffset; // how many bytes add to this to take pointer to parent
		inline void Zero ()
		{
			ZeroMemory ( this, sizeof ( OVERLAPPED ) );
		}
	};
	struct DoubleOverlapped_t
	{
		SingleOverlapped_t	m_dWrite;
		SingleOverlapped_t	m_dRead;
		CSphAtomic			m_iRefs { 0 };
		DoubleOverlapped_t ()
		{
			m_dWrite.Zero();
			m_dRead.Zero();
			m_dWrite.m_uParentOffset = (LPBYTE) &m_dWrite-(LPBYTE) this;
			m_dRead.m_uParentOffset = (LPBYTE) &m_dRead-(LPBYTE) this;
		}
	};
	using LPKEY = DoubleOverlapped_t *;
#else
	using LPKEY = void *;
#endif

class MultiAgentDesc_c;

class IOVec_c
{
	SmartOutputBuffer_t &m_dSource; // source we associated with

	CSphVector<sphIovec> m_dIOVec;
	size_t m_iIOChunks = 0;

public:

	IOVec_c( SmartOutputBuffer_t & dSrc)
		: m_dSource ( dSrc )
	{
		m_dSource.AddRef ();
	};

	~IOVec_c()
	{
		m_dSource.Release();
	};

	void Build (); /// take data from linked source
	void Reset ();

	/// consume received chunk
	void StepForward ( size_t uStep );

	inline bool HasUnsent () const
	{
		return m_iIOChunks!=0;
	}

	/// if we have data (despite it is sent or not)
	inline bool IsEmpty () const
	{
		return m_dIOVec.IsEmpty ();
	}

	/// buf for sendmsg/WSAsend
	inline sphIovec * IOPtr () const
	{
		return m_dIOVec.end () - m_iIOChunks;
	}

	/// num of io vecs for sendmsg/WSAsend
	inline size_t IOSize () const
	{
		return m_iIOChunks;
	}
};

using Clbck_f = std::function<void ()>;
using Clbcktm_f = std::function<void ( int64_t, DWORD )>;

/// remote agent connection (local per-query state)
struct AgentConn_t : public ISphRefcountedMT
{
	AgentDesc_t		m_tDesc;			///< desc of my host // fixme! turn to ref to MultiAgent mirror?
	int				m_iSock = -1;

	// time-tracking and timeout settings
	int				m_iMyConnectTimeout { g_iAgentConnectTimeout };	///< populated from parent distr
	int				m_iMyQueryTimeout { g_iAgentQueryTimeout };		///< in msec
	int64_t			m_iStartQuery = 0;	///< the timestamp of the latest request // actualized
	int64_t			m_iEndQuery = 0;	///< the timestamp of the end of the latest operation // actual
	int64_t			m_iWall = 0;		///< wall time spent vs this agent // actualized
	int64_t			m_iWaited = 0;		///< statistics of waited

	// some external stuff
	CSphVector<CSphQueryResult> m_dResults;	///< multi-query results
	CSphString		m_sFailure;				///< failure message (both network and logical)
	mutable int		m_iStoreTag = -1;	///< cookie, m.b. used to 'glue' to concrete connection
	int				m_iWeight = -1;		///< weight of the index, will be send with query to remote host

	IReporter_t *	m_pReporter = nullptr;	///< used to report back when we're finished
	LPKEY			m_pPollerTask = nullptr; ///< internal for poller. fixme! privatize?
	bool			m_bSuccess = false;		///< agent got processed, no need to retry

public:
	AgentConn_t () = default;

	void SetMultiAgent ( const CSphString &sIndex, MultiAgentDesc_c * pMirror );
	void SetRetryLimit ( int iValue, bool bScale = true );

	inline bool NeedKick () { bool bRes = m_bNeedKick; m_bNeedKick = false; return bRes; }
	inline bool InNetLoop() const { return m_bInNetLoop; }
	inline bool IsBl () const { return m_tDesc.m_bBlackhole; }

	void InitRemoteTask ( IRequestBuilder_t * pQuery, IReplyParser_t * pParser, IReporter_t * pReporter
						  , int iQueryRetry, int iQueryDelay );
	void StartRemoteLoopTry (bool bSkipFirstRetry=false);

	void ErrorCallback ( int64_t iWaited );
	void SendCallback ( int64_t iWaited, DWORD uSent );
	void RecvCallback ( int64_t iWaited, DWORD uReceived );

	// helper for beatiful logging
	inline const char * StateName () const 	{ return Agent_e_Name ( m_eConnState ); }

private:

	// prepare buf, parse result
	IRequestBuilder_t * m_pBuilder = nullptr; ///< fixme! check if it is ok to have as the member, or we don't need it actually
	IReplyParser_t *	m_pParser = nullptr;

	// working with mirrors
	MultiAgentDesc_c *	m_pMultiAgent = nullptr; ///< my manager, could turn me into another mirror
	int			m_iRetries = 0;        ///< initialized to max num of tries. When zeroed, it is time to fail
	int			m_iMirrorsCount = 1;
	int			m_iDelay { g_iAgentRetryDelay };	///< delay between retries

	// active timeout (directly used by poller)
	int64_t			m_iPoolerTimeout = 0;    ///< m.b. query, or connect+query when TCP_FASTOPEN

	// receiving buffer stuff
	CSphFixedVector<BYTE>	m_dReplyBuf { 0 };
	int			m_iReplySize = -1;    ///< how many reply bytes are there
	static const size_t	REPLY_HEADER_SIZE = 12;
	BYTE		m_dReplyHeader[REPLY_HEADER_SIZE];
	BYTE *		m_pReplyCur = nullptr;

	// sending buffer stuff
	SmartOutputBuffer_t m_tOutput;		///< chain of blobs we're sending to a host
	IOVec_c 			m_dIOVec { m_tOutput };

	// states and flags
	bool m_bConnectHandshake = false;	///< if we need to establish new connection, and so, wait back handshake version
	bool m_bInNetLoop	= false;			///< if we're inside netloop (1-thread work with schedule)
	bool m_bNeedKick	= false;            ///< if we've installed callback from outside th and need to kick netloop
	bool m_bNeedInitiatePersist = false;    ///< for just created persistent - need SEARCHD_COMMAND_PERSIST
	bool m_bManyTries = false;	///< to avoid report 'retries limit esceeded' if we have ONLY one retry

	Agent_e			m_eConnState { Agent_e::HEALTHY };	///< current state
	SearchdStatus_e m_eReplyStatus { SEARCHD_ERROR };    ///< reply status code

private:
	~AgentConn_t () override;

	// switch/check internal state
	inline bool StateIs ( Agent_e eState ) const { return eState==m_eConnState; }
	void State ( Agent_e eState );



	bool GetPersist ();
	void ReturnPersist ();

	bool Fail ( const char * sFmt, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );
	bool Fatal ( AgentStats_e eStat, const char * sMessage, ... ) __attribute__ ( ( format ( printf, 3, 4 ) ) );
	void Finish ( bool bFailed = false ); /// finish the task, stat time.
	void BadResult ( int iError = 0 );	/// always return false
	void ReportFinish ( bool bSuccess = true );
	void SendingState (); ///< from CONNECTING state go to HEALTHY and switch timer to QUERY timeout.

	void SetupPersist();
	bool StartNextRetry ();
	bool ConnectionAlive ();

	void TimeoutTask ( int64_t iTimeoutMS = -1, Clbck_f &&fTimeout = nullptr, BYTE ActivateIO=0 );
	void ScheduleCallbacks ();
	void DisableWrite();

	void BuildData ();
	size_t ReplyBufPlace () const;
	void InitReplyBuf ( int iSize = 0 );
	inline bool IsReplyHeader() const { return m_iReplySize<0; }

	SSIZE_T SendChunk (); // low-level (platform specific) send
	SSIZE_T RecvChunk (); // low-level (platform specific) recv

	int DoTFO ( struct sockaddr * pSs, int iLen );

	bool DoQuery ();
	bool EstablishConnection ();
	bool SendQuery (DWORD uSent = 0);
	bool ReceiveAnswer (DWORD uReceived = 0);
	bool CommitResult ();
};

using VectorAgentConn_t = CSphVector<AgentConn_t *>;
using VecRefPtrsAgentConn_t = VecRefPtrs_t<AgentConn_t *>;
class IRemoteAgentsObserver : public IReporter_t
{
public:
	// check that there are no works to do
	virtual bool IsDone () const = 0;

	// get num of succeeded agents
	virtual long GetSucceeded () const = 0;

	// get num of succeeded agents
	virtual long GetFinished () const = 0;

	// block execution while there are works to do
	virtual void Finish () = 0;

	// block execution while some works finished
	virtual void WaitChanges () = 0;
};

IRemoteAgentsObserver * GetObserver ();

void ScheduleDistrJobs ( VectorAgentConn_t &dRemotes, IRequestBuilder_t * pQuery,
	IReplyParser_t * pParser, IRemoteAgentsObserver * pReporter=nullptr, int iQueryRetry = -1, int iQueryDelay = -1 );

extern const char * sAgentStatsNames[eMaxAgentStat + ehMaxStat];
using HostStatSnapshot_t = uint64_t[eMaxAgentStat + ehMaxStat];

/// per-host dashboard
struct HostDashboard_t : public ISphRefcountedMT
{
	HostDesc_t		m_tHost;				// only host info, no indices. Used for ping.
	bool			m_bNeedPing = false;	// we'll ping only HA agents, not everyone
	PersistentConnectionsPool_c * m_pPersPool = nullptr;	// persistence pool also lives here, one per dashboard

	mutable RwLock_t	 m_dDataLock;		// guards everything essential (see thread annotations)
	int64_t		m_iLastAnswerTime GUARDED_BY ( m_dDataLock ) = 0;	// updated when we get an answer from the host
	int64_t		m_iLastQueryTime GUARDED_BY ( m_dDataLock ) = 0;	// updated when we send a query to a host
	int64_t		m_iErrorsARow GUARDED_BY ( m_dDataLock ) = 0;		// num of errors a row, updated when we update the general statistic.

public:
	explicit HostDashboard_t ( const HostDesc_t & tAgent );
	bool IsOlder ( int64_t iTime ) const REQUIRES_SHARED ( m_dDataLock );
	AgentDash_t &GetCurrentStat () REQUIRES ( m_dDataLock );
	void GetCollectedStat ( HostStatSnapshot_t &dResult, int iPeriods = 1 ) const REQUIRES ( !m_dDataLock );

	static DWORD GetCurSeconds();
	static bool IsHalfPeriodChanged ( DWORD * pLast );

private:
	struct
	{
		AgentDash_t m_dData;
		DWORD m_uPeriod = 0xFFFFFFFF;
	} m_dStats[STATS_DASH_PERIODS] GUARDED_BY ( m_dDataLock );

	~HostDashboard_t ();
};


// set of options which are applied to every agent line
// and come partially from global config, partially m.b. set immediately in agent line as an option.
struct AgentOptions_t
{
	bool m_bBlackhole;
	bool m_bPersistent;
	HAStrategies_e m_eStrategy;
	int m_iRetryCount;
	int m_iRetryCountMultiplier;
};

/// context which keeps name of the index and agent
/// (mainly for error-reporting)
struct WarnInfo_t
{
	const char * m_szIndexName;
	const char * m_szAgent;

	void Warn ( const char * sFmt, ... ) const
	{
		va_list ap;
		va_start ( ap, sFmt );
		if ( m_szIndexName )
			sphLogVa ( CSphString ().SetSprintf ( "index '%s': agent '%s': %s", m_szIndexName, m_szAgent, sFmt ).cstr ()
					   , ap, SPH_LOG_INFO );
		else
			sphLogVa ( CSphString ().SetSprintf ( "host '%s': %s", m_szAgent, sFmt ).cstr ()
					   , ap, SPH_LOG_INFO );
		va_end ( ap );
	}

	/// format an error message using idx and agent names from own context
	/// \return always false, to simplify statements
	bool ErrSkip ( const char * sFmt, ... ) const
	{
		va_list ap;
		va_start ( ap, sFmt );
		if ( m_szIndexName )
			sphLogVa (
				CSphString ().SetSprintf ( "index '%s': agent '%s': %s, - SKIPPING AGENT", m_szIndexName, m_szAgent, sFmt ).cstr ()
				   , ap );
		else
			sphLogVa (
				CSphString ().SetSprintf ( "host '%s': %s, - SKIPPING AGENT", m_szAgent , sFmt ).cstr ()
				, ap );
		va_end ( ap );
		return false;
	}
};

/// descriptor for set of agents (mirrors) (stored in a global hash)
class MultiAgentDesc_c : public ISphRefcountedMT, public CSphFixedVector<AgentDesc_t>
{
	CSphAtomic				m_iRRCounter;	/// round-robin counter
	mutable RwLock_t		m_dWeightLock;	/// manages access to m_pWeights
	CSphFixedVector<float>	m_dWeights		/// the weights of the hosts
			GUARDED_BY (m_dWeightLock) { 0 };
	DWORD					m_uTimestamp { HostDashboard_t::GetCurSeconds() };	/// timestamp of last weight's actualization
	HAStrategies_e			m_eStrategy { HA_DEFAULT };
	int 					m_iMultiRetryCount = 0;

	~MultiAgentDesc_c () final = default;

public:
	MultiAgentDesc_c()
		: CSphFixedVector<AgentDesc_t> {0}
	{}

	// configure using dTemplateHosts as source of urls/indexes
	bool Init ( const CSphVector<AgentDesc_t*> &dTemplateHosts, const AgentOptions_t &tOpt, const WarnInfo_t &tWarn );

	const AgentDesc_t & ChooseAgent () REQUIRES ( !m_dWeightLock );

	inline bool IsHA() const
	{
		return GetLength() > 1;
	}

	inline int GetRetryLimit() const
	{
		return m_iMultiRetryCount;
	}

	CSphFixedVector<float> GetWeights () const REQUIRES (!m_dWeightLock)
	{
		CSphScopedRLock tRguard ( m_dWeightLock );
		CSphFixedVector<float> dResult {0};
		dResult.CopyFrom ( m_dWeights );
		return dResult;
	}

private:

	const AgentDesc_t & RRAgent ();
	const AgentDesc_t & RandAgent ();
	const AgentDesc_t & StDiscardDead () REQUIRES ( !m_dWeightLock );
	const AgentDesc_t & StLowErrors () REQUIRES ( !m_dWeightLock );

	void ChooseWeightedRandAgent ( int * pBestAgent, CSphVector<int> & dCandidates ) REQUIRES ( !m_dWeightLock );
	void CheckRecalculateWeights ( const CSphFixedVector<int64_t> &dTimers ) REQUIRES ( !m_dWeightLock );
};

using MultiAgentDescPtr_c = CSphRefcountedPtr<MultiAgentDesc_c>;

/////////////////////////////////////////////////////////////////////////////
// DISTRIBUTED QUERIES
/////////////////////////////////////////////////////////////////////////////


/// distributed index
struct DistributedIndex_t : public ServedStats_c, public ISphRefcountedMT
{
	VecRefPtrs_t<MultiAgentDesc_c *> m_dAgents;	///< remote agents
	StrVec_t m_dLocal;								///< local indexes
	CSphBitvec m_dKillBreak;
	int m_iAgentConnectTimeout		{ g_iAgentConnectTimeout };	///< in msec
	int m_iAgentQueryTimeout		{ g_iAgentQueryTimeout };	///< in msec
	int m_iAgentRetryCount			= 0;			///< overrides global one
	bool m_bDivideRemoteRanges		= false;		///< whether we divide big range onto agents or not
	HAStrategies_e m_eHaStrategy	= HA_DEFAULT;	///< how to select the best of my agents

	// get hive of all index'es hosts (not agents, but hosts, i.e. all mirrors as simple vector)
	void GetAllHosts ( VectorAgentConn_t &dTarget ) const;

	inline bool IsEmpty() const
	{
		return m_dAgents.IsEmpty() && m_dLocal.IsEmpty();
	}

	using ProcessFunctor = std::function<void ( AgentDesc_t & )>;
	// apply a function (non-const) to every single host in the hive
	void ForEveryHost ( ProcessFunctor );

private:
	~DistributedIndex_t () {
		sphLogDebugv ( "DistributedIndex_t %p removed", this );
	};
};

using DistributedIndexRefPtr_t = CSphRefcountedPtr<DistributedIndex_t>;

class SCOPED_CAPABILITY RLockedDistrIt_c : public RLockedHashIt_c
{
public:
	explicit RLockedDistrIt_c ( const GuardedHash_c * pHash ) ACQUIRE_SHARED ( pHash->IndexesRWLock ()
																				, m_pHash->IndexesRWLock () )
		: RLockedHashIt_c ( pHash )
	{}

	~RLockedDistrIt_c () UNLOCK_FUNCTION() {};

	DistributedIndexRefPtr_t Get () REQUIRES_SHARED ( m_pHash->IndexesRWLock () )
	{
		auto pDistr = ( DistributedIndex_t * ) RLockedHashIt_c::Get ();
		DistributedIndexRefPtr_t pRes ( pDistr );
		return pRes;
	}
};

extern GuardedHash_c * g_pDistIndexes;    // distributed indexes hash

inline DistributedIndexRefPtr_t GetDistr ( const CSphString &sName )
{
	return DistributedIndexRefPtr_t ( ( DistributedIndex_t * ) g_pDistIndexes->Get ( sName ) );
}

struct SearchdStats_t
{
	DWORD		m_uStarted = 0;
	CSphAtomicL		m_iConnections;
	CSphAtomicL		m_iMaxedOut;
	CSphAtomicL		m_iCommandCount[SEARCHD_COMMAND_TOTAL];
	CSphAtomicL		m_iAgentConnect;
	CSphAtomicL		m_iAgentRetry;

	CSphAtomicL		m_iQueries;			///< search queries count (differs from search commands count because of multi-queries)
	CSphAtomicL		m_iQueryTime;		///< wall time spent (including network wait time)
	CSphAtomicL		m_iQueryCpuTime;	///< CPU time spent

	CSphAtomicL		m_iDistQueries;		///< distributed queries count
	CSphAtomicL		m_iDistWallTime;	///< wall time spent on distributed queries
	CSphAtomicL		m_iDistLocalTime;	///< wall time spent searching local indexes in distributed queries
	CSphAtomicL		m_iDistWaitTime;	///< time spent waiting for remote agents in distributed queries

	CSphAtomicL		m_iDiskReads;		///< total read IO calls (fired by search queries)
	CSphAtomicL		m_iDiskReadBytes;	///< total read IO traffic
	CSphAtomicL		m_iDiskReadTime;	///< total read IO time

	CSphAtomicL		m_iPredictedTime;	///< total agent predicted query time
	CSphAtomicL		m_iAgentPredictedTime;	///< total agent predicted query time
};

class cDashStorage : public ISphNoncopyable
{
	VecRefPtrs_t<HostDashboard_t *>	m_dDashes GUARDED_BY(m_tDashLock);
	mutable RwLock_t				m_tDashLock;

public:
	void				LinkHost ( HostDesc_t &dHost );
	HostDashboardPtr_t	FindAgent ( const CSphString& sAgent ) const;
	void				GetActiveDashes ( VecRefPtrs_t<HostDashboard_t *> & dAgents ) const;
};

extern SearchdStats_t			g_tStats;
extern cDashStorage				g_tDashes;

// parse strategy name into enum value
bool ParseStrategyHA ( const char * sName, HAStrategies_e * pStrategy );

// parse ','-delimited list of indexes
void ParseIndexList ( const CSphString &sIndexes, StrVec_t &dOut );

// try to parse hostname/ip/port or unixsocket on current pConfigLine.
// fill pAgent fields on success and move ppLine pointer next after parsed instance
// if :port is skipped in the line, IANA 9312 will be used in the case
bool ParseAddressPort ( HostDesc_t & pAgent, const char ** ppLine, const WarnInfo_t& dInfo );

//bool ParseAgentLine ( MultiAgentDesc_c &tAgent, const char * szAgent, const char * szIndexName, AgentOptions_t tDesc );
bool ConfigureMultiAgent ( MultiAgentDesc_c &tAgent, const char * szAgent, const char * szIndexName
						   , AgentOptions_t tOptions );

struct IRequestBuilder_t : public ISphNoncopyable
{
	virtual ~IRequestBuilder_t () {} // to avoid gcc4 warns
	virtual void BuildRequest ( const AgentConn_t &tAgent, CachedOutputBuffer_c &tOut ) const = 0;
};


struct IReplyParser_t
{
	virtual ~IReplyParser_t () {} // to avoid gcc4 warns
	virtual bool ParseReply ( MemInputBuffer_c &tReq, AgentConn_t &tAgent ) const = 0;
};

// an event we use to wake up pollers (also used in net events in threadpool)
struct PollableEvent_t : ISphNoncopyable
{
public:
	PollableEvent_t ();
	virtual ~PollableEvent_t ();

	inline bool IsPollable () const { return m_iPollablefd!=-1; }
	void Close();

	/// fire an event
	bool FireEvent () const;

	/// remove fired event
	void DisposeEvent () const;

public:
	int				m_iPollablefd = -1; ///< listener's fd, to be used in pollers
	CSphString		m_sError;

protected:
	int m_iSignalEvent = -1; ///< firing fd, writing here will wake up m_iPollablefd
	static int PollableErrno();
};

/// check if a non-blocked socket is still connected
bool sphNBSockEof ( int iSock );


//////////////////////////////////////////////////////////////////////////
// Universal work with select/poll/epoll/kqueue
//////////////////////////////////////////////////////////////////////////
using Clbck_f = std::function<void ()>;
using Clbcktm_f = std::function<void (int64_t, DWORD)>;

void EvActions ( AgentConn_t * pConnection, Clbcktm_f && fRead = nullptr
				 , Clbcktm_f && fWrite = nullptr);
void FirePoller ();

// wrapper around epoll/kqueue/poll
struct NetEventsIterator_t
{
	const void * 		m_pData = nullptr;
	DWORD				m_uEvents = 0;

	void Reset ()
	{
		m_pData = nullptr;
		m_uEvents = 0;
	}

	bool IsReadable () const;
	bool IsWritable () const;
};

class ISphNetEvents : public ISphNoncopyable
{
public:
	enum PoolEvents_e
	{
		SPH_POLL_RD = 1UL << 0,
		SPH_POLL_WR = 1UL << 1,
		SPH_POLL_HUP = 1UL << 2,
		SPH_POLL_ERR = 1UL << 3,
		SPH_POLL_PRI = 1UL << 4,
	};
public:
	virtual ~ISphNetEvents () {};
	virtual void SetupEvent ( int iSocket, PoolEvents_e eFlags, const void * pData ) = 0;
	virtual bool Wait ( int ) = 0;
	virtual int IterateStart () = 0;
	virtual bool IterateNextAll () = 0;
	virtual bool IterateNextReady () = 0;
	virtual void IterateChangeEvent ( int iSocket, PoolEvents_e eFlags ) = 0;
	virtual void IterateRemove ( int iSocket ) = 0;
	virtual NetEventsIterator_t & IterateGet () = 0;
};


inline bool NetEventsIterator_t::IsReadable () const
{
	return bool ( m_uEvents & ISphNetEvents::SPH_POLL_RD );
}

inline bool NetEventsIterator_t::IsWritable () const
{
	return bool ( m_uEvents & ISphNetEvents::SPH_POLL_WR );
}

// all fresh codeflows use version with poll/epoll/kqueue.
// legacy also set bFallbackSelect and it invokes 'select' for the case
// when nothing of poll/epoll/kqueue is available.
ISphNetEvents * sphCreatePoll ( int iSizeHint, bool bFallbackSelect = false );

// determine which branch will be used
// defs placed here for easy switch between/debug
#if HAVE_EPOLL
#define POLLING_EPOLL 1
#elif HAVE_KQUEUE
#define POLLING_KQUEUE 1
#elif HAVE_POLL
#define POLLING_POLL 1
#else
#define POLLING_SELECT 1
#endif

#endif // _searchdha_
