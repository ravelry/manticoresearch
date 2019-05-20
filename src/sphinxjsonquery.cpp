//
// Copyright (c) 2017-2019, Manticore Software LTD (http://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinxjsonquery.h"
#include "sphinxquery.h"
#include "sphinxsearch.h"
#include "sphinxplugin.h"
#include "sphinxutils.h"
#include "sphinxpq.h"
#include "searchdaemon.h"
#include "sphinxjson.h"
#include "attribute.h"

#include "json/cJSON.h"

static const char * g_szAll = "_all";
static const char * g_szFilter = "_@filter_";
static const char g_sHighlight[] = "_@highlight_";
static const char g_sOrder[] = "_@order_";


static bool	IsFilter ( const JsonObj_c & tJson )
{
	if ( !tJson )
		return false;

	CSphString sName = tJson.Name();

	if ( sName=="equals" )
		return true;

	if ( sName=="range" )
		return true;

	if ( sName=="geo_distance" )
		return true;

	return false;
}

//////////////////////////////////////////////////////////////////////////
class QueryTreeBuilder_c : public XQParseHelper_c
{
public:
					QueryTreeBuilder_c ( const CSphQuery * pQuery, const ISphTokenizer * pQueryTokenizerQL, const CSphIndexSettings & tSettings );

	void			CollectKeywords ( const char * szStr, XQNode_t * pNode, const XQLimitSpec_t & tLimitSpec );

	bool			HandleFieldBlockStart ( const char * & /*pPtr*/ ) override { return true; }
	virtual bool	HandleSpecialFields ( const char * & pPtr, FieldMask_t & dFields ) override;
	virtual bool	NeedTrailingSeparator() override { return false; }

	XQNode_t *		CreateNode ( XQLimitSpec_t & tLimitSpec );

	const ISphTokenizer *		GetQLTokenizer() { return m_pQueryTokenizerQL; }
	const CSphIndexSettings &	GetIndexSettings() { return m_tSettings; }
	const CSphQuery *			GetQuery() { return m_pQuery; }

private:
	const CSphQuery *			m_pQuery {nullptr};
	const ISphTokenizer *		m_pQueryTokenizerQL {nullptr};
	const CSphIndexSettings &	m_tSettings;

	void			AddChildKeyword ( XQNode_t * pParent, const char * szKeyword, int iSkippedPosBeforeToken, const XQLimitSpec_t & tLimitSpec );
};


QueryTreeBuilder_c::QueryTreeBuilder_c ( const CSphQuery * pQuery, const ISphTokenizer * pQueryTokenizerQL, const CSphIndexSettings & tSettings )
	: m_pQuery ( pQuery )
	, m_pQueryTokenizerQL ( pQueryTokenizerQL )
	, m_tSettings ( tSettings )
{}


void QueryTreeBuilder_c::CollectKeywords ( const char * szStr, XQNode_t * pNode, const XQLimitSpec_t & tLimitSpec )
{
	m_pTokenizer->SetBuffer ( (const BYTE*)szStr, strlen ( szStr ) );

	while (true)
	{
		int iSkippedPosBeforeToken = 0;
		if ( m_bWasBlended )
		{
			iSkippedPosBeforeToken = m_pTokenizer->SkipBlended();
			// just add all skipped blended parts except blended head (already added to atomPos)
			if ( iSkippedPosBeforeToken>1 )
				m_iAtomPos += iSkippedPosBeforeToken - 1;
		}

		const char * sToken = (const char *) m_pTokenizer->GetToken ();
		if ( !sToken )
		{
			AddChildKeyword ( pNode, NULL, iSkippedPosBeforeToken, tLimitSpec );
			break;
		}

		// now let's do some token post-processing
		m_bWasBlended = m_pTokenizer->TokenIsBlended();

		int iPrevDeltaPos = 0;
		if ( m_pPlugin && m_pPlugin->m_fnPushToken )
			sToken = m_pPlugin->m_fnPushToken ( m_pPluginData, (char*)sToken, &iPrevDeltaPos, m_pTokenizer->GetTokenStart(), m_pTokenizer->GetTokenEnd() - m_pTokenizer->GetTokenStart() );

		m_iAtomPos += 1 + iPrevDeltaPos;

		bool bMultiDestHead = false;
		bool bMultiDest = false;
		int iDestCount = 0;
		// do nothing inside phrase
		if ( !m_pTokenizer->m_bPhrase )
			bMultiDest = m_pTokenizer->WasTokenMultiformDestination ( bMultiDestHead, iDestCount );

		// check for stopword, and create that node
		// temp buffer is required, because GetWordID() might expand (!) the keyword in-place
		BYTE sTmp [ MAX_TOKEN_BYTES ];

		strncpy ( (char*)sTmp, sToken, MAX_TOKEN_BYTES );
		sTmp[MAX_TOKEN_BYTES-1] = '\0';

		int iStopWord = 0;
		if ( m_pPlugin && m_pPlugin->m_fnPreMorph )
			m_pPlugin->m_fnPreMorph ( m_pPluginData, (char*)sTmp, &iStopWord );

		SphWordID_t uWordId = iStopWord ? 0 : m_pDict->GetWordID ( sTmp );

		if ( uWordId && m_pPlugin && m_pPlugin->m_fnPostMorph )
		{
			int iRes = m_pPlugin->m_fnPostMorph ( m_pPluginData, (char*)sTmp, &iStopWord );
			if ( iStopWord )
				uWordId = 0;
			else if ( iRes )
				uWordId = m_pDict->GetWordIDNonStemmed ( sTmp );
		}

		if ( !uWordId )
		{
			sToken = NULL;
			// stopwords with step=0 must not affect pos
			if ( m_bEmptyStopword )
				m_iAtomPos--;
		}

		if ( bMultiDest && !bMultiDestHead )
		{
			assert ( m_dMultiforms.GetLength() );
			m_dMultiforms.Last().m_iDestCount++;
			m_dDestForms.Add ( sToken );
		} else
			AddChildKeyword ( pNode, sToken, iSkippedPosBeforeToken, tLimitSpec );

		if ( bMultiDestHead )
		{
			MultiformNode_t & tMulti = m_dMultiforms.Add();
			tMulti.m_pNode = pNode;
			tMulti.m_iDestStart = m_dDestForms.GetLength();
			tMulti.m_iDestCount = 0;
		}
	}
}


bool QueryTreeBuilder_c::HandleSpecialFields ( const char * & pPtr, FieldMask_t & dFields )
{
	if ( *pPtr=='_' )
	{
		int iLen = strlen(g_szAll);
		if ( !strncmp ( pPtr, g_szAll, iLen ) )
		{
			pPtr += iLen;
			dFields.SetAll();
			return true;
		}
	}

	return false;
}


XQNode_t * QueryTreeBuilder_c::CreateNode ( XQLimitSpec_t & tLimitSpec )
{
	XQNode_t * pNode = new XQNode_t(tLimitSpec);
	m_dSpawned.Add ( pNode );
	return pNode;
}


void QueryTreeBuilder_c::AddChildKeyword ( XQNode_t * pParent, const char * szKeyword, int iSkippedPosBeforeToken, const XQLimitSpec_t & tLimitSpec )
{
	XQKeyword_t tKeyword ( szKeyword, m_iAtomPos );
	tKeyword.m_iSkippedBefore = iSkippedPosBeforeToken;
	XQNode_t * pNode = new XQNode_t ( tLimitSpec );
	pNode->m_pParent = pParent;
	pNode->m_dWords.Add ( tKeyword );
	pParent->m_dChildren.Add ( pNode );
	m_dSpawned.Add ( pNode );
}

//////////////////////////////////////////////////////////////////////////

class QueryParserJson_c : public QueryParser_i
{
public:
	virtual bool	IsFullscan ( const CSphQuery & tQuery ) const;
	virtual bool	IsFullscan ( const XQQuery_t & tQuery ) const;
	virtual bool	ParseQuery ( XQQuery_t & tParsed, const char * sQuery, const CSphQuery * pQuery,
		const ISphTokenizer * pQueryTokenizer, const ISphTokenizer * pQueryTokenizerJson,
		const CSphSchema * pSchema, CSphDict * pDict, const CSphIndexSettings & tSettings ) const;

private:
	XQNode_t *		ConstructMatchNode ( XQNode_t * pParent, const JsonObj_c & tJson, bool bPhrase, QueryTreeBuilder_c & tBuilder ) const;
	XQNode_t *		ConstructBoolNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const;
	XQNode_t *		ConstructQLNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const;
	XQNode_t *		ConstructMatchAllNode ( XQNode_t * pParent, QueryTreeBuilder_c & tBuilder ) const;

	bool			ConstructBoolNodeItems ( const JsonObj_c & tClause, CSphVector<XQNode_t *> & dItems, QueryTreeBuilder_c & tBuilder ) const;
	bool			ConstructNodeOrFilter ( const JsonObj_c & tItem, CSphVector<XQNode_t *> & dNodes, QueryTreeBuilder_c & tBuilder ) const;

	XQNode_t *		ConstructNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const;
};


bool QueryParserJson_c::IsFullscan ( const CSphQuery & tQuery ) const
{
	// fixme: add more checks here
	return tQuery.m_sQuery.IsEmpty();
}


bool QueryParserJson_c::IsFullscan ( const XQQuery_t & tQuery ) const
{
	return !tQuery.m_pRoot || ( !tQuery.m_pRoot->m_dChildren.GetLength() && !tQuery.m_pRoot->m_dWords.GetLength() );
}


bool QueryParserJson_c::ParseQuery ( XQQuery_t & tParsed, const char * szQuery, const CSphQuery * pQuery,
	const ISphTokenizer * pQueryTokenizerQL, const ISphTokenizer * pQueryTokenizerJson, const CSphSchema * pSchema, CSphDict * pDict,
	const CSphIndexSettings & tSettings ) const
{
	JsonObj_c tRoot ( szQuery );

	// take only the first item of the query; ignore the rest
	int iNumIndexes = tRoot.Size();
	if ( !iNumIndexes )
	{
		tParsed.m_sParseError = "\"query\" property is empty";
		return false;
	}

	ISphTokenizerRefPtr_c pMyJsonTokenizer { pQueryTokenizerJson->Clone ( SPH_CLONE_QUERY_LIGHTWEIGHT ) };
	CSphDictRefPtr_c pMyDict { GetStatelessDict ( pDict ) };

	QueryTreeBuilder_c tBuilder ( pQuery, pQueryTokenizerQL, tSettings );
	tBuilder.Setup ( pSchema, pMyJsonTokenizer, pMyDict, &tParsed, tSettings );

	tParsed.m_pRoot = ConstructNode ( nullptr, tRoot[0], tBuilder );
	if ( tBuilder.IsError() )
	{
		tBuilder.Cleanup();
		return false;
	}

	XQLimitSpec_t tLimitSpec;
	tParsed.m_pRoot = tBuilder.FixupTree ( tParsed.m_pRoot, tLimitSpec );
	if ( tBuilder.IsError() )
	{
		tBuilder.Cleanup();
		return false;
	}

	return true;
}


static const char * g_szOperatorNames[]=
{
	"and",
	"or"
};


static XQOperator_e StrToNodeOp ( const char * szStr )
{
	if ( !szStr )
		return SPH_QUERY_TOTAL;

	int iOp=0;
	for ( auto i : g_szOperatorNames )
	{
		if ( !strcmp ( szStr, i ) )
			return XQOperator_e(iOp);

		iOp++;
	}

	return SPH_QUERY_TOTAL;
}


XQNode_t * QueryParserJson_c::ConstructMatchNode ( XQNode_t * pParent, const JsonObj_c & tJson, bool bPhrase, QueryTreeBuilder_c & tBuilder ) const
{
	if ( !tJson.IsObj() )
	{
		tBuilder.Error ( "\"match\" value should be an object" );
		return nullptr;
	}

	if ( tJson.Size()!=1 )
	{
		tBuilder.Error ( "ill-formed \"match\" property" );
		return nullptr;
	}

	JsonObj_c tFields = tJson[0];
	tBuilder.SetString ( tFields.Name() );

	XQLimitSpec_t tLimitSpec;
	const char * szQuery = nullptr;
	XQOperator_e eNodeOp = bPhrase ? SPH_QUERY_PHRASE : SPH_QUERY_OR;
	bool bIgnore = false;

	if ( !tBuilder.ParseFields ( tLimitSpec.m_dFieldMask, tLimitSpec.m_iFieldMaxPos, bIgnore ) )
		return nullptr;

	if ( bIgnore )
	{
		tBuilder.Warning ( R"(ignoring fields in "%s", using "_all")", tFields.Name() );
		tLimitSpec.Reset();
	}

	tLimitSpec.m_bFieldSpec = true;

	if ( tFields.IsObj() )
	{
		// matching with flags
		CSphString sError;
		JsonObj_c tQuery = tFields.GetStrItem ( "query", sError );
		if ( !tQuery )
		{
			tBuilder.Error ( "%s", sError.cstr() );
			return nullptr;
		}

		szQuery = tQuery.SzVal();

		if ( !bPhrase )
		{
			JsonObj_c tOp = tFields.GetItem ( "operator" );
			if ( tOp ) // "and", "or"
			{
				eNodeOp = StrToNodeOp ( tOp.SzVal() );
				if ( eNodeOp==SPH_QUERY_TOTAL )
				{
					tBuilder.Error ( "unknown operator: \"%s\"", tOp.SzVal() );
					return nullptr;
				}
			}
		}
	} else
	{
		// simple list of keywords
		if ( !tFields.IsStr() )
		{
			tBuilder.Warning ( "values of properties in \"match\" should be strings or objects" );
			return nullptr;
		}

		szQuery = tFields.SzVal();
	}

	assert ( szQuery );

	XQNode_t * pNewNode = tBuilder.CreateNode ( tLimitSpec );
	pNewNode->SetOp ( eNodeOp );
	pNewNode->m_pParent = pParent;

	tBuilder.CollectKeywords ( szQuery, pNewNode, tLimitSpec );

	return pNewNode;
}


bool QueryParserJson_c::ConstructNodeOrFilter ( const JsonObj_c & tItem, CSphVector<XQNode_t *> & dNodes, QueryTreeBuilder_c & tBuilder ) const
{
	// we created filters before, no need to process them again
	if ( !IsFilter ( tItem ) )
	{
		XQNode_t * pNode = ConstructNode ( NULL, tItem, tBuilder );
		if ( !pNode )
			return false;

		dNodes.Add ( pNode );
	}

	return true;
}


bool QueryParserJson_c::ConstructBoolNodeItems ( const JsonObj_c & tClause, CSphVector<XQNode_t *> & dItems, QueryTreeBuilder_c & tBuilder ) const
{
	if ( tClause.IsArray() )
	{
		for ( const auto & tObject : tClause )
		{
			if ( !tObject.IsObj() )
			{
				tBuilder.Error ( "\"%s\" array value should be an object", tClause.Name() );
				return false;
			}

			if ( !ConstructNodeOrFilter ( tObject[0], dItems, tBuilder ) )
				return false;
		}
	} else if ( tClause.IsObj() )
	{
		if ( !ConstructNodeOrFilter ( tClause[0], dItems, tBuilder ) )
			return false;
	} else
	{
		tBuilder.Error ( "\"%s\" value should be an object or an array", tClause.Name() );
		return false;
	}

	return true;
}


XQNode_t * QueryParserJson_c::ConstructBoolNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const
{
	if ( !tJson.IsObj() )
	{
		tBuilder.Error ( "\"bool\" value should be an object" );
		return nullptr;
	}

	CSphVector<XQNode_t *> dMust, dShould, dMustNot;

	for ( const auto & tClause : tJson )
	{
		CSphString sName = tClause.Name();
		if ( sName=="must" )
		{
			if ( !ConstructBoolNodeItems ( tClause, dMust, tBuilder ) )
				return nullptr;
		} else if ( sName=="should" )
		{
			if ( !ConstructBoolNodeItems ( tClause, dShould, tBuilder ) )
				return nullptr;
		} else if ( sName=="must_not" )
		{
			if ( !ConstructBoolNodeItems ( tClause, dMustNot, tBuilder ) )
				return nullptr;
		} else
		{
			tBuilder.Error ( "unknown bool query type: \"%s\"", sName.cstr() );
			return nullptr;
		}
	}

	XQNode_t * pMustNode = nullptr;
	XQNode_t * pShouldNode = nullptr;
	XQNode_t * pMustNotNode = nullptr;

	XQLimitSpec_t tLimitSpec;

	if ( dMust.GetLength() )
	{
		// no need to construct AND node for a single child
		if ( dMust.GetLength()==1 )
			pMustNode = dMust[0];
		else
		{
			XQNode_t * pAndNode = tBuilder.CreateNode ( tLimitSpec );
			pAndNode->SetOp ( SPH_QUERY_AND );

			for ( auto & i : dMust )
			{
				pAndNode->m_dChildren.Add(i);
				i->m_pParent = pAndNode;
			}

			pMustNode = pAndNode;
		}
	}

	if ( dShould.GetLength() )
	{
		if ( dShould.GetLength()==1 )
			pShouldNode = dShould[0];
		else
		{
			XQNode_t * pOrNode = tBuilder.CreateNode ( tLimitSpec );
			pOrNode->SetOp ( SPH_QUERY_OR );

			for ( auto & i : dShould )
			{
				pOrNode->m_dChildren.Add(i);
				i->m_pParent = pOrNode;
			}

			pShouldNode = pOrNode;
		}
	}

	// slightly different case - we need to construct the NOT node anyway
	if ( dMustNot.GetLength() )
	{
		XQNode_t * pNotNode = tBuilder.CreateNode ( tLimitSpec );
		pNotNode->SetOp ( SPH_QUERY_NOT );

		if ( dMustNot.GetLength()==1 )
		{
			pNotNode->m_dChildren.Add ( dMustNot[0] );
			dMustNot[0]->m_pParent = pNotNode;
		} else
		{
			XQNode_t * pOrNode = tBuilder.CreateNode ( tLimitSpec );
			pOrNode->SetOp ( SPH_QUERY_OR );

			for ( auto & i : dMustNot )
			{
				pOrNode->m_dChildren.Add ( i );
				i->m_pParent = pOrNode;
			}

			pNotNode->m_dChildren.Add ( pOrNode );
			pOrNode->m_pParent = pNotNode;
		}

		pMustNotNode = pNotNode;
	}

	int iTotalNodes = 0;
	iTotalNodes += pMustNode ? 1 : 0;
	iTotalNodes += pShouldNode ? 1 : 0;
	iTotalNodes += pMustNotNode ? 1 : 0;
	
	if ( !iTotalNodes )
		return nullptr;
	else if ( iTotalNodes==1 )
	{
		XQNode_t * pResultNode = nullptr;
		if ( pMustNode )
			pResultNode = pMustNode;
		else if ( pShouldNode )
			pResultNode = pShouldNode;
		else
			pResultNode = pMustNotNode;

		assert ( pResultNode );

		pResultNode->m_pParent = pParent;
		return pResultNode;
	} else
	{
		XQNode_t * pResultNode = pMustNode ? pMustNode : pMustNotNode;
		assert ( pResultNode );
		
		// combine 'must' and 'must_not' with AND
		if ( pMustNode && pMustNotNode )
		{
			XQNode_t * pAndNode = tBuilder.CreateNode(tLimitSpec);
			pAndNode->SetOp(SPH_QUERY_AND);
			pAndNode->m_dChildren.Add ( pMustNode );
			pAndNode->m_dChildren.Add ( pMustNotNode );
			pAndNode->m_pParent = pParent;		// may be modified later

			pMustNode->m_pParent = pAndNode;
			pMustNotNode->m_pParent = pAndNode;

			pResultNode = pAndNode;
		}

		// combine 'result' node and 'should' node with MAYBE
		if ( pShouldNode )
		{
			XQNode_t * pMaybeNode = tBuilder.CreateNode ( tLimitSpec );
			pMaybeNode->SetOp ( SPH_QUERY_MAYBE );
			pMaybeNode->m_dChildren.Add ( pResultNode );
			pMaybeNode->m_dChildren.Add ( pShouldNode );
			pMaybeNode->m_pParent = pParent;

			pShouldNode->m_pParent = pMaybeNode;
			pResultNode->m_pParent = pMaybeNode;

			pResultNode = pMaybeNode;
		}

		return pResultNode;
	}

	return nullptr;
}


XQNode_t * QueryParserJson_c::ConstructQLNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const
{
	if ( !tJson.IsStr() )
	{
		tBuilder.Error ( "\"query_string\" value should be an string" );
		return nullptr;
	}

	XQQuery_t tParsed;
	if ( !sphParseExtendedQuery ( tParsed, tJson.StrVal().cstr(), tBuilder.GetQuery(), tBuilder.GetQLTokenizer(), tBuilder.GetSchema(), tBuilder.GetDict(), tBuilder.GetIndexSettings() ) )
	{
		tBuilder.Error ( "%s", tParsed.m_sParseError.cstr() );
		return nullptr;
	}

	if ( !tParsed.m_sParseWarning.IsEmpty() )
		tBuilder.Warning ( "%s", tParsed.m_sParseWarning.cstr() );

	XQNode_t * pRoot = tParsed.m_pRoot;
	tParsed.m_pRoot = nullptr;
	return pRoot;
}


XQNode_t * QueryParserJson_c::ConstructMatchAllNode ( XQNode_t * pParent, QueryTreeBuilder_c & tBuilder ) const
{
	XQLimitSpec_t tLimitSpec;
	XQNode_t * pNewNode = tBuilder.CreateNode ( tLimitSpec );
	pNewNode->SetOp ( SPH_QUERY_NULL );
	pNewNode->m_pParent = pParent;
	return pNewNode;
}


XQNode_t * QueryParserJson_c::ConstructNode ( XQNode_t * pParent, const JsonObj_c & tJson, QueryTreeBuilder_c & tBuilder ) const
{
	CSphString sName = tJson.Name();
	if ( !tJson || sName.IsEmpty() )
	{
		tBuilder.Error ( "empty json found" );
		return nullptr;
	}

	bool bMatch = sName=="match";
	bool bPhrase = sName=="match_phrase";
	if ( bMatch || bPhrase )
		return ConstructMatchNode ( pParent, tJson, bPhrase, tBuilder );

	if ( sName=="match_all" )
		return ConstructMatchAllNode ( pParent, tBuilder );

	if ( sName=="bool" )
		return ConstructBoolNode ( pParent, tJson, tBuilder );

	if ( sName=="query_string" )
		return ConstructQLNode ( pParent, tJson, tBuilder );

	return nullptr;
}

bool NonEmptyQuery ( const JsonObj_c & tQuery )
{
	return ( tQuery.HasItem("match")
	|| tQuery.HasItem("match_phrase")
	|| tQuery.HasItem("bool") )
	|| tQuery.HasItem("query_string");
}


//////////////////////////////////////////////////////////////////////////
struct LocationField_t
{
	float m_fLat =0.0f;
	float m_fLon = 0.0f;
};

struct LocationSource_t
{
	CSphString m_sLat;
	CSphString m_sLon;
};

static bool ParseLocation ( const char * sName, const JsonObj_c & tLoc, LocationField_t * pField, LocationSource_t * pSource, CSphString & sError );

class GeoDistInfo_c
{
public:
	bool				Parse ( const JsonObj_c & tRoot, bool bNeedDistance, CSphString & sError, CSphString & sWarning );
	CSphString			BuildExprString() const;
	bool				IsGeoDist() const { return m_bGeodist; }
	float				GetDistance() const { return m_fDistance; }

private:
	bool				m_bGeodist {false};
	bool				m_bGeodistAdaptive {true};
	float				m_fDistance {0.0f};

	LocationField_t		m_tLocAnchor;
	LocationSource_t	m_tLocSource;

	bool				ParseDistance ( const JsonObj_c & tDistance, CSphString & sError );
};


bool GeoDistInfo_c::Parse ( const JsonObj_c & tRoot, bool bNeedDistance, CSphString & sError, CSphString & sWarning )
{
	JsonObj_c tLocAnchor = tRoot.GetItem("location_anchor");
	JsonObj_c tLocSource = tRoot.GetItem("location_source");

	if ( !tLocAnchor || !tLocSource )
	{
		if ( !tLocAnchor && !tLocSource )
			sError = R"("location_anchor" and "location_source" properties missing)";
		else
			sError.SetSprintf ( "\"%s\" property missing", ( !tLocAnchor ? "location_anchor" : "location_source" ) );
		return false;
	}

	if ( !ParseLocation ( "location_anchor", tLocAnchor, &m_tLocAnchor, nullptr, sError )
		|| !ParseLocation ( "location_source", tLocSource, nullptr, &m_tLocSource, sError ) )
		return false;

	JsonObj_c tType = tRoot.GetStrItem ( "distance_type", sError, true );
	if ( tType )
	{
		CSphString sType = tType.StrVal();
		if ( sType!="adaptive" && sType!="haversine" )
		{
			sWarning.SetSprintf ( "\"distance_type\" property type is invalid: \"%s\", defaulting to \"adaptive\"", sType.cstr() );
			m_bGeodistAdaptive = true;
		} else
			m_bGeodistAdaptive = sType=="adaptive";

	} else if ( !sError.IsEmpty() )
		return false;

	JsonObj_c tDistance = tRoot.GetItem("distance");
	if ( tDistance )
	{
		if ( !ParseDistance ( tDistance, sError ) )
			return false;
	} else if ( bNeedDistance )
	{
		sError = "\"distance\" not specified";
		return false;
	}

	m_bGeodist = true;

	return true;
}


bool GeoDistInfo_c::ParseDistance ( const JsonObj_c & tDistance, CSphString & sError )
{
	if ( tDistance.IsNum() )
	{
		// no units specified, meters assumed
		m_fDistance = tDistance.FltVal();
		return true;
	}

	if ( !tDistance.IsStr() )
	{
		sError = "\"distance\" property should be a number or a string";
		return false;
	}

	const char * p = tDistance.SzVal();
	assert ( p );
	while ( *p && sphIsSpace(*p) )
		p++;

	const char * szNumber = p;
	while ( *p && ( *p=='.' || ( *p>='0' && *p<='9' ) ) )
		p++;

	CSphString sNumber;
	sNumber.SetBinary ( szNumber, p-szNumber );

	while ( *p && sphIsSpace(*p) )
		p++;

	const char * szUnit = p;
	while ( *p && sphIsAlpha(*p) )
		p++;

	CSphString sUnit;
	sUnit.SetBinary ( szUnit, p-szUnit );

	m_fDistance = (float)atof ( sNumber.cstr() );

	float fCoeff = 1.0f;	
	if ( !sphGeoDistanceUnit ( sUnit.cstr(), fCoeff ) )
	{
		sError.SetSprintf ( "unknown distance unit: %s", sUnit.cstr() );
		return false;
	}

	m_fDistance *= fCoeff;

	return true;
}


CSphString GeoDistInfo_c::BuildExprString() const
{
	CSphString sResult;
	sResult.SetSprintf ( "GEODIST(%f, %f, %s, %s, {in=deg, out=m, method=%s})", m_tLocAnchor.m_fLat, m_tLocAnchor.m_fLon, m_tLocSource.m_sLat.cstr(), m_tLocSource.m_sLon.cstr(), m_bGeodistAdaptive ? "adaptive" : "haversine" );
	return sResult;
}


//////////////////////////////////////////////////////////////////////////
static void AddToSelectList ( CSphQuery & tQuery, const CSphVector<CSphQueryItem> & dItems, int iFirstItem = 0 )
{
	for ( int i = iFirstItem; i < dItems.GetLength(); i++ )
		tQuery.m_sSelect.SetSprintf ( "%s, %s as %s", tQuery.m_sSelect.cstr(), dItems[i].m_sExpr.cstr(), dItems[i].m_sAlias.cstr() );
}


static JsonObj_c GetFilterColumn ( const JsonObj_c & tJson, CSphString & sError )
{
	if ( !tJson.IsObj() )
	{
		sError = "filter should be an object";
		return JsonNull;
	}

	if ( tJson.Size()!=1 )
	{
		sError = "\"equals\" filter should have only one element";
		return JsonNull;
	}

	JsonObj_c tColumn = tJson[0];
	if ( !tColumn )
	{
		sError = "empty filter found";
		return JsonNull;
	}

	return tColumn;
}


static bool ConstructEqualsFilter ( const JsonObj_c & tJson, CSphVector<CSphFilterSettings> & dFilters, CSphString & sError )
{
	JsonObj_c tColumn = GetFilterColumn ( tJson, sError );
	if ( !tColumn )
		return false;

	if ( !tColumn.IsNum() && !tColumn.IsStr() )
	{
		sError = "\"equals\" filter expects numeric or string values";
		return false;
	}

	CSphFilterSettings tFilter;
	tFilter.m_sAttrName = tColumn.Name();
	sphColumnToLowercase ( const_cast<char *>( tFilter.m_sAttrName.cstr() ) );

	if ( tColumn.IsInt() )
	{
		tFilter.m_eType = SPH_FILTER_VALUES;
		tFilter.m_dValues.Add ( tColumn.IntVal() );
	} else if ( tColumn.IsNum() )
	{
		tFilter.m_eType = SPH_FILTER_FLOATRANGE;
		tFilter.m_fMinValue = tColumn.FltVal();
		tFilter.m_fMaxValue = tColumn.FltVal();
		tFilter.m_bHasEqualMin = true;
		tFilter.m_bHasEqualMax = true;
		tFilter.m_bExclude = false;
	} else
	{
		tFilter.m_eType = SPH_FILTER_STRING;
		tFilter.m_dStrings.Add ( tColumn.StrVal() );
		tFilter.m_bExclude = false;
	}

	dFilters.Add ( tFilter );

	return true;
}


static bool ConstructRangeFilter ( const JsonObj_c & tJson, CSphVector<CSphFilterSettings> & dFilters, CSphString & sError )
{
	JsonObj_c tColumn = GetFilterColumn ( tJson, sError );
	if ( !tColumn )
		return false;

	CSphFilterSettings tNewFilter;
	tNewFilter.m_sAttrName = tColumn.Name();
	sphColumnToLowercase ( const_cast<char *>( tNewFilter.m_sAttrName.cstr() ) );

	tNewFilter.m_bHasEqualMin = false;
	tNewFilter.m_bHasEqualMax = false;

	JsonObj_c tLess = tColumn.GetItem("lt");
	if ( !tLess )
	{
		tLess = tColumn.GetItem("lte");
		tNewFilter.m_bHasEqualMax = tLess;
	}

	JsonObj_c tGreater = tColumn.GetItem("gt");
	if ( !tGreater )
	{
		tGreater = tColumn.GetItem("gte");
		tNewFilter.m_bHasEqualMin = tGreater;
	}

	bool bLess = tLess;
	bool bGreater = tGreater;

	if ( !bLess && !bGreater )
	{
		sError = "empty filter found";
		return false;
	}

	if ( ( bLess && !tLess.IsNum() ) || ( bGreater && !tGreater.IsNum() ) )
	{
		sError = "range filter expects numeric values";
		return false;
	}

	bool bIntFilter = ( bLess && tLess.IsInt() ) || ( bGreater && tGreater.IsInt() );

	if ( bLess )
	{
		if ( bIntFilter )
			tNewFilter.m_iMaxValue = tLess.IntVal();
		else
			tNewFilter.m_fMaxValue = tLess.FltVal();

		tNewFilter.m_bOpenLeft = !bGreater;
	}

	if ( bGreater )
	{
		if ( bIntFilter )
			tNewFilter.m_iMinValue = tGreater.IntVal();
		else
			tNewFilter.m_fMinValue = tGreater.FltVal();

		tNewFilter.m_bOpenRight = !bLess;
	}

	tNewFilter.m_eType = bIntFilter ? SPH_FILTER_RANGE : SPH_FILTER_FLOATRANGE;

	// float filters don't support open ranges
	if ( !bIntFilter )
	{
		if ( tNewFilter.m_bOpenRight )
			tNewFilter.m_fMaxValue = FLT_MAX;

		if ( tNewFilter.m_bOpenLeft )
			tNewFilter.m_fMinValue = FLT_MIN;
	}

	dFilters.Add ( tNewFilter );

	return true;
}


static bool ConstructGeoFilter ( const JsonObj_c & tJson, CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphQueryItem> & dQueryItems, int & iQueryItemId, CSphString & sError, CSphString & sWarning )
{
	GeoDistInfo_c tGeoDist;
	if ( !tGeoDist.Parse ( tJson, true, sError, sWarning ) )
		return false;

	CSphQueryItem & tQueryItem = dQueryItems.Add();
	tQueryItem.m_sExpr = tGeoDist.BuildExprString();
	tQueryItem.m_sAlias.SetSprintf ( "%s%d", g_szFilter, iQueryItemId++ );

	CSphFilterSettings & tFilter = dFilters.Add();
	tFilter.m_sAttrName = tQueryItem.m_sAlias;
	tFilter.m_bOpenLeft = true;
	tFilter.m_bHasEqualMax = true;
	tFilter.m_fMaxValue = tGeoDist.GetDistance();
	tFilter.m_eType = SPH_FILTER_FLOATRANGE;

	return true;
}


static bool ConstructFilter ( const JsonObj_c & tJson, CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphQueryItem> & dQueryItems, int & iQueryItemId, CSphString & sError, CSphString & sWarning )
{
	if ( !IsFilter ( tJson ) )
		return true;

	CSphString sName = tJson.Name();
	if ( sName=="equals" )
		return ConstructEqualsFilter ( tJson, dFilters, sError );

	if ( sName=="range" )
		return ConstructRangeFilter ( tJson, dFilters, sError );

	if ( sName=="geo_distance" )
		return ConstructGeoFilter ( tJson, dFilters, dQueryItems, iQueryItemId, sError, sWarning );

	sError.SetSprintf ( "unknown filter type: %s", sName.cstr() );
	return false;
}


static bool ConstructBoolNodeFilters ( const JsonObj_c & tClause, CSphVector<CSphFilterSettings> & dFilters, CSphVector<CSphQueryItem> & dQueryItems, int & iQueryItemId, CSphString & sError, CSphString & sWarning )
{
	if ( tClause.IsArray() )
	{
		for ( const auto & tObject : tClause )
		{
			if ( !tObject.IsObj() )
			{
				sError.SetSprintf ( "\"%s\" array value should be an object", tClause.Name() );
				return false;
			}

			JsonObj_c tItem = tObject[0];
			if ( !ConstructFilter ( tItem, dFilters, dQueryItems, iQueryItemId, sError, sWarning ) )
				return false;
		}
	} else if ( tClause.IsObj() )
	{
		JsonObj_c tItem = tClause[0];
		if ( !ConstructFilter ( tItem, dFilters, dQueryItems, iQueryItemId, sError, sWarning ) )
			return false;
	} else
	{
		sError.SetSprintf ( "\"%s\" value should be an object or an array", tClause.Name() );
		return false;
	}

	return true;
}


static bool ConstructBoolFilters ( const JsonObj_c & tBool, CSphQuery & tQuery, int & iQueryItemId, CSphString & sError, CSphString & sWarning )
{
	// non-recursive for now, maybe we should fix this later
	if ( !tBool.IsObj() )
	{
		sError = "\"bool\" value should be an object";
		return false;
	}

	CSphVector<CSphFilterSettings> dMust, dShould, dMustNot;
	CSphVector<CSphQueryItem> dMustQI, dShouldQI, dMustNotQI;

	for ( const auto & tClause : tBool )
	{
		CSphString sName = tClause.Name();

		if ( sName=="must" )
		{
			if ( !ConstructBoolNodeFilters ( tClause, dMust, dMustQI, iQueryItemId, sError, sWarning ) )
				return false;
		} else if ( sName=="should" )
		{
			if ( !ConstructBoolNodeFilters ( tClause, dShould, dShouldQI, iQueryItemId, sError, sWarning ) )
				return false;
		} else if ( sName=="must_not" )
		{
			if ( !ConstructBoolNodeFilters ( tClause, dMustNot, dMustNotQI, iQueryItemId, sError, sWarning ) )
				return false;
		} else
		{
			sError.SetSprintf ( "unknown bool query type: \"%s\"", sName.cstr() );
			return false;
		}
	}

	if ( dMustNot.GetLength() )
	{
		int iFilter = dMust.GetLength();
		dMust.Resize ( dMust.GetLength()+dMustNot.GetLength() );
		for ( auto & i : dMustNot )
		{
			i.m_bExclude = true;
			dMust[iFilter++] = i;
		}

		int iQueryItem = dMustQI.GetLength();
		dMustQI.Resize ( dMustQI.GetLength()+dMustNotQI.GetLength() );
		for ( auto & i : dMustNotQI )
			dMustQI[iQueryItem++] = i;
	}

	if ( dMust.GetLength() )
	{
		AddToSelectList ( tQuery, dMustQI );
		tQuery.m_dFilters.SwapData ( dMust );
		tQuery.m_dItems.SwapData ( dMustQI );
		return true;
	}

	if ( dShould.GetLength() )
	{
		AddToSelectList ( tQuery, dShouldQI );
		tQuery.m_dFilters.SwapData ( dShould );
		tQuery.m_dItems.SwapData ( dShouldQI );

		// need a filter tree
		FilterTreeItem_t & tTreeItem = tQuery.m_dFilterTree.Add();
		tTreeItem.m_iFilterItem = 0;
		int iRootNode = 0;

		ARRAY_FOREACH ( i, tQuery.m_dFilters )
		{
			int iNewFilterNodeId = tQuery.m_dFilterTree.GetLength();
			FilterTreeItem_t & tNewFilterNode = tQuery.m_dFilterTree.Add();
			tNewFilterNode.m_iFilterItem = i;

			int iNewOrNodeId = tQuery.m_dFilterTree.GetLength();
			FilterTreeItem_t & tNewOrNode = tQuery.m_dFilterTree.Add();
			tNewOrNode.m_bOr = true;
			tNewOrNode.m_iLeft = iRootNode;
			tNewOrNode.m_iRight = iNewFilterNodeId;

			iRootNode = iNewOrNodeId;
		}
	}

	return true;
}


static bool ConstructFilters ( const JsonObj_c & tJson, CSphQuery & tQuery, CSphString & sError, CSphString & sWarning )
{
	if ( !tJson )
		return false;

	CSphString sName = tJson.Name();
	if ( sName.IsEmpty() )
		return false;

	if ( sName!="query" )
	{
		sError.SetSprintf ( R"("query" expected, got %s)", sName.cstr() );
		return false;
	}

	int iQueryItemId = 0;

	JsonObj_c tBool = tJson.GetItem("bool");
	if ( tBool )
		return ConstructBoolFilters ( tBool, tQuery, iQueryItemId, sError, sWarning );

	for ( const auto & tChild : tJson )
		if ( IsFilter ( tChild ) )
		{
			int iFirstNewItem = tQuery.m_dItems.GetLength();
			if ( !ConstructFilter ( tChild, tQuery.m_dFilters, tQuery.m_dItems, iQueryItemId, sError, sWarning ) )
				return false;

			AddToSelectList ( tQuery, tQuery.m_dItems, iFirstNewItem );

			// handle only the first filter in this case
			break;
		}

	return true;
}

//////////////////////////////////////////////////////////////////////////

static bool ParseSnippet ( const JsonObj_c & tSnip, CSphQuery & tQuery, CSphString & sError );
static bool ParseSort ( const JsonObj_c & tSort, CSphQuery & tQuery, bool & bGotWeight, CSphString & sError, CSphString & sWarning );
static bool ParseSelect ( const JsonObj_c & tSelect, CSphQuery & tQuery, CSphString & sError );
static bool ParseExpr ( const JsonObj_c & tExpr, CSphQuery & tQuery, CSphString & sError );


static bool ParseIndex ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, CSphString & sError )
{
	if ( !tRoot )
	{
		sError.SetSprintf ( "unable to parse: %s", tRoot.GetErrorPtr() );
		return false;
	}

	JsonObj_c tIndex = tRoot.GetStrItem ( "index", sError );
	if ( !tIndex )
		return false;

	tStmt.m_sIndex = tIndex.StrVal();
	tStmt.m_tQuery.m_sIndexes = tStmt.m_sIndex;

	return true;
}


static bool ParseIndexId ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	if ( !ParseIndex ( tRoot, tStmt, sError ) )
		return false;

	JsonObj_c tId = tRoot.GetIntItem ( "id", sError );
	if ( !tId )
		return false;

	tDocId = tId.IntVal();

	return true;
}


QueryParser_i * sphCreateJsonQueryParser()
{
	return new QueryParserJson_c;
}


bool ParseJsonQueryFilters ( const JsonObj_c & tJson, CSphQuery & tQuery, CSphString & sError, CSphString & sWarning )
{
	if ( tJson && !tJson.IsObj() )
	{	
		sError = "\"query\" property value should be an object";
		return false;
	}

	CSphQueryItem & tItem = tQuery.m_dItems.Add();
	tItem.m_sExpr = "*";
	tItem.m_sAlias = "*";
	tQuery.m_sSelect = "*";

	// we need to know if the query is fullscan before re-parsing it to build AST tree
	// so we need to do some preprocessing here
	bool bFullscan = !tJson || ( tJson.Size()==1 && tJson.HasItem("match_all") );

	if ( !bFullscan )
		tQuery.m_sQuery = tJson.AsString();

	// because of the way sphinxql parsing was implemented
	// we need to parse our query and extract filters now
	// and parse the rest of the query later
	if ( tJson )
	{
		if ( !ConstructFilters ( tJson, tQuery, sError, sWarning ) )
			return false;
	}

	return true;
}


static bool ParseLimits ( const JsonObj_c & tRoot, CSphQuery & tQuery, CSphString & sError )
{
	JsonObj_c tLimit = tRoot.GetIntItem ( "limit", "size", sError );
	if ( !sError.IsEmpty() )
		return false;

	if ( tLimit )
		tQuery.m_iLimit = tLimit.IntVal();

	JsonObj_c tOffset = tRoot.GetIntItem ( "offset", "from", sError );
	if ( !sError.IsEmpty() )
		return false;

	if ( tOffset )
		tQuery.m_iOffset = tOffset.IntVal();

	return true;
}


bool sphParseJsonQuery ( const char * szQuery, CSphQuery & tQuery, bool & bProfile, bool & bAttrsHighlight,
	CSphString & sError, CSphString & sWarning )
{
	JsonObj_c tRoot ( szQuery );
	if ( !tRoot )
	{
		sError.SetSprintf ( "unable to parse: %s", tRoot.GetErrorPtr() );
		return false;
	}

	tQuery.m_sRawQuery = szQuery;

	JsonObj_c tIndex = tRoot.GetStrItem ( "index", sError );
	if ( !tIndex )
		return false;

	tQuery.m_sIndexes = tIndex.StrVal();
	tQuery.m_sIndexes.ToLower();

	if ( tQuery.m_sIndexes==g_szAll )
		tQuery.m_sIndexes = "*";

	if ( !ParseLimits ( tRoot, tQuery, sError ) )
		return false;

	JsonObj_c tJsonQuery = tRoot.GetItem("query");

	// common code used by search queries and update/delete by query
	if ( !ParseJsonQueryFilters ( tJsonQuery, tQuery, sError, sWarning ) )
		return false;

	bProfile = false;
	if ( !tRoot.FetchBoolItem ( bProfile, "profile", sError, true ) )
		return false;

	// expression columns go first to select list
	JsonObj_c tExpr = tRoot.GetItem ( "script_fields" );
	if ( tExpr && !ParseExpr ( tExpr, tQuery, sError ) )
		return false;

	JsonObj_c tSnip = tRoot.GetObjItem ( "highlight", sError, true );
	if ( tSnip )
	{
		if ( !ParseSnippet ( tSnip, tQuery, sError ) )
			return false;
		else
			bAttrsHighlight = true;
	} else if ( !sError.IsEmpty() )
		return false;

	JsonObj_c tSort = tRoot.GetItem("sort");
	if ( tSort && !( tSort.IsArray() || tSort.IsObj() ) )
	{
		sError = "\"sort\" property value should be an array or an object";
		return false;
	}

	if ( tSort )
	{
		bool bGotWeight = false;
		if ( !ParseSort ( tSort, tQuery, bGotWeight, sError, sWarning ) )
			return false;

		JsonObj_c tTrackScore = tRoot.GetBoolItem ( "track_scores", sError, true );
		if ( !sError.IsEmpty() )
			return false;

		bool bTrackScore = tTrackScore && tTrackScore.BoolVal();
		if ( !bGotWeight && !bTrackScore )
			tQuery.m_eRanker = SPH_RANK_NONE;
	}

	// source \ select filter
	JsonObj_c tSelect = tRoot.GetItem("_source");
	if ( tSelect && !ParseSelect ( tSelect, tQuery, sError ) )
		return false;

	return true;
}


bool ParseJsonInsert ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, DocID_t & tDocId, bool bReplace, CSphString & sError )
{
	tStmt.m_eStmt = bReplace ? STMT_REPLACE : STMT_INSERT;

	if ( !ParseIndexId ( tRoot, tStmt, tDocId, sError ) )
		return false;

	tStmt.m_dInsertSchema.Add ( sphGetDocidName() );
	SqlInsert_t & tId = tStmt.m_dInsertValues.Add();
	tId.m_iType = sphGetTokTypeInt();
	tId.m_iVal = tDocId;

	// "doc" is optional
	JsonObj_c tSource = tRoot.GetItem("doc");
	if ( tSource )
	{
		for ( const auto & tItem : tSource )
		{
			tStmt.m_dInsertSchema.Add ( tItem.Name() );
			tStmt.m_dInsertSchema.Last().ToLower();

			SqlInsert_t & tNewValue = tStmt.m_dInsertValues.Add();
			if ( tItem.IsStr() )
			{
				tNewValue.m_iType = sphGetTokTypeStr();
				tNewValue.m_sVal = tItem.StrVal();
			} else if ( tItem.IsDbl() )
			{
				tNewValue.m_iType = sphGetTokTypeFloat();
				tNewValue.m_fVal = tItem.FltVal();
			} else if ( tItem.IsInt() || tItem.IsBool() )
			{
				tNewValue.m_iType = sphGetTokTypeInt();
				tNewValue.m_iVal = tItem.IntVal();
			} else if ( tItem.IsArray() )
			{
				tNewValue.m_iType = sphGetTokTypeConstMVA();
				tNewValue.m_pVals = new RefcountedVector_c<SphAttr_t>;

				for ( const auto & tArrayItem : tItem )
				{
					if ( !tArrayItem.IsInt() )
					{
						sError = "MVA elements should be integers";
						return false;
					}

					tNewValue.m_pVals->Add ( tArrayItem.IntVal() );
				}
			} else if ( tItem.IsObj() )
			{
				tNewValue.m_iType = sphGetTokTypeStr();
				tNewValue.m_sVal = tItem.AsString();
			} else
			{
				sError = "unsupported value type";
				return false;
			}
		}
	}

	if ( !tStmt.CheckInsertIntegrity() )
	{
		sError = "wrong number of values";
		return false;
	}

	return true;
}


bool sphParseJsonInsert ( const char * szInsert, SqlStmt_t & tStmt, DocID_t & tDocId, bool bReplace, CSphString & sError )
{
	JsonObj_c tRoot ( szInsert );
	return ParseJsonInsert ( tRoot, tStmt, tDocId, bReplace, sError );
}


static bool ParseUpdateDeleteQueries ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{	
	tStmt.m_tQuery.m_sSelect = "*";
	if ( !ParseIndex ( tRoot, tStmt, sError ) )
		return false;

	JsonObj_c tId = tRoot.GetIntItem ( "id", sError );
	if ( tId )
	{
		CSphFilterSettings & tFilter = tStmt.m_tQuery.m_dFilters.Add();
		tFilter.m_eType = SPH_FILTER_VALUES;
		tFilter.m_dValues.Add ( tId.IntVal() );
		tFilter.m_sAttrName = "@id";

		tDocId = tId.IntVal();
	}

	// "query" is optional
	JsonObj_c tQuery = tRoot.GetItem("query");
	if ( tQuery && tId )
	{
		sError = R"(both "id" and "query" specified)";
		return false;
	}

	CSphString sWarning; // fixme: add to results
	if ( !ParseJsonQueryFilters ( tQuery, tStmt.m_tQuery, sError, sWarning ) )
		return false;

	return true;
}


static bool ParseJsonUpdate ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	tStmt.m_eStmt = STMT_UPDATE;
	tStmt.m_tUpdate.m_dRowOffset.Add ( 0 );

	if ( !ParseUpdateDeleteQueries ( tRoot, tStmt, tDocId, sError ) )
		return false;

	JsonObj_c tSource = tRoot.GetObjItem ( "doc", sError );
	if ( !tSource )
		return false;

	for ( const auto & tItem : tSource )
	{
		bool bFloat = tItem.IsNum();
		bool bInt = tItem.IsInt();
		bool bBool = tItem.IsBool();
		bool bString = tItem.IsStr();

		if ( !bFloat && !bInt && !bBool && !bString )
		{
			sError = "unsupported value type";
			return false;
		}

		CSphAttrUpdate & tUpd = tStmt.m_tUpdate;
		CSphString sAttr = tItem.Name();
		TypedAttribute_t & tTypedAttr = tUpd.m_dAttributes.Add();
		tTypedAttr.m_sName = sAttr.ToLower();

		if ( bInt || bBool )
		{
			int64_t iValue = tItem.IntVal();

			tUpd.m_dPool.Add ( (DWORD)iValue );
			DWORD uHi = (DWORD)( iValue>>32 );

			if ( uHi )
			{
				tUpd.m_dPool.Add ( uHi );
				tTypedAttr.m_eType = SPH_ATTR_BIGINT;
			} else
				tTypedAttr.m_eType = SPH_ATTR_INTEGER;
		}
		else if ( bFloat )
		{
			auto fValue = tItem.FltVal();
			tUpd.m_dPool.Add ( sphF2DW ( fValue ) );
			tTypedAttr.m_eType = SPH_ATTR_FLOAT;
		}
		else if ( bString )
		{
			const char * szValue = tItem.SzVal();

			int iLength = strlen ( szValue );
			tUpd.m_dPool.Add ( tUpd.m_dBlobs.GetLength() );
			tUpd.m_dPool.Add ( iLength );

			if ( iLength )
			{
				BYTE * pBlob = tUpd.m_dBlobs.AddN ( iLength+2 );	// a couple of extra \0 for json parser to be happy
				memcpy ( pBlob, szValue, iLength );
				pBlob[iLength] = 0;
				pBlob[iLength+1] = 0;
			}

			tTypedAttr.m_eType = SPH_ATTR_STRING;
		}
	}

	return true;
}


bool sphParseJsonUpdate ( const char * szUpdate, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	JsonObj_c tRoot ( szUpdate );
	return ParseJsonUpdate ( tRoot, tStmt, tDocId, sError );
}


static bool ParseJsonDelete ( const JsonObj_c & tRoot, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	tStmt.m_eStmt = STMT_DELETE;
	return ParseUpdateDeleteQueries ( tRoot, tStmt, tDocId, sError );
}


bool sphParseJsonDelete ( const char * szDelete, SqlStmt_t & tStmt, DocID_t & tDocId, CSphString & sError )
{
	JsonObj_c tRoot ( szDelete );
	return ParseJsonDelete ( tRoot, tStmt, tDocId, sError );
}


bool sphParseJsonStatement ( const char * szStmt, SqlStmt_t & tStmt, CSphString & sStmt, CSphString & sQuery, DocID_t & tDocId, CSphString & sError )
{
	JsonObj_c tRoot ( szStmt );
	if ( !tRoot )
	{
		sError.SetSprintf ( "unable to parse: %s", tRoot.GetErrorPtr() );
		return false;
	}

	JsonObj_c tJsonStmt = tRoot[0];
	if ( !tJsonStmt )
	{
		sError = "no statement found";
		return false;
	}

	sStmt = tJsonStmt.Name();

	if ( !tJsonStmt.IsObj() )
	{
		sError.SetSprintf ( "statement %s should be an object", sStmt.cstr() );
		return false;
	}

	if ( sStmt=="index" || sStmt=="replace" )
	{
		if ( !ParseJsonInsert ( tJsonStmt, tStmt, tDocId, true, sError ) )
			return false;
	}  else if ( sStmt=="create" || sStmt=="insert" )
	{
		if ( !ParseJsonInsert ( tJsonStmt, tStmt, tDocId, false, sError ) )
			return false;
	} else if ( sStmt=="update" )
	{
		if ( !ParseJsonUpdate ( tJsonStmt, tStmt, tDocId, sError ) )
			return false;
	} else if ( sStmt=="delete" )
	{
		if ( !ParseJsonDelete ( tJsonStmt, tStmt, tDocId, sError ) )
			return false;
	} else
	{
		sError.SetSprintf ( "unknown bulk operation: %s", sStmt.cstr() );
		return false;
	}

	sQuery = tJsonStmt.AsString();

	return true;
}


//////////////////////////////////////////////////////////////////////////
static void PackedShortMVA2Json ( StringBuilder_c& tOut, const BYTE * pMVA )
{
	int iLengthBytes = sphUnpackPtrAttr ( pMVA, &pMVA );
	int nValues = iLengthBytes / sizeof ( DWORD );
	auto pValues = ( const DWORD * ) pMVA;
	for ( int i = 0; i<nValues; ++i )
		tOut.Sprintf ( "%u", pValues[i] );
}

static void PackedWideMVA2Json ( StringBuilder_c &tOut, const BYTE * pMVA )
{
	int iLengthBytes = sphUnpackPtrAttr ( pMVA, &pMVA );
	int nValues = iLengthBytes / sizeof ( int64_t );
	auto pValues = ( const int64_t * ) pMVA;
	for ( int i = 0; i<nValues; ++i )
		tOut.Sprintf ( "%l", pValues[i] );
}

static void JsonObjAddAttr ( JsonEscapedBuilder & tOut, const AggrResult_t &tRes, ESphAttr eAttrType, const char * szCol,
	const CSphMatch &tMatch, const CSphAttrLocator &tLoc )
{
	assert ( sphPlainAttrToPtrAttr ( eAttrType )==eAttrType );
	tOut.AppendName(szCol);

	switch ( eAttrType )
	{
	case SPH_ATTR_INTEGER:
	case SPH_ATTR_TIMESTAMP:
	case SPH_ATTR_TOKENCOUNT:
	case SPH_ATTR_BIGINT: tOut.Sprintf( "%l", tMatch.GetAttr ( tLoc ) );
		break;

	case SPH_ATTR_FLOAT: tOut.Sprintf ( "%f", tMatch.GetAttrFloat ( tLoc ) );
		break;

	case SPH_ATTR_BOOL: tOut << ( tMatch.GetAttr ( tLoc ) ? "true" : "false" );
		break;

	case SPH_ATTR_UINT32SET_PTR:
	case SPH_ATTR_INT64SET_PTR:
	{
		tOut.StartBlock( ",", "[", "]" );
		const BYTE * pMVA = ( const BYTE * ) tMatch.GetAttr ( tLoc );
		if ( eAttrType==SPH_ATTR_UINT32SET_PTR )
			PackedShortMVA2Json ( tOut, pMVA );
		else
			PackedWideMVA2Json ( tOut, pMVA );
		tOut.FinishBlock(false);
	}
	break;

	case SPH_ATTR_STRINGPTR:
	{
		const auto * pString = ( const BYTE * ) tMatch.GetAttr ( tLoc );
		int iLen = sphUnpackPtrAttr ( pString, &pString );

		// special process for legacy typed strings
		if ( pString && iLen>1 && pString[iLen-2]=='\0')
		{
			auto uSubtype = pString[iLen-1];
			iLen -= 2;
			switch ( uSubtype)
			{
				case 1: // ql
				{
					ScopedComma_c sBrackets ( tOut, nullptr, R"({"ql":)", "}" );
					tOut.AppendEscaped (( const char* ) pString, EscBld::eEscape, iLen );
					break;
				}
				case 0: // json
					tOut << ( const char* ) pString;
					break;

				default:
					tOut.Sprintf ("\"internal error! wrong subtype of stringptr %d\"", uSubtype );
			}
			break;
		}
		tOut.AppendEscaped ( ( const char * ) pString, EscBld::eEscape, iLen );
	}
	break;

	case SPH_ATTR_JSON_PTR:
	{
		const BYTE * pJSON = ( const BYTE * ) tMatch.GetAttr ( tLoc );
		sphUnpackPtrAttr ( pJSON, &pJSON );

		// no object at all? return NULL
		if ( !pJSON )
		{
			tOut << "null";
			break;
		}
		sphJsonFormat ( tOut, pJSON );
	}
	break;

	case SPH_ATTR_FACTORS:
	case SPH_ATTR_FACTORS_JSON:
	{
		const BYTE * pFactors = ( const BYTE * ) tMatch.GetAttr ( tLoc );
		sphUnpackPtrAttr ( pFactors, &pFactors );
		if ( pFactors )
			sphFormatFactors ( tOut, ( const unsigned int * ) pFactors, true );
		else
			tOut << "null";
	}
	break;

	case SPH_ATTR_JSON_FIELD_PTR:
	{
		const BYTE * pField = ( const BYTE * ) tMatch.GetAttr ( tLoc );
		sphUnpackPtrAttr ( pField, &pField );
		if ( !pField )
		{
			tOut << "null";
			break;
		}

		auto eJson = ESphJsonType ( *pField++ );
		if ( eJson==JSON_NULL )
			tOut << "null";
		else
			sphJsonFieldFormat ( tOut, pField, eJson, true );
	}
	break;

	default: assert ( 0 && "Unknown attribute" );
		break;
	}
}

static void UnpackSnippets ( JsonEscapedBuilder &tOut, const CSphMatch &tMatch, const CSphAttrLocator &tLoc );

static bool IsHighlightAttr ( const CSphString & sName )
{
	return sName.Begins ( g_sHighlight );
}


static bool NeedToSkipAttr ( const CSphString & sName, const CSphQuery & tQuery )
{
	const char * szName = sName.cstr();

	if ( szName[0]=='i' && szName[1]=='d' && szName[2]=='\0' ) return true;
	if ( sName.Begins ( g_sHighlight ) ) return true;
	if ( sName.Begins ( g_szFilter ) ) return true;
	if ( sName.Begins ( g_sOrder ) ) return true;

	if ( !tQuery.m_dIncludeItems.GetLength() && !tQuery.m_dExcludeItems.GetLength () )
		return false;

	// empty include - shows all select list items
	// exclude with only "*" - skip all select list items
	bool bInclude = ( tQuery.m_dIncludeItems.GetLength()==0 );
	for ( const auto &iItem: tQuery.m_dIncludeItems )
	{
		if ( sphWildcardMatch ( szName, iItem.cstr() ) )
		{
			bInclude = true;
			break;
		}
	}
	if ( bInclude && tQuery.m_dExcludeItems.GetLength() )
	{
		for ( const auto& iItem: tQuery.m_dExcludeItems )
		{
			if ( sphWildcardMatch ( szName, iItem.cstr() ) )
			{
				bInclude = false;
				break;
			}
		}
	}

	return !bInclude;
}


CSphString sphEncodeResultJson ( const AggrResult_t & tRes, const CSphQuery & tQuery,
	CSphQueryProfile * pProfile, bool bAttrsHighlight )
{
	JsonEscapedBuilder tOut;
	CSphString sResult;

	if ( !tRes.m_iSuccesses )
	{
		tOut.StartBlock ( nullptr, R"({"error":{"type":"Error","reason":)", "}}" );
		tOut.AppendEscaped ( tRes.m_sError.cstr (), EscBld::eEscape );
		tOut.FinishBlock (false);
		tOut.MoveTo (sResult); // since simple return tOut.cstr() will cause copy of string, then returning it.
		return sResult;
	}

	tOut.StartBlock( ",", "{", "}" );

	tOut.Sprintf (R"("took":%d,"timed_out":false)", tRes.m_iQueryTime);
	if ( !tRes.m_sWarning.IsEmpty() )
	{
		tOut.StartBlock ( nullptr, R"("warning":{"reason":)", "}" );
		tOut.AppendEscaped ( tRes.m_sWarning.cstr (), EscBld::eEscape );
		tOut.FinishBlock ( false );
	}

	auto sHitMeta = tOut.StartBlock ( ",", R"("hits":{)", "}" );

	tOut.Sprintf ( R"("total":%d)", tRes.m_iTotalMatches );

	const ISphSchema & tSchema = tRes.m_tSchema;
	CSphVector<BYTE> dTmp;

	CSphBitvec tAttrsToSend;
	sphGetAttrsToSend ( tSchema, false, true, tAttrsToSend );

	int nSchemaAttrs = tSchema.GetAttrsCount();
	CSphBitvec dHiAttrs ( nSchemaAttrs );
	CSphBitvec dSkipAttrs ( nSchemaAttrs );
	for ( int iAttr=0; iAttr<nSchemaAttrs; iAttr++ )
	{
		if ( !tAttrsToSend.BitGet(iAttr) )
			continue;

		const CSphColumnInfo & tCol = tSchema.GetAttr(iAttr);
		const char * sName = tCol.m_sName.cstr();

		if ( bAttrsHighlight && IsHighlightAttr ( sName ) )
			dHiAttrs.BitSet ( iAttr );

		if ( NeedToSkipAttr ( sName, tQuery ) )
			dSkipAttrs.BitSet ( iAttr );
	}

	tOut.StartBlock ( ",", R"("hits":[)", "]" );

	const CSphColumnInfo * pId = tSchema.GetAttr ( sphGetDocidName() );		
	assert(pId);

	for ( int iMatch=tRes.m_iOffset; iMatch<tRes.m_iOffset+tRes.m_iCount; ++iMatch )
	{
		const CSphMatch & tMatch = tRes.m_dMatches[iMatch];

		ScopedComma_c sQueryComma ( tOut, ",", "{", "}" );

		// note, that originally there is string UID, so we just output number in quotes for docid here
		DocID_t tDocID = tMatch.GetAttr ( pId->m_tLocator );
		tOut.Sprintf ( R"("_id":"%l","_score":%d)", tDocID, tMatch.m_iWeight );
		tOut.StartBlock ( ",", "\"_source\":{", "}");

		for ( int iAttr=0; iAttr<nSchemaAttrs; iAttr++ )
		{
			if ( !tAttrsToSend.BitGet(iAttr) )
				continue;

			if ( dSkipAttrs.BitGet ( iAttr ) )
				continue;

			const CSphColumnInfo & tCol = tSchema.GetAttr(iAttr);
			const char * sName = tCol.m_sName.cstr();

			JsonObjAddAttr ( tOut, tRes, tCol.m_eAttrType, sName, tMatch, tCol.m_tLocator );
		}

		tOut.FinishBlock ( false ); // _source obj

		if ( bAttrsHighlight )
		{
			ScopedComma_c sHighlightComma ( tOut, ",", R"("highlight":{)", "}" );

			for ( int iAttr=0; iAttr<nSchemaAttrs; iAttr++ )
			{
				if ( !tAttrsToSend.BitGet(iAttr) )
					continue;

				if ( !dHiAttrs.BitGet ( iAttr ) )
					continue;


				const CSphColumnInfo & tCol = tSchema.GetAttr(iAttr);
				const char * sName = tCol.m_sName.cstr() + sizeof(g_sHighlight) - 1;
				assert ( tCol.m_eAttrType==SPH_ATTR_STRINGPTR );

				tOut.AppendName (sName);
				ScopedComma_c sHighlight ( tOut, ",", "[", "]" );
				UnpackSnippets ( tOut, tMatch, tCol.m_tLocator );
			}
		}
	}

	tOut.FinishBlocks ( sHitMeta, false ); // hits array, hits meta

	if ( pProfile )
	{
		const char * sProfileResult = pProfile->GetResultAsStr();
		// FIXME: result can be empty if we run a fullscan
		if ( sProfileResult && strlen ( sProfileResult ) )
			tOut.Sprintf ( R"("profile":{"query":%s})", sProfileResult );
		else
			tOut << R"("profile":null)";
	}

	tOut.FinishBlocks (); tOut.MoveTo ( sResult ); return sResult;
}


JsonObj_c sphEncodeInsertResultJson ( const char * szIndex, bool bReplace, DocID_t tDocId )
{
	JsonObj_c tObj;

	tObj.AddStr ( "_index", szIndex );
	tObj.AddInt ( "_id", tDocId );
	tObj.AddBool ( "created", !bReplace );
	tObj.AddStr ( "result", bReplace ? "updated" : "created" );
	tObj.AddInt ( "status", bReplace ? 200 : 201 );

	return tObj;
}


JsonObj_c sphEncodeUpdateResultJson ( const char * szIndex, DocID_t tDocId, int iAffected )
{
	JsonObj_c tObj;

	tObj.AddStr ( "_index", szIndex );

	if ( !tDocId )
		tObj.AddInt ( "updated", iAffected );
	else
	{
		tObj.AddInt ( "_id", tDocId );
		tObj.AddStr ( "result", iAffected ? "updated" : "noop" );
	}

	return tObj;
}


JsonObj_c sphEncodeDeleteResultJson ( const char * szIndex, DocID_t tDocId, int iAffected )
{
	JsonObj_c tObj;

	tObj.AddStr ( "_index", szIndex );

	if ( !tDocId )
		tObj.AddInt ( "deleted", iAffected );
	else
	{
		tObj.AddInt ( "_id", tDocId );
		tObj.AddBool ( "found", !!iAffected );
		tObj.AddStr ( "result", iAffected ? "deleted" : "not found" );
	}

	return tObj;
}


JsonObj_c sphEncodeInsertErrorJson ( const char * szIndex, const char * szError )
{
	JsonObj_c tObj, tErr;

	tErr.AddStr ( "type", szError );
	tErr.AddStr ( "index", szIndex );

	tObj.AddItem ( "error", tErr );
	tObj.AddInt ( "status", 500 );

	return tObj;
}


bool sphGetResultStats ( const char * szResult, int & iAffected, int & iWarnings, bool bUpdate )
{
	JsonObj_c tJsonRoot ( szResult );
	if ( !tJsonRoot )
		return false;

	// no warnings in json results for now
	iWarnings = 0;

	if ( tJsonRoot.HasItem("error") )
	{
		iAffected = 0;
		return true;
	}

	// its either update or delete
	CSphString sError;
	JsonObj_c tAffected = tJsonRoot.GetIntItem ( bUpdate ? "updated" : "deleted", sError );
	if ( tAffected )
	{
		iAffected = tAffected.IntVal();
		return true;
	}

	// it was probably a query with an "_id"
	JsonObj_c tId = tJsonRoot.GetIntItem ( "_id", sError );
	if ( tId )
	{
		iAffected = 1;
		return true;
	}

	return false;
}


void AddAccessSpecs ( JsonEscapedBuilder &tOut, const XQNode_t * pNode, const CSphSchema &tSchema, const StrVec_t &dZones )
{
	assert ( pNode );

	// dump spec for keyword nodes
	// FIXME? double check that spec does *not* affect non keyword nodes
	if ( pNode->m_dSpec.IsEmpty () || !pNode->m_dWords.GetLength () )
		return;

	const XQLimitSpec_t &tSpec = pNode->m_dSpec;
	if ( tSpec.m_bFieldSpec && !tSpec.m_dFieldMask.TestAll ( true ) )
	{
		ScopedComma_c sFieldsArray ( tOut, ",", "\"fields\":[", "]" );
		for ( int i = 0; i<tSchema.GetFieldsCount (); ++i )
			if ( tSpec.m_dFieldMask.Test ( i ) )
				tOut.AppendEscaped ( tSchema.GetFieldName ( i ), EscBld::eEscape );
	}
	tOut.Sprintf ( "\"max_field_pos\":%d", tSpec.m_iFieldMaxPos );

	if ( !tSpec.m_dZones.IsEmpty () )
	{
		ScopedComma_c sZoneDelim ( tOut, ",", tSpec.m_bZoneSpan ? "\"zonespans\":[" : "\"zones\":[", "]" );
		for ( int iZone : tSpec.m_dZones )
			tOut.AppendEscaped ( dZones[iZone].cstr(), EscBld::eEscape );
	}
}

void CreateKeywordNode ( JsonEscapedBuilder & tOut, const XQKeyword_t &tKeyword )
{
	ScopedComma_c sRoot ( tOut, ",", "{", "}");
	tOut << R"("type":"KEYWORD")";
	tOut << "\"word\":"; tOut.AppendEscaped ( tKeyword.m_sWord.cstr (), EscBld::eEscape | EscBld::eSkipComma );
	tOut.Sprintf ( R"("querypos":%d)", tKeyword.m_iAtomPos);

	if ( tKeyword.m_bExcluded )
		tOut << R"("excluded":true)";

	if ( tKeyword.m_bExpanded )
		tOut << R"("expanded":true)";

	if ( tKeyword.m_bFieldStart )
		tOut << R"("field_start":true)";

	if ( tKeyword.m_bFieldEnd )
		tOut << R"("field_end":true)";

	if ( tKeyword.m_bMorphed )
		tOut << R"("morphed":true)";

	if ( tKeyword.m_fBoost!=1.0f ) // really comparing floats?
		tOut.Sprintf ( R"("boost":%f)", tKeyword.m_fBoost) ;
}

void sphBuildProfileJson ( JsonEscapedBuilder &tOut, const XQNode_t * pNode, const CSphSchema &tSchema, const StrVec_t &dZones )
{
	assert ( pNode );
	auto dRootBlock = tOut.StartBlock ( ",", "{", "}" );

	CSphString sNodeName ( sphXQNodeToStr ( pNode ) );
	tOut << "\"type\":"; tOut.AppendEscaped ( sNodeName.cstr (), EscBld::eEscape | EscBld::eSkipComma );

	CSphString sDescription ( sphExplainQueryBrief ( pNode, tSchema ) );
	tOut << "\"description\":"; tOut.AppendEscaped ( sDescription.cstr (), EscBld::eEscape | EscBld::eSkipComma );

	CSphString sNodeOptions ( sphXQNodeGetExtraStr ( pNode ) );
	if ( !sNodeOptions.IsEmpty () )
	{
		tOut << "\"options\":"; tOut.AppendEscaped ( sNodeOptions.cstr (), EscBld::eEscape | EscBld::eSkipComma );
	}

	AddAccessSpecs ( tOut, pNode, tSchema, dZones );

	tOut.StartBlock ( ",", "\"children\":[", "]" );
	if ( pNode->m_dChildren.GetLength () )
	{
		for ( const auto& i : pNode->m_dChildren )
			sphBuildProfileJson ( tOut, i, tSchema, dZones );
	} else
	{
		for ( const auto& i : pNode->m_dWords )
			CreateKeywordNode ( tOut, i );
	}
	tOut.FinishBlocks ( dRootBlock );
}

//////////////////////////////////////////////////////////////////////////
// Highlight

struct HttpSnippetField_t
{
	int m_iFragmentSize = -1;
	int m_iFragmentCount = -1;
	CSphString m_sName;
};

static bool CheckField ( HttpSnippetField_t & tParsed, CSphString & sError, const JsonObj_c & tField )
{
	assert ( tField.IsObj() );
	if ( !tField.Size() )
		return true;

	JsonObj_c tType = tField.GetStrItem ( "type", sError, true );
	if ( tType )
	{
		if ( tType.StrVal()!="unified" )
		{
			sError = R"(only "unified" supported for "type" property)";
			return false;
		}
	} else if ( !sError.IsEmpty() )
		return false;

	if ( tField.HasItem ( "force_source" ) )
	{
		sError = R"("force_source" property not supported)";
		return false;
	}

	JsonObj_c tFragmenter = tField.GetStrItem ( "fragmenter", sError, true );
	if ( tFragmenter )
	{
		if ( tFragmenter.StrVal()!="span" )
		{
			sError = R"(only "span" supported for "fragmenter" property)";
			return false;
		}
	} else if ( !sError.IsEmpty() )
		return false;

	if ( !tField.FetchIntItem ( tParsed.m_iFragmentSize, "fragment_size", sError, true ) )
		return false;

	if ( !tField.FetchIntItem ( tParsed.m_iFragmentCount, "number_of_fragments", sError, true ) )
		return false;

	return true;
}


struct SnippetOptions_t
{
	int			m_iNoMatch {0};
	bool		m_bWeightOrder {false};
	bool		m_bKeepHtml {false};
	CSphString	m_sQuery;
	CSphString	m_sPreTag;
	CSphString	m_sPostTag;
	CSphVector<HttpSnippetField_t> m_dFields;
};


static void FormatSnippetOpts ( const SnippetOptions_t & tOpts, CSphQuery & tQuery )
{
	for ( const HttpSnippetField_t & tSnip : tOpts.m_dFields )
	{
		StringBuilder_c sItem;
		const char * sHiQuery = ( tOpts.m_sQuery.IsEmpty() ? tQuery.m_sQuery.cstr() : tOpts.m_sQuery.cstr() );
		sItem << "SNIPPET(" << tSnip.m_sName << ", '" << sHiQuery << "'";

		if ( !tOpts.m_sPreTag.IsEmpty() )
			sItem << ", 'before_match=" << tOpts.m_sPreTag << "'";
		if ( !tOpts.m_sPostTag.IsEmpty() )
			sItem << ", 'after_match=" << tOpts.m_sPostTag << "'";
		if ( tSnip.m_iFragmentSize!=-1 && !tOpts.m_bKeepHtml )
			sItem.Appendf ( ", 'limit=%d'", tSnip.m_iFragmentSize );
		if ( tSnip.m_iFragmentCount!=-1 && !tOpts.m_bKeepHtml )
			sItem.Appendf ( ", 'limit_passages=%d'", tSnip.m_iFragmentCount );
		if ( tOpts.m_iNoMatch<1 )
			sItem << ", 'allow_empty=1'";
		if ( tOpts.m_bWeightOrder )
			sItem << ", 'weight_order=1'";
		if ( tOpts.m_bKeepHtml )
			sItem << ", 'html_strip_mode=retain', 'limit=0'";

		sItem += ", 'json_query=1')";

		CSphQueryItem & tItem = tQuery.m_dItems.Add();
		tItem.m_sExpr = sItem.cstr ();
		tItem.m_sAlias.SetSprintf ( "%s%s", g_sHighlight, tSnip.m_sName.cstr() );
	}
}


static bool ParseSnippet ( const JsonObj_c & tSnip, CSphQuery & tQuery, CSphString & sError )
{
	const char * dUnsupported[] = { "tags_schema", "require_field_match", "boundary_scanner", "max_fragment_length" };
	for ( auto szOption : dUnsupported )
		if ( tSnip.HasItem(szOption) )
		{
			sError.SetSprintf ( R"("%s" property not supported)", szOption );
			return false;
		}

	JsonObj_c tFields = tSnip.GetObjItem ( "fields", sError, true );
	if ( !tFields && !sError.IsEmpty() )
		return false;

	SnippetOptions_t tOpts;

	JsonObj_c tEncoder = tSnip.GetStrItem ( "encoder", sError, true );
	if ( tEncoder )
		tOpts.m_bKeepHtml = tEncoder.StrVal()=="html";
	else if ( !sError.IsEmpty() )
		return false;

	JsonObj_c tHlQuery = tSnip.GetObjItem ( "highlight_query", sError, true );
	if ( tHlQuery )
		tOpts.m_sQuery = tHlQuery.AsString();
	else if ( !sError.IsEmpty() )
		return false;

	if ( !tSnip.FetchStrItem ( tOpts.m_sPreTag, "pre_tags", sError, true ) )
		return false;

	if ( !tSnip.FetchStrItem ( tOpts.m_sPostTag, "post_tags", sError, true ) )
		return false;

	if ( !tSnip.FetchIntItem ( tOpts.m_iNoMatch, "no_match_size", sError, true ) )
		return false;

	JsonObj_c tOrder = tSnip.GetStrItem ( "order", sError, true );
	if ( tOrder )
		tOpts.m_bWeightOrder = tOrder.StrVal()=="score";
	else if ( !sError.IsEmpty() )
		return false;

	HttpSnippetField_t tGlobalOptions;
	if ( !CheckField ( tGlobalOptions, sError, tSnip ) )
		return false;

	tOpts.m_dFields.Reserve ( tFields.Size() );

	for ( const auto & tField : tFields )
	{
		if ( !tField.IsObj() )
		{
			sError.SetSprintf ( "\"%s\" field should be an object", tField.Name() );
			return false;
		}

		HttpSnippetField_t & tSnippetField = tOpts.m_dFields.Add();
		tSnippetField.m_sName = tField.Name();
		if ( !CheckField ( tSnippetField, sError, tField ) )
			return false;

		if ( tGlobalOptions.m_iFragmentSize!=-1 )
			tSnippetField.m_iFragmentSize = tGlobalOptions.m_iFragmentSize;
		if ( tGlobalOptions.m_iFragmentCount!=-1 )
			tSnippetField.m_iFragmentCount = tGlobalOptions.m_iFragmentCount;
	}

	FormatSnippetOpts ( tOpts, tQuery );

	return true;
}


struct PassageLocator_t
{
	int m_iOff;
	int m_iSize;
};


int PackSnippets ( const CSphVector<BYTE> & dRes, CSphVector<int> & dSeparators, int iSepLen, const BYTE ** ppStr )
{
	if ( !dSeparators.GetLength() && !dRes.GetLength() )
		return 0;

	int iLast = 0;
	CSphVector<PassageLocator_t> dPassages;
	dPassages.Reserve ( dSeparators.GetLength() );
	for ( int iCur : dSeparators )
	{
		int iFrom = iLast;
		int iLen = iCur - iFrom;
		iLast = iCur + iSepLen;
		if ( iLen<=0 )
			continue;
		PassageLocator_t & tPass = dPassages.Add();
		tPass.m_iOff = iFrom;
		tPass.m_iSize = iLen;
	}

	if ( !dPassages.GetLength() )
	{
		PassageLocator_t & tPass = dPassages.Add();
		tPass.m_iOff = 0;
		tPass.m_iSize = dRes.GetLength();
	}

	int iPassageCount = dPassages.GetLength();

	CSphVector<BYTE> dOut;
	BYTE * pData;

	pData = dOut.AddN ( sizeof(iPassageCount) );
	sphUnalignedWrite ( pData, iPassageCount );

	ARRAY_FOREACH ( iPassage, dPassages )
	{
		int iSize = dPassages[iPassage].m_iSize + 1;

		pData = dOut.AddN ( sizeof(iSize) );
		sphUnalignedWrite ( pData, iSize );
	}

	const BYTE * sText = dRes.Begin();
	for ( const auto & dPassage : dPassages )
	{
		dOut.Append( sText + dPassage.m_iOff, dPassage.m_iSize );
		dOut.Add('\0'); // make sz-string from binary
	}

	int iTotalSize = dOut.GetLength();
	*ppStr = dOut.LeakData();
	return iTotalSize;
}

static void UnpackSnippets ( JsonEscapedBuilder &tOut, const CSphMatch &tMatch, const CSphAttrLocator &tLoc )
{
	auto pData = ( const BYTE * ) tMatch.GetAttr ( tLoc );
	sphUnpackPtrAttr ( pData, &pData );
	if ( !pData )
		return;

	int iPassageCount = sphUnalignedRead ( *( int * ) pData );
	pData += sizeof ( iPassageCount );

	auto pSize = ( const int * ) pData;
	auto pText = ( const char * ) ( pData + sizeof ( iPassageCount ) * iPassageCount );
	int iTextOff = 0;
	for ( int i = 0; i<iPassageCount; ++i )
	{
		const char * pPassage = pText + iTextOff;
		tOut.AppendEscaped ( pPassage, EscBld::eEscape );
		iTextOff += sphUnalignedRead ( *pSize );
		++pSize;
	}
}

//////////////////////////////////////////////////////////////////////////
// Sort
struct SortField_t : public GeoDistInfo_c
{
	CSphString m_sName;
	CSphString m_sMode;
	bool m_bAsc {true};
};


static void FormatSortBy ( const CSphVector<SortField_t> & dSort, CSphQuery & tQuery, bool & bGotWeight )
{
	StringBuilder_c sSortBuf;
	Comma_c sComma;

	for ( const SortField_t &tItem : dSort )
	{
		const char * sSort = ( tItem.m_bAsc ? " asc" : " desc" );
		if ( tItem.IsGeoDist() )
		{
			// ORDER BY statement
			sSortBuf << sComma << g_sOrder << tItem.m_sName << sSort;

			// query item
			CSphQueryItem & tQueryItem = tQuery.m_dItems.Add();
			tQueryItem.m_sExpr = tItem.BuildExprString();
			tQueryItem.m_sAlias.SetSprintf ( "%s%s", g_sOrder, tItem.m_sName.cstr() );

			// select list
			StringBuilder_c sTmp;
			sTmp << tQuery.m_sSelect << ", " << tQueryItem.m_sExpr << " as " << tQueryItem.m_sAlias;
			sTmp.MoveTo ( tQuery.m_sSelect );
		} else if ( tItem.m_sMode.IsEmpty() )
		{
			// sort by attribute or weight
			sSortBuf << sComma << ( tItem.m_sName=="_score" ? "@weight" : tItem.m_sName ) << sSort;
			bGotWeight |= ( tItem.m_sName=="_score" );
		} else
		{
			// sort by MVA
			// ORDER BY statement
			sSortBuf << sComma << g_sOrder << tItem.m_sName << sSort;

			// query item
			StringBuilder_c sTmp;
			sTmp << ( tItem.m_sMode=="min" ? "least" : "greatest" ) << "(" << tItem.m_sName << ")";
			CSphQueryItem & tQueryItem = tQuery.m_dItems.Add();
			sTmp.MoveTo (tQueryItem.m_sExpr);
			tQueryItem.m_sAlias.SetSprintf ( "%s%s", g_sOrder, tItem.m_sName.cstr() );

			// select list
			sTmp << tQuery.m_sSelect << ", " << tQueryItem.m_sExpr << " as " << tQueryItem.m_sAlias;
			sTmp.MoveTo ( tQuery.m_sSelect );
		}
	}

	if ( !dSort.GetLength() )
	{
		sSortBuf += "@weight desc";
		bGotWeight = true;
	}

	tQuery.m_eSort = SPH_SORT_EXTENDED;
	sSortBuf.MoveTo ( tQuery.m_sSortBy );
}


static bool ParseSort ( const JsonObj_c & tSort, CSphQuery & tQuery, bool & bGotWeight, CSphString & sError, CSphString & sWarning )
{
	bGotWeight = false;

	// unsupported options
	if ( tSort.HasItem("_script") )
	{
		sError = "\"_script\" property not supported";
		return false;
	}

	CSphVector<SortField_t> dSort;
	dSort.Reserve ( tSort.Size() );

	for ( const auto & tItem : tSort )
	{
		CSphString sName = tItem.Name();

		bool bString = tItem.IsStr();
		bool bObj = tItem.IsObj();
		if ( !bString && !bObj )
		{
			sError.SetSprintf ( R"("sort" property "%s" should be a string or an object)", sName.scstr() );
			return false;
		}

		if ( bObj && tItem.Size()!=1 )
		{
			sError.SetSprintf ( R"("sort" property "%s" should be an object)", sName.scstr() );
			return false;
		}

		// [ "attr_name" ]
		if ( bString )
		{
			SortField_t & tSortField = dSort.Add();
			tSortField.m_sName = tItem.StrVal();
			// order defaults to desc when sorting on the _score, and defaults to asc when sorting on anything else
			tSortField.m_bAsc = ( tSortField.m_sName!="_score" );
			continue;
		}

		JsonObj_c tSortItem = tItem[0];
		if ( !tSortItem )
		{
			sError = R"(invalid "sort" property item)";
			return false;
		}

		bool bSortString = tSortItem.IsStr();
		bool bSortObj = tSortItem.IsObj();

		CSphString sSortName = tSortItem.Name();
		if ( ( !bSortString && !bSortObj ) || !tSortItem.Name() || ( bSortString && !tSortItem.SzVal() ) )
		{
			sError.SetSprintf ( R"("sort" property 0("%s") should be %s)", sSortName.scstr(), ( bSortObj ? "a string" : "an object" ) );
			return false;
		}

		// [ { "attr_name" : "sort_mode" } ]
		if ( bSortString )
		{
			CSphString sOrder = tSortItem.StrVal();
			if ( sOrder!="asc" && sOrder!="desc" )
			{
				sError.SetSprintf ( R"("sort" property "%s" order is invalid %s)", sSortName.scstr(), sOrder.cstr() );
				return false;
			}

			SortField_t & tItem = dSort.Add();
			tItem.m_sName = sSortName;
			tItem.m_bAsc = ( sOrder=="asc" );
			continue;
		}

		// [ { "attr_name" : { "order" : "sort_mode" } } ]
		SortField_t & tSortField = dSort.Add();
		tSortField.m_sName = sSortName;

		JsonObj_c tAttrItems = tSortItem.GetItem("order");
		if ( tAttrItems )
		{
			if ( !tAttrItems.IsStr() )
			{
				sError.SetSprintf ( R"("sort" property "%s" order is invalid)", tAttrItems.Name() );
				return false;
			}

			CSphString sOrder = tAttrItems.StrVal();
			tSortField.m_bAsc = ( sOrder=="asc" );
		}

		JsonObj_c tMode = tSortItem.GetItem("mode");
		if ( tMode )
		{
			if ( tAttrItems && !tMode.IsStr() )
			{
				sError.SetSprintf ( R"("mode" property "%s" order is invalid)", tAttrItems.Name() );
				return false;
			}

			CSphString sMode = tMode.StrVal();
			if ( sMode!="min" && sMode!="max" )
			{
				sError.SetSprintf ( R"("mode" supported are "min" and "max", got "%s", not supported)", sMode.cstr() );
				return false;
			}

			tSortField.m_sMode = sMode;
		}

		// geodist
		if ( tSortField.m_sName=="_geo_distance" )
		{
			if ( tMode )
			{
				sError = R"("mode" property not supported with "_geo_distance")";
				return false;
			}

			if ( tSortItem.HasItem("unit") )
			{
				sError = R"("unit" property not supported with "_geo_distance")";
				return false;
			}

			if ( !tSortField.Parse ( tSortItem, false, sError, sWarning ) )
				return false;
		}

		// unsupported options
		const char * dUnsupported[] = { "unmapped_type", "missing", "nested_path", "nested_filter"};
		for ( auto szOption : dUnsupported )
			if ( tSortItem.HasItem(szOption) )
			{
				sError.SetSprintf ( R"("%s" property not supported)", szOption );
				return false;
			}
	}

	FormatSortBy ( dSort, tQuery, bGotWeight );

	return true;
}


static bool ParseLatLon ( const JsonObj_c & tLat, const JsonObj_c & tLon, LocationField_t * pField, LocationSource_t * pSource, CSphString & sError )
{
	if ( !tLat || !tLon )
	{
		if ( !tLat && !tLon )
			sError = R"("lat" and "lon" properties missing)";
		else
			sError.SetSprintf ( R"("%s" property missing)", ( !tLat ? "lat" : "lon" ) );

		return false;
	}

	bool bParseField = !!pField;
	bool bLatChecked = bParseField ? tLat.IsNum() : tLat.IsStr();
	bool bLonChecked = bParseField ? tLon.IsNum() : tLon.IsStr();
	if ( !bLatChecked || !bLonChecked )
	{
		if ( !bLatChecked && !bLonChecked )
			sError.SetSprintf ( R"("lat" and "lon" property values should be %s)", ( bParseField ? "numeric" : "string" ) );
		else
			sError.SetSprintf ( R"("%s" property value should be %s)", ( !bLatChecked ? "lat" : "lon" ), ( bParseField ? "numeric" : "string" ) );

		return false;
	}

	if ( bParseField )
	{
		pField->m_fLat = tLat.FltVal();
		pField->m_fLon = tLon.FltVal();
	} else
	{
		pSource->m_sLat = tLat.StrVal();
		pSource->m_sLon = tLon.StrVal();
	}

	return true;
}


static bool ParseLocation ( const char * sName, const JsonObj_c & tLoc, LocationField_t * pField, LocationSource_t * pSource, CSphString & sError )
{
	bool bParseField = !!pField;
	assert ( ( bParseField && pField ) || pSource );

	bool bObj = tLoc.IsObj();
	bool bString = tLoc.IsStr();
	bool bArray = tLoc.IsArray();

	if ( !bObj && !bString && !bArray )
	{
		sError.SetSprintf ( "\"%s\" property value should be either an object or a string or an array", sName );
		return false;
	}

	if ( bObj )
		return ParseLatLon ( tLoc.GetItem("lat"), tLoc.GetItem("lon"), pField, pSource, sError );

	if ( bString )
	{
		StrVec_t dVals;
		sphSplit ( dVals, tLoc.SzVal() );

		if ( dVals.GetLength()!=2 )
		{
			sError.SetSprintf ( "\"%s\" property values should be sting with lat,lon items, got %d items", sName, dVals.GetLength() );
			return false;
		}

		// string and array order differs
		// string - lat, lon
		// array - lon, lat
		int iLatLen = dVals[0].Length();
		int iLonLen = dVals[1].Length();
		if ( !iLatLen || !iLonLen )
		{
			if ( !iLatLen && !iLonLen )
				sError.SetSprintf ( R"("lat" and "lon" values should be %s)", ( bParseField ? "numeric" : "string" ) );
			else
				sError.SetSprintf ( "\"%s\" value should be %s", ( !iLatLen ? "lat" : "lon" ), ( bParseField ? "numeric" : "string" ) );

			return false;
		}

		if ( bParseField )
		{
			pField->m_fLat = (float)atof ( dVals[0].cstr() );
			pField->m_fLon = (float)atof ( dVals[1].cstr() );
		} else
		{
			pSource->m_sLat = dVals[0];
			pSource->m_sLon = dVals[1];
		}

		return true;
	}

	assert ( bArray );
	int iCount = tLoc.Size();
	if ( iCount!=2 )
	{
		sError.SetSprintf ( "\"%s\" property values should be an array with lat,lon items, got %d items", sName, iCount );
		return false;
	}

	// string and array order differs
	// array - lon, lat
	// string - lat, lon
	return ParseLatLon ( tLoc[1], tLoc[0], pField, pSource, sError );
}

//////////////////////////////////////////////////////////////////////////
// _source / select list

static bool ParseStringArray ( const JsonObj_c & tArray, const char * szProp, StrVec_t & dItems, CSphString & sError )
{
	for ( const auto & tItem : tArray )
	{
		if ( !tItem.IsStr() )
		{
			sError.SetSprintf ( R"("%s" property should be a string)", szProp );
			return false;
		}

		dItems.Add ( tItem.StrVal() );
	}

	return true;
}


static bool ParseSelect ( const JsonObj_c & tSelect, CSphQuery & tQuery, CSphString & sError )
{
	bool bString = tSelect.IsStr();
	bool bArray = tSelect.IsArray();
	bool bObj = tSelect.IsObj();

	if ( !bString && !bArray && !bObj )
	{
		sError = R"("_source" property should be a string or an array or an object)";
		return false;
	}

	if ( bString )
	{
		tQuery.m_dIncludeItems.Add ( tSelect.StrVal() );
		if ( tQuery.m_dIncludeItems[0]=="*" || tQuery.m_dIncludeItems[0].IsEmpty() )
			tQuery.m_dIncludeItems.Reset();

		return true;
	}

	if ( bArray )
		return ParseStringArray ( tSelect, R"("_source")", tQuery.m_dIncludeItems, sError );

	assert ( bObj );

	// includes part of _source object
	JsonObj_c tInclude = tSelect.GetArrayItem ( "includes", sError, true );
	if ( tInclude )
	{
		if ( !ParseStringArray ( tInclude, R"("_source" "includes")", tQuery.m_dIncludeItems, sError ) )
			return false;

		if ( tQuery.m_dIncludeItems.GetLength()==1 && tQuery.m_dIncludeItems[0]=="*" )
			tQuery.m_dIncludeItems.Reset();
	} else if ( !sError.IsEmpty() )
		return false;

	// excludes part of _source object
	JsonObj_c tExclude = tSelect.GetArrayItem ( "excludes", sError, true );
	if ( tExclude )
	{
		if ( !ParseStringArray ( tExclude, R"("_source" "excludes")", tQuery.m_dExcludeItems, sError ) )
			return false;

		if ( !tQuery.m_dExcludeItems.GetLength() )
			tQuery.m_dExcludeItems.Add ( "*" );
	} else if ( !sError.IsEmpty() )
		return false;

	return true;
}

//////////////////////////////////////////////////////////////////////////
// script_fields / expressions

static bool ParseExpr ( const JsonObj_c & tExpr, CSphQuery & tQuery, CSphString & sError )
{
	if ( !tExpr )
		return true;

	if ( !tExpr.IsObj() )
	{
		sError = R"("script_fields" property should be an object)";
		return false;
	}

	StringBuilder_c sSelect;
	sSelect << tQuery.m_sSelect;

	for ( const auto & tAlias : tExpr )
	{
		if ( !tAlias.IsObj() )
		{
			sError = R"("script_fields" properties should be objects)";
			return false;
		}

		if ( CSphString ( tAlias.Name() ).IsEmpty() )
		{
			sError = R"("script_fields" empty property name)";
			return false;
		}

		JsonObj_c tAliasScript = tAlias.GetItem("script");
		if ( !tAliasScript )
		{
			sError = R"("script_fields" property should have "script" object)";
			return false;
		}

		CSphString sExpr;
		if ( !tAliasScript.FetchStrItem ( sExpr, "inline", sError ) )
			return false;

		const char * dUnsupported[] = { "lang", "params", "stored", "file" };
		for ( auto szOption : dUnsupported )
			if ( tAliasScript.HasItem(szOption) )
			{
				sError.SetSprintf ( R"("%s" property not supported in "script_fields")", szOption );
				return false;
			}

		// add to query
		CSphQueryItem & tQueryItem = tQuery.m_dItems.Add();
		tQueryItem.m_sExpr = sExpr;
		tQueryItem.m_sAlias = tAlias.Name();

		// add to select list
		sSelect.Appendf ( ", %s as %s", tQueryItem.m_sExpr.cstr(), tQueryItem.m_sAlias.cstr() );
	}

	sSelect.MoveTo ( tQuery.m_sSelect );
	return true;
}
