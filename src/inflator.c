/*
 * Copyright (C) 2023, jpn
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <jdeflate/inflator.h>
#include <ctoolbox/memory.h>


#if defined(AUTOINCLUDE_1)

/* deflate format definitions */
#define DEFLT_MAXBITS    15
#define DEFLT_WINDOWSZ   32768

#define WNDWSIZE DEFLT_WINDOWSZ


#else

/* root bits for main tables */
#define LROOTBITS 9
#define DROOTBITS 7
#define CROOTBITS 7

#define DEFLT_LMAXSYMBOL 288
#define DEFLT_DMAXSYMBOL 32
#define DEFLT_CMAXSYMBOL 19


/* these values were calculated using enough in zlib/examples, if the
 * rootbits values are changed these values must be recalculated */
#define ENOUGHL 854  /* enough 288 9 15 */
#define ENOUGHD 402  /* enough  32 7 15 */


/* private stuff */
struct TINFLTPrvt {
	/* public fields */
	struct TInflator hidden;

	/* state */
	uintxx substate;
	uintxx final;
	uintxx used;

	/* auxiliar fields */
	uintxx aux0;
	uintxx aux1;
	uintxx aux2;
	uintxx aux3;
	uintxx aux4;

	/* bit buffer */
#if defined(CTB_ENV64)
	uint64 bbuffer;
#else
	uint32 bbuffer;
#endif
	uintxx bcount;

	/* window buffer */
	uint8* window;
	uintxx count;          /* bytes avaible in the window buffer + target */
	uintxx end;            /* window buffer end */

	/* decoding tables */
	struct TINFLTTEntry* ltable;
	struct TINFLTTEntry* dtable;

	/* dynamic tables (trees) */
	struct TTINFLTTables {
		/* */
		struct TINFLTTEntry {
			/* the symbol if it's a literal, a distance or length base
			* value, or a subtable offset if the code length is larger than
			* rootbits */
			uint16 info;

			/* extra bits or tag */
			uint8 etag;
			uint8 length;   /* length of the code */
		}
		symbols[
			ENOUGHL +
			ENOUGHD
		];

		uint16 lengths[DEFLT_LMAXSYMBOL + DEFLT_DMAXSYMBOL];
	}
	*tables;

	/* custom allocator */
	struct TAllocator* allocator;
};

#endif


#if defined(AUTOINCLUDE_1)

#define PRVT ((struct TINFLTPrvt*) state)

TInflator*
inflator_create(TAllocator* allocator)
{
	struct TInflator* state;

	if (allocator) {
		state = allocator->reserve(allocator->user, sizeof(struct TINFLTPrvt));
	}
	else {
		state = CTB_RESERVE(sizeof(struct TINFLTPrvt));
	}
	if (state == NULL) {
		return NULL;
	}
	PRVT->allocator = allocator;

	PRVT->window = NULL;
	PRVT->tables = NULL;
	inflator_reset(state);
	if (state->error) {
		inflator_destroy(state);
		return NULL;
	}
	return state;
}

CTB_INLINE void*
_reserve(struct TINFLTPrvt* p, uintxx amount)
{
	if (p->allocator) {
		return p->allocator->reserve(p->allocator->user, amount);
	}
	return CTB_RESERVE(amount);
}

CTB_INLINE void
_release(struct TINFLTPrvt* p, void* memory)
{
	if (p->allocator) {
		p->allocator->release(p->allocator->user, memory);
		return;
	}
	CTB_RELEASE(memory);
}


#define SETERROR(ERROR) (state->error = (ERROR))

void
inflator_reset(TInflator* state)
{
	CTB_ASSERT(state);

	/* public fields */
	state->state = 0;
	state->error = 0;
	state->finalinput = 0;

	state->source = NULL;
	state->sbgn = NULL;
	state->send = NULL;

	state->target = NULL;
	state->tbgn = NULL;
	state->tend = NULL;

	/* private fields */
	PRVT->final = 0;
	PRVT->substate = 0;

	PRVT->used = 0;
	PRVT->aux0 = 0;
	PRVT->aux1 = 0;
	PRVT->aux2 = 0;
	PRVT->aux3 = 0;
	PRVT->aux4 = 0;

	PRVT->bbuffer = 0;
	PRVT->bcount  = 0;

	PRVT->count = 0;
	PRVT->end   = 0;

	/* */
	if (PRVT->window == NULL) {
		PRVT->window = _reserve(PRVT, WNDWSIZE);
		if (PRVT->window == NULL) {
			goto L_ERROR;
		}
	}
	if (PRVT->tables == NULL) {
		PRVT->tables = _reserve(PRVT, sizeof(struct TTINFLTTables));
		if (PRVT->tables == NULL) {
			goto L_ERROR;
		}
	}
	return;

L_ERROR:
	SETERROR(INFLT_EOOM);
	state->state = INFLT_BADSTATE;
}

void
inflator_destroy(TInflator* state)
{
	if (state == NULL) {
		return;
	}

	if (PRVT->window) {
		_release(PRVT, PRVT->window);
	}
	if (PRVT->tables) {
		_release(PRVT, PRVT->tables);
	}
	_release(PRVT, state);
}


CTB_FORCEINLINE uint16
reversecode(uint16 code, uintxx length)
{
	uintxx a;
	uintxx b;
	uintxx r;

	static const uint8 rtable[] = {
		0x00, 0x08, 0x04, 0x0c,
		0x02, 0x0a, 0x06, 0x0e,
		0x01, 0x09, 0x05, 0x0d,
		0x03, 0x0b, 0x07, 0x0f
	};

	if (length > 8) {
		a = (uint8) (code >> 0);
		b = (uint8) (code >> 8);
		a = rtable[a >> 4] | (rtable[a & 0x0f] << 4);
		b = rtable[b >> 4] | (rtable[b & 0x0f] << 4);

		r = b | (a << 8);
		return (uint16) (r >> (0x10 - length));
	}

	a = (uint8) code;
	r = rtable[a >> 4] | (rtable[a & 0x0f] << 4);

	return (uint16) (r >> (0x08 - length));
}

CTB_FORCEINLINE uintxx
clzero24(uint32 n)
{
#if defined(__GNUC__)
	return __builtin_clz(n);
#else
	static const unsigned char clztable[] = {
		0x08, 0x07, 0x06, 0x06, 0x05, 0x05, 0x05, 0x05,
		0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
		0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
		0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	uintxx r;
	uintxx a;
	uintxx b;

	a = clztable[(uint8) (n >> 0x18)];
	b = clztable[(uint8) (n >> 0x10)];

	if (a == 0x08) {
		r = a + b;
		if (r == 0x10)
			r += clztable[(uint8) (n >> 0x08)];
		return r;
	}
	return a;
#endif
}

CTB_FORCEINLINE uint16
reverseinc(uint16 n, uintxx length)
{
	uintxx offset;
	uintxx s;

	n <<= (offset = 0x10 - length);

	if ((s = 0x8000 >> clzero24(~n << 16)) == 0) {
		return 0;
	}
	return (uint16) (((n & (s - 1)) + s) >> offset);
}


/* tags for the table entries, 0 to 13 is used to indicate the
 * extra bits if the symbol is not a literal */
#define LITERALSYMBOL 0x10
#define ENDOFBLOCK    0x11
#define SUBTABLEENTRY 0x12
#define INVALIDCODE   0x13


/* symbol info */
struct TSInfo {
	uint16 base;
	uint8  extrabits;
};

/* length base value and extra bits */
static const struct TSInfo lnsinfo[] = {
	{0x0100, 0x11}, {0x0003, 0x00},  /* <- end of block */
	{0x0004, 0x00}, {0x0005, 0x00},
	{0x0006, 0x00}, {0x0007, 0x00},
	{0x0008, 0x00}, {0x0009, 0x00},
	{0x000a, 0x00}, {0x000b, 0x01},
	{0x000d, 0x01}, {0x000f, 0x01},
	{0x0011, 0x01}, {0x0013, 0x02},
	{0x0017, 0x02}, {0x001b, 0x02},
	{0x001f, 0x02}, {0x0023, 0x03},
	{0x002b, 0x03}, {0x0033, 0x03},
	{0x003b, 0x03}, {0x0043, 0x04},
	{0x0053, 0x04}, {0x0063, 0x04},
	{0x0073, 0x04}, {0x0083, 0x05},
	{0x00a3, 0x05}, {0x00c3, 0x05},
	{0x00e3, 0x05}, {0x0102, 0x00}
};

/* distance base value and extra bits */
static const struct TSInfo dstinfo[] = {
	{0x0001, 0x00}, {0x0002, 0x00},
	{0x0003, 0x00}, {0x0004, 0x00},
	{0x0005, 0x01}, {0x0007, 0x01},
	{0x0009, 0x02}, {0x000d, 0x02},
	{0x0011, 0x03}, {0x0019, 0x03},
	{0x0021, 0x04}, {0x0031, 0x04},
	{0x0041, 0x05}, {0x0061, 0x05},
	{0x0081, 0x06}, {0x00c1, 0x06},
	{0x0101, 0x07}, {0x0181, 0x07},
	{0x0201, 0x08}, {0x0301, 0x08},
	{0x0401, 0x09}, {0x0601, 0x09},
	{0x0801, 0x0a}, {0x0c01, 0x0a},
	{0x1001, 0x0b}, {0x1801, 0x0b},
	{0x2001, 0x0c}, {0x3001, 0x0c},
	{0x4001, 0x0d}, {0x6001, 0x0d}
};


#define LTABLEMODE 0
#define DTABLEMODE 1
#define CTABLEMODE 2

static uintxx
buildtable(uint16* lengths, uintxx n, struct TINFLTTEntry* table, uintxx mode)
{
	uint16 code;
	uintxx mlen;
	intxx  left;
	intxx  i;
	intxx  j;
	uintxx symbol;
	uintxx length;
	uintxx mbits;
	uintxx mmask;
	struct TINFLTTEntry* entry;
	const struct TSInfo* sinfo;

	uint16 counts[DEFLT_MAXBITS + 1];
	uint16 ncodes[DEFLT_MAXBITS + 1];

	sinfo = dstinfo;
	switch (mode) {
		case LTABLEMODE: mbits = LROOTBITS; sinfo = lnsinfo - 256; break;
		case DTABLEMODE: mbits = DROOTBITS; break;
		default:
			mbits = CROOTBITS;
	}

	for (i = 0; i <= DEFLT_MAXBITS; i++) {
		counts[i] = 0;
		ncodes[i] = 0;
	}

	/* count the number of lengths for each length */
	i = n - 1;
	while(i >= 0)
		counts[lengths[i--]] += 1;

	if (counts[0] == n) {
		/* RFC:
		 * One distance code of zero bits means that there are no distance
		 * codes used at all (the data is all literals). */
		if (mode == DTABLEMODE) {
			for (i = 0; (uintxx) i < ((uintxx) 1L << mbits); i++) {
				entry = table + i;

				entry->info = 0xffff;
				entry->etag = INVALIDCODE;
				entry->length = 15;
			}
			return 0;
		}
		/* we need at least one symbol (256) for literal-length codes */
		return INFLT_ERROR;
	}

	counts[0] = 0;

	/* get the longest length */
	i = DEFLT_MAXBITS;
	while (counts[i] == 0)
		i--;
	mlen = i;

	/* check for vality */
	left = 1;
	for (i = 1; i <= DEFLT_MAXBITS; i++) {
		left = (left << 1) - counts[i];

		if (left < 0) {
			/* over subscribed */
			return INFLT_ERROR;
		}
	}
	if (left) {
		if (mlen != 1) {
			return INFLT_ERROR;
		}
		else {
			if (mode != DTABLEMODE) {
				return INFLT_ERROR;
			}
		}
	}

	/* determine the first codeword of each length */
	code = 0;
	for (i = 1; (uintxx) i <= mlen; i++) {
		code = (code + counts[i - 1]) << 1;
		ncodes[i] = reversecode(code, i);
	}
	mmask = ((uintxx) 1 << mbits) - 1;

	if (mlen > mbits) {
		uintxx count;
		uintxx offset;
		uintxx r;

		/* setup the secondary tables */
		for (i = 0; (uintxx) i <= mmask; i++) {
			table[i].etag = 0;
		}

		offset = mmask + 1;
		for (r = mlen - mbits; r; r--) {
			count = counts[mbits + r];
			if (count == 0) {
				continue;
			}

			code = ncodes[mbits + r] & mmask;
			j = count >> r;
			if (count & (((uintxx) 1 << r) - 1))
				j++;

			for (i = 0; i < j; i++) {
				entry = table + code;
				if (entry->etag == SUBTABLEENTRY) {
					continue;
				}
				entry->etag = SUBTABLEENTRY;
				entry->info = (uint16) offset;
				entry->length = (uint8) (mbits + r);

				code = reverseinc(code, mbits);
				offset += (uintxx) 1L << r;
			}
		}

		if (mode == DTABLEMODE) {
			if (offset > ENOUGHD) {
				return INFLT_ERROR;
			}
		}
		else {
			if (offset > ENOUGHL) {
				return INFLT_ERROR;
			}
		}
	}

	/* populate the table */
	code = 0;
	for (symbol = 0; symbol < n; symbol++) {
		struct TINFLTTEntry e;

		length = lengths[symbol];
		if (length == 0){
			continue;
		}

		if (mode == DTABLEMODE || symbol >= 256) {
			e.info = sinfo[symbol].base;
			e.etag = sinfo[symbol].extrabits;
		}
		else {
			e.info = (uint16) symbol;
			e.etag = LITERALSYMBOL;
		}
		e.length = (uint8) length;

		code = ncodes[length];
		ncodes[length] = reverseinc(code, length);
		if (length > (uintxx) mbits) {
			/* secondary table */
			entry = table + (code & mmask);
			j = entry->length - length;
			i = entry->info;

			length -= mbits;
			code  >>= mbits;
		}
		else {
			j = mbits - length;
			i = 0;
		}

		for (j = ((uintxx) 1 << j) - 1; j >= 0; j--) {
			table[i + (code | (j << length))] = e;
		}
	}

	/* RFC:
	 * If only one distance code is used, it is encoded using one bit, not
	 * zero bits; in this case there is a single code length of one, with one
	 * unused code. */
	if (mlen == 1 && code == 0) {
		code = 1;

		for (j = ((uintxx) 1 << (mbits - 1)) - 1; j >= 0; j--) {
			entry = table + (code | (j << 1));

			entry->info = 0xffff;
			entry->etag = INVALIDCODE;
			entry->length = 15;
		}
	}
	return 0;
}


#if defined(CTB_ENV64)
	#define BBTYPE uint64
#else
	#define BBTYPE uint32
#endif

/*
 * BIT input operations */

CTB_FORCEINLINE bool
tryreadbits(struct TInflator* state, uintxx n)
{
	for(; n > PRVT->bcount; PRVT->bcount += 8) {
		if (state->source >= state->send) {
			return 0;
		}
		PRVT->bbuffer |= (BBTYPE) *state->source++ << PRVT->bcount;
	}
	return 1;
}

CTB_FORCEINLINE bool
fetchbyte(struct TInflator* state)
{
	if (state->source < state->send) {
		PRVT->bbuffer |= (BBTYPE) *state->source++ << PRVT->bcount;
		PRVT->bcount += 8;
		return 1;
	}
	return 0;
}

CTB_FORCEINLINE void
dropbits(struct TInflator* state, uintxx n)
{
	PRVT->bbuffer = PRVT->bbuffer >> n;
	PRVT->bcount -= n;
}

CTB_FORCEINLINE uintxx
getbits(struct TInflator* state, uintxx n)
{
	return PRVT->bbuffer & ((((BBTYPE) 1) << n) - 1);
}

static uintxx
updatewindow(struct TInflator* state)
{
	uintxx total;
	uintxx maxrun;
	uint8* begin;

	total = (uintxx) (state->target - state->tbgn);
	if (total == 0) {
		return 0;
	}

	if (total > WNDWSIZE) {
		total = WNDWSIZE;
	}
	begin = state->target - total;
	if (PRVT->count < WNDWSIZE) {
		uintxx bytes;

		bytes = PRVT->count + total;
		if (bytes > WNDWSIZE)
			bytes = WNDWSIZE;
		PRVT->count = bytes;
	}

	maxrun = WNDWSIZE - PRVT->end;
	if (total < maxrun)
		maxrun = total;
	ctb_memcpy(PRVT->window + PRVT->end, begin, maxrun);

	total -= maxrun;
	if (total) {
		ctb_memcpy(PRVT->window, begin + maxrun, total);
		PRVT->end = total;
	}
	else {
		PRVT->end = PRVT->end + maxrun;
	}
	return 0;
}


static uintxx decodeblck(struct TInflator* state);

static uintxx decodestrd(struct TInflator* state);
static uintxx decodednmc(struct TInflator* state);

eINFLTResult
inflator_inflate(TInflator* state, uintxx final)
{
	uintxx r;

	if (UNLIKELY(state->finalinput == 0 && final)) {
		state->finalinput = 1;
	}

	PRVT->used = 1;
	if (LIKELY(state->state == 5)) {
L_DECODE:
		if (LIKELY((r = decodeblck(state)) != 0)) {
			if (state->finalinput && r == INFLT_SRCEXHSTD) {
				SETERROR(INFLT_EINPUTEND);
				return INFLT_ERROR;
			}
			return r;
		}
		state->state = 0;
	}

	for (;;) {
		switch (state->state) {
			case 0: {
				if (UNLIKELY(PRVT->final)) {
					state->state = INFLT_BADSTATE;
					return INFLT_OK;
				}

				if (tryreadbits(state, 3)) {
					PRVT->final  = getbits(state, 1); dropbits(state, 1);
					state->state = getbits(state, 2); dropbits(state, 2);
					state->state++;
					continue;
				}

				if (state->finalinput) {
					SETERROR(INFLT_EINPUTEND);
					return INFLT_ERROR;
				}
				if (updatewindow(state)) {
					return INFLT_ERROR;
				}

				return INFLT_SRCEXHSTD;
			}

			/* stored */
			case 1: {
				if (LIKELY((r = decodestrd(state)) != 0)) {
					if (state->finalinput && r == INFLT_SRCEXHSTD) {
						SETERROR(INFLT_EINPUTEND);
						return INFLT_ERROR;
					}
					return r;
				}
				state->state = 0;
				continue;
			}

			/* static */
			case 2: {
				PRVT->ltable = (void*) lsttctable;
				PRVT->dtable = (void*) dsttctable;

				state->state = 5;
				goto L_DECODE;
			}

			/* dynamic */
			case 3: {
				if (LIKELY((r = decodednmc(state)) != 0)) {
					if (state->finalinput && r == INFLT_SRCEXHSTD) {
						SETERROR(INFLT_EINPUTEND);
						return INFLT_ERROR;
					}
					return r;
				}
				state->state = 5;
				goto L_DECODE;
			}

			case 4:
				SETERROR(INFLT_EBADBLOCK);

			/* fallthrough */
			default:
				goto L_ERROR;
		}
	}

L_ERROR:
	if (state->error == 0) {
		SETERROR(INFLT_EBADSTATE);
	}
	return INFLT_ERROR;
}

void
inflator_setdctnr(TInflator* state, uint8* dict, uintxx size)
{
	CTB_ASSERT(state && dict);

	if (PRVT->used) {
		SETERROR(INFLT_EINCORRECTUSE);
		state->state = INFLT_BADSTATE;
		return;
	}

	if (PRVT->window == NULL) {
		uint8* buffer;

		buffer = _reserve(PRVT, WNDWSIZE);
		if (buffer == NULL) {
			SETERROR(INFLT_EOOM);
			state->state = INFLT_BADSTATE;
			return;
		}
		PRVT->window = buffer;
	}

	if (size > WNDWSIZE)
		size = WNDWSIZE;
	ctb_memcpy(PRVT->window, dict, size);
	PRVT->count = size;
	PRVT->end   = size;

	PRVT->used = 1;
}


#define slength PRVT->aux0

static uintxx
decodestrd(struct TInflator* state)
{
	uintxx maxrun;
	uintxx sourceleft;
	uintxx targetleft;

	switch (PRVT->substate) {
		case 0:
			break;
		case 1: goto L_STATE1;
		case 2: goto L_STATE2;
		case 3: goto L_STATE3;
	}

	if (tryreadbits(state, 8)) {
		dropbits(state, PRVT->bcount & 7);
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}
		return INFLT_SRCEXHSTD;
	}

	PRVT->substate++;

L_STATE1:
	if (tryreadbits(state, 16)) {
		uint8 a;
		uint8 b;

		a = (uint8) getbits(state, 8); dropbits(state, 8);
		b = (uint8) getbits(state, 8); dropbits(state, 8);
		slength = a | (b << 8);
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}
		return INFLT_SRCEXHSTD;
	}
	PRVT->substate++;

L_STATE2:
	if (tryreadbits(state, 16)) {
		uintxx nlength;
		uint8 a;
		uint8 b;

		a = (uint8) getbits(state, 8); dropbits(state, 8);
		b = (uint8) getbits(state, 8); dropbits(state, 8);
		nlength = a | (b << 8);

		if ((uint16) ~slength != nlength) {
			SETERROR(INFLT_EBADBLOCK);
			return INFLT_ERROR;
		}
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}
		return INFLT_SRCEXHSTD;
	}
	PRVT->substate++;

L_STATE3:
	sourceleft = (uintxx) (state->send - state->source);
	targetleft = (uintxx) (state->tend - state->target);
	maxrun = slength;

	if (targetleft < maxrun)
		maxrun = targetleft;
	if (sourceleft < maxrun)
		maxrun = sourceleft;

	ctb_memcpy(state->target, state->source, maxrun);
	state->target += maxrun;
	state->source += maxrun;

	slength -= maxrun;
	if (slength) {
		if (PRVT->final == 0) {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}
		}
		if ((targetleft - maxrun) == 0) {
			return INFLT_TGTEXHSTD;
		}

		return INFLT_SRCEXHSTD;
	}

	PRVT->substate = 0;
	return INFLT_OK;
}

#undef slength


#define slcount PRVT->aux0
#define sdcount PRVT->aux1
#define sccount PRVT->aux2
#define scindex PRVT->aux3

CTB_INLINE uintxx
readlengths(struct TInflator* state, uintxx n, uint16* lengths)
{
	struct TINFLTTEntry e;
	uintxx length;
	uintxx replen;

	const uint8 sinfo[][2] = {
		{0x02, 0x03},
		{0x03, 0x03},
		{0x07, 0x0b}
	};

	while (scindex < n) {
		for (;;) {
			e = PRVT->ltable[getbits(state, CROOTBITS)];
			if (e.length <= PRVT->bcount) {
				break;
			}

			if(fetchbyte(state) == 0) {
				if (updatewindow(state)) {
					return INFLT_ERROR;
				}
				return INFLT_SRCEXHSTD;
			}
		}

		if (e.info < 16) {
			lengths[scindex++] = e.info;
			dropbits(state, e.length);
			continue;
		}

		replen = sinfo[e.info - 16][1];
		length = sinfo[e.info - 16][0];

		if (tryreadbits(state, e.length + length)) {
			dropbits(state, e.length);

			replen += getbits(state, length);
			dropbits(state, length);
		}
		else {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			return INFLT_SRCEXHSTD;
		}

		if (length == 2) {
			if (scindex == 0) {
				SETERROR(INFLT_EBADTREE);
				return INFLT_ERROR;
			}
			length = lengths[scindex - 1];
		}
		else {
			length = 0;
		}

		if (scindex + replen > DEFLT_DMAXSYMBOL + DEFLT_LMAXSYMBOL) {
			SETERROR(INFLT_EBADTREE);
			return INFLT_ERROR;
		}

		while (replen--)
			lengths[scindex++] = (uint16) length;
	}

	return 0;
}

static uintxx
decodednmc(struct TInflator* state)
{
	static const uint8 lcorder[] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	uintxx r;
	uint16* lengths;

	switch (PRVT->substate) {
		case 0:
			break;
		case 1: goto L_STATE1;
		case 2: goto L_STATE2;
	}

	PRVT->ltable = PRVT->tables->symbols;
	PRVT->dtable = PRVT->tables->symbols + ENOUGHL;

	if (tryreadbits(state, 14)) {
		slcount = getbits(state, 5) + 257; dropbits(state, 5);
		sdcount = getbits(state, 5) +   1; dropbits(state, 5);
		sccount = getbits(state, 4) +   4; dropbits(state, 4);

		if (slcount > 286 || sdcount > 30) {
			SETERROR(INFLT_EBADTREE);
			return INFLT_ERROR;
		}
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}

		return INFLT_SRCEXHSTD;
	}
	PRVT->substate++;
	scindex = 0;

L_STATE1:
	lengths = PRVT->tables->lengths;
	for (; sccount > scindex; scindex++) {
		if (tryreadbits(state, 3)) {
			lengths[lcorder[scindex]] = (uint16) getbits(state, 3);
			dropbits(state, 3);
		}
		else {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			return INFLT_SRCEXHSTD;
		}
	}
	sccount = DEFLT_CMAXSYMBOL;
	for (; sccount > scindex; scindex++)
		lengths[lcorder[scindex]] = 0;

	r = buildtable(lengths, DEFLT_CMAXSYMBOL, PRVT->ltable, CTABLEMODE);
	if (r) {
		SETERROR(INFLT_EBADTREE);
		return r;
	}

	PRVT->substate++;
	scindex = 0;

L_STATE2:
	lengths = PRVT->tables->lengths;
	r = readlengths(state, slcount + sdcount, lengths);
	if (r) {
		return r;
	}

	if (lengths[256] == 0) {
		SETERROR(INFLT_EBADTREE);
		return INFLT_ERROR;
	}

	r = buildtable(lengths, slcount, PRVT->ltable, LTABLEMODE);
	if (r) {
		SETERROR(INFLT_EBADTREE);
		return INFLT_ERROR;
	}
	lengths = lengths + slcount;
	r = buildtable(lengths, sdcount, PRVT->dtable, DTABLEMODE);
	if (r) {
		SETERROR(INFLT_EBADTREE);
		return INFLT_ERROR;
	}

	PRVT->substate = 0;
	return 0;
}

#undef slcount
#undef sdcount
#undef sccount
#undef scindex


#define slength   PRVT->aux0
#define sbextra   PRVT->aux1
#define sdistance PRVT->aux2

CTB_INLINE uintxx
copybytes(struct TInflator* state, uintxx distance, uintxx length)
{
	uint8* buffer;
	uintxx maxrun;
	uintxx avaible;

	avaible = (uintxx) (state->tend - state->target);
	do {
		if (avaible == 0) {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			slength   = length;
			sdistance = distance;

			PRVT->substate = 4;
			return INFLT_TGTEXHSTD;
		}

		maxrun = (uintxx) (state->target - state->tbgn);
		if (distance > maxrun) {
			uintxx offset;

			maxrun = distance - maxrun;
			if (UNLIKELY(maxrun > PRVT->count)) {
				SETERROR(INFLT_EFAROFFSET);
				return INFLT_ERROR;
			}

			buffer = PRVT->window;
			if (maxrun > PRVT->end) {
				maxrun -= PRVT->end;

				offset =  WNDWSIZE - maxrun;
			}
			else {
				offset = PRVT->end - maxrun;
			}
			buffer += offset;

			if (maxrun > length)
				maxrun = length;
		}
		else {
			buffer = state->target - distance;
			maxrun = length;
		}

		if (maxrun > avaible)
			maxrun = avaible;

		length  -= maxrun;
		avaible -= maxrun;

		do {
			*state->target++ = *buffer++;
		} while (--maxrun);
	} while (length);

	return 0;
}


#if defined(CTB_ENV64)
	#define FASTSRCLEFT 14
	#define FASTTGTLEFT 274
#else
	#define FASTSRCLEFT 10
	#define FASTTGTLEFT 266
#endif

static uintxx decodefast(struct TInflator* state);


#if defined(__MSVC__)
	#pragma warning(push)
	#pragma warning(disable: 4702)
#endif

static uintxx
decodeblck(struct TInflator* state)
{
	uintxx r;
	uintxx length;
	uintxx distance;
	uintxx bextra;
	uintxx fastcheck;
	struct TINFLTTEntry e;

	length   = slength;
	bextra   = sbextra;
	distance = sdistance;

	fastcheck = 1;
	switch (PRVT->substate) {
		case 0:
			break;
		case 1: goto L_STATE1;
		case 2: goto L_STATE2;
		case 3: goto L_STATE3;
		case 4: goto L_STATE4;
	}

L_LOOP:
	if (UNLIKELY(fastcheck)) {
		uintxx targetleft;
		uintxx sourceleft;

		targetleft = state->tend - state->target;
		sourceleft = state->send - state->source;
		if (targetleft >= FASTTGTLEFT && sourceleft >= FASTSRCLEFT) {
			r = decodefast(state);
			if (r == ENDOFBLOCK) {
				PRVT->substate = 0;
				return 0;
			}
			else {
				if (r) {
					return INFLT_ERROR;
				}
			}
		}
		fastcheck = 0;
	}

	for (;;) {
		e = PRVT->ltable[getbits(state, LROOTBITS)];
		if (LIKELY(e.length <= PRVT->bcount)) {
			break;
		}

		if (UNLIKELY(fetchbyte(state) == 0)) {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			PRVT->substate = 0;
			return INFLT_SRCEXHSTD;
		}
	}

	if (LIKELY(e.etag == SUBTABLEENTRY)) {
		uintxx base;
		uintxx bits;

		base = e.info;
		bits = e.length;
		for (;;) {
			e = PRVT->ltable[base + (getbits(state, bits) >> LROOTBITS)];
			if (LIKELY(e.length <= PRVT->bcount)) {
				break;
			}

			if (UNLIKELY(fetchbyte(state) == 0)) {
				if (updatewindow(state)) {
					return INFLT_ERROR;
				}

				PRVT->substate = 0;
				return INFLT_SRCEXHSTD;
			}
		}
	}

	if (LIKELY(e.etag == LITERALSYMBOL)) {
		if (LIKELY(state->tend > state->target)) {
			*state->target++ = (uint8) e.info;
		}
		else {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			PRVT->substate = 0;
			return INFLT_TGTEXHSTD;
		}
		dropbits(state, e.length);
		goto L_LOOP;
	}

	if (UNLIKELY(e.etag == ENDOFBLOCK)) {
		dropbits(state, e.length);
		PRVT->substate = 0;
		return 0;
	}

	if (UNLIKELY(e.etag == INVALIDCODE)) {
		SETERROR(INFLT_EBADCODE);
		return INFLT_ERROR;
	}

	dropbits(state, e.length);
	length = e.info;
	bextra = e.etag;

L_STATE1:
	if (LIKELY(tryreadbits(state, bextra))) {
		length += getbits(state, bextra);
		dropbits(state, bextra);
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}

		PRVT->substate = 1;
		slength   = length;
		sbextra   = bextra;
		sdistance = distance;

		return INFLT_SRCEXHSTD;
	}

L_STATE2:
	/* decode distance */
	for (;;) {
		e = PRVT->dtable[getbits(state, DROOTBITS)];
		if (LIKELY(e.length <= PRVT->bcount)) {
			break;
		}

		if (UNLIKELY(fetchbyte(state) == 0)) {
			if (updatewindow(state)) {
				return INFLT_ERROR;
			}

			PRVT->substate = 2;
			slength   = length;
			sbextra   = bextra;
			sdistance = distance;

			return INFLT_SRCEXHSTD;
		}
	}

	if (LIKELY(e.etag == SUBTABLEENTRY)) {
		uintxx base;
		uintxx bits;

		base = e.info;
		bits = e.length;
		for (;;) {
			e = PRVT->dtable[base + (getbits(state, bits) >> DROOTBITS)];
			if (LIKELY(e.length <= PRVT->bcount)) {
				break;
			}

			if(UNLIKELY(fetchbyte(state) == 0)) {
				if (updatewindow(state)) {
					SETERROR(INFLT_EOOM);
					return INFLT_ERROR;
				}

				PRVT->substate = 2;
				slength   = length;
				sbextra   = bextra;
				sdistance = distance;

				return INFLT_SRCEXHSTD;
			}
		}
	}

	if (UNLIKELY(e.etag == INVALIDCODE)) {
		SETERROR(INFLT_EBADCODE);
		return INFLT_ERROR;
	}

	dropbits(state, e.length);
	distance = e.info;
	bextra   = e.etag;

L_STATE3:
	if (LIKELY(tryreadbits(state, bextra))) {
		distance += getbits(state, bextra);
		dropbits(state, bextra);
	}
	else {
		if (updatewindow(state)) {
			return INFLT_ERROR;
		}

		PRVT->substate = 3;
		slength   = length;
		sbextra   = bextra;
		sdistance = distance;

		return INFLT_SRCEXHSTD;
	}

L_STATE4:
	r = copybytes(state, distance, length);
	if (UNLIKELY(r)) {
		return r;
	}
	goto L_LOOP;

	return 0;
}

#if defined(__MSVC__)
	#pragma warning(pop)
#endif


#define GETBITS(BUFFER, N) ((BBTYPE) (BUFFER) & ((((BBTYPE) 1UL) << (N)) - 1))

#define DROPBITS(BUFFER, BCOUNT, N) (BUFFER) >>= (N); (BCOUNT) -= (N);

#if defined(CTB_ENV64)
	#define BBMASK 0x0000ffffffffffffULL
	#define BBSWAP CTB_SWAP64ONBE
#else
	#define BBMASK 0x0000ffffUL
	#define BBSWAP CTB_SWAP32ONBE
#endif

#if !defined(CTB_STRICTALIGNMENT) && defined(CTB_FASTUNALIGNED)
	#define FILLBBUFFER() \
		n = BBSWAP(((BBTYPE*) source)[0]) & BBMASK;
#else
	#if defined(CTB_ENV64)
		#define FILLBBUFFER() \
			n = (((BBTYPE) source[0]) << 0x00) | \
			    (((BBTYPE) source[1]) << 0x08) | \
			    (((BBTYPE) source[2]) << 0x10) | \
			    (((BBTYPE) source[3]) << 0x18) | \
			    (((BBTYPE) source[4]) << 0x20) | \
			    (((BBTYPE) source[5]) << 0x28);
	#else
		#define FILLBBUFFER() \
			n = (((BBTYPE) source[0]) << 0x00) | \
			    (((BBTYPE) source[1]) << 0x08);
	#endif
#endif


static uintxx
decodefast(struct TInflator* state)
{
	uint8* source;
	uint8* target;
	uint8* send;
	uint8* tend;
	uintxx bb;
	uintxx bc;
	uintxx r;
	uintxx length;
	uintxx distance;
	uintxx targetsz;
	uintxx maxrun;
	uint8* buffer;
	BBTYPE n;
	struct TINFLTTEntry e;

	source = state->source;
	target = state->target;
	send   = state->send;
	tend   = state->tend;

	bb = PRVT->bbuffer;
	bc = PRVT->bcount;
	r = 0;

L_LOOP:
	if (UNLIKELY(tend - target < FASTTGTLEFT || send - source < FASTSRCLEFT))
		goto L_DONE;

	if (LIKELY(bc < 15)) {
		FILLBBUFFER();

		bb |= n << bc;
		bc +=     (sizeof(BBTYPE) - 2) << 3;
		source += (sizeof(BBTYPE) - 2);
	}

	/* decode literal or length */
	e = PRVT->ltable[GETBITS(bb, LROOTBITS)];
	if (LIKELY(e.etag == SUBTABLEENTRY)) {
		e = PRVT->ltable[e.info + (GETBITS(bb, e.length) >> LROOTBITS)];
	}
	DROPBITS(bb, bc, e.length);

	if (LIKELY(e.etag == LITERALSYMBOL)) {
		*target++ = (uint8) e.info;
		goto L_LOOP;
	}

	if (UNLIKELY(e.etag == ENDOFBLOCK)) {
		r = ENDOFBLOCK;
		goto L_DONE;
	}

	if (UNLIKELY(e.etag == INVALIDCODE)) {
		SETERROR(INFLT_EBADCODE);
		return INFLT_ERROR;
	}

	/* length */
	if (UNLIKELY(e.etag > bc)) {
		FILLBBUFFER();

		bb |= n << bc;
		bc +=     (sizeof(BBTYPE) - 2) << 3;
		source += (sizeof(BBTYPE) - 2);
	}

	length = e.info + GETBITS(bb, e.etag);
	DROPBITS(bb, bc, e.etag);

	if (UNLIKELY(bc < 15)) {
		FILLBBUFFER();

		bb |= n << bc;
		bc +=     (sizeof(BBTYPE) - 2) << 3;
		source += (sizeof(BBTYPE) - 2);
	}

	/* decode distance */
	e = PRVT->dtable[GETBITS(bb, DROOTBITS)];
	if (LIKELY(e.etag == SUBTABLEENTRY)) {
		e = PRVT->dtable[e.info + (GETBITS(bb, e.length) >> DROOTBITS)];
	}
	DROPBITS(bb, bc, e.length);

	if (UNLIKELY(e.etag == INVALIDCODE)) {
		SETERROR(INFLT_EBADCODE);
		return INFLT_ERROR;
	}

	if (UNLIKELY(e.etag > bc)) {
		FILLBBUFFER();

		bb |= n << bc;
		bc +=     (sizeof(BBTYPE) - 2) << 3;
		source += (sizeof(BBTYPE) - 2);
	}

	distance = e.info + GETBITS(bb, e.etag);
	DROPBITS(bb, bc, e.etag);

	targetsz = (uintxx) (target - state->tbgn);
	if (LIKELY(distance < targetsz)) {
		uint8* end;

		buffer = target - distance;
		maxrun = length;
		end = target + maxrun;

		if (distance >= sizeof(BBTYPE)) {
#if defined(CTB_FASTUNALIGNED)
			((uint32*) target)[0] = ((uint32*) buffer)[0];
#if defined(CTB_ENV64)
			((uint32*) target)[1] = ((uint32*) buffer)[1];
#endif
#else
			target[0] = buffer[0];
			target[1] = buffer[1];
			target[2] = buffer[2];
			target[3] = buffer[3];
#if defined(CTB_ENV64)
			target[4] = buffer[4];
			target[5] = buffer[5];
			target[6] = buffer[6];
			target[7] = buffer[7];
#endif
#endif
			target += sizeof(BBTYPE);
			buffer += sizeof(BBTYPE);
			do {
#if defined(CTB_FASTUNALIGNED)
				((uint32*) target)[0] = ((uint32*) buffer)[0];
#if defined(CTB_ENV64)
				((uint32*) target)[1] = ((uint32*) buffer)[1];
#endif
#else
				target[0] = buffer[0];
				target[1] = buffer[1];
				target[2] = buffer[2];
				target[3] = buffer[3];
#if defined(CTB_ENV64)
				target[4] = buffer[4];
				target[5] = buffer[5];
				target[6] = buffer[6];
				target[7] = buffer[7];
#endif
#endif
				target += sizeof(BBTYPE);
				buffer += sizeof(BBTYPE);
			} while (target < end);
			target = end;
		}
		else {
			*target++ = *buffer++;
			*target++ = *buffer++;
			do {
				*target++ = *buffer++;
			} while (target < end);
		}
	}
	else {
		do {
			maxrun = (uintxx) (target - state->tbgn);
			if (distance > maxrun) {
				uintxx offset;

				maxrun = distance - maxrun;

				if (UNLIKELY(maxrun > PRVT->count)) {
					SETERROR(INFLT_EFAROFFSET);
					return INFLT_ERROR;
				}

				buffer = PRVT->window;
				if (maxrun > PRVT->end) {
					maxrun -= PRVT->end;

					offset = WNDWSIZE  - maxrun;
				}
				else {
					offset = PRVT->end - maxrun;
				}
				buffer += offset;

				if (maxrun > length)
					maxrun = length;
			}
			else {
				buffer = target - distance;
				maxrun = length;
			}

			length -= maxrun;
			do {
				*target++ = *buffer++;
			} while (--maxrun);
		} while (length);
	}
	goto L_LOOP;

L_DONE:
	/* restore unused bytes */
	n = bc & 7;
	PRVT->bbuffer = bb & ((((uintxx) 1) << n) - 1);
	PRVT->bcount  = n;

	state->source = source - ((bc - n) >> 3);
	state->target = target;
	return r;
}


#undef slength
#undef sbextra
#undef sdistance

#undef PRVT

#else

/* ****************************************************************************
 * Static Tables
 *************************************************************************** */

static const struct TINFLTTEntry lsttctable[1L << LROOTBITS] = {
	{0x0100, 0x11, 0x07}, {0x0050, 0x10, 0x08}, {0x0010, 0x10, 0x08},
	{0x0073, 0x04, 0x08}, {0x001f, 0x02, 0x07}, {0x0070, 0x10, 0x08},
	{0x0030, 0x10, 0x08}, {0x00c0, 0x10, 0x09}, {0x000a, 0x00, 0x07},
	{0x0060, 0x10, 0x08}, {0x0020, 0x10, 0x08}, {0x00a0, 0x10, 0x09},
	{0x0000, 0x10, 0x08}, {0x0080, 0x10, 0x08}, {0x0040, 0x10, 0x08},
	{0x00e0, 0x10, 0x09}, {0x0006, 0x00, 0x07}, {0x0058, 0x10, 0x08},
	{0x0018, 0x10, 0x08}, {0x0090, 0x10, 0x09}, {0x003b, 0x03, 0x07},
	{0x0078, 0x10, 0x08}, {0x0038, 0x10, 0x08}, {0x00d0, 0x10, 0x09},
	{0x0011, 0x01, 0x07}, {0x0068, 0x10, 0x08}, {0x0028, 0x10, 0x08},
	{0x00b0, 0x10, 0x09}, {0x0008, 0x10, 0x08}, {0x0088, 0x10, 0x08},
	{0x0048, 0x10, 0x08}, {0x00f0, 0x10, 0x09}, {0x0004, 0x00, 0x07},
	{0x0054, 0x10, 0x08}, {0x0014, 0x10, 0x08}, {0x00e3, 0x05, 0x08},
	{0x002b, 0x03, 0x07}, {0x0074, 0x10, 0x08}, {0x0034, 0x10, 0x08},
	{0x00c8, 0x10, 0x09}, {0x000d, 0x01, 0x07}, {0x0064, 0x10, 0x08},
	{0x0024, 0x10, 0x08}, {0x00a8, 0x10, 0x09}, {0x0004, 0x10, 0x08},
	{0x0084, 0x10, 0x08}, {0x0044, 0x10, 0x08}, {0x00e8, 0x10, 0x09},
	{0x0008, 0x00, 0x07}, {0x005c, 0x10, 0x08}, {0x001c, 0x10, 0x08},
	{0x0098, 0x10, 0x09}, {0x0053, 0x04, 0x07}, {0x007c, 0x10, 0x08},
	{0x003c, 0x10, 0x08}, {0x00d8, 0x10, 0x09}, {0x0017, 0x02, 0x07},
	{0x006c, 0x10, 0x08}, {0x002c, 0x10, 0x08}, {0x00b8, 0x10, 0x09},
	{0x000c, 0x10, 0x08}, {0x008c, 0x10, 0x08}, {0x004c, 0x10, 0x08},
	{0x00f8, 0x10, 0x09}, {0x0003, 0x00, 0x07}, {0x0052, 0x10, 0x08},
	{0x0012, 0x10, 0x08}, {0x00a3, 0x05, 0x08}, {0x0023, 0x03, 0x07},
	{0x0072, 0x10, 0x08}, {0x0032, 0x10, 0x08}, {0x00c4, 0x10, 0x09},
	{0x000b, 0x01, 0x07}, {0x0062, 0x10, 0x08}, {0x0022, 0x10, 0x08},
	{0x00a4, 0x10, 0x09}, {0x0002, 0x10, 0x08}, {0x0082, 0x10, 0x08},
	{0x0042, 0x10, 0x08}, {0x00e4, 0x10, 0x09}, {0x0007, 0x00, 0x07},
	{0x005a, 0x10, 0x08}, {0x001a, 0x10, 0x08}, {0x0094, 0x10, 0x09},
	{0x0043, 0x04, 0x07}, {0x007a, 0x10, 0x08}, {0x003a, 0x10, 0x08},
	{0x00d4, 0x10, 0x09}, {0x0013, 0x02, 0x07}, {0x006a, 0x10, 0x08},
	{0x002a, 0x10, 0x08}, {0x00b4, 0x10, 0x09}, {0x000a, 0x10, 0x08},
	{0x008a, 0x10, 0x08}, {0x004a, 0x10, 0x08}, {0x00f4, 0x10, 0x09},
	{0x0005, 0x00, 0x07}, {0x0056, 0x10, 0x08}, {0x0016, 0x10, 0x08},
	{0x0000, 0x00, 0x08}, {0x0033, 0x03, 0x07}, {0x0076, 0x10, 0x08},
	{0x0036, 0x10, 0x08}, {0x00cc, 0x10, 0x09}, {0x000f, 0x01, 0x07},
	{0x0066, 0x10, 0x08}, {0x0026, 0x10, 0x08}, {0x00ac, 0x10, 0x09},
	{0x0006, 0x10, 0x08}, {0x0086, 0x10, 0x08}, {0x0046, 0x10, 0x08},
	{0x00ec, 0x10, 0x09}, {0x0009, 0x00, 0x07}, {0x005e, 0x10, 0x08},
	{0x001e, 0x10, 0x08}, {0x009c, 0x10, 0x09}, {0x0063, 0x04, 0x07},
	{0x007e, 0x10, 0x08}, {0x003e, 0x10, 0x08}, {0x00dc, 0x10, 0x09},
	{0x001b, 0x02, 0x07}, {0x006e, 0x10, 0x08}, {0x002e, 0x10, 0x08},
	{0x00bc, 0x10, 0x09}, {0x000e, 0x10, 0x08}, {0x008e, 0x10, 0x08},
	{0x004e, 0x10, 0x08}, {0x00fc, 0x10, 0x09}, {0x0100, 0x11, 0x07},
	{0x0051, 0x10, 0x08}, {0x0011, 0x10, 0x08}, {0x0083, 0x05, 0x08},
	{0x001f, 0x02, 0x07}, {0x0071, 0x10, 0x08}, {0x0031, 0x10, 0x08},
	{0x00c2, 0x10, 0x09}, {0x000a, 0x00, 0x07}, {0x0061, 0x10, 0x08},
	{0x0021, 0x10, 0x08}, {0x00a2, 0x10, 0x09}, {0x0001, 0x10, 0x08},
	{0x0081, 0x10, 0x08}, {0x0041, 0x10, 0x08}, {0x00e2, 0x10, 0x09},
	{0x0006, 0x00, 0x07}, {0x0059, 0x10, 0x08}, {0x0019, 0x10, 0x08},
	{0x0092, 0x10, 0x09}, {0x003b, 0x03, 0x07}, {0x0079, 0x10, 0x08},
	{0x0039, 0x10, 0x08}, {0x00d2, 0x10, 0x09}, {0x0011, 0x01, 0x07},
	{0x0069, 0x10, 0x08}, {0x0029, 0x10, 0x08}, {0x00b2, 0x10, 0x09},
	{0x0009, 0x10, 0x08}, {0x0089, 0x10, 0x08}, {0x0049, 0x10, 0x08},
	{0x00f2, 0x10, 0x09}, {0x0004, 0x00, 0x07}, {0x0055, 0x10, 0x08},
	{0x0015, 0x10, 0x08}, {0x0102, 0x00, 0x08}, {0x002b, 0x03, 0x07},
	{0x0075, 0x10, 0x08}, {0x0035, 0x10, 0x08}, {0x00ca, 0x10, 0x09},
	{0x000d, 0x01, 0x07}, {0x0065, 0x10, 0x08}, {0x0025, 0x10, 0x08},
	{0x00aa, 0x10, 0x09}, {0x0005, 0x10, 0x08}, {0x0085, 0x10, 0x08},
	{0x0045, 0x10, 0x08}, {0x00ea, 0x10, 0x09}, {0x0008, 0x00, 0x07},
	{0x005d, 0x10, 0x08}, {0x001d, 0x10, 0x08}, {0x009a, 0x10, 0x09},
	{0x0053, 0x04, 0x07}, {0x007d, 0x10, 0x08}, {0x003d, 0x10, 0x08},
	{0x00da, 0x10, 0x09}, {0x0017, 0x02, 0x07}, {0x006d, 0x10, 0x08},
	{0x002d, 0x10, 0x08}, {0x00ba, 0x10, 0x09}, {0x000d, 0x10, 0x08},
	{0x008d, 0x10, 0x08}, {0x004d, 0x10, 0x08}, {0x00fa, 0x10, 0x09},
	{0x0003, 0x00, 0x07}, {0x0053, 0x10, 0x08}, {0x0013, 0x10, 0x08},
	{0x00c3, 0x05, 0x08}, {0x0023, 0x03, 0x07}, {0x0073, 0x10, 0x08},
	{0x0033, 0x10, 0x08}, {0x00c6, 0x10, 0x09}, {0x000b, 0x01, 0x07},
	{0x0063, 0x10, 0x08}, {0x0023, 0x10, 0x08}, {0x00a6, 0x10, 0x09},
	{0x0003, 0x10, 0x08}, {0x0083, 0x10, 0x08}, {0x0043, 0x10, 0x08},
	{0x00e6, 0x10, 0x09}, {0x0007, 0x00, 0x07}, {0x005b, 0x10, 0x08},
	{0x001b, 0x10, 0x08}, {0x0096, 0x10, 0x09}, {0x0043, 0x04, 0x07},
	{0x007b, 0x10, 0x08}, {0x003b, 0x10, 0x08}, {0x00d6, 0x10, 0x09},
	{0x0013, 0x02, 0x07}, {0x006b, 0x10, 0x08}, {0x002b, 0x10, 0x08},
	{0x00b6, 0x10, 0x09}, {0x000b, 0x10, 0x08}, {0x008b, 0x10, 0x08},
	{0x004b, 0x10, 0x08}, {0x00f6, 0x10, 0x09}, {0x0005, 0x00, 0x07},
	{0x0057, 0x10, 0x08}, {0x0017, 0x10, 0x08}, {0x0000, 0x00, 0x08},
	{0x0033, 0x03, 0x07}, {0x0077, 0x10, 0x08}, {0x0037, 0x10, 0x08},
	{0x00ce, 0x10, 0x09}, {0x000f, 0x01, 0x07}, {0x0067, 0x10, 0x08},
	{0x0027, 0x10, 0x08}, {0x00ae, 0x10, 0x09}, {0x0007, 0x10, 0x08},
	{0x0087, 0x10, 0x08}, {0x0047, 0x10, 0x08}, {0x00ee, 0x10, 0x09},
	{0x0009, 0x00, 0x07}, {0x005f, 0x10, 0x08}, {0x001f, 0x10, 0x08},
	{0x009e, 0x10, 0x09}, {0x0063, 0x04, 0x07}, {0x007f, 0x10, 0x08},
	{0x003f, 0x10, 0x08}, {0x00de, 0x10, 0x09}, {0x001b, 0x02, 0x07},
	{0x006f, 0x10, 0x08}, {0x002f, 0x10, 0x08}, {0x00be, 0x10, 0x09},
	{0x000f, 0x10, 0x08}, {0x008f, 0x10, 0x08}, {0x004f, 0x10, 0x08},
	{0x00fe, 0x10, 0x09}, {0x0100, 0x11, 0x07}, {0x0050, 0x10, 0x08},
	{0x0010, 0x10, 0x08}, {0x0073, 0x04, 0x08}, {0x001f, 0x02, 0x07},
	{0x0070, 0x10, 0x08}, {0x0030, 0x10, 0x08}, {0x00c1, 0x10, 0x09},
	{0x000a, 0x00, 0x07}, {0x0060, 0x10, 0x08}, {0x0020, 0x10, 0x08},
	{0x00a1, 0x10, 0x09}, {0x0000, 0x10, 0x08}, {0x0080, 0x10, 0x08},
	{0x0040, 0x10, 0x08}, {0x00e1, 0x10, 0x09}, {0x0006, 0x00, 0x07},
	{0x0058, 0x10, 0x08}, {0x0018, 0x10, 0x08}, {0x0091, 0x10, 0x09},
	{0x003b, 0x03, 0x07}, {0x0078, 0x10, 0x08}, {0x0038, 0x10, 0x08},
	{0x00d1, 0x10, 0x09}, {0x0011, 0x01, 0x07}, {0x0068, 0x10, 0x08},
	{0x0028, 0x10, 0x08}, {0x00b1, 0x10, 0x09}, {0x0008, 0x10, 0x08},
	{0x0088, 0x10, 0x08}, {0x0048, 0x10, 0x08}, {0x00f1, 0x10, 0x09},
	{0x0004, 0x00, 0x07}, {0x0054, 0x10, 0x08}, {0x0014, 0x10, 0x08},
	{0x00e3, 0x05, 0x08}, {0x002b, 0x03, 0x07}, {0x0074, 0x10, 0x08},
	{0x0034, 0x10, 0x08}, {0x00c9, 0x10, 0x09}, {0x000d, 0x01, 0x07},
	{0x0064, 0x10, 0x08}, {0x0024, 0x10, 0x08}, {0x00a9, 0x10, 0x09},
	{0x0004, 0x10, 0x08}, {0x0084, 0x10, 0x08}, {0x0044, 0x10, 0x08},
	{0x00e9, 0x10, 0x09}, {0x0008, 0x00, 0x07}, {0x005c, 0x10, 0x08},
	{0x001c, 0x10, 0x08}, {0x0099, 0x10, 0x09}, {0x0053, 0x04, 0x07},
	{0x007c, 0x10, 0x08}, {0x003c, 0x10, 0x08}, {0x00d9, 0x10, 0x09},
	{0x0017, 0x02, 0x07}, {0x006c, 0x10, 0x08}, {0x002c, 0x10, 0x08},
	{0x00b9, 0x10, 0x09}, {0x000c, 0x10, 0x08}, {0x008c, 0x10, 0x08},
	{0x004c, 0x10, 0x08}, {0x00f9, 0x10, 0x09}, {0x0003, 0x00, 0x07},
	{0x0052, 0x10, 0x08}, {0x0012, 0x10, 0x08}, {0x00a3, 0x05, 0x08},
	{0x0023, 0x03, 0x07}, {0x0072, 0x10, 0x08}, {0x0032, 0x10, 0x08},
	{0x00c5, 0x10, 0x09}, {0x000b, 0x01, 0x07}, {0x0062, 0x10, 0x08},
	{0x0022, 0x10, 0x08}, {0x00a5, 0x10, 0x09}, {0x0002, 0x10, 0x08},
	{0x0082, 0x10, 0x08}, {0x0042, 0x10, 0x08}, {0x00e5, 0x10, 0x09},
	{0x0007, 0x00, 0x07}, {0x005a, 0x10, 0x08}, {0x001a, 0x10, 0x08},
	{0x0095, 0x10, 0x09}, {0x0043, 0x04, 0x07}, {0x007a, 0x10, 0x08},
	{0x003a, 0x10, 0x08}, {0x00d5, 0x10, 0x09}, {0x0013, 0x02, 0x07},
	{0x006a, 0x10, 0x08}, {0x002a, 0x10, 0x08}, {0x00b5, 0x10, 0x09},
	{0x000a, 0x10, 0x08}, {0x008a, 0x10, 0x08}, {0x004a, 0x10, 0x08},
	{0x00f5, 0x10, 0x09}, {0x0005, 0x00, 0x07}, {0x0056, 0x10, 0x08},
	{0x0016, 0x10, 0x08}, {0x0000, 0x00, 0x08}, {0x0033, 0x03, 0x07},
	{0x0076, 0x10, 0x08}, {0x0036, 0x10, 0x08}, {0x00cd, 0x10, 0x09},
	{0x000f, 0x01, 0x07}, {0x0066, 0x10, 0x08}, {0x0026, 0x10, 0x08},
	{0x00ad, 0x10, 0x09}, {0x0006, 0x10, 0x08}, {0x0086, 0x10, 0x08},
	{0x0046, 0x10, 0x08}, {0x00ed, 0x10, 0x09}, {0x0009, 0x00, 0x07},
	{0x005e, 0x10, 0x08}, {0x001e, 0x10, 0x08}, {0x009d, 0x10, 0x09},
	{0x0063, 0x04, 0x07}, {0x007e, 0x10, 0x08}, {0x003e, 0x10, 0x08},
	{0x00dd, 0x10, 0x09}, {0x001b, 0x02, 0x07}, {0x006e, 0x10, 0x08},
	{0x002e, 0x10, 0x08}, {0x00bd, 0x10, 0x09}, {0x000e, 0x10, 0x08},
	{0x008e, 0x10, 0x08}, {0x004e, 0x10, 0x08}, {0x00fd, 0x10, 0x09},
	{0x0100, 0x11, 0x07}, {0x0051, 0x10, 0x08}, {0x0011, 0x10, 0x08},
	{0x0083, 0x05, 0x08}, {0x001f, 0x02, 0x07}, {0x0071, 0x10, 0x08},
	{0x0031, 0x10, 0x08}, {0x00c3, 0x10, 0x09}, {0x000a, 0x00, 0x07},
	{0x0061, 0x10, 0x08}, {0x0021, 0x10, 0x08}, {0x00a3, 0x10, 0x09},
	{0x0001, 0x10, 0x08}, {0x0081, 0x10, 0x08}, {0x0041, 0x10, 0x08},
	{0x00e3, 0x10, 0x09}, {0x0006, 0x00, 0x07}, {0x0059, 0x10, 0x08},
	{0x0019, 0x10, 0x08}, {0x0093, 0x10, 0x09}, {0x003b, 0x03, 0x07},
	{0x0079, 0x10, 0x08}, {0x0039, 0x10, 0x08}, {0x00d3, 0x10, 0x09},
	{0x0011, 0x01, 0x07}, {0x0069, 0x10, 0x08}, {0x0029, 0x10, 0x08},
	{0x00b3, 0x10, 0x09}, {0x0009, 0x10, 0x08}, {0x0089, 0x10, 0x08},
	{0x0049, 0x10, 0x08}, {0x00f3, 0x10, 0x09}, {0x0004, 0x00, 0x07},
	{0x0055, 0x10, 0x08}, {0x0015, 0x10, 0x08}, {0x0102, 0x00, 0x08},
	{0x002b, 0x03, 0x07}, {0x0075, 0x10, 0x08}, {0x0035, 0x10, 0x08},
	{0x00cb, 0x10, 0x09}, {0x000d, 0x01, 0x07}, {0x0065, 0x10, 0x08},
	{0x0025, 0x10, 0x08}, {0x00ab, 0x10, 0x09}, {0x0005, 0x10, 0x08},
	{0x0085, 0x10, 0x08}, {0x0045, 0x10, 0x08}, {0x00eb, 0x10, 0x09},
	{0x0008, 0x00, 0x07}, {0x005d, 0x10, 0x08}, {0x001d, 0x10, 0x08},
	{0x009b, 0x10, 0x09}, {0x0053, 0x04, 0x07}, {0x007d, 0x10, 0x08},
	{0x003d, 0x10, 0x08}, {0x00db, 0x10, 0x09}, {0x0017, 0x02, 0x07},
	{0x006d, 0x10, 0x08}, {0x002d, 0x10, 0x08}, {0x00bb, 0x10, 0x09},
	{0x000d, 0x10, 0x08}, {0x008d, 0x10, 0x08}, {0x004d, 0x10, 0x08},
	{0x00fb, 0x10, 0x09}, {0x0003, 0x00, 0x07}, {0x0053, 0x10, 0x08},
	{0x0013, 0x10, 0x08}, {0x00c3, 0x05, 0x08}, {0x0023, 0x03, 0x07},
	{0x0073, 0x10, 0x08}, {0x0033, 0x10, 0x08}, {0x00c7, 0x10, 0x09},
	{0x000b, 0x01, 0x07}, {0x0063, 0x10, 0x08}, {0x0023, 0x10, 0x08},
	{0x00a7, 0x10, 0x09}, {0x0003, 0x10, 0x08}, {0x0083, 0x10, 0x08},
	{0x0043, 0x10, 0x08}, {0x00e7, 0x10, 0x09}, {0x0007, 0x00, 0x07},
	{0x005b, 0x10, 0x08}, {0x001b, 0x10, 0x08}, {0x0097, 0x10, 0x09},
	{0x0043, 0x04, 0x07}, {0x007b, 0x10, 0x08}, {0x003b, 0x10, 0x08},
	{0x00d7, 0x10, 0x09}, {0x0013, 0x02, 0x07}, {0x006b, 0x10, 0x08},
	{0x002b, 0x10, 0x08}, {0x00b7, 0x10, 0x09}, {0x000b, 0x10, 0x08},
	{0x008b, 0x10, 0x08}, {0x004b, 0x10, 0x08}, {0x00f7, 0x10, 0x09},
	{0x0005, 0x00, 0x07}, {0x0057, 0x10, 0x08}, {0x0017, 0x10, 0x08},
	{0x0000, 0x00, 0x08}, {0x0033, 0x03, 0x07}, {0x0077, 0x10, 0x08},
	{0x0037, 0x10, 0x08}, {0x00cf, 0x10, 0x09}, {0x000f, 0x01, 0x07},
	{0x0067, 0x10, 0x08}, {0x0027, 0x10, 0x08}, {0x00af, 0x10, 0x09},
	{0x0007, 0x10, 0x08}, {0x0087, 0x10, 0x08}, {0x0047, 0x10, 0x08},
	{0x00ef, 0x10, 0x09}, {0x0009, 0x00, 0x07}, {0x005f, 0x10, 0x08},
	{0x001f, 0x10, 0x08}, {0x009f, 0x10, 0x09}, {0x0063, 0x04, 0x07},
	{0x007f, 0x10, 0x08}, {0x003f, 0x10, 0x08}, {0x00df, 0x10, 0x09},
	{0x001b, 0x02, 0x07}, {0x006f, 0x10, 0x08}, {0x002f, 0x10, 0x08},
	{0x00bf, 0x10, 0x09}, {0x000f, 0x10, 0x08}, {0x008f, 0x10, 0x08},
	{0x004f, 0x10, 0x08}, {0x00ff, 0x10, 0x09}
};

static const struct TINFLTTEntry dsttctable[1L << DROOTBITS] = {
	{0x0001, 0x00, 0x05}, {0x0101, 0x07, 0x05}, {0x0011, 0x03, 0x05},
	{0x1001, 0x0b, 0x05}, {0x0005, 0x01, 0x05}, {0x0401, 0x09, 0x05},
	{0x0041, 0x05, 0x05}, {0x4001, 0x0d, 0x05}, {0x0003, 0x00, 0x05},
	{0x0201, 0x08, 0x05}, {0x0021, 0x04, 0x05}, {0x2001, 0x0c, 0x05},
	{0x0009, 0x02, 0x05}, {0x0801, 0x0a, 0x05}, {0x0081, 0x06, 0x05},
	{0x0000, 0x00, 0x05}, {0x0002, 0x00, 0x05}, {0x0181, 0x07, 0x05},
	{0x0019, 0x03, 0x05}, {0x1801, 0x0b, 0x05}, {0x0007, 0x01, 0x05},
	{0x0601, 0x09, 0x05}, {0x0061, 0x05, 0x05}, {0x6001, 0x0d, 0x05},
	{0x0004, 0x00, 0x05}, {0x0301, 0x08, 0x05}, {0x0031, 0x04, 0x05},
	{0x3001, 0x0c, 0x05}, {0x000d, 0x02, 0x05}, {0x0c01, 0x0a, 0x05},
	{0x00c1, 0x06, 0x05}, {0x0000, 0x00, 0x05}, {0x0001, 0x00, 0x05},
	{0x0101, 0x07, 0x05}, {0x0011, 0x03, 0x05}, {0x1001, 0x0b, 0x05},
	{0x0005, 0x01, 0x05}, {0x0401, 0x09, 0x05}, {0x0041, 0x05, 0x05},
	{0x4001, 0x0d, 0x05}, {0x0003, 0x00, 0x05}, {0x0201, 0x08, 0x05},
	{0x0021, 0x04, 0x05}, {0x2001, 0x0c, 0x05}, {0x0009, 0x02, 0x05},
	{0x0801, 0x0a, 0x05}, {0x0081, 0x06, 0x05}, {0x0000, 0x00, 0x05},
	{0x0002, 0x00, 0x05}, {0x0181, 0x07, 0x05}, {0x0019, 0x03, 0x05},
	{0x1801, 0x0b, 0x05}, {0x0007, 0x01, 0x05}, {0x0601, 0x09, 0x05},
	{0x0061, 0x05, 0x05}, {0x6001, 0x0d, 0x05}, {0x0004, 0x00, 0x05},
	{0x0301, 0x08, 0x05}, {0x0031, 0x04, 0x05}, {0x3001, 0x0c, 0x05},
	{0x000d, 0x02, 0x05}, {0x0c01, 0x0a, 0x05}, {0x00c1, 0x06, 0x05},
	{0x0000, 0x00, 0x05}, {0x0001, 0x00, 0x05}, {0x0101, 0x07, 0x05},
	{0x0011, 0x03, 0x05}, {0x1001, 0x0b, 0x05}, {0x0005, 0x01, 0x05},
	{0x0401, 0x09, 0x05}, {0x0041, 0x05, 0x05}, {0x4001, 0x0d, 0x05},
	{0x0003, 0x00, 0x05}, {0x0201, 0x08, 0x05}, {0x0021, 0x04, 0x05},
	{0x2001, 0x0c, 0x05}, {0x0009, 0x02, 0x05}, {0x0801, 0x0a, 0x05},
	{0x0081, 0x06, 0x05}, {0x0000, 0x00, 0x05}, {0x0002, 0x00, 0x05},
	{0x0181, 0x07, 0x05}, {0x0019, 0x03, 0x05}, {0x1801, 0x0b, 0x05},
	{0x0007, 0x01, 0x05}, {0x0601, 0x09, 0x05}, {0x0061, 0x05, 0x05},
	{0x6001, 0x0d, 0x05}, {0x0004, 0x00, 0x05}, {0x0301, 0x08, 0x05},
	{0x0031, 0x04, 0x05}, {0x3001, 0x0c, 0x05}, {0x000d, 0x02, 0x05},
	{0x0c01, 0x0a, 0x05}, {0x00c1, 0x06, 0x05}, {0x0000, 0x00, 0x05},
	{0x0001, 0x00, 0x05}, {0x0101, 0x07, 0x05}, {0x0011, 0x03, 0x05},
	{0x1001, 0x0b, 0x05}, {0x0005, 0x01, 0x05}, {0x0401, 0x09, 0x05},
	{0x0041, 0x05, 0x05}, {0x4001, 0x0d, 0x05}, {0x0003, 0x00, 0x05},
	{0x0201, 0x08, 0x05}, {0x0021, 0x04, 0x05}, {0x2001, 0x0c, 0x05},
	{0x0009, 0x02, 0x05}, {0x0801, 0x0a, 0x05}, {0x0081, 0x06, 0x05},
	{0x0000, 0x00, 0x05}, {0x0002, 0x00, 0x05}, {0x0181, 0x07, 0x05},
	{0x0019, 0x03, 0x05}, {0x1801, 0x0b, 0x05}, {0x0007, 0x01, 0x05},
	{0x0601, 0x09, 0x05}, {0x0061, 0x05, 0x05}, {0x6001, 0x0d, 0x05},
	{0x0004, 0x00, 0x05}, {0x0301, 0x08, 0x05}, {0x0031, 0x04, 0x05},
	{0x3001, 0x0c, 0x05}, {0x000d, 0x02, 0x05}, {0x0c01, 0x0a, 0x05},
	{0x00c1, 0x06, 0x05}, {0x0000, 0x00, 0x05}
};


#define AUTOINCLUDE_1
	#include "inflator.c"
#undef  AUTOINCLUDE_1

#endif
