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

#include "indexsettings.h"

#include "sphinxstd.h"
#include "sphinxint.h"
#include "fileutils.h"
#include "sphinxstem.h"
#include "icu.h"
#include "attribute.h"

#if !USE_WINDOWS
	#include <glob.h>
#endif


static CreateFilenameBuilder_fn g_fnCreateFilenameBuilder = nullptr;

void SetIndexFilenameBuilder ( CreateFilenameBuilder_fn pBuilder )
{
	g_fnCreateFilenameBuilder = pBuilder;
}


CreateFilenameBuilder_fn GetIndexFilenameBuilder()
{
	return g_fnCreateFilenameBuilder;
}


static const char * BigramName ( ESphBigram eType )
{
	switch ( eType )
	{
	case SPH_BIGRAM_ALL:
		return "all";

	case SPH_BIGRAM_FIRSTFREQ:
		return "first_freq";

	case SPH_BIGRAM_BOTHFREQ:
		return "both_freq";

	case SPH_BIGRAM_NONE:
	default:
		return "none";
	}
}


CSphString CompressionToStr ( Compression_e eComp )
{
	switch ( eComp )
	{
	case Compression_e::LZ4:
		return "lz4";

	case Compression_e::LZ4HC:
		return "lz4hc";

	case Compression_e::NONE:
	default:
		return "none";
	}
}

//////////////////////////////////////////////////////////////////////////

struct SettingsFormatterState_t
{
	FILE *				m_pFile = nullptr;
	StringBuilder_c *	m_pBuf = nullptr;
	bool				m_bFirst = true;

						SettingsFormatterState_t ( FILE * pFile );
						SettingsFormatterState_t ( StringBuilder_c & tBuf );
};


SettingsFormatterState_t::SettingsFormatterState_t ( FILE * pFile )
	: m_pFile ( pFile )
{}


SettingsFormatterState_t::SettingsFormatterState_t ( StringBuilder_c & tBuf )
	: m_pBuf ( &tBuf )
{}


class SettingsFormatter_c
{
public:
				SettingsFormatter_c ( SettingsFormatterState_t & tState, const char * szPrefix, const char * szEq, const char * szPostfix, const char * szSeparator, bool bIgnoreConf = false );

	template <typename T>
	void		Add ( const char * szKey, T tVal, bool bCond );

	template <typename T>
	void		AddEmbedded ( const char * szKey, const VecTraits_T<T> & dEmbedded, bool bCond );

private:
	SettingsFormatterState_t & m_tState;
	CSphString			m_sPrefix;
	CSphString			m_sEq;
	CSphString			m_sPostfix;
	CSphString			m_sSeparator;
	bool				m_bIgnoreCond = false;
};


SettingsFormatter_c::SettingsFormatter_c ( SettingsFormatterState_t & tState, const char * szPrefix, const char * szEq, const char * szPostfix, const char * szSeparator, bool bIgnoreCond )
	: m_tState		( tState )
	, m_sPrefix		( szPrefix )
	, m_sEq			( szEq )
	, m_sPostfix	( szPostfix )
	, m_sSeparator	( szSeparator )
	, m_bIgnoreCond	( bIgnoreCond )
{}

template <typename T>
void SettingsFormatter_c::Add ( const char * szKey, T tVal, bool bCond )
{
	if ( !m_bIgnoreCond && !bCond )
		return;

	if ( m_tState.m_pBuf )
	{
		if ( !m_tState.m_bFirst )
			(*m_tState.m_pBuf) << m_sSeparator;

		(*m_tState.m_pBuf) << m_sPrefix << szKey << m_sEq << tVal << m_sPostfix;
	}

	if ( m_tState.m_pFile )
	{
		StringBuilder_c tBuilder;
		if ( !m_tState.m_bFirst )
			tBuilder << m_sSeparator;

		tBuilder << m_sPrefix << szKey << m_sEq << tVal << m_sPostfix;
		fputs ( tBuilder.cstr(), m_tState.m_pFile );
	}

	m_tState.m_bFirst = false;
}

template <typename T>
void SettingsFormatter_c::AddEmbedded ( const char * szKey, const VecTraits_T<T> & dEmbedded, bool bCond )
{
	CSphString sPlural;
	sPlural.SetSprintf( "%ss", szKey );
	Add ( sPlural.cstr(), bCond ? 1 : 0, true );

	if ( bCond )
	{
		ARRAY_FOREACH ( i, dEmbedded )
		{
			CSphString sName;
			sName.SetSprintf ( "%s [%d]", szKey, i );
			Add ( sName.cstr(), dEmbedded[i], true );
		}
	}
}


//////////////////////////////////////////////////////////////////////////

void SettingsWriter_c::DumpReadable ( SettingsFormatterState_t & tState, const CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder ) const
{
	SettingsFormatter_c tFormatter ( tState, "", ": ", "", "\n", true );
	Format ( tFormatter, pFilenameBuilder );
}


//////////////////////////////////////////////////////////////////////////

static RtTypedAttr_t g_dRtTypedAttrs[]=
{
	{ SPH_ATTR_INTEGER,		"rt_attr_uint" },
	{ SPH_ATTR_BIGINT,		"rt_attr_bigint" },
	{ SPH_ATTR_TIMESTAMP,	"rt_attr_timestamp" },
	{ SPH_ATTR_BOOL,		"rt_attr_bool" },
	{ SPH_ATTR_FLOAT,		"rt_attr_float" },
	{ SPH_ATTR_STRING,		"rt_attr_string" },
	{ SPH_ATTR_JSON,		"rt_attr_json" },
	{ SPH_ATTR_UINT32SET,	"rt_attr_multi" },
	{ SPH_ATTR_INT64SET,	"rt_attr_multi_64" }
};


int	GetNumRtTypes()
{
	return sizeof(g_dRtTypedAttrs)/sizeof(g_dRtTypedAttrs[0]);
}


const RtTypedAttr_t & GetRtType ( int iType )
{
	return g_dRtTypedAttrs[iType];
}


static CSphString FormatPath ( const CSphString & sFile, const FilenameBuilder_i * pFilenameBuilder )
{
	if ( !pFilenameBuilder || sFile.IsEmpty() )
		return sFile;

	return pFilenameBuilder->GetFullPath(sFile);
}

//////////////////////////////////////////////////////////////////////////

ESphWordpart CSphSourceSettings::GetWordpart ( const char * sField, bool bWordDict )
{
	if ( bWordDict )
		return SPH_WORDPART_WHOLE;

	bool bPrefix = ( m_iMinPrefixLen>0 ) && ( m_dPrefixFields.IsEmpty () || m_dPrefixFields.Contains ( sField ) );
	bool bInfix = ( m_iMinInfixLen>0 ) && ( m_dInfixFields.IsEmpty() || m_dInfixFields.Contains ( sField ) );

	assert ( !( bPrefix && bInfix ) ); // no field must be marked both prefix and infix
	if ( bPrefix )
		return SPH_WORDPART_PREFIX;
	if ( bInfix )
		return SPH_WORDPART_INFIX;
	return SPH_WORDPART_WHOLE;
}

int CSphSourceSettings::GetMinPrefixLen ( bool bWordDict ) const
{
	if ( !bWordDict )
		return m_iMinPrefixLen;

	if ( m_iMinPrefixLen )
		return m_iMinPrefixLen;

	if ( m_iMinInfixLen )
		return 1;

	return 0;
}

void CSphSourceSettings::SetMinPrefixLen ( int iMinPrefixLen )
{
	m_iMinPrefixLen = iMinPrefixLen;
}

int CSphSourceSettings::RawMinPrefixLen () const
{
	return m_iMinPrefixLen;
}

//////////////////////////////////////////////////////////////////////////

void DocstoreSettings_t::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	DocstoreSettings_t tDefault;

	tOut.Add ( "docstore_compression",			CompressionToStr(m_eCompression), m_eCompression!=tDefault.m_eCompression );
	tOut.Add ( "docstore_compression_level",	m_iCompressionLevel,	m_iCompressionLevel!=tDefault.m_iCompressionLevel );
	tOut.Add ( "docstore_block_size",			m_uBlockSize,			m_uBlockSize!=tDefault.m_uBlockSize );
}

//////////////////////////////////////////////////////////////////////////

void CSphTokenizerSettings::Setup ( const CSphConfigSection & hIndex, CSphString & sWarning )
{
	m_iNgramLen = Max ( hIndex.GetInt ( "ngram_len" ), 0 );

	if ( hIndex ( "ngram_chars" ) )
	{
		if ( m_iNgramLen )
			m_iType = TOKENIZER_NGRAM;
		else
			sWarning = "ngram_chars specified, but ngram_len=0; IGNORED";
	}

	m_sCaseFolding = hIndex.GetStr ( "charset_table" );
	m_iMinWordLen = Max ( hIndex.GetInt ( "min_word_len", 1 ), 1 );
	m_sNgramChars = hIndex.GetStr ( "ngram_chars" );
	m_sSynonymsFile = hIndex.GetStr ( "exceptions" ); // new option name
	m_sIgnoreChars = hIndex.GetStr ( "ignore_chars" );
	m_sBlendChars = hIndex.GetStr ( "blend_chars" );
	m_sBlendMode = hIndex.GetStr ( "blend_mode" );

	// phrase boundaries
	int iBoundaryStep = Max ( hIndex.GetInt ( "phrase_boundary_step" ), -1 );
	if ( iBoundaryStep!=0 )
		m_sBoundary = hIndex.GetStr ( "phrase_boundary" );
}


bool CSphTokenizerSettings::Load ( const FilenameBuilder_i * pFilenameBuilder, CSphReader & tReader, CSphEmbeddedFiles & tEmbeddedFiles, CSphString & sWarning )
{
	m_iType = tReader.GetByte ();
	if ( m_iType!=TOKENIZER_UTF8 && m_iType!=TOKENIZER_NGRAM )
	{
		sWarning = "can't load an old index with SBCS tokenizer";
		return false;
	}

	m_sCaseFolding = tReader.GetString ();
	m_iMinWordLen = tReader.GetDword ();
	tEmbeddedFiles.m_bEmbeddedSynonyms = false;
	tEmbeddedFiles.m_bEmbeddedSynonyms = !!tReader.GetByte();
	if ( tEmbeddedFiles.m_bEmbeddedSynonyms )
	{
		int nSynonyms = (int)tReader.GetDword();
		tEmbeddedFiles.m_dSynonyms.Resize ( nSynonyms );
		ARRAY_FOREACH ( i, tEmbeddedFiles.m_dSynonyms )
			tEmbeddedFiles.m_dSynonyms[i] = tReader.GetString();
	}

	m_sSynonymsFile = tReader.GetString ();
	CSphString sFilePath = FormatPath ( m_sSynonymsFile, pFilenameBuilder );
	tEmbeddedFiles.m_tSynonymFile.Read ( tReader, sFilePath.cstr(), false, tEmbeddedFiles.m_bEmbeddedSynonyms ? NULL : &sWarning );
	m_sBoundary = tReader.GetString ();
	m_sIgnoreChars = tReader.GetString ();
	m_iNgramLen = tReader.GetDword ();
	m_sNgramChars = tReader.GetString ();
	m_sBlendChars = tReader.GetString ();
	m_sBlendMode = tReader.GetString();

	return true;
}


void CSphTokenizerSettings::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	bool bKnownTokenizer = ( m_iType==TOKENIZER_UTF8 || m_iType==TOKENIZER_NGRAM );
	tOut.Add ( "charset_type",		bKnownTokenizer ? "utf-8" : "unknown tokenizer (deprecated sbcs?)", !bKnownTokenizer );

	// fixme! need unified default charset handling
	tOut.Add ( "charset_table",		m_sCaseFolding,	!m_sCaseFolding.IsEmpty() && m_sCaseFolding!="non_cjk" );
	tOut.Add ( "min_word_len",		m_iMinWordLen,	m_iMinWordLen>1 );
	tOut.Add ( "ngram_len",			m_iNgramLen,	m_iNgramLen && !m_sNgramChars.IsEmpty() );
	tOut.Add ( "ngram_chars",		m_sNgramChars,	m_iNgramLen && !m_sNgramChars.IsEmpty() );
	tOut.Add ( "phrase_boundary",	m_sBoundary,	!m_sBoundary.IsEmpty() );
	tOut.Add ( "ignore_chars",		m_sIgnoreChars,	!m_sIgnoreChars.IsEmpty() );
	tOut.Add ( "blend_chars",		m_sBlendChars,	!m_sBlendChars.IsEmpty() );
	tOut.Add ( "blend_mode",		m_sBlendMode,	!m_sBlendMode.IsEmpty() );

	CSphString sSynonymsFile = FormatPath ( m_sSynonymsFile, pFilenameBuilder );
	tOut.Add ( "exceptions",		sSynonymsFile,	!sSynonymsFile.IsEmpty() );
}


void CSphTokenizerSettings::DumpReadable ( SettingsFormatterState_t & tState, const CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder ) const
{
	SettingsFormatter_c tFormatter ( tState, "tokenizer-", ": ", "", "\n", true );
	Format ( tFormatter, pFilenameBuilder );

	tFormatter.AddEmbedded ( "embedded_exception", tEmbeddedFiles.m_dSynonyms, tEmbeddedFiles.m_bEmbeddedSynonyms );
}


//////////////////////////////////////////////////////////////////////////

void CSphDictSettings::Setup ( const CSphConfigSection & hIndex, FilenameBuilder_i * pFilenameBuilder, CSphString & sWarning )
{
	m_sMorphology = hIndex.GetStr ( "morphology" );
	m_sMorphFields = hIndex.GetStr ( "morphology_skip_fields" );
	m_sStopwords = hIndex.GetStr ( "stopwords" );
	m_iMinStemmingLen = hIndex.GetInt ( "min_stemming_len", 1 );
	m_bStopwordsUnstemmed = hIndex.GetInt ( "stopwords_unstemmed" )!=0;

	for ( CSphVariant * pWordforms = hIndex("wordforms"); pWordforms; pWordforms = pWordforms->m_pNext )
	{
		if ( !pWordforms->cstr() || !*pWordforms->cstr() )
			continue;

		CSphString sWordformFile = FormatPath ( pWordforms->cstr(), pFilenameBuilder );
		StrVec_t dFilesFound = FindFiles ( sWordformFile.cstr() );

		for ( auto & i : dFilesFound )
		{
			if ( pFilenameBuilder )
				StripPath(i);

			m_dWordforms.Add(i);
		}
	}

	if ( hIndex("dict") )
	{
		m_bWordDict = true; // default to keywords
		if ( hIndex["dict"]=="crc" )
			m_bWordDict = false;
		else if ( hIndex["dict"]!="keywords" )
			sWarning.SetSprintf ( "WARNING: unknown dict=%s, defaulting to keywords\n", hIndex["dict"].cstr() );
	}
}


void CSphDictSettings::Load ( CSphReader & tReader, CSphEmbeddedFiles & tEmbeddedFiles, CSphString & sWarning )
{
	m_sMorphology = tReader.GetString();
	m_sMorphFields = tReader.GetString();

	tEmbeddedFiles.m_bEmbeddedStopwords = false;
	tEmbeddedFiles.m_bEmbeddedStopwords = !!tReader.GetByte();
	if ( tEmbeddedFiles.m_bEmbeddedStopwords )
	{
		int nStopwords = (int)tReader.GetDword();
		tEmbeddedFiles.m_dStopwords.Resize ( nStopwords );
		ARRAY_FOREACH ( i, tEmbeddedFiles.m_dStopwords )
			tEmbeddedFiles.m_dStopwords[i] = (SphWordID_t)tReader.UnzipOffset();
	}

	m_sStopwords = tReader.GetString ();
	int nFiles = tReader.GetDword ();

	CSphString sFile;
	tEmbeddedFiles.m_dStopwordFiles.Resize ( nFiles );
	for ( int i = 0; i < nFiles; i++ )
	{
		sFile = tReader.GetString ();
		tEmbeddedFiles.m_dStopwordFiles[i].Read ( tReader, sFile.cstr(), true, tEmbeddedFiles.m_bEmbeddedSynonyms ? NULL : &sWarning );
	}

	tEmbeddedFiles.m_bEmbeddedWordforms = false;
	tEmbeddedFiles.m_bEmbeddedWordforms = !!tReader.GetByte();
	if ( tEmbeddedFiles.m_bEmbeddedWordforms )
	{
		int nWordforms = (int)tReader.GetDword();
		tEmbeddedFiles.m_dWordforms.Resize ( nWordforms );
		ARRAY_FOREACH ( i, tEmbeddedFiles.m_dWordforms )
			tEmbeddedFiles.m_dWordforms[i] = tReader.GetString();
	}

	m_dWordforms.Resize ( tReader.GetDword() );

	tEmbeddedFiles.m_dWordformFiles.Resize ( m_dWordforms.GetLength() );
	ARRAY_FOREACH ( i, m_dWordforms )
	{
		m_dWordforms[i] = tReader.GetString();
		tEmbeddedFiles.m_dWordformFiles[i].Read ( tReader, m_dWordforms[i].cstr(), false, tEmbeddedFiles.m_bEmbeddedWordforms ? NULL : &sWarning );
	}

	m_iMinStemmingLen = tReader.GetDword ();

	m_bWordDict = ( tReader.GetByte()!=0 );

	m_bStopwordsUnstemmed = ( tReader.GetByte()!=0 );
	m_sMorphFingerprint = tReader.GetString();
}


void CSphDictSettings::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	tOut.Add ( "dict",					m_bWordDict ? "keywords" : "crc", !m_bWordDict );
	tOut.Add ( "morphology",			m_sMorphology,		!m_sMorphology.IsEmpty() );
	tOut.Add ( "min_stemming_len",		m_iMinStemmingLen,	m_iMinStemmingLen>1 );
	tOut.Add ( "stopwords_unstemmed",	1,					m_bStopwordsUnstemmed );

	CSphString sStopwordsFile = FormatPath ( m_sStopwords, pFilenameBuilder );
	tOut.Add ( "stopwords",				sStopwordsFile,		!sStopwordsFile.IsEmpty() );

	StringBuilder_c sAllWordforms(" ");
	for ( const auto & i : m_dWordforms )
		sAllWordforms << FormatPath ( i, pFilenameBuilder );

	tOut.Add ( "wordforms",	sAllWordforms.cstr(), !sAllWordforms.IsEmpty() );
}


void CSphDictSettings::DumpReadable ( SettingsFormatterState_t & tState, const CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder ) const
{
	SettingsFormatter_c tFormatter ( tState, "dictionary-", ": ", "", "\n", true );
	Format ( tFormatter, pFilenameBuilder );

	tFormatter.AddEmbedded ( "embedded_stopword", tEmbeddedFiles.m_dStopwords, tEmbeddedFiles.m_bEmbeddedStopwords );
	tFormatter.AddEmbedded ( "embedded_wordform", tEmbeddedFiles.m_dWordforms, tEmbeddedFiles.m_bEmbeddedWordforms );
}

//////////////////////////////////////////////////////////////////////////

bool CSphFieldFilterSettings::Setup ( const CSphConfigSection & hIndex, CSphString & sWarning )
{
#if USE_RE2
	// regular expressions
	m_dRegexps.Resize ( 0 );
	for ( CSphVariant * pFilter = hIndex("regexp_filter"); pFilter; pFilter = pFilter->m_pNext )
		m_dRegexps.Add ( pFilter->cstr() );

	return m_dRegexps.GetLength() > 0;
#else
	if ( hIndex ( "regexp_filter" ) )
		sWarning.SetSprintf ( "regexp_filter specified but no regexp support compiled" );

	return false;
#endif
}


void CSphFieldFilterSettings::Load ( CSphReader & tReader )
{
	int nRegexps = tReader.GetDword();
	if ( !nRegexps )
		return;

	m_dRegexps.Resize ( nRegexps );
	for ( auto & i : m_dRegexps )
		i = tReader.GetString();
}


void CSphFieldFilterSettings::Save ( CSphWriter & tWriter ) const
{
	tWriter.PutDword ( m_dRegexps.GetLength() );
	for ( const auto & i : m_dRegexps )
		tWriter.PutString(i);
}


void CSphFieldFilterSettings::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * /*pFilenameBuilder*/ ) const
{
	for ( const auto & i : m_dRegexps )
		tOut.Add ( "regexp_filter", i, !i.IsEmpty() );
}

//////////////////////////////////////////////////////////////////////////

CSphString KillListTarget_t::Format() const
{
	CSphString sTarget, sFlags;

	DWORD uMask = KillListTarget_t::USE_KLIST | KillListTarget_t::USE_DOCIDS;
	if ( (m_uFlags & uMask) != uMask )
	{
		if ( m_uFlags & KillListTarget_t::USE_KLIST )
			sFlags=":kl";

		if ( m_uFlags & KillListTarget_t::USE_DOCIDS )
			sFlags=":id";
	} else
		sFlags = "";

	sTarget.SetSprintf ( "%s%s", m_sIndex.cstr(), sFlags.cstr() );

	return sTarget;
}


bool KillListTargets_c::Parse ( const CSphString & sTargets, const char * szIndexName, CSphString & sError )
{
	StrVec_t dIndexes;
	sphSplit ( dIndexes, sTargets.cstr(), " \t," );

	m_dTargets.Resize(dIndexes.GetLength());
	ARRAY_FOREACH ( i, m_dTargets )
	{
		KillListTarget_t & tTarget = m_dTargets[i];
		const char * szTargetName = dIndexes[i].cstr();
		const char * sSplit = strstr ( szTargetName, ":" );
		if ( sSplit )
		{
			CSphString sOptions = sSplit+1;

			if ( sOptions=="kl" )
				tTarget.m_uFlags = KillListTarget_t::USE_KLIST;
			else if ( sOptions=="id" )
				tTarget.m_uFlags = KillListTarget_t::USE_DOCIDS;
			else
			{
				sError.SetSprintf ( "unknown kill list target option near '%s'\n", dIndexes[i].cstr() );
				return false;
			}

			tTarget.m_sIndex = dIndexes[i].SubString ( 0, sSplit-szTargetName );
		}
		else
			tTarget.m_sIndex = szTargetName;

		if ( tTarget.m_sIndex==szIndexName )
		{
			sError.SetSprintf ( "cannot apply kill list to myself: killlist_target=%s\n", sTargets.cstr() );
			return false;
		}
	}

	return true;
}


void KillListTargets_c::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * /*pFilenameBuilder*/ ) const
{
	StringBuilder_c sTargets;
	auto tComma = ScopedComma_c ( sTargets, "," );
	for ( const auto & i : m_dTargets )
		sTargets += i.Format().cstr();

	tOut.Add ( "killlist_target", sTargets.cstr(), !sTargets.IsEmpty() );
}

//////////////////////////////////////////////////////////////////////////

void CSphIndexSettings::ParseStoredFields ( const CSphConfigSection & hIndex )
{
	CSphString sFields = hIndex.GetStr ( "stored_fields" );
	sFields.ToLower();
	sphSplit ( m_dStoredFields, sFields.cstr() );
	m_dStoredFields.Uniq();

	sFields = hIndex.GetStr ( "stored_only_fields" );
	sFields.ToLower();
	sphSplit ( m_dStoredOnlyFields, sFields.cstr() );
	m_dStoredOnlyFields.Uniq();
}


#if USE_COLUMNAR
bool CSphIndexSettings::ParseColumnarSettings ( const CSphConfigSection & hIndex, CSphString & sError )
{
	if ( hIndex.Exists("columnar_attrs") && !IsColumnarLibLoaded() )
	{
		sError = "columnar library not loaded";
		return false;
	}

	{
		CSphString sAttrs = hIndex.GetStr ( "columnar_attrs" );
		sAttrs.ToLower();
		sphSplit ( m_dColumnarAttrs, sAttrs.cstr() );
		m_dColumnarAttrs.Uniq();
	}

	{
		CSphString sAttrs = hIndex.GetStr ( "columnar_strings_no_hash" );
		sAttrs.ToLower();
		sphSplit ( m_dColumnarStringsNoHash, sAttrs.cstr() );
		m_dColumnarStringsNoHash.Uniq();
	}

	m_sCompressionUINT32 = hIndex.GetStr ( "columnar_compression_uint32", m_sCompressionUINT32.c_str() ).cstr();
	m_sCompressionUINT64 = hIndex.GetStr ( "columnar_compression_int64", m_sCompressionUINT64.c_str() ).cstr();
	m_iSubblockSize = hIndex.GetInt ( "columnar_subblock", 128 );
	m_iSubblockSizeMva = hIndex.GetInt ( "columnar_subblock_mva", 128 );
	m_iMinMaxLeafSize = hIndex.GetInt ( "columnar_minmax_leaf", 128 );

	return true;
}
#endif

bool CSphIndexSettings::ParseDocstoreSettings ( const CSphConfigSection & hIndex, CSphString & sWarning, CSphString & sError )
{
	m_uBlockSize = hIndex.GetSize ( "docstore_block_size", DEFAULT_DOCSTORE_BLOCK );
	m_iCompressionLevel = hIndex.GetInt ( "docstore_compression_level", DEFAULT_COMPRESSION_LEVEL );

	if ( !hIndex.Exists("docstore_compression") )
		return true;

	CSphString sCompression = hIndex["docstore_compression"].cstr();

	if ( sCompression=="none" )
		m_eCompression = Compression_e::NONE;
	else if ( sCompression=="lz4" )
		m_eCompression = Compression_e::LZ4;
	else if ( sCompression=="lz4hc" )
		m_eCompression = Compression_e::LZ4HC;
	else
	{
		sError.SetSprintf ( "unknown compression specified in 'docstore_compression': '%s'\n", sCompression.cstr() ); 
		return false;
	}

	if ( hIndex.Exists("docstore_compression_level") && m_eCompression!=Compression_e::LZ4HC )
		sWarning.SetSprintf ( "docstore_compression_level works only with LZ4HC compression" ); 

	return true;
}


static const int64_t DEFAULT_ATTR_UPDATE_RESERVE = 131072;

bool CSphIndexSettings::Setup ( const CSphConfigSection & hIndex, const char * szIndexName, CSphString & sWarning, CSphString & sError )
{
	// misc settings
	SetMinPrefixLen ( Max ( hIndex.GetInt ( "min_prefix_len" ), 0 ) );
	m_iMinInfixLen = Max ( hIndex.GetInt ( "min_infix_len" ), 0 );
	m_iMaxSubstringLen = Max ( hIndex.GetInt ( "max_substring_len" ), 0 );
	m_iBoundaryStep = Max ( hIndex.GetInt ( "phrase_boundary_step" ), -1 );
	m_bIndexExactWords = hIndex.GetInt ( "index_exact_words" )!=0;
	m_iOvershortStep = Min ( Max ( hIndex.GetInt ( "overshort_step", 1 ), 0 ), 1 );
	m_iStopwordStep = Min ( Max ( hIndex.GetInt ( "stopword_step", 1 ), 0 ), 1 );
	m_iEmbeddedLimit = hIndex.GetSize ( "embedded_limit", 16384 );
	m_bIndexFieldLens = hIndex.GetInt ( "index_field_lengths" )!=0;
	m_sIndexTokenFilter = hIndex.GetStr ( "index_token_filter" );
	m_tBlobUpdateSpace = hIndex.GetSize64 ( "attr_update_reserve", DEFAULT_ATTR_UPDATE_RESERVE );

	if ( !m_tKlistTargets.Parse ( hIndex.GetStr ( "killlist_target" ), szIndexName, sError ) )
		return false;

	// prefix/infix fields
	CSphString sFields;

	sFields = hIndex.GetStr ( "prefix_fields" );
	sFields.ToLower();
	sphSplit ( m_dPrefixFields, sFields.cstr() );

	sFields = hIndex.GetStr ( "infix_fields" );
	sFields.ToLower();
	sphSplit ( m_dInfixFields, sFields.cstr() );

	ParseStoredFields(hIndex);

#if USE_COLUMNAR
	if ( !ParseColumnarSettings ( hIndex, sError ) )
		return false;
#endif

	if ( RawMinPrefixLen()==0 && m_dPrefixFields.GetLength()!=0 )
	{
		sWarning = "min_prefix_len=0, prefix_fields ignored";
		m_dPrefixFields.Reset();
	}

	if ( m_iMinInfixLen==0 && m_dInfixFields.GetLength()!=0 )
	{
		sWarning = "min_infix_len=0, infix_fields ignored";
		m_dInfixFields.Reset();
	}

	m_dPrefixFields.Uniq();
	m_dInfixFields.Uniq();

	for ( const auto & sField : m_dPrefixFields )
		if ( m_dInfixFields.Contains(sField) )
		{
			sError.SetSprintf ( "field '%s' marked both as prefix and infix", sField.cstr() );
			return false;
		}

	if ( m_iMaxSubstringLen && m_iMaxSubstringLen<m_iMinInfixLen )
	{
		sError.SetSprintf ( "max_substring_len=%d is less than min_infix_len=%d", m_iMaxSubstringLen, m_iMinInfixLen );
		return false;
	}

	if ( m_iMaxSubstringLen && m_iMaxSubstringLen<RawMinPrefixLen() )
	{
		sError.SetSprintf ( "max_substring_len=%d is less than min_prefix_len=%d", m_iMaxSubstringLen, RawMinPrefixLen() );
		return false;
	}

	if ( !ParseDocstoreSettings ( hIndex, sWarning, sError ) )
		return false;

	CSphString sIndexType = hIndex.GetStr ( "dict", "keywords" );

	bool bWordDict = true;
	if ( sIndexType=="crc" )
		bWordDict = false;
	else if ( sIndexType!="keywords" )
	{
		sError.SetSprintf ( "index '%s': unknown dict=%s; only 'keywords' or 'crc' values allowed", szIndexName, sIndexType.cstr() );
		return false;
	}

	if ( hIndex("type") && hIndex["type"]=="rt" && ( m_iMinInfixLen>0 || RawMinPrefixLen()>0 ) && !bWordDict )
	{
		sError.SetSprintf ( "RT indexes support prefixes and infixes with only dict=keywords" );
		return false;
	}

	if ( bWordDict && m_iMaxSubstringLen>0 )
	{
		sError.SetSprintf ( "max_substring_len can not be used with dict=keywords" );
		return false;
	}

	// the only way we could have both prefixes and infixes enabled is when specific field subsets are configured
	if ( !bWordDict && m_iMinInfixLen>0 && RawMinPrefixLen()>0
		&& ( !m_dPrefixFields.GetLength() || !m_dInfixFields.GetLength() ) )
	{
		sError.SetSprintf ( "prefixes and infixes can not both be enabled on all fields" );
		return false;
	}

	// html stripping
	if ( hIndex ( "html_strip" ) )
	{
		m_bHtmlStrip = hIndex.GetInt ( "html_strip" )!=0;
		m_sHtmlIndexAttrs = hIndex.GetStr ( "html_index_attrs" );
		m_sHtmlRemoveElements = hIndex.GetStr ( "html_remove_elements" );
	}

	// hit format
	// TODO! add the description into documentation.
	m_eHitFormat = SPH_HIT_FORMAT_INLINE;
	if ( hIndex("hit_format") )
	{
		if ( hIndex["hit_format"]=="plain" )		m_eHitFormat = SPH_HIT_FORMAT_PLAIN;
		else if ( hIndex["hit_format"]=="inline" )	m_eHitFormat = SPH_HIT_FORMAT_INLINE;
		else
			sWarning.SetSprintf ( "unknown hit_format=%s, defaulting to inline", hIndex["hit_format"].cstr() );
	}

	// hit-less indices
	if ( hIndex("hitless_words") )
	{
		const CSphString & sValue = hIndex["hitless_words"].strval();
		if ( sValue=="all" )
			m_eHitless = SPH_HITLESS_ALL;
		else
		{
			m_eHitless = SPH_HITLESS_SOME;
			m_sHitlessFiles = sValue;
		}
	}

	// sentence and paragraph indexing
	m_bIndexSP = ( hIndex.GetInt ( "index_sp" )!=0 );
	m_sZones = hIndex.GetStr ( "index_zones" );

	// bigrams
	m_eBigramIndex = SPH_BIGRAM_NONE;
	if ( hIndex("bigram_index") )
	{
		CSphString s = hIndex["bigram_index"].strval();
		s.ToLower();
		if ( s=="all" )
			m_eBigramIndex = SPH_BIGRAM_ALL;
		else if ( s=="first_freq" )
			m_eBigramIndex = SPH_BIGRAM_FIRSTFREQ;
		else if ( s=="both_freq" )
			m_eBigramIndex = SPH_BIGRAM_BOTHFREQ;
		else
		{
			sError.SetSprintf ( "unknown bigram_index=%s (must be all, first_freq, or both_freq)", s.cstr() );
			return false;
		}
	}

	m_sBigramWords = hIndex.GetStr ( "bigram_freq_words" );
	m_sBigramWords.Trim();

	bool bEmptyOk = m_eBigramIndex==SPH_BIGRAM_NONE || m_eBigramIndex==SPH_BIGRAM_ALL;
	if ( bEmptyOk!=m_sBigramWords.IsEmpty() )
	{
		sError.SetSprintf ( "bigram_index=%s, bigram_freq_words must%s be empty", hIndex["bigram_index"].cstr(),
			bEmptyOk ? "" : " not" );
		return false;
	}

	// aot
	StrVec_t dMorphs;
	sphSplit ( dMorphs, hIndex.GetStr ( "morphology" ).cstr() );

	m_uAotFilterMask = 0;
	for ( int j=0; j<AOT_LENGTH; ++j )
	{
		char buf_all[20];
		sprintf ( buf_all, "lemmatize_%s_all", AOT_LANGUAGES[j] ); //NOLINT
		ARRAY_FOREACH ( i, dMorphs )
			if ( dMorphs[i]==buf_all )
			{
				m_uAotFilterMask |= (1UL) << j;
				break;
			}
	}

	m_ePreprocessor = dMorphs.Contains ( "icu_chinese" ) ? Preprocessor_e::ICU : Preprocessor_e::NONE;

	if ( !sphCheckConfigICU ( *this, sError ) )
		return false;

	// all good
	return true;
}


void CSphIndexSettings::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	tOut.Add ( "min_prefix_len",		RawMinPrefixLen(),		RawMinPrefixLen()!=0 );
	tOut.Add ( "min_infix_len",			m_iMinInfixLen,			m_iMinInfixLen!=0 );
	tOut.Add ( "max_substring_len",		m_iMaxSubstringLen,		m_iMaxSubstringLen!=0 );
	tOut.Add ( "index_exact_words",		1,						m_bIndexExactWords );
	tOut.Add ( "html_strip",			1,						m_bHtmlStrip );
	tOut.Add ( "html_index_attrs",		m_sHtmlIndexAttrs,		!m_sHtmlIndexAttrs.IsEmpty() );
	tOut.Add ( "html_remove_elements",	m_sHtmlRemoveElements,	!m_sHtmlRemoveElements.IsEmpty() );
	tOut.Add ( "index_zones",			m_sZones,				!m_sZones.IsEmpty() );
	tOut.Add ( "index_field_lengths",	1,						m_bIndexFieldLens );
	tOut.Add ( "index_sp",				1,						m_bIndexSP );
	tOut.Add ( "phrase_boundary_step",	m_iBoundaryStep,		m_iBoundaryStep!=0 );
	tOut.Add ( "stopword_step",			m_iStopwordStep,		m_iStopwordStep!=1 );
	tOut.Add ( "overshort_step",		m_iOvershortStep,		m_iOvershortStep!=1 );
	tOut.Add ( "bigram_index",			BigramName(m_eBigramIndex), m_eBigramIndex!=SPH_BIGRAM_NONE );
	tOut.Add ( "bigram_freq_words",		m_sBigramWords,			!m_sBigramWords.IsEmpty() );
	tOut.Add ( "index_token_filter",	m_sIndexTokenFilter,	!m_sIndexTokenFilter.IsEmpty() );
	tOut.Add ( "attr_update_reserve",	m_tBlobUpdateSpace,		m_tBlobUpdateSpace!=DEFAULT_ATTR_UPDATE_RESERVE );

	if ( m_eHitless==SPH_HITLESS_ALL )
	{
		tOut.Add ( "hitless_words",		"all",					true );
	} else if ( m_eHitless==SPH_HITLESS_SOME )
	{
		CSphString sHitlessFiles = FormatPath ( m_sHitlessFiles, pFilenameBuilder );
		tOut.Add ( "hitless_words",		sHitlessFiles,			true );
	}

	DocstoreSettings_t::Format ( tOut, pFilenameBuilder );
}

//////////////////////////////////////////////////////////////////////////

void FileAccessSettings_t::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	FileAccessSettings_t tDefault;

	tOut.Add ( "read_buffer_docs",		m_iReadBufferDocList,		m_iReadBufferDocList!=tDefault.m_iReadBufferDocList );
	tOut.Add ( "read_buffer_hits",		m_iReadBufferHitList,		m_iReadBufferHitList!=tDefault.m_iReadBufferHitList );

	tOut.Add ( "access_doclists",		FileAccessName(m_eDoclist),		m_eDoclist!=tDefault.m_eDoclist );
	tOut.Add ( "access_hitlists",		FileAccessName(m_eHitlist),		m_eHitlist!=tDefault.m_eHitlist );
	tOut.Add ( "access_plain_attrs",	FileAccessName(m_eAttr) ,		m_eAttr!=tDefault.m_eAttr );
	tOut.Add ( "access_blob_attrs",		FileAccessName(m_eBlob) ,		m_eBlob!=tDefault.m_eBlob );
}

//////////////////////////////////////////////////////////////////////////

static StrVec_t SplitArg ( const CSphString & sValue, StrVec_t & dFiles )
{
	StrVec_t dValues = sphSplit ( sValue.cstr(), sValue.Length(), " \t," );
	for ( auto & i : dValues )
	{
		dFiles.Add ( i.Trim() );
		StripPath(i);
	}

	return dValues;
}


bool IndexSettingsContainer_c::AddOption ( const CSphString & sName, const CSphString & sValue )
{
	if ( sName=="type" && sValue=="pq" )
	{
		CSphString sNewValue = "percolate";
		return Add ( sName, sNewValue );
	}

	if ( sName=="stopwords" )
	{
		RemoveKeys(sName);
		m_dStopwordFiles.Reset();

		StrVec_t dValues = SplitArg ( sValue, m_dStopwordFiles );

		// m_dStopwordFiles now holds the files with olds paths
		// there's a hack in stopword loading code that modifies the folder to pre-installed
		// let's use this hack here and copy that file too so that we're fully standalone
		for ( auto & i : m_dStopwordFiles )
			if ( !sphIsReadable(i) )
			{
				CSphString sFilename = i;
				sFilename.SetSprintf ( "%s/stopwords/%s", GET_FULL_SHARE_DIR (), StripPath(sFilename).cstr() );
				if ( sphIsReadable ( sFilename.cstr() ) )
					i = sFilename;
			}

		StringBuilder_c sNewValue = " ";
		for ( auto & i : dValues )
			sNewValue << i;

		return Add ( sName, sNewValue.cstr() );
	}

	if ( sName=="exceptions" )
	{
		RemoveKeys(sName);
		m_dExceptionFiles.Reset();

		StrVec_t dValues = SplitArg ( sValue, m_dExceptionFiles );
		if ( dValues.GetLength()>1 )
		{
			m_sError = "'exceptions' options only supports a single file";
			return false;
		}

		return Add ( sName, dValues[0] );
	}

	if ( sName=="wordforms" )
	{
		RemoveKeys(sName);
		m_dWordformFiles.Reset();

		StrVec_t dValues = SplitArg ( sValue, m_dWordformFiles );
		for ( auto & i : dValues )
			Add ( sName, i );

		return true;
	}

	if ( sName=="hitless_words" && ( sValue!="none" && sValue!="all" ) )
	{
		RemoveKeys ( sName );
		m_dHitlessFiles.Reset();
		StrVec_t dValues = SplitArg ( sValue, m_dHitlessFiles );

		// need only names for hitless files
		StringBuilder_c sTmp ( " " );
		for ( const CSphString & sVal : dValues )
			sTmp << sVal;

		return Add ( sName, sTmp.cstr() );

	}

	return Add ( sName, sValue );
}


void IndexSettingsContainer_c::RemoveKeys ( const CSphString & sName )
{
	m_hCfg.Delete(sName);
}


bool IndexSettingsContainer_c::Populate ( const CreateTableSettings_t & tCreateTable )
{
	StringBuilder_c sStoredFields(",");
	StringBuilder_c sStoredOnlyFields(",");
	for ( const auto & i : tCreateTable.m_dFields )
	{
		Add ( "rt_field", i.m_sName );

		DWORD uFlags = i.m_uFieldFlags;
		if ( !uFlags )
			uFlags = CSphColumnInfo::FIELD_INDEXED | CSphColumnInfo::FIELD_STORED;

		if ( uFlags==CSphColumnInfo::FIELD_STORED )
			sStoredOnlyFields << i.m_sName;
		else if ( uFlags & CSphColumnInfo::FIELD_STORED )
			sStoredFields << i.m_sName;
	}

	if ( sStoredFields.GetLength() )
		Add ( "stored_fields", sStoredFields.cstr() );

	if ( sStoredOnlyFields.GetLength() )
		Add ( "stored_only_fields", sStoredOnlyFields.cstr() );

	for ( const auto & i : tCreateTable.m_dAttrs )
		for ( const auto & j : g_dRtTypedAttrs )
			if ( i.m_eAttrType==j.m_eType )
			{
				CSphString sValue;
				if ( i.m_eAttrType==SPH_ATTR_INTEGER && i.m_tLocator.m_iBitCount!=-1 )
					sValue.SetSprintf ( "%s:%d", i.m_sName.cstr(), i.m_tLocator.m_iBitCount );
				else
					sValue = i.m_sName;

				Add ( j.m_szName, sValue );
				break;
			}

	for ( const auto & i : tCreateTable.m_dOpts )
		if ( !AddOption ( i.m_sName, i.m_sValue ) )
			return false;

	if ( !Contains("type") )
		Add ( "type", "rt" );


	bool bDistributed = Get("type")=="distributed";
	if ( !bDistributed )
		Add ( "embedded_limit", "0" );

	SetDefaults();

	if ( !CheckPaths() )
		return false;

	return true;
}


bool IndexSettingsContainer_c::Add ( const char * szName, const CSphString & sValue )
{
	// same behavior as an ordinary config parser
	m_hCfg.AddEntry ( szName, sValue.cstr() );
	return true;
}


bool IndexSettingsContainer_c::Add ( const CSphString & sName, const CSphString & sValue )
{
	return Add ( sName.cstr(), sValue );
}


CSphString IndexSettingsContainer_c::Get ( const CSphString & sName ) const
{
	if ( !Contains ( sName.cstr() ) )
		return "";

	return m_hCfg[sName].strval();
}


bool IndexSettingsContainer_c::Contains ( const char * szName ) const
{
	return m_hCfg.Exists(szName);
}


StrVec_t IndexSettingsContainer_c::GetFiles() const
{
	StrVec_t dFiles;
	for ( const auto & i : m_dStopwordFiles )
		dFiles.Add(i);

	for ( const auto & i : m_dExceptionFiles )
		dFiles.Add(i);

	for ( const auto & i : m_dWordformFiles )
	{
		StrVec_t dFilesFound = FindFiles ( i.cstr() );
		for ( const auto & j : dFilesFound )
			dFiles.Add(j);
	}

	for ( const auto & i : m_dHitlessFiles )
		dFiles.Add ( i );

	return dFiles;
}


const CSphConfigSection & IndexSettingsContainer_c::AsCfg() const
{
	return m_hCfg;
}

// TODO: read defaults from file or predefined templates
static std::pair<const char* , const char *> g_dIndexSettingsDefaults[] =
{
	{ "charset_table", "non_cjk" }
};

void IndexSettingsContainer_c::SetDefaults()
{
	for ( const auto & tItem : g_dIndexSettingsDefaults )
	{
		if ( !m_hCfg.Exists ( tItem.first ) )
			Add ( tItem.first, tItem.second );
	}
}


bool IndexSettingsContainer_c::CheckPaths()
{
	StrVec_t dFiles = GetFiles();
	for ( const auto & i : dFiles )
	{
		if ( !sphIsReadable(i) )
		{
			m_sError.SetSprintf ( "file not found: '%s'", i.cstr() );
			return false;
		}

		if ( !IsPathAbsolute(i) )
		{
			m_sError.SetSprintf ( "paths to external files should be absolute: '%s'" , i.cstr() );
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

static void WriteFileInfo ( CSphWriter & tWriter, const CSphSavedFile & tInfo )
{
	tWriter.PutOffset ( tInfo.m_uSize );
	tWriter.PutOffset ( tInfo.m_uCTime );
	tWriter.PutOffset ( tInfo.m_uMTime );
	tWriter.PutDword ( tInfo.m_uCRC32 );
}


/// gets called from and MUST be in sync with RtIndex_c::SaveDiskHeader()!
/// note that SaveDiskHeader() occasionaly uses some PREVIOUS format version!
void SaveTokenizerSettings ( CSphWriter & tWriter, const ISphTokenizer * pTokenizer, int iEmbeddedLimit )
{
	assert ( pTokenizer );

	const CSphTokenizerSettings & tSettings = pTokenizer->GetSettings ();
	tWriter.PutByte ( tSettings.m_iType );
	tWriter.PutString ( tSettings.m_sCaseFolding.cstr() );
	tWriter.PutDword ( tSettings.m_iMinWordLen );

	bool bEmbedSynonyms = ( iEmbeddedLimit>0 && pTokenizer->GetSynFileInfo ().m_uSize<=(SphOffset_t)iEmbeddedLimit );
	tWriter.PutByte ( bEmbedSynonyms ? 1 : 0 );
	if ( bEmbedSynonyms )
		pTokenizer->WriteSynonyms ( tWriter );

	tWriter.PutString ( tSettings.m_sSynonymsFile.cstr() );
	WriteFileInfo ( tWriter, pTokenizer->GetSynFileInfo () );
	tWriter.PutString ( tSettings.m_sBoundary.cstr() );
	tWriter.PutString ( tSettings.m_sIgnoreChars.cstr() );
	tWriter.PutDword ( tSettings.m_iNgramLen );
	tWriter.PutString ( tSettings.m_sNgramChars.cstr() );
	tWriter.PutString ( tSettings.m_sBlendChars.cstr() );
	tWriter.PutString ( tSettings.m_sBlendMode.cstr() );
}


/// gets called from and MUST be in sync with RtIndex_c::SaveDiskHeader()!
/// note that SaveDiskHeader() occasionaly uses some PREVIOUS format version!
void SaveDictionarySettings ( CSphWriter & tWriter, const CSphDict * pDict, bool bForceWordDict, int iEmbeddedLimit )
{
	assert ( pDict );
	const CSphDictSettings & tSettings = pDict->GetSettings ();

	tWriter.PutString ( tSettings.m_sMorphology.cstr() );
	tWriter.PutString ( tSettings.m_sMorphFields.cstr() );

	const CSphVector <CSphSavedFile> & dSWFileInfos = pDict->GetStopwordsFileInfos ();
	SphOffset_t uTotalSize = 0;
	ARRAY_FOREACH ( i, dSWFileInfos )
		uTotalSize += dSWFileInfos[i].m_uSize;

	// embed only in case it allowed
	bool bEmbedStopwords = ( iEmbeddedLimit>0 && uTotalSize<=(SphOffset_t)iEmbeddedLimit );
	tWriter.PutByte ( bEmbedStopwords ? 1 : 0 );
	if ( bEmbedStopwords )
		pDict->WriteStopwords ( tWriter );

	tWriter.PutString ( tSettings.m_sStopwords.cstr() );
	tWriter.PutDword ( dSWFileInfos.GetLength () );
	ARRAY_FOREACH ( i, dSWFileInfos )
	{
		tWriter.PutString ( dSWFileInfos[i].m_sFilename.cstr() );
		WriteFileInfo ( tWriter, dSWFileInfos[i] );
	}

	const CSphVector <CSphSavedFile> & dWFFileInfos = pDict->GetWordformsFileInfos ();
	uTotalSize = 0;
	ARRAY_FOREACH ( i, dWFFileInfos )
		uTotalSize += dWFFileInfos[i].m_uSize;

	bool bEmbedWordforms = uTotalSize<=(SphOffset_t)iEmbeddedLimit;
	tWriter.PutByte ( bEmbedWordforms ? 1 : 0 );
	if ( bEmbedWordforms )
		pDict->WriteWordforms ( tWriter );

	tWriter.PutDword ( dWFFileInfos.GetLength() );
	ARRAY_FOREACH ( i, dWFFileInfos )
	{
		tWriter.PutString ( tSettings.m_dWordforms[i] );
		WriteFileInfo ( tWriter, dWFFileInfos[i] );
	}

	tWriter.PutDword ( tSettings.m_iMinStemmingLen );
	tWriter.PutByte ( tSettings.m_bWordDict || bForceWordDict );
	tWriter.PutByte ( tSettings.m_bStopwordsUnstemmed );
	tWriter.PutString ( pDict->GetMorphDataFingerprint() );
}

//////////////////////////////////////////////////////////////////////////

static void FormatAllSettings ( const CSphIndex & tIndex, SettingsFormatter_c & tFormatter, FilenameBuilder_i * pFilenameBuilder )
{
	if ( tIndex.IsPQ() )
		tFormatter.Add ( "type", "pq", true );

	tIndex.GetSettings().Format ( tFormatter, pFilenameBuilder );

	CSphFieldFilterSettings tFieldFilter;
	tIndex.GetFieldFilterSettings ( tFieldFilter );
	tFieldFilter.Format ( tFormatter, pFilenameBuilder );

	KillListTargets_c tKlistTargets;
	CSphString sWarning;
	if ( !tIndex.LoadKillList ( nullptr, tKlistTargets, sWarning ) )
		tKlistTargets.m_dTargets.Reset();

	tKlistTargets.Format ( tFormatter, pFilenameBuilder );

	if ( tIndex.GetTokenizer() )
		tIndex.GetTokenizer()->GetSettings().Format ( tFormatter, pFilenameBuilder );

	if ( tIndex.GetDictionary() )
		tIndex.GetDictionary()->GetSettings().Format ( tFormatter, pFilenameBuilder );

	tIndex.GetMutableSettings().Format ( tFormatter, pFilenameBuilder );
}


// fixme! this is basically a duplicate of the above function, but has extra code due to embedded
void DumpReadable ( FILE * fp, const CSphIndex & tIndex, const CSphEmbeddedFiles & tEmbeddedFiles, FilenameBuilder_i * pFilenameBuilder )
{
	SettingsFormatterState_t tState(fp);
	tIndex.GetSettings().DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );

	CSphFieldFilterSettings tFieldFilter;
	tIndex.GetFieldFilterSettings ( tFieldFilter );
	tFieldFilter.DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );

	KillListTargets_c tKlistTargets;
	CSphString sWarning;
	if ( !tIndex.LoadKillList ( nullptr, tKlistTargets, sWarning ) )
		tKlistTargets.m_dTargets.Reset();

	tKlistTargets.DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );

	if ( tIndex.GetTokenizer() )
		tIndex.GetTokenizer()->GetSettings().DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );

	if ( tIndex.GetDictionary() )
		tIndex.GetDictionary()->GetSettings().DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );

	tIndex.GetMutableSettings().m_tFileAccess.DumpReadable ( tState, tEmbeddedFiles, pFilenameBuilder );
}


void DumpSettings ( StringBuilder_c & tBuf, const CSphIndex & tIndex, FilenameBuilder_i * pFilenameBuilder )
{
	SettingsFormatterState_t tState(tBuf);
	SettingsFormatter_c tFormatter ( tState, "", " = ", "", "\n" );
	FormatAllSettings ( tIndex, tFormatter, pFilenameBuilder );
}


void DumpSettingsCfg ( FILE * fp, const CSphIndex & tIndex, FilenameBuilder_i * pFilenameBuilder )
{
	SettingsFormatterState_t tState(fp);
	SettingsFormatter_c tFormatter ( tState, "\t", " = ", "", "\n" );
	FormatAllSettings ( tIndex, tFormatter, pFilenameBuilder );
}


static void DumpCreateTable ( StringBuilder_c & tBuf, const CSphIndex & tIndex, FilenameBuilder_i * pFilenameBuilder )
{
	SettingsFormatterState_t tState(tBuf);
	SettingsFormatter_c tFormatter ( tState, "", "='", "'", " " );
	FormatAllSettings ( tIndex, tFormatter, pFilenameBuilder );
}

//////////////////////////////////////////////////////////////////////////

static void AddWarning ( StrVec_t & dWarnings, const CSphString & sWarning )
{
	if ( !sWarning.IsEmpty() )
		dWarnings.Add(sWarning);
}


bool sphFixupIndexSettings ( CSphIndex * pIndex, const CSphConfigSection & hIndex, bool bStripPath, FilenameBuilder_i * pFilenameBuilder, StrVec_t & dWarnings, CSphString & sError )
{
	bool bTokenizerSpawned = false;

	if ( !pIndex->GetTokenizer () )
	{
		CSphTokenizerSettings tSettings;
		CSphString sWarning;
		tSettings.Setup ( hIndex, sWarning );
		AddWarning ( dWarnings, sWarning );

		TokenizerRefPtr_c pTokenizer { ISphTokenizer::Create ( tSettings, nullptr, pFilenameBuilder, dWarnings, sError ) };
		if ( !pTokenizer )
			return false;

		bTokenizerSpawned = true;
		pIndex->SetTokenizer ( pTokenizer );
	}

	if ( !pIndex->GetDictionary () )
	{
		DictRefPtr_c pDict;
		CSphDictSettings tSettings;
		CSphString sWarning;
		tSettings.Setup ( hIndex, pFilenameBuilder, sWarning );
		AddWarning ( dWarnings, sWarning );

		pDict = sphCreateDictionaryCRC ( tSettings, nullptr, pIndex->GetTokenizer (), pIndex->GetName(), bStripPath, pIndex->GetSettings().m_iSkiplistBlockSize, pFilenameBuilder, sError );
		if ( !pDict )
			return false;

		pIndex->SetDictionary ( pDict );
	}

	if ( bTokenizerSpawned )
	{
		TokenizerRefPtr_c pOldTokenizer { pIndex->LeakTokenizer () };
		TokenizerRefPtr_c pMultiTokenizer { ISphTokenizer::CreateMultiformFilter ( pOldTokenizer, pIndex->GetDictionary ()->GetMultiWordforms () ) };
		pIndex->SetTokenizer ( pMultiTokenizer );
	}

	pIndex->SetupQueryTokenizer();

	if ( !pIndex->IsStripperInited () )
	{
		CSphIndexSettings tSettings = pIndex->GetSettings ();

		if ( hIndex ( "html_strip" ) )
		{
			tSettings.m_bHtmlStrip = hIndex.GetInt ( "html_strip" )!=0;
			tSettings.m_sHtmlIndexAttrs = hIndex.GetStr ( "html_index_attrs" );
			tSettings.m_sHtmlRemoveElements = hIndex.GetStr ( "html_remove_elements" );
		}
		tSettings.m_sZones = hIndex.GetStr ( "index_zones" );

		pIndex->Setup ( tSettings );
	}

	if ( !pIndex->GetFieldFilter() )
	{
		FieldFilterRefPtr_c pFieldFilter;
		CSphFieldFilterSettings tFilterSettings;
		bool bSetupOk = tFilterSettings.Setup ( hIndex, sError );

		// treat warnings as errors
		if ( !sError.IsEmpty() )
			return false;

		if ( bSetupOk )
		{
			CSphString sWarning;
			pFieldFilter = sphCreateRegexpFilter ( tFilterSettings, sWarning );
			AddWarning ( dWarnings, sWarning );
		}

		CSphString sWarning;
		sphSpawnFilterICU ( pFieldFilter, pIndex->GetSettings(), pIndex->GetTokenizer()->GetSettings(), pIndex->GetName(), sWarning );
		AddWarning ( dWarnings, sWarning );

		pIndex->SetFieldFilter ( pFieldFilter );
	}

	// exact words fixup, needed for RT indexes
	// cloned from indexer, remove somehow?
	DictRefPtr_c pDict { pIndex->GetDictionary() };
	SafeAddRef ( pDict );
	assert ( pDict );

	CSphIndexSettings tSettings = pIndex->GetSettings ();
	bool bNeedExact = ( pDict->HasMorphology() || pDict->GetWordformsFileInfos().GetLength() );
	if ( tSettings.m_bIndexExactWords && !bNeedExact )
	{
		tSettings.m_bIndexExactWords = false;
		pIndex->Setup ( tSettings );
		dWarnings.Add ( "no morphology, index_exact_words=1 has no effect, ignoring" );
	}

	if ( pDict->GetSettings().m_bWordDict && pDict->HasMorphology() &&
		( tSettings.RawMinPrefixLen() || tSettings.m_iMinInfixLen || !pDict->GetSettings().m_sMorphFields.IsEmpty() ) && !tSettings.m_bIndexExactWords )
	{
		tSettings.m_bIndexExactWords = true;
		pIndex->Setup ( tSettings );
		dWarnings.Add ( "dict=keywords and prefixes and morphology enabled, forcing index_exact_words=1" );
	}

	pIndex->PostSetup();
	return true;
}


static RtTypedAttr_t g_dTypeNames[] =
{
	{ SPH_ATTR_INTEGER,		"integer" },
	{ SPH_ATTR_BIGINT,		"bigint" },
	{ SPH_ATTR_FLOAT,		"float" },
	{ SPH_ATTR_BOOL,		"bool" },
	{ SPH_ATTR_UINT32SET,	"multi" },
	{ SPH_ATTR_INT64SET,	"multi64" },
	{ SPH_ATTR_JSON,		"json" },
	{ SPH_ATTR_STRING,		"string" },
	{ SPH_ATTR_STRINGPTR,	"string" },
	{ SPH_ATTR_TIMESTAMP,	"timestamp" }
};


static CSphString GetAttrTypeName ( const CSphColumnInfo & tAttr )
{
	if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER && tAttr.m_tLocator.m_iBitCount!=32 )
	{
		CSphString sRes;
		sRes.SetSprintf( "bit(%d)", tAttr.m_tLocator.m_iBitCount );
		return sRes;
	}

	for ( const auto & i : g_dTypeNames )
		if ( tAttr.m_eAttrType==i.m_eType )
			return i.m_szName;

	assert ( 0 && "Internal error: unknown attr type" );
	return "";
}


static void AddFieldSettings ( StringBuilder_c & sRes, const CSphColumnInfo & tField )
{
	DWORD uAllSet = CSphColumnInfo::FIELD_INDEXED | CSphColumnInfo::FIELD_STORED;
	if ( (tField.m_uFieldFlags & uAllSet) != uAllSet )
	{
		if ( tField.m_uFieldFlags & CSphColumnInfo::FIELD_INDEXED )
			sRes << " indexed";

		if ( tField.m_uFieldFlags & CSphColumnInfo::FIELD_STORED )
			sRes << " stored";
	}
}


CSphString BuildCreateTable ( const CSphString & sName, const CSphIndex * pIndex, const CSphSchema & tSchema )
{
	assert ( pIndex );

	StringBuilder_c sRes;
	sRes << "CREATE TABLE " << sName << " (\n";

	CSphVector<const CSphColumnInfo *> dExclude;

	bool bHasAttrs = false;
	for ( int i = 0; i < tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr(i);
		if ( sphIsInternalAttr ( tAttr.m_sName ) || tAttr.m_sName==sphGetDocidName() )
			continue;

		if ( bHasAttrs )
			sRes << ",\n";

		const CSphColumnInfo * pField = tSchema.GetField ( tAttr.m_sName.cstr() );
		if ( pField && tAttr.m_eAttrType==SPH_ATTR_STRING )
		{
			sRes << tAttr.m_sName << " " << GetAttrTypeName(tAttr) << " attribute";
			AddFieldSettings ( sRes, *pField );
			dExclude.Add(pField);
		}
		else
			sRes << tAttr.m_sName << " " << GetAttrTypeName(tAttr);

		bHasAttrs = true;
	}

	dExclude.Uniq();

	for ( int i = 0; i < tSchema.GetFieldsCount(); i++ )
	{
		const CSphColumnInfo & tField = tSchema.GetField(i);

		if ( dExclude.BinarySearch(&tField) )
			continue;

		if ( i || bHasAttrs )
			sRes << ",\n";

		sRes << tField.m_sName << " text";
		AddFieldSettings ( sRes, tField );
	}

	sRes << "\n)";

	StringBuilder_c tBuf;

	CSphScopedPtr<FilenameBuilder_i> pFilenameBuilder ( g_fnCreateFilenameBuilder ? g_fnCreateFilenameBuilder ( pIndex->GetName() ) : nullptr );
	DumpCreateTable ( tBuf, *pIndex, pFilenameBuilder.Ptr() );

	if ( tBuf.GetLength() )
		sRes << " " << tBuf.cstr();

	CSphString sResult = sRes.cstr();
	return sResult;
}


const char * FileAccessName ( FileAccess_e eValue )
{
	switch ( eValue )
	{
	case FileAccess_e::FILE : return "file";
	case FileAccess_e::MMAP : return "mmap";
	case FileAccess_e::MMAP_PREREAD : return "mmap_preread";
	case FileAccess_e::MLOCK : return "mlock";
	case FileAccess_e::UNKNOWN : return "unknown";
	default:
		assert ( 0 && "Not all values of FileAccess_e named");
		return "";
	}
}


FileAccess_e ParseFileAccess ( CSphString sVal )
{
	if ( sVal=="file" ) return FileAccess_e::FILE;
	if ( sVal=="mmap" )	return FileAccess_e::MMAP;
	if ( sVal=="mmap_preread" ) return FileAccess_e::MMAP_PREREAD;
	if ( sVal=="mlock" ) return FileAccess_e::MLOCK;
	return FileAccess_e::UNKNOWN;
}

int ParseKeywordExpansion ( const char * sValue )
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

enum class MutableName_e
{
	EXPAND_KEYWORDS,
	RT_MEM_LIMIT,
	PREOPEN,
	ACCESS_PLAIN_ATTRS,
	ACCESS_BLOB_ATTRS,
	ACCESS_DOCLISTS,
	ACCESS_HITLISTS,
	READ_BUFFER_DOCS,
	READ_BUFFER_HITS,

	TOTAL
};

const char * GetMutableName ( MutableName_e eName )
{
	switch ( eName ) 
	{
		case MutableName_e::EXPAND_KEYWORDS: return "expand_keywords";
		case MutableName_e::RT_MEM_LIMIT: return "rt_mem_limit";
		case MutableName_e::PREOPEN: return "preopen";
		case MutableName_e::ACCESS_PLAIN_ATTRS: return "access_plain_attrs";
		case MutableName_e::ACCESS_BLOB_ATTRS: return "access_blob_attrs";
		case MutableName_e::ACCESS_DOCLISTS: return "access_doclists";
		case MutableName_e::ACCESS_HITLISTS: return "access_hitlists";
		case MutableName_e::READ_BUFFER_DOCS: return "read_buffer_docs";
		case MutableName_e::READ_BUFFER_HITS: return "read_buffer_hits";
		default: assert ( 0 && "Invalid mutable option" ); return "";
	}
}

static bool GetFileAccess ( const CSphString & sVal, const char * sKey, bool bList, FileAccess_e & eRes )
{
	// should use original value as default due to deprecated options
	if ( sVal.IsEmpty() )
		return false;

	FileAccess_e eParsed = ParseFileAccess ( sVal.cstr() );
	if ( eParsed==FileAccess_e::UNKNOWN )
	{
		sphWarning( "%s unknown value %s, use default %s", sKey, sVal.cstr(), FileAccessName( eRes ) );
		return false;
	}

	// but then check might reset invalid value to real default
	if ( ( bList && eParsed==FileAccess_e::MMAP_PREREAD) ||
		( !bList && eParsed==FileAccess_e::FILE) )
	{
		sphWarning( "%s invalid value %s, use default %s", sKey, FileAccessName ( eParsed ), FileAccessName ( eRes ));
		return false;
	}

	eRes = eParsed;
	return true;
}

FileAccess_e GetFileAccess (  const CSphConfigSection & hIndex, const char * sKey, bool bList, FileAccess_e eDefault )
{
	FileAccess_e eRes = eDefault;
	if ( !GetFileAccess ( hIndex.GetStr ( sKey ), sKey, bList, eRes ) )
		return eDefault;

	return eRes;
}

static void GetFileAccess ( const JsonObj_c & tSetting, MutableName_e eName, bool bList, FileAccess_e & eRes, CSphBitvec & dLoaded )
{
	const char * sName = GetMutableName ( eName );

	CSphString sError;
	JsonObj_c tVal = tSetting.GetStrItem ( sName, sError, true );
	if ( !tVal )
	{
		if ( !sError.IsEmpty() )
			sphWarning ( "%s", sError.cstr() );
		return;
	}

	if ( !GetFileAccess ( tVal.StrVal(), sName, bList, eRes ) )
		return;

	dLoaded.BitSet ( (int)eName );
}

static void GetFileAccess (  const CSphConfigSection & hIndex, MutableName_e eName, bool bList, FileAccess_e & eRes, CSphBitvec & dLoaded )
{
	const char * sName = GetMutableName ( eName );
	if ( !GetFileAccess ( hIndex.GetStr ( sName ), sName, bList, eRes ) )
		return;

	dLoaded.BitSet ( (int)eName );
}

MutableIndexSettings_c::MutableIndexSettings_c()
	: m_iExpandKeywords { KWE_DISABLED }
	, m_iMemLimit { DEFAULT_RT_MEM_LIMIT }
	, m_dLoaded ( (int)MutableName_e::TOTAL )
{
#if !USE_WINDOWS
	m_bPreopen	= true;
#else
	m_bPreopen	= false;
#endif
}

static int64_t GetMemLimit ( int64_t iMemLimit, StrVec_t * pWarnings )
{
	if ( iMemLimit<128 * 1024 )
	{
		if ( pWarnings )
			pWarnings->Add ( "rt_mem_limit extremely low, using 128K instead" );
		else
			sphWarning ( "rt_mem_limit extremely low, using 128K instead" );
		iMemLimit = 128 * 1024;
	} else if ( iMemLimit<8 * 1024 * 1024 )
	{
		if ( pWarnings )
			pWarnings->Add ( "rt_mem_limit very low (under 8 MB)" );
		else
			sphWarning ( "rt_mem_limit very low (under 8 MB)" );
	}

	return iMemLimit;
}

bool MutableIndexSettings_c::Load ( const char * sFileName, const char * sIndexName )
{
	CSphString sError;
	CSphAutofile tReader;
	int iFD = tReader.Open ( sFileName, SPH_O_READ, sError );
	if ( iFD<0 ) // mutable settings is optional file - no need to fail
		return true;

	int64_t iSize = tReader.GetSize();
	if ( !iSize )
		return true;

	CSphFixedVector<BYTE> dBuf ( iSize+1 );
	if ( !tReader.Read ( dBuf.Begin(), iSize, sError ) )
	{
		sphWarning ( "index %s, error: %s", sIndexName, sError.cstr() );
		return false;
	}
	dBuf[iSize] = '\0';

	JsonObj_c tParser ( (const char *)dBuf.Begin() );
	if ( !tParser )
		return false;

	// read values

	JsonObj_c tExpand = tParser.GetStrItem ( "expand_keywords", sError, true );
	if ( tExpand )
	{
		m_iExpandKeywords = ParseKeywordExpansion ( tExpand.StrVal().cstr() );
		m_dLoaded.BitSet ( (int)MutableName_e::EXPAND_KEYWORDS );
	} else if ( !sError.IsEmpty() )
	{
		sphWarning ( "index %s: %s", sIndexName, sError.cstr() );
		sError = "";
	}

	JsonObj_c tMemLimit = tParser.GetIntItem ( "rt_mem_limit", sError, true );
	if ( tMemLimit )
	{
		m_iMemLimit = GetMemLimit ( tMemLimit.IntVal(), nullptr );
		m_dLoaded.BitSet ( (int)MutableName_e::RT_MEM_LIMIT );
	} else if ( !sError.IsEmpty() )
	{
		sphWarning ( "index %s: %s", sIndexName, sError.cstr() );
		sError = "";
	}

	JsonObj_c tPreopen = tParser.GetBoolItem ( "preopen", sError, true );
	if ( tPreopen )
	{
		m_bPreopen = tPreopen.BoolVal();
		m_dLoaded.BitSet ( (int)MutableName_e::PREOPEN );
	} else if ( !sError.IsEmpty() )
	{
		sphWarning ( "index %s: %s", sIndexName, sError.cstr() );
		sError = "";
	}

	GetFileAccess( tParser, MutableName_e::ACCESS_PLAIN_ATTRS, false, m_tFileAccess.m_eAttr, m_dLoaded );
	GetFileAccess( tParser, MutableName_e::ACCESS_BLOB_ATTRS, false, m_tFileAccess.m_eBlob, m_dLoaded );
	GetFileAccess( tParser, MutableName_e::ACCESS_DOCLISTS, true, m_tFileAccess.m_eDoclist, m_dLoaded );
	GetFileAccess( tParser, MutableName_e::ACCESS_HITLISTS, true, m_tFileAccess.m_eHitlist, m_dLoaded );

	JsonObj_c tReadBuffer = tParser.GetIntItem ( "read_buffer_docs", sError, true );
	if ( tReadBuffer )
	{
		m_tFileAccess.m_iReadBufferDocList = GetReadBuffer ( tReadBuffer.IntVal() );
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_DOCS );
	} else if ( !sError.IsEmpty() )
	{
		sphWarning ( "index %s: %s", sIndexName, sError.cstr() );
		sError = "";
	}

	tReadBuffer = tParser.GetIntItem ( "read_buffer_hits", sError, true );
	if ( tReadBuffer )
	{
		m_tFileAccess.m_iReadBufferHitList = GetReadBuffer ( tReadBuffer.IntVal() );
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_HITS );
	} else if ( !sError.IsEmpty() )
	{
		sphWarning ( "index %s: %s", sIndexName, sError.cstr() );
		sError = "";
	}

	m_bNeedSave = true;

	return true;
}

void MutableIndexSettings_c::Load ( const CSphConfigSection & hIndex, bool bNeedSave, StrVec_t * pWarnings )
{
	m_bNeedSave |= bNeedSave;

	if ( hIndex.Exists ( "expand_keywords" ) )
	{
		m_iExpandKeywords = ParseKeywordExpansion ( hIndex.GetStr( "expand_keywords" ).cstr() );
		m_dLoaded.BitSet ( (int)MutableName_e::EXPAND_KEYWORDS );
	}

	// RAM chunk size
	if ( hIndex.Exists ( "rt_mem_limit" ) )
	{
		m_iMemLimit = GetMemLimit ( hIndex.GetSize64 ( "rt_mem_limit", DEFAULT_RT_MEM_LIMIT ), pWarnings );
		m_dLoaded.BitSet ( (int)MutableName_e::RT_MEM_LIMIT );
	}

	if (  hIndex.Exists ( "preopen" )  )
	{
		m_bPreopen = ( hIndex.GetInt ( "preopen", 0 )!=0 );
		m_dLoaded.BitSet ( (int)MutableName_e::PREOPEN );
	}

	// DEPRICATED - remove these 2 options
	if ( hIndex.GetBool ( "mlock", false ) )
	{
		m_tFileAccess.m_eAttr = FileAccess_e::MLOCK;
		m_tFileAccess.m_eBlob = FileAccess_e::MLOCK;
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_PLAIN_ATTRS );
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_BLOB_ATTRS );
	}
	if ( hIndex.Exists ( "ondisk_attrs" ) )
	{
		bool bOnDiskAttrs = hIndex.GetBool ( "ondisk_attrs", false );
		bool bOnDiskPools = ( hIndex.GetStr ( "ondisk_attrs" )=="pool" );

		if ( bOnDiskAttrs || bOnDiskPools )
		{
			m_tFileAccess.m_eAttr = FileAccess_e::MMAP;
			m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_PLAIN_ATTRS );
		}
		if ( bOnDiskPools )
		{
			m_tFileAccess.m_eBlob = FileAccess_e::MMAP;
			m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_BLOB_ATTRS );
		}
	}

	// need to keep value from deprecated options for some time - use it as defaults on parse for now
	GetFileAccess( hIndex, MutableName_e::ACCESS_PLAIN_ATTRS, false, m_tFileAccess.m_eAttr, m_dLoaded );
	GetFileAccess( hIndex, MutableName_e::ACCESS_BLOB_ATTRS, false, m_tFileAccess.m_eBlob, m_dLoaded );
	GetFileAccess( hIndex, MutableName_e::ACCESS_DOCLISTS, true, m_tFileAccess.m_eDoclist, m_dLoaded );
	GetFileAccess( hIndex, MutableName_e::ACCESS_HITLISTS, true, m_tFileAccess.m_eHitlist, m_dLoaded );

	if ( hIndex.Exists ( "read_buffer_docs" ) )
	{
		m_tFileAccess.m_iReadBufferDocList = GetReadBuffer ( hIndex.GetInt ( "read_buffer_docs", m_tFileAccess.m_iReadBufferDocList ) );
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_DOCS );
	}

	if ( hIndex.Exists ( "read_buffer_hits" ) )
	{
		m_tFileAccess.m_iReadBufferHitList = GetReadBuffer ( hIndex.GetInt ( "read_buffer_hits", m_tFileAccess.m_iReadBufferHitList ) );
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_HITS );
	}
}

static void AddStr ( const CSphBitvec & dLoaded, MutableName_e eName, JsonObj_c & tRoot, const char * sVal )
{
	if ( !dLoaded.BitGet ( (int)eName ) )
		return;

	tRoot.AddStr ( GetMutableName ( eName ), sVal );
}

static void AddInt ( const CSphBitvec & dLoaded, MutableName_e eName, JsonObj_c & tRoot, int64_t iVal )
{
	if ( !dLoaded.BitGet ( (int)eName ) )
		return;

	tRoot.AddInt ( GetMutableName ( eName ), iVal );
}

static const char * GetExpandKwName ( int iExpandKeywords )
{
	if ( ( iExpandKeywords & KWE_ENABLED )==KWE_ENABLED )
		return "1";
	else if ( ( iExpandKeywords & KWE_EXACT )==KWE_EXACT )
		return "exact";
	else if ( ( iExpandKeywords & KWE_STAR )==KWE_STAR )
		return "star";
	else
		return "0";

}

bool MutableIndexSettings_c::Save ( CSphString & sBuf ) const
{
	if ( !m_bNeedSave )
		return false;

	JsonObj_c tRoot;
	
	if ( m_dLoaded.BitGet ( (int)MutableName_e::EXPAND_KEYWORDS ) )
		tRoot.AddStr ( "expand_keywords", GetExpandKwName ( m_iExpandKeywords ) );

	AddInt ( m_dLoaded, MutableName_e::RT_MEM_LIMIT, tRoot, m_iMemLimit );
	if ( m_dLoaded.BitGet ( (int)MutableName_e::PREOPEN ) )
		tRoot.AddBool ( "preopen", m_bPreopen );
	
	AddStr ( m_dLoaded, MutableName_e::ACCESS_PLAIN_ATTRS, tRoot, FileAccessName ( m_tFileAccess.m_eAttr ) );
	AddStr ( m_dLoaded, MutableName_e::ACCESS_BLOB_ATTRS, tRoot, FileAccessName ( m_tFileAccess.m_eBlob ) );
	AddStr ( m_dLoaded, MutableName_e::ACCESS_DOCLISTS, tRoot, FileAccessName ( m_tFileAccess.m_eDoclist ) );
	AddStr ( m_dLoaded, MutableName_e::ACCESS_HITLISTS, tRoot, FileAccessName ( m_tFileAccess.m_eHitlist ) );

	AddInt ( m_dLoaded, MutableName_e::READ_BUFFER_DOCS, tRoot, m_tFileAccess.m_iReadBufferDocList );
	AddInt ( m_dLoaded, MutableName_e::READ_BUFFER_HITS, tRoot, m_tFileAccess.m_iReadBufferHitList );

	sBuf = tRoot.AsString ( true );

	return true;
}

void MutableIndexSettings_c::Combine ( const MutableIndexSettings_c & tOther )
{
	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::EXPAND_KEYWORDS ) )
	{
		m_iExpandKeywords = tOther.m_iExpandKeywords;
		m_dLoaded.BitSet ( (int)MutableName_e::EXPAND_KEYWORDS );
	}

	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::RT_MEM_LIMIT ) )
	{
		m_iMemLimit = tOther.m_iMemLimit;
		m_dLoaded.BitSet ( (int)MutableName_e::RT_MEM_LIMIT );
	}

	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::PREOPEN ) )
	{
		m_bPreopen = tOther.m_bPreopen;
		m_dLoaded.BitSet ( (int)MutableName_e::PREOPEN );
	}

	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::ACCESS_PLAIN_ATTRS ) )
	{
		m_tFileAccess.m_eAttr = tOther.m_tFileAccess.m_eAttr;
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_PLAIN_ATTRS );
	}
	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::ACCESS_BLOB_ATTRS ) )
	{
		m_tFileAccess.m_eBlob = tOther.m_tFileAccess.m_eBlob;
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_BLOB_ATTRS );
	}
	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::ACCESS_DOCLISTS ) )
	{
		m_tFileAccess.m_eDoclist = tOther.m_tFileAccess.m_eDoclist;
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_DOCLISTS );
	}
	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::ACCESS_HITLISTS ) )
	{
		m_tFileAccess.m_eHitlist = tOther.m_tFileAccess.m_eHitlist;
		m_dLoaded.BitSet ( (int)MutableName_e::ACCESS_HITLISTS );
	}

	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::READ_BUFFER_DOCS ) )
	{
		m_tFileAccess.m_iReadBufferDocList = tOther.m_tFileAccess.m_iReadBufferDocList;
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_DOCS );
	}
	if ( tOther.m_dLoaded.BitGet ( (int)MutableName_e::READ_BUFFER_HITS ) )
	{
		m_tFileAccess.m_iReadBufferHitList = tOther.m_tFileAccess.m_iReadBufferHitList;
		m_dLoaded.BitSet ( (int)MutableName_e::READ_BUFFER_HITS );
	}
}

static MutableIndexSettings_c g_tMutableDefaults;
MutableIndexSettings_c & MutableIndexSettings_c::GetDefaults ()
{
	return g_tMutableDefaults;
}

static bool FormatCond ( bool bNeedSave, const CSphBitvec & dLoaded, MutableName_e eName, bool bNotEq )
{
	return ( ( bNeedSave && dLoaded.BitGet ( (int)eName ) ) || ( !bNeedSave && bNotEq ) );
}

void MutableIndexSettings_c::Format ( SettingsFormatter_c & tOut, FilenameBuilder_i * pFilenameBuilder ) const
{
	const MutableIndexSettings_c & tDefaults = GetDefaults ();

	tOut.Add ( GetMutableName ( MutableName_e::EXPAND_KEYWORDS ), GetExpandKwName ( m_iExpandKeywords ),
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::EXPAND_KEYWORDS, m_iExpandKeywords!=tDefaults.m_iExpandKeywords ) );
	tOut.Add ( GetMutableName ( MutableName_e::RT_MEM_LIMIT ), m_iMemLimit,
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::RT_MEM_LIMIT, m_iMemLimit!=tDefaults.m_iMemLimit ) );
	tOut.Add ( GetMutableName ( MutableName_e::PREOPEN ), m_bPreopen,
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::PREOPEN, m_bPreopen!=tDefaults.m_bPreopen ) );

	tOut.Add ( GetMutableName ( MutableName_e::ACCESS_PLAIN_ATTRS ), FileAccessName ( m_tFileAccess.m_eAttr ),
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::ACCESS_PLAIN_ATTRS, m_tFileAccess.m_eAttr!=tDefaults.m_tFileAccess.m_eAttr ) );
	tOut.Add ( GetMutableName ( MutableName_e::ACCESS_BLOB_ATTRS ), FileAccessName ( m_tFileAccess.m_eBlob ),
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::ACCESS_BLOB_ATTRS, m_tFileAccess.m_eBlob!=tDefaults.m_tFileAccess.m_eBlob ) );
	tOut.Add ( GetMutableName ( MutableName_e::ACCESS_DOCLISTS ), FileAccessName ( m_tFileAccess.m_eDoclist ),
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::ACCESS_DOCLISTS, m_tFileAccess.m_eDoclist!=tDefaults.m_tFileAccess.m_eDoclist ) );
	tOut.Add ( GetMutableName ( MutableName_e::ACCESS_HITLISTS ), FileAccessName ( m_tFileAccess.m_eHitlist ),
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::ACCESS_HITLISTS, m_tFileAccess.m_eHitlist!=tDefaults.m_tFileAccess.m_eHitlist ) );

	tOut.Add ( GetMutableName ( MutableName_e::READ_BUFFER_DOCS ), m_tFileAccess.m_iReadBufferDocList,
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::READ_BUFFER_DOCS, m_tFileAccess.m_iReadBufferDocList!=tDefaults.m_tFileAccess.m_iReadBufferDocList ) );
	tOut.Add ( GetMutableName ( MutableName_e::READ_BUFFER_HITS ), m_tFileAccess.m_iReadBufferHitList,
		FormatCond ( m_bNeedSave, m_dLoaded, MutableName_e::READ_BUFFER_HITS, m_tFileAccess.m_iReadBufferHitList!=tDefaults.m_tFileAccess.m_iReadBufferHitList ) );
}

void SaveMutableSettings ( const MutableIndexSettings_c & tSettings, const CSphString & sPath )
{
	CSphString sBuf;
	if ( !tSettings.Save ( sBuf ) ) // no need to save in case settings were set from config
		return;

	CSphString sError;
	CSphString sMutableNew, sMutable;
	sMutableNew.SetSprintf ( "%s%s.new", sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );
	sMutable.SetSprintf ( "%s%s", sPath.cstr(), sphGetExt ( SPH_EXT_SETTINGS ).cstr() );

	CSphWriter tWriter;
	if ( !tWriter.OpenFile ( sMutableNew, sError ) )
		sphDie ( "failed to serialize mutable settings: %s", sError.cstr() ); // !COMMIT handle this gracefully

	tWriter.PutBytes ( sBuf.cstr(), sBuf.Length() );
	tWriter.CloseFile();

	if ( tWriter.IsError() )
	{
		sphWarning ( "%s", sError.cstr() );
		return;
	}

	// rename
	if ( sph::rename ( sMutableNew.cstr(), sMutable.cstr() ) )
		sphDie ( "failed to rename mutable settings(src=%s, dst=%s, errno=%d, error=%s)",
			sMutableNew.cstr(), sMutable.cstr(), errno, strerrorm(errno) ); // !COMMIT handle this gracefully
}
