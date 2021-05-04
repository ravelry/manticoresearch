//
// Copyright (c) 2021, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#pragma once

#include "sphinxstd.h"
#include "threadutils.h"
#include "optional.h"

/*
 * Lowest info came from thread. It includes low details as thread name, thread ID and basic profile statistic.
 *
 * When a task engaged into thread, we also have details about the task, like connection id, proto, etc.
 * OR, for system task - the name of the action.
 *
 * Task may produce subtasks - like parallel processing rt chunks, and they also include details like current chunk N.
 * It may be any kind of different pieces of data, caming from diffefent subtasks, and each need to be processed
 * different way, depending from the essense of the data itself.
 *
 * When task engaged, it fills it's own TaskInfo structure with it's essential data, and stores it in the
 * field m_pTaskInfo of thread descriptor LowThreadDesc_t. Previous value of that field stored in task's m_pNext field.
 * So, moving deep from task to task we finally have linked list of task infos, starting from one stored in Thread
 * and moving forward with m_pNext pointers.
 *
 * Each piece lives in own context (task) and owned by the task.
 * For displaying data (in show threads) we have 'public thread descriptor' with plain structure and fixed fields
 * Filling of that structure from low thread descriptor performed by GatherPublicTaskInfo. It first copies low thread
 * attributes, and then walk over task info chains, collecting essential data.
 *
 * Each task info has TaskType flag, and provides function which publish essential data into public descriptor.
 * When unwinding task chain we look for the type and call saved function, if any.
 * If no 'displaying' func provided, we just skip to the next layer.
 *
 */

// snapshot of current thread and tasks, used in 'show threads', etc. and contains flat rendered info.
struct PublicThreadDesc_t
{
	int					m_iThreadID		= -1;		///< OS thread id
	Optional_T<int64_t>	m_tmStart;					///< when did the current request start?
	int64_t 			m_tmLastJobStartTimeUS = -1;///< time where I've done something useful
	int64_t				m_tmLastJobDoneTimeUS = -1;	///< time where I've done something useful
	int64_t				m_tmTotalWorkedTimeUS = -1;	///< total time I've worked on useful tasks
	int64_t				m_tmTotalWorkedCPUTimeUS = -1;	///< total time I've worked on useful tasks
	int64_t				m_iTotalJobsDone = -1;		///< total jobs I've completed
	CSphString			m_sThreadName;

	StringBuilder_c		m_sChain;
	StringBuilder_c		m_sClientName	{" "};
	StringBuilder_c		m_sDescription	{" "};
	StringBuilder_c		m_sProto		{","};

	int 				m_iDescriptionLimit = -1;	///< cb flag when collecting info with columns=N, avoid copy huge descriptions then
	int64_t				m_tmConnect		= -1; ///< when did the client connect?
	CSphScopedPtr<CSphQuery>	m_pQuery {nullptr}; /// currently running query, if not sphinxql
	const char*			m_sCommand		= nullptr;	/// simple static SYSTEM, SELECT, UPDATE, etc. Used in show threads, crash dumping
	int					m_iConnID		= -1; ///< current conn-id for this thread. For logging and tracking in mysql

	Proto_e				m_eProto		= Proto_e::UNKNOWN; /// used in show threads to format or not format query
	TaskState_e			m_eTaskState	= TaskState_e::UNKNOWN; /// show threads, crash dumping

	PublicThreadDesc_t() = default;
	void Swap (PublicThreadDesc_t& rhs);
	MOVE_BYSWAP ( PublicThreadDesc_t );
};

// flatten info from thread. iCols make hint for huge descriptions to avoid full copy
PublicThreadDesc_t GatherPublicTaskInfo ( const Threads::LowThreadDesc_t * pSrc, int iCols );

// internal helpers
// we don't expect all possible taskinfos being located in this file,
// Instead, for each type of info you need to call RegisterRenderer and provide pointer to function which knows how to
// fill PublicThreadDesc from given type. Byte returned by registrar is then became ID of that type of info,
// and has to be written in m_eType field of each instance of such info
using RenderFnPtr = void ( * ) ( const void * pSrc, PublicThreadDesc_t & dDst );
BYTE RegisterRenderer ( RenderFnPtr pFunc );

// Declare static member func 'Render', and provide initial registration and storing type ID
#define DECLARE_RENDER( TASKINFO )    \
    static BYTE m_eTask;    \
    TASKINFO () {  \
        if (!m_eTask) m_eTask = RegisterRenderer ( TASKINFO::Render ); \
        m_eType = m_eTask; \
    };                                  \
    static void Render ( const void * pSrc, PublicThreadDesc_t & dDst )

// Defines declared stuf (has to be written in .cpp to avoid multiple definitions)
#define DEFINE_RENDER( TASKINFO ) \
	BYTE TASKINFO::m_eTask=0;        \
	void TASKINFO::Render ( const void * pSrc, PublicThreadDesc_t & dDst )


// generic task info
struct TaskInfo_t
{
	DECLARE_RENDER ( TaskInfo_t );

	std::atomic<void *> m_pPrev { nullptr }; // link to previous (parent) chain. Hazard, NOT owned!
	BYTE m_eType;
};

namespace internal_myinfo {
	void RefCountInc ( BYTE eType );
	void RefCountDec ( BYTE eType );
}
// RAII task info
// Store info to TLS root, stores previous root to the chain
// On dtr restores stored chain as root and retire info (uses hazard pointers)
// if explicit root provided - uses it instead of TLS root.
template<typename TASKINFO>
class ScopedInfo_T : public hazard::ScopedPtr_t<TASKINFO*>
{
	using BASE = hazard::ScopedPtr_t<TASKINFO*>;
	BYTE m_eType;

public:
	explicit ScopedInfo_T ( TASKINFO* pInfo )
		: BASE ( pInfo )
	{
		m_eType = TASKINFO::m_eTask;
		pInfo->m_pPrev = Threads::MyThd ().m_pTaskInfo.load ( std::memory_order_acquire );
		internal_myinfo::RefCountInc ( m_eType );
		Threads::MyThd ().m_pTaskInfo.store ( pInfo, std::memory_order_release );
	}

	ScopedInfo_T ( ScopedInfo_T && rhs ) noexcept
	{
		BASE::Swap ( rhs );
	}

	~ScopedInfo_T<TASKINFO> ()
	{
		Threads::MyThd ().m_pTaskInfo.store ( ( BASE::operator TASKINFO * () )->m_pPrev, std::memory_order_release );
		internal_myinfo::RefCountDec ( m_eType );
	}
};

// make provided task info (by typed, NOT generic! ptr) displayed in the thread.
// when produced RAII obj come out of scope, info will be hidden and retired.
template<typename TASKINFO>
ScopedInfo_T<TASKINFO> PublishTaskInfo ( TASKINFO* pPtr )
{
	return ScopedInfo_T<TASKINFO> ( pPtr );
}

// wide used task infos (declare them here to be accessed from anywhere, and also as examples)

// system task - command and description
struct MiniTaskInfo_t : public TaskInfo_t
{
	DECLARE_RENDER( MiniTaskInfo_t );

	int64_t m_tmStart = sphMicroTimer ();
	const char *	m_sCommand = nullptr; // is always mt-safe since always set static const
	hazard::ScopedPtr_t<const CSphString *> m_pHazardDescription;
	int				m_iDescriptionLen = 0;	// len of string in m_pHazardDescription
};

using ScopedMiniInfo_t = ScopedInfo_T<MiniTaskInfo_t>;

// create and publish info about system task (what before did ThreadSystem_t)
// command = 'SYSTEM', description = 'SYSTEM sDescription'
ScopedMiniInfo_t PublishSystemInfo ( const char * sDescription );

// client connection
struct ClientTaskInfo_t : public MiniTaskInfo_t
{
	DECLARE_RENDER( ClientTaskInfo_t );

	TaskState_e	m_eTaskState = TaskState_e::UNKNOWN;
	Proto_e		m_eProto = Proto_e::UNKNOWN;
	int			m_iConnID = -1;
	int 		m_iThrottlingPeriod = -1;
	int 		m_iDistThreads = 0;
	int 		m_iDesiredStack = -1;
	CSphString	m_sClientName; // set once before info is published and never changes. So, assume always mt-safe
	bool 		m_bSsl = false;
	bool 		m_bVip = false;
};

using ScopedClientInfo_t = ScopedInfo_T<ClientTaskInfo_t>;

namespace myinfo {

	// descriptions in m_pHazardDescription bigger than this limit will be retired as soon as possible
	static const int HazardDescriptionSizeLimit = 256*1024;

	// thread-local m_pTaskInfo available globally from any thread
	// it is hazard, since implicitly assume that I own this info and so, it will not be deleted by another thread
	TaskInfo_t * HazardTaskInfo ();

	// num of tasks with given type
	int Count ( BYTE eType );

	// num of all tasks
	int CountAll();

	// num of client tasks
	inline int CountClients ()
	{
		return Count ( ClientTaskInfo_t::m_eTask );
	}

	// access to thread-local fields (look first appropriate node of info chain)

	// first ptr to node with type eType
	TaskInfo_t * GetHazardTypedNode ( BYTE eType );

	// bind current taskinfo content to handler
	Threads::Handler StickParent ( Threads::Handler fnHandler );

	// bind current taskinfo and add new scoped mini info for coro handler
	Threads::Handler OwnMini ( Threads::Handler fnHandler );

	// first ptr to node of given type
	template <typename TASKINFO>
	TASKINFO* ref()
	{
		return (TASKINFO *) GetHazardTypedNode ( TASKINFO::m_eTask );
	}

	// returns ClientTaskInfo_t::m_iDistThreads
	int DistThreads();

	// set ClientTaskInfo_t::m_iDistThreads
	void SetDistThreads ( int iValue );

	// returns ClientTaskInfo_t::m_iThrottlingPeriod
	int ThrottlingPeriodMS();

	// set ClientTaskInfo_t::m_iThrottlingPeriod
	void SetThrottlingPeriodMS ( int iValue );

	// returns ClientTaskInfo_t::m_iDesiredStack
	int DesiredStack ();

	// set ClientTaskInfo_t::m_iDesiredStack.
	// It might be used to warn (task-wide) about heavy calculations which need deep stack
	void SetDesiredStack ( int iValue );

	// returns ClientTaskInfo_t::m_iConnID
	int ConnID ();

	// returns ClientTaskInfo_t::m_bVip
	bool IsVIP ();

	// returns ClientTaskInfo_t::m_bSsl
	bool IsSSL ();

	// set ClientTaskInfo_t::m_bSsl
	void SetSSL ( bool bValue=true );

	// set MiniTaskInfo_t::m_sCommand
	void SetCommand ( const char * sCommand );

	// set ClientTaskInfo_t::m_eProto
	void SetProto ( Proto_e eProto );

	// get ClientTaskInfo_t::m_eProto
	Proto_e GetProto ();

	// set ClientTaskInfo_t::m_eTaskState
	void TaskState ( TaskState_e eState );

	// returns ClientTaskInfo_t::m_sClientName
	const char * szClientName ();

	// set MiniTaskInfo_t::m_pHazardDescription. and refresh timer
	// iLen used to select retire policy (lazy, or immediate retire)
	void SetDescription ( CSphString sDescription, int iLen );

	// set MiniTaskInfo_t::m_pHazardDescription and refresh timer fixme! rename to SetTaskInfo
	void SetThreadInfo ( const char * sTemplate, ... );

	// returns non-guarded ref to MiniTaskInfo_t::m_pHazardDescription (to be used in same scope as set functions)
	Str_t UnsafeDescription ();

} // namespace myinfo

volatile int &getDistThreads ();

// provides daemon-wide global dist-threads, or task-local, if any exists, or 0 if none
int GetEffectiveDistThreads ();
