/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Redis Labs, Inc
 * All rights reserved.
 *

/* SDS allocator selection.
 *
 * This file is used in order to change the SDS allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#ifndef __SDS_ALLOC_H__
#define __SDS_ALLOC_H__

#include "over-zmalloc.h"

#define s_malloc zmalloc
#define s_realloc zrealloc
#define s_trymalloc ztrymalloc
#define s_tryrealloc ztryrealloc
#define s_free zfree
#define s_malloc_usable zmalloc_usable
#define s_realloc_usable zrealloc_usable
#define s_trymalloc_usable ztrymalloc_usable
#define s_tryrealloc_usable ztryrealloc_usable
#define s_free_usable zfree_usable

#endif
