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

#ifndef _searchdddl_
#define _searchdddl_

#include "sphinx.h"
#include "searchdsql.h"


class DdlParser_c : public SqlParserTraits_c
{
public:
	// this exists because we have separate field/attribute entities in the schema, but not in DDL
	enum
	{
		FLAG_NONE		= 0,
		FLAG_STORED		= 1<<0,
		FLAG_INDEXED	= 1<<1,
		FLAG_ATTRIBUTE	= 1<<2
	};

			DdlParser_c ( CSphVector<SqlStmt_t> & dStmt );

	void	SetFlag ( DWORD uFlag );
	void	AddCreateTableCol ( const SqlNode_t & tCol, ESphAttr eAttrType );
	void	AddCreateTableBitCol ( const SqlNode_t & tCol, int iBits );
	bool	AddCreateTableField ( const SqlNode_t & tCol, CSphString * pError=nullptr );
	void	AddCreateTableOption ( const SqlNode_t & tName, const SqlNode_t & tValue );

	void	JoinClusterAt ( const SqlNode_t & tAt );

	void	AddInsval ( CSphVector<SqlInsert_t> & dVec, const SqlNode_t & tNode );

private:
	DWORD	m_uFlags = 0;

	void	AddField ( const CSphString & sName, DWORD uFlags );
};


bool ParseDdl ( const char * sQuery, int iLen, CSphVector<SqlStmt_t> & dStmt, CSphString & sError );
bool IsDdlQuery ( const char * szQuery, int iLen );

#endif // _searchdddl_
