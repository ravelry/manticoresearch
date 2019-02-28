//
// $Id$
//

//
// Copyright (c) 2011-2016, Andrew Aksyonoff
// Copyright (c) 2011-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxjson_
#define _sphinxjson_

#include "sphinx.h"
#include "sphinxutils.h"

/// supported JSON value types
enum ESphJsonType : BYTE
{
	JSON_EOF			= 0,
	JSON_INT32			= 1,
	JSON_INT64			= 2,
	JSON_DOUBLE			= 3,
	JSON_STRING			= 4,
	JSON_STRING_VECTOR	= 5,
	JSON_INT32_VECTOR	= 6,
	JSON_INT64_VECTOR	= 7,
	JSON_DOUBLE_VECTOR	= 8,
	JSON_MIXED_VECTOR	= 9,
	JSON_OBJECT			= 10,
	JSON_TRUE			= 11,
	JSON_FALSE			= 12,
	JSON_NULL			= 13,
	JSON_ROOT			= 14,

	JSON_TOTAL
};


#if UNALIGNED_RAM_ACCESS && USE_LITTLE_ENDIAN

template < typename NUM32 >
inline NUM32 GetNUM32LE ( const BYTE * p )
{
	return *( const NUM32 * ) p;
}

template < typename NUM32 >
inline void StoreNUM32LE ( BYTE * p, NUM32 v )
{
	*( NUM32 * ) ( p ) = v;
}

inline void StoreBigintLE ( BYTE * p, int64_t v )
{
	*( int64_t * ) ( p ) = v;
}

/// get stored value from SphinxBSON blob
inline int64_t sphJsonLoadBigint ( const BYTE ** pp )
{
	int64_t iRes = *( const int64_t * ) ( *pp );
	*pp += 8;
	return iRes;
}

#else

template < typename NUM32 >
inline NUM32 GetNUM32LE ( const BYTE * p )
{
	return p[0] + ( p[1]<<8 ) + ( p[2]<<16 ) + ( p[3]<<24 );
}

template < typename NUM32 >
inline void StoreNUM32LE ( BYTE * p, NUM32 v )
{
	p[0] = BYTE ( DWORD ( v ) );
	p[1] = BYTE ( DWORD ( v ) >> 8 );
	p[2] = BYTE ( DWORD ( v ) >> 16 );
	p[3] = BYTE ( DWORD ( v ) >> 24 );
}

inline void StoreBigintLE ( BYTE * p, int64_t v )
{
	StoreNUM32LE ( p, ( DWORD ) ( v & 0xffffffffUL ) );
	StoreNUM32LE ( p + 4, ( int ) ( v >> 32 ) );
}

/// get stored value from SphinxBSON blob
inline int64_t sphJsonLoadBigint ( const BYTE ** pp )
{
	uint64_t uRes = GetNUM32LE<DWORD> ( *pp );
	uRes += ( uint64_t ) GetNUM32LE<DWORD> ( (*pp)+4 ) << 32;
	*pp+=8;
	return uRes;
}

#endif


/// get stored value from SphinxBSON blob
inline int sphJsonLoadInt ( const BYTE ** pp )
{
	auto uRes = GetNUM32LE<int> ( *pp );
	*pp += 4;
	return uRes;
}


/// unpack value from SphinxBSON blob
/// 0..251 == value itself
/// 252 == 2 more bytes
/// 253 == 3 more bytes
/// 254 == 4 more bytes
/// 255 == reserved
inline DWORD sphJsonUnpackInt ( const BYTE ** pp )
{
	const BYTE * p = *pp;
	DWORD uRes = p[0];
	switch ( p[0] )
	{
		default:
			(*pp) += 1;
			break;
		case 252:
			uRes = p[1] + ( p[2]<<8 );
			(*pp) += 3;
			break;
		case 253:
			uRes = p[1] + ( p[2]<<8 ) + ( p[3]<<16 );
			(*pp) += 4;
			break;
		case 254:
			uRes = GetNUM32LE<DWORD> (&p[1]);
			(*pp) += 5;
			break;
		case 255:
			assert ( 0 && "unexpected reserved length code 255" );
			(*pp) += 1;
			break;
	}
	return uRes;
}

struct EscapeJsonString_t
{
	static const char cQuote = '"';
	inline static bool IsEscapeChar ( char c )
	{
		return strchr ( "\"\\\b\f\n\r\t", c )!=nullptr;
	}

	inline static char GetEscapedChar ( char c )
	{
		switch ( c )
		{
		case '\b': return 'b';
		case '\f': return 'f';
		case '\n': return 'n';
		case '\r': return 'r';
		case '\t': return 't';
		default: return c;
		}
	}
};

using JsonEscapedBuilder = EscapedStringBuilder_T<EscapeJsonString_t>;

/// parse JSON, convert it into SphinxBSON blob
bool sphJsonParse ( CSphVector<BYTE> & dData, char * sData, bool bAutoconv, bool bToLowercase, CSphString & sError );
bool sphJsonParse ( CSphVector<BYTE> & dData, char * sData, bool bAutoconv, bool bToLowercase, StringBuilder_c &sMsg );

/// convert SphinxBSON blob back to JSON document
void sphJsonFormat ( JsonEscapedBuilder & dOut, const BYTE * pData );

/// convert SphinxBSON blob back to JSON document
/// NOTE, bQuoteString==false is intended to format individual values only (and avoid quoting string values in that case)
const BYTE * sphJsonFieldFormat ( JsonEscapedBuilder & sOut, const BYTE * pData, ESphJsonType eType, bool bQuoteString=true );

/// compute key mask (for Bloom filtering) from the key name
DWORD sphJsonKeyMask ( const char * sKey, int iLen );

/// find first value in SphinxBSON blob, return associated type
ESphJsonType sphJsonFindFirst ( const BYTE ** ppData );

/// find value by key in SphinxBSON blob, return associated type
ESphJsonType sphJsonFindByKey ( ESphJsonType eType, const BYTE ** ppValue, const void * pKey, int iLen, DWORD uMask );

/// find value by index in SphinxBSON blob, return associated type
ESphJsonType sphJsonFindByIndex ( ESphJsonType eType, const BYTE ** ppValue, int iIndex );

/// split name to object and key parts, return false if not JSON name
bool sphJsonNameSplit ( const char * sName, CSphString * sColumn, CSphString * sKey );

/// compute node size, in bytes
/// returns -1 when data itself is required to compute the size, but pData is NULL
int sphJsonNodeSize ( ESphJsonType eType, const BYTE * pData );

/// skip the current node, and update the pointer
void sphJsonSkipNode ( ESphJsonType eType, const BYTE ** ppData );

/// return object length or array length, in elements
/// POD types return 1, empty objects return 0
int sphJsonFieldLength ( ESphJsonType eType, const BYTE * pData );

/// inplace JSON update, both for realtime and non-realtime indexes, returns true if update is possible
bool sphJsonInplaceUpdate ( ESphJsonType eValueType, int64_t iValue, ISphExpr * pExpr, BYTE * pStrings, const CSphRowitem * pRow, bool bUpdate );

/// converts string to number
bool sphJsonStringToNumber ( const char * s, int iLen, ESphJsonType & eType, int64_t & iVal, double & fVal );

/// internal cJSON init
void sphInitCJson();

struct cJSON;

/// simple cJSON wrapper
class JsonObj_c
{
public:
	explicit		JsonObj_c ( bool bArray = false );
	explicit		JsonObj_c ( cJSON * pRoot, bool bOwner = true );
	explicit		JsonObj_c ( const char * szJson );
					JsonObj_c ( JsonObj_c && rhs );
					~JsonObj_c();

					// a shortcut for !Empty()
					operator bool() const;
	JsonObj_c &		operator = ( JsonObj_c && rhs );
	JsonObj_c		operator[] ( int iItem ) const;
	JsonObj_c &		operator++();
	JsonObj_c 		operator*();

	void			AddStr ( const char * szName, const char * szValue );
	void			AddStr ( const char * szName, const CSphString & sValue );
	void			AddNum ( const char * szName, int64_t iValue );
	void			AddBool ( const char * szName, bool bValue );
	void			AddItem ( const char * szName, JsonObj_c & tObj );
	void			AddItem ( JsonObj_c & tObj );
	void			DelItem ( const char * szName );

	int				Size() const;
	JsonObj_c		GetItem ( const char * szName ) const;
	JsonObj_c		GetIntItem ( const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	JsonObj_c		GetIntItem ( const char * szName1, const char * szName2, CSphString & sError, bool bIgnoreMissing=false ) const;
	JsonObj_c		GetBoolItem ( const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	JsonObj_c		GetStrItem ( const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	JsonObj_c		GetObjItem ( const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	JsonObj_c		GetArrayItem ( const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	bool			FetchIntItem ( int & iValue, const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	bool			FetchBoolItem ( bool & bValue, const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	bool			FetchStrItem ( CSphString & sValue, const char * szName, CSphString & sError, bool bIgnoreMissing=false ) const;
	bool			HasItem ( const char * szName ) const;

	static JsonObj_c CreateStr ( const CSphString & sStr );

	bool			IsInt() const;
	bool			IsDbl() const;
	bool			IsNum() const;
	bool			IsBool() const;
	bool			IsObj() const;
	bool			IsStr() const;
	bool			IsArray() const;
	bool			Empty() const;
	const char *	Name() const;

	int64_t			IntVal() const;
	bool			BoolVal() const;
	float			FltVal() const;
	double			DblVal() const;
	const char *	SzVal() const;
	CSphString		StrVal() const;

	const char *	GetErrorPtr() const;
	bool			GetError ( const char * szBuf, int iBufLen, CSphString & sError ) const;
	cJSON *			GetRoot();
	CSphString		AsString ( bool bFormat=false ) const;

	JsonObj_c		begin() const;
	JsonObj_c		end() const;

protected:
	cJSON *			m_pRoot {nullptr};
	bool			m_bOwner {true};

	cJSON *			Leak();
	JsonObj_c		GetChild ( const char * szName, CSphString & sError, bool bIgnoreMissing ) const;
};

#ifndef JsonNull
#define JsonNull JsonObj_c(nullptr,false)
#endif

namespace bson {



using NodeHandle_t = std::pair< const BYTE *, ESphJsonType>;
const NodeHandle_t nullnode = { nullptr, JSON_EOF };

bool IsAssoc ( const NodeHandle_t & );
bool IsArray ( const NodeHandle_t & );
bool IsPODBlob ( const NodeHandle_t & );
bool IsString ( const NodeHandle_t & );
bool IsInt ( const NodeHandle_t & );
bool IsDouble ( const NodeHandle_t & );
bool IsNumeric ( const NodeHandle_t & );

// access to values by locator
bool Bool ( const NodeHandle_t& tLocator );
int64_t Int ( const NodeHandle_t & tLocator );
double Double ( const NodeHandle_t & tLocator );
CSphString String ( const NodeHandle_t & tLocator );
inline bool IsNullNode ( const NodeHandle_t & dNode ) { return dNode==nullnode; }

// iterate over mixed vec/ string vec/ object (without names).
using Action_f = std::function<void ( const NodeHandle_t &tNode )>;
void ForEach ( const NodeHandle_t &tLocator, Action_f fAction );

// iterate over mixed vec/ string vec/ object (incl. names).
using NamedAction_f = std::function<void ( CSphString sName, const NodeHandle_t &tNode )>;
void ForEach ( const NodeHandle_t &tLocator, NamedAction_f fAction );

// iterate over mixed vec/ string vec/ object (without names). Return false from action means 'stop iteration'
using CondAction_f = std::function<bool ( const NodeHandle_t &tNode )>;
void ForSome ( const NodeHandle_t &tLocator, CondAction_f fAction );

// iterate over mixed vec/ string vec/ object (incl. names).  Return false from action means 'stop iteration'
using CondNamedAction_f = std::function<bool ( CSphString sName, const NodeHandle_t &tNode )>;
void ForSome ( const NodeHandle_t &tLocator, CondNamedAction_f fAction );

// suitable for strings and vectors of pods as int, int64, double.
std::pair<const char*, int> RawBlob ( const NodeHandle_t &tLocator );

// many internals might be represented as vector
template<typename BLOB> VecTraits_T<BLOB> Vector ( const NodeHandle_t &tLocator )
{
	auto dBlob = RawBlob ( tLocator );
	return VecTraits_T<BLOB> ( (BLOB*) dBlob.first, dBlob.second );
}

// access to encoded bson
class Bson_c
{
protected:
	NodeHandle_t	m_dData { nullnode };
	mutable StringBuilder_c	m_sError;

public:
	Bson_c ( NodeHandle_t dBson = nullnode )
		: m_dData { std::move (dBson) }
	{}
	explicit Bson_c ( const VecTraits_T<BYTE>& dBlob );

	// traits
	bool IsAssoc () const { return bson::IsAssoc ( m_dData ); }; /// whether we can access members by name
	bool IsArray () const { return bson::IsArray ( m_dData ); }; /// whether we can access members by index
	bool IsNull () const { return m_dData==nullnode; } /// whether node valid (i.e. not eof type, not nullptr locator).
	operator bool() const { return !IsNull(); } // same as IsNull.
	bool IsString () const { return m_dData.second==JSON_STRING; }
	bool IsInt () const { return bson::IsInt ( m_dData ); }
	bool IsDouble () const { return bson::IsDouble ( m_dData ); }
	bool IsNumeric () const { return bson::IsNumeric (m_dData); }

	bool IsNonEmptyString () const { return bson::IsString ( m_dData ) && !IsEmpty (); }; /// whether we can return non-empty string
	bool IsEmpty () const; /// whether container bson has child elements, or string is empty.
	int CountValues() const; /// count of elems. Objs and root will linearly iterate, other returns immediately.
	int StandaloneSize() const; /// size of blob need to save node as root (standalone) bson
	bool StrEq ( const char* sValue ) const; // true if value is string and eq to sValue

	// navigate over bson
	NodeHandle_t ChildByName ( const char* sName ) const; // look by direct child name
	NodeHandle_t ChildByIndex ( int iIdx ) const; // look by direct child idx
	NodeHandle_t ChildByPath ( const char * sPath ) const; // complex look like 'query.percolate.documents[3].title'

	// rapid lookup by name helpers. Ellipsis must be list of str literals,
	// like HasAnyOf (2, "foo", "bar"); HasAnyOf (3, "foo", "bar", "baz")
	bool HasAnyOf ( int iNames, ... ) const;

	// access to my values
	bool Bool () const;
	int64_t Int () const;
	double Double () const;
	CSphString String () const;
	void ForEach ( Action_f&& fAction ) const;
	void ForEach ( NamedAction_f&& fAction ) const;
	void ForSome ( CondAction_f&& fAction ) const;
	void ForSome ( CondNamedAction_f&& fAction ) const;

	// format back to json
	bool BsonToJson ( CSphString& ) const;

	// save as standalone (root) bson.
	bool BsonToBson ( BYTE* ) const;
	bool BsonToBson ( CSphVector<BYTE>& dOutput ) const;

	// helpers
	inline ESphJsonType GetType() const { return m_dData.second; }
	operator NodeHandle_t () const { return m_dData; }
	const char * sError () const;
};

// iterate over Bson_c
class BsonIterator_c : public Bson_c
{
	const BYTE * m_pData = nullptr;
	ESphJsonType m_eType = JSON_EOF; // parent's type
	int m_iSize = -1; // for nodes with known size
	CSphString m_sName;

	inline bool Finish () // used as shortcut return
	{
		if ( m_iSize>0 )
			m_iSize = 0;
		m_dData = bson::nullnode;
		return false;
	}

public:
	explicit BsonIterator_c ( const NodeHandle_t &dParent );
	bool Next();
	int NumElems() const { return m_iSize; } // how many items we still not iterated ( actual only for arrays; otherwise it is -1 )
	CSphString GetName() const { return m_sName; }
};

// parse and access to encoded bson
class BsonContainer_c : public Bson_c
{
	CSphVector<BYTE> m_Bson;
public:
	explicit BsonContainer_c ( char* sJson, bool bAutoconv=false, bool bToLowercase=true );
	explicit BsonContainer_c ( const char * sJsonc, bool bAutoconv=false, bool bToLowercase = true )
	: BsonContainer_c ( ( char * ) CSphString ( sJsonc ).cstr (), bToLowercase ) {}
};

class BsonContainer2_c : public Bson_c
{
	CSphVector<BYTE> m_Bson;
public:
	explicit BsonContainer2_c ( const char * sJsonc, bool bAutoconv = false, bool bToLowercase = true );
};

bool	JsonObjToBson ( JsonObj_c & tJSON, CSphVector<BYTE> &dData, bool bAutoconv, bool bToLowercase/*, StringBuilder_c &sMsg*/ );
bool	cJsonToBson ( cJSON * pCJSON, CSphVector<BYTE> &dData, bool bAutoconv=false, bool bToLowercase = true /*, StringBuilder_c &sMsg */);

}; // namespace sph

#endif // _sphinxjson_

//
// $Id$
//
