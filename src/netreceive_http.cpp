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

#include "netreceive_http.h"
#include "searchdssl.h"

extern int g_iClientTimeoutS; // from searchd.cpp
extern volatile bool g_bMaintenance;

static const char g_sContentLength[] = "\r\r\n\nCcOoNnTtEeNnTt--LlEeNnGgTtHh\0";
static const size_t g_sContentLengthSize = sizeof ( g_sContentLength ) - 1;
static const char g_sHeadEnd[] = "\r\n\r\n";

struct HttpHeaderStreamParser_t
{
	int m_iHeaderEnd = 0;
	int m_iFieldContentLenStart = 0;
	int m_iFieldContentLenVal = 0;

	int m_iCur = 0;
	int m_iCRLF = 0;
	int m_iName = 0;

	bool HeaderFound ( ByteBlob_t tPacket )
	{
		if ( IsNull ( tPacket ) )
			return false;

		auto pBuf = tPacket.first;
		auto iLen = tPacket.second;
		// early exit at for already found request header
		if ( m_iHeaderEnd || m_iCur>=iLen )
			return true;

		const int iCNwoLFSize = ( g_sContentLengthSize-5 ) / 2; // size of just Content-Length field name
		for ( ; m_iCur<iLen; ++m_iCur )
		{
			m_iCRLF = ( pBuf[m_iCur]==g_sHeadEnd[m_iCRLF] ? m_iCRLF+1 : 0 );
			m_iName = ( !m_iFieldContentLenStart
					&& ( pBuf[m_iCur]==g_sContentLength[m_iName] || pBuf[m_iCur]==g_sContentLength[m_iName+1] )
					? m_iName+2 : 0 );

			// header end found
			if ( m_iCRLF==sizeof ( g_sHeadEnd )-1 )
			{
				m_iHeaderEnd = m_iCur+1;
				break;
			}
			// Content-Length field found
			if ( !m_iFieldContentLenStart && m_iName==g_sContentLengthSize-1 )
				m_iFieldContentLenStart = m_iCur-iCNwoLFSize+1;
		}

		// parse Content-Length field value
		while ( m_iHeaderEnd && m_iFieldContentLenStart )
		{
			int iNumStart = m_iFieldContentLenStart+iCNwoLFSize;
			// skip spaces
			while ( iNumStart<m_iHeaderEnd && pBuf[iNumStart]==' ' )
				++iNumStart;
			if ( iNumStart>=m_iHeaderEnd || pBuf[iNumStart]!=':' )
				break;

			++iNumStart; // skip ':' delimiter
			m_iFieldContentLenVal = atoi (
					(const char *) pBuf+iNumStart ); // atoi handles leading spaces and tail not digital chars
			break;
		}

		return ( m_iHeaderEnd>0 );
	}
};

void HttpServe ( AsyncNetBufferPtr_c pBuf )
{
	// non-vip connections in maintainance should be already rejected on accept
	assert  ( !g_bMaintenance || myinfo::IsVIP() );

	bool bINeedSSL = myinfo::GetProto ()==Proto_e::HTTPS;
	bool bHeNeedSSL = myinfo::IsSSL ();
	bool bICanSSL = bHeNeedSSL ? CheckWeCanUseSSL () : false;

	myinfo::SetProto ( Proto_e::HTTP );

	// he needs, but I can't
	if ( bHeNeedSSL && !bICanSSL )
	{
		if ( bINeedSSL )
			sphWarning ( "Client tries to connect with https to secure port, but we can't serve" );

		// that will drop the connection (we can't say anything as can't encrypt our message)
		return;
	}

	// he connects to secured port with plain http
	if ( bINeedSSL && !bHeNeedSSL )
	{
		CSphVector<BYTE> dResult;
		sphHttpErrorReply ( dResult, SPH_HTTP_STATUS_400, "The plain HTTP request was sent to HTTPS port" );
		auto & tOut = *(NetGenericOutputBuffer_c *) pBuf;
		tOut.SwapData ( dResult );
		tOut.Flush (); // no need to check return code since we break anyway
		return;
	}


	// set off query guard
	auto & tCrashQuery = GlobalCrashQueryGetRef();
	tCrashQuery.m_eType = QUERY_JSON;

	int iCID = myinfo::ConnID();
	const char * sClientIP = myinfo::szClientName();

	// needed to check permission to turn maintenance mode on/off

	if ( bHeNeedSSL )
		myinfo::SetSSL ( MakeSecureLayer ( pBuf ) );

	auto& tOut = *(NetGenericOutputBuffer_c *) pBuf;
	auto& tIn = *(AsyncNetInputBuffer_c *) pBuf;

	bool bKeepAlive = false;

	do
	{
		tIn.DiscardProcessed ( -1 ); // -1 means 'force flush'

		HttpHeaderStreamParser_t tHeadParser;
		while ( !tHeadParser.HeaderFound ( tIn.Tail() ))
		{
			auto iChunk = tIn.ReadAny ( g_iMaxPacketSize );
			if ( iChunk>0 )
				continue;

			if ( !iChunk )
				sphWarning ( "failed to receive HTTP request (client=%s(%d)) max packet size(%d) exceeded)",
					sClientIP, iCID, g_iMaxPacketSize );

			return;
		}

		int iPacketLen = tHeadParser.m_iHeaderEnd+tHeadParser.m_iFieldContentLenVal;
		if ( !tIn.ReadFrom ( iPacketLen )) {
			sphWarning ( "failed to receive HTTP request (client=%s(%d), exp=%d, error='%s')", sClientIP, iCID,
						 iPacketLen, sphSockError ());
			return;
		}

		// Temporary write \0 at the end, since parser wants z-terminated buf
		auto uOldByte = tIn.Terminate ( iPacketLen, '\0' );
		auto tPacket = tIn.PopTail (iPacketLen);

		CSphVector<BYTE> dResult;
		if ( IsMaxedOut() )
		{
			sphHttpErrorReply ( dResult, SPH_HTTP_STATUS_503, g_sMaxedOutMessage );
			tOut.SwapData ( dResult );
			tOut.Flush (); // no need to check return code since we break anyway
			gStats().m_iMaxedOut.fetch_add ( 1, std::memory_order_relaxed );
			break;
		}

		tCrashQuery.m_dQuery = tPacket;

		if ( sphLoopClientHttp ( tPacket.first, tPacket.second, dResult ) )
		{
			if ( !bKeepAlive )
				tIn.SetTimeoutUS ( S2US * g_iClientTimeoutS );
			bKeepAlive = true;
		} else {
			if ( bKeepAlive )
				tIn.SetTimeoutUS ( S2US * g_iReadTimeoutS );
			bKeepAlive = false;
		}

		tIn.Terminate ( 0, uOldByte ); // return back prev byte

		tOut.SwapData (dResult);
		if ( !tOut.Flush () )
			break;
	} while ( bKeepAlive );
}