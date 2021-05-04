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

#ifndef _collation_
#define _collation_

#include "fnv64.h"

class LibcCSHash_fn
{
public:
	static uint64_t Hash ( const BYTE * pStr, int iLen, uint64_t uPrev=SPH_FNV64_SEED );
};


class LibcCIHash_fn
{
public:
	static uint64_t Hash ( const BYTE * pStr, int iLen, uint64_t uPrev=SPH_FNV64_SEED );
};


class Utf8CIHash_fn
{
public:
	static uint64_t Hash ( const BYTE * pStr, int iLen, uint64_t uPrev=SPH_FNV64_SEED );
};


class BinaryHash_fn
{
public:
	static uint64_t Hash ( const BYTE * pStr, int iLen, uint64_t uPrev=SPH_FNV64_SEED );
};


/// known collations
enum ESphCollation
{
	SPH_COLLATION_LIBC_CI,
	SPH_COLLATION_LIBC_CS,
	SPH_COLLATION_UTF8_GENERAL_CI,
	SPH_COLLATION_BINARY,

	SPH_COLLATION_DEFAULT = SPH_COLLATION_LIBC_CI
};

// iLen1 and iLen2 don't need to be specified for STRINGPTR attrs
using SphStringCmp_fn = int ( * ) ( ByteBlob_t dStr1, ByteBlob_t dStr2, bool bDataPtr );

SphStringCmp_fn GetStringCmpFunc ( ESphCollation eCollation );

void sphCollationInit();

#endif // _collation_