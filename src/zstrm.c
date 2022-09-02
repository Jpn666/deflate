/*
 * Copyright (C) 2021, jpn
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

#include "../zstrm.h"
#include <crypto/crc32.h>
#include <crypto/adler32.h>


#define ZIOBFFRSZ 8192


CTB_INLINE void*
reserve(struct TZStrm* p, uintxx amount)
{
	if (p->allocator) {
		return p->allocator->reserve(p->allocator->user, amount);
	}
	return CTB_MALLOC(amount);
}

CTB_INLINE void
release(struct TZStrm* p, void* memory)
{
	if (p->allocator) {
		p->allocator->release(p->allocator->user, memory);
		return;
	}
	CTB_FREE(memory);
}


#define ZSTRM_MODEMASK 0x03
#define ZSTRM_TYPEMASK 0x1c

TZStrm*
zstrm_create(uintxx flags, uintxx level, TAllocator* allocator)
{
	uintxx mode;
	uintxx type;
	struct TZStrm* state;

	mode = flags & ZSTRM_MODEMASK;
	type = flags & ZSTRM_TYPEMASK;
	switch (mode) {
		case ZSTRM_WMODE:
		case ZSTRM_RMODE:
			break;
		default:
			/* invalid mode */
			return NULL;
	}
	if (type == 0) {
		return NULL;
	}

	if (mode == ZSTRM_WMODE) {
		if (level > 9) {
			/* invalid compression level */
			return NULL;
		}

		if ((type & ZSTRM_DFLT) && (type & ~ZSTRM_DFLT)) {
			return NULL;
		}
		if ((type & ZSTRM_ZLIB) && (type & ~ZSTRM_ZLIB)) {
			return NULL;
		}
		if ((type & ZSTRM_GZIP) && (type & ~ZSTRM_GZIP)) {
			return NULL;
		}
	}

	if (allocator) {
		state = allocator->reserve(allocator->user, sizeof(struct TZStrm));
	}
	else {
		state = CTB_MALLOC(sizeof(struct TZStrm));
	}
	if (state == NULL) {
		return NULL;
	}
	state->allocator = allocator;

	state->sbgn = reserve(state, ZIOBFFRSZ);
	state->tbgn = reserve(state, ZIOBFFRSZ);
	if (state->sbgn == NULL || state->tbgn == NULL) {
		if (state->sbgn) {
			release(state, state->sbgn);
		}
		if (state->tbgn) {
			release(state, state->tbgn);
		}
		release(state, state);
		return NULL;
	}
	state->infltr = NULL;
	state->defltr = NULL;

	if (mode == ZSTRM_RMODE) {
		state->infltr = inflator_create(allocator);
		if (state->infltr == NULL) {
			zstrm_destroy(state);
			return NULL;
		}
	}
	else {
		state->defltr = deflator_create((state->level = level), allocator);
		if (state->defltr == NULL) {
			zstrm_destroy(state);
			return NULL;
		}
	}

	state->smode = mode;
	state->mtype = type;
	if (mode == ZSTRM_WMODE) {
		state->stype = type;
		if (flags & ZSTRM_DOCRC32) state->docrc32 = 1;
		if (flags & ZSTRM_DOADLER) state->doadler = 1;

		if (type ^ ZSTRM_DFLT) {
			if (type & ZSTRM_GZIP) state->docrc32 = 1;
			if (type & ZSTRM_ZLIB) state->doadler = 1;
		}
	}
	state->flags = flags;
	zstrm_reset(state);
	return state;
}

void
zstrm_destroy(TZStrm* state)
{
	if (state == NULL) {
		return;
	}

	release(state, state->sbgn);
	release(state, state->tbgn);

	if (state->infltr) {
		inflator_destroy(state->infltr);
	}
	if (state->defltr) {
		deflator_destroy(state->defltr);
	}
	release(state, state);
}

void
zstrm_reset(TZStrm* state)
{
	ASSERT(state);

	state->state  = 0;
	state->error  = 0;
	if (state->smode == ZSTRM_RMODE) {
		state->stype = 0;
	}

	state->dictid = 0;
	state->dict   = 0;
	if (state->smode == ZSTRM_RMODE) {
		state->docrc32 = 0;
		state->doadler = 0;
		if (state->flags & ZSTRM_DOCRC32) state->docrc32 = 1;
		if (state->flags & ZSTRM_DOADLER) state->doadler = 1;
	}
	state->crc32 = 0xffffffff;
	state->adler = 1;
	state->total = 0;

	state->result = 0;
	state->iofn    = NULL;
	state->payload = NULL;

	state->source = state->sbgn;
	state->send   = state->sbgn;
	state->target = state->tbgn;
	state->tend   = state->tbgn;
	state->sbgn[0] = 0x07;  /* invalid stream */
	state->tbgn[0] = 0x00;
	if (state->infltr) {
		inflator_reset(state->infltr);
	}
	if (state->defltr) {
		state->send += ZIOBFFRSZ;
		state->tend += ZIOBFFRSZ;
		deflator_reset(state->defltr, state->level);
	}
}


#define SETERROR(ERROR) (state->error = (ERROR))
#define SETSTATE(STATE) (state->state = (STATE))

void
zstrm_setiofn(TZStrm* state, TZStrmIOFn fn, void* payload)
{
	ASSERT(state);

	if (state->state) {
		SETSTATE(4);
		if (state->error == 0) {
			SETERROR(ZSTRM_EINCORRECTUSE);
		}
		return;
	}
	SETSTATE(1);

	state->iofn    = fn;
	state->payload = payload;
}


static uintxx parsehead(TZStrm* state);

void
zstrm_setdctn(TZStrm* state, uint8* dict, uintxx size)
{
	uint32 adler;
	ASSERT(state);

	if (state->state == 0 || state->state == 4) {
		if (state->state == 4) {
			return;
		}
		goto L_ERROR;
	}

	if (state->smode == ZSTRM_RMODE) {
		if (state->state == 1) {
			if (parsehead(state) == 0) {
				goto L_ERROR;
			}
		}

		if (state->stype == ZSTRM_GZIP) {
			goto L_ERROR;
		}
		adler = adler32_update(1, dict, size);
		if (state->state == 2) {
			if (adler ^ state->dictid) {
				SETERROR(ZSTRM_EINCORRECTDICT);
				goto L_ERROR;
			}
		}
		else {
			state->dictid = adler;
		}
		SETSTATE(3);
		inflator_setdctnr(state->infltr, dict, size);
	}

	if (state->smode == ZSTRM_WMODE) {
		if (state->state ^ 1) {
			if (state->state == 4) {
				return;
			}
			goto L_ERROR;
		}
		if (state->stype & ZSTRM_GZIP || state->dict == 1) {
			goto L_ERROR;
		}

		state->dictid = adler32_update(1, dict, size);;
		state->dict   = 1;
		deflator_setdctnr(state->defltr, dict, size);
	}
	return;

	L_ERROR:
	if (state->error == 0)
		SETERROR(ZSTRM_EINCORRECTUSE);
	SETSTATE(4);
}

uintxx
zstrm_getstate(TZStrm* state, uintxx* error)
{
	ASSERT(state);

	if (state->state == 1) {
		if (state->smode == ZSTRM_RMODE) {
			if (parsehead(state) == 0) {
				SETSTATE(4);
				goto L_ERROR;
			}

			if (state->state == 2) {
				return state->state;
			}
			SETSTATE(3);
		}
	}

	L_ERROR:
	if (error)
		error[0] = state->error;
	return state->state;
}


CTB_INLINE uint8
fetchbyte(struct TZStrm* state)
{
	intxx r;

	if (LIKELY(state->source < state->send)) {
		return *state->source++;
	}

	if (state->error == 0) {
		r = state->iofn(state->sbgn, ZIOBFFRSZ, state->payload);
		if (LIKELY(r)) {
			if ((uintxx) r > ZIOBFFRSZ) {
				SETERROR(ZSTRM_EIOERROR);
				return 0;
			}
			state->source = state->sbgn;
			state->send   = state->sbgn + r;
			return *state->source++;
		}
	}
	else {
		return 0;
	}
	SETERROR(ZSTRM_EBADDATA);
	return 0;
}

static uintxx
parsegziphead(struct TZStrm* state)
{
	uint8 id1;
	uint8 id2;
	uint8 flags;

	id1 = fetchbyte(state);
	id2 = fetchbyte(state);
	if (id1 != 0x1f || id2 != 0x8b) {
		if (state->error == 0)
			SETERROR(ZSTRM_EBADDATA);
		return 0;
	}

	/* compression method (deflate only) */
	if (fetchbyte(state) != 0x08) {
		if (state->error == 0)
			SETERROR(ZSTRM_EBADDATA);
		return 0;
	}

	flags = fetchbyte(state);
	fetchbyte(state);
	fetchbyte(state);
	fetchbyte(state);
	fetchbyte(state);
	fetchbyte(state);
	fetchbyte(state);

	/* extra */
	if (flags & 0x04) {
		uint8 a;
		uint8 b;
		uint16 length;

		a = fetchbyte(state);
		b = fetchbyte(state);
		for (length = a | (b << 0x08); length; length--)
			fetchbyte(state);
	}

	/* name, comment */
	if (flags & 0x08)
		while (fetchbyte(state));
	if (flags & 0x10)
		while (fetchbyte(state));

	/* header crc16 */
	if (flags & 0x02) {
		fetchbyte(state);
		fetchbyte(state);
	}

	if (state->error) {
		return 0;
	}
	return 1;
}

#define TOI32(A, B, C, D)  ((A) | (B << 0x08) | (C << 0x10) | (D << 0x18))

static uintxx
parsezlibhead(struct TZStrm* state)
{
	uint8 a;
	uint8 b;

	a = fetchbyte(state);
	b = fetchbyte(state);
	if (state->error == 0) {
		uintxx cm;
		uintxx ci;

		/* CM and CINFO */
		cm = (a >> 0) & 0x0f;
		ci = (a >> 4) & 0x0f;
		if (cm == 8 && ci <= 7) {
			uintxx fchck;
			uintxx fdict;

			/* FLG (FLaGs) */
			fchck = (b >> 0) & 0x1f;
			fdict = (b >> 5) & 0x01;
			(void) fchck;
			if (fdict) {
				uint8 c;
				uint8 d;

				d = fetchbyte(state);
				c = fetchbyte(state);
				b = fetchbyte(state);
				a = fetchbyte(state);
				if (state->error) {
					return 0;
				}
				state->dictid = TOI32(a, b, c, d);

				SETSTATE(2);
				return 1;
			}
		}
		else {
			if (state->error == 0)
				SETERROR(ZSTRM_EBADDATA);
			return 0;
		}
	}
	else {
		return 0;
	}

	return 1;
}

static uintxx
parsehead(struct TZStrm* state)
{
	uintxx type;
	uint8 head;

	head = fetchbyte(state);
	if (state->error) {
		return 0;
	}

	type = 0;
	if (head == 0x1f) {
		type = ZSTRM_GZIP;
	}
	else {
		head = head & 0x0f;
		if (head == 0x08) {
			type = ZSTRM_ZLIB;
		}
		else {
			if (head == 0x06 || head == 0x07) {
				/* invalid block type 11 (reserved) */
				SETERROR(ZSTRM_EBADDATA);
				return 0;
			}
			type = ZSTRM_DFLT;
		}
	}

	if ((state->mtype & type) == 0) {
		SETERROR(ZSTRM_EFORMAT);
		return 0;
	}
	else {
		state->stype = type;
	}

	state->source--;
	switch (state->stype) {
		case ZSTRM_GZIP: state->docrc32 = 1; parsegziphead(state); break;
		case ZSTRM_ZLIB: state->doadler = 1; parsezlibhead(state); break;
		case ZSTRM_DFLT:
			break;
	}
	if (state->error) {
		return 0;
	}

	inflator_setsrc(state->infltr, state->source, state->send - state->source);
	state->result = INFLT_TGTEXHSTD;
	return 1;
}


CTB_INLINE void
checkgziptail(struct TZStrm* state)
{
	uint32 crc32;
	uint32 total;
	uint8 a;
	uint8 b;
	uint8 c;
	uint8 d;

	/* crc32 */
	a = fetchbyte(state);
	b = fetchbyte(state);
	c = fetchbyte(state);
	d = fetchbyte(state);
	crc32 = TOI32(a, b, c, d);

	if (state->error == 0) {
		if (crc32 != state->crc32) {
			SETERROR(ZSTRM_ECHECKSUM);
			return;
		}
	}
	else {
		return;
	}

	/* isize */
	a = fetchbyte(state);
	b = fetchbyte(state);
	c = fetchbyte(state);
	d = fetchbyte(state);
	total = TOI32(a, b, c, d);

	if (total != state->total) {
		if (state->error) {
			return;
		}
		SETERROR(ZSTRM_EBADDATA);
	}
}

CTB_INLINE void
checkzlibtail(struct TZStrm* state)
{
	uint8 a;
	uint8 b;
	uint8 c;
	uint8 d;
	uint32 adler;

	/* tail */
	d = fetchbyte(state);
	c = fetchbyte(state);
	b = fetchbyte(state);
	a = fetchbyte(state);
	adler = TOI32(a, b, c, d);
	if (adler != state->adler) {
		if (state->error == 0)
			SETERROR(ZSTRM_ECHECKSUM);
		return;
	}
}

#undef TOI32


static uintxx
inflate(struct TZStrm* state, uint8* buffer, uintxx size)
{
	uintxx n;
	uintxx maxrun;
	uint8* bbegin;
	uint8* target;
	uint8* tend;

	target = state->target;
	tend   = state->tend;
	bbegin = buffer;

	while (LIKELY(size)) {
		maxrun = (uintxx) (tend - target);
		if (LIKELY(maxrun)) {
			if (maxrun > size)
				maxrun = size;

			for (size -= maxrun; maxrun >= 16; maxrun -= 16) {
				memcpy(buffer, target, 16);
				target += 16;
				buffer += 16;
			}
			for (;maxrun; maxrun--)
				*buffer++ = *target++;

			continue;
		}

		if (LIKELY(state->result == INFLT_SRCEXHSTD)) {
			intxx r;

			r = state->iofn(state->sbgn, ZIOBFFRSZ, state->payload);
			if (LIKELY(r)) {
				if (UNLIKELY((uintxx) r > ZIOBFFRSZ)) {
					SETERROR(ZSTRM_EIOERROR);
					SETSTATE(4);
					return 0;
				}

				state->source = state->sbgn;
				state->send   = state->send + r;
				inflator_setsrc(state->infltr, state->sbgn, r);
			}
			else {
				SETERROR(ZSTRM_EBADDATA);
				SETSTATE(4);
				return 0;
			}
		}
		else {
			if (UNLIKELY(state->result == INFLT_OK)) {
				/* end of the stream */
				state->source += inflator_srcend(state->infltr);

				if (state->docrc32)
					CRC32_FINALIZE(state->crc32);

				switch (state->stype) {
					case ZSTRM_GZIP:
						checkgziptail(state);
						break;
					case ZSTRM_ZLIB:
						checkzlibtail(state);
						break;
				}
				SETSTATE(4);
				if (state->error) {
					return 0;
				}
				break;
			}
		}

		inflator_settgt(state->infltr, state->tbgn, ZIOBFFRSZ);
		state->result = inflator_inflate(state->infltr, 0);
		if (UNLIKELY(state->result == INFLT_ERROR)) {
			SETERROR(ZSTRM_EDEFLATE);
			SETSTATE(4);
			return 0;
		}

		n = inflator_tgtend(state->infltr);
		target = state->tbgn;
		tend   = state->tbgn + n;

		/* update the checksums */
		if (LIKELY(state->docrc32))
			state->crc32 =   crc32_update(state->crc32, target, n);
		if (LIKELY(state->doadler)) {
			state->adler = adler32_update(state->adler, target, n);
		}
		state->total += n;
	}

	state->target = target;
	state->tend   = tend;
	return (uintxx) (buffer - bbegin);
}

uintxx
zstrm_r(TZStrm* state, uint8* buffer, uintxx size)
{
	ASSERT(state);

	/* check the stream mode */
	if (UNLIKELY(state->infltr == NULL)) {
		SETSTATE(4);
		if (state->error == 0) {
			SETERROR(ZSTRM_EINCORRECTUSE);
		}
		return 0;
	}
	if (LIKELY(state->state == 3)) {
		return inflate(state, buffer, size);
	}

	if (state->state == 1) {
		if (UNLIKELY(state->iofn == NULL)) {
			SETSTATE(4);
			SETERROR(ZSTRM_EIOERROR);
			return 0;
		}

		if (parsehead(state) == 0) {
			SETSTATE(4);
		}
		else {
			if (state->state == 2) {
				SETERROR(ZSTRM_EMISSINGDICT);
			}
		}

		if (state->error) {
			SETSTATE(4);
			return 0;
		}
		SETSTATE(3);
		return inflate(state, buffer, size);
	}

	if (state->state == 2) {
		SETERROR(ZSTRM_EMISSINGDICT);
		SETSTATE(4);
	}
	return 0;
}


CTB_INLINE void
emittarget(struct TZStrm* state, uintxx count)
{
	intxx r;

	if (UNLIKELY(count == 0)) {
		return;
	}
	r = state->iofn(state->tbgn, count, state->payload);
	if (LIKELY(r)) {
		if ((uintxx) r > count) {
			SETERROR(ZSTRM_EIOERROR);
			return;
		}
		state->target = state->tbgn;
	}
}

static void
emitbyte(struct TZStrm* state, uint8 value)
{
	if (LIKELY(state->error == 0)) {
		if (LIKELY(state->target < state->tend)) {
			*state->target++ = value;
		}
		else {
			emittarget(state, (uintxx) (state->target - state->tbgn));
			if (UNLIKELY(state->error)) {
				return;
			}
			*state->target++ = value;
		}
	}
}

CTB_INLINE void
emitgziphead(struct TZStrm* state)
{
	/* file ID */
	emitbyte(state, 0x1f);
	emitbyte(state, 0x8b);

	/* compression method */
	emitbyte(state, 0x08);

	emitbyte(state, 0x00);
	emitbyte(state, 0x00);
	emitbyte(state, 0x00);
	emitbyte(state, 0x00);
	emitbyte(state, 0x00);
	emitbyte(state, 0x00);
	emitbyte(state, 0x00);

	emittarget(state, (uintxx) (state->target - state->tbgn));
}

CTB_INLINE void
emitzlibhead(struct TZStrm* state)
{
	uintxx a;
	uintxx b;

	/* compression method + log(window size) - 8 */
	a = 0x78;
	b = 0;
	if (state->dict) {
		b |= 1 << 5;
	}

	/* fcheck */
	b = b + (31 - ((a << 8) | b % 31));

	emitbyte(state, (uint8) a);
	emitbyte(state, (uint8) b);
	if (state->dict) {
		uint32 n;

		n = state->dictid;
		emitbyte(state, (uint8) (n >> 0x18));
		emitbyte(state, (uint8) (n >> 0x10));
		emitbyte(state, (uint8) (n >> 0x08));
		emitbyte(state, (uint8) (n >> 0x00));
	}
	emittarget(state, (uintxx) (state->target - state->tbgn));
}


static uintxx
deflate(TZStrm* state, uint8* buffer, uintxx size)
{
	uintxx maxrun;
	uintxx r;
	uint8* bbegin;
	uint8* source;
	uint8* send;
	uint8* sbgn;

	source = state->source;
	send   = state->send;
	sbgn   = state->sbgn;
	bbegin = buffer;
	while (LIKELY(size)) {
		maxrun = (uintxx) (send - source);
		if (LIKELY(maxrun)) {
			if (maxrun > size)
				maxrun = size;

			for (size -= maxrun; maxrun >= 16; maxrun -= 16) {
				memcpy(source, buffer, 16);
				source += 16;
				buffer += 16;
			};
			for (;maxrun; maxrun--)
				*source++ = *buffer++;

			continue;
		}

		deflator_setsrc(state->defltr, sbgn, ZIOBFFRSZ);

		/* update the checksums */
		if (LIKELY(state->docrc32))
			state->crc32 =   crc32_update(state->crc32, sbgn, ZIOBFFRSZ);
		if (LIKELY(state->doadler)) {
			state->adler = adler32_update(state->adler, sbgn, ZIOBFFRSZ);
		}
		state->total += ZIOBFFRSZ;

		do {
			deflator_settgt(state->defltr, state->tbgn, ZIOBFFRSZ);
			r = deflator_deflate(state->defltr, 0);

			emittarget(state, deflator_tgtend(state->defltr));
			if (UNLIKELY(state->error)) {
				SETSTATE(4);
				return 0;
			}
		} while (r == DEFLT_TGTEXHSTD);

		source = state->sbgn;
	}

	state->source = source;
	return (uintxx) (buffer - bbegin);
}

uintxx
zstrm_w(TZStrm* state, uint8* buffer, uintxx size)
{
	ASSERT(state);

	/* check the stream mode */
	if (UNLIKELY(state->defltr == NULL)) {
		SETSTATE(4);
		if (state->error == 0) {
			SETERROR(ZSTRM_EINCORRECTUSE);
		}
		return 0;
	}

	if (LIKELY(state->state == 3)) {
		return deflate(state, buffer, size);
	}
	if (LIKELY(state->state == 1 || state->state == 2)) {
		if (UNLIKELY(state->iofn == NULL)) {
			SETERROR(ZSTRM_EIOERROR);
			SETSTATE(4);
			return 0;
		}

		switch (state->stype) {
			case ZSTRM_GZIP: emitgziphead(state); break;
			case ZSTRM_ZLIB: emitzlibhead(state); break;
		}
		if (state->error) {
			SETSTATE(4);
			return 0;
		}
		SETSTATE(3);

		return deflate(state, buffer, size);
	}
	return 0;
}

static void
flush(TZStrm* state, uintxx flush)
{
	uintxx total;
	uintxx r;

	total = (uintxx) (state->source - state->sbgn);
	if (total) {
		deflator_setsrc(state->defltr, state->sbgn, total);

		/* update the checksums */
		if (LIKELY(state->docrc32))
			state->crc32 =   crc32_update(state->crc32, state->sbgn, total);
		if (LIKELY(state->doadler)) {
			state->adler = adler32_update(state->adler, state->sbgn, total);
		}
		state->total += total;
	}

	do {
		deflator_settgt(state->defltr, state->tbgn, ZIOBFFRSZ);
		r = deflator_deflate(state->defltr, flush);

		emittarget(state, deflator_tgtend(state->defltr));
		if (UNLIKELY(state->error)) {
			SETSTATE(4);
			return;
		}
	} while (r == DEFLT_TGTEXHSTD);
}

CTB_INLINE void
emitgziptail(struct TZStrm* state)
{
	uint32 n;

	CRC32_FINALIZE(state->crc32);
	n = state->crc32;
	emitbyte(state, (uint8) (n >> 0x00));
	emitbyte(state, (uint8) (n >> 0x08));
	emitbyte(state, (uint8) (n >> 0x10));
	emitbyte(state, (uint8) (n >> 0x18));

	n = (uint32) state->total;
	emitbyte(state, (uint8) (n >> 0x00));
	emitbyte(state, (uint8) (n >> 0x08));
	emitbyte(state, (uint8) (n >> 0x10));
	emitbyte(state, (uint8) (n >> 0x18));
	emittarget(state, (uintxx) (state->target - state->tbgn));
}

CTB_INLINE void
emitzlibtail(struct TZStrm* state)
{
	uint32 n;

	n = state->adler;
	emitbyte(state, (uint8) (n >> 0x18));
	emitbyte(state, (uint8) (n >> 0x10));
	emitbyte(state, (uint8) (n >> 0x08));
	emitbyte(state, (uint8) (n >> 0x00));
	emittarget(state, (uintxx) (state->target - state->tbgn));
}

void
zstrm_flush(TZStrm* state, bool final)
{
	ASSERT(state);

	if (UNLIKELY(state->defltr == NULL || state->state ^ 3)) {
		if (state->infltr) {
			SETERROR(ZSTRM_EINCORRECTUSE);
			SETSTATE(4);
		}
		return;
	}

	if (final) {
		flush(state, DEFLT_END);
		if (state->error) {
			return;
		}

		switch (state->stype) {
			case ZSTRM_GZIP: emitgziptail(state); break;
			case ZSTRM_ZLIB: emitzlibtail(state); break;
		}
		SETSTATE(4);
	}
	else {
		flush(state, DEFLT_FLUSH);
		if (state->error) {
			return;
		}
	}
}

