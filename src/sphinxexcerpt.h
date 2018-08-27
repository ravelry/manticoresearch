//
// Copyright (c) 2017-2018, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _sphinxexcerpt_
#define _sphinxexcerpt_

#include "sphinx.h"
#include "sphinxquery.h"

enum ESphSpz : DWORD
{
	SPH_SPZ_NONE		= 0,
	SPH_SPZ_SENTENCE	= 1UL<<0,
	SPH_SPZ_PARAGRAPH	= 1UL<<1,
	SPH_SPZ_ZONE		= 1UL<<2
};

/// a query to generate an excerpt
/// everything string is expected to be UTF-8
struct ExcerptQuery_t
{
public:
	CSphString		m_sSource;			///< source text (or file name, see m_bLoadFiles)
	CSphString		m_sWords;			///< words themselves
	CSphString		m_sBeforeMatch {"<b>"};	///< string to insert before each match
	CSphString		m_sAfterMatch {"</b>"};	///< string to insert after each match
	CSphString		m_sChunkSeparator {" ... \0"};	///< string to insert between matching chunks (in limited mode only)
	CSphString		m_sStripMode {"index"};	///< strip mode
	int				m_iLimit = 256;			///< max chars in snippet (0 if unlimited)
	int				m_iLimitWords = 0;		///< max words in snippet
	int				m_iLimitPassages = 0;	///< max passages in snippet
	int				m_iAround = 5;			///< how much words to highlight around each match
	int				m_iPassageId = 1;		///< current %PASSAGE_ID% counter value (must start at 1)
	bool			m_bRemoveSpaces = false;///< whether to collapse whitespace
	bool			m_bExactPhrase = false;	///< whether to highlight exact phrase matches only
	bool			m_bUseBoundaries = false;	///< whether to extract passages by phrase boundaries setup in tokenizer
	bool			m_bWeightOrder = false;	///< whether to order best passages in document (default) or weight order
	bool			m_bHighlightQuery = false;	///< whether try to highlight the whole query, or always word-by-word
	bool			m_bForceAllWords = false;///< whether to ignore limit until all needed keywords are highlighted (#448)
	BYTE			m_uFilesMode = 0;		///< sources are text(0), files(1), scattered files(2), only scattered files (3).
	bool			m_bAllowEmpty = false;	///< whether to allow empty snippets (by default, return something from the start)
	bool			m_bEmitZones = false;	///< whether to emit zone for passage

	CSphVector<BYTE>	m_dRes;			///< snippet result holder
	CSphString		m_sError;			///< snippet error message
	CSphString		m_sWarning;			///< snippet warning message
	bool			m_bHasBeforePassageMacro = false;
	bool			m_bHasAfterPassageMacro = false;
	CSphString		m_sBeforeMatchPassage;
	CSphString		m_sAfterMatchPassage;

	ESphSpz			m_ePassageSPZ { SPH_SPZ_NONE };
	bool			m_bJsonQuery { false };
	CSphVector<int> m_dSeparators;

};

/// snippet setupper and builder
/// used by searchd and SNIPPET() function in exprs
/// a precursor to BuildExcerpt() call
class SnippetContext_t : ISphNoncopyable
{
private:
	CSphScopedPtr<CSphDict> m_tDictKeeper { nullptr };
	CSphScopedPtr<CSphDict> m_tExactDictKeeper { nullptr };
	CSphScopedPtr<ISphTokenizer> m_tTokenizer { nullptr };
	CSphScopedPtr<CSphHTMLStripper> m_tStripper { nullptr };
	CSphScopedPtr<ISphTokenizer> m_pQueryTokenizer { nullptr };
	CSphDict * m_pDict = nullptr;
	XQQuery_t m_tExtQuery;
	DWORD m_eExtQuerySPZ { SPH_SPZ_NONE };

public:
	bool Setup ( const CSphIndex * pIndex, const ExcerptQuery_t &tSettings, CSphString &sError );
	void BuildExcerpt ( ExcerptQuery_t &tOptions, const CSphIndex * pIndex ) const;
};

extern CSphString g_sSnippetsFilePrefix;
#endif // _sphinxexcerpt_
