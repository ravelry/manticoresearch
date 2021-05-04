//
// Copyright (c) 2021, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "threadutils.h"
#include <csignal>

using namespace Threads;

namespace {
	RwLock_t& g_dDetachedGuard()
	{
		static RwLock_t dDetachedGuard;
		return dDetachedGuard;
	}

	CSphVector<LowThreadDesc_t *>& g_dDetachedThreads()
	{
		static CSphVector<LowThreadDesc_t *> dDetachedThreads GUARDED_BY ( g_dDetachedGuard () );
		return dDetachedThreads;
	}
}

// walk over list of running detached threads and apply fnHandler to each of them
// Each call made under r-lock to keep thread list intact
void Iterate ( ThreadFN& fnHandler )
{
	ScRL_t _ ( g_dDetachedGuard () );
	for ( auto * pThread : g_dDetachedThreads () )
		fnHandler ( pThread );
}

// register shutdown action that will walk over list of running detached threads and send SIGTERM to each of them.
// then wait until they're finished.
// Also register iterations right now;
void Detached::AloneShutdowncatch ()
{
#ifndef NDEBUG
	static bool bAlreadyInvoked = false;
	assert ( !bAlreadyInvoked );
	bAlreadyInvoked = true;
#endif

	Threads::RegisterIterator ( Iterate );

	// all about windows that we use pthread_kill right now.
	// if analogue exists there, the limitation can be removed.
#if !USE_WINDOWS
	searchd::AddShutdownCb ( [&]
	{
		int iThreads;
		{
			ScRL_t _ ( g_dDetachedGuard () );
			iThreads = g_dDetachedThreads().GetLength();
		}
		int iTurn = 1;

		while ( iThreads>0 )
		{
			{
				ScRL_t _ ( g_dDetachedGuard() );
				sphWarning ( "AloneShutdowncatch will kill %d threads", iThreads );
				for ( auto * pThread : g_dDetachedThreads () )
				{
					if ( pThread )
					{
						sphInfo ( "Kill thread '%s' with id %d, try %d",
							pThread->m_sThreadName.cstr (), pThread->m_iThreadID, iTurn );
						pthread_kill ( pThread->m_tThread, SIGTERM );
					}
				}
			}

			auto iStart = 0;
			while (true)
			{
				{
					ScRL_t _ ( g_dDetachedGuard() );
					iThreads = g_dDetachedThreads().GetLength ();
				}
				if ( iThreads<=0 )
						break;

				sphSleepMsec ( 50 );
				iStart += 50;
				if ( iStart>=10000 ) // wait 10 seconds between tries
				{
					sphWarning ( "AloneShutdowncatch catch still has %d alone threads", iThreads );
					break;
				}
			}

			++iTurn;
		}
	});
#endif
}


void Detached::AddThread ( LowThreadDesc_t* pThread )
{
	ScWL_t _ ( g_dDetachedGuard() );
	sphLogDebug ( "Detached::AddThread called for '%s', tid %d",
			   pThread->m_sThreadName.cstr(), pThread->m_iThreadID );
	g_dDetachedThreads ().Add ( pThread );
}


void Detached::RemoveThread ( LowThreadDesc_t* pVictim )
{
	sphLogDebug ( "Detached::RemoveThread called for %d", pVictim->m_iThreadID );
	ScWL_t _ ( g_dDetachedGuard() );
	ARRAY_FOREACH ( i, g_dDetachedThreads() )
	{
		auto pThread = g_dDetachedThreads ()[i];
		if ( Threads::Same ( pThread, pVictim ) )
		{
			sphLogDebug ( "Terminated thread %d, '%s'", pThread->m_iThreadID, pThread->m_sThreadName.cstr () );
			g_dDetachedThreads().RemoveFast ( i );
			return;
		}
	}
}
