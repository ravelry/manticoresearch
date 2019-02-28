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

/// @file _searchdreplication_.h
/// Declarations for the stuff specifically needed for replication


#ifndef _searchdreplication_
#define _searchdreplication_

void JsonLoadConfig ( const CSphConfigSection & hSearchd );
void JsonDoneConfig();
void JsonConfigConfigureAndPreload ( int & iValidIndexes, int & iCounter  );

enum ReplicationCommand_e
{
	RCOMMAND_PQUERY_ADD = 0,
	RCOMMAND_ROLLBACK,
	RCOMMAND_DELETE,
	RCOMMAND_TRUNCATE,
	RCOMMAND_CLUSTER_ALTER_ADD,
	RCOMMAND_CLUSTER_ALTER_DROP,

	RCOMMAND_TOTAL
};

struct ReplicationCommand_t
{
	// common
	ReplicationCommand_e	m_eCommand { RCOMMAND_TOTAL };
	CSphString				m_sIndex;
	CSphString				m_sCluster;

	// add
	StoredQueryDesc_t		m_tPQ;
	StoredQuery_i *			m_pStored = nullptr;

	// delete
	CSphVector<uint64_t>	m_dDeleteQueries;
	CSphString				m_sDeleteTags;

	// truncate
	bool					m_bReconfigure = false;
	CSphReconfigureSettings m_tReconfigureSettings;

	bool					m_bCheckIndex = true;
	bool					m_bIsolated = false;
};

bool ReplicateSetOption ( const CSphString & sCluster, const CSphString & sOpt, CSphString & sError );
bool HandleCmdReplicate ( ReplicationCommand_t & tCmd, CSphString & sError, int * pDeletedCount );

void Shutdown ();
// unfreeze threads waiting of replication started
void ReplicateClustersDelete();
bool ReplicationStart ( const CSphConfigSection & hSearchd, bool bNewCluster, bool bForce );
bool ClusterJoin ( const CSphString & sCluster, const StrVec_t & dNames, const CSphVector<SqlInsert_t> & dValues, CSphString & sError );
bool ClusterCreate ( const CSphString & sCluster, const StrVec_t & dNames, const CSphVector<SqlInsert_t> & dValues, CSphString & sError );
bool ClusterDelete ( const CSphString & sCluster, CSphString & sError, CSphString & sWarning );
void HandleCommandClusterPq ( CachedOutputBuffer_c & tOut, WORD uCommandVer, InputBuffer_c & tBuf, const char * sClient );
bool ClusterAlter ( const CSphString & sCluster, const CSphString & sIndex, bool bAdd, CSphString & sError, CSphString & sWarning );

// 'like' matcher
class CheckLike
{
private:
	CSphString m_sPattern;

public:
	explicit CheckLike ( const char * sPattern );
	bool Match ( const char * sValue );
};

// string vector with 'like' matcher
class VectorLike : public StrVec_t, public CheckLike
{
public:
	CSphString m_sColKey;
	CSphString m_sColValue;

public:

	VectorLike ();
	explicit VectorLike ( const CSphString& sPattern );

	const char * szColKey() const;
	const char * szColValue() const;
	bool MatchAdd ( const char* sValue );
	bool MatchAddVa ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );
};

void ReplicateClustersStatus ( VectorLike & dStatus );

#define SPH_ADDRESS_SIZE		sizeof("000.000.000.000")
#define SPH_ADDRPORT_SIZE		sizeof("000.000.000.000:00000")

enum ProtocolType_e
{
	PROTO_SPHINX = 0,
	PROTO_MYSQL41,
	PROTO_HTTP,
	PROTO_REPLICATION,

	PROTO_TOTAL
};

struct ListenerDesc_t
{
	ProtocolType_e	m_eProto;
	CSphString		m_sUnix;
	DWORD			m_uIP;
	int				m_iPort;
	bool			m_bVIP;
};

ListenerDesc_t ParseListener ( const char * sSpec );
ESphAddIndex ConfigureAndPreload ( const CSphConfigSection & hIndex, const char * sIndexName, bool bJson );
ESphAddIndex AddIndex ( const char * szIndexName, const CSphConfigSection & hIndex, bool bReplace=false );
bool PreallocNewIndex ( ServedDesc_t & tIdx, const CSphConfigSection * pConfig, const char * szIndexName );
bool CheckIndexCluster ( const CSphString & sIndexName, const ServedDesc_t & tDesc, const CSphString & sStmtCluster, CSphString & sError );

#endif // _searchdreplication_
