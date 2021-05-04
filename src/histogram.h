//
// Copyright (c) 2018-2020, Manticore Software LTD (http://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#ifndef _histogram_
#define _histogram_

#include "sphinx.h"

class CSphReader;
class CSphWriter;

enum HistogramType_e
{
	HISTOGRAM_STREAMED_UINT32,
	HISTOGRAM_STREAMED_INT64,
	HISTOGRAM_STREAMED_FLOAT
};

class Histogram_i
{
public:
	virtual			~Histogram_i() {}

	virtual void	Insert ( SphAttr_t tAttrVal ) = 0;		// index time insert element when values and counters could be updated
	virtual void	UpdateCounter ( SphAttr_t tAttr ) = 0;	// run-time update counters only element values are same
	virtual void	Delete ( SphAttr_t tAttrVal ) = 0;
	virtual bool	EstimateRsetSize ( const CSphFilterSettings & tFilter, int64_t & iEstimate ) const = 0;
	virtual DWORD	GetNumValues() const = 0;
	virtual bool	IsOutdated() const = 0;

	virtual HistogramType_e		GetType() const = 0;
	virtual const CSphString &	GetAttrName() const = 0;

	virtual void	Finalize() {}
	virtual bool	Save ( CSphWriter & tWriter ) const = 0;
	virtual bool	Load ( CSphReader & tReader, CSphString & sError ) = 0;

	virtual void	Dump ( StringBuilder_c & tOut ) const = 0;
};


class HistogramContainer_c
{
public:
					~HistogramContainer_c();

	bool			Save ( const CSphString & sFile, CSphString & sError );
	bool			Load ( const CSphString & sFile, CSphString & sError );
	bool			Add ( Histogram_i * pHistogram );
	void			Remove ( const CSphString & sAttr );
	Histogram_i *	Get ( const CSphString & sAttr ) const;
	DWORD			GetNumValues() const;

private:
	SmallStringHash_T<Histogram_i*>	m_dHistogramHash;

	void			Reset();
};


Histogram_i *	CreateHistogram ( const CSphString & sAttr, ESphAttr eAttrType, int iSize=0 );

int64_t			EstimateFilterSelectivity ( const CSphFilterSettings & tSettings, const HistogramContainer_c * pHistogramContainer );

#endif // _histogram_