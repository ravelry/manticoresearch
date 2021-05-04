//
// Copyright (c) 2017-2021, Manticore Software LTD (https://manticoresearch.com)
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
#include "searchdssl.h"

#if !USE_SSL

void SetServerSSLKeys ( CSphVariant *,  CSphVariant *,  CSphVariant * ) {}
bool CheckWeCanUseSSL () { return false; }
bool MakeSecureLayer ( AsyncNetBufferPtr_c & ) { return false; }

#else

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

// need by OpenSSL
struct CRYPTO_dynlock_value
{
	CSphMutex m_tLock;
};

static CSphFixedVector<CSphMutex> g_dSslLocks { 0 };

static CSphString g_sSslCert;
static CSphString g_sSslKey;
static CSphString g_sSslCa;

void fnSslLock ( int iMode, int iLock, const char * , int )
{
	if ( iMode & CRYPTO_LOCK )
		g_dSslLocks[iLock].Lock();
	else
		g_dSslLocks[iLock].Unlock();
}

CRYPTO_dynlock_value * fnSslLockDynCreate ( const char * , int )
{
	auto * pLock = new CRYPTO_dynlock_value;
	return pLock;
}

void fnSslLockDyn ( int iMode, CRYPTO_dynlock_value * pLock, const char * , int )
{
	assert ( pLock );
	if ( iMode & CRYPTO_LOCK )
		pLock->m_tLock.Lock();
	else
		pLock->m_tLock.Unlock();
}

void fnSslLockDynDestroy ( CRYPTO_dynlock_value * pLock, const char * , int )
{
	SafeDelete ( pLock );
}

static BIO_METHOD * BIO_s_coroAsync ( bool bDestroy = false );

static int fnSslError ( const char * pStr, size_t iLen, void * )
{
	// trim line ending from string end
	while ( iLen && sphIsSpace ( pStr[iLen-1] ) )
		iLen--;

	sphWarning ( "%.*s", (int)iLen, pStr );
	return 1;
}

#define FBLACK	"\x1b[30m"
#define FRED	"\x1b[31m"
#define FGREEN	"\x1b[32m"
#define FYELLOW	"\x1b[33m"
#define FCYAN	"\x1b[34m"
#define FPURPLE	"\x1b[35m"
#define FBLUE	"\x1b[35m"
#define FWHITE	"\x1b[35m"

#define NORM    "\x1b[0m"

#define FRONT FRED
#define FRONTN FPURPLE
#define BACK FGREEN
#define BACKN FYELLOW
#define SYSN FCYAN

void SetServerSSLKeys ( CSphVariant* pSslCert, CSphVariant* pSslKey, CSphVariant* pSslCa )
{
	if ( pSslCert )
		g_sSslCert = pSslCert->cstr();
	if ( pSslKey )
		g_sSslKey = pSslKey->cstr ();
	if ( pSslCa )
		g_sSslCa = pSslCa->cstr ();
}

static bool IsKeysSet()
{
	return !( g_sSslCert.IsEmpty () && g_sSslKey.IsEmpty () && g_sSslCa.IsEmpty ());
}

// set SSL key, certificate and ca-certificate to global SSL context
static bool SetGlobalKeys ( SSL_CTX * pCtx )
{
	if ( !(IsKeysSet()) )
		return false;

	if ( !g_sSslCert.IsEmpty () && SSL_CTX_use_certificate_file ( pCtx, g_sSslCert.cstr (), SSL_FILETYPE_PEM )<=0 )
	{
		ERR_print_errors_cb ( &fnSslError, nullptr );
		return false;
	}

	if ( !g_sSslKey.IsEmpty () && SSL_CTX_use_PrivateKey_file ( pCtx, g_sSslKey.cstr (), SSL_FILETYPE_PEM )<=0 )
	{
		ERR_print_errors_cb ( &fnSslError, nullptr );
		return false;
	}

	if ( !g_sSslCa.IsEmpty () && SSL_CTX_load_verify_locations ( pCtx, g_sSslCa.cstr(), nullptr )<=0 )
	{
		ERR_print_errors_cb ( &fnSslError, nullptr );
		return false;
	}

	// check key and certificate file match
	if ( SSL_CTX_check_private_key( pCtx )!=1 )
	{
		ERR_print_errors_cb ( &fnSslError, nullptr );
		return false;
	}

	return true;
}

// free SSL related data
static void SslFreeCtx ( SSL_CTX * pCtx )
{
	if ( !pCtx )
		return;

	SSL_CTX_free ( pCtx );
	pCtx = nullptr;

	CRYPTO_set_locking_callback ( nullptr );
	CRYPTO_set_dynlock_create_callback ( nullptr );
	CRYPTO_set_dynlock_lock_callback ( nullptr );
	CRYPTO_set_dynlock_destroy_callback ( nullptr );

	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
	ERR_remove_state ( 0 );
	ERR_free_strings();

	g_dSslLocks.Reset ( 0 );
}

using SmartSSL_CTX_t = SharedPtrCustom_t<SSL_CTX *>;

// init SSL library and global context by demand
static SmartSSL_CTX_t GetSslCtx ()
{
	static SmartSSL_CTX_t pSslCtx;
	if ( !pSslCtx )
	{
		int iLocks = CRYPTO_num_locks();
		g_dSslLocks.Reset ( iLocks );

		CRYPTO_set_locking_callback ( &fnSslLock );
		CRYPTO_set_dynlock_create_callback ( &fnSslLockDynCreate );
		CRYPTO_set_dynlock_lock_callback ( &fnSslLockDyn );
		CRYPTO_set_dynlock_destroy_callback ( &fnSslLockDynDestroy );

		SSL_load_error_strings();
		SSL_library_init();

		const SSL_METHOD * pMode = nullptr;
		#if HAVE_TLS_SERVER_METHOD
		pMode = TLS_server_method ();
		#elif HAVE_TLSV1_2_METHOD
		pMode = TLSv1_2_server_method();
		#elif HAVE_TLSV1_1_SERVER_METHOD
		pMode = TLSv1_1_server_method();
		#else
		pMode = SSLv23_server_method();
		#endif
		pSslCtx = SmartSSL_CTX_t ( SSL_CTX_new ( pMode ), [] ( SSL_CTX * pCtx )
		{
			sphLogDebugv ( BACKN "~~ Releasing ssl context." NORM );
			BIO_s_coroAsync ( true );
			SslFreeCtx ( pCtx );
		});
		SSL_CTX_set_verify ( pSslCtx, SSL_VERIFY_NONE, nullptr );

		// shedule callback for final shutdown.
		searchd::AddShutdownCb ( [pRefCtx = pSslCtx] {
			sphLogDebugv ( BACKN "~~ Shutdowncb called." NORM );
			pSslCtx = nullptr;
			// pRefCtx will be also deleted going out of scope
		} );
	}
	return pSslCtx;
}

static SmartSSL_CTX_t GetReadySslCtx ()
{
	if ( !IsKeysSet ())
		return SmartSSL_CTX_t ( nullptr );

	auto pCtx = GetSslCtx ();
	if ( !pCtx )
		return pCtx;

	static bool bKeysLoaded = false;

	if ( !bKeysLoaded && SetGlobalKeys ( pCtx ))
		bKeysLoaded = true;

	if ( !bKeysLoaded )
		return SmartSSL_CTX_t ( nullptr );

	return pCtx;
}

// is global SSL context created and keys set
bool CheckWeCanUseSSL ()
{
	static bool bCheckPerformed = false; // to check only once
	static bool bWeCanUseSSL;

	if ( bCheckPerformed )
		return bWeCanUseSSL;

	bCheckPerformed = true;
	bWeCanUseSSL = ( GetReadySslCtx ()!=nullptr );
	return bWeCanUseSSL;
}

// translates AsyncNetBuffer_c to openSSL BIO calls.
class BioAsyncNetAdapter_c
{
	AsyncNetBufferPtr_c m_pBackend;
	NetGenericOutputBuffer_c& m_tOut;
	AsyncNetInputBuffer_c& m_tIn;

public:
	explicit BioAsyncNetAdapter_c ( AsyncNetBufferPtr_c pSource )
		: m_pBackend ( std::move (pSource) )
		, m_tOut ( *m_pBackend )
		, m_tIn ( *m_pBackend )
	{}

	int BioRead ( char * pBuf, int iLen )
	{
		sphLogDebugv ( BACK "<< BioBackRead (%p) for %p, %d, in buf %d" NORM, this, pBuf, iLen, m_tIn.HasBytes () );
		if ( !pBuf || iLen<=0 )
			return 0;
		if ( !m_tIn.ReadFrom ( iLen ))
			iLen = -1;
		auto dBlob = m_tIn.PopTail ( iLen );
		if ( IsNull ( dBlob ))
			return 0;

		memcpy ( pBuf, dBlob.first, dBlob.second );
		return dBlob.second;
	}

	int BioWrite ( const char * pBuf, int iLen )
	{
		sphLogDebugv ( BACK ">> BioBackWrite (%p) for %p, %d" NORM, this, pBuf, iLen );
		if ( !pBuf || iLen<=0 )
			return 0;
		m_tOut.SendBytes ( pBuf, iLen );
		return iLen;
	}

	long BioCtrl ( int iCmd, long iNum, void * pPtr)
	{
		long iRes = 0;
		switch ( iCmd )
		{
		case BIO_CTRL_DGRAM_SET_RECV_TIMEOUT: // BIO_CTRL_DGRAM* used for convenience, as something named 'TIMEOUT'
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) set recv tm %lds" NORM, this, long (iNum / S2US) );
			m_tIn.SetTimeoutUS ( iNum );
			iRes = 1;
			break;
		case BIO_CTRL_DGRAM_GET_RECV_TIMEOUT:
			iRes = m_tIn.GetTimeoutUS();
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) get recv tm %lds" NORM, this, long (iRes / S2US) );
			break;
		case BIO_CTRL_DGRAM_SET_SEND_TIMEOUT:
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) set send tm %lds" NORM, this, long (iNum / S2US) );
			m_tOut.SetWTimeoutUS ( iNum );
			iRes = 1;
			break;
		case BIO_CTRL_DGRAM_GET_SEND_TIMEOUT:
			iRes = m_tOut.GetWTimeoutUS();
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) get recv tm %lds" NORM, this, long (iRes / S2US) );
			break;
		case BIO_CTRL_FLUSH:
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) flush" NORM, this );
			iRes = m_tOut.Flush () ? 1 : -1;
			break;
		case BIO_CTRL_PENDING:
			iRes = Max (0, m_tIn.HasBytes () );
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) read pending, has %ld" NORM, this, iRes );
			break;
		case BIO_CTRL_EOF:
			iRes = m_tIn.GetError() ? 1 : 0;
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) eof, is %ld" NORM, this, iRes );
			break;
		case BIO_CTRL_WPENDING:
			iRes = m_tOut.GetSentCount ();
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) write pending, has %ld" NORM, this, iRes );
			break;
		case BIO_CTRL_PUSH:
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) push %p, ignore" NORM, this, pPtr );
			break;
		case BIO_CTRL_POP:
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) pop %p, ignore" NORM, this, pPtr );
			break;
		default:
			sphLogDebugv ( BACKN "~~ BioBackCtrl (%p) with %d, %ld, %p" NORM, this, iCmd, iNum, pPtr );
		}

		return iRes;
	}
};

#if ( OPENSSL_VERSION_NUMBER < 0x1010000fL)

#define BIO_set_shutdown(pBio,CODE) pBio->shutdown = CODE
#define BIO_get_shutdown(pBio) pBio->shutdown
#define BIO_set_init(pBio,CODE) pBio->init = CODE
#define BIO_set_data(pBio,DATA) pBio->ptr = DATA
#define BIO_get_data(pBio) pBio->ptr
#define BIO_meth_free(pMethod) delete pMethod
#define BIO_get_new_index() (24)
#define BIO_meth_set_create(pMethod,pFn) pMethod->create = pFn
#define BIO_meth_set_destroy(pMethod, pFn ) pMethod->destroy = pFn
#define BIO_meth_set_read(pMethod,pFn) pMethod->bread = pFn
#define BIO_meth_set_write(pMethod,pFn) pMethod->bwrite = pFn
#define BIO_meth_set_ctrl(pMethod,pFn) pMethod->ctrl = pFn

inline static BIO_METHOD * BIO_meth_new ( int iType, const char * szName )
{
	auto pMethod = new BIO_METHOD;
	memset ( pMethod, 0, sizeof ( BIO_METHOD ) );
	pMethod->type = iType;
	pMethod->name = szName;
	return pMethod;
}

#endif // OPENSSL_VERSON_NUMBER dependent code

static int MyBioCreate ( BIO * pBio )
{
	sphLogDebugv ( BACKN "~~ MyBioCreate called with %p" NORM, pBio );
	BIO_set_shutdown ( pBio, BIO_CLOSE );
	BIO_set_init ( pBio, 0 ); // without it write, read will not be called
	BIO_set_data ( pBio, nullptr );
	BIO_clear_flags ( pBio, ~0 );
	return 1;
}

static int MyBioDestroy ( BIO * pBio )
{
	sphLogDebugv ( BACKN "~~ MyBioDestroy called with %p" NORM, pBio );
	if ( !pBio )
		return 0;
	auto pAdapter = ( BioAsyncNetAdapter_c*) BIO_get_data ( pBio );
	assert ( pAdapter );
	SafeDelete ( pAdapter );
	if ( BIO_get_shutdown ( pBio ) )
	{
		BIO_clear_flags ( pBio, ~0 );
		BIO_set_init ( pBio, 0 );
	}
	return 1;
}

static int MyBioWrite ( BIO * pBio, const char * cBuf, int iNum )
{
	auto pAdapter = (BioAsyncNetAdapter_c *) BIO_get_data ( pBio );
	assert ( pAdapter );
	return pAdapter->BioWrite ( cBuf, iNum );
}

static int MyBioRead ( BIO * pBio, char * cBuf, int iNum )
{
	auto pAdapter = (BioAsyncNetAdapter_c *) BIO_get_data ( pBio );
	assert ( pAdapter );
	return pAdapter->BioRead ( cBuf, iNum );
}

static long MyBioCtrl ( BIO * pBio, int iCmd, long iNum, void * pPtr )
{
	auto pAdapter = (BioAsyncNetAdapter_c *) BIO_get_data ( pBio );
	assert ( pAdapter );
	return pAdapter->BioCtrl ( iCmd, iNum, pPtr );
}

static BIO_METHOD * BIO_s_coroAsync ( bool bDestroy )
{
	static BIO_METHOD * pMethod = nullptr;
	if ( bDestroy && pMethod )
	{
		sphLogDebugv ( FRONT "~~ BIO_s_coroAsync (%d)" NORM, !!bDestroy );
		BIO_meth_free ( pMethod );
		pMethod = nullptr;
	} else if ( !bDestroy && !pMethod )
	{
		sphLogDebugv ( FRONT "~~ BIO_s_coroAsync (%d)" NORM, !!bDestroy );
		pMethod = BIO_meth_new ( BIO_get_new_index () | BIO_TYPE_DESCRIPTOR | BIO_TYPE_SOURCE_SINK, "async sock coroutine" );
		BIO_meth_set_create ( pMethod, MyBioCreate );
		BIO_meth_set_destroy ( pMethod, MyBioDestroy );
		BIO_meth_set_read ( pMethod, MyBioRead );
		BIO_meth_set_write ( pMethod, MyBioWrite );
		BIO_meth_set_ctrl ( pMethod, MyBioCtrl );
	}
	return pMethod;
}

static BIO * BIO_new_coroAsync ( AsyncNetBufferPtr_c pSource )
{
	auto pBio = BIO_new ( BIO_s_coroAsync ());
	BIO_set_data ( pBio, new BioAsyncNetAdapter_c ( std::move ( pSource ) ) );
	BIO_set_init ( pBio, 1 );
	return pBio;
}

using BIOPtr_c = SharedPtrCustom_t<BIO *>;

class AsyncSSBufferedSocket_c final : public AsyncNetBuffer_c
{
	BIOPtr_c m_pSslBackend;

	bool SendBuffer ( const VecTraits_T<BYTE> & dData ) final
	{
		assert ( m_pSslBackend );
		CSphScopedProfile tProf ( m_pProfile, SPH_QSTATE_NET_WRITE );
		sphLogDebugv ( FRONT "~~ BioFrontWrite (%p) %d bytes" NORM,
				(BIO*)m_pSslBackend, dData.GetLength () );
		int iSent = 0;
		if ( !dData.IsEmpty ())
			iSent = BIO_write ( m_pSslBackend, dData.begin (), dData.GetLength () );
		auto iRes = BIO_flush ( m_pSslBackend );
		sphLogDebugv ( FRONT ">> BioFrontWrite (%p) done (%d) %d bytes of %d" NORM,
				(BIO*)m_pSslBackend, iRes, iSent, dData.GetLength () );
		return ( iRes>0 );
	}

	int ReadFromBackend ( int iNeed, int iHaveSpace, bool ) final
	{
		assert ( iNeed <= iHaveSpace );
		auto pBuf = AddN(0);

		int iGotTotal = 0;
		while ( iNeed>0 )
		{
			auto iPending = BIO_pending( m_pSslBackend );
			if ( !iPending && BIO_eof ( m_pSslBackend ))
			{
				sphLogDebugv ( FRONT "~~ BIO_eof on frontend. Bailing" NORM );
				return -1;
			}
			auto iCanRead = Max ( iNeed, Min ( iHaveSpace, iPending ));
			sphLogDebugv ( FRONT "~~ BioReadFront %d..%d, can %d, pending %d" NORM,
					iNeed, iHaveSpace, iCanRead, iPending );
			int iGot = BIO_read ( m_pSslBackend, pBuf, iCanRead );
			sphLogDebugv ( FRONT "<< BioReadFront (%p) done %d from %d..%d" NORM,
					(BIO *) m_pSslBackend, iGot, iNeed, iHaveSpace );
			pBuf += iGot;
			iGotTotal += iGot;
			iNeed -= iGot;
			iHaveSpace -= iGot;
			if ( !iGot )
			{
				sphLogDebugv ( FRONT "<< BioReadFront (%p) breaking on %d" NORM,
						(BIO *) m_pSslBackend, iGotTotal );
				break;
			}
		}
		return iGotTotal;
	}

public:
	explicit AsyncSSBufferedSocket_c ( BIOPtr_c pSslFrontend )
		: m_pSslBackend ( std::move ( pSslFrontend ) )
	{}

	void SetWTimeoutUS ( int64_t iTimeoutUS ) final
	{
		BIO_ctrl ( m_pSslBackend, BIO_CTRL_DGRAM_SET_SEND_TIMEOUT, iTimeoutUS, nullptr );
	};

	int64_t GetWTimeoutUS () const final
	{
		return BIO_ctrl ( m_pSslBackend, BIO_CTRL_DGRAM_GET_SEND_TIMEOUT, 0, nullptr );
	}

	void SetTimeoutUS ( int64_t iTimeoutUS ) final
	{
		BIO_ctrl ( m_pSslBackend, BIO_CTRL_DGRAM_SET_RECV_TIMEOUT, iTimeoutUS, nullptr );
	}

	int64_t GetTimeoutUS () const final
	{
		return BIO_ctrl ( m_pSslBackend, BIO_CTRL_DGRAM_GET_RECV_TIMEOUT, 0, nullptr );
	}
};

bool MakeSecureLayer ( AsyncNetBufferPtr_c& pSource )
{
	auto pCtx = GetReadySslCtx();
	if ( !pCtx )
		return false;

	BIOPtr_c pFrontEnd ( BIO_new_ssl ( pCtx, 0 ), [pCtx] (BIO * pBio) {
		BIO_free_all ( pBio );
	} );
	SSL * pSSL = nullptr;

	BIO_get_ssl ( pFrontEnd, &pSSL );
	SSL_set_mode ( pSSL, SSL_MODE_AUTO_RETRY );
	BIO_push ( pFrontEnd, BIO_new_coroAsync ( std::move ( pSource ) ) );
	pSource = new AsyncSSBufferedSocket_c ( std::move ( pFrontEnd ) );
	return true;
}

#endif