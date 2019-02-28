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
#include "sphinxutils.h"
#include "sphinxexcerpt.h"
#include "sphinxrt.h"
#include "sphinxpq.h"
#include "sphinxint.h"
#include "sphinxquery.h"
#include "sphinxjson.h"
#include "sphinxjsonquery.h"
#include "sphinxplugin.h"
#include "sphinxqcache.h"
#include "sphinxrlp.h"

extern "C"
{
#include "sphinxudf.h"
}

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#include <locale.h>
#include <math.h>

#include "searchdaemon.h"
#include "searchdha.h"
#include "searchdreplication.h"

#if HAVE_MALLOC_H
#include <malloc.h>
#endif

#define SEARCHD_BACKLOG			5
#define SPHINXAPI_PORT			9312
#define SPHINXQL_PORT			9306
#define MVA_UPDATES_POOL		1048576
#define NETOUTBUF				8192
#define PING_INTERVAL			1000
#define QLSTATE_FLUSH_MSEC		50

// don't shutdown on SIGKILL (debug purposes)
// 1 - SIGKILL will shut down the whole daemon; 0 - watchdog will reincarnate the daemon
#define WATCHDOG_SIGKILL		1

#define SPH_MYSQL_FLAG_STATUS_AUTOCOMMIT 2	// mysql.h: SERVER_STATUS_AUTOCOMMIT
#define SPH_MYSQL_FLAG_MORE_RESULTS 8		// mysql.h: SERVER_MORE_RESULTS_EXISTS

/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS
	// Win-specific headers and calls
	#include <io.h>
	#include <tlhelp32.h>

	#define sphSeek		_lseeki64
	#define stat		_stat

#else
	// UNIX-specific headers and calls
	#include <unistd.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/file.h>
	#include <sys/time.h>
	#include <sys/wait.h>
	#include <netdb.h>
	#include <sys/syscall.h>


	// for thr_self()
	#ifdef __FreeBSD__
	#include <sys/thr.h>
	#endif

	#define sphSeek		lseek

#if HAVE_EVENTFD
	#include <sys/eventfd.h>
#endif

#endif

#if USE_SYSLOG
	#include <syslog.h>
#endif

#if HAVE_GETRLIMIT & HAVE_SETRLIMIT
	#include <sys/time.h>
	#include <sys/resource.h>
#endif

/////////////////////////////////////////////////////////////////////////////


static const char * g_dProtoNames[PROTO_TOTAL] =
{
	"sphinxapi", "sphinxql", "http"
};


static bool				g_bService		= false;
#if USE_WINDOWS
static bool				g_bServiceStop	= false;
static const char *		g_sServiceName	= "searchd";
HANDLE					g_hPipe			= INVALID_HANDLE_VALUE;
#endif

static StrVec_t	g_dArgs;


enum LogFormat_e
{
	LOG_FORMAT_PLAIN,
	LOG_FORMAT_SPHINXQL
};

#define					LOG_COMPACT_IN	128						// upto this many IN(..) values allowed in query_log

static int				g_iLogFile			= STDOUT_FILENO;	// log file descriptor
static auto& 			g_iParentPID		= getParentPID ();  // set by watchdog
static bool				g_bLogSyslog		= false;
static bool				g_bQuerySyslog		= false;
static CSphString		g_sLogFile;								// log file name
static bool				g_bLogTty			= false;			// cached isatty(g_iLogFile)
static bool				g_bLogStdout		= true;				// extra copy of startup log messages to stdout; true until around "accepting connections", then MUST be false
static LogFormat_e		g_eLogFormat		= LOG_FORMAT_PLAIN;
static bool				g_bLogCompactIn		= false;			// whether to cut list in IN() clauses.
static int				g_iQueryLogMinMsec	= 0;				// log 'slow' threshold for query
static char				g_sLogFilter[SPH_MAX_FILENAME_LEN+1] = "\0";
static int				g_iLogFilterLen = 0;
static int				g_iLogFileMode = 0;

static const int64_t	MS2SEC = I64C ( 1000000 );
int						g_iReadTimeout		= 5;	// sec
static int				g_iWriteTimeout		= 5;
static int				g_iClientTimeout	= 300;
static int				g_iClientQlTimeout	= 900;	// sec
static int				g_iMaxChildren		= 0;
#if !USE_WINDOWS
static bool				g_bPreopenIndexes	= true;
#else
static bool				g_bPreopenIndexes	= false;
#endif
static bool				g_bWatchdog			= true;
static int				g_iExpansionLimit	= 0;
static bool				g_bOnDiskAttrs		= false;
static bool				g_bOnDiskPools		= false;
static int				g_iShutdownTimeout	= 3000000; // default timeout on daemon shutdown and stopwait is 3 seconds
static int				g_iBacklog			= SEARCHD_BACKLOG;
static int				g_iThdPoolCount		= 2;
static int				g_iThdQueueMax		= 0;
static int				g_tmWait = 1;
bool					g_bGroupingInUtc	= false;
static auto&			g_iTFO = sphGetTFO ();
static CSphString		g_sShutdownToken;

struct Listener_t
{
	int					m_iSock;
	bool				m_bTcp;
	ProtocolType_e		m_eProto;
	bool				m_bVIP;
};
static CSphVector<Listener_t>	g_dListeners;

static int				g_iQueryLogFile	= -1;
static CSphString		g_sQueryLogFile;
static CSphString		g_sPidFile;
static bool				g_bPidIsMine = false;		// if PID is not mine, don't unlink it on fail
static int				g_iPidFD		= -1;

static int				g_iMaxCachedDocs	= 0;	// in bytes
static int				g_iMaxCachedHits	= 0;	// in bytes

static int				g_iAttrFlushPeriod	= 0;			// in seconds; 0 means "do not flush"
int				g_iMaxPacketSize	= 8*1024*1024;	// in bytes; for both query packets from clients and response packets from agents
static int				g_iMaxFilters		= 256;
static int				g_iMaxFilterValues	= 4096;
static int				g_iMaxBatchQueries	= 32;
static ESphCollation	g_eCollation = SPH_COLLATION_DEFAULT;

static ISphThdPool *	g_pThdPool			= NULL;
int				g_iDistThreads		= 0;

int				g_iAgentConnectTimeout = 1000;
int				g_iAgentQueryTimeout = 3000;	// global (default). May be override by index-scope values, if one specified

const int	MAX_RETRY_COUNT		= 8;
const int	MAX_RETRY_DELAY		= 1000;

int						g_iAgentRetryCount = 0;
int						g_iAgentRetryDelay = MAX_RETRY_DELAY/2;	// global (default) values. May be override by the query options 'retry_count' and 'retry_timeout'
bool					g_bHostnameLookup = false;
CSphString				g_sMySQLVersion = SPHINX_VERSION;

// for CLang thread-safety analysis
ThreadRole MainThread; // functions which called only from main thread
ThreadRole HandlerThread; // thread which serves clients

struct ServiceThread_t
{
	SphThread_t m_tThread;
	bool m_bCreated = false;
	
	~ServiceThread_t()
	{
		Join();
	}

	bool Create ( void (*fnThread)(void*), void * pArg, const char * sName = nullptr )
	{
		m_bCreated = sphThreadCreate ( &m_tThread, fnThread, pArg, false, sName );
		return m_bCreated;
	}

	void Join ()
	{
		if ( m_bCreated && sphGetShutdown() )
			sphThreadJoin ( &m_tThread );
		m_bCreated = false;
	}
};

enum ThdState_e
{
	THD_HANDSHAKE = 0,
	THD_NET_READ,
	THD_NET_WRITE,
	THD_QUERY,
	THD_NET_IDLE,

	THD_STATE_TOTAL
};

static const char * g_dThdStates[THD_STATE_TOTAL] = {
	"handshake", "net_read", "net_write", "query", "net_idle"
};

struct ThdDesc_t : public ListNode_t
{
	SphThread_t		m_tThd {0};
	ProtocolType_e	m_eProto { PROTO_MYSQL41 };
	int				m_iClientSock = 0;
	CSphString		m_sClientName;
	bool			m_bVip = false;

	ThdState_e		m_eThdState { THD_HANDSHAKE };
	const char *	m_sCommand = nullptr;
	int				m_iConnID = -1;	///< current conn-id for this thread

	// stuff for SHOW THREADS
	int				m_iTid = 0;		///< OS thread id, or 0 if unknown
	int64_t			m_tmConnect = 0;	///< when did the client connect?
	int64_t			m_tmStart = 0;	///< when did the current request start?
	bool			m_bSystem = false;
	CSphFixedVector<char> m_dBuf {512};	///< current request description
	int				m_iCookie = 0;	///< may be used in case of pool to distinguish threads

	CSphMutex m_tQueryLock;
	const CSphQuery *	m_pQuery GUARDED_BY (m_tQueryLock) = nullptr;

	ThdDesc_t ()
	{
		m_dBuf[0] = '\0';
		m_dBuf.Last() = '\0';
	}

	void SetThreadInfo ( const char * sTemplate, ... )
	{
		// thread safe modification of string at m_dBuf
		m_dBuf[0] = '\0';
		m_dBuf.Last() = '\0';

		va_list ap;

		va_start ( ap, sTemplate );
		int iPrinted = vsnprintf ( m_dBuf.Begin(), m_dBuf.GetLength()-1, sTemplate, ap );
		va_end ( ap );

		if ( iPrinted>0 )
			m_dBuf[Min ( iPrinted, m_dBuf.GetLength()-1 )] = '\0';
	}

	void SetSearchQuery ( const CSphQuery * pQuery )
	{
		m_tQueryLock.Lock();
		m_pQuery = pQuery;
		m_tQueryLock.Unlock();
	}
};

static RwLock_t				g_tThdLock;
static List_t				g_dThd GUARDED_BY ( g_tThdLock );				///< existing threads table

static void ThreadSetSnippetInfo ( const char * sQuery, int64_t iSizeKB, bool bApi, ThdDesc_t &tThd )
{
	if ( bApi )
		tThd.SetThreadInfo ( "api-snippet datasize=%d.%d""k query=\"%s\"", int ( iSizeKB / 10 ), int ( iSizeKB % 10 ), sQuery );
	else
		tThd.SetThreadInfo ( "sphinxql-snippet datasize=%d.%d""k query=\"%s\"", int ( iSizeKB / 10 ), int ( iSizeKB % 10 ), sQuery );
}

static void ThreadSetSnippetInfo ( const char * sQuery, int64_t iSizeKB, ThdDesc_t &tThd )
{
	tThd.SetThreadInfo ( "snippet datasize=%d.%d""k query=\"%s\"", int ( iSizeKB / 10 ), int ( iSizeKB % 10 ), sQuery );
}

static void ThreadAdd ( ThdDesc_t * pThd ) EXCLUDES ( g_tThdLock )
{
	ScWL_t dThdLock ( g_tThdLock );
	g_dThd.Add ( pThd );
}

static void ThreadRemove ( ThdDesc_t * pThd ) EXCLUDES ( g_tThdLock )
{
	ScWL_t dThdLock ( g_tThdLock );
	g_dThd.Remove ( pThd );
}

static int ThreadsNum () EXCLUDES ( g_tThdLock )
{
	ScRL_t dThdLock ( g_tThdLock );
	return g_dThd.GetLength ();
}

static void ThdState ( ThdState_e eState, ThdDesc_t & tThd )
{
	tThd.m_eThdState = eState;
	tThd.m_tmStart = sphMicroTimer();
	if ( eState==THD_NET_IDLE )
		tThd.m_dBuf[0] = '\0';
}

static const char * g_sSystemName = "SYSTEM";
struct ThreadSystem_t
{
	ThdDesc_t m_tDesc;

	explicit ThreadSystem_t ( const char * sName )
	{
		m_tDesc.m_bSystem = true;
		m_tDesc.m_tmStart = sphMicroTimer();
		m_tDesc.m_iTid = GetOsThreadId();
		m_tDesc.SetThreadInfo ( "SYSTEM %s", sName );
		m_tDesc.m_sCommand = g_sSystemName;

		ThreadAdd ( &m_tDesc );
	}

	~ThreadSystem_t()
	{
		ThreadRemove ( &m_tDesc );
	}
};

struct ThreadLocal_t
{
	ThdDesc_t m_tDesc;

	explicit ThreadLocal_t ( const ThdDesc_t & tDesc )
	{
		m_tDesc.m_iTid = GetOsThreadId();
		m_tDesc.m_eProto = tDesc.m_eProto;
		m_tDesc.m_iClientSock = tDesc.m_iClientSock;
		m_tDesc.m_sClientName = tDesc.m_sClientName;
		m_tDesc.m_eThdState = tDesc.m_eThdState;
		m_tDesc.m_sCommand = tDesc.m_sCommand;
		m_tDesc.m_iConnID = tDesc.m_iConnID;
		m_tDesc.m_iCookie = tDesc.m_iCookie;

		m_tDesc.m_tmConnect = tDesc.m_tmConnect;
		m_tDesc.m_tmStart = tDesc.m_tmStart;

		ThreadAdd ( &m_tDesc );
	}

	~ThreadLocal_t()
	{
		ThreadRemove ( &m_tDesc );
	}
};

static int						g_iConnectionID = 0;		///< global conn-id

// handshake
static char						g_sMysqlHandshake[128];
static int						g_iMysqlHandshake = 0;

//////////////////////////////////////////////////////////////////////////

static CSphString		g_sConfigFile;
static DWORD			g_uCfgCRC32		= 0;
static struct stat		g_tCfgStat;

#if USE_WINDOWS
static bool				g_bSeamlessRotate	= false;
#else
static bool				g_bSeamlessRotate	= true;
#endif

static bool				g_bIOStats		= false;
static bool				g_bCpuStats		= false;
static bool				g_bOptNoDetach	= false;
static bool				g_bOptNoLock	= false;
static bool				g_bSafeTrace	= false;
static bool				g_bStripPath	= false;
static bool				g_bCoreDump		= false;

static volatile sig_atomic_t g_bGotSighup		= 0;	// we just received SIGHUP; need to log
static volatile sig_atomic_t g_bGotSigterm		= 0;	// we just received SIGTERM; need to shutdown
static volatile sig_atomic_t g_bGotSigusr1		= 0;	// we just received SIGUSR1; need to reopen logs

// pipe to watchdog to inform that daemon is going to close, so no need to restart it in case of crash
static CSphLargeBuffer<DWORD, true>	g_bDaemonAtShutdown;
static auto&					g_bShutdown = sphGetShutdown();
volatile bool					g_bMaintenance = false;
volatile bool					g_bPrereading = false;
static CSphLargeBuffer<DWORD, true>	g_bHaveTTY;

GuardedHash_c *								g_pLocalIndexes = new GuardedHash_c();	// served (local) indexes hash
GuardedHash_c *								g_pDisabledIndexes = new GuardedHash_c(); // not yet ready (in process of loading/rotation) indexes
GuardedHash_c *								g_pDistIndexes = new GuardedHash_c ();    // distributed indexes hash


static RwLock_t								g_tRotateConfigMutex;
static CSphConfigParser g_pCfg GUARDED_BY ( g_tRotateConfigMutex );
static ServiceThread_t						g_tRotateThread;
static ServiceThread_t						g_tRotationServiceThread;
static volatile bool						g_bInvokeRotationService = false;
static volatile bool						g_bNeedRotate = false;		// true if there were pending HUPs to handle (they could fly in during previous rotate)
static volatile bool						g_bInRotate = false;		// true while we are rotating

static ServiceThread_t						g_tPingThread;

static CSphVector<SphThread_t>				g_dTickPoolThread;

/// flush parameters of rt indexes
static ServiceThread_t						g_tRtFlushThread;
static ServiceThread_t						g_tBinlogFlushThread;
static BinlogFlushInfo_t					g_tBinlogAutoflush;

// optimize thread
static ServiceThread_t						g_tOptimizeThread;
static CSphMutex							g_tOptimizeQueueMutex;
static StrVec_t								g_dOptimizeQueue;

static CSphMutex							g_tPersLock;
static CSphAtomic							g_iPersistentInUse;

static ServiceThread_t						g_tPrereadThread;

/// master-agent API protocol extensions version
enum
{
	VER_MASTER = 17
};


/// command names
static const char * g_dApiCommands[] =
{
	"search", "excerpt", "update", "keywords", "persist", "status", "query", "flushattrs", "query", "ping", "delete", "set",  "insert", "replace", "commit", "suggest", "json",
	"callpq", "clusterpq"
};

STATIC_ASSERT ( sizeof(g_dApiCommands)/sizeof(g_dApiCommands[0])==SEARCHD_COMMAND_TOTAL, SEARCHD_COMMAND_SHOULD_BE_SAME_AS_SEARCHD_COMMAND_TOTAL );

//////////////////////////////////////////////////////////////////////////

const char * sAgentStatsNames[eMaxAgentStat+ehMaxStat]=
	{ "query_timeouts", "connect_timeouts", "connect_failures",
		"network_errors", "wrong_replies", "unexpected_closings",
		"warnings", "succeeded_queries", "total_query_time",
		"connect_count", "connect_avg", "connect_max" };

static RwLock_t					g_tLastMetaLock;
static CSphQueryResultMeta		g_tLastMeta GUARDED_BY ( g_tLastMetaLock );

//////////////////////////////////////////////////////////////////////////

struct FlushState_t
{
	int		m_bFlushing;		///< update flushing in progress
	int		m_iFlushTag;		///< last flushed tag
	bool	m_bForceCheck;		///< forced check/flush flag
};

static FlushState_t				g_tFlush;

//////////////////////////////////////////////////////////////////////////

/// available uservar types
enum Uservar_e
{
	USERVAR_INT_SET
};

/// uservar name to value binding
struct Uservar_t
{
	Uservar_e			m_eType { USERVAR_INT_SET };
	CSphRefcountedPtr<UservarIntSet_c>	m_pVal;
};

static CSphMutex					g_tUservarsMutex;
static SmallStringHash_T<Uservar_t>	g_hUservars GUARDED_BY ( g_tUservarsMutex );

static volatile int64_t				g_tmSphinxqlState; // last state (uservars+udfs+...) update timestamp
static ServiceThread_t				g_tSphinxqlStateFlushThread;
static CSphString					g_sSphinxqlState;

/////////////////////////////////////////////////////////////////////////////
// MISC
/////////////////////////////////////////////////////////////////////////////

void ReleaseTTYFlag()
{
	if ( !g_bHaveTTY.IsEmpty() )
		*g_bHaveTTY.GetWritePtr() = 1;
}

//////////////////////////////////////////////////////////////////////////
void QueryStatContainer_c::Add ( uint64_t uFoundRows, uint64_t uQueryTime, uint64_t uTimestamp )
{
	if ( !m_dRecords.IsEmpty() )
	{
		QueryStatRecord_t & tLast = m_dRecords.Last();
		const uint64_t BUCKET_TIME_DELTA = 100000;
		if ( uTimestamp-tLast.m_uTimestamp<=BUCKET_TIME_DELTA )
		{
			tLast.m_uFoundRowsMin = Min ( uFoundRows, tLast.m_uFoundRowsMin );
			tLast.m_uFoundRowsMax = Max ( uFoundRows, tLast.m_uFoundRowsMax );
			tLast.m_uFoundRowsSum += uFoundRows;

			tLast.m_uQueryTimeMin = Min ( uQueryTime, tLast.m_uQueryTimeMin );
			tLast.m_uQueryTimeMax = Max ( uQueryTime, tLast.m_uQueryTimeMax );
			tLast.m_uQueryTimeSum += uQueryTime;

			tLast.m_iCount++;

			return;
		}
	}

	const uint64_t MAX_TIME_DELTA = 15*60*1000000;
	while ( !m_dRecords.IsEmpty() && ( uTimestamp-m_dRecords[0].m_uTimestamp ) > MAX_TIME_DELTA )
		m_dRecords.Pop();

	QueryStatRecord_t & tRecord = m_dRecords.Push();
	tRecord.m_uFoundRowsMin = uFoundRows;
	tRecord.m_uFoundRowsMax = uFoundRows;
	tRecord.m_uFoundRowsSum = uFoundRows;

	tRecord.m_uQueryTimeMin = uQueryTime;
	tRecord.m_uQueryTimeMax = uQueryTime;
	tRecord.m_uQueryTimeSum = uQueryTime;

	tRecord.m_uTimestamp = uTimestamp;
	tRecord.m_iCount = 1;
}

void QueryStatContainer_c::GetRecord ( int iRecord, QueryStatRecord_t & tRecord ) const
{
	tRecord = m_dRecords[iRecord];
}


int QueryStatContainer_c::GetNumRecords() const
{
	return m_dRecords.GetLength();
}

QueryStatContainer_c::QueryStatContainer_c() = default;

QueryStatContainer_c::QueryStatContainer_c ( QueryStatContainer_c && tOther ) noexcept
	: m_dRecords { std::move ( tOther.m_dRecords ) } {}

QueryStatContainer_c & QueryStatContainer_c::operator = ( QueryStatContainer_c && tOther ) noexcept
{
	if ( &tOther!=this )
		m_dRecords = std::move ( tOther.m_dRecords );
	return *this;
}

//////////////////////////////////////////////////////////////////////////

#ifndef NDEBUG
void QueryStatContainerExact_c::Add ( uint64_t uFoundRows, uint64_t uQueryTime, uint64_t uTimestamp )
{
	const uint64_t MAX_TIME_DELTA = 15*60*1000000;
	while ( !m_dRecords.IsEmpty() && ( uTimestamp-m_dRecords[0].m_uTimestamp ) > MAX_TIME_DELTA )
		m_dRecords.Pop();

	QueryStatRecordExact_t & tRecord = m_dRecords.Push();
	tRecord.m_uFoundRows = uFoundRows;
	tRecord.m_uQueryTime = uQueryTime;
	tRecord.m_uTimestamp = uTimestamp;
}


int QueryStatContainerExact_c::GetNumRecords() const
{
	return m_dRecords.GetLength();
}


void QueryStatContainerExact_c::GetRecord ( int iRecord, QueryStatRecord_t & tRecord ) const
{
	const QueryStatRecordExact_t & tExact = m_dRecords[iRecord];

	tRecord.m_uQueryTimeMin = tExact.m_uQueryTime;
	tRecord.m_uQueryTimeMax = tExact.m_uQueryTime;
	tRecord.m_uQueryTimeSum = tExact.m_uQueryTime;
	tRecord.m_uFoundRowsMin = tExact.m_uFoundRows;
	tRecord.m_uFoundRowsMax = tExact.m_uFoundRows;
	tRecord.m_uFoundRowsSum = tExact.m_uFoundRows;

	tRecord.m_uTimestamp = tExact.m_uTimestamp;
	tRecord.m_iCount = 1;
}

QueryStatContainerExact_c::QueryStatContainerExact_c() = default;

QueryStatContainerExact_c::QueryStatContainerExact_c ( QueryStatContainerExact_c && tOther ) noexcept
	: m_dRecords { std::move ( tOther.m_dRecords ) }
{
}

QueryStatContainerExact_c & QueryStatContainerExact_c::operator = ( QueryStatContainerExact_c && tOther ) noexcept
{
	if ( &tOther!=this )
		m_dRecords = std::move ( tOther.m_dRecords );
	return *this;
}
#endif


//////////////////////////////////////////////////////////////////////////
ServedDesc_t::~ServedDesc_t ()
{
	if ( m_pIndex )
		m_pIndex->Dealloc ();
	if ( !m_sUnlink.IsEmpty () )
	{
		sphLogDebug ( "unlink %s", m_sUnlink.cstr() );
		sphUnlinkIndex ( m_sUnlink.cstr (), false );
	}
	SafeDelete ( m_pIndex );
}

//////////////////////////////////////////////////////////////////////////
ServedStats_c::ServedStats_c()
{
	Verify (m_tStatsLock.Init ( true ));
	m_pQueryTimeDigest = sphCreateTDigest();
	m_pRowsFoundDigest = sphCreateTDigest();
	assert ( m_pQueryTimeDigest && m_pRowsFoundDigest );
}


ServedStats_c::~ServedStats_c()
{
	SafeDelete ( m_pRowsFoundDigest );
	SafeDelete ( m_pQueryTimeDigest );
	m_tStatsLock.Done ();
}

void ServedStats_c::AddQueryStat ( uint64_t uFoundRows, uint64_t uQueryTime )
{
	ScWL_t wLock ( m_tStatsLock );

	m_pRowsFoundDigest->Add ( (double)uFoundRows );
	m_pQueryTimeDigest->Add ( (double)uQueryTime );

	uint64_t uTimeStamp = sphMicroTimer();
	m_tQueryStatRecords.Add ( uFoundRows, uQueryTime, uTimeStamp );

#ifndef NDEBUG
	m_tQueryStatRecordsExact.Add ( uFoundRows, uQueryTime, uTimeStamp );
#endif
	
	m_uTotalFoundRowsMin = Min ( uFoundRows, m_uTotalFoundRowsMin );
	m_uTotalFoundRowsMax = Max ( uFoundRows, m_uTotalFoundRowsMax );
	m_uTotalFoundRowsSum+= uFoundRows;
	
	m_uTotalQueryTimeMin = Min ( uQueryTime, m_uTotalQueryTimeMin );
	m_uTotalQueryTimeMax = Max ( uQueryTime, m_uTotalQueryTimeMax );
	m_uTotalQueryTimeSum+= uQueryTime;
	
	++m_uTotalQueries;
}


static const uint64_t g_dStatsIntervals[]=
{
	1*60*1000000,
	5*60*1000000,
	15*60*1000000
};


void ServedStats_c::DoStatCalcStats ( const QueryStatContainer_i * pContainer,
	QueryStats_t & tRowsFoundStats, QueryStats_t & tQueryTimeStats ) const
{
	assert ( pContainer );

	auto uTimestamp = sphMicroTimer();

	ScRL_t rLock ( m_tStatsLock );

	int iRecords = m_tQueryStatRecords.GetNumRecords();
	for ( int i = QUERY_STATS_INTERVAL_1MIN; i<=QUERY_STATS_INTERVAL_15MIN; ++i )
		CalcStatsForInterval ( pContainer, tRowsFoundStats.m_dStats[i], tQueryTimeStats.m_dStats[i], uTimestamp, g_dStatsIntervals[i], iRecords );

	auto & tRowsAllStats = tRowsFoundStats.m_dStats[QUERY_STATS_INTERVAL_ALLTIME];
	tRowsAllStats.m_dData[QUERY_STATS_TYPE_AVG] = m_uTotalQueries ? m_uTotalFoundRowsSum/m_uTotalQueries : 0;
	tRowsAllStats.m_dData[QUERY_STATS_TYPE_MIN] = m_uTotalFoundRowsMin;
	tRowsAllStats.m_dData[QUERY_STATS_TYPE_MAX] = m_uTotalFoundRowsMax;
	tRowsAllStats.m_dData[QUERY_STATS_TYPE_95] = (uint64_t)m_pRowsFoundDigest->Percentile(95);
	tRowsAllStats.m_dData[QUERY_STATS_TYPE_99] = (uint64_t)m_pRowsFoundDigest->Percentile(99);
	tRowsAllStats.m_uTotalQueries = m_uTotalQueries;

	auto & tQueryAllStats = tQueryTimeStats.m_dStats[QUERY_STATS_INTERVAL_ALLTIME];
	tQueryAllStats.m_dData[QUERY_STATS_TYPE_AVG] = m_uTotalQueries ? m_uTotalQueryTimeSum/m_uTotalQueries : 0;
	tQueryAllStats.m_dData[QUERY_STATS_TYPE_MIN] = m_uTotalQueryTimeMin;
	tQueryAllStats.m_dData[QUERY_STATS_TYPE_MAX] = m_uTotalQueryTimeMax;
	tQueryAllStats.m_dData[QUERY_STATS_TYPE_95] = (uint64_t)m_pQueryTimeDigest->Percentile(95);
	tQueryAllStats.m_dData[QUERY_STATS_TYPE_99] = (uint64_t)m_pQueryTimeDigest->Percentile(99);
	tQueryAllStats.m_uTotalQueries = m_uTotalQueries;
}


void ServedStats_c::CalculateQueryStats ( QueryStats_t & tRowsFoundStats, QueryStats_t & tQueryTimeStats ) const
{
	DoStatCalcStats ( &m_tQueryStatRecords, tRowsFoundStats, tQueryTimeStats );
}


#ifndef NDEBUG
void ServedStats_c::CalculateQueryStatsExact ( QueryStats_t & tRowsFoundStats, QueryStats_t & tQueryTimeStats ) const
{
	DoStatCalcStats ( &m_tQueryStatRecordsExact, tRowsFoundStats, tQueryTimeStats );
}
#endif // !NDEBUG


void ServedStats_c::CalcStatsForInterval ( const QueryStatContainer_i * pContainer, QueryStatElement_t & tRowResult, QueryStatElement_t & tTimeResult, uint64_t uTimestamp, uint64_t uInterval, int iRecords )
{
	assert ( pContainer );

	tRowResult.m_dData[QUERY_STATS_TYPE_AVG] = 0;
	tRowResult.m_dData[QUERY_STATS_TYPE_MIN] = UINT64_MAX;
	tRowResult.m_dData[QUERY_STATS_TYPE_MAX] = 0;
	
	tTimeResult.m_dData[QUERY_STATS_TYPE_AVG] = 0;
	tTimeResult.m_dData[QUERY_STATS_TYPE_MIN] = UINT64_MAX;
	tTimeResult.m_dData[QUERY_STATS_TYPE_MAX] = 0;
	
	CSphTightVector<uint64_t> dFound, dTime;
	dFound.Reserve ( iRecords );
	dTime.Reserve ( iRecords );

	DWORD uTotalQueries = 0;
	QueryStatRecord_t tRecord;

	for ( int i = 0; i < pContainer->GetNumRecords(); ++i )
	{
		pContainer->GetRecord ( i, tRecord );

		if ( uTimestamp-tRecord.m_uTimestamp<=uInterval )
		{
			tRowResult.m_dData[QUERY_STATS_TYPE_MIN] = Min ( tRecord.m_uFoundRowsMin, tRowResult.m_dData[QUERY_STATS_TYPE_MIN] );
			tRowResult.m_dData[QUERY_STATS_TYPE_MAX] = Max ( tRecord.m_uFoundRowsMax, tRowResult.m_dData[QUERY_STATS_TYPE_MAX] );

			tTimeResult.m_dData[QUERY_STATS_TYPE_MIN] = Min ( tRecord.m_uQueryTimeMin, tTimeResult.m_dData[QUERY_STATS_TYPE_MIN] );
			tTimeResult.m_dData[QUERY_STATS_TYPE_MAX] = Max ( tRecord.m_uQueryTimeMax, tTimeResult.m_dData[QUERY_STATS_TYPE_MAX] );

			dFound.Add ( tRecord.m_uFoundRowsSum/tRecord.m_iCount );
			dTime.Add ( tRecord.m_uQueryTimeSum/tRecord.m_iCount );

			tRowResult.m_dData[QUERY_STATS_TYPE_AVG] += tRecord.m_uFoundRowsSum;
			tTimeResult.m_dData[QUERY_STATS_TYPE_AVG] += tRecord.m_uQueryTimeSum;
			uTotalQueries += tRecord.m_iCount;
		}
	}
	
	dFound.Sort();
	dTime.Sort();

	tRowResult.m_uTotalQueries = uTotalQueries;
	tTimeResult.m_uTotalQueries = uTotalQueries;
	
	if ( !dFound.GetLength() )
		return;
	
	tRowResult.m_dData[QUERY_STATS_TYPE_AVG]/= uTotalQueries;
	tTimeResult.m_dData[QUERY_STATS_TYPE_AVG]/= uTotalQueries;
	
	int u95 = Max ( 0, Min ( int ( ceilf ( dFound.GetLength()*0.95f ) + 0.5f )-1, dFound.GetLength()-1 ) );
	int u99 = Max ( 0, Min ( int ( ceilf ( dFound.GetLength()*0.99f ) + 0.5f )-1, dFound.GetLength()-1 ) );

	tRowResult.m_dData[QUERY_STATS_TYPE_95] = dFound[u95];
	tRowResult.m_dData[QUERY_STATS_TYPE_99] = dFound[u99];
	
	tTimeResult.m_dData[QUERY_STATS_TYPE_95] = dTime[u95];
	tTimeResult.m_dData[QUERY_STATS_TYPE_99] = dTime[u99];
}

//////////////////////////////////////////////////////////////////////////

// want write lock to wipe out reader and not wait readers
// but only for RT and PQ indexes as these operations are rare there
ServedIndex_c::ServedIndex_c ( const ServedDesc_t & tDesc )
	: m_tLock ( tDesc.m_eType==IndexType_e::RT || tDesc.m_eType==IndexType_e::PERCOLATE )
{
	*(ServedDesc_t*)(this) = tDesc;
}

ServedDesc_t * ServedIndex_c::ReadLock () const
{
	if ( m_tLock.ReadLock () )
		sphLogDebugvv ( "ReadLock %p", this );
	else
	{
		sphLogDebug ( "ReadLock %p failed", this );
		assert ( false );
	}
	AddRef ();
	return ( ServedDesc_t * ) this;
}

ServedDesc_t * ServedIndex_c::WriteLock () const
{
	sphLogDebugvv ( "WriteLock %p wait", this );
	if ( m_tLock.WriteLock() )
		sphLogDebugvv ( "WriteLock %p", this );
	else
	{
		sphLogDebug ( "WriteLock %p failed", this );
		assert ( false );
	}
	AddRef ();
	return ( ServedDesc_t * ) this;
}

void ServedIndex_c::Unlock () const
{
	if ( m_tLock.Unlock() )
		sphLogDebugvv ( "Unlock %p", this );
	else
	{
		sphLogDebug ( "Unlock %p failed", this );
		assert ( false );
	}
	Release ();
}

//////////////////////////////////////////////////////////////////////////
GuardedHash_c::GuardedHash_c ()
{
	if ( !m_tIndexesRWLock.Init() )
		sphDie ( "failed to init hash indexes rwlock" );
}


GuardedHash_c::~GuardedHash_c()
{
	ReleaseAndClear ();
	Verify ( m_tIndexesRWLock.Done() );
}

int GuardedHash_c::GetLength () const
{
	CSphScopedRLock dRL { m_tIndexesRWLock };
	return GetLengthUnl();
}

int GuardedHash_c::GetLengthUnl () const
{
	return m_hIndexes.GetLength ();
}

void GuardedHash_c::ReleaseAndClear ()
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	for ( m_hIndexes.IterateStart (); m_hIndexes.IterateNext(); )
		SafeRelease ( m_hIndexes.IterateGet () );

	m_hIndexes.Reset();
}

void GuardedHash_c::Rlock () const
{
	Verify ( m_tIndexesRWLock.ReadLock() );
}


void GuardedHash_c::Wlock () const
{
	Verify ( m_tIndexesRWLock.WriteLock() );
}


void GuardedHash_c::Unlock () const
{
	Verify ( m_tIndexesRWLock.Unlock() );
}


bool GuardedHash_c::Delete ( const CSphString & tKey )
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	ISphRefcountedMT ** ppEntry = m_hIndexes ( tKey );
	// release entry - last owner will free it
	if ( ppEntry )
		SafeRelease(*ppEntry);

	// remove from hash
	return m_hIndexes.Delete ( tKey );
}

bool GuardedHash_c::DeleteIfNull ( const CSphString & tKey )
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	ISphRefcountedMT ** ppEntry = m_hIndexes ( tKey );
	if ( ppEntry && *ppEntry )
		return false;
	return m_hIndexes.Delete ( tKey );
}

bool GuardedHash_c::AddUniq ( ISphRefcountedMT * pValue, const CSphString &tKey ) REQUIRES ( MainThread )
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	int iPrevSize = GetLengthUnl ();
	ISphRefcountedMT * &pVal = m_hIndexes.AddUnique ( tKey );
	if ( iPrevSize==GetLengthUnl () )
		return false;

	pVal = pValue;
	return true;
}

void GuardedHash_c::AddOrReplace ( ISphRefcountedMT * pValue, const CSphString &tKey )
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	// can not use AddUnique as new inserted item has no values
	ISphRefcountedMT ** ppEntry = m_hIndexes ( tKey );
	if ( ppEntry )
	{
		SafeRelease ( *ppEntry );
		(*ppEntry) = pValue;
	} else
	{
		Verify ( m_hIndexes.Add ( pValue, tKey ) );
	}
}

// check if hash contains an entry
bool GuardedHash_c::Contains ( const CSphString &tKey ) const
{
	ScRL_t hHashRLock { m_tIndexesRWLock };
	ISphRefcountedMT ** ppEntry = m_hIndexes ( tKey );
	return ppEntry!=nullptr;
}

ISphRefcountedMT * GuardedHash_c::Get ( const CSphString &tKey ) const
{
	ScRL_t hHashRLock { m_tIndexesRWLock };
	ISphRefcountedMT ** ppEntry = m_hIndexes ( tKey );
	if ( !ppEntry )
		return nullptr;
	if (!*ppEntry)
		return nullptr;
	(*ppEntry)->AddRef();
	return *ppEntry;
}

ISphRefcountedMT * GuardedHash_c::TryAddThenGet ( ISphRefcountedMT * pValue, const CSphString &tKey )
{
	ScWL_t hHashWLock { m_tIndexesRWLock };
	int iPrevSize = GetLengthUnl ();
	ISphRefcountedMT *&pVal = m_hIndexes.AddUnique ( tKey );
	if ( iPrevSize<GetLengthUnl () ) // value just inserted
	{
		pVal = pValue;
		if ( pVal )
			pVal->AddRef ();
	}

	if ( pVal )
		pVal->AddRef ();

	return pVal;
}


/////////////////////////////////////////////////////////////////////////////
// LOGGING
/////////////////////////////////////////////////////////////////////////////

void Shutdown (); // forward ref for sphFatal()


/// format current timestamp for logging
int sphFormatCurrentTime ( char * sTimeBuf, int iBufLen )
{
	int64_t iNow = sphMicroTimer ();
	time_t ts = (time_t) ( iNow/1000000 ); // on some systems (eg. FreeBSD 6.2), tv.tv_sec has another type and we can't just pass it

#if !USE_WINDOWS
	struct tm tmp;
	localtime_r ( &ts, &tmp );
#else
	struct tm tmp;
	tmp = *localtime ( &ts );
#endif

	static const char * sWeekday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char * sMonth[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	return snprintf ( sTimeBuf, iBufLen, "%.3s %.3s%3d %.2d:%.2d:%.2d.%.3d %d",
		sWeekday [ tmp.tm_wday ],
		sMonth [ tmp.tm_mon ],
		tmp.tm_mday, tmp.tm_hour,
		tmp.tm_min, tmp.tm_sec, (int)((iNow%1000000)/1000),
		1900+tmp.tm_year );
}


/// physically emit log entry
/// buffer must have 1 extra byte for linefeed
#if USE_WINDOWS
void sphLogEntry ( ESphLogLevel eLevel, char * sBuf, char * sTtyBuf )
#else
void sphLogEntry ( ESphLogLevel , char * sBuf, char * sTtyBuf )
#endif
{
#if USE_WINDOWS
	if ( g_bService && g_iLogFile==STDOUT_FILENO )
	{
		HANDLE hEventSource;
		LPCTSTR lpszStrings[2];

		hEventSource = RegisterEventSource ( NULL, g_sServiceName );
		if ( hEventSource )
		{
			lpszStrings[0] = g_sServiceName;
			lpszStrings[1] = sBuf;

			WORD eType = EVENTLOG_INFORMATION_TYPE;
			switch ( eLevel )
			{
				case SPH_LOG_FATAL:		eType = EVENTLOG_ERROR_TYPE; break;
				case SPH_LOG_WARNING:	eType = EVENTLOG_WARNING_TYPE; break;
				case SPH_LOG_INFO:		eType = EVENTLOG_INFORMATION_TYPE; break;
			}

			ReportEvent ( hEventSource,	// event log handle
				eType,					// event type
				0,						// event category
				0,						// event identifier
				NULL,					// no security identifier
				2,						// size of lpszStrings array
				0,						// no binary data
				lpszStrings,			// array of strings
				NULL );					// no binary data

			DeregisterEventSource ( hEventSource );
		}

	} else
#endif
	{
		strcat ( sBuf, "\n" ); // NOLINT

		sphSeek ( g_iLogFile, 0, SEEK_END );
		if ( g_bLogTty )
			sphWrite ( g_iLogFile, sTtyBuf, strlen(sTtyBuf) );
		else
			sphWrite ( g_iLogFile, sBuf, strlen(sBuf) );

		if ( g_bLogStdout && g_iLogFile!=STDOUT_FILENO )
			sphWrite ( STDOUT_FILENO, sTtyBuf, strlen(sTtyBuf) );
	}
}

/// log entry (with log levels, dupe catching, etc)
/// call with NULL format for dupe flushing
void sphLog ( ESphLogLevel eLevel, const char * sFmt, va_list ap )
{
	// dupe catcher state
	static const int	FLUSH_THRESH_TIME	= 1000000; // in microseconds
	static const int	FLUSH_THRESH_COUNT	= 100;

	static ESphLogLevel eLastLevel = SPH_LOG_INFO;
	static DWORD uLastEntry = 0;
	static int64_t tmLastStamp = -1000000-FLUSH_THRESH_TIME;
	static int iLastRepeats = 0;

	// only if we can
	if ( sFmt && eLevel>g_eLogLevel )
		return;

#if USE_SYSLOG
	if ( g_bLogSyslog && sFmt )
	{
		const int levels[SPH_LOG_MAX+1] = { LOG_EMERG, LOG_WARNING, LOG_INFO, LOG_DEBUG, LOG_DEBUG, LOG_DEBUG, LOG_DEBUG };
		vsyslog ( levels[eLevel], sFmt, ap );
		return;
	}
#endif

	if ( g_iLogFile<0 && !g_bService )
		return;

	// format the banner
	char sTimeBuf[128];
	sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );

	const char * sBanner = "";
	if ( sFmt==NULL ) eLevel = eLastLevel;
	if ( eLevel==SPH_LOG_FATAL ) sBanner = "FATAL: ";
	if ( eLevel==SPH_LOG_WARNING ) sBanner = "WARNING: ";
	if ( eLevel>=SPH_LOG_DEBUG ) sBanner = "DEBUG: ";
	if ( eLevel==SPH_LOG_RPL_DEBUG ) sBanner = "RPL: ";

	char sBuf [ 1024 ];
	snprintf ( sBuf, sizeof(sBuf)-1, "[%s] [%d] ", sTimeBuf, GetOsThreadId() );

	char * sTtyBuf = sBuf + strlen(sBuf);
	strncpy ( sTtyBuf, sBanner, 32 ); // 32 is arbitrary; just something that is enough and keeps lint happy

	int iLen = strlen(sBuf);

	// format the message
	if ( sFmt )
	{
		// need more space for tail zero and "\n" that added at sphLogEntry
		int iSafeGap = 4;
		int iBufSize = sizeof(sBuf)-iLen-iSafeGap;
		vsnprintf ( sBuf+iLen, iBufSize, sFmt, ap );
		sBuf[ sizeof(sBuf)-iSafeGap ] = '\0';
	}

	if ( sFmt && eLevel>SPH_LOG_INFO && g_iLogFilterLen )
	{
		if ( strncmp ( sBuf+iLen, g_sLogFilter, g_iLogFilterLen )!=0 )
			return;
	}

	// catch dupes
	DWORD uEntry = sFmt ? sphCRC32 ( sBuf+iLen ) : 0;
	int64_t tmNow = sphMicroTimer();

	// accumulate while possible
	if ( sFmt && eLevel==eLastLevel && uEntry==uLastEntry && iLastRepeats<FLUSH_THRESH_COUNT && tmNow<tmLastStamp+FLUSH_THRESH_TIME )
	{
		tmLastStamp = tmNow;
		iLastRepeats++;
		return;
	}

	// flush if needed
	if ( iLastRepeats!=0 && ( sFmt || tmNow>=tmLastStamp+FLUSH_THRESH_TIME ) )
	{
		// flush if we actually have something to flush, and
		// case 1: got a message we can't accumulate
		// case 2: got a periodic flush and been otherwise idle for a thresh period
		char sLast[256];
		iLen = Min ( iLen, 256 );
		strncpy ( sLast, sBuf, iLen );
		if ( iLen < 256 )
			snprintf ( sLast+iLen, sizeof(sLast)-iLen, "last message repeated %d times", iLastRepeats );
		sphLogEntry ( eLastLevel, sLast, sLast + ( sTtyBuf-sBuf ) );

		tmLastStamp = tmNow;
		iLastRepeats = 0;
		eLastLevel = SPH_LOG_INFO;
		uLastEntry = 0;
	}

	// was that a flush-only call?
	if ( !sFmt )
		return;

	tmLastStamp = tmNow;
	iLastRepeats = 0;
	eLastLevel = eLevel;
	uLastEntry = uEntry;

	// do the logging
	sphLogEntry ( eLevel, sBuf, sTtyBuf );
}


void sphFatal ( const char * sFmt, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );
void sphFatal ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( SPH_LOG_FATAL, sFmt, ap );
	va_end ( ap );
	Shutdown ();
	exit ( 1 );
}


void sphFatalLog ( const char * sFmt, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );
void sphFatalLog ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	sphLog ( SPH_LOG_FATAL, sFmt, ap );
	va_end ( ap );
}


#if !USE_WINDOWS
static CSphString GetNamedPipeName ( int iPid )
{
	CSphString sRes;
	sRes.SetSprintf ( "/tmp/searchd_%d", iPid );
	return sRes;
}
#endif

void LogWarning ( const char * sWarning )
{
	sphWarning ( "%s", sWarning );
}

void LogChangeMode ( int iFile, int iMode )
{
	if ( iFile<0 || iMode==0 || iFile==STDOUT_FILENO || iFile==STDERR_FILENO )
		return;

#if !USE_WINDOWS
	fchmod ( iFile, iMode );
#endif
}

/////////////////////////////////////////////////////////////////////////////

static int CmpString ( const CSphString & a, const CSphString & b )
{
	if ( !a.cstr() && !b.cstr() )
		return 0;

	if ( !a.cstr() || !b.cstr() )
		return a.cstr() ? -1 : 1;

	return strcmp ( a.cstr(), b.cstr() );
}

struct SearchFailure_t
{
public:
	CSphString	m_sParentIndex;
	CSphString	m_sIndex;	///< searched index name
	CSphString	m_sError;	///< search error message

public:
	SearchFailure_t () {}

public:
	bool operator == ( const SearchFailure_t & r ) const
	{
		return m_sIndex==r.m_sIndex && m_sError==r.m_sError && m_sParentIndex==r.m_sParentIndex;
	}

	bool operator < ( const SearchFailure_t & r ) const
	{
		int iRes = CmpString ( m_sError.cstr(), r.m_sError.cstr() );
		if ( !iRes )
			iRes = CmpString ( m_sParentIndex.cstr (), r.m_sParentIndex.cstr () );
		if ( !iRes )
			iRes = CmpString ( m_sIndex.cstr(), r.m_sIndex.cstr() );
		return iRes<0;
	}

	const SearchFailure_t & operator = ( const SearchFailure_t & r )
	{
		if ( this!=&r )
		{
			m_sParentIndex = r.m_sParentIndex;
			m_sIndex = r.m_sIndex;
			m_sError = r.m_sError;
		}
		return *this;
	}
};


static void ReportIndexesName ( int iSpanStart, int iSpandEnd, const CSphVector<SearchFailure_t> & dLog, StringBuilder_c & sOut );

class SearchFailuresLog_c
{
	CSphVector<SearchFailure_t>		m_dLog;

public:
	void Submit ( const char * sIndex, const char * sParentIndex , const char * sError )
	{
		SearchFailure_t & tEntry = m_dLog.Add ();
		tEntry.m_sParentIndex = sParentIndex;
		tEntry.m_sIndex = sIndex;
		tEntry.m_sError = sError;
	}

	void SubmitVa ( const char * sIndex, const char * sParentIndex, const char * sTemplate, va_list ap )
	{
		StringBuilder_c tError;
		tError.vAppendf ( sTemplate, ap );

		SearchFailure_t &tEntry = m_dLog.Add ();
		tEntry.m_sParentIndex = sParentIndex;
		tEntry.m_sIndex = sIndex;
		tError.MoveTo ( tEntry.m_sError );
	}

	inline void Submit ( const CSphString & sIndex, const char * sParentIndex, const char * sError )
	{
		Submit ( sIndex.cstr(), sParentIndex, sError );
	}

	void SubmitEx ( const char * sIndex, const char * sParentIndex, const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 4, 5 ) ) )
	{
		va_list ap;
		va_start ( ap, sTemplate );
		SubmitVa ( sIndex, sParentIndex, sTemplate, ap);
		va_end ( ap );
	}

	void SubmitEx ( const CSphString &sIndex, const char * sParentIndex, const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 4, 5 ) ) )
	{
		va_list ap;
		va_start ( ap, sTemplate );
		SubmitVa ( sIndex.cstr(), sParentIndex, sTemplate, ap );
		va_end ( ap );
	}

	bool IsEmpty ()
	{
		return m_dLog.GetLength()==0;
	}

	int GetReportsCount()
	{
		return m_dLog.GetLength();
	}

	void BuildReport ( StringBuilder_c & sReport )
	{
		if ( IsEmpty() )
			return;

		// collapse same messages
		m_dLog.Uniq ();
		int iSpanStart = 0;
		Comma_c sDelimiter (";\n");

		for ( int i=1; i<=m_dLog.GetLength(); ++i )
		{
			// keep scanning while error text is the same
			if ( i!=m_dLog.GetLength() )
				if ( m_dLog[i].m_sError==m_dLog[i-1].m_sError )
					continue;

			sReport << sDelimiter << "index ";

			ReportIndexesName ( iSpanStart, i, m_dLog, sReport );
			sReport << m_dLog[iSpanStart].m_sError;

			// done
			iSpanStart = i;
		}
	}
};

/////////////////////////////////////////////////////////////////////////////
// SIGNAL HANDLERS
/////////////////////////////////////////////////////////////////////////////


static bool SaveIndexes ()
{
	CSphString sError;
	bool bAllSaved = true;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
	{
		ServedDescRPtr_c pServed (it.Get());
		if ( pServed && !pServed->m_pIndex->SaveAttributes ( sError ) )
		{
			sphWarning ( "index %s: attrs save failed: %s", it.GetName ().cstr(), sError.cstr() );
			bAllSaved = false;
		}
	}
	return bAllSaved;
}

void Shutdown () REQUIRES ( MainThread ) NO_THREAD_SAFETY_ANALYSIS
{
#if !USE_WINDOWS
	int fdStopwait = -1;
#endif
	bool bAttrsSaveOk = true;
	g_bShutdown = true;
	if ( !g_bDaemonAtShutdown.IsEmpty() )
	{
		*g_bDaemonAtShutdown.GetWritePtr() = 1;
	}

#if !USE_WINDOWS
	// stopwait handshake
	CSphString sPipeName = GetNamedPipeName ( getpid() );
	fdStopwait = ::open ( sPipeName.cstr(), O_WRONLY | O_NONBLOCK );
	if ( fdStopwait>=0 )
	{
		DWORD uHandshakeOk = 0;
		int VARIABLE_IS_NOT_USED iDummy = ::write ( fdStopwait, &uHandshakeOk, sizeof(DWORD) );
	}
#endif
	g_tRotationServiceThread.Join();
	g_tPingThread.Join();

	// force even long time searches to shut
	sphInterruptNow();

	// tell flush-rt thread to shutdown, and wait until it does
	g_tRtFlushThread.Join();
	if ( g_tBinlogAutoflush.m_fnWork )
		g_tBinlogFlushThread.Join();

	// tell rotation thread to shutdown, and wait until it does
	if ( g_bSeamlessRotate )
	{
		g_tRotateThread.Join();
	}

	// tell uservars flush thread to shutdown, and wait until it does
	if ( !g_sSphinxqlState.IsEmpty() )
		g_tSphinxqlStateFlushThread.Join();

	g_tOptimizeThread.Join();

	g_tPrereadThread.Join();

	int64_t tmShutStarted = sphMicroTimer();
	// stop search threads; up to shutdown_timeout seconds
	while ( ( ThreadsNum() > 0 || g_bPrereading ) && ( sphMicroTimer()-tmShutStarted )<g_iShutdownTimeout )
		sphSleepMsec ( 50 );

	if ( g_pThdPool )
	{
		g_pThdPool->Shutdown();
		SafeDelete ( g_pThdPool );
		ARRAY_FOREACH ( i, g_dTickPoolThread )
			sphThreadJoin ( g_dTickPoolThread.Begin() + i );
	}

	// save attribute updates for all local indexes
	bAttrsSaveOk = SaveIndexes();

	// right before unlock loop
	JsonDoneConfig();

	// unlock indexes and release locks if needed
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
	{
		ServedDescRPtr_c pIdx ( it.Get() );
		if ( pIdx && pIdx->m_pIndex )
			pIdx->m_pIndex->Unlock();
	}
	SafeDelete ( g_pLocalIndexes );

	// unlock Distr indexes automatically done by d-tr
	SafeDelete ( g_pDistIndexes );

	// clear shut down of rt indexes + binlog
	sphDoneIOStats();
	sphRTDone();

	ReplicateClustersDelete();

	sphShutdownWordforms ();
	sphShutdownGlobalIDFs ();
	sphAotShutdown ();
	sphRLPDone();

	ARRAY_FOREACH ( i, g_dListeners )
		if ( g_dListeners[i].m_iSock>=0 )
			sphSockClose ( g_dListeners[i].m_iSock );

	ClosePersistentSockets();

	// close pid
	if ( g_iPidFD!=-1 )
		::close ( g_iPidFD );
	g_iPidFD = -1;

	// remove pid file, if we owned it
	if ( g_bPidIsMine && !g_sPidFile.IsEmpty() )
		::unlink ( g_sPidFile.cstr() );

	sphInfo ( "shutdown complete" );

	SphCrashLogger_c::Done();
	sphThreadDone ( g_iLogFile );

#if USE_WINDOWS
	CloseHandle ( g_hPipe );
#else
	if ( fdStopwait>=0 )
	{
		DWORD uStatus = bAttrsSaveOk;
		int VARIABLE_IS_NOT_USED iDummy = ::write ( fdStopwait, &uStatus, sizeof(DWORD) );
		::close ( fdStopwait );
	}
#endif
}

void sighup ( int )
{
	g_bGotSighup = 1;
}


void sigterm ( int )
{
	// tricky bit
	// we can't call exit() here because malloc()/free() are not re-entrant
	// we could call _exit() but let's try to die gracefully on TERM
	// and let signal sender wait and send KILL as needed
	g_bGotSigterm = 1;
	sphInterruptNow();
}


void sigusr1 ( int )
{
	g_bGotSigusr1 = 1;
}

struct QueryCopyState_t
{
	BYTE * m_pDst;
	BYTE * m_pDstEnd;
	const BYTE * m_pSrc;
	const BYTE * m_pSrcEnd;
};

// crash query handler
static const int g_iQueryLineLen = 80;
static const char g_dEncodeBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
bool sphCopyEncodedBase64 ( QueryCopyState_t & tEnc )
{
	BYTE * pDst = tEnc.m_pDst;
	const BYTE * pDstBase = tEnc.m_pDst;
	const BYTE * pSrc = tEnc.m_pSrc;
	const BYTE * pDstEnd = tEnc.m_pDstEnd-5;
	const BYTE * pSrcEnd = tEnc.m_pSrcEnd-3;

	while ( pDst<=pDstEnd && pSrc<=pSrcEnd )
	{
		// put line delimiter at max line length
		if ( ( ( pDst-pDstBase ) % g_iQueryLineLen )>( ( pDst-pDstBase+4 ) % g_iQueryLineLen ) )
			*pDst++ = '\n';

		// Convert to big endian
		DWORD uSrc = ( pSrc[0] << 16 ) | ( pSrc[1] << 8 ) | ( pSrc[2] );
		pSrc += 3;

		*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x00FC0000 ) >> 18 ];
		*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x0003F000 ) >> 12 ];
		*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x00000FC0 ) >> 6 ];
		*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x0000003F ) ];
	}

	// there is a tail in source data and a room for it at destination buffer
	if ( pSrc<tEnc.m_pSrcEnd && ( tEnc.m_pSrcEnd-pSrc<3 ) && ( pDst<=pDstEnd-4 ) )
	{
		int iLeft = ( tEnc.m_pSrcEnd - pSrc ) % 3;
		if ( iLeft==1 )
		{
			DWORD uSrc = pSrc[0]<<16;
			pSrc += 1;
			*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x00FC0000 ) >> 18 ];
			*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x0003F000 ) >> 12 ];
			*pDst++ = '=';
			*pDst++ = '=';
		} else if ( iLeft==2 )
		{
			DWORD uSrc = ( pSrc[0]<<16 ) | ( pSrc[1] << 8 );
			pSrc += 2;
			*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x00FC0000 ) >> 18 ];
			*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x0003F000 ) >> 12 ];
			*pDst++ = g_dEncodeBase64 [ ( uSrc & 0x00000FC0 ) >> 6 ];
			*pDst++ = '=';
		}
	}

	tEnc.m_pDst = pDst;
	tEnc.m_pSrc = pSrc;

	return ( tEnc.m_pSrc<tEnc.m_pSrcEnd );
}

static bool sphCopySphinxQL ( QueryCopyState_t & tState )
{
	BYTE * pDst = tState.m_pDst;
	const BYTE * pSrc = tState.m_pSrc;
	BYTE * pNextLine = pDst+g_iQueryLineLen;

	while ( pDst<tState.m_pDstEnd && pSrc<tState.m_pSrcEnd )
	{
		if ( pDst>pNextLine && pDst+1<tState.m_pDstEnd && ( sphIsSpace ( *pSrc ) || *pSrc==',' ) )
		{
			*pDst++ = *pSrc++;
			*pDst++ = '\n';
			pNextLine = pDst + g_iQueryLineLen;
		} else
		{
			*pDst++ = *pSrc++;
		}
	}

	tState.m_pDst = pDst;
	tState.m_pSrc = pSrc;

	return ( tState.m_pSrc<tState.m_pSrcEnd );
}

static bool sphCopySphinxHttp ( QueryCopyState_t & tState )
{
	BYTE * pDst = tState.m_pDst;
	const BYTE * pSrc = tState.m_pSrc;

	while ( pDst<tState.m_pDstEnd && pSrc<tState.m_pSrcEnd )
	{
		*pDst++ = *pSrc++;
	}

	tState.m_pDst = pDst;
	tState.m_pSrc = pSrc;

	return ( tState.m_pSrc<tState.m_pSrcEnd );
}


typedef bool CopyQuery_fn ( QueryCopyState_t & tState );

#define SPH_TIME_PID_MAX_SIZE 256
const char		g_sCrashedBannerAPI[] = "\n--- crashed SphinxAPI request dump ---\n";
const char		g_sCrashedBannerMySQL[] = "\n--- crashed SphinxQL request dump ---\n";
const char		g_sCrashedBannerHTTP[] = "\n--- crashed HTTP request dump ---\n";
const char		g_sCrashedBannerBad[] = "\n--- crashed invalid query ---\n";
const char		g_sCrashedBannerTail[] = "\n--- request dump end ---\n";
#if USE_WINDOWS
const char		g_sMinidumpBanner[] = "minidump located at: ";
#endif
#if SPH_ALLOCS_PROFILER
const char		g_sMemoryStatBanner[] = "\n--- memory statistics ---\n";
#endif
static BYTE		g_dCrashQueryBuff [4096];
static char		g_sCrashInfo [SPH_TIME_PID_MAX_SIZE] = "[][]\n";
static int		g_iCrashInfoLen = 0;

#if USE_WINDOWS
static char		g_sMinidump[SPH_TIME_PID_MAX_SIZE] = "";
#endif

SphThreadKey_t SphCrashLogger_c::m_tTLS = SphThreadKey_t ();

static CrashQuery_t g_tUnhandled;

// lets invalidate pointer when this instance goes out of scope to get immediate crash
// instead of a reference to incorrect stack frame in case of some programming error
SphCrashLogger_c::~SphCrashLogger_c () { sphThreadSet ( m_tTLS, NULL ); }

void SphCrashLogger_c::Init ()
{
	sphBacktraceInit();
	Verify ( sphThreadKeyCreate ( &m_tTLS ) );
}

void SphCrashLogger_c::Done ()
{
	sphThreadKeyDelete ( m_tTLS );
}


#if !USE_WINDOWS
void SphCrashLogger_c::HandleCrash ( int sig ) NO_THREAD_SAFETY_ANALYSIS
#else
LONG WINAPI SphCrashLogger_c::HandleCrash ( EXCEPTION_POINTERS * pExc )
#endif // !USE_WINDOWS
{
	if ( g_iLogFile<0 )
	{
		if ( g_bCoreDump )
		{
			CRASH_EXIT_CORE;
		} else
		{
			CRASH_EXIT;
		}
	}

	// log [time][pid]
	sphSeek ( g_iLogFile, 0, SEEK_END );
	sphWrite ( g_iLogFile, g_sCrashInfo, g_iCrashInfoLen );

	// log query
	CrashQuery_t tQuery = SphCrashLogger_c::GetQuery();

	bool bValidQuery = ( tQuery.m_pQuery && tQuery.m_iSize>0 );
#if !USE_WINDOWS
	if ( bValidQuery )
	{
		size_t iPageSize = getpagesize();

		// FIXME! That is too complex way, remove all of this and just move query dump to the bottom
		// remove also mincore_test.cmake, it's invokation from CMakeLists.txt and HAVE_UNSIGNED_MINCORE
		// declatarion from config_cmake.h.in
#if HAVE_UNSIGNED_MINCORE
		BYTE dPages = 0;
#else
		char dPages = 0;
#endif

		uintptr_t pPageStart = (uintptr_t )( tQuery.m_pQuery );
		pPageStart &= ~( iPageSize - 1 );
		bValidQuery &= ( mincore ( ( void * ) pPageStart, 1, &dPages )==0 );

		uintptr_t pPageEnd = (uintptr_t )( tQuery.m_pQuery + tQuery.m_iSize - 1 );
		pPageEnd &= ~( iPageSize - 1 );
		bValidQuery &= ( mincore ( ( void * ) pPageEnd, 1, &dPages )==0 );
	}
#endif

	// request dump banner
	const char * pBanner = g_sCrashedBannerAPI;
	int iBannerLen = sizeof(g_sCrashedBannerAPI) - 1;
	if ( tQuery.m_bMySQL )
	{
		pBanner = g_sCrashedBannerMySQL;
		iBannerLen = sizeof(g_sCrashedBannerMySQL) - 1;
	}
	if ( tQuery.m_bHttp )
	{
		pBanner = g_sCrashedBannerHTTP;
		iBannerLen = sizeof(g_sCrashedBannerHTTP) - 1;
	}
	if ( !bValidQuery )
	{
		iBannerLen = sizeof(g_sCrashedBannerBad) - 1;
		pBanner = g_sCrashedBannerBad;
	}
	sphWrite ( g_iLogFile, pBanner, iBannerLen );

	// query
	if ( bValidQuery )
	{
		QueryCopyState_t tCopyState;
		tCopyState.m_pDst = g_dCrashQueryBuff;
		tCopyState.m_pDstEnd = g_dCrashQueryBuff + sizeof(g_dCrashQueryBuff);
		tCopyState.m_pSrc = tQuery.m_pQuery;
		tCopyState.m_pSrcEnd = tQuery.m_pQuery + tQuery.m_iSize;

		CopyQuery_fn * pfnCopy = NULL;
		if ( !tQuery.m_bMySQL && !tQuery.m_bHttp )
		{
			pfnCopy = &sphCopyEncodedBase64;

			// should be power of 3 to seamlessly convert to BASE64
			BYTE dHeader[] = {
				(BYTE)( ( tQuery.m_uCMD>>8 ) & 0xff ),
				(BYTE)( tQuery.m_uCMD & 0xff ),
				(BYTE)( ( tQuery.m_uVer>>8 ) & 0xff ),
				(BYTE)( tQuery.m_uVer & 0xff ),
				(BYTE)( ( tQuery.m_iSize>>24 ) & 0xff ),
				(BYTE)( ( tQuery.m_iSize>>16 ) & 0xff ),
				(BYTE)( ( tQuery.m_iSize>>8 ) & 0xff ),
				(BYTE)( tQuery.m_iSize & 0xff ),
				*tQuery.m_pQuery
			};

			QueryCopyState_t tHeaderState;
			tHeaderState.m_pDst = g_dCrashQueryBuff;
			tHeaderState.m_pDstEnd = g_dCrashQueryBuff + sizeof(g_dCrashQueryBuff);
			tHeaderState.m_pSrc = dHeader;
			tHeaderState.m_pSrcEnd = dHeader + sizeof(dHeader);
			pfnCopy ( tHeaderState );
			assert ( tHeaderState.m_pSrc==tHeaderState.m_pSrcEnd );
			tCopyState.m_pDst = tHeaderState.m_pDst;
			tCopyState.m_pSrc++;
		} else if ( tQuery.m_bHttp )
		{
			pfnCopy = &sphCopySphinxHttp;
		} else
		{
			pfnCopy = &sphCopySphinxQL;
		}

		while ( pfnCopy ( tCopyState ) )
		{
			sphWrite ( g_iLogFile, g_dCrashQueryBuff, tCopyState.m_pDst-g_dCrashQueryBuff );
			tCopyState.m_pDst = g_dCrashQueryBuff; // reset the destination buffer
		}
		assert ( tCopyState.m_pSrc==tCopyState.m_pSrcEnd );

		int iLeft = tCopyState.m_pDst-g_dCrashQueryBuff;
		if ( iLeft>0 )
		{
			sphWrite ( g_iLogFile, g_dCrashQueryBuff, iLeft );
		}
	}

	// tail
	sphWrite ( g_iLogFile, g_sCrashedBannerTail, sizeof(g_sCrashedBannerTail)-1 );

	sphSafeInfo ( g_iLogFile, "Manticore " SPHINX_VERSION );

#if USE_WINDOWS
	// mini-dump reference
	int iMiniDumpLen = snprintf ( (char *)g_dCrashQueryBuff, sizeof(g_dCrashQueryBuff),
		"%s %s.%p.mdmp\n", g_sMinidumpBanner, g_sMinidump, tQuery.m_pQuery );
	sphWrite ( g_iLogFile, g_dCrashQueryBuff, iMiniDumpLen );
	snprintf ( (char *)g_dCrashQueryBuff, sizeof(g_dCrashQueryBuff), "%s.%p.mdmp",
		g_sMinidump, tQuery.m_pQuery );
#endif

	// log trace
#if !USE_WINDOWS
	sphSafeInfo ( g_iLogFile, "Handling signal %d", sig );
	// print message to stdout during daemon start
	if ( g_bLogStdout && g_iLogFile!=STDOUT_FILENO )
		sphSafeInfo ( STDOUT_FILENO, "Crash!!! Handling signal %d", sig );
	sphBacktrace ( g_iLogFile, g_bSafeTrace );
#else
	sphBacktrace ( pExc, (char *)g_dCrashQueryBuff );
#endif

	// threads table
	// FIXME? should we try to lock threads table somehow?
	sphSafeInfo ( g_iLogFile, "--- %d active threads ---", g_dThd.GetLength() );

	int iThd = 0;
	for ( const ListNode_t * pIt = g_dThd.Begin (); pIt!=g_dThd.End(); pIt = pIt->m_pNext )
	{
		auto * pThd = (ThdDesc_t *)pIt;
		sphSafeInfo ( g_iLogFile, "thd %d, proto %s, state %s, command %s",
			iThd,
			g_dProtoNames[pThd->m_eProto],
			g_dThdStates[pThd->m_eThdState],
			pThd->m_sCommand ? pThd->m_sCommand : "-" );
		++iThd;
	}

	// memory info
#if SPH_ALLOCS_PROFILER
	sphWrite ( g_iLogFile, g_sMemoryStatBanner, sizeof ( g_sMemoryStatBanner )-1 );
	sphMemStatDump ( g_iLogFile );
#endif

	sphSafeInfo ( g_iLogFile, "------- CRASH DUMP END -------" );

	if ( g_bCoreDump )
	{
		CRASH_EXIT_CORE;
	} else
	{
		CRASH_EXIT;
	}
}

void SphCrashLogger_c::SetLastQuery ( const CrashQuery_t & tQuery )
{
	CrashQuery_t * pQuery = (CrashQuery_t *)sphThreadGet ( m_tTLS );
	assert ( pQuery );
	*pQuery = tQuery;
}

void SphCrashLogger_c::SetupTimePID ()
{
	char sTimeBuf[SPH_TIME_PID_MAX_SIZE];
	sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );

	g_iCrashInfoLen = snprintf ( g_sCrashInfo, SPH_TIME_PID_MAX_SIZE-1,
		"------- FATAL: CRASH DUMP -------\n[%s] [%5d]\n", sTimeBuf, (int)getpid() );
}

void SphCrashLogger_c::SetTopQueryTLS ( CrashQuery_t * pQuery )
{
	Verify ( sphThreadSet ( m_tTLS, pQuery ) );
}

CrashQuery_t SphCrashLogger_c::GetQuery()
{
	const CrashQuery_t * pQuery = (CrashQuery_t *)sphThreadGet ( m_tTLS );

	// in case TLS not set \ found handler still should process crash
	// FIXME!!! some service threads use raw threads instead ThreadCreate
	if ( !pQuery )
		return g_tUnhandled;
	else
		return *pQuery;
}

bool SphCrashLogger_c::ThreadCreate ( SphThread_t * pThread, void (*pCall)(void*), void * pArg, bool bDetached, const char* sName )
{
	auto * pWrapperArg = new CallArgPair_t ( pCall, pArg );
	bool bSuccess = sphThreadCreate ( pThread, ThreadWrapper, pWrapperArg, bDetached, sName );
	if ( !bSuccess )
		SafeDelete ( pWrapperArg );
	return bSuccess;
}

void SphCrashLogger_c::ThreadWrapper ( void * pArg )
{
	auto * pPair = static_cast<CallArgPair_t *> ( pArg );
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );
	pPair->m_pCall ( pPair->m_pArg );
	SafeDelete( pPair );
}


#if USE_WINDOWS
void SetSignalHandlers ( bool )
{
	SphCrashLogger_c::Init();
	snprintf ( g_sMinidump, SPH_TIME_PID_MAX_SIZE-1, "%s.%d", g_sPidFile.scstr(), (int)getpid() );
	SetUnhandledExceptionFilter ( SphCrashLogger_c::HandleCrash );
}
#else
void SetSignalHandlers ( bool bAllowCtrlC=false ) REQUIRES ( MainThread )
{
	SphCrashLogger_c::Init();
	struct sigaction sa;
	sigfillset ( &sa.sa_mask );
	sa.sa_flags = SA_NOCLDSTOP;

	bool bSignalsSet = false;
	while (true)
	{
		sa.sa_handler = sigterm;	if ( sigaction ( SIGTERM, &sa, NULL )!=0 ) break;
		if ( !bAllowCtrlC )
		{
			sa.sa_handler = sigterm;
			if ( sigaction ( SIGINT, &sa, NULL )!=0 )
				break;
		}
		sa.sa_handler = sighup;		if ( sigaction ( SIGHUP, &sa, NULL )!=0 ) break;
		sa.sa_handler = sigusr1;	if ( sigaction ( SIGUSR1, &sa, NULL )!=0 ) break;
		sa.sa_handler = SIG_IGN;	if ( sigaction ( SIGPIPE, &sa, NULL )!=0 ) break;

		sa.sa_flags |= SA_RESETHAND;
		sa.sa_handler = SphCrashLogger_c::HandleCrash;	if ( sigaction ( SIGSEGV, &sa, NULL )!=0 ) break;
		sa.sa_handler = SphCrashLogger_c::HandleCrash;	if ( sigaction ( SIGBUS, &sa, NULL )!=0 ) break;
		sa.sa_handler = SphCrashLogger_c::HandleCrash;	if ( sigaction ( SIGABRT, &sa, NULL )!=0 ) break;
		sa.sa_handler = SphCrashLogger_c::HandleCrash;	if ( sigaction ( SIGILL, &sa, NULL )!=0 ) break;
		sa.sa_handler = SphCrashLogger_c::HandleCrash;	if ( sigaction ( SIGFPE, &sa, NULL )!=0 ) break;

		bSignalsSet = true;
		break;
	}
	if ( !bSignalsSet )
		sphFatal ( "sigaction(): %s", strerrorm(errno) );
}
#endif


/////////////////////////////////////////////////////////////////////////////
// NETWORK STUFF
/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS

const int		WIN32_PIPE_BUFSIZE		= 32;

/// on Windows, the wrapper just prevents the warnings

#pragma warning(push) // store current warning values
#pragma warning(disable:4127) // conditional expr is const
#pragma warning(disable:4389) // signed/unsigned mismatch

void sphFDSet ( int fd, fd_set * fdset )
{
	FD_SET ( fd, fdset );
}

void sphFDClr ( int fd, fd_set * fdset )
{
	FD_SET ( fd, fdset );
}

#pragma warning(pop) // restore warnings

#else // !USE_WINDOWS

#define SPH_FDSET_OVERFLOW(_fd) ( (_fd)<0 || (_fd)>=(int)FD_SETSIZE )

/// on UNIX, we also check that the descript won't corrupt the stack
void sphFDSet ( int fd, fd_set * set )
{
	if ( SPH_FDSET_OVERFLOW(fd) )
		sphFatal ( "sphFDSet() failed fd=%d, FD_SETSIZE=%d", fd, FD_SETSIZE );
	else
		FD_SET ( fd, set );
}

void sphFDClr ( int fd, fd_set * set )
{
	if ( SPH_FDSET_OVERFLOW( fd ) )
		sphFatal ( "sphFDClr() failed fd=%d, FD_SETSIZE=%d", fd, FD_SETSIZE );
	else
		FD_CLR ( fd, set );
}

#endif // USE_WINDOWS


#if USE_WINDOWS
const char * sphSockError ( int iErr )
{
	if ( iErr==0 )
		iErr = WSAGetLastError ();

	static char sBuf [ 256 ];
	_snprintf ( sBuf, sizeof(sBuf), "WSA error %d", iErr );
	return sBuf;
}
#else
const char * sphSockError ( int )
{
	return strerrorm ( errno );
}
#endif


int sphSockGetErrno ()
{
	#if USE_WINDOWS
		return WSAGetLastError();
	#else
		return errno;
	#endif
}


void sphSockSetErrno ( int iErr )
{
	#if USE_WINDOWS
		WSASetLastError ( iErr );
	#else
		errno = iErr;
	#endif
}


int sphSockPeekErrno ()
{
	int iRes = sphSockGetErrno();
	sphSockSetErrno ( iRes );
	return iRes;
}


/// formats IP address given in network byte order into sBuffer
/// returns the buffer
char * sphFormatIP ( char * sBuffer, int iBufferSize, DWORD uAddress )
{
	const BYTE *a = (const BYTE *)&uAddress;
	snprintf ( sBuffer, iBufferSize, "%u.%u.%u.%u", a[0], a[1], a[2], a[3] );
	return sBuffer;
}


static const bool GETADDR_STRICT = true; ///< strict check, will die with sphFatal() on failure

DWORD sphGetAddress ( const char * sHost, bool bFatal, bool bIP )
{
	struct addrinfo tHints, *pResult = nullptr;
	memset ( &tHints, 0, sizeof(tHints));
	tHints.ai_family = AF_INET;
	tHints.ai_socktype = SOCK_STREAM;
	if ( bIP )
		tHints.ai_flags = AI_NUMERICHOST;

	int iResult = getaddrinfo ( sHost, nullptr, &tHints, &pResult );
	auto pOrigResult = pResult;
	if ( iResult!=0 || !pResult )
	{
		if ( bFatal )
			sphFatal ( "no AF_INET address found for: %s", sHost );
		else
			sphLogDebugv ( "no AF_INET address found for: %s", sHost );
		return 0;
	}

	assert ( pResult );
	auto * pSockaddr_ipv4 = (struct sockaddr_in *) pResult->ai_addr;
	DWORD uAddr = pSockaddr_ipv4->sin_addr.s_addr;

	if ( pResult->ai_next )
	{
		StringBuilder_c sBuf ( "; ip=", "ip=" );
		for ( ; pResult->ai_next; pResult = pResult->ai_next )
		{
			char sAddrBuf [ SPH_ADDRESS_SIZE ];
			auto * pAddr = (struct sockaddr_in *)pResult->ai_addr;
			DWORD uNextAddr = pAddr->sin_addr.s_addr;
			sphFormatIP ( sAddrBuf, sizeof(sAddrBuf), uNextAddr );
			sBuf << sAddrBuf;
		}

		sphWarning ( "multiple addresses found for '%s', using the first one (%s)", sHost, sBuf.cstr() );
	}

	freeaddrinfo ( pOrigResult );
	return uAddr;
}


#if !USE_WINDOWS
int sphCreateUnixSocket ( const char * sPath )
{
	static struct sockaddr_un uaddr;
	size_t len = strlen ( sPath );

	if ( len + 1 > sizeof( uaddr.sun_path ) )
		sphFatal ( "UNIX socket path is too long (len=%d)", (int)len );

	sphInfo ( "listening on UNIX socket %s", sPath );

	memset ( &uaddr, 0, sizeof(uaddr) );
	uaddr.sun_family = AF_UNIX;
	memcpy ( uaddr.sun_path, sPath, len + 1 );

	int iSock = socket ( AF_UNIX, SOCK_STREAM, 0 );
	if ( iSock==-1 )
		sphFatal ( "failed to create UNIX socket: %s", sphSockError() );

	if ( unlink ( sPath )==-1 )
	{
		if ( errno!=ENOENT )
			sphFatal ( "unlink() on UNIX socket file failed: %s", sphSockError() );
	}

	int iMask = umask ( 0 );
	if ( bind ( iSock, (struct sockaddr *)&uaddr, sizeof(uaddr) )!=0 )
		sphFatal ( "bind() on UNIX socket failed: %s", sphSockError() );
	umask ( iMask );

	return iSock;
}
#endif // !USE_WINDOWS


int sphCreateInetSocket ( DWORD uAddr, int iPort )
{
	char sAddress[SPH_ADDRESS_SIZE];
	sphFormatIP ( sAddress, SPH_ADDRESS_SIZE, uAddr );

	if ( uAddr==htonl ( INADDR_ANY ) )
		sphInfo ( "listening on all interfaces, port=%d", iPort );
	else
		sphInfo ( "listening on %s:%d", sAddress, iPort );

	static struct sockaddr_in iaddr;
	memset ( &iaddr, 0, sizeof(iaddr) );
	iaddr.sin_family = AF_INET;
	iaddr.sin_addr.s_addr = uAddr;
	iaddr.sin_port = htons ( (short)iPort );

	int iSock = socket ( AF_INET, SOCK_STREAM, 0 );
	if ( iSock==-1 )
		sphFatal ( "failed to create TCP socket: %s", sphSockError() );

	int iOn = 1;
	if ( setsockopt ( iSock, SOL_SOCKET, SO_REUSEADDR, (char*)&iOn, sizeof(iOn) ) )
		sphWarning ( "setsockopt(SO_REUSEADDR) failed: %s", sphSockError() );
#if HAVE_SO_REUSEPORT
	if ( setsockopt ( iSock, SOL_SOCKET, SO_REUSEPORT, (char*)&iOn, sizeof(iOn) ) )
		sphWarning ( "setsockopt(SO_REUSEPORT) failed: %s", sphSockError() );
#endif
#ifdef TCP_NODELAY
	if ( setsockopt ( iSock, IPPROTO_TCP, TCP_NODELAY, (char*)&iOn, sizeof(iOn) ) )
		sphWarning ( "setsockopt(TCP_NODELAY) failed: %s", sphSockError() );
#endif

#ifdef TCP_FASTOPEN
	if ( ( g_iTFO!=TFO_ABSENT ) && ( g_iTFO & TFO_LISTEN ) )
	{
		if ( setsockopt ( iSock, IPPROTO_TCP, TCP_FASTOPEN, ( char * ) &iOn, sizeof ( iOn ) ) )
			sphWarning ( "setsockopt(TCP_FASTOPEN) failed: %s", sphSockError () );
	}
#endif

	int iTries = 12;
	int iRes;
	do
	{
		iRes = bind ( iSock, (struct sockaddr *)&iaddr, sizeof(iaddr) );
		if ( iRes==0 )
			break;

		sphInfo ( "bind() failed on %s, retrying...", sAddress );
		sphSleepMsec ( 3000 );
	} while ( --iTries>0 );
	if ( iRes )
		sphFatal ( "bind() failed on %s: %s", sAddress, sphSockError() );

	return iSock;
}


bool IsPortInRange ( int iPort )
{
	return ( iPort>0 ) && ( iPort<=0xFFFF );
}


void CheckPort ( int iPort )
{
	if ( !IsPortInRange(iPort) )
		sphFatal ( "port %d is out of range", iPort );
}

void ProtoByName ( const CSphString & sProto, ListenerDesc_t & tDesc )
{
	if ( sProto=="sphinx" )			tDesc.m_eProto = PROTO_SPHINX;
	else if ( sProto=="mysql41" )	tDesc.m_eProto = PROTO_MYSQL41;
	else if ( sProto=="http" )		tDesc.m_eProto = PROTO_HTTP;
	else if ( sProto=="replication" )	tDesc.m_eProto = PROTO_REPLICATION;
	else if ( sProto=="sphinx_vip" )
	{
		tDesc.m_eProto = PROTO_SPHINX;
		tDesc.m_bVIP = true;
	} else if ( sProto=="mysql41_vip" )
	{
		tDesc.m_eProto = PROTO_MYSQL41;
		tDesc.m_bVIP = true;
	} else if ( sProto=="http_vip" )
	{
		tDesc.m_eProto = PROTO_HTTP;
		tDesc.m_bVIP = true;
	} else
	{
		sphFatal ( "unknown listen protocol type '%s'", sProto.cstr() ? sProto.cstr() : "(NULL)" );
	}
}

ListenerDesc_t ParseListener ( const char * sSpec )
{
	ListenerDesc_t tRes;
	tRes.m_eProto = PROTO_SPHINX;
	tRes.m_sUnix = "";
	tRes.m_uIP = htonl ( INADDR_ANY );
	tRes.m_iPort = SPHINXAPI_PORT;
	tRes.m_bVIP = false;

	// split by colon
	int iParts = 0;
	CSphString sParts[3];

	const char * sPart = sSpec;
	for ( const char * p = sSpec; ; p++ )
		if ( *p=='\0' || *p==':' )
	{
		if ( iParts==3 )
			sphFatal ( "invalid listen format (too many fields)" );

		sParts[iParts++].SetBinary ( sPart, p-sPart );
		if ( !*p )
			break; // bail out on zero

		sPart = p+1;
	}
	assert ( iParts>=1 && iParts<=3 );

	// handle UNIX socket case
	// might be either name on itself (1 part), or name+protocol (2 parts)
	sPart = sParts[0].cstr();
	if ( sPart[0]=='/' )
	{
		if ( iParts>2 )
			sphFatal ( "invalid listen format (too many fields)" );

		if ( iParts==2 )
			ProtoByName ( sParts[1], tRes );

#if USE_WINDOWS
		sphFatal ( "UNIX sockets are not supported on Windows" );
#else
		tRes.m_sUnix = sPart;
		return tRes;
#endif
	}

	// check if it all starts with a valid port number
	sPart = sParts[0].cstr();
	int iLen = strlen(sPart);

	bool bAllDigits = true;
	for ( int i=0; i<iLen && bAllDigits; i++ )
		if ( !isdigit ( sPart[i] ) )
			bAllDigits = false;

	int iPort = 0;
	if ( bAllDigits && iLen<=5 )
	{
		iPort = atol(sPart);
		CheckPort ( iPort ); // lets forbid ambiguous magic like 0:sphinx or 99999:mysql41
	}

	// handle TCP port case
	// one part. might be either port name, or host name, or UNIX socket name
	if ( iParts==1 )
	{
		if ( iPort )
		{
			// port name on itself
			tRes.m_uIP = htonl ( INADDR_ANY );
			tRes.m_iPort = iPort;
		} else
		{
			// host name on itself
			tRes.m_uIP = sphGetAddress ( sSpec, GETADDR_STRICT );
			tRes.m_iPort = SPHINXAPI_PORT;
		}
		return tRes;
	}

	// two or three parts
	if ( iPort )
	{
		// 1st part is a valid port number; must be port:proto
		if ( iParts!=2 )
			sphFatal ( "invalid listen format (expected port:proto, got extra trailing part in listen=%s)", sSpec );

		tRes.m_uIP = htonl ( INADDR_ANY );
		tRes.m_iPort = iPort;
		ProtoByName ( sParts[1], tRes );

	} else
	{
		// 1st part must be a host name; must be host:port[:proto]
		if ( iParts==3 )
			ProtoByName ( sParts[2], tRes );

		tRes.m_iPort = atol ( sParts[1].cstr() );
		CheckPort ( tRes.m_iPort );

		tRes.m_uIP = sParts[0].IsEmpty()
			? htonl ( INADDR_ANY )
			: sphGetAddress ( sParts[0].cstr(), GETADDR_STRICT );
	}
	return tRes;
}


void AddListener ( const CSphString & sListen, bool bHttpAllowed )
{
	ListenerDesc_t tDesc = ParseListener ( sListen.cstr() );

	Listener_t tListener;
	tListener.m_eProto = tDesc.m_eProto;
	tListener.m_bTcp = true;
	tListener.m_bVIP = tDesc.m_bVIP;

	if ( tDesc.m_eProto==PROTO_HTTP && !bHttpAllowed )
	{
		sphWarning ( "thread_pool disabled, can not listen for http interface, port=%d, use workers=thread_pool", tDesc.m_iPort );
		return;
	}

#if !USE_WINDOWS
	if ( !tDesc.m_sUnix.IsEmpty() )
	{
		tListener.m_iSock = sphCreateUnixSocket ( tDesc.m_sUnix.cstr() );
		tListener.m_bTcp = false;
	} else
#endif
		tListener.m_iSock = sphCreateInetSocket ( tDesc.m_uIP, tDesc.m_iPort );

	g_dListeners.Add ( tListener );
}


int sphSetSockNB ( int iSock )
{
	#if USE_WINDOWS
		u_long uMode = 1;
		return ioctlsocket ( iSock, FIONBIO, &uMode );
	#else
		return fcntl ( iSock, F_SETFL, O_NONBLOCK );
	#endif
}

/// wait until socket is readable or writable
int sphPoll ( int iSock, int64_t tmTimeout, bool bWrite=false )
{
	// don't need any epoll/kqueue here, since we check only 1 socket
#if HAVE_POLL
	struct pollfd pfd;
	pfd.fd = iSock;
	pfd.events = bWrite ? POLLOUT : POLLIN;

	return ::poll ( &pfd, 1, int ( tmTimeout/1000 ) );
#else
	fd_set fdSet;
	FD_ZERO ( &fdSet );
	sphFDSet ( iSock, &fdSet );

	struct timeval tv;
	tv.tv_sec = (int)( tmTimeout / 1000000 );
	tv.tv_usec = (int)( tmTimeout % 1000000 );

	return ::select ( iSock+1, bWrite ? NULL : &fdSet, bWrite ? &fdSet : NULL, NULL, &tv );
#endif
}

int RecvNBChunk ( int iSock, char * &pBuf, int& iLeftBytes)
{
	// try to receive next chunk
	auto iRes = sphSockRecv ( iSock, pBuf, iLeftBytes );

	if ( iRes>0 )
	{
		pBuf += iRes;
		iLeftBytes -= iRes;
	}
	return (int)iRes;
}


int sphSockRead ( int iSock, void * buf, int iLen, int iReadTimeout, bool bIntr )
{
	assert ( iLen>0 );

	int64_t tmMaxTimer = sphMicroTimer() + I64C(1000000)*Max ( 1, iReadTimeout ); // in microseconds
	int iLeftBytes = iLen; // bytes to read left

	auto pBuf = (char*) buf;
	int iErr = 0;
	int iRes = -1;

	while ( iLeftBytes>0 )
	{
		int64_t tmMicroLeft = tmMaxTimer - sphMicroTimer();
		if ( tmMicroLeft<=0 )
			break; // timed out

#if USE_WINDOWS
		// Windows EINTR emulation
		// Ctrl-C will not interrupt select on Windows, so let's handle that manually
		// forcibly limit select() to 100 ms, and check flag afterwards
		if ( bIntr )
			tmMicroLeft = Min ( tmMicroLeft, 100000 );
#endif

		// wait until there is data
		iRes = sphPoll ( iSock, tmMicroLeft );

		// if there was EINTR, retry
		// if any other error, bail
		if ( iRes==-1 )
		{
			// only let SIGTERM (of all them) to interrupt, and only if explicitly allowed
			iErr = sphSockGetErrno();
			if ( iErr==EINTR )
			{
				if ( !( g_bGotSigterm && bIntr ))
					continue;
				sphLogDebug ( "sphSockRead: select got SIGTERM, exit -1" );
			}
			return -1;
		}

		// if there was a timeout, report it as an error
		if ( iRes==0 )
		{
#if USE_WINDOWS
			// Windows EINTR emulation
			if ( bIntr )
			{
				// got that SIGTERM
				if ( g_bGotSigterm )
				{
					sphLogDebug ( "sphSockRead: got SIGTERM emulation on Windows, exit -1" );
					sphSockSetErrno ( EINTR );
					return -1;
				}

				// timeout might not be fully over just yet, so re-loop
				continue;
			}
#endif

			sphSockSetErrno ( ETIMEDOUT );
			return -1;
		}

		// try to receive next chunk
		iRes = RecvNBChunk ( iSock, pBuf, iLeftBytes );

		// if there was eof, we're done
		if ( !iRes )
		{
			sphSockSetErrno ( ECONNRESET );
			return -1;
		}

		// if there was EINTR, retry
		// if any other error, bail
		if ( iRes==-1 )
		{
			// only let SIGTERM (of all them) to interrupt, and only if explicitly allowed
			iErr = sphSockGetErrno ();
			if ( iErr==EINTR )
			{
				if ( !( g_bGotSigterm && bIntr ) )
					continue;
				sphLogDebug ( "sphSockRead: select got SIGTERM, exit -1" );
			}
			return -1;
		}

		// avoid partial buffer loss in case of signal during the 2nd (!) read
		bIntr = false;
	}

	// if there was a timeout, report it as an error
	if ( iLeftBytes!=0 )
	{
		sphSockSetErrno ( ETIMEDOUT );
		return -1;
	}

	return iLen;
}

int SockReadFast ( int iSock, void * buf, int iLen, int iReadTimeout )
{
	auto pBuf = ( char * ) buf;
	int iFullLen = iLen;
	// try to receive available chunk
	int iChunk = RecvNBChunk ( iSock, pBuf, iLen );
	if ( !iLen ) // all read in one-shot
	{
		assert (iChunk==iFullLen);
		return iFullLen;
	}

	auto iRes = sphSockRead ( iSock, pBuf, iLen, iReadTimeout, false );
	if ( iRes>=0 )
		iRes += iChunk;
	return iRes;
}
/////////////////////////////////////////////////////////////////////////////

ISphOutputBuffer::ISphOutputBuffer ()
{
	m_dBuf.Reserve ( NETOUTBUF );
}

// construct via adopting external buf
ISphOutputBuffer::ISphOutputBuffer ( CSphVector<BYTE>& dChunk )
{
	m_dBuf.SwapData (dChunk);
}


void ISphOutputBuffer::SendString ( const char * sStr )
{
	int iLen = sStr ? strlen(sStr) : 0;
	SendInt ( iLen );
	SendBytes ( sStr, iLen );
}

/////////////////////////////////////////////////////////////////////////////
// encodes Mysql Length-coded binary
BYTE * MysqlPackInt ( BYTE * pOutput, int iValue )
{
	if ( iValue<0 )
		return pOutput;

	if ( iValue<251 )
	{
		*pOutput++ = (BYTE)iValue;
		return pOutput;
	}

	if ( iValue<=0xFFFF )
	{
		*pOutput++ = (BYTE)'\xFC'; // 252
		*pOutput++ = (BYTE)iValue;
		*pOutput++ = (BYTE)( iValue>>8 );
		return pOutput;
	}

	if ( iValue<=0xFFFFFF )
	{
		*pOutput++ = (BYTE)'\xFD'; // 253
		*pOutput++ = (BYTE)iValue;
		*pOutput++ = (BYTE)( iValue>>8 );
		*pOutput++ = (BYTE)( iValue>>16 );
		return pOutput;
	}

	*pOutput++ = (BYTE)'\xFE'; // 254
	*pOutput++ = (BYTE)iValue;
	*pOutput++ = (BYTE)( iValue>>8 );
	*pOutput++ = (BYTE)( iValue>>16 );
	*pOutput++ = (BYTE)( iValue>>24 );
	*pOutput++ = 0;
	*pOutput++ = 0;
	*pOutput++ = 0;
	*pOutput++ = 0;
	return pOutput;
}

int MysqlUnpack ( InputBuffer_c & tReq, DWORD * pSize )
{
	assert ( pSize );

	int iRes = tReq.GetByte();
	--*pSize;
	if ( iRes < 251 )
		return iRes;

	if ( iRes==0xFC )
	{
		*pSize -=2;
		return tReq.GetByte() + ((int)tReq.GetByte()<<8);
	}

	if ( iRes==0xFD )
	{
		*pSize -= 3;
		return tReq.GetByte() + ((int)tReq.GetByte()<<8) + ((int)tReq.GetByte()<<16);
	}

	if ( iRes==0xFE )
		iRes = tReq.GetByte() + ((int)tReq.GetByte()<<8) + ((int)tReq.GetByte()<<16) + ((int)tReq.GetByte()<<24);

	tReq.GetByte();
	tReq.GetByte();
	tReq.GetByte();
	tReq.GetByte();
	*pSize -= 8;
	return iRes;
}

/////////////////////////////////////////////////////////////////////////////
void ISphOutputBuffer::SendBytes ( const void * pBuf, int iLen )
{
	m_dBuf.Append ( pBuf, iLen );
}

void ISphOutputBuffer::SendBytes ( const char * pBuf )
{
	if ( !pBuf )
		return;
	SendBytes ( pBuf, strlen ( pBuf ) );
}

void ISphOutputBuffer::SendBytes ( const CSphString& sStr )
{
	SendBytes ( sStr.cstr(), sStr.Length() );
}

void ISphOutputBuffer::SendBytes ( const VecTraits_T<BYTE> & dBuf )
{
	m_dBuf.Append ( dBuf );
}

void ISphOutputBuffer::SendBytes ( const StringBuilder_c &dBuf )
{
	SendBytes ( dBuf.begin(), dBuf.GetLength () );
}

void ISphOutputBuffer::SendArray ( const ISphOutputBuffer &tOut )
{
	int iLen = tOut.m_dBuf.GetLength();
	SendInt ( iLen );
	SendBytes ( tOut.m_dBuf.Begin(), iLen );
}

void ISphOutputBuffer::SendArray ( const VecTraits_T<BYTE> &dBuf, int iElems )
{
	if ( iElems==-1 )
	{
		SendInt ( dBuf.GetLength () );
		SendBytes ( dBuf );
		return;
	}
	assert ( dBuf.GetLength() == (int) dBuf.GetLengthBytes() );
	assert ( iElems<=dBuf.GetLength ());
	SendInt ( iElems );
	SendBytes ( dBuf.begin(), iElems );
}

void ISphOutputBuffer::SendArray ( const void * pBuf, int iLen )
{
	if ( !pBuf )
		iLen=0;
	assert ( iLen>=0 );
	SendInt ( iLen );
	SendBytes ( pBuf, iLen );
}

void ISphOutputBuffer::SendArray ( const StringBuilder_c &dBuf )
{
	SendArray ( dBuf.begin(), dBuf.GetLength () );
}

/////////////////////////////////////////////////////////////////////////////

void CachedOutputBuffer_c::Flush()
{
	CommitAllMeasuredLengths();
	ISphOutputBuffer::Flush();
}

intptr_t CachedOutputBuffer_c::StartMeasureLength ()
{
	auto iPos = ( intptr_t ) m_dBuf.GetLength ();
	m_dBlobs.Add ( iPos );
	SendInt(0);
	return iPos;
}

void CachedOutputBuffer_c::CommitMeasuredLength ( intptr_t iStoredPos )
{
	if ( m_dBlobs.IsEmpty() ) // possible if flush happens before APIheader destroyed.
		return;
	auto iPos = m_dBlobs.Pop();
	assert ( iStoredPos==-1 || iStoredPos==iPos );
	int iBlobLen = m_dBuf.GetLength () - iPos - sizeof ( int );
	WriteInt ( iPos, iBlobLen );
}

void CachedOutputBuffer_c::CommitAllMeasuredLengths()
{
	while ( !m_dBlobs.IsEmpty() )
	{
		auto uPos = m_dBlobs.Pop ();
		int iBlobLen = m_dBuf.GetLength () - uPos - sizeof ( int );
		WriteInt ( uPos, iBlobLen );
	}
}

/////////////////////////////////////////////////////////////////////////////

NetOutputBuffer_c::NetOutputBuffer_c ( int iSock )
	: m_iSock ( iSock )
{
	assert ( m_iSock>0 );
}

void NetOutputBuffer_c::Flush ()
{
	CommitAllMeasuredLengths ();

	if ( m_bError )
		return;

	int iLen = m_dBuf.GetLength();
	if ( !iLen )
		return;

	if ( g_bGotSigterm )
		sphLogDebug ( "SIGTERM in NetOutputBuffer::Flush" );

	StringBuilder_c sError;
	auto * pBuffer = (const char *)m_dBuf.Begin();

	CSphScopedProfile tProf ( m_pProfile, SPH_QSTATE_NET_WRITE );

	const int64_t tmMaxTimer = sphMicroTimer() + MS2SEC * g_iWriteTimeout; // in microseconds
	while ( !m_bError )
	{
		auto iRes = sphSockSend ( m_iSock, pBuffer, iLen );
		if ( iRes<0 )
		{
			int iErrno = sphSockGetErrno();
			if ( iErrno==EINTR ) // interrupted before any data was sent; just loop
				continue;
			if ( iErrno!=EAGAIN && iErrno!=EWOULDBLOCK )
			{
				sError.Sprintf ( "send() failed: %d: %s", iErrno, sphSockError ( iErrno ) );
				sphWarning ( "%s", sError.cstr () );
				m_bError = true;
				break;
			}
		} else
		{
			m_iSent += iRes;
			pBuffer += iRes;
			iLen -= iRes;
			if ( iLen==0 )
				break;
		}

		// wait until we can write
		int64_t tmMicroLeft = tmMaxTimer - sphMicroTimer();
		iRes = 0;
		if ( tmMicroLeft>0 )
			iRes = sphPoll ( m_iSock, tmMicroLeft, true );

		if ( !iRes ) // timeout
		{
			sError << "timed out while trying to flush network buffers";
			sphWarning ( "%s", sError.cstr () );
			m_bError = true;
			break;
		}

		if ( iRes<0 )
		{
			int iErrno = sphSockGetErrno ();
			if ( iErrno==EINTR )
				break;
			sError.Sprintf ( "sphPoll() failed: %d: %s", iErrno, sphSockError ( iErrno ) );
			sphWarning ( "%s", sError.cstr () );
			m_bError = true;
			break;
		}
		assert (iRes>0);
	}

	m_dBuf.Resize ( 0 );
}


/////////////////////////////////////////////////////////////////////////////

InputBuffer_c::InputBuffer_c ( const BYTE * pBuf, int iLen )
	: m_pBuf ( pBuf )
	, m_pCur ( pBuf )
	, m_bError ( !pBuf || iLen<0 )
	, m_iLen ( iLen )
{}


CSphString InputBuffer_c::GetString ()
{
	CSphString sRes;

	int iLen = GetInt ();
	if ( m_bError || iLen<0 || iLen>g_iMaxPacketSize || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return sRes;
	}

	if ( iLen )
		sRes.SetBinary ( (char*)m_pCur, iLen );

	m_pCur += iLen;
	return sRes;
}


CSphString InputBuffer_c::GetRawString ( int iLen )
{
	CSphString sRes;

	if ( m_bError || iLen<0 || iLen>g_iMaxPacketSize || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return sRes;
	}

	if ( iLen )
		sRes.SetBinary ( (char*)m_pCur, iLen );

	m_pCur += iLen;
	return sRes;
}


bool InputBuffer_c::GetString ( CSphVector<BYTE> & dBuffer )
{
	int iLen = GetInt ();
	if ( m_bError || iLen<0 || iLen>g_iMaxPacketSize || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return false;
	}

	if ( !iLen )
		return true;

	return GetBytes ( dBuffer.AddN ( iLen ), iLen );
}


bool InputBuffer_c::GetBytes ( void * pBuf, int iLen )
{
	assert ( pBuf );
	assert ( iLen>0 && iLen<=g_iMaxPacketSize );

	if ( m_bError || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return false;
	}

	memcpy ( pBuf, m_pCur, iLen );
	m_pCur += iLen;
	return true;
}

bool InputBuffer_c::GetBytesZerocopy ( const BYTE ** ppData, int iLen )
{
	assert ( ppData );
	assert ( iLen>0 && iLen<=g_iMaxPacketSize );

	if ( m_bError || ( m_pCur+iLen > m_pBuf+m_iLen ) )
	{
		SetError ( true );
		return false;
	}

	*ppData = m_pCur;
	m_pCur += iLen;
	return true;
}


template < typename T > bool InputBuffer_c::GetDwords ( CSphVector<T> & dBuffer, int & iGot, int iMax )
{
	iGot = GetInt ();
	if ( iGot<0 || iGot>iMax )
	{
		SetError ( true );
		return false;
	}

	dBuffer.Resize ( iGot );
	ARRAY_FOREACH ( i, dBuffer )
		dBuffer[i] = GetDword ();

	if ( m_bError )
		dBuffer.Reset ();

	return !m_bError;
}


template < typename T > bool InputBuffer_c::GetQwords ( CSphVector<T> & dBuffer, int & iGot, int iMax )
{
	iGot = GetInt ();
	if ( iGot<0 || iGot>iMax )
	{
		SetError ( true );
		return false;
	}

	dBuffer.Resize ( iGot );
	ARRAY_FOREACH ( i, dBuffer )
		dBuffer[i] = GetUint64 ();

	if ( m_bError )
		dBuffer.Reset ();

	return !m_bError;
}
/////////////////////////////////////////////////////////////////////////////

NetInputBuffer_c::NetInputBuffer_c ( int iSock )
	: STORE (NET_MINIBUFFER_SIZE)
	, InputBuffer_c ( m_pData, NET_MINIBUFFER_SIZE )
	, m_iSock ( iSock )
{
	Resize (0);
}


bool NetInputBuffer_c::ReadFrom ( int iLen, int iTimeout, bool bIntr, bool bAppend )
{
	int iTail = bAppend ? m_iLen : 0;

	m_bIntr = false;
	if ( iLen<=0 || iLen>g_iMaxPacketSize || m_iSock<0 )
		return false;

	Resize ( m_iLen );
	Reserve ( iTail + iLen );
	BYTE * pBuf = m_pData + iTail;
	m_pCur = m_pBuf = pBuf;
	int iGot = sphSockRead ( m_iSock, pBuf , iLen, iTimeout, bIntr );
	if ( g_bGotSigterm )
	{
		sphLogDebug ( "NetInputBuffer_c::ReadFrom: got SIGTERM, return false" );
		m_bError = true;
		m_bIntr = true;
		return false;
	}

	m_bError = ( iGot!=iLen );
	m_bIntr = m_bError && ( sphSockPeekErrno()==EINTR );
	m_iLen = m_bError ? 0 : iTail+iLen;
	return !m_bError;
}


void SendErrorReply ( CachedOutputBuffer_c & tOut, const char * sTemplate, ... )
{
	CSphString sError;
	va_list ap;
	va_start ( ap, sTemplate );
	sError.SetSprintfVa ( sTemplate, ap );
	va_end ( ap );

	APICommand_t dError ( tOut, SEARCHD_ERROR );
	tOut.SendString ( sError.cstr() );

	// --console logging
	if ( g_bOptNoDetach && g_eLogFormat!=LOG_FORMAT_SPHINXQL )
		sphInfo ( "query error: %s", sError.cstr() );
}

// fix MSVC 2005 fuckup
#if USE_WINDOWS
#pragma conform(forScope,on)
#endif

void DistributedIndex_t::GetAllHosts ( VectorAgentConn_t &dTarget ) const
{
	for ( const auto * pMultiAgent : m_dAgents )
		for ( const auto & dHost : *pMultiAgent )
		{
			auto * pAgent = new AgentConn_t;
			pAgent->m_tDesc.CloneFrom ( dHost );
			pAgent->m_iMyQueryTimeout = m_iAgentQueryTimeout;
			pAgent->m_iMyConnectTimeout = m_iAgentConnectTimeout;
			dTarget.Add ( pAgent );
		}
}

void DistributedIndex_t::ForEveryHost ( ProcessFunctor pFunc )
{
	for ( auto * pAgent : m_dAgents )
		for ( auto &dHost : *pAgent )
			pFunc ( dHost );
}

DistributedIndex_t::~DistributedIndex_t ()
{
	sphLogDebugv ( "DistributedIndex_t %p removed", this );
	for ( auto * pAgent : m_dAgents )
		SafeRelease ( pAgent );

	// cleanup global
	MultiAgentDesc_c::CleanupOrphaned ();
};

/////////////////////////////////////////////////////////////////////////////
// SEARCH HANDLER
/////////////////////////////////////////////////////////////////////////////

struct SearchRequestBuilder_t : public IRequestBuilder_t
{
	SearchRequestBuilder_t ( const CSphVector<CSphQuery> & dQueries, int iStart, int iEnd, int iDivideLimits )
		: m_dQueries ( dQueries ), m_iStart ( iStart ), m_iEnd ( iEnd ), m_iDivideLimits ( iDivideLimits )
	{}

	void		BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const final;

protected:
	void		SendQuery ( const char * sIndexes, ISphOutputBuffer & tOut, const CSphQuery & q, int iWeight, int iAgentQueryTimeout ) const;

protected:
	const CSphVector<CSphQuery> &		m_dQueries;
	const int							m_iStart;
	const int							m_iEnd;
	const int							m_iDivideLimits;
};


class SearchReplyParser_c : public IReplyParser_t, public ISphNoncopyable
{
public:
	SearchReplyParser_c ( int iStart, int iEnd )
		: m_iStart ( iStart )
		, m_iEnd ( iEnd )
	{}

	bool		ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & tAgent ) const final;

private:
	int					m_iStart;
	int					m_iEnd;

	void				ParseSchema ( CSphQueryResult & tRes, MemInputBuffer_c & tReq ) const;
	void				ParseMatch ( CSphMatch & tMatch, MemInputBuffer_c & tReq, const CSphSchema & tSchema, bool bAgent64 ) const;
};

/////////////////////////////////////////////////////////////////////////////

/// qflag means Query Flag
/// names are internal to searchd and may be changed for clarity
/// values are communicated over network between searchds and APIs and MUST NOT CHANGE
enum
{
	QFLAG_REVERSE_SCAN			= 1UL << 0,
	QFLAG_SORT_KBUFFER			= 1UL << 1,
	QFLAG_MAX_PREDICTED_TIME	= 1UL << 2,
	QFLAG_SIMPLIFY				= 1UL << 3,
	QFLAG_PLAIN_IDF				= 1UL << 4,
	QFLAG_GLOBAL_IDF			= 1UL << 5,
	QFLAG_NORMALIZED_TF			= 1UL << 6,
	QFLAG_LOCAL_DF				= 1UL << 7,
	QFLAG_LOW_PRIORITY			= 1UL << 8,
	QFLAG_FACET					= 1UL << 9,
	QFLAG_FACET_HEAD			= 1UL << 10,
	QFLAG_JSON_QUERY			= 1UL << 11
};

void SearchRequestBuilder_t::SendQuery ( const char * sIndexes, ISphOutputBuffer & tOut, const CSphQuery & q, int iWeight, int iAgentQueryTimeout ) const
{
	bool bAgentWeight = ( iWeight!=-1 );
	// starting with command version 1.27, flags go first
	// reason being, i might add flags that affect *any* of the subsequent data (eg. qflag_pack_ints)
	DWORD uFlags = 0;
	uFlags |= QFLAG_REVERSE_SCAN * q.m_bReverseScan;
	uFlags |= QFLAG_SORT_KBUFFER * q.m_bSortKbuffer;
	uFlags |= QFLAG_MAX_PREDICTED_TIME * ( q.m_iMaxPredictedMsec > 0 );
	uFlags |= QFLAG_SIMPLIFY * q.m_bSimplify;
	uFlags |= QFLAG_PLAIN_IDF * q.m_bPlainIDF;
	uFlags |= QFLAG_GLOBAL_IDF * q.m_bGlobalIDF;
	uFlags |= QFLAG_NORMALIZED_TF * q.m_bNormalizedTFIDF;
	uFlags |= QFLAG_LOCAL_DF * q.m_bLocalDF;
	uFlags |= QFLAG_LOW_PRIORITY * q.m_bLowPriority;
	uFlags |= QFLAG_FACET * q.m_bFacet;
	uFlags |= QFLAG_FACET_HEAD * q.m_bFacetHead;

	if ( q.m_eQueryType==QUERY_JSON )
		uFlags |= QFLAG_JSON_QUERY;

	tOut.SendDword ( uFlags );

	// The Search Legacy
	tOut.SendInt ( 0 ); // offset is 0
	if ( !q.m_bHasOuter )
	{
		if ( m_iDivideLimits==1 )
			tOut.SendInt ( q.m_iMaxMatches ); // OPTIMIZE? normally, agent limit is max_matches, even if master limit is less
		else // FIXME!!! that is broken with offset + limit
			tOut.SendInt ( 1 + ( ( q.m_iOffset + q.m_iLimit )/m_iDivideLimits) );
	} else
	{
		// with outer order by, inner limit must match between agent and master
		tOut.SendInt ( q.m_iLimit );
	}
	tOut.SendInt ( (DWORD)q.m_eMode ); // match mode
	tOut.SendInt ( (DWORD)q.m_eRanker ); // ranking mode
	if ( q.m_eRanker==SPH_RANK_EXPR || q.m_eRanker==SPH_RANK_EXPORT )
		tOut.SendString ( q.m_sRankerExpr.cstr() );
	tOut.SendInt ( q.m_eSort ); // sort mode
	tOut.SendString ( q.m_sSortBy.cstr() ); // sort attr

	if ( q.m_eQueryType==QUERY_JSON )
		tOut.SendString ( q.m_sQuery.cstr() );
	else
	{
		if ( q.m_sRawQuery.IsEmpty() )
			tOut.SendString ( q.m_sQuery.cstr() );
		else
			tOut.SendString ( q.m_sRawQuery.cstr() ); // query
	}

	tOut.SendInt ( q.m_dWeights.GetLength() );
	ARRAY_FOREACH ( j, q.m_dWeights )
		tOut.SendInt ( q.m_dWeights[j] ); // weights
	tOut.SendString ( sIndexes ); // indexes
	tOut.SendInt ( 1 ); // id range bits
	tOut.SendDocid ( 0 ); // default full id range (any client range must be in filters at this stage)
	tOut.SendDocid ( DOCID_MAX );
	tOut.SendInt ( q.m_dFilters.GetLength() );
	ARRAY_FOREACH ( j, q.m_dFilters )
	{
		const CSphFilterSettings & tFilter = q.m_dFilters[j];
		tOut.SendString ( tFilter.m_sAttrName.cstr() );
		tOut.SendInt ( tFilter.m_eType );
		switch ( tFilter.m_eType )
		{
			case SPH_FILTER_VALUES:
				tOut.SendInt ( tFilter.GetNumValues () );
				for ( int k = 0; k < tFilter.GetNumValues (); k++ )
					tOut.SendUint64 ( tFilter.GetValue ( k ) );
				break;

			case SPH_FILTER_RANGE:
				tOut.SendUint64 ( tFilter.m_iMinValue );
				tOut.SendUint64 ( tFilter.m_iMaxValue );
				break;

			case SPH_FILTER_FLOATRANGE:
				tOut.SendFloat ( tFilter.m_fMinValue );
				tOut.SendFloat ( tFilter.m_fMaxValue );
				break;

			case SPH_FILTER_USERVAR:
			case SPH_FILTER_STRING:
				tOut.SendString ( tFilter.m_dStrings.GetLength()==1 ? tFilter.m_dStrings[0].cstr() : nullptr );
				break;

			case SPH_FILTER_NULL:
				tOut.SendByte ( tFilter.m_bIsNull );
				break;

			case SPH_FILTER_STRING_LIST:
				tOut.SendInt ( tFilter.m_dStrings.GetLength() );
				ARRAY_FOREACH ( iString, tFilter.m_dStrings )
					tOut.SendString ( tFilter.m_dStrings[iString].cstr() );
				break;
			case SPH_FILTER_EXPRESSION: // need only name and type
				break;
		}
		tOut.SendInt ( tFilter.m_bExclude );
		tOut.SendInt ( tFilter.m_bHasEqualMin );
		tOut.SendInt ( tFilter.m_bHasEqualMax );
		tOut.SendInt ( tFilter.m_bOpenLeft );
		tOut.SendInt ( tFilter.m_bOpenRight );
		tOut.SendInt ( tFilter.m_eMvaFunc );
	}
	tOut.SendInt ( q.m_eGroupFunc );
	tOut.SendString ( q.m_sGroupBy.cstr() );
	if ( m_iDivideLimits==1 )
		tOut.SendInt ( q.m_iMaxMatches );
	else
		tOut.SendInt ( 1+(q.m_iMaxMatches/m_iDivideLimits) ); // Reduce the max_matches also.
	tOut.SendString ( q.m_sGroupSortBy.cstr() );
	tOut.SendInt ( q.m_iCutoff );
	tOut.SendInt ( q.m_iRetryCount<0 ? 0 : q.m_iRetryCount ); // runaround for old clients.
	tOut.SendInt ( q.m_iRetryDelay<0 ? 0 : q.m_iRetryDelay );
	tOut.SendString ( q.m_sGroupDistinct.cstr() );
	tOut.SendInt ( q.m_bGeoAnchor );
	if ( q.m_bGeoAnchor )
	{
		tOut.SendString ( q.m_sGeoLatAttr.cstr() );
		tOut.SendString ( q.m_sGeoLongAttr.cstr() );
		tOut.SendFloat ( q.m_fGeoLatitude );
		tOut.SendFloat ( q.m_fGeoLongitude );
	}
	if ( bAgentWeight )
	{
		tOut.SendInt ( 1 );
		tOut.SendString ( "*" );
		tOut.SendInt ( iWeight );
	} else
	{
		tOut.SendInt ( q.m_dIndexWeights.GetLength() );
		ARRAY_FOREACH ( i, q.m_dIndexWeights )
		{
			tOut.SendString ( q.m_dIndexWeights[i].m_sName.cstr() );
			tOut.SendInt ( q.m_dIndexWeights[i].m_iValue );
		}
	}
	DWORD iQueryTimeout = ( q.m_uMaxQueryMsec ? q.m_uMaxQueryMsec : iAgentQueryTimeout );
	tOut.SendDword ( iQueryTimeout );
	tOut.SendInt ( q.m_dFieldWeights.GetLength() );
	ARRAY_FOREACH ( i, q.m_dFieldWeights )
	{
		tOut.SendString ( q.m_dFieldWeights[i].m_sName.cstr() );
		tOut.SendInt ( q.m_dFieldWeights[i].m_iValue );
	}
	tOut.SendString ( q.m_sComment.cstr() );
	tOut.SendInt ( q.m_dOverrides.GetLength() );
	ARRAY_FOREACH ( i, q.m_dOverrides )
	{
		const CSphAttrOverride & tEntry = q.m_dOverrides[i];
		tOut.SendString ( tEntry.m_sAttr.cstr() );
		tOut.SendDword ( tEntry.m_eAttrType );
		tOut.SendInt ( tEntry.m_dValues.GetLength() );
		ARRAY_FOREACH ( j, tEntry.m_dValues )
		{
			tOut.SendUint64 ( tEntry.m_dValues[j].m_uDocID );
			switch ( tEntry.m_eAttrType )
			{
				case SPH_ATTR_FLOAT:	tOut.SendFloat ( tEntry.m_dValues[j].m_fValue ); break;
				case SPH_ATTR_BIGINT:	tOut.SendUint64 ( tEntry.m_dValues[j].m_uValue ); break;
				default:				tOut.SendDword ( (DWORD)tEntry.m_dValues[j].m_uValue ); break;
			}
		}
	}
	tOut.SendString ( q.m_sSelect.cstr() );
	if ( q.m_iMaxPredictedMsec>0 )
		tOut.SendInt ( q.m_iMaxPredictedMsec );

	// emulate empty sud-select for agent (client ver 1.29) as master sends fixed outer offset+limits
	tOut.SendString ( NULL );
	tOut.SendInt ( 0 );
	tOut.SendInt ( 0 );
	tOut.SendInt ( q.m_bHasOuter );

	// master-agent extensions
	tOut.SendDword ( q.m_eCollation ); // v.1
	tOut.SendString ( q.m_sOuterOrderBy.cstr() ); // v.2
	if ( q.m_bHasOuter )
		tOut.SendInt ( q.m_iOuterOffset + q.m_iOuterLimit );
	tOut.SendInt ( q.m_iGroupbyLimit );
	tOut.SendString ( q.m_sUDRanker.cstr() );
	tOut.SendString ( q.m_sUDRankerOpts.cstr() );
	tOut.SendString ( q.m_sQueryTokenFilterLib.cstr() );
	tOut.SendString ( q.m_sQueryTokenFilterName.cstr() );
	tOut.SendString ( q.m_sQueryTokenFilterOpts.cstr() );
	tOut.SendInt ( q.m_dFilterTree.GetLength() );
	ARRAY_FOREACH ( i, q.m_dFilterTree )
	{
		tOut.SendInt ( q.m_dFilterTree[i].m_iLeft );
		tOut.SendInt ( q.m_dFilterTree[i].m_iRight );
		tOut.SendInt ( q.m_dFilterTree[i].m_iFilterItem );
		tOut.SendInt ( q.m_dFilterTree[i].m_bOr );
	}
	tOut.SendInt( q.m_dItems.GetLength() );
	ARRAY_FOREACH ( i, q.m_dItems )
	{
		const CSphQueryItem & tItem = q.m_dItems[i];
		tOut.SendString ( tItem.m_sAlias.cstr() );
		tOut.SendString ( tItem.m_sExpr.cstr() );
		tOut.SendDword ( tItem.m_eAggrFunc );
	}
	tOut.SendInt( q.m_dRefItems.GetLength() );
	ARRAY_FOREACH ( i, q.m_dRefItems )
	{
		const CSphQueryItem & tItem = q.m_dRefItems[i];
		tOut.SendString ( tItem.m_sAlias.cstr() );
		tOut.SendString ( tItem.m_sExpr.cstr() );
		tOut.SendDword ( tItem.m_eAggrFunc );
	}
	tOut.SendDword ( q.m_eExpandKeywords );
}


void SearchRequestBuilder_t::BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const
{
	APICommand_t tWr { tOut, SEARCHD_COMMAND_SEARCH, VER_COMMAND_SEARCH }; // API header

	tOut.SendInt ( VER_MASTER );
	tOut.SendInt ( m_iEnd-m_iStart+1 );
	for ( int i=m_iStart; i<=m_iEnd; ++i )
		SendQuery ( tAgent.m_tDesc.m_sIndexes.cstr (), tOut, m_dQueries[i], tAgent.m_iWeight, tAgent.m_iMyQueryTimeout );
}


struct cSearchResult : public iQueryResult
{
	CSphVector<CSphQueryResult>	m_dResults;

	void Reset () final
	{
		m_dResults.Reset();
	}

	bool HasWarnings () const final
	{
		return m_dResults.FindFirst ( [] ( const CSphQueryResult &dRes ) { return !dRes.m_sWarning.IsEmpty (); } );
	}
};


/////////////////////////////////////////////////////////////////////////////

void SearchReplyParser_c::ParseMatch ( CSphMatch & tMatch, MemInputBuffer_c & tReq, const CSphSchema & tSchema, bool bAgent64 ) const
{
	tMatch.Reset ( tSchema.GetRowSize() );
	tMatch.m_uDocID = bAgent64 ? (SphDocID_t)tReq.GetUint64() : tReq.GetDword();
	tMatch.m_iWeight = tReq.GetInt ();
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr(i);

		assert ( sphPlainAttrToPtrAttr(tAttr.m_eAttrType)==tAttr.m_eAttrType );

		switch ( tAttr.m_eAttrType )
		{
		case SPH_ATTR_UINT32SET_PTR:
		case SPH_ATTR_INT64SET_PTR:
			{
				int iValues = tReq.GetDword ();
				BYTE * pData = nullptr;
				BYTE * pPacked = sphPackPtrAttr ( iValues*sizeof(DWORD), &pData );
				tMatch.SetAttr ( tAttr.m_tLocator, (SphAttr_t)pPacked );
				DWORD * pMVA = (DWORD *)pData;
				if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET_PTR )
				{
					while ( iValues-- )
						sphUnalignedWrite ( pMVA++, tReq.GetDword() );
				} else
				{
					assert ( ( iValues%2 )==0 );
					for ( ; iValues; iValues -= 2 )
					{
						uint64_t uMva = tReq.GetUint64();
						sphUnalignedWrite ( pMVA, uMva );
						pMVA += 2;
					}
				}
			}
			break;

		case SPH_ATTR_STRINGPTR:
		case SPH_ATTR_JSON_PTR:
		case SPH_ATTR_FACTORS:
		case SPH_ATTR_FACTORS_JSON:
			{
				int iLen = tReq.GetDword();
				BYTE * pData = nullptr;
				tMatch.SetAttr ( tAttr.m_tLocator, (SphAttr_t)sphPackPtrAttr ( iLen, &pData ) );
				if ( iLen )
					tReq.GetBytes ( pData, iLen );
			}
			break;

		case SPH_ATTR_JSON_FIELD_PTR:
			{
				// FIXME: no reason for json_field to be any different from other *_PTR attributes
				ESphJsonType eJson = (ESphJsonType)tReq.GetByte();
				if ( eJson==JSON_EOF )
					tMatch.SetAttr ( tAttr.m_tLocator, 0 );
				else
				{
					int iLen = tReq.GetDword();
					BYTE * pData = nullptr;
					tMatch.SetAttr ( tAttr.m_tLocator, (SphAttr_t)sphPackPtrAttr ( iLen+1, &pData ) );
					*pData++ = (BYTE)eJson;
					tReq.GetBytes ( pData, iLen );
				}
			}
			break;

		case SPH_ATTR_FLOAT:
			tMatch.SetAttr ( tAttr.m_tLocator, sphF2DW ( tReq.GetFloat() ) );
			break;

		case SPH_ATTR_BIGINT:
			tMatch.SetAttr ( tAttr.m_tLocator, tReq.GetUint64() );
			break;

		default:
			tMatch.SetAttr ( tAttr.m_tLocator, tReq.GetDword() );
			break;
		}
	}
}


void SearchReplyParser_c::ParseSchema ( CSphQueryResult & tRes, MemInputBuffer_c & tReq ) const
{
	CSphSchema & tSchema = tRes.m_tSchema;
	tSchema.Reset ();

	int nFields = tReq.GetInt(); // FIXME! add a sanity check
	for ( int j = 0; j < nFields; j++ )
		tSchema.AddField ( tReq.GetString().cstr() );

	int iNumAttrs = tReq.GetInt(); // FIXME! add a sanity check
	for ( int j=0; j<iNumAttrs; j++ )
	{
		CSphColumnInfo tCol;
		tCol.m_sName = tReq.GetString ();
		tCol.m_eAttrType = (ESphAttr) tReq.GetDword (); // FIXME! add a sanity check

		// we always work with plain attrs (not *_PTR) when working with agents
		tCol.m_eAttrType = sphPlainAttrToPtrAttr ( tCol.m_eAttrType );
		tSchema.AddAttr ( tCol, true ); // all attributes received from agents are dynamic
	}
}


bool SearchReplyParser_c::ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & tAgent ) const
{
	int iResults = m_iEnd-m_iStart+1;
	assert ( iResults>0 );

	auto pResult = ( cSearchResult * ) tAgent.m_pResult.Ptr ();
	if ( !pResult )
	{
		pResult = new cSearchResult;
		tAgent.m_pResult = pResult;
	}

	auto &dResults = pResult->m_dResults;

	dResults.Resize ( iResults );
	for ( auto & tRes : dResults )
		tRes.m_iSuccesses = 0;

	for ( auto & tRes : dResults )
	{
		tRes.m_sError = "";
		tRes.m_sWarning = "";

		// get status and message
		auto eStatus = ( SearchdStatus_e ) tReq.GetDword ();
		switch ( eStatus )
		{
			case SEARCHD_ERROR:		tRes.m_sError = tReq.GetString (); continue;
			case SEARCHD_RETRY:		tRes.m_sError = tReq.GetString (); break;
			case SEARCHD_WARNING:	tRes.m_sWarning = tReq.GetString (); break;
			default:				tAgent.m_sFailure.SetSprintf ( "internal error: unknown status %d, message %s", eStatus, tReq.GetString ().cstr() );
			case SEARCHD_OK: break;
		}

		ParseSchema ( tRes, tReq );

		// get matches
		int iMatches = tReq.GetInt ();
		if ( iMatches<0 )
		{
			tAgent.m_sFailure.SetSprintf ( "invalid match count received (count=%d)", iMatches );
			return false;
		}

		bool bAgent64 = !!tReq.GetInt();
		if ( !bAgent64 )
		{
			tAgent.m_sFailure.SetSprintf ( "agent has 32-bit docids; no longer supported" );
			return false;
		}

		assert ( !tRes.m_dMatches.GetLength() );
		if ( iMatches )
		{
			tRes.m_dMatches.Resize ( iMatches );
			for ( auto & tMatch : tRes.m_dMatches )
				ParseMatch ( tMatch, tReq, tRes.m_tSchema, bAgent64 );
		}

		// read totals (retrieved count, total count, query time, word count)
		int iRetrieved = tReq.GetInt ();
		tRes.m_iTotalMatches = (unsigned int)tReq.GetInt ();
		tRes.m_iQueryTime = tReq.GetInt ();

		// agents always send IO/CPU stats to master
		BYTE uStatMask = tReq.GetByte();
		if ( uStatMask & 1 )
		{
			tRes.m_tIOStats.m_iReadTime = tReq.GetUint64();
			tRes.m_tIOStats.m_iReadOps = tReq.GetDword();
			tRes.m_tIOStats.m_iReadBytes = tReq.GetUint64();
			tRes.m_tIOStats.m_iWriteTime = tReq.GetUint64();
			tRes.m_tIOStats.m_iWriteOps = tReq.GetDword();
			tRes.m_tIOStats.m_iWriteBytes = tReq.GetUint64();
		}

		if ( uStatMask & 2 )
			tRes.m_iCpuTime = tReq.GetUint64();

		if ( uStatMask & 4 )
			tRes.m_iPredictedTime = tReq.GetUint64();

		tRes.m_iAgentFetchedDocs = tReq.GetDword();
		tRes.m_iAgentFetchedHits = tReq.GetDword();
		tRes.m_iAgentFetchedSkips = tReq.GetDword();

		const int iWordsCount = tReq.GetInt (); // FIXME! sanity check?
		if ( iRetrieved!=iMatches )
		{
			tAgent.m_sFailure.SetSprintf ( "expected %d retrieved documents, got %d", iMatches, iRetrieved );
			return false;
		}

		// read per-word stats
		for ( int i=0; i<iWordsCount; i++ )
		{
			const CSphString sWord = tReq.GetString ();
			const int64_t iDocs = (unsigned int)tReq.GetInt ();
			const int64_t iHits = (unsigned int)tReq.GetInt ();
			tReq.GetByte(); // statistics have no expanded terms for now

			tRes.AddStat ( sWord, iDocs, iHits );
		}

		// mark this result as ok
		tRes.m_iSuccesses = 1;
	}

	// all seems OK (and buffer length checks are performed by caller)
	return true;
}

/////////////////////////////////////////////////////////////////////////////

// returns true if incoming schema (src) is equal to existing (dst); false otherwise
bool MinimizeSchema ( CSphSchema & tDst, const ISphSchema & tSrc )
{
	// if dst is empty, result is also empty
	if ( tDst.GetAttrsCount()==0 )
		return tSrc.GetAttrsCount()==0;

	// check for equality, and remove all dst attributes that are not present in src
	CSphVector<CSphColumnInfo> dDst;
	for ( int i=0; i<tDst.GetAttrsCount(); i++ )
		dDst.Add ( tDst.GetAttr(i) );

	bool bEqual = ( tDst.GetAttrsCount()==tSrc.GetAttrsCount() );
	ARRAY_FOREACH ( i, dDst )
	{
		int iSrcIdx = tSrc.GetAttrIndex ( dDst[i].m_sName.cstr() );

		// check for index mismatch
		if ( iSrcIdx!=i )
			bEqual = false;

		// check for type/size mismatch (and fixup if needed)
		if ( iSrcIdx>=0 )
		{
			const CSphColumnInfo & tSrcAttr = tSrc.GetAttr ( iSrcIdx );

			// should seamlessly convert ( bool > float ) | ( bool > int > bigint )
			ESphAttr eDst = dDst[i].m_eAttrType;
			ESphAttr eSrc = tSrcAttr.m_eAttrType;
			bool bSame = ( eDst==eSrc )
				|| ( ( eDst==SPH_ATTR_FLOAT && eSrc==SPH_ATTR_BOOL ) || ( eDst==SPH_ATTR_BOOL && eSrc==SPH_ATTR_FLOAT ) )
				|| ( ( eDst==SPH_ATTR_BOOL || eDst==SPH_ATTR_INTEGER || eDst==SPH_ATTR_BIGINT )
					&& ( eSrc==SPH_ATTR_BOOL || eSrc==SPH_ATTR_INTEGER || eSrc==SPH_ATTR_BIGINT ) );

			int iDstBitCount = dDst[i].m_tLocator.m_iBitCount;
			int iSrcBitCount = tSrcAttr.m_tLocator.m_iBitCount;

			if ( !bSame )
			{
				// different types? remove the attr
				iSrcIdx = -1;
				bEqual = false;

			} else if ( iDstBitCount!=iSrcBitCount )
			{
				// different bit sizes? choose the max one
				dDst[i].m_tLocator.m_iBitCount = Max ( iDstBitCount, iSrcBitCount );
				bEqual = false;
				if ( iDstBitCount<iSrcBitCount )
					dDst[i].m_eAttrType = tSrcAttr.m_eAttrType;
			}

			if ( tSrcAttr.m_tLocator.m_iBitOffset!=dDst[i].m_tLocator.m_iBitOffset )
			{
				// different offsets? have to force target dynamic then, since we can't use one locator for all matches
				bEqual = false;
			}

			if ( tSrcAttr.m_tLocator.m_bDynamic!=dDst[i].m_tLocator.m_bDynamic )
			{
				// different location? have to force target dynamic then
				bEqual = false;
			}
		}

		// check for presence
		if ( iSrcIdx<0 )
		{
			dDst.Remove ( i );
			i--;
		}
	}

	if ( !bEqual )
	{
		CSphVector<CSphColumnInfo> dFields { tDst.GetFieldsCount() };
		for ( int i = 0; i < tDst.GetFieldsCount(); i++ )
			dFields[i] = tDst.GetField(i);

		tDst.Reset();

		for ( auto& dAttr : dDst )
			tDst.AddAttr ( dAttr, true );

		for ( auto& dField: dFields )
			tDst.AddField ( dField );

	} else
		tDst.SwapAttrs ( dDst );

	return bEqual;
}

static void CheckQuery ( const CSphQuery & tQuery, CSphString & sError )
{
	#define LOC_ERROR(_msg) { sError.SetSprintf ( _msg ); return; }
	#define LOC_ERROR1(_msg,_arg1) { sError.SetSprintf ( _msg, _arg1 ); return; }
	#define LOC_ERROR2(_msg,_arg1,_arg2) { sError.SetSprintf ( _msg, _arg1, _arg2 ); return; }

	sError = NULL;

	if ( (int)tQuery.m_eMode<0 || tQuery.m_eMode>SPH_MATCH_TOTAL )
		LOC_ERROR1 ( "invalid match mode %d", tQuery.m_eMode );

	if ( (int)tQuery.m_eRanker<0 || tQuery.m_eRanker>SPH_RANK_TOTAL )
		LOC_ERROR1 ( "invalid ranking mode %d", tQuery.m_eRanker );

	if ( tQuery.m_iMaxMatches<1 )
		LOC_ERROR ( "max_matches can not be less than one" );

	if ( tQuery.m_iOffset<0 || tQuery.m_iOffset>=tQuery.m_iMaxMatches )
		LOC_ERROR2 ( "offset out of bounds (offset=%d, max_matches=%d)",
			tQuery.m_iOffset, tQuery.m_iMaxMatches );

	if ( tQuery.m_iLimit<0 )
		LOC_ERROR1 ( "limit out of bounds (limit=%d)", tQuery.m_iLimit );

	if ( tQuery.m_iCutoff<0 )
		LOC_ERROR1 ( "cutoff out of bounds (cutoff=%d)", tQuery.m_iCutoff );

	if ( ( tQuery.m_iRetryCount!=-1 )
		&& ( tQuery.m_iRetryCount>MAX_RETRY_COUNT ) )
		LOC_ERROR1 ( "retry count out of bounds (count=%d)", tQuery.m_iRetryCount );

	if ( ( tQuery.m_iRetryDelay!=-1 )
		&& ( tQuery.m_iRetryDelay>MAX_RETRY_DELAY ) )
			LOC_ERROR1 ( "retry delay out of bounds (delay=%d)", tQuery.m_iRetryDelay );

	if ( tQuery.m_iOffset>0 && tQuery.m_bHasOuter )
		LOC_ERROR1 ( "inner offset must be 0 when using outer order by (offset=%d)", tQuery.m_iOffset );

	#undef LOC_ERROR
	#undef LOC_ERROR1
	#undef LOC_ERROR2
}


void PrepareQueryEmulation ( CSphQuery * pQuery )
{
	if ( pQuery->m_eMode==SPH_MATCH_BOOLEAN )
		pQuery->m_eRanker = SPH_RANK_NONE;

	if ( pQuery->m_eMode==SPH_MATCH_FULLSCAN )
		pQuery->m_sQuery = "";

	if ( pQuery->m_eMode!=SPH_MATCH_ALL && pQuery->m_eMode!=SPH_MATCH_ANY && pQuery->m_eMode!=SPH_MATCH_PHRASE )
		return;

	const char * szQuery = pQuery->m_sRawQuery.cstr ();
	int iQueryLen = ( szQuery ? strlen(szQuery) : 0 );

	pQuery->m_sQuery.Reserve ( iQueryLen*2+8 );
	char * szRes = (char*) pQuery->m_sQuery.cstr ();
	char c;

	if ( pQuery->m_eMode==SPH_MATCH_ANY || pQuery->m_eMode==SPH_MATCH_PHRASE )
		*szRes++ = '\"';

	if ( iQueryLen )
	{
		while ( ( c = *szQuery++ )!=0 )
		{
			// must be in sync with EscapeString (php api)
			const char sMagics[] = "<\\()|-!@~\"&/^$=";
			for ( const char * s = sMagics; *s; s++ )
				if ( c==*s )
				{
					*szRes++ = '\\';
					break;
				}
			*szRes++ = c;
		}
	}

	switch ( pQuery->m_eMode )
	{
		case SPH_MATCH_ALL:		pQuery->m_eRanker = SPH_RANK_PROXIMITY; *szRes = '\0'; break;
		case SPH_MATCH_ANY:		pQuery->m_eRanker = SPH_RANK_MATCHANY; strncpy ( szRes, "\"/1", 8 ); break;
		case SPH_MATCH_PHRASE:	pQuery->m_eRanker = SPH_RANK_PROXIMITY; *szRes++ = '\"'; *szRes = '\0'; break;
		default:				return;
	}
}


static void FixupQuerySettings ( CSphQuery & tQuery )
{
	// sort filters
	for ( auto & i : tQuery.m_dFilters )
		i.m_dValues.Sort();

	// sort overrides
	for ( auto & i : tQuery.m_dOverrides )
		i.m_dValues.Sort();

	if ( !tQuery.m_bHasOuter )
	{
		tQuery.m_sOuterOrderBy = "";
		tQuery.m_iOuterOffset = 0;
		tQuery.m_iOuterLimit = 0;
	}
}


static bool ParseSearchFilter ( CSphFilterSettings & tFilter, InputBuffer_c & tReq, CachedOutputBuffer_c & tOut, int iMasterVer )
{
	tFilter.m_sAttrName = tReq.GetString ();
	sphColumnToLowercase ( const_cast<char *>( tFilter.m_sAttrName.cstr() ) );

	tFilter.m_eType = (ESphFilter) tReq.GetDword ();
	switch ( tFilter.m_eType )
	{
	case SPH_FILTER_RANGE:
		tFilter.m_iMinValue = tReq.GetUint64();
		tFilter.m_iMaxValue = tReq.GetUint64();
		break;

	case SPH_FILTER_FLOATRANGE:
		tFilter.m_fMinValue = tReq.GetFloat ();
		tFilter.m_fMaxValue = tReq.GetFloat ();
		break;

	case SPH_FILTER_VALUES:
		{
			int iGot = 0;
			bool bRes = tReq.GetQwords ( tFilter.m_dValues, iGot, g_iMaxFilterValues );
			if ( !bRes )
			{
				SendErrorReply ( tOut, "invalid attribute '%s' set length %d (should be in 0..%d range)", tFilter.m_sAttrName.cstr(), iGot, g_iMaxFilterValues );
				return false;
			}
		}
		break;

	case SPH_FILTER_STRING:
		tFilter.m_dStrings.Add ( tReq.GetString() );
		break;

	case SPH_FILTER_NULL:
		tFilter.m_bIsNull = tReq.GetByte()!=0;
		break;

	case SPH_FILTER_USERVAR:
		tFilter.m_dStrings.Add ( tReq.GetString() );
		break;

	case SPH_FILTER_STRING_LIST:
		{
			int iCount = tReq.GetDword();
			if ( iCount<0 || iCount>g_iMaxFilterValues )
			{
				SendErrorReply ( tOut, "invalid attribute '%s' set length %d (should be in 0..%d range)", tFilter.m_sAttrName.cstr(), iCount, g_iMaxFilterValues );
				return false;
			}
			tFilter.m_dStrings.Resize ( iCount );
			ARRAY_FOREACH ( iString, tFilter.m_dStrings )
				tFilter.m_dStrings[iString] = tReq.GetString();
		}
		break;
	case SPH_FILTER_EXPRESSION: // need only name and type
		break;

	default:
		SendErrorReply ( tOut, "unknown filter type (type-id=%d)", tFilter.m_eType );
		return false;
	}

	tFilter.m_bExclude = !!tReq.GetDword ();

	if ( iMasterVer>=15 )
	{
		tFilter.m_bHasEqualMin = !!tReq.GetDword();
		tFilter.m_bHasEqualMax = !!tReq.GetDword();
	} else if ( iMasterVer>=5 )
		tFilter.m_bHasEqualMin = tFilter.m_bHasEqualMax = !!tReq.GetDword();

	if ( iMasterVer>=15 )
	{
		tFilter.m_bOpenLeft = !!tReq.GetDword();
		tFilter.m_bOpenRight = !!tReq.GetDword();
	}

	tFilter.m_eMvaFunc = SPH_MVAFUNC_ANY;
	if ( iMasterVer>=13 )
		tFilter.m_eMvaFunc = (ESphMvaFunc)tReq.GetDword();

	return true;
}


bool ParseSearchQuery ( InputBuffer_c & tReq, CachedOutputBuffer_c & tOut, CSphQuery & tQuery, WORD uVer, WORD uMasterVer )
{
	// daemon-level defaults
	tQuery.m_iRetryCount = -1;
	tQuery.m_iRetryDelay = -1;
	tQuery.m_iAgentQueryTimeout = g_iAgentQueryTimeout;

	// v.1.27+ flags come first
	DWORD uFlags = 0;
	if ( uVer>=0x11B )
		uFlags = tReq.GetDword();

	// v.1.0. mode, limits, weights, ID/TS ranges
	tQuery.m_iOffset = tReq.GetInt ();
	tQuery.m_iLimit = tReq.GetInt ();
	tQuery.m_eMode = (ESphMatchMode) tReq.GetInt ();
	tQuery.m_eRanker = (ESphRankMode) tReq.GetInt ();
	if ( tQuery.m_eRanker==SPH_RANK_EXPR || tQuery.m_eRanker==SPH_RANK_EXPORT )
		tQuery.m_sRankerExpr = tReq.GetString();

	tQuery.m_eSort = (ESphSortOrder) tReq.GetInt ();
	tQuery.m_sSortBy = tReq.GetString ();
	sphColumnToLowercase ( const_cast<char *>( tQuery.m_sSortBy.cstr() ) );
	tQuery.m_sRawQuery = tReq.GetString ();
	{
		int iGot = 0;
		if ( !tReq.GetDwords ( tQuery.m_dWeights, iGot, SPH_MAX_FIELDS ) )
		{
			SendErrorReply ( tOut, "invalid weight count %d (should be in 0..%d range)", iGot, SPH_MAX_FIELDS );
			return false;
		}
	}

	tQuery.m_sIndexes = tReq.GetString ();
	bool bIdrange64 = tReq.GetInt()!=0;

	SphDocID_t uMinID = bIdrange64 ? (SphDocID_t)tReq.GetUint64 () : tReq.GetDword ();
	SphDocID_t uMaxID = bIdrange64 ? (SphDocID_t)tReq.GetUint64 () : tReq.GetDword ();

	if ( uVer<0x108 && uMaxID==0xffffffffUL )
		uMaxID = 0; // fixup older clients which send 32-bit UINT_MAX by default

	if ( uMaxID==0 )
		uMaxID = DOCID_MAX;

	int iAttrFilters = tReq.GetInt ();
	if ( iAttrFilters>g_iMaxFilters )
	{
		SendErrorReply ( tOut, "too many attribute filters (req=%d, max=%d)", iAttrFilters, g_iMaxFilters );
		return false;
	}

	tQuery.m_dFilters.Resize ( iAttrFilters );
	for ( auto & i : tQuery.m_dFilters )
		if ( !ParseSearchFilter ( i, tReq, tOut, uMasterVer ) )
			return false;

	// now add id range filter
	if ( uMinID!=0 || uMaxID!=DOCID_MAX )
	{
		CSphFilterSettings & tFilter = tQuery.m_dFilters.Add();
		tFilter.m_sAttrName = "@id";
		tFilter.m_eType = SPH_FILTER_RANGE;
		tFilter.m_iMinValue = uMinID;
		tFilter.m_iMaxValue = uMaxID;
	}

	tQuery.m_eGroupFunc = (ESphGroupBy) tReq.GetDword ();
	tQuery.m_sGroupBy = tReq.GetString ();
	sphColumnToLowercase ( const_cast<char *>( tQuery.m_sGroupBy.cstr() ) );

	tQuery.m_iMaxMatches = tReq.GetInt ();
	tQuery.m_sGroupSortBy = tReq.GetString ();
	tQuery.m_iCutoff = tReq.GetInt();
	tQuery.m_iRetryCount = tReq.GetInt ();
	tQuery.m_iRetryDelay = tReq.GetInt ();
	tQuery.m_sGroupDistinct = tReq.GetString ();
	sphColumnToLowercase ( const_cast<char *>( tQuery.m_sGroupDistinct.cstr() ) );

	tQuery.m_bGeoAnchor = ( tReq.GetInt()!=0 );
	if ( tQuery.m_bGeoAnchor )
	{
		tQuery.m_sGeoLatAttr = tReq.GetString ();
		tQuery.m_sGeoLongAttr = tReq.GetString ();
		tQuery.m_fGeoLatitude = tReq.GetFloat ();
		tQuery.m_fGeoLongitude = tReq.GetFloat ();
	}

	tQuery.m_dIndexWeights.Resize ( tReq.GetInt() ); // FIXME! add sanity check
	for ( auto& dIndexWeight : tQuery.m_dIndexWeights )
	{
		dIndexWeight.m_sName = tReq.GetString ();
		dIndexWeight.m_iValue = tReq.GetInt ();
	}

	tQuery.m_uMaxQueryMsec = tReq.GetDword ();

	tQuery.m_dFieldWeights.Resize ( tReq.GetInt() ); // FIXME! add sanity check
	for ( auto & dFieldWeight : tQuery.m_dFieldWeights )
	{
		dFieldWeight.m_sName = tReq.GetString ();
		dFieldWeight.m_iValue = tReq.GetInt ();
	}

	tQuery.m_sComment = tReq.GetString ();

	tQuery.m_dOverrides.Resize ( tReq.GetInt() ); // FIXME! add sanity check
	for ( CSphAttrOverride &tOverride : tQuery.m_dOverrides )
	{
		tOverride.m_sAttr = tReq.GetString ();
		tOverride.m_eAttrType = (ESphAttr) tReq.GetDword ();

		tOverride.m_dValues.Resize ( tReq.GetInt() ); // FIXME! add sanity check
		for ( auto& tEntry : tOverride.m_dValues )
		{
			tEntry.m_uDocID = (SphDocID_t) tReq.GetUint64 ();

			if ( tOverride.m_eAttrType==SPH_ATTR_FLOAT )		tEntry.m_fValue = tReq.GetFloat ();
			else if ( tOverride.m_eAttrType==SPH_ATTR_BIGINT )	tEntry.m_uValue = tReq.GetUint64 ();
			else												tEntry.m_uValue = tReq.GetDword ();
		}
	}

	tQuery.m_sSelect = tReq.GetString ();
	tQuery.m_bAgent = ( uMasterVer>0 );
	if ( tQuery.m_sSelect.Begins ( "*,*" ) ) // this is the legacy mark of agent for debug purpose
	{
		tQuery.m_bAgent = true;
		int iSelectLen = tQuery.m_sSelect.Length();
		tQuery.m_sSelect = ( iSelectLen>4 ? tQuery.m_sSelect.SubString ( 4, iSelectLen-4 ) : "*" );
	}
	// fixup select list
	if ( tQuery.m_sSelect.IsEmpty () )
		tQuery.m_sSelect = "*";

	// master sends items to agents since master.version=15 
	CSphString sError;
	if ( uMasterVer<15 && !ParseSelectList ( sError, tQuery ) )
	{
		// we want to see a parse error in query_log_format=sphinxql mode too
		if ( g_eLogFormat==LOG_FORMAT_SPHINXQL && g_iQueryLogFile>=0 )
		{
			StringBuilder_c tBuf;
			char sTimeBuf [ SPH_TIME_PID_MAX_SIZE ];
			sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );
			tBuf << "/""* " << sTimeBuf << "*""/ " << tQuery.m_sSelect << " # error=" << sError << "\n";
			sphSeek ( g_iQueryLogFile, 0, SEEK_END );
			sphWrite ( g_iQueryLogFile, tBuf.cstr(), tBuf.GetLength() );
		}

		SendErrorReply ( tOut, "select: %s", sError.cstr () );
		return false;
	}

	// v.1.27
	if ( uVer>=0x11B )
	{
		// parse simple flags
		tQuery.m_bReverseScan = !!( uFlags & QFLAG_REVERSE_SCAN );
		tQuery.m_bSortKbuffer = !!( uFlags & QFLAG_SORT_KBUFFER );
		tQuery.m_bSimplify = !!( uFlags & QFLAG_SIMPLIFY );
		tQuery.m_bPlainIDF = !!( uFlags & QFLAG_PLAIN_IDF );
		tQuery.m_bGlobalIDF = !!( uFlags & QFLAG_GLOBAL_IDF );
		tQuery.m_bLocalDF = !!( uFlags & QFLAG_LOCAL_DF );
		tQuery.m_bLowPriority = !!( uFlags & QFLAG_LOW_PRIORITY );
		tQuery.m_bFacet = !!( uFlags & QFLAG_FACET );
		tQuery.m_bFacetHead = !!( uFlags & QFLAG_FACET_HEAD );
		tQuery.m_eQueryType = (uFlags & QFLAG_JSON_QUERY) ? QUERY_JSON : QUERY_API;

		if ( uMasterVer>0 || uVer==0x11E )
			tQuery.m_bNormalizedTFIDF = !!( uFlags & QFLAG_NORMALIZED_TF );

		// fetch optional stuff
		if ( uFlags & QFLAG_MAX_PREDICTED_TIME )
			tQuery.m_iMaxPredictedMsec = tReq.GetInt();
	}

	// v.1.29
	if ( uVer>=0x11D )
	{
		tQuery.m_sOuterOrderBy = tReq.GetString();
		tQuery.m_iOuterOffset = tReq.GetDword();
		tQuery.m_iOuterLimit = tReq.GetDword();
		tQuery.m_bHasOuter = ( tReq.GetInt()!=0 );
	}

	// extension v.1
	tQuery.m_eCollation = g_eCollation;
	if ( uMasterVer>=1 )
		tQuery.m_eCollation = (ESphCollation)tReq.GetDword();

	// extension v.2
	if ( uMasterVer>=2 )
	{
		tQuery.m_sOuterOrderBy = tReq.GetString();
		if ( tQuery.m_bHasOuter )
			tQuery.m_iOuterLimit = tReq.GetInt();
	}

	if ( uMasterVer>=6 )
		tQuery.m_iGroupbyLimit = tReq.GetInt();

	if ( uMasterVer>=14 )
	{
		tQuery.m_sUDRanker = tReq.GetString();
		tQuery.m_sUDRankerOpts = tReq.GetString();
	}

	if ( uMasterVer>=14 || uVer>=0x120 )
	{
		tQuery.m_sQueryTokenFilterLib = tReq.GetString();
		tQuery.m_sQueryTokenFilterName = tReq.GetString();
		tQuery.m_sQueryTokenFilterOpts = tReq.GetString();
	}

	if ( uVer>=0x121 )
	{
		tQuery.m_dFilterTree.Resize ( tReq.GetInt() );
		for ( FilterTreeItem_t &tItem : tQuery.m_dFilterTree )
		{
			tItem.m_iLeft = tReq.GetInt();
			tItem.m_iRight = tReq.GetInt();
			tItem.m_iFilterItem = tReq.GetInt();
			tItem.m_bOr = ( tReq.GetInt()!=0 );
		}
	}

	if ( uMasterVer>=15 )
	{
		tQuery.m_dItems.Resize ( tReq.GetInt() );
		for ( CSphQueryItem &tItem : tQuery.m_dItems )
		{
			tItem.m_sAlias = tReq.GetString();
			tItem.m_sExpr = tReq.GetString();
			tItem.m_eAggrFunc = (ESphAggrFunc)tReq.GetDword();
		}
		tQuery.m_dRefItems.Resize ( tReq.GetInt() );
		for ( CSphQueryItem &tItem : tQuery.m_dRefItems )
		{
			tItem.m_sAlias = tReq.GetString();
			tItem.m_sExpr = tReq.GetString();
			tItem.m_eAggrFunc = (ESphAggrFunc)tReq.GetDword();
		}
	}

	if ( uMasterVer>=16 )
		tQuery.m_eExpandKeywords = (QueryOption_e)tReq.GetDword();

	/////////////////////
	// additional checks
	/////////////////////

	if ( tReq.GetError() )
	{
		SendErrorReply ( tOut, "invalid or truncated request" );
		return false;
	}

	CheckQuery ( tQuery, sError );
	if ( !sError.IsEmpty() )
	{
		SendErrorReply ( tOut, "%s", sError.cstr() );
		return false;
	}

	// now prepare it for the engine
	tQuery.m_sQuery = tQuery.m_sRawQuery;

	if ( tQuery.m_eQueryType!=QUERY_JSON )
		PrepareQueryEmulation ( &tQuery );

	FixupQuerySettings ( tQuery );

	// all ok
	return true;
}

//////////////////////////////////////////////////////////////////////////

struct EscapeQuotation_t
{
	static const char cQuote = '\'';

	inline static bool IsEscapeChar ( char c )
	{
		return ( c=='\\' || c=='\'' );
	}

	inline static char GetEscapedChar ( char c )
	{
		return c;
	}
};


using QuotationEscapedBuilder = EscapedStringBuilder_T<EscapeQuotation_t>;


void LogQueryPlain ( const CSphQuery & tQuery, const CSphQueryResult & tRes )
{
	assert ( g_eLogFormat==LOG_FORMAT_PLAIN );
	if ( ( !g_bQuerySyslog && g_iQueryLogFile<0 ) || !tRes.m_sError.IsEmpty() )
		return;

	QuotationEscapedBuilder tBuf;

	// [time]
#if USE_SYSLOG
	if ( !g_bQuerySyslog )
	{
#endif

		char sTimeBuf[SPH_TIME_PID_MAX_SIZE];
		sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );
		tBuf.Appendf ( "[%s]", sTimeBuf );

#if USE_SYSLOG
	} else
		tBuf += "[query]";
#endif

	// querytime sec
	int iQueryTime = Max ( tRes.m_iQueryTime, 0 );
	int iRealTime = Max ( tRes.m_iRealQueryTime, 0 );
	tBuf.Appendf ( " %d.%03d sec", iRealTime/1000, iRealTime%1000 );
	tBuf.Appendf ( " %d.%03d sec", iQueryTime/1000, iQueryTime%1000 );

	// optional multi-query multiplier
	if ( tRes.m_iMultiplier>1 )
		tBuf.Appendf ( " x%d", tRes.m_iMultiplier );

	// [matchmode/numfilters/sortmode matches (offset,limit)
	static const char * sModes [ SPH_MATCH_TOTAL ] = { "all", "any", "phr", "bool", "ext", "scan", "ext2" };
	static const char * sSort [ SPH_SORT_TOTAL ] = { "rel", "attr-", "attr+", "tsegs", "ext", "expr" };
	tBuf.Appendf ( " [%s/%d/%s " INT64_FMT " (%d,%d)",
		sModes [ tQuery.m_eMode ], tQuery.m_dFilters.GetLength(), sSort [ tQuery.m_eSort ],
		tRes.m_iTotalMatches, tQuery.m_iOffset, tQuery.m_iLimit );

	// optional groupby info
	if ( !tQuery.m_sGroupBy.IsEmpty() )
		tBuf.Appendf ( " @%s", tQuery.m_sGroupBy.cstr() );

	// ] [indexes]
	tBuf.Appendf ( "] [%s]", tQuery.m_sIndexes.cstr() );

	// optional performance counters
	if ( g_bIOStats || g_bCpuStats )
	{
		const CSphIOStats & IOStats = tRes.m_tIOStats;

		tBuf += " [";

		if ( g_bIOStats )
			tBuf.Appendf ( "ios=%d kb=%d.%d ioms=%d.%d",
				IOStats.m_iReadOps, (int)( IOStats.m_iReadBytes/1024 ), (int)( IOStats.m_iReadBytes%1024 )*10/1024,
				(int)( IOStats.m_iReadTime/1000 ), (int)( IOStats.m_iReadTime%1000 )/100 );

		if ( g_bIOStats && g_bCpuStats )
			tBuf += " ";

		if ( g_bCpuStats )
			tBuf.Appendf ( "cpums=%d.%d", (int)( tRes.m_iCpuTime/1000 ), (int)( tRes.m_iCpuTime%1000 )/100 );

		tBuf += "]";
	}

	// optional query comment
	if ( !tQuery.m_sComment.IsEmpty() )
		tBuf.Appendf ( " [%s]", tQuery.m_sComment.cstr() );

	// query
	// (m_sRawQuery is empty when using MySQL handler)
	const CSphString & sQuery = tQuery.m_sRawQuery.IsEmpty()
		? tQuery.m_sQuery
		: tQuery.m_sRawQuery;

	if ( !sQuery.IsEmpty() )
	{
		tBuf += " ";
		tBuf.AppendEscaped ( sQuery.cstr(), EscBld::eFixupSpace );
	}

#if USE_SYSLOG
	if ( !g_bQuerySyslog )
	{
#endif

	// line feed
	tBuf += "\n";

	sphSeek ( g_iQueryLogFile, 0, SEEK_END );
	sphWrite ( g_iQueryLogFile, tBuf.cstr(), tBuf.GetLength() );

#if USE_SYSLOG
	} else
	{
		syslog ( LOG_INFO, "%s", tBuf.cstr() );
	}
#endif
}

class UnBackquote_fn : public ISphNoncopyable
{
	CSphString m_sBuf;
	const char * m_pDst;

public:
	explicit UnBackquote_fn ( const char * pSrc )
	{
		m_pDst = pSrc;
		int iLen = 0;
		if ( pSrc && *pSrc )
			iLen = strlen ( pSrc );

		if ( iLen && memchr ( pSrc, '`', iLen ) )
		{
			m_sBuf = pSrc;
			char * pDst = const_cast<char *>( m_sBuf.cstr() );
			const char * pEnd = pSrc + iLen;

			while ( pSrc<pEnd )
			{
				*pDst = *pSrc++;
				if ( *pDst!='`' )
					pDst++;
			}
			*pDst = '\0';
			m_pDst = m_sBuf.cstr();
		}
	}

	const char * cstr() { return m_pDst; }
};

static void FormatOrderBy ( StringBuilder_c * pBuf, const char * sPrefix, ESphSortOrder eSort, const CSphString & sSort )
{
	assert ( pBuf );
	if ( eSort==SPH_SORT_EXTENDED && sSort=="@weight desc" )
		return;

	const char * sSubst = "@weight";
	if ( sSort!="@relevance" )
			sSubst = sSort.cstr();

	UnBackquote_fn tUnquoted ( sSubst );
	sSubst = tUnquoted.cstr();

	*pBuf << " " << sPrefix << " ";
	switch ( eSort )
	{
	case SPH_SORT_ATTR_DESC:		*pBuf << sSubst << " DESC"; break;
	case SPH_SORT_ATTR_ASC:			*pBuf << sSubst << " ASC"; break;
	case SPH_SORT_TIME_SEGMENTS:	*pBuf << "TIME_SEGMENT(" << sSubst << ")"; break;
	case SPH_SORT_EXTENDED:			*pBuf << sSubst; break;
	case SPH_SORT_EXPR:				*pBuf << "BUILTIN_EXPR()"; break;
	case SPH_SORT_RELEVANCE:		*pBuf << "weight() desc" << ( sSubst && *sSubst ? ", " : nullptr ) << ( sSubst && *sSubst ? sSubst : nullptr ); break;
	default:						pBuf->Appendf ( "mode-%d", (int)eSort ); break;
	}
}

static const CSphQuery g_tDefaultQuery {};

static void FormatSphinxql ( const CSphQuery & q, int iCompactIN, QuotationEscapedBuilder & tBuf );
static void FormatList ( const CSphVector<CSphNamedInt> & dValues, StringBuilder_c & tBuf )
{
	ScopedComma_c tComma ( tBuf );
	for ( const auto& dValue : dValues )
		tBuf.Appendf ( "%s=%d", dValue.m_sName.cstr(), dValue.m_iValue );
}

static void FormatOption ( const CSphQuery & tQuery, StringBuilder_c & tBuf )
{
	ScopedComma_c tOptionComma ( tBuf, ", ", " OPTION ");

	if ( tQuery.m_iMaxMatches!=DEFAULT_MAX_MATCHES )
		tBuf.Appendf ( "max_matches=%d", tQuery.m_iMaxMatches );

	if ( !tQuery.m_sComment.IsEmpty() )
		tBuf.Appendf ( "comment='%s'", tQuery.m_sComment.cstr() ); // FIXME! escape, replace newlines..

	if ( tQuery.m_eRanker!=SPH_RANK_DEFAULT )
	{
		const char * sRanker = sphGetRankerName ( tQuery.m_eRanker );
		if ( !sRanker )
			sRanker = sphGetRankerName ( SPH_RANK_DEFAULT );

		tBuf.Appendf ( "ranker=%s", sRanker );

		if ( tQuery.m_sRankerExpr.IsEmpty() )
			tBuf.Appendf ( "ranker=%s", sRanker );
		else
			tBuf.Appendf ( "ranker=%s(\'%s\')", sRanker, tQuery.m_sRankerExpr.scstr() );
	}

	if ( tQuery.m_iAgentQueryTimeout!=g_iAgentQueryTimeout )
		tBuf.Appendf ( "agent_query_timeout=%d", tQuery.m_iAgentQueryTimeout );

	if ( tQuery.m_iCutoff!=g_tDefaultQuery.m_iCutoff )
		tBuf.Appendf ( "cutoff=%d", tQuery.m_iCutoff );

	if ( tQuery.m_dFieldWeights.GetLength() )
	{
		tBuf.StartBlock (nullptr,"field_weights=(",")");
		FormatList ( tQuery.m_dFieldWeights, tBuf );
		tBuf.FinishBlock ();
	}

	if ( tQuery.m_bGlobalIDF!=g_tDefaultQuery.m_bGlobalIDF )
		tBuf << "global_idf=1";

	if ( tQuery.m_bPlainIDF || !tQuery.m_bNormalizedTFIDF )
	{
		tBuf.StartBlock(",","idf='","'");
		tBuf << ( tQuery.m_bPlainIDF ? "plain" : "normalized" )
		<< ( tQuery.m_bNormalizedTFIDF ? "tfidf_normalized" : "tfidf_unnormalized" );
		tBuf.FinishBlock ();
	}

	if ( tQuery.m_bLocalDF!=g_tDefaultQuery.m_bLocalDF )
		tBuf << "local_df=1";

	if ( tQuery.m_dIndexWeights.GetLength() )
	{
		tBuf.StartBlock ( nullptr, "index_weights=(", ")" );
		FormatList ( tQuery.m_dIndexWeights, tBuf );
		tBuf.FinishBlock ();
	}

	if ( tQuery.m_uMaxQueryMsec!=g_tDefaultQuery.m_uMaxQueryMsec )
		tBuf.Appendf ( "max_query_time=%u", tQuery.m_uMaxQueryMsec );

	if ( tQuery.m_iMaxPredictedMsec!=g_tDefaultQuery.m_iMaxPredictedMsec )
		tBuf.Appendf ( "max_predicted_time=%d", tQuery.m_iMaxPredictedMsec );

	if ( tQuery.m_iRetryCount!=-1 )
		tBuf.Appendf ( "retry_count=%d", tQuery.m_iRetryCount );

	if ( tQuery.m_iRetryDelay!=-1 )
		tBuf.Appendf ( "retry_delay=%d", tQuery.m_iRetryDelay );

	if ( tQuery.m_iRandSeed!=g_tDefaultQuery.m_iRandSeed )
		tBuf.Appendf ( "rand_seed=" INT64_FMT, tQuery.m_iRandSeed );

	if ( !tQuery.m_sQueryTokenFilterLib.IsEmpty() )
	{
		if ( tQuery.m_sQueryTokenFilterOpts.IsEmpty() )
			tBuf.Appendf ( "token_filter = '%s:%s'", tQuery.m_sQueryTokenFilterLib.cstr(), tQuery.m_sQueryTokenFilterName.cstr() );
		else
			tBuf.Appendf ( "token_filter = '%s:%s:%s'", tQuery.m_sQueryTokenFilterLib.cstr(), tQuery.m_sQueryTokenFilterName.cstr(), tQuery.m_sQueryTokenFilterOpts.cstr() );
	}

	if ( tQuery.m_bIgnoreNonexistent )
		tBuf << "ignore_nonexistent_columns=1";

	if ( tQuery.m_bIgnoreNonexistentIndexes )
		tBuf << "ignore_nonexistent_indexes=1";

	if ( tQuery.m_bStrict )
		tBuf << "strict=1";

	if ( tQuery.m_eExpandKeywords!=QUERY_OPT_DEFAULT )
		tBuf.Appendf ( "expand_keywords=%d", ( tQuery.m_eExpandKeywords==QUERY_OPT_ENABLED ? 1 : 0 ) );
}

static void LogQuerySphinxql ( const CSphQuery & q, const CSphQueryResult & tRes, const CSphVector<int64_t> & dAgentTimes, int iCid )
{
	assert ( g_eLogFormat==LOG_FORMAT_SPHINXQL );
	if ( g_iQueryLogFile<0 )
		return;

	QuotationEscapedBuilder tBuf;
	int iCompactIN = ( g_bLogCompactIn ? LOG_COMPACT_IN : 0 );

	// time, conn id, wall, found
	int iQueryTime = Max ( tRes.m_iQueryTime, 0 );
	int iRealTime = Max ( tRes.m_iRealQueryTime, 0 );

	char sTimeBuf[SPH_TIME_PID_MAX_SIZE];
	sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );

	tBuf += R"(/* )";
	tBuf += sTimeBuf;

	if ( tRes.m_iMultiplier>1 )
		tBuf.Appendf ( " conn %d real %d.%03d wall %d.%03d x%d found " INT64_FMT " *""/ ",
			iCid, iRealTime/1000, iRealTime%1000, iQueryTime/1000, iQueryTime%1000, tRes.m_iMultiplier, tRes.m_iTotalMatches );
	else
		tBuf.Appendf ( " conn %d real %d.%03d wall %d.%03d found " INT64_FMT " *""/ ",
			iCid, iRealTime/1000, iRealTime%1000, iQueryTime/1000, iQueryTime%1000, tRes.m_iTotalMatches );

	///////////////////////////////////
	// format request as SELECT query
	///////////////////////////////////

	FormatSphinxql ( q, iCompactIN, tBuf );

	///////////////
	// query stats
	///////////////

	// next block ecnlosed in /* .. */, space-separated
	tBuf.StartBlock ( " ", R"( /*)", " */" );
	if ( !tRes.m_sError.IsEmpty() )
	{
		// all we have is an error
		tBuf.Appendf ( "error=%s", tRes.m_sError.cstr() );

	} else if ( g_bIOStats || g_bCpuStats || dAgentTimes.GetLength() || !tRes.m_sWarning.IsEmpty() )
	{
		// performance counters
		if ( g_bIOStats || g_bCpuStats )
		{
			const CSphIOStats & IOStats = tRes.m_tIOStats;

			if ( g_bIOStats )
				tBuf.Appendf ( "ios=%d kb=%d.%d ioms=%d.%d",
				IOStats.m_iReadOps, (int)( IOStats.m_iReadBytes/1024 ), (int)( IOStats.m_iReadBytes%1024 )*10/1024,
				(int)( IOStats.m_iReadTime/1000 ), (int)( IOStats.m_iReadTime%1000 )/100 );

			if ( g_bCpuStats )
				tBuf.Appendf ( "cpums=%d.%d", (int)( tRes.m_iCpuTime/1000 ), (int)( tRes.m_iCpuTime%1000 )/100 );
		}

		// per-agent times
		if ( dAgentTimes.GetLength() )
		{
			ScopedComma_c dAgents ( tBuf, ", ", " agents=(",")");
			for ( auto iTime : dAgentTimes )
				tBuf.Appendf ( "%d.%03d",
					(int)( iTime/1000),
					(int)( iTime%1000) );
		}

		// warning
		if ( !tRes.m_sWarning.IsEmpty() )
			tBuf.Appendf ( "warning=%s", tRes.m_sWarning.cstr() );
	}
	tBuf.FinishBlock (); // close the comment

	// line feed
	tBuf += "\n";

	sphSeek ( g_iQueryLogFile, 0, SEEK_END );
	sphWrite ( g_iQueryLogFile, tBuf.cstr(), tBuf.GetLength() );
}


void FormatSphinxql ( const CSphQuery & q, int iCompactIN, QuotationEscapedBuilder & tBuf )
{
	if ( q.m_bHasOuter )
		tBuf += "SELECT * FROM (";

	UnBackquote_fn tUnquoted ( q.m_sSelect.cstr() );
	tBuf.Appendf ( "SELECT %s FROM %s", tUnquoted.cstr(), q.m_sIndexes.cstr() );

	// WHERE clause
	// (m_sRawQuery is empty when using MySQL handler)
	const CSphString & sQuery = q.m_sQuery;
	if ( !sQuery.IsEmpty() || q.m_dFilters.GetLength() )
	{
		ScopedComma_c sWHERE ( tBuf, " AND ", " WHERE ");

		if ( !sQuery.IsEmpty() )
		{
			ScopedComma_c sMatch (tBuf, nullptr, "MATCH(", ")");
			tBuf.AppendEscaped ( sQuery.cstr() );
		}

		FormatFiltersQL ( q.m_dFilters, q.m_dFilterTree, tBuf, iCompactIN );
	}

	// ORDER BY and/or GROUP BY clause
	if ( q.m_sGroupBy.IsEmpty() )
	{
		if ( !q.m_sSortBy.IsEmpty() ) // case API SPH_MATCH_EXTENDED2 - SPH_SORT_RELEVANCE
			FormatOrderBy ( &tBuf, " ORDER BY", q.m_eSort, q.m_sSortBy );
	} else
	{
		tBuf.Appendf ( " GROUP BY %s", q.m_sGroupBy.cstr() );
		FormatOrderBy ( &tBuf, "WITHIN GROUP ORDER BY", q.m_eSort, q.m_sSortBy );
		if ( !q.m_tHaving.m_sAttrName.IsEmpty() )
		{
			ScopedComma_c sHawing ( tBuf, nullptr," HAVING ");
			FormatFilterQL ( q.m_tHaving, tBuf, iCompactIN );
		}
		if ( q.m_sGroupSortBy!="@group desc" )
			FormatOrderBy ( &tBuf, "ORDER BY", SPH_SORT_EXTENDED, q.m_sGroupSortBy );
	}

	// LIMIT clause
	if ( q.m_iOffset!=0 || q.m_iLimit!=20 )
		tBuf.Appendf ( " LIMIT %d,%d", q.m_iOffset, q.m_iLimit );

	// OPTION clause
	FormatOption ( q, tBuf );

	// outer order by, limit
	if ( q.m_bHasOuter )
	{
		tBuf += ")";
		if ( !q.m_sOuterOrderBy.IsEmpty() )
			tBuf.Appendf ( " ORDER BY %s", q.m_sOuterOrderBy.cstr() );
		if ( q.m_iOuterOffset>0 )
			tBuf.Appendf ( " LIMIT %d, %d", q.m_iOuterOffset, q.m_iOuterLimit );
		else if ( q.m_iOuterLimit>0 )
			tBuf.Appendf ( " LIMIT %d", q.m_iOuterLimit );
	}

	// finish SQL statement
	tBuf += ";";
}

static void LogQuery ( const CSphQuery & q, const CSphQueryResult & tRes, const CSphVector<int64_t> & dAgentTimes, int iCid )
{
	if ( g_iQueryLogMinMsec>0 && tRes.m_iQueryTime<g_iQueryLogMinMsec )
		return;

	switch ( g_eLogFormat )
	{
		case LOG_FORMAT_PLAIN:		LogQueryPlain ( q, tRes ); break;
		case LOG_FORMAT_SPHINXQL:	LogQuerySphinxql ( q, tRes, dAgentTimes, iCid ); break;
	}
}


static void LogSphinxqlError ( const char * sStmt, const char * sError, int iCid )
{
	if ( g_eLogFormat!=LOG_FORMAT_SPHINXQL || g_iQueryLogFile<0 || !sStmt || !sError )
		return;

	char sTimeBuf[SPH_TIME_PID_MAX_SIZE];
	sphFormatCurrentTime ( sTimeBuf, sizeof(sTimeBuf) );

	StringBuilder_c tBuf;
	tBuf.Appendf ( "/""* %s conn %d *""/ %s # error=%s\n", sTimeBuf, iCid, sStmt, sError );

	sphSeek ( g_iQueryLogFile, 0, SEEK_END );
	sphWrite ( g_iQueryLogFile, tBuf.cstr(), tBuf.GetLength() );
}


void ReportIndexesName ( int iSpanStart, int iSpandEnd, const CSphVector<SearchFailure_t> & dLog, StringBuilder_c & sOut )
{
	int iSpanLen = iSpandEnd - iSpanStart;

	// report distributed index in case all failures are from their locals
	if ( iSpanLen>1 && !dLog[iSpanStart].m_sParentIndex.IsEmpty ()
		&& dLog[iSpanStart].m_sParentIndex==dLog[iSpandEnd-1].m_sParentIndex )
	{
		auto pDist = GetDistr ( dLog[iSpanStart].m_sParentIndex );
		if ( pDist && pDist->m_dLocal.GetLength ()==iSpanLen )
		{
			sOut << dLog[iSpanStart].m_sParentIndex << ": ";
			return;
		}
	}

	// report only first indexes up to 4
	int iEndReport = ( iSpanLen>4 ) ? iSpanStart+3 : iSpandEnd;
	sOut.StartBlock (",");
	for ( int j=iSpanStart; j<iEndReport; ++j )
		sOut << dLog[j].m_sIndex;
	sOut.FinishBlock ();

	// add total index count
	if ( iEndReport!=iSpandEnd )
		sOut.Sprintf ( " and %d more: ", iSpandEnd-iEndReport );
	else
		sOut += ": ";
}

//////////////////////////////////////////////////////////////////////////

// internals attributes are last no need to send them
int sphSendGetAttrCount ( const ISphSchema & tSchema, bool bAgentMode )
{
	int iCount = tSchema.GetAttrsCount();

	if ( bAgentMode )
		return iCount;

	if ( iCount && sphIsSortStringInternal ( tSchema.GetAttr ( iCount-1 ).m_sName.cstr() ) )
	{
		for ( int i=iCount-1; i>=0 && sphIsSortStringInternal ( tSchema.GetAttr(i).m_sName.cstr() ); --i )
		{
			iCount = i;
		}
	}

	return iCount;
}

static int SendDataPtrAttr ( ISphOutputBuffer * pOut, const BYTE * pData )
{
	int iLen = pData ? sphUnpackPtrAttr ( pData, &pData ) : 0;
	if ( pOut )
		pOut->SendArray ( pData, iLen );
	return iLen;
}

static char g_sJsonNull[] = "{}";

static int SendJsonAsString ( ISphOutputBuffer * pOut, const BYTE * pJSON )
{
	if ( pJSON )
	{
		int iLengthBytes = sphUnpackPtrAttr ( pJSON, &pJSON );
		JsonEscapedBuilder dJson;
		dJson.GrowEnough ( iLengthBytes*2 );
		sphJsonFormat ( dJson, pJSON );

		if ( pOut )
			pOut->SendArray ( dJson );

		return dJson.GetLength();
	} else
	{
		// magic zero - "{}"
		int iLengthBytes = sizeof(g_sJsonNull)-1;
		if ( pOut )
		{
			pOut->SendDword ( iLengthBytes );
			pOut->SendBytes ( g_sJsonNull, iLengthBytes );
		} 
		
		return iLengthBytes;
	}
}


static int SendJson ( ISphOutputBuffer * pOut, const BYTE * pJSON, bool bSendJson )
{
	if ( bSendJson )
		return SendDataPtrAttr ( pOut, pJSON ); // send BSON
	else
		return SendJsonAsString ( pOut, pJSON ); // send string
}


static int SendJsonFieldAsString ( ISphOutputBuffer * pOut, const BYTE * pJSON )
{
	if ( pJSON )
	{
		int iLengthBytes = sphUnpackPtrAttr ( pJSON, &pJSON );
		JsonEscapedBuilder dJson;
		dJson.GrowEnough ( iLengthBytes * 2 );

		auto eJson = (ESphJsonType)*pJSON++;
		sphJsonFieldFormat ( dJson, pJSON, eJson, false );

		if ( pOut )
			pOut->SendArray ( dJson );
		else
			return dJson.GetLength();
	} else
	{
		if ( pOut )
			pOut->SendDword ( 0 );
	}

	return 0;
}


static int SendJsonField ( ISphOutputBuffer * pOut, const BYTE * pJSON, bool bSendJsonField )
{
	if ( bSendJsonField )
	{
		int iLen = sphUnpackPtrAttr ( pJSON, &pJSON );
		if ( iLen )
		{
			ESphJsonType eJson = (ESphJsonType)*pJSON++;
			iLen--;

			if ( pOut )
			{
				pOut->SendByte ( (BYTE)eJson );
				pOut->SendArray ( pJSON, iLen );
			}

			return iLen+1;
		}
		else
		{
			if ( pOut )
				pOut->SendByte ( JSON_EOF );

			return -3; // 4 bytes by default, and we send only 1. this useless magic should be fixed
		}
	} else
		return SendJsonFieldAsString ( pOut, pJSON );
}


static int SendMVA ( ISphOutputBuffer * pOut, const BYTE * pMVA, bool b64bit )
{
	if ( pMVA )
	{
		int iLengthBytes = sphUnpackPtrAttr( pMVA, &pMVA );
		int iValues = iLengthBytes / sizeof(DWORD);
		if ( pOut )
			pOut->SendDword ( iValues );

		const DWORD * pValues = (const DWORD *)pMVA;

		if ( b64bit )
		{
			assert ( ( iValues%2 )==0 );
			while ( iValues )
			{
				uint64_t uVal = (uint64_t)MVA_UPSIZE ( pValues );
				if ( pOut )
					pOut->SendUint64 ( uVal );
				pValues += 2;
				iValues -= 2;
			}
		} else
		{
			while ( iValues-- )
			{
				if ( pOut )
					pOut->SendDword ( *pValues++ );
			}
		}

		return iLengthBytes;
	} else
	{
		if ( pOut )
			pOut->SendDword ( 0 );
	}

	return 0;
}


static ESphAttr FixupAttrForNetwork ( ESphAttr eAttr, WORD uMasterVer, bool bAgentMode )
{
	bool bSendJson = ( bAgentMode && uMasterVer>=3 );
	bool bSendJsonField = ( bAgentMode && uMasterVer>=4 );

	switch ( eAttr )
	{
	case SPH_ATTR_UINT32SET_PTR:
		return SPH_ATTR_UINT32SET;

	case SPH_ATTR_INT64SET_PTR:
		return SPH_ATTR_INT64SET;

	case SPH_ATTR_STRINGPTR:
		return SPH_ATTR_STRING;

	case SPH_ATTR_JSON:
	case SPH_ATTR_JSON_PTR:
		return bSendJson ? SPH_ATTR_JSON : SPH_ATTR_STRING;

	case SPH_ATTR_JSON_FIELD:
	case SPH_ATTR_JSON_FIELD_PTR:
		return bSendJsonField ? SPH_ATTR_JSON_FIELD : SPH_ATTR_STRING;

	default: return eAttr;
	} 
}


static void SendSchema ( ISphOutputBuffer & tOut, const CSphQueryResult & tRes, int iAttrsCount, WORD uMasterVer, bool bAgentMode )
{
	tOut.SendInt ( tRes.m_tSchema.GetFieldsCount() );
	for ( int i=0; i < tRes.m_tSchema.GetFieldsCount(); ++i )
		tOut.SendString ( tRes.m_tSchema.GetFieldName(i) );

	tOut.SendInt ( iAttrsCount );
	for ( int i=0; i<iAttrsCount; ++i )
	{
		const CSphColumnInfo & tCol = tRes.m_tSchema.GetAttr(i);
		tOut.SendString ( tCol.m_sName.cstr() );

		ESphAttr eCol = FixupAttrForNetwork ( tCol.m_eAttrType, uMasterVer, bAgentMode );
		tOut.SendDword ( (DWORD)eCol );
	}
}


static void SendAttribute ( ISphOutputBuffer & tOut, const CSphMatch & tMatch, const CSphColumnInfo & tAttr, int iVer, WORD uMasterVer, bool bAgentMode )
{
	// at this point we should not have any attributes that point to pooled data
	assert ( sphPlainAttrToPtrAttr(tAttr.m_eAttrType)==tAttr.m_eAttrType );

	// send binary json only to master
	bool bSendJson = bAgentMode && uMasterVer>=3;
	bool bSendJsonField = bAgentMode && uMasterVer>=4;

	const CSphAttrLocator & tLoc = tAttr.m_tLocator;
	
	switch ( tAttr.m_eAttrType )
	{
	case SPH_ATTR_UINT32SET_PTR:
	case SPH_ATTR_INT64SET_PTR:
		SendMVA ( &tOut, (const BYTE*)tMatch.GetAttr(tLoc), tAttr.m_eAttrType==SPH_ATTR_INT64SET_PTR );
		break;

	case SPH_ATTR_JSON_PTR:
		SendJson ( &tOut, (const BYTE*)tMatch.GetAttr(tLoc), bSendJson );
		break;

	case SPH_ATTR_STRINGPTR:
		SendDataPtrAttr ( &tOut, (const BYTE*)tMatch.GetAttr(tLoc) );
		break;

	case SPH_ATTR_JSON_FIELD_PTR:
		SendJsonField ( &tOut, (const BYTE*)tMatch.GetAttr(tLoc), bSendJsonField );
		break;

	case SPH_ATTR_FACTORS:
	case SPH_ATTR_FACTORS_JSON:
		if ( iVer<0x11C )
		{
			tOut.SendDword ( 0 );
			break;
		}

		SendDataPtrAttr ( &tOut, (const BYTE*)tMatch.GetAttr(tLoc) );
		break;

	case SPH_ATTR_FLOAT:
		tOut.SendFloat ( tMatch.GetAttrFloat(tLoc) );
		break;
	case SPH_ATTR_BIGINT:
		tOut.SendUint64 ( tMatch.GetAttr(tLoc) );
		break;
	default:
		tOut.SendDword ( (DWORD)tMatch.GetAttr(tLoc) );
		break;
	}
}


void SendResult ( int iVer, ISphOutputBuffer & tOut, const CSphQueryResult * pRes, bool bAgentMode, const CSphQuery & tQuery, WORD uMasterVer )
{
	// multi-query status
	bool bError = !pRes->m_sError.IsEmpty();
	bool bWarning = !bError && !pRes->m_sWarning.IsEmpty();

	if ( bError )
	{
		tOut.SendInt ( SEARCHD_ERROR ); // fixme! m.b. use APICommand_t and refactor to common API way
		tOut.SendString ( pRes->m_sError.cstr() );
		if ( g_bOptNoDetach && g_eLogFormat!=LOG_FORMAT_SPHINXQL )
			sphInfo ( "query error: %s", pRes->m_sError.cstr() );
		return;

	} else if ( bWarning )
	{
		tOut.SendDword ( SEARCHD_WARNING );
		tOut.SendString ( pRes->m_sWarning.cstr() );
		if ( g_bOptNoDetach && g_eLogFormat!=LOG_FORMAT_SPHINXQL )
			sphInfo ( "query warning: %s", pRes->m_sWarning.cstr() );
	} else
		tOut.SendDword ( SEARCHD_OK );

	int iAttrsCount = sphSendGetAttrCount ( pRes->m_tSchema, bAgentMode );

	// send schema
	SendSchema ( tOut, *pRes, iAttrsCount, uMasterVer, bAgentMode );

	// send matches
	tOut.SendInt ( pRes->m_iCount );
	tOut.SendInt ( 1 ); // was USE_64BIT

	CSphVector<BYTE> dJson ( 512 );

	for ( int i=0; i<pRes->m_iCount; ++i )
	{
		const CSphMatch & tMatch = pRes->m_dMatches [ pRes->m_iOffset+i ];
		tOut.SendUint64 ( tMatch.m_uDocID );
		tOut.SendInt ( tMatch.m_iWeight );

		assert ( tMatch.m_pStatic || !pRes->m_tSchema.GetStaticSize() );
#if 0
		// not correct any more because of internal attrs (such as string sorting ptrs)
		assert ( tMatch.m_pDynamic || !pRes->m_tSchema.GetDynamicSize() );
		assert ( !tMatch.m_pDynamic || (int)tMatch.m_pDynamic[-1]==pRes->m_tSchema.GetDynamicSize() );
#endif
		for ( int j=0; j<iAttrsCount; ++j )
			SendAttribute ( tOut, tMatch, pRes->m_tSchema.GetAttr(j), iVer, uMasterVer, bAgentMode );
	}

	if ( tQuery.m_bAgent && tQuery.m_iLimit )
		tOut.SendInt ( pRes->m_iCount );
	else
		tOut.SendInt ( pRes->m_dMatches.GetLength() );

	tOut.SendAsDword ( pRes->m_iTotalMatches );
	tOut.SendInt ( Max ( pRes->m_iQueryTime, 0 ) );

	if ( iVer>=0x11A && bAgentMode )
	{
		bool bNeedPredictedTime = tQuery.m_iMaxPredictedMsec > 0;

		BYTE uStatMask = ( bNeedPredictedTime ? 4 : 0 ) | ( g_bCpuStats ? 2 : 0 ) | ( g_bIOStats ? 1 : 0 );
		tOut.SendByte ( uStatMask );

		if ( g_bIOStats )
		{
			CSphIOStats tStats = pRes->m_tIOStats;
			tStats.Add ( pRes->m_tAgentIOStats );
			tOut.SendUint64 ( tStats.m_iReadTime );
			tOut.SendDword ( tStats.m_iReadOps );
			tOut.SendUint64 ( tStats.m_iReadBytes );
			tOut.SendUint64 ( tStats.m_iWriteTime );
			tOut.SendDword ( tStats.m_iWriteOps );
			tOut.SendUint64 ( tStats.m_iWriteBytes );
		}

		if ( g_bCpuStats )
		{
			int64_t iCpuTime = pRes->m_iCpuTime + pRes->m_iAgentCpuTime;
			tOut.SendUint64 ( iCpuTime );
		}

		if ( bNeedPredictedTime )
			tOut.SendUint64 ( pRes->m_iPredictedTime+pRes->m_iAgentPredictedTime );
	}
	if ( bAgentMode && uMasterVer>=7 )
	{
		tOut.SendDword ( pRes->m_tStats.m_iFetchedDocs + pRes->m_iAgentFetchedDocs );
		tOut.SendDword ( pRes->m_tStats.m_iFetchedHits + pRes->m_iAgentFetchedHits );
		if ( uMasterVer>=8 )
			tOut.SendDword ( pRes->m_tStats.m_iSkips + pRes->m_iAgentFetchedSkips );
	}

	tOut.SendInt ( pRes->m_hWordStats.GetLength() );

	pRes->m_hWordStats.IterateStart();
	while ( pRes->m_hWordStats.IterateNext() )
	{
		const CSphQueryResultMeta::WordStat_t & tStat = pRes->m_hWordStats.IterateGet();
		tOut.SendString ( pRes->m_hWordStats.IterateGetKey().cstr() );
		tOut.SendAsDword ( tStat.m_iDocs );
		tOut.SendAsDword ( tStat.m_iHits );
		if ( bAgentMode )
			tOut.SendByte ( 0 ); // statistics have no expanded terms for now
	}
}

/////////////////////////////////////////////////////////////////////////////

void AggrResult_t::FreeMatchesPtrs ( int iLimit, bool bCommonSchema )
{
	if ( m_dMatches.GetLength ()<=iLimit )
		return;

	if ( bCommonSchema )
	{
		for ( int i = iLimit; i<m_dMatches.GetLength (); ++i )
			m_tSchema.FreeDataPtrs ( &m_dMatches[i] );
	} else
	{
		int nMatches = 0;
		ARRAY_FOREACH ( i, m_dMatchCounts )
		{
			nMatches += m_dMatchCounts[i];

			if ( iLimit<nMatches )
			{
				int iFrom = Max ( iLimit, nMatches - m_dMatchCounts[i] );
				for ( int j = iFrom; j<nMatches; ++j )
					m_dSchemas[i].FreeDataPtrs ( &m_dMatches[j] );
			}
		}
	}
}

void AggrResult_t::ClampMatches ( int iLimit, bool bCommonSchema )
{
	FreeMatchesPtrs ( iLimit, bCommonSchema );
	if ( m_dMatches.GetLength()<=iLimit )
		return;
	m_dMatches.Resize ( iLimit );
}


struct TaggedMatchSorter_fn : public SphAccessor_T<CSphMatch>
{
	void CopyKey ( CSphMatch * pMed, CSphMatch * pVal ) const
	{
		pMed->m_uDocID = pVal->m_uDocID;
		pMed->m_iTag = pVal->m_iTag;
	}

	bool IsLess ( const CSphMatch & a, const CSphMatch & b ) const
	{
		bool bDistA = ( ( a.m_iTag & 0x80000000 )==0x80000000 );
		bool bDistB = ( ( b.m_iTag & 0x80000000 )==0x80000000 );
		// sort by doc_id, dist_tag, tag
		return ( a.m_uDocID < b.m_uDocID ) ||
			( a.m_uDocID==b.m_uDocID && ( ( !bDistA && bDistB ) || ( ( a.m_iTag & 0x7FFFFFFF )>( b.m_iTag & 0x7FFFFFFF ) ) ) );
	}

	// inherited swap does not work on gcc
	void Swap ( CSphMatch * a, CSphMatch * b ) const
	{
		::Swap ( *a, *b );
	}
};


void RemapResult ( const ISphSchema * pTarget, AggrResult_t * pRes )
{
	int iCur = 0;
	CSphVector<int> dMapFrom ( pTarget->GetAttrsCount() );
	CSphVector<int> dRowItems ( pTarget->GetAttrsCount () );
	static const int SIZE_OF_ROW = 8 * sizeof ( CSphRowitem );

	ARRAY_FOREACH ( iSchema, pRes->m_dSchemas )
	{
		dMapFrom.Resize ( 0 );
		dRowItems.Resize ( 0 );
		CSphSchema & dSchema = pRes->m_dSchemas[iSchema];
		for ( int i=0; i<pTarget->GetAttrsCount(); i++ )
		{
			auto iSrcCol = dSchema.GetAttrIndex ( pTarget->GetAttr ( i ).m_sName.cstr () );
			const CSphColumnInfo &tSrcCol = dSchema.GetAttr ( iSrcCol );
			dMapFrom.Add ( iSrcCol );
			dRowItems.Add ( tSrcCol.m_tLocator.m_iBitOffset / SIZE_OF_ROW );
			assert ( dMapFrom[i]>=0
				|| pTarget->GetAttr(i).m_tLocator.IsID()
				|| sphIsSortStringInternal ( pTarget->GetAttr(i).m_sName.cstr() )
				|| pTarget->GetAttr(i).m_sName=="@groupbystr"
				);
		}
		int iLimit = Min ( iCur + pRes->m_dMatchCounts[iSchema], pRes->m_dMatches.GetLength() );

		// inverse dRowItems - we'll free only those NOT enumerated yet
		dRowItems = dSchema.SubsetPtrs ( dRowItems );
		for ( int i=iCur; i<iLimit; i++ )
		{
			CSphMatch & tMatch = pRes->m_dMatches[i];

			// create new and shiny (and properly sized) match
			CSphMatch tRow;
			tRow.Reset ( pTarget->GetDynamicSize() );
			tRow.m_uDocID = tMatch.m_uDocID;
			tRow.m_iWeight = tMatch.m_iWeight;
			tRow.m_iTag = tMatch.m_iTag;

			// remap attrs
			for ( int j=0; j<pTarget->GetAttrsCount(); j++ )
			{
				const CSphColumnInfo & tDst = pTarget->GetAttr(j);
				// we could keep some of the rows static
				// and so, avoid the duplication of the data.
				if ( !tDst.m_tLocator.m_bDynamic )
				{
					assert ( dMapFrom[j]<0 || !dSchema.GetAttr ( dMapFrom[j] ).m_tLocator.m_bDynamic );
					tRow.m_pStatic = tMatch.m_pStatic;
				} else if ( dMapFrom[j]>=0 )
				{
					const CSphColumnInfo & tSrc = dSchema.GetAttr ( dMapFrom[j] );
					if ( tDst.m_eAttrType==SPH_ATTR_FLOAT && tSrc.m_eAttrType==SPH_ATTR_BOOL )
					{
						tRow.SetAttrFloat ( tDst.m_tLocator, ( tMatch.GetAttr ( tSrc.m_tLocator )>0 ? 1.0f : 0.0f ) );
					} else
					{
						tRow.SetAttr ( tDst.m_tLocator, tMatch.GetAttr ( tSrc.m_tLocator ) );
					}
				}
			}
			// swap out old (most likely wrong sized) match
			Swap ( tMatch, tRow );
			dSchema.FreeDataSpecial ( &tRow, dRowItems );
		}

		iCur = iLimit;
	}
	assert ( iCur==pRes->m_dMatches.GetLength() );
}

// rebuild the results itemlist expanding stars
const CSphVector<CSphQueryItem> & ExpandAsterisk ( const ISphSchema & tSchema,
	const CSphVector<CSphQueryItem> & tItems, CSphVector<CSphQueryItem> & tExpanded, bool bNoID, bool bOnlyPlain, bool & bHaveExprs )
{
	// the result schema usually is the index schema + calculated items + @-items
	// we need to extract the index schema only
	CSphVector<int> dIndexSchemaItems;
	bool bHaveAsterisk = false;
	for ( const auto & i : tItems )
	{
		if ( i.m_sAlias.cstr() )
		{
			int j = tSchema.GetAttrIndex ( i.m_sAlias.cstr() );
			if ( j>=0 )
				dIndexSchemaItems.Add(j);
		}

		bHaveAsterisk |= i.m_sExpr=="*";
	}

	// no stars? Nothing to do.
	if ( !bHaveAsterisk )
		return tItems;

	dIndexSchemaItems.Sort();

	// find items that are in index schema but not in our requested item list
	// not do not include @-items
	CSphVector<int> dItemsLeftInSchema;
	for ( int i = 0; i < tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr(i);

		if ( tAttr.m_pExpr )
		{
			bHaveExprs = true;

			if ( bOnlyPlain )
				continue;
		}

		if ( tAttr.m_sName.cstr()[0]!='@' && !dIndexSchemaItems.BinarySearch(i) )
			dItemsLeftInSchema.Add(i);
	}

	ARRAY_FOREACH ( i, tItems )
	{
		if ( tItems[i].m_sExpr=="*" )
		{ // asterisk expands to 'id' + all the items from the schema
			if ( tSchema.GetAttrIndex ( "id" )<0 && !bNoID )
				tExpanded.Add().m_sExpr = "id";

			for ( auto j : dItemsLeftInSchema )
			{
				const CSphString & sName = tSchema.GetAttr(j).m_sName;
				if ( !j && bNoID && sName=="id" )
					continue;
				
				tExpanded.Add().m_sExpr = sName;
			}
		} else
			tExpanded.Add ( tItems[i] );
	}

	return tExpanded;
}


static int KillAllDupes ( ISphMatchSorter * pSorter, AggrResult_t & tRes )
{
	assert ( pSorter );
	int iDupes = 0;

	if ( pSorter->IsGroupby () )
	{
		// groupby sorter does that automagically
		pSorter->SetMVAPool ( NULL, false ); // because we must be able to group on @groupby anyway
		pSorter->SetStringPool ( NULL );
		int iMC = 0;
		int iBound = 0;

		ARRAY_FOREACH ( i, tRes.m_dMatches )
		{
			CSphMatch & tMatch = tRes.m_dMatches[i];
			if ( !pSorter->PushGrouped ( tMatch, i==iBound ) )
				iDupes++;

			if ( i==iBound )
				iBound += tRes.m_dMatchCounts[iMC++];
		}
	} else
	{
		// normal sorter needs massasging
		// sort by docid and then by tag to guarantee the replacement order
		TaggedMatchSorter_fn fnSort;
		sphSort ( tRes.m_dMatches.Begin(), tRes.m_dMatches.GetLength(), fnSort, fnSort );

		// by default, simply remove dupes (select first by tag)
		ARRAY_FOREACH ( i, tRes.m_dMatches )
		{
			if ( i==0 || tRes.m_dMatches[i].m_uDocID!=tRes.m_dMatches[i-1].m_uDocID )
				pSorter->Push ( tRes.m_dMatches[i] );
			else
				iDupes++;
		}
	}

	ARRAY_FOREACH ( i, tRes.m_dMatches )
		tRes.m_tSchema.FreeDataPtrs ( &(tRes.m_dMatches[i]) );

	tRes.m_dMatches.Reset ();
	sphFlattenQueue ( pSorter, &tRes, -1 );
	return iDupes;
}


static void RecoverAggregateFunctions ( const CSphQuery & tQuery, const AggrResult_t & tRes )
{
	ARRAY_FOREACH ( i, tQuery.m_dItems )
	{
		const CSphQueryItem & tItem = tQuery.m_dItems[i];
		if ( tItem.m_eAggrFunc==SPH_AGGR_NONE )
			continue;

		for ( int j=0; j<tRes.m_tSchema.GetAttrsCount(); j++ )
		{
			CSphColumnInfo & tCol = const_cast<CSphColumnInfo&> ( tRes.m_tSchema.GetAttr(j) );
			if ( tCol.m_sName==tItem.m_sAlias )
			{
				assert ( tCol.m_eAggrFunc==SPH_AGGR_NONE );
				tCol.m_eAggrFunc = tItem.m_eAggrFunc;
			}
		}
	}
}


struct GenericMatchSort_fn : public CSphMatchComparatorState
{
	bool IsLess ( const CSphMatch * a, const CSphMatch * b ) const
	{
		for ( int i=0; i<CSphMatchComparatorState::MAX_ATTRS; i++ )
			switch ( m_eKeypart[i] )
		{
			case SPH_KEYPART_ID:
				if ( a->m_uDocID==b->m_uDocID )
					continue;
				return ( ( m_uAttrDesc>>i ) & 1 ) ^ ( a->m_uDocID < b->m_uDocID );

			case SPH_KEYPART_WEIGHT:
				if ( a->m_iWeight==b->m_iWeight )
					continue;
				return ( ( m_uAttrDesc>>i ) & 1 ) ^ ( a->m_iWeight < b->m_iWeight );

			case SPH_KEYPART_INT:
			{
				register SphAttr_t aa = a->GetAttr ( m_tLocator[i] );
				register SphAttr_t bb = b->GetAttr ( m_tLocator[i] );
				if ( aa==bb )
					continue;
				return ( ( m_uAttrDesc>>i ) & 1 ) ^ ( aa < bb );
			}
			case SPH_KEYPART_FLOAT:
			{
				register float aa = a->GetAttrFloat ( m_tLocator[i] );
				register float bb = b->GetAttrFloat ( m_tLocator[i] );
				if ( aa==bb )
					continue;
				return ( ( m_uAttrDesc>>i ) & 1 ) ^ ( aa < bb );
			}
			case SPH_KEYPART_STRINGPTR:
			case SPH_KEYPART_STRING:
			{
				int iCmp = CmpStrings ( *a, *b, i );
				if ( iCmp!=0 )
					return ( ( m_uAttrDesc>>i ) & 1 ) ^ ( iCmp < 0 );
				break;
			}
		}
		return false;
	}
};


/// returns internal magic names for expressions like COUNT(*) that have a corresponding one
/// returns expression itself otherwise
const char * GetMagicSchemaName ( const CSphString & s )
{
	if ( s=="count(*)" )
		return "@count";
	if ( s=="weight()" )
		return "@weight";
	if ( s=="groupby()" )
		return "@groupby";
	return s.cstr();
}


/// a functor to sort columns by (is_aggregate ASC, column_index ASC)
struct AggregateColumnSort_fn
{
	bool IsAggr ( const CSphColumnInfo & c ) const
	{
		return c.m_eAggrFunc!=SPH_AGGR_NONE || c.m_sName=="@groupby" || c.m_sName=="@count" || c.m_sName=="@distinct" || c.m_sName=="@groupbystr";
	}

	bool IsLess ( const CSphColumnInfo & a, const CSphColumnInfo & b ) const
	{
		bool aa = IsAggr(a);
		bool bb = IsAggr(b);
		if ( aa!=bb )
			return aa < bb;
		return a.m_iIndex < b.m_iIndex;
	}
};


static void ExtractPostlimit ( const ISphSchema & tSchema, CSphVector<const CSphColumnInfo *> & dPostlimit )
{
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = tSchema.GetAttr ( i );
		if ( tCol.m_eStage==SPH_EVAL_POSTLIMIT )
			dPostlimit.Add ( &tCol );
	}
}


static void ProcessPostlimit ( const CSphVector<const CSphColumnInfo *> & dPostlimit, int iFrom, int iTo, AggrResult_t & tRes )
{
	if ( !dPostlimit.GetLength() )
		return;

	for ( int i=iFrom; i<iTo; i++ )
	{
		CSphMatch & tMatch = tRes.m_dMatches[i];
		// remote match (tag highest bit 1) == everything is already computed
		if ( tMatch.m_iTag & 0x80000000 )
			continue;

		ARRAY_FOREACH ( j, dPostlimit )
		{
			const CSphColumnInfo * pCol = dPostlimit[j];
			assert ( pCol->m_pExpr );

			// OPTIMIZE? only if the tag did not change?
			pCol->m_pExpr->Command ( SPH_EXPR_SET_MVA_POOL, &tRes.m_dTag2Pools [ tMatch.m_iTag ] );
			pCol->m_pExpr->Command ( SPH_EXPR_SET_STRING_POOL, (void*)tRes.m_dTag2Pools [ tMatch.m_iTag ].m_pStrings );

			if ( pCol->m_eAttrType==SPH_ATTR_INTEGER )
				tMatch.SetAttr ( pCol->m_tLocator, pCol->m_pExpr->IntEval(tMatch) );
			else if ( pCol->m_eAttrType==SPH_ATTR_BIGINT )
				tMatch.SetAttr ( pCol->m_tLocator, pCol->m_pExpr->Int64Eval(tMatch) );
			else if ( pCol->m_eAttrType==SPH_ATTR_STRINGPTR )
				tMatch.SetAttr ( pCol->m_tLocator, (SphAttr_t) pCol->m_pExpr->StringEvalPacked ( tMatch ) ); // FIXME! a potential leak of *previous* value?
			else
				tMatch.SetAttrFloat ( pCol->m_tLocator, pCol->m_pExpr->Eval(tMatch) );
		}
	}
}


static void ProcessLocalPostlimit ( const CSphQuery & tQuery, AggrResult_t & tRes )
{
	bool bGotPostlimit = false;
	for ( int i=0; i<tRes.m_tSchema.GetAttrsCount() && !bGotPostlimit; i++ )
		bGotPostlimit = ( tRes.m_tSchema.GetAttr(i).m_eStage==SPH_EVAL_POSTLIMIT );

	if ( !bGotPostlimit )
		return;

	int iSetNext = 0;
	CSphVector<const CSphColumnInfo *> dPostlimit;
	ARRAY_FOREACH ( iSchema, tRes.m_dSchemas )
	{
		int iSetStart = iSetNext;
		int iSetCount = tRes.m_dMatchCounts[iSchema];
		iSetNext += iSetCount;
		assert ( iSetNext<=tRes.m_dMatches.GetLength() );

		dPostlimit.Resize ( 0 );
		ExtractPostlimit ( tRes.m_dSchemas[iSchema], dPostlimit );
		if ( !dPostlimit.GetLength() )
			continue;

		int iTo = iSetCount;
		int iOff = Max ( tQuery.m_iOffset, tQuery.m_iOuterOffset );
		int iCount = ( tQuery.m_iOuterLimit ? tQuery.m_iOuterLimit : tQuery.m_iLimit );
		iTo = Max ( Min ( iOff + iCount, iTo ), 0 );
		// we can't estimate limit.offset per result set
		// as matches got merged and sort next step
		int iFrom = 0;

		iFrom += iSetStart;
		iTo += iSetStart;

		ProcessPostlimit ( dPostlimit, iFrom, iTo, tRes );
	}
}


/// merges multiple result sets, remaps columns, does reorder for outer selects
/// query is only (!) non-const to tweak order vs reorder clauses
bool MinimizeAggrResult ( AggrResult_t & tRes, const CSphQuery & tQuery, bool bHaveLocals,
	const sph::StringSet& hExtraColumns, CSphQueryProfile * pProfiler,
	const CSphFilterSettings * pAggrFilter, bool bForceRefItems )
{
	// sanity check
	// verify that the match counts are consistent
	int iExpected = 0;
	ARRAY_FOREACH ( i, tRes.m_dMatchCounts )
		iExpected += tRes.m_dMatchCounts[i];
	if ( iExpected!=tRes.m_dMatches.GetLength() )
	{
		tRes.m_sError.SetSprintf ( "INTERNAL ERROR: expected %d matches in combined result set, got %d",
			iExpected, tRes.m_dMatches.GetLength() );
		return false;
	}

	bool bReturnZeroCount = !tRes.m_dZeroCount.IsEmpty();
	bool bQueryFromAPI = tQuery.m_eQueryType==QUERY_API;
	bool bAgent = tQuery.m_bAgent;
	bool bUsualApi = !bAgent && bQueryFromAPI;

	// 0 matches via SphinxAPI? no fiddling with schemes is necessary
	// (and via SphinxQL, we still need to return the right schema)
	if ( bQueryFromAPI && tRes.m_dMatches.IsEmpty() )
		return true;

	// 0 result set schemes via SphinxQL? just bail
	if ( !bQueryFromAPI && tRes.m_dSchemas.IsEmpty() && !bReturnZeroCount )
		return true;

	// build a minimal schema over all the (potentially different) schemes
	// that we have in our aggregated result set
	assert ( tRes.m_dSchemas.GetLength() || bReturnZeroCount );
	if ( tRes.m_dSchemas.GetLength() )
		tRes.m_tSchema = tRes.m_dSchemas[0];

	bool bAllEqual = true;

	// FIXME? add assert ( tRes.m_tSchema==tRes.m_dSchemas[0] );
	for ( int i=1; i<tRes.m_dSchemas.GetLength(); i++ )
		if ( !MinimizeSchema ( tRes.m_tSchema, tRes.m_dSchemas[i] ) )
			bAllEqual = false;

	const CSphVector<CSphQueryItem> & dQueryItems = ( tQuery.m_bFacet || tQuery.m_bFacetHead || bForceRefItems ) ? tQuery.m_dRefItems : tQuery.m_dItems;

	// build a list of select items that the query asked for
	bool bHaveExprs = false;
	CSphVector<CSphQueryItem> tExtItems;
	const CSphVector<CSphQueryItem> & tItems = ExpandAsterisk ( tRes.m_tSchema, dQueryItems, tExtItems, bQueryFromAPI, tQuery.m_bFacetHead, bHaveExprs );

	// api + index without attributes + select * case
	// can not skip aggregate filtering
	if ( bQueryFromAPI && !tItems.GetLength() && !pAggrFilter && !bHaveExprs )
	{
		tRes.FreeMatchesPtrs ( 0, bAllEqual );
		return true;
	}

	// build the final schemas!
	// ???
	CSphVector<CSphColumnInfo> dFrontend { tItems.GetLength() };

	// track select items that made it into the internal schema
	CSphVector<int> dKnownItems;

	// we use this vector to reduce amount of work in next nested loop
	// instead of looping through all dItems we FOREACH only unmapped ones
	CSphVector<int> dUnmappedItems;
	ARRAY_FOREACH ( i, tItems )
	{
		int iCol = -1;
		if ( !bQueryFromAPI && tItems[i].m_sAlias.IsEmpty() )
			iCol = tRes.m_tSchema.GetAttrIndex ( tItems[i].m_sExpr.cstr() );

		if ( iCol>=0 )
		{
			dFrontend[i].m_sName = tItems[i].m_sExpr;
			dFrontend[i].m_iIndex = iCol;
			dKnownItems.Add(i);
		} else
			dUnmappedItems.Add(i);
	}

	// ???
	for ( int iCol=0; iCol<tRes.m_tSchema.GetAttrsCount(); iCol++ )
	{
		const CSphColumnInfo & tCol = tRes.m_tSchema.GetAttr(iCol);

		assert ( !tCol.m_sName.IsEmpty() );
		bool bMagic = ( *tCol.m_sName.cstr()=='@' );

		if ( !bMagic && tCol.m_pExpr )
		{
			ARRAY_FOREACH ( j, dUnmappedItems )
				if ( tItems[ dUnmappedItems[j] ].m_sAlias==tCol.m_sName )
			{
				int k = dUnmappedItems[j];
				dFrontend[k].m_iIndex = iCol;
				dFrontend[k].m_sName = tItems[k].m_sAlias;
				dKnownItems.Add(k);
				dUnmappedItems.Remove ( j-- ); // do not skip an element next to removed one!
			}

			// FIXME?
			// really not sure if this is the right thing to do
			// but it fixes a couple queries in test_163 in compaitbility mode
			if ( bAgent && !dFrontend.Contains ( bind ( &CSphColumnInfo::m_sName ), tCol.m_sName ) )
			{
				CSphColumnInfo & t = dFrontend.Add();
				t.m_iIndex = iCol;
				t.m_sName = tCol.m_sName;
			}
		} else if ( bMagic && ( tCol.m_pExpr || bUsualApi ) )
		{
			ARRAY_FOREACH ( j, dUnmappedItems )
				if ( tCol.m_sName==GetMagicSchemaName ( tItems[ dUnmappedItems[j] ].m_sExpr ) )
			{
				int k = dUnmappedItems[j];
				dFrontend[k].m_iIndex = iCol;
				dFrontend[k].m_sName = tItems[k].m_sAlias;
				dKnownItems.Add(k);
				dUnmappedItems.Remove ( j-- ); // do not skip an element next to removed one!
			}
			if ( !dFrontend.Contains ( bind ( &CSphColumnInfo::m_sName ), tCol.m_sName ) )
			{
				CSphColumnInfo & t = dFrontend.Add();
				t.m_iIndex = iCol;
				t.m_sName = tCol.m_sName;
			}
		} else
		{
			bool bAdded = false;
			ARRAY_FOREACH ( j, dUnmappedItems )
			{
				int k = dUnmappedItems[j];
				const CSphQueryItem & t = tItems[k];
				if ( ( tCol.m_sName==GetMagicSchemaName ( t.m_sExpr ) && t.m_eAggrFunc==SPH_AGGR_NONE )
						|| ( t.m_sAlias==tCol.m_sName &&
							( tRes.m_tSchema.GetAttrIndex ( GetMagicSchemaName ( t.m_sExpr ) )==-1 || t.m_eAggrFunc!=SPH_AGGR_NONE ) ) )
				{
					// tricky bit about naming
					//
					// in master mode, we can just use the alias or expression or whatever
					// the data will be fetched using the locator anyway, column name does not matter anymore
					//
					// in agent mode, however, we need to keep the original column names in our response
					// otherwise, queries like SELECT col1 c, count(*) c FROM dist will fail on master
					// because it won't be able to identify the count(*) aggregate by its name
					dFrontend[k].m_iIndex = iCol;
					dFrontend[k].m_sName = bAgent
						? tCol.m_sName
						: ( tItems[k].m_sAlias.IsEmpty()
							? tItems[k].m_sExpr
							: tItems[k].m_sAlias );
					dKnownItems.Add(k);
					bAdded = true;
					dUnmappedItems.Remove ( j-- ); // do not skip an element next to removed one!
				}
			}

			// column was not found in the select list directly
			// however we might need it anyway because of a non-NULL extra-schema
			// (extra-schema is additinal set of columns came from right side of query
			// when you perform 'select a from index order by b', the 'b' is not displayed, but need for sorting,
			// so extra-schema in the case will contain 'b').
			// bMagic condition added for @groupbystr in the agent mode
			if ( !bAdded && bAgent && ( hExtraColumns[tCol.m_sName] || !bHaveLocals || bMagic ) )
			{
				CSphColumnInfo & t = dFrontend.Add();
				t.m_iIndex = iCol;
				t.m_sName = tCol.m_sName;
			}
		}
	}

	// sanity check
	// verify that we actually have all the queried select items
	assert ( dUnmappedItems.IsEmpty() || ( dUnmappedItems.GetLength()==1 && tItems [ dUnmappedItems[0] ].m_sExpr=="id" ) );
	dKnownItems.Sort();
	ARRAY_FOREACH ( i, tItems )
		if ( !dKnownItems.BinarySearch(i) && tItems[i].m_sExpr!="id" )
	{
		tRes.m_sError.SetSprintf ( "INTERNAL ERROR: column '%s/%s' not found in result set schema",
			tItems[i].m_sExpr.cstr(), tItems[i].m_sAlias.cstr() );
		return false;
	}

	// finalize the frontend schema columns
	// we kept indexes into internal schema there, now use them to lookup and copy column data
	ARRAY_FOREACH ( i, dFrontend )
	{
		CSphColumnInfo & d = dFrontend[i];
		if ( d.m_iIndex<0 && tItems[i].m_sExpr=="id" )
		{
			// handle one single exception to select-list-to-minimized-schema mapping loop above
			// "id" is not a part of a schema, so we gotta handle it here
			d.m_tLocator.m_bDynamic = true;
			d.m_sName = tItems[i].m_sAlias.IsEmpty() ? "id" : tItems[i].m_sAlias;
			d.m_eAttrType = SPH_ATTR_BIGINT;
			d.m_tLocator.m_iBitOffset = -8*(int)sizeof(SphDocID_t); // FIXME? move to locator method?
			d.m_tLocator.m_iBitCount = 8*sizeof(SphDocID_t);
		} else
		{
			// everything else MUST have been mapped in the loop just above
			// so use GetAttr(), and let it assert() at will
			const CSphColumnInfo & s = tRes.m_tSchema.GetAttr ( d.m_iIndex );
			d.m_tLocator = s.m_tLocator;
			d.m_eAttrType = s.m_eAttrType;
			d.m_eAggrFunc = s.m_eAggrFunc; // for a sort loop just below
		}
		d.m_iIndex = i; // to make the aggr sort loop just below stable
	}

	// tricky bit
	// in agents only, push aggregated columns, if any, to the end
	// for that, sort the schema by (is_aggregate ASC, column_index ASC)
	if ( bAgent )
		dFrontend.Sort ( AggregateColumnSort_fn() );

	// tricky bit
	// in purely distributed case, all schemas are received from the wire, and miss aggregate functions info
	// thus, we need to re-assign that info
	if ( !bHaveLocals )
		RecoverAggregateFunctions ( tQuery, tRes );

	// if there's more than one result set,
	// we now have to merge and order all the matches
	// this is a good time to apply outer order clause, too
	if ( tRes.m_iSuccesses>1 || pAggrFilter )
	{
		ESphSortOrder eQuerySort = ( tQuery.m_sOuterOrderBy.IsEmpty() ? SPH_SORT_RELEVANCE : SPH_SORT_EXTENDED );
		CSphQuery tQueryCopy = tQuery;
		// got outer order? gotta do a couple things
		if ( tQueryCopy.m_bHasOuter )
		{
			// first, temporarily patch up sorting clause and max_matches (we will restore them later)
			Swap ( tQueryCopy.m_sOuterOrderBy, tQueryCopy.m_sGroupBy.IsEmpty() ? tQueryCopy.m_sSortBy : tQueryCopy.m_sGroupSortBy );
			Swap ( eQuerySort, tQueryCopy.m_eSort );
			tQueryCopy.m_iMaxMatches *= tRes.m_dMatchCounts.GetLength();
			// FIXME? probably not right; 20 shards with by 300 matches might be too much
			// but propagating too small inner max_matches to the outer is not right either

			// second, apply inner limit now, before (!) reordering
			int iOut = 0;
			int iSetStart = 0;
			for ( int & iCurMatches : tRes.m_dMatchCounts )
			{
				assert ( tQueryCopy.m_iLimit>=0 );
				int iLimitedMatches = Min ( tQueryCopy.m_iLimit, iCurMatches );
				for ( int i=0; i<iLimitedMatches; ++i )
					Swap ( tRes.m_dMatches[iOut++], tRes.m_dMatches[iSetStart+i] );
				iSetStart += iCurMatches;
				iCurMatches = iLimitedMatches;
			}
			tRes.ClampMatches ( iOut, bAllEqual ); // false means no common schema; true == use common schema
		}

		// so we need to bring matches to the schema that the *sorter* wants
		// so we need to create the sorter before conversion
		//
		// create queue
		// at this point, we do not need to compute anything; it all must be here
		SphQueueSettings_t tQueueSettings ( tQueryCopy, tRes.m_tSchema, tRes.m_sError );
		tQueueSettings.m_bComputeItems = false;
		tQueueSettings.m_pAggrFilter = pAggrFilter;
		CSphScopedPtr<ISphMatchSorter> pSorter  ( sphCreateQueue ( tQueueSettings ) );

		// restore outer order related patches, or it screws up the query log
		if ( tQueryCopy.m_bHasOuter )
		{
			Swap ( tQueryCopy.m_sOuterOrderBy, tQueryCopy.m_sGroupBy.IsEmpty() ? tQueryCopy.m_sSortBy : tQueryCopy.m_sGroupSortBy );
			Swap ( eQuerySort, tQueryCopy.m_eSort );
			tQueryCopy.m_iMaxMatches /= tRes.m_dMatchCounts.GetLength();
		}

		if ( !pSorter )
			return false;

		// reset bAllEqual flag if sorter makes new attributes
		if ( bAllEqual )
		{
			// at first we count already existed internal attributes
			// then check if sorter makes more
			CSphVector<SphStringSorterRemap_t> dRemapAttr;
			sphSortGetStringRemap ( tRes.m_tSchema, tRes.m_tSchema, dRemapAttr );
			int iRemapCount = dRemapAttr.GetLength();
			sphSortGetStringRemap ( *pSorter->GetSchema(), tRes.m_tSchema, dRemapAttr );

			bAllEqual = ( dRemapAttr.GetLength()<=iRemapCount );
		}

		// sorter expects this

		// just doing tRes.m_tSchema = *pSorter->GetSchema() won't work here
		// because pSorter->GetSchema() may already contain a pointer to tRes.m_tSchema as m_pIndexSchema
		// that's why we explicitly copy a CSphRsetSchema to a plain CSphSchema and move it to tRes.m_tSchema
		CSphSchema tSchemaCopy;
		tSchemaCopy = *pSorter->GetSchema();
		tRes.m_tSchema = std::move ( tSchemaCopy );

		// convert all matches to sorter schema - at least to manage all static to dynamic
		if ( !bAllEqual )
		{
			// post-limit stuff first
			if ( bHaveLocals )
			{
				CSphScopedProfile tProf ( pProfiler, SPH_QSTATE_EVAL_POST );
				ProcessLocalPostlimit ( tQueryCopy, tRes );
			}

			RemapResult ( &tRes.m_tSchema, &tRes );
		}

		// do the sort work!
		tRes.m_iTotalMatches -= KillAllDupes ( pSorter.Ptr(), tRes );
	}

	// apply outer order clause to single result set
	// (multiple combined sets just got reordered above)
	// apply inner limit first
	if ( tRes.m_iSuccesses==1 && tQuery.m_bHasOuter )
		tRes.ClampMatches ( tQuery.m_iLimit, bAllEqual );

	if ( tRes.m_iSuccesses==1 && tQuery.m_bHasOuter && !tQuery.m_sOuterOrderBy.IsEmpty() )
	{
		// reorder (aka outer order)
		ESphSortFunc eFunc;
		GenericMatchSort_fn tReorder;

		ESortClauseParseResult eRes = sphParseSortClause ( &tQuery, tQuery.m_sOuterOrderBy.cstr(),
			tRes.m_tSchema, eFunc, tReorder, tRes.m_sError );
		if ( eRes==SORT_CLAUSE_RANDOM )
			tRes.m_sError = "order by rand() not supported in outer select";
		if ( eRes!=SORT_CLAUSE_OK )
			return false;

		assert ( eFunc==FUNC_GENERIC2 || eFunc==FUNC_GENERIC3 || eFunc==FUNC_GENERIC4 || eFunc==FUNC_GENERIC5 );
		sphSort ( tRes.m_dMatches.Begin(), tRes.m_dMatches.GetLength(), tReorder, MatchSortAccessor_t() );
	}

	// compute post-limit stuff
	if ( bAllEqual && bHaveLocals )
	{
		CSphScopedProfile ( pProfiler, SPH_QSTATE_EVAL_POST );

		CSphVector<const CSphColumnInfo *> dPostlimit;
		ExtractPostlimit ( tRes.m_tSchema, dPostlimit );

		// post compute matches only between offset - limit
		// however at agent we can't estimate limit.offset at master merged result set
		// but master don't provide offset to agents only offset+limit as limit
		// so computing all matches up to iiner.limit \ outer.limit
		int iTo = tRes.m_dMatches.GetLength();
		int iOff = Max ( tQuery.m_iOffset, tQuery.m_iOuterOffset );
		int iCount = ( tQuery.m_iOuterLimit ? tQuery.m_iOuterLimit : tQuery.m_iLimit );
		iTo = Max ( Min ( iOff + iCount, iTo ), 0 );
		int iFrom = Min ( iOff, iTo );

		ProcessPostlimit ( dPostlimit, iFrom, iTo, tRes );
	}

	// remap groupby() and aliased groupby() to @groupbystr or string attribute
	const CSphColumnInfo * p = tRes.m_tSchema.GetAttr ( "@groupbystr" );
	if ( !p )
	{
		// try string attribute (multiple group-by still displays hashes)
		if ( !tQuery.m_sGroupBy.IsEmpty() )
		{
			p = tRes.m_tSchema.GetAttr ( tQuery.m_sGroupBy.cstr() );
			if ( p && p->m_eAttrType!=SPH_ATTR_STRINGPTR )
				p = NULL;
		}
	}
	if ( p )
	{
		ARRAY_FOREACH ( i, dFrontend )
		{
			CSphColumnInfo & d = dFrontend[i];
			if ( d.m_sName=="groupby()" )
			{
				d.m_tLocator = p->m_tLocator;
				d.m_eAttrType = p->m_eAttrType;
				d.m_eAggrFunc = p->m_eAggrFunc;
			}
		}

		// check aliases too
		ARRAY_FOREACH ( j, dQueryItems )
		{
			const CSphQueryItem & tItem = dQueryItems[j];
			if ( tItem.m_sExpr=="groupby()" )
			{
				ARRAY_FOREACH ( i, dFrontend )
				{
					CSphColumnInfo & d = dFrontend[i];
					if ( d.m_sName==tItem.m_sAlias )
					{
						d.m_tLocator = p->m_tLocator;
						d.m_eAttrType = p->m_eAttrType;
						d.m_eAggrFunc = p->m_eAggrFunc;
					}
				}
			}
		}
	}

	// facets
	if ( tQuery.m_bFacet || tQuery.m_bFacetHead )
	{
		// remap MVA/JSON column to @groupby/@groupbystr in facet queries
		const CSphColumnInfo * pGroupByCol = tRes.m_tSchema.GetAttr ( "@groupbystr" );
		if ( !pGroupByCol )
			pGroupByCol = tRes.m_tSchema.GetAttr ( "@groupby" );

		if ( pGroupByCol )
		{
			ARRAY_FOREACH ( i, dFrontend )
			{
				CSphColumnInfo & d = dFrontend[i];
				ESphAttr eAttr = d.m_eAttrType;
				// checking _PTR attrs only because we should not have and non-ptr attr at this point
				if ( tQuery.m_sGroupBy==d.m_sName && ( eAttr==SPH_ATTR_UINT32SET_PTR || eAttr==SPH_ATTR_INT64SET_PTR || eAttr==SPH_ATTR_JSON_FIELD_PTR ) )
				{
					d.m_tLocator = pGroupByCol->m_tLocator;
					d.m_eAttrType = pGroupByCol->m_eAttrType;
					d.m_eAggrFunc = pGroupByCol->m_eAggrFunc;
				}
			}
		}
	}

	// all the merging and sorting is now done
	// replace the minimized matches schema with its subset, the result set schema
	tRes.m_tSchema.SwapAttrs ( dFrontend );
	return true;
}

/////////////////////////////////////////////////////////////////////////////
static int StringBinary2Number ( const char * sStr, int iLen )
{
	if ( !sStr || !iLen )
		return 0;

	char sBuf[64];
	if ( (int)(sizeof ( sBuf )-1 )<iLen )
		iLen = sizeof ( sBuf )-1;
	memcpy ( sBuf, sStr, iLen );
	sBuf[iLen] = '\0';

	return atoi ( sBuf );
}

static bool SnippetTransformPassageMacros ( CSphString & sSrc, CSphString & sPost )
{
	const char sPassageMacro[] = "%PASSAGE_ID%";

	const char * sPass = NULL;
	if ( !sSrc.IsEmpty() )
		sPass = strstr ( sSrc.cstr(), sPassageMacro );

	if ( !sPass )
		return false;

	int iSrcLen = sSrc.Length();
	int iPassLen = sizeof ( sPassageMacro ) - 1;
	int iTailLen = iSrcLen - iPassLen - ( sPass - sSrc.cstr() );

	// copy tail
	if ( iTailLen )
		sPost.SetBinary ( sPass+iPassLen, iTailLen );

	CSphString sPre;
	sPre.SetBinary ( sSrc.cstr(), sPass - sSrc.cstr() );
	sSrc.Swap ( sPre );

	return true;
}

ESphSpz GetPassageBoundary ( const CSphString & );
/// suddenly, searchd-level expression function!
struct Expr_Snippet_c : public ISphStringExpr
{
	CSphRefcountedPtr<ISphExpr>					m_pArgs;
	CSphRefcountedPtr<ISphExpr>					m_pText;
	CSphIndex *					m_pIndex;
	SnippetContext_t			m_tCtx;
	mutable ExcerptQuery_t		m_tHighlight;
	CSphQueryProfile *			m_pProfiler;

	explicit Expr_Snippet_c ( ISphExpr * pArglist, CSphIndex * pIndex, CSphQueryProfile * pProfiler, CSphString & sError )
		: m_pArgs ( pArglist )
		, m_pIndex ( pIndex )
		, m_pProfiler ( pProfiler )
	{
		SafeAddRef ( pArglist );
		assert ( pArglist->IsArglist() );
		m_pText = pArglist->GetArg(0);
		SafeAddRef ( m_pText );

		CSphMatch tDummy;
		char * pWords;
		assert ( !pArglist->GetArg(1)->IsDataPtrAttr() ); // aware of memleaks potentially caused by StringEval()
		pArglist->GetArg(1)->StringEval ( tDummy, (const BYTE**)&pWords );
		m_tHighlight.m_sWords = pWords;

		for ( int i = 2; i < pArglist->GetNumArgs(); i++ )
		{
			assert ( !pArglist->GetArg(i)->IsDataPtrAttr() ); // aware of memleaks potentially caused by StringEval()
			int iLen = pArglist->GetArg(i)->StringEval ( tDummy, (const BYTE**)&pWords );
			if ( !pWords || !iLen )
				continue;

			CSphString sArgs;
			sArgs.SetBinary ( pWords, iLen );
			char * pWords = const_cast<char *> ( sArgs.cstr() );

			const char * sEnd = pWords + iLen;
			while ( pWords<sEnd && *pWords && sphIsSpace ( *pWords ) )	pWords++;
			char * szOption = pWords;
			while ( pWords<sEnd && *pWords && sphIsAlpha ( *pWords ) )	pWords++;
			char * szOptEnd = pWords;
			while ( pWords<sEnd && *pWords && sphIsSpace ( *pWords ) )	pWords++;

			if ( *pWords++!='=' )
			{
				sError.SetSprintf ( "Error parsing SNIPPET options: %s", pWords );
				return;
			}

			*szOptEnd = '\0';
			while ( pWords<sEnd && *pWords && sphIsSpace ( *pWords ) )	pWords++;
			char * sValue = pWords;

			if ( !*sValue )
			{
				sError.SetSprintf ( "Error parsing SNIPPET options" );
				return;
			}

			while ( pWords<sEnd && *pWords ) pWords++;
			int iStrValLen = pWords - sValue;

			if ( !strcasecmp ( szOption, "before_match" ) )					{ m_tHighlight.m_sBeforeMatch.SetBinary ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "after_match" ) )				{ m_tHighlight.m_sAfterMatch.SetBinary ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "chunk_separator" ) )			{ m_tHighlight.m_sChunkSeparator.SetBinary ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "limit" ) )					{ m_tHighlight.m_iLimit = StringBinary2Number ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "around" ) )					{ m_tHighlight.m_iAround = StringBinary2Number ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "exact_phrase" ) )			{ m_tHighlight.m_bExactPhrase = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "use_boundaries" ) )			{ m_tHighlight.m_bUseBoundaries = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "weight_order" ) )			{ m_tHighlight.m_bWeightOrder = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "query_mode" ) )				{ m_tHighlight.m_bHighlightQuery = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "force_all_words" ) )			{ m_tHighlight.m_bForceAllWords = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "limit_passages" ) )			{ m_tHighlight.m_iLimitPassages = StringBinary2Number ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "limit_words" ) )				{ m_tHighlight.m_iLimitWords = StringBinary2Number ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "start_passage_id" ) )		{ m_tHighlight.m_iPassageId = StringBinary2Number ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "load_files" ) )				{ m_tHighlight.m_uFilesMode |= ( StringBinary2Number ( sValue, iStrValLen )!=0 ? 1 : 0 ); }
			else if ( !strcasecmp ( szOption, "load_files_scattered" ) )	{ m_tHighlight.m_uFilesMode |= ( StringBinary2Number ( sValue, iStrValLen )!=0 ? 2 : 0 ); }
			else if ( !strcasecmp ( szOption, "html_strip_mode" ) )			{ m_tHighlight.m_sStripMode.SetBinary ( sValue, iStrValLen ); }
			else if ( !strcasecmp ( szOption, "allow_empty" ) )				{ m_tHighlight.m_bAllowEmpty= ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "emit_zones" ) )				{ m_tHighlight.m_bEmitZones = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "force_passages" ) )			{ m_tHighlight.m_bForcePassages = ( StringBinary2Number ( sValue, iStrValLen )!=0 ); }
			else if ( !strcasecmp ( szOption, "passage_boundary" ) )
			{
				CSphString sBuf;
				sBuf.SetBinary ( sValue, iStrValLen );
				m_tHighlight.m_ePassageSPZ = GetPassageBoundary (sBuf);
			}
			else if ( !strcasecmp ( szOption, "json_query" ) )
			{
				m_tHighlight.m_bJsonQuery = ( StringBinary2Number ( sValue, iStrValLen )!=0 );
				if ( m_tHighlight.m_bJsonQuery )
					m_tHighlight.m_bHighlightQuery = true;
			} else
			{
				CSphString sBuf;
				sBuf.SetBinary ( sValue, iStrValLen );
				sError.SetSprintf ( "Unknown SNIPPET option: %s=%s", szOption, sBuf.cstr() );
				return;
			}
		}

		m_tHighlight.m_bHasBeforePassageMacro = SnippetTransformPassageMacros ( m_tHighlight.m_sBeforeMatch, m_tHighlight.m_sBeforeMatchPassage );
		m_tHighlight.m_bHasAfterPassageMacro = SnippetTransformPassageMacros ( m_tHighlight.m_sAfterMatch, m_tHighlight.m_sAfterMatchPassage );

		m_tCtx.Setup ( m_pIndex, m_tHighlight, sError );
	}

	int StringEval ( const CSphMatch & tMatch, const BYTE ** ppStr ) const final
	{
		CSphScopedProfile ( m_pProfiler, SPH_QSTATE_SNIPPET );

		*ppStr = nullptr;

		const BYTE * sSource = nullptr;
		int iLen = m_pText->StringEval ( tMatch, &sSource );

		if ( !iLen )
		{
			if ( m_pText->IsDataPtrAttr() )
				SafeDeleteArray ( sSource );
			return 0;
		}

		// for dynamic strings (eg. fetched by UDFs), just take ownership
		// for static ones (eg. attributes), treat as binary (ie. mind that
		// the trailing zero is NOT guaranteed), and copy them
		if ( m_pText->IsDataPtrAttr() )
			m_tHighlight.m_sSource.Adopt ( (char**)&sSource );
		else
			m_tHighlight.m_sSource.SetBinary ( (const char*)sSource, iLen );

		// FIXME! fill in all the missing options; use consthash?
		m_tCtx.BuildExcerpt ( m_tHighlight, m_pIndex );
		
		if ( !m_tHighlight.m_bJsonQuery )
		{
			assert ( m_tHighlight.m_dRes.IsEmpty() || m_tHighlight.m_dRes.Last()=='\0' );
			int iResultLength = m_tHighlight.m_dRes.GetLength();
			*ppStr = m_tHighlight.m_dRes.LeakData();
			// skip trailing zero
			return ( iResultLength ? iResultLength-1 : 0 );
		} else
			return PackSnippets ( m_tHighlight.m_dRes, m_tHighlight.m_dSeparators, m_tHighlight.m_sChunkSeparator.Length(), ppStr );
	}

	bool IsDataPtrAttr () const final { return true; }

	void FixupLocator ( const ISphSchema * pOldSchema, const ISphSchema * pNewSchema ) override
	{
		if ( m_pText )
			m_pText->FixupLocator ( pOldSchema, pNewSchema );
	}

	void Command ( ESphExprCommand eCmd, void * pArg ) override
	{
		if ( eCmd!=SPH_EXPR_SET_STRING_POOL )
			return;

		if ( m_pArgs )
			m_pArgs->Command ( SPH_EXPR_SET_STRING_POOL, pArg );
		if ( m_pText )
			m_pText->Command ( SPH_EXPR_SET_STRING_POOL, pArg );
	}

	uint64_t GetHash ( const ISphSchema &, uint64_t, bool & ) override
	{
		assert ( 0 && "no snippets in filters" );
		return 0;
	}
};


/// searchd expression hook
/// needed to implement functions that are builtin for searchd,
/// but can not be builtin in the generic expression engine itself,
/// like SNIPPET() function that must know about indexes, tokenizers, etc
struct ExprHook_t : public ISphExprHook
{
	static const int HOOK_SNIPPET = 1;
	CSphIndex * m_pIndex = nullptr; /// BLOODY HACK
	CSphQueryProfile * m_pProfiler = nullptr;

	int IsKnownIdent ( const char * ) final
	{
		return -1;
	}

	int IsKnownFunc ( const char * sFunc ) final
	{
		if ( !strcasecmp ( sFunc, "SNIPPET" ) )
			return HOOK_SNIPPET;
		return -1;
	}

	ISphExpr * CreateNode ( int DEBUGARG(iID), ISphExpr * pLeft, ESphEvalStage * pEvalStage, CSphString & sError ) final
	{
		assert ( iID==HOOK_SNIPPET );
		if ( pEvalStage )
			*pEvalStage = SPH_EVAL_POSTLIMIT;

		CSphRefcountedPtr<ISphExpr> pRes { new Expr_Snippet_c ( pLeft, m_pIndex, m_pProfiler, sError ) };
		if ( !sError.IsEmpty () )
			pRes = nullptr;

		return pRes.Leak();
	}

	ESphAttr GetIdentType ( int ) final
	{
		assert ( 0 );
		return SPH_ATTR_NONE;
	}

	ESphAttr GetReturnType ( int DEBUGARG(iID), const CSphVector<ESphAttr> & dArgs, bool, CSphString & sError ) final
	{
		assert ( iID==HOOK_SNIPPET );
		if ( dArgs.GetLength()<2 )
		{
			sError = "SNIPPET() requires 2 or more arguments";
			return SPH_ATTR_NONE;
		}
		if ( dArgs[0]!=SPH_ATTR_STRINGPTR && dArgs[0]!=SPH_ATTR_STRING )
		{
			sError = "1st argument to SNIPPET() must be a string expression";
			return SPH_ATTR_NONE;
		}

		for ( int i = 1; i < dArgs.GetLength(); i++ )
			if ( dArgs[i]!=SPH_ATTR_STRING )
			{
				sError.SetSprintf ( "%d argument to SNIPPET() must be a constant string", i );
				return SPH_ATTR_NONE;
			}

		return SPH_ATTR_STRINGPTR;
	}

	void CheckEnter ( int ) final {}
	void CheckExit ( int ) final {}
};


struct LocalIndex_t
{
	CSphString	m_sName;
	CSphString	m_sParentIndex;
	int			m_iOrderTag = 0;
	int			m_iWeight = 1;
	int64_t		m_iMass = 0;
	bool		m_bKillBreak = false;
};


struct QueryStat_t
{
	uint64_t	m_uQueryTime = 0;
	uint64_t	m_uFoundRows = 0;
	int			m_iSuccesses = 0;
};


struct StatsPerQuery_t
{
	CSphVector<QueryStat_t> m_dStats;
};


struct DistrServedByAgent_t : StatsPerQuery_t
{
	CSphString						m_sIndex;
	CSphVector<int>					m_dAgentIds;
	StrVec_t						m_dLocalNames;
};

/// manage collection of pre-locked indexes (to avoid multilocks)
/// Get(name) - returns an index from collection.
/// AddRLocked(name) - add local idx to collection, read-locking it.
/// AddUnmanaged(name,pidx) - add pre-locked idx, to make it available with GetIndex()
/// d-tr unlocks indexes, added with AddRLockedIndex.
class LockedCollection_c : public ISphNoncopyable
{
	SmallStringHash_T<ServedDescRPtr_c*> m_hUsed;
	SmallStringHash_T<const ServedDesc_t*> 	m_hUnmanaged;
public:
	~LockedCollection_c();
	bool AddRLocked ( const CSphString &sName );
	void AddUnmanaged ( const CSphString &sName, const ServedDesc_t * pIdx );

	const ServedDesc_t * Get ( const CSphString &sName ) const;
};


struct LocalSearch_t
{
	int					m_iLocal;
	ISphMatchSorter **	m_ppSorters;
	CSphQueryResult **	m_ppResults;
	bool				m_bResult;
	int64_t				m_iMass;
};

class SearchHandler_c : public ISphSearchHandler
{
	friend void LocalSearchThreadFunc ( void * pArg );

public:
									SearchHandler_c ( int iQueries, const QueryParser_i * pParser, QueryType_e eQueryType, bool bMaster, const ThdDesc_t & tThd );
									~SearchHandler_c() final;

	void							RunQueries () final;					///< run all queries, get all results
	void							RunUpdates ( const CSphQuery & tQuery, const CSphString & sIndex, CSphAttrUpdateEx * pUpdates ); ///< run Update command instead of Search
	void							RunDeletes ( const CSphQuery & tQuery, const CSphString & sIndex, CSphString * pErrors, CSphVector<SphDocID_t> * pDelDocs );
	void							SetQuery ( int iQuery, const CSphQuery & tQuery, ISphTableFunc * pTableFunc ) final;
	void							SetQueryParser ( const QueryParser_i * pParser );
	void							SetQueryType ( QueryType_e eQueryType );
	void							SetProfile ( CSphQueryProfile * pProfile ) final;
	AggrResult_t *					GetResult ( int iResult ) final { return m_dResults.Begin() + iResult; }
	void							SetFederatedUser () { m_bFederatedUser = true; }
	void 							RunLocalSearchMT ( LocalSearch_t &dWork, ThreadLocal_t &tThd );

public:
	CSphVector<CSphQuery>			m_dQueries;						///< queries which i need to search
	CSphVector<AggrResult_t>		m_dResults;						///< results which i obtained
	CSphVector<StatsPerQuery_t>		m_dQueryIndexStats;				///< statistics for current query
	CSphVector<SearchFailuresLog_c>	m_dFailuresSet;					///< failure logs for each query
	CSphVector < CSphVector<int64_t> >	m_dAgentTimes;				///< per-agent time stats
	LockedCollection_c				m_dLocked;						/// locked indexes
	CSphFixedVector<ISphTableFunc *>	m_dTables;
	const ThdDesc_t &				m_tThd;

protected:
	void							RunSubset ( int iStart, int iEnd );	///< run queries against index(es) from first query in the subset
	void							RunLocalSearches();
	void							RunLocalSearchesParallel ();
	bool							AllowsMulti ( int iStart, int iEnd ) const;
	void							SetupLocalDF ( int iStart, int iEnd );

	int								m_iStart = 0;			///< subset start
	int								m_iEnd = 0;				///< subset end
	bool							m_bMultiQueue = false;	///< whether current subset is subject to multi-queue optimization
	bool							m_bFacetQueue = false;	///< whether current subset is subject to facet-queue optimization
	CSphVector<LocalIndex_t>		m_dLocal;				///< local indexes for the current subset
	mutable CSphVector<CSphVector<StrVec_t> > m_dExtraSchemas; 	///< the extra attrs for agents. One vec per thread
	CSphAttrUpdateEx *				m_pUpdates = nullptr;	///< holder for updates
	CSphVector<SphDocID_t> *		m_pDelDocs = nullptr;	///< this query is for deleting

	CSphQueryProfile *				m_pProfile = nullptr;
	QueryType_e						m_eQueryType {QUERY_API}; ///< queries from sphinxql require special handling
	const QueryParser_i *			m_pQueryParser;	///< parser used for queries in this handler. e.g. plain or json-style

	mutable ExprHook_t				m_tHook;

	SmallStringHash_T < int64_t >	m_hLocalDocs;
	int64_t							m_iTotalDocs = 0;
	bool							m_bGotLocalDF = false;
	bool							m_bMaster;
	bool							m_bFederatedUser;

	void							OnRunFinished ();

	const ServedDesc_t *			GetIndex ( const CSphString &sName ) const;
private:
	bool							CheckMultiQuery ( int iStart, int iEnd ) const;
	bool							RLockInvokedIndexes();
	void PrepareQueryIndexes ( VectorAgentConn_t &dRemotes, CSphVector<DistrServedByAgent_t> &dDistrServedByAgent );
	void							UniqLocals ();
	void							RunActionQuery ( const CSphQuery & tQuery, const CSphString & sIndex, CSphString * pErrors ); ///< run delete/update

};


ISphSearchHandler * sphCreateSearchHandler ( int iQueries, const QueryParser_i * pQueryParser, QueryType_e eQueryType, bool bMaster, const ThdDesc_t & tThd )
{
	return new SearchHandler_c ( iQueries, pQueryParser, eQueryType, bMaster, tThd );
}


SearchHandler_c::SearchHandler_c ( int iQueries, const QueryParser_i * pQueryParser, QueryType_e eQueryType, bool bMaster, const ThdDesc_t & tThd )
	: m_dTables ( iQueries )
	, m_tThd ( tThd )
{
	m_dQueries.Resize ( iQueries );
	m_dResults.Resize ( iQueries );
	m_dFailuresSet.Resize ( iQueries );
	m_dExtraSchemas.Resize ( iQueries );
	m_dAgentTimes.Resize ( iQueries );
	m_bMaster = bMaster;
	m_bFederatedUser = false;
	ARRAY_FOREACH ( i, m_dTables )
		m_dTables[i] = nullptr;

	SetQueryParser ( pQueryParser );
	SetQueryType ( eQueryType );
}


SearchHandler_c::~SearchHandler_c ()
{
	SafeDelete ( m_pQueryParser );
	ARRAY_FOREACH ( i, m_dTables )
		SafeDelete ( m_dTables[i] );
}


void SearchHandler_c::SetQueryParser ( const QueryParser_i * pParser )
{
	m_pQueryParser = pParser;
	for ( auto & dQuery : m_dQueries )
		dQuery.m_pQueryParser = pParser;
}


void SearchHandler_c::SetQueryType ( QueryType_e eQueryType )
{
	m_eQueryType = eQueryType;
	for ( auto & dQuery : m_dQueries )
		dQuery.m_eQueryType = eQueryType;
}

LockedCollection_c::~LockedCollection_c()
{
	for ( m_hUsed.IterateStart (); m_hUsed.IterateNext(); )
		SafeDelete ( m_hUsed.IterateGet () );
}

bool LockedCollection_c::AddRLocked ( const CSphString & sName )
{
	if ( m_hUsed.Exists ( sName ) || m_hUnmanaged.Exists ( sName ) )
		return true;

	auto pServed = GetServed ( sName );
	if ( !pServed )
		return false;

	m_hUsed.Add ( new ServedDescRPtr_c ( pServed ), sName );
	return true;
}

void LockedCollection_c::AddUnmanaged ( const CSphString &sName, const ServedDesc_t * pIdx )
{
	if ( m_hUsed.Exists ( sName ) || m_hUnmanaged.Exists ( sName ) )
		return;

	m_hUnmanaged.Add ( pIdx, sName );
}


const ServedDesc_t * LockedCollection_c::Get ( const CSphString & sName ) const
{
	auto * pppIndex = m_hUsed ( sName );
	if ( pppIndex )
		return **pppIndex;

	auto * ppUnmanaged = m_hUnmanaged ( sName );
	if ( ppUnmanaged )
		return *ppUnmanaged;

	return nullptr;
}


void SearchHandler_c::RunUpdates ( const CSphQuery & tQuery, const CSphString & sIndex,	CSphAttrUpdateEx * pUpdates )
{
	m_pUpdates = pUpdates;
	RunActionQuery ( tQuery, sIndex, pUpdates->m_pError );
}

void SearchHandler_c::RunDeletes ( const CSphQuery &tQuery, const CSphString &sIndex, CSphString * pErrors, CSphVector<SphDocID_t> * pDelDocs )
{
	m_pDelDocs = pDelDocs;
	RunActionQuery ( tQuery, sIndex, pErrors );
}

void SearchHandler_c::RunActionQuery ( const CSphQuery & tQuery, const CSphString & sIndex, CSphString * pErrors )
{
	SetQuery ( 0, tQuery, nullptr );
	m_dQueries[0].m_sIndexes = sIndex;
	m_dResults[0].m_dTag2Pools.Resize ( 1 );
	m_dLocal.Add ().m_sName = sIndex;

	CheckQuery ( tQuery, *pErrors );
	if ( !pErrors->IsEmpty() )
		return;

	int64_t tmLocal = -sphMicroTimer();

	RunLocalSearches();
	tmLocal += sphMicroTimer();

	OnRunFinished();

	CSphQueryResult & tRes = m_dResults[0];

	tRes.m_iOffset = tQuery.m_iOffset;
	tRes.m_iCount = Max ( Min ( tQuery.m_iLimit, tRes.m_dMatches.GetLength()-tQuery.m_iOffset ), 0 );

	tRes.m_iQueryTime += (int)(tmLocal/1000);
	tRes.m_iCpuTime += tmLocal;

	if ( !tRes.m_iSuccesses )
	{
		StringBuilder_c sFailures;
		m_dFailuresSet[0].BuildReport ( sFailures );
		sFailures.MoveTo ( *pErrors );

	} else if ( !tRes.m_sError.IsEmpty() )
	{
		StringBuilder_c sFailures;
		m_dFailuresSet[0].BuildReport ( sFailures );
		sFailures.MoveTo ( tRes.m_sWarning ); // FIXME!!! commit warnings too
	}

	const CSphIOStats & tIO = tRes.m_tIOStats;

	++g_tStats.m_iQueries;
	g_tStats.m_iQueryTime += tmLocal;
	g_tStats.m_iQueryCpuTime += tmLocal;
	g_tStats.m_iDiskReads += tIO.m_iReadOps;
	g_tStats.m_iDiskReadTime += tIO.m_iReadTime;
	g_tStats.m_iDiskReadBytes += tIO.m_iReadBytes;

	LogQuery ( m_dQueries[0], m_dResults[0], m_dAgentTimes[0], m_tThd.m_iConnID );
}

void SearchHandler_c::SetQuery ( int iQuery, const CSphQuery & tQuery, ISphTableFunc * pTableFunc )
{
	m_dQueries[iQuery] = tQuery;
	m_dQueries[iQuery].m_pQueryParser = m_pQueryParser;
	m_dQueries[iQuery].m_eQueryType = m_eQueryType;
	m_dTables[iQuery] = pTableFunc;
}


void SearchHandler_c::SetProfile ( CSphQueryProfile * pProfile )
{
	assert ( pProfile );
	m_pProfile = pProfile;
}


void SearchHandler_c::RunQueries()
{
	// batch queries to same index(es)
	// or work each query separately if indexes are different

	int iStart = 0, iEnd = 0;
	ARRAY_FOREACH ( i, m_dQueries )
	{
		if ( m_dQueries[i].m_sIndexes!=m_dQueries[iStart].m_sIndexes )
		{
			RunSubset ( iStart, iEnd );
			iStart = i;
		}
		iEnd = i;
	}
	RunSubset ( iStart, iEnd );
	ARRAY_FOREACH ( i, m_dQueries )
		LogQuery ( m_dQueries[i], m_dResults[i], m_dAgentTimes[i], m_tThd.m_iConnID );
	OnRunFinished();
}


// final fixup
void SearchHandler_c::OnRunFinished()
{
	for ( auto & dResult : m_dResults )
		dResult.m_iMatches = dResult.m_dMatches.GetLength();
}


/// return cpu time, in microseconds
int64_t sphCpuTimer ()
{
#ifdef HAVE_CLOCK_GETTIME
	if ( !g_bCpuStats )
		return 0;

#if defined (CLOCK_THREAD_CPUTIME_ID)
// CPU time (user+sys), Linux style, current thread
#define LOC_CLOCK CLOCK_THREAD_CPUTIME_ID
#elif defined(CLOCK_PROCESS_CPUTIME_ID)
// CPU time (user+sys), Linux style
#define LOC_CLOCK CLOCK_PROCESS_CPUTIME_ID
#elif defined(CLOCK_PROF)
// CPU time (user+sys), FreeBSD style
#define LOC_CLOCK CLOCK_PROF
#else
// POSIX fallback (wall time)
#define LOC_CLOCK CLOCK_REALTIME
#endif

	struct timespec tp;
	if ( clock_gettime ( LOC_CLOCK, &tp ) )
		return 0;

	return tp.tv_sec*1000000 + tp.tv_nsec/1000;
#else
	return sphMicroTimer();
#endif
}


struct LocalSearchThreadContext_t
{
	SphThread_t					m_tThd {0};
	SearchHandler_c *			m_pHandler = nullptr;
	CrashQuery_t				m_tCrashQuery;
	long						m_iSearches = 0;
	LocalSearch_t *				m_pSearches = nullptr;
	CSphAtomic *				m_pCurSearch = nullptr;
	int							m_iLocalThreadID = 0;

	void LocalSearch ()
	{
		SphCrashLogger_c::SetLastQuery ( m_tCrashQuery );
		ThreadLocal_t tThd ( m_pHandler->m_tThd );
		tThd.m_tDesc.m_iCookie = m_iLocalThreadID;
		for ( long iCurSearch = ( *m_pCurSearch )++; iCurSearch<m_iSearches; iCurSearch = ( *m_pCurSearch )++ )
			m_pHandler->RunLocalSearchMT ( m_pSearches[iCurSearch], tThd );
	}
};

void LocalSearchThreadFunc ( void * pArg )
{
	( ( LocalSearchThreadContext_t * ) pArg )->LocalSearch ();
}


static void MergeWordStats ( CSphQueryResultMeta & tDstResult,
	const SmallStringHash_T<CSphQueryResultMeta::WordStat_t> & hSrc,
	SearchFailuresLog_c * pLog, const char * sIndex, const char * sParentIndex )
{
	assert ( pLog );

	if ( !tDstResult.m_hWordStats.GetLength() )
	{
		// nothing has been set yet; just copy
		tDstResult.m_hWordStats = hSrc;
		return;
	}

	CSphString sDiff;
	SphWordStatChecker_t tDiff;
	tDiff.Set ( hSrc );
	tDiff.DumpDiffer ( tDstResult.m_hWordStats, NULL, sDiff );
	if ( !sDiff.IsEmpty() )
		pLog->SubmitEx ( sIndex, sParentIndex, "%s", sDiff.cstr() );

	hSrc.IterateStart();
	while ( hSrc.IterateNext() )
	{
		const CSphQueryResultMeta::WordStat_t & tSrcStat = hSrc.IterateGet();
		tDstResult.AddStat ( hSrc.IterateGetKey(), tSrcStat.m_iDocs, tSrcStat.m_iHits );
	}
}


static int64_t CalcPredictedTimeMsec ( const CSphQueryResult & tRes )
{
	assert ( tRes.m_bHasPrediction );

	int64_t iNanoResult = int64_t(g_iPredictorCostSkip)*tRes.m_tStats.m_iSkips+
		g_iPredictorCostDoc*tRes.m_tStats.m_iFetchedDocs+
		g_iPredictorCostHit*tRes.m_tStats.m_iFetchedHits+
		g_iPredictorCostMatch*tRes.m_iTotalMatches;

	return iNanoResult/1000000;
}


static void FlattenToRes ( ISphMatchSorter * pSorter, AggrResult_t & tRes, int iTag )
{
	assert ( pSorter );

	if ( pSorter->GetLength() )
	{
		CSphSchema & tNewSchema = tRes.m_dSchemas.Add();
		tNewSchema = *pSorter->GetSchema();
		PoolPtrs_t & tPoolPtrs = tRes.m_dTag2Pools[iTag];
		assert ( !tPoolPtrs.m_pMva && !tPoolPtrs.m_pStrings );
		tPoolPtrs.m_pMva = tRes.m_pMva;
		tPoolPtrs.m_pStrings = tRes.m_pStrings;
		tPoolPtrs.m_bArenaProhibit = tRes.m_bArenaProhibit;
		int iCopied = sphFlattenQueue ( pSorter, &tRes, iTag );
		tRes.m_dMatchCounts.Add ( iCopied );

		// clean up for next index search
		tRes.m_pMva = nullptr;
		tRes.m_pStrings = nullptr;
		tRes.m_bArenaProhibit = false;
	}
}


static void RemoveMissedRows ( AggrResult_t & tRes )
{
	if ( !tRes.m_dMatchCounts.Last() )
		return;

	CSphMatch * pStart = tRes.m_dMatches.Begin() + tRes.m_dMatches.GetLength() - tRes.m_dMatchCounts.Last();
	CSphMatch * pSrc = pStart;
	CSphMatch * pDst = pStart;
	const CSphMatch * pEnd = tRes.m_dMatches.Begin() + tRes.m_dMatches.GetLength();

	while ( pSrc<pEnd )
	{
		if ( !pSrc->m_pStatic )
		{
			tRes.m_tSchema.FreeDataPtrs ( pSrc );
			pSrc++;
			continue;
		}

		Swap ( *pSrc, *pDst );
		pSrc++;
		pDst++;
	}

	tRes.m_dMatchCounts.Last() = pDst - pStart;
	tRes.m_dMatches.Resize ( pDst - tRes.m_dMatches.Begin() );
}


void SearchHandler_c::RunLocalSearchesParallel()
{
	int64_t tmLocal = sphMicroTimer();

	// setup local searches
	const int iQueries = m_iEnd-m_iStart+1;
	CSphVector<LocalSearch_t> dWorks ( m_dLocal.GetLength() );
	CSphVector<CSphQueryResult> dResults ( m_dLocal.GetLength()*iQueries );
	CSphVector<ISphMatchSorter*> dSorters ( m_dLocal.GetLength()*iQueries );
	CSphVector<CSphQueryResult*> dResultPtrs ( m_dLocal.GetLength()*iQueries );

	ARRAY_FOREACH ( i, dResultPtrs )
		dResultPtrs[i] = &dResults[i];

	ARRAY_FOREACH ( i, m_dLocal )
	{
		dWorks[i].m_iLocal = i;
		dWorks[i].m_iMass = -m_dLocal[i].m_iMass; // minus for reverse order
		dWorks[i].m_ppSorters = &dSorters [ i*iQueries ];
		dWorks[i].m_ppResults = &dResultPtrs [ i*iQueries ];
	}
	dWorks.Sort ( bind ( &LocalSearch_t::m_iMass ) );

	// setup threads
	int iThreads = Min ( g_iDistThreads, dWorks.GetLength () );
	CSphVector<LocalSearchThreadContext_t> dThreads ( iThreads );
	CrashQuery_t tCrashQuery = SphCrashLogger_c::GetQuery(); // transfer query info for crash logger to new thread

	// reserve extra schema set for each thread
	// (that is simpler than ping-pong with mutex on each addition)
	assert ( m_dQueries.GetLength() == m_dExtraSchemas.GetLength() );
	ARRAY_FOREACH ( i, m_dQueries )
		if ( m_dQueries[i].m_bAgent && m_dExtraSchemas[i].IsEmpty() )
			m_dExtraSchemas[i].Resize ( iThreads );

	// fire searcher threads
	CSphAtomic iaCursor;
	ARRAY_FOREACH ( i, dThreads )
	{
		auto& t = dThreads[i];
		t.m_iLocalThreadID = i;
		t.m_pHandler = this;
		t.m_tCrashQuery = tCrashQuery;
		t.m_pCurSearch = &iaCursor;
		t.m_iSearches = dWorks.GetLength();
		t.m_pSearches = dWorks.Begin();
		SphCrashLogger_c::ThreadCreate ( &t.m_tThd, LocalSearchThreadFunc, (void*)&t, false, "LocalSearch" ); // FIXME! check result
	}

	// wait for them to complete
	for ( auto &t : dThreads )
		sphThreadJoin ( &t.m_tThd );

	int iTotalSuccesses = 0;

	// now merge the results
	ARRAY_FOREACH ( iLocal, dWorks )
	{
		bool bResult = dWorks[iLocal].m_bResult;
		const char * sLocal = m_dLocal[iLocal].m_sName.cstr();
		const char * sParentIndex = m_dLocal[iLocal].m_sParentIndex.cstr();
		int iOrderTag = m_dLocal[iLocal].m_iOrderTag;

		if ( !bResult )
		{
			// failed
			for ( int iQuery=m_iStart; iQuery<=m_iEnd; iQuery++ )
			{
				int iResultIndex = iLocal*iQueries;
				if ( !m_bMultiQueue )
					iResultIndex += iQuery - m_iStart;
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, dResults[iResultIndex].m_sError.cstr() );
			}
			continue;
		}

		// multi-query succeeded
		for ( int iQuery=m_iStart; iQuery<=m_iEnd; ++iQuery )
		{
			// base result set index
			// in multi-queue case, the only (!) result set actually filled with meta info
			// in non-multi-queue case, just a first index, we fix it below
			int iResultIndex = iLocal*iQueries;

			// current sorter ALWAYS resides at this index, in all cases
			// (current as in sorter for iQuery-th query against iLocal-th index)
			int iSorterIndex = iResultIndex + iQuery - m_iStart;

			if ( !m_bMultiQueue )
			{
				// non-multi-queue case
				// means that we have mere 1:1 mapping between results and sorters
				// so let's adjust result set index
				iResultIndex = iSorterIndex;

			} else if ( dResults[iResultIndex].m_iMultiplier==-1 )
			{
				// multi-queue case
				// need to additionally check per-query failures of MultiQueryEx
				// those are reported through multiplier
				// note that iSorterIndex just below is NOT a typo
				// separate errors still go into separate result sets
				// even though regular meta does not
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, dResults[iSorterIndex].m_sError.cstr() );
				continue;
			}

			// no sorter, no fun
			ISphMatchSorter * pSorter = dSorters [ iSorterIndex ];
			if ( !pSorter )
				continue;

			// this one seems OK
			AggrResult_t & tRes = m_dResults[iQuery];
			CSphQueryResult & tRaw = dResults[iResultIndex];

			++iTotalSuccesses;

			++tRes.m_iSuccesses;
			tRes.m_iTotalMatches += pSorter->GetTotalCount();

			tRes.m_pMva = tRaw.m_pMva;
			tRes.m_pStrings = tRaw.m_pStrings;
			tRes.m_bArenaProhibit = tRaw.m_bArenaProhibit;
			MergeWordStats ( tRes, tRaw.m_hWordStats, &m_dFailuresSet[iQuery], sLocal, sParentIndex );

			tRes.m_bHasPrediction |= tRaw.m_bHasPrediction;
			tRes.m_iMultiplier = m_bMultiQueue ? iQueries : 1;
			tRes.m_iCpuTime += tRaw.m_iCpuTime / tRes.m_iMultiplier;
			tRes.m_tIOStats.Add ( tRaw.m_tIOStats );
			if ( tRaw.m_bHasPrediction )
			{
				tRes.m_tStats.Add ( tRaw.m_tStats );
				tRes.m_iPredictedTime = CalcPredictedTimeMsec ( tRes );
			}
			if ( tRaw.m_iBadRows )
				tRes.m_sWarning.SetSprintf ( "query result is inaccurate because of " INT64_FMT " missed documents", tRaw.m_iBadRows );

			m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart].m_iSuccesses = 1;
			m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart].m_uFoundRows = pSorter->GetTotalCount();

			// extract matches from sorter
			FlattenToRes ( pSorter, tRes, iOrderTag+iQuery-m_iStart );

			if ( tRaw.m_iBadRows )
				RemoveMissedRows ( tRes );

			// take over the schema from sorter, it doesn't need it anymore
			tRes.m_tSchema = *pSorter->GetSchema();

			if ( !tRaw.m_sWarning.IsEmpty() )
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, tRaw.m_sWarning.cstr() );
		}
	}

	for ( auto & pSorter : dSorters )
		SafeDelete ( pSorter );

	// update our wall time for every result set
	tmLocal = sphMicroTimer() - tmLocal;
	for ( int iQuery=m_iStart; iQuery<=m_iEnd; iQuery++ )
		m_dResults[iQuery].m_iQueryTime += (int)( tmLocal/1000 );

	ARRAY_FOREACH ( iLocal, dWorks )
		for ( int iQuery=m_iStart; iQuery<=m_iEnd; ++iQuery )
		{
			QueryStat_t & tStat = m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart];
			if ( tStat.m_iSuccesses )
				tStat.m_uQueryTime = (int)( tmLocal/1000/iTotalSuccesses );
		}
}

int64_t sphCpuTimer();

// invoked from MT searches. So, must be MT-aware!
void SearchHandler_c::RunLocalSearchMT ( LocalSearch_t &dWork, ThreadLocal_t &tThd )
{

	// FIXME!!! handle different proto
	tThd.m_tDesc.SetThreadInfo ( R"(api-search query="%s" comment="%s" index="%s")"
								 , m_dQueries[m_iStart].m_sQuery.scstr (), m_dQueries[m_iStart].m_sComment.scstr ()
								 , m_dLocal[dWork.m_iLocal].m_sName.scstr () );
	tThd.m_tDesc.m_tmStart = sphMicroTimer ();

	int64_t iCpuTime = -sphCpuTimer ();;
	const int iQueries = m_iEnd-m_iStart+1;
	dWork.m_bResult = false;
	auto ppResults = dWork.m_ppResults;
	auto ppSorters = dWork.m_ppSorters;
	auto iLocal = dWork.m_iLocal;
	auto * pServed = m_dLocked.Get ( m_dLocal[dWork.m_iLocal].m_sName );
	if ( !pServed )
	{
		// FIXME! submit a failure?
		return;
	}
	assert ( pServed->m_pIndex );

	// create sorters
	int iValidSorters = 0;
	DWORD uFactorFlags = SPH_FACTOR_DISABLE;
	for ( int i=0; i<iQueries; ++i )
	{
		CSphString & sError = ppResults[i]->m_sError;
		const CSphQuery & tQuery = m_dQueries[i+m_iStart];

		m_tHook.m_pIndex = pServed->m_pIndex;
		SphQueueSettings_t tQueueSettings ( tQuery, pServed->m_pIndex->GetMatchSchema(), sError, m_pProfile );
		tQueueSettings.m_bComputeItems = true;
		if ( tQuery.m_bAgent )
		{
			assert ( m_dExtraSchemas[i + m_iStart].GetLength ()>tThd.m_tDesc.m_iCookie );
			tQueueSettings.m_pExtra = m_dExtraSchemas[i + m_iStart].begin () + tThd.m_tDesc.m_iCookie;
		}

		tQueueSettings.m_pUpdate = m_pUpdates;
		tQueueSettings.m_pCollection = m_pDelDocs;
		tQueueSettings.m_pHook = &m_tHook;

		ppSorters[i] = sphCreateQueue ( tQueueSettings );

		uFactorFlags |= tQueueSettings.m_uPackedFactorFlags;

		if ( ppSorters[i] )
			iValidSorters++;

		// can't use multi-query for sorter with string attribute at group by or sort
		if ( ppSorters[i] && m_bMultiQueue )
			m_bMultiQueue = ppSorters[i]->CanMulti();
	}
	if ( !iValidSorters )
		return;

	// setup kill-lists
	KillListVector dKillist;
	for ( int i=iLocal+1; i<m_dLocal.GetLength(); ++i )
	{
		if ( m_dLocal[i].m_bKillBreak )
			break;

		auto * pKillListIndex = m_dLocked.Get ( m_dLocal[i].m_sName );
		if ( !pKillListIndex )
			continue;

		if ( pKillListIndex->m_pIndex->GetKillListSize() )
		{
			auto & tElem = dKillist.Add ();
			tElem.m_pBegin = pKillListIndex->m_pIndex->GetKillList();
			tElem.m_iLen = pKillListIndex->m_pIndex->GetKillListSize();
		}
	}

	int iIndexWeight = m_dLocal[iLocal].m_iWeight;

	// do the query
	CSphMultiQueryArgs tMultiArgs ( dKillist, iIndexWeight );
	tMultiArgs.m_uPackedFactorFlags = uFactorFlags;
	if ( m_bGotLocalDF )
	{
		tMultiArgs.m_bLocalDF = true;
		tMultiArgs.m_pLocalDocs = &m_hLocalDocs;
		tMultiArgs.m_iTotalDocs = m_iTotalDocs;
	}

	ppResults[0]->m_tIOStats.Start();
	if ( m_bMultiQueue )
		dWork.m_bResult = pServed->m_pIndex->MultiQuery ( &m_dQueries[m_iStart], ppResults[0], iQueries, ppSorters, tMultiArgs );
	else
		dWork.m_bResult = pServed->m_pIndex->MultiQueryEx ( iQueries, &m_dQueries[m_iStart], ppResults, ppSorters, tMultiArgs );
	ppResults[0]->m_tIOStats.Stop();

	iCpuTime += sphCpuTimer();
	for ( int i=0; i<iQueries; ++i )
		ppResults[i]->m_iCpuTime = iCpuTime;
}


void SearchHandler_c::RunLocalSearches()
{
	m_dQueryIndexStats.Resize ( m_dLocal.GetLength () );
	for ( auto & dQueryIndexStats : m_dQueryIndexStats )
		dQueryIndexStats.m_dStats.Resize ( m_iEnd-m_iStart+1 );

	if ( g_iDistThreads>1 && m_dLocal.GetLength()>1 )
	{
		RunLocalSearchesParallel();
		return;
	}

	ARRAY_FOREACH ( iLocal, m_dLocal )
	{
		const LocalIndex_t &dLocal = m_dLocal [iLocal];
		const char * sLocal = dLocal.m_sName.cstr();
		const char * sParentIndex = dLocal.m_sParentIndex.cstr();
		int iOrderTag = dLocal.m_iOrderTag;
		int iIndexWeight = dLocal.m_iWeight;

		const auto * pServed = m_dLocked.Get ( sLocal );
		if ( !pServed )
		{
			if ( sParentIndex )
				for ( int i=m_iStart; i<=m_iEnd; ++i )
					m_dFailuresSet[i].SubmitEx ( sParentIndex, nullptr, "local index %s missing", sLocal );

			continue;
		}

		assert ( pServed->m_pIndex );

		// create sorters
		CSphVector<ISphMatchSorter*> dSorters ( m_iEnd-m_iStart+1 );
		dSorters.ZeroMem ();

		DWORD uTotalFactorFlags = SPH_FACTOR_DISABLE;
		int iValidSorters = 0;
		for ( int iQuery=m_iStart; iQuery<=m_iEnd; ++iQuery )
		{
			CSphString sError;
			CSphQuery & tQuery = m_dQueries[iQuery];

			// create queue
			m_tHook.m_pIndex = pServed->m_pIndex;
			SphQueueSettings_t tQueueSettings ( tQuery, pServed->m_pIndex->GetMatchSchema(), sError, m_pProfile );
			tQueueSettings.m_bComputeItems = true;
			if ( tQuery.m_bAgent )
			{
				if ( m_dExtraSchemas[iQuery].IsEmpty() )
					m_dExtraSchemas[iQuery].Add();
				tQueueSettings.m_pExtra = m_dExtraSchemas[iQuery].begin();
			}
			tQueueSettings.m_pUpdate = m_pUpdates;
			tQueueSettings.m_pCollection = m_pDelDocs;
			tQueueSettings.m_pHook = &m_tHook;

			ISphMatchSorter * pSorter = sphCreateQueue ( tQueueSettings );

			uTotalFactorFlags |= tQueueSettings.m_uPackedFactorFlags;
			tQuery.m_bZSlist = tQueueSettings.m_bZonespanlist;
			if ( !pSorter )
			{
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, sError.cstr() );
				continue;
			}

			if ( m_bMultiQueue )
			{
				// can't use multi-query for sorter with string attribute at group by or sort
				m_bMultiQueue = pSorter->CanMulti();

				if ( !m_bMultiQueue )
					m_bFacetQueue = false;
			}

			if ( !sError.IsEmpty() )
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, sError.cstr() );

			dSorters[iQuery-m_iStart] = pSorter;
			++iValidSorters;
		}
		if ( !iValidSorters )
			continue;

		// if sorter schemes have dynamic part, its lengths should be the same for queries to be optimized
		const ISphMatchSorter * pLastMulti = dSorters[0];
		for ( int i=1; i<dSorters.GetLength() && m_bMultiQueue; ++i )
		{
			if ( !dSorters[i] )
				continue;

			if ( !pLastMulti )
			{
				pLastMulti = dSorters[i];
				continue;
			}

			assert ( pLastMulti && dSorters[i] );
			m_bMultiQueue = pLastMulti->GetSchema()->GetDynamicSize()==dSorters[i]->GetSchema()->GetDynamicSize();
		}

		// facets, sanity check for json fields (can't be multi-queried yet)
		for ( int i=1; i<dSorters.GetLength() && m_bFacetQueue; ++i )
		{
			if ( !dSorters[i] )
				continue;
			for ( int j=0; j<dSorters[i]->GetSchema()->GetAttrsCount(); j++ )
				if ( dSorters[i]->GetSchema()->GetAttr(j).m_eAttrType==SPH_ATTR_JSON_FIELD )
				{
					m_bMultiQueue = m_bFacetQueue = false;
					break;
				}
		}

		if ( m_bFacetQueue )
			m_bMultiQueue = true;

		// me shortcuts
		AggrResult_t tStats;

		// set kill-list
		KillListVector dKillist;
		for ( int i=iLocal+1; i<m_dLocal.GetLength(); ++i )
		{
			if ( m_dLocal[i].m_bKillBreak )
				break;

			const auto * pKillListIndex = m_dLocked.Get ( m_dLocal[i].m_sName );
			if ( !pKillListIndex )
				continue;

			if ( pKillListIndex->m_pIndex->GetKillListSize() )
			{
				KillListTrait_t & tElem = dKillist.Add ();
				tElem.m_pBegin = pKillListIndex->m_pIndex->GetKillList();
				tElem.m_iLen = pKillListIndex->m_pIndex->GetKillListSize();
			}
		}

		// do the query
		CSphMultiQueryArgs tMultiArgs ( dKillist, iIndexWeight );
		tMultiArgs.m_uPackedFactorFlags = uTotalFactorFlags;
		if ( m_bGotLocalDF )
		{
			tMultiArgs.m_bLocalDF = true;
			tMultiArgs.m_pLocalDocs = &m_hLocalDocs;
			tMultiArgs.m_iTotalDocs = m_iTotalDocs;
		}

		bool bResult = false;
		if ( m_bMultiQueue )
		{
			tStats.m_tIOStats.Start();
			bResult = pServed->m_pIndex->MultiQuery ( &m_dQueries[m_iStart], &tStats, dSorters.GetLength(), dSorters.Begin(), tMultiArgs );
			tStats.m_tIOStats.Stop();
		} else
		{
			CSphVector<CSphQueryResult*> dResults ( m_dResults.GetLength() );
			ARRAY_FOREACH ( i, m_dResults )
			{
				dResults[i] = &m_dResults[i];
				dResults[i]->m_pMva = nullptr;
				dResults[i]->m_pStrings = nullptr;
			}

			dResults[m_iStart]->m_tIOStats.Start();
			bResult = pServed->m_pIndex->MultiQueryEx ( dSorters.GetLength(), &m_dQueries[m_iStart], &dResults[m_iStart], &dSorters[0], tMultiArgs );
			dResults[m_iStart]->m_tIOStats.Stop();
		}

		// handle results
		if ( !bResult )
		{
			// failed, submit local (if not empty) or global error string
			for ( int iQuery=m_iStart; iQuery<=m_iEnd; iQuery++ )
				m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, tStats.m_sError.IsEmpty()
					? m_dResults [ m_bMultiQueue ? m_iStart : iQuery ].m_sError.cstr()
					: tStats.m_sError.cstr() );
		} else
		{
			// multi-query succeeded
			for ( int iQuery=m_iStart; iQuery<=m_iEnd; ++iQuery )
			{
				// but some of the sorters could had failed at "create sorter" stage
				ISphMatchSorter * pSorter = dSorters [ iQuery-m_iStart ];
				if ( !pSorter )
					continue;

				// this one seems OK
				AggrResult_t & tRes = m_dResults[iQuery];

				int64_t iBadRows = m_bMultiQueue ? tStats.m_iBadRows : tRes.m_iBadRows;
				if ( iBadRows )
					tRes.m_sWarning.SetSprintf ( "query result is inaccurate because of " INT64_FMT " missed documents", iBadRows );

				int iQTimeForStats = tRes.m_iQueryTime;

				// multi-queue only returned one result set meta, so we need to replicate it
				if ( m_bMultiQueue )
				{
					// these times will be overridden below, but let's be clean
					tRes.m_iQueryTime += tStats.m_iQueryTime / ( m_iEnd-m_iStart+1 );
					tRes.m_iCpuTime += tStats.m_iCpuTime / ( m_iEnd-m_iStart+1 );
					tRes.m_tIOStats.Add ( tStats.m_tIOStats );
					tRes.m_pMva = tStats.m_pMva;
					tRes.m_pStrings = tStats.m_pStrings;
					tRes.m_bArenaProhibit = tStats.m_bArenaProhibit;
					MergeWordStats ( tRes, tStats.m_hWordStats, &m_dFailuresSet[iQuery], sLocal, sParentIndex );
					tRes.m_iMultiplier = m_iEnd-m_iStart+1;
					iQTimeForStats = tStats.m_iQueryTime / ( m_iEnd-m_iStart+1 );
				} else if ( tRes.m_iMultiplier==-1 )
				{
					m_dFailuresSet[iQuery].Submit ( sLocal, sParentIndex, tRes.m_sError.cstr() );
					continue;
				}

				tRes.m_iSuccesses++;
				// lets do this schema copy just once
				tRes.m_tSchema = *pSorter->GetSchema();
				tRes.m_iTotalMatches += pSorter->GetTotalCount();
				tRes.m_iPredictedTime = tRes.m_bHasPrediction ? CalcPredictedTimeMsec ( tRes ) : 0;

				m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart].m_iSuccesses = 1;
				m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart].m_uQueryTime = iQTimeForStats;
				m_dQueryIndexStats[iLocal].m_dStats[iQuery-m_iStart].m_uFoundRows = pSorter->GetTotalCount();

				// extract matches from sorter
				FlattenToRes ( pSorter, tRes, iOrderTag+iQuery-m_iStart );

				if ( iBadRows )
					RemoveMissedRows ( tRes );
			}
		}

		// cleanup sorters
		for ( auto &pSorter : dSorters )
			SafeDelete ( pSorter );
	}
}


// check expressions into a query to make sure that it's ready for multi query optimization
bool SearchHandler_c::AllowsMulti ( int iStart, int iEnd ) const
{
	// in some cases the same select list allows queries to be multi query optimized
	// but we need to check dynamic parts size equality and we do it later in RunLocalSearches()
	const CSphVector<CSphQueryItem> & tFirstQueryItems = m_dQueries [ iStart ].m_dItems;
	bool bItemsSameLen = true;
	for ( int i=iStart+1; i<=iEnd && bItemsSameLen; i++ )
		bItemsSameLen = ( tFirstQueryItems.GetLength()==m_dQueries[i].m_dItems.GetLength() );
	if ( bItemsSameLen )
	{
		bool bSameItems = true;
		ARRAY_FOREACH_COND ( i, tFirstQueryItems, bSameItems )
		{
			const CSphQueryItem & tItem1 = tFirstQueryItems[i];
			for ( int j=iStart+1; j<=iEnd && bSameItems; j++ )
			{
				const CSphQueryItem & tItem2 = m_dQueries[j].m_dItems[i];
				bSameItems = tItem1.m_sExpr==tItem2.m_sExpr && tItem1.m_eAggrFunc==tItem2.m_eAggrFunc;
			}
		}

		if ( bSameItems )
			return true;
	}

	// if select lists do not contain any expressions we can optimize queries too
	for ( const auto & dLocal : m_dLocal )
	{
		const auto * pServedIndex = m_dLocked.Get ( dLocal.m_sName );

		// check that it exists
		if ( !pServedIndex )
			continue;

		const CSphSchema & tSchema = pServedIndex->m_pIndex->GetMatchSchema();
		for ( int i=iStart; i<=iEnd; ++i )
			if ( sphHasExpressions ( m_dQueries[i], tSchema ) )
				return false;
	}
	return true;
}


struct IndexSettings_t
{
	uint64_t	m_uHash;
	int			m_iLocal;
};

void SearchHandler_c::SetupLocalDF ( int iStart, int iEnd )
{
	if ( m_dLocal.GetLength()<2 )
		return;

	if ( m_pProfile )
		m_pProfile->Switch ( SPH_QSTATE_LOCAL_DF );

	bool bGlobalIDF = true;
	ARRAY_FOREACH_COND ( i, m_dLocal, bGlobalIDF )
	{
		ServedDescRPtr_c pDesc ( GetServed( m_dLocal[i].m_sName ) );
		bGlobalIDF = ( pDesc && !pDesc->m_sGlobalIDFPath.IsEmpty () );
	}
	// bail out on all indexes with global idf set
	if ( bGlobalIDF )
		return;

	bool bOnlyNoneRanker = true;
	bool bOnlyFullScan = true;
	bool bHasLocalDF = false;
	for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
	{
		const CSphQuery & tQuery = m_dQueries[iQuery];

		bOnlyFullScan &= tQuery.m_sQuery.IsEmpty();
		bHasLocalDF |= tQuery.m_bLocalDF;
		if ( !tQuery.m_sQuery.IsEmpty() && tQuery.m_bLocalDF )
			bOnlyNoneRanker &= ( tQuery.m_eRanker==SPH_RANK_NONE );
	}
	// bail out queries: full-scan, ranker=none, local_idf=0
	if ( bOnlyFullScan || bOnlyNoneRanker || !bHasLocalDF )
		return;

	CSphVector<char> dQuery ( 512 );
	dQuery.Resize ( 0 );
	for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
	{
		const CSphQuery & tQuery = m_dQueries[iQuery];
		if ( tQuery.m_sQuery.IsEmpty() || !tQuery.m_bLocalDF || tQuery.m_eRanker==SPH_RANK_NONE )
			continue;

		int iLen = tQuery.m_sQuery.Length();
		auto * pDst = dQuery.AddN ( iLen + 1 );
		memcpy ( pDst, tQuery.m_sQuery.cstr(), iLen );
		dQuery.Last() = ' '; // queries delimiter
	}
	// bail out on empty queries
	if ( !dQuery.GetLength() )
		return;

	dQuery.Add ( '\0' );

	// order indexes by settings
	CSphVector<IndexSettings_t> dLocal ( m_dLocal.GetLength() );
	dLocal.Resize ( 0 );
	ARRAY_FOREACH ( i, m_dLocal )
	{
		const auto * pIndex = m_dLocked.Get ( m_dLocal[i].m_sName );
		if ( !pIndex )
			continue;

		dLocal.Add();
		dLocal.Last().m_iLocal = i;
		// TODO: cache settingsFNV on index load
		// FIXME!!! no need to count dictionary hash
		dLocal.Last().m_uHash = pIndex->m_pIndex->GetTokenizer()->GetSettingsFNV() ^ pIndex->m_pIndex->GetDictionary()->GetSettingsFNV();
	}
	dLocal.Sort ( bind ( &IndexSettings_t::m_uHash ) );

	// gather per-term docs count
	CSphVector < CSphKeywordInfo > dKeywords;
	ARRAY_FOREACH ( i, dLocal )
	{
		int iLocalIndex = dLocal[i].m_iLocal;
		const auto * pIndex = m_dLocked.Get ( m_dLocal[iLocalIndex].m_sName );
		if ( !pIndex )
			continue;

		m_iTotalDocs += pIndex->m_pIndex->GetStats().m_iTotalDocuments;

		if ( i && dLocal[i].m_uHash==dLocal[i-1].m_uHash )
		{
			ARRAY_FOREACH ( kw, dKeywords )
				dKeywords[kw].m_iDocs = 0;

			// no need to tokenize query just fill docs count
			pIndex->m_pIndex->FillKeywords ( dKeywords );
		} else
		{
			GetKeywordsSettings_t tSettings;
			tSettings.m_bStats = true;
			dKeywords.Resize ( 0 );
			pIndex->m_pIndex->GetKeywords ( dKeywords, dQuery.Begin(), tSettings, NULL );

			// FIXME!!! move duplicate removal to GetKeywords to do less QWord setup and dict searching
			// custom uniq - got rid of word duplicates
			dKeywords.Sort ( bind ( &CSphKeywordInfo::m_sNormalized ) );
			if ( dKeywords.GetLength()>1 )
			{
				int iSrc = 1, iDst = 1;
				while ( iSrc<dKeywords.GetLength() )
				{
					if ( dKeywords[iDst-1].m_sNormalized==dKeywords[iSrc].m_sNormalized )
						iSrc++;
					else
					{
						Swap ( dKeywords[iDst], dKeywords[iSrc] );
						iDst++;
						iSrc++;
					}
				}
				dKeywords.Resize ( iDst );
			}
		}

		ARRAY_FOREACH ( j, dKeywords )
		{
			const CSphKeywordInfo & tKw = dKeywords[j];
			int64_t * pDocs = m_hLocalDocs ( tKw.m_sNormalized );
			if ( pDocs )
				*pDocs += tKw.m_iDocs;
			else
				m_hLocalDocs.Add ( tKw.m_iDocs, tKw.m_sNormalized );
		}
	}

	m_bGotLocalDF = true;
}


static int GetIndexWeight ( const CSphString& sName, const CSphVector<CSphNamedInt> & dIndexWeights, int iDefaultWeight )
{
	for ( auto& dWeight : dIndexWeights )
		if ( dWeight.m_sName==sName )
			return dWeight.m_iValue;

	// distributed index adds {'*', weight} to all agents in case it got custom weight
	if ( dIndexWeights.GetLength() && dIndexWeights.Last().m_sName=="*" )
		return dIndexWeights[0].m_iValue;

	return iDefaultWeight;
}

static uint64_t CalculateMass ( const CSphIndexStatus & dStats )
{
	return dStats.m_iNumChunks * 1000000 + dStats.m_iRamUse + dStats.m_iDiskUse * 10;
}

static uint64_t GetIndexMass ( const CSphString & sName )
{
	ServedDescRPtr_c pIdx ( GetServed ( sName ) );
	uint64_t iMass = pIdx ? pIdx->m_iMass : 0;
	return iMass;
}

struct TaggedLocalSorter_fn
{
	bool IsLess ( const LocalIndex_t & a, const LocalIndex_t & b ) const
	{
		return ( a.m_sName < b.m_sName ) || ( a.m_sName==b.m_sName && ( a.m_iOrderTag & 0x7FFFFFFF )>( b.m_iOrderTag & 0x7FFFFFFF ) );
	}
};

////////////////////////////////////////////////////////////////
// check for single-query, multi-queue optimization possibility
////////////////////////////////////////////////////////////////
bool SearchHandler_c::CheckMultiQuery ( int iStart, int iEnd ) const
{
	if (iStart>=iEnd)
		return false;

	for ( int iCheck = iStart + 1; iCheck<=iEnd; ++iCheck )
	{
		const CSphQuery &qFirst = m_dQueries[iStart];
		const CSphQuery &qCheck = m_dQueries[iCheck];

		// these parameters must be the same
		if (
			( qCheck.m_sRawQuery!=qFirst.m_sRawQuery ) || // query string
				( qCheck.m_dWeights.GetLength ()!=qFirst.m_dWeights.GetLength () ) || // weights count
				( qCheck.m_dWeights.GetLength () && memcmp ( qCheck.m_dWeights.Begin (), qFirst.m_dWeights.Begin (),
					sizeof ( qCheck.m_dWeights[0] ) * qCheck.m_dWeights.GetLength () ) ) || // weights
				( qCheck.m_eMode!=qFirst.m_eMode ) || // search mode
				( qCheck.m_eRanker!=qFirst.m_eRanker ) || // ranking mode
				( qCheck.m_dFilters.GetLength ()!=qFirst.m_dFilters.GetLength () ) || // attr filters count
				( qCheck.m_dFilterTree.GetLength ()!=qFirst.m_dFilterTree.GetLength () ) ||
				( qCheck.m_iCutoff!=qFirst.m_iCutoff ) || // cutoff
				( qCheck.m_eSort==SPH_SORT_EXPR && qFirst.m_eSort==SPH_SORT_EXPR && qCheck.m_sSortBy!=qFirst.m_sSortBy )
				|| // sort expressions
					( qCheck.m_bGeoAnchor!=qFirst.m_bGeoAnchor ) || // geodist expression
				( qCheck.m_bGeoAnchor && qFirst.m_bGeoAnchor
					&& ( qCheck.m_fGeoLatitude!=qFirst.m_fGeoLatitude
						|| qCheck.m_fGeoLongitude!=qFirst.m_fGeoLongitude ) ) ) // some geodist cases

			return false;

		// filters must be the same too
		assert ( qCheck.m_dFilters.GetLength ()==qFirst.m_dFilters.GetLength () );
		assert ( qCheck.m_dFilterTree.GetLength ()==qFirst.m_dFilterTree.GetLength () );
		ARRAY_FOREACH ( i, qCheck.m_dFilters )
		{
			if ( qCheck.m_dFilters[i]!=qFirst.m_dFilters[i] )
				return false;
		}
		ARRAY_FOREACH ( i, qCheck.m_dFilterTree )
		{
			if ( qCheck.m_dFilterTree[i]!=qFirst.m_dFilterTree[i] )
				return false;
		}
	}
	return true;
}

// lock local indexes invoked in query
// Fails if an index is absent and this is not allowed
bool SearchHandler_c::RLockInvokedIndexes()
{
	// if unexistent allowed, short flow
	if ( m_dQueries[m_iStart].m_bIgnoreNonexistentIndexes )
	{
		ARRAY_FOREACH ( i, m_dLocal )
			if ( !m_dLocked.AddRLocked ( m_dLocal[i].m_sName ) )
				m_dLocal.Remove ( i-- );
		return true;
	}

	// _build the list of non-existent
	StringBuilder_c sFailed (", ");
	for ( const auto & dLocal : m_dLocal )
		if ( !m_dLocked.AddRLocked ( dLocal.m_sName ) )
			sFailed << dLocal.m_sName;

	// no absent indexes, viola!
	if ( sFailed.IsEmpty ())
		return true;

	// report failed for each result
	for ( auto i = m_iStart; i<=m_iEnd; ++i )
		m_dResults[i].m_sError.SetSprintf ( "unknown local index(es) '%s' in search request", sFailed.cstr() );

	return false;
}

void SearchHandler_c::UniqLocals()
{
	m_dLocal.Sort ( TaggedLocalSorter_fn () );
	int iSrc = 1, iDst = 1;
	while ( iSrc<m_dLocal.GetLength () )
	{
		if ( m_dLocal[iDst - 1].m_sName==m_dLocal[iSrc].m_sName )
			++iSrc;
		else
			m_dLocal[iDst++] = m_dLocal[iSrc++];
	}
	m_dLocal.Resize ( iDst );
	m_dLocal.Sort ( bind ( &LocalIndex_t::m_iOrderTag ) ); // keep initial order of locals
}

// one ore more queries against one and same set of indexes
void SearchHandler_c::RunSubset ( int iStart, int iEnd )
{
	m_iStart = iStart;
	m_iEnd = iEnd;

	// all my stats
	int64_t tmSubset = sphMicroTimer();
	int64_t tmLocal = 0;
	int64_t tmCpu = sphCpuTimer ();

	ESphQueryState eOldState = SPH_QSTATE_UNKNOWN;
	if ( m_pProfile )
		eOldState = m_pProfile->m_eState;


	// prepare for descent
	const CSphQuery & tFirst = m_dQueries[iStart];

	for ( int iRes=iStart; iRes<=iEnd; ++iRes )
		m_dResults[iRes].m_iSuccesses = 0;

	if ( iStart==iEnd && m_pProfile )
	{
		m_dResults[iStart].m_pProfile = m_pProfile;
		m_tHook.m_pProfiler = m_pProfile;
	}

	// check for facets
	m_bFacetQueue = iEnd>iStart;
	for ( int iCheck=iStart+1; iCheck<=iEnd && m_bFacetQueue; ++iCheck )
		if ( !m_dQueries[iCheck].m_bFacet )
			m_bFacetQueue = false;

	m_bMultiQueue = m_bFacetQueue || CheckMultiQuery ( iStart, iEnd );

	////////////////////////////
	// build local indexes list
	////////////////////////////

	VecRefPtrsAgentConn_t dRemotes;
	CSphVector<DistrServedByAgent_t> dDistrServedByAgent;
	int iDivideLimits = 1;
	int iTagsCount = 0, iTagStep = iEnd - iStart + 1;
	m_dLocal.Reset ();

	// they're all local, build the list
	if ( tFirst.m_sIndexes=="*" )
	{
		// search through all local indexes
		for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
		{
			if ( !it.Get() )
				continue;
			auto &dLocal = m_dLocal.Add ();
			dLocal.m_sName = it.GetName ();
			dLocal.m_iOrderTag = iTagsCount;
			dLocal.m_iWeight = GetIndexWeight ( it.GetName (), tFirst.m_dIndexWeights, 1 );
			dLocal.m_iMass = ServedDescRPtr_c ( it.Get() )->m_iMass;
			iTagsCount += iTagStep;
		}
	} else
	{
		StrVec_t dIdxNames;
		// search through specified local indexes
		ParseIndexList ( tFirst.m_sIndexes, dIdxNames );

		int iDistCount = 0;
		bool bDivideRemote = false;

		for ( const auto& sIndex : dIdxNames )
		{
			auto pDist = GetDistr ( sIndex );
			if ( !pDist )
			{
				auto &dLocal = m_dLocal.Add ();
				dLocal.m_sName = sIndex;
				dLocal.m_iOrderTag = iTagsCount;
				dLocal.m_iWeight = GetIndexWeight ( sIndex, tFirst.m_dIndexWeights, 1 );
				dLocal.m_iMass = GetIndexMass ( sIndex );
				iTagsCount += iTagStep;
			} else
			{
				++iDistCount;
				int iWeight = GetIndexWeight ( sIndex, tFirst.m_dIndexWeights, -1 );
				auto & tDistrStat = dDistrServedByAgent.Add();
				tDistrStat.m_sIndex = sIndex;
				tDistrStat.m_dStats.Resize ( iEnd-iStart+1 );
				tDistrStat.m_dStats.ZeroMem();
				for ( auto * pAgent : pDist->m_dAgents )
				{
					tDistrStat.m_dAgentIds.Add ( dRemotes.GetLength() );
					auto * pConn = new AgentConn_t;
					pConn->SetMultiAgent ( sIndex, pAgent );
					pConn->m_iStoreTag = iTagsCount;
					pConn->m_iWeight = iWeight;
					pConn->m_iMyConnectTimeout = pDist->m_iAgentConnectTimeout;
					pConn->m_iMyQueryTimeout = pDist->m_iAgentQueryTimeout;
					dRemotes.Add ( pConn );
					iTagsCount += iTagStep;
				}

				ARRAY_FOREACH ( j, pDist->m_dLocal )
				{
					const CSphString& sLocalAgent = pDist->m_dLocal[j];
					tDistrStat.m_dLocalNames.Add ( sLocalAgent );
					auto &dLocal = m_dLocal.Add ();
					dLocal.m_sName = sLocalAgent;
					dLocal.m_iOrderTag = iTagsCount;
					if ( iWeight!=-1 )
						dLocal.m_iWeight = iWeight;
					dLocal.m_iMass = GetIndexMass ( sLocalAgent );
					dLocal.m_sParentIndex = sIndex;
					iTagsCount += iTagStep;
					if ( pDist->m_dKillBreak.GetBits() && pDist->m_dKillBreak.BitGet ( j ) )
						dLocal.m_bKillBreak = true;
				}

				bDivideRemote |= pDist->m_bDivideRemoteRanges;
			}
		}

		// set remote divider
		if ( bDivideRemote )
		{
			if ( iDistCount==1 )
				iDivideLimits = dRemotes.GetLength();
			else
			{
				for ( int iRes=iStart; iRes<=iEnd; ++iRes )
					m_dResults[iRes].m_sWarning.SetSprintf ( "distribute multi-index query '%s' doesn't support divide_remote_ranges", tFirst.m_sIndexes.cstr() );
			}
		}

		// eliminate local dupes that come from distributed indexes
		if ( !dRemotes.IsEmpty () && !m_dLocal.IsEmpty() )
			UniqLocals();
	}

	if ( !RLockInvokedIndexes () )
		return;

	// at this point m_dLocal contains list of valid local indexes (i.e., existing ones),
	// and these indexes are also rlocked and available by calling m_dLocked.Get()

	// sanity check
	if ( dRemotes.IsEmpty() && m_dLocal.IsEmpty() )
	{
		const char * sIndexType = ( dRemotes.GetLength() ? "indexes" : "local indexes" );
		for ( int iRes=iStart; iRes<=iEnd; ++iRes )
			m_dResults[iRes].m_sError.SetSprintf ( "no enabled %s to search", sIndexType );
		return;
	}

	m_dQueryIndexStats.Resize ( m_dLocal.GetLength () );

	for ( int iRes=iStart; iRes<=iEnd; ++iRes )
		m_dResults[iRes].m_dTag2Pools.Resize ( iTagsCount );

 	// select lists must have no expressions
	if ( m_bMultiQueue )
		m_bMultiQueue = AllowsMulti ( iStart, iEnd );

	assert ( !m_bFacetQueue || AllowsMulti ( iStart, iEnd ) );
	if ( !m_bMultiQueue )
		m_bFacetQueue = false;

	///////////////////////////////////////////////////////////
	// main query loop (with multiple retries for distributed)
	///////////////////////////////////////////////////////////

	// connect to remote agents and query them, if required
	CSphScopedPtr<SearchRequestBuilder_t> tReqBuilder { nullptr };
	CSphRefcountedPtr<IRemoteAgentsObserver> tReporter { nullptr };
	CSphScopedPtr<IReplyParser_t> tParser { nullptr };
	if ( !dRemotes.IsEmpty() )
	{
		if ( m_pProfile )
			m_pProfile->Switch ( SPH_QSTATE_DIST_CONNECT );

		tReqBuilder = new SearchRequestBuilder_t ( m_dQueries, iStart, iEnd, iDivideLimits );
		tParser = new SearchReplyParser_c ( iStart, iEnd );
		tReporter = GetObserver();

		// run remote queries. tReporter will tell us when they're finished.
		// also blackholes will be removed from this flow of remotes.
		ScheduleDistrJobs ( dRemotes, tReqBuilder.Ptr (),
			tParser.Ptr (),
			tReporter, tFirst.m_iRetryCount, tFirst.m_iRetryDelay );
	}

	/////////////////////
	// run local queries
	//////////////////////

	// while the remote queries are running, do local searches
	if ( m_dLocal.GetLength() )
	{
		SetupLocalDF ( iStart, iEnd );

		if ( m_pProfile )
			m_pProfile->Switch ( SPH_QSTATE_LOCAL_SEARCH );

		tmLocal = -sphMicroTimer();
		RunLocalSearches();
		tmLocal += sphMicroTimer();
	}

	///////////////////////
	// poll remote queries
	///////////////////////

	if ( !dRemotes.IsEmpty() )
	{
		if ( m_pProfile )
			m_pProfile->Switch ( SPH_QSTATE_DIST_WAIT );

		bool bDistDone = false;
		while ( !bDistDone )
		{
			// don't forget to check incoming replies after send was over
			bDistDone = tReporter->IsDone();
			if ( !bDistDone )
				tReporter->WaitChanges (); /// wait one or more remote queries to complete

			ARRAY_FOREACH ( iAgent, dRemotes )
			{
				AgentConn_t * pAgent = dRemotes[iAgent];
				assert ( !pAgent->IsBlackhole () ); // must not be any blacknole here.

				if ( !pAgent->m_bSuccess )
					continue;

				sphLogDebugv ( "agent %d, state %s, order %d, sock %d", iAgent, pAgent->StateName(), pAgent->m_iStoreTag, pAgent->m_iSock );

				DistrServedByAgent_t * pDistr = nullptr;
				for ( auto &tDistr : dDistrServedByAgent )
					if ( tDistr.m_dAgentIds.Contains ( iAgent ) )
					{
						pDistr = &tDistr;
						break;
					}
				assert ( pDistr );

				int iOrderTag = pAgent->m_iStoreTag;
				// merge this agent's results
				for ( int iRes=iStart; iRes<=iEnd; ++iRes )
				{
					auto pResult = ( cSearchResult * ) pAgent->m_pResult.Ptr ();
					if ( !pResult )
						continue;

					const CSphQueryResult &tRemoteResult = pResult->m_dResults[iRes - iStart];

					// copy errors or warnings
					if ( !tRemoteResult.m_sError.IsEmpty() )
						m_dFailuresSet[iRes].SubmitEx ( tFirst.m_sIndexes.cstr(), NULL,
							"agent %s: remote query error: %s",
							pAgent->m_tDesc.GetMyUrl().cstr(), tRemoteResult.m_sError.cstr() );
					if ( !tRemoteResult.m_sWarning.IsEmpty() )
						m_dFailuresSet[iRes].SubmitEx ( tFirst.m_sIndexes.cstr(), NULL,
							"agent %s: remote query warning: %s",
							pAgent->m_tDesc.GetMyUrl().cstr(), tRemoteResult.m_sWarning.cstr() );

					if ( tRemoteResult.m_iSuccesses<=0 )
						continue;

					AggrResult_t & tRes = m_dResults[iRes];
					tRes.m_iSuccesses++;
					tRes.m_tSchema = tRemoteResult.m_tSchema;

					assert ( !tRes.m_dTag2Pools[iOrderTag + iRes - iStart].m_pMva && !tRes.m_dTag2Pools[iOrderTag + iRes - iStart].m_pStrings );

					tRes.m_dMatches.Reserve ( tRes.m_dMatches.GetLength() + tRemoteResult.m_dMatches.GetLength() );
					ARRAY_FOREACH ( i, tRemoteResult.m_dMatches )
					{
						tRes.m_dMatches.Add();
						tRemoteResult.m_tSchema.CloneWholeMatch ( &tRes.m_dMatches.Last(), tRemoteResult.m_dMatches[i] );
						tRes.m_dMatches.Last().m_iTag = ( iOrderTag + iRes - iStart ) | 0x80000000;
					}

					tRes.m_pMva = nullptr;
					tRes.m_pStrings = nullptr;
					tRes.m_dTag2Pools[iOrderTag+iRes-iStart].m_pMva = nullptr;
					tRes.m_dTag2Pools[iOrderTag+iRes-iStart].m_pStrings = nullptr;
					tRes.m_dMatchCounts.Add ( tRemoteResult.m_dMatches.GetLength() );
					tRes.m_dSchemas.Add ( tRemoteResult.m_tSchema );
					// note how we do NOT add per-index weight here

					// merge this agent's stats
					tRes.m_iTotalMatches += tRemoteResult.m_iTotalMatches;
					tRes.m_iQueryTime += tRemoteResult.m_iQueryTime;
					tRes.m_iAgentCpuTime += tRemoteResult.m_iCpuTime;
					tRes.m_tAgentIOStats.Add ( tRemoteResult.m_tIOStats );
					tRes.m_iAgentPredictedTime += tRemoteResult.m_iPredictedTime;
					tRes.m_iAgentFetchedDocs += tRemoteResult.m_iAgentFetchedDocs;
					tRes.m_iAgentFetchedHits += tRemoteResult.m_iAgentFetchedHits;
					tRes.m_iAgentFetchedSkips += tRemoteResult.m_iAgentFetchedSkips;
					tRes.m_bHasPrediction |= ( m_dQueries[iRes].m_iMaxPredictedMsec>0 );

					if ( pDistr )
					{
						pDistr->m_dStats[iRes-iStart].m_uQueryTime += tRemoteResult.m_iQueryTime;
						pDistr->m_dStats[iRes-iStart].m_uFoundRows += tRemoteResult.m_iTotalMatches;
						pDistr->m_dStats[iRes-iStart].m_iSuccesses++;
					}

					// merge this agent's words
					MergeWordStats ( tRes, tRemoteResult.m_hWordStats, &m_dFailuresSet[iRes], tFirst.m_sIndexes.cstr(), NULL );
				}

				// dismissed
				if ( pAgent->m_pResult )
					pAgent->m_pResult->Reset ();
				pAgent->m_bSuccess = false;
				pAgent->m_sFailure = "";
			}
		} // while ( !bDistDone )
	} // if ( bDist && dRemotes.GetLength() )

	// submit failures from failed agents
	// copy timings from all agents
	if ( !dRemotes.IsEmpty() )
	{
		for ( const AgentConn_t * pAgent : dRemotes )
		{
			assert ( !pAgent->IsBlackhole () ); // must not be any blacknole here.

			for ( int j=iStart; j<=iEnd; ++j )
			{
				assert ( pAgent->m_iWall>=0 );
				m_dAgentTimes[j].Add ( ( pAgent->m_iWall ) / ( 1000 * ( iEnd-iStart+1 ) ) );
			}

			if ( !pAgent->m_bSuccess && !pAgent->m_sFailure.IsEmpty() )
				for ( int j=iStart; j<=iEnd; j++ )
					m_dFailuresSet[j].SubmitEx ( tFirst.m_sIndexes.cstr(), nullptr, "agent %s: %s",
						pAgent->m_tDesc.GetMyUrl().cstr(), pAgent->m_sFailure.cstr() );
		}
	}

	/////////////////////
	// merge all results
	/////////////////////

	if ( m_pProfile )
		m_pProfile->Switch ( SPH_QSTATE_AGGREGATE );

	CSphIOStats tIO;

	for ( int iRes=iStart; iRes<=iEnd; ++iRes )
	{
		AggrResult_t & tRes = m_dResults[iRes];
		const CSphQuery & tQuery = m_dQueries[iRes];
		sph::StringSet hExtra;
		if ( !m_dExtraSchemas.IsEmpty () )
			for ( const auto &dExtraSet : m_dExtraSchemas[iRes] )
				for ( const CSphString &sExtra : dExtraSet )
					hExtra.Add ( sExtra );


		// minimize sorters needs these pointers
		tIO.Add ( tRes.m_tIOStats );

		// if there were no successful searches at all, this is an error
		if ( !tRes.m_iSuccesses )
		{
			StringBuilder_c sFailures;
			m_dFailuresSet[iRes].BuildReport ( sFailures );
			sFailures.MoveTo (tRes.m_sError);
			continue;
		}

		// minimize schema and remove dupes
		// assuming here ( tRes.m_tSchema==tRes.m_dSchemas[0] )
		const CSphFilterSettings * pAggrFilter = nullptr;
		if ( m_bMaster && !tQuery.m_tHaving.m_sAttrName.IsEmpty() )
			pAggrFilter = &tQuery.m_tHaving;

		const CSphVector<CSphQueryItem> & dItems = ( tQuery.m_dRefItems.GetLength() ? tQuery.m_dRefItems : tQuery.m_dItems );

		if ( tRes.m_iSuccesses>1 || dItems.GetLength() || pAggrFilter )
		{

			if ( m_bMaster && tRes.m_iSuccesses && dItems.GetLength() && tQuery.m_sGroupBy.IsEmpty() && tRes.m_dMatches.GetLength()==0 )
			{
				for ( auto& dItem : dItems )
				{
					if ( dItem.m_sExpr=="count(*)" || ( dItem.m_sExpr=="@distinct" ) )
						tRes.m_dZeroCount.Add ( dItem.m_sAlias );
				}
			}

			bool bOk = MinimizeAggrResult ( tRes, tQuery, !m_dLocal.IsEmpty(), hExtra, m_pProfile, pAggrFilter, m_bFederatedUser );

			if ( !bOk )
			{
				tRes.m_iSuccesses = 0;
				return; // FIXME? really return, not just continue?
			}
		}

		if ( !m_dFailuresSet[iRes].IsEmpty() )
		{
			StringBuilder_c sFailures;
			m_dFailuresSet[iRes].BuildReport ( sFailures );
			sFailures.MoveTo ( tRes.m_sWarning );
		}

		////////////
		// finalize
		////////////

		tRes.m_iOffset = Max ( tQuery.m_iOffset, tQuery.m_iOuterOffset );
		tRes.m_iCount = ( tQuery.m_iOuterLimit ? tQuery.m_iOuterLimit : tQuery.m_iLimit );
		tRes.m_iCount = Max ( Min ( tRes.m_iCount, tRes.m_dMatches.GetLength()-tRes.m_iOffset ), 0 );
	}

	/////////////////////////////////
	// functions on a table argument
	/////////////////////////////////

	for ( int iRes=iStart; iRes<=iEnd; ++iRes )
	{
		AggrResult_t & tRes = m_dResults[iRes];
		ISphTableFunc * pTableFunc = m_dTables[iRes];

		// FIXME! log such queries properly?
		if ( pTableFunc )
		{
			if ( m_pProfile )
				m_pProfile->Switch ( SPH_QSTATE_TABLE_FUNC );
			if ( !pTableFunc->Process ( &tRes, tRes.m_sError ) )
				tRes.m_iSuccesses = 0;
		}
	}

	/////////
	// stats
	/////////

	tmSubset = sphMicroTimer() - tmSubset;
	tmCpu = sphCpuTimer() - tmCpu;

	// in multi-queue case (1 actual call per N queries), just divide overall query time evenly
	// otherwise (N calls per N queries), divide common query time overheads evenly
	const int iQueries = iEnd-iStart+1;
	if ( m_bMultiQueue )
	{
		for ( int iRes=iStart; iRes<=iEnd; ++iRes )
		{
			m_dResults[iRes].m_iQueryTime = (int)( tmSubset/1000/iQueries );
			m_dResults[iRes].m_iRealQueryTime = (int)( tmSubset/1000/iQueries );
			m_dResults[iRes].m_iCpuTime = tmCpu/iQueries;
		}
	} else
	{
		int64_t tmAccountedWall = 0;
		int64_t tmAccountedCpu = 0;
		for ( int iRes=iStart; iRes<=iEnd; ++iRes )
		{
			tmAccountedWall += m_dResults[iRes].m_iQueryTime*1000;
			assert ( ( m_dResults[iRes].m_iCpuTime==0 && m_dResults[iRes].m_iAgentCpuTime==0 ) ||	// all work was done in this thread
					( m_dResults[iRes].m_iCpuTime>0 && m_dResults[iRes].m_iAgentCpuTime==0 ) ||		// children threads work
					( m_dResults[iRes].m_iAgentCpuTime>0 && m_dResults[iRes].m_iCpuTime==0 ) );		// agents work
			tmAccountedCpu += m_dResults[iRes].m_iCpuTime;
			tmAccountedCpu += m_dResults[iRes].m_iAgentCpuTime;
		}
		// whether we had work done in children threads (dist_threads>1) or in agents
		bool bExternalWork = tmAccountedCpu!=0;

		int64_t tmDeltaWall = ( tmSubset - tmAccountedWall ) / iQueries;

		for ( int iRes=iStart; iRes<=iEnd; ++iRes )
		{
			m_dResults[iRes].m_iQueryTime += (int)(tmDeltaWall/1000);
			m_dResults[iRes].m_iRealQueryTime = (int)( tmSubset/1000/iQueries );
			m_dResults[iRes].m_iCpuTime = tmCpu/iQueries;
			if ( bExternalWork )
				m_dResults[iRes].m_iCpuTime += tmAccountedCpu;
		}

		// correct per-index stats from agents
		int iTotalSuccesses = 0;
		for ( int iRes=iStart; iRes<=iEnd; ++iRes )
			iTotalSuccesses += m_dResults[iRes].m_iSuccesses;

		int nValidDistrIndexes = 0;
		for ( const auto &tDistrStat : dDistrServedByAgent )
			for ( int iQuery=iStart; iQuery<=iEnd; ++iQuery )
				if ( tDistrStat.m_dStats[iQuery-iStart].m_iSuccesses )
				{
					++nValidDistrIndexes;
					break;
				}

		if ( iTotalSuccesses && nValidDistrIndexes )
			for ( auto &tDistrStat : dDistrServedByAgent )
				for ( int iQuery=iStart; iQuery<=iEnd; ++iQuery )
				{
					QueryStat_t & tStat = tDistrStat.m_dStats[iQuery-iStart];
					int64_t tmDeltaWallAgent = ( tmSubset - tmAccountedWall ) * tStat.m_iSuccesses / ( iTotalSuccesses*nValidDistrIndexes );
					tStat.m_uQueryTime += (int)(tmDeltaWallAgent/1000);
				}

		int nValidLocalIndexes = 0;
		for ( const auto & dQueryIndexStat : m_dQueryIndexStats )
			for ( int iQuery=iStart; iQuery<=iEnd; ++iQuery )
				if ( dQueryIndexStat.m_dStats[iQuery-iStart].m_iSuccesses )
				{
					++nValidLocalIndexes;
					break;
				}

		if ( iTotalSuccesses && nValidLocalIndexes )
			for ( auto &dQueryIndexStat : m_dQueryIndexStats )
				for ( int iQuery=iStart; iQuery<=iEnd; iQuery++ )
				{
					QueryStat_t & tStat = dQueryIndexStat.m_dStats[iQuery-iStart];
					int64_t tmDeltaWallLocal = ( tmSubset - tmAccountedWall ) * tStat.m_iSuccesses / ( iTotalSuccesses*nValidLocalIndexes );
					tStat.m_uQueryTime += (int)(tmDeltaWallLocal/1000);
				}

		// don't forget to add this to stats
		if ( bExternalWork )
			tmCpu += tmAccountedCpu;
	}

	// calculate per-index stats
	ARRAY_FOREACH ( iLocal, m_dLocal )
	{
		// a little of durty casting: from ServedDesc_t* to ServedIndex_c*
		// in order to save statistics.
		auto pServed = ( ServedIndex_c * ) m_dLocked.Get ( m_dLocal[iLocal].m_sName );
		for ( int iQuery=iStart; iQuery<=iEnd; ++iQuery )
		{
			QueryStat_t & tStat = m_dQueryIndexStats[iLocal].m_dStats[iQuery-iStart];
			if ( !tStat.m_iSuccesses )
				continue;

			pServed->AddQueryStat ( tStat.m_uFoundRows, tStat.m_uQueryTime );

			for ( auto &tDistr : dDistrServedByAgent )
			{
				if ( tDistr.m_dLocalNames.Contains ( m_dLocal[iLocal].m_sName ) )
				{
					tDistr.m_dStats[iQuery - iStart].m_uQueryTime += tStat.m_uQueryTime;
					tDistr.m_dStats[iQuery - iStart].m_uFoundRows += tStat.m_uFoundRows;
					tDistr.m_dStats[iQuery - iStart].m_iSuccesses++;
				}
			}
		}
	}

	for ( auto &tDistr : dDistrServedByAgent )
	{
		auto pServedDistIndex = GetDistr ( tDistr.m_sIndex );
		if ( pServedDistIndex )
			for ( int iQuery=iStart; iQuery<=iEnd; ++iQuery )
			{
				auto & tStat = tDistr.m_dStats[iQuery-iStart];
				if ( !tStat.m_iSuccesses )
					continue;

				pServedDistIndex->AddQueryStat ( tStat.m_uFoundRows, tStat.m_uQueryTime );
			}
	}

	g_tStats.m_iQueries += iQueries;
	g_tStats.m_iQueryTime += tmSubset;
	g_tStats.m_iQueryCpuTime += tmCpu;
	if ( dRemotes.GetLength() )
	{
		int64_t tmWait = 0;
		for ( const AgentConn_t * pAgent : dRemotes )
			tmWait += pAgent->m_iWaited;

		// do *not* count queries to dist indexes w/o actual remote agents
		++g_tStats.m_iDistQueries;
		g_tStats.m_iDistWallTime += tmSubset;
		g_tStats.m_iDistLocalTime += tmLocal;
		g_tStats.m_iDistWaitTime += tmWait;
	}
	g_tStats.m_iDiskReads += tIO.m_iReadOps;
	g_tStats.m_iDiskReadTime += tIO.m_iReadTime;
	g_tStats.m_iDiskReadBytes += tIO.m_iReadBytes;

	if ( m_pProfile )
		m_pProfile->Switch ( eOldState );
}


bool CheckCommandVersion ( WORD uVer, WORD uDaemonVersion, CachedOutputBuffer_c & tOut )
{
	if ( ( uVer>>8)!=( uDaemonVersion>>8) )
	{
		SendErrorReply ( tOut, "major command version mismatch (expected v.%d.x, got v.%d.%d)",
			uDaemonVersion>>8, uVer>>8, uVer&0xff );
		return false;
	}
	if ( uVer>uDaemonVersion )
	{
		SendErrorReply ( tOut, "client version is higher than daemon version (client is v.%d.%d, daemon is v.%d.%d)",
			uVer>>8, uVer&0xff, uDaemonVersion>>8, uDaemonVersion&0xff );
		return false;
	}
	return true;
}

void HandleCommandSearch ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq, ThdDesc_t & tThd )
{
	MEMORY ( MEM_API_SEARCH );

	if ( !CheckCommandVersion ( uVer, VER_COMMAND_SEARCH, tOut ) )
		return;

	const WORD MIN_VERSION = 0x119;
	if ( uVer<MIN_VERSION )
	{
		SendErrorReply ( tOut, "client version is too old; upgrade your client (client is v.%d.%d, min is v.%d.%d)", uVer>>8, uVer&0xff, MIN_VERSION>>8, MIN_VERSION&0xff );
		return;
	}

	int iMasterVer = tReq.GetInt();
	if ( iMasterVer<0 || iMasterVer>VER_MASTER )
	{
		SendErrorReply ( tOut, "master-agent version mismatch; update me first, then update master!" );
		return;
	}
	WORD uMasterVer { WORD (iMasterVer) };
	bool bAgentMode = ( uMasterVer>0 );

	// parse request
	int iQueries = tReq.GetDword ();

	if ( g_iMaxBatchQueries>0 && ( iQueries<=0 || iQueries>g_iMaxBatchQueries ) )
	{
		SendErrorReply ( tOut, "bad multi-query count %d (must be in 1..%d range)", iQueries, g_iMaxBatchQueries );
		return;
	}

	SearchHandler_c tHandler ( iQueries, nullptr, QUERY_API, ( iMasterVer==0 ), tThd );
	for ( auto &dQuery : tHandler.m_dQueries )
		if ( !ParseSearchQuery ( tReq, tOut, dQuery, uVer, uMasterVer ) )
			return;

	if ( tHandler.m_dQueries.GetLength() )
	{
		QueryType_e eQueryType = tHandler.m_dQueries[0].m_eQueryType;

#ifndef NDEBUG
		// we assume that all incoming queries have the same type
		for ( const auto & i: tHandler.m_dQueries )
			assert ( i.m_eQueryType==eQueryType );
#endif

		QueryParser_i * pParser {nullptr};
		if ( eQueryType==QUERY_JSON )
			pParser = sphCreateJsonQueryParser();
		else
			pParser = sphCreatePlainQueryParser();

		assert ( pParser );
		tHandler.SetQueryParser ( pParser );
		tHandler.SetQueryType ( eQueryType );

		const CSphQuery & q = tHandler.m_dQueries[0];
		tThd.SetThreadInfo ( "api-search query=\"%s\" comment=\"%s\" index=\"%s\"", q.m_sQuery.scstr(), q.m_sComment.scstr(), q.m_sIndexes.scstr() );
		tThd.SetSearchQuery ( &q );
	}

	// run queries, send response
	tHandler.RunQueries();

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_SEARCH );
	ARRAY_FOREACH ( i, tHandler.m_dQueries )
		SendResult ( uVer, tOut, &tHandler.m_dResults[i], bAgentMode, tHandler.m_dQueries[i], uMasterVer );


	int64_t iTotalPredictedTime = 0;
	int64_t iTotalAgentPredictedTime = 0;
	for ( const auto& dResult : tHandler.m_dResults )
	{
		iTotalPredictedTime += dResult.m_iPredictedTime;
		iTotalAgentPredictedTime += dResult.m_iAgentPredictedTime;
	}

	g_tStats.m_iPredictedTime += iTotalPredictedTime;
	g_tStats.m_iAgentPredictedTime += iTotalAgentPredictedTime;

	ScWL_t dLastMetaLock ( g_tLastMetaLock );
	g_tLastMeta = tHandler.m_dResults[tHandler.m_dResults.GetLength () - 1];

	// clean up query at thread descriptor
	tThd.SetSearchQuery ( nullptr );
}

//////////////////////////////////////////////////////////////////////////
// TABLE FUNCTIONS
//////////////////////////////////////////////////////////////////////////

// table functions take an arbitrary result set as their input,
// and return a new, processed, (completely) different one as their output
//
// 1st argument should be the input result set, but a table function
// can optionally take and handle more arguments
//
// table function can completely (!) change the result set
// including (!) the schema
//
// for now, only builtin table functions are supported
// UDFs are planned when the internal call interface is stabilized

#define LOC_ERROR(_msg) { sError = _msg; return false; }
#define LOC_ERROR1(_msg,_arg1) { sError.SetSprintf ( _msg, _arg1 ); return false; }

class CSphTableFuncRemoveRepeats : public ISphTableFunc
{
protected:
	CSphString	m_sCol;
	int			m_iOffset;
	int			m_iLimit;

public:
	virtual bool ValidateArgs ( const StrVec_t & dArgs, const CSphQuery &, CSphString & sError )
	{
		if ( dArgs.GetLength()!=3 )
			LOC_ERROR ( "REMOVE_REPEATS() requires 4 arguments (result_set, column, offset, limit)" );
		if ( !isdigit ( *dArgs[1].cstr() ) )
			LOC_ERROR ( "REMOVE_REPEATS() argument 3 (offset) must be integer" );
		if ( !isdigit ( *dArgs[2].cstr() ) )
			LOC_ERROR ( "REMOVE_REPEATS() argument 4 (limit) must be integer" );

		m_sCol = dArgs[0];
		m_iOffset = atoi ( dArgs[1].cstr() );
		m_iLimit = atoi ( dArgs[2].cstr() );

		if ( !m_iLimit )
			LOC_ERROR ( "REMOVE_REPEATS() argument 4 (limit) must be greater than 0" );
		return true;
	}


	virtual bool Process ( AggrResult_t * pResult, CSphString & sError )
	{
		assert ( pResult );

		CSphSwapVector<CSphMatch> & dMatches = pResult->m_dMatches;
		if ( !dMatches.GetLength() )
			return true;

		const CSphColumnInfo * pCol = pResult->m_tSchema.GetAttr ( m_sCol.cstr() );
		if ( !pCol )
			LOC_ERROR1 ( "REMOVE_REPEATS() argument 2 (column %s) not found in result set", m_sCol.cstr() );

		ESphAttr t = pCol->m_eAttrType;
		if ( t!=SPH_ATTR_INTEGER && t!=SPH_ATTR_BIGINT && t!=SPH_ATTR_TOKENCOUNT && t!=SPH_ATTR_STRINGPTR && t!=SPH_ATTR_STRING )
			LOC_ERROR1 ( "REMOVE_REPEATS() argument 2 (column %s) must be of INTEGER, BIGINT, or STRINGPTR type", m_sCol.cstr() );

		// we need to initialize the "last seen" value with a key that
		// is guaranteed to be different from the 1st match that we will scan
		// hence (val-1) for scalars, and NULL for strings
		SphAttr_t iLastValue = ( t==SPH_ATTR_STRING || t==SPH_ATTR_STRINGPTR )
			? 0
			: ( dMatches [ pResult->m_iOffset ].GetAttr ( pCol->m_tLocator ) - 1 );

		// LIMIT N,M clause must be applied before (!) table function
		// so we scan source matches N to N+M-1
		//
		// within those matches, we filter out repeats in a given column,
		// skip first m_iOffset eligible ones, and emit m_iLimit more
		int iOutPos = 0;
		for ( int i=pResult->m_iOffset; i<Min ( dMatches.GetLength(), pResult->m_iOffset+pResult->m_iCount ); i++ )
		{
			// get value, skip repeats
			SphAttr_t iCur = dMatches[i].GetAttr ( pCol->m_tLocator );
			if ( t==SPH_ATTR_STRING && iCur!=0 )
				iCur = (SphAttr_t)( pResult->m_dTag2Pools [ dMatches[i].m_iTag ].m_pStrings + iCur );

			if ( iCur==iLastValue )
				continue;
			if ( iCur && iLastValue && t==SPH_ATTR_STRINGPTR )
			{
				const BYTE * a = (const BYTE*) iCur;
				const BYTE * b = (const BYTE*) iLastValue;
				int iLen1 = sphUnpackPtrAttr ( a, &a );
				int iLen2 = sphUnpackPtrAttr ( b, &b );
				if ( iLen1==iLen2 && memcmp ( a, b, iLen1 )==0 )
					continue;
			}

			if ( iCur && iLastValue && t==SPH_ATTR_STRING )
			{
				const BYTE * a = (const BYTE*) iCur;
				const BYTE * b = (const BYTE*) iLastValue;
				int iLen1 = sphUnpackStr ( a, &a );
				int iLen2 = sphUnpackStr ( b, &b );
				if ( iLen1==iLen2 && memcmp ( a, b, iLen1 )==0 )
					continue;
			}

			iLastValue = iCur;

			// skip eligible rows according to tablefunc offset
			if ( m_iOffset>0 )
			{
				m_iOffset--;
				continue;
			}

			// emit!
			if ( iOutPos!=i )
				Swap ( dMatches[iOutPos], dMatches[i] );

			// break if we reached the tablefunc limit
			if ( ++iOutPos==m_iLimit )
				break;
		}

		// adjust the result set limits
		pResult->ClampMatches ( iOutPos, true );
		pResult->m_iOffset = 0;
		pResult->m_iCount = dMatches.GetLength();
		return true;
	}
};

#undef LOC_ERROR1
#undef LOC_ERROR

//////////////////////////////////////////////////////////////////////////
// SQL PARSER
//////////////////////////////////////////////////////////////////////////
// FIXME? verify or generate these automatically somehow?
static const char * g_dSqlStmts[] =
{
	"parse_error", "dummy", "select", "insert", "replace", "delete", "show_warnings",
	"show_status", "show_meta", "set", "begin", "commit", "rollback", "call",
	"desc", "show_tables", "update", "create_func", "drop_func", "attach_index",
	"flush_rtindex", "flush_ramchunk", "show_variables", "truncate_rtindex", "select_sysvar",
	"show_collation", "show_character_set", "optimize_index", "show_agent_status",
	"show_index_status", "show_profile", "alter_add", "alter_drop", "show_plan",
	"select_dual", "show_databases", "create_plugin", "drop_plugin", "show_plugins", "show_threads",
	"facet", "alter_reconfigure", "show_index_settings", "flush_index", "reload_plugins", "reload_index",
	"flush_hostnames", "flush_logs", "reload_indexes", "sysfilters", "debug",
	"join_cluster", "cluster_create", "cluster_delete", "cluster_index_add", "cluster_index_delete"
};


STATIC_ASSERT ( sizeof(g_dSqlStmts)/sizeof(g_dSqlStmts[0])==STMT_TOTAL, STMT_DESC_SHOULD_BE_SAME_AS_STMT_TOTAL );


/// parser view on a generic node
/// CAUTION, nodes get copied in the parser all the time, must keep assignment slim
struct SqlNode_t
{
	int						m_iStart = 0;	///< first byte relative to m_pBuf, inclusive
	int						m_iEnd = 0;		///< last byte relative to m_pBuf, exclusive! thus length = end - start
	int64_t					m_iValue = 0;
	int						m_iType = 0;	///< TOK_xxx type for insert values; SPHINXQL_TOK_xxx code for special idents
	float					m_fValue = 0.0;
	AttrValues_p			m_pValues { nullptr };	///< filter values vector (FIXME? replace with numeric handles into parser state?)
	int						m_iParsedOp = -1;

	SqlNode_t() = default;

};
#define YYSTYPE SqlNode_t

SqlStmt_t::SqlStmt_t ()
{
	m_tQuery.m_eMode = SPH_MATCH_EXTENDED2; // only new and shiny matching and sorting
	m_tQuery.m_eSort = SPH_SORT_EXTENDED;
	m_tQuery.m_sSortBy = "@weight desc"; // default order
	m_tQuery.m_sOrderBy = "@weight desc";
	m_tQuery.m_iAgentQueryTimeout = g_iAgentQueryTimeout;
	m_tQuery.m_iRetryCount = -1;
	m_tQuery.m_iRetryDelay = -1;
}

SqlStmt_t::~SqlStmt_t()
{
	SafeDelete ( m_pTableFunc );
}

bool SqlStmt_t::AddSchemaItem ( const char * psName )
{
	m_dInsertSchema.Add ( psName );
	m_dInsertSchema.Last().ToLower();
	m_iSchemaSz = m_dInsertSchema.GetLength();
	return true; // stub; check if the given field actually exists in the schema
}

// check if the number of fields which would be inserted is in accordance to the given schema
bool SqlStmt_t::CheckInsertIntegrity()
{
	// cheat: if no schema assigned, assume the size of schema as the size of the first row.
	// (if it is wrong, it will be revealed later)
	if ( !m_iSchemaSz )
		m_iSchemaSz = m_dInsertValues.GetLength();

	m_iRowsAffected++;
	return m_dInsertValues.GetLength()==m_iRowsAffected*m_iSchemaSz;
}


/// magic codes passed via SqlNode_t::m_iStart to handle certain special tokens
/// for instance, to fixup "count(*)" as "@count" easily
enum
{
	SPHINXQL_TOK_COUNT		= -1,
	SPHINXQL_TOK_GROUPBY	= -2,
	SPHINXQL_TOK_WEIGHT		= -3,
	SPHINXQL_TOK_ID			= -4
};

/// types of string-list filters.
enum class STRLIST {
	// string matching: assume attr is a whole solid string
	// attr MUST match any of variants provided, assuming collation applied
	IN_, 	/// 'hello' OP ( 'hello', 'foo') true, OP ( 'foo', 'fee' ) false

	// tags matching: assume attr is string of space-separated tags, no collation
	// any separate tag of attr MUST match any of variants provided
	ANY,	/// 'hello world' OP ('hello', 'foo') true, OP ('foo', 'fee' ) false

	// every separate tag of attr MUST match any of variants provided
	ALL,    /// 'hello world' OP ('world', 'hello') true, OP ('a','world','hello') false
};

struct SqlParser_c : ISphNoncopyable
{
public:
	void *			m_pScanner = nullptr;
	const char *	m_pBuf = nullptr;
	const char *	m_pLastTokenStart = nullptr;
	CSphString *	m_pParseError = nullptr;
	CSphQuery *		m_pQuery = nullptr;
	bool			m_bGotQuery;
	SqlStmt_t *		m_pStmt = nullptr;
	CSphVector<SqlStmt_t> & m_dStmt;
	ESphCollation	m_eCollation;
	BYTE			m_uSyntaxFlags = 0;

	CSphVector<FilterTreeItem_t> m_dFilterTree;
	CSphVector<int>				m_dFiltersPerStmt;
	bool						m_bGotFilterOr = false;
	CSphString		m_sErrorHeader;

public:
	explicit		SqlParser_c ( CSphVector<SqlStmt_t> & dStmt, ESphCollation eCollation );

	void			PushQuery ();

	bool			AddOption ( const SqlNode_t & tIdent );
	bool			AddOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue );
	bool			AddOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue, const SqlNode_t & sArg );
	bool			AddOption ( const SqlNode_t & tIdent, CSphVector<CSphNamedInt> & dNamed );
	bool			AddInsertOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue );
	void			AddItem ( SqlNode_t * pExpr, ESphAggrFunc eFunc=SPH_AGGR_NONE, SqlNode_t * pStart=NULL, SqlNode_t * pEnd=NULL );
	bool			AddItem ( const char * pToken, SqlNode_t * pStart=NULL, SqlNode_t * pEnd=NULL );
	bool			AddCount ();
	void			AliasLastItem ( SqlNode_t * pAlias );

	/// called on transition from an outer select to inner select
	void ResetSelect()
	{
		if ( m_pQuery )
			m_pQuery->m_iSQLSelectStart = m_pQuery->m_iSQLSelectEnd = -1;
	}

	/// called every time we capture a select list item
	/// (i think there should be a simpler way to track these though)
	void SetSelect ( SqlNode_t * pStart, SqlNode_t * pEnd=NULL )
	{
		if ( m_pQuery )
		{
			if ( pStart && ( m_pQuery->m_iSQLSelectStart<0 || m_pQuery->m_iSQLSelectStart>pStart->m_iStart ) )
				m_pQuery->m_iSQLSelectStart = pStart->m_iStart;
			if ( !pEnd )
				pEnd = pStart;
			if ( pEnd && ( m_pQuery->m_iSQLSelectEnd<0 || m_pQuery->m_iSQLSelectEnd<pEnd->m_iEnd ) )
				m_pQuery->m_iSQLSelectEnd = pEnd->m_iEnd;
		}
	}

	inline CSphString & ToString ( CSphString & sRes, const SqlNode_t & tNode ) const
	{
		if ( tNode.m_iType>=0 )
			sRes.SetBinary ( m_pBuf + tNode.m_iStart, tNode.m_iEnd - tNode.m_iStart );
		else switch ( tNode.m_iType )
		{
			case SPHINXQL_TOK_COUNT:	sRes = "@count"; break;
			case SPHINXQL_TOK_GROUPBY:	sRes = "@groupby"; break;
			case SPHINXQL_TOK_WEIGHT:	sRes = "@weight"; break;
			case SPHINXQL_TOK_ID:		sRes = "@id"; break;
			default:					assert ( 0 && "INTERNAL ERROR: unknown parser ident code" );
		}
		return sRes;
	}

	inline void ToStringUnescape ( CSphString & sRes, const SqlNode_t & tNode ) const
	{
		assert ( tNode.m_iType>=0 );
		SqlUnescape ( sRes, m_pBuf + tNode.m_iStart, tNode.m_iEnd - tNode.m_iStart );
	}

	bool			AddSchemaItem ( SqlNode_t * pNode );
	bool			SetMatch ( const SqlNode_t & tValue );
	void			AddConst ( int iList, const SqlNode_t& tValue );
	void			SetStatement ( const SqlNode_t & tName, SqlSet_e eSet );
	bool			AddFloatRangeFilter ( const SqlNode_t & tAttr, float fMin, float fMax, bool bHasEqual, bool bExclude=false );
	bool			AddIntRangeFilter ( const SqlNode_t & tAttr, int64_t iMin, int64_t iMax, bool bExclude );
	bool			AddIntFilterGreater ( const SqlNode_t & tAttr, int64_t iVal, bool bHasEqual );
	bool			AddIntFilterLesser ( const SqlNode_t & tAttr, int64_t iVal, bool bHasEqual );
	bool			AddUservarFilter ( const SqlNode_t & tCol, const SqlNode_t & tVar, bool bExclude );
	void			AddGroupBy ( const SqlNode_t & tGroupBy );
	bool			AddDistinct ( SqlNode_t * pNewExpr, SqlNode_t * pStart, SqlNode_t * pEnd );
	CSphFilterSettings *	AddFilter ( const SqlNode_t & tCol, ESphFilter eType );
	bool					AddStringFilter ( const SqlNode_t & tCol, const SqlNode_t & tVal, bool bExclude );
	CSphFilterSettings *	AddValuesFilter ( const SqlNode_t & tCol ) { return AddFilter ( tCol, SPH_FILTER_VALUES ); }
	bool			AddStringListFilter ( const SqlNode_t & tCol, SqlNode_t & tVal, STRLIST eType, bool bInverse=false );
	bool					AddNullFilter ( const SqlNode_t & tCol, bool bEqualsNull );
	void			AddHaving ();

	void			FilterGroup ( SqlNode_t & tNode, SqlNode_t & tExpr );
	void			FilterOr ( SqlNode_t & tNode, const SqlNode_t & tLeft, const SqlNode_t & tRight );
	void			FilterAnd ( SqlNode_t & tNode, const SqlNode_t & tLeft, const SqlNode_t & tRight );
	void			SetOp ( SqlNode_t & tNode );

	inline bool		SetOldSyntax()
	{
		m_uSyntaxFlags |= 1;
		return IsGoodSyntax ();
	}

	inline bool		SetNewSyntax()
	{
		m_uSyntaxFlags |= 2;
		return IsGoodSyntax ();
	}
	bool IsGoodSyntax ();
	inline bool IsDeprecatedSyntax () const
	{
		return m_uSyntaxFlags & 1;
	}

	int							AllocNamedVec ();
	CSphVector<CSphNamedInt> &	GetNamedVec ( int iIndex );
	void						FreeNamedVec ( int iIndex );
	bool						UpdateStatement ( SqlNode_t * pNode );
	bool						DeleteStatement ( SqlNode_t * pNode );

	void						AddUpdatedAttr ( const SqlNode_t & tName, ESphAttr eType ) const;
	void						UpdateMVAAttr ( const SqlNode_t & tName, const SqlNode_t& dValues );
	void						SetGroupbyLimit ( int iLimit );
	void						SetLimit ( int iOffset, int iLimit );
	void						SetIndex ( const SqlNode_t & tIndex );

private:
	void						AutoAlias ( CSphQueryItem & tItem, SqlNode_t * pStart, SqlNode_t * pEnd );
	void						GenericStatement ( SqlNode_t * pNode, SqlStmt_e iStmt );

protected:
	bool						m_bNamedVecBusy = false;
	CSphVector<CSphNamedInt>	m_dNamedVec;
};

//////////////////////////////////////////////////////////////////////////

// unused parameter, simply to avoid type clash between all my yylex() functions
#define YY_DECL static int my_lex ( YYSTYPE * lvalp, void * yyscanner, SqlParser_c * pParser )

#if USE_WINDOWS
#define YY_NO_UNISTD_H 1
#endif

#ifdef CMAKE_GENERATED_LEXER

#ifdef __GNUC__
	#pragma GCC diagnostic push 
	#pragma GCC diagnostic ignored "-Wsign-compare"
	#pragma GCC diagnostic ignored "-Wpragmas"
	#pragma GCC diagnostic ignored "-Wunneeded-internal-declaration"
#endif

#include "flexsphinxql.c"

#ifdef __GNUC__
	#pragma GCC diagnostic pop
#endif

#else
#include "llsphinxql.c"
#endif

void yyerror ( SqlParser_c * pParser, const char * sMessage )
{
	// flex put a zero at last token boundary; make it undo that
	yylex_unhold ( pParser->m_pScanner );

	// create our error message
	pParser->m_pParseError->SetSprintf ( "%s %s near '%s'", pParser->m_sErrorHeader.cstr(), sMessage,
		pParser->m_pLastTokenStart ? pParser->m_pLastTokenStart : "(null)" );

	// fixup TOK_xxx thingies
	char * s = const_cast<char*> ( pParser->m_pParseError->cstr() );
	char * d = s;
	while ( *s )
	{
		if ( strncmp ( s, "TOK_", 4 )==0 )
			s += 4;
		else
			*d++ = *s++;
	}
	*d = '\0';
}


#ifndef NDEBUG
// using a proxy to be possible to debug inside yylex
static int yylex ( YYSTYPE * lvalp, SqlParser_c * pParser )
{
	int res = my_lex ( lvalp, pParser->m_pScanner, pParser );
	return res;
}
#else
static int yylex ( YYSTYPE * lvalp, SqlParser_c * pParser )
{
	return my_lex ( lvalp, pParser->m_pScanner, pParser );
}
#endif

#ifdef CMAKE_GENERATED_GRAMMAR
	#include "bissphinxql.c"
#else
	#include "yysphinxql.c"
#endif

//////////////////////////////////////////////////////////////////////////

int sphGetTokTypeInt()
{
	return TOK_CONST_INT;
}


int sphGetTokTypeFloat()
{
	return TOK_CONST_FLOAT;
}


int sphGetTokTypeStr()
{
	return TOK_QUOTED_STRING;
}

int sphGetTokTypeConstMVA()
{
	return TOK_CONST_MVA;
}

//////////////////////////////////////////////////////////////////////////

class CSphMatchVariant : public CSphMatch
{
public:
	inline static SphAttr_t ToInt ( const SqlInsert_t & tVal )
	{
		switch ( tVal.m_iType )
		{
			case TOK_QUOTED_STRING :	return strtoul ( tVal.m_sVal.cstr(), NULL, 10 ); // FIXME? report conversion error?
			case TOK_CONST_INT:			return int(tVal.m_iVal);
			case TOK_CONST_FLOAT:		return int(tVal.m_fVal); // FIXME? report conversion error
		}
		return 0;
	}

	inline static SphAttr_t ToBigInt ( const SqlInsert_t & tVal )
	{
		switch ( tVal.m_iType )
		{
			case TOK_QUOTED_STRING :	return strtoll ( tVal.m_sVal.cstr(), NULL, 10 ); // FIXME? report conversion error?
			case TOK_CONST_INT:			return tVal.m_iVal;
			case TOK_CONST_FLOAT:		return int(tVal.m_fVal); // FIXME? report conversion error?
		}
		return 0;
	}

	inline static SphDocID_t ToDocid ( const SqlInsert_t & tVal )
	{
		// FIXME? report conversion errors?
		SphDocID_t uRes = DOCID_MAX;
		switch ( tVal.m_iType )
		{
			case TOK_QUOTED_STRING :	uRes = (SphDocID_t) strtoull ( tVal.m_sVal.cstr(), NULL, 10 ); break;
			case TOK_CONST_INT:			uRes = (SphDocID_t) tVal.m_iVal; break;
			case TOK_CONST_FLOAT:		uRes = (SphDocID_t) tVal.m_fVal; break;
		}
		if ( uRes==DOCID_MAX )
			uRes = 0;
		return uRes;
	}

	bool SetAttr ( const CSphAttrLocator & tLoc, const SqlInsert_t & tVal, ESphAttr eTargetType )
	{
		switch ( eTargetType )
		{
			case SPH_ATTR_INTEGER:
			case SPH_ATTR_TIMESTAMP:
			case SPH_ATTR_BOOL:
			case SPH_ATTR_TOKENCOUNT:
				CSphMatch::SetAttr ( tLoc, ToInt(tVal) );
				break;
			case SPH_ATTR_BIGINT:
				CSphMatch::SetAttr ( tLoc, ToBigInt(tVal) );
				break;
			case SPH_ATTR_FLOAT:
				if ( tVal.m_iType==TOK_QUOTED_STRING )
					SetAttrFloat ( tLoc, (float)strtod ( tVal.m_sVal.cstr(), NULL ) ); // FIXME? report conversion error?
				else if ( tVal.m_iType==TOK_CONST_INT )
					SetAttrFloat ( tLoc, float(tVal.m_iVal) ); // FIXME? report conversion error?
				else if ( tVal.m_iType==TOK_CONST_FLOAT )
					SetAttrFloat ( tLoc, tVal.m_fVal );
				break;
			case SPH_ATTR_STRING:
			case SPH_ATTR_STRINGPTR:
			case SPH_ATTR_UINT32SET:
			case SPH_ATTR_INT64SET:
			case SPH_ATTR_JSON:
				CSphMatch::SetAttr ( tLoc, 0 );
				break;
			default:
				return false;
		};
		return true;
	}

	inline bool SetDefaultAttr ( const CSphAttrLocator & tLoc, ESphAttr eTargetType )
	{
		SqlInsert_t tVal;
		tVal.m_iType = TOK_CONST_INT;
		tVal.m_iVal = 0;
		return SetAttr ( tLoc, tVal, eTargetType );
	}
};

SqlParser_c::SqlParser_c ( CSphVector<SqlStmt_t> & dStmt, ESphCollation eCollation )
	: m_dStmt ( dStmt )
	, m_eCollation ( eCollation )
	, m_sErrorHeader ( "sphinxql:" )
{
	assert ( !m_dStmt.GetLength() );
	PushQuery ();
}

void SqlParser_c::PushQuery ()
{
	assert ( m_dStmt.GetLength() || ( !m_pQuery && !m_pStmt ) );

	// post set proper result-set order
	if ( m_dStmt.GetLength() && m_pQuery )
	{
		if ( m_pQuery->m_sGroupBy.IsEmpty() )
			m_pQuery->m_sSortBy = m_pQuery->m_sOrderBy;
		else
			m_pQuery->m_sGroupSortBy = m_pQuery->m_sOrderBy;

		m_dFiltersPerStmt.Add ( m_dFilterTree.GetLength() );
	}

	// add new
	m_dStmt.Add ( SqlStmt_t() );
	m_pStmt = &m_dStmt.Last();
	m_pQuery = &m_pStmt->m_tQuery;
	m_pQuery->m_eCollation = m_eCollation;

	m_bGotQuery = false;
}


bool SqlParser_c::AddOption ( const SqlNode_t & tIdent )
{
	CSphString sOpt, sVal;
	ToString ( sOpt, tIdent ).ToLower();

	if ( sOpt=="low_priority" )
	{
		m_pQuery->m_bLowPriority = true;
	} else if ( sOpt=="debug_no_payload" )
	{
		m_pStmt->m_tQuery.m_uDebugFlags |= QUERY_DEBUG_NO_PAYLOAD;
	} else
	{
		m_pParseError->SetSprintf ( "unknown option '%s'", sOpt.cstr() );
		return false;
	}

	return true;
}


bool SqlParser_c::AddOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue )
{
	CSphString sOpt, sVal;
	ToString ( sOpt, tIdent ).ToLower();
	ToString ( sVal, tValue ).ToLower().Unquote();

	// OPTIMIZE? hash possible sOpt choices?
	if ( sOpt=="ranker" )
	{
		m_pQuery->m_eRanker = SPH_RANK_TOTAL;
		for ( int iRanker = SPH_RANK_PROXIMITY_BM25; iRanker<=SPH_RANK_SPH04; iRanker++ )
			if ( sVal==sphGetRankerName ( ESphRankMode ( iRanker ) ) )
			{
				m_pQuery->m_eRanker = ESphRankMode ( iRanker );
				break;
			}

		if ( m_pQuery->m_eRanker==SPH_RANK_TOTAL )
		{
			if ( sVal==sphGetRankerName ( SPH_RANK_EXPR ) || sVal==sphGetRankerName ( SPH_RANK_EXPORT ) )
			{
				m_pParseError->SetSprintf ( "missing ranker expression (use OPTION ranker=expr('1+2') for example)" );
				return false;
			} else if ( sphPluginExists ( PLUGIN_RANKER, sVal.cstr() ) )
			{
				m_pQuery->m_eRanker = SPH_RANK_PLUGIN;
				m_pQuery->m_sUDRanker = sVal;
			}
			m_pParseError->SetSprintf ( "unknown ranker '%s'", sVal.cstr() );
			return false;
		}
	} else if ( sOpt=="token_filter" )	// tokfilter = hello.dll:hello:some_opts
	{
		StrVec_t dParams;
		if ( !sphPluginParseSpec ( sVal, dParams, *m_pParseError ) )
			return false;

		if ( !dParams.GetLength() )
		{
			m_pParseError->SetSprintf ( "missing token filter spec string" );
			return false;
		}

		m_pQuery->m_sQueryTokenFilterLib = dParams[0];
		m_pQuery->m_sQueryTokenFilterName = dParams[1];
		m_pQuery->m_sQueryTokenFilterOpts = dParams[2];
	} else if ( sOpt=="max_matches" )
	{
		m_pQuery->m_iMaxMatches = (int)tValue.m_iValue;

	} else if ( sOpt=="cutoff" )
	{
		m_pQuery->m_iCutoff = (int)tValue.m_iValue;

	} else if ( sOpt=="max_query_time" )
	{
		m_pQuery->m_uMaxQueryMsec = (int)tValue.m_iValue;

	} else if ( sOpt=="retry_count" )
	{
		m_pQuery->m_iRetryCount = (int)tValue.m_iValue;

	} else if ( sOpt=="retry_delay" )
	{
		m_pQuery->m_iRetryDelay = (int)tValue.m_iValue;

	} else if ( sOpt=="reverse_scan" )
	{
		m_pQuery->m_bReverseScan = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="ignore_nonexistent_columns" )
	{
		m_pQuery->m_bIgnoreNonexistent = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="comment" )
	{
		ToStringUnescape ( m_pQuery->m_sComment, tValue );

	} else if ( sOpt=="sort_method" )
	{
		if ( sVal=="pq" )			m_pQuery->m_bSortKbuffer = false;
		else if ( sVal=="kbuffer" )	m_pQuery->m_bSortKbuffer = true;
		else
		{
			m_pParseError->SetSprintf ( "unknown sort_method=%s (known values are pq, kbuffer)", sVal.cstr() );
			return false;
		}

	} else if ( sOpt=="agent_query_timeout" )
	{
		m_pQuery->m_iAgentQueryTimeout = (int)tValue.m_iValue;

	} else if ( sOpt=="max_predicted_time" )
	{
		m_pQuery->m_iMaxPredictedMsec = int ( tValue.m_iValue > INT_MAX ? INT_MAX : tValue.m_iValue );

	} else if ( sOpt=="boolean_simplify" )
	{
		m_pQuery->m_bSimplify = true;

	} else if ( sOpt=="idf" )
	{
		StrVec_t dOpts;
		sphSplit ( dOpts, sVal.cstr() );

		ARRAY_FOREACH ( i, dOpts )
		{
			if ( dOpts[i]=="normalized" )
				m_pQuery->m_bPlainIDF = false;
			else if ( dOpts[i]=="plain" )
				m_pQuery->m_bPlainIDF = true;
			else if ( dOpts[i]=="tfidf_normalized" )
				m_pQuery->m_bNormalizedTFIDF = true;
			else if ( dOpts[i]=="tfidf_unnormalized" )
				m_pQuery->m_bNormalizedTFIDF = false;
			else
			{
				m_pParseError->SetSprintf ( "unknown flag %s in idf=%s (known values are plain, normalized, tfidf_normalized, tfidf_unnormalized)",
					dOpts[i].cstr(), sVal.cstr() );
				return false;
			}
		}
	} else if ( sOpt=="global_idf" )
	{
		m_pQuery->m_bGlobalIDF = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="local_df" )
	{
		m_pQuery->m_bLocalDF = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="ignore_nonexistent_indexes" )
	{
		m_pQuery->m_bIgnoreNonexistentIndexes = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="strict" )
	{
		m_pQuery->m_bStrict = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="columns" ) // for SHOW THREADS
	{
		m_pStmt->m_iThreadsCols = Max ( (int)tValue.m_iValue, 0 );

	} else if ( sOpt=="rand_seed" )
	{
		m_pStmt->m_tQuery.m_iRandSeed = int64_t(DWORD(tValue.m_iValue));

	} else if ( sOpt=="sync" )
	{
		m_pQuery->m_bSync = ( tValue.m_iValue!=0 );

	} else if ( sOpt=="expand_keywords" )
	{
		m_pQuery->m_eExpandKeywords = ( tValue.m_iValue!=0 ? QUERY_OPT_ENABLED : QUERY_OPT_DISABLED );

	} else if ( sOpt=="format" ) // for SHOW THREADS
	{
		m_pStmt->m_sThreadFormat = sVal;

	} else
	{
		m_pParseError->SetSprintf ( "unknown option '%s' (or bad argument type)", sOpt.cstr() );
		return false;
	}

	return true;
}


bool SqlParser_c::AddOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue, const SqlNode_t & tArg )
{
	CSphString sOpt, sVal;
	ToString ( sOpt, tIdent ).ToLower();
	ToString ( sVal, tValue ).ToLower().Unquote();

	if ( sOpt=="ranker" )
	{
		if ( sVal=="expr" || sVal=="export" )
		{
			m_pQuery->m_eRanker = sVal=="expr" ? SPH_RANK_EXPR : SPH_RANK_EXPORT;
			ToStringUnescape ( m_pQuery->m_sRankerExpr, tArg );
			return true;
		} else if ( sphPluginExists ( PLUGIN_RANKER, sVal.cstr() ) )
		{
			m_pQuery->m_eRanker = SPH_RANK_PLUGIN;
			m_pQuery->m_sUDRanker = sVal;
			ToStringUnescape ( m_pQuery->m_sUDRankerOpts, tArg );
			return true;
		}
	}

	m_pParseError->SetSprintf ( "unknown option or extra argument to '%s=%s'", sOpt.cstr(), sVal.cstr() );
	return false;
}


bool SqlParser_c::AddOption ( const SqlNode_t & tIdent, CSphVector<CSphNamedInt> & dNamed )
{
	CSphString sOpt;
	ToString ( sOpt, tIdent ).ToLower ();

	if ( sOpt=="field_weights" )
	{
		m_pQuery->m_dFieldWeights.SwapData ( dNamed );

	} else if ( sOpt=="index_weights" )
	{
		m_pQuery->m_dIndexWeights.SwapData ( dNamed );

	} else
	{
		m_pParseError->SetSprintf ( "unknown option '%s' (or bad argument type)", sOpt.cstr() );
		return false;
	}

	return true;
}


bool SqlParser_c::AddInsertOption ( const SqlNode_t & tIdent, const SqlNode_t & tValue )
{
	CSphString sOpt, sVal;
	ToString ( sOpt, tIdent ).ToLower();
	ToString ( sVal, tValue ).Unquote();

	if ( sOpt=="token_filter_options" )
	{
		m_pStmt->m_sStringParam = sVal;
	} else
	{
		m_pParseError->SetSprintf ( "unknown option '%s' (or bad argument type)", sOpt.cstr() );
		return false;
	}
	return true;
}


void SqlParser_c::AliasLastItem ( SqlNode_t * pAlias )
{
	if ( pAlias )
	{
		CSphQueryItem & tItem = m_pQuery->m_dItems.Last();
		tItem.m_sAlias.SetBinary ( m_pBuf + pAlias->m_iStart, pAlias->m_iEnd - pAlias->m_iStart );
		tItem.m_sAlias.ToLower();
		SetSelect ( pAlias );
	}
}

void SqlParser_c::AutoAlias ( CSphQueryItem & tItem, SqlNode_t * pStart, SqlNode_t * pEnd )
{
	if ( pStart && pEnd )
	{
		tItem.m_sAlias.SetBinary ( m_pBuf + pStart->m_iStart, pEnd->m_iEnd - pStart->m_iStart );
		sphColumnToLowercase ( const_cast<char *>( tItem.m_sAlias.cstr() ) );
	} else
	{
		tItem.m_sAlias = tItem.m_sExpr;
	}
	SetSelect ( pStart, pEnd );
}

void SqlParser_c::AddItem ( SqlNode_t * pExpr, ESphAggrFunc eAggrFunc, SqlNode_t * pStart, SqlNode_t * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr.SetBinary ( m_pBuf + pExpr->m_iStart, pExpr->m_iEnd - pExpr->m_iStart );
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	tItem.m_eAggrFunc = eAggrFunc;
	AutoAlias ( tItem, pStart?pStart:pExpr, pEnd?pEnd:pExpr );
}

bool SqlParser_c::AddItem ( const char * pToken, SqlNode_t * pStart, SqlNode_t * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr = pToken;
	tItem.m_eAggrFunc = SPH_AGGR_NONE;
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	AutoAlias ( tItem, pStart, pEnd );
	return SetNewSyntax();
}

bool SqlParser_c::AddCount ()
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr = tItem.m_sAlias = "count(*)";
	tItem.m_eAggrFunc = SPH_AGGR_NONE;
	return SetNewSyntax();
}

void SqlParser_c::AddGroupBy ( const SqlNode_t & tGroupBy )
{
	if ( m_pQuery->m_sGroupBy.IsEmpty() )
	{
		m_pQuery->m_eGroupFunc = SPH_GROUPBY_ATTR;
		m_pQuery->m_sGroupBy.SetBinary ( m_pBuf + tGroupBy.m_iStart, tGroupBy.m_iEnd - tGroupBy.m_iStart );
		sphColumnToLowercase ( const_cast<char *>( m_pQuery->m_sGroupBy.cstr() ) );
	} else
	{
		m_pQuery->m_eGroupFunc = SPH_GROUPBY_MULTIPLE;
		CSphString sTmp;
		sTmp.SetBinary ( m_pBuf + tGroupBy.m_iStart, tGroupBy.m_iEnd - tGroupBy.m_iStart );
		sphColumnToLowercase ( const_cast<char *>( sTmp.cstr() ) );
		m_pQuery->m_sGroupBy.SetSprintf ( "%s, %s", m_pQuery->m_sGroupBy.cstr(), sTmp.cstr() );
	}
}

void SqlParser_c::SetGroupbyLimit ( int iLimit )
{
	m_pQuery->m_iGroupbyLimit = iLimit;
}

bool SqlParser_c::AddDistinct ( SqlNode_t * pNewExpr, SqlNode_t * pStart, SqlNode_t * pEnd )
{
	if ( !m_pQuery->m_sGroupDistinct.IsEmpty() )
	{
		yyerror ( this, "too many COUNT(DISTINCT) clauses" );
		return false;
	}

	ToString ( m_pQuery->m_sGroupDistinct, *pNewExpr );
	return AddItem ( "@distinct", pStart, pEnd );
}

bool SqlParser_c::AddSchemaItem ( YYSTYPE * pNode )
{
	assert ( m_pStmt );
	CSphString sItem;
	sItem.SetBinary ( m_pBuf + pNode->m_iStart, pNode->m_iEnd - pNode->m_iStart );
	return m_pStmt->AddSchemaItem ( sItem.cstr() );
}

bool SqlParser_c::SetMatch ( const YYSTYPE& tValue )
{
	if ( m_bGotQuery )
	{
		yyerror ( this, "too many MATCH() clauses" );
		return false;
	};

	ToStringUnescape ( m_pQuery->m_sQuery, tValue );
	m_pQuery->m_sRawQuery = m_pQuery->m_sQuery;
	return m_bGotQuery = true;
}

void SqlParser_c::AddConst ( int iList, const YYSTYPE& tValue )
{
	CSphVector<CSphNamedInt> & dVec = GetNamedVec ( iList );

	dVec.Add();
	ToString ( dVec.Last().m_sName, tValue ).ToLower();
	dVec.Last().m_iValue = (int) tValue.m_iValue;
}

void SqlParser_c::SetStatement ( const YYSTYPE & tName, SqlSet_e eSet )
{
	m_pStmt->m_eStmt = STMT_SET;
	m_pStmt->m_eSet = eSet;
	ToString ( m_pStmt->m_sSetName, tName );
}

void SqlParser_c::GenericStatement ( SqlNode_t * pNode, SqlStmt_e iStmt )
{
	m_pStmt->m_eStmt = iStmt;
	m_pStmt->m_iListStart = pNode->m_iStart;
	m_pStmt->m_iListEnd = pNode->m_iEnd;
	ToString ( m_pStmt->m_sIndex, *pNode );
}

bool SqlParser_c::UpdateStatement ( SqlNode_t * pNode )
{
	GenericStatement ( pNode, STMT_UPDATE );
	m_pStmt->m_tUpdate.m_dRowOffset.Add ( 0 );
	return true;
}

bool SqlParser_c::DeleteStatement ( SqlNode_t * pNode )
{
	GenericStatement ( pNode, STMT_DELETE );
	SetIndex ( *pNode );
	return true;
}

void SqlParser_c::AddUpdatedAttr ( const SqlNode_t & tName, ESphAttr eType ) const
{
	CSphAttrUpdate & tUpd = m_pStmt->m_tUpdate;
	CSphString sAttr;
	tUpd.m_dAttrs.Add ( ToString ( sAttr, tName ).ToLower().Leak() );
	tUpd.m_dTypes.Add ( eType ); // sorry, ints only for now, riding on legacy shit!
}


void SqlParser_c::UpdateMVAAttr ( const SqlNode_t & tName, const SqlNode_t & dValues )
{
	CSphAttrUpdate & tUpd = m_pStmt->m_tUpdate;
	ESphAttr eType = SPH_ATTR_UINT32SET;

	if ( dValues.m_pValues && dValues.m_pValues->GetLength()>0 )
	{
		// got MVA values, let's process them
		dValues.m_pValues->Uniq(); // don't need dupes within MVA
		tUpd.m_dPool.Add ( dValues.m_pValues->GetLength()*2 );
		SphAttr_t * pVal = dValues.m_pValues->Begin();
		SphAttr_t * pValMax = pVal + dValues.m_pValues->GetLength();
		for ( ;pVal<pValMax; pVal++ )
		{
			SphAttr_t uVal = *pVal;
			if ( uVal>UINT_MAX )
			{
				eType = SPH_ATTR_INT64SET;
			}
			tUpd.m_dPool.Add ( (DWORD)uVal );
			tUpd.m_dPool.Add ( (DWORD)( uVal>>32 ) );
		}
	} else
	{
		// no values, means we should delete the attribute
		// we signal that to the update code by putting a single zero
		// to the values pool (meaning a zero-length MVA values list)
		tUpd.m_dPool.Add ( 0 );
	}

	AddUpdatedAttr ( tName, eType );
}

CSphFilterSettings * SqlParser_c::AddFilter ( const SqlNode_t & tCol, ESphFilter eType )
{
	CSphString sCol;
	ToString ( sCol, tCol ); // do NOT lowercase just yet, might have to retain case for JSON cols
	
	FilterTreeItem_t & tElem = m_dFilterTree.Add();
	tElem.m_iFilterItem = m_pQuery->m_dFilters.GetLength();

	CSphFilterSettings * pFilter = &m_pQuery->m_dFilters.Add();
	pFilter->m_sAttrName = ( !strcasecmp ( sCol.cstr(), "id" ) ) ? "@id" : sCol;
	pFilter->m_eType = eType;
	sphColumnToLowercase ( const_cast<char *>( pFilter->m_sAttrName.cstr() ) );
	return pFilter;
}

bool SqlParser_c::AddFloatRangeFilter ( const SqlNode_t & sAttr, float fMin, float fMax, bool bHasEqual, bool bExclude )
{
	CSphFilterSettings * pFilter = AddFilter ( sAttr, SPH_FILTER_FLOATRANGE );
	if ( !pFilter )
		return false;
	pFilter->m_fMinValue = fMin;
	pFilter->m_fMaxValue = fMax;
	pFilter->m_bHasEqualMin = bHasEqual;
	pFilter->m_bHasEqualMax = bHasEqual;
	pFilter->m_bExclude = bExclude;
	return true;
}

bool SqlParser_c::AddIntRangeFilter ( const SqlNode_t & sAttr, int64_t iMin, int64_t iMax, bool bExclude )
{
	CSphFilterSettings * pFilter = AddFilter ( sAttr, SPH_FILTER_RANGE );
	if ( !pFilter )
		return false;
	pFilter->m_iMinValue = iMin;
	pFilter->m_iMaxValue = iMax;
	pFilter->m_bExclude = bExclude;
	return true;
}

bool SqlParser_c::AddIntFilterGreater ( const SqlNode_t & tAttr, int64_t iVal, bool bHasEqual )
{
	CSphFilterSettings * pFilter = AddFilter ( tAttr, SPH_FILTER_RANGE );
	if ( !pFilter )
		return false;
	bool bId = ( pFilter->m_sAttrName=="@id" ) || ( pFilter->m_sAttrName=="id" );
	pFilter->m_iMaxValue = bId ? (SphAttr_t)ULLONG_MAX : LLONG_MAX;
	pFilter->m_iMinValue = iVal;
	pFilter->m_bHasEqualMin = bHasEqual;
	pFilter->m_bOpenRight = true;

	return true;
}

bool SqlParser_c::AddIntFilterLesser ( const SqlNode_t & tAttr, int64_t iVal, bool bHasEqual )
{
	CSphFilterSettings * pFilter = AddFilter ( tAttr, SPH_FILTER_RANGE );
	if ( !pFilter )
		return false;
	bool bId = ( pFilter->m_sAttrName=="@id" ) || ( pFilter->m_sAttrName=="id" );
	pFilter->m_iMinValue = bId ? 0 : LLONG_MIN;
	pFilter->m_iMaxValue = iVal;
	pFilter->m_bHasEqualMax = bHasEqual;
	pFilter->m_bOpenLeft = true;

	return true;
}

bool SqlParser_c::AddUservarFilter ( const SqlNode_t & tCol, const SqlNode_t & tVar, bool bExclude )
{
	CSphFilterSettings * pFilter = AddFilter ( tCol, SPH_FILTER_USERVAR );
	if ( !pFilter )
		return false;
	CSphString & sUserVar = pFilter->m_dStrings.Add();
	ToString ( sUserVar, tVar ).ToLower();
	pFilter->m_bExclude = bExclude;
	return true;
}


bool SqlParser_c::AddStringFilter ( const SqlNode_t & tCol, const SqlNode_t & tVal, bool bExclude )
{
	CSphFilterSettings * pFilter = AddFilter ( tCol, SPH_FILTER_STRING );
	if ( !pFilter )
		return false;
	CSphString & sFilterString = pFilter->m_dStrings.Add();
	ToStringUnescape ( sFilterString, tVal );
	pFilter->m_bExclude = bExclude;
	return true;
}


bool SqlParser_c::AddStringListFilter ( const SqlNode_t & tCol, SqlNode_t & tVal, STRLIST eType, bool bInverse )
{
	CSphFilterSettings * pFilter = AddFilter ( tCol, SPH_FILTER_STRING_LIST );
	if ( !pFilter || !tVal.m_pValues )
		return false;

	pFilter->m_dStrings.Resize ( tVal.m_pValues->GetLength() );
	ARRAY_FOREACH ( i, ( *tVal.m_pValues ) )
	{
		uint64_t uVal = ( *tVal.m_pValues )[i];
		int iOff = ( uVal>>32 );
		int iLen = ( uVal & 0xffffffff );
		SqlUnescape ( pFilter->m_dStrings[i], m_pBuf + iOff, iLen );
	}
	tVal.m_pValues = nullptr;
	pFilter->m_bExclude = bInverse;
	assert ( pFilter->m_eMvaFunc == SPH_MVAFUNC_NONE ); // that is default for IN filter
	if ( eType==STRLIST::ANY )
		pFilter->m_eMvaFunc = SPH_MVAFUNC_ANY;
	else if ( eType==STRLIST::ALL )
		pFilter->m_eMvaFunc = SPH_MVAFUNC_ALL;
	return true;
}


bool SqlParser_c::AddNullFilter ( const SqlNode_t & tCol, bool bEqualsNull )
{
	CSphFilterSettings * pFilter = AddFilter ( tCol, SPH_FILTER_NULL );
	if ( !pFilter )
		return false;
	pFilter->m_bIsNull = bEqualsNull;
	return true;
}

void SqlParser_c::AddHaving ()
{
	assert ( m_pQuery->m_dFilters.GetLength() );
	m_pQuery->m_tHaving = m_pQuery->m_dFilters.Pop();
}


bool SqlParser_c::IsGoodSyntax ()
{
	if ( ( m_uSyntaxFlags & 3 )!=3 )
		return true;
	yyerror ( this, "Mixing the old-fashion internal vars (@id, @count, @weight) with new acronyms like count(*), weight() is prohibited" );
	return false;
}


int SqlParser_c::AllocNamedVec ()
{
	// we only allow one such vector at a time, right now
	assert ( !m_bNamedVecBusy );
	m_bNamedVecBusy = true;
	m_dNamedVec.Resize ( 0 );
	return 0;
}

void SqlParser_c::SetLimit ( int iOffset, int iLimit )
{
	m_pQuery->m_iOffset = iOffset;
	m_pQuery->m_iLimit = iLimit;
	m_pStmt->m_bLimitSet = true;
}

#ifndef NDEBUG
CSphVector<CSphNamedInt> & SqlParser_c::GetNamedVec ( int iIndex )
#else
CSphVector<CSphNamedInt> & SqlParser_c::GetNamedVec ( int )
#endif
{
	assert ( m_bNamedVecBusy && iIndex==0 );
	return m_dNamedVec;
}

#ifndef NDEBUG
void SqlParser_c::FreeNamedVec ( int iIndex )
#else
void SqlParser_c::FreeNamedVec ( int )
#endif
{
	assert ( m_bNamedVecBusy && iIndex==0 );
	m_bNamedVecBusy = false;
	m_dNamedVec.Resize ( 0 );
}

void SqlParser_c::SetOp ( SqlNode_t & tNode )
{
	tNode.m_iParsedOp = m_dFilterTree.GetLength() - 1;
}

void SqlParser_c::FilterGroup ( SqlNode_t & tNode, SqlNode_t & tExpr )
{
	tNode.m_iParsedOp = tExpr.m_iParsedOp;
}

void SqlParser_c::FilterAnd ( SqlNode_t & tNode, const SqlNode_t & tLeft, const SqlNode_t & tRight )
{
	tNode.m_iParsedOp = m_dFilterTree.GetLength();
	
	FilterTreeItem_t & tElem = m_dFilterTree.Add();
	tElem.m_iLeft = tLeft.m_iParsedOp;
	tElem.m_iRight = tRight.m_iParsedOp;
}

void SqlParser_c::FilterOr ( SqlNode_t & tNode, const SqlNode_t & tLeft, const SqlNode_t & tRight )
{
	tNode.m_iParsedOp = m_dFilterTree.GetLength();
	m_bGotFilterOr = true;

	FilterTreeItem_t & tElem = m_dFilterTree.Add();
	tElem.m_bOr = true;
	tElem.m_iLeft = tLeft.m_iParsedOp;
	tElem.m_iRight = tRight.m_iParsedOp;
}


struct QueryItemProxy_t
{
	DWORD m_uHash;
	int m_iIndex;
	CSphQueryItem * m_pItem;

	bool operator < ( const QueryItemProxy_t & tItem ) const
	{
		return ( ( m_uHash<tItem.m_uHash ) || ( m_uHash==tItem.m_uHash && m_iIndex<tItem.m_iIndex ) );
	}

	bool operator == ( const QueryItemProxy_t & tItem ) const
	{
		return ( m_uHash==tItem.m_uHash );
	}

	void QueryItemHash ()
	{
		assert ( m_pItem );
		m_uHash = sphCRC32 ( m_pItem->m_sAlias.cstr() );
		m_uHash = sphCRC32 ( m_pItem->m_sExpr.cstr(), m_pItem->m_sExpr.Length(), m_uHash );
		m_uHash = sphCRC32 ( (const void*)&m_pItem->m_eAggrFunc, sizeof(m_pItem->m_eAggrFunc), m_uHash );
	}
};

static void CreateFilterTree ( const CSphVector<FilterTreeItem_t> & dOps, int iStart, int iCount, CSphQuery & tQuery )
{
	bool bHasOr = false;
	int iTreeCount = iCount - iStart;
	CSphVector<FilterTreeItem_t> dTree ( iTreeCount );
	for ( int i = 0; i<iTreeCount; i++ )
	{
		FilterTreeItem_t tItem = dOps[iStart + i];
		tItem.m_iLeft = ( tItem.m_iLeft==-1 ? -1 : tItem.m_iLeft - iStart );
		tItem.m_iRight = ( tItem.m_iRight==-1 ? -1 : tItem.m_iRight - iStart );
		dTree[i] = tItem;
		bHasOr |= ( tItem.m_iFilterItem==-1 && tItem.m_bOr );
	}

	// query has only plain AND filters - no need for filter tree
	if ( !bHasOr )
		return;

	tQuery.m_dFilterTree.SwapData ( dTree );
}

bool sphParseSqlQuery ( const char * sQuery, int iLen, CSphVector<SqlStmt_t> & dStmt, CSphString & sError, ESphCollation eCollation )
{
	if ( !sQuery || !iLen )
	{
		sError = "query was empty";
		return false;
	}

	SqlParser_c tParser ( dStmt, eCollation );
	tParser.m_pBuf = sQuery;
	tParser.m_pLastTokenStart = NULL;
	tParser.m_pParseError = &sError;
	tParser.m_eCollation = eCollation;

	char * sEnd = const_cast<char *>( sQuery ) + iLen;
	sEnd[0] = 0; // prepare for yy_scan_buffer
	sEnd[1] = 0; // this is ok because string allocates a small gap

	yylex_init ( &tParser.m_pScanner );
	YY_BUFFER_STATE tLexerBuffer = yy_scan_buffer ( const_cast<char *>( sQuery ), iLen+2, tParser.m_pScanner );
	if ( !tLexerBuffer )
	{
		sError = "internal error: yy_scan_buffer() failed";
		return false;
	}

	int iRes = yyparse ( &tParser );
	yy_delete_buffer ( tLexerBuffer, tParser.m_pScanner );
	yylex_destroy ( tParser.m_pScanner );

	dStmt.Pop(); // last query is always dummy

	int iFilterStart = 0;
	int iFilterCount = 0;
	ARRAY_FOREACH ( iStmt, dStmt )
	{
		// select expressions will be reparsed again, by an expression parser,
		// when we have an index to actually bind variables, and create a tree
		//
		// so at SQL parse stage, we only do quick validation, and at this point,
		// we just store the select list for later use by the expression parser
		CSphQuery & tQuery = dStmt[iStmt].m_tQuery;
		if ( tQuery.m_iSQLSelectStart>=0 )
		{
			if ( tQuery.m_iSQLSelectStart-1>=0 && tParser.m_pBuf[tQuery.m_iSQLSelectStart-1]=='`' )
				tQuery.m_iSQLSelectStart--;
			if ( tQuery.m_iSQLSelectEnd<iLen && tParser.m_pBuf[tQuery.m_iSQLSelectEnd]=='`' )
				tQuery.m_iSQLSelectEnd++;

			tQuery.m_sSelect.SetBinary ( tParser.m_pBuf + tQuery.m_iSQLSelectStart,
				tQuery.m_iSQLSelectEnd - tQuery.m_iSQLSelectStart );
		}

		// validate tablefuncs
		// tablefuncs are searchd-level builtins rather than common expression-level functions
		// so validation happens here, expression parser does not know tablefuncs (ignorance is bliss)
		if ( dStmt[iStmt].m_eStmt==STMT_SELECT && !dStmt[iStmt].m_sTableFunc.IsEmpty() )
		{
			CSphString & sFunc = dStmt[iStmt].m_sTableFunc;
			sFunc.ToUpper();

			ISphTableFunc * pFunc = NULL;
			if ( sFunc=="REMOVE_REPEATS" )
				pFunc = new CSphTableFuncRemoveRepeats();

			if ( !pFunc )
			{
				sError.SetSprintf ( "unknown table function %s()", sFunc.cstr() );
				return false;
			}
			if ( !pFunc->ValidateArgs ( dStmt[iStmt].m_dTableFuncArgs, tQuery, sError ) )
			{
				SafeDelete ( pFunc );
				return false;
			}
			dStmt[iStmt].m_pTableFunc = pFunc;
		}

		// validate filters
		ARRAY_FOREACH ( i, tQuery.m_dFilters )
		{
			const CSphString & sCol = tQuery.m_dFilters[i].m_sAttrName;
			if ( !strcasecmp ( sCol.cstr(), "@count" ) || !strcasecmp ( sCol.cstr(), "count(*)" ) )
			{
				sError.SetSprintf ( "sphinxql: Aggregates in 'where' clause prohibited, use 'having'" );
				return false;
			}
		}

		iFilterCount = tParser.m_dFiltersPerStmt[iStmt];
		// all queries have only plain AND filters - no need for filter tree
		if ( iFilterCount && tParser.m_bGotFilterOr )
			CreateFilterTree ( tParser.m_dFilterTree, iFilterStart, iFilterCount, tQuery );
		iFilterStart += iFilterCount;
	}

	if ( iRes!=0 || !dStmt.GetLength() )
		return false;

	if ( tParser.IsDeprecatedSyntax() )
	{
		sError = "Using the old-fashion @variables (@count, @weight, etc.) is deprecated";
		return false;
	}

	// facets
	bool bGotFacet = false;
	ARRAY_FOREACH ( i, dStmt )
	{
		const SqlStmt_t & tHeadStmt = dStmt[i];
		const CSphQuery & tHeadQuery = tHeadStmt.m_tQuery;
		if ( dStmt[i].m_eStmt==STMT_SELECT )
		{
			i++;
			if ( i<dStmt.GetLength() && dStmt[i].m_eStmt==STMT_FACET )
			{
				bGotFacet = true;
				const_cast<CSphQuery &>(tHeadQuery).m_bFacetHead = true;
			}

			for ( ; i<dStmt.GetLength() && dStmt[i].m_eStmt==STMT_FACET; i++ )
			{
				SqlStmt_t & tStmt = dStmt[i];
				tStmt.m_tQuery.m_bFacet = true;

				tStmt.m_eStmt = STMT_SELECT;
				tStmt.m_tQuery.m_sIndexes = tHeadQuery.m_sIndexes;
				tStmt.m_tQuery.m_sSelect = tStmt.m_tQuery.m_sFacetBy;
				tStmt.m_tQuery.m_sQuery = tHeadQuery.m_sQuery;
				tStmt.m_tQuery.m_iMaxMatches = tHeadQuery.m_iMaxMatches;
				
				// need to keep same wide result set schema
				tStmt.m_tQuery.m_sGroupDistinct = tHeadQuery.m_sGroupDistinct;

				// append filters
				ARRAY_FOREACH ( k, tHeadQuery.m_dFilters )
					tStmt.m_tQuery.m_dFilters.Add ( tHeadQuery.m_dFilters[k] );
				ARRAY_FOREACH ( k, tHeadQuery.m_dFilterTree )
					tStmt.m_tQuery.m_dFilterTree.Add ( tHeadQuery.m_dFilterTree[k] );
			}
		}
	}

	if ( bGotFacet )
	{
		// need to keep order of query items same as at select list however do not duplicate items
		// that is why raw Vector.Uniq does not work here
		CSphVector<QueryItemProxy_t> dSelectItems;
		ARRAY_FOREACH ( i, dStmt )
		{
			ARRAY_FOREACH ( k, dStmt[i].m_tQuery.m_dItems )
			{
				QueryItemProxy_t & tItem = dSelectItems.Add();
				tItem.m_pItem = dStmt[i].m_tQuery.m_dItems.Begin() + k;
				tItem.m_iIndex = dSelectItems.GetLength() - 1;
				tItem.QueryItemHash();
			}
		}
		// got rid of duplicates
		dSelectItems.Uniq();
		// sort back to select list appearance order
		dSelectItems.Sort ( bind ( &QueryItemProxy_t::m_iIndex ) );
		// get merged select list
		CSphVector<CSphQueryItem> dItems ( dSelectItems.GetLength() );
		ARRAY_FOREACH ( i, dSelectItems )
		{
			dItems[i] = *dSelectItems[i].m_pItem;
		}

		ARRAY_FOREACH ( i, dStmt )
		{
			SqlStmt_t & tStmt = dStmt[i];
			// keep original items
			tStmt.m_tQuery.m_dItems.SwapData ( dStmt[i].m_tQuery.m_dRefItems );
			tStmt.m_tQuery.m_dItems = dItems;

			// for FACET strip off group by expression items
			// these come after count(*)
			if ( tStmt.m_tQuery.m_bFacet )
			{
				ARRAY_FOREACH ( j, tStmt.m_tQuery.m_dRefItems )
				{
					if ( tStmt.m_tQuery.m_dRefItems[j].m_sAlias=="count(*)" )
					{
						tStmt.m_tQuery.m_dRefItems.Resize ( j+1 );
						break;
					}
				}
			}
		}
	}

	return true;
}

void SqlParser_c::SetIndex ( const SqlNode_t & tIndex )
{
	assert ( m_pStmt );
	ToString ( m_pStmt->m_sIndex, tIndex );

	// split ident ( cluster:index ) to parts
	if ( !m_pStmt->m_sIndex.IsEmpty() )
	{
		const char * sDelimiter = strchr ( m_pStmt->m_sIndex.cstr(), ':' );
		if ( sDelimiter )
		{
			CSphString sTmp = m_pStmt->m_sIndex; // m_sIndex.SetBinary can not accept this(m_sIndex) pointer

			int iPos = sDelimiter - m_pStmt->m_sIndex.cstr();
			int iLen = m_pStmt->m_sIndex.Length();
			m_pStmt->m_sIndex.SetBinary ( sTmp.cstr() + iPos + 1, iLen - iPos - 1 );
			m_pStmt->m_sCluster.SetBinary ( sTmp.cstr(), iPos );
		}
	}
}

/////////////////////////////////////////////////////////////////////////////

ESphSpz GetPassageBoundary ( const CSphString & sPassageBoundaryMode )
{
	if ( sPassageBoundaryMode.IsEmpty() )
		return SPH_SPZ_NONE;

	ESphSpz eSPZ = SPH_SPZ_NONE;
	if ( sPassageBoundaryMode=="sentence" )
		eSPZ = SPH_SPZ_SENTENCE;
	else if ( sPassageBoundaryMode=="paragraph" )
		eSPZ = SPH_SPZ_PARAGRAPH;
	else if ( sPassageBoundaryMode=="zone" )
		eSPZ = SPH_SPZ_ZONE;

	return eSPZ;
}

const char * PassageBoundarySz ( ESphSpz eBoundary )
{
	switch ( eBoundary )
	{
	case SPH_SPZ_SENTENCE: return "sentence";
	case SPH_SPZ_PARAGRAPH: return "paragraph";
	case SPH_SPZ_ZONE: return "zone";
	default: return "";
	}
}

bool sphCheckOptionsSPZ ( const ExcerptQuery_t & q, ESphSpz eMode, CSphString & sError )
{
	if ( q.m_ePassageSPZ )
	{
		if ( q.m_iAround==0 )
		{
			sError.SetSprintf ( "invalid combination of passage_boundary=%s and around=%d", PassageBoundarySz(eMode), q.m_iAround );
			return false;
		} else if ( q.m_bUseBoundaries )
		{
			sError.SetSprintf ( "invalid combination of passage_boundary=%s and use_boundaries", PassageBoundarySz(eMode) );
			return false;
		}
	}

	if ( q.m_bEmitZones )
	{
		if ( q.m_ePassageSPZ!=SPH_SPZ_ZONE )
		{
			sError.SetSprintf ( "invalid combination of passage_boundary=%s and emit_zones", PassageBoundarySz(eMode) );
			return false;
		}
		if ( !( q.m_sStripMode=="strip" || q.m_sStripMode=="index" ) )
		{
			sError.SetSprintf ( "invalid combination of strip=%s and emit_zones", q.m_sStripMode.cstr() );
			return false;
		}
	}

	return true;
}

/////////////////////////////////////////////////////////////////////////////
// EXCERPTS HANDLER
/////////////////////////////////////////////////////////////////////////////

enum eExcerpt_Flags
{
	EXCERPT_FLAG_REMOVESPACES		= 1,
	EXCERPT_FLAG_EXACTPHRASE		= 2,
	EXCERPT_FLAG_SINGLEPASSAGE		= 4,
	EXCERPT_FLAG_USEBOUNDARIES		= 8,
	EXCERPT_FLAG_WEIGHTORDER		= 16,
	EXCERPT_FLAG_QUERY				= 32,
	EXCERPT_FLAG_FORCE_ALL_WORDS	= 64,
	EXCERPT_FLAG_LOAD_FILES			= 128,
	EXCERPT_FLAG_ALLOW_EMPTY		= 256,
	EXCERPT_FLAG_EMIT_ZONES			= 512,
	EXCERPT_FLAG_FILES_SCATTERED	= 1024,
	EXCERPT_FLAG_FORCEPASSAGES		= 2048
};

enum
{
	PROCESSED_ITEM					= -2,
	EOF_ITEM						= -1
};

int PackAPISnippetFlags ( const ExcerptQuery_t &q, bool bOnlyScattered = false )
{
	int iRawFlags = q.m_bRemoveSpaces ? EXCERPT_FLAG_REMOVESPACES : 0;
	iRawFlags |= q.m_bExactPhrase ? EXCERPT_FLAG_EXACTPHRASE : 0;
	iRawFlags |= q.m_iLimitPassages ? EXCERPT_FLAG_SINGLEPASSAGE : 0;
	iRawFlags |= q.m_bUseBoundaries ? EXCERPT_FLAG_USEBOUNDARIES : 0;
	iRawFlags |= q.m_bWeightOrder ? EXCERPT_FLAG_WEIGHTORDER : 0;
	iRawFlags |= q.m_bHighlightQuery ? EXCERPT_FLAG_QUERY : 0;
	iRawFlags |= q.m_bForceAllWords ? EXCERPT_FLAG_FORCE_ALL_WORDS : 0;
	if ( !bOnlyScattered || !( q.m_uFilesMode & 2 ) )
		iRawFlags |= ( q.m_uFilesMode & 1 ) ? EXCERPT_FLAG_LOAD_FILES : 0;
	iRawFlags |= q.m_bAllowEmpty ? EXCERPT_FLAG_ALLOW_EMPTY : 0;
	iRawFlags |= q.m_bEmitZones ? EXCERPT_FLAG_EMIT_ZONES : 0;
	iRawFlags |= ( q.m_uFilesMode & 2 ) ? EXCERPT_FLAG_FILES_SCATTERED : 0;
	iRawFlags |= q.m_bForcePassages ? EXCERPT_FLAG_FORCEPASSAGES : 0;
	return iRawFlags;
}

struct SnippetChain_t
{
	int64_t						m_iTotal = 0;
	int							m_iHead { EOF_ITEM };
};

struct ExcerptQueryChained_t : ExcerptQuery_t
{
	int64_t m_iSize = 0;        ///< file size, to sort to work-queue order
	int m_iSeq = 0;            ///< request order, to sort back to request order
	int m_iNext = PROCESSED_ITEM; ///< the next one in one-link list for batch processing. -1 terminate the list. -2 sign of other (out-of-the-lists)
};

struct SnippetsRemote_t : ISphNoncopyable
{
	VecRefPtrsAgentConn_t			m_dAgents;
	CSphVector<SnippetChain_t>		m_dTasks;
	CSphVector<ExcerptQueryChained_t> &	m_dQueries;

	explicit SnippetsRemote_t ( CSphVector<ExcerptQueryChained_t> & dQueries )
		: m_dQueries ( dQueries )
	{}
};

struct SnippetJob_t : public ISphJob
{
	long						m_iQueries = 0;
	ExcerptQueryChained_t *		m_pQueries = nullptr;
	CSphAtomic *				m_pCurQuery = nullptr;
	CSphIndex *					m_pIndex = nullptr;
	CrashQuery_t				m_tCrashQuery;

	void Call () final
	{
		CrashQuerySet ( m_tCrashQuery ); // transfer crash info into container

		SnippetContext_t tCtx;
		tCtx.Setup ( m_pIndex, *m_pQueries, m_pQueries->m_sError );

		for ( long iQuery = (*m_pCurQuery)++; iQuery<m_iQueries; iQuery = (*m_pCurQuery)++ )
		{
			auto &dQuery = m_pQueries[iQuery];
			if ( dQuery.m_iNext!=PROCESSED_ITEM )
				continue;

			tCtx.BuildExcerpt ( dQuery, m_pIndex );
		}
	}
};


struct SnippetRequestBuilder_t : public IRequestBuilder_t
{
	explicit SnippetRequestBuilder_t ( const SnippetsRemote_t * pWorker )
		: m_pWorker ( pWorker )
	{}
	void BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const final;

private:
	const SnippetsRemote_t * m_pWorker;
	mutable CSphAtomic m_iWorker;
};


struct SnippetReplyParser_t : public IReplyParser_t
{
	explicit SnippetReplyParser_t ( SnippetsRemote_t * pWorker )
		: m_pWorker ( pWorker )
	{}

	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const final;

private:
	const SnippetsRemote_t * m_pWorker;
};


void SnippetRequestBuilder_t::BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const
{
	// it sends either all queries to each agent or sequence of queries to current agent
//	auto iWorker = ( int ) m_iWorker++;
	auto iWorker = tAgent.m_iStoreTag;
	if ( iWorker<0 )
	{
		iWorker = ( int ) m_iWorker++;
		tAgent.m_iStoreTag = iWorker;
	}
	auto & dQueries = m_pWorker->m_dQueries;
	const ExcerptQuery_t & q = dQueries[0];
	int iHead = m_pWorker->m_dTasks[iWorker].m_iHead;
	const char * sIndex = tAgent.m_tDesc.m_sIndexes.cstr();

	APICommand_t tWr { tOut, SEARCHD_COMMAND_EXCERPT, VER_COMMAND_EXCERPT };

	tOut.SendInt ( 0 );
	tOut.SendInt ( PackAPISnippetFlags ( q, true ) );
	tOut.SendString ( sIndex );
	tOut.SendString ( q.m_sWords.cstr() );
	tOut.SendString ( q.m_sBeforeMatch.cstr() );
	tOut.SendString ( q.m_sAfterMatch.cstr() );
	tOut.SendString ( q.m_sChunkSeparator.cstr() );
	tOut.SendInt ( q.m_iLimit );
	tOut.SendInt ( q.m_iAround );
	tOut.SendInt ( q.m_iLimitPassages );
	tOut.SendInt ( q.m_iLimitWords );
	tOut.SendInt ( q.m_iPassageId );
	tOut.SendString ( q.m_sStripMode.cstr() );
	tOut.SendString ( PassageBoundarySz ( q.m_ePassageSPZ ) );

	int iNumDocs = 0;
	for ( int iDoc = iHead; iDoc!=EOF_ITEM; iDoc = dQueries[iDoc].m_iNext )
		++iNumDocs;

	tOut.SendInt ( iNumDocs );
	for ( int iDoc = iHead; iDoc!=EOF_ITEM; iDoc=dQueries[iDoc].m_iNext )
		tOut.SendString ( dQueries[iDoc].m_sSource.cstr() );
}

bool SnippetReplyParser_t::ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & tAgent ) const
{
	auto & dQueries = m_pWorker->m_dQueries;
	int iDoc = m_pWorker->m_dTasks[tAgent.m_iStoreTag].m_iHead;
	bool bOk = true;
	while ( iDoc!=EOF_ITEM )
	{
		auto & dQuery = dQueries[iDoc];
		if ( dQuery.m_uFilesMode & 2 ) // scattered files
		{
			if ( !tReq.GetString ( dQuery.m_dRes ) || dQuery.m_dRes.IsEmpty () )
			{
				bOk = false;
				dQuery.m_dRes.Reset();
			} else
				dQuery.m_sError = "";

			iDoc = dQuery.m_iNext;
			continue;
		}
		tReq.GetString ( dQuery.m_dRes );
		auto iNextDoc = dQuery.m_iNext;
		dQuery.m_iNext = PROCESSED_ITEM;
		iDoc = iNextDoc;
	}

	return bOk;
}

static int64_t GetSnippetDataSize ( const CSphVector<ExcerptQueryChained_t> &dSnippets )
{
	int64_t iSize = 0;
	for ( const auto & dSnippet: dSnippets )
	{
		if ( dSnippet.m_iSize )
			iSize -= dSnippet.m_iSize; // because iSize negative for sorting purpose
		else
			iSize += dSnippet.m_sSource.Length ();
	}
	iSize /= 100;
	return iSize;
}

bool MakeSnippets ( CSphString sIndex, CSphVector<ExcerptQueryChained_t> & dQueries, CSphString & sError, ThdDesc_t & tThd )
{
	SnippetsRemote_t dRemoteSnippets ( dQueries );
	ExcerptQuery_t &q = *dQueries.begin ();

	// Both load_files && load_files_scattered report the absent files as errors.
	// load_files_scattered without load_files just omits the absent files (returns empty strings).
	auto bScattered = !!( q.m_uFilesMode & 2 );
	auto bNeedAllFiles = !!( q.m_uFilesMode & 1 );

	auto pDist = GetDistr ( sIndex );
	if ( pDist )
		for ( auto * pAgent : pDist->m_dAgents )
		{
			auto * pConn = new AgentConn_t;
			pConn->SetMultiAgent ( sIndex, pAgent );
			pConn->m_iMyConnectTimeout = pDist->m_iAgentConnectTimeout;
			pConn->m_iMyQueryTimeout = pDist->m_iAgentQueryTimeout;
			dRemoteSnippets.m_dAgents.Add ( pConn );
		}

	bool bRemote = !dRemoteSnippets.m_dAgents.IsEmpty ();
	if ( bRemote )
	{
		if ( pDist->m_dLocal.GetLength()!=1 )
		{
			sError.SetSprintf ( "%s", "The distributed index for snippets must have exactly one local agent" );
			return false;
		}

		if ( !q.m_uFilesMode )
		{
			sError.SetSprintf ( "%s", "The distributed index for snippets available only when using external files" );
			return false;
		}

		if ( g_iDistThreads<=1 && bScattered )
		{
			sError.SetSprintf ( "%s", "load_files_scattered works only together with dist_threads>1" );
			return false;
		}
		sIndex = *pDist->m_dLocal.begin();
	}

	ServedDescRPtr_c pServed ( GetServed ( sIndex ) );
	if ( !pServed || !pServed->m_pIndex )
	{
		sError.SetSprintf ( "unknown local index '%s' in search request", sIndex.cstr() );
		return false;
	}

	CSphIndex * pIndex = pServed->m_pIndex;
	assert ( pIndex );

	SnippetContext_t tCtx;
	if ( !tCtx.Setup ( pIndex, q, sError ) ) // same path for single - threaded snippets, bail out here on error
		return false;

	///////////////////
	// do highlighting
	///////////////////

	// boring single threaded loop
	StringBuilder_c sErrors ( "; " );
	if ( g_iDistThreads<=1 || dQueries.GetLength()<2 )
	{
		for ( auto & dQuery: dQueries )
		{
			tCtx.BuildExcerpt ( dQuery, pIndex );
			sErrors << dQuery.m_sError;
		}
		sErrors.MoveTo (sError);
		return sError.IsEmpty ();
	}

	// not boring mt loop with (may be) scattered.
	ARRAY_FOREACH ( i, dQueries )
		dQueries[i].m_iSeq = i;

	// collect file sizes; mark absent with EOF_ITEM
	for ( auto & dQuery : dQueries )
	{
		assert ( dQuery.m_iNext == PROCESSED_ITEM );
		if ( dQuery.m_uFilesMode )
		{
			CSphString sFilename, sStatError;
			sFilename.SetSprintf ( "%s%s", g_sSnippetsFilePrefix.cstr(), dQuery.m_sSource.scstr() );
			auto iFileSize = sphGetFileSize (sFilename, &sStatError);
			if ( iFileSize<0 )
			{
				if ( !bScattered )
				{
					sError = sStatError;
					return false;
				}
				dQuery.m_iNext = EOF_ITEM;
			} else
				dQuery.m_iSize = -iFileSize; // so that sort would put bigger ones first
		} else
			dQuery.m_iSize = -dQuery.m_sSource.Length();
	}

	// set correct data size for snippets
	ThreadSetSnippetInfo ( dQueries[0].m_sWords.scstr (), GetSnippetDataSize (dQueries), tThd );

	// tough jobs first
	if ( !bScattered )
		dQueries.Sort ( bind ( &ExcerptQueryChained_t::m_iSize ) );

	// build list of absent files (that's ok for scattered).
	// later all the list will be sent to remotes (and some of them have to answer)
	int iAbsentHead = EOF_ITEM;
	ARRAY_FOREACH ( i, dQueries )
		if ( dQueries[i].m_iNext==EOF_ITEM )
		{
			dQueries[i].m_iNext = iAbsentHead;
			iAbsentHead = i;
			if ( bNeedAllFiles )
				dQueries[i].m_sError.SetSprintf ( "absenthead: failed to stat %s: %s", dQueries[i].m_sSource.cstr(), strerrorm(errno) );
		}

	// check if all files are available locally.
	if ( bScattered && iAbsentHead==EOF_ITEM )
		bRemote = false;

	// stuff for thread-pooling
	auto * pPool = sphThreadPoolCreate ( g_iDistThreads - 1, "snippets", sError );
	if ( !pPool )
		sphWarning ( "failed to create thread_pool, single thread snippets used: %s", sError.cstr () );
	CrashQuery_t tCrashQuery = SphCrashLogger_c::GetQuery (); // transfer query info for crash logger to new thread
	CSphAtomic iCurQuery;
	CSphVector<SnippetJob_t> dThreads ( Min ( 1, g_iDistThreads ) );
	SnippetJob_t * pJobLocal = nullptr;

	// one more boring case: multithreaded, but no remote agents.
	if ( !bRemote )
	{
		// do MT searching
		for ( auto & t : dThreads )
		{
			t.m_iQueries = dQueries.GetLength ();
			t.m_pQueries = dQueries.Begin ();
			t.m_pCurQuery = &iCurQuery;
			t.m_pIndex = pIndex;
			t.m_tCrashQuery = tCrashQuery;
			if ( !pJobLocal )
				pJobLocal = &t;
			else if ( pPool )
				pPool->AddJob ( &t );
		}
		if ( pJobLocal )
			pJobLocal->Call();
		SafeDelete ( pPool );

		// back in query order
		if ( !bScattered )
			dQueries.Sort ( bind ( &ExcerptQueryChained_t::m_iSeq ) );
		dQueries.Apply ( [&] ( const ExcerptQuery_t &dQuery ) { sErrors << dQuery.m_sError; } );
		sErrors.MoveTo ( sError );
		return sError.IsEmpty ();
	}

	// and finally most interesting remote case with possibly scattered.

	int iRemoteAgents = dRemoteSnippets.m_dAgents.GetLength();
	dRemoteSnippets.m_dTasks.Resize ( iRemoteAgents );

	if ( bScattered )
	{
		// on scattered case - just push the chain of absent files to all remotes
		assert ( iAbsentHead!=EOF_ITEM ); // otherwize why we have remotes?
		for ( auto & dTask : dRemoteSnippets.m_dTasks )
			dTask.m_iHead = iAbsentHead;
	} else
	{
		// distribute queries among tasks by total task size
		ARRAY_FOREACH ( i, dQueries )
		{
			auto & dHeadTask = *dRemoteSnippets.m_dTasks.begin();
			dHeadTask.m_iTotal -= dQueries[i].m_iSize; // -= since size stored as negative.
			// queries sheduled for local still have iNext==PROCESSED_ITEM
			dQueries[i].m_iNext = dHeadTask.m_iHead;
			dHeadTask.m_iHead = i;
			dRemoteSnippets.m_dTasks.Sort ( bind ( &SnippetChain_t::m_iTotal ) );
		}
	}

	// do MT searching
	for ( auto &t : dThreads )
	{
		t.m_iQueries = dQueries.GetLength();
		t.m_pQueries = dQueries.Begin();
		t.m_pCurQuery = &iCurQuery;
		t.m_pIndex = pIndex;
		t.m_tCrashQuery = tCrashQuery;
		if ( !pJobLocal )
			pJobLocal = &t;
		else if ( pPool )
			pPool->AddJob ( &t );
	}

	// connect to remote agents and query them
	SnippetRequestBuilder_t tReqBuilder ( &dRemoteSnippets );
	SnippetReplyParser_t  tParser ( &dRemoteSnippets );
	CSphRefcountedPtr<IRemoteAgentsObserver> tReporter ( GetObserver() );
	ScheduleDistrJobs ( dRemoteSnippets.m_dAgents, &tReqBuilder, &tParser, tReporter );

	// run local worker in current thread also
	if ( pJobLocal )
		pJobLocal->Call ();

	// wait local jobs to finish
	SafeDelete ( pPool );

	// wait remotes to finish also
	tReporter->Finish ();

	auto iSuccesses = ( int ) tReporter->GetSucceeded ();
	auto iAgentsDone = ( int ) tReporter->GetFinished ();

	if ( iSuccesses!=dRemoteSnippets.m_dAgents.GetLength() )
	{
		sphWarning ( "Remote snippets: some of the agents didn't answered: %d queried, %d finished, %d succeeded",
			dRemoteSnippets.m_dAgents.GetLength(), iAgentsDone,	iSuccesses );

		if ( !bScattered )
		{
			int iFailed = 0;
			// inverse the success/failed state - so that the queries with negative m_iNext are treated as failed
			dQueries.Apply ( [&] ( ExcerptQueryChained_t &dQuery ) {
				if ( dQuery.m_iNext!=PROCESSED_ITEM )
				{
					dQuery.m_iNext = PROCESSED_ITEM;
					++iFailed;
				} else
					dQuery.m_iNext = 0;
			} );

			// failsafe - one more turn for failed queries on local agent
			if ( iFailed )
			{
				sphWarning ( "Snippets: failsafe for %d failed items", iFailed );
				auto & t = dThreads[0];
				t.m_pQueries = dQueries.Begin();
				iCurQuery = 0;
				t.Call();
			}
		}
	}

	// back in query order
	if ( !bScattered )
		dQueries.Sort ( bind ( &ExcerptQueryChained_t::m_iSeq ) );

	dQueries.Apply ( [&] ( const ExcerptQuery_t &dQuery ) { sErrors << dQuery.m_sError;});
	sErrors.MoveTo ( sError );
	return sError.IsEmpty ();
}

// throw out tailing \0 if any
inline static void FixupResultTail (CSphVector<BYTE> & dData)
{
	if ( !dData.IsEmpty() && !dData.Last () )
		dData.Pop ();
}

void HandleCommandExcerpt ( CachedOutputBuffer_c & tOut, int iVer, InputBuffer_c & tReq, ThdDesc_t & tThd )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_EXCERPT, tOut ) )
		return;

	/////////////////////////////
	// parse and process request
	/////////////////////////////

	const int EXCERPT_MAX_ENTRIES			= 1024;

	// v.1.1
	ExcerptQueryChained_t q;

	tReq.GetInt (); // mode field is for now reserved and ignored
	int iFlags = tReq.GetInt ();
	CSphString sIndex = tReq.GetString ();

	q.m_sWords = tReq.GetString ();
	q.m_sBeforeMatch = tReq.GetString ();
	q.m_sAfterMatch = tReq.GetString ();
	q.m_sChunkSeparator = tReq.GetString ();
	q.m_iLimit = tReq.GetInt ();
	q.m_iAround = tReq.GetInt ();

	if ( iVer>=0x102 )
	{
		q.m_iLimitPassages = tReq.GetInt();
		q.m_iLimitWords = tReq.GetInt();
		q.m_iPassageId = tReq.GetInt();
		q.m_sStripMode = tReq.GetString();
		if ( q.m_sStripMode!="none" && q.m_sStripMode!="index" && q.m_sStripMode!="strip" && q.m_sStripMode!="retain" )
		{
			SendErrorReply ( tOut, "unknown html_strip_mode=%s", q.m_sStripMode.cstr() );
			return;
		}
	}

	q.m_bHasBeforePassageMacro = SnippetTransformPassageMacros ( q.m_sBeforeMatch, q.m_sBeforeMatchPassage );
	q.m_bHasAfterPassageMacro = SnippetTransformPassageMacros ( q.m_sAfterMatch, q.m_sAfterMatchPassage );

	CSphString sPassageBoundaryMode;
	if ( iVer>=0x103 )
		q.m_ePassageSPZ = GetPassageBoundary ( tReq.GetString() );

	q.m_bRemoveSpaces = ( iFlags & EXCERPT_FLAG_REMOVESPACES )!=0;
	q.m_bExactPhrase = ( iFlags & EXCERPT_FLAG_EXACTPHRASE )!=0;
	q.m_bUseBoundaries = ( iFlags & EXCERPT_FLAG_USEBOUNDARIES )!=0;
	q.m_bWeightOrder = ( iFlags & EXCERPT_FLAG_WEIGHTORDER )!=0;
	q.m_bHighlightQuery = ( iFlags & EXCERPT_FLAG_QUERY )!=0;
	q.m_bForceAllWords = ( iFlags & EXCERPT_FLAG_FORCE_ALL_WORDS )!=0;
	if ( iFlags & EXCERPT_FLAG_SINGLEPASSAGE )
		q.m_iLimitPassages = 1;
	q.m_uFilesMode = ( iFlags & EXCERPT_FLAG_LOAD_FILES )?1:0;
	bool bScattered = ( iFlags & EXCERPT_FLAG_FILES_SCATTERED )!=0;
	q.m_uFilesMode |= bScattered?2:0;
	q.m_bAllowEmpty = ( iFlags & EXCERPT_FLAG_ALLOW_EMPTY )!=0;
	q.m_bEmitZones = ( iFlags & EXCERPT_FLAG_EMIT_ZONES )!=0;
	q.m_bForcePassages = ( iFlags & EXCERPT_FLAG_FORCEPASSAGES )!=0;

	int iCount = tReq.GetInt ();
	if ( iCount<=0 || iCount>EXCERPT_MAX_ENTRIES )
	{
		SendErrorReply ( tOut, "invalid entries count %d", iCount );
		return;
	}

	CSphString sError;

	if ( !sphCheckOptionsSPZ ( q, q.m_ePassageSPZ, sError ) )
	{
		SendErrorReply ( tOut, "%s", sError.cstr() );
		return;
	}

	CSphVector<ExcerptQueryChained_t> dQueries { iCount };

	for ( auto & dQuery : dQueries )
	{
		dQuery = q; // copy settings
		dQuery.m_sSource = tReq.GetString (); // fetch data
		if ( tReq.GetError() )
		{
			SendErrorReply ( tOut, "invalid or truncated request" );
			return;
		}
	}
	ThreadSetSnippetInfo ( dQueries[0].m_sWords.scstr (), GetSnippetDataSize ( dQueries ), false, tThd );

	if ( !MakeSnippets ( sIndex, dQueries, sError, tThd ) )
	{
		SendErrorReply ( tOut, "%s", sError.cstr() );
		return;
	}

	////////////////
	// serve result
	////////////////

	for ( auto & dQuery : dQueries )
	{
		auto &dData = dQuery.m_dRes;
		FixupResultTail ( dData );
		// handle errors
		if ( !bScattered && dData.IsEmpty() && !dQuery.m_sError.IsEmpty () )
		{
			SendErrorReply ( tOut, "highlighting failed: %s", dQuery.m_sError.cstr() );
			return;
		}
	}

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_EXCERPT );
	for ( const auto& dQuery : dQueries )
		tOut.SendArray ( dQuery.m_dRes );
}

/////////////////////////////////////////////////////////////////////////////
// KEYWORDS HANDLER
/////////////////////////////////////////////////////////////////////////////

static bool DoGetKeywords ( const CSphString & sIndex, const CSphString & sQuery, const GetKeywordsSettings_t & tSettings, CSphVector <CSphKeywordInfo> & dKeywords, CSphString & sError, SearchFailuresLog_c & tFailureLog );

void HandleCommandKeywords ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_KEYWORDS, tOut ) )
		return;

	GetKeywordsSettings_t tSettings;
	CSphString sQuery = tReq.GetString ();
	CSphString sIndex = tReq.GetString ();
	tSettings.m_bStats = !!tReq.GetInt ();
	if ( uVer>=0x101 )
	{
		tSettings.m_bFoldLemmas = !!tReq.GetInt ();
		tSettings.m_bFoldBlended = !!tReq.GetInt ();
		tSettings.m_bFoldWildcards = !!tReq.GetInt ();
		tSettings.m_iExpansionLimit = tReq.GetInt ();
	}

	CSphString sError;
	SearchFailuresLog_c tFailureLog;
	CSphVector < CSphKeywordInfo > dKeywords;
	bool bOk = DoGetKeywords ( sIndex, sQuery, tSettings, dKeywords, sError, tFailureLog );
	if ( !bOk )
	{
		SendErrorReply ( tOut, "%s", sError.cstr() );
		return;
	}
	// just log distribute index error as command has no warning filed to pass such error into
	if ( !tFailureLog.IsEmpty() )
	{
		StringBuilder_c sErrorBuf;
		tFailureLog.BuildReport ( sErrorBuf );
		sphWarning ( "%s", sErrorBuf.cstr() );
	}

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_KEYWORDS );
	tOut.SendInt ( dKeywords.GetLength () );
	for ( auto & dKeyword : dKeywords )
	{
		tOut.SendString ( dKeyword.m_sTokenized.cstr () );
		tOut.SendString ( dKeyword.m_sNormalized.cstr () );
		if ( uVer>=0x101 )
			tOut.SendInt ( dKeyword.m_iQpos );
		if ( tSettings.m_bStats )
		{
			tOut.SendInt ( dKeyword.m_iDocs );
			tOut.SendInt ( dKeyword.m_iHits );
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
// UPDATES HANDLER
/////////////////////////////////////////////////////////////////////////////

struct UpdateRequestBuilder_t : public IRequestBuilder_t
{
	explicit UpdateRequestBuilder_t ( const CSphAttrUpdate & pUpd ) : m_tUpd ( pUpd ) {}
	void BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c& tOut ) const final;

protected:
	const CSphAttrUpdate & m_tUpd;
};


struct UpdateReplyParser_t : public IReplyParser_t
{
	explicit UpdateReplyParser_t ( int * pUpd )
		: m_pUpdated ( pUpd )
	{}

	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const final
	{
		*m_pUpdated += tReq.GetDword ();
		return true;
	}

protected:
	int * m_pUpdated;
};


void UpdateRequestBuilder_t::BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const
{
	const char * sIndexes = tAgent.m_tDesc.m_sIndexes.cstr();
	bool bMva = false;
	ARRAY_FOREACH ( i, m_tUpd.m_dTypes )
	{
		assert ( m_tUpd.m_dTypes[i]!=SPH_ATTR_INT64SET ); // mva64 goes only via SphinxQL (SphinxqlRequestBuilder_c)
		bMva |= ( m_tUpd.m_dTypes[i]==SPH_ATTR_UINT32SET );
	}

	// API header
	APICommand_t tWr { tOut, SEARCHD_COMMAND_UPDATE, VER_COMMAND_UPDATE };

	tOut.SendString ( sIndexes );
	tOut.SendInt ( m_tUpd.m_dAttrs.GetLength() );
	tOut.SendInt ( m_tUpd.m_bIgnoreNonexistent ? 1 : 0 );
	ARRAY_FOREACH ( i, m_tUpd.m_dAttrs )
	{
		tOut.SendString ( m_tUpd.m_dAttrs[i] );
		tOut.SendInt ( ( m_tUpd.m_dTypes[i]==SPH_ATTR_UINT32SET ) ? 1 : 0 );
	}
	tOut.SendInt ( m_tUpd.m_dDocids.GetLength() );

	if ( !bMva )
	{
		ARRAY_FOREACH ( iDoc, m_tUpd.m_dDocids )
		{
			tOut.SendUint64 ( m_tUpd.m_dDocids[iDoc] );
			int iHead = m_tUpd.m_dRowOffset[iDoc];
			int iTail = m_tUpd.m_dPool.GetLength ();
			if ( (iDoc+1)<m_tUpd.m_dDocids.GetLength() )
				iTail = m_tUpd.m_dRowOffset[iDoc+1];

			for ( int j=iHead; j<iTail; j++ )
				tOut.SendDword ( m_tUpd.m_dPool[j] );
		}
	} else
	{
		// size down in case of MVA
		// MVA stored as mva64 in pool but API could handle only mva32 due to HandleCommandUpdate
		// SphinxQL only could work either mva32 or mva64 and only SphinxQL could receive mva64 updates
		// SphinxQL master communicate to agent via SphinxqlRequestBuilder_c

		ARRAY_FOREACH ( iDoc, m_tUpd.m_dDocids )
		{
			tOut.SendUint64 ( m_tUpd.m_dDocids[iDoc] );

			const DWORD * pPool = m_tUpd.m_dPool.Begin() + m_tUpd.m_dRowOffset[iDoc];
			ARRAY_FOREACH ( iAttr, m_tUpd.m_dTypes )
			{
				DWORD uVal = *pPool++;
				if ( m_tUpd.m_dTypes[iAttr]!=SPH_ATTR_UINT32SET )
				{
					tOut.SendDword ( uVal );
				} else
				{
					const DWORD * pEnd = pPool + uVal;
					tOut.SendDword ( uVal/2 );
					while ( pPool<pEnd )
					{
						tOut.SendDword ( *pPool );
						pPool += 2;
					}
				}
			}
		}
	}
}

static void DoCommandUpdate ( const char * sIndex, const char * sDistributed, const CSphAttrUpdate & tUpd,
	int & iSuccesses, int & iUpdated,
	SearchFailuresLog_c & dFails, const ServedDesc_t * pServed )
{
	if ( !pServed )
	{
		dFails.Submit ( sIndex, sDistributed, "index not available" );
		return;
	}

	CSphString sError, sWarning;
	int iUpd = pServed->m_pIndex->UpdateAttributes ( tUpd, -1, sError, sWarning );
	if ( iUpd<0 )
	{
		dFails.Submit ( sIndex, sDistributed, sError.cstr() );

	} else
	{
		iUpdated += iUpd;
		++iSuccesses;
		if ( sWarning.Length() )
			dFails.Submit ( sIndex, sDistributed, sWarning.cstr() );
	}
}

using DistrPtrs_t = VecRefPtrs_t< const DistributedIndex_t *>;
static bool ExtractDistributedIndexes ( const StrVec_t &dNames, DistrPtrs_t &dDistributed, CSphString& sMissed )
{
	dDistributed.Reset();
	dDistributed.Resize( dNames.GetLength () );
	dDistributed.ZeroMem ();

	ARRAY_FOREACH ( i, dNames )
	{
		if ( !g_pLocalIndexes->Contains ( dNames[i] ) )
		{
			// search amongst distributed and copy for further processing
			dDistributed[i] = GetDistr ( dNames[i] );

			if ( !dDistributed[i] )
			{
				sMissed = dNames[i];
				return false;
			}
			dDistributed[i]->AddRef ();
		}
	}
	return true;
}

void HandleCommandUpdate ( CachedOutputBuffer_c & tOut, int iVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( iVer, VER_COMMAND_UPDATE, tOut ) )
		return;

	// parse request
	CSphString sIndexes = tReq.GetString ();
	CSphAttrUpdate tUpd;
	CSphVector<DWORD> dMva;

	bool bMvaUpdate = false;

	tUpd.m_dAttrs.Resize ( tReq.GetDword() ); // FIXME! check this
	tUpd.m_dTypes.Resize ( tUpd.m_dAttrs.GetLength() );
	if ( iVer>=0x103 )
		tUpd.m_bIgnoreNonexistent = ( tReq.GetDword() & 1 )!=0;
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		tUpd.m_dAttrs[i] = tReq.GetString().ToLower().Leak();
		tUpd.m_dTypes[i] = SPH_ATTR_INTEGER;
		if ( iVer>=0x102 )
		{
			if ( tReq.GetDword() )
			{
				tUpd.m_dTypes[i] = SPH_ATTR_UINT32SET;
				bMvaUpdate = true;
			}
		}
	}

	int iNumUpdates = tReq.GetInt (); // FIXME! check this
	tUpd.m_dDocids.Reserve ( iNumUpdates );
	tUpd.m_dRowOffset.Reserve ( iNumUpdates );

	for ( int i=0; i<iNumUpdates; i++ )
	{
		// v.1.0 always sends 32-bit ids; v.1.1+ always send 64-bit ones
		uint64_t uDocid = ( iVer>=0x101 ) ? tReq.GetUint64 () : tReq.GetDword ();

		tUpd.m_dDocids.Add ( (SphDocID_t)uDocid ); // FIXME! check this
		tUpd.m_dRowOffset.Add ( tUpd.m_dPool.GetLength() );

		ARRAY_FOREACH ( iAttr, tUpd.m_dAttrs )
		{
			if ( tUpd.m_dTypes[iAttr]==SPH_ATTR_UINT32SET )
			{
				DWORD uCount = tReq.GetDword ();
				if ( !uCount )
				{
					tUpd.m_dPool.Add ( 0 );
					continue;
				}

				dMva.Resize ( uCount );
				for ( DWORD j=0; j<uCount; j++ )
				{
					dMva[j] = tReq.GetDword();
				}
				dMva.Uniq(); // don't need dupes within MVA

				tUpd.m_dPool.Add ( dMva.GetLength()*2 );
				ARRAY_FOREACH ( j, dMva )
				{
					tUpd.m_dPool.Add ( dMva[j] );
					tUpd.m_dPool.Add ( 0 ); // dummy expander mva32 -> mva64
				}
			} else
			{
				tUpd.m_dPool.Add ( tReq.GetDword() );
			}
		}
	}

	if ( tReq.GetError() )
	{
		SendErrorReply ( tOut, "invalid or truncated request" );
		return;
	}

	// check index names
	StrVec_t dIndexNames;
	ParseIndexList ( sIndexes, dIndexNames );

	if ( !dIndexNames.GetLength() )
	{
		SendErrorReply ( tOut, "no valid indexes in update request" );
		return;
	}

	DistrPtrs_t dDistributed;
	// copy distributed indexes description
	CSphString sMissed;
	if ( !ExtractDistributedIndexes ( dIndexNames, dDistributed, sMissed ) )
	{
		SendErrorReply ( tOut, "unknown index '%s' in update request", sMissed.cstr() );
		return;
	}

	// do update
	SearchFailuresLog_c dFails;
	int iSuccesses = 0;
	int iUpdated = 0;
	tUpd.m_dRows.Resize ( tUpd.m_dDocids.GetLength() );
	for ( auto& pRow : tUpd.m_dRows )
		pRow = nullptr;

	ARRAY_FOREACH ( iIdx, dIndexNames )
	{
		const char * sReqIndex = dIndexNames[iIdx].cstr();
		auto pLocal = GetServed ( sReqIndex );
		if ( pLocal )
		{
			if ( bMvaUpdate )
				DoCommandUpdate ( sReqIndex, nullptr, tUpd, iSuccesses,
					iUpdated, dFails, ServedDescWPtr_c ( pLocal ) );
			else
				DoCommandUpdate ( sReqIndex, nullptr, tUpd, iSuccesses,
					iUpdated, dFails, ServedDescRPtr_c ( pLocal ) );
		} else if ( dDistributed[iIdx] )
		{
			auto * pDist = dDistributed[iIdx];

			assert ( !pDist->IsEmpty() );

			for ( const auto & sLocal : pDist->m_dLocal )
			{
				auto pServed = GetServed ( sLocal );
				if ( pServed )
				{
					if ( bMvaUpdate )
						DoCommandUpdate ( sLocal.cstr (), sReqIndex, tUpd, iSuccesses,
							iUpdated, dFails, ServedDescWPtr_c ( pServed ) );
					else
						DoCommandUpdate ( sLocal.cstr (), sReqIndex, tUpd,
							iSuccesses, iUpdated, dFails, ServedDescRPtr_c ( pServed ) );
				}
			}

			// update remote agents
			if ( !dDistributed[iIdx]->m_dAgents.IsEmpty() )
			{
				VecRefPtrsAgentConn_t dAgents;
				pDist->GetAllHosts ( dAgents );

				// connect to remote agents and query them
				UpdateRequestBuilder_t tReqBuilder ( tUpd );
				UpdateReplyParser_t tParser ( &iUpdated );
				iSuccesses += PerformRemoteTasks ( dAgents, &tReqBuilder, &tParser );
			}
		}
	}

	// serve reply to client
	StringBuilder_c sReport;
	dFails.BuildReport ( sReport );

	if ( !iSuccesses )
	{
		SendErrorReply ( tOut, "%s", sReport.cstr() );
		return;
	}

	APICommand_t dOk ( tOut, dFails.IsEmpty() ? SEARCHD_OK : SEARCHD_WARNING, VER_COMMAND_UPDATE );
	if ( !dFails.IsEmpty() )
		tOut.SendString ( sReport.cstr () );
	tOut.SendInt ( iUpdated );
}

// 'like' matcher
CheckLike::CheckLike ( const char * sPattern )
{
	if ( !sPattern )
		return;

	m_sPattern.Reserve ( 2*strlen ( sPattern ) );
	char * d = const_cast<char*> ( m_sPattern.cstr() );

	// remap from SQL LIKE syntax to Sphinx wildcards syntax
	// '_' maps to '?', match any single char
	// '%' maps to '*', match zero or mor chars
	for ( const char * s = sPattern; *s; s++ )
	{
		switch ( *s )
		{
			case '_':	*d++ = '?'; break;
			case '%':	*d++ = '*'; break;
			case '?':	*d++ = '\\'; *d++ = '?'; break;
			case '*':	*d++ = '\\'; *d++ = '*'; break;
			default:	*d++ = *s; break;
		}
	}
	*d = '\0';
}

bool CheckLike::Match ( const char * sValue )
{
	return sValue && ( m_sPattern.IsEmpty() || sphWildcardMatch ( sValue, m_sPattern.cstr() ) );
}

// string vector with 'like' matcher
VectorLike::VectorLike ()
	: CheckLike ( NULL )
{}

VectorLike::VectorLike ( const CSphString& sPattern )
	: CheckLike ( sPattern.cstr() )
	, m_sColKey ( "Variable_name" )
	, m_sColValue ( "Value" )
{}

const char * VectorLike::szColKey() const
{
	return m_sColKey.cstr();
}

const char * VectorLike::szColValue() const
{
	return m_sColValue.cstr();
}

bool VectorLike::MatchAdd ( const char* sValue )
{
	if ( Match ( sValue ) )
	{
		Add ( sValue );
		return true;
	}
	return false;
}

bool VectorLike::MatchAddVa ( const char * sTemplate, ... )
{
	va_list ap;
	CSphString sValue;

	va_start ( ap, sTemplate );
	sValue.SetSprintfVa ( sTemplate, ap );
	va_end ( ap );

	return MatchAdd ( sValue.cstr() );
}

//////////////////////////////////////////////////////////////////////////
// STATUS HANDLER
//////////////////////////////////////////////////////////////////////////

static inline void FormatMsec ( CSphString & sOut, int64_t tmTime )
{
	sOut.SetSprintf ( "%d.%03d", (int)( tmTime/1000000 ), (int)( (tmTime%1000000)/1000 ) );
}

void BuildStatus ( VectorLike & dStatus )
{
	const char * FMT64 = INT64_FMT;
	const char * FLOAT = "%.2f";
	const char * OFF = "OFF";

	const int64_t iQueriesDiv = Max ( g_tStats.m_iQueries.GetValue(), 1 );
	const int64_t iDistQueriesDiv = Max ( g_tStats.m_iDistQueries.GetValue(), 1 );

	dStatus.m_sColKey = "Counter";

	// FIXME? non-transactional!!!
	if ( dStatus.MatchAdd ( "uptime" ) )
		dStatus.Add().SetSprintf ( "%u", (DWORD)time(NULL)-g_tStats.m_uStarted );
	if ( dStatus.MatchAdd ( "connections" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iConnections );
	if ( dStatus.MatchAdd ( "maxed_out" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iMaxedOut );
	if ( dStatus.MatchAdd ( "version" ) )
		dStatus.Add().SetSprintf ( "%s", SPHINX_VERSION );
	if ( dStatus.MatchAdd ( "mysql_version" ) )
		dStatus.Add().SetSprintf ( "%s", g_sMySQLVersion.cstr() );
	if ( dStatus.MatchAdd ( "command_search" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_SEARCH] );
	if ( dStatus.MatchAdd ( "command_excerpt" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_EXCERPT] );
	if ( dStatus.MatchAdd ( "command_update" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_UPDATE] );
	if ( dStatus.MatchAdd ( "command_delete" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_DELETE] );
	if ( dStatus.MatchAdd ( "command_keywords" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_KEYWORDS] );
	if ( dStatus.MatchAdd ( "command_persist" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_PERSIST] );
	if ( dStatus.MatchAdd ( "command_status" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_STATUS] );
	if ( dStatus.MatchAdd ( "command_flushattrs" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_FLUSHATTRS] );
	if ( dStatus.MatchAdd ( "command_set" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_UVAR] );
	if ( dStatus.MatchAdd ( "command_insert" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_INSERT] );
	if ( dStatus.MatchAdd ( "command_replace" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_REPLACE] );
	if ( dStatus.MatchAdd ( "command_commit" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_COMMIT] );
	if ( dStatus.MatchAdd ( "command_suggest" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_SUGGEST] );
	if ( dStatus.MatchAdd ( "command_json" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_JSON] );
	if ( dStatus.MatchAdd ( "command_callpq" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iCommandCount[SEARCHD_COMMAND_CALLPQ] );
	if ( dStatus.MatchAdd ( "agent_connect" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iAgentConnect );
	if ( dStatus.MatchAdd ( "agent_retry" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iAgentRetry );
	if ( dStatus.MatchAdd ( "queries" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iQueries );
	if ( dStatus.MatchAdd ( "dist_queries" ) )
		dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iDistQueries );

	// status of thread pool
	if ( g_pThdPool )
	{
		if ( dStatus.MatchAdd ( "workers_total" ) )
			dStatus.Add().SetSprintf ( "%d", g_pThdPool->GetTotalWorkerCount() );
		if ( dStatus.MatchAdd ( "workers_active" ) )
			dStatus.Add().SetSprintf ( "%d", g_pThdPool->GetActiveWorkerCount() );
		if ( dStatus.MatchAdd ( "work_queue_length" ) )
			dStatus.Add().SetSprintf ( "%d", g_pThdPool->GetQueueLength() );
	}

	for ( RLockedDistrIt_c it ( g_pDistIndexes ); it.Next (); )
	{
		const char * sIdx = it.GetName().cstr();
		const auto &dAgents = it.Get ()->m_dAgents;
		ARRAY_FOREACH ( i, dAgents )
		{
			MultiAgentDesc_c &dMultiAgent = *dAgents[i];
			ARRAY_FOREACH ( j, dMultiAgent )
			{
				const auto pDash = dMultiAgent[j].m_pStats;
				for ( int k=0; k<eMaxAgentStat; ++k )
					if ( dStatus.MatchAddVa ( "ag_%s_%d_%d_%s", sIdx, i+1, j+1, sAgentStatsNames[k] ) )
						dStatus.Add().SetSprintf ( FMT64, (int64_t) pDash->m_dCounters[k] );
				for ( int k = 0; k<ehMaxStat; ++k )
					if ( dStatus.MatchAddVa ( "ag_%s_%d_%d_%s", sIdx, i + 1, j + 1, sAgentStatsNames[eMaxAgentStat+k] ) )
					{
						if ( k==ehTotalMsecs || k==ehAverageMsecs || k==ehMaxMsecs )
							dStatus.Add ().SetSprintf ( FLOAT, (float) pDash->m_dMetrics[k] / 1000.0 );
						else
							dStatus.Add ().SetSprintf ( FMT64, (int64_t) pDash->m_dMetrics[k] );
					}

			}
		}
	}

	if ( dStatus.MatchAdd ( "query_wall" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iQueryTime );

	if ( dStatus.MatchAdd ( "query_cpu" ) )
	{
		if ( g_bCpuStats )
			FormatMsec ( dStatus.Add(), g_tStats.m_iQueryCpuTime );
		else
			dStatus.Add() = OFF;
	}

	if ( dStatus.MatchAdd ( "dist_wall" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistWallTime );
	if ( dStatus.MatchAdd ( "dist_local" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistLocalTime );
	if ( dStatus.MatchAdd ( "dist_wait" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistWaitTime );

	if ( g_bIOStats )
	{
		if ( dStatus.MatchAdd ( "query_reads" ) )
			dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iDiskReads );
		if ( dStatus.MatchAdd ( "query_readkb" ) )
			dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iDiskReadBytes/1024 );
		if ( dStatus.MatchAdd ( "query_readtime" ) )
			FormatMsec ( dStatus.Add(), g_tStats.m_iDiskReadTime );
	} else
	{
		if ( dStatus.MatchAdd ( "query_reads" ) )
			dStatus.Add() = OFF;
		if ( dStatus.MatchAdd ( "query_readkb" ) )
			dStatus.Add() = OFF;
		if ( dStatus.MatchAdd ( "query_readtime" ) )
			dStatus.Add() = OFF;
	}

	if ( g_tStats.m_iPredictedTime || g_tStats.m_iAgentPredictedTime )
	{
		if ( dStatus.MatchAdd ( "predicted_time" ) )
			dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iPredictedTime );
		if ( dStatus.MatchAdd ( "dist_predicted_time" ) )
			dStatus.Add().SetSprintf ( FMT64, (int64_t) g_tStats.m_iAgentPredictedTime );
	}

	if ( dStatus.MatchAdd ( "avg_query_wall" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iQueryTime / iQueriesDiv );
	if ( dStatus.MatchAdd ( "avg_query_cpu" ) )
	{
		if ( g_bCpuStats )
			FormatMsec ( dStatus.Add(), g_tStats.m_iQueryCpuTime / iQueriesDiv );
		else
			dStatus.Add ( OFF );
	}

	if ( dStatus.MatchAdd ( "avg_dist_wall" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistWallTime / iDistQueriesDiv );
	if ( dStatus.MatchAdd ( "avg_dist_local" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistLocalTime / iDistQueriesDiv );
	if ( dStatus.MatchAdd ( "avg_dist_wait" ) )
		FormatMsec ( dStatus.Add(), g_tStats.m_iDistWaitTime / iDistQueriesDiv );
	if ( g_bIOStats )
	{
		if ( dStatus.MatchAdd ( "avg_query_reads" ) )
			dStatus.Add().SetSprintf ( "%.1f", (float)( g_tStats.m_iDiskReads*10/iQueriesDiv )/10.0f );
		if ( dStatus.MatchAdd ( "avg_query_readkb" ) )
			dStatus.Add().SetSprintf ( "%.1f", (float)( g_tStats.m_iDiskReadBytes/iQueriesDiv )/1024.0f );
		if ( dStatus.MatchAdd ( "avg_query_readtime" ) )
			FormatMsec ( dStatus.Add(), g_tStats.m_iDiskReadTime/iQueriesDiv );
	} else
	{
		if ( dStatus.MatchAdd ( "avg_query_reads" ) )
			dStatus.Add() = OFF;
		if ( dStatus.MatchAdd ( "avg_query_readkb" ) )
			dStatus.Add() = OFF;
		if ( dStatus.MatchAdd ( "avg_query_readtime" ) )
			dStatus.Add() = OFF;
	}

	const QcacheStatus_t & s = QcacheGetStatus();
	if ( dStatus.MatchAdd ( "qcache_max_bytes" ) )
		dStatus.Add().SetSprintf ( INT64_FMT, s.m_iMaxBytes );
	if ( dStatus.MatchAdd ( "qcache_thresh_msec" ) )
		dStatus.Add().SetSprintf ( "%d", s.m_iThreshMsec );
	if ( dStatus.MatchAdd ( "qcache_ttl_sec" ) )
		dStatus.Add().SetSprintf ( "%d", s.m_iTtlSec );

	if ( dStatus.MatchAdd ( "qcache_cached_queries" ) )
		dStatus.Add().SetSprintf ( "%d", s.m_iCachedQueries );
	if ( dStatus.MatchAdd ( "qcache_used_bytes" ) )
		dStatus.Add().SetSprintf ( INT64_FMT, s.m_iUsedBytes );
	if ( dStatus.MatchAdd ( "qcache_hits" ) )
		dStatus.Add().SetSprintf ( INT64_FMT, s.m_iHits );

	// clusters
	ReplicateClustersStatus ( dStatus );
}

void BuildOneAgentStatus ( VectorLike & dStatus, HostDashboard_t* pDash, const char * sPrefix="agent" )
{
	assert ( pDash );

	const char * FMT64 = UINT64_FMT;
	const char * FLOAT = "%.2f";

	{
		CSphScopedRLock tGuard ( pDash->m_dDataLock );
		if ( dStatus.MatchAddVa ( "%s_hostname", sPrefix ) )
			dStatus.Add().SetSprintf ( "%s", pDash->m_tHost.GetMyUrl().cstr() );

		if ( dStatus.MatchAddVa ( "%s_references", sPrefix ) )
			dStatus.Add().SetSprintf ( "%d", (int) pDash->GetRefcount()-1 ); // -1 since we currently also 'use' the agent, reading it's stats
		if ( dStatus.MatchAddVa ( "%s_ping", sPrefix ) )
			dStatus.Add ().SetSprintf ( "%s", pDash->m_iNeedPing ? "yes" : "no" );
		if ( dStatus.MatchAddVa ( "%s_has_perspool", sPrefix ) )
			dStatus.Add ().SetSprintf ( "%s", pDash->m_pPersPool ? "yes" : "no" );
		if ( dStatus.MatchAddVa ( "%s_need_resolve", sPrefix ) )
			dStatus.Add ().SetSprintf ( "%s", pDash->m_tHost.m_bNeedResolve ? "yes" : "no" );
		uint64_t iCur = sphMicroTimer();
		uint64_t iLastAccess = iCur - pDash->m_iLastQueryTime;
		float fPeriod = (float)iLastAccess/1000000.0f;
		if ( dStatus.MatchAddVa ( "%s_lastquery", sPrefix ) )
			dStatus.Add().SetSprintf ( FLOAT, fPeriod );
		iLastAccess = iCur - pDash->m_iLastAnswerTime;
		fPeriod = (float)iLastAccess/1000000.0f;
		if ( dStatus.MatchAddVa ( "%s_lastanswer", sPrefix ) )
			dStatus.Add().SetSprintf ( FLOAT, fPeriod );
		uint64_t iLastTimer = pDash->m_iLastAnswerTime-pDash->m_iLastQueryTime;
		if ( dStatus.MatchAddVa ( "%s_lastperiodmsec", sPrefix ) )
			dStatus.Add().SetSprintf ( FMT64, iLastTimer/1000 );
		if ( dStatus.MatchAddVa ( "%s_errorsarow", sPrefix ) )
			dStatus.Add().SetSprintf ( FMT64, pDash->m_iErrorsARow );
	}
	int iPeriods = 1;

	while ( iPeriods>0 )
	{
		HostStatSnapshot_t dDashStat;
		pDash->GetCollectedStat ( dDashStat, iPeriods );
		{
			for ( int j = 0; j<ehMaxStat+eMaxAgentStat; ++j )
				// hack. Avoid microseconds in human-readable statistic
				if ( j==ehTotalMsecs && dStatus.MatchAddVa ( "%s_%dperiods_msecsperqueryy", sPrefix, iPeriods ) )
				{
					if ( dDashStat[ehConnTries]>0 )
						dStatus.Add ().SetSprintf ( FLOAT, (float) ((dDashStat[ehTotalMsecs] / 1000.0)
																	/ dDashStat[ehConnTries]) );
					else
						dStatus.Add ( "n/a" );
				} else if ( dStatus.MatchAddVa ( "%s_%dperiods_%s", sPrefix, iPeriods, sAgentStatsNames[j] ) )
				{
					if ( j==ehMaxMsecs || j==ehAverageMsecs )
						dStatus.Add ().SetSprintf ( FLOAT, (float) dDashStat[j] / 1000.0);
					else
						dStatus.Add ().SetSprintf ( FMT64, dDashStat[j] );
				}
		}

		if ( iPeriods==1 )
			iPeriods = 5;
		else if ( iPeriods==5 )
			iPeriods = STATS_DASH_PERIODS;
		else if ( iPeriods==STATS_DASH_PERIODS )
			iPeriods = -1;
	}
}

static bool BuildDistIndexStatus ( VectorLike & dStatus, const CSphString& sIndex )
{
	auto pDistr = GetDistr ( sIndex );
	if ( !pDistr )
		return false;

	ARRAY_FOREACH ( i, pDistr->m_dLocal )
	{
		if ( dStatus.MatchAddVa ( "dstindex_local_%d", i+1 ) )
			dStatus.Add ( pDistr->m_dLocal[i].cstr() );
	}

	CSphString sKey;
	ARRAY_FOREACH ( i, pDistr->m_dAgents )
	{
		MultiAgentDesc_c & tAgents = *pDistr->m_dAgents[i];
		if ( dStatus.MatchAddVa ( "dstindex_%d_is_ha", i+1 ) )
			dStatus.Add ( tAgents.IsHA()? "1": "0" );

		auto dWeights = tAgents.GetWeights ();

		ARRAY_FOREACH ( j, tAgents )
		{
			if ( tAgents.IsHA() )
				sKey.SetSprintf ( "dstindex_%dmirror%d", i+1, j+1 );
			else
				sKey.SetSprintf ( "dstindex_%dagent", i+1 );

			const AgentDesc_t & dDesc = tAgents[j];

			if ( dStatus.MatchAddVa ( "%s_id", sKey.cstr() ) )
				dStatus.Add().SetSprintf ( "%s:%s", dDesc.GetMyUrl().cstr(), dDesc.m_sIndexes.cstr() );

			if ( tAgents.IsHA() && dStatus.MatchAddVa ( "%s_probability_weight", sKey.cstr() ) )
				dStatus.Add ().SetSprintf ( "%0.2f%%", dWeights[j] );

			if ( dStatus.MatchAddVa ( "%s_is_blackhole", sKey.cstr() ) )
				dStatus.Add ( dDesc.m_bBlackhole ? "1" : "0" );

			if ( dStatus.MatchAddVa ( "%s_is_persistent", sKey.cstr() ) )
				dStatus.Add ( dDesc.m_bPersistent ? "1" : "0" );
		}
	}
	return true;
}

void BuildAgentStatus ( VectorLike &dStatus, const CSphString& sIndexOrAgent )
{
	if ( !sIndexOrAgent.IsEmpty() )
	{
		if ( !BuildDistIndexStatus ( dStatus, sIndexOrAgent ) )
		{
			auto pAgent = g_tDashes.FindAgent ( sIndexOrAgent );
			if ( !pAgent )
			{
				if ( dStatus.MatchAdd ( "status_error" ) )
					dStatus.Add ().SetSprintf ( "No such distr index or agent: %s", sIndexOrAgent.cstr () );
				return;
			}
			BuildOneAgentStatus ( dStatus, pAgent );
		}
		return;
	}

	dStatus.m_sColKey = "Key";

	if ( dStatus.MatchAdd ( "status_period_seconds" ) )
		dStatus.Add().SetSprintf ( "%d", g_uHAPeriodKarma );
	if ( dStatus.MatchAdd ( "status_stored_periods" ) )
		dStatus.Add().SetSprintf ( "%d", STATS_DASH_PERIODS );

	VecRefPtrs_t<HostDashboard_t *> dDashes;
	g_tDashes.GetActiveDashes ( dDashes );

	CSphString sPrefix;
	ARRAY_FOREACH ( i, dDashes )
	{
		sPrefix.SetSprintf ( "ag_%d", i );
		BuildOneAgentStatus ( dStatus, dDashes[i], sPrefix.cstr() );
	}
}

static void AddIOStatsToMeta ( VectorLike & dStatus, const CSphIOStats & tStats, const char * sPrefix )
{
	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_read_time" ) )
		dStatus.Add().SetSprintf ( "%d.%03d", (int)( tStats.m_iReadTime/1000 ), (int)( tStats.m_iReadTime%1000 ) );

	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_read_ops" ) )
		dStatus.Add().SetSprintf ( "%u", tStats.m_iReadOps );

	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_read_kbytes" ) )
		dStatus.Add().SetSprintf ( "%d.%d", (int)( tStats.m_iReadBytes/1024 ), (int)( tStats.m_iReadBytes%1024 )/100 );

	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_write_time" ) )
		dStatus.Add().SetSprintf ( "%d.%03d", (int)( tStats.m_iWriteTime/1000 ), (int)( tStats.m_iWriteTime%1000 ) );

	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_write_ops" ) )
		dStatus.Add().SetSprintf ( "%u", tStats.m_iWriteOps );

	if ( dStatus.MatchAddVa ( "%s%s", sPrefix, "io_write_kbytes" ) )
		dStatus.Add().SetSprintf ( "%d.%d", (int)( tStats.m_iWriteBytes/1024 ), (int)( tStats.m_iWriteBytes%1024 )/100 );
}

void BuildMeta ( VectorLike & dStatus, const CSphQueryResultMeta & tMeta )
{
	if ( !tMeta.m_sError.IsEmpty() && dStatus.MatchAdd ( "error" ) )
		dStatus.Add ( tMeta.m_sError );

	if ( !tMeta.m_sWarning.IsEmpty() && dStatus.MatchAdd ( "warning" ) )
		dStatus.Add ( tMeta.m_sWarning );

	if ( dStatus.MatchAdd ( "total" ) )
		dStatus.Add().SetSprintf ( "%d", tMeta.m_iMatches );

	if ( dStatus.MatchAdd ( "total_found" ) )
		dStatus.Add().SetSprintf ( INT64_FMT, tMeta.m_iTotalMatches );

	if ( dStatus.MatchAdd ( "time" ) )
		dStatus.Add().SetSprintf ( "%d.%03d", tMeta.m_iQueryTime/1000, tMeta.m_iQueryTime%1000 );

	if ( tMeta.m_iMultiplier>1 && dStatus.MatchAdd ( "multiplier" ) )
		dStatus.Add().SetSprintf ( "%d", tMeta.m_iMultiplier );

	if ( g_bCpuStats )
	{
		if ( dStatus.MatchAdd ( "cpu_time" ) )
			dStatus.Add().SetSprintf ( "%d.%03d", (int)( tMeta.m_iCpuTime/1000 ), (int)( tMeta.m_iCpuTime%1000 ) );

		if ( dStatus.MatchAdd ( "agents_cpu_time" ) )
			dStatus.Add().SetSprintf ( "%d.%03d", (int)( tMeta.m_iAgentCpuTime/1000 ), (int)( tMeta.m_iAgentCpuTime%1000 ) );
	}

	if ( g_bIOStats )
	{
		AddIOStatsToMeta ( dStatus, tMeta.m_tIOStats, "" );
		AddIOStatsToMeta ( dStatus, tMeta.m_tAgentIOStats, "agent_" );
	}

	if ( tMeta.m_bHasPrediction )
	{
		if ( dStatus.MatchAdd ( "local_fetched_docs" ) )
			dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iFetchedDocs );
		if ( dStatus.MatchAdd ( "local_fetched_hits" ) )
			dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iFetchedHits );
		if ( dStatus.MatchAdd ( "local_fetched_skips" ) )
			dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iSkips );

		if ( dStatus.MatchAdd ( "predicted_time" ) )
			dStatus.Add().SetSprintf ( "%lld", INT64 ( tMeta.m_iPredictedTime ) );
		if ( tMeta.m_iAgentPredictedTime && dStatus.MatchAdd ( "dist_predicted_time" ) )
			dStatus.Add().SetSprintf ( "%lld", INT64 ( tMeta.m_iAgentPredictedTime ) );
		if ( tMeta.m_iAgentFetchedDocs || tMeta.m_iAgentFetchedHits || tMeta.m_iAgentFetchedSkips )
		{
			if ( dStatus.MatchAdd ( "dist_fetched_docs" ) )
				dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iFetchedDocs + tMeta.m_iAgentFetchedDocs );
			if ( dStatus.MatchAdd ( "dist_fetched_hits" ) )
				dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iFetchedHits + tMeta.m_iAgentFetchedHits );
			if ( dStatus.MatchAdd ( "dist_fetched_skips" ) )
				dStatus.Add().SetSprintf ( "%d", tMeta.m_tStats.m_iSkips + tMeta.m_iAgentFetchedSkips );
		}
	}


	int iWord = 0;
	// multiple readers might iterate word hash here from multiple client queries
	// that invalidates internal hash iterator - need external iterator
	void * pWordIt =  nullptr;
	while ( tMeta.m_hWordStats.IterateNext( &pWordIt ) )
	{
		const CSphQueryResultMeta::WordStat_t & tStat = tMeta.m_hWordStats.IterateGet ( &pWordIt );

		if ( dStatus.MatchAddVa ( "keyword[%d]", iWord ) )
			dStatus.Add ( tMeta.m_hWordStats.IterateGetKey ( &pWordIt ) );

		if ( dStatus.MatchAddVa ( "docs[%d]", iWord ) )
			dStatus.Add().SetSprintf ( INT64_FMT, tStat.m_iDocs );

		if ( dStatus.MatchAddVa ( "hits[%d]", iWord ) )
			dStatus.Add().SetSprintf ( INT64_FMT, tStat.m_iHits );

		++iWord;
	}
}


void HandleCommandStatus ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_STATUS, tOut ) )
		return;

	bool bGlobalStat = tReq.GetDword ()!=0;

	VectorLike dStatus;

	if ( bGlobalStat )
		BuildStatus ( dStatus );
	else
	{
		ScRL_t dMetaRlock ( g_tLastMetaLock );
		BuildMeta ( dStatus, g_tLastMeta );
		if ( g_tStats.m_iPredictedTime || g_tStats.m_iAgentPredictedTime )
		{
			if ( dStatus.MatchAdd ( "predicted_time" ) )
				dStatus.Add().SetSprintf ( INT64_FMT, (int64_t) g_tStats.m_iPredictedTime );
			if ( dStatus.MatchAdd ( "dist_predicted_time" ) )
				dStatus.Add().SetSprintf ( INT64_FMT, (int64_t) g_tStats.m_iAgentPredictedTime );
		}
	}

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_STATUS );
	tOut.SendInt ( dStatus.GetLength()/2 ); // rows
	tOut.SendInt ( 2 ); // cols
	for ( const auto & dLines : dStatus )
		tOut.SendString ( dLines.cstr() );
}

//////////////////////////////////////////////////////////////////////////
// FLUSH HANDLER
//////////////////////////////////////////////////////////////////////////

static int CommandFlush ()
{
	// force a check in head process, and wait it until completes
	// FIXME! semi active wait..
	sphLogDebug ( "attrflush: forcing check, tag=%d", g_tFlush.m_iFlushTag );
	g_tFlush.m_bForceCheck = true;
	while ( g_tFlush.m_bForceCheck )
		sphSleepMsec ( 1 );

	// if we are flushing now, wait until flush completes
	while ( g_tFlush.m_bFlushing )
		sphSleepMsec ( 10 );
	sphLogDebug ( "attrflush: check finished, tag=%d", g_tFlush.m_iFlushTag );

	return g_tFlush.m_iFlushTag;
}

void HandleCommandFlush ( CachedOutputBuffer_c & tOut, WORD uVer )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_FLUSHATTRS, tOut ) )
		return;

	int iTag = CommandFlush ();
	// return last flush tag, just for the fun of it
	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_FLUSHATTRS );
	tOut.SendInt ( iTag );
}


/////////////////////////////////////////////////////////////////////////////
// GENERAL HANDLER
/////////////////////////////////////////////////////////////////////////////

void HandleCommandSphinxql ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq, ThdDesc_t & tThd ); // definition is below
void HandleCommandJson ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq, ThdDesc_t & tThd );
void StatCountCommand ( SearchdCommand_e eCmd );
void HandleCommandUserVar ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq );
void HandleCommandCallPq ( CachedOutputBuffer_c &tOut, WORD uVer, InputBuffer_c &tReq );

/// ping/pong exchange over API
void HandleCommandPing ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_PING, tOut ) )
		return;

	// parse ping
	int iCookie = tReq.GetInt();
	if ( tReq.GetError () )
		return;

	// return last flush tag, just for the fun of it
	APICommand_t tHeader ( tOut, SEARCHD_OK, VER_COMMAND_PING ); // API header
	tOut.SendInt ( iCookie ); // echo the cookie back
}

static bool LoopClientSphinx ( SearchdCommand_e eCommand, WORD uCommandVer,
	int iLength, ThdDesc_t & tThd,
	InputBuffer_c & tBuf, CachedOutputBuffer_c & tOut, bool bManagePersist );

static void HandleClientSphinx ( int iSock, ThdDesc_t & tThd ) REQUIRES ( HandlerThread )
{
	if ( iSock<0 )
	{
		sphWarning ( "invalid socket passed to HandleClientSphinx" );
		return;
	}

	MEMORY ( MEM_API_HANDLE );
	ThdState ( THD_HANDSHAKE, tThd );

	int64_t iCID = tThd.m_iConnID;
	const char * sClientIP = tThd.m_sClientName.cstr();

	// send my version
	DWORD uServer = htonl ( SPHINX_SEARCHD_PROTO );
	if ( sphSockSend ( iSock, (char*)&uServer, sizeof(uServer) )!=sizeof(uServer) )
	{
		sphWarning ( "failed to send server version (client=%s(" INT64_FMT "))", sClientIP, iCID );
		return;
	}

	// get client version and request
	int iMagic = 0;
	int iGot = SockReadFast ( iSock, &iMagic, sizeof(iMagic), g_iReadTimeout );

	bool bReadErr = ( iGot!=sizeof(iMagic) );
	sphLogDebugv ( "conn %s(" INT64_FMT "): got handshake, major v.%d, err %d", sClientIP, iCID, iMagic, (int)bReadErr );
	if ( bReadErr )
	{
		sphLogDebugv ( "conn %s(" INT64_FMT "): exiting on handshake error", sClientIP, iCID );
		return;
	}

	bool bPersist = false;
	int iPconnIdle = 0;
	do
	{
		NetInputBuffer_c tBuf ( iSock );
		NetOutputBuffer_c tOut ( iSock );

		int iTimeout = bPersist ? 1 : g_iReadTimeout;

		// in "persistent connection" mode, we want interruptible waits
		// so that the worker child could be forcibly restarted
		//
		// currently, the only signal allowed to interrupt this read is SIGTERM
		// letting SIGHUP interrupt causes trouble under query/rotation pressure
		// see sphSockRead() and ReadFrom() for details
		ThdState ( THD_NET_IDLE, tThd );
		bool bCommand = tBuf.ReadFrom ( 8, iTimeout, bPersist );

		// on SIGTERM, bail unconditionally and immediately, at all times
		if ( !bCommand && g_bGotSigterm )
		{
			sphLogDebugv ( "conn %s(" INT64_FMT "): bailing on SIGTERM", sClientIP, iCID );
			break;
		}

		// on SIGHUP vs pconn, bail if a pconn was idle for 1 sec
		if ( bPersist && !bCommand && g_bGotSighup && sphSockPeekErrno()==ETIMEDOUT )
		{
			sphLogDebugv ( "conn %s(" INT64_FMT "): bailing idle pconn on SIGHUP", sClientIP, iCID );
			break;
		}

		// on pconn that was idle for 300 sec (client_timeout), bail
		if ( bPersist && !bCommand && sphSockPeekErrno()==ETIMEDOUT )
		{
			iPconnIdle += iTimeout;
			bool bClientTimedout = ( iPconnIdle>=g_iClientTimeout );
			if ( bClientTimedout )
				sphLogDebugv ( "conn %s(" INT64_FMT "): bailing idle pconn on client_timeout", sClientIP, iCID );

			if ( !bClientTimedout )
				continue;
			else
				break;
		}
		iPconnIdle = 0;

		// on any other signals vs pconn, ignore and keep looping
		// (redundant for now, as the only allowed interruption is SIGTERM, but.. let's keep it)
		if ( bPersist && !bCommand && tBuf.IsIntr() )
			continue;

		// okay, signal related mess should be over, try to parse the command
		// (but some other socket error still might had happened, so beware)
		ThdState ( THD_NET_READ, tThd );
		auto eCommand = ( SearchdCommand_e ) tBuf.GetWord ();
		WORD uCommandVer = tBuf.GetWord ();
		int iLength = tBuf.GetInt ();
		if ( tBuf.GetError() )
		{
			// under high load, there can be pretty frequent accept() vs connect() timeouts
			// lets avoid agent log flood
			//
			// sphWarning ( "failed to receive client version and request (client=%s, error=%s)", sClientIP, sphSockError() );
			sphLogDebugv ( "conn %s(" INT64_FMT "): bailing on failed request header (sockerr=%s)", sClientIP, iCID, sphSockError() );
			break;
		}

		// check request
		if ( eCommand>=SEARCHD_COMMAND_WRONG || iLength<0 || iLength>g_iMaxPacketSize )
		{
			// unknown command, default response header
			SendErrorReply ( tOut, "invalid command (code=%d, len=%d)", eCommand, iLength );
			tOut.Flush ();

			// if request length is insane, low level comm is broken, so we bail out
			if ( iLength<0 || iLength>g_iMaxPacketSize )
				sphWarning ( "ill-formed client request (length=%d out of bounds)", iLength );

			// if command is insane, low level comm is broken, so we bail out
			if ( eCommand>=SEARCHD_COMMAND_WRONG )
				sphWarning ( "ill-formed client request (command=%d, SEARCHD_COMMAND_TOTAL=%d)", eCommand, SEARCHD_COMMAND_TOTAL );

			break;
		}

		// get request body
		assert ( iLength>=0 && iLength<=g_iMaxPacketSize );
		if ( iLength && !tBuf.ReadFrom ( iLength ) )
		{
			sphWarning ( "failed to receive client request body (client=%s(" INT64_FMT "), exp=%d, error='%s')", sClientIP, iCID, iLength, sphSockError() );
			break;
		}

		bPersist |= LoopClientSphinx ( eCommand, uCommandVer, iLength, tThd, tBuf, tOut, true );
		tOut.Flush();
	} while ( bPersist );

	if ( bPersist )
		g_iPersistentInUse.Dec();

	sphLogDebugv ( "conn %s(" INT64_FMT "): exiting", sClientIP, iCID );
}


bool LoopClientSphinx ( SearchdCommand_e eCommand, WORD uCommandVer, int iLength,
	ThdDesc_t & tThd, InputBuffer_c & tBuf, CachedOutputBuffer_c & tOut, bool bManagePersist )
{
	// set on query guard
	CrashQuery_t tCrashQuery;
	tCrashQuery.m_pQuery = tBuf.GetBufferPtr();
	tCrashQuery.m_iSize = iLength;
	tCrashQuery.m_bMySQL = false;
	tCrashQuery.m_uCMD = eCommand;
	tCrashQuery.m_uVer = uCommandVer;
	SphCrashLogger_c::SetLastQuery ( tCrashQuery );

	// handle known commands
	assert ( eCommand<SEARCHD_COMMAND_WRONG );

	// count commands
	StatCountCommand ( eCommand );

	tThd.m_sCommand = g_dApiCommands[eCommand];
	ThdState ( THD_QUERY, tThd );

	bool bPersist = false;
	sphLogDebugv ( "conn %s(%d): got command %d, handling", tThd.m_sClientName.cstr(), tThd.m_iConnID, eCommand );
	switch ( eCommand )
	{
		case SEARCHD_COMMAND_SEARCH:	HandleCommandSearch ( tOut, uCommandVer, tBuf, tThd ); break;
		case SEARCHD_COMMAND_EXCERPT:	HandleCommandExcerpt ( tOut, uCommandVer, tBuf, tThd ); break;
		case SEARCHD_COMMAND_KEYWORDS:	HandleCommandKeywords ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_UPDATE:	HandleCommandUpdate ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_PERSIST:
			{
				bPersist = ( tBuf.GetInt()!=0 );
				sphLogDebugv ( "conn %s(%d): pconn is now %s", tThd.m_sClientName.cstr(), tThd.m_iConnID, bPersist ? "on" : "off" );
				if ( !bManagePersist ) // thread pool handles all persist connections
					break;

				// FIXME!!! remove that mess
				if ( bPersist )
				{
					bPersist = ( g_iMaxChildren && 1+g_iPersistentInUse.GetValue()<g_iMaxChildren );
					if ( bPersist )
						g_iPersistentInUse.Inc();
				} else
				{
					if ( g_iPersistentInUse.GetValue()>=1 )
						g_iPersistentInUse.Dec();
				}
			}
			break;
		case SEARCHD_COMMAND_STATUS:	HandleCommandStatus ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_FLUSHATTRS:HandleCommandFlush ( tOut, uCommandVer ); break;
		case SEARCHD_COMMAND_SPHINXQL:	HandleCommandSphinxql ( tOut, uCommandVer, tBuf, tThd ); break;
		case SEARCHD_COMMAND_JSON:		HandleCommandJson ( tOut, uCommandVer, tBuf, tThd ); break;
		case SEARCHD_COMMAND_PING:		HandleCommandPing ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_UVAR:		HandleCommandUserVar ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_CALLPQ:	HandleCommandCallPq ( tOut, uCommandVer, tBuf ); break;
		case SEARCHD_COMMAND_CLUSTERPQ:	HandleCommandClusterPq ( tOut, uCommandVer, tBuf, tThd.m_sClientName.cstr() ); break;
		default:						assert ( 0 && "INTERNAL ERROR: unhandled command" ); break;
	}

	// set off query guard
	SphCrashLogger_c::SetLastQuery ( CrashQuery_t() );
	return bPersist;
}

//////////////////////////////////////////////////////////////////////////
// MYSQLD PRETENDER
//////////////////////////////////////////////////////////////////////////

// our copy of enum_field_types
// we can't rely on mysql_com.h because it might be unavailable
//
// MYSQL_TYPE_DECIMAL = 0
// MYSQL_TYPE_TINY = 1
// MYSQL_TYPE_SHORT = 2
// MYSQL_TYPE_LONG = 3
// MYSQL_TYPE_FLOAT = 4
// MYSQL_TYPE_DOUBLE = 5
// MYSQL_TYPE_NULL = 6
// MYSQL_TYPE_TIMESTAMP = 7
// MYSQL_TYPE_LONGLONG = 8
// MYSQL_TYPE_INT24 = 9
// MYSQL_TYPE_DATE = 10
// MYSQL_TYPE_TIME = 11
// MYSQL_TYPE_DATETIME = 12
// MYSQL_TYPE_YEAR = 13
// MYSQL_TYPE_NEWDATE = 14
// MYSQL_TYPE_VARCHAR = 15
// MYSQL_TYPE_BIT = 16
// MYSQL_TYPE_NEWDECIMAL = 246
// MYSQL_TYPE_ENUM = 247
// MYSQL_TYPE_SET = 248
// MYSQL_TYPE_TINY_BLOB = 249
// MYSQL_TYPE_MEDIUM_BLOB = 250
// MYSQL_TYPE_LONG_BLOB = 251
// MYSQL_TYPE_BLOB = 252
// MYSQL_TYPE_VAR_STRING = 253
// MYSQL_TYPE_STRING = 254
// MYSQL_TYPE_GEOMETRY = 255

enum MysqlColumnType_e
{
	MYSQL_COL_DECIMAL	= 0,
	MYSQL_COL_LONG		= 3,
	MYSQL_COL_FLOAT	= 4,
	MYSQL_COL_LONGLONG	= 8,
	MYSQL_COL_STRING	= 254
};

enum MysqlColumnFlag_e
{
	MYSQL_COL_UNSIGNED_FLAG = 32
};

#define SPH_MYSQL_ERROR_MAX_LENGTH 512

void SendMysqlErrorPacket ( ISphOutputBuffer & tOut, BYTE uPacketID, const char * sStmt,
	const char * sError, int iCID, MysqlErrors_e iErr )
{
	if ( sError==NULL )
		sError = "(null)";

	LogSphinxqlError ( sStmt, sError, iCID );

	int iErrorLen = strlen(sError)+1; // including the trailing zero

	// cut the error message to fix isseue with long message for popular clients
	if ( iErrorLen>SPH_MYSQL_ERROR_MAX_LENGTH )
	{
		iErrorLen = SPH_MYSQL_ERROR_MAX_LENGTH;
		char * sErr = const_cast<char *>( sError );
		sErr[iErrorLen-3] = '.';
		sErr[iErrorLen-2] = '.';
		sErr[iErrorLen-1] = '.';
		sErr[iErrorLen] = '\0';
	}
	int iLen = 9 + iErrorLen;
	int iError = iErr; // pretend to be mysql syntax error for now

	// send packet header
	tOut.SendLSBDword ( (uPacketID<<24) + iLen );
	tOut.SendByte ( 0xff ); // field count, always 0xff for error packet
	tOut.SendByte ( (BYTE)( iError & 0xff ) );
	tOut.SendByte ( (BYTE)( iError>>8 ) );

	// send sqlstate (1 byte marker, 5 byte state)
	switch ( iErr )
	{
		case MYSQL_ERR_SERVER_SHUTDOWN:
		case MYSQL_ERR_UNKNOWN_COM_ERROR:
			tOut.SendBytes ( "#08S01", 6 );
			break;
		case MYSQL_ERR_NO_SUCH_TABLE:
			tOut.SendBytes ( "#42S02", 6 );
			break;
		default:
			tOut.SendBytes ( "#42000", 6 );
			break;
	}

	// send error message
	tOut.SendBytes ( sError, iErrorLen );
}

void SendMysqlEofPacket ( ISphOutputBuffer & tOut, BYTE uPacketID, int iWarns, bool bMoreResults, bool bAutoCommit )
{
	if ( iWarns<0 ) iWarns = 0;
	if ( iWarns>65535 ) iWarns = 65535;
	if ( bMoreResults )
		iWarns |= ( SPH_MYSQL_FLAG_MORE_RESULTS<<16 );
	if ( bAutoCommit )
		iWarns |= ( SPH_MYSQL_FLAG_STATUS_AUTOCOMMIT<<16 );

	tOut.SendLSBDword ( (uPacketID<<24) + 5 );
	tOut.SendByte ( 0xfe );
	tOut.SendLSBDword ( iWarns ); // N warnings, 0 status
}


// was defaults ( ISphOutputBuffer & tOut, BYTE uPacketID, int iAffectedRows=0, int iWarns=0, const char * sMessage=nullptr, bool bMoreResults=false, bool bAutoCommit )
void SendMysqlOkPacket ( ISphOutputBuffer & tOut, BYTE uPacketID, int iAffectedRows, int iWarns, const char * sMessage, bool bMoreResults, bool bAutoCommit )
{
	DWORD iInsert_id = 0;
	BYTE sVarLen[20] = {0}; // max 18 for packed number, +1 more just for fun
	BYTE * pBuf = sVarLen;
	pBuf = MysqlPackInt ( pBuf, iAffectedRows );
	pBuf = MysqlPackInt ( pBuf, iInsert_id );
	int iLen = pBuf - sVarLen;

	int iMsgLen = 0;
	if ( sMessage )
		iMsgLen = strlen(sMessage) + 1; // FIXME! does or doesn't the trailing zero necessary in Ok packet?

	tOut.SendLSBDword ( DWORD (uPacketID<<24) + iLen + iMsgLen + 5);
	tOut.SendByte ( 0 );				// ok packet
	tOut.SendBytes ( sVarLen, iLen );	// packed affected rows & insert_id
	if ( iWarns<0 ) iWarns = 0;
	if ( iWarns>65535 ) iWarns = 65535;
	DWORD uWarnStatus = ( iWarns<<16 );
	// order of WORDs is opposite to EOF packet above
	if ( bMoreResults )
		uWarnStatus |= SPH_MYSQL_FLAG_MORE_RESULTS;
	if ( bAutoCommit )
		uWarnStatus |= SPH_MYSQL_FLAG_STATUS_AUTOCOMMIT;

	tOut.SendLSBDword ( uWarnStatus );		// 0 status, N warnings
	if ( sMessage )
		tOut.SendBytes ( sMessage, iMsgLen );
}


void SendMysqlOkPacket ( ISphOutputBuffer & tOut, BYTE uPacketID, bool bAutoCommit )
{
	SendMysqlOkPacket ( tOut, uPacketID, 0, 0, nullptr, false, bAutoCommit );
}


//////////////////////////////////////////////////////////////////////////
// Mysql row buffer and command handler

// filter interface for named data
class IDataTupleter
{
public:
	virtual ~IDataTupleter();
	virtual void DataTuplet ( const char *, const char *) = 0;
	virtual void DataTuplet ( const char *, int64_t ) = 0;
};

IDataTupleter::~IDataTupleter() = default;

#define SPH_MAX_NUMERIC_STR 64
class SqlRowBuffer_c : public ISphNoncopyable, public IDataTupleter, private LazyVector_T<BYTE>
{
	using BaseVec = LazyVector_T<BYTE>;
	BYTE &m_uPacketID;
	ISphOutputBuffer &m_tOut;
	int m_iCID; // connection ID for error report
	bool m_bAutoCommit = false;
#ifndef NDEBUG
	size_t m_iColumns = 0; // used for head/data columns num sanitize check
#endif

	// how many bytes this int will occupy in proto mysql
	static inline int SqlSizeOf ( int iLen )
	{
		if ( iLen<251 )
			return 1;
		if ( iLen<=0xffff )
			return 3;
		if ( iLen<=0xffffff )
			return 4;
		return 9;
	}

	// how many bytes this string will occupy in proto mysql
	int SqlStrlen ( const char * sStr )
	{
		auto iLen = ( int ) strlen ( sStr );
		return SqlSizeOf ( iLen ) + iLen;
	}

	void SendSqlInt ( int iVal )
	{
		BYTE dBuf[12];
		auto * pBuf = MysqlPackInt ( dBuf, iVal );
		m_tOut.SendBytes ( dBuf, ( int ) ( pBuf - dBuf ) );
	}

	void SendSqlString ( const char * sStr )
	{
		int iLen = strlen ( sStr );
		SendSqlInt ( iLen );
		m_tOut.SendBytes ( sStr, iLen );
	}

	void SendSqlFieldPacket ( const char * sCol, MysqlColumnType_e eType, WORD uFlags )
	{
		const char * sDB = "";
		const char * sTable = "";

		int iLen = 17 + SqlStrlen ( sDB ) + 2 * ( SqlStrlen ( sTable ) + SqlStrlen ( sCol ) );

		int iColLen = 0;
		switch ( eType )
		{
		case MYSQL_COL_DECIMAL: iColLen = 20;
			break;
		case MYSQL_COL_LONG: iColLen = 11;
			break;
		case MYSQL_COL_FLOAT: iColLen = 20;
			break;
		case MYSQL_COL_LONGLONG: iColLen = 20;
			break;
		case MYSQL_COL_STRING: iColLen = 255;
			break;
		}

		m_tOut.SendLSBDword ( ( ( m_uPacketID++ ) << 24 ) + iLen );
		SendSqlString ( "def" ); // catalog
		SendSqlString ( sDB ); // db
		SendSqlString ( sTable ); // table
		SendSqlString ( sTable ); // org_table
		SendSqlString ( sCol ); // name
		SendSqlString ( sCol ); // org_name

		m_tOut.SendByte ( 12 ); // filler, must be 12 (following pseudo-string length)
		m_tOut.SendByte ( 0x21 ); // charset_nr, 0x21 is utf8
		m_tOut.SendByte ( 0 ); // charset_nr
		m_tOut.SendLSBDword ( iColLen ); // length
		m_tOut.SendByte ( BYTE ( eType ) ); // type (0=decimal)
		m_tOut.SendByte ( uFlags & 255 );
		m_tOut.SendByte ( uFlags >> 8 );
		m_tOut.SendByte ( 0 ); // decimals
		m_tOut.SendWord ( 0 ); // filler
	}

public:

	SqlRowBuffer_c ( BYTE * pPacketID, ISphOutputBuffer * pOut, int iCID, bool bAutoCommit )
		: m_uPacketID ( *pPacketID )
		, m_tOut ( *pOut )
		, m_iCID ( iCID )
		, m_bAutoCommit ( bAutoCommit )
	{}

	using BaseVec::Add;

	void PutFloatAsString ( float fVal, const char * sFormat = nullptr )
	{
		ReserveGap ( SPH_MAX_NUMERIC_STR );
		auto pSize = End();
		int iLen = sFormat
			? snprintf (( char* ) pSize + 1, SPH_MAX_NUMERIC_STR - 1, sFormat, fVal )
			: sph::PrintVarFloat (( char* ) pSize + 1, fVal );
		*pSize = BYTE ( iLen );
		AddN ( iLen + 1 );
	}

	template < typename NUM >
	void PutNumAsString ( NUM tVal )
	{
		ReserveGap ( SPH_MAX_NUMERIC_STR );
		auto pSize = End();
		int iLen = sph::NtoA ( ( char * ) pSize + 1, tVal );
		*pSize = BYTE ( iLen );
		AddN ( iLen + 1 );
	}


	// pack raw array (i.e. packed length, then blob) into proto mysql
	void PutArray ( const void * pBlob, int iLen, bool bSendEmpty = false )
	{
		if ( iLen<0 )
			return;

		if ( !iLen && bSendEmpty )
		{
			PutNULL();
			return;
		}

		ReserveGap ( iLen + 9 ); // 9 is taken from MysqlPack() implementation (max possible offset)
		auto * pStr = MysqlPackInt ( End(), iLen );
		if ( iLen )
			memcpy ( pStr, pBlob, iLen );
		Resize ( Idx ( pStr ) + iLen );
	}

	void PutArray ( const VecTraits_T<BYTE> &dData )
	{
		PutArray ( ( const char * ) dData.begin (), dData.GetLength () );
	}

	void PutArray ( const StringBuilder_c& dData, bool bSendEmpty = true )
	{
		PutArray ( ( const char * ) dData.begin (), dData.GetLength (), bSendEmpty );
	}

	// pack zero-terminated string (or "" if it is zero itself)
	// const void * used to avoid explicit const char* <> const BYTE* casts
	void PutString ( const void * _sMsg )
	{
		auto sMsg = (const char*)_sMsg;
		int iLen = ( sMsg && *sMsg ) ? ( int ) strlen ( sMsg ) : 0;

		if (!sMsg)
			sMsg = "";

		PutArray ( sMsg, iLen );
	}

	inline void PutString ( const CSphString& sMsg )
	{
		PutString ( sMsg.cstr() );
	}

	void PutMicrosec ( int64_t iUsec )
	{
		iUsec = Max ( iUsec, 0 );

		ReserveGap ( SPH_MAX_NUMERIC_STR+1 );
		auto pSize = (char*) End();
		int iLen = sph::IFtoA ( pSize + 1, iUsec, 6 );
		*pSize = BYTE ( iLen );
		AddN ( iLen + 1 );
	}

	void PutNULL ()
	{
		Add ( 0xfb ); // MySQL NULL is 0xfb at VLB length
	}

	// popular pattern of 2 columns of data
	void DataTuplet ( const char * pLeft, const char * pRight ) override
	{
		PutString ( pLeft );
		PutString ( pRight );
		Commit ();
	}

	void DataTuplet ( const char * pLeft, int64_t iRight ) override
	{
		PutString ( pLeft );
		PutNumAsString (iRight);
		Commit();
	}

	template <typename NUM>
	void DataTuplet ( const char * pLeft, NUM iRight )
	{
		PutString ( pLeft );
		PutNumAsString ( iRight );
		Commit ();
	}

	void DataTupletf ( const char * pLeft, const char * sFmt, ... )
	{
		StringBuilder_c sRight;
		PutString ( pLeft );
		va_list ap;
		va_start ( ap, sFmt );
		sRight.vSprintf ( sFmt, ap );
		va_end ( ap );
		PutString ( sRight.cstr() );
		Commit();
	}

public:
	/// more high level. Processing the whole tables.
	// sends collected data, then reset
	void Commit()
	{
		m_tOut.SendLSBDword ( ((m_uPacketID++)<<24) + ( GetLength() ) );
		m_tOut.SendBytes ( *this );
		Resize(0);
	}

	// wrappers for popular packets
	inline void Eof ( bool bMoreResults=false, int iWarns=0 )
	{
		SendMysqlEofPacket ( m_tOut, m_uPacketID++, iWarns, bMoreResults, m_bAutoCommit );
	}

	inline void Error ( const char * sStmt, const char * sError, MysqlErrors_e iErr = MYSQL_ERR_PARSE_ERROR )
	{
		SendMysqlErrorPacket ( m_tOut, m_uPacketID, sStmt, sError, m_iCID, iErr );
	}

	inline void ErrorEx ( MysqlErrors_e iErr, const char * sTemplate, ... )
	{
		char sBuf[1024];
		va_list ap;

		va_start ( ap, sTemplate );
		vsnprintf ( sBuf, sizeof(sBuf), sTemplate, ap );
		va_end ( ap );

		Error ( nullptr, sBuf, iErr );
	}

	inline void Ok ( int iAffectedRows=0, int iWarns=0, const char * sMessage=NULL, bool bMoreResults=false )
	{
		SendMysqlOkPacket ( m_tOut, m_uPacketID, iAffectedRows, iWarns, sMessage, bMoreResults, m_bAutoCommit );
		if ( bMoreResults )
			m_uPacketID++;
	}

	// Header of the table with defined num of columns
	inline void HeadBegin ( int iColumns )
	{
		m_tOut.SendLSBDword ( ((m_uPacketID++)<<24) + SqlSizeOf ( iColumns ) );
		SendSqlInt ( iColumns );
#ifndef NDEBUG
		m_iColumns = iColumns;
#endif
	}

	inline void HeadEnd ( bool bMoreResults=false, int iWarns=0 )
	{
		Eof ( bMoreResults, iWarns );
		Resize(0);
	}

	// add the next column. The EOF after the tull set will be fired automatically
	inline void HeadColumn ( const char * sName, MysqlColumnType_e uType=MYSQL_COL_STRING, WORD uFlags=0 )
	{
		assert ( m_iColumns-->0 && "you try to send more mysql columns than declared in InitHead" );
		SendSqlFieldPacket ( sName, uType, uFlags );
	}

	// Fire he header for table with iSize string columns
	inline void HeadOfStrings ( const char ** ppNames, size_t iSize )
	{
		HeadBegin(iSize);
		for ( ; iSize>0 ; --iSize )
			HeadColumn ( *ppNames++ );
		HeadEnd();
	}

	// table of 2 columns (we really often use them!)
	inline void HeadTuplet ( const char * pLeft, const char * pRight )
	{
		HeadBegin(2);
		HeadColumn(pLeft);
		HeadColumn(pRight);
		HeadEnd();
	}
};

class TableLike : public CheckLike
				, public ISphNoncopyable
				, public IDataTupleter
{
	SqlRowBuffer_c & m_tOut;
public:

	explicit TableLike ( SqlRowBuffer_c & tOut, const char * sPattern = nullptr )
		: CheckLike ( sPattern )
		, m_tOut ( tOut )
	{}

	bool MatchAdd ( const char* sValue )
	{
		if ( Match ( sValue ) )
		{
			m_tOut.PutString ( sValue );
			return true;
		}
		return false;
	}

	bool MatchAddVa ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) )
	{
		va_list ap;
		CSphString sValue;

		va_start ( ap, sTemplate );
		sValue.SetSprintfVa ( sTemplate, ap );
		va_end ( ap );

		return MatchAdd ( sValue.cstr() );
	}

	// popular pattern of 2 columns of data
	inline void MatchDataTuplet ( const char * pLeft, const char * pRight )
	{
		if ( Match ( pLeft ) )
			m_tOut.DataTuplet ( pLeft, pRight );
	}

	inline void MatchDataTuplet ( const char * pLeft, int64_t iRight )
	{
		if ( Match ( pLeft ) )
			m_tOut.DataTuplet ( pLeft, iRight );
	}

	// popular pattern of 2 columns of data
	void DataTuplet ( const char * pLeft, const char * pRight ) override
	{
		MatchDataTuplet ( pLeft, pRight );
	}

	void DataTuplet ( const char * pLeft, int64_t iRight ) override
	{
		MatchDataTuplet ( pLeft, iRight );
	}
};


class StmtErrorReporter_c : public StmtErrorReporter_i
{
public:
	explicit StmtErrorReporter_c ( SqlRowBuffer_c & tBuffer )
		: m_tRowBuffer ( tBuffer )
	{}

	void Ok ( int iAffectedRows, const CSphString & sWarning ) final
	{
		m_tRowBuffer.Ok ( iAffectedRows, sWarning.IsEmpty() ? 0 : 1 );
	}

	void Ok ( int iAffectedRows, int nWarnings ) final
	{
		m_tRowBuffer.Ok ( iAffectedRows, nWarnings );
	}

	void Error ( const char * sStmt, const char * sError, MysqlErrors_e iErr ) final
	{
		m_tRowBuffer.Error ( sStmt, sError, iErr );
	}

	SqlRowBuffer_c * GetBuffer() final { return &m_tRowBuffer; }

private:
	SqlRowBuffer_c & m_tRowBuffer;
};

bool PercolateParseFilters ( const char * sFilters, ESphCollation eCollation, const CSphSchema & tSchema,
	CSphVector<CSphFilterSettings> & dFilters, CSphVector<FilterTreeItem_t> & dFilterTree, CSphString & sError )
{
	if ( !sFilters || !*sFilters )
		return true;

	StringBuilder_c sBuf;
	sBuf << "sysfilters " << sFilters;
	int iLen = sBuf.GetLength();

	CSphVector<SqlStmt_t> dStmt;
	SqlParser_c tParser ( dStmt, eCollation );
	tParser.m_pBuf = sBuf.cstr();
	tParser.m_pLastTokenStart = nullptr;
	tParser.m_pParseError = &sError;
	tParser.m_eCollation = eCollation;
	tParser.m_sErrorHeader = "percolate filters:";

	char * sEnd = const_cast<char *>( sBuf.cstr() ) + iLen;
	sEnd[0] = 0; // prepare for yy_scan_buffer
	sEnd[1] = 0; // this is ok because string allocates a small gap

	yylex_init ( &tParser.m_pScanner );
	YY_BUFFER_STATE tLexerBuffer = yy_scan_buffer ( const_cast<char *>( sBuf.cstr() ), iLen+2, tParser.m_pScanner );
	if ( !tLexerBuffer )
	{
		sError = "internal error: yy_scan_buffer() failed";
		return false;
	}

	int iRes = yyparse ( &tParser );
	yy_delete_buffer ( tLexerBuffer, tParser.m_pScanner );
	yylex_destroy ( tParser.m_pScanner );

	dStmt.Pop(); // last query is always dummy

	if ( dStmt.GetLength()>1 )
	{
		sError.SetSprintf ( "internal error: too many FILTERS statements, got %d", dStmt.GetLength() );
		return false;
	}

	if ( dStmt.GetLength() && dStmt[0].m_eStmt!=STMT_SYSFILTERS )
	{
		sError.SetSprintf ( "internal error: not FILTERS statement parsed, got %d", dStmt[0].m_eStmt );
		return false;
	}

	if ( dStmt.GetLength() )
	{
		CSphQuery & tQuery = dStmt[0].m_tQuery;

		int iFilterCount = tParser.m_dFiltersPerStmt[0];
		CreateFilterTree ( tParser.m_dFilterTree, 0, iFilterCount, tQuery );
		
		dFilters.SwapData ( tQuery.m_dFilters );
		dFilterTree.SwapData ( tQuery.m_dFilterTree );
	}

	// maybe its better to create real filter instead of just checking column name
	if ( iRes==0 && dFilters.GetLength() )
	{
		ARRAY_FOREACH ( i, dFilters )
		{
			const CSphFilterSettings & tFilter = dFilters[i];
			if ( tFilter.m_sAttrName.IsEmpty() )
			{
				sError.SetSprintf ( "bad filter %d name", i );
				return false;
			}

			if ( tFilter.m_sAttrName.Begins ( "@" ) )
			{
				sError.SetSprintf ( "unsupported filter column '%s'", tFilter.m_sAttrName.cstr() );
				return false;
			}
	
			const char * sAttrName = tFilter.m_sAttrName.cstr();
			
			// might be a JSON.field
			CSphString sJsonField;
			const char * sJsonDot = strchr ( sAttrName, '.' );
			if ( sJsonDot )
			{
				assert ( sJsonDot>sAttrName );
				sJsonField.SetBinary ( sAttrName, sJsonDot - sAttrName );
				sAttrName = sJsonField.cstr();
			}

			int iCol = tSchema.GetAttrIndex ( sAttrName );
			if ( iCol==-1 )
			{
				sError.SetSprintf ( "no such filter attribute '%s'", sAttrName );
				return false;
			}
		}
	}

	// TODO: change way of filter -> expression create: produce single error, share parser code
	// try expression
	if ( iRes!=0 && !dFilters.GetLength() && sError.Begins ( "percolate filters: syntax error" ) )
	{
		ESphAttr eAttrType = SPH_ATTR_NONE;
		CSphScopedPtr<ISphExpr> pExpr { sphExprParse ( sFilters, tSchema, &eAttrType, NULL, sError, NULL, eCollation ) };
		if ( pExpr )
		{
			sError = "";
			iRes = 0;
			CSphFilterSettings & tExpr = dFilters.Add();
			tExpr.m_eType = SPH_FILTER_EXPRESSION;
			tExpr.m_sAttrName = sFilters;
		} else
		{
			return false;
		}
	}

	return ( iRes==0 );
}

static bool String2JsonPack ( char * pStr, CSphVector<BYTE> & dBuf, CSphString & sError, CSphString & sWarning )
{
	dBuf.Resize ( 0 ); // buffer for JSON parser must be empty to properly set JSON_ROOT data
	if ( !pStr )
		return true;

	if ( !sphJsonParse ( dBuf, pStr, g_bJsonAutoconvNumbers, g_bJsonKeynamesToLowercase, sError ) )
	{
		if ( g_bJsonStrict )
			return false;

		if ( sWarning.IsEmpty() )
			sWarning = sError;
		else
			sWarning.SetSprintf ( "%s; %s", sWarning.cstr(), sError.cstr() );

		sError = "";
	}

	return true;
}

struct StringPtrTraits_t
{
	CSphVector<BYTE> m_dPackedData;
	CSphFixedVector<int> m_dOff { 0 };
	CSphVector<BYTE> m_dParserBuf;

	// remap offsets to string pointers
	void SavePointersTo ( VecTraits_T<const char *> &dStrings, bool bSkipInvalid=true ) const
	{
		if ( bSkipInvalid )
			ARRAY_FOREACH ( i, m_dOff )
			{
				int iOff = m_dOff[i];
				if ( iOff<0 )
					continue;
				dStrings[i] = ( const char * ) m_dPackedData.Begin () + iOff;
			}
		else
			ARRAY_FOREACH ( i, m_dOff )
			{
				int iOff = m_dOff[i];
				dStrings[i] = ( iOff>=0 ? ( const char * ) m_dPackedData.Begin () + iOff : nullptr );
			}
	}

	void Reset ()
	{
		m_dPackedData.Resize ( 0 );
		m_dParserBuf.Resize ( 0 );
		m_dOff.Fill ( -1 );
	}

	BYTE* ReserveBlob ( int iBlobSize, int iOffset )
	{
		if ( !iBlobSize )
			return nullptr;

		m_dOff[iOffset] = m_dPackedData.GetLength ();

		m_dPackedData.ReserveGap ( iBlobSize + 4 );
		sphPackStrlen ( m_dPackedData.AddN ( 4 ), iBlobSize );
		return m_dPackedData.AddN ( iBlobSize );
	}

	void AddBlob ( const VecTraits_T<BYTE>& dBlob, int iOffset )
	{
		memcpy ( ReserveBlob ( dBlob.GetLength (), iOffset ), dBlob.begin (), dBlob.GetLength () );
	}
};

static void BsonToSqlInsert ( const bson::Bson_c& dBson, SqlInsert_t& tAttr )
{
	switch ( dBson.GetType () )
	{
	case JSON_INT32:
	case JSON_INT64: tAttr.m_iType = TOK_CONST_INT;
		tAttr.m_iVal = dBson.Int ();
		break;
	case JSON_DOUBLE: tAttr.m_iType = TOK_CONST_FLOAT;
		tAttr.m_fVal = float ( dBson.Double () );
		break;
	case JSON_STRING: tAttr.m_iType = TOK_QUOTED_STRING;
		tAttr.m_sVal = dBson.String ();
	default: break;
	}
}

// save bson array to 64 bit mvaint64 mva
static int BsonArrayToMvaWide ( CSphVector<DWORD> &dMva, const bson::Bson_c &dBson )
{
	using namespace bson;
	int iOff = dMva.GetLength ();
	dMva.Add ();
	int iStored = 0;

	// int64 may be processed natively; that is the fastest way!
	if ( dBson.GetType ()==JSON_INT64_VECTOR )
	{
		auto dValues = Vector<int64_t> ( dBson );
		iStored = 2 * dValues.GetLength ();
		dMva.Append ( dValues.begin(), iStored );
	} else if ( dBson.GetType ()==JSON_INT32_VECTOR )
	{ // int32 may be easy shrinked
		auto dValues = Vector<DWORD> ( dBson );
		iStored = 2 * dValues.GetLength ();
		auto pDst = ( int64_t * ) dMva.AddN ( iStored );
		ARRAY_FOREACH ( i, dValues )
			pDst[i] = dValues[i];
	} else
	{ // slowest path - m.b. need conversion of every value
		BsonIterator_c dIter ( dBson );
		iStored = 2 * dIter.NumElems ();
		auto pDst = ( int64_t * ) dMva.AddN ( iStored );
		for ( ; dIter; dIter.Next () )
			*pDst++ = dIter.Int ();
	}

	if ( !iStored ) // empty mva; discard resize
	{
		dMva.Resize ( iOff );
		return -1;
	}

	auto pDst = ( int64_t * ) &dMva[iOff + 1];

	sphSort ( pDst, iStored / 2 );
	iStored = 2 * sphUniq ( pDst, iStored / 2 );
	dMva[iOff] = iStored;
	dMva.Resize ( iOff + iStored + 1 );
	return iOff;
}

// save bson array to mva (32- or 64-bit wide)
static int BsonArrayToMva( CSphVector<DWORD> &dMva, const bson::Bson_c& dBson, bool bTargetWide )
{
	assert ( dBson.IsArray () );
	if ( bTargetWide )
		return BsonArrayToMvaWide ( dMva, dBson );

	using namespace bson;
	int iOff = dMva.GetLength();
	dMva.Add();
	int iStored = 0;

	// int32 may be processed natively; that is the fastest way!
	if ( dBson.GetType ()==JSON_INT32_VECTOR )
	{
		auto dValues = Vector<DWORD> (dBson);
		iStored = dValues.GetLength ();
		dMva.Append ( dValues );
	} else if ( dBson.GetType()==JSON_INT64_VECTOR )
	{ // int64 may be easy truncated
		auto dValues = Vector<int64_t> ( dBson );
		iStored = dValues.GetLength ();
		dMva.ReserveGap ( iStored );
		for ( auto& iValue : dValues )
			dMva.Add ( ( DWORD ) iValue );
	} else
	{ // slowest path - m.b. need conversion of every value
		BsonIterator_c dIter ( dBson );
		iStored = dIter.NumElems ();
		dMva.ReserveGap ( iStored );
		for ( ; dIter; dIter.Next () )
			dMva.Add ( dIter.Int() );
	}

	if ( !iStored ) // empty mva; discard resize
	{
		dMva.Resize ( iOff );
		return -1;
	}

	auto dSlice = dMva.Slice ( iOff + 1 );
	dSlice.Sort ();
	iStored = sphUniq ( dSlice.begin (), dSlice.GetLength() );
	dMva[iOff] = iStored;
	dMva.Resize ( iOff + iStored + 1 );
	return iOff;
}

static bool ParseBsonDocument ( const VecTraits_T<BYTE>& dDoc, const CSphHash<SchemaItemVariant_t> & tLoc,
	const CSphString & sIdAlias, int iRow, VecTraits_T<VecTraits_T<const char>>& dFields, CSphMatchVariant & tDoc,
	StringPtrTraits_t & tStrings, CSphVector<DWORD> & dMva, Warner_c & sMsg )
{
	using namespace bson;
	Bson_c dBson ( dDoc );
	if ( dDoc.IsEmpty () )
		return false;

	SqlInsert_t tAttr;

	const SchemaItemVariant_t * pId = sIdAlias.IsEmpty () ? nullptr : tLoc.Find ( sphFNV64 ( sIdAlias.cstr() ) );
	BsonIterator_c dChild ( dBson );
	for ( ; dChild; dChild.Next () )
	{
		CSphString sName = dChild.GetName ();
		const SchemaItemVariant_t * pItem = tLoc.Find ( sphFNV64 ( sName.cstr() ) );

		// FIXME!!! warn on unknown JSON fields
		if ( pItem )
		{
			if ( pItem->m_iField!=-1 && dChild.IsString () )
			{
				// stripper prior to build hits does not preserve field length
				// but works with \0 strings and could walk all document and modifies it and alter field length
				const VecTraits_T<const char> tField = Vector<const char> ( dChild );
				if ( tField.GetLength() )
				{
					int64_t iOff = tStrings.m_dPackedData.GetLength();

					// copy field content with tail zeroes
					BYTE * pDst = tStrings.m_dPackedData.AddN ( tField.GetLength() + 1 + CSphString::GetGap() );
					memcpy ( pDst, tField.Begin(), tField.GetLength() );
					memset ( pDst + tField.GetLength(), 0, 1 + CSphString::GetGap() );

					// pack offset into pointer then restore pointer after m_dPackedData filed
					dFields[pItem->m_iField] = VecTraits_T<const char> ( (const char *)iOff, tField.GetLength() );
				} else
				{
					dFields[pItem->m_iField] = tField;
				}

				if ( pItem==pId )
					sMsg.Warn ( "field '%s' requested as docs_id identifier, but it is field!", sName.cstr() );
			} else
			{
				BsonToSqlInsert ( dChild, tAttr );
				tDoc.SetAttr ( pItem->m_tLoc, tAttr, pItem->m_eType );
				if ( pId==pItem )
					tDoc.m_uDocID = ( SphDocID_t ) dChild.Int ();

				switch ( pItem->m_eType )
				{
					case SPH_ATTR_JSON:
						assert ( pItem->m_iStr!=-1 );
						{
							// just save bson blob
							BYTE * pDst = tStrings.ReserveBlob ( dChild.StandaloneSize (), pItem->m_iStr );
							dChild.BsonToBson ( pDst );
						}
						break;
					case SPH_ATTR_STRING:
						assert ( pItem->m_iStr!=-1 );
						{
							auto dStrBlob = RawBlob ( dChild );
							if ( dStrBlob.second )
							{
								tStrings.m_dOff[pItem->m_iStr] = tStrings.m_dPackedData.GetLength ();
								BYTE * sDst = tStrings.m_dPackedData.AddN ( 1 + dStrBlob.second + CSphString::GetGap () );
								memcpy ( sDst, dStrBlob.first, dStrBlob.second );
								memset ( sDst + dStrBlob.second, 0, 1 + CSphString::GetGap () );
							}
						}
						break;
					case SPH_ATTR_UINT32SET:
					case SPH_ATTR_INT64SET:
						assert ( pItem->m_iMva!=-1 );
						if ( dChild.IsArray() )
						{
							int iOff = BsonArrayToMva ( dMva, dChild, pItem->m_eType==SPH_ATTR_INT64SET );
							if ( iOff>=0 )
								dMva[pItem->m_iMva] = iOff;
						} else
						{
							sMsg.Warn ( "MVA item should be array" );
						}
					default:
						break;
				}
			}
		} else if ( !sIdAlias.IsEmpty() && sIdAlias==sName )
		{
			tDoc.m_uDocID = ( SphDocID_t ) dChild.Int ();
		}
	}
	return true;
}

static void FixParsedMva ( const CSphVector<DWORD> & dParsed, CSphVector<DWORD> & dMva, int iCount )
{
	if ( !iCount )
		return;

	// dParsed:
	// 0 - iCount elements: offset to MVA values with leading MVA element count
	// Could be not in right order

	dMva.Resize ( 0 );
	for ( int i=0; i<iCount; ++i )
	{
		int iOff = dParsed[i];
		if ( !iOff )
		{
			dMva.Add ( 0 );
			continue;
		}

		DWORD uMvaCount = dParsed[iOff];
		DWORD * pMva = dMva.AddN ( uMvaCount + 1 );
		*pMva++ = uMvaCount;
		memcpy ( pMva, dParsed.Begin() + iOff + 1, sizeof(dMva[0]) * uMvaCount );
	}
}

class PqRequestBuilder_c : public IRequestBuilder_t
{
	const BlobVec_t &m_dDocs;
	const PercolateOptions_t &m_tOpts;
	mutable CSphAtomic m_iWorker;
	int m_iStart;
	int m_iStep;

public:
	explicit PqRequestBuilder_c ( const BlobVec_t &dDocs, const PercolateOptions_t &tOpts, int iStart=0, int iStep=0 )
		: m_dDocs ( dDocs )
		, m_tOpts ( tOpts )
		, m_iStart ( iStart )
		, m_iStep ( iStep)
	{}

	void BuildRequest ( const AgentConn_t &tAgent, CachedOutputBuffer_c &tOut ) const final
	{
		// it sends either all queries to each agent or sequence of queries to current agent

		auto iWorker = tAgent.m_iStoreTag;
		if ( iWorker<0 )
		{
			iWorker = ( int ) m_iWorker++;
			tAgent.m_iStoreTag = iWorker;
		}

		const char * sIndex = tAgent.m_tDesc.m_sIndexes.cstr ();
		APICommand_t tWr { tOut, SEARCHD_COMMAND_CALLPQ, VER_COMMAND_CALLPQ };

		DWORD uFlags = 0;
		if ( m_tOpts.m_bGetDocs )
			uFlags = 1;
		if ( m_tOpts.m_bGetQuery )
			uFlags |= 2;
		if ( m_tOpts.m_bJsonDocs )
			uFlags |= 4;
		if ( m_tOpts.m_bVerbose )
			uFlags |= 8;
		if ( m_tOpts.m_bSkipBadJson )
			uFlags |= 16;

		tOut.SendDword ( uFlags );
		tOut.SendString ( m_tOpts.m_sIdAlias.cstr () );
		tOut.SendString ( sIndex );

		// send docs (all or chunk)
		int iStart = 0;
		int iStep = m_dDocs.GetLength();
		if ( m_iStep ) // sparsed case, calculate the interval.
		{
			iStart = m_iStart + m_iStep * iWorker;
			iStep = Min ( iStep - iStart, m_iStep );
		}
		tOut.SendInt ( iStart );
		tOut.SendInt ( iStep );
		for ( int i=iStart; i<iStart+iStep; ++i)
			tOut.SendArray ( m_dDocs[i] );
	}
};



struct PqReplyParser_t : public IReplyParser_t
{
	bool ParseReply ( MemInputBuffer_c &tReq, AgentConn_t &tAgent ) const final
	{
		//	auto &dQueries = m_pWorker->m_dQueries;
		//	int iDoc = m_pWorker->m_dTasks[tAgent.m_iStoreTag].m_iHead;

		auto pResult = ( CPqResult * ) tAgent.m_pResult.Ptr ();
		if ( !pResult )
		{
			pResult = new CPqResult;
			tAgent.m_pResult = pResult;
		}

		auto &dResult = pResult->m_dResult;
		auto uFlags = tReq.GetDword ();
		bool bDumpDocs = !!(uFlags & 1);
		bool bQuery = !!(uFlags & 2);
		bool bDeduplicatedDocs = !!(uFlags & 4);

		dResult.m_bGetDocs = bDumpDocs;
		dResult.m_bGetQuery = bQuery;
		CSphVector<int> dDocs;
		CSphVector<int64_t> dDocids;
		dDocids.Add(0); // just to keep docids 1-based and so, simplify processing by avoid checks.

		int iRows = tReq.GetInt ();
		dResult.m_dQueryDesc.Reset ( iRows );
		for ( auto &tDesc : dResult.m_dQueryDesc )
		{
			tDesc.m_uQID = tReq.GetUint64 ();
			if ( bDumpDocs )
			{
				int iCount = tReq.GetInt ();
				dDocs.Add ( iCount );
				if ( bDeduplicatedDocs )
				{
					for ( int iDoc = 0; iDoc<iCount; ++iDoc )
					{
						dDocs.Add ( dDocids.GetLength () );
						dDocids.Add ( ( int64_t ) tReq.GetUint64 () );
					}
				} else
				{
					for ( int iDoc = 0; iDoc<iCount; ++iDoc )
						dDocs.Add ( tReq.GetInt () );
				}
			}

			if ( bQuery )
			{
				auto uDescFlags = tReq.GetDword ();
				if ( uDescFlags & 1 )
					tDesc.m_sQuery = tReq.GetString ();
				if ( uDescFlags & 2 )
					tDesc.m_sTags = tReq.GetString ();
				if ( uDescFlags & 4 )
					tDesc.m_sFilters = tReq.GetString ();
				tDesc.m_bQL = !!(uDescFlags & 8);
			}
		}

		// meta
		dResult.m_tmTotal = tReq.GetUint64 ();
		dResult.m_tmSetup = tReq.GetUint64 ();
		dResult.m_iQueriesMatched = tReq.GetInt();
		dResult.m_iQueriesFailed = tReq.GetInt ();
		dResult.m_iDocsMatched = tReq.GetInt ();
		dResult.m_iTotalQueries = tReq.GetInt ();
		dResult.m_iOnlyTerms = tReq.GetInt ();
		dResult.m_iEarlyOutQueries = tReq.GetInt ();
		auto iDts = tReq.GetInt();
		dResult.m_dQueryDT.Reset ( iDts );
		for ( int& iDt : dResult.m_dQueryDT )
			iDt = tReq.GetInt();

		dResult.m_sMessages.Warn ( tReq.GetString () );

		auto iDocs = dDocs.GetLength ();
		dResult.m_dDocs.Set ( dDocs.LeakData (), iDocs );

		if ( dDocids.GetLength()>1 )
		{
			iDocs = dDocids.GetLength ();
			pResult->m_dDocids.Set ( dDocids.LeakData (), iDocs );
		}

		return true;
	}
};

static void SendAPIPercolateReply ( CachedOutputBuffer_c &tOut, const CPqResult &tResult, int iShift=0 )
{
	APICommand_t dCallPq ( tOut, SEARCHD_OK, VER_COMMAND_CALLPQ );

	CSphVector<int64_t> dTmpDocs;
	int iDocOff = -1;

	const PercolateMatchResult_t &tRes = tResult.m_dResult;
	const CSphFixedVector<int64_t> &dDocids = tResult.m_dDocids;
	bool bHasDocids = !dDocids.IsEmpty ();
	bool bDumpDocs = tRes.m_bGetDocs;
	bool bQuery = tRes.m_bGetQuery;

	DWORD uFlags = 0;

	if ( bDumpDocs )
		uFlags = 1;
	if ( bQuery )
		uFlags |=2;
	if ( bHasDocids )
		uFlags |=4;

	tOut.SendDword ( uFlags );

	tOut.SendInt ( tRes.m_dQueryDesc.GetLength () );
	for ( const auto &tDesc : tRes.m_dQueryDesc )
	{
		tOut.SendUint64 ( tDesc.m_uQID );
		if ( bDumpDocs )
		{
			// document count + document id(s)
			auto iCount = ( int ) ( tRes.m_dDocs[++iDocOff] );
			if ( bHasDocids ) // need de-duplicate docs
			{
				dTmpDocs.Resize ( iCount );
				for ( int iDoc = 0; iDoc<iCount; ++iDoc )
				{
					int iRow = tRes.m_dDocs[++iDocOff];
					dTmpDocs[iDoc] = dDocids[iRow];
				}
				dTmpDocs.Uniq ();
				tOut.SendInt ( dTmpDocs.GetLength());
				for ( auto dTmpDoc : dTmpDocs )
					tOut.SendUint64 ( dTmpDoc );
			} else
			{
				tOut.SendInt ( iCount );
				for ( int iDoc = 0; iDoc<iCount; ++iDoc )
					tOut.SendInt ( iShift+tRes.m_dDocs[++iDocOff] );
			}
		}
		if ( bQuery )
		{
			DWORD uDescFlags = 0;
			if ( !tDesc.m_sQuery.IsEmpty ())
				uDescFlags |=1;
			if ( !tDesc.m_sQuery.IsEmpty () )
				uDescFlags |= 2;
			if ( !tDesc.m_sQuery.IsEmpty () )
				uDescFlags |= 4;
			if ( tDesc.m_bQL )
				uDescFlags |= 8;

			tOut.SendDword ( uDescFlags );
			if ( uDescFlags & 1 )
				tOut.SendString ( tDesc.m_sQuery.cstr () );
			if ( uDescFlags & 2 )
				tOut.SendString ( tDesc.m_sTags.cstr () );
			if ( uDescFlags & 4 )
				tOut.SendString ( tDesc.m_sFilters.cstr () );
		}
	}

	// send meta
	tOut.SendUint64 ( tRes.m_tmTotal );
	tOut.SendUint64 ( tRes.m_tmSetup );
	tOut.SendInt ( tRes.m_iQueriesMatched );
	tOut.SendInt ( tRes.m_iQueriesFailed );
	tOut.SendInt ( tRes.m_iDocsMatched );
	tOut.SendInt ( tRes.m_iTotalQueries );
	tOut.SendInt ( tRes.m_iOnlyTerms );
	tOut.SendInt ( tRes.m_iEarlyOutQueries );
	tOut.SendInt ( tRes.m_dQueryDT.GetLength () );
	for ( int iDT : tRes.m_dQueryDT )
		tOut.SendInt ( iDT );

	tOut.SendString ( tRes.m_sMessages.sWarning () );
}

static void SendMysqlPercolateReply ( SqlRowBuffer_c &tOut, const CPqResult &tResult, int iShift=0 )
{
	// shortcuts
	const PercolateMatchResult_t &tRes = tResult.m_dResult;
	const CSphFixedVector<int64_t> &dDocids = tResult.m_dDocids;

	bool bDumpDocs = tRes.m_bGetDocs;
	bool bQuery = tRes.m_bGetQuery;

	// result set header packet. We will attach EOF manually at the end.
	int iColumns = bDumpDocs ? 2 : 1;
	if ( bQuery )
		iColumns += 3;
	tOut.HeadBegin ( iColumns );

	tOut.HeadColumn ( "id", MYSQL_COL_LONGLONG, MYSQL_COL_UNSIGNED_FLAG );
	if ( bDumpDocs )
		tOut.HeadColumn ( "documents", MYSQL_COL_STRING );
	if ( bQuery )
	{
		tOut.HeadColumn ( "query" );
		tOut.HeadColumn ( "tags" );
		tOut.HeadColumn ( "filters" );
	}

	// EOF packet is sent explicitly due to non-default params.
	auto iWarns = tRes.m_sMessages.WarnEmpty () ? 0 : 1;
	tOut.HeadEnd ( false, iWarns );

	CSphVector<int64_t> dTmpDocs;
	int iDocOff = -1;
	StringBuilder_c sDocs;
	for ( const auto &tDesc : tRes.m_dQueryDesc )
	{
		tOut.PutNumAsString ( tDesc.m_uQID );
		if ( bDumpDocs )
		{
			sDocs.StartBlock ( "," );
			// document count + document id(s)
			auto iCount = ( int ) ( tRes.m_dDocs[++iDocOff] );
			if ( dDocids.GetLength () ) // need de-duplicate docs
			{
				dTmpDocs.Resize ( iCount );
				for ( int iDoc = 0; iDoc<iCount; ++iDoc )
				{
					int iRow = tRes.m_dDocs[++iDocOff];
					dTmpDocs[iDoc] = dDocids[iRow];
				}
				dTmpDocs.Uniq ();
				for ( auto dTmpDoc : dTmpDocs )
					sDocs.Sprintf ( "%l", dTmpDoc );
			} else
			{
				for ( int iDoc = 0; iDoc<iCount; ++iDoc )
				{
					int iRow = tRes.m_dDocs[++iDocOff];
					sDocs.Sprintf ( "%d", iRow + iShift );
				}
			}

			tOut.PutString ( sDocs.cstr () );
			sDocs.Clear ();
		}
		if ( bQuery )
		{
			tOut.PutString ( tDesc.m_sQuery );
			tOut.PutString ( tDesc.m_sTags );
			tOut.PutString ( tDesc.m_sFilters );
		}

		tOut.Commit ();
	}

	tOut.Eof ( false, iWarns );
}

// process one(!) local(!) pq index
static void PQLocalMatch ( const BlobVec_t &dDocs, const CSphString& sIndex, const PercolateOptions_t & tOpt,
	CSphSessionAccum &tAcc, CPqResult &tResult, int iStart, int iDocs )
{
	CSphString sWarning, sError;
	auto &sMsg = tResult.m_dResult.m_sMessages;
	tResult.m_dResult.m_bGetDocs = tOpt.m_bGetDocs;
	tResult.m_dResult.m_bVerbose = tOpt.m_bVerbose;
	tResult.m_dResult.m_bGetQuery = tOpt.m_bGetQuery;
	sMsg.Clear ();

	if ( !iDocs || ( iStart + iDocs )>dDocs.GetLength () )
		iDocs = dDocs.GetLength () - iStart;

	if ( !iDocs )
	{
		sMsg.Warn ( "No more docs for sparse matching" );
		return;
	}

	ServedDescRPtr_c pServed ( GetServed ( sIndex ) );
	if ( !pServed || !pServed->m_pIndex )
	{
		sMsg.Err ( "unknown local index '%s' in search request", sIndex.cstr () );
		return;
	}

	if ( pServed->m_eType!=IndexType_e::PERCOLATE )
	{
		sMsg.Err ( "index '%s' is not percolate", sIndex.cstr () );
		return;
	}

	auto pIndex = ( PercolateIndex_i * ) pServed->m_pIndex;
	ISphRtAccum * pAccum = tAcc.GetAcc ( pIndex, sError );
	sMsg.Err ( sError );

	if ( !sMsg.ErrEmpty () )
		return;


	const CSphSchema &tSchema = pIndex->GetInternalSchema ();
	int iFieldsCount = tSchema.GetFieldsCount ();
	CSphFixedVector<VecTraits_T<const char>> dFields ( iFieldsCount );

	// set defaults
	CSphMatchVariant tDoc;
	tDoc.Reset ( tSchema.GetRowSize () );
	int iAttrsCount = tSchema.GetAttrsCount ();
	for ( int i = 0; i<iAttrsCount; ++i )
	{
		const CSphColumnInfo &tCol = tSchema.GetAttr ( i );
		CSphAttrLocator tLoc = tCol.m_tLocator;
		tLoc.m_bDynamic = true;
		tDoc.SetDefaultAttr ( tLoc, tCol.m_eAttrType );
	}

	int iStrCounter = 0;
	int iMvaCounter = 0;
	CSphHash<SchemaItemVariant_t> hSchemaLocators;
	if ( tOpt.m_bJsonDocs )
	{

		// hash attrs
		for ( int i = 0; i<iAttrsCount; ++i )
		{
			const CSphColumnInfo &tCol = tSchema.GetAttr ( i );
			SchemaItemVariant_t tAttr;
			tAttr.m_tLoc = tCol.m_tLocator;
			tAttr.m_tLoc.m_bDynamic = true; /// was just set above
			tAttr.m_eType = tCol.m_eAttrType;
			if ( tCol.m_eAttrType==SPH_ATTR_STRING || tCol.m_eAttrType==SPH_ATTR_JSON )
				tAttr.m_iStr = iStrCounter++;
			if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
				tAttr.m_iMva = iMvaCounter++;

			hSchemaLocators.Add ( sphFNV64 ( tCol.m_sName.cstr () ), tAttr );
		}
		for ( int i = 0; i<iFieldsCount; ++i )
		{
			const CSphColumnInfo &tField = tSchema.GetField ( i );
			SchemaItemVariant_t tAttr;
			tAttr.m_iField = i;
			hSchemaLocators.Add ( sphFNV64 ( tField.m_sName.cstr () ), tAttr );
		}
	} else
	{
		// even without JSON docs MVA should match to schema definition on inserting data into accumulator
		for ( int i = 0; i<iAttrsCount; ++i )
		{
			const CSphColumnInfo &tCol = tSchema.GetAttr ( i );
			if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
				++iMvaCounter;
		}
	}

	int iDocsNoIdCount = 0;
	bool bAutoId = tOpt.m_sIdAlias.IsEmpty ();
	tResult.m_dDocids.Reset ( bAutoId ? 0 : iDocs + 1 );
	SphDocID_t uSeqDocid = 1;

	CSphFixedVector<const char *> dStrings ( iStrCounter );
	StringPtrTraits_t tStrings;
	tStrings.m_dOff.Reset ( iStrCounter );
	CSphVector<DWORD> dMvaParsed ( iMvaCounter );
	CSphVector<DWORD> dMva;

	CSphString sTokenFilterOpts;
	for ( auto iDoc = iStart; iDoc<iStart+iDocs; ++iDoc )
	{
		// doc-id
		tDoc.m_uDocID = 0;
		dFields[0] = dDocs[iDoc];

		dMvaParsed.Resize ( iMvaCounter );
		dMvaParsed.Fill ( 0 );

		if ( tOpt.m_bJsonDocs )
		{
			// reset all back to defaults
			dFields.Fill ( { nullptr, 0 } );
			for ( int i = 0; i<iAttrsCount; ++i )
			{
				const CSphColumnInfo &tCol = tSchema.GetAttr ( i );
				CSphAttrLocator tLoc = tCol.m_tLocator;
				tLoc.m_bDynamic = true;
				tDoc.SetDefaultAttr ( tLoc, tCol.m_eAttrType );
			}
			tStrings.Reset ();

			if ( !ParseBsonDocument ( dDocs[iDoc], hSchemaLocators, tOpt.m_sIdAlias, iDoc,
						dFields, tDoc, tStrings, dMvaParsed, sMsg ) )
			{
				// for now the only case of fail - if provided bson is empty (null) document.
				if ( tOpt.m_bSkipBadJson )
				{
					sMsg.Warn ( "ERROR: Document %d is empty", iDoc + tOpt.m_iShift + 1 );
					continue;
				}

				sMsg.Err ( "Document %d is empty", iDoc + tOpt.m_iShift + 1 );
				break;
			}

			tStrings.SavePointersTo ( dStrings, false );

			// convert back offset into tStrings buffer into pointers
			for ( VecTraits_T<const char> & tField : dFields )
			{
				if ( !tField.GetLength() )
					continue;

				int64_t iOff = int64_t( tField.Begin() );
				int iLen = tField.GetLength();
				tField = VecTraits_T<const char> ( (const char *)( tStrings.m_dPackedData.Begin()+iOff ), iLen );
			}
		}
		FixParsedMva ( dMvaParsed, dMva, iMvaCounter );

		if ( !sMsg.ErrEmpty () )
			break;


		if ( !bAutoId )
		{
			// in user-provides-id mode let's skip all docs without id
			if ( !tDoc.m_uDocID )
			{
				++iDocsNoIdCount;
				continue;
			}

			// store provided doc-id for result set sending
			tResult.m_dDocids[uSeqDocid] = ( int64_t ) tDoc.m_uDocID;
			tDoc.m_uDocID = uSeqDocid++;
		} else
			// PQ work with sequential document numbers, 0 element unused
			tDoc.m_uDocID = iDoc+1; // +1 since docid is 1-based

		// add document
		pIndex->AddDocument ( dFields, tDoc, true, sTokenFilterOpts, dStrings.Begin(), dMva, sError, sWarning, pAccum );
		sMsg.Err ( sError );
		sMsg.Warn ( sWarning );

		if ( !sMsg.ErrEmpty () )
			break;
	}

	// fire exit
	if ( !sMsg.ErrEmpty () )
	{
		pIndex->RollBack ( pAccum ); // clean up collected data
		return;
	}

	pIndex->MatchDocuments ( pAccum, tResult.m_dResult );

	if ( iDocsNoIdCount )
		sMsg.Warn ( "skipped %d document(s) without id field '%s'", iDocsNoIdCount, tOpt.m_sIdAlias.cstr () );

}

void PercolateMatchDocuments ( const BlobVec_t & dDocs, const PercolateOptions_t & tOpts,
	CSphSessionAccum & tAcc, CPqResult &tResult )
{
	CSphString sIndex = tOpts.m_sIndex;
	CSphString sWarning, sError;

	StrVec_t dLocalIndexes;
	auto * pLocalIndexes = &dLocalIndexes;

	VecRefPtrsAgentConn_t dAgents;
	auto pDist = GetDistr ( sIndex );
	if ( pDist )
	{
		for ( auto * pAgent : pDist->m_dAgents )
		{
			auto * pConn = new AgentConn_t;
			pConn->SetMultiAgent ( sIndex, pAgent );
			pConn->m_iMyConnectTimeout = pDist->m_iAgentConnectTimeout;
			pConn->m_iMyQueryTimeout = pDist->m_iAgentQueryTimeout;
			dAgents.Add ( pConn );
		}

		pLocalIndexes = &pDist->m_dLocal;
	} else
		dLocalIndexes.Add ( sIndex );

	// at this point we know total num of involved indexes,
	// and can eventually split (sparse) docs among them.
	int iChunks = 0;
	if ( tOpts.m_eMode==PercolateOptions_t::unknown || tOpts.m_eMode==PercolateOptions_t::sparsed)
		iChunks = dAgents.GetLength () + pLocalIndexes->GetLength ();
	int iStart = 0;
	int iStep = iChunks>1 ? ( ( dDocs.GetLength () - 1 ) / iChunks + 1 ) : 0;

	PqRequestBuilder_c tReqBuilder ( dDocs, tOpts, iStart, iStep );
	iStart += iStep * dAgents.GetLength();
	PqReplyParser_t tParser;
	CSphRefcountedPtr<IRemoteAgentsObserver> tReporter ( GetObserver () );
	ScheduleDistrJobs ( dAgents, &tReqBuilder, &tParser, tReporter );

	LazyVector_T <CPqResult> dLocalResults;
	for ( const auto & sPqIndex : *pLocalIndexes )
	{
		auto & dResult = dLocalResults.Add();
		PQLocalMatch ( dDocs, sPqIndex, tOpts, tAcc, dResult, iStart, iStep );
		iStart += iStep;
	}

	tReporter->Finish ();

	auto iSuccesses = ( int ) tReporter->GetSucceeded ();
	auto iAgentsDone = ( int ) tReporter->GetFinished ();

	LazyVector_T<CPqResult*> dAllResults;
	for ( auto & dLocalRes : dLocalResults )
		dAllResults.Add ( &dLocalRes );

	CPqResult dMsgs; // fake resultset just to grab errors from remotes
	if ( iAgentsDone>iSuccesses )
		dAllResults.Add ( &dMsgs );

	if ( iAgentsDone )
	{
		for ( auto * pAgent : dAgents )
		{
			if ( !pAgent->m_bSuccess )
			{
				dMsgs.m_dResult.m_sMessages.Err ( pAgent->m_sFailure );
				continue;
			}

			auto pResult = ( CPqResult * ) pAgent->m_pResult.Ptr ();
			if ( !pResult )
				continue;

			dAllResults.Add ( pResult );
		}
	}

	MergePqResults ( dAllResults, tResult, iChunks<2 );

	if ( iSuccesses!=iAgentsDone )
	{
		sphWarning ( "Remote PQ: some of the agents didn't answered: %d queried, %d finished, %d succeeded"
					 , dAgents.GetLength (), iAgentsDone, iSuccesses );

	}
}

/// call PQ command over API
void HandleCommandCallPq ( CachedOutputBuffer_c &tOut, WORD uVer, InputBuffer_c &tReq ) REQUIRES ( HandlerThread )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_CALLPQ, tOut ) )
		return;

	// options
	PercolateOptions_t tOpts;

	DWORD uFlags = tReq.GetDword ();
	tOpts.m_bGetDocs	= !!(uFlags & 1);
	tOpts.m_bGetQuery	= !!(uFlags & 2);
	tOpts.m_bJsonDocs	= !!(uFlags & 4);
	tOpts.m_bVerbose	= !!(uFlags & 8);
	tOpts.m_bSkipBadJson = !! ( uFlags & 16 );

	tOpts.m_sIdAlias = tReq.GetString();

	// index name
	tOpts.m_sIndex = tReq.GetString();

	// document(s)
	tOpts.m_iShift = tReq.GetInt();
	BlobVec_t dDocs ( tReq.GetInt() );
	for ( auto & sDoc : dDocs )
		if ( !tReq.GetString ( sDoc ) )
		{
			SendErrorReply ( tOut, "Can't retrieve doc from input buffer" );
			return;
		}

	// working
	CSphSessionAccum tAcc ( true );
	CPqResult tResult;

	PercolateMatchDocuments ( dDocs, tOpts, tAcc, tResult );

	if ( tResult.m_dResult.m_iQueriesFailed )
		tResult.m_dResult.m_sMessages.Err ( "%d queries failed", tResult.m_dResult.m_iQueriesFailed );

	if ( !tResult.m_dResult.m_sMessages.ErrEmpty () )
	{
		SendErrorReply ( tOut, "%s", tResult.m_dResult.m_sMessages.sError() );
		return;
	}

	SendAPIPercolateReply ( tOut, tResult, tOpts.m_iShift );
}

static void HandleMysqlCallPQ ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt, CSphSessionAccum & tAcc,
	CPqResult & tResult )
{

	PercolateMatchResult_t &tRes = tResult.m_dResult;
	tRes.Reset();

	// check arguments
	// index name, document | documents list, [named opts]
	if ( tStmt.m_dInsertValues.GetLength()!=2 )
	{
		tOut.Error ( tStmt.m_sStmt, "PQ() expects exactly 2 arguments (index, document(s))" );
		return;
	}
	auto &dStmtIndex = tStmt.m_dInsertValues[0];
	auto &dStmtDocs = tStmt.m_dInsertValues[1];

	if ( dStmtIndex.m_iType!=TOK_QUOTED_STRING )
	{
		tOut.Error ( tStmt.m_sStmt, "PQ() argument 1 must be a string" );
		return;
	}
	if ( dStmtDocs.m_iType!=TOK_QUOTED_STRING && dStmtDocs.m_iType!=TOK_CONST_STRINGS )
	{
		tOut.Error ( tStmt.m_sStmt, "PQ() argument 2 must be a string or a string list" );
		return;
	}

	// document(s)
	StrVec_t dDocs;
	if ( dStmtDocs.m_iType==TOK_QUOTED_STRING )
		dDocs.Add ( dStmtDocs.m_sVal );
	else
		dDocs.SwapData ( tStmt.m_dCallStrings );

	// options last
	CSphString sError;
	PercolateOptions_t tOpts;
	tOpts.m_sIndex = dStmtIndex.m_sVal;
	bool bSkipEmpty = false;
	ARRAY_FOREACH ( i, tStmt.m_dCallOptNames )
	{
		CSphString & sOpt = tStmt.m_dCallOptNames[i];
		const SqlInsert_t & v = tStmt.m_dCallOptValues[i];

		sOpt.ToLower();
		int iExpType = TOK_CONST_INT;

		if ( sOpt=="docs_id" ) 			{ tOpts.m_sIdAlias = v.m_sVal; iExpType = TOK_QUOTED_STRING; }
		else if ( sOpt=="docs" )		tOpts.m_bGetDocs = ( v.m_iVal!=0 );
		else if ( sOpt=="verbose" )		tOpts.m_bVerbose = ( v.m_iVal!=0 );
		else if ( sOpt=="docs_json" )	tOpts.m_bJsonDocs = ( v.m_iVal!=0 );
		else if ( sOpt=="query" )		tOpts.m_bGetQuery = ( v.m_iVal!=0 );
		else if ( sOpt=="skip_bad_json" )	tOpts.m_bSkipBadJson = ( v.m_iVal!=0 );
		else if ( sOpt=="skip_empty" ) 	bSkipEmpty = true;
		else if ( sOpt=="shift" ) 		tOpts.m_iShift = v.m_iVal;
		else if ( sOpt=="mode" )
		{
			auto sMode = v.m_sVal;
			iExpType = TOK_QUOTED_STRING;
			sMode.ToLower();
			if ( sMode=="sparsed" )
				tOpts.m_eMode = PercolateOptions_t::sparsed;
			else if ( sMode=="sharded" )
				tOpts.m_eMode = PercolateOptions_t::sharded;
			else
			{
				sError.SetSprintf ( "unknown mode %s. (Expected 'sparsed' or 'sharded')", v.m_sVal.cstr () );
				break;
			}
		} else
		{
			sError.SetSprintf ( "unknown option %s", sOpt.cstr() );
			break;
		}

		// post-conf type check
		if ( iExpType!=v.m_iType )
		{
			sError.SetSprintf ( "unexpected option %s type", sOpt.cstr() );
			break;
		}
	}

	if ( tOpts.m_bSkipBadJson && !tOpts.m_bJsonDocs ) // fixme! do we need such warn? Uncomment, if so.
		tRes.m_sMessages.Warn ( "option to skip bad json has no sense since docs are not in json form" );

	if ( !sError.IsEmpty() )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	BlobVec_t dBlobDocs;
	dBlobDocs.Reserve ( dDocs.GetLength() ); // actually some docs may be complex
	CSphVector<int> dBadDocs;

	if ( !tOpts.m_bJsonDocs )
		for ( auto &dDoc : dDocs )
			dDoc.LeakToVec ( dBlobDocs.Add () );
	else
		ARRAY_FOREACH ( i, dDocs )
		{
			using namespace bson;
			CSphVector<BYTE> dData;
			if ( !sphJsonParse ( dData, ( char * ) dDocs[i].cstr (), g_bJsonAutoconvNumbers, g_bJsonKeynamesToLowercase, sError ) )
			{
				dBadDocs.Add ( i + 1 );
				continue;
			}

			Bson_c dBson ( dData );
			if ( dBson.IsArray () )
			{
				for ( BsonIterator_c dItem ( dBson ); dItem; dItem.Next() )
				{
					if ( dItem.IsAssoc () )
						dItem.BsonToBson ( dBlobDocs.Add () );
					else
					{
						dBadDocs.Add ( i + 1 );	// fixme! m.b. report it as 'wrong doc N in string M'?
						break;
					}
				}
			}
			else if ( dBson.IsAssoc() )
			{
				dData.SwapData ( dBlobDocs.Add () );
			}
			else if ( bSkipEmpty && dBson.IsEmpty() )
				continue;
			else
				dBadDocs.Add ( i + 1 ); // let it be just 'an error' for now
			if ( !dBadDocs.IsEmpty() && !tOpts.m_bSkipBadJson )
				break;
		}

	if ( !dBadDocs.IsEmpty() )
	{
		StringBuilder_c sBad ( ",", "Bad JSON objects in strings: " );
		for ( int iBadDoc:dBadDocs )
			sBad.Sprintf ( "%d", iBadDoc );

		if ( !tOpts.m_bSkipBadJson )
		{
			tOut.Error ( tStmt.m_sStmt, sBad.cstr ());
			return;
		}
		tRes.m_sMessages.Warn ( sBad.cstr () );
	}

	tResult.m_dDocids.Reset ( tOpts.m_sIdAlias.IsEmpty () ? 0 : dBlobDocs.GetLength () + 1 );

	if ( tOpts.m_iShift && !tOpts.m_sIdAlias.IsEmpty () )
		tRes.m_sMessages.Warn ( "'shift' option works only for automatic ids, when 'docs_id' is not defined" );

	PercolateMatchDocuments ( dBlobDocs, tOpts, tAcc, tResult );

	if ( !tRes.m_sMessages.ErrEmpty () )
	{
		tRes.m_sMessages.MoveAllTo ( sError );
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	SendMysqlPercolateReply ( tOut, tResult, tOpts.m_iShift );
}

void HandleMysqlPercolateMeta ( const CPqResult &tResult, const CSphString & sWarning, SqlRowBuffer_c & tOut )
{
	// shortcuts
	const PercolateMatchResult_t &tMeta = tResult.m_dResult;

	tOut.HeadTuplet ( "Name", "Value" );
	tOut.DataTupletf ( "Total", "%.3D sec", tMeta.m_tmTotal / 1000 );
	if ( tMeta.m_tmSetup && tMeta.m_tmSetup>0 )
		tOut.DataTupletf ( "Setup", "%.3D sec", tMeta.m_tmSetup / 1000 );
	tOut.DataTuplet ( "Queries matched", tMeta.m_iQueriesMatched );
	tOut.DataTuplet ( "Queries failed", tMeta.m_iQueriesFailed );
	tOut.DataTuplet ( "Document matched", tMeta.m_iDocsMatched );
	tOut.DataTuplet ( "Total queries stored", tMeta.m_iTotalQueries );
	tOut.DataTuplet ( "Term only queries", tMeta.m_iOnlyTerms );
	tOut.DataTuplet ( "Fast rejected queries", tMeta.m_iEarlyOutQueries );

	if ( !tMeta.m_dQueryDT.IsEmpty() )
	{
		uint64_t tmMatched = 0;
		StringBuilder_c sList (", ");
		assert ( tMeta.m_iQueriesMatched==tMeta.m_dQueryDT.GetLength() );
		for ( int tmQuery : tMeta.m_dQueryDT )
		{
			sList.Sprintf ( "%d", tmQuery );
			tmMatched += tmQuery;
		}
		tOut.DataTuplet ( "Time per query", sList.cstr() );
		tOut.DataTuplet ( "Time of matched queries", tmMatched );
	}
	if ( !sWarning.IsEmpty() )
		tOut.DataTuplet ( "Warning", sWarning.cstr() );

	tOut.Eof();
}


class SphinxqlRequestBuilder_c : public IRequestBuilder_t
{
public:
			SphinxqlRequestBuilder_c ( const CSphString & sQuery, const SqlStmt_t & tStmt );
	void	BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const final;

protected:
	const CSphString m_sBegin;
	const CSphString m_sEnd;
};


void sphHandleMysqlInsert ( StmtErrorReporter_i & tOut, SqlStmt_t & tStmt, bool bReplace, bool bCommit,
	  CSphString & sWarning, CSphSessionAccum & tAcc, ESphCollation	eCollation )
{
	MEMORY ( MEM_SQL_INSERT );

	CSphString sError;
	ServedDescRPtr_c pServed ( GetServed ( tStmt.m_sIndex ) );
	if ( !pServed )
	{
		sError.SetSprintf ( "no such local index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	if ( !pServed->IsMutable () )
	{
		sError.SetSprintf ( "index '%s' does not support INSERT", tStmt.m_sIndex.cstr ());
		tOut.Error ( tStmt.m_sStmt, sError.cstr ());
		return;
	}

	bool bPq = pServed->m_eType==IndexType_e::PERCOLATE;

	auto * pIndex = (ISphRtIndex *)pServed->m_pIndex;

	// get schema, check values count
	const CSphSchema & tSchema = pIndex->GetMatchSchema ();
	int iSchemaSz = tSchema.GetAttrsCount() + tSchema.GetFieldsCount() + 1;
	if ( pIndex->GetSettings().m_bIndexFieldLens )
		iSchemaSz -= tSchema.GetFieldsCount();
	int iExp = tStmt.m_iSchemaSz;
	int iGot = tStmt.m_dInsertValues.GetLength();
	if ( !tStmt.m_dInsertSchema.GetLength()
		&& iSchemaSz!=tStmt.m_iSchemaSz
		&& !( bPq && (2==tStmt.m_iSchemaSz || 1==tStmt.m_iSchemaSz) ) // pq allows 'query' orand 'tags'
		)
	{
		sError.SetSprintf ( "column count does not match schema (expected %d, got %d)", iSchemaSz, iGot );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	if ( ( iGot % iExp )!=0 )
	{
		sError.SetSprintf ( "column count does not match value count (expected %d, got %d)", iExp, iGot );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	if ( !sError.IsEmpty() )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	CSphVector<int> dAttrSchema ( tSchema.GetAttrsCount() );
	CSphVector<int> dFieldSchema ( tSchema.GetFieldsCount() );
	CSphVector<bool> dFieldAttrs ( tSchema.GetFieldsCount() );
	ARRAY_FOREACH ( i, dFieldAttrs )
		dFieldAttrs[i] = false;

	int iIdIndex = 0;
	if ( !tStmt.m_dInsertSchema.GetLength() )
	{
		// no columns list, use index schema
		ARRAY_FOREACH ( i, dFieldSchema )
			dFieldSchema[i] = i + 1;
		int iFields = dFieldSchema.GetLength ();
		ARRAY_FOREACH ( j, dAttrSchema )
			dAttrSchema[j] = j + iFields + 1;

		// schemaless pq - expect either just 'query', or 'query' and 'tags'. And no id.
		if ( bPq )
		{
			iIdIndex	   = -1;
			dAttrSchema[0] = 0; // query
			dAttrSchema[1] = ( 1==tStmt.m_iSchemaSz ) ? -1 : 1; // tags
			dAttrSchema[2] = -1; // filters
		}
	} else
	{
		// got a list of columns, check for 1) existance, 2) dupes
		StrVec_t dCheck = tStmt.m_dInsertSchema;
		ARRAY_FOREACH ( i, dCheck )
			// OPTIMIZE! GetAttrIndex and GetFieldIndex use the linear searching. M.b. hash instead?
			if ( dCheck[i]!="id" && tSchema.GetAttrIndex ( dCheck[i].cstr() )==-1 && tSchema.GetFieldIndex ( dCheck[i].cstr() )==-1 )
			{
				sError.SetSprintf ( "unknown column: '%s'", dCheck[i].cstr() );
				tOut.Error ( tStmt.m_sStmt, sError.cstr(), MYSQL_ERR_PARSE_ERROR );
				return;
			}

		dCheck.Sort();
		for ( int i=1; i<dCheck.GetLength(); i++ )
			if ( dCheck[i-1]==dCheck[i] )
			{
				sError.SetSprintf ( "column '%s' specified twice", dCheck[i].cstr() );
				tOut.Error ( tStmt.m_sStmt, sError.cstr(), MYSQL_ERR_FIELD_SPECIFIED_TWICE );
				return;
			}

		// hash column list
		// OPTIMIZE! hash index columns once (!) instead
		SmallStringHash_T<int> dInsertSchema;
		ARRAY_FOREACH ( i, tStmt.m_dInsertSchema )
			dInsertSchema.Add ( i, tStmt.m_dInsertSchema[i] );

		// get id index
		if ( !dInsertSchema.Exists("id") )
		{
			if ( !bPq )
			{
				tOut.Error ( tStmt.m_sStmt, "column list must contain an 'id' column" );
				return;
			}
			iIdIndex = -1;
		} else
			iIdIndex = dInsertSchema["id"];

		// map fields
		bool bIdDupe = false;
		ARRAY_FOREACH ( i, dFieldSchema )
		{
			if ( dInsertSchema.Exists ( tSchema.GetFieldName(i) ) )
			{
				int iField = dInsertSchema[tSchema.GetFieldName(i)];
				if ( iField==iIdIndex )
				{
					bIdDupe = true;
					break;
				}
				dFieldSchema[i] = iField;

				// does an attribute with the same name exist?
				if ( tSchema.GetAttr ( tSchema.GetFieldName(i) ) )
					dFieldAttrs[i] = true;
			} else
				dFieldSchema[i] = -1;
		}
		if ( bIdDupe )
		{
			tOut.Error ( tStmt.m_sStmt, "fields must never be named 'id' (fix your config)" );
			return;
		}

		// map attrs
		ARRAY_FOREACH ( j, dAttrSchema )
		{
			if ( dInsertSchema.Exists ( tSchema.GetAttr(j).m_sName ) )
			{
				int iField = dInsertSchema[tSchema.GetAttr(j).m_sName];
				if ( iField==iIdIndex )
				{
					bIdDupe = true;
					break;
				}
				dAttrSchema[j] = iField;
			} else
				dAttrSchema[j] = -1;
		}
		if ( bIdDupe )
		{
			sError.SetSprintf ( "attributes must never be named 'id' (fix your config)" );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
	}

	CSphVector<DWORD> dMvas;
	CSphVector<const char *> dStrings;
	StringPtrTraits_t tStrings;
	tStrings.m_dOff.Reset ( tSchema.GetAttrsCount() );
	ISphRtAccum* pAccum = tAcc.GetAcc ( pIndex, sError );

	// convert attrs
	for ( int c=0; c<tStmt.m_iRowsAffected; ++c )
	{
		assert ( sError.IsEmpty() );

		// pq specific: only ONE row allowed for now (remove this check on #684 resolving)
		if ( c && bPq )
		{
			sError.SetSprintf ( "PQ for now allows only 1 rows per insert, %d provided", tStmt.m_iRowsAffected );
			break;
		}

		CSphMatchVariant tDoc;
		tDoc.Reset ( tSchema.GetRowSize() );
		if ( iIdIndex>=0 )
			tDoc.m_uDocID = CSphMatchVariant::ToDocid ( tStmt.m_dInsertValues[iIdIndex + c * iExp] );
		else {
			assert ( bPq );
			assert ( tDoc.m_uDocID == 0 );
		}
		dStrings.Resize ( 0 );
		tStrings.Reset();
		dMvas.Resize ( 0 );

		int iSchemaAttrCount = tSchema.GetAttrsCount();
		if ( pIndex->GetSettings().m_bIndexFieldLens )
			iSchemaAttrCount -= tSchema.GetFieldsCount();
		for ( int i=0; i<iSchemaAttrCount; i++ )
		{
			// shortcuts!
			const CSphColumnInfo & tCol = tSchema.GetAttr(i);
			CSphAttrLocator tLoc = tCol.m_tLocator;
			tLoc.m_bDynamic = true;

			int iQuerySchemaIdx = dAttrSchema[i];
			bool bResult;
			if ( iQuerySchemaIdx < 0 )
			{
				bResult = tDoc.SetDefaultAttr ( tLoc, tCol.m_eAttrType );
				if ( tCol.m_eAttrType==SPH_ATTR_STRING || tCol.m_eAttrType==SPH_ATTR_STRINGPTR || tCol.m_eAttrType==SPH_ATTR_JSON )
					dStrings.Add ( nullptr );
				if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
					dMvas.Add ( 0 );
			} else
			{
				const SqlInsert_t & tVal = tStmt.m_dInsertValues[iQuerySchemaIdx + c * iExp];

				// sanity checks
				if ( tVal.m_iType!=TOK_QUOTED_STRING
					&& tVal.m_iType!=TOK_CONST_INT
					&& tVal.m_iType!=TOK_CONST_FLOAT
					&& tVal.m_iType!=TOK_CONST_MVA )
				{
					sError.SetSprintf ( "row %d, column %d: internal error: unknown insval type %d", 1+c, 1+iQuerySchemaIdx, tVal.m_iType ); // 1 for human base
					break;
				}
				if ( tVal.m_iType==TOK_CONST_MVA
					&& !( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET ) )
				{
					sError.SetSprintf ( "row %d, column %d: MVA value specified for a non-MVA column", 1+c, 1+iQuerySchemaIdx ); // 1 for human base
					break;
				}
				if ( ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
				&& tVal.m_iType!=TOK_CONST_MVA )
				{
					sError.SetSprintf ( "row %d, column %d: non-MVA value specified for a MVA column", 1+c, 1+iQuerySchemaIdx ); // 1 for human base
					break;
				}

				// ok, checks passed; do work
				// MVA column? grab the values
				if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET )
				{
					// collect data from scattered insvals
					// FIXME! maybe remove this mess, and just have a single m_dMvas pool in parser instead?
					int iLen = 0;
					if ( tVal.m_pVals )
					{
						tVal.m_pVals->Uniq();
						iLen = tVal.m_pVals->GetLength();
					}
					if ( tCol.m_eAttrType==SPH_ATTR_INT64SET )
					{
						dMvas.Add ( iLen*2 );
						for ( int j=0; j<iLen; j++ )
						{
							uint64_t uVal = ( *tVal.m_pVals )[j];
							DWORD uLow = (DWORD)uVal;
							DWORD uHi = (DWORD)( uVal>>32 );
							dMvas.Add ( uLow );
							dMvas.Add ( uHi );
						}
					} else
					{
						dMvas.Add ( iLen );
						for ( int j=0; j<iLen; j++ )
							dMvas.Add ( (DWORD)( *tVal.m_pVals )[j] );
					}
				}

				// FIXME? index schema is lawfully static, but our temp match obviously needs to be dynamic
				bResult = tDoc.SetAttr ( tLoc, tVal, tCol.m_eAttrType );
				if ( tCol.m_eAttrType==SPH_ATTR_STRING || tCol.m_eAttrType==SPH_ATTR_STRINGPTR)
				{
					if ( tVal.m_sVal.Length() > 0x3FFFFF )
					{
						*( char * ) ( tVal.m_sVal.cstr () + 0x3FFFFF ) = '\0';
						sWarning.SetSprintf ( "String column %d at row %d too long, truncated to 4MB", i, c );
					}
					dStrings.Add ( tVal.m_sVal.cstr() );
				} else if ( tCol.m_eAttrType==SPH_ATTR_JSON )
				{
					int iStrCount = dStrings.GetLength();
					dStrings.Add ( nullptr );

					// empty source string means NULL attribute
					if ( !tVal.m_sVal.IsEmpty() )
					{
						// sphJsonParse must be terminated with a double zero however usual CSphString have SAFETY_GAP of 4 zeros
						if ( !String2JsonPack ( (char *)tVal.m_sVal.cstr(), tStrings.m_dParserBuf, sError, sWarning ) )
							break;

						tStrings.AddBlob ( tStrings.m_dParserBuf, iStrCount );
					}
				}
			}

			if ( !bResult )
			{
				sError.SetSprintf ( "internal error: unknown attribute type in INSERT (typeid=%d)", tCol.m_eAttrType );
				break;
			}
		}
		if ( !sError.IsEmpty() )
			break;

		// remap JSON to string pointers
		tStrings.SavePointersTo ( dStrings );

		// convert fields
		CSphVector<VecTraits_T<const char>> dFields;

		// if strings and fields share one value, it might be modified by html stripper etc
		// we need to use separate storage for such string attributes and fields
		StrVec_t dTmpFieldStorage;
		dTmpFieldStorage.Resize(tSchema.GetFieldsCount());

		for ( int i = 0; i < tSchema.GetFieldsCount(); i++ )
		{
			int iQuerySchemaIdx = dFieldSchema[i];
			if ( iQuerySchemaIdx < 0 )
				dFields.Add (); // default value
			else
			{
				if ( tStmt.m_dInsertValues [ iQuerySchemaIdx + c * iExp ].m_iType!=TOK_QUOTED_STRING )
				{
					sError.SetSprintf ( "row %d, column %d: string expected", 1+c, 1+iQuerySchemaIdx ); // 1 for human base
					break;
				}

				const char * szFieldValue = tStmt.m_dInsertValues[ iQuerySchemaIdx + c * iExp ].m_sVal.cstr();
				if ( dFieldAttrs[i] )
				{
					dTmpFieldStorage[i] = szFieldValue;
					dFields.Add ( { dTmpFieldStorage[i].cstr(), dTmpFieldStorage[i].Length() } );
				} else
					dFields.Add ( { szFieldValue, ( int64_t) strlen(szFieldValue) } );
			}
		}
		if ( !sError.IsEmpty() )
			break;

		// do add
		if ( bPq )
		{
			if ( iIdIndex >= 0 && !tDoc.m_uDocID )
			{
				sError.SetSprintf ( "'id' column parsed as 0. Omit the column to enable auto-id" );
				break;
			}
			if ( CheckIndexCluster ( tStmt.m_sIndex, *pServed, tStmt.m_sCluster, sError ) ) {
				const CSphSchema& tSchema = pIndex->GetInternalSchema ();

				CSphVector<CSphFilterSettings> dFilters;
				CSphVector<FilterTreeItem_t>   dFilterTree;
				if ( !PercolateParseFilters ( dStrings[2], eCollation, tSchema, dFilters, dFilterTree, sError ) )
					break;

				PercolateQueryArgs_t tArgs ( dFilters, dFilterTree );
				tArgs.m_sQuery   = dStrings[0];
				tArgs.m_sTags	= dStrings[1];
				tArgs.m_uQUID	= tDoc.m_uDocID;
				tArgs.m_bReplace = bReplace;
				tArgs.m_bQL		 = true;

				// add query
				auto* pQIndex = (PercolateIndex_i *) pIndex;
				auto pStored = pQIndex->Query ( tArgs, sError );
				if ( pStored )
				{
					ReplicationCommand_t tCmd;
					tCmd.m_eCommand = RCOMMAND_PQUERY_ADD;
					tCmd.m_sIndex   = tStmt.m_sIndex;
					tCmd.m_sCluster = tStmt.m_sCluster;
					tCmd.m_pStored  = pStored;

					HandleCmdReplicate ( tCmd, sError, nullptr );
				}
			}

		}
		else
			pIndex->AddDocument ( dFields, tDoc, bReplace, tStmt.m_sStringParam, dStrings.Begin(),
				  dMvas, sError, sWarning, pAccum );

		if ( !sError.IsEmpty() )
			break;
	}

	// fire exit
	if ( !sError.IsEmpty() )
	{
		pIndex->RollBack ( pAccum ); // clean up collected data
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	// no errors so far
	if ( bCommit && !bPq )
		pIndex->Commit ( nullptr, pAccum );

	// my OK packet
	tOut.Ok ( tStmt.m_iRowsAffected, sWarning );
}


// our copy of enum_server_command
// we can't rely on mysql_com.h because it might be unavailable
//
// MYSQL_COM_SLEEP = 0
// MYSQL_COM_QUIT = 1
// MYSQL_COM_INIT_DB = 2
// MYSQL_COM_QUERY = 3
// MYSQL_COM_FIELD_LIST = 4
// MYSQL_COM_CREATE_DB = 5
// MYSQL_COM_DROP_DB = 6
// MYSQL_COM_REFRESH = 7
// MYSQL_COM_SHUTDOWN = 8
// MYSQL_COM_STATISTICS = 9
// MYSQL_COM_PROCESS_INFO = 10
// MYSQL_COM_CONNECT = 11
// MYSQL_COM_PROCESS_KILL = 12
// MYSQL_COM_DEBUG = 13
// MYSQL_COM_PING = 14
// MYSQL_COM_TIME = 15
// MYSQL_COM_DELAYED_INSERT = 16
// MYSQL_COM_CHANGE_USER = 17
// MYSQL_COM_BINLOG_DUMP = 18
// MYSQL_COM_TABLE_DUMP = 19
// MYSQL_COM_CONNECT_OUT = 20
// MYSQL_COM_REGISTER_SLAVE = 21
// MYSQL_COM_STMT_PREPARE = 22
// MYSQL_COM_STMT_EXECUTE = 23
// MYSQL_COM_STMT_SEND_LONG_DATA = 24
// MYSQL_COM_STMT_CLOSE = 25
// MYSQL_COM_STMT_RESET = 26
// MYSQL_COM_SET_OPTION = 27
// MYSQL_COM_STMT_FETCH = 28

enum
{
	MYSQL_COM_QUIT		= 1,
	MYSQL_COM_INIT_DB	= 2,
	MYSQL_COM_QUERY		= 3,
	MYSQL_COM_PING		= 14,
	MYSQL_COM_SET_OPTION	= 27
};


void HandleMysqlCallSnippets ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt, ThdDesc_t & tThd )
{
	CSphString sError;

	// check arguments
	// string data, string index, string query, [named opts]
	if ( tStmt.m_dInsertValues.GetLength()!=3 )
	{
		tOut.Error ( tStmt.m_sStmt, "SNIPPETS() expects exactly 3 arguments (data, index, query)" );
		return;
	}
	if ( tStmt.m_dInsertValues[0].m_iType!=TOK_QUOTED_STRING && tStmt.m_dInsertValues[0].m_iType!=TOK_CONST_STRINGS )
	{
		tOut.Error ( tStmt.m_sStmt, "SNIPPETS() argument 1 must be a string or a string list" );
		return;
	}
	if ( tStmt.m_dInsertValues[1].m_iType!=TOK_QUOTED_STRING )
	{
		tOut.Error ( tStmt.m_sStmt, "SNIPPETS() argument 2 must be a string" );
		return;
	}
	if ( tStmt.m_dInsertValues[2].m_iType!=TOK_QUOTED_STRING )
	{
		tOut.Error ( tStmt.m_sStmt, "SNIPPETS() argument 3 must be a string" );
		return;
	}

	// do magics
	CSphString sIndex = tStmt.m_dInsertValues[1].m_sVal;

	ExcerptQueryChained_t q;
	q.m_sWords = tStmt.m_dInsertValues[2].m_sVal;

	ARRAY_FOREACH ( i, tStmt.m_dCallOptNames )
	{
		CSphString & sOpt = tStmt.m_dCallOptNames[i];
		const SqlInsert_t & v = tStmt.m_dCallOptValues[i];

		sOpt.ToLower();
		int iExpType = -1;

		if ( sOpt=="before_match" )				{ q.m_sBeforeMatch = v.m_sVal; iExpType = TOK_QUOTED_STRING; }
		else if ( sOpt=="after_match" )			{ q.m_sAfterMatch = v.m_sVal; iExpType = TOK_QUOTED_STRING; }
		else if ( sOpt=="chunk_separator" )		{ q.m_sChunkSeparator = v.m_sVal; iExpType = TOK_QUOTED_STRING; }
		else if ( sOpt=="html_strip_mode" )		{ q.m_sStripMode = v.m_sVal; iExpType = TOK_QUOTED_STRING; }
		else if ( sOpt=="passage_boundary" )	{ q.m_ePassageSPZ = GetPassageBoundary(v.m_sVal); iExpType = TOK_QUOTED_STRING; }

		else if ( sOpt=="limit" )				{ q.m_iLimit = (int)v.m_iVal; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="limit_words" )			{ q.m_iLimitWords = (int)v.m_iVal; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="limit_passages" )		{ q.m_iLimitPassages = (int)v.m_iVal; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="around" )				{ q.m_iAround = (int)v.m_iVal; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="start_passage_id" )	{ q.m_iPassageId = (int)v.m_iVal; iExpType = TOK_CONST_INT; }

		else if ( sOpt=="exact_phrase" )		{ q.m_bExactPhrase = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="use_boundaries" )		{ q.m_bUseBoundaries = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="weight_order" )		{ q.m_bWeightOrder = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="query_mode" )			{ q.m_bHighlightQuery = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="force_all_words" )		{ q.m_bForceAllWords = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="load_files" )			{ q.m_uFilesMode = ( v.m_iVal!=0 )?1:0; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="load_files_scattered" ) { q.m_uFilesMode |= ( v.m_iVal!=0 )?2:0; iExpType = TOK_CONST_INT; }
		else if ( sOpt=="allow_empty" )			{ q.m_bAllowEmpty = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="emit_zones" )			{ q.m_bEmitZones = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }
		else if ( sOpt=="force_passages" )		{ q.m_bForcePassages = ( v.m_iVal!=0 ); iExpType = TOK_CONST_INT; }

		else
		{
			sError.SetSprintf ( "unknown option %s", sOpt.cstr() );
			break;
		}

		// post-conf type check
		if ( iExpType!=v.m_iType )
		{
			sError.SetSprintf ( "unexpected option %s type", sOpt.cstr() );
			break;
		}
	}
	if ( !sError.IsEmpty() )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	if ( !sphCheckOptionsSPZ ( q, q.m_ePassageSPZ, sError ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	q.m_bHasBeforePassageMacro = SnippetTransformPassageMacros ( q.m_sBeforeMatch, q.m_sBeforeMatchPassage );
	q.m_bHasAfterPassageMacro = SnippetTransformPassageMacros ( q.m_sAfterMatch, q.m_sAfterMatchPassage );

	CSphVector<ExcerptQueryChained_t> dQueries;
	if ( tStmt.m_dInsertValues[0].m_iType==TOK_QUOTED_STRING )
	{
		q.m_sSource = tStmt.m_dInsertValues[0].m_sVal; // OPTIMIZE?
		dQueries.Add ( q );
	} else
	{
		dQueries.Resize ( tStmt.m_dCallStrings.GetLength() );
		ARRAY_FOREACH ( i, tStmt.m_dCallStrings )
		{
			dQueries[i] = q; // copy the settings
			dQueries[i].m_sSource = tStmt.m_dCallStrings[i]; // OPTIMIZE?
		}
	}

	ThreadSetSnippetInfo ( dQueries[0].m_sWords.scstr (), GetSnippetDataSize ( dQueries ), true, tThd );

	if ( !MakeSnippets ( sIndex, dQueries, sError, tThd ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	if ( !dQueries.FindFirst ( [] ( const ExcerptQuery_t& dQuery ) { return !dQuery.m_dRes.IsEmpty(); } ) )
	{
		// just one last error instead of all errors is hopefully ok
		sError.SetSprintf ( "highlighting failed: %s", sError.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	// result set header packet
	tOut.HeadBegin(1);
	tOut.HeadColumn("snippet");
	tOut.HeadEnd();

	// data
	for ( auto & dQuery : dQueries )
	{
		auto &dData = dQuery.m_dRes;
		FixupResultTail ( dData );
		tOut.PutArray ( dData );
		tOut.Commit();
	}
	tOut.Eof();
}


struct KeywordsRequestBuilder_t : public IRequestBuilder_t
{
	KeywordsRequestBuilder_t ( const GetKeywordsSettings_t & tSettings, const CSphString & sTerm );
	void BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const final;

protected:
	const GetKeywordsSettings_t & m_tSettings;
	const CSphString & m_sTerm;
};


struct KeywordsReplyParser_t : public IReplyParser_t
{
	KeywordsReplyParser_t ( bool bGetStats, CSphVector<CSphKeywordInfo> & dKeywords );
	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & tAgent ) const final;

	bool m_bStats;
	CSphVector<CSphKeywordInfo> & m_dKeywords;
};

static void MergeKeywords ( CSphVector<CSphKeywordInfo> & dKeywords );
static void SortKeywords ( const GetKeywordsSettings_t & tSettings, CSphVector<CSphKeywordInfo> & dKeywords );
bool DoGetKeywords ( const CSphString & sIndex, const CSphString & sQuery, const GetKeywordsSettings_t & tSettings, CSphVector <CSphKeywordInfo> & dKeywords, CSphString & sError, SearchFailuresLog_c & tFailureLog )
{
	auto pLocal = GetServed ( sIndex );
	auto pDistributed = GetDistr ( sIndex );

	if ( !pLocal && !pDistributed )
	{
		sError.SetSprintf ( "no such index %s", sIndex.cstr() );
		return false;
	}

	bool bOk = false;
	// just local plain or template index
	if ( pLocal )
	{
		ServedDescRPtr_c pIndex ( pLocal );
		if ( !pIndex->m_pIndex )
		{
			sError.SetSprintf ( "missed index '%s'", sIndex.cstr() );
		} else
		{
			bOk = pIndex->m_pIndex->GetKeywords ( dKeywords, sQuery.cstr(), tSettings, &sError );
		}
	} else
	{
		// FIXME!!! g_iDistThreads thread pool for locals.
		// locals
		const StrVec_t & dLocals = pDistributed->m_dLocal;
		CSphVector<CSphKeywordInfo> dKeywordsLocal;
		for ( const CSphString &sLocal : dLocals )
		{
			ServedDescRPtr_c pServed ( GetServed ( sLocal ) );
			if ( !pServed || !pServed->m_pIndex )
			{
				tFailureLog.Submit ( sLocal.cstr(), sIndex.cstr(), "missed index" );
				continue;
			}

			dKeywordsLocal.Resize(0);
			if ( pServed->m_pIndex->GetKeywords ( dKeywordsLocal, sQuery.cstr(), tSettings, &sError ) )
				dKeywords.Append ( dKeywordsLocal );
			else
				tFailureLog.SubmitEx ( sLocal.cstr (), sIndex.cstr (), "keyword extraction failed: %s", sError.cstr () );
		}

		// remote agents requests send off thread
		VecRefPtrsAgentConn_t dAgents;
		// fixme! We don't need all hosts here, only usual selected mirrors
		pDistributed->GetAllHosts ( dAgents );

		int iAgentsReply = 0;
		if ( !dAgents.IsEmpty() )
		{
			// connect to remote agents and query them
			KeywordsRequestBuilder_t tReqBuilder ( tSettings, sQuery );
			KeywordsReplyParser_t tParser ( tSettings.m_bStats, dKeywords );
			iAgentsReply = PerformRemoteTasks ( dAgents, &tReqBuilder, &tParser );

			for ( const AgentConn_t * pAgent : dAgents )
				if ( !pAgent->m_bSuccess && !pAgent->m_sFailure.IsEmpty() )
					tFailureLog.SubmitEx ( pAgent->m_tDesc.m_sIndexes.cstr(), sIndex.cstr(),
						"agent %s: %s", pAgent->m_tDesc.GetMyUrl().cstr(), pAgent->m_sFailure.cstr() );
		}

		// process result sets
		if ( dLocals.GetLength() + iAgentsReply>1 )
			MergeKeywords ( dKeywords );

		bOk = true;
	}

	SortKeywords ( tSettings, dKeywords );

	return bOk;
}

void HandleMysqlCallKeywords ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt, CSphString & sWarning )
{
	CSphString sError;

	// string query, string index, [bool hits] || [value as option_name, ...]
	int iArgs = tStmt.m_dInsertValues.GetLength();
	if ( iArgs<2
		|| iArgs>3
		|| tStmt.m_dInsertValues[0].m_iType!=TOK_QUOTED_STRING
		|| tStmt.m_dInsertValues[1].m_iType!=TOK_QUOTED_STRING
		|| ( iArgs==3 && tStmt.m_dInsertValues[2].m_iType!=TOK_CONST_INT ) )
	{
		tOut.Error ( tStmt.m_sStmt, "bad argument count or types in KEYWORDS() call" );
		return;
	}

	GetKeywordsSettings_t tSettings;
	tSettings.m_bStats = ( iArgs==3 && tStmt.m_dInsertValues[2].m_iVal!=0 );
	ARRAY_FOREACH ( i, tStmt.m_dCallOptNames )
	{
		CSphString & sOpt = tStmt.m_dCallOptNames[i];
		sOpt.ToLower ();
		bool bEnabled = ( tStmt.m_dCallOptValues[i].m_iVal!=0 );
		bool bOptInt = true;

		if ( sOpt=="stats" )
			tSettings.m_bStats = bEnabled;
		else if ( sOpt=="fold_lemmas" )
			tSettings.m_bFoldLemmas = bEnabled;
		else if ( sOpt=="fold_blended" )
			tSettings.m_bFoldBlended = bEnabled;
		else if ( sOpt=="fold_wildcards" )
			tSettings.m_bFoldWildcards = bEnabled;
		else if ( sOpt=="expansion_limit" )
			tSettings.m_iExpansionLimit = int ( tStmt.m_dCallOptValues[i].m_iVal );
		else if ( sOpt=="sort_mode" )
		{
			// FIXME!!! add more sorting modes
			if ( tStmt.m_dCallOptValues[i].m_sVal!="docs" && tStmt.m_dCallOptValues[i].m_sVal!="hits" )
			{
				sError.SetSprintf ( "unknown option %s mode '%s'", sOpt.cstr(), tStmt.m_dCallOptValues[i].m_sVal.cstr() );
				tOut.Error ( tStmt.m_sStmt, sError.cstr() );
				return;
			}
			tSettings.m_bSortByDocs = ( tStmt.m_dCallOptValues[i].m_sVal=="docs" );
			tSettings.m_bSortByHits = ( tStmt.m_dCallOptValues[i].m_sVal=="hits" );
			bOptInt = false;
						
		} else
		{
			sError.SetSprintf ( "unknown option %s", sOpt.cstr () );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}

		// post-conf type check
		if ( bOptInt && tStmt.m_dCallOptValues[i].m_iType!=TOK_CONST_INT )
		{
			sError.SetSprintf ( "unexpected option %s type", sOpt.cstr () );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}
	}
	const CSphString & sTerm = tStmt.m_dInsertValues[0].m_sVal;
	const CSphString & sIndex = tStmt.m_dInsertValues[1].m_sVal;
	CSphVector<CSphKeywordInfo> dKeywords;
	SearchFailuresLog_c tFailureLog;

	if ( !DoGetKeywords ( sIndex, sTerm, tSettings, dKeywords, sError, tFailureLog ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}


	// result set header packet
	tOut.HeadBegin ( tSettings.m_bStats ? 5 : 3 );
	tOut.HeadColumn("qpos");
	tOut.HeadColumn("tokenized");
	tOut.HeadColumn("normalized");
	if ( tSettings.m_bStats )
	{
		tOut.HeadColumn("docs");
		tOut.HeadColumn("hits");
	}
	tOut.HeadEnd();

	// data
	char sBuf[16];
	ARRAY_FOREACH ( i, dKeywords )
	{
		snprintf ( sBuf, sizeof(sBuf), "%d", dKeywords[i].m_iQpos );
		tOut.PutString ( sBuf );
		tOut.PutString ( dKeywords[i].m_sTokenized );
		tOut.PutString ( dKeywords[i].m_sNormalized );
		if ( tSettings.m_bStats )
		{
			snprintf ( sBuf, sizeof(sBuf), "%d", dKeywords[i].m_iDocs );
			tOut.PutString ( sBuf );
			snprintf ( sBuf, sizeof(sBuf), "%d", dKeywords[i].m_iHits );
			tOut.PutString ( sBuf );
		}
		tOut.Commit();
	}

	// put network errors and warnings to meta as warning
	int iWarnings = 0;
	if ( !tFailureLog.IsEmpty() )
	{
		iWarnings = tFailureLog.GetReportsCount();

		StringBuilder_c sErrorBuf;
		tFailureLog.BuildReport ( sErrorBuf );
		sErrorBuf.MoveTo ( sWarning );
		sphWarning ( "%s", sWarning.cstr() );
	}

	tOut.Eof ( false, iWarnings );
}

KeywordsRequestBuilder_t::KeywordsRequestBuilder_t ( const GetKeywordsSettings_t & tSettings, const CSphString & sTerm )
	: m_tSettings ( tSettings )
	, m_sTerm ( sTerm )
{
}

void KeywordsRequestBuilder_t::BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const
{
	const CSphString & sIndexes = tAgent.m_tDesc.m_sIndexes;

	APICommand_t tWr { tOut, SEARCHD_COMMAND_KEYWORDS, VER_COMMAND_KEYWORDS };

	tOut.SendString ( m_sTerm.cstr() );
	tOut.SendString ( sIndexes.cstr() );
	tOut.SendInt ( m_tSettings.m_bStats );
	tOut.SendInt ( m_tSettings.m_bFoldLemmas );
	tOut.SendInt ( m_tSettings.m_bFoldBlended );
	tOut.SendInt ( m_tSettings.m_bFoldWildcards );
	tOut.SendInt ( m_tSettings.m_iExpansionLimit );
}

KeywordsReplyParser_t::KeywordsReplyParser_t ( bool bGetStats, CSphVector<CSphKeywordInfo> & dKeywords )
	: m_bStats ( bGetStats )
	, m_dKeywords ( dKeywords )
{
}

bool KeywordsReplyParser_t::ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const
{
	int iWords = tReq.GetInt();
	int iLen = m_dKeywords.GetLength();
	m_dKeywords.Resize ( iWords + iLen );
	for ( int i=0; i<iWords; i++ )
	{
		CSphKeywordInfo & tWord = m_dKeywords[i + iLen];
		tWord.m_sTokenized = tReq.GetString();
		tWord.m_sNormalized = tReq.GetString();
		tWord.m_iQpos = tReq.GetInt();
		if ( m_bStats )
		{
			tWord.m_iDocs = tReq.GetInt();
			tWord.m_iHits = tReq.GetInt();
		}
	}

	return true;
}

struct KeywordSorter_fn
{
	bool IsLess ( const CSphKeywordInfo & a, const CSphKeywordInfo & b ) const
	{
		return ( ( a.m_iQpos<b.m_iQpos )
			|| ( a.m_iQpos==b.m_iQpos && a.m_iHits>b.m_iHits )
			|| ( a.m_iQpos==b.m_iQpos && a.m_iHits==b.m_iHits && a.m_sNormalized<b.m_sNormalized ) );
	}
};

void MergeKeywords ( CSphVector<CSphKeywordInfo> & dSrc )
{
	CSphOrderedHash < CSphKeywordInfo, uint64_t, IdentityHash_fn, 256 > hWords;
	ARRAY_FOREACH ( i, dSrc )
	{
		const CSphKeywordInfo & tInfo = dSrc[i];
		uint64_t uKey = sphFNV64 ( &tInfo.m_iQpos, sizeof(tInfo.m_iQpos) );
		uKey = sphFNV64 ( tInfo.m_sNormalized.cstr(), tInfo.m_sNormalized.Length(), uKey );

		CSphKeywordInfo & tVal = hWords.AddUnique ( uKey );
		if ( !tVal.m_iQpos )
		{
			tVal = tInfo;
		} else
		{
			tVal.m_iDocs += tInfo.m_iDocs;
			tVal.m_iHits += tInfo.m_iHits;
		}
	}

	dSrc.Resize ( 0 );
	hWords.IterateStart();
	while ( hWords.IterateNext() )
	{
		dSrc.Add ( hWords.IterateGet() );
	}

	sphSort ( dSrc.Begin(), dSrc.GetLength(), KeywordSorter_fn() );
}

struct KeywordSorterDocs_fn
{
	bool IsLess ( const CSphKeywordInfo & a, const CSphKeywordInfo & b ) const
	{
		return ( ( a.m_iQpos<b.m_iQpos )
			|| ( a.m_iQpos==b.m_iQpos && a.m_iDocs>b.m_iDocs )
			|| ( a.m_iQpos==b.m_iQpos && a.m_iDocs==b.m_iDocs && a.m_sNormalized<b.m_sNormalized ) );
	}
};


void SortKeywords ( const GetKeywordsSettings_t & tSettings, CSphVector<CSphKeywordInfo> & dKeywords )
{
	if ( !tSettings.m_bSortByDocs && !tSettings.m_bSortByHits )
		return;

	if ( tSettings.m_bSortByHits )
		dKeywords.Sort ( KeywordSorter_fn() );
	else
		dKeywords.Sort ( KeywordSorterDocs_fn() );
}

// sort by distance asc, document count desc, ABC asc
struct CmpDistDocABC_fn
{
	const char * m_pBuf;
	explicit CmpDistDocABC_fn ( const char * pBuf ) : m_pBuf ( pBuf ) {}

	inline bool IsLess ( const SuggestWord_t & a, const SuggestWord_t & b ) const
	{
		if ( a.m_iDistance==b.m_iDistance && a.m_iDocs==b.m_iDocs )
		{
			return ( sphDictCmpStrictly ( m_pBuf + a.m_iNameOff, a.m_iLen, m_pBuf + b.m_iNameOff, b.m_iLen )<0 );
		}

		if ( a.m_iDistance==b.m_iDistance )
			return a.m_iDocs>=b.m_iDocs;
		return a.m_iDistance<b.m_iDistance;
	}
};

void HandleMysqlCallSuggest ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt, bool bQueryMode )
{
	CSphString sError;

	// string query, string index, [value as option_name, ...]
	int iArgs = tStmt.m_dInsertValues.GetLength ();
	if ( iArgs<2
			|| iArgs>3
			|| tStmt.m_dInsertValues[0].m_iType!=TOK_QUOTED_STRING
			|| tStmt.m_dInsertValues[1].m_iType!=TOK_QUOTED_STRING
			|| ( iArgs==3 && tStmt.m_dInsertValues[2].m_iType!=TOK_CONST_INT ) )
	{
		tOut.Error ( tStmt.m_sStmt, "bad argument count or types in KEYWORDS() call" );
		return;
	}

	SuggestArgs_t tArgs;
	SuggestResult_t tRes;
	const char * sWord = tStmt.m_dInsertValues[0].m_sVal.cstr();
	tArgs.m_bQueryMode = bQueryMode;

	ARRAY_FOREACH ( i, tStmt.m_dCallOptNames )
	{
		CSphString & sOpt = tStmt.m_dCallOptNames[i];
		sOpt.ToLower ();
		int iTokType = TOK_CONST_INT;

		if ( sOpt=="limit" )
		{
			tArgs.m_iLimit = int ( tStmt.m_dCallOptValues[i].m_iVal );
		} else if ( sOpt=="delta_len" )
		{
			tArgs.m_iDeltaLen = int ( tStmt.m_dCallOptValues[i].m_iVal );
		} else if ( sOpt=="max_matches" )
		{
			tArgs.m_iQueueLen = int ( tStmt.m_dCallOptValues[i].m_iVal );
		} else if ( sOpt=="reject" )
		{
			tArgs.m_iRejectThr = int ( tStmt.m_dCallOptValues[i].m_iVal );
		} else if ( sOpt=="max_edits" )
		{
			tArgs.m_iMaxEdits = int ( tStmt.m_dCallOptValues[i].m_iVal );
		} else if ( sOpt=="result_line" )
		{
			tArgs.m_bResultOneline = ( tStmt.m_dCallOptValues[i].m_iVal!=0 );
		} else if ( sOpt=="result_stats" )
		{
			tArgs.m_bResultStats = ( tStmt.m_dCallOptValues[i].m_iVal!=0 );
		} else if ( sOpt=="non_char" )
		{
			tArgs.m_bNonCharAllowed = ( tStmt.m_dCallOptValues[i].m_iVal!=0 );
		} else
		{
			sError.SetSprintf ( "unknown option %s", sOpt.cstr () );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}

		// post-conf type check
		if ( tStmt.m_dCallOptValues[i].m_iType!=iTokType )
		{
			sError.SetSprintf ( "unexpected option %s type", sOpt.cstr () );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}
	}



	{ // scope for ServedINdexPtr_c
		ServedDescRPtr_c pServed ( GetServed ( tStmt.m_dInsertValues[1].m_sVal ) );
		if ( !pServed || !pServed->m_pIndex )
		{
			sError.SetSprintf ( "no such index %s", tStmt.m_dInsertValues[1].m_sVal.cstr () );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}
		if ( !pServed->m_pIndex->GetSettings().m_iMinInfixLen || !pServed->m_pIndex->GetDictionary()->GetSettings().m_bWordDict )
		{
			sError.SetSprintf ( "suggests work only for keywords dictionary with infix enabled" );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}

		if ( tRes.SetWord ( sWord, pServed->m_pIndex->GetQueryTokenizer(), tArgs.m_bQueryMode ) )
		{
			pServed->m_pIndex->GetSuggest ( tArgs, tRes );
		}
	}

	// data
	if ( tArgs.m_bResultOneline )
	{
		// let's resort by alphabet to better compare result sets
		CmpDistDocABC_fn tCmp ( (const char *)( tRes.m_dBuf.Begin() ) );
		tRes.m_dMatched.Sort ( tCmp );

		// result set header packet
		tOut.HeadBegin ( 2 );
		tOut.HeadColumn ( "name" );
		tOut.HeadColumn ( "value" );
		tOut.HeadEnd ();

		StringBuilder_c sBuf ( "," );
		for ( auto& dMatched : tRes.m_dMatched )
			sBuf << (const char*) tRes.m_dBuf.Begin() + dMatched.m_iNameOff;
		tOut.PutString ( "suggests" );
		tOut.PutString ( sBuf.cstr() );
		tOut.Commit();

		if ( tArgs.m_bResultStats )
		{
			sBuf.Clear ();
			sBuf.StartBlock ( "," );
			for ( auto &dMatched : tRes.m_dMatched )
				sBuf.Appendf ( "%d", dMatched.m_iDistance );
			tOut.PutString ( "distance" );
			tOut.PutString ( sBuf.cstr () );
			tOut.Commit ();

			sBuf.Clear ();
			sBuf.StartBlock ( "," );
			for ( auto &dMatched : tRes.m_dMatched )
				sBuf.Appendf ( "%d", dMatched.m_iDocs );
			tOut.PutString ( "docs" );
			tOut.PutString ( sBuf.cstr () );
			tOut.Commit ();
		}
	} else
	{
		// result set header packet
		tOut.HeadBegin ( tArgs.m_bResultStats ? 3 : 1 );
		tOut.HeadColumn ( "suggest" );
		if ( tArgs.m_bResultStats )
		{
			tOut.HeadColumn ( "distance" );
			tOut.HeadColumn ( "docs" );
		}
		tOut.HeadEnd ();

		auto * szResult = (const char *)( tRes.m_dBuf.Begin() );
		ARRAY_FOREACH ( i, tRes.m_dMatched )
		{
			const SuggestWord_t & tWord = tRes.m_dMatched[i];
			tOut.PutString ( szResult + tWord.m_iNameOff );
			if ( tArgs.m_bResultStats )
			{
				tOut.PutNumAsString ( tWord.m_iDistance );
				tOut.PutNumAsString ( tWord.m_iDocs );
			}
			tOut.Commit();
		}
	}

	tOut.Eof();
}


void HandleMysqlDescribe ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt )
{
	TableLike dCondOut ( tOut, tStmt.m_sStringParam.cstr () );

	ServedDescRPtr_c pServed ( GetServed ( tStmt.m_sIndex ) );
	if ( pServed && pServed->m_pIndex )
	{
		// result set header packet
		tOut.HeadTuplet ( "Field", "Type" );

		// data
		dCondOut.MatchDataTuplet ( "id", "bigint" );

		const CSphSchema *pSchema = &pServed->m_pIndex->GetMatchSchema ();
		if ( tStmt.m_iIntParam==42 ) // user wants internal schema instead
		{
			if ( pServed->IsMutable () )
			{
				auto pRtIndex = (ISphRtIndex*)pServed->m_pIndex;
				pSchema = &pRtIndex->GetInternalSchema ();
			}
		}

		const CSphSchema &tSchema = *pSchema;
		for ( int i = 0; i<tSchema.GetFieldsCount (); i++ )
			dCondOut.MatchDataTuplet ( tSchema.GetFieldName ( i ), "field" );

		char sTmp[SPH_MAX_WORD_LEN];
		for ( int i = 0; i<tSchema.GetAttrsCount (); i++ )
		{
			const CSphColumnInfo &tCol = tSchema.GetAttr ( i );
			if ( tCol.m_eAttrType==SPH_ATTR_INTEGER && tCol.m_tLocator.m_iBitCount!=ROWITEM_BITS )
			{
				snprintf ( sTmp, sizeof ( sTmp ), "%s:%d", sphTypeName ( tCol.m_eAttrType )
						   , tCol.m_tLocator.m_iBitCount );
				dCondOut.MatchDataTuplet ( tCol.m_sName.cstr (), sTmp );
			} else
			{
				dCondOut.MatchDataTuplet ( tCol.m_sName.cstr (), sphTypeName ( tCol.m_eAttrType ) );
			}
		}
		tOut.Eof ();
		return;
	}

	auto pDistr = GetDistr ( tStmt.m_sIndex );

	if ( !pDistr )
	{
		CSphString sError;
		sError.SetSprintf ( "no such index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr(), MYSQL_ERR_NO_SUCH_TABLE );
		return;
	}

	tOut.HeadTuplet ( "Agent", "Type" );
	for ( const auto& sIdx : pDistr->m_dLocal )
		dCondOut.MatchDataTuplet ( sIdx.cstr(), "local" );

	ARRAY_FOREACH ( i, pDistr->m_dAgents )
	{
		const MultiAgentDesc_c & tMultiAgent = *pDistr->m_dAgents[i];
		if ( tMultiAgent.IsHA() )
		{
			ARRAY_FOREACH ( j, tMultiAgent )
			{
				const AgentDesc_t &tDesc = tMultiAgent[j];
				StringBuilder_c sKey;
				sKey += tDesc.m_bBlackhole ? "blackhole_" : "remote_";
				sKey.Appendf ( "%d_mirror_%d", i+1, j+1 );
				CSphString sValue;
				sValue.SetSprintf ( "%s:%s", tDesc.GetMyUrl().cstr(), tDesc.m_sIndexes.cstr() );
				dCondOut.MatchDataTuplet ( sValue.cstr(), sKey.cstr() );
			}
		} else
		{
			const AgentDesc_t &tDesc = tMultiAgent[0];
			StringBuilder_c sKey;
			sKey+= tDesc.m_bBlackhole?"blackhole_":"remote_";
			sKey.Appendf ( "%d", i+1 );
			CSphString sValue;
			sValue.SetSprintf ( "%s:%s", tDesc.GetMyUrl().cstr(), tDesc.m_sIndexes.cstr() );
			dCondOut.MatchDataTuplet ( sValue.cstr(), sKey.cstr() );
		}
	}
	tOut.Eof();
}


struct IndexNameLess_fn
{
	inline bool IsLess ( const CSphNamedInt & a, const CSphNamedInt & b ) const
	{
		return strcasecmp ( a.m_sName.cstr(), b.m_sName.cstr() )<0;
	}
};


void HandleMysqlShowTables ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt )
{
	// 0 local, 1 distributed, 2 rt, 3 template, 4 percolate, 5 unknown
	static const char* sTypes[] = {"local", "distributed", "rt", "template", "percolate", "unknown"};
	CSphVector<CSphNamedInt> dIndexes;

	// collect local, rt, percolate
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
		if ( it.Get() )
		{
			ServedDescRPtr_c pIdx ( it.Get () );
			switch ( pIdx->m_eType )
			{
				case IndexType_e::PLAIN:
					dIndexes.Add ( CSphNamedInt ( it.GetName (), 0 ) );
					break;
				case IndexType_e::RT:
					dIndexes.Add ( CSphNamedInt ( it.GetName (), 2 ) );
					break;
				case IndexType_e::PERCOLATE:
					dIndexes.Add ( CSphNamedInt ( it.GetName (), 4 ) );
					break;
				case IndexType_e::TEMPLATE:
					dIndexes.Add ( CSphNamedInt ( it.GetName (), 3 ) );
					break;
				default:
					dIndexes.Add ( CSphNamedInt ( it.GetName (), 5 ) );
			}
		}

	// collect distributed
	for ( RLockedDistrIt_c it ( g_pDistIndexes ); it.Next (); )
		// no need to check distr's it, iterating guarantees index existance.
		dIndexes.Add ( CSphNamedInt ( it.GetName (), 1 ) );

	dIndexes.Sort ( IndexNameLess_fn() );

	// output the results
	tOut.HeadTuplet ( "Index", "Type" );
	TableLike dCondOut ( tOut, tStmt.m_sStringParam.cstr() );
	for ( auto& dPair : dIndexes )
		dCondOut.MatchDataTuplet ( dPair.m_sName.cstr (), sTypes[dPair.m_iValue] );
	tOut.Eof();
}

// MySQL Workbench (and maybe other clients) crashes without it
void HandleMysqlShowDatabases ( SqlRowBuffer_c & tOut, SqlStmt_t & )
{
	tOut.HeadBegin ( 1 );
	tOut.HeadColumn ( "Databases" );
	tOut.HeadEnd();
	tOut.Eof();
}


void HandleMysqlShowPlugins ( SqlRowBuffer_c & tOut, SqlStmt_t & )
{
	CSphVector<PluginInfo_t> dPlugins;
	sphPluginList ( dPlugins );

	tOut.HeadBegin ( 5 );
	tOut.HeadColumn ( "Type" );
	tOut.HeadColumn ( "Name" );
	tOut.HeadColumn ( "Library" );
	tOut.HeadColumn ( "Users" );
	tOut.HeadColumn ( "Extra" );
	tOut.HeadEnd();

	ARRAY_FOREACH ( i, dPlugins )
	{
		const PluginInfo_t & p = dPlugins[i];
		tOut.PutString ( g_dPluginTypes[p.m_eType] );
		tOut.PutString ( p.m_sName );
		tOut.PutString ( p.m_sLib );
		tOut.PutNumAsString ( p.m_iUsers );
		tOut.PutString ( p.m_sExtra );
		tOut.Commit();
	}
	tOut.Eof();
}

enum ThreadInfoFormat_e
{
	THD_FORMAT_NATIVE,
	THD_FORMAT_SPHINXQL
};

static const char * FormatInfo ( ThdDesc_t & tThd, ThreadInfoFormat_e eFmt, QuotationEscapedBuilder & tBuf )
{
	ScopedMutex_t pQueryGuard ( tThd.m_tQueryLock );
	if ( tThd.m_pQuery && eFmt==THD_FORMAT_SPHINXQL && tThd.m_eProto!=PROTO_MYSQL41 )
	{
		bool bGotQuery = false;
		if ( tThd.m_pQuery )
		{
			tBuf.Clear();
			FormatSphinxql ( *tThd.m_pQuery, 0, tBuf );
			bGotQuery = true;
		}

		// query might be removed prior to lock then go to common path
		if ( bGotQuery )
			return tBuf.cstr();
	}

	if ( tThd.m_dBuf[0]=='\0' && tThd.m_sCommand )
		return tThd.m_sCommand;
	else
		return tThd.m_dBuf.Begin();
}

void HandleMysqlShowThreads ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	int64_t tmNow = sphMicroTimer();


	tOut.HeadBegin ( 7 );
	tOut.HeadColumn ( "Tid" );
	tOut.HeadColumn ( "Name" );
	tOut.HeadColumn ( "Proto" );
	tOut.HeadColumn ( "State" );
	tOut.HeadColumn ( "Host" );
	tOut.HeadColumn ( "Time" );
	tOut.HeadColumn ( "Info" );
	tOut.HeadEnd();

	QuotationEscapedBuilder tBuf;
	ThreadInfoFormat_e eFmt = THD_FORMAT_NATIVE;
	if ( tStmt.m_sThreadFormat=="sphinxql" )
		eFmt = THD_FORMAT_SPHINXQL;

	ScRL_t dThdLock ( g_tThdLock );
	for ( const ListNode_t * pIt = g_dThd.Begin (); pIt!=g_dThd.End(); pIt = pIt->m_pNext )
	{
		auto * pThd = (ThdDesc_t *)pIt;

		int iLen = strnlen ( pThd->m_dBuf.Begin(), pThd->m_dBuf.GetLength() );
		if ( tStmt.m_iThreadsCols>0 && iLen>tStmt.m_iThreadsCols )
			pThd->m_dBuf[tStmt.m_iThreadsCols] = '\0';

		tOut.PutNumAsString ( pThd->m_iTid );
		tOut.PutString ( GetThreadName ( const_cast <SphThread_t *>(&pThd->m_tThd) ) );
		tOut.PutString ( pThd->m_bSystem ? "-" : g_dProtoNames [ pThd->m_eProto ] );
		tOut.PutString ( pThd->m_bSystem ? "-" : g_dThdStates [ pThd->m_eThdState ] );
		tOut.PutString ( pThd->m_sClientName );
		tOut.PutMicrosec ( tmNow - pThd->m_tmStart );
		tOut.PutString ( FormatInfo ( *pThd, eFmt, tBuf ) );

		tOut.Commit();
	}

	tOut.Eof();

}

void HandleMysqlFlushHostnames ( SqlRowBuffer_c & tOut )
{
	SmallStringHash_T<DWORD> hHosts;

	// Collect all urls from all distr indexes
	for ( RLockedDistrIt_c it ( g_pDistIndexes ); it.Next (); )
		it.Get ()->ForEveryHost ( [&] ( AgentDesc_t &tDesc )
		{
			if ( tDesc.m_bNeedResolve )
				hHosts.Add ( tDesc.m_uAddr, tDesc.m_sAddr );
		});


	for ( hHosts.IterateStart (); hHosts.IterateNext(); )
	{
		DWORD uRenew = sphGetAddress ( hHosts.IterateGetKey().cstr() );
		if ( uRenew )
			hHosts.IterateGet() = uRenew;
	}

	// copy back renew hosts to distributed agents.
	// case when distr index list changed between collecting urls and applying them
	// is safe, since we are iterate over the list again, and also apply
	// only existing hosts.
	for ( RLockedDistrIt_c it ( g_pDistIndexes ); it.Next (); )
		it.Get ()->ForEveryHost ( [&] ( AgentDesc_t &tDesc ) {
			if ( tDesc.m_bNeedResolve )
			{
				DWORD * pRenew = hHosts ( tDesc.m_sAddr );
				if ( pRenew && *pRenew )
					tDesc.m_uAddr = *pRenew;
			}
		});

	tOut.Ok ( hHosts.GetLength() );
}

void HandleMysqlFlushLogs ( SqlRowBuffer_c &tOut )
{
	sigusr1(1);
	tOut.Ok ();
}

void HandleMysqlReloadIndexes ( SqlRowBuffer_c &tOut )
{
	sighup(1);
	tOut.Ok ();
}

// The pinger
struct PingRequestBuilder_t : public IRequestBuilder_t
{
	explicit PingRequestBuilder_t ( int iCookie = 0 )
		: m_iCookie ( iCookie )
	{}
	void BuildRequest ( const AgentConn_t &, CachedOutputBuffer_c & tOut ) const final
	{
		// API header
		APICommand_t tWr { tOut, SEARCHD_COMMAND_PING, VER_COMMAND_PING };
		tOut.SendInt ( m_iCookie );
	}

protected:
	const int m_iCookie;
};

struct PingReplyParser_t : public IReplyParser_t
{
	explicit PingReplyParser_t ( int * pCookie )
		: m_pCookie ( pCookie )
	{}

	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const final
	{
		*m_pCookie += tReq.GetDword ();
		return true;
	}

protected:
	int * m_pCookie;
};


static void CheckPing ( int64_t iNow )
{
	VecRefPtrsAgentConn_t dConnections;
	VecRefPtrs_t<HostDashboard_t *> dDashes;
	g_tDashes.GetActiveDashes ( dDashes );
	for ( auto& pDash : dDashes )
	{
		assert ( pDash->m_iNeedPing>=0 );
		if ( pDash->m_iNeedPing )
		{
			CSphScopedRLock tRguard ( pDash->m_dDataLock );
			if ( pDash->IsOlder ( iNow ) )
			{
				assert ( !pDash->m_tHost.m_pDash );
				auto * pAgent = new AgentConn_t;
				pAgent->m_tDesc.CloneFromHost ( pDash->m_tHost );
				pAgent->m_tDesc.m_pDash = pDash;
				pAgent->m_iMyConnectTimeout = g_iPingInterval;
				pAgent->m_iMyQueryTimeout = g_iPingInterval;
				pDash->AddRef (); // m_tDesc above will release on destroy
				dConnections.Add ( pAgent );
			}
		}
	}

	if ( dConnections.IsEmpty() )
		return;

	int iReplyCookie = 0;


	// fixme! Let's rewrite ping proc another (async) way.
	PingRequestBuilder_t tReqBuilder ( (int)iNow );
	PingReplyParser_t tParser ( &iReplyCookie );
	PerformRemoteTasks ( dConnections, &tReqBuilder, &tParser );

	// connect to remote agents and query them
	if ( g_bShutdown )
		return;
}


static void PingThreadFunc ( void * )
{
	if ( g_iPingInterval<=0 )
		return;

	// crash logging for the thread
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

	int64_t iLastCheck = 0;

	while ( !g_bShutdown )
	{
		// check if we have work to do
		int64_t iNow = sphMicroTimer();
		if ( ( iNow-iLastCheck )<g_iPingInterval*1000LL )
		{
			sphSleepMsec ( 50 );
			continue;
		}

		ThreadSystem_t tThdSystemDesc ( "PING" );

		CheckPing ( iNow );
		iLastCheck = sphMicroTimer();
	}
}


/////////////////////////////////////////////////////////////////////////////
// user variables these send from master to agents
/////////////////////////////////////////////////////////////////////////////

struct UVarRequestBuilder_t : public IRequestBuilder_t
{
	UVarRequestBuilder_t ( const char * sName, const CSphVector<SphAttr_t> &dSetValues )
		: m_sName ( sName )
	{
		m_iUserVars = dSetValues.GetLength ();
		m_dBuf.Reset ( m_iUserVars * sizeof ( dSetValues[0] ) + 129 );
		// 129 above is the safe gap for VLB delta encoding 64-bits ints.
		// If we have 1st value 0x8000`0000`0000`0000 - it will occupy 10 bytes VLB,
		// then up to 127 values 0x0100.. - each will occupy 9 bytes VLB,
		// deltas 0x00XX.. takes 8 bytes or less. So, 2+127 bytes gap is enough to cover worst possible case
		// (since 0x80.. + 127 * 0x01.. produce 0xFF.. num, any other delta >0x01.. impossible, since
		// it will cause overflow, and deltals <0x01.. occupy <=8 bytes each).

		SphAttr_t iLast = 0;
		BYTE * pCur = m_dBuf.Begin ();
		for ( const auto &dValue : dSetValues )
		{
			pCur += sphEncodeVLB8 ( pCur, dValue - iLast );
			iLast = dValue;
		}
		m_iLength = pCur-m_dBuf.Begin();
	}

	void BuildRequest ( const AgentConn_t &, CachedOutputBuffer_c & tOut ) const final
	{
		// API header
		APICommand_t tWr { tOut, SEARCHD_COMMAND_UVAR, VER_COMMAND_UVAR };

		tOut.SendString ( m_sName.cstr() );
		tOut.SendInt ( m_iUserVars );
		tOut.SendArray ( m_dBuf, m_iLength );
	}

	CSphString m_sName;
	CSphFixedVector<BYTE> m_dBuf { 0 };
	int m_iUserVars = 0;
	int m_iLength = 0;
};

struct UVarReplyParser_t : public IReplyParser_t
{
	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const final
	{
		// error got handled at call site
		bool bOk = ( tReq.GetByte()==1 );
		return bOk;
	}
};


static void UservarAdd ( const CSphString & sName, CSphVector<SphAttr_t> & dVal ) EXCLUDES ( g_tUservarsMutex );

// create or update the variable
static void SetLocalUserVar ( const CSphString & sName, CSphVector<SphAttr_t> & dSetValues )
{
	UservarAdd ( sName, dSetValues );
	g_tmSphinxqlState = sphMicroTimer();
}

static bool SendUserVar ( const char * sIndex, const char * sUserVarName, CSphVector<SphAttr_t> & dSetValues, CSphString & sError )
{
	auto pIndex = GetDistr ( sIndex );
	if ( !pIndex )
	{
		sError.SetSprintf ( "unknown index '%s' in Set statement", sIndex );
		return false;
	}

	VecRefPtrsAgentConn_t dAgents;
	pIndex->GetAllHosts ( dAgents );
	bool bGotLocal = !pIndex->m_dLocal.IsEmpty();

	// FIXME!!! warn on missed agents
	if ( dAgents.IsEmpty() && !bGotLocal )
		return true;

	dSetValues.Uniq();

	// FIXME!!! warn on empty agents
	// connect to remote agents and query them
	if ( !dAgents.IsEmpty() )
	{
		UVarRequestBuilder_t tReqBuilder ( sUserVarName, dSetValues );
		UVarReplyParser_t tParser;
		PerformRemoteTasks ( dAgents, &tReqBuilder, &tParser );
	}

	// should be at the end due to swap of dSetValues values
	if ( bGotLocal )
		SetLocalUserVar ( sUserVarName, dSetValues );

	return true;
}


void HandleCommandUserVar ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_UVAR, tOut ) )
		return;

	CSphString sUserVar = tReq.GetString();
	int iCount = tReq.GetInt();
	CSphVector<SphAttr_t> dUserVar ( iCount );
	int iLength = tReq.GetInt();
	CSphFixedVector<BYTE> dBuf ( iLength );
	tReq.GetBytes ( dBuf.Begin(), iLength );

	if ( tReq.GetError() )
	{
		SendErrorReply ( tOut, "invalid or truncated request" );
		return;
	}

	SphAttr_t iLast = 0;
	const BYTE * pCur = dBuf.Begin();
	ARRAY_FOREACH ( i, dUserVar )
	{
		uint64_t iDelta = 0;
		pCur = spnDecodeVLB8 ( pCur, iDelta );
		assert ( iDelta>0 );
		iLast += iDelta;
		dUserVar[i] = iLast;
	}

	SetLocalUserVar ( sUserVar, dUserVar );

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_UVAR );
	tOut.SendInt ( 1 );
}



/////////////////////////////////////////////////////////////////////////////
// SMART UPDATES HANDLER
/////////////////////////////////////////////////////////////////////////////

struct SphinxqlReplyParser_t : public IReplyParser_t
{
	explicit SphinxqlReplyParser_t ( int * pUpd, int * pWarns )
		: m_pUpdated ( pUpd )
		, m_pWarns ( pWarns )
	{}

	bool ParseReply ( MemInputBuffer_c & tReq, AgentConn_t & ) const final
	{
		DWORD uSize = ( tReq.GetLSBDword() & 0x00ffffff ) - 1;
		BYTE uCommand = tReq.GetByte();

		if ( uCommand==0 ) // ok packet
		{
			*m_pUpdated += MysqlUnpack ( tReq, &uSize );
			MysqlUnpack ( tReq, &uSize ); ///< int Insert_id (don't used).
			*m_pWarns += tReq.GetLSBDword(); ///< num of warnings
			uSize -= 4;
			if ( uSize )
				tReq.GetRawString ( uSize );
			return true;
		}
		if ( uCommand==0xff ) // error packet
		{
			tReq.GetByte();
			tReq.GetByte(); ///< num of errors (2 bytes), we don't use it for now.
			uSize -= 2;
			if ( uSize )
				tReq.GetRawString ( uSize );
		}

		return false;
	}

protected:
	int * m_pUpdated;
	int * m_pWarns;
};


SphinxqlRequestBuilder_c::SphinxqlRequestBuilder_c ( const CSphString& sQuery, const SqlStmt_t & tStmt )
	: m_sBegin ( sQuery.cstr(), tStmt.m_iListStart )
	, m_sEnd ( sQuery.cstr() + tStmt.m_iListEnd, sQuery.Length() - tStmt.m_iListEnd )
{
}


void SphinxqlRequestBuilder_c::BuildRequest ( const AgentConn_t & tAgent, CachedOutputBuffer_c & tOut ) const
{
	const char* sIndexes = tAgent.m_tDesc.m_sIndexes.cstr();

	// API header
	APICommand_t tWr { tOut, SEARCHD_COMMAND_SPHINXQL, VER_COMMAND_SPHINXQL };
	tOut.StartMeasureLength (); // sphinxql wrapped twice, so one more length need to be written.
	tOut.SendBytes ( m_sBegin );
	tOut.SendBytes ( sIndexes );
	tOut.SendBytes ( m_sEnd );
	tOut.CommitMeasuredLength ();
}


class PlainParserFactory_c : public QueryParserFactory_i
{
public:
	QueryParser_i * CreateQueryParser () const override
	{
		return sphCreatePlainQueryParser();
	}

	IRequestBuilder_t * CreateRequestBuilder ( const CSphString & sQuery, const SqlStmt_t & tStmt ) const override
	{
		return new SphinxqlRequestBuilder_c ( sQuery, tStmt );
	}

	IReplyParser_t * CreateReplyParser ( int & iUpdated, int & iWarnings ) const override
	{
		return new SphinxqlReplyParser_t ( &iUpdated, &iWarnings );
	}
};

//////////////////////////////////////////////////////////////////////////

static void DoExtendedUpdate ( const char * sIndex, const QueryParserFactory_i & tQueryParserFactory, const char * sDistributed, const SqlStmt_t & tStmt, int & iSuccesses, int & iUpdated,
	SearchFailuresLog_c & dFails, const ServedDesc_t * pServed, CSphString & sWarning, const ThdDesc_t & tThd )
{
	if ( !pServed || !pServed->m_pIndex )
	{
		dFails.Submit ( sIndex, sDistributed, "index not available" );
		return;
	}

	SearchHandler_c tHandler ( 1, tQueryParserFactory.CreateQueryParser(), tStmt.m_tQuery.m_eQueryType, false, tThd );
	CSphAttrUpdateEx tUpdate;
	CSphString sError;

	tUpdate.m_pUpdate = &tStmt.m_tUpdate;
	tUpdate.m_pIndex = pServed->m_pIndex;
	tUpdate.m_pError = &sError;
	tUpdate.m_pWarning = &sWarning;

	tHandler.m_dLocked.AddUnmanaged ( sIndex, pServed );
	tHandler.RunUpdates ( tStmt.m_tQuery, sIndex, &tUpdate );

	if ( sError.Length() )
	{
		dFails.Submit ( sIndex, sDistributed, sError.cstr() );
		return;
	}

	iUpdated += tUpdate.m_iAffected;
	iSuccesses++;
}


void sphHandleMysqlUpdate ( StmtErrorReporter_i & tOut, const QueryParserFactory_i & tQueryParserFactory, const SqlStmt_t & tStmt, const CSphString & sQuery, CSphString & sWarning, const ThdDesc_t & tThd )
{
	CSphString sError;

	// extract index names
	StrVec_t dIndexNames;
	ParseIndexList ( tStmt.m_sIndex, dIndexNames );
	if ( dIndexNames.IsEmpty() )
	{
		sError.SetSprintf ( "no such index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	DistrPtrs_t dDistributed;
	// copy distributed indexes description
	CSphString sMissed;
	if ( !ExtractDistributedIndexes ( dIndexNames, dDistributed, sMissed ) )
	{
		sError.SetSprintf ( "unknown index '%s' in update request", sMissed.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	// do update
	SearchFailuresLog_c dFails;
	int iSuccesses = 0;
	int iUpdated = 0;
	int iWarns = 0;

	bool bMvaUpdate = tStmt.m_tUpdate.m_dTypes.FindFirst (
		[] ( ESphAttr iFoo ) { return iFoo==SPH_ATTR_UINT32SET || iFoo==SPH_ATTR_INT64SET; } );

	ARRAY_FOREACH ( iIdx, dIndexNames )
	{
		const char * sReqIndex = dIndexNames[iIdx].cstr();
		auto pLocked = GetServed ( sReqIndex );
		if ( pLocked )
		{
			if ( bMvaUpdate )
				DoExtendedUpdate ( sReqIndex, tQueryParserFactory, nullptr, tStmt, iSuccesses,
					iUpdated, dFails, ServedDescWPtr_c ( pLocked ), sWarning, tThd );
			else
				DoExtendedUpdate ( sReqIndex, tQueryParserFactory, nullptr, tStmt, iSuccesses,
					iUpdated, dFails, ServedDescRPtr_c ( pLocked ), sWarning, tThd );
		} else if ( dDistributed[iIdx] )
		{
			assert ( !dDistributed[iIdx]->IsEmpty() );
			const StrVec_t & dLocal = dDistributed[iIdx]->m_dLocal;

			ARRAY_FOREACH ( i, dLocal )
			{
				const char * sLocal = dLocal[i].cstr();
				auto pServed = GetServed ( sLocal );
				if ( bMvaUpdate )
					DoExtendedUpdate ( sLocal, tQueryParserFactory, sReqIndex, tStmt, iSuccesses,
						iUpdated, dFails, ServedDescWPtr_c ( pServed ), sWarning, tThd );
				else
					DoExtendedUpdate ( sLocal, tQueryParserFactory, sReqIndex, tStmt, iSuccesses,
						iUpdated, dFails, ServedDescRPtr_c ( pServed ), sWarning, tThd );
			}
		}

		// update remote agents
		if ( dDistributed[iIdx] && !dDistributed[iIdx]->m_dAgents.IsEmpty() )
		{
			const DistributedIndex_t * pDist = dDistributed[iIdx];

			VecRefPtrs_t<AgentConn_t *> dAgents;
			pDist->GetAllHosts ( dAgents );

			// connect to remote agents and query them
			CSphScopedPtr<IRequestBuilder_t> pRequestBuilder ( tQueryParserFactory.CreateRequestBuilder ( sQuery, tStmt ) ) ;
			CSphScopedPtr<IReplyParser_t> pReplyParser ( tQueryParserFactory.CreateReplyParser ( iUpdated, iWarns ) );
			iSuccesses += PerformRemoteTasks ( dAgents, pRequestBuilder.Ptr (), pReplyParser.Ptr () );
		}
	}

	StringBuilder_c sReport;
	dFails.BuildReport ( sReport );

	if ( !iSuccesses )
	{
		tOut.Error ( tStmt.m_sStmt, sReport.cstr() );
		return;
	}

	tOut.Ok ( iUpdated, iWarns );
}

bool HandleMysqlSelect ( SqlRowBuffer_c & dRows, SearchHandler_c & tHandler )
{
	// lets check all query for errors
	CSphString sError;
	CSphVector<int64_t> dAgentTimes; // dummy for error reporting
	ARRAY_FOREACH ( i, tHandler.m_dQueries )
	{
		CheckQuery ( tHandler.m_dQueries[i], tHandler.m_dResults[i].m_sError );
		if ( !tHandler.m_dResults[i].m_sError.IsEmpty() )
		{
			LogQuery ( tHandler.m_dQueries[i], tHandler.m_dResults[i], dAgentTimes, tHandler.m_tThd.m_iConnID );
			if ( sError.IsEmpty() )
			{
				if ( tHandler.m_dQueries.GetLength()==1 )
					sError = tHandler.m_dResults[0].m_sError;
				else
					sError.SetSprintf ( "query %d error: %s", i, tHandler.m_dResults[i].m_sError.cstr() );
			} else
				sError.SetSprintf ( "%s; query %d error: %s", sError.cstr(), i, tHandler.m_dResults[i].m_sError.cstr() );
		}
	}

	if ( sError.Length() )
	{
		// stmt is intentionally NULL, as we did all the reporting just above
		dRows.Error ( NULL, sError.cstr() );
		return false;
	}

	// actual searching
	tHandler.RunQueries();

	if ( g_bGotSigterm )
	{
		sphLogDebug ( "HandleClientMySQL: got SIGTERM, sending the packet MYSQL_ERR_SERVER_SHUTDOWN" );
		dRows.Error ( NULL, "Server shutdown in progress", MYSQL_ERR_SERVER_SHUTDOWN );
		return false;
	}

	return true;
}

inline static int Bit ( int iBit, const unsigned int * pData )
{
	return ( ( pData[iBit / 32] & ( 1 << ( iBit % 32 ) ) ) ? 1 : 0 );
}

void sphFormatFactors ( StringBuilder_c & sOut, const unsigned int * pFactors, bool bJson )
{
	sOut.GrowEnough ( 512 );

	// format lines for header, fields, words
	const char * sBmFmt = nullptr;
	const char * sFieldFmt = nullptr;
	const char * sWordFmt = nullptr;
	ScopedComma_c sDelim;
	if ( bJson )
	{
		sBmFmt = R"("bm25":%d, "bm25a":%f, "field_mask":%u, "doc_word_count":%d)";
		sFieldFmt = R"({"field":%d, "lcs":%u, "hit_count":%u, "word_count":%u, "tf_idf":%f, "min_idf":%f, )"
			R"("max_idf":%f, "sum_idf":%f, "min_hit_pos":%d, "min_best_span_pos":%d, "exact_hit":%u, )"
	 		R"("max_window_hits":%d, "min_gaps":%d, "exact_order":%u, "lccs":%d, "wlccs":%f, "atc":%f})";
		sWordFmt = R"(%i{"tf":%d, "idf":%f})";
		sDelim.Init ( sOut, ", ", "{", "}" );

	} else
	{
		sBmFmt = "bm25=%d, bm25a=%f, field_mask=%u, doc_word_count=%d";
		sFieldFmt = "field%d=(lcs=%u, hit_count=%u, word_count=%u, tf_idf=%f, min_idf=%f, max_idf=%f, sum_idf=%f, "
					"min_hit_pos=%d, min_best_span_pos=%d, exact_hit=%u, max_window_hits=%d, "
					"min_gaps=%d, exact_order=%u, lccs=%d, wlccs=%f, atc=%f)";
		sWordFmt = "word%d=(tf=%d, idf=%f)";
		sDelim.Init ( sOut );
	}
#define DI( _factor ) sphinx_get_doc_factor_int ( pFactors, SPH_DOCF_##_factor )
#define DF( _factor ) sphinx_get_doc_factor_float ( pFactors, SPH_DOCF_##_factor )
	sOut.Sprintf ( sBmFmt, DI( BM25 ), DF( BM25A ), DI( MATCHED_FIELDS ), DI( DOC_WORD_COUNT ) );
	{ ScopedComma_c sFields;
		if ( bJson )
			sFields.Init ( sOut, ", ", R"("fields":[)", "]");

		auto pExactHit		= sphinx_get_doc_factor_ptr ( pFactors, SPH_DOCF_EXACT_HIT_MASK );
		auto pExactOrder	= sphinx_get_doc_factor_ptr ( pFactors, SPH_DOCF_EXACT_ORDER_MASK );
		int iFields = DI ( NUM_FIELDS );
		for ( int i = 0; i<iFields; ++i )
		{
#define FI( _factor ) sphinx_get_field_factor_int ( pField, SPH_FIELDF_##_factor )
#define FF( _factor ) sphinx_get_field_factor_float ( pField, SPH_FIELDF_##_factor )
			auto pField = sphinx_get_field_factors ( pFactors, i );
			if ( !FI (HIT_COUNT) )
				continue;
			sOut.Sprintf ( sFieldFmt, i, FI (LCS), FI (HIT_COUNT), FI (WORD_COUNT), FF (TF_IDF), FF (MIN_IDF),
				FF (MAX_IDF), FF (SUM_IDF), FI (MIN_HIT_POS), FI (MIN_BEST_SPAN_POS), Bit (i, pExactHit),
				FI (MAX_WINDOW_HITS), FI (MIN_GAPS), Bit (i, pExactOrder), FI (LCCS), FF (WLCCS), FF (ATC) );
#undef FF
#undef FI
		}
	} // fields block

	{ ScopedComma_c sWords;
		if ( bJson )
			sWords.Init ( sOut, ", ", R"("words":[)", "]" );

		auto iUniqQpos = DI ( MAX_UNIQ_QPOS );
		for ( int i = 0; i<iUniqQpos; ++i )
		{
			auto pTerm = sphinx_get_term_factors ( pFactors, i + 1 );
			if ( !sphinx_get_term_factor_int ( pTerm, SPH_TERMF_KEYWORD_MASK ) )
				continue;
			sOut.Sprintf ( sWordFmt, i, sphinx_get_term_factor_int ( pTerm, SPH_TERMF_TF ),
				sphinx_get_term_factor_float ( pTerm, SPH_TERMF_IDF ) );
		}
	} // words block
#undef DF
#undef DI
}


static void ReturnZeroCount ( const CSphSchema & tSchema, int iAttrsCount, const StrVec_t & dCounts,
	SqlRowBuffer_c & dRows )
{
	for ( int i=0; i<iAttrsCount; ++i )
	{
		const CSphColumnInfo & tCol = tSchema.GetAttr ( i );

		// @count or its alias or count(distinct attr_name)
		if ( dCounts.Contains ( tCol.m_sName ) )
		{
			dRows.PutNumAsString ( 0 );
		} else
		{
			// essentially the same as SELECT_DUAL, parse and print constant expressions
			ESphAttr eAttrType;
			CSphString sError;
			ISphExprRefPtr_c pExpr { sphExprParse ( tCol.m_sName.cstr(), tSchema, &eAttrType, NULL, sError, NULL )};

			if ( !pExpr || !pExpr->IsConst() )
				eAttrType = SPH_ATTR_NONE;

			CSphMatch tMatch;
			const BYTE * pStr = nullptr;

			switch ( eAttrType )
			{
				case SPH_ATTR_STRINGPTR:
					pExpr->StringEval ( tMatch, &pStr );
					dRows.PutString ( pStr );
					SafeDelete ( pStr );
					break;
				case SPH_ATTR_INTEGER: dRows.PutNumAsString ( pExpr->IntEval ( tMatch ) ); break;
				case SPH_ATTR_BIGINT: dRows.PutNumAsString ( pExpr->Int64Eval ( tMatch ) ); break;
				case SPH_ATTR_FLOAT:	dRows.PutFloatAsString ( pExpr->Eval ( tMatch ) ); break;
				default:
					dRows.PutNULL();
					break;
			}
		}
	}
	dRows.Commit();
}

template <typename T>
static void MVA2Str ( const T * pMVA, int iLengthBytes, StringBuilder_c & dStr)
{
	dStr.GrowEnough ( (SPH_MAX_NUMERIC_STR+1)*iLengthBytes/sizeof(DWORD) );
	int nValues = iLengthBytes / sizeof(T);
	Comma_c sComma ( "," );
	for ( int i = 0; i < nValues; ++i )
	{
		dStr << sComma;
		dStr.GrowEnough ( SPH_MAX_NUMERIC_STR );
		dStr += sph::NtoA ( dStr.end (), pMVA[i] );
	}
}

void sphPackedMVA2Str ( const BYTE * pMVA, bool b64bit, StringBuilder_c & dStr )
{
	int iLengthBytes = sphUnpackPtrAttr ( pMVA, &pMVA );
	if ( b64bit )
		MVA2Str ( (const int64_t *)pMVA, iLengthBytes, dStr );
	else
		MVA2Str ( (const DWORD *)pMVA, iLengthBytes, dStr );
}

void SendMysqlSelectResult ( SqlRowBuffer_c & dRows, const AggrResult_t & tRes, bool bMoreResultsFollow, bool bAddQueryColumn, const CSphString * pQueryColumn, CSphQueryProfile * pProfile )
{
	CSphScopedProfile tProf ( pProfile, SPH_QSTATE_NET_WRITE );

	if ( !tRes.m_iSuccesses )
	{
		// at this point, SELECT error logging should have been handled, so pass a NULL stmt to logger
		dRows.Error ( nullptr, tRes.m_sError.cstr() );
		return;
	}

	// empty result sets just might carry the full uberschema
	// bummer! lets protect ourselves against that
	int iSchemaAttrsCount = 0;
	int iAttrsCount = 1;
	bool bReturnZeroCount = !tRes.m_dZeroCount.IsEmpty();
	if ( tRes.m_dMatches.GetLength() || bReturnZeroCount )
	{
		iSchemaAttrsCount = sphSendGetAttrCount ( tRes.m_tSchema );
		iAttrsCount = iSchemaAttrsCount;
	}
	if ( bAddQueryColumn )
		iAttrsCount++;

	// result set header packet. We will attach EOF manually at the end.
	dRows.HeadBegin ( iAttrsCount );

	// field packets
	if ( tRes.m_dMatches.IsEmpty() && !bReturnZeroCount )
	{
		// in case there are no matches, send a dummy schema
		dRows.HeadColumn ( "id", MYSQL_COL_LONGLONG, MYSQL_COL_UNSIGNED_FLAG );
	} else
	{
		for ( int i=0; i<iSchemaAttrsCount; ++i )
		{
			const CSphColumnInfo & tCol = tRes.m_tSchema.GetAttr(i);
			MysqlColumnType_e eType = MYSQL_COL_STRING;
			switch ( tCol.m_eAttrType )
			{
			case SPH_ATTR_INTEGER:
			case SPH_ATTR_TIMESTAMP:
			case SPH_ATTR_BOOL:
				eType = MYSQL_COL_LONG; break;
			case SPH_ATTR_FLOAT:
				eType = MYSQL_COL_FLOAT; break;
			case SPH_ATTR_BIGINT:
				eType = MYSQL_COL_LONGLONG; break;
			default:
				break;
			}
			dRows.HeadColumn ( tCol.m_sName.cstr(), eType, tCol.m_sName=="id" ? MYSQL_COL_UNSIGNED_FLAG : 0 );
		}
	}

	if ( bAddQueryColumn )
		dRows.HeadColumn ( "query", MYSQL_COL_STRING );

	// EOF packet is sent explicitly due to non-default params.
	auto iWarns = tRes.m_sWarning.IsEmpty() ? 0 : 1;
	dRows.HeadEnd ( bMoreResultsFollow, iWarns );

	// FIXME!!! replace that vector relocations by SqlRowBuffer

	// rows
	const CSphSchema &tSchema = tRes.m_tSchema;
	for ( int iMatch = tRes.m_iOffset; iMatch < tRes.m_iOffset + tRes.m_iCount; ++iMatch )
	{
		const CSphMatch & tMatch = tRes.m_dMatches [ iMatch ];
		for ( int i=0; i<iSchemaAttrsCount; ++i )
		{
			const CSphColumnInfo & dAttr = tSchema.GetAttr(i);
			CSphAttrLocator tLoc = dAttr.m_tLocator;
			ESphAttr eAttrType = dAttr.m_eAttrType;

			assert ( sphPlainAttrToPtrAttr(eAttrType)==eAttrType );

			switch ( eAttrType )
			{
			case SPH_ATTR_INTEGER:
			case SPH_ATTR_TIMESTAMP:
			case SPH_ATTR_BOOL:
			case SPH_ATTR_TOKENCOUNT:
				dRows.PutNumAsString ( ( DWORD ) tMatch.GetAttr ( tLoc ) );
				break;

			case SPH_ATTR_BIGINT:
				{
					// how to get rid of this if?
					if ( dAttr.m_sName == "id" )
						dRows.PutNumAsString ( tMatch.m_uDocID ); // uint64_t
					else
						dRows.PutNumAsString ( tMatch.GetAttr ( tLoc ) ); // int64_t
					break;
				}

			case SPH_ATTR_FLOAT:
				dRows.PutFloatAsString ( tMatch.GetAttrFloat(tLoc) );
				break;

			case SPH_ATTR_INT64SET_PTR:
			case SPH_ATTR_UINT32SET_PTR:
				{
					StringBuilder_c dStr;
					sphPackedMVA2Str ( (const BYTE *)tMatch.GetAttr(tLoc), eAttrType==SPH_ATTR_INT64SET_PTR, dStr );
					dRows.PutArray ( dStr, false );
					break;
				}

			case SPH_ATTR_STRINGPTR:
				{
					auto * pString = ( const BYTE * ) tMatch.GetAttr ( tLoc );
					int iLen=0;
					if ( pString )
						iLen = sphUnpackPtrAttr ( pString, &pString );
					if ( pString && iLen>1 && pString[iLen - 2]=='\0' )
						iLen -= 2;
					dRows.PutArray ( pString, iLen );
				}
				break;
			case SPH_ATTR_JSON_PTR:
				{
					auto * pString = (const BYTE*) tMatch.GetAttr ( tLoc );
					JsonEscapedBuilder sTmp;
					if ( pString )
					{
						sphUnpackPtrAttr ( pString, &pString );
						sphJsonFormat ( sTmp, pString );
					}
					dRows.PutArray ( sTmp );
				}
				break;

			case SPH_ATTR_FACTORS:
			case SPH_ATTR_FACTORS_JSON:
				{
					const BYTE * pFactors = nullptr;
					sphUnpackPtrAttr ( (const BYTE*)tMatch.GetAttr ( tLoc ), &pFactors );
					StringBuilder_c sTmp;
					if ( pFactors )
						sphFormatFactors ( sTmp, (const unsigned int *)pFactors, eAttrType==SPH_ATTR_FACTORS_JSON );
					dRows.PutArray ( sTmp, false );
					break;
				}

			case SPH_ATTR_JSON_FIELD_PTR:
				{
					const BYTE * pField = (const BYTE *)tMatch.GetAttr ( tLoc );
					if ( !pField )
					{
						dRows.PutNULL();
						break;
					}

					sphUnpackPtrAttr ( pField, &pField );
					auto eJson = ESphJsonType ( *pField++ );

					if ( eJson==JSON_NULL )
					{
						dRows.PutNULL();
						break;
					}
			
					// send string to client
					JsonEscapedBuilder sTmp;
					sphJsonFieldFormat ( sTmp, pField, eJson, false );
					dRows.PutArray ( sTmp, false );
					break;
				}

			default:
				dRows.Add(1);
				dRows.Add('-');
				break;
			}
		}

		if ( bAddQueryColumn )
		{
			assert ( pQueryColumn );
			dRows.PutString ( *pQueryColumn );
		}

		dRows.Commit();
	}

	if ( bReturnZeroCount )
		ReturnZeroCount ( tRes.m_tSchema, iSchemaAttrsCount, tRes.m_dZeroCount, dRows );

	// eof packet
	dRows.Eof ( bMoreResultsFollow, iWarns );
}


void HandleMysqlWarning ( const CSphQueryResultMeta & tLastMeta, SqlRowBuffer_c & dRows, bool bMoreResultsFollow )
{
	// can't send simple ok if there are more results to send
	// as it breaks order of multi-result output
	if ( tLastMeta.m_sWarning.IsEmpty() && !bMoreResultsFollow )
	{
		dRows.Ok();
		return;
	}

	// result set header packet
	dRows.HeadBegin(3);
	dRows.HeadColumn ( "Level" );
	dRows.HeadColumn ( "Code", MYSQL_COL_DECIMAL );
	dRows.HeadColumn ( "Message" );
	dRows.HeadEnd ( bMoreResultsFollow );

	// row
	dRows.PutString ( "warning" );
	dRows.PutString ( "1000" );
	dRows.PutString ( tLastMeta.m_sWarning );
	dRows.Commit();

	// cleanup
	dRows.Eof ( bMoreResultsFollow );
}

void HandleMysqlMeta ( SqlRowBuffer_c & dRows, const SqlStmt_t & tStmt, const CSphQueryResultMeta & tLastMeta, bool bMoreResultsFollow )
{
	VectorLike dStatus ( tStmt.m_sStringParam );

	switch ( tStmt.m_eStmt )
	{
	case STMT_SHOW_STATUS:
		BuildStatus ( dStatus );
		break;
	case STMT_SHOW_META:
		BuildMeta ( dStatus, tLastMeta );
		break;
	case STMT_SHOW_AGENT_STATUS:
		BuildAgentStatus ( dStatus, tStmt.m_sIndex );
		break;
	default:
		assert(0); // only 'show' statements allowed here.
		break;
	}

	// result set header packet
	dRows.HeadTuplet ( dStatus.szColKey(), dStatus.szColValue() );

	// send rows
	for ( int iRow=0; iRow<dStatus.GetLength(); iRow+=2 )
		dRows.DataTuplet ( dStatus[iRow+0].cstr(), dStatus[iRow+1].cstr() );

	// cleanup
	dRows.Eof ( bMoreResultsFollow );
}

static int PercolateDeleteDocuments ( const CSphString & sIndex, const CSphString & sCluster, const SqlStmt_t & tStmt, CSphString & sError )
{
	// prohibit double copy of filters
	const CSphQuery & tQuery = tStmt.m_tQuery;
	ReplicationCommand_t tCmd;
	tCmd.m_eCommand = RCOMMAND_DELETE;
	tCmd.m_sIndex = sIndex;
	tCmd.m_sCluster = sCluster;

	if ( tQuery.m_dFilters.GetLength()>1 )
	{
		sError.SetSprintf ( "only single filter supported, got %d", tQuery.m_dFilters.GetLength() );
		return 0;
	}

	if ( tQuery.m_dFilters.GetLength() )
	{
		const CSphFilterSettings * pFilter = tQuery.m_dFilters.Begin();
		if ( ( pFilter->m_bHasEqualMin || pFilter->m_bHasEqualMax ) && !pFilter->m_bExclude && pFilter->m_eType==SPH_FILTER_VALUES
			&& ( pFilter->m_sAttrName=="@id" || pFilter->m_sAttrName=="uid" ) )
		{
			tCmd.m_dDeleteQueries.Reserve ( pFilter->GetNumValues() );
			const SphAttr_t * pA = pFilter->GetValueArray();
			for ( int i = 0; i < pFilter->GetNumValues(); ++i )
				tCmd.m_dDeleteQueries.Add ( pA[i] );
		} else if ( pFilter->m_eType==SPH_FILTER_STRING && pFilter->m_sAttrName=="tags" && pFilter->m_dStrings.GetLength() )
		{
			tCmd.m_sDeleteTags = pFilter->m_dStrings[0].cstr();
		} else if ( pFilter->m_eType==SPH_FILTER_STRING_LIST && pFilter->m_sAttrName=="tags" && pFilter->m_dStrings.GetLength() )
		{
			StringBuilder_c tBuf;
			tBuf.StartBlock ( "," );
			for ( const CSphString & sVal : pFilter->m_dStrings )
				tBuf << sVal;
			tBuf.FinishBlock ();

			tCmd.m_sDeleteTags = tBuf.cstr();
		}
		else
		{
			sError.SetSprintf ( "unsupported filter type %d, attribute '%s'", pFilter->m_eType, pFilter->m_sAttrName.cstr() );
			return 0;
		}
	}

	int iDeleted = 0;
	HandleCmdReplicate ( tCmd, sError, &iDeleted );
	return iDeleted;
}

static int LocalIndexDoDeleteDocuments ( const CSphString & sName, const CSphString & sCluster, const QueryParserFactory_i & tQueryParserFactory, const char * sDistributed, const SqlStmt_t & tStmt,
	const SphDocID_t * pDocs, int iCount, SearchFailuresLog_c & dErrors, bool bCommit, CSphSessionAccum & tAcc, const ThdDesc_t & tThd )
{
	CSphString sError;

	// scope just for unlocked index for percolate call
	while ( true )
	{
		ServedDescRPtr_c pLocked ( GetServed ( sName ) );
		if ( !pLocked || !pLocked->m_pIndex )
		{
			dErrors.Submit ( sName, sDistributed, "index not available" );
			return 0;
		}

		auto * pIndex = static_cast<ISphRtIndex *> ( pLocked->m_pIndex );
		if ( !pLocked->IsMutable () )
		{
			sError.SetSprintf ( "does not support DELETE" );
			dErrors.Submit ( sName, sDistributed, sError.cstr() );
			return 0;
		}

		ISphRtAccum * pAccum = tAcc.GetAcc ( pIndex, sError );
		if ( !sError.IsEmpty() )
		{
			dErrors.Submit ( sName, sDistributed, sError.cstr() );
			return 0;
		}

		// goto to percolate path with unlocked index
		if ( pLocked->m_eType==IndexType_e::PERCOLATE )
		{
			if ( !CheckIndexCluster ( sName, *pLocked, sCluster, sError ) )
			{
				dErrors.Submit ( sName, sDistributed, sError.cstr() );
				return 0;
			} 

			break;
		}

		CSphScopedPtr<SearchHandler_c> pHandler ( nullptr );
		CSphVector<SphDocID_t> dValues;
		if ( !pDocs ) // needs to be deleted via query
		{
		pHandler = new SearchHandler_c ( 1, tQueryParserFactory.CreateQueryParser(), tStmt.m_tQuery.m_eQueryType, false, tThd );
			pHandler->m_dLocked.AddUnmanaged ( sName, pLocked );
			pHandler->RunDeletes ( tStmt.m_tQuery, sName, &sError, &dValues );
			pDocs = dValues.Begin();
			iCount = dValues.GetLength();
		}

		if ( !pIndex->DeleteDocument ( pDocs, iCount, sError, pAccum ) )
		{
			dErrors.Submit ( sName, sDistributed, sError.cstr() );
			return 0;
		}

		int iAffected = 0;
		if ( bCommit )
			pIndex->Commit ( &iAffected, pAccum );

		return iAffected;
	}

	// need unlocked index here
	int iAffected = PercolateDeleteDocuments ( sName, sCluster, tStmt, sError );
	if ( !sError.IsEmpty() )
		dErrors.Submit ( sName, sDistributed, sError.cstr() );

	return iAffected;

}


void sphHandleMysqlDelete ( StmtErrorReporter_i & tOut, const QueryParserFactory_i & tQueryParserFactory, const SqlStmt_t & tStmt, const CSphString & sQuery, bool bCommit, CSphSessionAccum & tAcc, const ThdDesc_t & tThd )
{
	MEMORY ( MEM_SQL_DELETE );

	CSphString sError;

	StrVec_t dNames;
	ParseIndexList ( tStmt.m_sIndex, dNames );
	if ( dNames.IsEmpty() )
	{
		sError.SetSprintf ( "no such index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	DistrPtrs_t dDistributed;
	CSphString sMissed;
	if ( !ExtractDistributedIndexes ( dNames, dDistributed, sMissed ) )
	{
		sError.SetSprintf ( "unknown index '%s' in delete request", sMissed.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	// delete to agents works only with commit=1
	if ( !bCommit  )
	{
		for ( auto &pDist : dDistributed )
		{
			if ( !pDist || pDist->m_dAgents.IsEmpty() )
				continue;

			sError.SetSprintf ( "index '%s': DELETE not working on agents when autocommit=0", tStmt.m_sIndex.cstr() );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
	}

	const SphDocID_t * pDocs = nullptr;
	int iDocsCount = 0;
	CSphVector<SphDocID_t> dDeleteIds;

	// now check the short path - if we have clauses 'id=smth' or 'id in (xx,yy)' or 'id in @uservar' - we know
	// all the values list immediatelly and don't have to run the heavy query here.
	const CSphQuery & tQuery = tStmt.m_tQuery; // shortcut

	if ( tQuery.m_sQuery.IsEmpty() && tQuery.m_dFilters.GetLength()==1 && !tQuery.m_dFilterTree.GetLength() )
	{
		const CSphFilterSettings* pFilter = tQuery.m_dFilters.Begin();
		if ( ( pFilter->m_bHasEqualMin || pFilter->m_bHasEqualMax ) && pFilter->m_eType==SPH_FILTER_VALUES
			&& pFilter->m_sAttrName=="@id" && !pFilter->m_bExclude )
		{
			if_const ( sizeof(SphAttr_t)==sizeof(SphDocID_t) ) // avoid copying in this case
			{
				pDocs = (SphDocID_t *)pFilter->GetValueArray();
				iDocsCount = pFilter->GetNumValues();
			} else
			{
				dDeleteIds.Reserve ( pFilter->GetNumValues() );
				const SphAttr_t* pA = pFilter->GetValueArray();
				for ( int i=0; i<pFilter->GetNumValues(); ++i )
					dDeleteIds.Add ( pA[i] );
				pDocs = dDeleteIds.Begin();
				iDocsCount = dDeleteIds.GetLength();
			}
		}
	}

	// do delete
	SearchFailuresLog_c dErrors;
	int iAffected = 0;

	// delete for local indexes
	ARRAY_FOREACH ( iIdx, dNames )
	{
		const CSphString & sName = dNames[iIdx];
		bool bLocal = g_pLocalIndexes->Contains ( sName );
		if ( bLocal )
		{
			iAffected += LocalIndexDoDeleteDocuments ( sName, tStmt.m_sCluster, tQueryParserFactory, nullptr,
				tStmt, pDocs, iDocsCount, dErrors, bCommit, tAcc, tThd );
		}
		else if ( dDistributed[iIdx] )
		{
			assert ( !dDistributed[iIdx]->IsEmpty() );
			for ( const CSphString& sLocal : dDistributed[iIdx]->m_dLocal )
			{
				bool bDistLocal = g_pLocalIndexes->Contains ( sLocal );
				if ( bDistLocal )
				{
					iAffected += LocalIndexDoDeleteDocuments ( sLocal, tStmt.m_sCluster, tQueryParserFactory, sName.cstr(),
						tStmt, pDocs, iDocsCount, dErrors, bCommit, tAcc, tThd );
				}
			}
		}

		// delete for remote agents
		if ( dDistributed[iIdx] && dDistributed[iIdx]->m_dAgents.GetLength() )
		{
			const DistributedIndex_t * pDist = dDistributed[iIdx];
			VecRefPtrsAgentConn_t dAgents;
			pDist->GetAllHosts ( dAgents );

			int iGot = 0;
			int iWarns = 0;

			// connect to remote agents and query them
			CSphScopedPtr<IRequestBuilder_t> pRequestBuilder ( tQueryParserFactory.CreateRequestBuilder ( sQuery, tStmt ) ) ;
			CSphScopedPtr<IReplyParser_t> pReplyParser ( tQueryParserFactory.CreateReplyParser ( iGot, iWarns ) );
			PerformRemoteTasks ( dAgents, pRequestBuilder.Ptr (), pReplyParser.Ptr () );

			// FIXME!!! report error & warnings from agents
			// FIXME? profile update time too?
			iAffected += iGot;
		}
	}

	if ( !dErrors.IsEmpty() )
	{
		StringBuilder_c sReport;
		dErrors.BuildReport ( sReport );
		tOut.Error ( tStmt.m_sStmt, sReport.cstr() );
		return;
	}

	tOut.Ok ( iAffected );
}


struct SessionVars_t
{
	bool			m_bAutoCommit = true;
	bool			m_bInTransaction = false;
	ESphCollation	m_eCollation { g_eCollation };
	bool			m_bProfile = false;
	bool			m_bVIP = false;
};

// fwd
void HandleMysqlShowProfile ( SqlRowBuffer_c & tOut, const CSphQueryProfile & p, bool bMoreResultsFollow );
static void HandleMysqlShowPlan ( SqlRowBuffer_c & tOut, const CSphQueryProfile & p, bool bMoreResultsFollow );


class CSphQueryProfileMysql : public CSphQueryProfile
{
public:
	void			BuildResult ( XQNode_t * pRoot, const CSphSchema & tSchema, const StrVec_t& dZones ) final;
	const char*	GetResultAsStr() const final;

private:
	CSphString				m_sResult;
	void					Explain ( const XQNode_t * pNode, const CSphSchema & tSchema, const StrVec_t& dZones, int iIdent );
};


void CSphQueryProfileMysql::BuildResult ( XQNode_t * pRoot, const CSphSchema & tSchema, const StrVec_t& dZones )
{
	m_sResult = sphExplainQuery ( pRoot, tSchema, dZones );
}


const char* CSphQueryProfileMysql::GetResultAsStr() const
{
	return m_sResult.cstr();
}


void HandleMysqlMultiStmt ( const CSphVector<SqlStmt_t> & dStmt, CSphQueryResultMeta & tLastMeta,
	SqlRowBuffer_c & dRows, ThdDesc_t & tThd, const CSphString& sWarning )
{
	// select count
	int iSelect = 0;
	ARRAY_FOREACH ( i, dStmt )
		if ( dStmt[i].m_eStmt==STMT_SELECT )
			iSelect++;

	CSphQueryResultMeta tPrevMeta = tLastMeta;

	tThd.m_sCommand = g_dSqlStmts[STMT_SELECT];
	for ( int i=0; i<iSelect; i++ )
		StatCountCommand ( SEARCHD_COMMAND_SEARCH );

	// setup query for searching
	SearchHandler_c tHandler ( iSelect, sphCreatePlainQueryParser(), QUERY_SQL, true, tThd );
	SessionVars_t tVars;
	CSphQueryProfileMysql tProfile;

	iSelect = 0;
	ARRAY_FOREACH ( i, dStmt )
	{
		if ( dStmt[i].m_eStmt==STMT_SELECT )
		{
			tHandler.SetQuery ( iSelect, dStmt[i].m_tQuery, dStmt[i].m_pTableFunc );
			dStmt[i].m_pTableFunc = nullptr;
			iSelect++;
		}
		else if ( dStmt[i].m_eStmt==STMT_SET && dStmt[i].m_eSet==SET_LOCAL )
		{
			CSphString sSetName ( dStmt[i].m_sSetName );
			sSetName.ToLower();
			tVars.m_bProfile = ( sSetName=="profiling" && dStmt[i].m_iSetValue!=0 );
		}
	}

	// use first meta for faceted search
	bool bUseFirstMeta = ( tHandler.m_dQueries.GetLength()>1 && !tHandler.m_dQueries[0].m_bFacet && tHandler.m_dQueries[1].m_bFacet );
	if ( tVars.m_bProfile )
		tHandler.SetProfile ( &tProfile );

	// do search
	bool bSearchOK = true;
	if ( iSelect )
	{
		bSearchOK = HandleMysqlSelect ( dRows, tHandler );

		// save meta for SHOW *
		tLastMeta = bUseFirstMeta ? tHandler.m_dResults[0] : tHandler.m_dResults.Last();

		// fix up overall query time
		if ( bUseFirstMeta )
			for ( int i=1; i<tHandler.m_dResults.GetLength(); i++ )
			{
				tLastMeta.m_iQueryTime += tHandler.m_dResults[i].m_iQueryTime;
				tLastMeta.m_iCpuTime += tHandler.m_dResults[i].m_iCpuTime;
				tLastMeta.m_iAgentCpuTime += tHandler.m_dResults[i].m_iAgentCpuTime;
			}
	}

	if ( !bSearchOK )
		return;

	// send multi-result set
	iSelect = 0;
	ARRAY_FOREACH ( i, dStmt )
	{
		SqlStmt_e eStmt = dStmt[i].m_eStmt;

		ThdState ( THD_QUERY, tThd );
		tThd.m_sCommand = g_dSqlStmts[eStmt];

		const CSphQueryResultMeta & tMeta = bUseFirstMeta ? tHandler.m_dResults[0] : ( iSelect-1>=0 ? tHandler.m_dResults[iSelect-1] : tPrevMeta );
		bool bMoreResultsFollow = (i+1)<dStmt.GetLength();

		if ( eStmt==STMT_SELECT )
		{
			AggrResult_t & tRes = tHandler.m_dResults[iSelect++];
			if ( !sWarning.IsEmpty() )
				tRes.m_sWarning = sWarning;
			SendMysqlSelectResult ( dRows, tRes, bMoreResultsFollow, false, nullptr, ( tVars.m_bProfile ? &tProfile : nullptr ) );
			// mysql server breaks send on error
			if ( !tRes.m_iSuccesses )
				break;
		} else if ( eStmt==STMT_SHOW_WARNINGS )
			HandleMysqlWarning ( tMeta, dRows, bMoreResultsFollow );
		else if ( eStmt==STMT_SHOW_STATUS || eStmt==STMT_SHOW_META || eStmt==STMT_SHOW_AGENT_STATUS )
			HandleMysqlMeta ( dRows, dStmt[i], tMeta, bMoreResultsFollow ); // FIXME!!! add prediction counters
		else if ( eStmt==STMT_SET ) // TODO implement all set statements and make them handle bMoreResultsFollow flag
			dRows.Ok ( 0, 0, NULL, bMoreResultsFollow );
		else if ( eStmt==STMT_SHOW_PROFILE )
			HandleMysqlShowProfile ( dRows, tProfile, bMoreResultsFollow );
		else if ( eStmt==STMT_SHOW_PLAN )
			HandleMysqlShowPlan ( dRows, tProfile, bMoreResultsFollow );

		if ( g_bGotSigterm )
		{
			sphLogDebug ( "HandleMultiStmt: got SIGTERM, sending the packet MYSQL_ERR_SERVER_SHUTDOWN" );
			dRows.Error ( NULL, "Server shutdown in progress", MYSQL_ERR_SERVER_SHUTDOWN );
			return;
		}
	}
}

static ESphCollation sphCollationFromName ( const CSphString & sName, CSphString * pError )
{
	assert ( pError );

	// FIXME! replace with a hash lookup?
	if ( sName=="libc_ci" )
		return SPH_COLLATION_LIBC_CI;
	else if ( sName=="libc_cs" )
		return SPH_COLLATION_LIBC_CS;
	else if ( sName=="utf8_general_ci" )
		return SPH_COLLATION_UTF8_GENERAL_CI;
	else if ( sName=="binary" )
		return SPH_COLLATION_BINARY;

	pError->SetSprintf ( "Unknown collation: '%s'", sName.cstr() );
	return SPH_COLLATION_DEFAULT;
}


static void UservarAdd ( const CSphString & sName, CSphVector<SphAttr_t> & dVal ) EXCLUDES ( g_tUservarsMutex )
{
	ScopedMutex_t tLock ( g_tUservarsMutex );
	Uservar_t * pVar = g_hUservars ( sName );
	if ( pVar )
	{
		// variable exists, release previous value
		// actual destruction of the value (aka data) might happen later
		// as the concurrent queries might still be using and holding that data
		// from here, the old value becomes nameless, though
		assert ( pVar->m_eType==USERVAR_INT_SET );
		assert ( pVar->m_pVal );
	} else
	{
		// create a shiny new variable
		Uservar_t tVar;
		g_hUservars.Add ( tVar, sName );
		pVar = g_hUservars ( sName );
	}

	// swap in the new value
	assert ( pVar );
	pVar->m_eType = USERVAR_INT_SET;
	pVar->m_pVal = new UservarIntSet_c(); // previous will be auto-released here
	pVar->m_pVal->SwapData ( dVal );
}


void HandleMysqlSet ( SqlRowBuffer_c & tOut, SqlStmt_t & tStmt, SessionVars_t & tVars, CSphSessionAccum & tAcc )
{
	MEMORY ( MEM_SQL_SET );
	CSphString sError;

	tStmt.m_sSetName.ToLower();
	switch ( tStmt.m_eSet )
	{
	case SET_LOCAL:
		if ( tStmt.m_sSetName=="autocommit" )
		{
			// per-session AUTOCOMMIT
			tVars.m_bAutoCommit = ( tStmt.m_iSetValue!=0 );
			tVars.m_bInTransaction = false;

			// commit all pending changes
			if ( tVars.m_bAutoCommit )
			{
				ISphRtIndex * pIndex = tAcc.GetIndex();
				if ( pIndex )
				{
					ISphRtAccum * pAccum = tAcc.GetAcc ( pIndex, sError );
					if ( !sError.IsEmpty() )
					{
						tOut.Error ( tStmt.m_sStmt, sError.cstr() );
						return;
					} else
					{
						pIndex->Commit ( NULL, pAccum );
					}
				}
			}
		} else if ( tStmt.m_sSetName=="collation_connection" )
		{
			// per-session COLLATION_CONNECTION
			CSphString & sVal = tStmt.m_sSetValue;
			sVal.ToLower();

			tVars.m_eCollation = sphCollationFromName ( sVal, &sError );
			if ( !sError.IsEmpty() )
			{
				tOut.Error ( tStmt.m_sStmt, sError.cstr() );
				return;
			}
		} else if ( tStmt.m_sSetName=="character_set_results"
			|| tStmt.m_sSetName=="sql_auto_is_null"
			|| tStmt.m_sSetName=="sql_safe_updates"
			|| tStmt.m_sSetName=="sql_mode" )
		{
			// per-session CHARACTER_SET_RESULTS et al; just ignore for now

		} else if ( tStmt.m_sSetName=="profiling" )
		{
			// per-session PROFILING
			tVars.m_bProfile = ( tStmt.m_iSetValue!=0 );

		} else
		{
			// unknown variable, return error
			sError.SetSprintf ( "Unknown session variable '%s' in SET statement", tStmt.m_sSetName.cstr() );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
		break;

	case SET_GLOBAL_UVAR:
	{
		// global user variable
		// INT_SET type must be sorted
		tStmt.m_dSetValues.Sort();
		SetLocalUserVar ( tStmt.m_sSetName, tStmt.m_dSetValues );
		break;
	}

	case SET_GLOBAL_SVAR:
		// global server variable
		if ( tStmt.m_sSetName=="query_log_format" )
		{
			if ( tStmt.m_sSetValue=="plain" )
				g_eLogFormat = LOG_FORMAT_PLAIN;
			else if ( tStmt.m_sSetValue=="sphinxql" )
				g_eLogFormat = LOG_FORMAT_SPHINXQL;
			else
			{
				tOut.Error ( tStmt.m_sStmt, "Unknown query_log_format value (must be plain or sphinxql)" );
				return;
			}
		} else if ( tStmt.m_sSetName=="log_level" )
		{
			if ( tStmt.m_sSetValue=="info" )
				g_eLogLevel = SPH_LOG_INFO;
			else if ( tStmt.m_sSetValue=="debug" )
				g_eLogLevel = SPH_LOG_DEBUG;
			else if ( tStmt.m_sSetValue=="debugv" )
				g_eLogLevel = SPH_LOG_VERBOSE_DEBUG;
			else if ( tStmt.m_sSetValue=="debugvv" )
				g_eLogLevel = SPH_LOG_VERY_VERBOSE_DEBUG;
			else if ( tStmt.m_sSetValue=="replication" )
				g_eLogLevel = SPH_LOG_RPL_DEBUG;
			else
			{
				tOut.Error ( tStmt.m_sStmt, "Unknown log_level value (must be one of info, debug, debugv, debugvv, replication)" );
				return;
			}
		} else if ( tStmt.m_sSetName=="query_log_min_msec" )
		{
			g_iQueryLogMinMsec = (int)tStmt.m_iSetValue;
		} else if ( tStmt.m_sSetName=="qcache_max_bytes" )
		{
			const QcacheStatus_t & s = QcacheGetStatus();
			QcacheSetup ( tStmt.m_iSetValue, s.m_iThreshMsec, s.m_iTtlSec );
		} else if ( tStmt.m_sSetName=="qcache_thresh_msec" )
		{
			const QcacheStatus_t & s = QcacheGetStatus();
			QcacheSetup ( s.m_iMaxBytes, (int)tStmt.m_iSetValue, s.m_iTtlSec );
		} else if ( tStmt.m_sSetName=="qcache_ttl_sec" )
		{
			const QcacheStatus_t & s = QcacheGetStatus();
			QcacheSetup ( s.m_iMaxBytes, s.m_iThreshMsec, (int)tStmt.m_iSetValue );
		} else if ( tStmt.m_sSetName=="log_debug_filter" )
		{
			int iLen = tStmt.m_sSetValue.Length();
			iLen = Min ( iLen, SPH_MAX_FILENAME_LEN );
			memcpy ( g_sLogFilter, tStmt.m_sSetValue.cstr(), iLen );
			g_sLogFilter[iLen] = '\0';
			g_iLogFilterLen = iLen;
		} else if ( tStmt.m_sSetName=="net_wait" )
		{
			g_tmWait = (int)tStmt.m_iSetValue;
		} else if ( tStmt.m_sSetName=="grouping_in_utc")
		{
			g_bGroupingInUtc = !!tStmt.m_iSetValue;
			setGroupingInUtc ( g_bGroupingInUtc );
		} else if ( tStmt.m_sSetName=="maintenance")
		{
			if ( tVars.m_bVIP )
				g_bMaintenance = !!tStmt.m_iSetValue;
			else
			{
				sError.SetSprintf ( "Only VIP connections can set maintenance mode" );
				tOut.Error ( tStmt.m_sStmt, sError.cstr() );
				return;
			}
		} else
		{
			sError.SetSprintf ( "Unknown system variable '%s'", tStmt.m_sSetName.cstr() );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
		break;

	case SET_INDEX_UVAR:
		if ( !SendUserVar ( tStmt.m_sIndex.cstr(), tStmt.m_sSetName.cstr(), tStmt.m_dSetValues, sError ) )
		{
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
		break;

	case SET_CLUSTER_UVAR:
	{
		if ( !ReplicateSetOption ( tStmt.m_sIndex, tStmt.m_sSetValue, sError ) )
		{
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
	}
	break;

	default:
		sError.SetSprintf ( "INTERNAL ERROR: unhandle SET mode %d", (int)tStmt.m_eSet );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	// it went ok
	tOut.Ok();
}


// fwd
bool PrereadNewIndex ( ServedDesc_t & tIdx, const CSphConfigSection & hIndex, const char * szIndexName );

void HandleMysqlAttach ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	const CSphString & sFrom = tStmt.m_sIndex;
	const CSphString & sTo = tStmt.m_sStringParam;
	bool bTruncate = ( tStmt.m_iIntParam==1 );
	CSphString sError;

	auto pServedFrom = GetServed ( sFrom );
	auto pServedTo = GetServed ( sTo );

	ServedDescWPtr_c pFrom ( pServedFrom ); // write-lock
	ServedDescWPtr_c pTo ( pServedTo ) ; // write-lock

	if ( !pFrom
		|| !pTo
		|| pFrom->m_eType!=IndexType_e::PLAIN
		|| pTo->m_eType!=IndexType_e::RT )
	{
		if ( !pFrom )
			tOut.ErrorEx ( MYSQL_ERR_PARSE_ERROR, "no such index '%s'", sFrom.cstr() );
		else if ( !pTo )
			tOut.ErrorEx ( MYSQL_ERR_PARSE_ERROR, "no such index '%s'", sTo.cstr() );
		else if ( pFrom->m_eType!=IndexType_e::PLAIN )
			tOut.Error ( tStmt.m_sStmt, "1st argument to ATTACH must be a plain index" );
		else if ( pTo->m_eType!=IndexType_e::RT )
			tOut.Error ( tStmt.m_sStmt, "2nd argument to ATTACH must be a RT index" );
		return;
	}


	auto * pRtTo = ( ISphRtIndex * ) pTo->m_pIndex;

	if ( ( bTruncate && !pRtTo->Truncate ( sError ) ) ||
		!pRtTo->AttachDiskIndex ( pFrom->m_pIndex, sError ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	// after a successfull Attach() RT index owns it
	pFrom->m_pIndex = nullptr;
	g_pLocalIndexes->Delete ( sFrom );
	tOut.Ok();
}


void HandleMysqlFlushRtindex ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	CSphString sError;
	ServedDescRPtr_c pIndex ( GetServed ( tStmt.m_sIndex ) );

	if ( !pIndex || !pIndex->IsMutable() )
	{
		tOut.Error ( tStmt.m_sStmt, "FLUSH RTINDEX requires an existing RT index" );
		return;
	}

	ISphRtIndex * pRt = (ISphRtIndex*)pIndex->m_pIndex;
	pRt->ForceRamFlush();
	tOut.Ok();
}


void HandleMysqlFlushRamchunk ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	CSphString sError;
	ServedDescRPtr_c pIndex ( GetServed ( tStmt.m_sIndex ) );

	if ( !pIndex || !pIndex->IsMutable() )
	{
		tOut.Error ( tStmt.m_sStmt, "FLUSH RAMCHUNK requires an existing RT index" );
		return;
	}

	ISphRtIndex * pRt = (ISphRtIndex*)pIndex->m_pIndex;
	pRt->ForceDiskChunk();
	tOut.Ok();
}


void HandleMysqlFlush ( SqlRowBuffer_c & tOut, const SqlStmt_t & )
{
	int iTag = CommandFlush();
	tOut.HeadBegin(1);
	tOut.HeadColumn ( "tag", MYSQL_COL_LONG );
	tOut.HeadEnd();

	// data packet, var value
	tOut.PutNumAsString ( iTag );
	tOut.Commit();

	// done
	tOut.Eof();
}

inline static CSphString strSHA1 ( const CSphString& sLine )
{
	return CalcSHA1 ( sLine.cstr(), sLine.Length() );
}

int GetLogFD ()
{
	if ( g_bLogStdout && g_iLogFile!=STDOUT_FILENO )
		return STDOUT_FILENO;
	return g_iLogFile;
}


void HandleMysqlDebug ( SqlRowBuffer_c &tOut, const SqlStmt_t &tStmt, bool bVipConn )
{
	CSphString sCommand = tStmt.m_sIndex;
	CSphString sParam = tStmt.m_sStringParam;
	sCommand.ToLower ();
	sCommand.Trim ();
	sParam.Trim ();

	if ( bVipConn && ( sCommand=="shutdown" || sCommand=="crash" ) )
	{
		if ( g_sShutdownToken.IsEmpty () )
		{

			tOut.HeadTuplet ("command","error");
			tOut.DataTuplet ("debug shutdown", "shutdown_token is empty. Provide it in searchd config section.");
		}
		else {
			if ( strSHA1(sParam) == g_sShutdownToken )
			{
				tOut.HeadTuplet ( "command", "result" );
				if ( sCommand=="shutdown" )
				{
					tOut.DataTuplet ( "debug shutdown <password>", "SUCCESS" );
					sigterm(1);
				} else if ( sCommand=="crash")
				{
					tOut.DataTuplet ( "debug crash <password>", "SUCCESS" );
					BYTE * pSegv = ( BYTE * ) ( 0 );
					*pSegv = 'a';
				}
			} else
				{
					tOut.HeadTuplet ( "command", "result" );
					tOut.DataTuplet ( "debug shutdown <password>", "FAIL" );
			}
			// perform challenge here...
		}
	} else if ( bVipConn && sCommand=="token" )
	{
		auto sSha = strSHA1(sParam);
		tOut.HeadTuplet ( "command", "result" );
		tOut.DataTuplet ( "debug token", sSha.cstr() );
	}
#if HAVE_MALLOC_STATS
	else if ( sCommand=="malloc_stats" )
	{
		tOut.HeadTuplet ( "command", "result" );
		// check where is stderr...
		int iOldErr = ::dup ( STDERR_FILENO );
		::dup2 ( GetLogFD (), STDERR_FILENO );
		malloc_stats();
		::dup2 ( iOldErr, STDERR_FILENO );
		::close ( iOldErr );
		tOut.DataTuplet ( "malloc_stats", g_sLogFile.cstr());
	}
#endif
#if HAVE_MALLOC_TRIM
	else if ( sCommand=="malloc_trim" )
	{
		tOut.HeadTuplet ( "command", "result" );
		CSphString sResult;
		sResult.SetSprintf ( "%d", malloc_trim (0));
		tOut.DataTuplet ( "malloc_trim", sResult.cstr () );
	}
#endif
#if !USE_WINDOWS
	else if ( bVipConn && sCommand=="procdump" )
	{
		tOut.HeadTuplet ( "command", "result" );
		if ( g_iParentPID<=0 )
			tOut.DataTuplet ( "procdump", "Unavailable (no watchdog)" );
		else
		{
			kill ( g_iParentPID, SIGUSR1 );
			tOut.DataTupletf ( "procdump", "Sent USR1 to wathcdog (%d)", g_iParentPID );
		}
	} else if ( bVipConn && sCommand=="setgdb" )
	{
		tOut.HeadTuplet ( "command", "result" );
		const auto& g_bHaveJemalloc = getHaveJemalloc ();
		if ( sParam=="status" )
		{
			if ( g_iParentPID>0 )
				tOut.DataTupletf ( "setgdb", "Enabled, managed by watchdog (pid=%d)", g_iParentPID );
			else if ( g_bHaveJemalloc )
				tOut.DataTupletf ( "setgdb", "Enabled, managed locally because of jemalloc", g_iParentPID );
			else if ( g_iParentPID==-1 )
				tOut.DataTuplet ( "setgdb", "Enabled locally, MAY HANG!" );
			else
				tOut.DataTuplet ( "setgdb", "Disabled" );
		} else
		{
			if ( g_iParentPID>0 )
				tOut.DataTupletf ( "setgdb", "Enabled by watchdog (pid=%d)", g_iParentPID );
			else if ( g_bHaveJemalloc )
				tOut.DataTuplet ( "setgdb", "Enabled locally because of jemalloc" );
			else if ( sParam=="on" )
			{
				g_iParentPID = -1;
				tOut.DataTuplet ( "setgdb", "Ok, enabled locally, MAY HANG!" );
			} else if ( sParam=="off" )
			{
				g_iParentPID = 0;
				tOut.DataTuplet ( "setgdb", "Ok, disabled" );
			}
		}
	}
#endif
	else if ( sCommand == "sleep" )
	{
		int64_t tmStart = sphMicroTimer();
		sphSleepMsec ( Max ( tStmt.m_iIntParam, 1 )*1000 );
		int64_t tmDelta = sphMicroTimer() - tmStart;

		tOut.HeadTuplet ( "command", "result" );
		CSphString sResult;
		sResult.SetSprintf ( "%.3f", (float)tmDelta/1000000.0f );
		tOut.DataTuplet ( "sleep", sResult.cstr () );
	} else
	{
		// no known command; provide short help.
		tOut.HeadTuplet ( "command", "meaning" );
		if ( bVipConn )
		{
			tOut.DataTuplet ( "debug shutdown <password>", "emulate TERM signal");
			tOut.DataTuplet ( "debug crash <password>", "crash daemon (make SIGSEGV action)" );
			tOut.DataTuplet ( "debug token <password>", "calculate token for password" );
#if !USE_WINDOWS
			tOut.DataTuplet ( "debug procdump", "ask watchdog to dump us" );
			tOut.DataTuplet ( "debug setgdb on|off|status", "enable or disable potentially dangerous crash dumping with gdb" );
#endif
		}
		tOut.DataTuplet ( "flush logs", "emulate USR1 signal" );
		tOut.DataTuplet ( "reload indexes", "emulate HUP signal" );
#if HAVE_MALLOC_STATS
		tOut.DataTuplet ( "malloc_stats", "perform 'malloc_stats', result in searchd.log" );
#endif
#if HAVE_MALLOC_TRIM
		tOut.DataTuplet ( "malloc_trim", "pefrorm 'malloc_trim' call" );
#endif
		tOut.DataTuplet ( "sleep Nsec", "sleep for N seconds" );
	}
	// done
	tOut.Eof ();
}

// fwd
static bool PrepareReconfigure ( const CSphString & sIndex, CSphReconfigureSettings & tSettings, CSphString & sError );

void HandleMysqlTruncate ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	bool bReconfigure = ( tStmt.m_iIntParam==1 );

	ReplicationCommand_t tCmd;
	CSphString sError;
	const CSphString & sIndex = tStmt.m_sIndex;

	if ( bReconfigure && !PrepareReconfigure ( sIndex, tCmd.m_tReconfigureSettings, sError ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	// get an exclusive lock for operation
	// but only read lock for check
	{
		ServedDescRPtr_c pIndex ( GetServed ( sIndex ) );
		if ( !pIndex || !pIndex->IsMutable () )
		{
			tOut.Error ( tStmt.m_sStmt, "TRUNCATE RTINDEX requires an existing RT index" );
			return;
		}

		if ( !CheckIndexCluster ( sIndex, *pIndex, tStmt.m_sCluster, sError ) )
		{
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
	}

	tCmd.m_eCommand = RCOMMAND_TRUNCATE;
	tCmd.m_sIndex = sIndex;
	tCmd.m_sCluster = tStmt.m_sCluster;
	tCmd.m_bReconfigure = bReconfigure;

	bool bRes = HandleCmdReplicate ( tCmd, sError, nullptr );
	if ( !bRes )
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
	else
		tOut.Ok();
}


void HandleMysqlOptimize ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	ServedDescRPtr_c pIndex ( GetServed ( tStmt.m_sIndex ) );
	if ( !pIndex || !pIndex->IsMutable() )
	{
		tOut.Error ( tStmt.m_sStmt, "OPTIMIZE INDEX requires an existing RT index" );
		return;
	}

	tOut.Ok();

	if ( tStmt.m_tQuery.m_bSync )
	{
		if ( pIndex->m_pIndex )
			static_cast<ISphRtIndex *>( pIndex->m_pIndex )->Optimize ( );
		return;
	}

	g_tOptimizeQueueMutex.Lock ();
	g_dOptimizeQueue.Add ( tStmt.m_sIndex );
	g_tOptimizeQueueMutex.Unlock();
}

void HandleMysqlSelectSysvar ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	tOut.HeadBegin ( tStmt.m_tQuery.m_dItems.GetLength() );
	ARRAY_FOREACH ( i, tStmt.m_tQuery.m_dItems )
		tOut.HeadColumn ( tStmt.m_tQuery.m_dItems[i].m_sAlias.cstr(), MYSQL_COL_LONG );
	tOut.HeadEnd();

	ARRAY_FOREACH ( i, tStmt.m_tQuery.m_dItems )
	{
		const CSphString & sVar = tStmt.m_tQuery.m_dItems[i].m_sExpr;

		// MySQL Connector/J, really expects an answer here
		if ( sVar=="@@session.auto_increment_increment" )
			tOut.PutString("1");
		else if ( sVar=="@@character_set_client" || sVar=="@@character_set_connection" )
			tOut.PutString("utf8");

		// MySQL Go connector, really expects an answer here
		else if ( sVar=="@@max_allowed_packet" )
			tOut.PutNumAsString ( g_iMaxPacketSize );
		else
			tOut.PutString("");
	}

	tOut.Commit();
	tOut.Eof();
}


void HandleMysqlSelectDual ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	CSphString sVar = tStmt.m_tQuery.m_sQuery;
	CSphSchema	tSchema;
	ESphAttr eAttrType;
	CSphString sError;

	CSphRefcountedPtr<ISphExpr> pExpr { sphExprParse ( sVar.cstr(), tSchema, &eAttrType, NULL, sError, NULL ) };

	if ( !pExpr )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	tOut.HeadBegin(1);
	tOut.HeadColumn ( sVar.cstr(), MYSQL_COL_STRING );
	tOut.HeadEnd();

	CSphMatch tMatch;
	const BYTE * pStr = nullptr;

	switch ( eAttrType )
	{
		case SPH_ATTR_STRINGPTR:
		{
			int  iLen = pExpr->StringEval ( tMatch, &pStr );
			tOut.PutArray ( pStr, iLen );
			if ( pExpr->IsDataPtrAttr() )
				SafeDeleteArray ( pStr );
			break;
		}
		case SPH_ATTR_INTEGER: tOut.PutNumAsString ( pExpr->IntEval ( tMatch ) ); break;
		case SPH_ATTR_BIGINT: tOut.PutNumAsString ( pExpr->Int64Eval ( tMatch ) ); break;
		case SPH_ATTR_FLOAT:	tOut.PutFloatAsString ( pExpr->Eval ( tMatch ) ); break;
		default:
			tOut.PutNULL();
			break;
	}

	// done
	tOut.Commit();
	tOut.Eof();
}


void HandleMysqlShowCollations ( SqlRowBuffer_c & tOut )
{
	// MySQL Connector/J really expects an answer here
	// field packets
	tOut.HeadBegin(6);
	tOut.HeadColumn ( "Collation" );
	tOut.HeadColumn ( "Charset" );
	tOut.HeadColumn ( "Id", MYSQL_COL_LONGLONG );
	tOut.HeadColumn ( "Default" );
	tOut.HeadColumn ( "Compiled" );
	tOut.HeadColumn ( "Sortlen" );
	tOut.HeadEnd();

	// data packets
	tOut.PutString ( "utf8_general_ci" );
	tOut.PutString ( "utf8" );
	tOut.PutString ( "33" );
	tOut.PutString ( "Yes" );
	tOut.PutString ( "Yes" );
	tOut.PutString ( "1" );
	tOut.Commit();

	// done
	tOut.Eof();
	return;
}

void HandleMysqlShowCharacterSet ( SqlRowBuffer_c & tOut )
{
	// MySQL Connector/J really expects an answer here
	// field packets
	tOut.HeadBegin(4);
	tOut.HeadColumn ( "Charset" );
	tOut.HeadColumn ( "Description" );
	tOut.HeadColumn ( "Default collation" );
	tOut.HeadColumn ( "Maxlen" );
	tOut.HeadEnd();

	// data packets
	tOut.PutString ( "utf8" );
	tOut.PutString ( "UTF-8 Unicode" );
	tOut.PutString ( "utf8_general_ci" );
	tOut.PutString ( "3" );
	tOut.Commit();

	// done
	tOut.Eof();
}

const char * sphCollationToName ( ESphCollation eColl )
{
	switch ( eColl )
	{
		case SPH_COLLATION_LIBC_CI:				return "libc_ci";
		case SPH_COLLATION_LIBC_CS:				return "libc_cs";
		case SPH_COLLATION_UTF8_GENERAL_CI:		return "utf8_general_ci";
		case SPH_COLLATION_BINARY:				return "binary";
		default:								return "unknown";
	}
}


static const char * LogLevelName ( ESphLogLevel eLevel )
{
	switch ( eLevel )
	{
		case SPH_LOG_FATAL:					return "fatal";
		case SPH_LOG_WARNING:				return "warning";
		case SPH_LOG_INFO:					return "info";
		case SPH_LOG_DEBUG:					return "debug";
		case SPH_LOG_VERBOSE_DEBUG:			return "debugv";
		case SPH_LOG_VERY_VERBOSE_DEBUG:	return "debugvv";
		default:							return "unknown";
	}
}


void HandleMysqlShowVariables ( SqlRowBuffer_c & dRows, const SqlStmt_t & tStmt, SessionVars_t & tVars )
{
	dRows.HeadTuplet ( "Variable_name", "Value" );

	TableLike dMatchRows ( dRows, tStmt.m_sStringParam.cstr () );
	dMatchRows.DataTuplet ("autocommit", tVars.m_bAutoCommit ? "1" : "0" );
	dMatchRows.DataTuplet ( "collation_connection", sphCollationToName ( tVars.m_eCollation ) );
	dMatchRows.DataTuplet ( "query_log_format", g_eLogFormat==LOG_FORMAT_PLAIN ? "plain" : "sphinxql" );
	dMatchRows.DataTuplet ( "log_level", LogLevelName ( g_eLogLevel ) );
	dMatchRows.DataTuplet ( "max_allowed_packet", g_iMaxPacketSize );
	dMatchRows.DataTuplet ( "character_set_client", "utf8" );
	dMatchRows.DataTuplet ( "character_set_connection", "utf8" );
	dMatchRows.DataTuplet ( "grouping_in_utc", g_bGroupingInUtc ? "1" : "0" );

	// fine
	dRows.Eof();
}


static void AddQueryStats ( IDataTupleter & tOut, const char * szPrefix, const QueryStats_t & tStats,
	void (*FormatFn)( StringBuilder_c & sBuf, uint64_t uQueries, uint64_t uStat, const char * sType ) )
{
	static const char * dStatIntervalNames[QUERY_STATS_INTERVAL_TOTAL] =
	{
		"1min",
		"5min",
		"15min",
		"total"
	};

	static const char * dStatTypeNames[QUERY_STATS_TYPE_TOTAL] =
	{
		"avg",
		"min",
		"max",
		"pct95",
		"pct99"
	};

	StringBuilder_c sBuf;
	StringBuilder_c sName;
	for ( int i = 0; i < QUERY_STATS_INTERVAL_TOTAL; ++i )
	{
		sBuf.Clear();
		sBuf.Appendf ( "{\"queries\":" UINT64_FMT, tStats.m_dStats[i].m_uTotalQueries );
		for ( int j = 0; j < QUERY_STATS_TYPE_TOTAL; ++j )
		{
			sBuf += ", ";
			FormatFn ( sBuf, tStats.m_dStats[i].m_uTotalQueries, tStats.m_dStats[i].m_dData[j], dStatTypeNames[j] );
		}
		sBuf += "}";

		sName.Clear();
		sName << szPrefix << "_" << dStatIntervalNames[i];

		tOut.DataTuplet ( sName.cstr(), sBuf.cstr() );
	}
}


static void AddQueryTimeStatsToOutput ( SqlRowBuffer_c & tOut, const char * szPrefix, const QueryStats_t & tQueryTimeStats )
{
	AddQueryStats ( tOut, szPrefix, tQueryTimeStats,
		[]( StringBuilder_c & sBuf, uint64_t uQueries, uint64_t uStat, const char * sType )
		{
			if ( uQueries )
				sBuf.Appendf ( "\"%s_sec\":%d.%03d", sType, DWORD ( uStat/1000 ), DWORD ( uStat%1000 ) );
			else
				sBuf << "\"" << sType << R"(":"-")";
		} );
}


static void AddFoundRowsStatsToOutput ( SqlRowBuffer_c & tOut, const char * szPrefix, const QueryStats_t & tRowsFoundStats )
{
	AddQueryStats ( tOut, szPrefix, tRowsFoundStats,
		[]( StringBuilder_c & sBuf, uint64_t uQueries, uint64_t uStat, const char * sType )
		{
			sBuf << "\"" << sType << "\":";
			if ( uQueries )
				sBuf.Appendf ( UINT64_FMT, uStat );
			else
				sBuf += "\"-\"";
		} );
}


static void AddIndexQueryStats ( SqlRowBuffer_c & tOut, const ServedStats_c * pStats )
{
	assert ( pStats );

	QueryStats_t tQueryTimeStats, tRowsFoundStats;
	pStats->CalculateQueryStats ( tRowsFoundStats, tQueryTimeStats );
	AddQueryTimeStatsToOutput ( tOut, "query_time", tQueryTimeStats );

#ifndef NDEBUG
	QueryStats_t tExactQueryTimeStats, tExactRowsFoundStats;
	pStats->CalculateQueryStatsExact ( tExactQueryTimeStats, tExactRowsFoundStats );
	AddQueryTimeStatsToOutput ( tOut, "exact_query_time", tQueryTimeStats );
#endif

	AddFoundRowsStatsToOutput ( tOut, "found_rows", tRowsFoundStats );
}

static void AddFederatedIndexStatus ( const CSphSourceStats & tStats, const CSphString & sName, SqlRowBuffer_c & tOut )
{
	const char * dHeader[] = { "Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length", "Data_length", "Max_data_length", "Index_length", "Data_free",
		"Auto_increment", "Create_time", "Update_time", "Check_time", "Collation", "Checksum", "Create_options", "Comment" };
	tOut.HeadOfStrings ( &dHeader[0], sizeof(dHeader)/sizeof(dHeader[0]) );

	tOut.PutString ( sName );	// Name
	tOut.PutString ( "InnoDB" );		// Engine
	tOut.PutString ( "10" );			// Version
	tOut.PutString ( "Dynamic" );		// Row_format
	tOut.PutNumAsString ( tStats.m_iTotalDocuments );	// Rows
	tOut.PutString ( "4096" );			// Avg_row_length
	tOut.PutString ( "0" );				// Data_length
	tOut.PutString ( "0" );				// Max_data_length
	tOut.PutString ( "0" );				// Index_length
	tOut.PutString ( "0" );				// Data_free
	tOut.PutString ( "5" );				// Auto_increment
	tOut.PutNULL();						// Create_time
	tOut.PutNULL();						// Update_time
	tOut.PutNULL();						// Check_time
	tOut.PutString ( "utf8" );			// Collation
	tOut.PutNULL();						// Checksum
	tOut.PutString ( "" );				// Create_options
	tOut.PutString ( "" );				// Comment

	tOut.Commit();
}

static void AddPlainIndexStatus ( SqlRowBuffer_c & tOut, const ServedIndex_c * pServed, bool bModeFederated, const CSphString & sName )
{
	assert ( pServed );
	ServedDescRPtr_c pLocked ( pServed );
	CSphIndex * pIndex = pLocked->m_pIndex;
	assert ( pIndex );

	if ( !bModeFederated )
	{
		tOut.HeadTuplet ( "Variable_name", "Value" );

		switch ( pLocked->m_eType )
		{
		case IndexType_e::PLAIN: tOut.DataTuplet ( "index_type", "disk" );
			break;
		case IndexType_e::RT: tOut.DataTuplet ( "index_type", "rt" );
			break;
		case IndexType_e::PERCOLATE: tOut.DataTuplet ( "index_type", "percolate" );
			break;
		case IndexType_e::TEMPLATE: tOut.DataTuplet ( "index_type", "template" );
			break;
		case IndexType_e::DISTR: tOut.DataTuplet ( "index_type", "distributed" );
			break;
		default:
			break;
		}
		tOut.DataTuplet ( "indexed_documents", pIndex->GetStats().m_iTotalDocuments );
		tOut.DataTuplet ( "indexed_bytes", pIndex->GetStats().m_iTotalBytes );

		const int64_t * pFieldLens = pIndex->GetFieldLens();
		if ( pFieldLens )
		{
			int64_t iTotalTokens = 0;
			for ( int i=0; i < pIndex->GetMatchSchema().GetFieldsCount(); i++ )
			{
				CSphString sKey;
				sKey.SetSprintf ( "field_tokens_%s", pIndex->GetMatchSchema().GetFieldName(i) );
				tOut.DataTuplet ( sKey.cstr(), pFieldLens[i] );
				iTotalTokens += pFieldLens[i];
			}
			tOut.DataTuplet ( "total_tokens", iTotalTokens );
		}

		CSphIndexStatus tStatus;
		pIndex->GetStatus ( &tStatus );
		tOut.DataTuplet ( "ram_bytes", tStatus.m_iRamUse );
		tOut.DataTuplet ( "disk_bytes", tStatus.m_iDiskUse );
		if ( pIndex->IsRT() )
		{
			tOut.DataTuplet ( "ram_chunk", tStatus.m_iRamChunkSize );
			tOut.DataTuplet ( "disk_chunks", tStatus.m_iNumChunks );
			tOut.DataTuplet ( "mem_limit", tStatus.m_iMemLimit );
			tOut.DataTuplet ( "ram_bytes_retired", tStatus.m_iRamRetired );
		}

		AddIndexQueryStats ( tOut, pServed );
	} else
	{
		const CSphSourceStats & tStats = pIndex->GetStats();
		AddFederatedIndexStatus ( tStats, sName, tOut );
	}

	tOut.Eof();
}


static void AddDistibutedIndexStatus ( SqlRowBuffer_c & tOut, DistributedIndex_t * pIndex, bool bFederatedUser, const CSphString & sName )
{
	assert ( pIndex );

	if ( !bFederatedUser )
	{
		tOut.HeadTuplet ( "Variable_name", "Value" );
		tOut.DataTuplet ( "index_type", "distributed" );

		AddIndexQueryStats ( tOut, pIndex );
	} else
	{
		CSphSourceStats tStats;
		tStats.m_iTotalDocuments = 1000; // TODO: check is it worth to query that number from agents
		AddFederatedIndexStatus ( tStats, sName, tOut );
	}

	tOut.Eof();
}


void HandleMysqlShowIndexStatus ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt, bool bFederatedUser )
{
	CSphString sError;
	auto pServed = GetServed ( tStmt.m_sIndex );

	if ( pServed )
	{
		AddPlainIndexStatus ( tOut, pServed, bFederatedUser, tStmt.m_sIndex );
		return;
	}

	auto pIndex = GetDistr ( tStmt.m_sIndex );

	if ( pIndex )
		AddDistibutedIndexStatus ( tOut, pIndex, bFederatedUser, tStmt.m_sIndex );
	else
		tOut.Error ( tStmt.m_sStmt, "SHOW INDEX STATUS requires an existing index" );
}


void DumpKey ( StringBuilder_c & tBuf, const char * sKey, const char * sVal, bool bCond )
{
	if ( bCond )
		tBuf << sKey << " = " << sVal << "\n";
}


void DumpKey ( StringBuilder_c & tBuf, const char * sKey, int iVal, bool bCond )
{
	if ( bCond )
		tBuf.Appendf ( "%s = %d\n", sKey, iVal );
}


void DumpIndexSettings ( StringBuilder_c & tBuf, CSphIndex * pIndex )
{
	const CSphIndexSettings & tSettings = pIndex->GetSettings();
	DumpKey ( tBuf, "docinfo",				"inline",								tSettings.m_eDocinfo==SPH_DOCINFO_INLINE );
	DumpKey ( tBuf, "min_prefix_len",		tSettings.m_iMinPrefixLen,				tSettings.m_iMinPrefixLen!=0 );
	DumpKey ( tBuf, "min_infix_len",		tSettings.m_iMinInfixLen,				tSettings.m_iMinInfixLen!=0 );
	DumpKey ( tBuf, "max_substring_len",	tSettings.m_iMaxSubstringLen,			tSettings.m_iMaxSubstringLen!=0 );
	DumpKey ( tBuf, "index_exact_words",	1,										tSettings.m_bIndexExactWords );
	DumpKey ( tBuf, "html_strip",			1,										tSettings.m_bHtmlStrip );
	DumpKey ( tBuf, "html_index_attrs",		tSettings.m_sHtmlIndexAttrs.cstr(),		!tSettings.m_sHtmlIndexAttrs.IsEmpty() );
	DumpKey ( tBuf, "html_remove_elements", tSettings.m_sHtmlRemoveElements.cstr(), !tSettings.m_sHtmlRemoveElements.IsEmpty() );
	DumpKey ( tBuf, "index_zones",			tSettings.m_sZones.cstr(),				!tSettings.m_sZones.IsEmpty() );
	DumpKey ( tBuf, "index_field_lengths",	1,										tSettings.m_bIndexFieldLens );
	DumpKey ( tBuf, "index_sp",				1,										tSettings.m_bIndexSP );
	DumpKey ( tBuf, "phrase_boundary_step",	tSettings.m_iBoundaryStep,				tSettings.m_iBoundaryStep!=0 );
	DumpKey ( tBuf, "stopword_step",		tSettings.m_iStopwordStep,				tSettings.m_iStopwordStep!=1 );
	DumpKey ( tBuf, "overshort_step",		tSettings.m_iOvershortStep,				tSettings.m_iOvershortStep!=1 );
	DumpKey ( tBuf, "bigram_index", sphBigramName ( tSettings.m_eBigramIndex ), tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE );
	DumpKey ( tBuf, "bigram_freq_words",	tSettings.m_sBigramWords.cstr(),		!tSettings.m_sBigramWords.IsEmpty() );
	DumpKey ( tBuf, "rlp_context",			tSettings.m_sRLPContext.cstr(),			!tSettings.m_sRLPContext.IsEmpty() );
	DumpKey ( tBuf, "index_token_filter",	tSettings.m_sIndexTokenFilter.cstr(),	!tSettings.m_sIndexTokenFilter.IsEmpty() );
	CSphFieldFilterSettings tFieldFilter;
	pIndex->GetFieldFilterSettings ( tFieldFilter );
	ARRAY_FOREACH ( i, tFieldFilter.m_dRegexps )
		DumpKey ( tBuf, "regexp_filter",	tFieldFilter.m_dRegexps[i].cstr(),		!tFieldFilter.m_dRegexps[i].IsEmpty() );


	if ( pIndex->GetTokenizer() )
	{
		const CSphTokenizerSettings & tTokSettings = pIndex->GetTokenizer()->GetSettings();
		DumpKey ( tBuf, "charset_type",		( tTokSettings.m_iType==TOKENIZER_UTF8 || tTokSettings.m_iType==TOKENIZER_NGRAM )
											? "utf-8"
											: "unknown tokenizer (deprecated sbcs?)", true );
		DumpKey ( tBuf, "charset_table",	tTokSettings.m_sCaseFolding.cstr(),		!tTokSettings.m_sCaseFolding.IsEmpty() );
		DumpKey ( tBuf, "min_word_len",		tTokSettings.m_iMinWordLen,				tTokSettings.m_iMinWordLen>1 );
		DumpKey ( tBuf, "ngram_len",		tTokSettings.m_iNgramLen,				tTokSettings.m_iNgramLen && !tTokSettings.m_sNgramChars.IsEmpty() );
		DumpKey ( tBuf, "ngram_chars",		tTokSettings.m_sNgramChars.cstr(),		tTokSettings.m_iNgramLen && !tTokSettings.m_sNgramChars.IsEmpty() );
		DumpKey ( tBuf, "exceptions",		tTokSettings.m_sSynonymsFile.cstr(),	!tTokSettings.m_sSynonymsFile.IsEmpty() );
		DumpKey ( tBuf, "phrase_boundary",	tTokSettings.m_sBoundary.cstr(),		!tTokSettings.m_sBoundary.IsEmpty() );
		DumpKey ( tBuf, "ignore_chars",		tTokSettings.m_sIgnoreChars.cstr(),		!tTokSettings.m_sIgnoreChars.IsEmpty() );
		DumpKey ( tBuf, "blend_chars",		tTokSettings.m_sBlendChars.cstr(),		!tTokSettings.m_sBlendChars.IsEmpty() );
		DumpKey ( tBuf, "blend_mode",		tTokSettings.m_sBlendMode.cstr(),		!tTokSettings.m_sBlendMode.IsEmpty() );
	}

	if ( pIndex->GetDictionary() )
	{
		const CSphDictSettings & tDictSettings = pIndex->GetDictionary()->GetSettings();
		StringBuilder_c tWordforms;
		for ( const auto& dWordform : tDictSettings.m_dWordforms )
			tWordforms << " " << dWordform;
		DumpKey ( tBuf, "dict",				"keywords",								tDictSettings.m_bWordDict );
		DumpKey ( tBuf, "morphology",		tDictSettings.m_sMorphology.cstr(),		!tDictSettings.m_sMorphology.IsEmpty() );
		DumpKey ( tBuf, "stopwords",		tDictSettings.m_sStopwords.cstr (),		!tDictSettings.m_sStopwords.IsEmpty() );
		DumpKey ( tBuf, "wordforms",		tWordforms.cstr()+1,					tDictSettings.m_dWordforms.GetLength()>0 );
		DumpKey ( tBuf, "min_stemming_len",	tDictSettings.m_iMinStemmingLen,		tDictSettings.m_iMinStemmingLen>1 );
		DumpKey ( tBuf, "stopwords_unstemmed", 1,									tDictSettings.m_bStopwordsUnstemmed );
	}
}


void HandleMysqlShowIndexSettings ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	CSphString sError;
	ServedDescRPtr_c pServed ( GetServed ( tStmt.m_sIndex ) );

	int iChunk = tStmt.m_iIntParam;
	CSphIndex * pIndex = pServed ? pServed->m_pIndex : nullptr;

	if ( iChunk>=0 && pServed && pIndex && pIndex->IsRT() )
		pIndex = static_cast<ISphRtIndex *>( pIndex )->GetDiskChunk ( iChunk );

	if ( !pServed || !pIndex )
	{
		tOut.Error ( tStmt.m_sStmt, "SHOW INDEX SETTINGS requires an existing index" );
		return;
	}

	tOut.HeadTuplet ( "Variable_name", "Value" );

	StringBuilder_c sBuf;
	DumpIndexSettings ( sBuf, pIndex );
	tOut.DataTuplet ( "settings", sBuf.cstr() );

	tOut.Eof();
}


void HandleMysqlShowProfile ( SqlRowBuffer_c & tOut, const CSphQueryProfile & p, bool bMoreResultsFollow )
{
	#define SPH_QUERY_STATE(_name,_desc) _desc,
	static const char * dStates [ SPH_QSTATE_TOTAL ] = { SPH_QUERY_STATES };
	#undef SPH_QUERY_STATES

	tOut.HeadBegin ( 4 );
	tOut.HeadColumn ( "Status" );
	tOut.HeadColumn ( "Duration" );
	tOut.HeadColumn ( "Switches" );
	tOut.HeadColumn ( "Percent" );
	tOut.HeadEnd ( bMoreResultsFollow );

	int64_t tmTotal = 0;
	int iCount = 0;
	for ( int i=0; i<SPH_QSTATE_TOTAL; i++ )
	{
		if ( p.m_dSwitches[i]<=0 )
			continue;
		tmTotal += p.m_tmTotal[i];
		iCount += p.m_dSwitches[i];
	}

	char sTime[32];
	for ( int i=0; i<SPH_QSTATE_TOTAL; i++ )
	{
		if ( p.m_dSwitches[i]<=0 )
			continue;
		snprintf ( sTime, sizeof(sTime), "%d.%06d", int(p.m_tmTotal[i]/1000000), int(p.m_tmTotal[i]%1000000) );
		tOut.PutString ( dStates[i] );
		tOut.PutString ( sTime );
		tOut.PutNumAsString ( p.m_dSwitches[i] );
		if ( tmTotal )
			tOut.PutFloatAsString ( 100.0f * p.m_tmTotal[i]/tmTotal, "%.2f" );
		else
			tOut.PutString ( "INF" );
		tOut.Commit();
	}
	snprintf ( sTime, sizeof(sTime), "%d.%06d", int(tmTotal/1000000), int(tmTotal%1000000) );
	tOut.PutString ( "total" );
	tOut.PutString ( sTime );
	tOut.PutNumAsString ( iCount );
	tOut.PutString ( "0" );
	tOut.Commit();
	tOut.Eof ( bMoreResultsFollow );
}


static void AddAttrToIndex ( const SqlStmt_t & tStmt, const ServedDesc_t * pServed, CSphString & sError )
{
	CSphString sAttrToAdd = tStmt.m_sAlterAttr;
	sAttrToAdd.ToLower();

	if ( pServed->m_pIndex->GetMatchSchema().GetAttr ( sAttrToAdd.cstr() ) )
	{
		sError.SetSprintf ( "'%s' attribute already in schema", sAttrToAdd.cstr() );
		return;
	}

	if ( tStmt.m_eAlterColType!=SPH_ATTR_STRING && pServed->m_pIndex->GetMatchSchema().GetFieldIndex ( sAttrToAdd.cstr () )!=-1 )
	{
		sError.SetSprintf ( "can not add attribute that shadows '%s' field", sAttrToAdd.cstr () );
		return;
	}

	pServed->m_pIndex->AddRemoveAttribute ( true, sAttrToAdd, tStmt.m_eAlterColType, sError );
}


static void RemoveAttrFromIndex ( const SqlStmt_t & tStmt, const ServedDesc_t * pServed, CSphString & sError )
{
	CSphString sAttrToRemove = tStmt.m_sAlterAttr;
	sAttrToRemove.ToLower();

	if ( !pServed->m_pIndex->GetMatchSchema().GetAttr ( sAttrToRemove.cstr() ) )
	{
		sError.SetSprintf ( "attribute '%s' does not exist", sAttrToRemove.cstr() );
		return;
	}

	if ( pServed->m_pIndex->GetMatchSchema().GetAttrsCount()==1 )
	{
		sError.SetSprintf ( "unable to remove last attribute '%s'", sAttrToRemove.cstr() );
		return;
	}

	pServed->m_pIndex->AddRemoveAttribute ( false, sAttrToRemove, SPH_ATTR_NONE, sError );
}


static void HandleMysqlAlter ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt, bool bAdd )
{
	MEMORY ( MEM_SQL_ALTER );

	SearchFailuresLog_c dErrors;
	CSphString sError;

	if ( bAdd && tStmt.m_eAlterColType==SPH_ATTR_NONE )
	{
		sError.SetSprintf ( "unsupported attribute type '%d'", tStmt.m_eAlterColType );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	StrVec_t dNames;
	ParseIndexList ( tStmt.m_sIndex, dNames );
	if ( dNames.IsEmpty() )
	{
		sError.SetSprintf ( "no such index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	for ( const auto & sName : dNames )
		if ( !g_pLocalIndexes->Contains ( sName )
			&& g_pDistIndexes->Contains ( sName ) )
		{
			sError.SetSprintf ( "ALTER is only supported on local (not distributed) indexes" );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}

	for ( const auto &sName : dNames )
	{
		auto pLocal = GetServed ( sName );
		if ( !pLocal )
		{
			dErrors.Submit ( sName, nullptr, "unknown local index in ALTER request" );
			continue;
		}

		ServedDescWPtr_c dWriteLocked ( pLocal ); // write-lock
		if ( dWriteLocked->m_pIndex->GetSettings().m_eDocinfo==SPH_DOCINFO_INLINE )
		{
			dErrors.Submit ( sName, nullptr, "docinfo is inline: ALTER disabled" );
			continue;
		}

		CSphString sAddError;

		if ( bAdd )
			AddAttrToIndex ( tStmt, dWriteLocked, sAddError );
		else
			RemoveAttrFromIndex ( tStmt, dWriteLocked, sAddError );

		if ( !sAddError.IsEmpty() )
			dErrors.Submit ( sName, nullptr, sAddError.cstr() );
	}

	if ( !dErrors.IsEmpty() )
	{
		StringBuilder_c sReport;
		dErrors.BuildReport ( sReport );
		tOut.Error ( tStmt.m_sStmt, sReport.cstr() );
		return;
	}
	tOut.Ok();
}


bool PrepareReconfigure ( const CSphString & sIndex, CSphReconfigureSettings & tSettings, CSphString & sError )
{
	CSphConfigParser tCfg;
	if ( !tCfg.ReParse ( g_sConfigFile.cstr () ) )
	{
		sError.SetSprintf ( "failed to parse config file '%s'; using previous settings", g_sConfigFile.cstr () );
		return false;
	}

	if ( !tCfg.m_tConf.Exists ( "index" ) )
	{
		sError.SetSprintf ( "failed to find any index at config file '%s'; using previous settings", g_sConfigFile.cstr () );
		return false;
	}

	const CSphConfig & hConf = tCfg.m_tConf;
	if ( !hConf["index"].Exists ( sIndex ) )
	{
		sError.SetSprintf ( "failed to find index '%s' at config file '%s'; using previous settings", sIndex.cstr(), g_sConfigFile.cstr () );
		return false;
	}

	const CSphConfigSection & hIndex = hConf["index"][sIndex];
	sphConfTokenizer ( hIndex, tSettings.m_tTokenizer );
	sphConfDictionary ( hIndex, tSettings.m_tDict );
	sphConfFieldFilter ( hIndex, tSettings.m_tFieldFilter, sError );

	if ( !sphConfIndex ( hIndex, tSettings.m_tIndex, sError ) )
	{
		sError.SetSprintf ( "'%s' failed to parse index settings, error '%s'", sIndex.cstr(), sError.cstr() );
		return false;
	}

	sphRTSchemaConfigure ( hIndex, &tSettings.m_tSchema, &sError, true );
	
	return true;
}


static void HandleMysqlReconfigure ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	MEMORY ( MEM_SQL_ALTER );

	const CSphString & sIndex = tStmt.m_sIndex.cstr();
	CSphString sError;
	CSphReconfigureSettings tSettings;
	CSphReconfigureSetup tSetup;

	if ( !PrepareReconfigure ( sIndex, tSettings, sError ) )
	{
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	auto pServed = GetServed ( tStmt.m_sIndex );
	if ( !pServed )
	{
		sError.SetSprintf ( "ALTER is only supported on local (not distributed) indexes" );
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	ServedDescWPtr_c dWLocked ( pServed );
	if ( !dWLocked->IsMutable() )
	{
		sError.SetSprintf ( "'%s' does not support ALTER (enabled, not mutable)", sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr () );
		return;
	}

	bool bSame = ( ( const ISphRtIndex * ) dWLocked->m_pIndex )->IsSameSettings ( tSettings, tSetup, sError );
	if ( !bSame && sError.IsEmpty() )
		( (ISphRtIndex *) dWLocked->m_pIndex )->Reconfigure ( tSetup );

	if ( sError.IsEmpty() )
		tOut.Ok();
	else
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
}


static void HandleMysqlShowPlan ( SqlRowBuffer_c & tOut, const CSphQueryProfile & p, bool bMoreResultsFollow )
{
	tOut.HeadBegin ( 2 );
	tOut.HeadColumn ( "Variable" );
	tOut.HeadColumn ( "Value" );
	tOut.HeadEnd ( bMoreResultsFollow );

	tOut.PutString ( "transformed_tree" );
	tOut.PutString ( p.GetResultAsStr() );
	tOut.Commit();

	tOut.Eof ( bMoreResultsFollow );
}

static bool RotateIndexMT ( const CSphString & sIndex, CSphString & sError );
static bool RotateIndexGreedy ( const ServedIndex_c * pIndex, ServedDesc_t & tServed, const char * sIndex, CSphString & sError );
static void HandleMysqlReloadIndex ( SqlRowBuffer_c & tOut, const SqlStmt_t & tStmt )
{
	CSphString sError;
	auto pIndex = GetServed ( tStmt.m_sIndex );
	if ( !pIndex )
	{
		sError.SetSprintf ( "unknown local index '%s'", tStmt.m_sIndex.cstr() );
		tOut.Error ( tStmt.m_sStmt, sError.cstr() );
		return;
	}

	{
		ServedDescRPtr_c pServed ( pIndex );
		if ( pServed->IsMutable () )
		{
			sError.SetSprintf ( "can not reload RT or percolate index" );
			tOut.Error ( tStmt.m_sStmt, sError.cstr () );
			return;
		}

		if ( !tStmt.m_sStringParam.IsEmpty () )
		{
			// try move files from arbitrary path to current index path before rotate, if needed.
			// fixme! what about concurrency? if 2 sessions simultaneously ask to rotate,
			// or if we have unapplied rotates from indexer - seems that it will garbage .new files?
			IndexFiles_c sIndexFiles ( pServed->m_sIndexPath );
			if ( !sIndexFiles.RelocateToNew ( tStmt.m_sStringParam.cstr () ) )
			{
				tOut.Error ( tStmt.m_sStmt, sIndexFiles.ErrorMsg () );
				return;
			}
		}
	}

	if ( g_bSeamlessRotate )
	{
		if ( !RotateIndexMT ( tStmt.m_sIndex, sError ) )
		{
			sphWarning ( "%s", sError.cstr() );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			return;
		}
	} else
	{
		ServedDescWPtr_c pServedWL ( pIndex );
		if ( !RotateIndexGreedy ( pIndex, *pServedWL, tStmt.m_sIndex.cstr(), sError ) )
		{
			sphWarning ( "%s", sError.cstr() );
			tOut.Error ( tStmt.m_sStmt, sError.cstr() );
			g_pLocalIndexes->Delete ( tStmt.m_sIndex ); // since it unusable - no sense just to disable it.
			// fixme! RotateIndexGreedy does prealloc. Do we need to perform/signal preload also?
			return;
		}
	}

	tOut.Ok();
}

//////////////////////////////////////////////////////////////////////////

CSphSessionAccum::CSphSessionAccum ( bool bManage )
	: m_bManage ( bManage )
{ }

CSphSessionAccum::~CSphSessionAccum()
{
	SafeDelete ( m_pAcc );
}

ISphRtAccum * CSphSessionAccum::GetAcc ( ISphRtIndex * pIndex, CSphString & sError )
{
	if ( !m_bManage )
		return nullptr;

	assert ( pIndex );
	if ( m_pAcc )
		return m_pAcc;

	m_pAcc = pIndex->CreateAccum ( sError );
	return m_pAcc;
}

ISphRtIndex * CSphSessionAccum::GetIndex ()
{
	if ( !m_bManage )
		return sphGetCurrentIndexRT();

	return ( m_pAcc ? m_pAcc->GetIndex() : NULL );
}

static bool FixupFederatedQuery ( ESphCollation eCollation, CSphVector<SqlStmt_t> & dStmt, CSphString & sError, CSphString & sFederatedQuery );

class CSphinxqlSession : public ISphNoncopyable
{
private:
	CSphString			m_sError;
	CSphQueryResultMeta m_tLastMeta;
	CSphSessionAccum	m_tAcc;
	CPqResult			m_tPercolateMeta;
	SqlStmt_e			m_eLastStmt { STMT_DUMMY };
	bool				m_bFederatedUser = false;
	CSphString			m_sFederatedQuery;

public:
	SessionVars_t			m_tVars;
	CSphQueryProfileMysql	m_tProfile;
	CSphQueryProfileMysql	m_tLastProfile;

public:
	explicit CSphinxqlSession ( bool bManage )
		: m_tAcc ( bManage )
	{}

	// just execute one sphinxql statement
	//
	// IMPORTANT! this does NOT start or stop profiling, as there a few external
	// things (client net reads and writes) that we want to profile, too
	//
	// returns true if the current profile should be kept (default)
	// returns false if profile should be discarded (eg. SHOW PROFILE case)
	bool Execute ( const CSphString & sQuery, ISphOutputBuffer & tOutput, BYTE & uPacketID, ThdDesc_t & tThd )
	{
		// set on query guard
		CrashQuery_t tCrashQuery;
		tCrashQuery.m_pQuery = (const BYTE *)sQuery.cstr();
		tCrashQuery.m_iSize = sQuery.Length();
		tCrashQuery.m_bMySQL = true;
		SphCrashLogger_c::SetLastQuery ( tCrashQuery );

		// parse SQL query
		if ( m_tVars.m_bProfile )
			m_tProfile.Switch ( SPH_QSTATE_SQL_PARSE );

		CSphVector<SqlStmt_t> dStmt;
		bool bParsedOK = sphParseSqlQuery ( sQuery.cstr(), tCrashQuery.m_iSize, dStmt, m_sError, m_tVars.m_eCollation );

		if ( m_tVars.m_bProfile )
			m_tProfile.Switch ( SPH_QSTATE_UNKNOWN );

		SqlStmt_e eStmt = STMT_PARSE_ERROR;
		if ( bParsedOK )
		{
			eStmt = dStmt[0].m_eStmt;
			dStmt[0].m_sStmt = sQuery.cstr();
		}
		const SqlStmt_e ePrevStmt = m_eLastStmt;
		if ( eStmt!=STMT_SHOW_META )
			m_eLastStmt = eStmt;

		SqlStmt_t * pStmt = dStmt.Begin();
		assert ( !bParsedOK || pStmt );

		tThd.m_sCommand = g_dSqlStmts[eStmt];
		ThdState ( THD_QUERY, tThd );

		SqlRowBuffer_c tOut ( &uPacketID, &tOutput, tThd.m_iConnID, m_tVars.m_bAutoCommit );

		if ( bParsedOK && m_bFederatedUser )
		{
			if ( !FixupFederatedQuery ( m_tVars.m_eCollation, dStmt,  m_sError, m_sFederatedQuery ) )
			{
				m_tLastMeta = CSphQueryResultMeta();
				m_tLastMeta.m_sError = m_sError;
				m_tLastMeta.m_sWarning = "";
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
				return true;
			}
		}

		// handle multi SQL query
		if ( bParsedOK && dStmt.GetLength()>1 )
		{
			m_sError = "";
			HandleMysqlMultiStmt ( dStmt, m_tLastMeta, tOut, tThd, m_sError );
			return true; // FIXME? how does this work with profiling?
		}

		// handle SQL query
		switch ( eStmt )
		{
		case STMT_PARSE_ERROR:
			m_tLastMeta = CSphQueryResultMeta();
			m_tLastMeta.m_sError = m_sError;
			m_tLastMeta.m_sWarning = "";
			tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			return true;

		case STMT_SELECT:
			{
				MEMORY ( MEM_SQL_SELECT );

				StatCountCommand ( SEARCHD_COMMAND_SEARCH );
				SearchHandler_c tHandler ( 1, sphCreatePlainQueryParser(), QUERY_SQL, true, tThd );
				tHandler.SetQuery ( 0, dStmt.Begin()->m_tQuery, dStmt.Begin()->m_pTableFunc );
				dStmt.Begin()->m_pTableFunc = nullptr;

				if ( m_tVars.m_bProfile )
					tHandler.SetProfile ( &m_tProfile );
				if ( m_bFederatedUser )
					tHandler.SetFederatedUser();

				if ( HandleMysqlSelect ( tOut, tHandler ) )
				{
					// query just completed ok; reset out error message
					m_sError = "";
					AggrResult_t & tLast = tHandler.m_dResults.Last();
					SendMysqlSelectResult ( tOut, tLast, false, m_bFederatedUser, &m_sFederatedQuery, ( m_tVars.m_bProfile ? &m_tProfile : nullptr ) );
				}

				// save meta for SHOW META (profile is saved elsewhere)
				m_tLastMeta = tHandler.m_dResults.Last();
				return true;
			}
		case STMT_SHOW_WARNINGS:
			HandleMysqlWarning ( m_tLastMeta, tOut, false );
			return true;

		case STMT_SHOW_STATUS:
		case STMT_SHOW_META:
		case STMT_SHOW_AGENT_STATUS:
			if ( eStmt==STMT_SHOW_STATUS )
			{
				StatCountCommand ( SEARCHD_COMMAND_STATUS );
			}
			if ( ePrevStmt!=STMT_CALL )
				HandleMysqlMeta ( tOut, *pStmt, m_tLastMeta, false );
			else
				HandleMysqlPercolateMeta ( m_tPercolateMeta, m_tLastMeta.m_sWarning, tOut );
			return true;

		case STMT_INSERT:
		case STMT_REPLACE:
			{
				m_tLastMeta = CSphQueryResultMeta();
				m_tLastMeta.m_sError = m_sError;
				m_tLastMeta.m_sWarning = "";
				StatCountCommand ( eStmt==STMT_INSERT ? SEARCHD_COMMAND_INSERT : SEARCHD_COMMAND_REPLACE );
				StmtErrorReporter_c tErrorReporter ( tOut );
				sphHandleMysqlInsert ( tErrorReporter, *pStmt, eStmt==STMT_REPLACE,
					m_tVars.m_bAutoCommit && !m_tVars.m_bInTransaction, m_tLastMeta.m_sWarning, m_tAcc, m_tVars.m_eCollation );
				return true;
			}

		case STMT_DELETE:
			{
				m_tLastMeta = CSphQueryResultMeta();
				m_tLastMeta.m_sError = m_sError;
				m_tLastMeta.m_sWarning = "";
				StatCountCommand ( SEARCHD_COMMAND_DELETE );
				StmtErrorReporter_c tErrorReporter ( tOut );
				PlainParserFactory_c tParserFactory;
				sphHandleMysqlDelete ( tErrorReporter, tParserFactory, *pStmt, sQuery, m_tVars.m_bAutoCommit && !m_tVars.m_bInTransaction, m_tAcc, tThd );
				return true;
			}

		case STMT_SET:
			StatCountCommand ( SEARCHD_COMMAND_UVAR );
			HandleMysqlSet ( tOut, *pStmt, m_tVars, m_tAcc );
			return false;

		case STMT_BEGIN:
			{
				MEMORY ( MEM_SQL_BEGIN );

				m_tVars.m_bInTransaction = true;
				ISphRtIndex * pIndex = m_tAcc.GetIndex();
				if ( pIndex )
				{
					ISphRtAccum * pAccum = m_tAcc.GetAcc ( pIndex, m_sError );
					if ( !m_sError.IsEmpty() )
					{
						tOut.Error ( sQuery.cstr(), m_sError.cstr() );
						return true;
					}
					pIndex->Commit ( NULL, pAccum );
				}
				tOut.Ok();
				return true;
			}
		case STMT_COMMIT:
		case STMT_ROLLBACK:
			{
				MEMORY ( MEM_SQL_COMMIT );

				m_tVars.m_bInTransaction = false;
				ISphRtIndex * pIndex = m_tAcc.GetIndex();
				if ( pIndex )
				{
					ISphRtAccum * pAccum = m_tAcc.GetAcc ( pIndex, m_sError );
					if ( !m_sError.IsEmpty() )
					{
						tOut.Error ( sQuery.cstr(), m_sError.cstr() );
						return true;
					}
					if ( eStmt==STMT_COMMIT )
					{
						StatCountCommand ( SEARCHD_COMMAND_COMMIT );
						pIndex->Commit ( NULL, pAccum );
					} else
					{
						pIndex->RollBack ( pAccum );
					}
				}
				tOut.Ok();
				return true;
			}
		case STMT_CALL:
			// IMPORTANT! if you add a new builtin here, do also add it
			// in the comment to STMT_CALL line in SqlStmt_e declaration,
			// the one that lists expansions for doc/check.pl
			pStmt->m_sCallProc.ToUpper();
			if ( pStmt->m_sCallProc=="SNIPPETS" )
			{
				StatCountCommand ( SEARCHD_COMMAND_EXCERPT );
				HandleMysqlCallSnippets ( tOut, *pStmt, tThd );
			} else if ( pStmt->m_sCallProc=="KEYWORDS" )
			{
				StatCountCommand ( SEARCHD_COMMAND_KEYWORDS );
				HandleMysqlCallKeywords ( tOut, *pStmt, m_tLastMeta.m_sWarning );
			} else if ( pStmt->m_sCallProc=="SUGGEST" )
			{
				StatCountCommand ( SEARCHD_COMMAND_SUGGEST );
				HandleMysqlCallSuggest ( tOut, *pStmt, false );
			} else if ( pStmt->m_sCallProc=="QSUGGEST" )
			{
				StatCountCommand ( SEARCHD_COMMAND_SUGGEST );
				HandleMysqlCallSuggest ( tOut, *pStmt, true );
			} else if ( pStmt->m_sCallProc=="PQ" )
			{
				StatCountCommand ( SEARCHD_COMMAND_CALLPQ );
				HandleMysqlCallPQ ( tOut, *pStmt, m_tAcc, m_tPercolateMeta );
				m_tPercolateMeta.m_dResult.m_sMessages.MoveWarningsTo ( m_tLastMeta.m_sWarning );
				m_tPercolateMeta.m_dDocids.Reset ( 0 ); // free occupied mem
			} else
			{
				m_sError.SetSprintf ( "no such builtin procedure %s", pStmt->m_sCallProc.cstr() );
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			}
			return true;

		case STMT_DESCRIBE:
			HandleMysqlDescribe ( tOut, *pStmt );
			return true;

		case STMT_SHOW_TABLES:
			HandleMysqlShowTables ( tOut, *pStmt );
			return true;

		case STMT_UPDATE:
			{
				m_tLastMeta = CSphQueryResultMeta();
				m_tLastMeta.m_sError = m_sError;
				m_tLastMeta.m_sWarning = "";
				StatCountCommand ( SEARCHD_COMMAND_UPDATE );
				StmtErrorReporter_c tErrorReporter ( tOut );
				PlainParserFactory_c tParserFactory;
				sphHandleMysqlUpdate ( tErrorReporter, tParserFactory, *pStmt, sQuery, m_tLastMeta.m_sWarning, tThd );
				return true;
			}

		case STMT_DUMMY:
			tOut.Ok();
			return true;

		case STMT_CREATE_FUNCTION:
			if ( !sphPluginCreate ( pStmt->m_sUdfLib.cstr(), PLUGIN_FUNCTION, pStmt->m_sUdfName.cstr(), pStmt->m_eUdfType, m_sError ) )
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			else
				tOut.Ok();
			g_tmSphinxqlState = sphMicroTimer();
			return true;

		case STMT_DROP_FUNCTION:
			if ( !sphPluginDrop ( PLUGIN_FUNCTION, pStmt->m_sUdfName.cstr(), m_sError ) )
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			else
				tOut.Ok();
			g_tmSphinxqlState = sphMicroTimer();
			return true;

		case STMT_CREATE_PLUGIN:
		case STMT_DROP_PLUGIN:
			{
				// convert plugin type string to enum
				PluginType_e eType = sphPluginGetType ( pStmt->m_sStringParam );
				if ( eType==PLUGIN_TOTAL )
				{
					tOut.Error ( "unknown plugin type '%s'", pStmt->m_sStringParam.cstr() );
					break;
				}

				// action!
				bool bRes;
				if ( eStmt==STMT_CREATE_PLUGIN )
					bRes = sphPluginCreate ( pStmt->m_sUdfLib.cstr(), eType, pStmt->m_sUdfName.cstr(), SPH_ATTR_NONE, m_sError );
				else
					bRes = sphPluginDrop ( eType, pStmt->m_sUdfName.cstr(), m_sError );

				// report
				if ( !bRes )
					tOut.Error ( sQuery.cstr(), m_sError.cstr() );
				else
					tOut.Ok();
				g_tmSphinxqlState = sphMicroTimer();
				return true;
			}

		case STMT_RELOAD_PLUGINS:
			if ( sphPluginReload ( pStmt->m_sUdfLib.cstr(), m_sError ) )
				tOut.Ok();
			else
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			return true;

		case STMT_ATTACH_INDEX:
			HandleMysqlAttach ( tOut, *pStmt );
			return true;

		case STMT_FLUSH_RTINDEX:
			HandleMysqlFlushRtindex ( tOut, *pStmt );
			return true;

		case STMT_FLUSH_RAMCHUNK:
			HandleMysqlFlushRamchunk ( tOut, *pStmt );
			return true;

		case STMT_SHOW_VARIABLES:
			HandleMysqlShowVariables ( tOut, *pStmt, m_tVars );
			return true;

		case STMT_TRUNCATE_RTINDEX:
			HandleMysqlTruncate ( tOut, *pStmt );
			return true;

		case STMT_OPTIMIZE_INDEX:
			HandleMysqlOptimize ( tOut, *pStmt );
			return true;

		case STMT_SELECT_SYSVAR:
			HandleMysqlSelectSysvar ( tOut, *pStmt );
			return true;

		case STMT_SHOW_COLLATION:
			HandleMysqlShowCollations ( tOut );
			return true;

		case STMT_SHOW_CHARACTER_SET:
			HandleMysqlShowCharacterSet ( tOut );
			return true;

		case STMT_SHOW_INDEX_STATUS:
			HandleMysqlShowIndexStatus ( tOut, *pStmt, m_bFederatedUser );
			return true;

		case STMT_SHOW_INDEX_SETTINGS:
			HandleMysqlShowIndexSettings ( tOut, *pStmt );
			return true;

		case STMT_SHOW_PROFILE:
			HandleMysqlShowProfile ( tOut, m_tLastProfile, false );
			return false; // do not profile this call, keep last query profile

		case STMT_ALTER_ADD:
			HandleMysqlAlter ( tOut, *pStmt, true );
			return true;

		case STMT_ALTER_DROP:
			HandleMysqlAlter ( tOut, *pStmt, false );
			return true;

		case STMT_SHOW_PLAN:
			HandleMysqlShowPlan ( tOut, m_tLastProfile, false );
			return false; // do not profile this call, keep last query profile

		case STMT_SELECT_DUAL:
			HandleMysqlSelectDual ( tOut, *pStmt );
			return true;

		case STMT_SHOW_DATABASES:
			HandleMysqlShowDatabases ( tOut, *pStmt );
			return true;

		case STMT_SHOW_PLUGINS:
			HandleMysqlShowPlugins ( tOut, *pStmt );
			return true;

		case STMT_SHOW_THREADS:
			HandleMysqlShowThreads ( tOut, *pStmt );
			return true;

		case STMT_ALTER_RECONFIGURE:
			HandleMysqlReconfigure ( tOut, *pStmt );
			return true;

		case STMT_FLUSH_INDEX:
			HandleMysqlFlush ( tOut, *pStmt );
			return true;

		case STMT_RELOAD_INDEX:
			HandleMysqlReloadIndex ( tOut, *pStmt );
			return true;

		case STMT_FLUSH_HOSTNAMES:
			HandleMysqlFlushHostnames ( tOut );
			return true;

		case STMT_FLUSH_LOGS: HandleMysqlFlushLogs ( tOut );
			return true;

		case STMT_RELOAD_INDEXES: HandleMysqlReloadIndexes ( tOut );
			return true;

		case STMT_DEBUG:
			HandleMysqlDebug ( tOut, *pStmt, m_tVars.m_bVIP );
			return true;

		case STMT_JOIN_CLUSTER:
			if ( ClusterJoin ( pStmt->m_sIndex, pStmt->m_dCallOptNames, pStmt->m_dCallOptValues, m_sError ) )
				tOut.Ok();
			else
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			return true;
		case STMT_CLUSTER_CREATE:
			if ( ClusterCreate ( pStmt->m_sIndex, pStmt->m_dCallOptNames, pStmt->m_dCallOptValues, m_sError ) )
				tOut.Ok();
			else
				tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			return true;

		case STMT_CLUSTER_DELETE:
			m_tLastMeta = CSphQueryResultMeta();
			if ( ClusterDelete ( pStmt->m_sIndex, m_tLastMeta.m_sError, m_tLastMeta.m_sWarning ) )
			{
				tOut.Ok ( 0, m_tLastMeta.m_sWarning.IsEmpty() ? 0 : 1 );
			} else
			{
				tOut.Error ( sQuery.cstr(), m_tLastMeta.m_sError.cstr() );
			}
			return true;

		case STMT_CLUSTER_ALTER_ADD:
		case STMT_CLUSTER_ALTER_DROP:
			m_tLastMeta = CSphQueryResultMeta();
			if ( ClusterAlter ( pStmt->m_sCluster, pStmt->m_sIndex, ( eStmt==STMT_CLUSTER_ALTER_ADD ), m_tLastMeta.m_sError, m_tLastMeta.m_sWarning ) )
			{
				tOut.Ok ( 0, m_tLastMeta.m_sWarning.IsEmpty() ? 0 : 1 );
			} else
			{
				tOut.Error ( sQuery.cstr(), m_tLastMeta.m_sError.cstr() );
			}
			return true;

		default:
			m_sError.SetSprintf ( "internal error: unhandled statement type (value=%d)", eStmt );
			tOut.Error ( sQuery.cstr(), m_sError.cstr() );
			return true;
		} // switch
		return true; // for cases that break early
	}

	void SetFederatedUser ()
	{
		m_bFederatedUser = true;
	}
};


/// sphinxql command over API
void HandleCommandSphinxql ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq, ThdDesc_t & tThd ) REQUIRES (HandlerThread)
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_SPHINXQL, tOut ) )
		return;

	// parse request
	CSphString sCommand = tReq.GetString ();

	BYTE uDummy = 0;

	// todo: move upper, if the session variables are also necessary in API access mode.
	CSphinxqlSession tSession ( true ); // FIXME!!! check that no accum related command used via API

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_SPHINXQL );
	tSession.Execute ( sCommand, tOut, uDummy, tThd );
}

/// json command over API
void HandleCommandJson ( CachedOutputBuffer_c & tOut, WORD uVer, InputBuffer_c & tReq, ThdDesc_t & tThd )
{
	if ( !CheckCommandVersion ( uVer, VER_COMMAND_JSON, tOut ) )
		return;

	// parse request
	CSphString sEndpoint = tReq.GetString ();
	CSphString sCommand = tReq.GetString ();
	
	ESphHttpEndpoint eEndpoint = sphStrToHttpEndpoint ( sEndpoint );
	assert ( eEndpoint!=SPH_HTTP_ENDPOINT_TOTAL );

	CSphVector<BYTE> dResult;
	SmallStringHash_T<CSphString> tOptions;
	sphProcessHttpQueryNoResponce ( eEndpoint, sCommand.cstr(), tOptions, tThd, dResult );

	APICommand_t dOk ( tOut, SEARCHD_OK, VER_COMMAND_JSON );
	tOut.SendString ( sEndpoint.cstr() );
	tOut.SendArray ( dResult );
}


void StatCountCommand ( SearchdCommand_e eCmd )
{
	if ( eCmd<SEARCHD_COMMAND_TOTAL )
		++g_tStats.m_iCommandCount[eCmd];
}

bool IsFederatedUser ( const BYTE * pPacket, int iLen )
{
	// handshake packet structure
	// 4              capability flags, CLIENT_PROTOCOL_41 always set
	// 4              max-packet size
	// 1              character set
	// string[23]     reserved (all [0])
	// string[NUL]    username

	if ( !pPacket || iLen<(4+4+1+23+1) )
		return false;

	const char * sFederated = "FEDERATED";
	const char * sSrc = (const char *)pPacket + 32;

	return ( strncmp ( sFederated, sSrc, iLen-32 )==0 );
}

bool FixupFederatedQuery ( ESphCollation eCollation, CSphVector<SqlStmt_t> & dStmt, CSphString & sError, CSphString & sFederatedQuery )
{
	if ( !dStmt.GetLength() )
		return true;

	if ( dStmt.GetLength()>1 )
	{
		sError.SetSprintf ( "multi query not supported" );
		return false;
	}


	SqlStmt_t & tStmt = dStmt[0];
	if ( tStmt.m_eStmt!=STMT_SELECT && tStmt.m_eStmt!=STMT_SHOW_INDEX_STATUS )
	{
		sError.SetSprintf ( "unhandled statement type (value=%d)", tStmt.m_eStmt );
		return false;
	}
	if ( tStmt.m_eStmt==STMT_SHOW_INDEX_STATUS )
		return true;

	CSphQuery & tSrcQuery = tStmt.m_tQuery;

	// remove query column as it got generated
	ARRAY_FOREACH ( i, tSrcQuery.m_dItems )
	{
		if ( tSrcQuery.m_dItems[i].m_sAlias=="query" )
		{
			tSrcQuery.m_dItems.Remove ( i );
			break;
		}
	}

	// move actual query from filter to query itself
	if ( tSrcQuery.m_dFilters.GetLength()!=1 ||
		tSrcQuery.m_dFilters[0].m_sAttrName!="query" || tSrcQuery.m_dFilters[0].m_eType!=SPH_FILTER_STRING || tSrcQuery.m_dFilters[0].m_dStrings.GetLength()!=1 )
		return true;

	const CSphString & sRealQuery = tSrcQuery.m_dFilters[0].m_dStrings[0];

	// parse real query
	CSphVector<SqlStmt_t> dRealStmt;
	bool bParsedOK = sphParseSqlQuery ( sRealQuery.cstr(), sRealQuery.Length(), dRealStmt, sError, eCollation );
	if ( !bParsedOK )
		return false;

	if ( dRealStmt.GetLength()!=1 )
	{
		sError.SetSprintf ( "multi query not supported, got queries=%d", dRealStmt.GetLength() );
		return false;
	}

	SqlStmt_t & tRealStmt = dRealStmt[0];
	if ( tRealStmt.m_eStmt!=STMT_SELECT )
	{
		sError.SetSprintf ( "unhandled statement type (value=%d)", tRealStmt.m_eStmt );
		return false;
	}

	// keep originals
	CSphQuery & tRealQuery = tRealStmt.m_tQuery;
	tRealQuery.m_dRefItems = tSrcQuery.m_dItems; //select list items
	tRealQuery.m_sIndexes = tSrcQuery.m_sIndexes; // index name
	sFederatedQuery = sRealQuery;

	// merge select list items
	SmallStringHash_T<int> hItems;
	ARRAY_FOREACH ( i, tRealQuery.m_dItems )
		hItems.Add ( i, tRealQuery.m_dItems[i].m_sAlias );
	ARRAY_FOREACH ( i, tSrcQuery.m_dItems )
	{
		const CSphQueryItem & tItem = tSrcQuery.m_dItems[i];
		if ( !hItems.Exists ( tItem.m_sAlias ) )
			tRealQuery.m_dItems.Add ( tItem );
	}

	// query setup
	tSrcQuery = tRealQuery;
	return true;
}

static bool LoopClientMySQL ( BYTE & uPacketID, CSphinxqlSession & tSession, CSphString & sQuery,
	int iPacketLen, bool bProfile, ThdDesc_t & tThd, InputBuffer_c & tIn, ISphOutputBuffer & tOut );

static bool ReadMySQLPacketHeader ( int iSock, int& iLen, BYTE& uPacketID )
{
	const int MAX_PACKET_LEN = 0xffffffL; // 16777215 bytes, max low level packet size
	NetInputBuffer_c tIn ( iSock );
	if ( !tIn.ReadFrom ( 4, g_iClientQlTimeout, true ) )
		return false;
	DWORD uAddon = tIn.GetLSBDword ();
	uPacketID = 1 + ( BYTE ) ( uAddon >> 24 );
	iLen = ( uAddon & MAX_PACKET_LEN );
	return true;
}

static void HandleClientMySQL ( int iSock, ThdDesc_t & tThd ) REQUIRES ( HandlerThread )
{
	MEMORY ( MEM_SQL_HANDLE );
	ThdState ( THD_HANDSHAKE, tThd );

	// set off query guard
	CrashQuery_t tCrashQuery;
	tCrashQuery.m_bMySQL = true;
	SphCrashLogger_c::SetLastQuery ( tCrashQuery );

	int iCID = tThd.m_iConnID;
	const char * sClientIP = tThd.m_sClientName.cstr();

	if ( sphSockSend ( iSock, g_sMysqlHandshake, g_iMysqlHandshake )!=g_iMysqlHandshake )
	{
		int iErrno = sphSockGetErrno ();
		sphWarning ( "failed to send server version (client=%s(%d), error: %d '%s')", sClientIP, iCID, iErrno, sphSockError ( iErrno ) );
		return;
	}

	CSphString sQuery; // to keep data alive for SphCrashQuery_c
	CSphinxqlSession tSession ( false ); // session variables and state
	tSession.m_tVars.m_bVIP = tThd.m_bVip;
	bool bAuthed = false;
	BYTE uPacketID = 1;
	int iPacketLen = 0;

	const int MAX_PACKET_LEN = 0xffffffL; // 16777215 bytes, max low level packet size
	while (true)
	{
		NetOutputBuffer_c tOut ( iSock ); // OPTIMIZE? looks like buffer size matters a lot..
		NetInputBuffer_c tIn ( iSock );

		// get next packet
		// we want interruptible calls here, so that shutdowns could be honored
		ThdState ( THD_NET_IDLE, tThd );
		if ( !ReadMySQLPacketHeader ( iSock, iPacketLen, uPacketID ) )
		{
			sphLogDebugv ( "conn %s(%d): bailing on failed MySQL header (sockerr=%s)", sClientIP, iCID, sphSockError() );
			break;
		}

		// setup per-query profiling
		bool bProfile = tSession.m_tVars.m_bProfile; // the current statement might change it
		if ( bProfile )
		{
			tSession.m_tProfile.Start ( SPH_QSTATE_NET_READ );
			tOut.SetProfiler ( &tSession.m_tProfile );
		}

		// keep getting that packet
		ThdState ( THD_NET_READ, tThd );
		if ( !tIn.ReadFrom ( iPacketLen, g_iClientQlTimeout, true ) )
		{
			sphWarning ( "failed to receive MySQL request body (client=%s(%d), exp=%d, error='%s')", sClientIP, iCID, iPacketLen, sphSockError() );
			break;
		}

		if ( bProfile )
			tSession.m_tProfile.Switch ( SPH_QSTATE_UNKNOWN );

		// handle big packets
		if ( iPacketLen==MAX_PACKET_LEN )
		{
			int iAddonLen = -1;
			do
			{
				if ( !ReadMySQLPacketHeader ( iSock, iAddonLen, uPacketID ) )
				{
					sphLogDebugv ( "conn %s(%d): bailing on failed MySQL header2 (sockerr=%s)", sClientIP, iCID, sphSockError() );
					break;
				}

				if ( !tIn.ReadFrom ( iAddonLen, g_iClientQlTimeout, true, true ) )
				{
					sphWarning ( "failed to receive MySQL request body2 (client=%s(%d), exp=%d, error='%s')", sClientIP, iCID, iAddonLen, sphSockError() );
					iAddonLen = -1;
					break;
				}
				iPacketLen += iAddonLen;
			} while ( iAddonLen==MAX_PACKET_LEN );
			if ( iAddonLen<0 )
				break;
			if ( iPacketLen<0 || iPacketLen>g_iMaxPacketSize )
			{
				sphWarning ( "ill-formed client request (length=%d out of bounds)", iPacketLen );
				break;
			}
		}

		// handle auth packet
		if ( !bAuthed )
		{
			ThdState ( THD_NET_WRITE, tThd );
			bAuthed = true;
			if ( IsFederatedUser ( tIn.GetBufferPtr(), tIn.GetLength() ) )
				tSession.SetFederatedUser();
			SendMysqlOkPacket ( tOut, uPacketID, tSession.m_tVars.m_bAutoCommit );
			tOut.Flush();
			if ( tOut.GetError() )
				break;
			else
				continue;
		}

		bool bRun = LoopClientMySQL ( uPacketID, tSession, sQuery, iPacketLen, bProfile, tThd, tIn, tOut );

		if ( !bRun )
			break;
	}

	// set off query guard
	SphCrashLogger_c::SetLastQuery ( CrashQuery_t() );
}

bool LoopClientMySQL ( BYTE & uPacketID, CSphinxqlSession & tSession, CSphString & sQuery, int iPacketLen,
	bool bProfile, ThdDesc_t & tThd, InputBuffer_c & tIn, ISphOutputBuffer & tOut )
{
	// get command, handle special packets
	const BYTE uMysqlCmd = tIn.GetByte ();
	if ( uMysqlCmd==MYSQL_COM_QUIT )
		return false;

	CSphString sError;
	bool bKeepProfile = true;
	switch ( uMysqlCmd )
	{
		case MYSQL_COM_PING:
		case MYSQL_COM_INIT_DB:
			// client wants a pong
			SendMysqlOkPacket ( tOut, uPacketID, tSession.m_tVars.m_bAutoCommit );
			break;

		case MYSQL_COM_SET_OPTION:
			// bMulti = ( tIn.GetWord()==MYSQL_OPTION_MULTI_STATEMENTS_ON ); // that's how we could double check and validate multi query
			// server reporting success in response to COM_SET_OPTION and COM_DEBUG
			SendMysqlEofPacket ( tOut, uPacketID, 0, false, tSession.m_tVars.m_bAutoCommit );
			break;

		case MYSQL_COM_QUERY:
			// handle query packet
			assert ( uMysqlCmd==MYSQL_COM_QUERY );
			sQuery = tIn.GetRawString ( iPacketLen-1 ); // OPTIMIZE? could be huge; avoid copying?
			assert ( !tIn.GetError() );
			ThdState ( THD_QUERY, tThd );
			tThd.SetThreadInfo ( "%s", sQuery.cstr() ); // OPTIMIZE? could be huge; avoid copying?
			bKeepProfile = tSession.Execute ( sQuery, tOut, uPacketID, tThd );
			break;

		default:
			// default case, unknown command
			sError.SetSprintf ( "unknown command (code=%d)", uMysqlCmd );
			SendMysqlErrorPacket ( tOut, uPacketID, NULL, sError.cstr(), tThd.m_iConnID, MYSQL_ERR_UNKNOWN_COM_ERROR );
			break;
	}

	// send the response packet
	ThdState ( THD_NET_WRITE, tThd );
	tOut.Flush();
	if ( tOut.GetError() )
		return false;

	// finalize query profile
	if ( bProfile )
		tSession.m_tProfile.Stop();
	if ( uMysqlCmd==MYSQL_COM_QUERY && bKeepProfile )
		tSession.m_tLastProfile = tSession.m_tProfile;
	tOut.SetProfiler ( NULL );
	return true;
}

//////////////////////////////////////////////////////////////////////////
// HANDLE-BY-LISTENER
//////////////////////////////////////////////////////////////////////////

void HandleClient ( ProtocolType_e eProto, int iSock, ThdDesc_t & tThd ) REQUIRES (HandlerThread)
{
	switch ( eProto )
	{
		case PROTO_SPHINX:		HandleClientSphinx ( iSock, tThd ); break;
		case PROTO_MYSQL41:		HandleClientMySQL ( iSock, tThd ); break;
		default:				assert ( 0 && "unhandled protocol type" ); break;
	}
}

/////////////////////////////////////////////////////////////////////////////
// INDEX ROTATION
/////////////////////////////////////////////////////////////////////////////
inline bool HasFiles ( const CSphString & sPath, const char * sType = "", DWORD uVersion = INDEX_FORMAT_VERSION )
{
	return IndexFiles_c ( sPath, uVersion ).HasAllFiles ( sType );
}

enum class ROTATE_FROM
{
	NONE,
	NEW,
	REENABLE,
	PATH_NEW,
	PATH_COPY
};

ROTATE_FROM CheckIndexHeaderRotate ( const ServedDesc_t & dServed )
{
	// check order:
	// current_path/idx.new.sph		- rotation of current index
	// current_path/idx.sph			- enable back current index
	// new_path/idx.new.sph			- rotation of current index but with new path via indexer --rotate
	// new_path/idx.sph				- rotation of current index but with new path via files copy

	if ( IndexFiles_c ( dServed.m_sIndexPath ).ReadVersion (".new") )
		return ROTATE_FROM::NEW;

	if ( dServed.m_bOnlyNew && IndexFiles_c ( dServed.m_sIndexPath ).ReadVersion() )
		return ROTATE_FROM::REENABLE;

	if ( dServed.m_sNewPath.IsEmpty () || dServed.m_sNewPath==dServed.m_sIndexPath )
		return ROTATE_FROM::NONE;

	if ( IndexFiles_c ( dServed.m_sNewPath ).ReadVersion ( ".new" ) )
		return ROTATE_FROM::PATH_NEW;

	if ( IndexFiles_c ( dServed.m_sNewPath ).ReadVersion () )
		return ROTATE_FROM::PATH_COPY;

	return ROTATE_FROM::NONE;
}

/// returns true if any version of the index (old or new one) has been preread
bool RotateIndexGreedy ( const ServedIndex_c * pIndex, ServedDesc_t &tWlockedIndex, const char * sIndex, CSphString & sError ) REQUIRES (pIndex->rwlock ())
{
	sphLogDebug ( "RotateIndexGreedy for '%s' invoked", sIndex );
	IndexFiles_c dFiles ( tWlockedIndex.m_sIndexPath );
	dFiles.SetName ( sIndex );
	ROTATE_FROM eRot = CheckIndexHeaderRotate ( tWlockedIndex );
	bool bReEnable = ( eRot==ROTATE_FROM::REENABLE );
	if ( eRot==ROTATE_FROM::PATH_NEW || eRot==ROTATE_FROM::PATH_COPY )
	{
		sError.SetSprintf ( "rotating index '%s': can not rotate from new path, switch to seamless_rotate=1; using old index", sIndex );
		return false;
	}

	const char * sFromSuffix = bReEnable ? "" : ".new";
	if ( !dFiles.ReadVersion ( sFromSuffix ) )
	{
		// no files or wrong files - no rotation
		sError = dFiles.ErrorMsg();
		return false;
	}

	if ( !dFiles.HasAllFiles ( sFromSuffix ) )
	{
		sphWarning ( "rotating index '%s': unreadable: %s; %s", sIndex, strerrorm ( errno ),
			tWlockedIndex.m_bOnlyNew ? "NOT SERVING" : "using old index" );
		return false;
	}
	sphLogDebug ( bReEnable ? "RotateIndexGreedy: re-enabling index" : "RotateIndexGreedy: new index is readable" );
	if ( !tWlockedIndex.m_bOnlyNew )
	{
		if ( !dFiles.RenameSuffix ( "", ".old" ) )
		{
			if ( dFiles.IsFatal () )
				sphFatal ( "%s", dFiles.FatalMsg ( "rotating" ).cstr () );
			sError.SetSprintf ( "rotating index '%s': %s; using old index", sIndex, dFiles.ErrorMsg() );
			return false;
		}
		sphLogDebug ( "RotateIndexGreedy: Current index renamed to .old" );
	}

	// rename new to current
	if ( !bReEnable )
	{
		if ( !dFiles.RenameSuffix ( ".new" ) )
		{
			if ( dFiles.IsFatal () )
				sphFatal ( "%s", dFiles.FatalMsg ( "rotating" ).cstr () );

			// rollback old ones
			if ( !tWlockedIndex.m_bOnlyNew )
			{
				if ( !dFiles.RollbackSuff ( ".old" ) )
					sphFatal ( "%s", dFiles.FatalMsg ( "rotating" ).cstr () );
				return false;
			}

			sError.SetSprintf ( "rotating index '%s': %s; using old index", sIndex, dFiles.ErrorMsg () );
			return false;
		}
		sphLogDebug ( "RotateIndexGreedy: New renamed to current" );
	}

	// try to use new index
	ISphTokenizerRefPtr_c	pTokenizer { tWlockedIndex.m_pIndex->LeakTokenizer () }; // FIXME! disable support of that old indexes and remove this bullshit
	CSphDictRefPtr_c		pDictionary { tWlockedIndex.m_pIndex->LeakDictionary () };

//	bool bRolledBack = false;
	bool bPreallocSuccess = tWlockedIndex.m_pIndex->Prealloc ( g_bStripPath );
	if ( !bPreallocSuccess )
	{
		if ( tWlockedIndex.m_bOnlyNew )
		{
			sError.SetSprintf ( "rotating index '%s': .new preload failed: %s; NOT SERVING", sIndex, tWlockedIndex.m_pIndex->GetLastError().cstr() );
			return false;
		}
		sphWarning ( "rotating index '%s': .new preload failed: %s", sIndex, tWlockedIndex.m_pIndex->GetLastError().cstr() );
		// try to recover: rollback cur to .new, .old to cur.
		if ( !dFiles.RollbackSuff ( "", ".new" ) )
			sphFatal ( "%s", dFiles.FatalMsg ( "rotating" ).cstr () );
		if ( !dFiles.RollbackSuff ( ".old" ) )
			sphFatal ( "%s", dFiles.FatalMsg ( "rotating" ).cstr () );

		sphLogDebug ( "RotateIndexGreedy: has recovered. Prelloc it." );
		bPreallocSuccess = tWlockedIndex.m_pIndex->Prealloc ( g_bStripPath );
		if ( !bPreallocSuccess )
			sError.SetSprintf ( "rotating index '%s': .new preload failed; ROLLBACK FAILED; INDEX UNUSABLE", sIndex );
//		bRolledBack = true;
	}

	if ( !tWlockedIndex.m_pIndex->GetLastWarning().IsEmpty() )
		sphWarning ( "rotating index '%s': %s", sIndex, tWlockedIndex.m_pIndex->GetLastWarning().cstr() );

	if ( !tWlockedIndex.m_pIndex->GetTokenizer () )
		tWlockedIndex.m_pIndex->SetTokenizer ( pTokenizer );

	if ( !tWlockedIndex.m_pIndex->GetDictionary () )
		tWlockedIndex.m_pIndex->SetDictionary ( pDictionary );

//	if ( bRolledBack )
//		return bPreallocSuccess;

	// unlink .old
	if ( !tWlockedIndex.m_bOnlyNew && sphGetUnlinkOld ())
	{
		dFiles.Unlink (".old");
		sphLogDebug ( "RotateIndexGreedy: the old index unlinked" );
	}

	// uff. all done
	tWlockedIndex.m_bOnlyNew = false;
	sphInfo ( "rotating index '%s': success", sIndex );
	return bPreallocSuccess;
}

void DumpMemStat ()
{
#if SPH_ALLOCS_PROFILER
	sphMemStatDump ( g_iLogFile );
#endif
}

/// check and report if there were any leaks since last call
void CheckLeaks () REQUIRES ( MainThread )
{
#if SPH_DEBUG_LEAKS
	static int iHeadAllocs = sphAllocsCount ();
	static int iHeadCheckpoint = sphAllocsLastID ();

	if ( g_dThd.GetLength()==0 && !g_bInRotate && iHeadAllocs!=sphAllocsCount() )
	{
		sphSeek ( g_iLogFile, 0, SEEK_END );
		sphAllocsDump ( g_iLogFile, iHeadCheckpoint );

		iHeadAllocs = sphAllocsCount ();
		iHeadCheckpoint = sphAllocsLastID ();
	}
#endif

#if SPH_ALLOCS_PROFILER
	int iAllocLogPeriod = 60 * 1000000;
	static int64_t tmLastLog = -iAllocLogPeriod*10;

	const int iAllocCount = sphAllocsCount();
	const float fMemTotal = (float)sphAllocBytes();

	if ( iAllocLogPeriod>0 && tmLastLog+iAllocLogPeriod<sphMicroTimer() )
	{
		tmLastLog = sphMicroTimer ();
		const int iThdsCount = g_dThd.GetLength ();
		const float fMB = 1024.0f*1024.0f;
		sphInfo ( "--- allocs-count=%d, mem-total=%.4f Mb, active-threads=%d", iAllocCount, fMemTotal/fMB, iThdsCount );
		DumpMemStat ();
	}
#endif
}

// RT index that got flushed for long time ago floats to start
struct RtFlushAge_fn
{
	bool IsLess ( const CSphNamedInt & a, const CSphNamedInt & b ) const
	{
		return ( b.m_iValue<a.m_iValue );
	}
};
#define SPH_RT_AUTO_FLUSH_CHECK_PERIOD ( 5000000 )

static void RtFlushThreadFunc ( void * )
{
	int64_t tmNextCheck = sphMicroTimer() + SPH_RT_AUTO_FLUSH_CHECK_PERIOD;
	while ( !g_bShutdown )
	{
		// stand still till save time
		if ( tmNextCheck>sphMicroTimer() )
		{
			sphSleepMsec ( 50 );
			continue;
		}

		ThreadSystem_t tThdSystemDesc ( "FLUSH RT" );

		int64_t tmNow = sphMicroTimer();
		// collecting available rt indexes at save time
		CSphVector<CSphNamedInt> dRtIndexes;
		for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next (); )
		{
			ServedDescRPtr_c pIdx ( it.Get () );
			if ( !pIdx || !pIdx->IsMutable() )
				continue;

			dRtIndexes.Add().m_sName = it.GetName ();
			ISphRtIndex * pRT = (ISphRtIndex *) pIdx->m_pIndex;
			int64_t tmFlushed = pRT->GetFlushAge();
			if ( tmFlushed==0 )
				dRtIndexes.Last().m_iValue = 0;
			else
				dRtIndexes.Last().m_iValue = int( tmNow - tmFlushed>INT_MAX ? INT_MAX : tmNow - tmFlushed );
		}

		sphSort ( dRtIndexes.Begin(), dRtIndexes.GetLength(), RtFlushAge_fn() );

		// do check+save
		ARRAY_FOREACH_COND ( i, dRtIndexes, !g_bShutdown )
		{
			auto pServed = GetServed ( dRtIndexes[i].m_sName );
			if ( !pServed )
				continue;

			ServedDescRPtr_c dReadLock ( pServed );
			auto * pRT = (ISphRtIndex *) dReadLock->m_pIndex;
			pRT->CheckRamFlush();
		}

		tmNextCheck = sphMicroTimer() + SPH_RT_AUTO_FLUSH_CHECK_PERIOD;
	}
}

static void RtBinlogAutoflushThreadFunc ( void * )
{
	assert ( g_tBinlogAutoflush.m_pLog && g_tBinlogAutoflush.m_fnWork );

	while ( !g_bShutdown )
	{
		ThreadSystem_t tThdSystemDesc ( "FLUSH RT BINLOG" );
		g_tBinlogAutoflush.m_fnWork ( g_tBinlogAutoflush.m_pLog );
		sphSleepMsec ( 50 );
	}
}


/// this gets called for every new physical index
/// that is, local and RT indexes, but not distributed once
bool PreallocNewIndex ( ServedDesc_t &tIdx, const CSphConfigSection * pConfig, const char * szIndexName )
{
	bool bOk = tIdx.m_pIndex->Prealloc ( g_bStripPath );
	if ( !bOk )
	{
		sphWarning ( "index '%s': prealloc: %s; NOT SERVING", szIndexName, tIdx.m_pIndex->GetLastError ().cstr () );
		return false;
	}

	// tricky bit
	// fixup was initially intended for (very old) index formats that did not store dict/tokenizer settings
	// however currently it also ends up configuring dict/tokenizer for fresh RT indexes!
	// (and for existing RT indexes, settings get loaded during the Prealloc() call)
	CSphString sError;
	if ( pConfig && !sphFixupIndexSettings ( tIdx.m_pIndex, *pConfig, sError, g_bStripPath ) )
	{
		sphWarning ( "index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return false;
	}

	// try to lock it
	if ( !g_bOptNoLock && !tIdx.m_pIndex->Lock () )
	{
		sphWarning ( "index '%s': lock: %s; NOT SERVING", szIndexName, tIdx.m_pIndex->GetLastError ().cstr () );
		return false;
	}

	CSphIndexStatus tStatus;
	tIdx.m_pIndex->GetStatus ( &tStatus );
	tIdx.m_iMass = CalculateMass ( tStatus );
	return true;
}

// same as above, but self-load config section for given index
bool PreallocNewIndex ( ServedDesc_t &tIdx, const char * szIndexName ) EXCLUDES ( g_tRotateConfigMutex )
{
	const CSphConfigSection * pIndexConfig = nullptr;
	CSphConfigSection tIndexConfig;
	{
		ScRL_t dRLockConfig { g_tRotateConfigMutex };
		if ( g_pCfg.m_tConf ( "index" ) )
			pIndexConfig = g_pCfg.m_tConf["index"] ( szIndexName );
		if ( pIndexConfig )
		{
			tIndexConfig = *pIndexConfig;
			pIndexConfig = &tIndexConfig;
		}
	}
	return PreallocNewIndex ( tIdx, pIndexConfig, szIndexName );
}

static CSphMutex g_tRotateThreadMutex;
// called either from MysqlReloadIndex, either from RotationThreadFunc (never from main thread).
static bool RotateIndexMT ( const CSphString & sIndex, CSphString & sError )
{
	// only one rotation and reload thread allowed to prevent deadlocks
	ScopedMutex_t tBlockRotations ( g_tRotateThreadMutex );

	// get existing index. Look first to disabled hash.
	auto pRotating = GetDisabled ( sIndex );
	if ( !pRotating )
	{
		pRotating = GetServed ( sIndex );
		if ( !pRotating )
		{
			sError.SetSprintf ( "rotating index '%s': INTERNAL ERROR, index went AWOL", sIndex.cstr() );
			return false;
		}
	}

	//////////////////
	// load new index
	//////////////////
	sphInfo ( "rotating index '%s': started", sIndex.cstr() );

	// create new index, copy some settings from existing one
	ServedDesc_t tNewIndex;
	tNewIndex.m_pIndex = sphCreateIndexPhrase ( sIndex.cstr (), nullptr );
	tNewIndex.m_pIndex->m_iExpansionLimit = g_iExpansionLimit;

	IndexFiles_c dActivePath, dNewPath;
	ROTATE_FROM eRot = ROTATE_FROM::NONE;
	{
		ServedDescRPtr_c pCurrentlyServed ( pRotating );
		if ( !pCurrentlyServed->m_pIndex )
		{
			sError.SetSprintf ( "rotating index '%s': INTERNAL ERROR, entry does not have an index", sIndex.cstr() );
			return false;
		}
		// keep settings from current index description
		tNewIndex.m_bMlock = pCurrentlyServed->m_bMlock;
		tNewIndex.m_iExpandKeywords = pCurrentlyServed->m_iExpandKeywords;
		tNewIndex.m_bPreopen = pCurrentlyServed->m_bPreopen;
		tNewIndex.m_sGlobalIDFPath = pCurrentlyServed->m_sGlobalIDFPath;
		tNewIndex.m_bOnDiskAttrs = pCurrentlyServed->m_bOnDiskAttrs;
		tNewIndex.m_bOnDiskPools = pCurrentlyServed->m_bOnDiskPools;

		// set settings into index
		tNewIndex.m_pIndex->m_iExpandKeywords = pCurrentlyServed->m_iExpandKeywords;
		tNewIndex.m_bOnlyNew = pCurrentlyServed->m_bOnlyNew;
		tNewIndex.m_pIndex->SetPreopen ( pCurrentlyServed->m_bPreopen || g_bPreopenIndexes );
		tNewIndex.m_pIndex->SetGlobalIDFPath ( pCurrentlyServed->m_sGlobalIDFPath );
		tNewIndex.m_pIndex->SetMemorySettings ( tNewIndex.m_bMlock, tNewIndex.m_bOnDiskAttrs, tNewIndex.m_bOnDiskPools );

		dActivePath.SetBase ( pCurrentlyServed->m_sIndexPath );
		dNewPath.SetBase ( pCurrentlyServed->m_sNewPath );
		eRot = CheckIndexHeaderRotate ( *pCurrentlyServed );
	}

	if ( eRot==ROTATE_FROM::NONE )
	{
		sphWarning ( "nothing to rotate for index '%s'", sIndex.cstr() );
		return false;
	}

	CSphString sPathTo;
	switch ( eRot )
	{
	case ROTATE_FROM::PATH_NEW: // move from new_path/idx.new.sph -> new_path/idx.sph
		tNewIndex.m_pIndex->SetBase ( dNewPath.MakePath (".new").cstr() );
		sPathTo = dNewPath.GetBase();
		break;

	case ROTATE_FROM::PATH_COPY: // load from new_path/idx.sph
		tNewIndex.m_pIndex->SetBase ( dNewPath.GetBase ().cstr() );
		break;

	case ROTATE_FROM::REENABLE: // load from current_path/idx.sph
		tNewIndex.m_pIndex->SetBase ( dActivePath.GetBase ().cstr() );
		break;

	//case ROTATE_FROM::NEW:  // move from current_path/idx.new.sph -> current_path/idx.sph
	default:
		tNewIndex.m_pIndex->SetBase ( dActivePath.MakePath ( ".new" ).cstr () );
		sPathTo = dActivePath.GetBase ();
	}

	// prealloc enough RAM and lock new index
	sphLogDebug ( "prealloc enough RAM and lock new index" );

	if ( tNewIndex.m_bOnlyNew )
	{
		if ( !PreallocNewIndex ( tNewIndex, sIndex.cstr () ) )
			return false;
	} else if ( !PreallocNewIndex ( tNewIndex, nullptr, sIndex.cstr () ) )
		return false;

	tNewIndex.m_pIndex->Preread();

	//////////////////////
	// activate new index
	//////////////////////

	sphLogDebug ( "activate new index" );

	ServedDescRPtr_c pCurrentlyServed ( pRotating );

	CSphIndex * pOld = pCurrentlyServed->m_pIndex;
	CSphIndex * pNew = tNewIndex.m_pIndex;

	bool bHaveBackup = false;
	if ( !sPathTo.IsEmpty () )
	{
		if ( dActivePath.GetBase ()==sPathTo && !pCurrentlyServed->m_bOnlyNew && dActivePath.ReadVersion () )
		{
			// moving to active path; need backup to .old!
			bHaveBackup = pOld->Rename ( dActivePath.MakePath ( ".old" ).cstr () );
			if ( !bHaveBackup )
			{
				sError.SetSprintf ( "rotating index '%s': cur to old rename failed: %s", sIndex.cstr ()
									, pOld->GetLastError ().cstr () );
				return false;
			}
		}

		if ( !pNew->Rename ( sPathTo.cstr() ) )
		{
			sError.SetSprintf ( "rotating index '%s': new to cur rename failed: %s", sIndex.cstr ()
								, pNew->GetLastError ().cstr () );
			if ( bHaveBackup )
			{
				if ( !pOld->Rename( dActivePath.GetBase ().cstr ()))
				{
					sError.SetSprintf ( "rotating index '%s': old to cur rename failed: %s; INDEX UNUSABLE"
										, sIndex.cstr (), pOld->GetLastError ().cstr () );
					g_pLocalIndexes->Delete ( sIndex );
				}
			}
			return false;
		}
		if ( bHaveBackup )
			pCurrentlyServed->m_sUnlink = dActivePath.MakePath ( ".old" );
	}

	// all went fine; swap them
	sphLogDebug ( "all went fine; swap them" );

	pNew->m_iTID = pOld->m_iTID;
	if ( g_pBinlog )
		g_pBinlog->NotifyIndexFlush ( sIndex.cstr(), pOld->m_iTID, false );

	// set new index to hash
	tNewIndex.m_sIndexPath = tNewIndex.m_pIndex->GetFilename();
	g_pLocalIndexes->AddOrReplace ( new ServedIndex_c ( tNewIndex ), sIndex );
	sphInfo ( "rotating index '%s': success", sIndex.cstr() );
	tNewIndex.m_pIndex = nullptr;
	return true;
}

void RotationThreadFunc ( void * )
{
	while ( !g_bShutdown )
	{
		// check if we have work to do
		if ( !g_pDisabledIndexes->GetLength () )
		{
			sphSleepMsec ( 50 );
			continue;
		}

		// want to track rotation thread only at work
		ThreadSystem_t tThdSystemDesc ( "ROTATION" );

		CSphString sIndex;
		bool bMutable = false;
		{
			ServedIndexRefPtr_c pIndex ( nullptr );
			// scope for g_pDisabledIndexes
			{
				RLockedServedIt_c it ( g_pDisabledIndexes );
				it.Next();
				sIndex = it.GetName ();
				pIndex = it.Get();
			}

			if ( pIndex ) // that is rt/percolate. Plain locals has just name and nullptr index
			{
				{
					ServedDescRPtr_c tLocked ( pIndex );
					bMutable = tLocked->IsMutable ();
					// cluster indexes got managed by different path
					if ( tLocked->IsCluster() )
						continue;
				}

				// prealloc RT and percolate here
				if ( bMutable )
				{
					ServedDescWPtr_c wLocked ( pIndex );
					if ( PreallocNewIndex ( *wLocked, sIndex.cstr() ) )
					{
						wLocked->m_bOnlyNew = false;
						g_pLocalIndexes->AddOrReplace ( pIndex, sIndex );
						pIndex->AddRef ();
					} else
					{
						g_pLocalIndexes->DeleteIfNull ( sIndex );
					}
				}
			}
		}

		if ( !bMutable )
		{
			CSphString sError;
			if ( !RotateIndexMT ( sIndex, sError ) )
				sphWarning ( "%s", sError.cstr () );
		}

		g_pDistIndexes->Delete ( sIndex ); // postponed delete of same-named distributed (if any)
		g_pDisabledIndexes->Delete ( sIndex );

		// FIXME!!! fix that mess with queue and atomic and sleep by event and queue
		// FIXME!!! move IDF rotation here too
		if ( !g_pDisabledIndexes->GetLength () )
		{
			g_bInRotate = false;
			g_bInvokeRotationService = true;
			sphInfo ( "rotating index: all indexes done" );
		}
	}
}

static void PrereadFunc ( void * )
{
	ThreadSystem_t tThdSystemDesc ( "PREREAD" );

	int64_t tmStart = sphMicroTimer();

	StrVec_t dIndexes;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
	{
		if ( it.Get() )
			dIndexes.Add ( it.GetName () );
	}

	g_bPrereading = true;
	sphInfo ( "prereading %d indexes", dIndexes.GetLength() );
	int iReaded = 0;

	for ( int i=0; i<dIndexes.GetLength() && !g_bShutdown; ++i )
	{
		const CSphString & sName = dIndexes[i];
		auto pServed = GetServed ( sName );
		if ( !pServed )
			continue;

		ServedDescRPtr_c dReadLock ( pServed );
		if ( dReadLock->m_eType==IndexType_e::TEMPLATE )
			continue;

		int64_t tmReading = sphMicroTimer();

		sphLogDebug ( "prereading index '%s'", sName.cstr() );

		dReadLock->m_pIndex->Preread();
		if ( !dReadLock->m_pIndex->GetLastWarning().IsEmpty() )
			sphWarning ( "'%s' preread: %s", sName.cstr(), dReadLock->m_pIndex->GetLastWarning().cstr() );

		int64_t tmReaded = sphMicroTimer() - tmReading;
		sphLogDebug ( "prereaded index '%s' in %0.3f sec", sName.cstr(), float(tmReaded)/1000000.0f );

		++iReaded;
	}

	g_bPrereading = false;
	int64_t tmFinished = sphMicroTimer() - tmStart;
	sphInfo ( "prereaded %d indexes in %0.3f sec", iReaded, float(tmFinished)/1000000.0f );
}


//////////////////////////////////////////////////////////////////////////
// SPHINXQL STATE
//////////////////////////////////////////////////////////////////////////

struct NamedRefVectorPair_t
{
	CSphString			m_sName;
	CSphRefcountedPtr<UservarIntSet_c>	m_pVal;
};


/// SphinxQL state writer thread
/// periodically flushes changes of uservars, UDFs
static void SphinxqlStateThreadFunc ( void * )
{
	assert ( !g_sSphinxqlState.IsEmpty() );
	CSphString sNewState;
	sNewState.SetSprintf ( "%s.new", g_sSphinxqlState.cstr() );

	char dBuf[512];
	const int iMaxString = 80;
	assert ( (int)sizeof(dBuf) > iMaxString );

	CSphString sError;
	CSphWriter tWriter;

	int64_t tmLast = g_tmSphinxqlState;
	while ( !g_bShutdown )
	{
		// stand still till save time
		if ( tmLast==g_tmSphinxqlState )
		{
			sphSleepMsec ( QLSTATE_FLUSH_MSEC );
			continue;
		}

		// close and truncate the .new file
		tWriter.CloseFile ( true );
		if ( !tWriter.OpenFile ( sNewState, sError ) )
		{
			sphWarning ( "sphinxql_state flush failed: %s", sError.cstr() );
			sphSleepMsec ( QLSTATE_FLUSH_MSEC );
			continue;
		}

		/////////////
		// save UDFs
		/////////////

		ThreadSystem_t tThdSystemDesc ( "SphinxQL state save" );

		sphPluginSaveState ( tWriter );

		/////////////////
		// save uservars
		/////////////////

		tmLast = g_tmSphinxqlState;
		CSphVector<NamedRefVectorPair_t> dUservars;
		{
			ScopedMutex_t tLock ( g_tUservarsMutex );
			dUservars.Reserve ( g_hUservars.GetLength () );
			g_hUservars.IterateStart ();
			while ( g_hUservars.IterateNext () )
			{
				if ( !g_hUservars.IterateGet ().m_pVal->GetLength () )
					continue;

				NamedRefVectorPair_t &tPair = dUservars.Add ();
				tPair.m_sName = g_hUservars.IterateGetKey ();
				tPair.m_pVal = g_hUservars.IterateGet ().m_pVal;
			}
		}
		dUservars.Sort ( bind ( &NamedRefVectorPair_t::m_sName ) );

		// reinitiate store process on new variables added
		ARRAY_FOREACH_COND ( i, dUservars, tmLast==g_tmSphinxqlState )
		{
			const CSphVector<SphAttr_t> & dVals = *dUservars[i].m_pVal;
			int iLen = snprintf ( dBuf, sizeof ( dBuf ), "SET GLOBAL %s = ( " INT64_FMT, dUservars[i].m_sName.cstr(), dVals[0] );
			for ( int j=1; j<dVals.GetLength(); j++ )
			{
				iLen += snprintf ( dBuf+iLen, sizeof ( dBuf ), ", " INT64_FMT, dVals[j] );

				if ( iLen>=iMaxString && j<dVals.GetLength()-1 )
				{
					iLen += snprintf ( dBuf+iLen, sizeof ( dBuf ), " \\\n" );
					tWriter.PutBytes ( dBuf, iLen );
					iLen = 0;
				}
			}

			if ( iLen )
				tWriter.PutBytes ( dBuf, iLen );

			char sTail[] = " );\n";
			tWriter.PutBytes ( sTail, sizeof ( sTail )-1 );
		}

		/////////////////////////////////
		// writing done, flip the burger
		/////////////////////////////////

		tWriter.CloseFile();
		if ( sph::rename ( sNewState.cstr(), g_sSphinxqlState.cstr() )==0 )
		{
			::unlink ( sNewState.cstr() );
		} else
		{
			sphWarning ( "sphinxql_state flush: rename %s to %s failed: %s",
				sNewState.cstr(), g_sSphinxqlState.cstr(), strerrorm(errno) );
		}
	}
}


/// process a single line from sphinxql state/startup script
static bool SphinxqlStateLine ( CSphVector<char> & dLine, CSphString * sError )
{
	assert ( sError );
	if ( !dLine.GetLength() )
		return true;

	// parser expects CSphString buffer with gap bytes at the end
	if ( dLine.Last()==';' )
		dLine.Pop();
	dLine.Add ( '\0' );
	dLine.Add ( '\0' );
	dLine.Add ( '\0' );

	CSphVector<SqlStmt_t> dStmt;
	bool bParsedOK = sphParseSqlQuery ( dLine.Begin(), dLine.GetLength(), dStmt, *sError, SPH_COLLATION_DEFAULT );
	if ( !bParsedOK )
		return false;

	bool bOk = true;
	ARRAY_FOREACH ( i, dStmt )
	{
		SqlStmt_t & tStmt = dStmt[i];
		if ( tStmt.m_eStmt==STMT_SET && tStmt.m_eSet==SET_GLOBAL_UVAR )
		{
			tStmt.m_dSetValues.Sort();
			UservarAdd ( tStmt.m_sSetName, tStmt.m_dSetValues );
		} else if ( tStmt.m_eStmt==STMT_CREATE_FUNCTION )
		{
			bOk &= sphPluginCreate ( tStmt.m_sUdfLib.cstr(), PLUGIN_FUNCTION, tStmt.m_sUdfName.cstr(), tStmt.m_eUdfType, *sError );

		} else if ( tStmt.m_eStmt==STMT_CREATE_PLUGIN )
		{
			bOk &= sphPluginCreate ( tStmt.m_sUdfLib.cstr(), sphPluginGetType ( tStmt.m_sStringParam ),
				tStmt.m_sUdfName.cstr(), SPH_ATTR_NONE, *sError );
		} else
		{
			bOk = false;
			sError->SetSprintf ( "unsupported statement (must be one of SET GLOBAL, CREATE FUNCTION, CREATE PLUGIN)" );
		}
	}

	return bOk;
}


/// uservars table reader
static void SphinxqlStateRead ( const CSphString & sName )
{
	if ( sName.IsEmpty() )
		return;

	CSphString sError;
	CSphAutoreader tReader;
	if ( !tReader.Open ( sName, sError ) )
		return;

	const int iReadBlock = 32*1024;
	const int iGapLen = 2;
	CSphVector<char> dLine;
	dLine.Reserve ( iReadBlock + iGapLen );

	bool bEscaped = false;
	int iLines = 0;
	while (true)
	{
		const BYTE * pData = NULL;
		int iRead = tReader.GetBytesZerocopy ( &pData, iReadBlock );
		// all uservars got read
		if ( iRead<=0 )
			break;

		// read escaped line
		dLine.Reserve ( dLine.GetLength() + iRead + iGapLen );
		const BYTE * s = pData;
		const BYTE * pEnd = pData+iRead;
		while ( s<pEnd )
		{
			// goto next line for escaped string
			if ( *s=='\\' || ( bEscaped && ( *s=='\n' || *s=='\r' ) ) )
			{
				s++;
				while ( s<pEnd && ( *s=='\n' || *s=='\r' ) )
				{
					iLines += ( *s=='\n' );
					s++;
				}
				bEscaped = ( s>=pEnd );
				continue;
			}

			bEscaped = false;
			if ( *s=='\n' || *s=='\r' )
			{
				if ( !SphinxqlStateLine ( dLine, &sError ) )
					sphWarning ( "sphinxql_state: parse error at line %d: %s", 1+iLines, sError.cstr() );

				dLine.Resize ( 0 );
				s++;
				while ( s<pEnd && ( *s=='\n' || *s=='\r' ) )
				{
					iLines += ( *s=='\n' );
					s++;
				}
				continue;
			}

			dLine.Add ( *s );
			s++;
		}
	}

	if ( !SphinxqlStateLine ( dLine, &sError ) )
		sphWarning ( "sphinxql_state: parse error at line %d: %s", 1+iLines, sError.cstr() );
}

//////////////////////////////////////////////////////////////////////////

void OptimizeThreadFunc ( void * )
{
	while ( !g_bShutdown )
	{
		// stand still till optimize time
		if ( !g_dOptimizeQueue.GetLength() )
		{
			sphSleepMsec ( 50 );
			continue;
		}

		CSphString sIndex;
		g_tOptimizeQueueMutex.Lock();
		if ( g_dOptimizeQueue.GetLength() )
		{
			sIndex = g_dOptimizeQueue[0];
			g_dOptimizeQueue.Remove(0);
		}
		g_tOptimizeQueueMutex.Unlock();

		auto pServed = GetServed ( sIndex );

		if ( !pServed )
			continue;

		ServedDescRPtr_c dReadLock ( pServed );
		if ( !dReadLock->m_pIndex )
			continue;

		// want to track optimize only at work
		ThreadSystem_t tThdSystemDesc ( "OPTIMIZE" );

		// FIXME: MVA update would wait w-lock here for a very long time
		assert ( dReadLock->m_eType==IndexType_e::RT );
		static_cast<ISphRtIndex *>( dReadLock->m_pIndex )->Optimize ();
	}
}

static int ParseKeywordExpansion ( const char * sValue ) REQUIRES ( MainThread )
{
	if ( !sValue || *sValue=='\0' )
		return KWE_DISABLED;

	int iOpt = KWE_DISABLED;
	while ( sValue && *sValue )
	{
		if ( !sphIsAlpha ( *sValue ) )
		{
			sValue++;
			continue;
		}

		if ( *sValue>='0' && *sValue<='9' )
		{
			int iVal = atoi ( sValue );
			if ( iVal!=0 )
				iOpt = KWE_ENABLED;
			break;
		}

		if ( sphStrMatchStatic ( "exact", sValue ) )
		{
			iOpt |= KWE_EXACT;
			sValue += 5;
		} else if ( sphStrMatchStatic ( "star", sValue ) )
		{
			iOpt |= KWE_STAR;
			sValue += 4;
		} else
		{
			sValue++;
		}
	}

	return iOpt;
}


void ConfigureTemplateIndex ( ServedDesc_t * pIdx, const CSphConfigSection & hIndex ) REQUIRES ( MainThread )
{
	pIdx->m_iExpandKeywords = ParseKeywordExpansion ( hIndex.GetStr( "expand_keywords", "" ) );
}

void ConfigureLocalIndex ( ServedDesc_t * pIdx, const CSphConfigSection & hIndex ) REQUIRES ( MainThread )
{
	ConfigureTemplateIndex ( pIdx, hIndex);
	pIdx->m_bMlock = ( hIndex.GetInt ( "mlock", 0 )!=0 ) && !g_bOptNoLock;
	pIdx->m_bPreopen = ( hIndex.GetInt ( "preopen", 0 )!=0 );
	pIdx->m_sGlobalIDFPath = hIndex.GetStr ( "global_idf" );
	pIdx->m_bOnDiskAttrs = ( hIndex.GetInt ( "ondisk_attrs", 0 )==1 );
	pIdx->m_bOnDiskPools = ( strcmp ( hIndex.GetStr ( "ondisk_attrs", "" ), "pool" )==0 );
	pIdx->m_bOnDiskAttrs |= g_bOnDiskAttrs;
	pIdx->m_bOnDiskPools |= g_bOnDiskPools;
}


static void ConfigureDistributedIndex ( DistributedIndex_t & tIdx, const char * szIndexName,
	const CSphConfigSection & hIndex ) REQUIRES ( MainThread )
{
	assert ( hIndex("type") && hIndex["type"]=="distributed" );

	bool bSetHA = false;
	// configure ha_strategy
	if ( hIndex("ha_strategy") )
	{
		bSetHA = ParseStrategyHA ( hIndex["ha_strategy"].cstr(), &tIdx.m_eHaStrategy );
		if ( !bSetHA )
			sphWarning ( "index '%s': ha_strategy (%s) is unknown for me, will use random", szIndexName, hIndex["ha_strategy"].cstr() );
	}

	bool bEnablePersistentConns = ( g_iPersistentPoolSize>0 );
	if ( hIndex ( "agent_persistent" ) && !bEnablePersistentConns )
	{
			sphWarning ( "index '%s': agent_persistent used, but no persistent_connections_limit defined. Fall back to non-persistent agent", szIndexName );
			bEnablePersistentConns = false;
	}

	// add local agents
	StrVec_t dLocs;
	CSphVector<int> dKillBreak;
	for ( CSphVariant * pLocal = hIndex("local"); pLocal; pLocal = pLocal->m_pNext )
	{
		dLocs.Resize(0);
		sphSplit ( dLocs, pLocal->cstr(), " \t," );
		for ( const auto& sLocal: dLocs )
		{
			// got hidden feature kill-list break
			if ( sLocal=="$BREAK" )
			{
				dKillBreak.Add ( tIdx.m_dLocal.GetLength() );
				continue;
			}

			if ( !g_pLocalIndexes->Contains ( sLocal ) )
			{
				sphWarning ( "index '%s': no such local index '%s', SKIPPED", szIndexName, sLocal.cstr() );
				continue;
			}
			tIdx.m_dLocal.Add ( sLocal );
		}
	}
	if ( dKillBreak.GetLength() )
	{
		tIdx.m_dKillBreak.Init ( tIdx.m_dLocal.GetLength()+1 );
		for ( int iBreak: dKillBreak )
			tIdx.m_dKillBreak.BitSet ( iBreak );
	}

	// index-level agent_retry_count
	if ( hIndex ( "agent_retry_count" ) )
	{
		if ( hIndex["agent_retry_count"].intval ()<=0 )
			sphWarning ( "index '%s': agent_retry_count must be positive, ignored", szIndexName );
		else
			tIdx.m_iAgentRetryCount = hIndex["agent_retry_count"].intval ();
	}

	if ( hIndex ( "mirror_retry_count" ) )
	{
		if ( hIndex["mirror_retry_count"].intval ()<=0 )
			sphWarning ( "index '%s': mirror_retry_count must be positive, ignored", szIndexName );
		else
		{
			if ( tIdx.m_iAgentRetryCount>0 )
				sphWarning ("index '%s': `agent_retry_count` and `mirror_retry_count` both specified (they are aliases)."
					"Value of `mirror_retry_count` will be used", szIndexName );
			tIdx.m_iAgentRetryCount = hIndex["mirror_retry_count"].intval ();
		}
	}

	if ( !tIdx.m_iAgentRetryCount )
		tIdx.m_iAgentRetryCount = g_iAgentRetryCount;


	// add remote agents
	struct { const char* sSect; bool bBlh; bool bPrs; } tAgentVariants[] = {
		{ "agent", 				false,	false},
		{ "agent_persistent", 	false,	bEnablePersistentConns },
		{ "agent_blackhole", 	true,	false }
	};

	for ( auto &tAg : tAgentVariants )
	{
		for ( CSphVariant * pAgentCnf = hIndex ( tAg.sSect ); pAgentCnf; pAgentCnf = pAgentCnf->m_pNext )
		{
			AgentOptions_t tAgentOptions { tAg.bBlh, tAg.bPrs, tIdx.m_eHaStrategy, tIdx.m_iAgentRetryCount, 0 };
			auto pAgent = ConfigureMultiAgent ( pAgentCnf->cstr(), szIndexName, tAgentOptions );
			if ( pAgent )
				tIdx.m_dAgents.Add ( pAgent );
		}
	}

	// configure options
	if ( hIndex("agent_connect_timeout") )
	{
		if ( hIndex["agent_connect_timeout"].intval()<=0 )
			sphWarning ( "index '%s': agent_connect_timeout must be positive, ignored", szIndexName );
		else
			tIdx.m_iAgentConnectTimeout = hIndex["agent_connect_timeout"].intval();
	}

	tIdx.m_bDivideRemoteRanges = hIndex.GetInt ( "divide_remote_ranges", 0 )!=0;

	if ( hIndex("agent_query_timeout") )
	{
		if ( hIndex["agent_query_timeout"].intval()<=0 )
			sphWarning ( "index '%s': agent_query_timeout must be positive, ignored", szIndexName );
		else
			tIdx.m_iAgentQueryTimeout = hIndex["agent_query_timeout"].intval();
	}

	bool bHaveHA = tIdx.m_dAgents.FindFirst ( [] ( MultiAgentDesc_c * ag ) { return ag->IsHA (); } );

	// configure ha_strategy
	if ( bSetHA && !bHaveHA )
		sphWarning ( "index '%s': ha_strategy defined, but no ha agents in the index", szIndexName );
}

//////////////////////////////////////////////////
/// configure distributed index and add it to hash
//////////////////////////////////////////////////
ESphAddIndex AddDistributedIndex ( const char * szIndexName, const CSphConfigSection &hIndex ) REQUIRES ( MainThread )
{

	DistributedIndexRefPtr_t pIdx ( new DistributedIndex_t );
	ConfigureDistributedIndex ( *pIdx, szIndexName, hIndex );

	// finally, check and add distributed index to global table
	if ( pIdx->IsEmpty () )
	{
		sphWarning ( "index '%s': no valid local/remote indexes in distributed index - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	if ( !g_pDistIndexes->AddUniq ( pIdx, szIndexName ) )
	{
		sphWarning ( "index '%s': unable to add name (duplicate?) - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	pIdx->AddRef (); // on succeed add - hash owns index
	return ADD_DISTR;
}

// hash any local index (plain, rt, etc.)
bool AddLocallyServedIndex ( const char * szIndexName, ServedDesc_t & tIdx, bool bReplace ) REQUIRES ( MainThread )
{
	ServedIndexRefPtr_c pIdx ( new ServedIndex_c ( tIdx ) );

	// at this point only template indexes are production-ready
	// so all no-templates we implicitly add to disabled hash
	if ( tIdx.m_eType!=IndexType_e::TEMPLATE )
	{
		g_pLocalIndexes->AddUniq ( nullptr, szIndexName );
		if ( !g_pDisabledIndexes->AddUniq ( pIdx, szIndexName ) )
		{
			sphWarning ( "INTERNAL ERROR: index '%s': hash add failed - NOT SERVING", szIndexName );
			g_pLocalIndexes->DeleteIfNull ( szIndexName );
			return false;
		}
	} else // templates we either add, either replace depending on requiested action
	{
		if ( bReplace )
			g_pLocalIndexes->AddOrReplace ( pIdx, szIndexName );
		else if ( !g_pLocalIndexes->AddUniq ( pIdx, szIndexName ) )
		{
			sphWarning ( "INTERNAL ERROR: index '%s': hash add failed - NOT SERVING", szIndexName );
			return false;
		}
	}
	// leak pointer, so it's destructor won't delete it
	tIdx.m_pIndex = nullptr;
	pIdx->AddRef ();
	return true;
}

// common configuration and add percolate or rt index
ESphAddIndex AddRTPercolate ( const char * szIndexName, ServedDesc_t & tIdx, const CSphConfigSection &hIndex, const CSphIndexSettings & tSettings, bool bReplace ) REQUIRES ( MainThread )
{
	tIdx.m_sIndexPath = hIndex["path"].strval ();
	ConfigureLocalIndex ( &tIdx, hIndex );
	tIdx.m_pIndex->m_iExpandKeywords = tIdx.m_iExpandKeywords;
	tIdx.m_pIndex->m_iExpansionLimit = g_iExpansionLimit;
	tIdx.m_pIndex->SetPreopen ( tIdx.m_bPreopen || g_bPreopenIndexes );
	tIdx.m_pIndex->SetGlobalIDFPath ( tIdx.m_sGlobalIDFPath );
	tIdx.m_pIndex->SetMemorySettings ( tIdx.m_bMlock, tIdx.m_bOnDiskAttrs, tIdx.m_bOnDiskPools );

	tIdx.m_pIndex->Setup ( tSettings );
	tIdx.m_pIndex->SetCacheSize ( g_iMaxCachedDocs, g_iMaxCachedHits );

	CSphIndexStatus tStatus;
	tIdx.m_pIndex->GetStatus ( &tStatus );
	tIdx.m_iMass = CalculateMass ( tStatus );

	// hash it
	if ( AddLocallyServedIndex ( szIndexName, tIdx, bReplace ) )
		return ADD_DSBLED;
	return ADD_ERROR;
}
///////////////////////////////////////////////
/// configure realtime index and add it to hash
///////////////////////////////////////////////
ESphAddIndex AddRTIndex ( const char * szIndexName, const CSphConfigSection &hIndex, bool bReplace ) REQUIRES ( MainThread )
{
	CSphString sError;
	CSphSchema tSchema ( szIndexName );
	if ( !sphRTSchemaConfigure ( hIndex, &tSchema, &sError, false ) )
	{
		sphWarning ( "index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}

	if ( !sError.IsEmpty () )
		sphWarning ( "index '%s': %s", szIndexName, sError.cstr () );

	// path
	if ( !hIndex ( "path" ) )
	{
		sphWarning ( "index '%s': path must be specified - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// check name
	if ( !bReplace && g_pLocalIndexes->Contains ( szIndexName ) )
	{
		sphWarning ( "index '%s': duplicate name - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// pick config settings
	// they should be overriden later by Preload() if needed
	CSphIndexSettings tSettings;
	if ( !sphConfIndex ( hIndex, tSettings, sError ) )
	{
		sphWarning ( "ERROR: index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}

	int iIndexSP = hIndex.GetInt ( "index_sp" );
	const char * sIndexZones = hIndex.GetStr ( "index_zones", "" );
	bool bHasStripEnabled ( hIndex.GetInt ( "html_strip" )!=0 );
	if ( ( iIndexSP!=0 || ( *sIndexZones ) ) && !bHasStripEnabled )
	{
		// SENTENCE indexing w\o stripper is valid combination
		if ( *sIndexZones )
		{
			sphWarning ( "ERROR: index '%s': has index_sp=%d, index_zones='%s' but disabled html_strip - NOT SERVING"
						 , szIndexName, iIndexSP, sIndexZones );
			return ADD_ERROR;
		} else
		{
			sphWarning ( "index '%s': has index_sp=%d but disabled html_strip - PARAGRAPH unavailable", szIndexName
						 , iIndexSP );
		}
	}

	// RAM chunk size
	int64_t iRamSize = hIndex.GetSize64 ( "rt_mem_limit", 128 * 1024 * 1024 );
	if ( iRamSize<128 * 1024 )
	{
		sphWarning ( "index '%s': rt_mem_limit extremely low, using 128K instead", szIndexName );
		iRamSize = 128 * 1024;
	} else if ( iRamSize<8 * 1024 * 1024 )
		sphWarning ( "index '%s': rt_mem_limit very low (under 8 MB)", szIndexName );

	// upgrading schema to store field lengths
	if ( tSettings.m_bIndexFieldLens )
		if ( !AddFieldLens ( tSchema, false, sError ) )
		{
			sphWarning ( "index '%s': failed to create field lengths attributes: %s", szIndexName, sError.cstr () );
			return ADD_ERROR;
		}

	bool bWordDict = strcmp ( hIndex.GetStr ( "dict", "keywords" ), "keywords" )==0;
	if ( !bWordDict )
		sphWarning ( "dict=crc deprecated, use dict=keywords instead" );
	if ( bWordDict && ( tSettings.m_dPrefixFields.GetLength () || tSettings.m_dInfixFields.GetLength () ) )
		sphWarning ( "WARNING: index '%s': prefix_fields and infix_fields has no effect with dict=keywords, ignoring\n", szIndexName);
	if ( bWordDict && tSettings.m_iMinInfixLen==1 )
	{
		sphWarn ( "min_infix_len must be greater than 1, changed to 2" );
		tSettings.m_iMinInfixLen = 2;
	}

	// index
	ServedDesc_t tIdx;
	tIdx.m_pIndex = sphCreateIndexRT ( tSchema, szIndexName, iRamSize, hIndex["path"].cstr (), bWordDict );
	tIdx.m_eType = IndexType_e::RT;
	return AddRTPercolate ( szIndexName, tIdx, hIndex, tSettings, bReplace );
}

////////////////////////////////////////////////
/// configure percolate index and add it to hash
////////////////////////////////////////////////
ESphAddIndex AddPercolateIndex ( const char * szIndexName, const CSphConfigSection &hIndex, bool bReplace ) REQUIRES ( MainThread )
{
	CSphString sError;
	CSphSchema tSchema ( szIndexName );
	if ( !sphRTSchemaConfigure ( hIndex, &tSchema, &sError, true ) )
	{
		sphWarning ( "index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}
	FixPercolateSchema ( tSchema );

	if ( !sError.IsEmpty () )
		sphWarning ( "index '%s': %s", szIndexName, sError.cstr () );

	// path
	if ( !hIndex ( "path" ) )
	{
		sphWarning ( "index '%s': path must be specified - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// check name
	if ( !bReplace && g_pLocalIndexes->Contains ( szIndexName ) )
	{
		sphWarning ( "index '%s': duplicate name - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// pick config settings
	// they should be overriden later by Preload() if needed
	CSphIndexSettings tSettings;
	if ( !sphConfIndex ( hIndex, tSettings, sError ) )
	{
		sphWarning ( "ERROR: index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}

	int iIndexSP = hIndex.GetInt ( "index_sp" );
	const char * sIndexZones = hIndex.GetStr ( "index_zones", "" );
	bool bHasStripEnabled ( hIndex.GetInt ( "html_strip" )!=0 );
	if ( ( iIndexSP!=0 || ( *sIndexZones ) ) && !bHasStripEnabled )
	{
		// SENTENCE indexing w\o stripper is valid combination
		if ( *sIndexZones )
		{
			sphWarning ( "ERROR: index '%s': has index_sp=%d, index_zones='%s' but disabled html_strip - NOT SERVING",
				szIndexName, iIndexSP, sIndexZones );
			return ADD_ERROR;
		} else
		{
			sphWarning ( "index '%s': has index_sp=%d but disabled html_strip - PARAGRAPH unavailable",
				szIndexName, iIndexSP );
		}
	}

	// upgrading schema to store field lengths
	if ( tSettings.m_bIndexFieldLens )
		if ( !AddFieldLens ( tSchema, false, sError ) )
		{
			sphWarning ( "index '%s': failed to create field lengths attributes: %s", szIndexName, sError.cstr () );
			return ADD_ERROR;
		}

	if ( ( tSettings.m_dPrefixFields.GetLength () || tSettings.m_dInfixFields.GetLength () ) )
		sphWarning ( "WARNING: index '%s': prefix_fields and infix_fields has no effect with dict=keywords, ignoring\n", szIndexName );

	if ( tSettings.m_iMinInfixLen==1 )
	{
		sphWarn ( "min_infix_len must be greater than 1, changed to 2" );
		tSettings.m_iMinInfixLen = 2;
	}


	// index
	ServedDesc_t tIdx;
	tIdx.m_pIndex = CreateIndexPercolate ( tSchema, szIndexName, hIndex["path"].cstr () );
	tIdx.m_eType = IndexType_e::PERCOLATE;
	return AddRTPercolate ( szIndexName, tIdx, hIndex, tSettings, bReplace );
}

////////////////////////////////////////////
/// configure local index and add it to hash
////////////////////////////////////////////
ESphAddIndex AddPlainIndex ( const char * szIndexName, const CSphConfigSection &hIndex, bool bReplace ) REQUIRES ( MainThread )
{
	ServedDesc_t tIdx;

	// check path
	if ( !hIndex.Exists ( "path" ) )
	{
		sphWarning ( "index '%s': key 'path' not found - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// check name
	if ( !bReplace && g_pLocalIndexes->Contains ( szIndexName ) )
	{
		sphWarning ( "index '%s': duplicate name - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// configure memlocking, star
	ConfigureLocalIndex ( &tIdx, hIndex );

	// try to create index
	tIdx.m_sIndexPath = hIndex["path"].strval ();
	tIdx.m_pIndex = sphCreateIndexPhrase ( szIndexName, tIdx.m_sIndexPath.cstr () );
	tIdx.m_pIndex->m_iExpandKeywords = tIdx.m_iExpandKeywords;
	tIdx.m_pIndex->m_iExpansionLimit = g_iExpansionLimit;
	tIdx.m_pIndex->SetPreopen ( tIdx.m_bPreopen || g_bPreopenIndexes );
	tIdx.m_pIndex->SetGlobalIDFPath ( tIdx.m_sGlobalIDFPath );
	tIdx.m_pIndex->SetMemorySettings ( tIdx.m_bMlock, tIdx.m_bOnDiskAttrs, tIdx.m_bOnDiskPools );
	tIdx.m_pIndex->SetCacheSize ( g_iMaxCachedDocs, g_iMaxCachedHits );
	CSphIndexStatus tStatus;
	tIdx.m_pIndex->GetStatus ( &tStatus );
	tIdx.m_iMass = CalculateMass ( tStatus );

	// done
	if ( AddLocallyServedIndex ( szIndexName, tIdx, bReplace ) )
		return ADD_DSBLED;
	return ADD_ERROR;
}

///////////////////////////////////////////////
/// configure template index and add it to hash
///////////////////////////////////////////////
ESphAddIndex AddTemplateIndex ( const char * szIndexName, const CSphConfigSection &hIndex, bool bReplace ) REQUIRES ( MainThread )
{
	ServedDesc_t tIdx;

	// check name
	if ( !bReplace && g_pLocalIndexes->Contains ( szIndexName ) )
	{
		sphWarning ( "index '%s': duplicate name - NOT SERVING", szIndexName );
		return ADD_ERROR;
	}

	// configure memlocking, star
	ConfigureTemplateIndex ( &tIdx, hIndex );

	// try to create index
	tIdx.m_pIndex = sphCreateIndexTemplate ();
	tIdx.m_pIndex->m_iExpandKeywords = tIdx.m_iExpandKeywords;
	tIdx.m_pIndex->m_iExpansionLimit = g_iExpansionLimit;

	CSphIndexSettings s;
	CSphString sError;
	if ( !sphConfIndex ( hIndex, s, sError ) )
	{
		sphWarning ( "failed to configure index %s: %s", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}
	tIdx.m_pIndex->Setup ( s );

	if ( !sphFixupIndexSettings ( tIdx.m_pIndex, hIndex, sError, g_bStripPath ) )
	{
		sphWarning ( "index '%s': %s - NOT SERVING", szIndexName, sError.cstr () );
		return ADD_ERROR;
	}

	CSphIndexStatus tStatus;
	tIdx.m_pIndex->GetStatus ( &tStatus );
	tIdx.m_iMass = CalculateMass ( tStatus );
	tIdx.m_eType = IndexType_e::TEMPLATE;

	// done
	if ( AddLocallyServedIndex ( szIndexName, tIdx, bReplace ) )
		return ADD_SERVED;
	return ADD_ERROR;
}

IndexType_e TypeOfIndexConfig ( const CSphString & sType )
{
	if ( sType=="distributed" )
		return IndexType_e::DISTR;

	if ( sType=="rt" )
		return IndexType_e::RT;

	if ( sType=="percolate" )
		return IndexType_e::PERCOLATE;

	if ( sType=="template" )
		return IndexType_e::TEMPLATE;

	if ( ( sType.IsEmpty () || sType=="plain" ) )
		return IndexType_e::PLAIN;

	return IndexType_e::ERROR_;
}

const char * g_dIndexTypeName[1+(int)IndexType_e::ERROR_] = { "plain", "template", "rt", "percolate", "distributed", "invalid" };

CSphString GetTypeName ( IndexType_e eType )
{
	return g_dIndexTypeName[(int)eType];
}

// ServiceMain() -> ConfigureAndPreload() -> AddIndex()
// ServiceMain() -> TickHead() -> CheckRotate() -> ReloadIndexSettings() -> AddIndex()
ESphAddIndex AddIndex ( const char * szIndexName, const CSphConfigSection & hIndex, bool bReplace ) REQUIRES ( MainThread )
{
	switch ( TypeOfIndexConfig ( hIndex.GetStr ( "type", nullptr ) ) )
	{
		case IndexType_e::DISTR:
			return AddDistributedIndex ( szIndexName, hIndex );
		case IndexType_e::RT:
			return AddRTIndex ( szIndexName, hIndex, bReplace );
		case IndexType_e::PERCOLATE:
			return AddPercolateIndex ( szIndexName, hIndex, bReplace );
		case IndexType_e::TEMPLATE:
			return AddTemplateIndex ( szIndexName, hIndex, bReplace );
		case IndexType_e::PLAIN:
			return AddPlainIndex ( szIndexName, hIndex, bReplace );
		case IndexType_e::ERROR_:
		default:
			break;
	}

	sphWarning ( "index '%s': unknown type '%s' - NOT SERVING", szIndexName, hIndex["type"].cstr() );
	return ADD_ERROR;
}


bool CheckConfigChanges ()
{
	DWORD uCRC32 = 0;
	struct stat tStat = {0}; // fixme! we have struct_stat in defines, investigate it!

#if !USE_WINDOWS
	char sBuf [ 8192 ];
	FILE * fp = nullptr;

	fp = fopen ( g_sConfigFile.cstr (), "rb" );
	if ( !fp )
		return true;
	if ( fstat ( fileno ( fp ), &tStat )<0 )
		memset ( &tStat, 0, sizeof ( tStat ) );
	bool bGotLine = fgets ( sBuf, sizeof(sBuf), fp );
	fclose ( fp );
	if ( !bGotLine )
		return true;

	char * p = sBuf;
	while ( isspace(*p) )
		p++;
	if ( p[0]=='#' && p[1]=='!' )
	{
		p += 2;

		CSphVector<char> dContent;
		char sError [ 1024 ];
		if ( !TryToExec ( p, g_sConfigFile.cstr(), dContent, sError, sizeof(sError) ) )
			return true;

		uCRC32 = sphCRC32 ( dContent.Begin(), dContent.GetLength() );
	} else
		sphCalcFileCRC32 ( g_sConfigFile.cstr (), uCRC32 );
#else
	sphCalcFileCRC32 ( g_sConfigFile.cstr (), uCRC32 );
	if ( stat ( g_sConfigFile.cstr (), &tStat )<0 )
		memset ( &tStat, 0, sizeof ( tStat ) );
#endif

	if ( g_uCfgCRC32==uCRC32 && tStat.st_size==g_tCfgStat.st_size
		&& tStat.st_mtime==g_tCfgStat.st_mtime && tStat.st_ctime==g_tCfgStat.st_ctime )
			return false;

	g_uCfgCRC32 = uCRC32;
	g_tCfgStat = tStat;

	return true;
}

// add or remove persistent pools to hosts
void InitPersistentPool()
{
	if ( !g_iPersistentPoolSize )
	{
		ClosePersistentSockets();
		return;
	}

	VecRefPtrs_t<HostDashboard_t *> tHosts;
	g_tDashes.GetActiveDashes (tHosts);
	tHosts.Apply ( [] ( HostDashboard_t *&pHost ) {
		if ( !pHost->m_pPersPool )
			pHost->m_pPersPool = new PersistentConnectionsPool_c;
		pHost->m_pPersPool->ReInit ( g_iPersistentPoolSize );
	} );
}


// refactor!
// make possible changing of an index role. I.e., was template, became distr, as example.
//
// Reloading called always from same thread (so, for now not need to be th-safe for itself)
// ServiceMain() -> TickHead() -> CheckRotate() -> ReloadIndexSettings().
static void ReloadIndexSettings ( CSphConfigParser & tCP ) REQUIRES ( MainThread, g_tRotateConfigMutex )
{
	if ( !tCP.ReParse ( g_sConfigFile.cstr () ) )
	{
		sphWarning ( "failed to parse config file '%s'; using previous settings", g_sConfigFile.cstr () );
		return;
	}

	// collect names of all existing local indexes as assumed for deletion
	SmallStringHash_T<bool> dLocalToDelete;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next (); )
	{
		// skip JSON indexes or indexes belong to cluster - no need to manage them
		ServedDescRPtr_c pServed ( it.Get() );
		if ( pServed && pServed->IsCluster() )
			continue;

		dLocalToDelete.Add ( true, it.GetName() );
	}

	// collect names of all existing distr indexes as assumed for deletion
	SmallStringHash_T<bool> dDistrToDelete;
	for ( RLockedDistrIt_c it ( g_pDistIndexes ); it.Next (); )
		dDistrToDelete.Add ( true, it.GetName () );

	const CSphConfig &hConf = tCP.m_tConf;
	for ( hConf["index"].IterateStart (); hConf["index"].IterateNext(); )
	{
		const CSphConfigSection & hIndex = hConf["index"].IterateGet();
		const auto & sIndexName = hConf["index"].IterateGetKey();
		IndexType_e eNewType = TypeOfIndexConfig ( hIndex.GetStr ( "type", nullptr ) );

		if ( eNewType==IndexType_e::ERROR_ )
			continue;

		bool bReplaceLocal = false;
		auto pServedIndex = GetServed ( sIndexName );
		if ( pServedIndex )
		{
			bool bGotLocal = false;
			bool bReconfigure = false;
			// Rlock scope for settings compare
			{
				ServedDescRPtr_c pServedRLocked ( pServedIndex );
				if ( pServedRLocked )
				{
					bGotLocal = true;
					bReplaceLocal = ( eNewType!=pServedRLocked->m_eType );
					if ( !bReplaceLocal )
					{
						ServedDesc_t tDesc;
						ConfigureLocalIndex ( &tDesc, hIndex );
						bReconfigure = ( tDesc.m_iExpandKeywords!=pServedRLocked->m_iExpandKeywords ||
							tDesc.m_bMlock!=pServedRLocked->m_bMlock ||
							tDesc.m_bPreopen!=pServedRLocked->m_bPreopen ||
							tDesc.m_sGlobalIDFPath!=pServedRLocked->m_sGlobalIDFPath ||
							tDesc.m_bOnDiskAttrs!=pServedRLocked->m_bOnDiskAttrs ||
							tDesc.m_bOnDiskPools!=pServedRLocked->m_bOnDiskPools );
						bReconfigure |= ( pServedRLocked->m_eType!=IndexType_e::TEMPLATE && hIndex.Exists ( "path" ) && hIndex["path"].strval ()!=pServedRLocked->m_sIndexPath );
					}
				}

			}
			// Wlock'ing only in case settings changed
			if ( bReconfigure )
			{
				ServedDescWPtr_c pServedWLocked ( pServedIndex );
				ConfigureLocalIndex ( pServedWLocked, hIndex );
				if ( pServedWLocked->m_eType!=IndexType_e::TEMPLATE && hIndex.Exists ( "path" ) && hIndex["path"].strval ()!=pServedWLocked->m_sIndexPath )
				{
					pServedWLocked->m_sNewPath = hIndex["path"].strval ();
					g_pDisabledIndexes->AddUniq ( nullptr, sIndexName );
				}
			}

			if ( bGotLocal && !bReplaceLocal )
			{
				dLocalToDelete[sIndexName] = false;
				continue;
			}
		}

		auto pDistrIndex = GetDistr ( sIndexName );
		if ( pDistrIndex && eNewType==IndexType_e::DISTR )
		{
			DistributedIndexRefPtr_t ptrIdx ( new DistributedIndex_t );
			ConfigureDistributedIndex ( *ptrIdx, sIndexName.cstr (), hIndex );

			if ( !ptrIdx->IsEmpty () )
				g_pDistIndexes->AddOrReplace ( ptrIdx.Leak (), sIndexName );
			else
				sphWarning ( "index '%s': no valid local/remote indexes in distributed index; using last valid definition", sIndexName.cstr () );

			dDistrToDelete[sIndexName] = false;
			continue;
		}

		// if index was distr and switched to local - it will be added into queue as local.
		// if it was local and now distr - it is already added as distr.
		// dupes will vanish with deletion pass then.

		ESphAddIndex eType = AddIndex ( sIndexName.cstr(), hIndex, bReplaceLocal );

		// If we've added disabled index (i.e. one which need to be prealloced first)
		// instead of existing distributed - we don't have to delete last right now,
		// let rotate it later.
		if ( eType==ADD_DSBLED )
		{
			auto pAddedIndex = GetDisabled ( sIndexName.cstr() );
			assert ( pAddedIndex );
			ServedDescWPtr_c pWlockedDisabled ( pAddedIndex );
			if ( pWlockedDisabled->m_eType==IndexType_e::PLAIN )
				pWlockedDisabled->m_bOnlyNew = true;
			if ( pDistrIndex )
				dDistrToDelete[sIndexName] = false;
		}
	}

	for ( dDistrToDelete.IterateStart (); dDistrToDelete.IterateNext ();)
		if ( dDistrToDelete.IterateGet() )
			g_pDistIndexes->Delete (dDistrToDelete.IterateGetKey ());

	for ( dLocalToDelete.IterateStart (); dLocalToDelete.IterateNext (); )
		if ( dLocalToDelete.IterateGet () )
			g_pLocalIndexes->Delete ( dLocalToDelete.IterateGetKey () );

	InitPersistentPool();
}

void CheckRotateGlobalIDFs ()
{
	CSphVector <CSphString> dFiles;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
	{
		ServedDescRPtr_c pIndex (it.Get());
		if ( pIndex && !pIndex->m_sGlobalIDFPath.IsEmpty() )
			dFiles.Add ( pIndex->m_sGlobalIDFPath );
	}

	ThreadSystem_t tThdSystemDesc ( "ROTATE global IDF" );
	sphUpdateGlobalIDFs ( dFiles );
}


void RotationServiceThreadFunc ( void * )
{
	while ( !g_bShutdown )
	{
		if ( g_bInvokeRotationService )
		{
			CheckRotateGlobalIDFs ();
			g_bInvokeRotationService = false;
		}
		sphSleepMsec ( 50 );
	}
}

// ServiceMain() -> TickHead() -> CheckRotate()
void CheckRotate () REQUIRES ( MainThread )
{
	// do we need to rotate now?
	if ( !g_bNeedRotate || g_bInRotate )
		return;

	g_bInRotate = true; // ok, another rotation cycle just started
	g_bNeedRotate = false; // which therefore clears any previous HUP signals

	sphLogDebug ( "CheckRotate invoked" );

	// fixme! disabled hash protected by g_bInRotate exclusion,
	// what about more explicit protection?
	g_pDisabledIndexes->ReleaseAndClear ();
	{
		ScWL_t dRotateConfigMutexWlocked { g_tRotateConfigMutex };
		if ( CheckConfigChanges () )
			ReloadIndexSettings ( g_pCfg );
	}

	// special pass for 'simple' rotation (i.e. *.new to current)
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next (); )
	{
		auto pIdx = it.Get();
		if ( !pIdx )
			continue;
		ServedDescRPtr_c rLocked ( pIdx );
		const CSphString &sIndex = it.GetName ();
		assert ( rLocked->m_pIndex );
		if ( rLocked->m_eType==IndexType_e::PLAIN && ROTATE_FROM::NEW==CheckIndexHeaderRotate ( *rLocked ) )
			g_pDisabledIndexes->AddUniq ( nullptr, sIndex );
	}

	/////////////////////
	// RAM-greedy rotate
	/////////////////////

	if ( !g_bSeamlessRotate )
	{
		ScRL_t tRotateConfigMutex { g_tRotateConfigMutex };
		for ( RLockedServedIt_c it ( g_pDisabledIndexes ); it.Next(); )
		{
			auto pIndex = it.Get();
			const char * sIndex = it.GetName ().cstr ();
			CSphString sError;

			// special processing for plain old rotation cur.new.* -> cur.*
			if ( !pIndex )
			{
				auto pRotating = GetServed ( sIndex );
				assert ( pRotating );
				ServedDescWPtr_c pWRotating ( pRotating );
				assert ( pWRotating->m_eType==IndexType_e::PLAIN );
				if ( !RotateIndexGreedy ( pRotating, *pWRotating, sIndex, sError ) )
				{
					sphWarning ( "%s", sError.cstr () );
					g_pLocalIndexes->Delete ( sIndex );
				}
				g_pDistIndexes->Delete ( sIndex ); // postponed delete of same-named distributed (if any)
				continue;
			}

			ServedDescWPtr_c pWlockedServedPtr ( pIndex );

			assert ( pWlockedServedPtr->m_pIndex );
			assert ( g_pLocalIndexes->Contains ( sIndex ) );

			// prealloc RT and percolate here
			if ( pWlockedServedPtr->IsMutable () )
			{
				pWlockedServedPtr->m_bOnlyNew = false;
				if ( PreallocNewIndex ( *pWlockedServedPtr, &g_pCfg.m_tConf["index"][sIndex], sIndex ) )
				{
					g_pLocalIndexes->AddOrReplace ( pIndex, sIndex );
					pIndex->AddRef ();
				} else
					g_pLocalIndexes->DeleteIfNull ( sIndex );
				g_pDistIndexes->Delete ( sIndex ); // postponed delete of same-named distributed (if any)
			}

			if ( pWlockedServedPtr->m_eType!=IndexType_e::PLAIN )
				continue;

			bool bWasAdded = pWlockedServedPtr->m_bOnlyNew;
			bool bOk = RotateIndexGreedy ( pIndex, *pWlockedServedPtr, sIndex, sError );
			if ( !bOk )
				sphWarning ( "%s", sError.cstr() );

			if ( bWasAdded && bOk && !sphFixupIndexSettings ( pWlockedServedPtr->m_pIndex, g_pCfg.m_tConf["index"][sIndex], sError, g_bStripPath ) )
			{
				sphWarning ( "index '%s': %s - NOT SERVING", sIndex, sError.cstr() );
				bOk = false;
			}

			if ( bOk )
			{
				pWlockedServedPtr->m_pIndex->Preread();
				g_pLocalIndexes->AddOrReplace ( pIndex, sIndex );
				pIndex->AddRef ();
			} else
				g_pLocalIndexes->DeleteIfNull ( sIndex );
			g_pDistIndexes->Delete ( sIndex ); // postponed delete of same-named distributed (if any)
		}

		g_pDisabledIndexes->ReleaseAndClear ();
		g_bInRotate = false;
		g_bInvokeRotationService = true;
		sphInfo ( "rotating finished" );
		return;
	}

	///////////////////
	// seamless rotate
	///////////////////

	// check what indexes need to be rotated
	SmallStringHash_T<bool> dNotCapableForRotation;
	for ( RLockedServedIt_c it ( g_pDisabledIndexes ); it.Next(); )
	{
		ServedDescRPtr_c rLocked ( it.Get() );
		const CSphString & sIndex = it.GetName ();
		if ( !rLocked )
			continue;

		assert ( rLocked->m_pIndex );
		if ( ROTATE_FROM::NONE==CheckIndexHeaderRotate ( *rLocked ) && !rLocked->IsMutable() )
		{
			dNotCapableForRotation.Add ( true, sIndex );
			sphLogDebug ( "Index %s (%s) is not capable for seamless rotate. Skipping", sIndex.cstr ()
						  , rLocked->m_sIndexPath.cstr () );
		}
	}

	if ( dNotCapableForRotation.GetLength () )
	{
		sphWarning ( "INTERNAL ERROR: non-empty queue on a rotation cycle start, got %d elements"
					 , dNotCapableForRotation.GetLength () );
		for ( dNotCapableForRotation.IterateStart (); dNotCapableForRotation.IterateNext (); )
		{
			g_pDisabledIndexes->Delete ( dNotCapableForRotation.IterateGetKey () );
			sphWarning ( "queue[] = %s", dNotCapableForRotation.IterateGetKey ().cstr () );
		}
	}

	if ( !g_pDisabledIndexes->GetLength () )
	{
		sphWarning ( "nothing to rotate after SIGHUP" );
		g_bInvokeRotationService = false;
		g_bInRotate = false;
		return;
	}
}


void CheckReopenLogs () REQUIRES ( MainThread )
{
	if ( !g_bGotSigusr1 )
		return;

	// reopen searchd log
	if ( g_iLogFile>=0 && !g_bLogTty )
	{
		int iFD = ::open ( g_sLogFile.cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
		if ( iFD<0 )
		{
			sphWarning ( "failed to reopen log file '%s': %s", g_sLogFile.cstr(), strerrorm(errno) );
		} else
		{
			::close ( g_iLogFile );
			g_iLogFile = iFD;
			g_bLogTty = ( isatty ( g_iLogFile )!=0 );
			LogChangeMode ( g_iLogFile, g_iLogFileMode );
			sphInfo ( "log reopened" );
		}
	}

	// reopen query log
	if ( !g_bQuerySyslog && g_iQueryLogFile!=g_iLogFile && g_iQueryLogFile>=0 && !isatty ( g_iQueryLogFile ) )
	{
		int iFD = ::open ( g_sQueryLogFile.cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
		if ( iFD<0 )
		{
			sphWarning ( "failed to reopen query log file '%s': %s", g_sQueryLogFile.cstr(), strerrorm(errno) );
		} else
		{
			::close ( g_iQueryLogFile );
			g_iQueryLogFile = iFD;
			LogChangeMode ( g_iQueryLogFile, g_iLogFileMode );
			sphInfo ( "query log reopened" );
		}
	}

	g_bGotSigusr1 = 0;
}


static void ThdSaveIndexes ( void * )
{
	ThreadSystem_t tThdSystemDesc ( "SAVE indexes" );
	SaveIndexes ();

	// we're no more flushing
	g_tFlush.m_bFlushing = false;
}

void CheckFlush () REQUIRES ( MainThread )
{
	if ( g_tFlush.m_bFlushing )
		return;

	// do a periodic check, unless we have a forced check
	if ( !g_tFlush.m_bForceCheck )
	{
		static int64_t tmLastCheck = -1000;
		int64_t tmNow = sphMicroTimer();

		if ( !g_iAttrFlushPeriod || ( tmLastCheck + int64_t(g_iAttrFlushPeriod)*I64C(1000000) )>=tmNow )
			return;

		tmLastCheck = tmNow;
		sphLogDebug ( "attrflush: doing periodic check" );
	} else
	{
		sphLogDebug ( "attrflush: doing forced check" );
	}

	// check if there are dirty indexes
	bool bDirty = false;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
	{
		auto pServed = it.Get();
		ServedDescRPtr_c pLocked ( pServed );
		if ( pServed && pLocked->m_pIndex->GetAttributeStatus() )
		{
			bDirty = true;
			break;
		}
	}

	// need to set this before clearing check flag
	if ( bDirty )
		g_tFlush.m_bFlushing = true;

	// if there was a forced check in progress, it no longer is
	if ( g_tFlush.m_bForceCheck )
		g_tFlush.m_bForceCheck = false;

	// nothing to do, no indexes were updated
	if ( !bDirty )
	{
		sphLogDebug ( "attrflush: no dirty indexes found" );
		return;
	}

	// launch the flush!
	g_tFlush.m_iFlushTag++;

	sphLogDebug ( "attrflush: starting writer, tag ( %d )", g_tFlush.m_iFlushTag );

	ThdDesc_t tThd;
	if ( !sphThreadCreate ( &tThd.m_tThd, ThdSaveIndexes, NULL, true, "SaveIndexes" ) )
		sphWarning ( "failed to create attribute save thread, error[%d] %s", errno, strerrorm(errno) );
}


#if !USE_WINDOWS
#define WINAPI
#else

SERVICE_STATUS			g_ss;
SERVICE_STATUS_HANDLE	g_ssHandle;


void MySetServiceStatus ( DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint )
{
	static DWORD dwCheckPoint = 1;

	if ( dwCurrentState==SERVICE_START_PENDING )
		g_ss.dwControlsAccepted = 0;
	else
		g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

	g_ss.dwCurrentState = dwCurrentState;
	g_ss.dwWin32ExitCode = dwWin32ExitCode;
	g_ss.dwWaitHint = dwWaitHint;

	if ( dwCurrentState==SERVICE_RUNNING || dwCurrentState==SERVICE_STOPPED )
		g_ss.dwCheckPoint = 0;
	else
		g_ss.dwCheckPoint = dwCheckPoint++;

	SetServiceStatus ( g_ssHandle, &g_ss );
}


void WINAPI ServiceControl ( DWORD dwControlCode )
{
	switch ( dwControlCode )
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			MySetServiceStatus ( SERVICE_STOP_PENDING, NO_ERROR, 0 );
			g_bServiceStop = true;
			break;

		default:
			MySetServiceStatus ( g_ss.dwCurrentState, NO_ERROR, 0 );
			break;
	}
}


// warning! static buffer, non-reentrable
const char * WinErrorInfo ()
{
	static char sBuf[1024];

	DWORD uErr = ::GetLastError ();
	snprintf ( sBuf, sizeof(sBuf), "code=%d, error=", uErr );

	int iLen = strlen(sBuf);
	if ( !FormatMessage ( FORMAT_MESSAGE_FROM_SYSTEM, NULL, uErr, 0, sBuf+iLen, sizeof(sBuf)-iLen, NULL ) ) // FIXME? force US-english langid?
		snprintf ( sBuf+iLen, sizeof(sBuf)-iLen, "(no message)" );

	return sBuf;
}


SC_HANDLE ServiceOpenManager ()
{
	SC_HANDLE hSCM = OpenSCManager (
		NULL,						// local computer
		NULL,						// ServicesActive database
		SC_MANAGER_ALL_ACCESS );	// full access rights

	if ( hSCM==NULL )
		sphFatal ( "OpenSCManager() failed: %s", WinErrorInfo() );

	return hSCM;
}


void AppendArg ( char * sBuf, int iBufLimit, const char * sArg )
{
	char * sBufMax = sBuf + iBufLimit - 2; // reserve place for opening space and trailing zero
	sBuf += strlen(sBuf);

	if ( sBuf>=sBufMax )
		return;

	int iArgLen = strlen(sArg);
	bool bQuote = false;
	for ( int i=0; i<iArgLen && !bQuote; i++ )
		if ( sArg[i]==' ' || sArg[i]=='"' )
			bQuote = true;

	*sBuf++ = ' ';
	if ( !bQuote )
	{
		// just copy
		int iToCopy = Min ( sBufMax-sBuf, iArgLen );
		memcpy ( sBuf, sArg, iToCopy );
		sBuf[iToCopy] = '\0';

	} else
	{
		// quote
		sBufMax -= 2; // reserve place for quotes
		if ( sBuf>=sBufMax )
			return;

		*sBuf++ = '"';
		while ( sBuf<sBufMax && *sArg )
		{
			if ( *sArg=='"' )
			{
				// quote
				if ( sBuf<sBufMax-1 )
				{
					*sBuf++ = '\\';
					*sBuf++ = *sArg++;
				}
			} else
			{
				// copy
				*sBuf++ = *sArg++;
			}
		}
		*sBuf++ = '"';
		*sBuf++ = '\0';
	}
}


void ServiceInstall ( int argc, char ** argv )
{
	if ( g_bService )
		return;

	sphInfo ( "Installing service..." );

	char szBinary[MAX_PATH];
	if ( !GetModuleFileName ( NULL, szBinary, MAX_PATH ) )
		sphFatal ( "GetModuleFileName() failed: %s", WinErrorInfo() );

	char szPath[MAX_PATH];
	szPath[0] = '\0';

	AppendArg ( szPath, sizeof(szPath), szBinary );
	AppendArg ( szPath, sizeof(szPath), "--ntservice" );
	for ( int i=1; i<argc; i++ )
		if ( strcmp ( argv[i], "--install" ) )
			AppendArg ( szPath, sizeof(szPath), argv[i] );

	SC_HANDLE hSCM = ServiceOpenManager ();
	SC_HANDLE hService = CreateService (
		hSCM,							// SCM database
		g_sServiceName,					// name of service
		g_sServiceName,					// service name to display
		SERVICE_ALL_ACCESS,				// desired access
		SERVICE_WIN32_OWN_PROCESS,		// service type
		SERVICE_AUTO_START,				// start type
		SERVICE_ERROR_NORMAL,			// error control type
		szPath+1,						// path to service's binary
		NULL,							// no load ordering group
		NULL,							// no tag identifier
		NULL,							// no dependencies
		NULL,							// LocalSystem account
		NULL );							// no password

	if ( !hService )
	{
		CloseServiceHandle ( hSCM );
		sphFatal ( "CreateService() failed: %s", WinErrorInfo() );

	} else
	{
		sphInfo ( "Service '%s' installed successfully.", g_sServiceName );
	}

	CSphString sDesc;
	sDesc.SetSprintf ( "%s-%s", g_sServiceName, SPHINX_VERSION );

	SERVICE_DESCRIPTION tDesc;
	tDesc.lpDescription = (LPSTR) sDesc.cstr();
	if ( !ChangeServiceConfig2 ( hService, SERVICE_CONFIG_DESCRIPTION, &tDesc ) )
		sphWarning ( "failed to set service description" );

	CloseServiceHandle ( hService );
	CloseServiceHandle ( hSCM );
}


void ServiceDelete ()
{
	if ( g_bService )
		return;

	sphInfo ( "Deleting service..." );

	// open manager
	SC_HANDLE hSCM = ServiceOpenManager ();

	// open service
	SC_HANDLE hService = OpenService ( hSCM, g_sServiceName, DELETE );
	if ( !hService )
	{
		CloseServiceHandle ( hSCM );
		sphFatal ( "OpenService() failed: %s", WinErrorInfo() );
	}

	// do delete
	bool bRes = !!DeleteService ( hService );
	CloseServiceHandle ( hService );
	CloseServiceHandle ( hSCM );

	if ( !bRes )
		sphFatal ( "DeleteService() failed: %s", WinErrorInfo() );
	else
		sphInfo ( "Service '%s' deleted successfully.", g_sServiceName );
}
#endif // USE_WINDOWS


void ShowHelp ()
{
	fprintf ( stdout,
		"Usage: searchd [OPTIONS]\n"
		"\n"
		"Options are:\n"
		"-h, --help\t\tdisplay this help message\n"
		"-v\t\t\tdisplay version information\n"
		"-c, --config <file>\tread configuration from specified file\n"
		"\t\t\t(default is sphinx.conf)\n"
		"--stop\t\t\tsend SIGTERM to currently running searchd\n"
		"--stopwait\t\tsend SIGTERM and wait until actual exit\n"
		"--status\t\tget ant print status variables\n"
		"\t\t\t(PID is taken from pid_file specified in config file)\n"
		"--iostats\t\tlog per-query io stats\n"
		"--cpustats\t\tlog per-query cpu stats\n"
#if USE_WINDOWS
		"--install\t\tinstall as Windows service\n"
		"--delete\t\tdelete Windows service\n"
		"--servicename <name>\tuse given service name (default is 'searchd')\n"
		"--ntservice\t\tinternal option used to invoke a Windows service\n"
#endif
		"--strip-path\t\tstrip paths from stopwords, wordforms, exceptions\n"
		"\t\t\tand other file names stored in the index header\n"
		"--replay-flags=<OPTIONS>\n"
		"\t\t\textra binary log replay options (current options \n"
		"\t\t\tare 'accept-desc-timestamp' and 'ignore-open-errors')\n"
		"\n"
		"Debugging options are:\n"
		"--console\t\trun in console mode (do not fork, do not log to files)\n"
		"-p, --port <port>\tlisten on given port (overrides config setting)\n"
		"-l, --listen <spec>\tlisten on given address, port or path (overrides\n"
		"\t\t\tconfig settings)\n"
		"-i, --index <index>\tonly serve given index(es)\n"
#if !USE_WINDOWS
		"--nodetach\t\tdo not detach into background\n"
#endif
		"--logdebug, --logdebugv, --logdebugvv\n"
		"\t\t\tenable additional debug information logging\n"
		"\t\t\t(with different verboseness)\n"
		"--pidfile\t\tforce using the PID file (useful with --console)\n"
		"--safetrace\t\tonly use system backtrace() call in crash reports\n"
		"--coredump\t\tsave core dump file on crash\n"
		"\n"
		"Examples:\n"
		"searchd --config /usr/local/sphinx/etc/sphinx.conf\n"
#if USE_WINDOWS
		"searchd --install --config c:\\sphinx\\sphinx.conf\n"
#endif
		);
}


void InitSharedBuffer ( CSphLargeBuffer<DWORD, true> & tBuffer )
{
	CSphString sError, sWarning;
	if ( !tBuffer.Alloc ( 1, sError ) )
		sphDie ( "failed to allocate shared buffer (msg=%s)", sError.cstr() );

	DWORD * pRes = tBuffer.GetWritePtr();
	memset ( pRes, 0, sizeof(DWORD) ); // reset
}


#if USE_WINDOWS
BOOL WINAPI CtrlHandler ( DWORD )
{
	if ( !g_bService )
	{
		g_bGotSigterm = 1;
		sphInterruptNow();
	}
	return TRUE;
}
#endif

static char g_sNameBuf[512] = { 0 };
static char g_sPid[30] = { 0 };


#if !USE_WINDOWS
// returns 'true' only once - at the very start, to show it beatiful way.
bool SetWatchDog ( int iDevNull ) REQUIRES ( MainThread )
{
	InitSharedBuffer ( g_bDaemonAtShutdown );
	InitSharedBuffer ( g_bHaveTTY );

	// Fork #1 - detach from controlling terminal
	switch ( fork() )
	{
		case -1:
			// error
			sphFatalLog ( "fork() failed (reason: %s)", strerrorm ( errno ) );
			exit ( 1 );
		case 0:
			// daemonized child - or new and free watchdog :)
			break;

		default:
			// tty-controlled parent
			while ( g_bHaveTTY[0]==0 )
				sphSleepMsec ( 100 );

			exit ( 0 );
	}

	// became the session leader
	if ( setsid()==-1 )
	{
		sphFatalLog ( "setsid() failed (reason: %s)", strerrorm ( errno ) );
		exit ( 1 );
	}

	// Fork #2 - detach from session leadership (may be not necessary, however)
	switch ( fork() )
	{
		case -1:
			// error
			sphFatalLog ( "fork() failed (reason: %s)", strerrorm ( errno ) );
			exit ( 1 );
		case 0:
			// daemonized child - or new and free watchdog :)
			break;

		default:
			// tty-controlled parent
			exit ( 0 );
	}

	// save path to our binary
	g_sNameBuf[::readlink ( "/proc/self/exe", g_sNameBuf, 511 )] = 0;

	// now we are the watchdog. Let us fork the actual process
	enum class EFork { Startup, Disabled, Restart } eReincarnate = EFork::Startup;
	bool bShutdown = false;
	bool bStreamsActive = true;
	int iChild = 0;
	g_iParentPID = getpid();
	while (true)
	{
		if ( eReincarnate!=EFork::Disabled )
			iChild = fork();

		if ( iChild==-1 )
		{
			sphFatalLog ( "fork() failed during watchdog setup (error=%s)", strerrorm(errno) );
			exit ( 1 );
		}

		// child process; return true to show that we have to reload everything
		if ( iChild==0 )
		{
			atexit ( &ReleaseTTYFlag );
			return bStreamsActive;
		}

		// parent process, watchdog
		// close the io files
		if ( bStreamsActive )
		{
			close ( STDIN_FILENO );
			close ( STDOUT_FILENO );
			close ( STDERR_FILENO );
			dup2 ( iDevNull, STDIN_FILENO );
			dup2 ( iDevNull, STDOUT_FILENO );
			dup2 ( iDevNull, STDERR_FILENO );
			bStreamsActive = false;
		}

		if ( eReincarnate!=EFork::Disabled )
		{
			sphInfo ( "watchdog: main process %d forked ok", iChild );
			sprintf ( g_sPid, "%d", iChild);
		}

		SetSignalHandlers();

		eReincarnate = EFork::Disabled;
		int iPid, iStatus;
		bool bDaemonAtShutdown = 0;
		while ( ( iPid = wait ( &iStatus ) )>0 )
		{
			bDaemonAtShutdown = ( g_bDaemonAtShutdown[0]!=0 );
			const char * sWillRestart = ( bDaemonAtShutdown ? "will not be restarted (daemon is shutting down)" : "will be restarted" );

			assert ( iPid==iChild );
			if ( WIFEXITED ( iStatus ) )
			{
				int iExit = WEXITSTATUS ( iStatus );
				if ( iExit==2 || iExit==6 ) // really crash
				{
					sphInfo ( "watchdog: main process %d crashed via CRASH_EXIT (exit code %d), %s", iPid, iExit, sWillRestart );
					eReincarnate = EFork::Restart;
				} else
				{
					sphInfo ( "watchdog: main process %d exited cleanly (exit code %d), shutting down", iPid, iExit );
					bShutdown = true;
				}
			} else if ( WIFSIGNALED ( iStatus ) )
			{
				int iSig = WTERMSIG ( iStatus );
				const char * sSig = NULL;
				if ( iSig==SIGINT )
					sSig = "SIGINIT";
				else if ( iSig==SIGTERM )
					sSig = "SIGTERM";
				else if ( WATCHDOG_SIGKILL && iSig==SIGKILL )
					sSig = "SIGKILL";
				if ( sSig )
				{
					sphInfo ( "watchdog: main process %d killed cleanly with %s, shutting down", iPid, sSig );
					bShutdown = true;
				} else
				{
					if ( WCOREDUMP ( iStatus ) )
						sphInfo ( "watchdog: main process %d killed dirtily with signal %d, core dumped, %s",
							iPid, iSig, sWillRestart );
					else
						sphInfo ( "watchdog: main process %d killed dirtily with signal %d, %s",
							iPid, iSig, sWillRestart );
					eReincarnate = EFork::Restart;
				}
			} else if ( WIFSTOPPED ( iStatus ) )
				sphInfo ( "watchdog: main process %d stopped with signal %d", iPid, WSTOPSIG ( iStatus ) );
#ifdef WIFCONTINUED
			else if ( WIFCONTINUED ( iStatus ) )
				sphInfo ( "watchdog: main process %d resumed", iPid );
#endif
		}

		if ( iPid==-1 )
		{
			if ( g_bGotSigusr1 )
			{
				g_bGotSigusr1 = 0;
				sphInfo ( "watchdog: got USR1, performing dump of child's stack" );
				sphDumpGdb ( g_iLogFile, g_sNameBuf, g_sPid );
			} else
				sphInfo ( "watchdog: got error %d, %s", errno, strerrorm ( errno ));
		}

		if ( bShutdown || g_bGotSigterm || bDaemonAtShutdown )
		{
			exit ( 0 );
		}
	}
}
#endif // !USE_WINDOWS

/// check for incoming signals, and react on them
void CheckSignals () REQUIRES ( MainThread )
{
#if USE_WINDOWS
	if ( g_bService && g_bServiceStop )
	{
		Shutdown ();
		MySetServiceStatus ( SERVICE_STOPPED, NO_ERROR, 0 );
		exit ( 0 );
	}
#endif

	if ( g_bGotSighup )
	{
		sphInfo ( "caught SIGHUP (seamless=%d, in_rotate=%d, need_rotate=%d)", (int)g_bSeamlessRotate, (int)g_bInRotate, (int)g_bNeedRotate );
		g_bNeedRotate = true;
		g_bGotSighup = 0;
	}

	if ( g_bGotSigterm )
	{
		sphInfo ( "caught SIGTERM, shutting down" );
		Shutdown ();
		exit ( 0 );
	}

#if USE_WINDOWS
	BYTE dPipeInBuf [ WIN32_PIPE_BUFSIZE ];
	DWORD nBytesRead = 0;
	BOOL bSuccess = ReadFile ( g_hPipe, dPipeInBuf, WIN32_PIPE_BUFSIZE, &nBytesRead, NULL );
	if ( nBytesRead > 0 && bSuccess )
	{
		for ( DWORD i=0; i<nBytesRead; i++ )
		{
			switch ( dPipeInBuf[i] )
			{
			case 0:
				g_bGotSighup = 1;
				break;

			case 1:
				g_bGotSigterm = 1;
				sphInterruptNow();
				if ( g_bService )
					g_bServiceStop = true;
				break;
			}
		}

		DisconnectNamedPipe ( g_hPipe );
		ConnectNamedPipe ( g_hPipe, NULL );
	}
#endif
}


void QueryStatus ( CSphVariant * v )
{
	char sBuf [ SPH_ADDRESS_SIZE ];
	char sListen [ 256 ];
	CSphVariant tListen;

	if ( !v )
	{
		snprintf ( sListen, sizeof ( sListen ), "127.0.0.1:%d:sphinx", SPHINXAPI_PORT );
		tListen = CSphVariant ( sListen, 0 );
		v = &tListen;
	}

	for ( ; v; v = v->m_pNext )
	{
		ListenerDesc_t tDesc = ParseListener ( v->cstr() );
		if ( tDesc.m_eProto!=PROTO_SPHINX )
			continue;

		int iSock = -1;
#if !USE_WINDOWS
		if ( !tDesc.m_sUnix.IsEmpty() )
		{
			// UNIX connection
			struct sockaddr_un uaddr;

			size_t len = strlen ( tDesc.m_sUnix.cstr() );
			if ( len+1 > sizeof(uaddr.sun_path ) )
				sphFatal ( "UNIX socket path is too long (len=%d)", (int)len );

			memset ( &uaddr, 0, sizeof(uaddr) );
			uaddr.sun_family = AF_UNIX;
			memcpy ( uaddr.sun_path, tDesc.m_sUnix.cstr(), len+1 );

			iSock = socket ( AF_UNIX, SOCK_STREAM, 0 );
			if ( iSock<0 )
				sphFatal ( "failed to create UNIX socket: %s", sphSockError() );

			if ( connect ( iSock, (struct sockaddr*)&uaddr, sizeof(uaddr) )<0 )
			{
				sphWarning ( "failed to connect to unix://%s: %s\n", tDesc.m_sUnix.cstr(), sphSockError() );
				sphSockClose ( iSock );
				continue;
			}

		} else
#endif
		{
			// TCP connection
			struct sockaddr_in sin;
			memset ( &sin, 0, sizeof(sin) );
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = ( tDesc.m_uIP==htonl ( INADDR_ANY ) )
				? htonl ( INADDR_LOOPBACK )
				: tDesc.m_uIP;
			sin.sin_port = htons ( (short)tDesc.m_iPort );

			iSock = socket ( AF_INET, SOCK_STREAM, 0 );
			if ( iSock<0 )
				sphFatal ( "failed to create TCP socket: %s", sphSockError() );

#ifdef TCP_NODELAY
			int iOn = 1;
			if ( setsockopt ( iSock, IPPROTO_TCP, TCP_NODELAY, (char*)&iOn, sizeof(iOn) ) )
				sphWarning ( "setsockopt() failed: %s", sphSockError() );
#endif

			if ( connect ( iSock, (struct sockaddr*)&sin, sizeof(sin) )<0 )
			{
				sphWarning ( "failed to connect to %s:%d: %s\n", sphFormatIP ( sBuf, sizeof(sBuf), tDesc.m_uIP ), tDesc.m_iPort, sphSockError() );
				sphSockClose ( iSock );
				continue;
			}
		}

		// send request
		NetOutputBuffer_c tOut ( iSock );
		tOut.SendDword ( SPHINX_CLIENT_VERSION );
		APICommand_t dStatus ( tOut, SEARCHD_COMMAND_STATUS, VER_COMMAND_STATUS );
		tOut.SendInt ( 1 ); // dummy body
		tOut.Flush ();

		// get reply
		NetInputBuffer_c tIn ( iSock );
		if ( !tIn.ReadFrom ( 12, 5 ) ) // magic_header_size=12, magic_timeout=5
			sphFatal ( "handshake failure (no response)" );

		DWORD uVer = tIn.GetDword();
		if ( uVer!=SPHINX_SEARCHD_PROTO && uVer!=0x01000000UL ) // workaround for all the revisions that sent it in host order...
			sphFatal ( "handshake failure (unexpected protocol version=%d)", uVer );

		if ( tIn.GetWord()!=SEARCHD_OK )
			sphFatal ( "status command failed" );

		if ( tIn.GetWord()!=VER_COMMAND_STATUS )
			sphFatal ( "status command version mismatch" );

		if ( !tIn.ReadFrom ( tIn.GetDword(), 5 ) ) // magic_timeout=5
			sphFatal ( "failed to read status reply" );

		fprintf ( stdout, "\nsearchd status\n--------------\n" );

		int iRows = tIn.GetDword();
		int iCols = tIn.GetDword();
		for ( int i=0; i<iRows && !tIn.GetError(); i++ )
		{
			for ( int j=0; j<iCols && !tIn.GetError(); j++ )
			{
				fprintf ( stdout, "%s", tIn.GetString().cstr() );
				fprintf ( stdout, ( j==0 ) ? ": " : " " );
			}
			fprintf ( stdout, "\n" );
		}

		// all done
		sphSockClose ( iSock );
		return;
	}
	sphFatal ( "failed to connect to daemon: please specify listen with sphinx protocol in your config file" );
}


void ShowProgress ( const CSphIndexProgress * pProgress, bool bPhaseEnd )
{
	assert ( pProgress );
	if ( bPhaseEnd )
	{
		fprintf ( stdout, "\r                                                            \r" );
	} else
	{
		fprintf ( stdout, "%s\r", pProgress->BuildMessage() );
	}
	fflush ( stdout );
}


void FailClient ( int iSock, const char * sMessage )
{
	NetOutputBuffer_c tOut ( iSock );
	tOut.SendInt ( SPHINX_CLIENT_VERSION );
	APICommand_t tHeader ( tOut, SEARCHD_RETRY );
	tOut.SendString ( sMessage );
	tOut.Flush ();

	// FIXME? without some wait, client fails to receive the response on windows
	sphSockClose ( iSock );
}

static const char * g_dSphinxQLMaxedOutPacket = "\x17\x00\x00\x00\xff\x10\x04Too many connections";
static const int g_iSphinxQLMaxedOutLen = 27;


void MysqlMaxedOut ( int iSock )
{
	if ( sphSockSend ( iSock, g_dSphinxQLMaxedOutPacket, g_iSphinxQLMaxedOutLen ) <0 )
	{
		int iErrno = sphSockGetErrno ();
		sphWarning ( "send() failed: %d: %s", iErrno, sphSockError ( iErrno ) );
	}
	sphSockClose ( iSock );
}


Listener_t * DoAccept ( int * pClientSock, char * sClientName ) REQUIRES ( MainThread )
{
	assert ( pClientSock );
	assert ( *pClientSock==-1 );

	int iMaxFD = 0;
	fd_set fdsAccept;
	FD_ZERO ( &fdsAccept );

	ARRAY_FOREACH ( i, g_dListeners )
	{
		sphFDSet ( g_dListeners[i].m_iSock, &fdsAccept );
		iMaxFD = Max ( iMaxFD, g_dListeners[i].m_iSock );
	}
	iMaxFD++;

	struct timeval tvTimeout;
	tvTimeout.tv_sec = USE_WINDOWS ? 0 : 1;
	tvTimeout.tv_usec = USE_WINDOWS ? 50000 : 0;

	// select should be OK here as listener sockets are created early and get low FDs
	int iRes = ::select ( iMaxFD, &fdsAccept, NULL, NULL, &tvTimeout );
	if ( iRes==0 )
		return NULL;

	if ( iRes<0 )
	{
		int iErrno = sphSockGetErrno();
		if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			return NULL;

		static int iLastErrno = -1;
		if ( iLastErrno!=iErrno )
			sphWarning ( "select() failed: %s", sphSockError(iErrno) );
		iLastErrno = iErrno;
		return NULL;
	}

	ARRAY_FOREACH ( i, g_dListeners )
	{
		if ( !FD_ISSET ( g_dListeners[i].m_iSock, &fdsAccept ) )
			continue;

		// accept
		struct sockaddr_storage saStorage;
		socklen_t uLength = sizeof(saStorage);
		int iClientSock = accept ( g_dListeners[i].m_iSock, (struct sockaddr *)&saStorage, &uLength );

		// handle failures
		if ( iClientSock<0 )
		{
			const int iErrno = sphSockGetErrno();
			if ( iErrno==EINTR || iErrno==ECONNABORTED || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
				return NULL;

			sphFatal ( "accept() failed: %s", sphSockError(iErrno) );
		}

#ifdef TCP_NODELAY
		int iOn = 1;
		if ( g_dListeners[i].m_bTcp && setsockopt ( iClientSock, IPPROTO_TCP, TCP_NODELAY, (char*)&iOn, sizeof(iOn) ) )
			sphWarning ( "setsockopt() failed: %s", sphSockError() );
#endif
		g_tStats.m_iConnections.Inc();

		if ( ++g_iConnectionID<0 )
			g_iConnectionID = 0;

		// format client address
		if ( sClientName )
		{
			sClientName[0] = '\0';
			if ( saStorage.ss_family==AF_INET )
			{
				struct sockaddr_in * pSa = ((struct sockaddr_in *)&saStorage);
				sphFormatIP ( sClientName, SPH_ADDRESS_SIZE, pSa->sin_addr.s_addr );

				char * d = sClientName;
				while ( *d )
					d++;
				snprintf ( d, 7, ":%d", (int)ntohs ( pSa->sin_port ) ); //NOLINT
			}
			if ( saStorage.ss_family==AF_UNIX )
				strncpy ( sClientName, "(local)", SPH_ADDRESS_SIZE );
		}

		// accepted!
#if !USE_WINDOWS && !HAVE_POLL
		// when there is no poll(), we use select(),
		// which can only handle a limited range of fds..
		if ( SPH_FDSET_OVERFLOW ( iClientSock ) )
		{
			// otherwise, we fail this client (we have to)
			FailClient ( iClientSock, SEARCHD_RETRY, "server maxed out, retry in a second" );
			sphWarning ( "maxed out, dismissing client (socket=%d)", iClientSock );
			sphSockClose ( iClientSock );
			return NULL;
		}
#endif

		*pClientSock = iClientSock;
		return &g_dListeners[i];
	}

	return NULL;
}


int GetOsThreadId()
{
#if USE_WINDOWS
	return GetCurrentThreadId();
#elif defined ( __APPLE__ )
	uint64_t tid;
 	pthread_threadid_np(NULL, &tid);
	return tid;
#elif defined(SYS_gettid)
	return syscall ( SYS_gettid );
#elif defined(__FreeBSD__)
	long tid;
	thr_self(&tid);
	return (int)tid;
#else
	return 0;
#endif
}

void HandlerThreadFunc ( void * pArg ) REQUIRES ( !HandlerThread )
{
	ScopedRole_c tHandler ( HandlerThread );
	// handle that client
	ThdDesc_t * pThd = (ThdDesc_t*) pArg;
	pThd->m_iTid = GetOsThreadId();
	HandleClient ( pThd->m_eProto, pThd->m_iClientSock, *pThd );
	sphSockClose ( pThd->m_iClientSock );

	// done; remove myself from the table
#if USE_WINDOWS
	// FIXME? this is sort of automatic on UNIX (pthread_exit() gets implicitly called on return)
	CloseHandle ( pThd->m_tThd );
#endif
	ThreadRemove ( pThd );
	SafeDelete ( pThd );
}


void TickHead () REQUIRES ( MainThread )
{
	CheckSignals ();
	CheckLeaks ();
	CheckReopenLogs ();
	CheckRotate ();
	CheckFlush ();

	if ( g_pThdPool )
	{
		sphInfo ( nullptr ); // flush dupes
#if USE_WINDOWS
		// at windows there is no signals that interrupt sleep
		// need to sleep less to make main loop more responsible
		int tmSleep = 100;
#else
		int tmSleep = 500;
#endif
		sphSleepMsec ( tmSleep );
		return;
	}

	int iClientSock = -1;
	char sClientName[SPH_ADDRPORT_SIZE];
	Listener_t * pListener = DoAccept ( &iClientSock, sClientName );
	if ( !pListener )
		return;

	if ( ( g_iMaxChildren && !g_pThdPool && ThreadsNum()>=g_iMaxChildren )
		|| ( g_bInRotate && !g_bSeamlessRotate ) )
	{
		if ( pListener->m_eProto==PROTO_SPHINX )
			FailClient ( iClientSock, "server maxed out, retry in a second" );
		else
			MysqlMaxedOut ( iClientSock );
		sphWarning ( "maxed out, dismissing client" );

		g_tStats.m_iMaxedOut.Inc();
		return;
	}

	auto * pThd = new ThdDesc_t ();
	pThd->m_eProto = pListener->m_eProto;
	pThd->m_iClientSock = iClientSock;
	pThd->m_sClientName = sClientName;
	pThd->m_iConnID = g_iConnectionID;
	pThd->m_tmConnect = sphMicroTimer();
	pThd->m_bVip = pListener->m_bVIP;

	ThreadAdd ( pThd );

	if ( !SphCrashLogger_c::ThreadCreate ( &pThd->m_tThd, HandlerThreadFunc, pThd, true, "handler" ) )
	{
		int iErr = errno;
		ThreadRemove ( pThd );
		SafeDelete ( pThd );

		FailClient ( iClientSock, "failed to create worker thread" );
		sphWarning ( "failed to create worker thread, threads(%d), error[%d] %s", ThreadsNum(), iErr, strerrorm(iErr) );
	}
}


/////////////////////////////////////////////////////////////////////////////
// NETWORK THREAD
/////////////////////////////////////////////////////////////////////////////

enum NetEvent_e
{
	NE_KEEP = 0,
	NE_IN = 1UL<<0,
	NE_OUT = 1UL<<1,
	NE_HUP = 1UL<<2,
	NE_ERR = 1UL<<3,
	NE_REMOVE = 1UL<<4,
	NE_REMOVED = 1UL<<5,
};

struct NetStateCommon_t;
class CSphNetLoop;
struct ISphNetAction : ISphNoncopyable
{
	int64_t				m_tmTimeout;
	const int			m_iSock;

	explicit ISphNetAction ( int iSock ) : m_tmTimeout ( 0 ), m_iSock ( iSock ) {}
	virtual ~ISphNetAction () {}
	virtual NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) = 0;
	virtual NetEvent_e		Setup ( int64_t tmNow ) = 0;
	virtual bool			GetStats ( int & ) { return false; }
	virtual void			CloseSocket () = 0;
};

struct NetStateCommon_t
{
	int					m_iClientSock = -1;
	int					m_iConnID = 0;
	char				m_sClientName[SPH_ADDRPORT_SIZE];
	bool				m_bKeepSocket = false;
	bool				m_bVIP = false;

	CSphVector<BYTE>	m_dBuf;
	int					m_iLeft = 0;
	int					m_iPos = 0;

	NetStateCommon_t ();
	virtual ~NetStateCommon_t ();

	int NetManageSocket ( bool bWrite, bool bAfterWrite = false );
	void CloseSocket ();
};

struct NetActionAccept_t : public ISphNetAction
{
	Listener_t			m_tListener;
	int					m_iConnections;
	NetStateCommon_t	m_tDummy;

	explicit NetActionAccept_t ( const Listener_t & tListener );

	NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) final;
	NetEvent_e		Setup ( int64_t tmNow ) final;
	bool			GetStats ( int & iConnections ) final;
	void			CloseSocket () final {}

	void			FillNetState ( NetStateCommon_t * pState, int iClientSock, int iConnID, bool bVIP, const sockaddr_storage & saStorage ) const;
};

// just new typedef for API state
using NetStateAPI_t = NetStateCommon_t;

struct NetStateQL_t : public NetStateCommon_t
{
	CSphinxqlSession	m_tSession { true };
	bool				m_bAuthed = false;
	BYTE				m_uPacketID = 1;
};

enum ActionAPI_e
{
	AAPI_HANDSHAKE_OUT = 0,
	AAPI_HANDSHAKE_IN,
	AAPI_COMMAND,
	AAPI_BODY,
	AAPI_TOTAL
};

struct NetReceiveDataAPI_t : public ISphNetAction
{
	CSphScopedPtr<NetStateAPI_t>	m_tState;

	ActionAPI_e			m_ePhase;

	SearchdCommand_e	m_eCommand = SEARCHD_COMMAND_WRONG;
	WORD				m_uCommandVer = VER_COMMAND_WRONG;

	explicit NetReceiveDataAPI_t ( NetStateAPI_t * pState );

	NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) override;
	NetEvent_e		Setup ( int64_t tmNow ) override;
	void			CloseSocket () override;

	void				SetupBodyPhase();
	void				AddJobAPI ( CSphNetLoop * pLoop );
};

enum ActionQL_e
{
	AQL_HANDSHAKE = 0,
	AQL_LENGTH,
	AQL_BODY,
	AQL_AUTH,
	AQL_TOTAL
};

struct NetReceiveDataQL_t : public ISphNetAction
{
	CSphScopedPtr<NetStateQL_t>		m_tState;

	ActionQL_e			m_ePhase;

	bool				m_bAppend;
	bool				m_bWrite;

	explicit NetReceiveDataQL_t ( NetStateQL_t * pState );

	NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) override;
	NetEvent_e		Setup ( int64_t tmNow ) override;
	void			CloseSocket () override;

	void				SetupHandshakePhase();
	void				SetupBodyPhase();
};

struct NetSendData_t : public ISphNetAction
{
	CSphScopedPtr<NetStateCommon_t>		m_tState;
	ProtocolType_e						m_eProto;
	bool								m_bContinue;

	NetSendData_t ( NetStateCommon_t * pState, ProtocolType_e eProto );

	NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) override;
	NetEvent_e		Setup ( int64_t tmNow ) override;
	void			CloseSocket () override;

	void SetContinue () { m_bContinue = true; }
};

struct HttpHeaderStreamParser_t
{
	int m_iHeaderEnd;
	int m_iFieldContentLenStart;
	int m_iFieldContentLenVal;

	int m_iCur;
	int m_iCRLF;
	int m_iName;

	HttpHeaderStreamParser_t ();
	bool HeaderFound ( const BYTE * pBuf, int iLen );
};

struct NetReceiveDataHttp_t : public ISphNetAction
{
	CSphScopedPtr<NetStateQL_t>		m_tState;
	HttpHeaderStreamParser_t		m_tHeadParser;

	explicit NetReceiveDataHttp_t ( NetStateQL_t * pState );

	NetEvent_e		Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop ) override;
	NetEvent_e		Setup ( int64_t tmNow ) override;
	void			CloseSocket () override;
};

struct EventsIterator_t
{
	ISphNetAction * m_pWork = nullptr;
	DWORD			m_uEvents = 0;
};


class NetActionsPoller
{
private:
	EventsIterator_t	m_tIter;
	ISphNetEvents *		m_pPoll;

	inline DWORD TranslateEvents ( DWORD uNetEvents )
	{
		DWORD uEvents = 0;
		if ( uNetEvents & ISphNetEvents::SPH_POLL_RD )
			uEvents |= NE_IN;
		if ( uNetEvents & ISphNetEvents::SPH_POLL_WR )
			uEvents |= NE_OUT;
		if ( uNetEvents & ISphNetEvents::SPH_POLL_HUP )
			uEvents |= NE_HUP;
		if ( uNetEvents & ISphNetEvents::SPH_POLL_ERR )
			uEvents |= NE_ERR;
		return uEvents;
	}
public:
	NetActionsPoller ()
	{
		m_pPoll = sphCreatePoll ( 1000 );
	}

	~NetActionsPoller()
	{
		if ( m_pPoll )
		{
			m_pPoll->IterateStart ();
			while ( m_pPoll->IterateNextAll() )
			{
				auto * pWork = (ISphNetAction *)m_pPoll->IterateGet().m_pData;
				SafeDelete ( pWork );
			}
		}
		SafeDelete ( m_pPoll );
	}

	void SetupEvent ( ISphNetAction * pWork, int64_t tmNow )
	{
		assert ( m_pPoll );
		assert ( pWork && pWork->m_iSock>=0 );

		NetEvent_e eSetup = pWork->Setup ( tmNow );
		assert ( eSetup==NE_IN || eSetup==NE_OUT );

		m_pPoll->SetupEvent ( pWork->m_iSock, 
			( eSetup==NE_IN ? ISphNetEvents::SPH_POLL_RD : ISphNetEvents::SPH_POLL_WR ), pWork );
	}

	bool Wait ( int timeoutMs )
	{
		assert ( m_pPoll );
		return m_pPoll->Wait ( timeoutMs );
	}

	bool IterateNextAll ()
	{
		assert ( m_pPoll );
		m_tIter.m_pWork = nullptr;
		m_tIter.m_uEvents = 0;
		bool bRes = m_pPoll->IterateNextAll();
		if ( bRes )
		{
			const NetEventsIterator_t & tBackendIterator = m_pPoll->IterateGet();
			m_tIter.m_pWork = (ISphNetAction *)tBackendIterator.m_pData;
			m_tIter.m_uEvents = TranslateEvents ( tBackendIterator.m_uEvents );
		}
		return bRes;
	}

	bool IterateNextReady ()
	{
		assert ( m_pPoll );
		m_tIter.m_pWork = nullptr;
		m_tIter.m_uEvents = 0;
		bool bRes = m_pPoll->IterateNextReady();
		if ( bRes )
		{
			const NetEventsIterator_t & tBackendIterator = m_pPoll->IterateGet();
			m_tIter.m_pWork = (ISphNetAction *)tBackendIterator.m_pData;
			m_tIter.m_uEvents = TranslateEvents ( tBackendIterator.m_uEvents );
		}
		return bRes;
	}

	void IterateChangeEvent ( NetEvent_e eEv, ISphNetAction * pAction)
	{
		assert ( m_pPoll );		
		m_pPoll->IterateChangeEvent ( pAction->m_iSock, ( eEv==NE_IN ? ISphNetEvents::SPH_POLL_RD : ISphNetEvents::SPH_POLL_WR ) );
	}

	void IterateRemove ()
	{
		assert ( m_pPoll );
		const NetEventsIterator_t & tBackendIterator = m_pPoll->IterateGet();
		auto * pAction = (ISphNetAction *)tBackendIterator.m_pData;

		m_pPoll->IterateRemove ( pAction->m_iSock );
		m_tIter.m_pWork = nullptr;
	}

	int IterateStart ()
	{
		assert ( m_pPoll );

		m_tIter.m_pWork = nullptr;
		m_tIter.m_uEvents = 0;

		return m_pPoll->IterateStart();
	}

	EventsIterator_t & IterateGet ()
	{
		return m_tIter;
	}
};

// event that wakes-up poll net loop from finished thread pool job
class CSphWakeupEvent : public PollableEvent_t, public ISphNetAction
{
public:
	CSphWakeupEvent ()
		: PollableEvent_t()
		, ISphNetAction ( m_iPollablefd )
	{
	}

	~CSphWakeupEvent () final
	{
		CloseSocket();
	}

	void Wakeup ()
	{
		if ( FireEvent () )
			return;
		int iErrno = PollableErrno ();
		sphLogDebugv ( "failed to wakeup net thread ( error %d,'%s')", iErrno, strerrorm ( iErrno ) );
	}

	NetEvent_e Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> &, CSphNetLoop * ) final
	{
		if ( uGotEvents & NE_IN )
			DisposeEvent();
		return NE_KEEP;
	}

	NetEvent_e Setup ( int64_t ) final
	{
		sphLogDebugv ( "%p wakeup setup, read=%d, write=%d", this, m_iPollablefd, m_iSignalEvent );
		return NE_IN;
	}

	void CloseSocket () final
	{
		Close();
		*const_cast<int *>( &m_iSock ) = -1;
	}
};

static bool g_bVtune = false;
static int64_t g_tmStarted = 0;

// vtune
#ifdef USE_VTUNE
#include "ittnotify.h"
#if USE_WINDOWS
#pragma comment(linker, "/defaultlib:libittnotify.lib")
#pragma message("Automatically linking with libittnotify.lib")
#endif
#endif

struct LoopProfiler_t
{
#ifdef USE_VTUNE
	__itt_domain *			m_pDomain;
	__itt_string_handle *	m_pTaskPoll;
	__itt_string_handle *	m_pTaskTick;
	__itt_string_handle *	m_pTaskActions;
	__itt_string_handle *	m_pTaskAT;
	__itt_string_handle *	m_pTaskAR;
	__itt_string_handle *	m_pTaskRemove;
	__itt_string_handle *	m_pTaskNext;
	__itt_string_handle *	m_pTaskExt;
	__itt_string_handle *	m_pTaskClean;
	__itt_string_handle *	m_pTaskStat;
#endif

	bool					m_bEnable;
	int64_t					m_tmTotal = 0;
	int m_iPerfEv=0, m_iPerfNext=0, m_iPerfExt=0, m_iPerfClean=0;

	LoopProfiler_t ()
	{
		m_bEnable = g_bVtune;
#ifdef USE_VTUNE
		__itt_thread_set_name ( "net-loop" );
		m_pDomain = __itt_domain_create ( "Task Domain" );
		m_pTaskPoll = __itt_string_handle_create ( "poll" );
		m_pTaskTick = __itt_string_handle_create ( "tick" );
		m_pTaskActions = __itt_string_handle_create ( "actions" );
		m_pTaskAT = __itt_string_handle_create ( "Ta" );
		m_pTaskAR = __itt_string_handle_create ( "ra" );
		m_pTaskRemove = __itt_string_handle_create ( "remove" );
		m_pTaskNext = __itt_string_handle_create ( "next" );
		m_pTaskExt = __itt_string_handle_create ( "ext" );
		m_pTaskClean = __itt_string_handle_create ( "clean" );
		m_pTaskStat = __itt_string_handle_create ( "stat" );
		if ( !m_bEnable )
			m_pDomain->flags = 0;
		else
			__itt_resume ();
#endif
	}

	void End ()
	{
		EndTask();
#ifdef USE_VTUNE
		if ( m_bEnable )
		{
			int64_t tmNow = sphMicroTimer();
			int64_t tmDelta = tmNow - m_tmTotal;
			int64_t tmPassed = tmNow - g_tmStarted;
			sphLogDebug ( "loop=%.3f, (act=%d, next=%d, ext=%d, cln=%d), passed=%.3f", ((float)tmDelta)/1000.0f,
				m_iPerfEv, m_iPerfNext, m_iPerfExt, m_iPerfClean, ((float)tmPassed)/1000000.0f );
		}
#endif
	}
	void EndTask ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_end ( m_pDomain );
#endif
	}
	void Start ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
		{
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskTick );
			m_tmTotal = sphMicroTimer();
			m_iPerfEv = m_iPerfNext = m_iPerfExt = m_iPerfClean = 0;
		}
#endif
	}
	void StartPoll ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskPoll );
#endif
	}
	void StartTick ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskActions );
#endif
	}
	void StartRemove ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskRemove );
#endif
	}
	void StartExt ()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskExt );
#endif
	}
	void StartAt()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskAT );
#endif
	}
	void StartAr()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskAR );
#endif
	}
	void StartClean()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskClean );
#endif
	}
	void StartNext()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskNext );
#endif
	}
	void StartStat()
	{
#ifdef USE_VTUNE
		if ( m_bEnable )
			__itt_task_begin ( m_pDomain, __itt_null, __itt_null, m_pTaskStat );
#endif
	}
};

struct ThdJobCleanup_t : public ISphJob
{
	CSphVector<ISphNetAction *> m_dCleanup;

	explicit ThdJobCleanup_t ( CSphVector<ISphNetAction *> & dCleanup );
	~ThdJobCleanup_t () override;

	void				Call () override;
	void				Clear();
};

static int	g_iNetWorkers = 1;
static int	g_iThrottleAction = 0;
static int	g_iThrottleAccept = 0;

class CSphNetLoop
{
public:
	DWORD							m_uTick;

private:
	CSphVector<ISphNetAction *>		m_dWorkExternal;
	volatile bool					m_bGotExternal;
	CSphWakeupEvent *				m_pWakeupExternal = nullptr; // FIXME!!! owned\deleted by event loop
	CSphMutex						m_tExtLock;
	LoopProfiler_t					m_tPrf;
	NetActionsPoller				m_tPoller;

	explicit CSphNetLoop ( CSphVector<Listener_t> & dListeners )
	{
		int64_t tmNow = sphMicroTimer();
		for ( const auto & dListener : dListeners )
		{
			auto * pCur = new NetActionAccept_t ( dListener );
			m_tPoller.SetupEvent ( pCur, tmNow );
		}

		CSphScopedPtr<CSphWakeupEvent> pWakeup ( new CSphWakeupEvent );
		if ( pWakeup->IsPollable() )
		{
			m_pWakeupExternal = pWakeup.LeakPtr ();
			m_tPoller.SetupEvent ( m_pWakeupExternal, tmNow );
		} else
			sphWarning ( "net-loop use timeout due to %s", pWakeup->m_sError.cstr () );

		m_bGotExternal = false;
		m_dWorkExternal.Reserve ( 1000 );
		m_uTick = 0;
	}

	~CSphNetLoop()
	{
		ARRAY_FOREACH ( i, m_dWorkExternal )
			SafeDelete ( m_dWorkExternal[i] );
	}

	void Tick ()
	{
		CSphVector<ISphNetAction *> dCleanup;
		CSphVector<ISphNetAction *> dWorkNext;
		dWorkNext.Reserve ( 1000 );
		dCleanup.Reserve ( 1000 );
		int64_t tmNextCheck = INT64_MAX;
		int64_t tmLastWait = sphMicroTimer();

		while ( !g_bShutdown )
		{
			m_tPrf.Start();

			// lets spin net-loop thread without syscall\sleep\wait up to net_wait period
			// in case we got events recently or call job that might finish early
			// otherwise poll ( 1 ) \ epoll_wait ( 1 ) put off this thread and introduce some latency, ie
			// sysbench test with 1 thd and 3 empty indexes reports:
			// 3k qps for net-loop without spin-wait
			// 5k qps for net-loop with spin-wait
			int iSpinWait = 0;
			if ( g_tmWait==-1 || ( g_tmWait>0 && sphMicroTimer()-tmLastWait>I64C(10000)*g_tmWait ) )
				iSpinWait = 1;

			m_tPrf.StartPoll();
			// need positive timeout for communicate threads back and shutdown
			bool bGot = m_tPoller.Wait ( iSpinWait );
			m_tPrf.EndTask();

			m_uTick++;

			// try to remove outdated items on no signals
			if ( !bGot && !m_bGotExternal )
			{
				tmNextCheck = RemoveOutdated ( tmNextCheck, dCleanup );
				Cleanup ( dCleanup );
				m_tPrf.End();
				continue;
			}

			// add actions planned by jobs
			if ( m_bGotExternal )
			{
				m_tPrf.StartExt();
				m_tPrf.m_iPerfExt = m_dWorkExternal.GetLength();
				assert ( dWorkNext.GetLength()==0 );
				Verify ( m_tExtLock.Lock() );
				dWorkNext.SwapData ( m_dWorkExternal );
				m_bGotExternal = false;
				Verify ( m_tExtLock.Unlock() );
				m_tPrf.EndTask();
			}

			// handle events and collect stats
			m_tPrf.StartTick();

			int iGotEvents = m_tPoller.IterateStart();
			sphLogDebugv ( "got events=%d, tick=%u", iGotEvents, m_uTick );

			int iConnections = 0;
			int iMaxIters = 0;
			while ( m_tPoller.IterateNextReady() && ( !g_iThrottleAction || iMaxIters<g_iThrottleAction ) )
			{
				m_tPrf.StartAt();
				assert ( m_tPoller.IterateGet().m_pWork && m_tPoller.IterateGet().m_uEvents );
				ISphNetAction * pWork = m_tPoller.IterateGet().m_pWork;

				NetEvent_e eEv = pWork->Tick ( m_tPoller.IterateGet().m_uEvents, dWorkNext, this );
				pWork->GetStats ( iConnections );
				m_tPrf.m_iPerfEv++;
				iMaxIters++;

				if ( eEv==NE_KEEP )
				{
					m_tPrf.EndTask();
					continue;
				}

				m_tPrf.StartAr();
				if ( eEv==NE_REMOVE || eEv==NE_REMOVED )
				{
					if ( eEv==NE_REMOVE )
						RemoveIterEvent();
					dCleanup.Add ( pWork );
				} else
				{
					m_tPoller.IterateChangeEvent ( eEv, pWork );
				}
				m_tPrf.EndTask();
				m_tPrf.EndTask();
			}
			m_tPrf.EndTask();

			tmNextCheck = RemoveOutdated ( tmNextCheck, dCleanup );
			Cleanup ( dCleanup );

			m_tPrf.StartNext();
			int64_t	tmNow = sphMicroTimer();
			// setup new handlers
			ARRAY_FOREACH ( i, dWorkNext )
			{
				ISphNetAction * pWork = dWorkNext[i];
				m_tPoller.SetupEvent ( pWork, tmNow );
				if ( pWork->m_tmTimeout )
					tmNextCheck = Min ( tmNextCheck, pWork->m_tmTimeout );
			}
			m_tPrf.m_iPerfNext = dWorkNext.GetLength();
			dWorkNext.Resize ( 0 );
			m_tPrf.EndTask();

			// update stats
			if ( iConnections )
			{
				m_tPrf.StartStat();
				g_tStats.m_iConnections += iConnections;
				m_tPrf.EndTask();
			}

			tmLastWait = sphMicroTimer();
			m_tPrf.End();
		}
	}

	int64_t RemoveOutdated ( int64_t tmNextCheck, CSphVector<ISphNetAction *> & dCleanup )
	{
		int64_t tmNow = sphMicroTimer();
		if ( tmNow<tmNextCheck )
			return tmNextCheck;

		m_tPrf.StartRemove();

		// remove outdated items on no signals
		tmNextCheck = INT64_MAX;
		m_tPoller.IterateStart();
		while ( m_tPoller.IterateNextAll() )
		{
			assert ( m_tPoller.IterateGet().m_pWork );
			ISphNetAction * pWork = m_tPoller.IterateGet().m_pWork;
			if ( !pWork->m_tmTimeout )
				continue;

			if ( tmNow<pWork->m_tmTimeout )
			{
				tmNextCheck = Min ( tmNextCheck, pWork->m_tmTimeout );
				continue;
			}

			sphLogDebugv ( "%p bailing on timeout no signal, sock=%d", pWork, pWork->m_iSock );
			m_tPoller.IterateRemove ();
			// SafeDelete ( pWork );
			// close socket immediately to prevent write by client into persist connection that just timed out
			// that does not need in case Work got removed at IterateRemove + SafeDelete ( pWork );
			// but deferred clean up by ThdJobCleanup_t needs to close socket here right after it was removed from e(poll) set
			pWork->CloseSocket();
			dCleanup.Add ( pWork );
		}

		m_tPrf.EndTask();

		return tmNextCheck;
	}

	void Cleanup ( CSphVector<ISphNetAction *> & dCleanup )
	{
		if ( !dCleanup.GetLength() )
			return;

		m_tPrf.m_iPerfClean = dCleanup.GetLength();
		m_tPrf.StartClean();
		ThdJobCleanup_t * pCleanup = new ThdJobCleanup_t ( dCleanup );
		g_pThdPool->AddJob ( pCleanup );
		m_tPrf.EndTask();
	}

public:
	void AddAction ( ISphNetAction * pElem )
	{
		Verify ( m_tExtLock.Lock() );
		m_bGotExternal = true;
		m_dWorkExternal.Add ( pElem );
		Verify ( m_tExtLock.Unlock() );
		if ( m_pWakeupExternal )
			m_pWakeupExternal->Wakeup();
	}

	void RemoveIterEvent ()
	{
		m_tPoller.IterateRemove();
	}

	// main thread wrapper
	static void ThdTick ( void * )
	{
		CrashQuery_t tQueryTLS;
		SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

		CSphNetLoop tLoop ( g_dListeners );
		tLoop.Tick();
	}
};

struct ThdJobAPI_t : public ISphJob
{
	CSphScopedPtr<NetStateAPI_t>		m_tState;
	CSphNetLoop *		m_pLoop;
	SearchdCommand_e	m_eCommand = SEARCHD_COMMAND_WRONG;
	WORD				m_uCommandVer = VER_COMMAND_WRONG;

	ThdJobAPI_t ( CSphNetLoop * pLoop, NetStateAPI_t * pState );

	void		Call () final;
};

struct ThdJobQL_t : public ISphJob
{
	CSphScopedPtr<NetStateQL_t>		m_tState;
	CSphNetLoop *		m_pLoop;

	ThdJobQL_t ( CSphNetLoop * pLoop, NetStateQL_t * pState );

	void		Call () final;
};


struct ThdJobHttp_t : public ISphJob
{
	CSphScopedPtr<NetStateQL_t>		m_tState;
	CSphNetLoop *		m_pLoop;

	ThdJobHttp_t ( CSphNetLoop * pLoop, NetStateQL_t * pState );

	void		Call () final;
};

static void JobDoSendNB ( NetSendData_t * pSend, CSphNetLoop * pLoop );

static void LogSocketError ( const char * sMsg, const NetStateCommon_t * pConn, bool bDebug )
{
	if ( bDebug && g_eLogLevel<SPH_LOG_VERBOSE_DEBUG )
		return;

	assert ( pConn );
	int iErrno = sphSockGetErrno();
	if ( iErrno==0 && pConn->m_iClientSock>=0 )
	{
		socklen_t iLen = sizeof(iErrno);
		int iRes = getsockopt ( pConn->m_iClientSock, SOL_SOCKET, SO_ERROR, (char*)&iErrno, &iLen );
		if ( iRes<0 )
			sphWarning ( "%s (client=%s(%d)), failed to get error: %d '%s'", sMsg, pConn->m_sClientName, pConn->m_iConnID, errno, strerrorm ( errno ) );
	}

	if ( bDebug || iErrno==ESHUTDOWN )
		sphLogDebugv ( "%s (client=%s(%d)), error: %d '%s', sock=%d", sMsg, pConn->m_sClientName, pConn->m_iConnID, iErrno, sphSockError ( iErrno ), pConn->m_iClientSock );
	else
		sphWarning ( "%s (client=%s(%d)), error: %d '%s', sock=%d", sMsg, pConn->m_sClientName, pConn->m_iConnID, iErrno, sphSockError ( iErrno ), pConn->m_iClientSock );
}

static bool CheckSocketError ( DWORD uGotEvents, const char * sMsg, const NetStateCommon_t * pConn, bool bDebug )
{
	bool bReadError = ( ( uGotEvents & NE_IN ) && ( uGotEvents & ( NE_ERR | NE_HUP ) ) );
	bool bWriteError = ( ( uGotEvents & NE_OUT ) && ( uGotEvents & NE_ERR ) );

	if ( bReadError || bWriteError )
	{
		LogSocketError ( sMsg, pConn, bDebug );
		return true;
	}
	return false;
}

static WORD NetBufGetWord ( const BYTE * pBuf )
{
	WORD uVal = sphUnalignedRead ( (WORD &)*pBuf );
	return ntohs ( uVal );
}

static DWORD NetBufGetLSBDword ( const BYTE * pBuf )
{
	return pBuf[0] + ( ( pBuf[1] ) <<8 ) + ( ( pBuf[2] )<<16 ) + ( ( pBuf[3] )<<24 );
}

static int NetBufGetInt ( const BYTE * pBuf )
{
	int iVal = sphUnalignedRead ( (int &)*pBuf );
	return ntohl ( iVal );
}


NetActionAccept_t::NetActionAccept_t ( const Listener_t & tListener )
	: ISphNetAction ( tListener.m_iSock )
{
	m_tListener = tListener;
	m_iConnections = 0;
}

NetEvent_e NetActionAccept_t::Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop )
{
	if ( CheckSocketError ( uGotEvents, "accept err WTF???", &m_tDummy, true ) )
		return NE_KEEP;

	// handle all incoming requests at once but not too much
	int iLastConn = m_iConnections;
	sockaddr_storage saStorage = {0};
	socklen_t uLength = sizeof(saStorage);

	while (true)
	{
		if ( g_iThrottleAccept && g_iThrottleAccept<m_iConnections-iLastConn )
		{
			sphLogDebugv ( "%p accepted %d connections throttled", this, m_iConnections-iLastConn );
			return NE_KEEP;
		}

		// accept
		int iClientSock = accept ( m_tListener.m_iSock, (struct sockaddr *)&saStorage, &uLength );

		// handle failures and no more incoming clients
		if ( iClientSock<0 )
		{
			const int iErrno = sphSockGetErrno();
			if ( iErrno==EINTR || iErrno==ECONNABORTED || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			{
				if ( m_iConnections!=iLastConn )
					sphLogDebugv ( "%p accepted %d connections all, tick=%u", this, m_iConnections-iLastConn, pLoop->m_uTick );

				return NE_KEEP;
			}

			if ( iErrno==EMFILE || iErrno==ENFILE )
			{
				sphWarning ( "accept() failed, raise ulimit -n and restart searchd: %s", sphSockError(iErrno) );
				return NE_KEEP;
			}

			sphFatal ( "accept() failed: %s", sphSockError(iErrno) );
		}

		if ( sphSetSockNB ( iClientSock )<0 )
		{
			sphWarning ( "sphSetSockNB() failed: %s", sphSockError() );
			sphSockClose ( iClientSock );
			return NE_KEEP;
		}

#ifdef	TCP_NODELAY
		int bNoDelay = 1;
		if ( saStorage.ss_family==AF_INET && setsockopt ( iClientSock, IPPROTO_TCP, TCP_NODELAY, (char*)&bNoDelay, sizeof(bNoDelay) )<0 )
		{
			sphWarning ( "set of TCP_NODELAY failed: %s", sphSockError() );
			sphSockClose ( iClientSock );
			return NE_KEEP;
		}
#endif

		if ( g_bMaintenance && !m_tListener.m_bVIP )
		{
			sphWarning ( "server is in maintenance mode: refusing connection" );
			sphSockClose ( iClientSock );
			return NE_KEEP;
		}

		m_iConnections++;
		int iConnID = ++g_iConnectionID;
		if ( g_iConnectionID<0 )
		{
			g_iConnectionID = 0;
			iConnID = 0;
		}

		ISphNetAction * pAction = NULL;
		if ( m_tListener.m_eProto==PROTO_SPHINX )
		{
			NetStateAPI_t * pStateAPI = new NetStateAPI_t ();
			pStateAPI->m_dBuf.Reserve ( 65535 );
			FillNetState ( pStateAPI, iClientSock, iConnID, m_tListener.m_bVIP, saStorage );
			pAction = new NetReceiveDataAPI_t ( pStateAPI );
		} else if ( m_tListener.m_eProto==PROTO_HTTP )
		{
			NetStateQL_t * pStateHttp = new NetStateQL_t ();
			pStateHttp->m_dBuf.Reserve ( 65535 );
			FillNetState ( pStateHttp, iClientSock, iConnID, false, saStorage );
			pAction = new NetReceiveDataHttp_t ( pStateHttp );
		} else
		{
			NetStateQL_t * pStateQL = new NetStateQL_t ();
			pStateQL->m_dBuf.Reserve ( 65535 );
			FillNetState ( pStateQL, iClientSock, iConnID, m_tListener.m_bVIP, saStorage );
			NetReceiveDataQL_t * pActionQL = new NetReceiveDataQL_t ( pStateQL );
			pActionQL->SetupHandshakePhase();
			pAction = pActionQL;
		}
		dNextTick.Add ( pAction );
		sphLogDebugv ( "%p accepted %s, sock=%d, tick=%u", this, g_dProtoNames[m_tListener.m_eProto], pAction->m_iSock, pLoop->m_uTick );
	}
}

NetEvent_e NetActionAccept_t::Setup ( int64_t )
{
	return NE_IN;
}

bool NetActionAccept_t::GetStats ( int & iConnections )
{
	if ( !m_iConnections )
		return false;

	iConnections += m_iConnections;
	m_iConnections = 0;
	return true;
}

void NetActionAccept_t::FillNetState ( NetStateCommon_t * pState, int iClientSock, int iConnID, bool bVIP, const sockaddr_storage & saStorage ) const
{
	pState->m_iClientSock = iClientSock;
	pState->m_iConnID = iConnID;
	pState->m_bVIP = bVIP;

	// format client address
	pState->m_sClientName[0] = '\0';
	if ( saStorage.ss_family==AF_INET )
	{
		struct sockaddr_in * pSa = ((struct sockaddr_in *)&saStorage);
		sphFormatIP ( pState->m_sClientName, SPH_ADDRESS_SIZE, pSa->sin_addr.s_addr );

		char * d = pState->m_sClientName;
		while ( *d )
			d++;
		snprintf ( d, 7, ":%d", (int)ntohs ( pSa->sin_port ) ); //NOLINT
	} else if ( saStorage.ss_family==AF_UNIX )
	{
		strncpy ( pState->m_sClientName, "(local)", SPH_ADDRESS_SIZE );
	}
}


const char * g_sErrorNetAPI[] = { "failed to send server version", "exiting on handshake error", "bailing on failed request header", "failed to receive client request body" };
STATIC_ASSERT ( sizeof(g_sErrorNetAPI)/sizeof(g_sErrorNetAPI[0])==AAPI_TOTAL, NOT_ALL_EMUN_DESCRIBERD );

NetReceiveDataAPI_t::NetReceiveDataAPI_t ( NetStateAPI_t *	pState )
	: ISphNetAction ( pState->m_iClientSock )
	, m_tState ( pState )
{
	m_ePhase = AAPI_HANDSHAKE_OUT;
	m_tState->m_iPos = 0;
	m_eCommand = SEARCHD_COMMAND_WRONG;
	m_uCommandVer = VER_COMMAND_WRONG;

	m_tState->m_dBuf.Resize ( 4 );
	*(DWORD *)( m_tState->m_dBuf.Begin() ) = htonl ( SPHINX_SEARCHD_PROTO );
	m_tState->m_iLeft = 4;
	assert ( m_tState.Ptr() );
}

NetEvent_e NetReceiveDataAPI_t::Setup ( int64_t tmNow )
{
	assert ( m_tState.Ptr() );
	sphLogDebugv ( "%p receive API setup, phase=%d, keep=%d, client=%s, conn=%d, sock=%d", this, m_ePhase, m_tState->m_bKeepSocket, m_tState->m_sClientName, m_tState->m_iConnID, m_tState->m_iClientSock );

	if ( !m_tState->m_bKeepSocket )
	{
		m_tmTimeout = tmNow + MS2SEC * g_iReadTimeout;
	} else
	{
		m_tmTimeout = tmNow + MS2SEC * g_iClientTimeout;
	}

	return ( m_ePhase==AAPI_HANDSHAKE_OUT ? NE_OUT : NE_IN );
}


void NetReceiveDataAPI_t::SetupBodyPhase()
{
	m_tState->m_dBuf.Resize ( 8 );
	m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
	m_ePhase = AAPI_COMMAND;
}

void NetReceiveDataAPI_t::AddJobAPI ( CSphNetLoop * pLoop )
{
	assert ( m_tState.Ptr() );
	pLoop->RemoveIterEvent();
	bool bStart = m_tState->m_bVIP;
	int iLen = m_tState->m_dBuf.GetLength();
	ThdJobAPI_t * pJob = new ThdJobAPI_t ( pLoop, m_tState.LeakPtr() );
	pJob->m_eCommand = m_eCommand;
	pJob->m_uCommandVer = m_uCommandVer;
	sphLogDebugv ( "%p receive API job created (%p), buf=%d, sock=%d, tick=%u", this, pJob, iLen, m_iSock, pLoop->m_uTick );
	if ( bStart )
		g_pThdPool->StartJob ( pJob );
	else
		g_pThdPool->AddJob ( pJob );
}

static char g_sMaxedOutMessage[] = "maxed out, dismissing client";

NetEvent_e NetReceiveDataAPI_t::Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop )
{
	assert ( m_tState.Ptr() );
	bool bDebug = ( m_ePhase==AAPI_HANDSHAKE_IN || m_ePhase==AAPI_COMMAND );
	if ( CheckSocketError ( uGotEvents, g_sErrorNetAPI[m_ePhase], m_tState.Ptr(), bDebug ) )
		return NE_REMOVE;

	bool bWasWrite = ( m_ePhase==AAPI_HANDSHAKE_OUT );
	// loop to handle similar operations at once
	while (true)
	{
		bool bWrite = ( m_ePhase==AAPI_HANDSHAKE_OUT );
		int iRes = m_tState->NetManageSocket ( bWrite, bWasWrite );
		if ( iRes==-1 )
		{
			LogSocketError ( g_sErrorNetAPI[m_ePhase], m_tState.Ptr(), false );
			return NE_REMOVE;
		}
		m_tState->m_iLeft -= iRes;
		m_tState->m_iPos += iRes;

		// socket would block - going back to polling
		if ( iRes==0 )
		{
			bool bNextWrite = ( m_ePhase==AAPI_HANDSHAKE_OUT );
			if ( bWasWrite!=bNextWrite )
				return ( bNextWrite ? NE_OUT : NE_IN );
			else
				return NE_KEEP;
		}

		// keep using that socket
		if ( m_tState->m_iLeft )
			continue;

		sphLogDebugv ( "%p pre-API phase=%d, buf=%d, write=%d, sock=%d, tick=%u", this, m_ePhase, m_tState->m_dBuf.GetLength(), (int)bWrite, m_iSock, pLoop->m_uTick );

		// FIXME!!! handle persist socket timeout
		m_tState->m_iPos = 0;
		switch ( m_ePhase )
		{
		case AAPI_HANDSHAKE_OUT:
			m_tState->m_dBuf.Resize ( 4 );
			m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
			m_ePhase = AAPI_HANDSHAKE_IN;
			break;

		case AAPI_HANDSHAKE_IN:
			// magic unused
			SetupBodyPhase();
			break;

		case AAPI_COMMAND:
		{
			m_eCommand = ( SearchdCommand_e ) NetBufGetWord ( m_tState->m_dBuf.Begin () );
			m_uCommandVer = NetBufGetWord ( m_tState->m_dBuf.Begin() + 2 );
			m_tState->m_iLeft = NetBufGetInt ( m_tState->m_dBuf.Begin() + 4 );
			bool bBadCommand = ( m_eCommand>=SEARCHD_COMMAND_WRONG );
			bool bBadLength = ( m_tState->m_iLeft<0 || m_tState->m_iLeft>g_iMaxPacketSize );
			if ( bBadCommand || bBadLength )
			{
				m_tState->m_bKeepSocket = false;

				// unknown command, default response header
				if ( bBadLength )
					sphWarning ( "ill-formed client request (length=%d out of bounds)", m_tState->m_iLeft );
				// if command is insane, low level comm is broken, so we bail out
				if ( bBadCommand )
					sphWarning ( "ill-formed client request (command=%d, SEARCHD_COMMAND_TOTAL=%d)", m_eCommand, SEARCHD_COMMAND_TOTAL );

				m_tState->m_dBuf.Resize ( 0 );
				CachedOutputBuffer_c tOut;
				tOut.SwapData ( m_tState->m_dBuf );
				SendErrorReply ( tOut, "invalid command (code=%d, len=%d)", m_eCommand, m_tState->m_iLeft );

				tOut.SwapData ( m_tState->m_dBuf );
				auto * pSend = new NetSendData_t ( m_tState.LeakPtr(), PROTO_SPHINX );
				dNextTick.Add ( pSend );
				return NE_REMOVE;
			}
			m_tState->m_dBuf.Resize ( m_tState->m_iLeft );
			m_ePhase = AAPI_BODY;
			if ( !m_tState->m_iLeft ) // command without body
			{
				AddJobAPI ( pLoop );
				return NE_REMOVED;
			}
		}
		break;

		case AAPI_BODY:
		{
			const bool bMaxedOut = ( g_iThdQueueMax && !m_tState->m_bVIP && g_pThdPool->GetQueueLength()>=g_iThdQueueMax );

			if ( m_eCommand==SEARCHD_COMMAND_PING )
			{
				CachedOutputBuffer_c tOut;
				MemInputBuffer_c tIn ( m_tState->m_dBuf.Begin (), m_tState->m_dBuf.GetLength () );
				HandleCommandPing ( tOut, m_uCommandVer, tIn );

				if ( tIn.GetError () )
					SendErrorReply ( tOut, "invalid command (code=%d, len=%d)", m_eCommand, m_tState->m_iLeft );

				m_tState->m_dBuf.Resize ( 0 );
				tOut.SwapData ( m_tState->m_dBuf );

				auto * pSend = new NetSendData_t ( m_tState.LeakPtr (), PROTO_SPHINX );
				dNextTick.Add ( pSend );

			} else if ( bMaxedOut )
			{
				m_tState->m_bKeepSocket = false;
				sphWarning ( "%s", g_sMaxedOutMessage );

				m_tState->m_dBuf.Resize ( 0 );
				CachedOutputBuffer_c tOut;
				tOut.SwapData ( m_tState->m_dBuf );

				APICommand_t tRetry ( tOut, SEARCHD_RETRY );
				tOut.SendString ( g_sMaxedOutMessage );

				tOut.SwapData ( m_tState->m_dBuf );
				auto * pSend = new NetSendData_t ( m_tState.LeakPtr(), PROTO_SPHINX );
				dNextTick.Add ( pSend );

			} else
			{
				AddJobAPI ( pLoop );
				return NE_REMOVED;
			}
			return NE_REMOVE;
		}
		break;

		default: return NE_REMOVE;
		} // switch

		bool bNextWrite = ( m_ePhase==AAPI_HANDSHAKE_OUT );
		sphLogDebugv ( "%p post-API phase=%d, buf=%d, write=%d, sock=%d, tick=%u", this, m_ePhase, m_tState->m_dBuf.GetLength(), (int)bNextWrite, m_iSock, pLoop->m_uTick );
	}
}

void NetReceiveDataAPI_t::CloseSocket()
{
	if ( m_tState.Ptr() )
		m_tState->CloseSocket();
}

const char * g_sErrorNetQL[] = { "failed to send SphinxQL handshake", "bailing on failed MySQL header", "failed to receive MySQL request body", "failed to send SphinxQL auth" };
STATIC_ASSERT ( sizeof(g_sErrorNetQL)/sizeof(g_sErrorNetQL[0])==AQL_TOTAL, NOT_ALL_EMUN_DESCRIBERD );

NetReceiveDataQL_t::NetReceiveDataQL_t ( NetStateQL_t * pState )
	: ISphNetAction ( pState->m_iClientSock )
	, m_tState ( pState )
{
	m_ePhase = AQL_HANDSHAKE;
	m_tState->m_iLeft = 0;
	m_tState->m_iPos = 0;
	m_bAppend = false;
	m_bWrite = false;
	assert ( m_tState.Ptr() );
}

void NetReceiveDataQL_t::SetupHandshakePhase()
{
	m_ePhase = AQL_HANDSHAKE;

	m_tState->m_dBuf.Resize ( g_iMysqlHandshake );
	memcpy ( m_tState->m_dBuf.Begin(), g_sMysqlHandshake, g_iMysqlHandshake );
	m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
	m_tState->m_iPos = 0;

	m_bAppend = false;
	m_bWrite = true;
}

void NetReceiveDataQL_t::SetupBodyPhase()
{
	m_tState->m_dBuf.Resize ( 4 );
	m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
	m_tState->m_iPos = 0;

	m_bAppend = false;
	m_bWrite = false;

	m_ePhase = AQL_LENGTH;
}

NetEvent_e NetReceiveDataQL_t::Setup ( int64_t tmNow )
{
	assert ( m_tState.Ptr() );
	sphLogDebugv ( "%p receive QL setup, phase=%d, client=%s, conn=%d, sock=%d", this, m_ePhase, m_tState->m_sClientName, m_tState->m_iConnID, m_tState->m_iClientSock );

	m_tmTimeout = tmNow + MS2SEC * g_iClientQlTimeout;

	NetEvent_e eEvent;
	if ( m_ePhase==AQL_HANDSHAKE )
	{
		eEvent = NE_OUT;
		m_bWrite = true;
	} else
	{
		eEvent = NE_IN;
		m_bWrite = false;
	}
	return eEvent;
}

NetEvent_e NetReceiveDataQL_t::Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> &, CSphNetLoop * pLoop )
{
	assert ( m_tState.Ptr() );
	bool bDebugErr = ( ( m_ePhase==AQL_LENGTH && !m_bAppend ) || m_ePhase==AQL_AUTH );
	if ( CheckSocketError ( uGotEvents, g_sErrorNetQL[m_ePhase], m_tState.Ptr(), bDebugErr ) )
		return NE_REMOVE;

	bool bWrite = m_bWrite;
	// loop to handle similar operations at once
	while (true)
	{
		int iRes = m_tState->NetManageSocket ( m_bWrite, bWrite );
		if ( iRes==-1 )
		{
			LogSocketError ( g_sErrorNetQL[m_ePhase], m_tState.Ptr(), false );
			return NE_REMOVE;
		}
		m_tState->m_iLeft -= iRes;
		m_tState->m_iPos += iRes;

		// socket would block - going back to polling
		if ( iRes==0 )
		{
			if ( bWrite!=m_bWrite )
				return ( m_bWrite ? NE_OUT : NE_IN );
			else
				return NE_KEEP;
		}

		// keep using that socket
		if ( m_tState->m_iLeft )
			continue;

		sphLogDebugv ( "%p pre-QL phase=%d, buf=%d, append=%d, write=%d, sock=%d, tick=%u", this, m_ePhase, m_tState->m_dBuf.GetLength(), (int)m_bAppend, (int)m_bWrite, m_iSock, pLoop->m_uTick );

		switch ( m_ePhase )
		{
		case AQL_HANDSHAKE:
			// no action required - switching to next phase
			SetupBodyPhase();
		break;

		case AQL_LENGTH:
		{
			const int MAX_PACKET_LEN = 0xffffffL; // 16777215 bytes, max low level packet size
			DWORD uHeader = 0;
			const BYTE * pBuf = ( m_bAppend ? m_tState->m_dBuf.Begin() + m_tState->m_iPos - sizeof(uHeader) : m_tState->m_dBuf.Begin() );
			uHeader = NetBufGetLSBDword ( pBuf );
			m_tState->m_uPacketID = 1 + (BYTE)( uHeader>>24 ); // client will expect this id
			m_tState->m_iLeft = ( uHeader & MAX_PACKET_LEN );

			if ( m_bAppend ) // reading big packet
			{
				m_tState->m_iPos = m_tState->m_dBuf.GetLength() - sizeof(uHeader);
				m_tState->m_dBuf.Resize ( m_tState->m_iPos + m_tState->m_iLeft );
			} else // reading regular packet
			{
				m_tState->m_iPos = 0;
				m_tState->m_dBuf.Resize ( m_tState->m_iLeft );
			}

			// check request length
			if ( m_tState->m_dBuf.GetLength()>g_iMaxPacketSize )
			{
				sphWarning ( "ill-formed client request (length=%d out of bounds)", m_tState->m_dBuf.GetLength() );
				return NE_REMOVE;
			}

			m_bWrite = false;
			m_bAppend = ( m_tState->m_iLeft==MAX_PACKET_LEN ); // might be next big packet
			m_ePhase = AQL_BODY;
		}
		break;

		case AQL_BODY:
		{
			if ( m_bAppend )
			{
				m_tState->m_iLeft = 4;
				m_tState->m_dBuf.Resize ( m_tState->m_iPos + m_tState->m_iLeft );

				m_bWrite = false;
				m_ePhase = AQL_LENGTH;
			} else if ( !m_tState->m_bAuthed )
			{
				m_tState->m_bAuthed = true;
				if ( IsFederatedUser ( m_tState->m_dBuf.Begin(), m_tState->m_dBuf.GetLength() ) )
					m_tState->m_tSession.SetFederatedUser();
				m_tState->m_dBuf.Resize ( 0 );
				ISphOutputBuffer tOut;
				tOut.SwapData ( m_tState->m_dBuf );
				SendMysqlOkPacket ( tOut, m_tState->m_uPacketID, m_tState->m_tSession.m_tVars.m_bAutoCommit );
				tOut.SwapData ( m_tState->m_dBuf );

				m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
				m_tState->m_iPos = 0;

				m_bWrite = true;
				m_ePhase = AQL_AUTH;
			} else
			{
				CSphVector<BYTE> & dBuf = m_tState->m_dBuf;
				int iBufLen = dBuf.GetLength();
				int iCmd = ( iBufLen>0 ? dBuf[0] : 0 );
				int iStrLen = Min ( iBufLen, 80 );
				sphLogDebugv ( "%p receive-QL, cmd=%d, buf=%d, sock=%d, '%.*s'", this, iCmd, iBufLen, m_iSock, iStrLen, ( dBuf.GetLength() ? (const char *)dBuf.Begin() : "" ) );

				bool bEmptyBuf = !dBuf.GetLength();
				bool bMaxedOut = ( iCmd==MYSQL_COM_QUERY && !m_tState->m_bVIP && g_iThdQueueMax && g_pThdPool->GetQueueLength()>=g_iThdQueueMax );

				if ( bEmptyBuf || bMaxedOut )
				{
					dBuf.Resize ( 0 );
					ISphOutputBuffer tOut;
					tOut.SwapData ( dBuf );

					if ( !bMaxedOut )
					{
						SendMysqlErrorPacket ( tOut, m_tState->m_uPacketID, "", "unknown command with size 0", m_tState->m_iConnID, MYSQL_ERR_UNKNOWN_COM_ERROR );
					} else
					{
						SendMysqlErrorPacket ( tOut, m_tState->m_uPacketID, "", "Too many connections", m_tState->m_iConnID, MYSQL_ERR_TOO_MANY_USER_CONNECTIONS );
					}

					tOut.SwapData ( dBuf );
					m_tState->m_iLeft = dBuf.GetLength();
					m_tState->m_iPos = 0;
					m_bWrite = true;
					m_ePhase = AQL_AUTH;
				} else
				{
					// all comes to an end
					if ( iCmd==MYSQL_COM_QUIT )
					{
						m_tState->m_bKeepSocket = false;
						return NE_REMOVE;
					}

					if ( iCmd==MYSQL_COM_QUERY )
					{
						pLoop->RemoveIterEvent();

						// going to actual work now
						bool bStart = m_tState->m_bVIP;
						ThdJobQL_t * pJob = new ThdJobQL_t ( pLoop, m_tState.LeakPtr() );
						if ( bStart )
							g_pThdPool->StartJob ( pJob );
						else
							g_pThdPool->AddJob ( pJob );

						return NE_REMOVED;
					} else
					{
						// short-cuts to keep action in place and don't dive to job then get back here
						CSphString sError;
						m_tState->m_dBuf.Resize ( 0 );
						ISphOutputBuffer tOut;
						tOut.SwapData ( m_tState->m_dBuf );
						switch ( iCmd )
						{
						case MYSQL_COM_PING:
						case MYSQL_COM_INIT_DB:
							// client wants a pong
							SendMysqlOkPacket ( tOut, m_tState->m_uPacketID, m_tState->m_tSession.m_tVars.m_bAutoCommit );
							break;

						case MYSQL_COM_SET_OPTION:
							// bMulti = ( tIn.GetWord()==MYSQL_OPTION_MULTI_STATEMENTS_ON ); // that's how we could double check and validate multi query
							// server reporting success in response to COM_SET_OPTION and COM_DEBUG
							SendMysqlEofPacket ( tOut, m_tState->m_uPacketID, 0, false, m_tState->m_tSession.m_tVars.m_bAutoCommit );
							break;

						default:
							// default case, unknown command
							sError.SetSprintf ( "unknown command (code=%d)", iCmd );
							SendMysqlErrorPacket ( tOut, m_tState->m_uPacketID, "", sError.cstr(), m_tState->m_iConnID, MYSQL_ERR_UNKNOWN_COM_ERROR );
							break;
						}

						tOut.SwapData ( dBuf );
						m_tState->m_iLeft = dBuf.GetLength();
						m_tState->m_iPos = 0;
						m_bWrite = true;
						m_ePhase = AQL_AUTH;
					}
				}
			}
		}
		break;

		case AQL_AUTH:
			m_tState->m_iLeft = 4;
			m_tState->m_dBuf.Resize ( m_tState->m_iLeft );
			m_tState->m_iPos = 0;

			m_bWrite = false;
			m_ePhase = AQL_LENGTH;
			break;

		default: return NE_REMOVE;
		}

		sphLogDebugv ( "%p post-QL phase=%d, buf=%d, append=%d, write=%d, sock=%d, tick=%u", this, m_ePhase, m_tState->m_dBuf.GetLength(), (int)m_bAppend, (int)m_bWrite, m_iSock, pLoop->m_uTick );
	}
}


void NetReceiveDataQL_t::CloseSocket()
{
	if ( m_tState.Ptr() )
		m_tState->CloseSocket();
}


NetSendData_t::NetSendData_t ( NetStateCommon_t * pState, ProtocolType_e eProto )
	: ISphNetAction ( pState->m_iClientSock )
	, m_tState ( pState )
	, m_eProto ( eProto )
	, m_bContinue ( false )
{
	assert ( pState );
	m_tState->m_iPos = 0;
	m_tState->m_iLeft = 0;
}

NetEvent_e NetSendData_t::Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & dNextTick, CSphNetLoop * pLoop )
{
	if ( CheckSocketError ( uGotEvents, "failed to send data", m_tState.Ptr(), false ) )
		return NE_REMOVE;

	for ( ; m_tState->m_iLeft>0; )
	{
		int iRes = m_tState->NetManageSocket ( true );
		if ( iRes==-1 )
		{
			LogSocketError ( "failed to send data", m_tState.Ptr(), false );
			return NE_REMOVE;
		}
		// for iRes==0 case might proceed after interruption
		m_tState->m_iLeft -= iRes;
		m_tState->m_iPos += iRes;

		// socket would block - going back to polling
		if ( iRes==0 )
			break;
	}

	if ( m_tState->m_iLeft>0 )
		return NE_KEEP;

	assert ( m_tState->m_iLeft==0 && m_tState->m_iPos==m_tState->m_dBuf.GetLength() );

	if ( m_tState->m_bKeepSocket )
	{
		sphLogDebugv ( "%p send %s job created, sent=%d, sock=%d, tick=%u", this, g_dProtoNames[m_eProto], m_tState->m_iPos, m_iSock, pLoop->m_uTick );
		switch ( m_eProto )
		{
			case PROTO_SPHINX:
			{
				auto * pAction = new NetReceiveDataAPI_t ( m_tState.LeakPtr() );
				pAction->SetupBodyPhase();
				dNextTick.Add ( pAction );
			}
			break;

			case PROTO_MYSQL41:
			{
				auto * pAction = new NetReceiveDataQL_t ( (NetStateQL_t *)m_tState.LeakPtr() );
				pAction->SetupBodyPhase();
				dNextTick.Add ( pAction );
			}
			break;

			case PROTO_HTTP:
			{
				auto * pAction = new NetReceiveDataHttp_t ( (NetStateQL_t *)m_tState.LeakPtr() );
				dNextTick.Add ( pAction );
			}
			break;

			default: break;
		}
	}

	return NE_REMOVE;
}

NetEvent_e NetSendData_t::Setup ( int64_t tmNow )
{
	assert ( m_tState.Ptr() );
	sphLogDebugv ( "%p send %s setup, keep=%d, buf=%d, client=%s, conn=%d, sock=%d", this, g_dProtoNames[m_eProto], (int)(m_tState->m_bKeepSocket),
		m_tState->m_dBuf.GetLength(), m_tState->m_sClientName, m_tState->m_iConnID, m_tState->m_iClientSock );

	if ( !m_bContinue )
	{
		m_tmTimeout = tmNow + MS2SEC * g_iWriteTimeout;

		assert ( m_tState->m_dBuf.GetLength() );
		m_tState->m_iLeft = m_tState->m_dBuf.GetLength();
		m_tState->m_iPos = 0;
	}
	m_bContinue = false;

	return NE_OUT;
}


void NetSendData_t::CloseSocket()
{
	if ( m_tState.Ptr() )
		m_tState->CloseSocket();
}


NetStateCommon_t::NetStateCommon_t ()
{
	m_sClientName[0] = '\0';
}

NetStateCommon_t::~NetStateCommon_t()
{
	CloseSocket();
}

void NetStateCommon_t::CloseSocket ()
{
	if ( m_iClientSock>=0 )
	{
		sphLogDebugv ( "%p state closing sock=%d", this, m_iClientSock );
		sphSockClose ( m_iClientSock );
		m_iClientSock = -1;
	}
}

int NetStateCommon_t::NetManageSocket ( bool bWrite, bool bAfterWrite )
{
	// try next chunk
	int iRes = 0;
	if ( bWrite )
		iRes = (int)sphSockSend ( m_iClientSock, (const char*) m_dBuf.begin() + m_iPos, m_iLeft );
	else
		iRes = (int)sphSockRecv ( m_iClientSock, (char*) m_dBuf.begin () + m_iPos, m_iLeft );

	// if there was EINTR, retry
	// if any other error, bail
	if ( iRes==-1 )
	{
		// only let SIGTERM (of all them) to interrupt
		int iErr = sphSockPeekErrno ();
		return ( ( iErr==EINTR || iErr==EAGAIN || iErr==EWOULDBLOCK ) ? 0 : -1 );
	}

	// if there was eof, we're done
	// but need to make sure that poll loop passed at least once,
	// ie write-read pattern should failed only this way write-poll-read
	if ( !bWrite && iRes==0 && m_iLeft!=0 && !bAfterWrite ) // request to read 0 raise error on Linux, but not on Mac
	{
		sphLogDebugv ( "read zero bytes, shutting down socket, sock=%d", m_iClientSock );
		sphSockSetErrno ( ESHUTDOWN );
		return -1;
	}

	return iRes;
}

NetReceiveDataHttp_t::NetReceiveDataHttp_t ( NetStateQL_t *	pState )
	: ISphNetAction ( pState->m_iClientSock )
	, m_tState ( pState )
{
	assert ( m_tState.Ptr() );
	m_tState->m_iPos = 0;
	m_tState->m_iLeft = 4000;
	m_tState->m_dBuf.Resize ( Max ( m_tState->m_dBuf.GetLimit(), 4000 ) );
}

NetEvent_e NetReceiveDataHttp_t::Setup ( int64_t tmNow )
{
	assert ( m_tState.Ptr() );
	sphLogDebugv ( "%p receive HTTP setup, keep=%d, client=%s, conn=%d, sock=%d", this, m_tState->m_bKeepSocket, m_tState->m_sClientName, m_tState->m_iConnID, m_tState->m_iClientSock );

	if ( !m_tState->m_bKeepSocket )
	{
		m_tmTimeout = tmNow + MS2SEC * g_iReadTimeout;
	} else
	{
		m_tmTimeout = tmNow + MS2SEC * g_iClientTimeout;
	}
	return NE_IN;
}

static const char g_sContentLength[] = "\r\r\n\nCcOoNnTtEeNnTt--LlEeNnGgTtHh\0";
static const size_t g_sContentLengthSize = sizeof ( g_sContentLength ) - 1;
static const char g_sHeadEnd[] = "\r\n\r\n";

HttpHeaderStreamParser_t::HttpHeaderStreamParser_t ()
{
	m_iHeaderEnd = 0;
	m_iFieldContentLenStart = 0;
	m_iFieldContentLenVal = 0;
	m_iCur = 0;
	m_iCRLF = 0;
	m_iName = 0;
}

bool HttpHeaderStreamParser_t::HeaderFound ( const BYTE * pBuf, int iLen )
{
	// early exit at for already found request header
	if ( m_iHeaderEnd || m_iCur>=iLen )
		return true;

	const int iCNwoLFSize = ( g_sContentLengthSize-5 )/2; // size of just Content-Length field name
	for ( ; m_iCur<iLen; m_iCur++ )
	{
		m_iCRLF = ( pBuf[m_iCur]==g_sHeadEnd[m_iCRLF] ? m_iCRLF+1 : 0 );
		m_iName = ( !m_iFieldContentLenStart && ( pBuf[m_iCur]==g_sContentLength[m_iName] || pBuf[m_iCur]==g_sContentLength[m_iName+1] ) ? m_iName+2 : 0 );

		// header end found
		if ( m_iCRLF==sizeof(g_sHeadEnd)-1 )
		{
			m_iHeaderEnd = m_iCur+1;
			break;
		}
		// Content-Length field found
		if ( !m_iFieldContentLenStart && m_iName==g_sContentLengthSize-1 )
			m_iFieldContentLenStart = m_iCur - iCNwoLFSize + 1;
	}

	// parse Content-Length field value
	while ( m_iHeaderEnd && m_iFieldContentLenStart )
	{
		int iNumStart = m_iFieldContentLenStart + iCNwoLFSize;
		// skip spaces
		while ( iNumStart<m_iHeaderEnd && pBuf[iNumStart]==' ' )
			iNumStart++;
		if ( iNumStart>=m_iHeaderEnd || pBuf[iNumStart]!=':' )
			break;

		iNumStart++; // skip ':' delimiter
		m_iFieldContentLenVal = atoi ( (const char *)pBuf + iNumStart ); // atoi handles leading spaces and tail not digital chars
		break;
	}

	return ( m_iHeaderEnd>0 );
}

NetEvent_e NetReceiveDataHttp_t::Tick ( DWORD uGotEvents, CSphVector<ISphNetAction *> & , CSphNetLoop * pLoop )
{
	assert ( m_tState.Ptr() );
	const char * sHttpError = "http error";
	if ( CheckSocketError ( uGotEvents, sHttpError, m_tState.Ptr(), false ) )
		return NE_REMOVE;

	// loop to handle similar operations at once
	while (true)
	{
		int iRes = m_tState->NetManageSocket ( false );
		if ( iRes==-1 )
		{
			// FIXME!!! report back to client buffer overflow with 413 error
			LogSocketError ( sHttpError, m_tState.Ptr(), false );
			return NE_REMOVE;
		}
		m_tState->m_iLeft -= iRes;
		m_tState->m_iPos += iRes;

		// socket would block - going back to polling
		if ( iRes==0 && m_tState->m_iLeft )
			return NE_KEEP;

		// keep fetching data till the end of a header
		if ( !m_tHeadParser.m_iHeaderEnd )
		{
			if ( !m_tHeadParser.HeaderFound ( m_tState->m_dBuf.Begin(), m_tState->m_iPos ) )
				continue;

			int iReqSize = m_tHeadParser.m_iHeaderEnd + m_tHeadParser.m_iFieldContentLenVal;
			if ( m_tHeadParser.m_iFieldContentLenVal && m_tState->m_iPos<iReqSize )
			{
				m_tState->m_dBuf.Resize ( iReqSize );
				m_tState->m_iLeft = iReqSize - m_tState->m_iPos;
				continue;
			}

			m_tState->m_iLeft = 0;
			m_tState->m_iPos = iReqSize;
			m_tState->m_dBuf.Resize ( iReqSize + 1 );
			m_tState->m_dBuf[m_tState->m_iPos] = '\0';
			m_tState->m_dBuf.Resize ( iReqSize );
		}

		// keep reading till end of buffer or data at socket
		if ( iRes>0 )
			continue;

		pLoop->RemoveIterEvent();

		sphLogDebugv ( "%p HTTP buf=%d, header=%d, content-len=%d, sock=%d, tick=%u", this, m_tState->m_dBuf.GetLength(), m_tHeadParser.m_iHeaderEnd, m_tHeadParser.m_iFieldContentLenVal, m_iSock, pLoop->m_uTick );

		// no VIP for http for now
		ThdJobHttp_t * pJob = new ThdJobHttp_t ( pLoop, m_tState.LeakPtr() );
		g_pThdPool->AddJob ( pJob );

		return NE_REMOVED;
	}
}

void NetReceiveDataHttp_t::CloseSocket()
{
	if ( m_tState.Ptr() )
		m_tState->CloseSocket();
}

ThdJobAPI_t::ThdJobAPI_t ( CSphNetLoop * pLoop, NetStateAPI_t * pState )
	: m_tState ( pState )
	, m_pLoop ( pLoop )
{
	assert ( m_tState.Ptr() );
}

void ThdJobAPI_t::Call ()
{
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

	sphLogDebugv ( "%p API job started, command=%d, tick=%u", this, m_eCommand, m_pLoop->m_uTick );

	int iTid = GetOsThreadId();

	ThdDesc_t tThdDesc;
	tThdDesc.m_eProto = PROTO_SPHINX;
	tThdDesc.m_iClientSock = m_tState->m_iClientSock;
	tThdDesc.m_sClientName = m_tState->m_sClientName;
	tThdDesc.m_iConnID = m_tState->m_iConnID;
	tThdDesc.m_tmConnect = sphMicroTimer();
	tThdDesc.m_iTid = iTid;

	ThreadAdd ( &tThdDesc );

	assert ( m_tState.Ptr() );

	// handle that client
	MemInputBuffer_c tBuf ( m_tState->m_dBuf.Begin(), m_tState->m_dBuf.GetLength() );
	CachedOutputBuffer_c tOut;

	if ( g_bMaintenance && !m_tState->m_bVIP )
	{
		SendErrorReply ( tOut, "server is in maintenance mode" );
		m_tState->m_bKeepSocket = false;
	} else
	{
		bool bProceed = LoopClientSphinx ( m_eCommand, m_uCommandVer,
			m_tState->m_dBuf.GetLength(), tThdDesc, tBuf, tOut, false );
		m_tState->m_bKeepSocket |= bProceed;
	}
	tOut.Flush();

	ThreadRemove ( &tThdDesc );

	sphLogDebugv ( "%p API job done, command=%d, tick=%u", this, m_eCommand, m_pLoop->m_uTick );

	if ( g_bShutdown )
		return;

	assert ( m_pLoop );
	if ( tOut.GetSentCount() )
	{
		tOut.SwapData ( m_tState->m_dBuf );
		auto * pSend = new NetSendData_t ( m_tState.LeakPtr(), PROTO_SPHINX );
		JobDoSendNB ( pSend, m_pLoop );
	} else if ( m_tState->m_bKeepSocket ) // no response - switching to receive
	{
		auto * pReceive = new NetReceiveDataAPI_t ( m_tState.LeakPtr() );
		pReceive->SetupBodyPhase();
		m_pLoop->AddAction ( pReceive );
	}
}


ThdJobQL_t::ThdJobQL_t ( CSphNetLoop * pLoop, NetStateQL_t * pState )
	: m_tState ( pState )
	, m_pLoop ( pLoop )
{
	assert ( m_tState.Ptr() );
}

void ThdJobQL_t::Call ()
{
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

	sphLogDebugv ( "%p QL job started, tick=%u", this, m_pLoop->m_uTick );

	int iTid = GetOsThreadId();

	ThdDesc_t tThdDesc;
	tThdDesc.m_eProto = PROTO_MYSQL41;
	tThdDesc.m_iClientSock = m_tState->m_iClientSock;
	tThdDesc.m_sClientName = m_tState->m_sClientName;
	tThdDesc.m_iConnID = m_tState->m_iConnID;
	tThdDesc.m_tmConnect = sphMicroTimer();
	tThdDesc.m_iTid = iTid;

	ThreadAdd ( &tThdDesc );

	CSphString sQuery; // to keep data alive for SphCrashQuery_c
	bool bProfile = m_tState->m_tSession.m_tVars.m_bProfile; // the current statement might change it
	if ( bProfile )
		m_tState->m_tSession.m_tProfile.Start ( SPH_QSTATE_TOTAL );

	MemInputBuffer_c tIn ( m_tState->m_dBuf.Begin(), m_tState->m_dBuf.GetLength() );
	ISphOutputBuffer tOut;

	// needed to check permission to turn maintenance mode on/off
	m_tState->m_tSession.m_tVars.m_bVIP = m_tState->m_bVIP;

	m_tState->m_bKeepSocket = false;
	bool bSendResponse = true;
	if ( g_bMaintenance && !m_tState->m_bVIP )
		SendMysqlErrorPacket ( tOut, m_tState->m_uPacketID, NULL, "server is in maintenance mode", tThdDesc.m_iConnID, MYSQL_ERR_UNKNOWN_COM_ERROR );
	else
	{
		bSendResponse = LoopClientMySQL ( m_tState->m_uPacketID, m_tState->m_tSession, sQuery, m_tState->m_dBuf.GetLength(), bProfile, tThdDesc, tIn, tOut );
		m_tState->m_bKeepSocket = bSendResponse;
	}

	sphLogDebugv ( "%p QL job done, tick=%u", this, m_pLoop->m_uTick );

	if ( bSendResponse && !g_bShutdown )
	{
		assert ( m_pLoop );
		tOut.SwapData ( m_tState->m_dBuf );
		NetSendData_t * pSend = new NetSendData_t ( m_tState.LeakPtr(), PROTO_MYSQL41 );
		JobDoSendNB ( pSend, m_pLoop );
	}

	ThreadRemove ( &tThdDesc );
}

static void LogCleanup ( const CSphVector<ISphNetAction *> & dCleanup )
{
	StringBuilder_c sTmp;
	ARRAY_FOREACH ( i, dCleanup )
		sTmp.Appendf ( "%p(%d), ", dCleanup[i], dCleanup[i]->m_iSock );

	sphLogDebugv ( "cleaned jobs(sock)=%d, %s", dCleanup.GetLength(), sTmp.cstr() );
}

ThdJobCleanup_t::ThdJobCleanup_t ( CSphVector<ISphNetAction *> & dCleanup )
{
	m_dCleanup.SwapData ( dCleanup );
	dCleanup.Reserve ( 1000 );
}

ThdJobCleanup_t::~ThdJobCleanup_t ()
{
	Clear();
}

void ThdJobCleanup_t::Call ()
{
	Clear();
}

void ThdJobCleanup_t::Clear ()
{
	if ( g_eLogLevel>=SPH_LOG_VERBOSE_DEBUG && m_dCleanup.GetLength() )
		LogCleanup ( m_dCleanup );

	ARRAY_FOREACH ( i, m_dCleanup )
		SafeDelete ( m_dCleanup[i] );

	m_dCleanup.Reset();
}

ThdJobHttp_t::ThdJobHttp_t ( CSphNetLoop * pLoop, NetStateQL_t * pState )
	: m_tState ( pState )
	, m_pLoop ( pLoop )
{
	assert ( m_tState.Ptr() );
}

void ThdJobHttp_t::Call ()
{
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

	sphLogDebugv ( "%p http job started, buffer len=%d, tick=%u", this, m_tState->m_dBuf.GetLength(), m_pLoop->m_uTick );

	int iTid = GetOsThreadId();

	ThdDesc_t tThdDesc;
	tThdDesc.m_eProto = PROTO_HTTP;
	tThdDesc.m_iClientSock = m_tState->m_iClientSock;
	tThdDesc.m_sClientName = m_tState->m_sClientName;
	tThdDesc.m_iConnID = m_tState->m_iConnID;
	tThdDesc.m_tmConnect = sphMicroTimer();
	tThdDesc.m_iTid = iTid;

	ThreadAdd ( &tThdDesc );

	assert ( m_tState.Ptr() );

	CrashQuery_t tCrashQuery;
	tCrashQuery.m_pQuery = m_tState->m_dBuf.Begin();
	tCrashQuery.m_iSize = m_tState->m_dBuf.GetLength();
	tCrashQuery.m_bHttp = true;
	SphCrashLogger_c::SetLastQuery ( tCrashQuery );


	if ( g_bMaintenance && !m_tState->m_bVIP )
	{
		sphHttpErrorReply ( m_tState->m_dBuf, SPH_HTTP_STATUS_503, "server is in maintenance mode" );
		m_tState->m_bKeepSocket = false;
	} else
	{
		CSphVector<BYTE> dResult;
		m_tState->m_bKeepSocket = sphLoopClientHttp ( m_tState->m_dBuf.Begin(), m_tState->m_dBuf.GetLength(), dResult, tThdDesc );
		m_tState->m_dBuf = std::move(dResult);
	}

	SphCrashLogger_c::SetLastQuery ( CrashQuery_t() );
	ThreadRemove ( &tThdDesc );

	sphLogDebugv ( "%p http job done, tick=%u", this, m_pLoop->m_uTick );

	if ( g_bShutdown )
		return;

	assert ( m_pLoop );
	NetSendData_t * pSend = new NetSendData_t ( m_tState.LeakPtr(), PROTO_HTTP );
	JobDoSendNB ( pSend, m_pLoop );
}


void JobDoSendNB ( NetSendData_t * pSend, CSphNetLoop * pLoop )
{
	assert ( pLoop && pSend );
	pSend->Setup ( sphMicroTimer() );

	// try to push data to socket here then transfer send-action to net-loop in case send needs poll to continue
	CSphVector<ISphNetAction *> dNext ( 1 );
	dNext.Resize ( 0 );
	DWORD uGotEvents = NE_OUT;
	NetEvent_e eRes = pSend->Tick ( uGotEvents, dNext, pLoop );
	if ( eRes==NE_REMOVE )
	{
		SafeDelete ( pSend );
		assert ( dNext.GetLength()<2 );
		if ( dNext.GetLength() )
			pLoop->AddAction ( dNext[0] );
	} else
	{
		pSend->SetContinue();
		pLoop->AddAction ( pSend );
	}
}

/////////////////////////////////////////////////////////////////////////////
// DAEMON OPTIONS
/////////////////////////////////////////////////////////////////////////////

static const QueryParser_i * PercolateQueryParserFactory ( bool bJson )
{
	if ( bJson )
		return sphCreateJsonQueryParser();
	else
		return sphCreatePlainQueryParser();
}


static void ParsePredictedTimeCosts ( const char * p )
{
	// yet another mini-parser!
	// ident=value [, ident=value [...]]
	while ( *p )
	{
		// parse ident
		while ( sphIsSpace(*p) )
			p++;
		if ( !*p )
			break;
		if ( !sphIsAlpha(*p) )
			sphDie ( "predicted_time_costs: parse error near '%s' (identifier expected)", p );
		const char * q = p;
		while ( sphIsAlpha(*p) )
			p++;
		CSphString sIdent;
		sIdent.SetBinary ( q, p-q );
		sIdent.ToLower();

		// parse =value
		while ( sphIsSpace(*p) )
			p++;
		if ( *p!='=' )
			sphDie ( "predicted_time_costs: parse error near '%s' (expected '=' sign)", p );
		p++;
		while ( sphIsSpace(*p) )
			p++;
		if ( *p<'0' || *p>'9' )
			sphDie ( "predicted_time_costs: parse error near '%s' (number expected)", p );
		q = p;
		while ( *p>='0' && *p<='9' )
			p++;
		CSphString sValue;
		sValue.SetBinary ( q, p-q );
		int iValue = atoi ( sValue.cstr() );

		// parse comma
		while ( sphIsSpace(*p) )
			p++;
		if ( *p && *p!=',' )
			sphDie ( "predicted_time_costs: parse error near '%s' (expected ',' or end of line)", p );
		p++;

		// bind value
		if ( sIdent=="skip" )
			g_iPredictorCostSkip = iValue;
		else if ( sIdent=="doc" )
			g_iPredictorCostDoc = iValue;
		else if ( sIdent=="hit" )
			g_iPredictorCostHit = iValue;
		else if ( sIdent=="match" )
			g_iPredictorCostMatch = iValue;
		else
			sphDie ( "predicted_time_costs: unknown identifier '%s' (known ones are skip, doc, hit, match)", sIdent.cstr() );
	}
}

// read system TFO settings and init g_ITFO according to it.
/* From https://www.kernel.org/doc/Documentation/networking/ip-sysctl.txt
 * possible bitmask values are:

	  0x1: (client) enables sending data in the opening SYN on the client.
	  0x2: (server) enables the server support, i.e., allowing data in
			a SYN packet to be accepted and passed to the
			application before 3-way handshake finishes.
	  0x4: (client) send data in the opening SYN regardless of cookie
			availability and without a cookie option.
	0x200: (server) accept data-in-SYN w/o any cookie option present.
	0x400: (server) enable all listeners to support Fast Open by
			default without explicit TCP_FASTOPEN socket option.

 Actually we interested only in first 2 bits.
 */
static void CheckSystemTFO ()
{
#if defined (MSG_FASTOPEN)
	char sBuf[20] = { 0 };
	g_iTFO = TFO_ABSENT;

	FILE * fp = fopen ( "/proc/sys/net/ipv4/tcp_fastopen", "rb" );
	if ( !fp )
	{
		sphWarning ( "TCP fast open unavailable (can't read /proc/sys/net/ipv4/tcp_fastopen, unsupported kernel?)" );
		return;
	}

	auto szResult = fgets ( sBuf, 20, fp );
	fclose ( fp );
	if ( !szResult )
		return;

	g_iTFO = atoi ( szResult );
#else
	g_iTFO = 3; // suggest it is available.
#endif
}

void ConfigureSearchd ( const CSphConfig & hConf, bool bOptPIDFile )
{
	if ( !hConf.Exists ( "searchd" ) || !hConf["searchd"].Exists ( "searchd" ) )
		sphFatal ( "'searchd' config section not found in '%s'", g_sConfigFile.cstr () );

	const CSphConfigSection & hSearchd = hConf["searchd"]["searchd"];

	sphCheckDuplicatePaths ( hConf );

	if ( bOptPIDFile )
		if ( !hSearchd ( "pid_file" ) )
			sphFatal ( "mandatory option 'pid_file' not found in 'searchd' section" );

	if ( hSearchd.Exists ( "read_timeout" ) && hSearchd["read_timeout"].intval()>=0 )
		g_iReadTimeout = hSearchd["read_timeout"].intval();

	if ( hSearchd.Exists ( "sphinxql_timeout" ) && hSearchd["sphinxql_timeout"].intval()>=0 )
		g_iClientQlTimeout = hSearchd["sphinxql_timeout"].intval();

	if ( hSearchd.Exists ( "client_timeout" ) && hSearchd["client_timeout"].intval()>=0 )
		g_iClientTimeout = hSearchd["client_timeout"].intval();

	if ( hSearchd.Exists ( "max_children" ) && hSearchd["max_children"].intval()>=0 )
	{
		g_iMaxChildren = hSearchd["max_children"].intval();
	}

	if ( hSearchd.Exists ( "persistent_connections_limit" ) && hSearchd["persistent_connections_limit"].intval()>=0 )
		g_iPersistentPoolSize = hSearchd["persistent_connections_limit"].intval();

	g_bPreopenIndexes = hSearchd.GetInt ( "preopen_indexes", (int)g_bPreopenIndexes )!=0;
	sphSetUnlinkOld ( hSearchd.GetInt ( "unlink_old", 1 )!=0 );
	g_iExpansionLimit = hSearchd.GetInt ( "expansion_limit", 0 );
	g_bOnDiskAttrs = ( hSearchd.GetInt ( "ondisk_attrs_default", 0 )==1 );
	g_bOnDiskPools = ( strcmp ( hSearchd.GetStr ( "ondisk_attrs_default", "" ), "pool" )==0 );

	if ( hSearchd("subtree_docs_cache") )
		g_iMaxCachedDocs = hSearchd.GetSize ( "subtree_docs_cache", g_iMaxCachedDocs );

	if ( hSearchd("subtree_hits_cache") )
		g_iMaxCachedHits = hSearchd.GetSize ( "subtree_hits_cache", g_iMaxCachedHits );

	if ( hSearchd("seamless_rotate") )
		g_bSeamlessRotate = ( hSearchd["seamless_rotate"].intval()!=0 );

	if ( hSearchd ( "grouping_in_utc" ) )
	{
		g_bGroupingInUtc = (hSearchd["grouping_in_utc"].intval ()!=0);
		setGroupingInUtc ( g_bGroupingInUtc );
	}

	// sha1 password hash for shutdown action
	g_sShutdownToken = hSearchd.GetStr ("shutdown_token");

	if ( !g_bSeamlessRotate && g_bPreopenIndexes )
		sphWarning ( "preopen_indexes=1 has no effect with seamless_rotate=0" );

	g_iAttrFlushPeriod = hSearchd.GetInt ( "attr_flush_period", g_iAttrFlushPeriod );
	g_iMaxPacketSize = hSearchd.GetSize ( "max_packet_size", g_iMaxPacketSize );
	g_iMaxFilters = hSearchd.GetInt ( "max_filters", g_iMaxFilters );
	g_iMaxFilterValues = hSearchd.GetInt ( "max_filter_values", g_iMaxFilterValues );
	g_iMaxBatchQueries = hSearchd.GetInt ( "max_batch_queries", g_iMaxBatchQueries );
	g_iDistThreads = hSearchd.GetInt ( "dist_threads", g_iDistThreads );
	sphSetThrottling ( hSearchd.GetInt ( "rt_merge_iops", 0 ), hSearchd.GetSize ( "rt_merge_maxiosize", 0 ) );
	g_iPingInterval = hSearchd.GetInt ( "ha_ping_interval", 1000 );
	g_uHAPeriodKarma = hSearchd.GetInt ( "ha_period_karma", 60 );
	g_iQueryLogMinMsec = hSearchd.GetInt ( "query_log_min_msec", g_iQueryLogMinMsec );
	g_iAgentConnectTimeout = hSearchd.GetInt ( "agent_connect_timeout", g_iAgentConnectTimeout );
	g_iAgentQueryTimeout = hSearchd.GetInt ( "agent_query_timeout", g_iAgentQueryTimeout );
	g_iAgentRetryDelay = hSearchd.GetInt ( "agent_retry_delay", g_iAgentRetryDelay );
	if ( g_iAgentRetryDelay > MAX_RETRY_DELAY )
		sphWarning ( "agent_retry_delay %d exceeded max recommended %d", g_iAgentRetryDelay, MAX_RETRY_DELAY );
	g_iAgentRetryCount = hSearchd.GetInt ( "agent_retry_count", g_iAgentRetryCount );
	if ( g_iAgentRetryCount > MAX_RETRY_COUNT )
		sphWarning ( "agent_retry_count %d exceeded max recommended %d", g_iAgentRetryCount, MAX_RETRY_COUNT );
	g_tmWait = hSearchd.GetInt ( "net_wait_tm", g_tmWait );
	g_iThrottleAction = hSearchd.GetInt ( "net_throttle_action", g_iThrottleAction );
	g_iThrottleAccept = hSearchd.GetInt ( "net_throttle_accept", g_iThrottleAccept );
	g_iNetWorkers = hSearchd.GetInt ( "net_workers", g_iNetWorkers );
	g_iNetWorkers = Max ( g_iNetWorkers, 1 );
	CheckSystemTFO();
	if ( g_iTFO!=TFO_ABSENT && hSearchd.GetInt ( "listen_tfo" )==0 )
	{
		g_iTFO &= ~TFO_LISTEN;
	}
	if ( hSearchd ( "collation_libc_locale" ) )
	{
		const char * sLocale = hSearchd.GetStr ( "collation_libc_locale" );
		if ( !setlocale ( LC_COLLATE, sLocale ) )
			sphWarning ( "setlocale failed (locale='%s')", sLocale );
	}

	if ( hSearchd ( "collation_server" ) )
	{
		CSphString sCollation = hSearchd.GetStr ( "collation_server" );
		CSphString sError;
		g_eCollation = sphCollationFromName ( sCollation, &sError );
		if ( !sError.IsEmpty() )
			sphWarning ( "%s", sError.cstr() );
	}

	if ( hSearchd("thread_stack") )
	{
		int iThreadStackSizeMin = 65536;
		int iThreadStackSizeMax = 8*1024*1024;
		int iStackSize = hSearchd.GetSize ( "thread_stack", iThreadStackSizeMin );
		if ( iStackSize<iThreadStackSizeMin || iStackSize>iThreadStackSizeMax )
			sphWarning ( "thread_stack %d out of bounds (64K..8M); clamped", iStackSize );

		iStackSize = Min ( iStackSize, iThreadStackSizeMax );
		iStackSize = Max ( iStackSize, iThreadStackSizeMin );
		sphSetMyStackSize ( iStackSize );
	}

	if ( hSearchd("predicted_time_costs") )
		ParsePredictedTimeCosts ( hSearchd["predicted_time_costs"].cstr() );

	if ( hSearchd("shutdown_timeout") )
	{
		int iTimeout = hSearchd.GetInt ( "shutdown_timeout", 0 );
		if ( iTimeout )
			g_iShutdownTimeout = iTimeout * 1000000;
	}

	if ( hSearchd.Exists ( "max_open_files" ) )
	{
#if HAVE_GETRLIMIT & HAVE_SETRLIMIT
		auto uLimit = ( rlim_t ) hSearchd["max_open_files"].intval ();
		bool bMax = hSearchd["max_open_files"].strval ()=="max";
		if ( !uLimit && !bMax )
			sphWarning ( "max_open_files is %d, expected positive value; ignored", (int) uLimit );
		else
		{
			struct rlimit dRlimit;
			if ( 0!=getrlimit ( RLIMIT_NOFILE, &dRlimit ) )
				sphWarning ( "Failed to getrlimit (RLIMIT_NOFILE), error %d: %s", errno, strerrorm ( errno ) );
			else
			{
				auto uPrevLimit = dRlimit.rlim_cur;
				if ( bMax )
					uLimit = dRlimit.rlim_max;
				dRlimit.rlim_cur = Min ( dRlimit.rlim_max, uLimit );
				if ( 0!=setrlimit ( RLIMIT_NOFILE, &dRlimit ) )
					sphWarning ( "Failed to setrlimit on %d, error %d: %s", (int)uLimit, errno, strerrorm ( errno ) );
				else
					sphInfo ( "Set max_open_files to %d (previous was %d), hardlimit is %d.",
						(int)uLimit, (int)uPrevLimit, (int)dRlimit.rlim_max );
			}
		}
#else
		sphWarning ("max_open_files defined, but this binary don't know about setrlimit() function");
#endif
	}

	QcacheStatus_t s = QcacheGetStatus();
	s.m_iMaxBytes = hSearchd.GetSize64 ( "qcache_max_bytes", s.m_iMaxBytes );
	s.m_iThreshMsec = hSearchd.GetInt ( "qcache_thresh_msec", s.m_iThreshMsec );
	s.m_iTtlSec = hSearchd.GetInt ( "qcache_ttl_sec", s.m_iTtlSec );
	QcacheSetup ( s.m_iMaxBytes, s.m_iThreshMsec, s.m_iTtlSec );

	// hostname_lookup = {config_load | request}
	g_bHostnameLookup = ( strcmp ( hSearchd.GetStr ( "hostname_lookup", "" ), "request" )==0 );

	CSphVariant * pLogMode = hSearchd ( "query_log_mode" );
	if ( pLogMode && !pLogMode->strval().IsEmpty() )
	{
		errno = 0;
		int iMode = strtol ( pLogMode->strval().cstr(), NULL, 8 );
		int iErr = errno;
		if ( iErr==ERANGE || iErr==EINVAL )
		{
			sphWarning ( "query_log_mode invalid value (value=%o, error=%s); skipped", iMode, strerrorm(iErr) );
		} else
		{
			g_iLogFileMode = iMode;
		}
	}

	//////////////////////////////////////////////////
	// prebuild MySQL wire protocol handshake packets
	//////////////////////////////////////////////////

	char sHandshake1[] =
		"\x00\x00\x00" // packet length
		"\x00" // packet id
		"\x0A"; // protocol version; v.10

	char sHandshake2[] =
		"\x01\x00\x00\x00" // thread id
		"\x01\x02\x03\x04\x05\x06\x07\x08" // salt1 (for auth)
		"\x00" // filler
		"\x08\x82" // server capabilities low WORD; CLIENT_PROTOCOL_41 | CLIENT_CONNECT_WITH_DB | CLIENT_SECURE_CONNECTION
		"\x21" // server language; let it be ut8_general_ci to make different clients happy
		"\x02\x00" // server status
		"\x00\x00" // server capabilities hi WORD; no CLIENT_PLUGIN_AUTH
		"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" // filler
		"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c" // salt2 (for auth, 4.1+)
  		"\x00"; // filler

	g_sMySQLVersion = hSearchd.GetStr ( "mysql_version_string", SPHINX_VERSION );
	int iLen = g_sMySQLVersion.Length();

	g_iMysqlHandshake = sizeof(sHandshake1) + iLen + sizeof(sHandshake2) - 1;
	if ( g_iMysqlHandshake>=(int)sizeof(g_sMysqlHandshake) )
	{
		sphWarning ( "mysql_version_string too long; using default (version=%s)", SPHINX_VERSION );
		g_iMysqlHandshake = sizeof(sHandshake1) + strlen(SPHINX_VERSION) + sizeof(sHandshake2) - 1;
		assert ( g_iMysqlHandshake < (int)sizeof(g_sMysqlHandshake) );
	}

	char * p = g_sMysqlHandshake;
	memcpy ( p, sHandshake1, sizeof(sHandshake1)-1 );
	memcpy ( p+sizeof(sHandshake1)-1, g_sMySQLVersion.cstr(), iLen+1 );
	memcpy ( p+sizeof(sHandshake1)+iLen, sHandshake2, sizeof(sHandshake2)-1 );
	g_sMysqlHandshake[0] = (char)(g_iMysqlHandshake-4); // safe, as long as buffer size is 128
}

ESphAddIndex ConfigureAndPreload ( const CSphConfigSection & hIndex, const char * sIndexName, bool bJson )
	REQUIRES ( MainThread )
{
	ESphAddIndex eAdd = AddIndex ( sIndexName, hIndex );

	// local plain, rt, percolate added, but need to be at least prealloced before they could work.
	if ( eAdd==ADD_DSBLED )
	{
		auto pHandle = GetDisabled ( sIndexName );
		ServedDescWPtr_c pJustAddedLocalWl ( pHandle );
		pJustAddedLocalWl->m_bJson = bJson;

		fprintf ( stdout, "precaching index '%s'\n", sIndexName );
		fflush ( stdout );

		IndexFiles_c dJustAddedFiles ( pJustAddedLocalWl->m_sIndexPath );
		bool bPreloadOk = true;
		if ( dJustAddedFiles.HasAllFiles ( ".new" ) )
		{
			pJustAddedLocalWl->m_bOnlyNew = !dJustAddedFiles.HasAllFiles();
			CSphString sError;
			if ( RotateIndexGreedy ( pHandle, *pJustAddedLocalWl, sIndexName, sError ) )
			{
				bPreloadOk = sphFixupIndexSettings ( pJustAddedLocalWl->m_pIndex, hIndex, sError, g_bStripPath );
				if ( !bPreloadOk )
				{
					sphWarning ( "index '%s': %s - NOT SERVING", sIndexName, sError.cstr() );
				}

			} else
			{
				pJustAddedLocalWl->m_bOnlyNew = false;
				sphWarning ( "%s", sError.cstr() );
				bPreloadOk = PreallocNewIndex ( *pJustAddedLocalWl, &hIndex, sIndexName );
			}

		} else
		{
			bPreloadOk = PreallocNewIndex ( *pJustAddedLocalWl, &hIndex, sIndexName );
		}

		if ( !bPreloadOk )
		{
			g_pLocalIndexes->DeleteIfNull ( sIndexName );
			return ADD_ERROR;
		}

		// finally add the index to the hash of enabled.
		g_pLocalIndexes->AddOrReplace ( pHandle, sIndexName );
		pHandle->AddRef ();

		CSphString sError;
		if ( !pJustAddedLocalWl->m_sGlobalIDFPath.IsEmpty() && !sphPrereadGlobalIDF ( pJustAddedLocalWl->m_sGlobalIDFPath, sError ) )
			sphWarning ( "index '%s': global IDF unavailable - IGNORING", sIndexName );
	}

	return eAdd;
}

// invoked once on start from ServiceMain (actually it creates the hashes)
void ConfigureAndPreload ( const CSphConfig & hConf, const StrVec_t & dOptIndexes ) REQUIRES (MainThread)
{
	int iCounter = 0;
	int iValidIndexes = 0;
	int64_t tmLoad = -sphMicroTimer();

	g_pDisabledIndexes->ReleaseAndClear ();

	if ( hConf.Exists ( "index" ) )
	{
		hConf["index"].IterateStart ();
		while ( hConf["index"].IterateNext() )
		{
			const CSphConfigSection & hIndex = hConf["index"].IterateGet();
			const char * sIndexName = hConf["index"].IterateGetKey().cstr();

			if ( !dOptIndexes.IsEmpty() && !dOptIndexes.FindFirst ( [&] ( const CSphString &rhs )	{ return rhs.EqN ( sIndexName ); } ) )
				continue;

			ESphAddIndex eAdd = ConfigureAndPreload ( hIndex, sIndexName, false );
			iValidIndexes += ( eAdd!=ADD_ERROR ? 1 : 0 );
			iCounter +=  ( eAdd==ADD_DSBLED ? 1 : 0 );
		}
	}

	JsonConfigConfigureAndPreload ( iValidIndexes, iCounter );

	// we don't have any more unprocessed disabled indexes during startup
	g_pDisabledIndexes->ReleaseAndClear ();

	InitPersistentPool();

	tmLoad += sphMicroTimer();
	if ( !iValidIndexes )
		sphWarning ( "no valid indexes to serve" );
	else
		fprintf ( stdout, "precached %d indexes in %0.3f sec\n", iCounter, float(tmLoad)/1000000 );
}

void OpenDaemonLog ( const CSphConfigSection & hSearchd, bool bCloseIfOpened=false )
{
	// create log
		const char * sLog = "searchd.log";
		if ( hSearchd.Exists ( "log" ) )
		{
			if ( hSearchd["log"]=="syslog" )
			{
#if !USE_SYSLOG
				if ( g_iLogFile<0 )
				{
					g_iLogFile = STDOUT_FILENO;
					sphWarning ( "failed to use syslog for logging. You have to reconfigure --with-syslog and rebuild the daemon!" );
					sphInfo ( "will use default file 'searchd.log' for logging." );
				}
#else
				g_bLogSyslog = true;
#endif
			} else
			{
				sLog = hSearchd["log"].cstr();
			}
		}

		umask ( 066 );
		if ( bCloseIfOpened && g_iLogFile!=STDOUT_FILENO )
		{
			close ( g_iLogFile );
			g_iLogFile = STDOUT_FILENO;
		}
		if ( !g_bLogSyslog )
		{
			g_iLogFile = open ( sLog, O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
			if ( g_iLogFile<0 )
			{
				g_iLogFile = STDOUT_FILENO;
				sphFatal ( "failed to open log file '%s': %s", sLog, strerrorm(errno) );
			}
			LogChangeMode ( g_iLogFile, g_iLogFileMode );
		}

		g_sLogFile = sLog;
		g_bLogTty = isatty ( g_iLogFile )!=0;
}

int WINAPI ServiceMain ( int argc, char **argv ) REQUIRES (!MainThread)
{
	ScopedRole_c thMain (MainThread);
	g_bLogTty = isatty ( g_iLogFile )!=0;

#ifdef USE_VTUNE
	__itt_pause ();
#endif // USE_VTUNE
	g_tmStarted = sphMicroTimer();

#if USE_WINDOWS
	CSphVector<char *> dArgs;
	if ( g_bService )
	{
		g_ssHandle = RegisterServiceCtrlHandler ( g_sServiceName, ServiceControl );
		if ( !g_ssHandle )
			sphFatal ( "failed to start service: RegisterServiceCtrlHandler() failed: %s", WinErrorInfo() );

		g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		MySetServiceStatus ( SERVICE_START_PENDING, NO_ERROR, 4000 );

		if ( argc<=1 )
		{
			dArgs.Resize ( g_dArgs.GetLength() );
			ARRAY_FOREACH ( i, g_dArgs )
				dArgs[i] = (char*) g_dArgs[i].cstr();

			argc = g_dArgs.GetLength();
			argv = &dArgs[0];
		}
	}

	char szPipeName[64];
	snprintf ( szPipeName, sizeof(szPipeName), "\\\\.\\pipe\\searchd_%d", getpid() );
	g_hPipe = CreateNamedPipe ( szPipeName, PIPE_ACCESS_INBOUND,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
		PIPE_UNLIMITED_INSTANCES, 0, WIN32_PIPE_BUFSIZE, NMPWAIT_NOWAIT, NULL );
	ConnectNamedPipe ( g_hPipe, NULL );
#endif

	tzset();

	if ( !g_bService )
		fprintf ( stdout, SPHINX_BANNER );

	const char* sEndian = sphCheckEndian();
	if ( sEndian )
		sphDie ( "%s", sEndian );

	//////////////////////
	// parse command line
	//////////////////////

	CSphConfig		conf;
	bool			bOptStop = false;
	bool			bOptStopWait = false;
	bool			bOptStatus = false;
	bool			bOptPIDFile = false;
	StrVec_t		dOptIndexes; // indexes explicitly pointed in cmdline options

	int				iOptPort = 0;
	bool			bOptPort = false;

	CSphString		sOptListen;
	bool			bOptListen = false;
	bool			bTestMode = false;
	bool			bTestThdPoolMode = false;
	bool			bOptDebugQlog = true;
	bool			bForcedPreread = false;
	bool			bNewCluster = false;
	bool			bNewClusterForce = false;

	DWORD			uReplayFlags = 0;

	#define OPT(_a1,_a2)	else if ( !strcmp(argv[i],_a1) || !strcmp(argv[i],_a2) )
	#define OPT1(_a1)		else if ( !strcmp(argv[i],_a1) )

	int i;
	for ( i=1; i<argc; i++ )
	{
		// handle non-options
		if ( argv[i][0]!='-' )		break;

		// handle no-arg options
		OPT ( "-h", "--help" )		{ ShowHelp(); return 0; }
		OPT ( "-?", "--?" )			{ ShowHelp(); return 0; }
		OPT1 ( "-v" )				{ return 0; }
		OPT1 ( "--console" )		{ g_bOptNoLock = true; g_bOptNoDetach = true; bTestMode = true; }
		OPT1 ( "--stop" )			bOptStop = true;
		OPT1 ( "--stopwait" )		{ bOptStop = true; bOptStopWait = true; }
		OPT1 ( "--status" )			bOptStatus = true;
		OPT1 ( "--pidfile" )		bOptPIDFile = true;
		OPT1 ( "--iostats" )		g_bIOStats = true;
		OPT1 ( "--cpustats" )		g_bCpuStats = true;
#if USE_WINDOWS
		OPT1 ( "--install" )		{ if ( !g_bService ) { ServiceInstall ( argc, argv ); return 0; } }
		OPT1 ( "--delete" )			{ if ( !g_bService ) { ServiceDelete (); return 0; } }
		OPT1 ( "--ntservice" )		{} // it's valid but handled elsewhere
#else
		OPT1 ( "--nodetach" )		g_bOptNoDetach = true;
#endif
		OPT1 ( "--logdebug" )		g_eLogLevel = SPH_LOG_DEBUG;
		OPT1 ( "--logdebugv" )		g_eLogLevel = SPH_LOG_VERBOSE_DEBUG;
		OPT1 ( "--logdebugvv" )		g_eLogLevel = SPH_LOG_VERY_VERBOSE_DEBUG;
		OPT1 ( "--logreplication" )	g_eLogLevel = SPH_LOG_RPL_DEBUG;
		OPT1 ( "--safetrace" )		g_bSafeTrace = true;
		OPT1 ( "--test" )			{ g_bWatchdog = false; bTestMode = true; } // internal option, do NOT document
		OPT1 ( "--test-thd-pool" )	{ g_bWatchdog = false; bTestMode = true; bTestThdPoolMode = true; } // internal option, do NOT document
		OPT1 ( "--strip-path" )		g_bStripPath = true;
		OPT1 ( "--vtune" )			g_bVtune = true;
		OPT1 ( "--noqlog" )			bOptDebugQlog = false;
		OPT1 ( "--force-preread" )	bForcedPreread = true;
		OPT1 ( "--coredump" )		g_bCoreDump = true;
		OPT1 ( "--new-cluster" )	bNewCluster = true;
		OPT1 ( "--new-cluster-force" )	bNewClusterForce = true;

		// FIXME! add opt=(csv)val handling here
		OPT1 ( "--replay-flags=accept-desc-timestamp" )		uReplayFlags |= SPH_REPLAY_ACCEPT_DESC_TIMESTAMP;
		OPT1 ( "--replay-flags=ignore-open-errors" )			uReplayFlags |= SPH_REPLAY_IGNORE_OPEN_ERROR;

		// handle 1-arg options
		else if ( (i+1)>=argc )		break;
		OPT ( "-c", "--config" )	g_sConfigFile = argv[++i];
		OPT ( "-p", "--port" )		{ bOptPort = true; iOptPort = atoi ( argv[++i] ); }
		OPT ( "-l", "--listen" )	{ bOptListen = true; sOptListen = argv[++i]; }
		OPT ( "-i", "--index" )		dOptIndexes.Add ( argv[++i] );
#if USE_WINDOWS
		OPT1 ( "--servicename" )	++i; // it's valid but handled elsewhere
#endif

		// handle unknown options
		else
			break;
	}
	if ( i!=argc )
		sphFatal ( "malformed or unknown option near '%s'; use '-h' or '--help' to see available options.", argv[i] );

#if USE_WINDOWS
	// init WSA on Windows
	// we need to do it this early because otherwise gethostbyname() from config parser could fail
	WSADATA tWSAData;
	int iStartupErr = WSAStartup ( WINSOCK_VERSION, &tWSAData );
	if ( iStartupErr )
		sphFatal ( "failed to initialize WinSock2: %s", sphSockError ( iStartupErr ) );

	if ( !LoadExFunctions () )
		sphFatal ( "failed to initialize extended socket functions: %s", sphSockError ( iStartupErr ) );


	// i want my windows sessions to log onto stdout
	// both in Debug and Release builds
	if ( !g_bService )
		g_bOptNoDetach = true;

#ifndef NDEBUG
	// i also want my windows debug builds to skip locking by default
	// NOTE, this also skips log files!
	g_bOptNoLock = true;
#endif
#endif

	if ( !bOptPIDFile )
		bOptPIDFile = !g_bOptNoLock;

	// check port and listen arguments early
	if ( !g_bOptNoDetach && ( bOptPort || bOptListen ) )
	{
		sphWarning ( "--listen and --port are only allowed in --console debug mode; switch ignored" );
		bOptPort = bOptListen = false;
	}

	if ( bOptPort )
	{
		if ( bOptListen )
			sphFatal ( "please specify either --port or --listen, not both" );

		CheckPort ( iOptPort );
	}

	/////////////////////
	// parse config file
	/////////////////////

	// fallback to defaults if there was no explicit config specified
	while ( !g_sConfigFile.cstr() )
	{
#ifdef SYSCONFDIR
		g_sConfigFile = SYSCONFDIR "/sphinx.conf";
		if ( sphIsReadable ( g_sConfigFile.cstr () ) )
			break;
#endif

		g_sConfigFile = "./sphinx.conf";
		if ( sphIsReadable ( g_sConfigFile.cstr () ) )
			break;

		g_sConfigFile = NULL;
		break;
	}

	if ( !g_sConfigFile.cstr () )
		sphFatal ( "no readable config file (looked in "
#ifdef SYSCONFDIR
			SYSCONFDIR "/sphinx.conf, "
#endif
			"./sphinx.conf)." );

	sphInfo ( "using config file '%s'...", g_sConfigFile.cstr () );

	CheckConfigChanges ();

	{
		ScWL_t dWLock { g_tRotateConfigMutex };
		// do parse
		if ( !g_pCfg.Parse ( g_sConfigFile.cstr () ) )
			sphFatal ( "failed to parse config file '%s'", g_sConfigFile.cstr () );
	}

	// strictly speaking we must hold g_tRotateConfigMutex all the way we work with config.
	// but in this very case we're starting in single main thread, no concurrency yet.
	const CSphConfig & hConf = g_pCfg.m_tConf;

	if ( !hConf.Exists ( "searchd" ) || !hConf["searchd"].Exists ( "searchd" ) )
		sphFatal ( "'searchd' config section not found in '%s'", g_sConfigFile.cstr () );

	const CSphConfigSection & hSearchdpre = hConf["searchd"]["searchd"];

	CSphString sError;
	if ( !sphInitCharsetAliasTable ( sError ) )
		sphFatal ( "failed to init charset alias table: %s", sError.cstr() );

	////////////////////////
	// stop running searchd
	////////////////////////

	if ( bOptStop )
	{
		if ( !hSearchdpre("pid_file") )
			sphFatal ( "stop: option 'pid_file' not found in '%s' section 'searchd'", g_sConfigFile.cstr () );

		const char * sPid = hSearchdpre["pid_file"].cstr(); // shortcut
		FILE * fp = fopen ( sPid, "r" );
		if ( !fp )
			sphFatal ( "stop: pid file '%s' does not exist or is not readable", sPid );

		char sBuf[16];
		int iLen = (int) fread ( sBuf, 1, sizeof(sBuf)-1, fp );
		sBuf[iLen] = '\0';
		fclose ( fp );

		int iPid = atoi(sBuf);
		if ( iPid<=0 )
			sphFatal ( "stop: failed to read valid pid from '%s'", sPid );

		int iWaitTimeout = g_iShutdownTimeout + 100000;

#if USE_WINDOWS
		bool bTerminatedOk = false;

		snprintf ( szPipeName, sizeof(szPipeName), "\\\\.\\pipe\\searchd_%d", iPid );

		HANDLE hPipe = INVALID_HANDLE_VALUE;

		while ( hPipe==INVALID_HANDLE_VALUE )
		{
			hPipe = CreateFile ( szPipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );

			if ( hPipe==INVALID_HANDLE_VALUE )
			{
				if ( GetLastError()!=ERROR_PIPE_BUSY )
				{
					fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
					break;
				}

				if ( !WaitNamedPipe ( szPipeName, iWaitTimeout/1000 ) )
				{
					fprintf ( stdout, "WARNING: could not open pipe (GetLastError()=%d)\n", GetLastError () );
					break;
				}
			}
		}

		if ( hPipe!=INVALID_HANDLE_VALUE )
		{
			DWORD uWritten = 0;
			BYTE uWrite = 1;
			BOOL bResult = WriteFile ( hPipe, &uWrite, 1, &uWritten, NULL );
			if ( !bResult )
				fprintf ( stdout, "WARNING: failed to send SIGHTERM to searchd (pid=%d, GetLastError()=%d)\n", iPid, GetLastError () );

			bTerminatedOk = !!bResult;

			CloseHandle ( hPipe );
		}

		if ( bTerminatedOk )
		{
			sphInfo ( "stop: successfully terminated pid %d", iPid );
			exit ( 0 );
		} else
			sphFatal ( "stop: error terminating pid %d", iPid );
#else
		CSphString sPipeName;
		int iPipeCreated = -1;
		int fdPipe = -1;
		if ( bOptStopWait )
		{
			sPipeName = GetNamedPipeName ( iPid );
			::unlink ( sPipeName.cstr () ); // avoid garbage to pollute us
			iPipeCreated = mkfifo ( sPipeName.cstr(), 0666 );
			if ( iPipeCreated!=-1 )
				fdPipe = ::open ( sPipeName.cstr(), O_RDONLY | O_NONBLOCK );

			if ( iPipeCreated==-1 )
				sphWarning ( "mkfifo failed (path=%s, err=%d, msg=%s); will NOT wait", sPipeName.cstr(), errno, strerrorm(errno) );
			else if ( fdPipe<0 )
				sphWarning ( "open failed (path=%s, err=%d, msg=%s); will NOT wait", sPipeName.cstr(), errno, strerrorm(errno) );
		}

		if ( kill ( iPid, SIGTERM ) )
			sphFatal ( "stop: kill() on pid %d failed: %s", iPid, strerrorm(errno) );
		else
			sphInfo ( "stop: successfully sent SIGTERM to pid %d", iPid );

		int iExitCode = ( bOptStopWait && ( iPipeCreated==-1 || fdPipe<0 ) ) ? 1 : 0;
		bool bHandshake = true;
		if ( bOptStopWait && fdPipe>=0 )
			while ( true )
			{
				int iReady = sphPoll ( fdPipe, iWaitTimeout );

				// error on wait
				if ( iReady<0 )
				{
					iExitCode = 3;
					sphWarning ( "stopwait%s error '%s'", ( bHandshake ? " handshake" : " " ), strerrorm(errno) );
					break;
				}

				// timeout
				if ( iReady==0 )
				{
					if ( !bHandshake )
						continue;

					iExitCode = 1;
					break;
				}

				// reading data
				DWORD uStatus = 0;
				int iRead = ::read ( fdPipe, &uStatus, sizeof(DWORD) );
				if ( iRead!=sizeof(DWORD) )
				{
					sphWarning ( "stopwait read fifo error '%s'", strerrorm(errno) );
					iExitCode = 3; // stopped demon crashed during stop
					break;
				} else
				{
					iExitCode = ( uStatus==1 ? 0 : 2 ); // uStatus == 1 - AttributeSave - ok, other values - error
				}

				if ( !bHandshake )
					break;

				bHandshake = false;
			}
		::unlink ( sPipeName.cstr () ); // is ok on linux after it is opened.

		if ( fdPipe>=0 )
			::close ( fdPipe );

		exit ( iExitCode );
#endif
	}

	////////////////////////////////
	// query running searchd status
	////////////////////////////////

	if ( bOptStatus )
	{
		QueryStatus ( hSearchdpre("listen") );
		exit ( 0 );
	}

	/////////////////////
	// configure searchd
	/////////////////////

	sphInitCJson();
	JsonLoadConfig ( hSearchdpre );

	ConfigureSearchd ( hConf, bOptPIDFile );
	sphConfigureCommon ( hConf ); // this also inits plugins now

	g_bWatchdog = hSearchdpre.GetInt ( "watchdog", g_bWatchdog )!=0;

	bool bThdPool = true;
	if ( hSearchdpre("workers") )
	{
		if ( hSearchdpre["workers"]=="threads" )
		{
			bThdPool = false;
		} else if ( hSearchdpre["workers"]=="thread_pool" )
		{
			bThdPool = true;
		}
	}

	if ( bThdPool || bTestThdPoolMode )
	{
#if HAVE_POLL || HAVE_EPOLL
		bThdPool = true;
#else
		bThdPool = false;
		sphWarning ( "no poll or epoll found, thread pool unavailable, going back to thread workers" );
#endif
	}
	if ( g_iMaxPacketSize<128*1024 || g_iMaxPacketSize>128*1024*1024 )
		sphFatal ( "max_packet_size out of bounds (128K..128M)" );

	if ( g_iMaxFilters<1 || g_iMaxFilters>10240 )
		sphFatal ( "max_filters out of bounds (1..10240)" );

	if ( g_iMaxFilterValues<1 || g_iMaxFilterValues>10485760 )
		sphFatal ( "max_filter_values out of bounds (1..10485760)" );

	bool bVisualLoad = true;
	bool bWatched = false;
#if !USE_WINDOWS
	// Let us start watchdog right now, on foreground first.
	int iDevNull = open ( "/dev/null", O_RDWR );
	if ( g_bWatchdog && !g_bOptNoDetach )
	{
		bWatched = true;
		if ( !g_bOptNoLock )
			OpenDaemonLog ( hConf["searchd"]["searchd"] );
		bVisualLoad = SetWatchDog ( iDevNull );
		OpenDaemonLog ( hConf["searchd"]["searchd"], true ); // just the 'IT Happens' magic - switch off, then on.
	}
#endif

	// here we either since plain startup, either being resurrected (forked) by watchdog.
	// create the pid
	if ( bOptPIDFile )
	{
		g_sPidFile = hSearchdpre["pid_file"].cstr();

		g_iPidFD = ::open ( g_sPidFile.scstr(), O_CREAT | O_WRONLY, S_IREAD | S_IWRITE );
		if ( g_iPidFD<0 )
			sphFatal ( "failed to create pid file '%s': %s", g_sPidFile.scstr(), strerrorm(errno) );
	}
	if ( bOptPIDFile && !sphLockEx ( g_iPidFD, false ) )
		sphFatal ( "failed to lock pid file '%s': %s (searchd already running?)", g_sPidFile.scstr(), strerrorm(errno) );

	g_bPidIsMine = true;

	// Actions on resurrection
	if ( bWatched && !bVisualLoad && CheckConfigChanges() )
	{
		// reparse the config file
		sphInfo ( "Reloading the config" );
		ScWL_t dWLock { g_tRotateConfigMutex };
		if ( !g_pCfg.ReParse ( g_sConfigFile.cstr () ) )
			sphFatal ( "failed to parse config file '%s'", g_sConfigFile.cstr () );

		sphInfo ( "Reconfigure the daemon" );
		ConfigureSearchd ( hConf, bOptPIDFile );
	}

	// hSearchdpre might be dead if we reloaded the config.
	const CSphConfigSection & hSearchd = hConf["searchd"]["searchd"];

	// handle my signals
	SetSignalHandlers ( g_bOptNoDetach );
	CrashQuerySetupHandlers ( SphCrashLogger_c::SetTopQueryTLS, SphCrashLogger_c::GetQuery, SphCrashLogger_c::SetLastQuery );

	// create logs
	if ( !g_bOptNoLock )
	{
		// create log
		OpenDaemonLog ( hSearchd, true );

		// create query log if required
		if ( hSearchd.Exists ( "query_log" ) )
		{
			if ( hSearchd["query_log"]=="syslog" )
				g_bQuerySyslog = true;
			else
			{
				g_iQueryLogFile = open ( hSearchd["query_log"].cstr(), O_CREAT | O_RDWR | O_APPEND, S_IREAD | S_IWRITE );
				if ( g_iQueryLogFile<0 )
					sphFatal ( "failed to open query log file '%s': %s", hSearchd["query_log"].cstr(), strerrorm(errno) );

				LogChangeMode ( g_iQueryLogFile, g_iLogFileMode );
			}
			g_sQueryLogFile = hSearchd["query_log"].cstr();
		}
	}

#if !USE_WINDOWS
	if ( !g_bOptNoDetach && !bWatched )
	{
		switch ( fork () )
		{
			case -1:
			// error
			sphFatalLog ( "fork() failed (reason: %s)", strerrorm ( errno ) );
			exit ( 1 );

			case 0:
			// daemonized child
			break;

			default:
			// tty-controlled parent
			exit ( 0 );
		}
	}
#endif

	////////////////////
	// network startup
	////////////////////

	Listener_t tListener;
	tListener.m_eProto = PROTO_SPHINX;
	tListener.m_bTcp = true;

	// command line arguments override config (but only in --console)
	if ( bOptListen )
	{
		AddListener ( sOptListen, bThdPool );

	} else if ( bOptPort )
	{
		tListener.m_iSock = sphCreateInetSocket ( htonl ( INADDR_ANY ), iOptPort );
		g_dListeners.Add ( tListener );

	} else
	{
		// listen directives in configuration file
		for ( CSphVariant * v = hSearchd("listen"); v; v = v->m_pNext )
			AddListener ( v->strval(), bThdPool );

		// default is to listen on our two ports
		if ( !g_dListeners.GetLength() )
		{
			tListener.m_iSock = sphCreateInetSocket ( htonl ( INADDR_ANY ), SPHINXAPI_PORT );
			tListener.m_eProto = PROTO_SPHINX;
			g_dListeners.Add ( tListener );

			tListener.m_iSock = sphCreateInetSocket ( htonl ( INADDR_ANY ), SPHINXQL_PORT );
			tListener.m_eProto = PROTO_MYSQL41;
			g_dListeners.Add ( tListener );
		}
	}

	//////////////////////
	// build indexes hash
	//////////////////////

	// setup mva updates arena here, since we could have saved persistent mva updates
	const char * sArenaError = sphArenaInit ( hSearchd.GetSize ( "mva_updates_pool", MVA_UPDATES_POOL ) );
	if ( sArenaError )
		sphWarning ( "process shared mutex unsupported, MVA update disabled ( %s )", sArenaError );

	// configure and preload

	if ( bTestMode ) // pass this flag here prior to index config
		sphRTSetTestMode();

	StrVec_t dExactIndexes;
	for ( const auto &dOptIndex : dOptIndexes )
		sphSplit ( dExactIndexes, dOptIndex.cstr (), "," );

	ConfigureAndPreload ( hConf, dExactIndexes );

	///////////
	// startup
	///////////

	sphRTInit ( hSearchd, bTestMode, hConf("common") ? hConf["common"]("common") : nullptr );

	if ( hSearchd.Exists ( "snippets_file_prefix" ) )
		g_sSnippetsFilePrefix = hSearchd["snippets_file_prefix"].cstr();

	const char* sLogFormat = hSearchd.GetStr ( "query_log_format", "plain" );
	if ( !strcmp ( sLogFormat, "sphinxql" ) )
		g_eLogFormat = LOG_FORMAT_SPHINXQL;
	else if ( strcmp ( sLogFormat, "plain" ) )
	{
		StrVec_t dParams;
		sphSplit ( dParams, sLogFormat );
		ARRAY_FOREACH ( j, dParams )
		{
			sLogFormat = dParams[j].cstr();
			if ( !strcmp ( sLogFormat, "sphinxql" ) )
				g_eLogFormat = LOG_FORMAT_SPHINXQL;
			else if ( !strcmp ( sLogFormat, "plain" ) )
				g_eLogFormat = LOG_FORMAT_PLAIN;
			else if ( !strcmp ( sLogFormat, "compact_in" ) )
				g_bLogCompactIn = true;
		}
	}
	if ( g_bLogCompactIn && g_eLogFormat==LOG_FORMAT_PLAIN )
		sphWarning ( "compact_in option only supported with query_log_format=sphinxql" );

	// prepare to detach
	if ( !g_bOptNoDetach )
	{
		ReleaseTTYFlag();

#if !USE_WINDOWS
		if ( !bWatched || bVisualLoad )
		{
			close ( STDIN_FILENO );
			close ( STDOUT_FILENO );
			close ( STDERR_FILENO );
			dup2 ( iDevNull, STDIN_FILENO );
			dup2 ( iDevNull, STDOUT_FILENO );
			dup2 ( iDevNull, STDERR_FILENO );
		}
#endif
	}

	if ( bOptPIDFile && !bWatched )
		sphLockUn ( g_iPidFD );

	sphRTConfigure ( hSearchd, bTestMode );
	SetPercolateQueryParserFactory ( PercolateQueryParserFactory );
	SetPercolateThreads ( g_iDistThreads );

	if ( bOptPIDFile )
	{
#if !USE_WINDOWS
		// re-lock pid
		// FIXME! there's a potential race here
		if ( !sphLockEx ( g_iPidFD, true ) )
			sphFatal ( "failed to re-lock pid file '%s': %s", g_sPidFile.scstr(), strerrorm(errno) );
#endif

		char sPid[16];
		snprintf ( sPid, sizeof(sPid), "%d\n", (int)getpid() );
		int iPidLen = strlen(sPid);

		sphSeek ( g_iPidFD, 0, SEEK_SET );
		if ( !sphWrite ( g_iPidFD, sPid, iPidLen ) )
			sphFatal ( "failed to write to pid file '%s' (errno=%d, msg=%s)", g_sPidFile.scstr(),
				errno, strerrorm(errno) );

		if ( ::ftruncate ( g_iPidFD, iPidLen ) )
			sphFatal ( "failed to truncate pid file '%s' (errno=%d, msg=%s)", g_sPidFile.scstr(),
				errno, strerrorm(errno) );
	}

#if USE_WINDOWS
	SetConsoleCtrlHandler ( CtrlHandler, TRUE );
#endif

	StrVec_t dFailed;
	if ( !g_bOptNoDetach && !bWatched && !g_bService )
	{
		// re-lock indexes
		for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); )
		{
			sphWarning ( "Relocking %s", it.GetName ().cstr () );
			ServedDescRPtr_c pServed ( it.Get() );
			// obtain exclusive lock
			if ( !pServed )
				dFailed.Add ( it.GetName() );
			else if ( !pServed->m_pIndex->Lock() )
			{
				sphWarning ( "index '%s': lock: %s; INDEX UNUSABLE", it.GetName ().cstr(), pServed->m_pIndex->GetLastError().cstr() );
				dFailed.Add ( it.GetName());
			}
		}
		for ( const auto& sFailed : dFailed )
			g_pLocalIndexes->Delete ( sFailed );
	}

	// if we're running in test console mode, dump queries to tty as well
	// unless we're explicitly asked not to!
	if ( hSearchd ( "query_log" ) && g_bOptNoLock && g_bOptNoDetach && bOptDebugQlog )
	{
		g_bQuerySyslog = false;
		g_bLogSyslog = false;
		g_iQueryLogFile = g_iLogFile;
	}

#if USE_SYSLOG
	if ( g_bLogSyslog || g_bQuerySyslog )
	{
		openlog ( "searchd", LOG_PID, LOG_DAEMON );
	}
#else
	if ( g_bQuerySyslog )
		sphFatal ( "Wrong query_log file! You have to reconfigure --with-syslog and rebuild daemon if you want to use syslog there." );
#endif
	/////////////////
	// serve clients
	/////////////////

	if ( bThdPool )
	{
		g_iThdQueueMax = hSearchd.GetInt ( "queue_max_length", g_iThdQueueMax );
		g_iThdPoolCount = Max ( 3*sphCpuThreadsCount()/2, 2 ); // default to 1.5*detected_cores but not less than 2 worker threads
		if ( hSearchd.Exists ( "max_children" ) && hSearchd["max_children"].intval()>0 )
			g_iThdPoolCount = hSearchd["max_children"].intval();
		g_pThdPool = sphThreadPoolCreate ( g_iThdPoolCount, "netloop", sError );
		if ( !g_pThdPool )
			sphDie ( "failed to create thread_pool: %s", sError.cstr() );
	}
#if USE_WINDOWS
	if ( g_bService )
		MySetServiceStatus ( SERVICE_RUNNING, NO_ERROR, 0 );
#endif

//	sphSetReadBuffers ( hSearchd.GetSize ( "read_buffer", 0 ), hSearchd.GetSize ( "read_unhinted", 0 ) );

	// in threaded mode, create a dedicated rotation thread
	if ( g_bSeamlessRotate && !g_tRotateThread.Create ( RotationThreadFunc, 0, "rotation" ) )
		sphDie ( "failed to create rotation thread" );

	// replay last binlog
	SmallStringHash_T<CSphIndex*> hIndexes;
	for ( RLockedServedIt_c it ( g_pLocalIndexes ); it.Next(); ) // FIXME!!!
	{
		ServedDescRPtr_c pLocked ( it.Get () );
		if ( pLocked )
			hIndexes.Add ( pLocked->m_pIndex, it.GetName () );
	}

	sphReplayBinlog ( hIndexes, uReplayFlags, DumpMemStat, g_tBinlogAutoflush );
	hIndexes.Reset();

	// no need to create another cluster on restart by watchdog resurrection
	if ( bWatched && !bVisualLoad )
	{
		bNewCluster = false;
		bNewClusterForce = false;
	}
	
	if ( g_tBinlogAutoflush.m_fnWork && !g_tBinlogFlushThread.Create ( RtBinlogAutoflushThreadFunc, 0, "binlog_flush" ) )
		sphDie ( "failed to create binlog flush thread" );

	if ( g_bIOStats && !sphInitIOStats () )
		sphWarning ( "unable to init IO statistics" );

	g_tStats.m_uStarted = (DWORD)time(NULL);

	// threads mode
	// create optimize and flush threads, and load saved sphinxql state
	if ( !g_tRtFlushThread.Create ( RtFlushThreadFunc, 0, "rt_flush" ) )
		sphDie ( "failed to create rt-flush thread" );

	if ( !g_tOptimizeThread.Create ( OptimizeThreadFunc, 0, "optimize" ) )
		sphDie ( "failed to create optimize thread" );

	g_sSphinxqlState = hSearchd.GetStr ( "sphinxql_state" );
	if ( !g_sSphinxqlState.IsEmpty() )
	{
		SphinxqlStateRead ( g_sSphinxqlState );
		g_tmSphinxqlState = sphMicroTimer();

		CSphWriter tWriter;
		CSphString sNewState;
		sNewState.SetSprintf ( "%s.new", g_sSphinxqlState.cstr() );
		// initial check that work can be done
		bool bCanWrite = tWriter.OpenFile ( sNewState, sError );
		tWriter.CloseFile();
		::unlink ( sNewState.cstr() );

		if ( !bCanWrite )
		{
			sphWarning ( "sphinxql_state flush disabled: %s", sError.cstr() );
			g_sSphinxqlState = ""; // need to disable thread join on shutdown 
		}
		else if ( !g_tSphinxqlStateFlushThread.Create ( SphinxqlStateThreadFunc, NULL, "sphinxql_state" ) )
			sphDie ( "failed to create sphinxql_state writer thread" );
	}

	if ( !g_tRotationServiceThread.Create ( RotationServiceThreadFunc, 0, "rotationservice" ) )
		sphDie ( "failed to create rotation service thread" );

	if ( !g_tPingThread.Create ( PingThreadFunc, 0, "ping_service" ) )
		sphDie ( "failed to create ping service thread" );

	if ( bForcedPreread )
	{
		PrereadFunc ( NULL );
	} else
	{
		if ( !g_tPrereadThread.Create ( PrereadFunc, 0, "preread" ) )
			sphWarning ( "failed to create preread thread" );
	}

	// almost ready, time to start listening
	g_iBacklog = hSearchd.GetInt ( "listen_backlog", g_iBacklog );
	ARRAY_FOREACH ( j, g_dListeners )
		if ( listen ( g_dListeners[j].m_iSock, g_iBacklog )==-1 )
		{
			if ( sphSockGetErrno()==EADDRINUSE )
				sphFatal ( "listen() failed with EADDRINUSE. A listener with other UID on same address:port?");
			else
				sphFatal ( "listen() failed: %s", sphSockError () );
		}

	if ( g_pThdPool )
	{
		// net thread needs non-blocking sockets
		ARRAY_FOREACH ( iListener, g_dListeners )
		{
			if ( sphSetSockNB ( g_dListeners[iListener].m_iSock )<0 )
			{
				sphWarning ( "sphSetSockNB() failed: %s", sphSockError() );
				sphSockClose ( g_dListeners[iListener].m_iSock );
			}
		}

		g_dTickPoolThread.Resize ( g_iNetWorkers );
		ARRAY_FOREACH ( iTick, g_dTickPoolThread )
		{
			if ( !sphThreadCreate ( g_dTickPoolThread.Begin()+iTick, CSphNetLoop::ThdTick,
				nullptr, false, Str_b().Sprintf ( "TickPool_%d", iTick ).cstr () ) )
				sphDie ( "failed to create tick pool thread" );
		}
	}

	// crash logging for the main thread (for --console case)
	CrashQuery_t tQueryTLS;
	SphCrashLogger_c::SetTopQueryTLS ( &tQueryTLS );

	// time for replication to sync with cluster
	ReplicationStart ( hSearchd, bNewCluster, bNewClusterForce );

	// ready, steady, go
	sphInfo ( "accepting connections" );

	// disable startup logging to stdout
	if ( !g_bOptNoDetach )
		g_bLogStdout = false;

	while (true)
	{
		SphCrashLogger_c::SetupTimePID();
		TickHead();
	}
} // NOLINT ServiceMain() function length


bool DieCallback ( const char * sMessage )
{
	g_bShutdown = true;
	sphLogFatal ( "%s", sMessage );
	return false; // caller should not log
}


UservarIntSet_c * UservarsHook ( const CSphString & sUservar )
{
	ScopedMutex_t tLock ( g_tUservarsMutex );
	Uservar_t * pVar = g_hUservars ( sUservar );
	if ( !pVar )
		return NULL;

	assert ( pVar->m_eType==USERVAR_INT_SET );
	pVar->m_pVal->AddRef();
	return pVar->m_pVal;
}


inline int mainimpl ( int argc, char **argv )
{
	// threads should be initialized before memory allocations
	char cTopOfMainStack;
	sphThreadInit();
	MemorizeStack ( &cTopOfMainStack );

	sphSetDieCallback ( DieCallback );
	sphSetLogger ( sphLog );
	g_pUservarsHook = UservarsHook;
	sphCollationInit ();
	sphBacktraceSetBinaryName ( argv[0] );
	GeodistInit();

#if USE_WINDOWS
	int iNameIndex = -1;
	for ( int i=1; i<argc; i++ )
	{
		if ( strcmp ( argv[i], "--ntservice" )==0 )
			g_bService = true;

		if ( strcmp ( argv[i], "--servicename" )==0 && (i+1)<argc )
		{
			iNameIndex = i+1;
			g_sServiceName = argv[iNameIndex];
		}
	}

	if ( g_bService )
	{
		for ( int i=0; i<argc; i++ )
			g_dArgs.Add ( argv[i] );

		if ( iNameIndex>=0 )
			g_sServiceName = g_dArgs[iNameIndex].cstr ();

		SERVICE_TABLE_ENTRY dDispatcherTable[] =
		{
			{ (LPSTR) g_sServiceName, (LPSERVICE_MAIN_FUNCTION)ServiceMain },
			{ NULL, NULL }
		};
		if ( !StartServiceCtrlDispatcher ( dDispatcherTable ) )
			sphFatal ( "StartServiceCtrlDispatcher() failed: %s", WinErrorInfo() );

		return 0;
	} else
#endif

	return ServiceMain ( argc, argv );
}

#ifndef SUPRESS_SEARCHD_MAIN
int main ( int argc, char ** argv )
{
	return mainimpl ( argc, argv );
}
#endif
