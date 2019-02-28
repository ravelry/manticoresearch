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

#ifndef _sphinxstd_
#define _sphinxstd_

#if _MSC_VER>=1400
#define _CRT_SECURE_NO_DEPRECATE 1
#define _CRT_NONSTDC_NO_DEPRECATE 1
#endif

#if _MSC_VER>=1600
#define HAVE_STDINT_H 1
#endif

#if (_MSC_VER>=1000) && !defined(__midl) && defined(_PREFAST_)
typedef int __declspec("SAL_nokernel") __declspec("SAL_nodriver") __prefast_flag_kernel_driver_mode;
#endif

#if defined(_MSC_VER) && (_MSC_VER<1400)
#define vsnprintf _vsnprintf
#endif

#ifndef __GNUC__
#define __attribute__(x)
#endif

#if HAVE_CONFIG_H
#include "config.h"
#endif

// supress C4577 ('noexcept' used with no exception handling mode specified)
#if _MSC_VER==1900
#pragma warning(disable:4577)
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>
#include <utility>
#include <memory>
#include <functional>

// for 64-bit types
#if HAVE_STDINT_H
#include <stdint.h>
#endif

#if HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#endif

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifndef USE_WINDOWS
#ifdef _MSC_VER
#define USE_WINDOWS 1
#else
#define USE_WINDOWS 0
#endif // _MSC_VER
#endif

#if !USE_WINDOWS
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#endif

/////////////////////////////////////////////////////////////////////////////
// COMPILE-TIME CHECKS
/////////////////////////////////////////////////////////////////////////////

#if defined (__GNUC__)
#define VARIABLE_IS_NOT_USED __attribute__((unused))
#else
#define  VARIABLE_IS_NOT_USED
#endif

#define STATIC_ASSERT(_cond,_name)		typedef char STATIC_ASSERT_FAILED_ ## _name [ (_cond) ? 1 : -1 ] VARIABLE_IS_NOT_USED
#define STATIC_SIZE_ASSERT(_type,_size)	STATIC_ASSERT ( sizeof(_type)==_size, _type ## _MUST_BE_ ## _size ## _BYTES )


#ifndef __analysis_assume
#define __analysis_assume(_arg)
#endif


/// some function arguments only need to have a name in debug builds
#ifndef NDEBUG
#define DEBUGARG(_arg) _arg
#else
#define DEBUGARG(_arg)
#endif

/////////////////////////////////////////////////////////////////////////////
// PORTABILITY
/////////////////////////////////////////////////////////////////////////////

#if _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h> // for bsr
#pragma intrinsic(_BitScanReverse)

#define strcasecmp			strcmpi
#define strncasecmp			_strnicmp
#define snprintf			_snprintf
#define strtoll				_strtoi64
#define strtoull			_strtoui64

#else

#if USE_ODBC
// UnixODBC compatible DWORD
#if defined(__alpha) || defined(__sparcv9) || defined(__LP64__) || (defined(__HOS_AIX__) && defined(_LP64)) || defined(__APPLE__)
typedef unsigned int		DWORD;
#else
typedef unsigned long		DWORD;
#endif
#else
// default DWORD
typedef unsigned int		DWORD;
#endif // USE_ODBC

typedef unsigned short		WORD;
typedef unsigned char		BYTE;

#endif // _WIN32

/////////////////////////////////////////////////////////////////////////////
// 64-BIT INTEGER TYPES AND MACROS
/////////////////////////////////////////////////////////////////////////////

#if defined(U64C) || defined(I64C)
#error "Internal 64-bit integer macros already defined."
#endif

#if !HAVE_STDINT_H

#if defined(_MSC_VER)
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#define U64C(v) v ## UI64
#define I64C(v) v ## I64
#define PRIu64 "I64d"
#define PRIi64 "I64d"
#else // !defined(_MSC_VER)
typedef long long int64_t;
typedef unsigned long long uint64_t;
#endif // !defined(_MSC_VER)

#endif // no stdint.h

// if platform-specific macros were not supplied, use common defaults
#ifndef U64C
#define U64C(v) v ## ULL
#endif

#ifndef I64C
#define I64C(v) v ## LL
#endif

#ifndef PRIu64
#define PRIu64 "llu"
#endif

#ifndef PRIi64
#define PRIi64 "lld"
#endif

#define UINT64_FMT "%" PRIu64
#define INT64_FMT "%" PRIi64

#ifndef UINT64_MAX
#define UINT64_MAX U64C(0xffffffffffffffff)
#endif

#ifndef INT64_MIN
#define INT64_MIN I64C(0x8000000000000000)
#endif

#ifndef INT64_MAX
#define INT64_MAX I64C(0x7fffffffffffffff)
#endif

STATIC_SIZE_ASSERT ( uint64_t, 8 );
STATIC_SIZE_ASSERT ( int64_t, 8 );

// conversion macros that suppress %lld format warnings vs printf
// problem is, on 64-bit Linux systems with gcc and stdint.h, int64_t is long int
// and despite sizeof(long int)==sizeof(long long int)==8, gcc bitches about that
// using PRIi64 instead of %lld is of course The Right Way, but ugly like fuck
// so lets wrap them args in INT64() instead
#define INT64(_v) ((long long int)(_v))
#define UINT64(_v) ((unsigned long long int)(_v))

/////////////////////////////////////////////////////////////////////////////
// MEMORY MANAGEMENT
/////////////////////////////////////////////////////////////////////////////

#define SPH_DEBUG_LEAKS			0
#define SPH_ALLOC_FILL			0
#define SPH_ALLOCS_PROFILER		0
#define SPH_DEBUG_BACKTRACES 0 // will add not only file/line, but also full backtrace

#if SPH_DEBUG_LEAKS || SPH_ALLOCS_PROFILER

/// debug new that tracks memory leaks
void *			operator new ( size_t iSize, const char * sFile, int iLine );

/// debug new that tracks memory leaks
void *			operator new [] ( size_t iSize, const char * sFile, int iLine );

/// debug allocate to use in custom allocator
void * debugallocate ( size_t );

/// debug deallocate to use in custom allocator
void debugdeallocate ( void * );

/// get current allocs count
int				sphAllocsCount ();

/// total allocated bytes
int64_t			sphAllocBytes ();

/// get last alloc id
int				sphAllocsLastID ();

/// dump all allocs since given id
void			sphAllocsDump ( int iFile, int iSinceID );

/// dump stats to stdout
void			sphAllocsStats ();

/// check all existing allocs; raises assertion failure in cases of errors
void			sphAllocsCheck ();

void			sphMemStatDump ( int iFD );

void			sphMemStatMMapAdd ( int64_t iSize );
void			sphMemStatMMapDel ( int64_t iSize );

#undef new
#define new		new(__FILE__,__LINE__)
#define NEW_IS_OVERRIDED 1

#if USE_RE2
#define MYTHROW() throw()
#else
#define MYTHROW() noexcept
#endif

/// delete for my new
void			operator delete ( void * pPtr ) MYTHROW();

/// delete for my new
void			operator delete [] ( void * pPtr ) MYTHROW();

template<typename T>
class managed_allocator
{
public:
    typedef size_t size_type;
    typedef T * pointer;
    typedef const T * const_pointer;
	typedef T value_type;

    template<typename _Tp1>
    struct rebind
    {
        typedef managed_allocator <_Tp1> other;
    };

    pointer allocate ( size_type n, const void * = 0 )
    {
		return ( T * ) debugallocate ( n * sizeof ( T ) );
    }

    void deallocate ( pointer p, size_type )
    {
		debugdeallocate (p);
    }
};
#else
template<typename T> using managed_allocator = std::allocator<T>;
#endif // SPH_DEBUG_LEAKS || SPH_ALLOCS_PROFILER

extern const char * strerrorm ( int errnum ); // defined in sphinxint.h
/////////////////////////////////////////////////////////////////////////////
// HELPERS
/////////////////////////////////////////////////////////////////////////////

// magick to determine widest from provided types and initialize a whole unions
// for example,
/*
 *	union foo {
 *		BYTE	a;
 *		char	b;
 *		DWORD	c;
 *		WORDID	w;
 *		sphDocid_t d;
 *		void*	p;
 *		WIDEST<BYTE,char,DWORD,WORDID,sphDocid_t,void*>::T _init = 0;
 *	};
 */
template < typename T1, typename T2, bool= (sizeof ( T1 )<sizeof ( T2 )) >
struct WIDER
{
	using T=T2;
};

template < typename T1, typename T2 >
struct WIDER < T1, T2, false >
{
	using T=T1;
};

template < typename T1, typename... TYPES >
struct WIDEST
{
	using T=typename WIDER < T1, typename WIDEST< TYPES... >::T >::T;
};

template < typename T1, typename T2 >
struct WIDEST<T1, T2>
{
	using T=typename WIDER < T1, T2 >::T;
};



inline int sphBitCount ( DWORD n )
{
	// MIT HACKMEM count
	// works for 32-bit numbers only
	// fix last line for 64-bit numbers
	register DWORD tmp;
	tmp = n - ((n >> 1) & 033333333333) - ((n >> 2) & 011111111111);
	return ( (tmp + (tmp >> 3) ) & 030707070707) % 63;
}

typedef			bool ( *SphDieCallback_t ) ( const char * );

/// crash with an error message, and do not have searchd watchdog attempt to resurrect
void			sphDie ( const char * sMessage, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );

/// crash with an error message, but have searchd watchdog attempt to resurrect
void			sphDieRestart ( const char * sMessage, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );

/// setup a callback function to call from sphDie() before exit
/// if callback returns false, sphDie() will not log to stdout
void			sphSetDieCallback ( SphDieCallback_t pfDieCallback );

/// how much bits do we need for given int
inline int sphLog2 ( uint64_t uValue )
{
#if USE_WINDOWS
	DWORD uRes;
	if ( BitScanReverse ( &uRes, (DWORD)( uValue>>32 ) ) )
		return 33+uRes;
	BitScanReverse ( &uRes, DWORD(uValue) );
	return 1+uRes;
#elif __GNUC__ || __clang__
	if ( !uValue )
		return 0;
	return 64 - __builtin_clzll(uValue);
#else
	int iBits = 0;
	while ( uValue )
	{
		uValue >>= 1;
		iBits++;
	}
	return iBits;
#endif
}

/// float vs dword conversion
inline DWORD sphF2DW ( float f )	{ union { float f; DWORD d; } u; u.f = f; return u.d; }

/// dword vs float conversion
inline float sphDW2F ( DWORD d )	{ union { float f; DWORD d; } u; u.d = d; return u.f; }

/// double to bigint conversion
inline uint64_t sphD2QW ( double f )	{ union { double f; uint64_t d; } u; u.f = f; return u.d; }

/// bigint to double conversion
inline double sphQW2D ( uint64_t d )	{ union { double f; uint64_t d; } u; u.d = d; return u.f; }

/// microsecond precision timestamp
/// current UNIX timestamp in seconds multiplied by 1000000, plus microseconds since the beginning of current second
int64_t		sphMicroTimer ();

/// double argument squared
inline double sqr ( double v ) { return v*v;}

/// float argument squared
inline float fsqr ( float v ) { return v*v; }

//////////////////////////////////////////////////////////////////////////
// RANDOM NUMBERS GENERATOR
//////////////////////////////////////////////////////////////////////////

/// seed RNG
void		sphSrand ( DWORD uSeed );

/// auto-seed RNG based on time and PID
void		sphAutoSrand ();

/// generate another random
DWORD		sphRand ();

/////////////////////////////////////////////////////////////////////////////
// DEBUGGING
/////////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS
#ifndef NDEBUG

void sphAssert ( const char * sExpr, const char * sFile, int iLine );

#undef assert
#define assert(_expr) (void)( (_expr) || ( sphAssert ( #_expr, __FILE__, __LINE__ ), 0 ) )

#endif // !NDEBUG
#endif // USE_WINDOWS


// to avoid disappearing of _expr in release builds
#ifndef NDEBUG
#define Verify(_expr) assert(_expr)
#else
#define Verify(_expr) _expr
#endif

/////////////////////////////////////////////////////////////////////////////
// GENERICS
/////////////////////////////////////////////////////////////////////////////

template <typename T> T Min ( T a, T b ) { return a<b ? a : b; }
template <typename T, typename U> typename WIDER<T,U>::T Min ( T a, U b )
{
	return a<b ? a : b;
}
template <typename T> T Max ( T a, T b ) { return a<b ? b : a; }
template <typename T, typename U> typename WIDER<T,U>::T Max ( T a, U b )
{
	return a<b ? b : a;
}
#define SafeDelete(_x)		{ if (_x) { delete (_x); (_x) = nullptr; } }
#define SafeDeleteArray(_x)	{ if (_x) { delete [] (_x); (_x) = nullptr; } }
#define SafeRelease(_x)		{ if (_x) { (_x)->Release(); (_x) = nullptr; } }
#define SafeAddRef( _x )        { if (_x) { (_x)->AddRef(); } }

/// swap
template < typename T > inline void Swap ( T & v1, T & v2 )
{
	T temp = std::move ( v1 );
	v1 = std::move ( v2 );
	v2 = std::move ( temp );
}

/// prevent copy
class ISphNoncopyable
{
public:
	ISphNoncopyable () = default;
	ISphNoncopyable ( const ISphNoncopyable & ) = delete;
	const ISphNoncopyable &		operator = ( const ISphNoncopyable & ) = delete;
};

//////////////////////////////////////////////////////////////////////////////

/// generic comparator
template < typename T >
struct SphLess_T
{
	static inline bool IsLess ( const T & a, const T & b )
	{
		return a < b;
	}
};


/// generic comparator
template < typename T >
struct SphGreater_T
{
	static inline bool IsLess ( const T & a, const T & b )
	{
		return b < a;
	}
};


/// generic comparator
template < typename T, typename C >
struct SphMemberLess_T
{
	const T C::* m_pMember;

	explicit SphMemberLess_T ( T C::* pMember )
		: m_pMember ( pMember )
	{}

	inline bool IsLess ( const C & a, const C & b ) const
	{
		return ( (&a)->*m_pMember ) < ( (&b)->*m_pMember );
	}
};

template < typename T, typename C >
inline SphMemberLess_T<T,C>
sphMemberLess ( T C::* pMember )
{
	return SphMemberLess_T<T,C> ( pMember );
}


/// generic accessor
template < typename T >
struct SphAccessor_T
{
	typedef T MEDIAN_TYPE;

	MEDIAN_TYPE & Key ( T * a ) const
	{
		return *a;
	}

	void CopyKey ( MEDIAN_TYPE * pMed, T * pVal ) const
	{
		*pMed = Key(pVal);
	}

	void Swap ( T * a, T * b ) const
	{
		::Swap ( *a, *b );
	}

	T * Add ( T * p, int i ) const
	{
		return p+i;
	}

	int Sub ( T * b, T * a ) const
	{
		return (int)(b-a);
	}
};


/// heap sort helper
template < typename T, typename U, typename V >
void sphSiftDown ( T * pData, int iStart, int iEnd, const U &COMP, const V &ACC )
{
	while (true)
	{
		int iChild = iStart*2+1;
		if ( iChild>iEnd )
			return;

		int iChild1 = iChild+1;
		if ( iChild1<=iEnd && COMP.IsLess ( ACC.Key ( ACC.Add ( pData, iChild ) ), ACC.Key ( ACC.Add ( pData, iChild1 ) ) ) )
			iChild = iChild1;

		if ( COMP.IsLess ( ACC.Key ( ACC.Add ( pData, iChild ) ), ACC.Key ( ACC.Add ( pData, iStart ) ) ) )
			return;
		ACC.Swap ( ACC.Add ( pData, iChild ), ACC.Add ( pData, iStart ) );
		iStart = iChild;
	}
}


/// heap sort
template < typename T, typename U, typename V >
void sphHeapSort ( T * pData, int iCount, const U &COMP, const V &ACC )
{
	if ( !pData || iCount<=1 )
		return;

	// build a max-heap, so that the largest element is root
	for ( int iStart=( iCount-2 )>>1; iStart>=0; --iStart )
		sphSiftDown ( pData, iStart, iCount-1, COMP, ACC );

	// now keep popping root into the end of array
	for ( int iEnd=iCount-1; iEnd>0; )
	{
		ACC.Swap ( pData, ACC.Add ( pData, iEnd ) );
		sphSiftDown ( pData, 0, --iEnd, COMP, ACC );
	}
}


/// generic sort
template < typename T, typename U, typename V >
void sphSort ( T * pData, int iCount, const U &COMP, const V &ACC )
{
	if ( iCount<2 )
		return;

	typedef T * P;
	// st0 and st1 are stacks with left and right bounds of array-part.
	// They allow us to avoid recursion in quicksort implementation.
	P st0[32], st1[32], a, b, i, j;
	typename V::MEDIAN_TYPE x;
	int k;

	const int SMALL_THRESH = 32;
	int iDepthLimit = sphLog2 ( iCount );
	iDepthLimit = ( ( iDepthLimit<<2 ) + iDepthLimit ) >> 1; // x2.5

	k = 1;
	st0[0] = pData;
	st1[0] = ACC.Add ( pData, iCount-1 );
	while ( k )
	{
		k--;
		i = a = st0[k];
		j = b = st1[k];

		// if quicksort fails on this data; switch to heapsort
		if ( !k )
		{
			if ( !--iDepthLimit )
			{
				sphHeapSort ( a, ACC.Sub ( b, a )+1, COMP, ACC );
				return;
			}
		}

		// for tiny arrays, switch to insertion sort
		int iLen = ACC.Sub ( b, a );
		if ( iLen<=SMALL_THRESH )
		{
			for ( i=ACC.Add ( a, 1 ); i<=b; i=ACC.Add ( i, 1 ) )
			{
				for ( j=i; j>a; )
				{
					P j1 = ACC.Add ( j, -1 );
					if ( COMP.IsLess ( ACC.Key(j1), ACC.Key(j) ) )
						break;
					ACC.Swap ( j, j1 );
					j = j1;
				}
			}
			continue;
		}

		// ATTENTION! This copy can lead to memleaks if your CopyKey
		// copies something which is not freed by objects destructor.
		ACC.CopyKey ( &x, ACC.Add ( a, iLen/2 ) );
		while ( a<b )
		{
			while ( i<=j )
			{
				while ( COMP.IsLess ( ACC.Key(i), x ) )
					i = ACC.Add ( i, 1 );
				while ( COMP.IsLess ( x, ACC.Key(j) ) )
					j = ACC.Add ( j, -1 );
				if ( i<=j )
				{
					ACC.Swap ( i, j );
					i = ACC.Add ( i, 1 );
					j = ACC.Add ( j, -1 );
				}
			}

			// Not so obvious optimization. We put smaller array-parts
			// to the top of stack. That reduces peak stack size.
			if ( ACC.Sub ( j, a )>=ACC.Sub ( b, i ) )
			{
				if ( a<j ) { st0[k] = a; st1[k] = j; k++; }
				a = i;
			} else
			{
				if ( i<b ) { st0[k] = i; st1[k] = b; k++; }
				b = j;
			}
		}
	}
}


template < typename T, typename U >
void sphSort ( T * pData, int iCount, const U& COMP )
{
	sphSort ( pData, iCount, COMP, SphAccessor_T<T>() );
}


template < typename T >
void sphSort ( T * pData, int iCount )
{
	sphSort ( pData, iCount, SphLess_T<T>() );
}

//////////////////////////////////////////////////////////////////////////

/// member functor, wraps object member access
template < typename T, typename CLASS >
struct SphMemberFunctor_T
{
	const T CLASS::*	m_pMember;

	explicit			SphMemberFunctor_T ( T CLASS::* pMember )	: m_pMember ( pMember ) {}
	const T &			operator () ( const CLASS & arg ) const		{ return (&arg)->*m_pMember; }

	inline bool IsLess ( const CLASS & a, const CLASS & b ) const
	{
		return (&a)->*m_pMember < (&b)->*m_pMember;
	}

	inline bool IsEq ( const CLASS & a, T b )
	{
		return ( (&a)->*m_pMember )==b;
	}
};


/// handy member functor generator
/// this sugar allows you to write like this
/// dArr.Sort ( bind ( &CSphType::m_iMember ) );
/// dArr.BinarySearch ( bind ( &CSphType::m_iMember ), iValue );
template < typename T, typename CLASS >
inline SphMemberFunctor_T < T, CLASS >
bind ( T CLASS::* ptr )
{
	return SphMemberFunctor_T < T, CLASS > ( ptr );
}


/// identity functor
template < typename T >
struct SphIdentityFunctor_T
{
	const T &			operator () ( const T & arg ) const			{ return arg; }
};

//////////////////////////////////////////////////////////////////////////

/// generic binary search
template < typename T, typename U, typename PRED >
T * sphBinarySearch ( T * pStart, T * pEnd, const PRED & tPred, U tRef )
{
	if ( !pStart || pEnd<pStart )
		return NULL;

	if ( tPred(*pStart)==tRef )
		return pStart;

	if ( tPred(*pEnd)==tRef )
		return pEnd;

	while ( pEnd-pStart>1 )
	{
		if ( tRef<tPred(*pStart) || tPred(*pEnd)<tRef )
			break;
		assert ( tPred(*pStart)<tRef );
		assert ( tRef<tPred(*pEnd) );

		T * pMid = pStart + (pEnd-pStart)/2;
		if ( tRef==tPred(*pMid) )
			return pMid;

		if ( tRef<tPred(*pMid) )
			pEnd = pMid;
		else
			pStart = pMid;
	}
	return NULL;
}


/// generic binary search
template < typename T >
T * sphBinarySearch ( T * pStart, T * pEnd, T & tRef )
{
	return sphBinarySearch ( pStart, pEnd, SphIdentityFunctor_T<T>(), tRef );
}

/// generic uniq
template < typename T, typename T_COUNTER >
T_COUNTER sphUniq ( T * pData, T_COUNTER iCount )
{
	if ( !iCount )
		return 0;

	T_COUNTER iSrc = 1, iDst = 1;
	while ( iSrc<iCount )
	{
		if ( pData[iDst-1]==pData[iSrc] )
			iSrc++;
		else
			pData[iDst++] = pData[iSrc++];
	}
	return iDst;
}

/// buffer traits - provides generic ops over a typed blob (vector).
/// just provide common operators; doesn't manage buffer anyway
template < typename T > class VecTraits_T
{
public:

	//! functional types to use as lambda type.
	//! If we use simple function pointer here (say, as (*bool) (const T&) )
	//! it will work, but only for pure lambdas.
	//! Any other non-pure lambda which binds (acquires) anyting from
	//! env will cause compile error and actually works well only with such
	//! std::function typedefs
	using fFilter=std::function<bool ( const T & )>;
	using fAction=std::function<void ( T & )>;

	VecTraits_T() = default;

	// this ctr allows to regard any typed blob as VecTraits, and use it's benefits.
	VecTraits_T( T* pData, int64_t iCount )
		: m_pData ( pData )
		, m_iCount ( iCount )
	{}

	template <typename TT>
	VecTraits_T ( TT * pData, int64_t iCount )
		: m_pData ( pData )
		, m_iCount ( iCount )
	{}

	VecTraits_T Slice ( int64_t iBegin=0, int64_t iCount=-1 )
	{
		// calculate starting bound
		if ( iBegin<0 )
			iBegin = 0;
		else if ( iBegin>m_iCount )
			iBegin = m_iCount;

		iCount = ( iCount<0 ) ? ( m_iCount - iBegin ) : Min ( iCount, m_iCount - iBegin );
		return VecTraits_T ( m_pData + iBegin, iCount );
	}

	/// accessor by forward index
	T &operator[] ( int64_t iIndex ) const
	{
		assert ( iIndex>=0 && iIndex<m_iCount );
		return m_pData[iIndex];
	}

	/// get first entry ptr
	T * Begin () const
	{
		return m_iCount ? m_pData : nullptr;
	}

	/// pointer to the item after the last
	T * End () const
	{
		return  m_pData + m_iCount;
	}

	/// make happy C++11 ranged for loops
	T * begin () const
	{
		return Begin ();
	}

	T * end () const
	{
		return m_iCount ? m_pData + m_iCount : nullptr;
	}

	/// get last entry
	T &Last () const
	{
		return ( *this )[m_iCount - 1];
	}

	/// make possible to pass VecTraits_T<T*> into funcs which need VecTraits_T<const T*>
	/// fixme! M.b. add check and fire error if T is not a pointer?
	operator VecTraits_T<const typename std::remove_pointer<T>::type *> & () const
	{
		return *( VecTraits_T<const typename std::remove_pointer<T>::type *>* ) ( this );
	}

	template<typename TT>
	operator VecTraits_T<TT> & () const
	{
		STATIC_ASSERT ( sizeof ( T )==sizeof ( TT ), SIZE_OF_DERIVED_NOT_SAME_AS_ORIGIN );
		return *( VecTraits_T<TT> * ) ( this );
	}

	/// check if i'm empty
	bool IsEmpty () const
	{
		return ( m_pData==nullptr || m_iCount==0 );
	}

	/// query current length, in elements
	int64_t GetLength64 () const
	{
		return m_iCount;
	}

	int GetLength () const
	{
		return (int)m_iCount;
	}

	/// get length in bytes
	size_t GetLengthBytes () const
	{
		return sizeof ( T ) * ( size_t ) m_iCount;
	}

	/// default sort
	void Sort ( int iStart = 0, int iEnd = -1 )
	{
		Sort ( SphLess_T<T> (), iStart, iEnd );
	}

	/// default reverse sort
	void RSort ( int iStart = 0, int iEnd = -1 )
	{
		Sort ( SphGreater_T<T> (), iStart, iEnd );
	}

	/// generic sort
	template < typename F >
	void Sort ( F COMP, int iStart = 0, int iEnd = -1 )
	{
		if ( m_iCount<2 )
			return;
		if ( iStart<0 )
			iStart += m_iCount;
		if ( iEnd<0 )
			iEnd += m_iCount;
		assert ( iStart<=iEnd );

		sphSort ( m_pData + iStart, iEnd - iStart + 1, COMP );
	}

	/// generic binary search
	/// assumes that the array is sorted in ascending order
	template < typename U, typename PRED >
	T * BinarySearch ( const PRED &tPred, U tRef ) const
	{
		return sphBinarySearch ( m_pData, m_pData + m_iCount - 1, tPred, tRef );
	}

	/// generic binary search
	/// assumes that the array is sorted in ascending order
	T * BinarySearch ( T tRef ) const
	{
		return sphBinarySearch ( m_pData, m_pData + m_iCount - 1, tRef );
	}

	/// generic linear search - 'ARRAY_ANY' replace
	/// see 'Contains()' below for examlpe of usage.
	inline bool FindFirst ( fFilter COND ) const
	{
		for ( int i = 0; i<m_iCount; ++i )
			if ( COND ( m_pData[i] ) )
				return true;
		return false;
	}

	inline int GetFirst ( fFilter COND ) const
	{
		for ( int i = 0; i<m_iCount; ++i )
			if ( COND ( m_pData[i] ) )
				return i;
		return -1;
	}

	/// generic 'ARRAY_ALL'
	inline bool TestAll ( fFilter COND ) const
	{
		for ( int i = 0; i<m_iCount; ++i )
			if ( !COND ( m_pData[i] ) )
				return false;
		return true;
	}

	/// Apply an action to every member
	/// Apply ( [] (T& item) {...} );
	void Apply ( fAction Verb ) const
	{
		for ( int i = 0; i<m_iCount; ++i )
			Verb ( m_pData[i] );
	}

	/// generic linear search
	bool Contains ( T tRef ) const
	{
		return FindFirst ( [&] ( const T &v ) { return tRef==v; } );
	}

	/// generic linear search
	template < typename FUNCTOR, typename U >
	bool Contains ( FUNCTOR COMP, U tValue )
	{
		return FindFirst ( [&] ( const T &v ) { return COMP.IsEq ( v, tValue ); } );
	}

	/// fill with given value
	void Fill ( const T &rhs )
	{
		for ( int i = 0; i<m_iCount; ++i )
			m_pData[i] = rhs;
	}

protected:
	T * m_pData = nullptr;
	int64_t m_iCount = 0;
};

namespace sph {

//////////////////////////////////////////////////////////////////////////
/// Storage backends for vector
/// Each backend provides Allocate and Deallocate

/// Default backend - uses plain old new/delete
template < typename T >
class DefaultStorage_T
{
protected:
	/// grow enough to hold that much entries.
	inline static T * Allocate ( int iLimit )
	{
		return new T[iLimit];
	}

	inline static void Deallocate ( T * pData )
	{
		delete[] pData;
	}
	static inline void DataIsNotOwned() {}
};

/// Static backend: small blobs stored localy,
/// bigger came to plain old new/delete
template < typename T, int STATICSIZE = 4096 >
class LazyStorage_T
{
public:
	// don't allow moving (it has no sence with embedded buffer)
	inline LazyStorage_T ( LazyStorage_T &&rhs ) noexcept = delete;
	inline LazyStorage_T &operator= ( LazyStorage_T &&rhs ) noexcept = delete;

	LazyStorage_T() = default;
	static const int iSTATICSIZE = STATICSIZE;
protected:
	inline T * Allocate ( int iLimit )
	{
		if ( iLimit<=STATICSIZE )
			return m_dData;
		return new T[iLimit];
	}

	inline void Deallocate ( T * pData ) const
	{
		if ( pData!=m_dData )
			delete[] pData;
	}

	// DataIsNotOwned is not defined here, so swap/leakdata will not compile.
	//static inline void DataIsNotOwned () {}

private:
	T m_dData[iSTATICSIZE];
};

//////////////////////////////////////////////////////////////////////////
/// Copy backends for vector
/// Each backend provides Copy, Move and CopyOrSwap

/// Copy/move vec of a data item-by-item
template < typename T, bool = std::is_pod<T>::value >
class DataMover_T
{
public:
	static inline void Copy ( T * pNew, T * pData, int iLength )
	{
		for ( int i = 0; i<iLength; ++i )
			pNew[i] = pData[i];
	}

	static inline void Move ( T * pNew, T * pData, int iLength )
	{
		for ( int i = 0; i<iLength; ++i )
			pNew[i] = std::move ( pData[i] );
	}
};

template < typename T > /// Copy/move blob of POD data using memmove
class DataMover_T<T, true>
{
public:
	static inline void Copy ( T * pNew, const T * pData, int iLength )
	{
		if ( iLength ) // m.b. work without this check, but sanitize for paranoids.
			memmove ( ( void * ) pNew, ( const void * ) pData, iLength * sizeof ( T ) );
	}

	static inline void Move ( T * pNew, const T * pData, int iLength )
	{ Copy ( pNew, pData, iLength ); }

	// append raw blob: defined ONLY in POD specialization.
	static inline void CopyVoid ( T * pNew, const void * pData, int iLength )
	{ Copy ( pNew, ( T * ) pData, iLength ); }
};

/// default vector mover
template < typename T >
class DefaultCopy_T : public DataMover_T<T>
{
public:
	static inline void CopyOrSwap ( T &pLeft, const T &pRight )
	{
		pLeft = pRight;
	}
};


/// swap-vector policy (for non-copyable classes)
/// use Swap() instead of assignment on resize
template < typename T >
class SwapCopy_T
{
public:
	static inline void Copy ( T * pNew, T * pData, int iLength )
	{
		for ( int i = 0; i<iLength; ++i )
			Swap ( pNew[i], pData[i] );
	}

	static inline void Move ( T * pNew, T * pData, int iLength )
	{
		for ( int i = 0; i<iLength; ++i )
			Swap ( pNew[i], pData[i] );
	}

	static inline void CopyOrSwap ( T &dLeft, T &dRight )
	{
		Swap ( dLeft, dRight );
	}
};

//////////////////////////////////////////////////////////////////////////
/// Resize backends for vector
/// Each backend provides Relimit

/// Default relimit: grow 2x
class DefaultRelimit
{
public:
	static const int MAGIC_INITIAL_LIMIT = 8;
	static inline int Relimit ( int iLimit, int iNewLimit )
	{
		if ( !iLimit )
			iLimit = MAGIC_INITIAL_LIMIT;
		while ( iLimit<iNewLimit )
		{
			iLimit *= 2;
			assert ( iLimit>0 );
		}
		return iLimit;
	}
};

/// tight-vector policy
/// grow only 1.2x on resize (not 2x) starting from a certain threshold
class TightRelimit : public DefaultRelimit
{
public:
	static const int SLOW_GROW_TRESHOLD = 1024;
	static inline int Relimit ( int iLimit, int iNewLimit )
	{
		if ( !iLimit )
			iLimit = MAGIC_INITIAL_LIMIT;
		while ( iLimit<iNewLimit && iLimit<SLOW_GROW_TRESHOLD )
		{
			iLimit *= 2;
			assert ( iLimit>0 );
		}
		while ( iLimit<iNewLimit )
		{
			iLimit = ( int ) ( iLimit * 1.2f );
			assert ( iLimit>0 );
		}
		return iLimit;
	}
};


/// generic vector
/// uses storage, mover and relimit backends
/// (don't even ask why it's not std::vector)
template < typename T, class POLICY=DefaultCopy_T<T>, class LIMIT=DefaultRelimit, class STORE=DefaultStorage_T<T> >
class Vector_T : public VecTraits_T<T>, protected STORE, protected LIMIT
{
protected:
	using BASE = VecTraits_T<T>;
	using BASE::m_pData;
	using BASE::m_iCount;
	using STORE::Allocate;
	using STORE::Deallocate;

public:
	using BASE::Begin;
	using BASE::Sort;
	using BASE::GetLength; // these are for IDE helpers to work
	using BASE::GetLength64;
	using BASE::GetLengthBytes;
	using BASE::Slice;
	using LIMIT::Relimit;


	/// ctor
	Vector_T () = default;

	/// ctor with initial size
	explicit Vector_T ( int iCount )
	{
		Resize ( iCount );
	}

	/// copy ctor
	Vector_T ( const Vector_T<T> & rhs )
	{
		*this = rhs;
	}

	/// move ctr
	Vector_T ( Vector_T<T> &&rhs ) noexcept
		: STORE (std::move(rhs))
	{
		m_iCount = rhs.m_iCount;
		m_iLimit = rhs.m_iLimit;
		m_pData = rhs.m_pData;

		rhs.m_pData = nullptr;
		rhs.m_iCount = 0;
		rhs.m_iLimit = 0;
	}

	/// dtor
	~Vector_T ()
	{
		Reset ();
	}

	/// add entry
	T & Add ()
	{
		if ( m_iCount>=m_iLimit )
			Reserve ( 1 + m_iCount );
		return m_pData[m_iCount++];
	}

	/// add entry
	void Add ( const T & tValue )
	{
		assert ( ( &tValue<m_pData || &tValue>=( m_pData + m_iCount ) ) && "inserting own value (like last()) by ref!" );
		if ( m_iCount>=m_iLimit )
			Reserve ( 1 + m_iCount );
		m_pData[m_iCount++] = tValue;
	}

	void Add ( T&& tValue )
	{
		assert ( ( &tValue<m_pData || &tValue>=( m_pData + m_iCount ) ) && "inserting own value (like last()) by ref!" );
		if ( m_iCount>=m_iLimit )
			Reserve ( 1 + m_iCount );
		m_pData[m_iCount++] = std::move ( tValue );
	}

	/// add N more entries, and return a pointer to that buffer
	T * AddN ( int iCount )
	{
		if ( m_iCount + iCount>m_iLimit )
			Reserve ( m_iCount + iCount );
		m_iCount += iCount;
		return m_pData + m_iCount - iCount;
	}

	/// return idx of the item pointed by pBuf, or -1
	inline int Idx ( const T* pBuf )
	{
		if ( !pBuf )
			return -1;

		if ( pBuf < m_pData || pBuf >= m_pData + m_iLimit )
			return -1;

		return pBuf - m_pData;
	}


	/// add unique entry (ie. do not add if equal to last one)
	void AddUnique ( const T & tValue )
	{
		assert ( ( &tValue<m_pData || &tValue>=( m_pData + m_iCount ) ) && "inserting own value (like last()) by ref!" );
		if ( m_iCount>=m_iLimit )
			Reserve ( 1 + m_iCount );

		if ( m_iCount==0 || m_pData[m_iCount - 1]!=tValue )
			m_pData[m_iCount++] = tValue;
	}


	/// remove element by index
	void Remove ( int iIndex )
	{
		assert ( iIndex>=0 && iIndex<m_iCount );
		if ( --m_iCount>iIndex )
			POLICY::Move ( m_pData + iIndex, m_pData + iIndex + 1, m_iCount - iIndex );
	}

	/// remove element by index, swapping it with the tail
	void RemoveFast ( int iIndex )
	{
		assert ( iIndex>=0 && iIndex<m_iCount );
		if ( iIndex!=--m_iCount )
			Swap ( m_pData[iIndex], m_pData[m_iCount] ); // fixme! What about POLICY::CopyOrSwap here?
	}

	/// remove element by value (warning, linear O(n) search)
	bool RemoveValue ( T tValue )
	{
		for ( int i = 0; i<m_iCount; ++i )
			if ( m_pData[i]==tValue )
			{
				Remove ( i );
				return true;
			}
		return false;
	}

	/// remove element by value, asuming vec is sorted/uniq
	bool RemoveValueFromSorted ( T tValue )
	{
		T* pValue = VecTraits_T<T>::BinarySearch (tValue);
		if ( !pValue )
			return false;

		Remove ( pValue - Begin() );
		return true;
	}

	/// pop last value
	const T & Pop ()
	{
		assert ( m_iCount>0 );
		return m_pData[--m_iCount];
	}

public:
	/// grow enough to hold that much entries, if needed, but do *not* change the length
	void Reserve ( int iNewLimit )
	{
		// check that we really need to be called
		assert ( iNewLimit>=0 );
		if ( iNewLimit<=m_iLimit )
			return;

		// calc new limit
		m_iLimit = LIMIT::Relimit ( m_iLimit, iNewLimit );

		// realloc
		T * pNew = nullptr;
		if ( m_iLimit )
		{
			pNew = STORE::Allocate ( m_iLimit );

			if ( pNew==m_pData )
				return;

			__analysis_assume ( m_iCount<=m_iLimit );
			POLICY::Move ( pNew, m_pData, m_iCount );
		}
		Swap ( pNew, m_pData );
		STORE::Deallocate ( pNew );
	}

	/// ensure we have space for iGap more items (reserve more if necessary)
	inline void ReserveGap ( int iGap )
	{
		Reserve ( m_iCount + iGap );
	}

	/// resize
	void Resize ( int iNewLength )
	{
		assert ( iNewLength>=0 );
		if ( ( unsigned int ) iNewLength>( unsigned int ) m_iCount )
			Reserve ( iNewLength );
		m_iCount = iNewLength;
	}

	/// reset
	void Reset ()
	{
		STORE::Deallocate ( m_pData );
		m_pData = nullptr;
		m_iCount = 0;
		m_iLimit = 0;
	}

	/// memset whole reserved vec
	void ZeroMem ()
	{
		memset ( Begin (), 0, (size_t) AllocatedBytes () );
	}

	/// query current reserved size, in elements
	inline int GetLimit () const
	{
		return m_iLimit;
	}

	/// query currently allocated RAM, in bytes
	/// (could be > GetLengthBytes() since uses limit, not size)
	inline int AllocatedBytes () const
	{
		return m_iLimit*sizeof(T);
	}

public:
	/// filter unique
	void Uniq ()
	{
		if ( !m_iCount )
			return;

		Sort ();
		int iLeft = sphUniq ( m_pData, m_iCount );
		Resize ( iLeft );
	}

	/// copy
	Vector_T &operator= ( const Vector_T<T> &rhs )
	{
		Reset ();

		m_iCount = rhs.m_iCount;
		m_iLimit = rhs.m_iLimit;
		if ( m_iLimit )
			m_pData = new T [ m_iLimit ];
		__analysis_assume ( m_iCount<=m_iLimit );
		POLICY::Copy ( m_pData, rhs.m_pData, m_iCount );

		return *this;
	}

	/// move
	Vector_T &operator= ( Vector_T<T> &&rhs ) noexcept
	{
		Reset ();

		m_iCount = rhs.m_iCount;
		m_iLimit = rhs.m_iLimit;
		m_pData = rhs.m_pData;

		rhs.m_pData = nullptr;
		rhs.m_iCount = 0;
		rhs.m_iLimit = 0;

		return static_cast<Vector_T &>(STORE::operator= ( std::move ( rhs ) ));
//		return *this;
	}

	/// memmove N elements from raw pointer to the end
	/// works ONLY if T is POD type (i.e. may be simple memmoved)
	/// otherwize compile error will appear (if so, use typed version below).
	void Append ( const void * pData, int iN )
	{
		if ( iN<=0 )
			return;

		auto * pDst = AddN ( iN );
		POLICY::CopyVoid ( pDst, pData, iN );
	}

	/// append another vec to the end
	/// will use memmove (POD case), or one-by-one copying.
	void Append ( const VecTraits_T<T> &rhs )
	{
		if ( rhs.IsEmpty () )
			return;

		auto * pDst = AddN ( rhs.GetLength() );
		POLICY::Copy ( pDst, rhs.begin(), rhs.GetLength() );
	}

	/// swap
	template < typename L=LIMIT >
	void SwapData ( Vector_T<T, POLICY, L, STORE> &rhs )
	{
		STORE::DataIsNotOwned ();
		Swap ( m_iCount, rhs.m_iCount );
		Swap ( m_iLimit, rhs.m_iLimit );
		Swap ( m_pData, rhs.m_pData );
	}

	/// leak
	T * LeakData ()
	{
		STORE::DataIsNotOwned ();
		T * pData = m_pData;
		m_pData = NULL;
		Reset();
		return pData;
	}

	/// adopt external buffer
	/// note that caller must himself then nullify origin pData to avoid double-deletion
	void AdoptData ( T * pData, int64_t iLen, int64_t iLimit )
	{
		assert ( iLen>=0 );
		assert ( iLimit>=0 );
		assert ( pData || iLimit==0 );
		assert ( iLen<=iLimit );
		Reset();
		m_pData = pData;
		m_iLimit = iLimit;
		m_iCount = iLen;
	}

	/// insert into a middle (will fail to compile for swap vector)
	void Insert ( int iIndex, const T & tValue )
	{
		assert ( iIndex>=0 && iIndex<=this->m_iCount );

		if ( this->m_iCount>=this->m_iLimit )
			Reserve ( this->m_iCount+1 );

		for ( int i= this->m_iCount-1; i>=iIndex; --i )
			POLICY::CopyOrSwap ( this->m_pData [ i+1 ], this->m_pData[i] );
		POLICY::CopyOrSwap ( this->m_pData[iIndex], tValue );
		++this->m_iCount;
	}

	/// insert into a middle by policy-defined copier
	void Insert ( int iIndex, T &tValue )
	{
		assert ( iIndex>=0 && iIndex<=m_iCount );

		if ( this->m_iCount>=m_iLimit )
			Reserve ( this->m_iCount + 1 );

		for ( int i = this->m_iCount - 1; i>=iIndex; --i )
			POLICY::CopyOrSwap ( this->m_pData[i + 1], this->m_pData[i] );
		POLICY::CopyOrSwap ( this->m_pData[iIndex], tValue );
		++this->m_iCount;
	}

protected:
	int64_t		m_iLimit = 0;		///< entries allocated
};

} // namespace sph

#define ARRAY_FOREACH(_index,_array) \
	for ( int _index=0; _index<_array.GetLength(); ++_index )

#define ARRAY_FOREACH_COND(_index,_array,_cond) \
	for ( int _index=0; _index<_array.GetLength() && (_cond); ++_index )

//////////////////////////////////////////////////////////////////////////

/// old well-known vector
template < typename T, typename R=sph::DefaultRelimit >
using CSphVector = sph::Vector_T < T, sph::DefaultCopy_T<T>, R >;

template < typename T, typename R=sph::DefaultRelimit, int STATICSIZE=4096/sizeof(T) >
using LazyVector_T = sph::Vector_T<T, sph::DefaultCopy_T<T>, R, sph::LazyStorage_T<T, STATICSIZE> >;

/// swap-vector
template < typename T >
using CSphSwapVector = sph::Vector_T < T, sph::SwapCopy_T<T> >;

/// tight-vector
template < typename T >
using CSphTightVector =  CSphVector < T, sph::TightRelimit >;

//////////////////////////////////////////////////////////////////////////

/// dynamically allocated fixed-size vector
template < typename T >
class CSphFixedVector : public ISphNoncopyable, public VecTraits_T<T>
{
protected:
	using VecTraits_T<T>::m_pData;
	using VecTraits_T<T>::m_iCount;

public:
	explicit CSphFixedVector ( int iSize )
	{
		m_iCount = iSize;
		assert ( iSize>=0 );
		m_pData = ( iSize>0 ) ? new T [ iSize ] : nullptr;
	}

	~CSphFixedVector ()
	{
		SafeDeleteArray ( m_pData );
	}

	CSphFixedVector ( CSphFixedVector&& rhs ) noexcept
	{
		m_pData = rhs.m_pData;
		m_iCount = rhs.m_iCount;
		rhs.m_pData = nullptr;
		rhs.m_iCount = 0;
	}

	CSphFixedVector & operator= ( CSphFixedVector&& rhs ) noexcept
	{
		if ( &rhs!=this )
		{
			SafeDeleteArray ( m_pData );
			m_pData = rhs.m_pData;
			m_iCount = rhs.m_iCount;

			rhs.m_pData = nullptr;
			rhs.m_iCount = 0;
		}
		return *this;
	}

	void Reset ( int iSize )
	{
		SafeDeleteArray ( m_pData );
		assert ( iSize>=0 );
		m_pData = ( iSize>0 ) ? new T [ iSize ] : nullptr;
		m_iCount = iSize;
	}

	void CopyFrom ( const VecTraits_T<T>& dOrigin )
	{
		Reset ( dOrigin.GetLength() );
		memcpy ( m_pData, dOrigin.begin(), dOrigin.GetLengthBytes());
	}

	T * LeakData ()
	{
		T * pData = m_pData;
		m_pData = nullptr;
		Reset ( 0 );
		return pData;
	}

	/// swap
	void SwapData ( CSphFixedVector<T> & rhs )
	{
		Swap ( m_pData, rhs.m_pData );
		Swap ( m_iCount, rhs.m_iCount );
	}

	void Set ( T * pData, int iSize )
	{
		SafeDeleteArray ( m_pData );
		m_pData = pData;
		m_iCount = iSize;
	}
};

//////////////////////////////////////////////////////////////////////////

/// simple dynamic hash
/// implementation: fixed-size bucket + chaining
/// keeps the order, so Iterate() return the entries in the order they was inserted
/// WARNING: slow copy
template < typename T, typename KEY, typename HASHFUNC, int LENGTH >
class CSphOrderedHash
{
protected:
	struct HashEntry_t
	{
		KEY				m_tKey;						///< key, owned by the hash
		T 				m_tValue;					///< data, owned by the hash
		HashEntry_t *	m_pNextByHash = nullptr;	///< next entry in hash list
		HashEntry_t *	m_pPrevByOrder = nullptr;	///< prev entry in the insertion order
		HashEntry_t *	m_pNextByOrder = nullptr;	///< next entry in the insertion order
	};


protected:
	HashEntry_t *	m_dHash [ LENGTH ];			///< all the hash entries
	HashEntry_t *	m_pFirstByOrder = nullptr;	///< first entry in the insertion order
	HashEntry_t *	m_pLastByOrder = nullptr;	///< last entry in the insertion order
	int				m_iLength = 0;				///< entries count

protected:

	inline unsigned int HashPos ( const KEY & tKey ) const
	{
		return ( ( unsigned int ) HASHFUNC::Hash ( tKey ) ) % LENGTH;
	}

	/// find entry by key
	HashEntry_t * FindByKey ( const KEY & tKey ) const
	{
		HashEntry_t * pEntry = m_dHash[HashPos ( tKey )];

		while ( pEntry )
		{
			if ( pEntry->m_tKey==tKey )
				return pEntry;
			pEntry = pEntry->m_pNextByHash;
		}
		return nullptr;
	}

	HashEntry_t * AddImpl ( const KEY &tKey )
	{
		// check if this key is already hashed
		HashEntry_t ** ppEntry = &m_dHash[HashPos ( tKey )];
		HashEntry_t * pEntry = *ppEntry;
		while ( pEntry )
		{
			if ( pEntry->m_tKey==tKey )
				return nullptr;

			ppEntry = &pEntry->m_pNextByHash;
			pEntry = pEntry->m_pNextByHash;
		}

		// it's not; let's add the entry
		assert ( !pEntry );
		assert ( !*ppEntry );

		pEntry = new HashEntry_t;
		pEntry->m_tKey = tKey;

		*ppEntry = pEntry;

		if ( !m_pFirstByOrder )
			m_pFirstByOrder = pEntry;

		if ( m_pLastByOrder )
		{
			assert ( !m_pLastByOrder->m_pNextByOrder );
			assert ( !pEntry->m_pNextByOrder );
			m_pLastByOrder->m_pNextByOrder = pEntry;
			pEntry->m_pPrevByOrder = m_pLastByOrder;
		}
		m_pLastByOrder = pEntry;

		++m_iLength;
		return pEntry;
	}

public:
	/// ctor
	CSphOrderedHash ()
	{
		for ( auto &pHash : m_dHash )
			pHash = nullptr;
	}

	/// dtor
	~CSphOrderedHash ()
	{
		Reset ();
	}

	/// reset
	void Reset ()
	{
		assert ( ( m_pFirstByOrder && m_iLength ) || ( !m_pFirstByOrder && !m_iLength ) );
		HashEntry_t * pKill = m_pFirstByOrder;
		while ( pKill )
		{
			HashEntry_t * pNext = pKill->m_pNextByOrder;
			SafeDelete ( pKill );
			pKill = pNext;
		}

		for ( auto &pHash : m_dHash )
			pHash = nullptr;

		m_pFirstByOrder = nullptr;
		m_pLastByOrder = nullptr;
		m_pIterator = nullptr;
		m_iLength = 0;
	}

	/// add new entry
	/// returns true on success
	/// returns false if this key is already hashed
	bool Add ( T&& tValue, const KEY & tKey )
	{
		// check if this key is already hashed
		HashEntry_t * pEntry = AddImpl ( tKey );
		if ( !pEntry )
			return false;
		pEntry->m_tValue = std::move ( tValue );
		return true;
	}

	bool Add ( const T & tValue, const KEY & tKey )
	{
		// check if this key is already hashed
		HashEntry_t * pEntry = AddImpl ( tKey );
		if ( !pEntry )
			return false;
		pEntry->m_tValue = tValue;
		return true;
	}

	/// add new entry
	/// returns ref to just intersed or previously existed value
	T & AddUnique ( const KEY & tKey )
	{
		// check if this key is already hashed
		HashEntry_t ** ppEntry = &m_dHash[HashPos ( tKey )];
		HashEntry_t * pEntry = *ppEntry;

		while ( pEntry )
		{
			if ( pEntry->m_tKey==tKey )
				return pEntry->m_tValue;

			ppEntry = &pEntry->m_pNextByHash;
			pEntry = *ppEntry;
		}

		// it's not; let's add the entry
		assert ( !pEntry );

		pEntry = new HashEntry_t;
		pEntry->m_tKey = tKey;

		*ppEntry = pEntry;

		if ( !m_pFirstByOrder )
			m_pFirstByOrder = pEntry;

		if ( m_pLastByOrder )
		{
			assert ( !m_pLastByOrder->m_pNextByOrder );
			assert ( !pEntry->m_pNextByOrder );
			m_pLastByOrder->m_pNextByOrder = pEntry;
			pEntry->m_pPrevByOrder = m_pLastByOrder;
		}
		m_pLastByOrder = pEntry;

		++m_iLength;
		return pEntry->m_tValue;
	}

	/// delete an entry
	bool Delete ( const KEY & tKey )
	{
		auto uHash = HashPos ( tKey );
		HashEntry_t * pEntry = m_dHash [ uHash ];

		HashEntry_t * pPrevEntry = nullptr;
		HashEntry_t * pToDelete = nullptr;
		while ( pEntry )
		{
			if ( pEntry->m_tKey==tKey )
			{
				pToDelete = pEntry;
				if ( pPrevEntry )
					pPrevEntry->m_pNextByHash = pEntry->m_pNextByHash;
				else
					m_dHash [ uHash ] = pEntry->m_pNextByHash;

				break;
			}

			pPrevEntry = pEntry;
			pEntry = pEntry->m_pNextByHash;
		}

		if ( !pToDelete )
			return false;

		if ( pToDelete->m_pPrevByOrder )
			pToDelete->m_pPrevByOrder->m_pNextByOrder = pToDelete->m_pNextByOrder;
		else
			m_pFirstByOrder = pToDelete->m_pNextByOrder;

		if ( pToDelete->m_pNextByOrder )
			pToDelete->m_pNextByOrder->m_pPrevByOrder = pToDelete->m_pPrevByOrder;
		else
			m_pLastByOrder = pToDelete->m_pPrevByOrder;

		// step the iterator one item back - to gracefully hold deletion in iteration cycle
		if ( pToDelete==m_pIterator )
			m_pIterator = pToDelete->m_pPrevByOrder;

		SafeDelete ( pToDelete );
		--m_iLength;

		return true;
	}

	/// check if key exists
	bool Exists ( const KEY & tKey ) const
	{
		return FindByKey ( tKey )!=nullptr;
	}

	/// get value pointer by key
	T * operator () ( const KEY & tKey ) const
	{
		HashEntry_t * pEntry = FindByKey ( tKey );
		return pEntry ? &pEntry->m_tValue : nullptr;
	}

	/// get value reference by key, asserting that the key exists in hash
	T & operator [] ( const KEY & tKey ) const
	{
		HashEntry_t * pEntry = FindByKey ( tKey );
		assert ( pEntry && "hash missing value in operator []" );

		return pEntry->m_tValue;
	}

	/// copying
	CSphOrderedHash<T,KEY,HASHFUNC,LENGTH> & operator = ( const CSphOrderedHash<T,KEY,HASHFUNC,LENGTH> & rhs )
	{
		if ( this!=&rhs )
		{
			Reset ();
			for ( rhs.IterateStart (); rhs.IterateNext(); )
				Add ( rhs.IterateGet(), rhs.IterateGetKey() );
		}
		return *this;
	}

	/// copying ctor
	CSphOrderedHash<T,KEY,HASHFUNC,LENGTH> ( const CSphOrderedHash<T,KEY,HASHFUNC,LENGTH> & rhs )
	{
		*this = rhs;
	}

	/// length query
	int GetLength () const
	{
		return m_iLength;
	}

public:
	/// start iterating
	void IterateStart () const
	{
		m_pIterator = nullptr;
	}

	/// go to next existing entry
	bool IterateNext () const
	{
		m_pIterator = m_pIterator ? m_pIterator->m_pNextByOrder : m_pFirstByOrder;
		return m_pIterator!=nullptr;
	}

	/// get entry value
	T & IterateGet () const
	{
		assert ( m_pIterator );
		return m_pIterator->m_tValue;
	}

	/// get entry key
	const KEY & IterateGetKey () const
	{
		assert ( m_pIterator );
		return m_pIterator->m_tKey;
	}

	/// go to next existing entry in terms of external independed iterator
	bool IterateNext ( void ** ppCookie ) const
	{
		auto ** ppIterator = reinterpret_cast < HashEntry_t** > ( ppCookie );
		*ppIterator = ( *ppIterator ) ? ( *ppIterator )->m_pNextByOrder : m_pFirstByOrder;
		return ( *ppIterator )!=nullptr;
	}

	/// get entry value in terms of external independed iterator
	static T & IterateGet ( void ** ppCookie )
	{
		assert ( ppCookie );
		auto ** ppIterator = reinterpret_cast < HashEntry_t** > ( ppCookie );
		assert ( *ppIterator );
		return ( *ppIterator )->m_tValue;
	}

	/// get entry key in terms of external independed iterator
	static const KEY & IterateGetKey ( void ** ppCookie )
	{
		assert ( ppCookie );
		auto ** ppIterator = reinterpret_cast < HashEntry_t** > ( ppCookie );
		assert ( *ppIterator );
		return ( *ppIterator )->m_tKey;
	}


private:
	/// current iterator
	mutable HashEntry_t *	m_pIterator = nullptr;
};

/// very popular and so, moved here
/// use integer values as hash values (like document IDs, for example)
struct IdentityHash_fn
{
	template <typename INT>
	static inline INT Hash ( INT iValue )	{ return iValue; }
};

/////////////////////////////////////////////////////////////////////////////

/// immutable C string proxy
struct CSphString
{
protected:
	char *				m_sValue = nullptr;
	// Empty ("") string optimization.
	static char EMPTY[];

private:
	/// safety gap after the string end; for instance, UTF-8 Russian stemmer
	/// which treats strings as 16-bit word sequences needs this in some cases.
	/// note that this zero-filled gap does NOT include trailing C-string zero,
	/// and does NOT affect strlen() as well.
	static const int	SAFETY_GAP = 4;

	inline void SafeFree ()
	{ if ( m_sValue!=EMPTY ) SafeDeleteArray ( m_sValue ); }

public:
	CSphString () = default;

	// take a note this is not an explicit constructor
	// so a lot of silent constructing and deleting of strings is possible
	// Example:
	// SmallStringHash_T<int> hHash;
	// ...
	// hHash.Exists ( "asdf" ); // implicit CSphString construction and deletion here
	CSphString ( const CSphString & rhs )
	{
		*this = rhs;
	}

	CSphString ( CSphString&& rhs ) noexcept
		: m_sValue ( rhs.m_sValue )
	{
		rhs.m_sValue = nullptr;
	}

	~CSphString ()
	{
		SafeFree();
	}

	const char * cstr () const
	{
		return m_sValue;
	}

	const char * scstr() const
	{
		return m_sValue ? m_sValue : EMPTY;
	}

	inline bool operator == ( const char * t ) const
	{
		if ( !t || !m_sValue )
			return ( ( !t && !m_sValue ) || ( !t && m_sValue && !*m_sValue ) || ( !m_sValue && t && !*t ) );
		return strcmp ( m_sValue, t )==0;
	}

	inline bool operator == ( const CSphString & t ) const
	{
		return operator==( t.cstr() );
	}

	inline bool operator != ( const CSphString & t ) const
	{
		return !operator==( t );
	}

	bool operator != ( const char * t ) const
	{
		return !operator==( t );
	}

	// compare ignoring case
	inline bool EqN ( const char * t ) const
	{
		if ( !t || !m_sValue )
			return ( ( !t && !m_sValue ) || ( !t && m_sValue && !*m_sValue ) || ( !m_sValue && t && !*t ) );
		return strcasecmp ( m_sValue, t )==0;
	}

	inline bool EqN ( const CSphString &t ) const
	{
		return EqN ( t.cstr () );
	}

	CSphString ( const char * sString ) // NOLINT
	{
		if ( sString )
		{
			if ( sString[0]=='\0' )
			{
				m_sValue = EMPTY;
			} else
			{
				int iLen = 1+strlen(sString);
				m_sValue = new char [ iLen+SAFETY_GAP ];

				strcpy ( m_sValue, sString ); // NOLINT
				memset ( m_sValue+iLen, 0, SAFETY_GAP );
			}
		}
	}

	CSphString ( const char * sValue, int iLen )
	{
		SetBinary ( sValue, iLen );
	}

	CSphString & operator = ( const CSphString & rhs )
	{
		if ( m_sValue==rhs.m_sValue )
			return *this;
		SafeFree ();
		if ( rhs.m_sValue )
		{
			if ( rhs.m_sValue[0]=='\0' )
			{
				m_sValue = EMPTY;
			} else
			{
				int iLen = 1+strlen(rhs.m_sValue);
				m_sValue = new char [ iLen+SAFETY_GAP ];

				strcpy ( m_sValue, rhs.m_sValue ); // NOLINT
				memset ( m_sValue+iLen, 0, SAFETY_GAP );
			}
		}
		return *this;
	}

	CSphString & operator = ( CSphString&& rhs ) noexcept
	{
		if ( m_sValue==rhs.m_sValue )
			return *this;
		SafeFree ();

		if ( rhs.m_sValue )
		{
			if ( rhs.m_sValue[0]=='\0' )
			{
				m_sValue = EMPTY;
			} else
			{
				m_sValue = rhs.m_sValue;
				rhs.m_sValue = nullptr;
			}
		}
		return *this;
	}

	CSphString SubString ( int iStart, int iCount ) const
	{
		#ifndef NDEBUG
		int iLen = strlen(m_sValue);
		#endif
		assert ( iStart>=0 && iStart<iLen );
		assert ( iCount>0 );
		assert ( (iStart+iCount)>=0 && (iStart+iCount)<=iLen );

		CSphString sRes;
		sRes.m_sValue = new char [ 1+SAFETY_GAP+iCount ];
		strncpy ( sRes.m_sValue, m_sValue+iStart, iCount );
		memset ( sRes.m_sValue+iCount, 0, 1+SAFETY_GAP );
		return sRes;
	}

	// tries to reuse memory buffer, but calls Length() every time
	// hope this won't kill performance on a huge strings
	void SetBinary ( const char * sValue, int iLen )
	{
		if ( Length ()<( iLen + SAFETY_GAP + 1 ) )
		{
			SafeFree ();
			if ( !sValue )
				m_sValue = EMPTY;
			else
			{
				m_sValue = new char [ 1+SAFETY_GAP+iLen ];
				memcpy ( m_sValue, sValue, iLen );
				memset ( m_sValue+iLen, 0, 1+SAFETY_GAP );
			}
			return;
		}

		if ( sValue && iLen )
		{
			memcpy ( m_sValue, sValue, iLen );
			memset ( m_sValue + iLen, 0, 1 + SAFETY_GAP );
		} else
		{
			SafeFree ();
			m_sValue = EMPTY;
		}
	}

	void Reserve ( int iLen )
	{
		SafeFree ();
		m_sValue = new char [ 1+SAFETY_GAP+iLen ];
		memset ( m_sValue, 0, 1+SAFETY_GAP+iLen );
	}

	const CSphString & SetSprintf ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) )
	{
		char sBuf[1024];
		va_list ap;

		va_start ( ap, sTemplate );
		vsnprintf ( sBuf, sizeof(sBuf), sTemplate, ap );
		va_end ( ap );

		(*this) = sBuf;
		return (*this);
	}

	/// format value using provided va_list
	const CSphString & SetSprintfVa ( const char * sTemplate, va_list ap )
	{
		char sBuf[1024];
		vsnprintf ( sBuf, sizeof(sBuf), sTemplate, ap );

		(*this) = sBuf;
		return (*this);
	}
	/// \return true if internal char* ptr is null, of value is empty.
	bool IsEmpty () const
	{
		if ( !m_sValue )
			return true;
		return ( (*m_sValue)=='\0' );
	}

	CSphString & ToLower ()
	{
		if ( m_sValue )
			for ( char * s=m_sValue; *s; s++ )
				*s = (char) tolower ( *s );
		return *this;
	}

	CSphString & ToUpper ()
	{
		if ( m_sValue )
			for ( char * s=m_sValue; *s; s++ )
				*s = (char) toupper ( *s );
		return *this;
	}

	void Swap ( CSphString & rhs )
	{
		::Swap ( m_sValue, rhs.m_sValue );
	}

	/// \return true if the string begins with sPrefix
	bool Begins ( const char * sPrefix ) const
	{
		if ( !m_sValue || !sPrefix )
			return false;
		return strncmp ( m_sValue, sPrefix, strlen(sPrefix) )==0;
	}

	/// \return true if the string ends with sSuffix
	bool Ends ( const char * sSuffix ) const
	{
		if ( !m_sValue || !sSuffix )
			return false;

		int iVal = strlen ( m_sValue );
		int iSuffix = strlen ( sSuffix );
		if ( iVal<iSuffix )
			return false;
		return strncmp ( m_sValue+iVal-iSuffix, sSuffix, iSuffix )==0;
	}

	/// trim leading and trailing spaces
	void Trim ()
	{
		if ( m_sValue )
		{
			const char * sStart = m_sValue;
			const char * sEnd = m_sValue + strlen(m_sValue) - 1;
			while ( sStart<=sEnd && isspace ( (unsigned char)*sStart ) ) sStart++;
			while ( sStart<=sEnd && isspace ( (unsigned char)*sEnd ) ) sEnd--;
			memmove ( m_sValue, sStart, sEnd-sStart+1 );
			m_sValue [ sEnd-sStart+1 ] = '\0';
		}
	}

	int Length () const
	{
		return m_sValue ? (int)strlen(m_sValue) : 0;
	}

	/// \return internal string and releases it from being destroyed in d-tr
	char * Leak ()
	{
		if ( m_sValue==EMPTY )
		{
			m_sValue = nullptr;
			auto * pBuf = new char[1];
			pBuf[0] = '\0';
			return pBuf;
		}
		char * pBuf = m_sValue;
		m_sValue = nullptr;
		return pBuf;
	}

	/// \return internal string and releases it from being destroyed in d-tr
	void LeakToVec ( CSphVector<BYTE> &dVec )
	{
		if ( m_sValue==EMPTY )
		{
			m_sValue = nullptr;
			auto * pBuf = new char[1];
			pBuf[0] = '\0';
			dVec.AdoptData ((BYTE*)pBuf,0,1);
			return;
		}
		int iLen = Length();
		dVec.AdoptData ( ( BYTE * ) m_sValue, iLen, iLen + 1 + SAFETY_GAP );
		m_sValue = nullptr;
	}

	/// take string from outside and 'adopt' it as own child.
	void Adopt ( char ** sValue )
	{
		SafeFree ();
		m_sValue = *sValue;
		*sValue = nullptr;
	}

	void Adopt ( char * && sValue )
	{
		SafeFree ();
		m_sValue = sValue;
		sValue = nullptr;
	}

	/// compares using strcmp
	bool operator < ( const CSphString & b ) const
	{
		if ( !m_sValue && !b.m_sValue )
			return false;
		if ( !m_sValue || !b.m_sValue )
			return !m_sValue;
		return strcmp ( m_sValue, b.m_sValue ) < 0;
	}

	void Unquote()
	{
		int l = Length();
		if ( l && m_sValue[0]=='\'' && m_sValue[l-1]=='\'' )
		{
			memmove ( m_sValue, m_sValue+1, l-2 );
			m_sValue[l-2] = '\0';
		}
	}

	static int GetGap () { return SAFETY_GAP; }
};

/// string swapper
inline void Swap ( CSphString & v1, CSphString & v2 )
{
	v1.Swap ( v2 );
}

// commonly used vector of strings
using StrVec_t = CSphVector<CSphString>;

// vector of byte vectors
using BlobVec_t = CSphVector<CSphVector<BYTE> >;

/////////////////////////////////////////////////////////////////////////////

/// immutable string/int/float variant list proxy
/// used in config parsing
struct CSphVariant
{
protected:
	CSphString		m_sValue;
	int				m_iValue = 0;
	int64_t			m_i64Value = 0;
	float			m_fValue = 0.0f;

public:
	CSphVariant *	m_pNext = nullptr;
	// tags are used for handling multiple same keys
	bool			m_bTag = false; // 'true' means override - no multi-valued; 'false' means multi-valued - chain them
	int				m_iTag = 0; // stores order like in config file

public:
	/// default ctor
	CSphVariant () = default;


	/// ctor from C string
	CSphVariant ( const char * sString, int iTag )
		: m_sValue ( sString )
		, m_iValue ( sString ? atoi ( sString ) : 0 )
		, m_i64Value ( sString ? (int64_t)strtoull ( sString, nullptr, 10 ) : 0 )
		, m_fValue ( sString ? (float)atof ( sString ) : 0.0f )
		, m_iTag ( iTag )
	{
	}

	/// copy ctor
	CSphVariant ( const CSphVariant & rhs )
	{
		m_pNext = nullptr;
		*this = rhs;
	}

	/// default dtor
	/// WARNING: automatically frees linked items!
	~CSphVariant ()
	{
		SafeDelete ( m_pNext );
	}

	const char * cstr() const { return m_sValue.cstr(); }

	const CSphString & strval () const { return m_sValue; }
	int intval () const	{ return m_iValue; }
	int64_t int64val () const { return m_i64Value; }
	float floatval () const	{ return m_fValue; }

	/// default copy operator
	CSphVariant & operator = ( const CSphVariant & rhs )
	{
		assert ( !m_pNext );
		if ( rhs.m_pNext )
			m_pNext = new CSphVariant ( *rhs.m_pNext );

		m_sValue = rhs.m_sValue;
		m_iValue = rhs.m_iValue;
		m_i64Value = rhs.m_i64Value;
		m_fValue = rhs.m_fValue;
		m_bTag = rhs.m_bTag;
		m_iTag = rhs.m_iTag;

		return *this;
	}

	bool operator== ( const char * s ) const { return m_sValue==s; }
	bool operator!= ( const char * s ) const { return m_sValue!=s; }
};

/// text delimiter
/// returns "" first time, then defined delimiter starting from 2-nd call
/// NOTE that using >1 call in one chain like out << comma << "foo" << comma << "bar" is NOT defined,
/// since order of calling 2 commas here is undefined (so, you may take "foo, bar", but may ", foobar" also).
/// Use out << comma << "foo"; out << comma << "bar"; in the case
class Comma_c
{
protected:
	const char * m_sDelimiter = nullptr;
	int m_iLength = 0;
	bool m_bStarted = false;

public:
	// standalone - call () when necessary
	Comma_c ( const char * sDelim = ", " )
		: m_sDelimiter ( sDelim )
	{
		if ( sDelim )
			m_iLength = strlen ( sDelim );
	}

	inline bool Started() const { return m_bStarted; };

	// returns "" first time, m_sDelimiter after
	operator const char * ()
	{
		auto * sComma = m_bStarted ? m_sDelimiter : nullptr;
		m_bStarted = true;
		return sComma ? sComma : "";
	}
};


/// string builder
/// somewhat quicker than a series of SetSprintf()s
/// lets you build strings bigger than 1024 bytes, too
class StringBuilder_c : public ISphNoncopyable
{
	// RAII comma for frequently used pattern of pushing into StringBuilder many values separated by ',', ';', etc.
	// When in scope, inject prefix before very first item, or delimiter before each next.
	class LazyComma_c : public Comma_c
	{
		friend class StringBuilder_c;

		bool m_bSkipNext = false;
		const char * m_sPrefix = nullptr;
		const char * m_sSuffix = nullptr;
		LazyComma_c * m_pPrevious = nullptr;

		// c-tr for managed - linked StringBuilder will inject RawComma() on each call, terminator at end
		LazyComma_c ( LazyComma_c * pPrevious, const char * sDelim, const char * sPrefix, const char * sTerm )
			: Comma_c ( sDelim )
			, m_sPrefix ( sPrefix )
			, m_sSuffix ( sTerm )
			, m_pPrevious ( pPrevious )
		{}

		~LazyComma_c ()
		{
			SafeDelete ( m_pPrevious );
		}

	public:
		const char * RawComma ( int &iLen, StringBuilder_c &dBuilder )
		{
			if ( m_bSkipNext )
			{
				m_bSkipNext = false;
				iLen = 0;
				return nullptr;
			}

			if ( Started() )
			{
				iLen = m_iLength;;
				return m_sDelimiter;
			}

			m_bStarted = true;
			if ( m_pPrevious )
			{
				int iPrevLen = 0;
				const char * sPrevDelim = m_pPrevious->RawComma ( iPrevLen, dBuilder );
				dBuilder.AppendRawChars ( sPrevDelim );
			}

			iLen = m_sPrefix ? strlen ( m_sPrefix ) : 0;
			return m_sPrefix;
		}

		inline void SkipNext () { m_bSkipNext = true; }

	};

private:
	void NewBuffer ();
	void InitBuffer ();
	friend class ScopedComma_c;

protected:
	char * m_sBuffer = nullptr;
	int m_iSize = 0;
	int m_iUsed = 0;
	static const BYTE uSTEP = 64; // how much to grow if no space left
	LazyComma_c * m_pDelimiter = nullptr;
	void Grow ( int iLen ); // unconditionally shrink enough to place at least iLen more bytes

public:
	// creates and m.b. start block
	StringBuilder_c ( const char * sDel = nullptr, const char * sPref = nullptr, const char * sTerm = nullptr );
	~StringBuilder_c ();

	StringBuilder_c ( StringBuilder_c&& rhs ) noexcept;
	StringBuilder_c& operator= ( StringBuilder_c&& rhs ) noexcept;

	// reset to initial state
	void Clear ();

	// get current build value
	const char * cstr () const { return m_sBuffer ? m_sBuffer : ""; }
	const char * rawstr () const { return m_sBuffer; }

	// move out (de-own) value
	BYTE * Leak ();
	void MoveTo ( CSphString &sTarget ); // leak to string

	// get state
	bool IsEmpty () const { return !m_sBuffer || m_sBuffer[0]=='\0'; }
	inline int GetLength () const { return m_iUsed; }

	// different kind of fullfillments
	StringBuilder_c &AppendChars ( const char * sText, int iLen, char cQuote = '\0' );
	StringBuilder_c &AppendString ( const CSphString &sText, char cQuote = '\0' );
	StringBuilder_c &operator+= ( const char * sText );
	StringBuilder_c &operator<< ( const VecTraits_T<char> &sText );
	inline StringBuilder_c &operator<< ( const char * sText ) { return *this += sText; }
	inline StringBuilder_c &operator<< ( const CSphString &sText ) { return *this += sText.cstr (); }
	inline StringBuilder_c &operator<< ( const CSphVariant &sText )	{ return *this += sText.cstr (); }

	// append 1 char despite any blocks.
	inline void RawC ( char cChar ) { GrowEnough ( 1 ); *end () = cChar; ++m_iUsed; }
	void AppendRawChars ( const char * sText ); // append without any commas
	StringBuilder_c &SkipNextComma ();
	StringBuilder_c &AppendName ( const char * sName); // append

	// these use standard sprintf() inside
	StringBuilder_c &vAppendf ( const char * sTemplate, va_list ap );
	StringBuilder_c &Appendf ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );

	// these use or own implementation sph::Sprintf which provides also some sugar
	StringBuilder_c &vSprintf ( const char * sTemplate, va_list ap );
	StringBuilder_c &Sprintf ( const char * sTemplate, ... );

	// comma manipulations
	// start new comma block; return pointer to it (for future possible reference in FinishBlocks())
	LazyComma_c * StartBlock ( const char * sDel = ", ", const char * sPref = nullptr, const char * sTerm = nullptr );

	// finish and close last opened comma block.
	// bAllowEmpty - close empty block output nothing(default), or prefix/suffix pair (if any).
	void FinishBlock ( bool bAllowEmpty = true );

	// finish and close all blocks including pLevels (by default - all blocks)
	void FinishBlocks ( LazyComma_c * pLevels = nullptr, bool bAllowEmpty = true );

	inline char * begin() const { return m_sBuffer; }
	inline char * end () const { return m_sBuffer + m_iUsed; }

	// shrink, if necessary, to be able to fit at least iLen more chars
	inline void GrowEnough ( int iLen )
	{
		if ( m_iUsed + iLen<m_iSize )
			return;
		Grow ( iLen );
	}

	// support for sph::Sprintf - emulate POD 'char*'
	inline StringBuilder_c & operator++() { GrowEnough ( 1 ); ++m_iUsed; return *this; }
	inline void operator+= (int i) { GrowEnough ( i ); m_iUsed += i; }
};

using Str_b = StringBuilder_c;

namespace EscBld {	// what kind of changes will do AppendEscaped of escaped string builder:
	enum eAct : BYTE
	{
		eNone		= 0, // [comma,] append raw text without changes
		eFixupSpace	= 1, // [comma,] change \t, \n, \r into spaces
		eEscape		= 2, // [comma,] all escaping according to provided interface
		eAll		= 3, // [comma,] escape and change spaces
		eSkipComma	= 4, // force to NOT prefix comma (if any active)
	};
}

template < typename T >
class EscapedStringBuilder_T : public StringBuilder_c
{
public:
	void AppendEscaped ( const char * sText, BYTE eWhat=EscBld::eAll, int iLen=-1 )
	{
		if ( ( !sText || !*sText ) )
		{
			if ( eWhat & EscBld::eEscape )
				iLen=0;
			else
				return;
		}

		// process comma
		int iComma = 0;
		if ( eWhat & EscBld::eSkipComma )
			eWhat -= EscBld::eSkipComma;
		else
		{
			const char * sPrefix = m_pDelimiter ? m_pDelimiter->RawComma ( iComma, *this ) : nullptr;
			GrowEnough ( iComma );
			if ( iComma )
			{
				assert ( sPrefix );
				memcpy ( end (), sPrefix, iComma );
				m_iUsed+=iComma;
			}
		}

		const char * pSrc = sText;
		int iFinalLen = 0;
		if ( eWhat & EscBld::eEscape )
		{
			if ( iLen<0 )
			{
				for ( ; *pSrc; ++pSrc )
					if ( T::IsEscapeChar (*pSrc) )
						++iFinalLen;
			} else
			{
				for ( ; iLen; ++pSrc, --iLen )
					if ( T::IsEscapeChar ( *pSrc ) )
						++iFinalLen;
			}
			iLen = (int) (pSrc - sText);
			iFinalLen += iLen+2; // 2 quotes: 1 prefix, 2 postfix.
		} else if ( iLen<0 )
			iFinalLen = iLen = (int) strlen (sText);
		else
			iFinalLen = iLen;

		GrowEnough ( iFinalLen+1 ); // + zero terminator

		auto * pCur = end();
		switch (eWhat)
		{
		case EscBld::eNone:
			memcpy ( pCur, sText, iFinalLen );
			pCur += iFinalLen;
			break;
		case EscBld::eFixupSpace:
			for ( ; iLen; --iLen )
			{
				char s = *sText++;
				*pCur++ = strchr ( "\t\n\r", s ) ? ' ' : s ;
			}
			break;
		case EscBld::eEscape:
			*pCur++ = T::cQuote;
			for ( ; iLen; --iLen )
			{
				char s = *sText++;
				if ( T::IsEscapeChar ( s ) )
				{
					*pCur++ = '\\';
					*pCur++ = T::GetEscapedChar ( s );
				} else
					*pCur++ = s;
			}
			*pCur++ = T::cQuote;
			break;
		case EscBld::eAll:
		default:
			*pCur++ = T::cQuote;
			for ( ; iLen; --iLen )
			{
				char s = *sText++;
				if ( T::IsEscapeChar ( s ) )
				{
					*pCur++ = '\\';
					*pCur++ = T::GetEscapedChar ( s );
				} else
					*pCur++ = strchr ( "\t\n\r", s ) ? ' ' : s;
			}
			*pCur++ = T::cQuote;
		}
		*pCur = '\0';
		m_iUsed += iFinalLen;
	}

	EscapedStringBuilder_T &SkipNextComma ()
	{
		StringBuilder_c::SkipNextComma ();
		return *this;
	}

	EscapedStringBuilder_T &AppendName ( const char * sName )
	{
		StringBuilder_c::AppendName(sName);
		return *this;
	}
};

class ScopedComma_c
{
	StringBuilder_c * m_pOwner = nullptr;
	StringBuilder_c::LazyComma_c * m_pLevel = nullptr;

public:
	ScopedComma_c () = default;
	explicit ScopedComma_c ( StringBuilder_c & tOwner,
		const char * sDel = ", ", const char * sPref = nullptr, const char * sTerm = nullptr )
		: m_pOwner ( &tOwner )
	{
		m_pLevel = tOwner.StartBlock (sDel, sPref, sTerm);
	}

	void Init ( StringBuilder_c &tOwner,
		const char * sDel = ", ", const char * sPref = nullptr, const char * sTerm = nullptr )
	{
		assert ( !m_pOwner );
		if ( m_pOwner )
			return;
		m_pOwner = &tOwner;
		m_pLevel = tOwner.StartBlock ( sDel, sPref, sTerm );
	}

	~ScopedComma_c()
	{
		if ( m_pOwner )
			m_pOwner->FinishBlocks ( m_pLevel );
	}
};

//////////////////////////////////////////////////////////////////////////

/// name+int pair
struct CSphNamedInt
{
	CSphString m_sName;
	int m_iValue = 0;

	CSphNamedInt() = default;
	CSphNamedInt ( const CSphString& sName, int iValue)
		: m_sName ( sName ), m_iValue (iValue) {};
};

inline void Swap ( CSphNamedInt & a, CSphNamedInt & b )
{
	a.m_sName.Swap ( b.m_sName );
	Swap ( a.m_iValue, b.m_iValue );
}


/////////////////////////////////////////////////////////////////////////////

/// string hash function
struct CSphStrHashFunc
{
	static int Hash ( const CSphString & sKey );
};

/// small hash with string keys
template < typename T, int LENGTH = 256 >
using SmallStringHash_T = CSphOrderedHash < T, CSphString, CSphStrHashFunc, LENGTH >;


namespace sph {

// used to simple add/delete strings and check if a string was added by [] op
class StringSet : private SmallStringHash_T<bool>
{
	using BASE = SmallStringHash_T<bool>;
public:
	inline void Add ( const CSphString& sKey )
	{
		BASE::Add ( true, sKey );
	}

	inline void Delete ( const CSphString& sKey )
	{
		BASE::Delete ( sKey );
	}

	inline bool operator[] ( const CSphString& sKey ) const
	{
		if ( BASE::Exists ( sKey ) )
			return BASE::operator[] ( sKey );
		return false;
	}
};
}

//////////////////////////////////////////////////////////////////////////

/// pointer with automatic safe deletion when going out of scope
template < typename T >
class CSphScopedPtr : public ISphNoncopyable
{
public:
	explicit		CSphScopedPtr ( T * pPtr )	{ m_pPtr = pPtr; }
					~CSphScopedPtr ()			{ SafeDelete ( m_pPtr ); }
	T *				operator -> () const		{ return m_pPtr; }
	T *				Ptr () const				{ return m_pPtr; }
	explicit operator bool () const				{ return m_pPtr!=nullptr; }
	CSphScopedPtr &	operator = ( T * pPtr )		{ SafeDelete ( m_pPtr ); m_pPtr = pPtr; return *this; }
	T *				LeakPtr ()					{ T * pPtr = m_pPtr; m_pPtr = NULL; return pPtr; }
	void			Reset ()					{ SafeDelete ( m_pPtr ); }

protected:
	T *				m_pPtr;
};

//////////////////////////////////////////////////////////////////////////

/// refcounted base
/// WARNING, FOR SINGLE-THREADED USE ONLY
struct ISphRefcounted : public ISphNoncopyable
{
protected:
					ISphRefcounted () : m_iRefCount ( 1 ) {}
	virtual			~ISphRefcounted () {}; // gcc 4.7.2 hates `=default` here

public:
	void			AddRef () const		{ m_iRefCount++; }
	void			Release () const	{ --m_iRefCount; assert ( m_iRefCount>=0 ); if ( m_iRefCount==0 ) delete this; }

protected:
	mutable int		m_iRefCount;
};


/// automatic pointer wrapper for refcounted objects
/// construction from or assignment of a raw pointer takes over (!) the ownership
template < typename T >
class CSphRefcountedPtr
{
public:
	explicit		CSphRefcountedPtr () = default;		///< default NULL wrapper construction (for vectors)
	explicit		CSphRefcountedPtr ( T * pPtr ) : m_pPtr ( pPtr ) {}	///< construction from raw pointer, takes over ownership!

	CSphRefcountedPtr ( const CSphRefcountedPtr& rhs )
	{
		if ( rhs.m_pPtr )
			rhs.m_pPtr->AddRef ();
		m_pPtr = rhs.m_pPtr;
	}

	CSphRefcountedPtr ( CSphRefcountedPtr&& rhs ) noexcept
		: m_pPtr ( rhs.m_pPtr )
	{
		rhs.m_pPtr = nullptr;
	}

	~CSphRefcountedPtr ()				{ SafeRelease ( m_pPtr ); }

	T *	operator -> () const			{ return m_pPtr; }
		explicit operator bool() const	{ return m_pPtr!=nullptr; }
		operator T * () const			{ return m_pPtr; }

	// drop the ownership and reset pointer
	inline T * Leak ()
	{
		T * pRes = m_pPtr;
		m_pPtr = nullptr;
		return pRes;
	}

	T * Ptr() const { return m_pPtr; }

public:
	/// assignment of a raw pointer, takes over ownership!
	CSphRefcountedPtr<T> & operator = ( T * pPtr )
	{
		SafeRelease ( m_pPtr );
		m_pPtr = pPtr;
		return *this;
	}

	/// wrapper assignment, does automated reference tracking
	CSphRefcountedPtr & operator = ( const CSphRefcountedPtr & rhs )
	{
		SafeAddRef ( rhs.m_pPtr );
		SafeRelease ( m_pPtr );
		m_pPtr = rhs.m_pPtr;
		return *this;
	}

	CSphRefcountedPtr & operator= ( CSphRefcountedPtr && rhs ) noexcept
	{
		if (this==&rhs)
			return *this;
		SafeRelease( m_pPtr );
		m_pPtr = rhs.m_pPtr;
		rhs.m_pPtr = nullptr;
		return *this;
	}

protected:
	T *				m_pPtr = nullptr;
};

//////////////////////////////////////////////////////////////////////////

void sphWarn ( const char *, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );
void SafeClose ( int & iFD );

/// open file for reading
int				sphOpenFile ( const char * sFile, CSphString & sError, bool bWrite );

/// return size of file descriptor
int64_t			sphGetFileSize ( int iFD, CSphString * sError = nullptr );
int64_t			sphGetFileSize ( const CSphString & sFile, CSphString * sError = nullptr );

/// buffer trait that neither own buffer nor clean-up it on destroy
template < typename T >
class CSphBufferTrait : public ISphNoncopyable, public VecTraits_T<T>
{
protected:
	using VecTraits_T<T>::m_pData;
	using VecTraits_T<T>::m_iCount;
public:
	using VecTraits_T<T>::GetLengthBytes;
	/// ctor
	CSphBufferTrait () = default;

	/// dtor
	virtual ~CSphBufferTrait ()
	{
		assert ( !m_bMemLocked && !m_pData );
	}

	virtual void Reset () = 0;


	/// get write address
	T * GetWritePtr () const
	{
		return m_pData;
	}

	void Set ( T * pData, int64_t iCount )
	{
		m_pData = pData;
		m_iCount = iCount;
	}

	bool MemLock ( CSphString & sWarning )
	{
#if USE_WINDOWS
		m_bMemLocked = ( VirtualLock ( m_pData, GetLengthBytes() )!=0 );
		if ( !m_bMemLocked )
			sWarning.SetSprintf ( "mlock() failed: errno %d", GetLastError() );

#else
		m_bMemLocked = ( mlock ( m_pData, GetLengthBytes() )==0 );
		if ( !m_bMemLocked )
			sWarning.SetSprintf ( "mlock() failed: %s", strerrorm(errno) );
#endif

		return m_bMemLocked;
	}

protected:

	bool		m_bMemLocked = false;

	void MemUnlock ()
	{
		if ( !m_bMemLocked )
			return;

		m_bMemLocked = false;
#if USE_WINDOWS
		bool bOk = ( VirtualUnlock ( m_pData, GetLengthBytes() )!=0 );
		if ( !bOk )
			sphWarn ( "munlock() failed: errno %d", GetLastError() );

#else
		bool bOk = ( munlock ( m_pData, GetLengthBytes() )==0 );
		if ( !bOk )
			sphWarn ( "munlock() failed: %s", strerrorm(errno) );
#endif
	}
};


//////////////////////////////////////////////////////////////////////////

#if !USE_WINDOWS
#ifndef MADV_DONTFORK
#define MADV_DONTFORK MADV_NORMAL
#endif
#endif

/// in-memory buffer shared between processes
template < typename T, bool SHARED=false >
class CSphLargeBuffer : public CSphBufferTrait < T >
{
public:
	/// ctor
	CSphLargeBuffer () {}

	/// dtor
	virtual ~CSphLargeBuffer ()
	{
		this->Reset();
	}

public:
	/// allocate storage
#if USE_WINDOWS
	bool Alloc ( int64_t iEntries, CSphString & sError )
#else
	bool Alloc ( int64_t iEntries, CSphString & sError )
#endif
	{
		assert ( !this->GetWritePtr() );

		int64_t uCheck = sizeof(T);
		uCheck *= iEntries;

		int64_t iLength = (size_t)uCheck;
		if ( uCheck!=iLength )
		{
			sError.SetSprintf ( "impossible to mmap() over 4 GB on 32-bit system" );
			return false;
		}

#if USE_WINDOWS
		T * pData = new T [ (size_t)iEntries ];
#else
		int iFlags = MAP_ANON | MAP_PRIVATE;
		if ( SHARED )
			iFlags = MAP_ANON | MAP_SHARED;

		T * pData = (T *) mmap ( NULL, iLength, PROT_READ | PROT_WRITE, iFlags, -1, 0 );
		if ( pData==MAP_FAILED )
		{
			if ( iLength>(int64_t)0x7fffffffUL )
				sError.SetSprintf ( "mmap() failed: %s (length=" INT64_FMT " is over 2GB, impossible on some 32-bit systems)",
					strerrorm(errno), iLength );
			else
				sError.SetSprintf ( "mmap() failed: %s (length=" INT64_FMT ")", strerrorm(errno), iLength );
			return false;
		}

		if ( !SHARED )
			madvise ( pData, iLength, MADV_DONTFORK );
#ifdef MADV_DONTDUMP
		madvise ( pData, iLength, MADV_DONTDUMP );
#endif

#if SPH_ALLOCS_PROFILER
		sphMemStatMMapAdd ( iLength );
#endif

#endif // USE_WINDOWS

		assert ( pData );
		this->Set ( pData, iEntries );
		return true;
	}


	/// deallocate storage
	virtual void Reset ()
	{
		this->MemUnlock();

		if ( !this->GetWritePtr() )
			return;

#if USE_WINDOWS
		delete [] this->GetWritePtr();
#else
		int iRes = munmap ( this->GetWritePtr(), this->GetLengthBytes() );
		if ( iRes )
			sphWarn ( "munmap() failed: %s", strerrorm(errno) );

#if SPH_ALLOCS_PROFILER
		sphMemStatMMapDel ( this->GetLengthBytes() );
#endif

#endif // USE_WINDOWS

		this->Set ( NULL, 0 );
	}
};


//////////////////////////////////////////////////////////////////////////

template < typename T >
class CSphMappedBuffer : public CSphBufferTrait < T >
{
public:
	/// ctor
	CSphMappedBuffer ()
	{
#if USE_WINDOWS
		m_iFD = INVALID_HANDLE_VALUE;
		m_iMap = INVALID_HANDLE_VALUE;
#else
		m_iFD = -1;
#endif
	}

	/// dtor
	virtual ~CSphMappedBuffer ()
	{
		this->Reset();
	}

	bool Setup ( const CSphString & sFileStr, CSphString & sError, bool bWrite = false )
	{
		const char * sFile = sFileStr.cstr();
#if USE_WINDOWS
		assert ( m_iFD==INVALID_HANDLE_VALUE );
#else
		assert ( m_iFD==-1 );
#endif
		assert ( !this->GetWritePtr() && !this->GetLength64() );

		T * pData = NULL;
		int64_t iCount = 0;

#if USE_WINDOWS
		int iAccessMode = GENERIC_READ;
		if ( bWrite )
			iAccessMode |= GENERIC_WRITE;

		DWORD uShare = FILE_SHARE_READ | FILE_SHARE_DELETE;
		if ( bWrite )
			uShare |= FILE_SHARE_WRITE; // wo this flag indexer and indextool unable to open attribute file that was opened by daemon

		HANDLE iFD = CreateFile ( sFile, iAccessMode, uShare, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
		if ( iFD==INVALID_HANDLE_VALUE )
		{
			sError.SetSprintf ( "failed to open file '%s' (errno %d)", sFile, ::GetLastError() );
			return false;
		}
		m_iFD = iFD;

		LARGE_INTEGER tLen;
		if ( GetFileSizeEx ( iFD, &tLen )==0 )
		{
			sError.SetSprintf ( "failed to fstat file '%s' (errno %d)", sFile, ::GetLastError() );
			Reset();
			return false;
		}

		// FIXME!!! report abount tail, ie m_iLen*sizeof(T)!=tLen.QuadPart
		iCount = tLen.QuadPart / sizeof(T);

		// mmap fails to map zero-size file
		if ( tLen.QuadPart>0 )
		{
			int iProtectMode = PAGE_READONLY;
			if ( bWrite )
				iProtectMode = PAGE_READWRITE;
			m_iMap = ::CreateFileMapping ( iFD, NULL, iProtectMode, 0, 0, NULL );
			int iMapAccessMode = FILE_MAP_READ;
			if ( bWrite )
				iMapAccessMode |= FILE_MAP_WRITE;
			pData = (T *)::MapViewOfFile ( m_iMap, iMapAccessMode, 0, 0, 0 );
			if ( !pData )
			{
				sError.SetSprintf ( "failed to map file '%s': (errno %d, length=" INT64_FMT ")", sFile, ::GetLastError(), (int64_t)tLen.QuadPart );
				Reset();
				return false;
			}
		}
#else

		int iFD = sphOpenFile ( sFile, sError, bWrite );
		if ( iFD<0 )
			return false;
		m_iFD = iFD;

		int64_t iFileSize = sphGetFileSize ( iFD, &sError );
		if ( iFileSize<0 )
			return false;

		// FIXME!!! report abount tail, ie m_iLen*sizeof(T)!=st.st_size
		iCount = iFileSize / sizeof(T);

		// mmap fails to map zero-size file
		if ( iFileSize>0 )
		{
			int iProt = PROT_READ;
			int iFlags = MAP_PRIVATE;

			if ( bWrite )
				iProt |= PROT_WRITE;

			pData = (T *)mmap ( NULL, iFileSize, iProt, iFlags, iFD, 0 );
			if ( pData==MAP_FAILED )
			{
				sError.SetSprintf ( "failed to mmap file '%s': %s (length=" INT64_FMT ")", sFile, strerrorm(errno), iFileSize );
				Reset();
				return false;
			}

			madvise ( pData, iFileSize, MADV_DONTFORK );
#ifdef MADV_DONTDUMP
			madvise ( pData, iFileSize, MADV_DONTDUMP );
#endif
		}
#endif

		this->Set ( pData, iCount );
		return true;
	}

	virtual void Reset ()
	{
		this->MemUnlock();

#if USE_WINDOWS
		if ( this->GetWritePtr() )
			::UnmapViewOfFile ( this->GetWritePtr() );

		if ( m_iMap!=INVALID_HANDLE_VALUE )
			::CloseHandle ( m_iMap );
		m_iMap = INVALID_HANDLE_VALUE;

		if ( m_iFD!=INVALID_HANDLE_VALUE )
			::CloseHandle ( m_iFD );
		m_iFD = INVALID_HANDLE_VALUE;
#else
		if ( this->GetWritePtr() )
			::munmap ( this->GetWritePtr(), this->GetLengthBytes() );

		SafeClose ( m_iFD );
#endif

		this->Set ( NULL, 0 );
	}

private:
#if USE_WINDOWS
	HANDLE		m_iFD;
	HANDLE		m_iMap;
#else
	int			m_iFD;
#endif
};


//////////////////////////////////////////////////////////////////////////

extern int g_iThreadStackSize;

/// my thread handle and thread func magic
#if USE_WINDOWS
typedef HANDLE SphThread_t;
typedef DWORD SphThreadKey_t;
#else
typedef pthread_t SphThread_t;
typedef pthread_key_t SphThreadKey_t;
#endif

/// my threading initialize routine
void * sphThreadInit ( bool bDetached=false );

/// my threading deinitialize routine
void sphThreadDone ( int iFD );

/// my create thread wrapper
bool sphThreadCreate ( SphThread_t * pThread, void (*fnThread)(void*), void * pArg, bool bDetached=false, const char * sName=nullptr );

/// get name of a thread
CSphString GetThreadName ( SphThread_t * pThread );

/// my join thread wrapper
bool sphThreadJoin ( SphThread_t * pThread );

/// add (cleanup) callback to run on thread exit
void sphThreadOnExit ( void (*fnCleanup)(void*), void * pArg );

/// alloc thread-local key
bool sphThreadKeyCreate ( SphThreadKey_t * pKey );

/// free thread-local key
void sphThreadKeyDelete ( SphThreadKey_t tKey );

/// get thread-local key value
void * sphThreadGet ( SphThreadKey_t tKey );

/// get the pointer to my thread's stack
void * sphMyStack ();

/// get size of used stack
int64_t sphGetStackUsed();

/// set the size of my thread's stack
void sphSetMyStackSize ( int iStackSize );

/// store the address in the TLS
void MemorizeStack ( void* PStack );

/// set thread-local key value
bool sphThreadSet ( SphThreadKey_t tKey, void * pValue );

#if !USE_WINDOWS
/// what kind of threading lib do we have? The number of frames in the stack depends from it
bool sphIsLtLib();
#endif

//////////////////////////////////////////////////////////////////////////

#if defined(__clang__)
#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__( x ) // no-op
#endif

#define CAPABILITY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

#define SCOPED_CAPABILITY \
    THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)

#define GUARDED_BY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

#define PT_GUARDED_BY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))

#define ACQUIRED_BEFORE( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

#define ACQUIRED_AFTER( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))

#define REQUIRES( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

#define REQUIRES_SHARED( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

#define ACQUIRE( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

#define ACQUIRE_SHARED( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

#define RELEASE( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define RELEASE_SHARED( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define TRY_ACQUIRE( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

#define TRY_ACQUIRE_SHARED( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

#define EXCLUDES( ... ) \
    THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))

#define ASSERT_CAPABILITY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

#define ASSERT_SHARED_CAPABILITY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

#define RETURN_CAPABILITY( x ) \
    THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

#define NO_THREAD_SAFETY_ANALYSIS \
    THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

// Replaced by TRY_ACQUIRE
#define EXCLUSIVE_TRYLOCK_FUNCTION( ... ) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))

// Replaced by TRY_ACQUIRE_SHARED
#define SHARED_TRYLOCK_FUNCTION( ... ) \
  THREAD_ANNOTATION_ATTRIBUTE__(shared_trylock_function(__VA_ARGS__))

// Replaced by RELEASE and RELEASE_SHARED
#define UNLOCK_FUNCTION( ... ) \
	THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))

/// capability for tracing threads
typedef int CAPABILITY ( "role" ) ThreadRole;

inline void AcquireRole ( ThreadRole R ) ACQUIRE(R) NO_THREAD_SAFETY_ANALYSIS
{}

inline void ReleaseRole ( ThreadRole R ) RELEASE( R ) NO_THREAD_SAFETY_ANALYSIS
{}

class SCOPED_CAPABILITY ScopedRole_c
{
	ThreadRole &m_tRoleRef;
public:
	/// acquire on creation
	inline explicit ScopedRole_c ( ThreadRole &tRole ) ACQUIRE( tRole )
		: m_tRoleRef ( tRole )
	{
		AcquireRole ( tRole );
	}

	/// release on going out of scope
	~ScopedRole_c () RELEASE()
	{
		ReleaseRole ( m_tRoleRef );
	}
};

/// mutex implementation
class CAPABILITY ( "mutex" ) CSphMutex : public ISphNoncopyable
{
public:
	CSphMutex ();
	~CSphMutex ();

	bool Lock () ACQUIRE();
	bool Unlock () RELEASE();
	bool TimedLock ( int iMsec ) TRY_ACQUIRE (true);

	// Just for clang negative capabilities.
	const CSphMutex &operator! () const { return *this; }

protected:
#if USE_WINDOWS
	HANDLE m_hMutex;
#else
	pthread_mutex_t m_tMutex;
#endif
};

// event implementation
class CSphAutoEvent : public ISphNoncopyable
{
public:
	CSphAutoEvent ()
	{
		Init();
	}
	~CSphAutoEvent()
	{
		Done();
	}

	// increase event's count and issue an event.
	void SetEvent();

	// decrease event's count. If count empty, go to sleep until new events
	void WaitEvent();

	inline bool Initialized() const
	{
		return m_bInitialized;
	}

private:
	bool Init ();
	bool Done ();
	bool m_bInitialized = false;
	volatile int  m_iSent = 0;

#if USE_WINDOWS
	HANDLE m_hEvent = 0;
#else
	pthread_cond_t m_tCond;
	pthread_mutex_t m_tMutex;
#endif
};

/// scoped mutex lock
template < typename T >
class SCOPED_CAPABILITY CSphScopedLock : public ISphNoncopyable
{
public:
	/// lock on creation
	explicit CSphScopedLock ( T & tMutex ) ACQUIRE( tMutex )
		: m_tMutexRef ( tMutex )
	{
		m_tMutexRef.Lock();
	}

	/// unlock on going out of scope
	~CSphScopedLock () RELEASE()
	{
		m_tMutexRef.Unlock ();
	}

protected:
	T &	m_tMutexRef;
};

using ScopedMutex_t = CSphScopedLock<CSphMutex>;


/// rwlock implementation
class CAPABILITY ( "mutex" ) CSphRwlock : public ISphNoncopyable
{
public:
	CSphRwlock ();
	~CSphRwlock () {
#if !USE_WINDOWS
		SafeDelete ( m_pLock );
		SafeDelete ( m_pWritePreferHelper );
#endif
	}

	bool Init ( bool bPreferWriter=false );
	bool Done ();

	bool ReadLock () ACQUIRE_SHARED();
	bool WriteLock () ACQUIRE();
	bool Unlock () UNLOCK_FUNCTION();

	// Just for clang negative capabilities.
	const CSphRwlock &operator! () const { return *this; }

private:
	bool				m_bInitialized = false;
#if USE_WINDOWS
	HANDLE				m_hWriteMutex = 0;
	HANDLE				m_hReadEvent = 0;
	LONG				m_iReaders = 0;
#else
	pthread_rwlock_t	* m_pLock;
	CSphMutex			* m_pWritePreferHelper = nullptr;
#endif
};

// rwlock with auto init/done
class RwLock_t : public CSphRwlock
{
public:
	RwLock_t()
	{
		Verify ( Init());
	}
	~RwLock_t()
	{
		Verify ( Done());
	}

	explicit RwLock_t ( bool bPreferWriter )
	{
		Verify ( Init ( bPreferWriter ) );
	}
};


/// scoped shared (read) lock
class SCOPED_CAPABILITY CSphScopedRLock : ISphNoncopyable
{
public:
	/// lock on creation
	CSphScopedRLock ( CSphRwlock & tLock ) ACQUIRE_SHARED( tLock )
		: m_tLock ( tLock )
	{
		m_tLock.ReadLock();
	}

	/// unlock on going out of scope
	~CSphScopedRLock () RELEASE ()
	{
		m_tLock.Unlock();
	}

protected:
	CSphRwlock & m_tLock;
};

/// scoped exclusive (write) lock
class SCOPED_CAPABILITY CSphScopedWLock : ISphNoncopyable
{
public:
	/// lock on creation
	CSphScopedWLock ( CSphRwlock & tLock ) ACQUIRE ( tLock ) EXCLUDES ( tLock )
		: m_tLock ( tLock )
	{
		m_tLock.WriteLock();
	}

	/// unlock on going out of scope
	~CSphScopedWLock () RELEASE ()
	{
		m_tLock.Unlock();
	}

protected:
	CSphRwlock & m_tLock;
};

/// scoped lock owner - unlock in dtr
template <class LOCKED=CSphRwlock>
class SCOPED_CAPABILITY ScopedUnlock_T : ISphNoncopyable
{
public:
	/// lock on creation
	ScopedUnlock_T ( LOCKED &tLock ) ACQUIRE ( tLock )
		: m_pLock ( &tLock )
	{}

	ScopedUnlock_T ( ScopedUnlock_T && tLock ) noexcept
		: m_pLock ( tLock.m_pLock )
	{
		tLock.m_pLock = nullptr;
	}

	ScopedUnlock_T &operator= ( ScopedUnlock_T &&rhs ) noexcept
		RELEASE()
	{
		if ( this==&rhs )
			return *this;
		if ( m_pLock )
			m_pLock->Unlock();
		m_pLock = rhs.m_pLock;
		rhs.m_pLock = nullptr;
		return *this;
	}

	/// unlock on going out of scope
	~ScopedUnlock_T () RELEASE ()
	{
		if ( m_pLock )
			m_pLock->Unlock ();
	}

protected:
	LOCKED * m_pLock;
};

// shortcuts (original names sometimes looks too long)
using ScRL_t = CSphScopedRLock;
using ScWL_t = CSphScopedWLock;

// perform any (function-defined) action on exit from a scope.
template < typename ACTION >
class AtScopeExit_T
{
	ACTION m_dAction;
public:
	AtScopeExit_T ( ACTION &&tAction )
		: m_dAction { std::forward<ACTION> ( tAction ) }
	{}

	AtScopeExit_T ( AtScopeExit_T &&rhs ) noexcept
		: m_dAction { std::move ( rhs.m_dAction ) }
	{}

	~AtScopeExit_T ()
	{
		m_dAction ();
	}
};

// create action to be performed on-exit-from-scope.
// usage example:
// someObject * pObj; // need to be freed going out of scope
// auto dObjDeleter = AtScopeExit ( [&pObj] { SafeDelete (pObj); } )
// ...
template < typename ACTION >
AtScopeExit_T<ACTION> AtScopeExit ( ACTION &&action )
{
	return AtScopeExit_T<ACTION>{ std::forward<ACTION> ( action ) };
}


//////////////////////////////////////////////////////////////////////////

/// generic dynamic bitvector
/// with a preallocated part for small-size cases, and a dynamic route for big-size ones
class CSphBitvec
{
protected:
	DWORD *		m_pData = nullptr;
	DWORD		m_uStatic[4] {0};
	int			m_iElements = 0;

public:
	CSphBitvec () = default;

	explicit CSphBitvec ( int iElements )
	{
		Init ( iElements );
	}

	~CSphBitvec ()
	{
		if ( m_pData!=m_uStatic )
			SafeDeleteArray ( m_pData );
	}

	/// copy ctor
	CSphBitvec ( const CSphBitvec & rhs )
	{
		m_pData = nullptr;
		m_iElements = 0;
		*this = rhs;
	}

	/// copy
	CSphBitvec & operator = ( const CSphBitvec & rhs )
	{
		if ( m_pData!=m_uStatic )
			SafeDeleteArray ( m_pData );

		Init ( rhs.m_iElements );
		memcpy ( m_pData, rhs.m_pData, sizeof(m_uStatic[0]) * GetSize() );

		return *this;
	}

	void Init ( int iElements )
	{
		assert ( iElements>=0 );
		m_iElements = iElements;
		if ( iElements > int(sizeof(m_uStatic)*8) )
		{
			int iSize = GetSize();
			m_pData = new DWORD [ iSize ];
		} else
		{
			m_pData = m_uStatic;
		}
		Clear();
	}

	void Clear ()
	{
		int iSize = GetSize();
		memset ( m_pData, 0, sizeof(DWORD)*iSize );
	}

	void Set ()
	{
		int iSize = GetSize();
		memset ( m_pData, 0xff, sizeof(DWORD)*iSize );
	}


	bool BitGet ( int iIndex ) const
	{
		assert ( m_pData );
		assert ( iIndex>=0 );
		assert ( iIndex<m_iElements );
		return ( m_pData [ iIndex>>5 ] & ( 1UL<<( iIndex&31 ) ) )!=0; // NOLINT
	}

	void BitSet ( int iIndex )
	{
		assert ( iIndex>=0 );
		assert ( iIndex<m_iElements );
		m_pData [ iIndex>>5 ] |= ( 1UL<<( iIndex&31 ) ); // NOLINT
	}

	void BitClear ( int iIndex )
	{
		assert ( iIndex>=0 );
		assert ( iIndex<m_iElements );
		m_pData [ iIndex>>5 ] &= ~( 1UL<<( iIndex&31 ) ); // NOLINT
	}

	const DWORD * Begin () const
	{
		return m_pData;
	}

	DWORD * Begin ()
	{
		return m_pData;
	}

	int GetSize() const
	{
		return (m_iElements+31)/32;
	}

	bool IsEmpty() const
	{
		if (!m_pData)
			return true;

		return GetSize ()==0;
	}

	int GetBits() const
	{
		return m_iElements;
	}

	int BitCount () const
	{
		int iBitSet = 0;
		for ( int i=0; i<GetSize(); i++ )
			iBitSet += sphBitCount ( m_pData[i] );

		return iBitSet;
	}
};

//////////////////////////////////////////////////////////////////////////

#if USE_WINDOWS
#define DISABLE_CONST_COND_CHECK \
	__pragma ( warning ( push ) ) \
	__pragma ( warning ( disable:4127 ) )
#define ENABLE_CONST_COND_CHECK \
	__pragma ( warning ( pop ) )
#else
#define DISABLE_CONST_COND_CHECK
#define ENABLE_CONST_COND_CHECK
#endif

#define if_const(_arg) \
	DISABLE_CONST_COND_CHECK \
	if ( _arg ) \
	ENABLE_CONST_COND_CHECK

//////////////////////////////////////////////////////////////////////////
// interlocked (atomic) operation

#if (USE_WINDOWS) || (HAVE_SYNC_FETCH)
#define NO_ATOMIC 0
#else
#define NO_ATOMIC 1
#endif

template<typename TLONG, int SIZE=sizeof(TLONG)>
class CSphAtomic_T
{
	alignas ( SIZE ) volatile TLONG m_iValue;

public:
	explicit CSphAtomic_T ( TLONG iValue = 0 )
		: m_iValue ( iValue )
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
	}

	~CSphAtomic_T ()
	{}

	inline operator TLONG() const
	{
		return GetValue ();
	}

	inline CSphAtomic_T& operator= ( TLONG iArg )
	{
		SetValue ( iArg );
		return *this;
	}

	inline TLONG operator++ ( int ) // postfix
	{
		return Inc ();
	}

	inline TLONG operator++ ( )
	{
		TLONG iPrev = Inc ();
		return ++iPrev;
	}

	inline CSphAtomic_T& operator+= ( TLONG iArg )
	{
		Add ( iArg );
		return *this;
	}

	inline TLONG operator-- ( int ) // postfix
	{
		return Dec ();
	}

	inline TLONG operator-- ()
	{
		TLONG iPrev = Dec ();
		return --iPrev;
	}

	inline CSphAtomic_T& operator-= ( TLONG iArg )
	{
		Sub ( iArg );
		return *this;
	}

	TLONG GetValue () const
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return m_iValue;
	}

#ifdef HAVE_SYNC_FETCH

	// return value here is original value, prior to operation took place
	inline TLONG Inc ()
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return __sync_fetch_and_add ( &m_iValue, 1 );
	}

	inline TLONG Dec ()
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return __sync_fetch_and_sub ( &m_iValue, 1 );
	}

	inline TLONG Add ( TLONG iValue )
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return __sync_fetch_and_add ( &m_iValue, iValue );
	}

	inline TLONG Sub ( TLONG iValue )
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return __sync_fetch_and_sub ( &m_iValue, iValue );
	}

	void SetValue ( TLONG iValue )
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		while (true)
		{
			TLONG iOld = GetValue ();
			if ( __sync_bool_compare_and_swap ( &m_iValue, iOld, iValue ) )
				break;
		}
	}

	//! Atomic Compare-And-Swap
	//! \param iOldVal - expected value to compare with
	//! \param iNewVal - new value to write
	//! \return old value was in atomic
	inline TLONG CAS ( TLONG iOldVal, TLONG iNewVal )
	{
		assert ( ( ( (size_t) &m_iValue )%( sizeof ( m_iValue ) ) )==0 && "unaligned atomic!" );
		return __sync_val_compare_and_swap ( &m_iValue, iOldVal, iNewVal );
	}

#elif USE_WINDOWS
	TLONG Inc ();
	TLONG Dec ();
	TLONG Add ( TLONG );
	TLONG Sub ( TLONG );
	void SetValue ( TLONG iValue );
	TLONG CAS ( TLONG, TLONG );
#endif

#if NO_ATOMIC
#error "Can't compile on system without atomic operations."
#endif
};

typedef CSphAtomic_T<long> CSphAtomic;
typedef CSphAtomic_T<int64_t> CSphAtomicL;

/// MT-aware refcounted base (uses atomics that sometimes m.b. slow because of inter-cpu sync)
struct ISphRefcountedMT : public ISphNoncopyable
{
protected:
	virtual ~ISphRefcountedMT ()
	{}

public:
	inline void AddRef () const
	{
		m_iRefCount.Inc();
	}

	inline void Release () const
	{
		long uRefs = m_iRefCount.Dec();
		assert ( uRefs>=1 );
		if ( uRefs==1 )
			delete this;
	}

	inline long GetRefcount() const
	{
		return m_iRefCount;
	}

	inline bool IsLast() const
	{
		return 1==m_iRefCount;
	}

protected:
	mutable CSphAtomic m_iRefCount { 1 };
};

template <class T>
struct VecRefPtrs_t : public ISphNoncopyable, public CSphVector<T>
{
	~VecRefPtrs_t ()
	{
		CSphVector<T>::Apply ( [] ( T &ptr ) { SafeRelease ( ptr ); } );
	}
};

struct ISphJob
{
	virtual ~ISphJob () {};
	virtual void Call () = 0;
};

struct ISphThdPool
{
	virtual ~ISphThdPool () {};
	virtual void Shutdown () = 0;
	virtual void AddJob ( ISphJob * pItem ) = 0;
	virtual bool StartJob ( ISphJob * pItem ) = 0;

	virtual int GetActiveWorkerCount () const = 0;
	virtual int GetTotalWorkerCount () const = 0;
	virtual int GetQueueLength () const = 0;
};

ISphThdPool * sphThreadPoolCreate ( int iThreads, const char * sName, CSphString & sError );

int sphCpuThreadsCount ();

//////////////////////////////////////////////////////////////////////////

/// simple open-addressing hash
/// for now, with int64_t keys (for docids), maybe i will templatize this later
template < typename VALUE >
class CSphHash
{
protected:
	typedef int64_t		KEY;
	static const KEY	NO_ENTRY = LLONG_MAX;		///< final entry in a chain, we can now safely stop searching
	static const KEY	DEAD_ENTRY = LLONG_MAX-1;	///< dead entry in a chain, more alive entries may follow

	struct Entry
	{
		KEY		m_Key;
		VALUE	m_Value;

		Entry() : m_Key ( NO_ENTRY ) {}
	};

	Entry *		m_pHash;	///< hash entries
	int			m_iSize;	///< total hash size
	int			m_iUsed;	///< how many entries are actually used
	int			m_iMaxUsed;	///< resize threshold

public:
	/// initialize hash of a given initial size
	explicit CSphHash ( int iSize=256 )
	{
		m_pHash = NULL;
		Reset ( iSize );
	}

	/// reset to a given size
	void Reset ( int iSize )
	{
		SafeDeleteArray ( m_pHash );
		if ( iSize<=0 )
		{
			m_iSize = m_iUsed = m_iMaxUsed = 0;
			return;
		}
		iSize = ( 1<<sphLog2 ( iSize-1 ) );
		m_pHash = new Entry[iSize];
		m_iSize = iSize;
		m_iUsed = 0;
		m_iMaxUsed = GetMaxLoad ( iSize );
	}

	void Clear()
	{
		for ( int i=0; i<m_iSize; i++ )
			m_pHash[i] = Entry();

		m_iUsed = 0;
	}

	~CSphHash()
	{
		SafeDeleteArray ( m_pHash );
	}

	/// hash me the key, quick!
	static inline DWORD GetHash ( KEY k )
	{
		return ( DWORD(k) * 0x607cbb77UL ) ^ ( k>>32 );
	}

	/// acquire value by key (ie. get existing hashed value, or add a new default value)
	VALUE & Acquire ( KEY k )
	{
		assert ( k!=NO_ENTRY && k!=DEAD_ENTRY );
		DWORD uHash = GetHash(k);
		int iIndex = uHash & ( m_iSize-1 );
		int iDead = -1;
		while (true)
		{
			// found matching key? great, return the value
			Entry * p = m_pHash + iIndex;
			if ( p->m_Key==k )
				return p->m_Value;

			// no matching keys? add it
			if ( p->m_Key==NO_ENTRY )
			{
				// not enough space? grow the hash and force rescan
				if ( m_iUsed>=m_iMaxUsed )
				{
					Grow();
					iIndex = uHash & ( m_iSize-1 );
					iDead = -1;
					continue;
				}

				// did we walk past a dead entry while probing? if so, lets reuse it
				if ( iDead>=0 )
					p = m_pHash + iDead;

				// store the newly added key
				p->m_Key = k;
				m_iUsed++;
				return p->m_Value;
			}

			// is this a dead entry? store its index for (possible) reuse
			if ( p->m_Key==DEAD_ENTRY )
				iDead = iIndex;

			// no match so far, keep probing
			iIndex = ( iIndex+1 ) & ( m_iSize-1 );
		}
	}

	/// find an existing value by key
	VALUE * Find ( KEY k ) const
	{
		Entry * e = FindEntry(k);
		return e ? &e->m_Value : NULL;
	}

	/// add or fail (if key already exists)
	bool Add ( KEY k, const VALUE & v )
	{
		int u = m_iUsed;
		VALUE & x = Acquire(k);
		if ( u==m_iUsed )
			return false; // found an existing value by k, can not add v
		x = v;
		return true;
	}

	/// find existing value, or add a new value
	VALUE & FindOrAdd ( KEY k, const VALUE & v )
	{
		int u = m_iUsed;
		VALUE & x = Acquire(k);
		if ( u!=m_iUsed )
			x = v; // did not find an existing value by k, so add v
		return x;
	}

	/// delete by key
	bool Delete ( KEY k )
	{
		Entry * e = FindEntry(k);
		if ( e )
			e->m_Key = DEAD_ENTRY;
		return e!=NULL;
	}

	/// get number of inserted key-value pairs
	int GetLength() const
	{
		return m_iUsed;
	}

	/// iterate the hash by entry index, starting from 0
	/// finds the next alive key-value pair starting from the given index
	/// returns that pair and updates the index on success
	/// returns NULL when the hash is over
	VALUE * Iterate ( int * pIndex, KEY * pKey ) const
	{
		if ( !pIndex || *pIndex<0 )
			return NULL;
		for ( int i = *pIndex; i < m_iSize; i++ )
		{
			if ( m_pHash[i].m_Key!=NO_ENTRY && m_pHash[i].m_Key!=DEAD_ENTRY )
			{
				*pIndex = i+1;
				if ( pKey )
					*pKey = m_pHash[i].m_Key;
				return &m_pHash[i].m_Value;
			}
		}
		return NULL;
	}

protected:
	/// get max load, ie. max number of actually used entries at given size
	int GetMaxLoad ( int iSize ) const
	{
		return (int)( iSize*0.95f );
	}

	/// we are overloaded, lets grow 2x and rehash
	void Grow()
	{
		Entry * pNew = new Entry [ 2*Max(m_iSize,8) ];

		for ( int i=0; i<m_iSize; i++ )
			if ( m_pHash[i].m_Key!=NO_ENTRY && m_pHash[i].m_Key!=DEAD_ENTRY )
			{
				int j = GetHash ( m_pHash[i].m_Key ) & ( 2*m_iSize-1 );
				while ( pNew[j].m_Key!=NO_ENTRY )
					j = ( j+1 ) & ( 2*m_iSize-1 );
				pNew[j] = m_pHash[i];
			}

			SafeDeleteArray ( m_pHash );
			m_pHash = pNew;
			m_iSize *= 2;
			m_iMaxUsed = GetMaxLoad ( m_iSize );
	}

	/// find (and do not touch!) entry by key
	inline Entry * FindEntry ( KEY k ) const
	{
		assert ( k!=NO_ENTRY && k!=DEAD_ENTRY );
		DWORD uHash = GetHash(k);
		int iIndex = uHash & ( m_iSize-1 );
		while (true)
		{
			Entry * p = m_pHash + iIndex;
			if ( p->m_Key==k )
				return p;
			if ( p->m_Key==NO_ENTRY )
				return NULL;
			iIndex = ( iIndex+1 ) & ( m_iSize-1 );
		}
	}
};


template<> inline CSphHash<int>::Entry::Entry() : m_Key ( NO_ENTRY ), m_Value ( 0 ) {}
template<> inline CSphHash<DWORD>::Entry::Entry() : m_Key ( NO_ENTRY ), m_Value ( 0 ) {}
template<> inline CSphHash<float>::Entry::Entry() : m_Key ( NO_ENTRY ), m_Value ( 0.0f ) {}
template<> inline CSphHash<int64_t>::Entry::Entry() : m_Key ( NO_ENTRY ), m_Value ( 0 ) {}
template<> inline CSphHash<uint64_t>::Entry::Entry() : m_Key ( NO_ENTRY ), m_Value ( 0 ) {}


/////////////////////////////////////////////////////////////////////////////

/// generic stateless priority queue
template < typename T, typename COMP >
class CSphQueue
{
protected:
	T * m_pData = nullptr;
	int m_iUsed = 0;
	int m_iSize;

public:
	/// ctor
	explicit CSphQueue ( int iSize )
		: m_iSize ( iSize )
	{
		Reset ( iSize );
	}

	/// dtor
	~CSphQueue ()
	{
		SafeDeleteArray ( m_pData );
	}

	void Reset ( int iSize )
	{
		SafeDeleteArray ( m_pData );
		assert ( iSize>=0 );
		m_iSize = iSize;
		if ( iSize )
			m_pData = new T[iSize];
		assert ( !iSize || m_pData );
	}

	/// add entry to the queue
	bool Push ( const T &tEntry )
	{
		assert ( m_pData );
		if ( m_iUsed==m_iSize )
		{
			// if it's worse that current min, reject it, else pop off current min
			if ( COMP::IsLess ( tEntry, m_pData[0] ) )
				return false;
			else
				Pop ();
		}

		// do add
		m_pData[m_iUsed] = tEntry;
		int iEntry = m_iUsed++;

		// shift up if needed, so that worst (lesser) ones float to the top
		while ( iEntry )
		{
			int iParent = ( iEntry - 1 ) >> 1;
			if ( !COMP::IsLess ( m_pData[iEntry], m_pData[iParent] ) )
				break;

			// entry is less than parent, should float to the top
			Swap ( m_pData[iEntry], m_pData[iParent] );
			iEntry = iParent;
		}

		return true;
	}

	/// remove root (ie. top priority) entry
	void Pop ()
	{
		assert ( m_iUsed && m_pData );
		if ( !( --m_iUsed ) ) // empty queue? just return
			return;

		// make the last entry my new root
		m_pData[0] = m_pData[m_iUsed];

		// shift down if needed
		int iEntry = 0;
		while ( true )
		{
			// select child
			int iChild = ( iEntry << 1 ) + 1;
			if ( iChild>=m_iUsed )
				break;

			// select smallest child
			if ( iChild + 1<m_iUsed )
				if ( COMP::IsLess ( m_pData[iChild + 1], m_pData[iChild] ) )
					++iChild;

			// if smallest child is less than entry, do float it to the top
			if ( COMP::IsLess ( m_pData[iChild], m_pData[iEntry] ) )
			{
				Swap ( m_pData[iChild], m_pData[iEntry] );
				iEntry = iChild;
				continue;
			}

			break;
		}
	}

	/// get entries count
	inline int GetLength () const
	{
		return m_iUsed;
	}

	/// get current root
	inline const T &Root () const
	{
		assert ( m_iUsed && m_pData );
		return m_pData[0];
	}
};


// simple circular buffer
template < typename T >
class CircularBuffer_T
{
public:
	explicit CircularBuffer_T ( int iInitialSize=256, float fGrowFactor=1.5f )
		: m_dValues ( iInitialSize )
		, m_fGrowFactor ( fGrowFactor )
	{}

	CircularBuffer_T ( CircularBuffer_T&& rhs ) noexcept
		: m_dValues ( std::move ( rhs.m_dValues ) )
		, m_fGrowFactor ( rhs.m_fGrowFactor )
		, m_iHead ( rhs.m_iHead )
		, m_iTail ( rhs.m_iTail )
		, m_iUsed ( rhs.m_iUsed )
	{
		rhs.m_iHead = 0;
		rhs.m_iTail = 0;
		rhs.m_iUsed = 0;
	}

	CircularBuffer_T & operator= ( CircularBuffer_T&& rhs ) noexcept
	{
		if ( &rhs!=this )
		{
			m_dValues = std::move ( rhs.m_dValues );
			m_fGrowFactor = rhs.m_fGrowFactor;
			m_iHead = rhs.m_iHead;
			m_iTail = rhs.m_iTail;
			m_iUsed = rhs.m_iUsed;

			rhs.m_iHead = 0;
			rhs.m_iTail = 0;
			rhs.m_iUsed = 0;
		}
		return *this;
	}


	void Push ( const T & tValue )
	{
		if ( m_iUsed==m_dValues.GetLength() )
			Resize ( int(m_iUsed*m_fGrowFactor) );

		m_dValues[m_iTail] = tValue;
		m_iTail = ( m_iTail+1 ) % m_dValues.GetLength();
		m_iUsed++;
	}

	T & Push()
	{
		if ( m_iUsed==m_dValues.GetLength() )
			Resize ( int ( m_iUsed*m_fGrowFactor ) );

		int iOldTail = m_iTail;
		m_iTail = (m_iTail + 1) % m_dValues.GetLength ();
		m_iUsed++;

		return m_dValues[iOldTail];
	}


	T & Pop()
	{
		assert ( !IsEmpty() );
		int iOldHead = m_iHead;
		m_iHead = ( m_iHead+1 ) % m_dValues.GetLength();
		m_iUsed--;

		return m_dValues[iOldHead];
	}

	const T & Last() const
	{
		assert (!IsEmpty());
		return operator[](GetLength()-1);
	}

	T & Last()
	{
		assert (!IsEmpty());
		int iIndex = GetLength()-1;
		return m_dValues[(iIndex+m_iHead) % m_dValues.GetLength()];
	}

	const T & operator [] ( int iIndex ) const
	{
		assert ( iIndex < m_iUsed );
		return m_dValues[(iIndex+m_iHead) % m_dValues.GetLength()];
	}

	bool IsEmpty() const
	{
		return m_iUsed==0;
	}

	int GetLength() const
	{
		return m_iUsed;
	}

private:
	CSphFixedVector<T>	m_dValues;
	float				m_fGrowFactor;
	int					m_iHead = 0;
	int					m_iTail = 0;
	int					m_iUsed = 0;

	void Resize ( int iNewLength )
	{
		CSphFixedVector<T> dNew ( iNewLength );
		for ( int i = 0; i < GetLength(); i++ )
			dNew[i] = m_dValues[(i+m_iHead) % m_dValues.GetLength()];

		m_dValues.SwapData(dNew);

		m_iHead = 0;
		m_iTail = m_iUsed;
	}
};


//////////////////////////////////////////////////////////////////////////
class TDigest_i
{
public:
	virtual				~TDigest_i() {}

	virtual void		Add ( double fValue, int64_t iWeight = 1 ) = 0;
	virtual double		Percentile ( int iPercent ) const = 0;
};

TDigest_i * sphCreateTDigest();

//////////////////////////////////////////////////////////////////////////
/// simple linked list
//////////////////////////////////////////////////////////////////////////
struct ListNode_t
{
	ListNode_t * m_pPrev = nullptr;
	ListNode_t * m_pNext = nullptr;
};


/// Simple linked list.
class List_t
{
public:
	List_t ()
	{
		m_tStub.m_pPrev = &m_tStub;
		m_tStub.m_pNext = &m_tStub;
		m_iCount = 0;
	}

	/// Append the node to the tail
	void Add ( ListNode_t * pNode )
	{
		assert ( !pNode->m_pNext && !pNode->m_pPrev );
		pNode->m_pNext = m_tStub.m_pNext;
		pNode->m_pPrev = &m_tStub;
		m_tStub.m_pNext->m_pPrev = pNode;
		m_tStub.m_pNext = pNode;

		++m_iCount;
	}

	void Remove ( ListNode_t * pNode )
	{
		assert ( pNode->m_pNext && pNode->m_pPrev );
		pNode->m_pNext->m_pPrev = pNode->m_pPrev;
		pNode->m_pPrev->m_pNext = pNode->m_pNext;
		pNode->m_pNext = nullptr;
		pNode->m_pPrev = nullptr;

		--m_iCount;
	}

	inline int GetLength () const
	{
		return m_iCount;
	}

	inline const ListNode_t * Begin () const
	{
		return m_tStub.m_pNext;
	}

	inline const ListNode_t * End () const
	{
		return &m_tStub;
	}

private:
	ListNode_t m_tStub;	///< stub node
	int m_iCount;
};


template <typename T>
inline int sphCalcZippedLen ( T tValue )
{
	int nBytes = 1;
	tValue>>=7;
	while ( tValue )
	{
		tValue >>= 7;
		nBytes++;
	}

	return nBytes;
}


template <typename T, typename C>
inline int sphZipValue ( T tValue, C * pClass, void (C::*fnPut)(int) )
{
	int nBytes = sphCalcZippedLen ( tValue );
	for ( int i = nBytes-1; i>=0; i-- )
		(pClass->*fnPut) ( ( 0x7f & ( tValue >> (7*i) ) ) | ( i ? 0x80 : 0 ) );

	return nBytes;
}


template <typename T>
inline int sphZipToPtr ( T tValue, BYTE * pData )
{
	int nBytes = sphCalcZippedLen ( tValue );
	for ( int i = nBytes-1; i>=0; i-- )
		*pData++ = ( 0x7f & ( tValue >> (7*i) ) ) | ( i ? 0x80 : 0 );

	return nBytes;
}

/// Allocation for small objects (namely - for movable dynamic attributes).
/// internals based on Alexandresku's 'loki' implementation - 'Allocator for small objects'
static const int MAX_SMALL_OBJECT_SIZE = 64;

#ifdef USE_SMALLALLOC
BYTE * sphAllocateSmall ( int iBytes );
void sphDeallocateSmall ( BYTE * pBlob, int iBytes );
size_t sphGetSmallAllocatedSize ();	// how many allocated right now
size_t sphGetSmallReservedSize ();	// how many pooled from the sys right now
#else
inline BYTE * sphAllocateSmall(int iBytes) {return new BYTE[iBytes];};
inline void sphDeallocateSmall(BYTE * pBlob, int) {delete[]pBlob;};
inline size_t sphGetSmallAllocatedSize() {return 0;};    // how many allocated right now
inline size_t sphGetSmallReservedSize() {return 0;};    // how many pooled from the sys right now
#endif // USE_SMALLALLOC

void sphDeallocatePacked ( BYTE * pBlob );

#endif // _sphinxstd_
