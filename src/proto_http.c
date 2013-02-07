/*
 * HTTP protocol analyzer
 *
 * Copyright 2000-2011 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <common/appsession.h>
#include <common/base64.h>
#include <common/chunk.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/memory.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/uri_auth.h>
#include <common/version.h>

#include <types/capture.h>
#include <types/global.h>

#include <proto/acl.h>
#include <proto/arg.h>
#include <proto/auth.h>
#include <proto/backend.h>
#include <proto/channel.h>
#include <proto/checks.h>
#include <proto/compression.h>
#include <proto/dumpstats.h>
#include <proto/fd.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/hdr_idx.h>
#include <proto/proto_tcp.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/queue.h>
#include <proto/sample.h>
#include <proto/server.h>
#include <proto/session.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

const char HTTP_100[] =
	"HTTP/1.1 100 Continue\r\n\r\n";

const struct chunk http_100_chunk = {
	.str = (char *)&HTTP_100,
	.len = sizeof(HTTP_100)-1
};

/* Warning: no "connection" header is provided with the 3xx messages below */
const char *HTTP_301 =
	"HTTP/1.1 301 Moved Permanently\r\n"
	"Cache-Control: no-cache\r\n"
	"Content-length: 0\r\n"
	"Location: "; /* not terminated since it will be concatenated with the URL */

const char *HTTP_302 =
	"HTTP/1.1 302 Found\r\n"
	"Cache-Control: no-cache\r\n"
	"Content-length: 0\r\n"
	"Location: "; /* not terminated since it will be concatenated with the URL */

/* same as 302 except that the browser MUST retry with the GET method */
const char *HTTP_303 =
	"HTTP/1.1 303 See Other\r\n"
	"Cache-Control: no-cache\r\n"
	"Content-length: 0\r\n"
	"Location: "; /* not terminated since it will be concatenated with the URL */

/* Warning: this one is an sprintf() fmt string, with <realm> as its only argument */
const char *HTTP_401_fmt =
	"HTTP/1.0 401 Unauthorized\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"WWW-Authenticate: Basic realm=\"%s\"\r\n"
	"\r\n"
	"<html><body><h1>401 Unauthorized</h1>\nYou need a valid user and password to access this content.\n</body></html>\n";

const char *HTTP_407_fmt =
	"HTTP/1.0 407 Unauthorized\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"Proxy-Authenticate: Basic realm=\"%s\"\r\n"
	"\r\n"
	"<html><body><h1>401 Unauthorized</h1>\nYou need a valid user and password to access this content.\n</body></html>\n";


const int http_err_codes[HTTP_ERR_SIZE] = {
	[HTTP_ERR_200] = 200,  /* used by "monitor-uri" */
	[HTTP_ERR_400] = 400,
	[HTTP_ERR_403] = 403,
	[HTTP_ERR_408] = 408,
	[HTTP_ERR_500] = 500,
	[HTTP_ERR_502] = 502,
	[HTTP_ERR_503] = 503,
	[HTTP_ERR_504] = 504,
};

static const char *http_err_msgs[HTTP_ERR_SIZE] = {
	[HTTP_ERR_200] =
	"HTTP/1.0 200 OK\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>200 OK</h1>\nService ready.\n</body></html>\n",

	[HTTP_ERR_400] =
	"HTTP/1.0 400 Bad request\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>400 Bad request</h1>\nYour browser sent an invalid request.\n</body></html>\n",

	[HTTP_ERR_403] =
	"HTTP/1.0 403 Forbidden\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>403 Forbidden</h1>\nRequest forbidden by administrative rules.\n</body></html>\n",

	[HTTP_ERR_408] =
	"HTTP/1.0 408 Request Time-out\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>408 Request Time-out</h1>\nYour browser didn't send a complete request in time.\n</body></html>\n",

	[HTTP_ERR_500] =
	"HTTP/1.0 500 Server Error\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>500 Server Error</h1>\nAn internal server error occured.\n</body></html>\n",

	[HTTP_ERR_502] =
	"HTTP/1.0 502 Bad Gateway\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>502 Bad Gateway</h1>\nThe server returned an invalid or incomplete response.\n</body></html>\n",

	[HTTP_ERR_503] =
	"HTTP/1.0 503 Service Unavailable\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>503 Service Unavailable</h1>\nNo server is available to handle this request.\n</body></html>\n",

	[HTTP_ERR_504] =
	"HTTP/1.0 504 Gateway Time-out\r\n"
	"Cache-Control: no-cache\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n"
	"\r\n"
	"<html><body><h1>504 Gateway Time-out</h1>\nThe server didn't respond in time.\n</body></html>\n",

};

/* status codes available for the stats admin page (strictly 4 chars length) */
const char *stat_status_codes[STAT_STATUS_SIZE] = {
	[STAT_STATUS_DENY] = "DENY",
	[STAT_STATUS_DONE] = "DONE",
	[STAT_STATUS_ERRP] = "ERRP",
	[STAT_STATUS_EXCD] = "EXCD",
	[STAT_STATUS_NONE] = "NONE",
	[STAT_STATUS_PART] = "PART",
	[STAT_STATUS_UNKN] = "UNKN",
};


/* We must put the messages here since GCC cannot initialize consts depending
 * on strlen().
 */
struct chunk http_err_chunks[HTTP_ERR_SIZE];

#define FD_SETS_ARE_BITFIELDS
#ifdef FD_SETS_ARE_BITFIELDS
/*
 * This map is used with all the FD_* macros to check whether a particular bit
 * is set or not. Each bit represents an ACSII code. FD_SET() sets those bytes
 * which should be encoded. When FD_ISSET() returns non-zero, it means that the
 * byte should be encoded. Be careful to always pass bytes from 0 to 255
 * exclusively to the macros.
 */
fd_set hdr_encode_map[(sizeof(fd_set) > (256/8)) ? 1 : ((256/8) / sizeof(fd_set))];
fd_set url_encode_map[(sizeof(fd_set) > (256/8)) ? 1 : ((256/8) / sizeof(fd_set))];

#else
#error "Check if your OS uses bitfields for fd_sets"
#endif

void init_proto_http()
{
	int i;
	char *tmp;
	int msg;

	for (msg = 0; msg < HTTP_ERR_SIZE; msg++) {
		if (!http_err_msgs[msg]) {
			Alert("Internal error: no message defined for HTTP return code %d. Aborting.\n", msg);
			abort();
		}

		http_err_chunks[msg].str = (char *)http_err_msgs[msg];
		http_err_chunks[msg].len = strlen(http_err_msgs[msg]);
	}

	/* initialize the log header encoding map : '{|}"#' should be encoded with
	 * '#' as prefix, as well as non-printable characters ( <32 or >= 127 ).
	 * URL encoding only requires '"', '#' to be encoded as well as non-
	 * printable characters above.
	 */
	memset(hdr_encode_map, 0, sizeof(hdr_encode_map));
	memset(url_encode_map, 0, sizeof(url_encode_map));
	for (i = 0; i < 32; i++) {
		FD_SET(i, hdr_encode_map);
		FD_SET(i, url_encode_map);
	}
	for (i = 127; i < 256; i++) {
		FD_SET(i, hdr_encode_map);
		FD_SET(i, url_encode_map);
	}

	tmp = "\"#{|}";
	while (*tmp) {
		FD_SET(*tmp, hdr_encode_map);
		tmp++;
	}

	tmp = "\"#";
	while (*tmp) {
		FD_SET(*tmp, url_encode_map);
		tmp++;
	}

	/* memory allocations */
	pool2_requri = create_pool("requri", REQURI_LEN, MEM_F_SHARED);
	pool2_uniqueid = create_pool("uniqueid", UNIQUEID_LEN, MEM_F_SHARED);
}

/*
 * We have 26 list of methods (1 per first letter), each of which can have
 * up to 3 entries (2 valid, 1 null).
 */
struct http_method_desc {
	http_meth_t meth;
	int len;
	const char text[8];
};

const struct http_method_desc http_methods[26][3] = {
	['C' - 'A'] = {
		[0] = {	.meth = HTTP_METH_CONNECT , .len=7, .text="CONNECT" },
	},
	['D' - 'A'] = {
		[0] = {	.meth = HTTP_METH_DELETE  , .len=6, .text="DELETE"  },
	},
	['G' - 'A'] = {
		[0] = {	.meth = HTTP_METH_GET     , .len=3, .text="GET"     },
	},
	['H' - 'A'] = {
		[0] = {	.meth = HTTP_METH_HEAD    , .len=4, .text="HEAD"    },
	},
	['P' - 'A'] = {
		[0] = {	.meth = HTTP_METH_POST    , .len=4, .text="POST"    },
		[1] = {	.meth = HTTP_METH_PUT     , .len=3, .text="PUT"     },
	},
	['T' - 'A'] = {
		[0] = {	.meth = HTTP_METH_TRACE   , .len=5, .text="TRACE"   },
	},
	/* rest is empty like this :
	 *      [1] = {	.meth = HTTP_METH_NONE    , .len=0, .text=""        },
	 */
};

/* It is about twice as fast on recent architectures to lookup a byte in a
 * table than to perform a boolean AND or OR between two tests. Refer to
 * RFC2616 for those chars.
 */

const char http_is_spht[256] = {
	[' '] = 1, ['\t'] = 1,
};

const char http_is_crlf[256] = {
	['\r'] = 1, ['\n'] = 1,
};

const char http_is_lws[256] = {
	[' '] = 1, ['\t'] = 1,
	['\r'] = 1, ['\n'] = 1,
};

const char http_is_sep[256] = {
	['('] = 1, [')']  = 1, ['<']  = 1, ['>'] = 1,
	['@'] = 1, [',']  = 1, [';']  = 1, [':'] = 1,
	['"'] = 1, ['/']  = 1, ['[']  = 1, [']'] = 1,
	['{'] = 1, ['}']  = 1, ['?']  = 1, ['='] = 1,
	[' '] = 1, ['\t'] = 1, ['\\'] = 1,
};

const char http_is_ctl[256] = {
	[0 ... 31] = 1,
	[127] = 1,
};

/*
 * A token is any ASCII char that is neither a separator nor a CTL char.
 * Do not overwrite values in assignment since gcc-2.95 will not handle
 * them correctly. Instead, define every non-CTL char's status.
 */
const char http_is_token[256] = {
	[' '] = 0, ['!'] = 1, ['"'] = 0, ['#'] = 1,
	['$'] = 1, ['%'] = 1, ['&'] = 1, ['\''] = 1,
	['('] = 0, [')'] = 0, ['*'] = 1, ['+'] = 1,
	[','] = 0, ['-'] = 1, ['.'] = 1, ['/'] = 0,
	['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1,
	['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1,
	['8'] = 1, ['9'] = 1, [':'] = 0, [';'] = 0,
	['<'] = 0, ['='] = 0, ['>'] = 0, ['?'] = 0,
	['@'] = 0, ['A'] = 1, ['B'] = 1, ['C'] = 1,
	['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1,
	['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1,
	['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1,
	['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1,
	['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1,
	['X'] = 1, ['Y'] = 1, ['Z'] = 1, ['['] = 0,
	['\\'] = 0, [']'] = 0, ['^'] = 1, ['_'] = 1,
	['`'] = 1, ['a'] = 1, ['b'] = 1, ['c'] = 1,
	['d'] = 1, ['e'] = 1, ['f'] = 1, ['g'] = 1,
	['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1,
	['l'] = 1, ['m'] = 1, ['n'] = 1, ['o'] = 1,
	['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1,
	['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1,
	['x'] = 1, ['y'] = 1, ['z'] = 1, ['{'] = 0,
	['|'] = 1, ['}'] = 0, ['~'] = 1, 
};


/*
 * An http ver_token is any ASCII which can be found in an HTTP version,
 * which includes 'H', 'T', 'P', '/', '.' and any digit.
 */
const char http_is_ver_token[256] = {
	['.'] = 1, ['/'] = 1,
	['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
	['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
	['H'] = 1, ['P'] = 1, ['T'] = 1,
};


/*
 * Silent debug that outputs only in strace, using fd #-1. Trash is modified.
 */
#if defined(DEBUG_FSM)
static void http_silent_debug(int line, struct session *s)
{
	chunk_printf(&trash,
	             "[%04d] req: p=%d(%d) s=%d bf=%08x an=%08x data=%p size=%d l=%d w=%p r=%p o=%p sm=%d fw=%ld tf=%08x\n",
	             line,
	             s->si[0].state, s->si[0].fd, s->txn.req.msg_state, s->req->flags, s->req->analysers,
	             s->req->buf->data, s->req->buf->size, s->req->l, s->req->w, s->req->r, s->req->buf->p, s->req->buf->o, s->req->to_forward, s->txn.flags);
	write(-1, trash.str, trash.len);

	chunk_printf(&trash,
	             " %04d  rep: p=%d(%d) s=%d bf=%08x an=%08x data=%p size=%d l=%d w=%p r=%p o=%p sm=%d fw=%ld\n",
                     line,
	             s->si[1].state, s->si[1].fd, s->txn.rsp.msg_state, s->rep->flags, s->rep->analysers,
	             s->rep->buf->data, s->rep->buf->size, s->rep->l, s->rep->w, s->rep->r, s->rep->buf->p, s->rep->buf->o, s->rep->to_forward);
	write(-1, trash.str, trash.len);
}
#else
#define http_silent_debug(l,s)  do { } while (0)
#endif

/*
 * Adds a header and its CRLF at the tail of the message's buffer, just before
 * the last CRLF. Text length is measured first, so it cannot be NULL.
 * The header is also automatically added to the index <hdr_idx>, and the end
 * of headers is automatically adjusted. The number of bytes added is returned
 * on success, otherwise <0 is returned indicating an error.
 */
int http_header_add_tail(struct http_msg *msg, struct hdr_idx *hdr_idx, const char *text)
{
	int bytes, len;

	len = strlen(text);
	bytes = buffer_insert_line2(msg->chn->buf, msg->chn->buf->p + msg->eoh, text, len);
	if (!bytes)
		return -1;
	http_msg_move_end(msg, bytes);
	return hdr_idx_add(len, 1, hdr_idx, hdr_idx->tail);
}

/*
 * Adds a header and its CRLF at the tail of the message's buffer, just before
 * the last CRLF. <len> bytes are copied, not counting the CRLF. If <text> is NULL, then
 * the buffer is only opened and the space reserved, but nothing is copied.
 * The header is also automatically added to the index <hdr_idx>, and the end
 * of headers is automatically adjusted. The number of bytes added is returned
 * on success, otherwise <0 is returned indicating an error.
 */
int http_header_add_tail2(struct http_msg *msg,
                          struct hdr_idx *hdr_idx, const char *text, int len)
{
	int bytes;

	bytes = buffer_insert_line2(msg->chn->buf, msg->chn->buf->p + msg->eoh, text, len);
	if (!bytes)
		return -1;
	http_msg_move_end(msg, bytes);
	return hdr_idx_add(len, 1, hdr_idx, hdr_idx->tail);
}

/*
 * Checks if <hdr> is exactly <name> for <len> chars, and ends with a colon.
 * If so, returns the position of the first non-space character relative to
 * <hdr>, or <end>-<hdr> if not found before. If no value is found, it tries
 * to return a pointer to the place after the first space. Returns 0 if the
 * header name does not match. Checks are case-insensitive.
 */
int http_header_match2(const char *hdr, const char *end,
		       const char *name, int len)
{
	const char *val;

	if (hdr + len >= end)
		return 0;
	if (hdr[len] != ':')
		return 0;
	if (strncasecmp(hdr, name, len) != 0)
		return 0;
	val = hdr + len + 1;
	while (val < end && HTTP_IS_SPHT(*val))
		val++;
	if ((val >= end) && (len + 2 <= end - hdr))
		return len + 2; /* we may replace starting from second space */
	return val - hdr;
}

/* Find the end of the header value contained between <s> and <e>. See RFC2616,
 * par 2.2 for more information. Note that it requires a valid header to return
 * a valid result. This works for headers defined as comma-separated lists.
 */
char *find_hdr_value_end(char *s, const char *e)
{
	int quoted, qdpair;

	quoted = qdpair = 0;
	for (; s < e; s++) {
		if (qdpair)                    qdpair = 0;
		else if (quoted) {
			if (*s == '\\')        qdpair = 1;
			else if (*s == '"')    quoted = 0;
		}
		else if (*s == '"')            quoted = 1;
		else if (*s == ',')            return s;
	}
	return s;
}

/* Find the first or next occurrence of header <name> in message buffer <sol>
 * using headers index <idx>, and return it in the <ctx> structure. This
 * structure holds everything necessary to use the header and find next
 * occurrence. If its <idx> member is 0, the header is searched from the
 * beginning. Otherwise, the next occurrence is returned. The function returns
 * 1 when it finds a value, and 0 when there is no more. It is designed to work
 * with headers defined as comma-separated lists. As a special case, if ctx->val
 * is NULL when searching for a new values of a header, the current header is
 * rescanned. This allows rescanning after a header deletion.
 */
int http_find_header2(const char *name, int len,
		      char *sol, struct hdr_idx *idx,
		      struct hdr_ctx *ctx)
{
	char *eol, *sov;
	int cur_idx, old_idx;

	cur_idx = ctx->idx;
	if (cur_idx) {
		/* We have previously returned a value, let's search
		 * another one on the same line.
		 */
		sol = ctx->line;
		ctx->del = ctx->val + ctx->vlen + ctx->tws;
		sov = sol + ctx->del;
		eol = sol + idx->v[cur_idx].len;

		if (sov >= eol)
			/* no more values in this header */
			goto next_hdr;

		/* values remaining for this header, skip the comma but save it
		 * for later use (eg: for header deletion).
		 */
		sov++;
		while (sov < eol && http_is_lws[(unsigned char)*sov])
			sov++;

		goto return_hdr;
	}

	/* first request for this header */
	sol += hdr_idx_first_pos(idx);
	old_idx = 0;
	cur_idx = hdr_idx_first_idx(idx);
	while (cur_idx) {
		eol = sol + idx->v[cur_idx].len;

		if (len == 0) {
			/* No argument was passed, we want any header.
			 * To achieve this, we simply build a fake request. */
			while (sol + len < eol && sol[len] != ':')
				len++;
			name = sol;
		}

		if ((len < eol - sol) &&
		    (sol[len] == ':') &&
		    (strncasecmp(sol, name, len) == 0)) {
			ctx->del = len;
			sov = sol + len + 1;
			while (sov < eol && http_is_lws[(unsigned char)*sov])
				sov++;

			ctx->line = sol;
			ctx->prev = old_idx;
		return_hdr:
			ctx->idx  = cur_idx;
			ctx->val  = sov - sol;

			eol = find_hdr_value_end(sov, eol);
			ctx->tws = 0;
			while (eol > sov && http_is_lws[(unsigned char)*(eol - 1)]) {
				eol--;
				ctx->tws++;
			}
			ctx->vlen = eol - sov;
			return 1;
		}
	next_hdr:
		sol = eol + idx->v[cur_idx].cr + 1;
		old_idx = cur_idx;
		cur_idx = idx->v[cur_idx].next;
	}
	return 0;
}

int http_find_header(const char *name,
		     char *sol, struct hdr_idx *idx,
		     struct hdr_ctx *ctx)
{
	return http_find_header2(name, strlen(name), sol, idx, ctx);
}

/* Remove one value of a header. This only works on a <ctx> returned by one of
 * the http_find_header functions. The value is removed, as well as surrounding
 * commas if any. If the removed value was alone, the whole header is removed.
 * The ctx is always updated accordingly, as well as the buffer and HTTP
 * message <msg>. The new index is returned. If it is zero, it means there is
 * no more header, so any processing may stop. The ctx is always left in a form
 * that can be handled by http_find_header2() to find next occurrence.
 */
int http_remove_header2(struct http_msg *msg, struct hdr_idx *idx, struct hdr_ctx *ctx)
{
	int cur_idx = ctx->idx;
	char *sol = ctx->line;
	struct hdr_idx_elem *hdr;
	int delta, skip_comma;

	if (!cur_idx)
		return 0;

	hdr = &idx->v[cur_idx];
	if (sol[ctx->del] == ':' && ctx->val + ctx->vlen + ctx->tws == hdr->len) {
		/* This was the only value of the header, we must now remove it entirely. */
		delta = buffer_replace2(msg->chn->buf, sol, sol + hdr->len + hdr->cr + 1, NULL, 0);
		http_msg_move_end(msg, delta);
		idx->used--;
		hdr->len = 0;   /* unused entry */
		idx->v[ctx->prev].next = idx->v[ctx->idx].next;
		if (idx->tail == ctx->idx)
			idx->tail = ctx->prev;
		ctx->idx = ctx->prev;    /* walk back to the end of previous header */
		ctx->line -= idx->v[ctx->idx].len + idx->v[cur_idx].cr + 1;
		ctx->val = idx->v[ctx->idx].len; /* point to end of previous header */
		ctx->tws = ctx->vlen = 0;
		return ctx->idx;
	}

	/* This was not the only value of this header. We have to remove between
	 * ctx->del+1 and ctx->val+ctx->vlen+ctx->tws+1 included. If it is the
	 * last entry of the list, we remove the last separator.
	 */

	skip_comma = (ctx->val + ctx->vlen + ctx->tws == hdr->len) ? 0 : 1;
	delta = buffer_replace2(msg->chn->buf, sol + ctx->del + skip_comma,
				sol + ctx->val + ctx->vlen + ctx->tws + skip_comma,
				NULL, 0);
	hdr->len += delta;
	http_msg_move_end(msg, delta);
	ctx->val = ctx->del;
	ctx->tws = ctx->vlen = 0;
	return ctx->idx;
}

/* This function handles a server error at the stream interface level. The
 * stream interface is assumed to be already in a closed state. An optional
 * message is copied into the input buffer, and an HTTP status code stored.
 * The error flags are set to the values in arguments. Any pending request
 * in this buffer will be lost.
 */
static void http_server_error(struct session *t, struct stream_interface *si,
			      int err, int finst, int status, const struct chunk *msg)
{
	channel_auto_read(si->ob);
	channel_abort(si->ob);
	channel_auto_close(si->ob);
	channel_erase(si->ob);
	channel_auto_close(si->ib);
	channel_auto_read(si->ib);
	if (status > 0 && msg) {
		t->txn.status = status;
		bo_inject(si->ib, msg->str, msg->len);
	}
	if (!(t->flags & SN_ERR_MASK))
		t->flags |= err;
	if (!(t->flags & SN_FINST_MASK))
		t->flags |= finst;
}

/* This function returns the appropriate error location for the given session
 * and message.
 */

struct chunk *http_error_message(struct session *s, int msgnum)
{
	if (s->be->errmsg[msgnum].str)
		return &s->be->errmsg[msgnum];
	else if (s->fe->errmsg[msgnum].str)
		return &s->fe->errmsg[msgnum];
	else
		return &http_err_chunks[msgnum];
}

/*
 * returns HTTP_METH_NONE if there is nothing valid to read (empty or non-text
 * string), HTTP_METH_OTHER for unknown methods, or the identified method.
 */
static http_meth_t find_http_meth(const char *str, const int len)
{
	unsigned char m;
	const struct http_method_desc *h;

	m = ((unsigned)*str - 'A');

	if (m < 26) {
		for (h = http_methods[m]; h->len > 0; h++) {
			if (unlikely(h->len != len))
				continue;
			if (likely(memcmp(str, h->text, h->len) == 0))
				return h->meth;
		};
		return HTTP_METH_OTHER;
	}
	return HTTP_METH_NONE;

}

/* Parse the URI from the given transaction (which is assumed to be in request
 * phase) and look for the "/" beginning the PATH. If not found, return NULL.
 * It is returned otherwise.
 */
static char *
http_get_path(struct http_txn *txn)
{
	char *ptr, *end;

	ptr = txn->req.chn->buf->p + txn->req.sl.rq.u;
	end = ptr + txn->req.sl.rq.u_l;

	if (ptr >= end)
		return NULL;

	/* RFC2616, par. 5.1.2 :
	 * Request-URI = "*" | absuri | abspath | authority
	 */

	if (*ptr == '*')
		return NULL;

	if (isalpha((unsigned char)*ptr)) {
		/* this is a scheme as described by RFC3986, par. 3.1 */
		ptr++;
		while (ptr < end &&
		       (isalnum((unsigned char)*ptr) || *ptr == '+' || *ptr == '-' || *ptr == '.'))
			ptr++;
		/* skip '://' */
		if (ptr == end || *ptr++ != ':')
			return NULL;
		if (ptr == end || *ptr++ != '/')
			return NULL;
		if (ptr == end || *ptr++ != '/')
			return NULL;
	}
	/* skip [user[:passwd]@]host[:[port]] */

	while (ptr < end && *ptr != '/')
		ptr++;

	if (ptr == end)
		return NULL;

	/* OK, we got the '/' ! */
	return ptr;
}

/* Returns a 302 for a redirectable request that reaches a server working in
 * in redirect mode. This may only be called just after the stream interface
 * has moved to SI_ST_ASS. Unprocessable requests are left unchanged and will
 * follow normal proxy processing. NOTE: this function is designed to support
 * being called once data are scheduled for forwarding.
 */
void http_perform_server_redirect(struct session *s, struct stream_interface *si)
{
	struct http_txn *txn;
	struct server *srv;
	char *path;
	int len, rewind;

	/* 1: create the response header */
	trash.len = strlen(HTTP_302);
	memcpy(trash.str, HTTP_302, trash.len);

	srv = objt_server(s->target);

	/* 2: add the server's prefix */
	if (trash.len + srv->rdr_len > trash.size)
		return;

	/* special prefix "/" means don't change URL */
	if (srv->rdr_len != 1 || *srv->rdr_pfx != '/') {
		memcpy(trash.str + trash.len, srv->rdr_pfx, srv->rdr_len);
		trash.len += srv->rdr_len;
	}

	/* 3: add the request URI. Since it was already forwarded, we need
	 * to temporarily rewind the buffer.
	 */
	txn = &s->txn;
	b_rew(s->req->buf, rewind = s->req->buf->o);

	path = http_get_path(txn);
	len = buffer_count(s->req->buf, path, b_ptr(s->req->buf, txn->req.sl.rq.u + txn->req.sl.rq.u_l));

	b_adv(s->req->buf, rewind);

	if (!path)
		return;

	if (trash.len + len > trash.size - 4) /* 4 for CRLF-CRLF */
		return;

	memcpy(trash.str + trash.len, path, len);
	trash.len += len;

	if (unlikely(txn->flags & TX_USE_PX_CONN)) {
		memcpy(trash.str + trash.len, "\r\nProxy-Connection: close\r\n\r\n", 29);
		trash.len += 29;
	} else {
		memcpy(trash.str + trash.len, "\r\nConnection: close\r\n\r\n", 23);
		trash.len += 23;
	}

	/* prepare to return without error. */
	si_shutr(si);
	si_shutw(si);
	si->err_type = SI_ET_NONE;
	si->err_loc  = NULL;
	si->state    = SI_ST_CLO;

	/* send the message */
	http_server_error(s, si, SN_ERR_PRXCOND, SN_FINST_C, 302, &trash);

	/* FIXME: we should increase a counter of redirects per server and per backend. */
	srv_inc_sess_ctr(srv);
}

/* Return the error message corresponding to si->err_type. It is assumed
 * that the server side is closed. Note that err_type is actually a
 * bitmask, where almost only aborts may be cumulated with other
 * values. We consider that aborted operations are more important
 * than timeouts or errors due to the fact that nobody else in the
 * logs might explain incomplete retries. All others should avoid
 * being cumulated. It should normally not be possible to have multiple
 * aborts at once, but just in case, the first one in sequence is reported.
 */
void http_return_srv_error(struct session *s, struct stream_interface *si)
{
	int err_type = si->err_type;

	if (err_type & SI_ET_QUEUE_ABRT)
		http_server_error(s, si, SN_ERR_CLICL, SN_FINST_Q,
				  503, http_error_message(s, HTTP_ERR_503));
	else if (err_type & SI_ET_CONN_ABRT)
		http_server_error(s, si, SN_ERR_CLICL, SN_FINST_C,
				  503, http_error_message(s, HTTP_ERR_503));
	else if (err_type & SI_ET_QUEUE_TO)
		http_server_error(s, si, SN_ERR_SRVTO, SN_FINST_Q,
				  503, http_error_message(s, HTTP_ERR_503));
	else if (err_type & SI_ET_QUEUE_ERR)
		http_server_error(s, si, SN_ERR_SRVCL, SN_FINST_Q,
				  503, http_error_message(s, HTTP_ERR_503));
	else if (err_type & SI_ET_CONN_TO)
		http_server_error(s, si, SN_ERR_SRVTO, SN_FINST_C,
				  503, http_error_message(s, HTTP_ERR_503));
	else if (err_type & SI_ET_CONN_ERR)
		http_server_error(s, si, SN_ERR_SRVCL, SN_FINST_C,
				  503, http_error_message(s, HTTP_ERR_503));
	else /* SI_ET_CONN_OTHER and others */
		http_server_error(s, si, SN_ERR_INTERNAL, SN_FINST_C,
				  500, http_error_message(s, HTTP_ERR_500));
}

extern const char sess_term_cond[8];
extern const char sess_fin_state[8];
extern const char *monthname[12];
struct pool_head *pool2_requri;
struct pool_head *pool2_capture = NULL;
struct pool_head *pool2_uniqueid;

/*
 * Capture headers from message starting at <som> according to header list
 * <cap_hdr>, and fill the <idx> structure appropriately.
 */
void capture_headers(char *som, struct hdr_idx *idx,
		     char **cap, struct cap_hdr *cap_hdr)
{
	char *eol, *sol, *col, *sov;
	int cur_idx;
	struct cap_hdr *h;
	int len;

	sol = som + hdr_idx_first_pos(idx);
	cur_idx = hdr_idx_first_idx(idx);

	while (cur_idx) {
		eol = sol + idx->v[cur_idx].len;

		col = sol;
		while (col < eol && *col != ':')
			col++;

		sov = col + 1;
		while (sov < eol && http_is_lws[(unsigned char)*sov])
			sov++;
				
		for (h = cap_hdr; h; h = h->next) {
			if ((h->namelen == col - sol) &&
			    (strncasecmp(sol, h->name, h->namelen) == 0)) {
				if (cap[h->index] == NULL)
					cap[h->index] =
						pool_alloc2(h->pool);

				if (cap[h->index] == NULL) {
					Alert("HTTP capture : out of memory.\n");
					continue;
				}
							
				len = eol - sov;
				if (len > h->len)
					len = h->len;
							
				memcpy(cap[h->index], sov, len);
				cap[h->index][len]=0;
			}
		}
		sol = eol + idx->v[cur_idx].cr + 1;
		cur_idx = idx->v[cur_idx].next;
	}
}


/* either we find an LF at <ptr> or we jump to <bad>.
 */
#define EXPECT_LF_HERE(ptr, bad)	do { if (unlikely(*(ptr) != '\n')) goto bad; } while (0)

/* plays with variables <ptr>, <end> and <state>. Jumps to <good> if OK,
 * otherwise to <http_msg_ood> with <state> set to <st>.
 */
#define EAT_AND_JUMP_OR_RETURN(good, st)   do { \
		ptr++;                          \
		if (likely(ptr < end))          \
			goto good;              \
		else {                          \
			state = (st);           \
			goto http_msg_ood;      \
		}                               \
	} while (0)


/*
 * This function parses a status line between <ptr> and <end>, starting with
 * parser state <state>. Only states HTTP_MSG_RPVER, HTTP_MSG_RPVER_SP,
 * HTTP_MSG_RPCODE, HTTP_MSG_RPCODE_SP and HTTP_MSG_RPREASON are handled. Others
 * will give undefined results.
 * Note that it is upon the caller's responsibility to ensure that ptr < end,
 * and that msg->sol points to the beginning of the response.
 * If a complete line is found (which implies that at least one CR or LF is
 * found before <end>, the updated <ptr> is returned, otherwise NULL is
 * returned indicating an incomplete line (which does not mean that parts have
 * not been updated). In the incomplete case, if <ret_ptr> or <ret_state> are
 * non-NULL, they are fed with the new <ptr> and <state> values to be passed
 * upon next call.
 *
 * This function was intentionally designed to be called from
 * http_msg_analyzer() with the lowest overhead. It should integrate perfectly
 * within its state machine and use the same macros, hence the need for same
 * labels and variable names. Note that msg->sol is left unchanged.
 */
const char *http_parse_stsline(struct http_msg *msg,
			       unsigned int state, const char *ptr, const char *end,
			       unsigned int *ret_ptr, unsigned int *ret_state)
{
	const char *msg_start = msg->chn->buf->p;

	switch (state)	{
	case HTTP_MSG_RPVER:
	http_msg_rpver:
		if (likely(HTTP_IS_VER_TOKEN(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpver, HTTP_MSG_RPVER);

		if (likely(HTTP_IS_SPHT(*ptr))) {
			msg->sl.st.v_l = ptr - msg_start;
			EAT_AND_JUMP_OR_RETURN(http_msg_rpver_sp, HTTP_MSG_RPVER_SP);
		}
		state = HTTP_MSG_ERROR;
		break;

	case HTTP_MSG_RPVER_SP:
	http_msg_rpver_sp:
		if (likely(!HTTP_IS_LWS(*ptr))) {
			msg->sl.st.c = ptr - msg_start;
			goto http_msg_rpcode;
		}
		if (likely(HTTP_IS_SPHT(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpver_sp, HTTP_MSG_RPVER_SP);
		/* so it's a CR/LF, this is invalid */
		state = HTTP_MSG_ERROR;
		break;

	case HTTP_MSG_RPCODE:
	http_msg_rpcode:
		if (likely(!HTTP_IS_LWS(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpcode, HTTP_MSG_RPCODE);

		if (likely(HTTP_IS_SPHT(*ptr))) {
			msg->sl.st.c_l = ptr - msg_start - msg->sl.st.c;
			EAT_AND_JUMP_OR_RETURN(http_msg_rpcode_sp, HTTP_MSG_RPCODE_SP);
		}

		/* so it's a CR/LF, so there is no reason phrase */
		msg->sl.st.c_l = ptr - msg_start - msg->sl.st.c;
	http_msg_rsp_reason:
		/* FIXME: should we support HTTP responses without any reason phrase ? */
		msg->sl.st.r = ptr - msg_start;
		msg->sl.st.r_l = 0;
		goto http_msg_rpline_eol;

	case HTTP_MSG_RPCODE_SP:
	http_msg_rpcode_sp:
		if (likely(!HTTP_IS_LWS(*ptr))) {
			msg->sl.st.r = ptr - msg_start;
			goto http_msg_rpreason;
		}
		if (likely(HTTP_IS_SPHT(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpcode_sp, HTTP_MSG_RPCODE_SP);
		/* so it's a CR/LF, so there is no reason phrase */
		goto http_msg_rsp_reason;

	case HTTP_MSG_RPREASON:
	http_msg_rpreason:
		if (likely(!HTTP_IS_CRLF(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpreason, HTTP_MSG_RPREASON);
		msg->sl.st.r_l = ptr - msg_start - msg->sl.st.r;
	http_msg_rpline_eol:
		/* We have seen the end of line. Note that we do not
		 * necessarily have the \n yet, but at least we know that we
		 * have EITHER \r OR \n, otherwise the response would not be
		 * complete. We can then record the response length and return
		 * to the caller which will be able to register it.
		 */
		msg->sl.st.l = ptr - msg_start - msg->sol;
		return ptr;

#ifdef DEBUG_FULL
	default:
		fprintf(stderr, "FIXME !!!! impossible state at %s:%d = %d\n", __FILE__, __LINE__, state);
		exit(1);
#endif
	}

 http_msg_ood:
	/* out of valid data */
	if (ret_state)
		*ret_state = state;
	if (ret_ptr)
		*ret_ptr = ptr - msg_start;
	return NULL;
}

/*
 * This function parses a request line between <ptr> and <end>, starting with
 * parser state <state>. Only states HTTP_MSG_RQMETH, HTTP_MSG_RQMETH_SP,
 * HTTP_MSG_RQURI, HTTP_MSG_RQURI_SP and HTTP_MSG_RQVER are handled. Others
 * will give undefined results.
 * Note that it is upon the caller's responsibility to ensure that ptr < end,
 * and that msg->sol points to the beginning of the request.
 * If a complete line is found (which implies that at least one CR or LF is
 * found before <end>, the updated <ptr> is returned, otherwise NULL is
 * returned indicating an incomplete line (which does not mean that parts have
 * not been updated). In the incomplete case, if <ret_ptr> or <ret_state> are
 * non-NULL, they are fed with the new <ptr> and <state> values to be passed
 * upon next call.
 *
 * This function was intentionally designed to be called from
 * http_msg_analyzer() with the lowest overhead. It should integrate perfectly
 * within its state machine and use the same macros, hence the need for same
 * labels and variable names. Note that msg->sol is left unchanged.
 */
const char *http_parse_reqline(struct http_msg *msg,
			       unsigned int state, const char *ptr, const char *end,
			       unsigned int *ret_ptr, unsigned int *ret_state)
{
	const char *msg_start = msg->chn->buf->p;

	switch (state)	{
	case HTTP_MSG_RQMETH:
	http_msg_rqmeth:
		if (likely(HTTP_IS_TOKEN(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rqmeth, HTTP_MSG_RQMETH);

		if (likely(HTTP_IS_SPHT(*ptr))) {
			msg->sl.rq.m_l = ptr - msg_start;
			EAT_AND_JUMP_OR_RETURN(http_msg_rqmeth_sp, HTTP_MSG_RQMETH_SP);
		}

		if (likely(HTTP_IS_CRLF(*ptr))) {
			/* HTTP 0.9 request */
			msg->sl.rq.m_l = ptr - msg_start;
		http_msg_req09_uri:
			msg->sl.rq.u = ptr - msg_start;
		http_msg_req09_uri_e:
			msg->sl.rq.u_l = ptr - msg_start - msg->sl.rq.u;
		http_msg_req09_ver:
			msg->sl.rq.v = ptr - msg_start;
			msg->sl.rq.v_l = 0;
			goto http_msg_rqline_eol;
		}
		state = HTTP_MSG_ERROR;
		break;

	case HTTP_MSG_RQMETH_SP:
	http_msg_rqmeth_sp:
		if (likely(!HTTP_IS_LWS(*ptr))) {
			msg->sl.rq.u = ptr - msg_start;
			goto http_msg_rquri;
		}
		if (likely(HTTP_IS_SPHT(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rqmeth_sp, HTTP_MSG_RQMETH_SP);
		/* so it's a CR/LF, meaning an HTTP 0.9 request */
		goto http_msg_req09_uri;

	case HTTP_MSG_RQURI:
	http_msg_rquri:
		if (likely((unsigned char)(*ptr - 33) <= 93)) /* 33 to 126 included */
			EAT_AND_JUMP_OR_RETURN(http_msg_rquri, HTTP_MSG_RQURI);

		if (likely(HTTP_IS_SPHT(*ptr))) {
			msg->sl.rq.u_l = ptr - msg_start - msg->sl.rq.u;
			EAT_AND_JUMP_OR_RETURN(http_msg_rquri_sp, HTTP_MSG_RQURI_SP);
		}

		if (likely((unsigned char)*ptr >= 128)) {
			/* non-ASCII chars are forbidden unless option
			 * accept-invalid-http-request is enabled in the frontend.
			 * In any case, we capture the faulty char.
			 */
			if (msg->err_pos < -1)
				goto invalid_char;
			if (msg->err_pos == -1)
				msg->err_pos = ptr - msg_start;
			EAT_AND_JUMP_OR_RETURN(http_msg_rquri, HTTP_MSG_RQURI);
		}

		if (likely(HTTP_IS_CRLF(*ptr))) {
			/* so it's a CR/LF, meaning an HTTP 0.9 request */
			goto http_msg_req09_uri_e;
		}

		/* OK forbidden chars, 0..31 or 127 */
	invalid_char:
		msg->err_pos = ptr - msg_start;
		state = HTTP_MSG_ERROR;
		break;

	case HTTP_MSG_RQURI_SP:
	http_msg_rquri_sp:
		if (likely(!HTTP_IS_LWS(*ptr))) {
			msg->sl.rq.v = ptr - msg_start;
			goto http_msg_rqver;
		}
		if (likely(HTTP_IS_SPHT(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rquri_sp, HTTP_MSG_RQURI_SP);
		/* so it's a CR/LF, meaning an HTTP 0.9 request */
		goto http_msg_req09_ver;

	case HTTP_MSG_RQVER:
	http_msg_rqver:
		if (likely(HTTP_IS_VER_TOKEN(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_rqver, HTTP_MSG_RQVER);

		if (likely(HTTP_IS_CRLF(*ptr))) {
			msg->sl.rq.v_l = ptr - msg_start - msg->sl.rq.v;
		http_msg_rqline_eol:
			/* We have seen the end of line. Note that we do not
			 * necessarily have the \n yet, but at least we know that we
			 * have EITHER \r OR \n, otherwise the request would not be
			 * complete. We can then record the request length and return
			 * to the caller which will be able to register it.
			 */
			msg->sl.rq.l = ptr - msg_start - msg->sol;
			return ptr;
		}

		/* neither an HTTP_VER token nor a CRLF */
		state = HTTP_MSG_ERROR;
		break;

#ifdef DEBUG_FULL
	default:
		fprintf(stderr, "FIXME !!!! impossible state at %s:%d = %d\n", __FILE__, __LINE__, state);
		exit(1);
#endif
	}

 http_msg_ood:
	/* out of valid data */
	if (ret_state)
		*ret_state = state;
	if (ret_ptr)
		*ret_ptr = ptr - msg_start;
	return NULL;
}

/*
 * Returns the data from Authorization header. Function may be called more
 * than once so data is stored in txn->auth_data. When no header is found
 * or auth method is unknown auth_method is set to HTTP_AUTH_WRONG to avoid
 * searching again for something we are unable to find anyway.
 */

char *get_http_auth_buff;

int
get_http_auth(struct session *s)
{

	struct http_txn *txn = &s->txn;
	struct chunk auth_method;
	struct hdr_ctx ctx;
	char *h, *p;
	int len;

#ifdef DEBUG_AUTH
	printf("Auth for session %p: %d\n", s, txn->auth.method);
#endif

	if (txn->auth.method == HTTP_AUTH_WRONG)
		return 0;

	if (txn->auth.method)
		return 1;

	txn->auth.method = HTTP_AUTH_WRONG;

	ctx.idx = 0;

	if (txn->flags & TX_USE_PX_CONN) {
		h = "Proxy-Authorization";
		len = strlen(h);
	} else {
		h = "Authorization";
		len = strlen(h);
	}

	if (!http_find_header2(h, len, s->req->buf->p, &txn->hdr_idx, &ctx))
		return 0;

	h = ctx.line + ctx.val;

	p = memchr(h, ' ', ctx.vlen);
	if (!p || p == h)
		return 0;

	chunk_initlen(&auth_method, h, 0, p-h);
	chunk_initlen(&txn->auth.method_data, p+1, 0, ctx.vlen-(p-h)-1);

	if (!strncasecmp("Basic", auth_method.str, auth_method.len)) {

		len = base64dec(txn->auth.method_data.str, txn->auth.method_data.len,
				get_http_auth_buff, global.tune.bufsize - 1);

		if (len < 0)
			return 0;


		get_http_auth_buff[len] = '\0';

		p = strchr(get_http_auth_buff, ':');

		if (!p)
			return 0;

		txn->auth.user = get_http_auth_buff;
		*p = '\0';
		txn->auth.pass = p+1;

		txn->auth.method = HTTP_AUTH_BASIC;
		return 1;
	}

	return 0;
}


/*
 * This function parses an HTTP message, either a request or a response,
 * depending on the initial msg->msg_state. The caller is responsible for
 * ensuring that the message does not wrap. The function can be preempted
 * everywhere when data are missing and recalled at the exact same location
 * with no information loss. The message may even be realigned between two
 * calls. The header index is re-initialized when switching from
 * MSG_R[PQ]BEFORE to MSG_RPVER|MSG_RQMETH. It modifies msg->sol among other
 * fields. Note that msg->sol will be initialized after completing the first
 * state, so that none of the msg pointers has to be initialized prior to the
 * first call.
 */
void http_msg_analyzer(struct http_msg *msg, struct hdr_idx *idx)
{
	unsigned int state;       /* updated only when leaving the FSM */
	register char *ptr, *end; /* request pointers, to avoid dereferences */
	struct buffer *buf;

	state = msg->msg_state;
	buf = msg->chn->buf;
	ptr = buf->p + msg->next;
	end = buf->p + buf->i;

	if (unlikely(ptr >= end))
		goto http_msg_ood;

	switch (state)	{
	/*
	 * First, states that are specific to the response only.
	 * We check them first so that request and headers are
	 * closer to each other (accessed more often).
	 */
	case HTTP_MSG_RPBEFORE:
	http_msg_rpbefore:
		if (likely(HTTP_IS_TOKEN(*ptr))) {
			/* we have a start of message, but we have to check
			 * first if we need to remove some CRLF. We can only
			 * do this when o=0.
			 */
			if (unlikely(ptr != buf->p)) {
				if (buf->o)
					goto http_msg_ood;
				/* Remove empty leading lines, as recommended by RFC2616. */
				bi_fast_delete(buf, ptr - buf->p);
			}
			msg->sol = 0;
			msg->sl.st.l = 0; /* used in debug mode */
			hdr_idx_init(idx);
			state = HTTP_MSG_RPVER;
			goto http_msg_rpver;
		}

		if (unlikely(!HTTP_IS_CRLF(*ptr)))
			goto http_msg_invalid;

		if (unlikely(*ptr == '\n'))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpbefore, HTTP_MSG_RPBEFORE);
		EAT_AND_JUMP_OR_RETURN(http_msg_rpbefore_cr, HTTP_MSG_RPBEFORE_CR);
		/* stop here */

	case HTTP_MSG_RPBEFORE_CR:
	http_msg_rpbefore_cr:
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_rpbefore, HTTP_MSG_RPBEFORE);
		/* stop here */

	case HTTP_MSG_RPVER:
	http_msg_rpver:
	case HTTP_MSG_RPVER_SP:
	case HTTP_MSG_RPCODE:
	case HTTP_MSG_RPCODE_SP:
	case HTTP_MSG_RPREASON:
		ptr = (char *)http_parse_stsline(msg,
						 state, ptr, end,
						 &msg->next, &msg->msg_state);
		if (unlikely(!ptr))
			return;

		/* we have a full response and we know that we have either a CR
		 * or an LF at <ptr>.
		 */
		hdr_idx_set_start(idx, msg->sl.st.l, *ptr == '\r');

		msg->sol = ptr - buf->p;
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_rpline_end, HTTP_MSG_RPLINE_END);
		goto http_msg_rpline_end;

	case HTTP_MSG_RPLINE_END:
	http_msg_rpline_end:
		/* msg->sol must point to the first of CR or LF. */
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_hdr_first, HTTP_MSG_HDR_FIRST);
		/* stop here */

	/*
	 * Second, states that are specific to the request only
	 */
	case HTTP_MSG_RQBEFORE:
	http_msg_rqbefore:
		if (likely(HTTP_IS_TOKEN(*ptr))) {
			/* we have a start of message, but we have to check
			 * first if we need to remove some CRLF. We can only
			 * do this when o=0.
			 */
			if (likely(ptr != buf->p)) {
				if (buf->o)
					goto http_msg_ood;
				/* Remove empty leading lines, as recommended by RFC2616. */
				bi_fast_delete(buf, ptr - buf->p);
			}
			msg->sol = 0;
			msg->sl.rq.l = 0; /* used in debug mode */
			state = HTTP_MSG_RQMETH;
			goto http_msg_rqmeth;
		}

		if (unlikely(!HTTP_IS_CRLF(*ptr)))
			goto http_msg_invalid;

		if (unlikely(*ptr == '\n'))
			EAT_AND_JUMP_OR_RETURN(http_msg_rqbefore, HTTP_MSG_RQBEFORE);
		EAT_AND_JUMP_OR_RETURN(http_msg_rqbefore_cr, HTTP_MSG_RQBEFORE_CR);
		/* stop here */

	case HTTP_MSG_RQBEFORE_CR:
	http_msg_rqbefore_cr:
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_rqbefore, HTTP_MSG_RQBEFORE);
		/* stop here */

	case HTTP_MSG_RQMETH:
	http_msg_rqmeth:
	case HTTP_MSG_RQMETH_SP:
	case HTTP_MSG_RQURI:
	case HTTP_MSG_RQURI_SP:
	case HTTP_MSG_RQVER:
		ptr = (char *)http_parse_reqline(msg,
						 state, ptr, end,
						 &msg->next, &msg->msg_state);
		if (unlikely(!ptr))
			return;

		/* we have a full request and we know that we have either a CR
		 * or an LF at <ptr>.
		 */
		hdr_idx_set_start(idx, msg->sl.rq.l, *ptr == '\r');

		msg->sol = ptr - buf->p;
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_rqline_end, HTTP_MSG_RQLINE_END);
		goto http_msg_rqline_end;

	case HTTP_MSG_RQLINE_END:
	http_msg_rqline_end:
		/* check for HTTP/0.9 request : no version information available.
		 * msg->sol must point to the first of CR or LF.
		 */
		if (unlikely(msg->sl.rq.v_l == 0))
			goto http_msg_last_lf;

		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_hdr_first, HTTP_MSG_HDR_FIRST);
		/* stop here */

	/*
	 * Common states below
	 */
	case HTTP_MSG_HDR_FIRST:
	http_msg_hdr_first:
		msg->sol = ptr - buf->p;
		if (likely(!HTTP_IS_CRLF(*ptr))) {
			goto http_msg_hdr_name;
		}
		
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_last_lf, HTTP_MSG_LAST_LF);
		goto http_msg_last_lf;

	case HTTP_MSG_HDR_NAME:
	http_msg_hdr_name:
		/* assumes msg->sol points to the first char */
		if (likely(HTTP_IS_TOKEN(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_name, HTTP_MSG_HDR_NAME);

		if (likely(*ptr == ':'))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l1_sp, HTTP_MSG_HDR_L1_SP);

		if (likely(msg->err_pos < -1) || *ptr == '\n')
			goto http_msg_invalid;

		if (msg->err_pos == -1) /* capture error pointer */
			msg->err_pos = ptr - buf->p; /* >= 0 now */

		/* and we still accept this non-token character */
		EAT_AND_JUMP_OR_RETURN(http_msg_hdr_name, HTTP_MSG_HDR_NAME);

	case HTTP_MSG_HDR_L1_SP:
	http_msg_hdr_l1_sp:
		/* assumes msg->sol points to the first char */
		if (likely(HTTP_IS_SPHT(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l1_sp, HTTP_MSG_HDR_L1_SP);

		/* header value can be basically anything except CR/LF */
		msg->sov = ptr - buf->p;

		if (likely(!HTTP_IS_CRLF(*ptr))) {
			goto http_msg_hdr_val;
		}
			
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l1_lf, HTTP_MSG_HDR_L1_LF);
		goto http_msg_hdr_l1_lf;

	case HTTP_MSG_HDR_L1_LF:
	http_msg_hdr_l1_lf:
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l1_lws, HTTP_MSG_HDR_L1_LWS);

	case HTTP_MSG_HDR_L1_LWS:
	http_msg_hdr_l1_lws:
		if (likely(HTTP_IS_SPHT(*ptr))) {
			/* replace HT,CR,LF with spaces */
			for (; buf->p + msg->sov < ptr; msg->sov++)
				buf->p[msg->sov] = ' ';
			goto http_msg_hdr_l1_sp;
		}
		/* we had a header consisting only in spaces ! */
		msg->eol = msg->sov;
		goto http_msg_complete_header;
		
	case HTTP_MSG_HDR_VAL:
	http_msg_hdr_val:
		/* assumes msg->sol points to the first char, and msg->sov
		 * points to the first character of the value.
		 */
		if (likely(!HTTP_IS_CRLF(*ptr)))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_val, HTTP_MSG_HDR_VAL);

		msg->eol = ptr - buf->p;
		/* Note: we could also copy eol into ->eoh so that we have the
		 * real header end in case it ends with lots of LWS, but is this
		 * really needed ?
		 */
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l2_lf, HTTP_MSG_HDR_L2_LF);
		goto http_msg_hdr_l2_lf;

	case HTTP_MSG_HDR_L2_LF:
	http_msg_hdr_l2_lf:
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		EAT_AND_JUMP_OR_RETURN(http_msg_hdr_l2_lws, HTTP_MSG_HDR_L2_LWS);

	case HTTP_MSG_HDR_L2_LWS:
	http_msg_hdr_l2_lws:
		if (unlikely(HTTP_IS_SPHT(*ptr))) {
			/* LWS: replace HT,CR,LF with spaces */
			for (; buf->p + msg->eol < ptr; msg->eol++)
				buf->p[msg->eol] = ' ';
			goto http_msg_hdr_val;
		}
	http_msg_complete_header:
		/*
		 * It was a new header, so the last one is finished.
		 * Assumes msg->sol points to the first char, msg->sov points
		 * to the first character of the value and msg->eol to the
		 * first CR or LF so we know how the line ends. We insert last
		 * header into the index.
		 */
		if (unlikely(hdr_idx_add(msg->eol - msg->sol, buf->p[msg->eol] == '\r',
					 idx, idx->tail) < 0))
			goto http_msg_invalid;

		msg->sol = ptr - buf->p;
		if (likely(!HTTP_IS_CRLF(*ptr))) {
			goto http_msg_hdr_name;
		}
		
		if (likely(*ptr == '\r'))
			EAT_AND_JUMP_OR_RETURN(http_msg_last_lf, HTTP_MSG_LAST_LF);
		goto http_msg_last_lf;

	case HTTP_MSG_LAST_LF:
	http_msg_last_lf:
		/* Assumes msg->sol points to the first of either CR or LF */
		EXPECT_LF_HERE(ptr, http_msg_invalid);
		ptr++;
		msg->sov = msg->next = ptr - buf->p;
		msg->eoh = msg->sol;
		msg->sol = 0;
		msg->msg_state = HTTP_MSG_BODY;
		return;

	case HTTP_MSG_ERROR:
		/* this may only happen if we call http_msg_analyser() twice with an error */
		break;

#ifdef DEBUG_FULL
	default:
		fprintf(stderr, "FIXME !!!! impossible state at %s:%d = %d\n", __FILE__, __LINE__, state);
		exit(1);
#endif
	}
 http_msg_ood:
	/* out of data */
	msg->msg_state = state;
	msg->next = ptr - buf->p;
	return;

 http_msg_invalid:
	/* invalid message */
	msg->msg_state = HTTP_MSG_ERROR;
	msg->next = ptr - buf->p;
	return;
}

/* convert an HTTP/0.9 request into an HTTP/1.0 request. Returns 1 if the
 * conversion succeeded, 0 in case of error. If the request was already 1.X,
 * nothing is done and 1 is returned.
 */
static int http_upgrade_v09_to_v10(struct http_txn *txn)
{
	int delta;
	char *cur_end;
	struct http_msg *msg = &txn->req;

	if (msg->sl.rq.v_l != 0)
		return 1;

	cur_end = msg->chn->buf->p + msg->sl.rq.l;
	delta = 0;

	if (msg->sl.rq.u_l == 0) {
		/* if no URI was set, add "/" */
		delta = buffer_replace2(msg->chn->buf, cur_end, cur_end, " /", 2);
		cur_end += delta;
		http_msg_move_end(msg, delta);
	}
	/* add HTTP version */
	delta = buffer_replace2(msg->chn->buf, cur_end, cur_end, " HTTP/1.0\r\n", 11);
	http_msg_move_end(msg, delta);
	cur_end += delta;
	cur_end = (char *)http_parse_reqline(msg,
					     HTTP_MSG_RQMETH,
					     msg->chn->buf->p, cur_end + 1,
					     NULL, NULL);
	if (unlikely(!cur_end))
		return 0;

	/* we have a full HTTP/1.0 request now and we know that
	 * we have either a CR or an LF at <ptr>.
	 */
	hdr_idx_set_start(&txn->hdr_idx, msg->sl.rq.l, *cur_end == '\r');
	return 1;
}

/* Parse the Connection: header of an HTTP request, looking for both "close"
 * and "keep-alive" values. If we already know that some headers may safely
 * be removed, we remove them now. The <to_del> flags are used for that :
 *  - bit 0 means remove "close" headers (in HTTP/1.0 requests/responses)
 *  - bit 1 means remove "keep-alive" headers (in HTTP/1.1 reqs/resp to 1.1).
 * Presence of the "Upgrade" token is also checked and reported.
 * The TX_HDR_CONN_* flags are adjusted in txn->flags depending on what was
 * found, and TX_CON_*_SET is adjusted depending on what is left so only
 * harmless combinations may be removed. Do not call that after changes have
 * been processed.
 */
void http_parse_connection_header(struct http_txn *txn, struct http_msg *msg, int to_del)
{
	struct hdr_ctx ctx;
	const char *hdr_val = "Connection";
	int hdr_len = 10;

	if (txn->flags & TX_HDR_CONN_PRS)
		return;

	if (unlikely(txn->flags & TX_USE_PX_CONN)) {
		hdr_val = "Proxy-Connection";
		hdr_len = 16;
	}

	ctx.idx = 0;
	txn->flags &= ~(TX_CON_KAL_SET|TX_CON_CLO_SET);
	while (http_find_header2(hdr_val, hdr_len, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
		if (ctx.vlen >= 10 && word_match(ctx.line + ctx.val, ctx.vlen, "keep-alive", 10)) {
			txn->flags |= TX_HDR_CONN_KAL;
			if (to_del & 2)
				http_remove_header2(msg, &txn->hdr_idx, &ctx);
			else
				txn->flags |= TX_CON_KAL_SET;
		}
		else if (ctx.vlen >= 5 && word_match(ctx.line + ctx.val, ctx.vlen, "close", 5)) {
			txn->flags |= TX_HDR_CONN_CLO;
			if (to_del & 1)
				http_remove_header2(msg, &txn->hdr_idx, &ctx);
			else
				txn->flags |= TX_CON_CLO_SET;
		}
		else if (ctx.vlen >= 7 && word_match(ctx.line + ctx.val, ctx.vlen, "upgrade", 7)) {
			txn->flags |= TX_HDR_CONN_UPG;
		}
	}

	txn->flags |= TX_HDR_CONN_PRS;
	return;
}

/* Apply desired changes on the Connection: header. Values may be removed and/or
 * added depending on the <wanted> flags, which are exclusively composed of
 * TX_CON_CLO_SET and TX_CON_KAL_SET, depending on what flags are desired. The
 * TX_CON_*_SET flags are adjusted in txn->flags depending on what is left.
 */
void http_change_connection_header(struct http_txn *txn, struct http_msg *msg, int wanted)
{
	struct hdr_ctx ctx;
	const char *hdr_val = "Connection";
	int hdr_len = 10;

	ctx.idx = 0;


	if (unlikely(txn->flags & TX_USE_PX_CONN)) {
		hdr_val = "Proxy-Connection";
		hdr_len = 16;
	}

	txn->flags &= ~(TX_CON_CLO_SET | TX_CON_KAL_SET);
	while (http_find_header2(hdr_val, hdr_len, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
		if (ctx.vlen >= 10 && word_match(ctx.line + ctx.val, ctx.vlen, "keep-alive", 10)) {
			if (wanted & TX_CON_KAL_SET)
				txn->flags |= TX_CON_KAL_SET;
			else
				http_remove_header2(msg, &txn->hdr_idx, &ctx);
		}
		else if (ctx.vlen >= 5 && word_match(ctx.line + ctx.val, ctx.vlen, "close", 5)) {
			if (wanted & TX_CON_CLO_SET)
				txn->flags |= TX_CON_CLO_SET;
			else
				http_remove_header2(msg, &txn->hdr_idx, &ctx);
		}
	}

	if (wanted == (txn->flags & (TX_CON_CLO_SET|TX_CON_KAL_SET)))
		return;

	if ((wanted & TX_CON_CLO_SET) && !(txn->flags & TX_CON_CLO_SET)) {
		txn->flags |= TX_CON_CLO_SET;
		hdr_val = "Connection: close";
		hdr_len  = 17;
		if (unlikely(txn->flags & TX_USE_PX_CONN)) {
			hdr_val = "Proxy-Connection: close";
			hdr_len = 23;
		}
		http_header_add_tail2(msg, &txn->hdr_idx, hdr_val, hdr_len);
	}

	if ((wanted & TX_CON_KAL_SET) && !(txn->flags & TX_CON_KAL_SET)) {
		txn->flags |= TX_CON_KAL_SET;
		hdr_val = "Connection: keep-alive";
		hdr_len = 22;
		if (unlikely(txn->flags & TX_USE_PX_CONN)) {
			hdr_val = "Proxy-Connection: keep-alive";
			hdr_len = 28;
		}
		http_header_add_tail2(msg, &txn->hdr_idx, hdr_val, hdr_len);
	}
	return;
}

/* Parse the chunk size at msg->next. Once done, it adjusts ->next to point to the
 * first byte of body, and increments msg->sov by the number of bytes parsed,
 * so that we know we can forward between ->sol and ->sov.
 * Return >0 on success, 0 when some data is missing, <0 on error.
 * Note: this function is designed to parse wrapped CRLF at the end of the buffer.
 */
static inline int http_parse_chunk_size(struct http_msg *msg)
{
	const struct buffer *buf = msg->chn->buf;
	const char *ptr = b_ptr(buf, msg->next);
	const char *ptr_old = ptr;
	const char *end = buf->data + buf->size;
	const char *stop = bi_end(buf);
	unsigned int chunk = 0;

	/* The chunk size is in the following form, though we are only
	 * interested in the size and CRLF :
	 *    1*HEXDIGIT *WSP *[ ';' extensions ] CRLF
	 */
	while (1) {
		int c;
		if (ptr == stop)
			return 0;
		c = hex2i(*ptr);
		if (c < 0) /* not a hex digit anymore */
			break;
		if (++ptr >= end)
			ptr = buf->data;
		if (chunk & 0xF8000000) /* integer overflow will occur if result >= 2GB */
			goto error;
		chunk = (chunk << 4) + c;
	}

	/* empty size not allowed */
	if (ptr == ptr_old)
		goto error;

	while (http_is_spht[(unsigned char)*ptr]) {
		if (++ptr >= end)
			ptr = buf->data;
		if (ptr == stop)
			return 0;
	}

	/* Up to there, we know that at least one byte is present at *ptr. Check
	 * for the end of chunk size.
	 */
	while (1) {
		if (likely(HTTP_IS_CRLF(*ptr))) {
			/* we now have a CR or an LF at ptr */
			if (likely(*ptr == '\r')) {
				if (++ptr >= end)
					ptr = buf->data;
				if (ptr == stop)
					return 0;
			}

			if (*ptr != '\n')
				goto error;
			if (++ptr >= end)
				ptr = buf->data;
			/* done */
			break;
		}
		else if (*ptr == ';') {
			/* chunk extension, ends at next CRLF */
			if (++ptr >= end)
				ptr = buf->data;
			if (ptr == stop)
				return 0;

			while (!HTTP_IS_CRLF(*ptr)) {
				if (++ptr >= end)
					ptr = buf->data;
				if (ptr == stop)
					return 0;
			}
			/* we have a CRLF now, loop above */
			continue;
		}
		else
			goto error;
	}

	/* OK we found our CRLF and now <ptr> points to the next byte,
	 * which may or may not be present. We save that into ->next and
	 * ->sov.
	 */
	if (ptr < ptr_old)
		msg->sov += buf->size;
	msg->sov += ptr - ptr_old;
	msg->next = buffer_count(buf, buf->p, ptr);
	msg->chunk_len = chunk;
	msg->body_len += chunk;
	msg->msg_state = chunk ? HTTP_MSG_DATA : HTTP_MSG_TRAILERS;
	return 1;
 error:
	msg->err_pos = buffer_count(buf, buf->p, ptr);
	return -1;
}

/* This function skips trailers in the buffer associated with HTTP
 * message <msg>. The first visited position is msg->next. If the end of
 * the trailers is found, it is automatically scheduled to be forwarded,
 * msg->msg_state switches to HTTP_MSG_DONE, and the function returns >0.
 * If not enough data are available, the function does not change anything
 * except maybe msg->next and msg->sov if it could parse some lines, and returns
 * zero. If a parse error is encountered, the function returns < 0 and does not
 * change anything except maybe msg->next and msg->sov. Note that the message
 * must already be in HTTP_MSG_TRAILERS state before calling this function,
 * which implies that all non-trailers data have already been scheduled for
 * forwarding, and that the difference between msg->sol and msg->sov exactly
 * matches the length of trailers already parsed and not forwarded. It is also
 * important to note that this function is designed to be able to parse wrapped
 * headers at end of buffer.
 */
static int http_forward_trailers(struct http_msg *msg)
{
	const struct buffer *buf = msg->chn->buf;

	/* we have msg->next which points to next line. Look for CRLF. */
	while (1) {
		const char *p1 = NULL, *p2 = NULL;
		const char *ptr = b_ptr(buf, msg->next);
		const char *stop = bi_end(buf);
		int bytes;

		/* scan current line and stop at LF or CRLF */
		while (1) {
			if (ptr == stop)
				return 0;

			if (*ptr == '\n') {
				if (!p1)
					p1 = ptr;
				p2 = ptr;
				break;
			}

			if (*ptr == '\r') {
				if (p1) {
					msg->err_pos = buffer_count(buf, buf->p, ptr);
					return -1;
				}
				p1 = ptr;
			}

			ptr++;
			if (ptr >= buf->data + buf->size)
				ptr = buf->data;
		}

		/* after LF; point to beginning of next line */
		p2++;
		if (p2 >= buf->data + buf->size)
			p2 = buf->data;

		bytes = p2 - b_ptr(buf, msg->next);
		if (bytes < 0)
			bytes += buf->size;

		/* schedule this line for forwarding */
		msg->sov += bytes;
		if (msg->sov >= buf->size)
			msg->sov -= buf->size;

		if (p1 == b_ptr(buf, msg->next)) {
			/* LF/CRLF at beginning of line => end of trailers at p2.
			 * Everything was scheduled for forwarding, there's nothing
			 * left from this message.
			 */
			msg->next = buffer_count(buf, buf->p, p2);
			msg->msg_state = HTTP_MSG_DONE;
			return 1;
		}
		/* OK, next line then */
		msg->next = buffer_count(buf, buf->p, p2);
	}
}

/* This function may be called only in HTTP_MSG_CHUNK_CRLF. It reads the CRLF or
 * a possible LF alone at the end of a chunk. It automatically adjusts msg->sov,
 * ->sol, ->next in order to include this part into the next forwarding phase.
 * Note that the caller must ensure that ->p points to the first byte to parse.
 * It also sets msg_state to HTTP_MSG_CHUNK_SIZE and returns >0 on success. If
 * not enough data are available, the function does not change anything and
 * returns zero. If a parse error is encountered, the function returns < 0 and
 * does not change anything. Note: this function is designed to parse wrapped
 * CRLF at the end of the buffer.
 */
static inline int http_skip_chunk_crlf(struct http_msg *msg)
{
	const struct buffer *buf = msg->chn->buf;
	const char *ptr;
	int bytes;

	/* NB: we'll check data availabilty at the end. It's not a
	 * problem because whatever we match first will be checked
	 * against the correct length.
	 */
	bytes = 1;
	ptr = buf->p;
	if (*ptr == '\r') {
		bytes++;
		ptr++;
		if (ptr >= buf->data + buf->size)
			ptr = buf->data;
	}

	if (bytes > buf->i)
		return 0;

	if (*ptr != '\n') {
		msg->err_pos = buffer_count(buf, buf->p, ptr);
		return -1;
	}

	ptr++;
	if (ptr >= buf->data + buf->size)
		ptr = buf->data;
	/* prepare the CRLF to be forwarded (between ->sol and ->sov) */
	msg->sol = 0;
	msg->sov = msg->next = bytes;
	msg->msg_state = HTTP_MSG_CHUNK_SIZE;
	return 1;
}


/*
 * Selects a compression algorithm depending on the client request.
 */
int select_compression_request_header(struct session *s, struct buffer *req)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->req;
	struct hdr_ctx ctx;
	struct comp_algo *comp_algo = NULL;
	struct comp_algo *comp_algo_back = NULL;

	/* Disable compression for older user agents announcing themselves as "Mozilla/4"
	 * unless they are known good (MSIE 6 with XP SP2, or MSIE 7 and later).
	 * See http://zoompf.com/2012/02/lose-the-wait-http-compression for more details.
	 */
	ctx.idx = 0;
	if (http_find_header2("User-Agent", 10, req->p, &txn->hdr_idx, &ctx) &&
	    ctx.vlen >= 9 &&
	    memcmp(ctx.line + ctx.val, "Mozilla/4", 9) == 0 &&
	    (ctx.vlen < 31 ||
	     memcmp(ctx.line + ctx.val + 25, "MSIE ", 5) != 0 ||
	     ctx.line[ctx.val + 30] < '6' ||
	     (ctx.line[ctx.val + 30] == '6' &&
	      (ctx.vlen < 54 || memcmp(ctx.line + 51, "SV1", 3) != 0)))) {
	    s->comp_algo = NULL;
	    return 0;
	}

	/* search for the algo in the backend in priority or the frontend */
	if ((s->be->comp && (comp_algo_back = s->be->comp->algos)) || (s->fe->comp && (comp_algo_back = s->fe->comp->algos))) {
		ctx.idx = 0;
		while (http_find_header2("Accept-Encoding", 15, req->p, &txn->hdr_idx, &ctx)) {
			for (comp_algo = comp_algo_back; comp_algo; comp_algo = comp_algo->next) {
				if (word_match(ctx.line + ctx.val, ctx.vlen, comp_algo->name, comp_algo->name_len)) {
					s->comp_algo = comp_algo;

					/* remove all occurrences of the header when "compression offload" is set */

					if ((s->be->comp && s->be->comp->offload) ||
					    (s->fe->comp && s->fe->comp->offload)) {
						http_remove_header2(msg, &txn->hdr_idx, &ctx);
						ctx.idx = 0;
						while (http_find_header2("Accept-Encoding", 15, req->p, &txn->hdr_idx, &ctx)) {
							http_remove_header2(msg, &txn->hdr_idx, &ctx);
						}
					}
					return 1;
				}
			}
		}
	}

	/* identity is implicit does not require headers */
	if ((s->be->comp && (comp_algo_back = s->be->comp->algos)) || (s->fe->comp && (comp_algo_back = s->fe->comp->algos))) {
		for (comp_algo = comp_algo_back; comp_algo; comp_algo = comp_algo->next) {
			if (comp_algo->add_data == identity_add_data) {
				s->comp_algo = comp_algo;
				return 1;
			}
		}
	}

	s->comp_algo = NULL;
	return 0;
}

/*
 * Selects a comression algorithm depending of the server response.
 */
int select_compression_response_header(struct session *s, struct buffer *res)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->rsp;
	struct hdr_ctx ctx;
	struct comp_type *comp_type;

	/* no common compression algorithm was found in request header */
	if (s->comp_algo == NULL)
		goto fail;

	/* HTTP < 1.1 should not be compressed */
	if (!(msg->flags & HTTP_MSGF_VER_11))
		goto fail;

	/* 200 only */
	if (txn->status != 200)
		goto fail;

	/* Content-Length is null */
	if (!(msg->flags & HTTP_MSGF_TE_CHNK) && msg->body_len == 0)
		goto fail;

	/* content is already compressed */
	ctx.idx = 0;
	if (http_find_header2("Content-Encoding", 16, res->p, &txn->hdr_idx, &ctx))
		goto fail;

	/* no compression when Cache-Control: no-transform is present in the message */
	ctx.idx = 0;
	while (http_find_header2("Cache-Control", 13, res->p, &txn->hdr_idx, &ctx)) {
		if (word_match(ctx.line + ctx.val, ctx.vlen, "no-transform", 12))
			goto fail;
	}

	comp_type = NULL;

	/* we don't want to compress multipart content-types, nor content-types that are
	 * not listed in the "compression type" directive if any. If no content-type was
	 * found but configuration requires one, we don't compress either. Backend has
	 * the priority.
	 */
	ctx.idx = 0;
	if (http_find_header2("Content-Type", 12, res->p, &txn->hdr_idx, &ctx)) {
		if (ctx.vlen >= 9 && strncasecmp("multipart", ctx.line+ctx.val, 9) == 0)
			goto fail;

		if ((s->be->comp && (comp_type = s->be->comp->types)) ||
		    (s->fe->comp && (comp_type = s->fe->comp->types))) {
			for (; comp_type; comp_type = comp_type->next) {
				if (ctx.vlen >= comp_type->name_len &&
				    strncasecmp(ctx.line+ctx.val, comp_type->name, comp_type->name_len) == 0)
					/* this Content-Type should be compressed */
					break;
			}
			/* this Content-Type should not be compressed */
			if (comp_type == NULL)
				goto fail;
		}
	}
	else { /* no content-type header */
		if ((s->be->comp && s->be->comp->types) || (s->fe->comp && s->fe->comp->types))
			goto fail; /* a content-type was required */
	}

	/* limit compression rate */
	if (global.comp_rate_lim > 0)
		if (read_freq_ctr(&global.comp_bps_in) > global.comp_rate_lim)
			goto fail;

	/* limit cpu usage */
	if (idle_pct < compress_min_idle)
		goto fail;

	/* initialize compression */
	if (s->comp_algo->init(&s->comp_ctx, global.tune.comp_maxlevel) < 0)
		goto fail;

	s->flags |= SN_COMP_READY;

	/* remove Content-Length header */
	ctx.idx = 0;
	if ((msg->flags & HTTP_MSGF_CNT_LEN) && http_find_header2("Content-Length", 14, res->p, &txn->hdr_idx, &ctx))
		http_remove_header2(msg, &txn->hdr_idx, &ctx);

	/* add Transfer-Encoding header */
	if (!(msg->flags & HTTP_MSGF_TE_CHNK))
		http_header_add_tail2(&txn->rsp, &txn->hdr_idx, "Transfer-Encoding: chunked", 26);

	/*
	 * Add Content-Encoding header when it's not identity encoding.
         * RFC 2616 : Identity encoding: This content-coding is used only in the
	 * Accept-Encoding header, and SHOULD NOT be used in the Content-Encoding
	 * header.
	 */
	if (s->comp_algo->add_data != identity_add_data) {
		trash.len = 18;
		memcpy(trash.str, "Content-Encoding: ", trash.len);
		memcpy(trash.str + trash.len, s->comp_algo->name, s->comp_algo->name_len);
		trash.len += s->comp_algo->name_len;
		trash.str[trash.len] = '\0';
		http_header_add_tail2(&txn->rsp, &txn->hdr_idx, trash.str, trash.len);
	}
	return 1;

fail:
	s->comp_algo = NULL;
	return 0;
}


/* This stream analyser waits for a complete HTTP request. It returns 1 if the
 * processing can continue on next analysers, or zero if it either needs more
 * data or wants to immediately abort the request (eg: timeout, error, ...). It
 * is tied to AN_REQ_WAIT_HTTP and may may remove itself from s->req->analysers
 * when it has nothing left to do, and may remove any analyser when it wants to
 * abort.
 */
int http_wait_for_request(struct session *s, struct channel *req, int an_bit)
{
	/*
	 * We will parse the partial (or complete) lines.
	 * We will check the request syntax, and also join multi-line
	 * headers. An index of all the lines will be elaborated while
	 * parsing.
	 *
	 * For the parsing, we use a 28 states FSM.
	 *
	 * Here is the information we currently have :
	 *   req->buf->p             = beginning of request
	 *   req->buf->p + msg->eoh  = end of processed headers / start of current one
	 *   req->buf->p + req->buf->i    = end of input data
	 *   msg->eol           = end of current header or line (LF or CRLF)
	 *   msg->next          = first non-visited byte
	 *
	 * At end of parsing, we may perform a capture of the error (if any), and
	 * we will set a few fields (msg->sol, txn->meth, sn->flags/SN_REDIRECTABLE).
	 * We also check for monitor-uri, logging, HTTP/0.9 to 1.0 conversion, and
	 * finally headers capture.
	 */

	int cur_idx;
	int use_close_only;
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->req;
	struct hdr_ctx ctx;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		req->buf->i,
		req->analysers);

	/* we're speaking HTTP here, so let's speak HTTP to the client */
	s->srv_error = http_return_srv_error;

	/* There's a protected area at the end of the buffer for rewriting
	 * purposes. We don't want to start to parse the request if the
	 * protected area is affected, because we may have to move processed
	 * data later, which is much more complicated.
	 */
	if (buffer_not_empty(req->buf) && msg->msg_state < HTTP_MSG_ERROR) {
		if ((txn->flags & TX_NOT_FIRST) &&
		    unlikely(channel_full(req) ||
			     bi_end(req->buf) < b_ptr(req->buf, msg->next) ||
			     bi_end(req->buf) > req->buf->data + req->buf->size - global.tune.maxrewrite)) {
			if (req->buf->o) {
				if (req->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_WRITE_ERROR|CF_WRITE_TIMEOUT))
					goto failed_keep_alive;
				/* some data has still not left the buffer, wake us once that's done */
				channel_dont_connect(req);
				req->flags |= CF_READ_DONTWAIT; /* try to get back here ASAP */
				return 0;
			}
			if (bi_end(req->buf) < b_ptr(req->buf, msg->next) ||
			    bi_end(req->buf) > req->buf->data + req->buf->size - global.tune.maxrewrite)
				buffer_slow_realign(msg->chn->buf);
		}

		/* Note that we have the same problem with the response ; we
		 * may want to send a redirect, error or anything which requires
		 * some spare space. So we'll ensure that we have at least
		 * maxrewrite bytes available in the response buffer before
		 * processing that one. This will only affect pipelined
		 * keep-alive requests.
		 */
		if ((txn->flags & TX_NOT_FIRST) &&
		    unlikely(channel_full(s->rep) ||
			     bi_end(s->rep->buf) < b_ptr(s->rep->buf, txn->rsp.next) ||
			     bi_end(s->rep->buf) > s->rep->buf->data + s->rep->buf->size - global.tune.maxrewrite)) {
			if (s->rep->buf->o) {
				if (s->rep->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_WRITE_ERROR|CF_WRITE_TIMEOUT))
					goto failed_keep_alive;
				/* don't let a connection request be initiated */
				channel_dont_connect(req);
				s->rep->flags &= ~CF_EXPECT_MORE; /* speed up sending a previous response */
				s->rep->analysers |= an_bit; /* wake us up once it changes */
				return 0;
			}
		}

		if (likely(msg->next < req->buf->i)) /* some unparsed data are available */
			http_msg_analyzer(msg, &txn->hdr_idx);
	}

	/* 1: we might have to print this header in debug mode */
	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)) &&
		     (msg->msg_state >= HTTP_MSG_BODY || msg->msg_state == HTTP_MSG_ERROR))) {
		char *eol, *sol;

		sol = req->buf->p;
		/* this is a bit complex : in case of error on the request line,
		 * we know that rq.l is still zero, so we display only the part
		 * up to the end of the line (truncated by debug_hdr).
		 */
		eol = sol + (msg->sl.rq.l ? msg->sl.rq.l : req->buf->i);
		debug_hdr("clireq", s, sol, eol);

		sol += hdr_idx_first_pos(&txn->hdr_idx);
		cur_idx = hdr_idx_first_idx(&txn->hdr_idx);

		while (cur_idx) {
			eol = sol + txn->hdr_idx.v[cur_idx].len;
			debug_hdr("clihdr", s, sol, eol);
			sol = eol + txn->hdr_idx.v[cur_idx].cr + 1;
			cur_idx = txn->hdr_idx.v[cur_idx].next;
		}
	}


	/*
	 * Now we quickly check if we have found a full valid request.
	 * If not so, we check the FD and buffer states before leaving.
	 * A full request is indicated by the fact that we have seen
	 * the double LF/CRLF, so the state is >= HTTP_MSG_BODY. Invalid
	 * requests are checked first. When waiting for a second request
	 * on a keep-alive session, if we encounter and error, close, t/o,
	 * we note the error in the session flags but don't set any state.
	 * Since the error will be noted there, it will not be counted by
	 * process_session() as a frontend error.
	 * Last, we may increase some tracked counters' http request errors on
	 * the cases that are deliberately the client's fault. For instance,
	 * a timeout or connection reset is not counted as an error. However
	 * a bad request is.
	 */

	if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
		/*
		 * First, let's catch bad requests.
		 */
		if (unlikely(msg->msg_state == HTTP_MSG_ERROR)) {
			session_inc_http_req_ctr(s);
			session_inc_http_err_ctr(s);
			proxy_inc_fe_req_ctr(s->fe);
			goto return_bad_req;
		}

		/* 1: Since we are in header mode, if there's no space
		 *    left for headers, we won't be able to free more
		 *    later, so the session will never terminate. We
		 *    must terminate it now.
		 */
		if (unlikely(buffer_full(req->buf, global.tune.maxrewrite))) {
			/* FIXME: check if URI is set and return Status
			 * 414 Request URI too long instead.
			 */
			session_inc_http_req_ctr(s);
			session_inc_http_err_ctr(s);
			proxy_inc_fe_req_ctr(s->fe);
			if (msg->err_pos < 0)
				msg->err_pos = req->buf->i;
			goto return_bad_req;
		}

		/* 2: have we encountered a read error ? */
		else if (req->flags & CF_READ_ERROR) {
			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_CLICL;

			if (txn->flags & TX_WAIT_NEXT_RQ)
				goto failed_keep_alive;

			/* we cannot return any message on error */
			if (msg->err_pos >= 0) {
				http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
				session_inc_http_err_ctr(s);
			}

			txn->status = 400;
			stream_int_retnclose(req->prod, NULL);
			msg->msg_state = HTTP_MSG_ERROR;
			req->analysers = 0;

			session_inc_http_req_ctr(s);
			proxy_inc_fe_req_ctr(s->fe);
			s->fe->fe_counters.failed_req++;
			if (s->listener->counters)
				s->listener->counters->failed_req++;

			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_R;
			return 0;
		}

		/* 3: has the read timeout expired ? */
		else if (req->flags & CF_READ_TIMEOUT || tick_is_expired(req->analyse_exp, now_ms)) {
			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_CLITO;

			if (txn->flags & TX_WAIT_NEXT_RQ)
				goto failed_keep_alive;

			/* read timeout : give up with an error message. */
			if (msg->err_pos >= 0) {
				http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
				session_inc_http_err_ctr(s);
			}
			txn->status = 408;
			stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_408));
			msg->msg_state = HTTP_MSG_ERROR;
			req->analysers = 0;

			session_inc_http_req_ctr(s);
			proxy_inc_fe_req_ctr(s->fe);
			s->fe->fe_counters.failed_req++;
			if (s->listener->counters)
				s->listener->counters->failed_req++;

			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_R;
			return 0;
		}

		/* 4: have we encountered a close ? */
		else if (req->flags & CF_SHUTR) {
			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_CLICL;

			if (txn->flags & TX_WAIT_NEXT_RQ)
				goto failed_keep_alive;

			if (msg->err_pos >= 0)
				http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
			txn->status = 400;
			stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));
			msg->msg_state = HTTP_MSG_ERROR;
			req->analysers = 0;

			session_inc_http_err_ctr(s);
			session_inc_http_req_ctr(s);
			proxy_inc_fe_req_ctr(s->fe);
			s->fe->fe_counters.failed_req++;
			if (s->listener->counters)
				s->listener->counters->failed_req++;

			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_R;
			return 0;
		}

		channel_dont_connect(req);
		req->flags |= CF_READ_DONTWAIT; /* try to get back here ASAP */
		s->rep->flags &= ~CF_EXPECT_MORE; /* speed up sending a previous response */
#ifdef TCP_QUICKACK
		if (s->listener->options & LI_O_NOQUICKACK && req->buf->i) {
			/* We need more data, we have to re-enable quick-ack in case we
			 * previously disabled it, otherwise we might cause the client
			 * to delay next data.
			 */
			setsockopt(s->si[0].conn->t.sock.fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
		}
#endif

		if ((msg->msg_state != HTTP_MSG_RQBEFORE) && (txn->flags & TX_WAIT_NEXT_RQ)) {
			/* If the client starts to talk, let's fall back to
			 * request timeout processing.
			 */
			txn->flags &= ~TX_WAIT_NEXT_RQ;
			req->analyse_exp = TICK_ETERNITY;
		}

		/* just set the request timeout once at the beginning of the request */
		if (!tick_isset(req->analyse_exp)) {
			if ((msg->msg_state == HTTP_MSG_RQBEFORE) &&
			    (txn->flags & TX_WAIT_NEXT_RQ) &&
			    tick_isset(s->be->timeout.httpka))
				req->analyse_exp = tick_add(now_ms, s->be->timeout.httpka);
			else
				req->analyse_exp = tick_add_ifset(now_ms, s->be->timeout.httpreq);
		}

		/* we're not ready yet */
		return 0;

	failed_keep_alive:
		/* Here we process low-level errors for keep-alive requests. In
		 * short, if the request is not the first one and it experiences
		 * a timeout, read error or shutdown, we just silently close so
		 * that the client can try again.
		 */
		txn->status = 0;
		msg->msg_state = HTTP_MSG_RQBEFORE;
		req->analysers = 0;
		s->logs.logwait = 0;
		s->rep->flags &= ~CF_EXPECT_MORE; /* speed up sending a previous response */
		stream_int_retnclose(req->prod, NULL);
		return 0;
	}

	/* OK now we have a complete HTTP request with indexed headers. Let's
	 * complete the request parsing by setting a few fields we will need
	 * later. At this point, we have the last CRLF at req->buf->data + msg->eoh.
	 * If the request is in HTTP/0.9 form, the rule is still true, and eoh
	 * points to the CRLF of the request line. msg->next points to the first
	 * byte after the last LF. msg->sov points to the first byte of data.
	 * msg->eol cannot be trusted because it may have been left uninitialized
	 * (for instance in the absence of headers).
	 */

	session_inc_http_req_ctr(s);
	proxy_inc_fe_req_ctr(s->fe); /* one more valid request for this FE */

	if (txn->flags & TX_WAIT_NEXT_RQ) {
		/* kill the pending keep-alive timeout */
		txn->flags &= ~TX_WAIT_NEXT_RQ;
		req->analyse_exp = TICK_ETERNITY;
	}


	/* Maybe we found in invalid header name while we were configured not
	 * to block on that, so we have to capture it now.
	 */
	if (unlikely(msg->err_pos >= 0))
		http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);

	/*
	 * 1: identify the method
	 */
	txn->meth = find_http_meth(req->buf->p, msg->sl.rq.m_l);

	/* we can make use of server redirect on GET and HEAD */
	if (txn->meth == HTTP_METH_GET || txn->meth == HTTP_METH_HEAD)
		s->flags |= SN_REDIRECTABLE;

	/*
	 * 2: check if the URI matches the monitor_uri.
	 * We have to do this for every request which gets in, because
	 * the monitor-uri is defined by the frontend.
	 */
	if (unlikely((s->fe->monitor_uri_len != 0) &&
		     (s->fe->monitor_uri_len == msg->sl.rq.u_l) &&
		     !memcmp(req->buf->p + msg->sl.rq.u,
			     s->fe->monitor_uri,
			     s->fe->monitor_uri_len))) {
		/*
		 * We have found the monitor URI
		 */
		struct acl_cond *cond;

		s->flags |= SN_MONITOR;
		s->fe->fe_counters.intercepted_req++;

		/* Check if we want to fail this monitor request or not */
		list_for_each_entry(cond, &s->fe->mon_fail_cond, list) {
			int ret = acl_exec_cond(cond, s->fe, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);

			ret = acl_pass(ret);
			if (cond->pol == ACL_COND_UNLESS)
				ret = !ret;

			if (ret) {
				/* we fail this request, let's return 503 service unavail */
				txn->status = 503;
				stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_503));
				goto return_prx_cond;
			}
		}

		/* nothing to fail, let's reply normaly */
		txn->status = 200;
		stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_200));
		goto return_prx_cond;
	}

	/*
	 * 3: Maybe we have to copy the original REQURI for the logs ?
	 * Note: we cannot log anymore if the request has been
	 * classified as invalid.
	 */
	if (unlikely(s->logs.logwait & LW_REQ)) {
		/* we have a complete HTTP request that we must log */
		if ((txn->uri = pool_alloc2(pool2_requri)) != NULL) {
			int urilen = msg->sl.rq.l;

			if (urilen >= REQURI_LEN)
				urilen = REQURI_LEN - 1;
			memcpy(txn->uri, req->buf->p, urilen);
			txn->uri[urilen] = 0;

			if (!(s->logs.logwait &= ~(LW_REQ|LW_INIT)))
				s->do_log(s);
		} else {
			Alert("HTTP logging : out of memory.\n");
		}
	}

	if (!LIST_ISEMPTY(&s->fe->format_unique_id))
		s->unique_id = pool_alloc2(pool2_uniqueid);

	/* 4. We may have to convert HTTP/0.9 requests to HTTP/1.0 */
	if (unlikely(msg->sl.rq.v_l == 0) && !http_upgrade_v09_to_v10(txn))
		goto return_bad_req;

	/* ... and check if the request is HTTP/1.1 or above */
	if ((msg->sl.rq.v_l == 8) &&
	    ((req->buf->p[msg->sl.rq.v + 5] > '1') ||
	     ((req->buf->p[msg->sl.rq.v + 5] == '1') &&
	      (req->buf->p[msg->sl.rq.v + 7] >= '1'))))
		msg->flags |= HTTP_MSGF_VER_11;

	/* "connection" has not been parsed yet */
	txn->flags &= ~(TX_HDR_CONN_PRS | TX_HDR_CONN_CLO | TX_HDR_CONN_KAL | TX_HDR_CONN_UPG);

	/* if the frontend has "option http-use-proxy-header", we'll check if
	 * we have what looks like a proxied connection instead of a connection,
	 * and in this case set the TX_USE_PX_CONN flag to use Proxy-connection.
	 * Note that this is *not* RFC-compliant, however browsers and proxies
	 * happen to do that despite being non-standard :-(
	 * We consider that a request not beginning with either '/' or '*' is
	 * a proxied connection, which covers both "scheme://location" and
	 * CONNECT ip:port.
	 */
	if ((s->fe->options2 & PR_O2_USE_PXHDR) &&
	    req->buf->p[msg->sl.rq.u] != '/' && req->buf->p[msg->sl.rq.u] != '*')
		txn->flags |= TX_USE_PX_CONN;

	/* transfer length unknown*/
	msg->flags &= ~HTTP_MSGF_XFER_LEN;

	/* 5: we may need to capture headers */
	if (unlikely((s->logs.logwait & LW_REQHDR) && txn->req.cap))
		capture_headers(req->buf->p, &txn->hdr_idx,
				txn->req.cap, s->fe->req_cap);

	/* 6: determine the transfer-length.
	 * According to RFC2616 #4.4, amended by the HTTPbis working group,
	 * the presence of a message-body in a REQUEST and its transfer length
	 * must be determined that way (in order of precedence) :
	 *   1. The presence of a message-body in a request is signaled by the
	 *      inclusion of a Content-Length or Transfer-Encoding header field
	 *      in the request's header fields.  When a request message contains
	 *      both a message-body of non-zero length and a method that does
	 *      not define any semantics for that request message-body, then an
	 *      origin server SHOULD either ignore the message-body or respond
	 *      with an appropriate error message (e.g., 413).  A proxy or
	 *      gateway, when presented the same request, SHOULD either forward
	 *      the request inbound with the message- body or ignore the
	 *      message-body when determining a response.
	 *
	 *   2. If a Transfer-Encoding header field (Section 9.7) is present
	 *      and the "chunked" transfer-coding (Section 6.2) is used, the
	 *      transfer-length is defined by the use of this transfer-coding.
	 *      If a Transfer-Encoding header field is present and the "chunked"
	 *      transfer-coding is not present, the transfer-length is defined
	 *      by the sender closing the connection.
	 *
	 *   3. If a Content-Length header field is present, its decimal value in
	 *      OCTETs represents both the entity-length and the transfer-length.
	 *      If a message is received with both a Transfer-Encoding header
	 *      field and a Content-Length header field, the latter MUST be ignored.
	 *
	 *   4. By the server closing the connection. (Closing the connection
	 *      cannot be used to indicate the end of a request body, since that
	 *      would leave no possibility for the server to send back a response.)
	 *
	 *   Whenever a transfer-coding is applied to a message-body, the set of
	 *   transfer-codings MUST include "chunked", unless the message indicates
	 *   it is terminated by closing the connection.  When the "chunked"
	 *   transfer-coding is used, it MUST be the last transfer-coding applied
	 *   to the message-body.
	 */

	use_close_only = 0;
	ctx.idx = 0;
	/* set TE_CHNK and XFER_LEN only if "chunked" is seen last */
	while ((msg->flags & HTTP_MSGF_VER_11) &&
	       http_find_header2("Transfer-Encoding", 17, req->buf->p, &txn->hdr_idx, &ctx)) {
		if (ctx.vlen == 7 && strncasecmp(ctx.line + ctx.val, "chunked", 7) == 0)
			msg->flags |= (HTTP_MSGF_TE_CHNK | HTTP_MSGF_XFER_LEN);
		else if (msg->flags & HTTP_MSGF_TE_CHNK) {
			/* bad transfer-encoding (chunked followed by something else) */
			use_close_only = 1;
			msg->flags &= ~(HTTP_MSGF_TE_CHNK | HTTP_MSGF_XFER_LEN);
			break;
		}
	}

	ctx.idx = 0;
	while (!(msg->flags & HTTP_MSGF_TE_CHNK) && !use_close_only &&
	       http_find_header2("Content-Length", 14, req->buf->p, &txn->hdr_idx, &ctx)) {
		signed long long cl;

		if (!ctx.vlen) {
			msg->err_pos = ctx.line + ctx.val - req->buf->p;
			goto return_bad_req;
		}

		if (strl2llrc(ctx.line + ctx.val, ctx.vlen, &cl)) {
			msg->err_pos = ctx.line + ctx.val - req->buf->p;
			goto return_bad_req; /* parse failure */
		}

		if (cl < 0) {
			msg->err_pos = ctx.line + ctx.val - req->buf->p;
			goto return_bad_req;
		}

		if ((msg->flags & HTTP_MSGF_CNT_LEN) && (msg->chunk_len != cl)) {
			msg->err_pos = ctx.line + ctx.val - req->buf->p;
			goto return_bad_req; /* already specified, was different */
		}

		msg->flags |= HTTP_MSGF_CNT_LEN | HTTP_MSGF_XFER_LEN;
		msg->body_len = msg->chunk_len = cl;
	}

	/* bodyless requests have a known length */
	if (!use_close_only)
		msg->flags |= HTTP_MSGF_XFER_LEN;

	/* end of job, return OK */
	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;
	return 1;

 return_bad_req:
	/* We centralize bad requests processing here */
	if (unlikely(msg->msg_state == HTTP_MSG_ERROR) || msg->err_pos >= 0) {
		/* we detected a parsing error. We want to archive this request
		 * in the dedicated proxy area for later troubleshooting.
		 */
		http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
	}

	txn->req.msg_state = HTTP_MSG_ERROR;
	txn->status = 400;
	stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));

	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;

 return_prx_cond:
	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;

	req->analysers = 0;
	req->analyse_exp = TICK_ETERNITY;
	return 0;
}

/* We reached the stats page through a POST request.
 * Parse the posted data and enable/disable servers if necessary.
 * Returns 1 if request was parsed or zero if it needs more data.
 */
int http_process_req_stat_post(struct stream_interface *si, struct http_txn *txn, struct channel *req)
{
	struct proxy *px = NULL;
	struct server *sv = NULL;

	char key[LINESIZE];
	int action = ST_ADM_ACTION_NONE;
	int reprocess = 0;

	int total_servers = 0;
	int altered_servers = 0;

	char *first_param, *cur_param, *next_param, *end_params;
	char *st_cur_param = NULL;
	char *st_next_param = NULL;

	first_param = req->buf->p + txn->req.eoh + 2;
	end_params  = first_param + txn->req.body_len;

	cur_param = next_param = end_params;

	if (end_params >= req->buf->data + req->buf->size - global.tune.maxrewrite) {
		/* Prevent buffer overflow */
		si->applet.ctx.stats.st_code = STAT_STATUS_EXCD;
		return 1;
	}
	else if (end_params > req->buf->p + req->buf->i) {
		/* we need more data */
		si->applet.ctx.stats.st_code = STAT_STATUS_NONE;
		return 0;
	}

	*end_params = '\0';

	si->applet.ctx.stats.st_code = STAT_STATUS_NONE;

	/*
	 * Parse the parameters in reverse order to only store the last value.
	 * From the html form, the backend and the action are at the end.
	 */
	while (cur_param > first_param) {
		char *value;
		int poffset, plen;

		cur_param--;
		if ((*cur_param == '&') || (cur_param == first_param)) {
 reprocess_servers:
			/* Parse the key */
			poffset = (cur_param != first_param ? 1 : 0);
			plen = next_param - cur_param + (cur_param == first_param ? 1 : 0);
			if ((plen > 0) && (plen <= sizeof(key))) {
				strncpy(key, cur_param + poffset, plen);
				key[plen - 1] = '\0';
			} else {
				si->applet.ctx.stats.st_code = STAT_STATUS_EXCD;
				goto out;
			}

			/* Parse the value */
			value = key;
			while (*value != '\0' && *value != '=') {
				value++;
			}
			if (*value == '=') {
				/* Ok, a value is found, we can mark the end of the key */
				*value++ = '\0';
			}

			if (!url_decode(key) || !url_decode(value))
				break;

			/* Now we can check the key to see what to do */
			if (!px && (strcmp(key, "b") == 0)) {
				if ((px = findproxy(value, PR_CAP_BE)) == NULL) {
					/* the backend name is unknown or ambiguous (duplicate names) */
					si->applet.ctx.stats.st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (!action && (strcmp(key, "action") == 0)) {
				if (strcmp(value, "disable") == 0) {
					action = ST_ADM_ACTION_DISABLE;
				}
				else if (strcmp(value, "enable") == 0) {
					action = ST_ADM_ACTION_ENABLE;
				}
				else if (strcmp(value, "stop") == 0) {
					action = ST_ADM_ACTION_STOP;
				}
				else if (strcmp(value, "start") == 0) {
					action = ST_ADM_ACTION_START;
				}
				else if (strcmp(value, "shutdown") == 0) {
					action = ST_ADM_ACTION_SHUTDOWN;
				}
				else {
					si->applet.ctx.stats.st_code = STAT_STATUS_ERRP;
					goto out;
				}
			}
			else if (strcmp(key, "s") == 0) {
				if (!(px && action)) {
					/*
					 * Indicates that we'll need to reprocess the parameters
					 * as soon as backend and action are known
					 */
					if (!reprocess) {
						st_cur_param  = cur_param;
						st_next_param = next_param;
					}
					reprocess = 1;
				}
				else if ((sv = findserver(px, value)) != NULL) {
					switch (action) {
					case ST_ADM_ACTION_DISABLE:
						if ((px->state != PR_STSTOPPED) && !(sv->state & SRV_MAINTAIN)) {
							/* Not already in maintenance, we can change the server state */
							sv->state |= SRV_MAINTAIN;
							set_server_down(sv);
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_ENABLE:
						if ((px->state != PR_STSTOPPED) && (sv->state & SRV_MAINTAIN)) {
							/* Already in maintenance, we can change the server state */
							set_server_up(sv);
							sv->health = sv->rise;	/* up, but will fall down at first failure */
							altered_servers++;
							total_servers++;
						}
						break;
					case ST_ADM_ACTION_STOP:
					case ST_ADM_ACTION_START:
						if (action == ST_ADM_ACTION_START)
							sv->uweight = sv->iweight;
						else
							sv->uweight = 0;

						if (px->lbprm.algo & BE_LB_PROP_DYN) {
							/* we must take care of not pushing the server to full throttle during slow starts */
							if ((sv->state & SRV_WARMINGUP) && (px->lbprm.algo & BE_LB_PROP_DYN))
								sv->eweight = (BE_WEIGHT_SCALE * (now.tv_sec - sv->last_change) + sv->slowstart - 1) / sv->slowstart;
							else
								sv->eweight = BE_WEIGHT_SCALE;
							sv->eweight *= sv->uweight;
						} else {
							sv->eweight = sv->uweight;
						}

						/* static LB algorithms are a bit harder to update */
						if (px->lbprm.update_server_eweight)
							px->lbprm.update_server_eweight(sv);
						else if (sv->eweight) {
							if (px->lbprm.set_server_status_up)
								px->lbprm.set_server_status_up(sv);
						}
						else {
							if (px->lbprm.set_server_status_down)
								px->lbprm.set_server_status_down(sv);
						}
						altered_servers++;
						total_servers++;
						break;
					case ST_ADM_ACTION_SHUTDOWN:
						if (px->state != PR_STSTOPPED) {
							struct session *sess, *sess_bck;

							list_for_each_entry_safe(sess, sess_bck, &sv->actconns, by_srv)
								if (sess->srv_conn == sv)
									session_shutdown(sess, SN_ERR_KILLED);

							altered_servers++;
							total_servers++;
						}
						break;
					}
				} else {
					/* the server name is unknown or ambiguous (duplicate names) */
					total_servers++;
				}
			}
			if (reprocess && px && action) {
				/* Now, we know the backend and the action chosen by the user.
				 * We can safely restart from the first server parameter
				 * to reprocess them
				 */
				cur_param  = st_cur_param;
				next_param = st_next_param;
				reprocess = 0;
				goto reprocess_servers;
			}

			next_param = cur_param;
		}
	}

	if (total_servers == 0) {
		si->applet.ctx.stats.st_code = STAT_STATUS_NONE;
	}
	else if (altered_servers == 0) {
		si->applet.ctx.stats.st_code = STAT_STATUS_ERRP;
	}
	else if (altered_servers == total_servers) {
		si->applet.ctx.stats.st_code = STAT_STATUS_DONE;
	}
	else {
		si->applet.ctx.stats.st_code = STAT_STATUS_PART;
	}
 out:
	return 1;
}

/* This function checks whether we need to enable a POST analyser to parse a
 * stats request, and also registers the stats I/O handler. It returns zero
 * if it needs to come back again, otherwise non-zero if it finishes. In the
 * latter case, it also clears the request analysers.
 */
int http_handle_stats(struct session *s, struct channel *req)
{
	struct stats_admin_rule *stats_admin_rule;
	struct stream_interface *si = s->rep->prod;
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->req;
	struct uri_auth *uri = s->be->uri_auth;

	/* now check whether we have some admin rules for this request */
	list_for_each_entry(stats_admin_rule, &s->be->uri_auth->admin_rules, list) {
		int ret = 1;

		if (stats_admin_rule->cond) {
			ret = acl_exec_cond(stats_admin_rule->cond, s->be, s, &s->txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (stats_admin_rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			/* no rule, or the rule matches */
			s->rep->prod->applet.ctx.stats.flags |= STAT_ADMIN;
			break;
		}
	}

	/* Was the status page requested with a POST ? */
	if (unlikely(txn->meth == HTTP_METH_POST)) {
		if (si->applet.ctx.stats.flags & STAT_ADMIN) {
			if (msg->msg_state < HTTP_MSG_100_SENT) {
				/* If we have HTTP/1.1 and Expect: 100-continue, then we must
				 * send an HTTP/1.1 100 Continue intermediate response.
				 */
				if (msg->flags & HTTP_MSGF_VER_11) {
					struct hdr_ctx ctx;
					ctx.idx = 0;
					/* Expect is allowed in 1.1, look for it */
					if (http_find_header2("Expect", 6, req->buf->p, &txn->hdr_idx, &ctx) &&
					    unlikely(ctx.vlen == 12 && strncasecmp(ctx.line+ctx.val, "100-continue", 12) == 0)) {
						bo_inject(s->rep, http_100_chunk.str, http_100_chunk.len);
					}
				}
				msg->msg_state = HTTP_MSG_100_SENT;
				s->logs.tv_request = now;  /* update the request timer to reflect full request */
			}
			if (!http_process_req_stat_post(si, txn, req))
				return 0;   /* we need more data */
		}
		else
			si->applet.ctx.stats.st_code = STAT_STATUS_DENY;

		/* We don't want to land on the posted stats page because a refresh will
		 * repost the data. We don't want this to happen on accident so we redirect
		 * the browse to the stats page with a GET.
		 */
		chunk_printf(&trash,
		             "HTTP/1.0 303 See Other\r\n"
		             "Cache-Control: no-cache\r\n"
		             "Content-Type: text/plain\r\n"
		             "Connection: close\r\n"
		             "Location: %s;st=%s\r\n"
		             "\r\n",
		             uri->uri_prefix,
		             ((si->applet.ctx.stats.st_code > STAT_STATUS_INIT) &&
		              (si->applet.ctx.stats.st_code < STAT_STATUS_SIZE) &&
		              stat_status_codes[si->applet.ctx.stats.st_code]) ?
		             stat_status_codes[si->applet.ctx.stats.st_code] :
		             stat_status_codes[STAT_STATUS_UNKN]);

		s->txn.status = 303;
		s->logs.tv_request = now;
		stream_int_retnclose(req->prod, &trash);
		s->target = &http_stats_applet.obj_type; /* just for logging the applet name */

		if (s->fe == s->be) /* report it if the request was intercepted by the frontend */
			s->fe->fe_counters.intercepted_req++;

		if (!(s->flags & SN_ERR_MASK))      // this is not really an error but it is
			s->flags |= SN_ERR_PRXCOND; // to mark that it comes from the proxy
		if (!(s->flags & SN_FINST_MASK))
			s->flags |= SN_FINST_R;
		req->analysers = 0;
		return 1;
	}

	/* OK, let's go on now */

	chunk_printf(&trash,
	             "HTTP/1.0 200 OK\r\n"
	             "Cache-Control: no-cache\r\n"
	             "Connection: close\r\n"
	             "Content-Type: %s\r\n",
	             (si->applet.ctx.stats.flags & STAT_FMT_HTML) ? "text/html" : "text/plain");

	if (uri->refresh > 0 && !(si->applet.ctx.stats.flags & STAT_NO_REFRESH))
		chunk_appendf(&trash, "Refresh: %d\r\n",
		              uri->refresh);

	chunk_appendf(&trash, "\r\n");

	s->txn.status = 200;
	s->logs.tv_request = now;

	if (s->fe == s->be) /* report it if the request was intercepted by the frontend */
		s->fe->fe_counters.intercepted_req++;

	if (!(s->flags & SN_ERR_MASK))      // this is not really an error but it is
		s->flags |= SN_ERR_PRXCOND; // to mark that it comes from the proxy
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;

	if (s->txn.meth == HTTP_METH_HEAD) {
		/* that's all we return in case of HEAD request, so let's immediately close. */
		stream_int_retnclose(req->prod, &trash);
		s->target = &http_stats_applet.obj_type; /* just for logging the applet name */
		req->analysers = 0;
		return 1;
	}

	/* OK, push the response and hand over to the stats I/O handler */
	bi_putchk(s->rep, &trash);

	s->task->nice = -32; /* small boost for HTTP statistics */
	stream_int_register_handler(s->rep->prod, &http_stats_applet);
	s->target = s->rep->prod->conn->target; // for logging only
	s->rep->prod->conn->xprt_ctx = s;
	s->rep->prod->applet.st0 = s->rep->prod->applet.st1 = 0;
	req->analysers = 0;
	return 1;
}

/* Executes the http-request rules <rules> for session <s>, proxy <px> and
 * transaction <txn>. Returns the first rule that prevents further processing
 * of the request (auth, deny, ...) or NULL if it executed all rules or stopped
 * on an allow. It may set the TX_CLDENY on txn->flags if it encounters a deny
 * rule.
 */
static struct http_req_rule *
http_req_get_intercept_rule(struct proxy *px, struct list *rules, struct session *s, struct http_txn *txn)
{
	struct http_req_rule *rule;
	struct hdr_ctx ctx;

	list_for_each_entry(rule, rules, list) {
		if (rule->action >= HTTP_REQ_ACT_MAX)
			continue;

		/* check optional condition */
		if (rule->cond) {
			int ret;

			ret = acl_exec_cond(rule->cond, px, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);

			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;

			if (!ret) /* condition not matched */
				continue;
		}


		switch (rule->action) {
		case HTTP_REQ_ACT_ALLOW:
			return NULL; /* "allow" rules are OK */

		case HTTP_REQ_ACT_DENY:
			txn->flags |= TX_CLDENY;
			return rule;

		case HTTP_REQ_ACT_TARPIT:
			txn->flags |= TX_CLTARPIT;
			return rule;

		case HTTP_REQ_ACT_AUTH:
			return rule;

		case HTTP_REQ_ACT_REDIR:
			return rule;

		case HTTP_REQ_ACT_SET_HDR:
			ctx.idx = 0;
			/* remove all occurrences of the header */
			while (http_find_header2(rule->arg.hdr_add.name, rule->arg.hdr_add.name_len,
						 txn->req.chn->buf->p, &txn->hdr_idx, &ctx)) {
				http_remove_header2(&txn->req, &txn->hdr_idx, &ctx);
			}
			/* now fall through to header addition */

		case HTTP_REQ_ACT_ADD_HDR:
			chunk_printf(&trash, "%s: ", rule->arg.hdr_add.name);
			memcpy(trash.str, rule->arg.hdr_add.name, rule->arg.hdr_add.name_len);
			trash.len = rule->arg.hdr_add.name_len;
			trash.str[trash.len++] = ':';
			trash.str[trash.len++] = ' ';
			trash.len += build_logline(s, trash.str + trash.len, trash.size - trash.len, &rule->arg.hdr_add.fmt);
			http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, trash.len);
			break;
		}
	}

	/* we reached the end of the rules, nothing to report */
	return NULL;
}


/* Perform an HTTP redirect based on the information in <rule>. The function
 * returns non-zero on success, or zero in case of a, irrecoverable error such
 * as too large a request to build a valid response.
 */
static int http_apply_redirect_rule(struct redirect_rule *rule, struct session *s, struct http_txn *txn)
{
	struct http_msg *msg = &txn->req;
	const char *msg_fmt;

	/* build redirect message */
	switch(rule->code) {
	case 303:
		msg_fmt = HTTP_303;
		break;
	case 301:
		msg_fmt = HTTP_301;
		break;
	case 302:
	default:
		msg_fmt = HTTP_302;
		break;
	}

	if (unlikely(!chunk_strcpy(&trash, msg_fmt)))
		return 0;

	switch(rule->type) {
	case REDIRECT_TYPE_SCHEME: {
		const char *path;
		const char *host;
		struct hdr_ctx ctx;
		int pathlen;
		int hostlen;

		host = "";
		hostlen = 0;
		ctx.idx = 0;
		if (http_find_header2("Host", 4, txn->req.chn->buf->p + txn->req.sol, &txn->hdr_idx, &ctx)) {
			host = ctx.line + ctx.val;
			hostlen = ctx.vlen;
		}

		path = http_get_path(txn);
		/* build message using path */
		if (path) {
			pathlen = txn->req.sl.rq.u_l + (txn->req.chn->buf->p + txn->req.sl.rq.u) - path;
			if (rule->flags & REDIRECT_FLAG_DROP_QS) {
				int qs = 0;
				while (qs < pathlen) {
					if (path[qs] == '?') {
						pathlen = qs;
						break;
					}
					qs++;
				}
			}
		} else {
			path = "/";
			pathlen = 1;
		}

		/* check if we can add scheme + "://" + host + path */
		if (trash.len + rule->rdr_len + 3 + hostlen + pathlen > trash.size - 4)
			return 0;

		/* add scheme */
		memcpy(trash.str + trash.len, rule->rdr_str, rule->rdr_len);
		trash.len += rule->rdr_len;

		/* add "://" */
		memcpy(trash.str + trash.len, "://", 3);
		trash.len += 3;

		/* add host */
		memcpy(trash.str + trash.len, host, hostlen);
		trash.len += hostlen;

		/* add path */
		memcpy(trash.str + trash.len, path, pathlen);
		trash.len += pathlen;

		/* append a slash at the end of the location is needed and missing */
		if (trash.len && trash.str[trash.len - 1] != '/' &&
		    (rule->flags & REDIRECT_FLAG_APPEND_SLASH)) {
			if (trash.len > trash.size - 5)
				return 0;
			trash.str[trash.len] = '/';
			trash.len++;
		}

		break;
	}
	case REDIRECT_TYPE_PREFIX: {
		const char *path;
		int pathlen;

		path = http_get_path(txn);
		/* build message using path */
		if (path) {
			pathlen = txn->req.sl.rq.u_l + (txn->req.chn->buf->p + txn->req.sl.rq.u) - path;
			if (rule->flags & REDIRECT_FLAG_DROP_QS) {
				int qs = 0;
				while (qs < pathlen) {
					if (path[qs] == '?') {
						pathlen = qs;
						break;
					}
					qs++;
				}
			}
		} else {
			path = "/";
			pathlen = 1;
		}

		if (trash.len + rule->rdr_len + pathlen > trash.size - 4)
			return 0;

		/* add prefix. Note that if prefix == "/", we don't want to
		 * add anything, otherwise it makes it hard for the user to
		 * configure a self-redirection.
		 */
		if (rule->rdr_len != 1 || *rule->rdr_str != '/') {
			memcpy(trash.str + trash.len, rule->rdr_str, rule->rdr_len);
			trash.len += rule->rdr_len;
		}

		/* add path */
		memcpy(trash.str + trash.len, path, pathlen);
		trash.len += pathlen;

		/* append a slash at the end of the location is needed and missing */
		if (trash.len && trash.str[trash.len - 1] != '/' &&
		    (rule->flags & REDIRECT_FLAG_APPEND_SLASH)) {
			if (trash.len > trash.size - 5)
				return 0;
			trash.str[trash.len] = '/';
			trash.len++;
		}

		break;
	}
	case REDIRECT_TYPE_LOCATION:
	default:
		if (trash.len + rule->rdr_len > trash.size - 4)
			return 0;

		/* add location */
		memcpy(trash.str + trash.len, rule->rdr_str, rule->rdr_len);
		trash.len += rule->rdr_len;
		break;
	}

	if (rule->cookie_len) {
		memcpy(trash.str + trash.len, "\r\nSet-Cookie: ", 14);
		trash.len += 14;
		memcpy(trash.str + trash.len, rule->cookie_str, rule->cookie_len);
		trash.len += rule->cookie_len;
		memcpy(trash.str + trash.len, "\r\n", 2);
		trash.len += 2;
	}

	/* add end of headers and the keep-alive/close status.
	 * We may choose to set keep-alive if the Location begins
	 * with a slash, because the client will come back to the
	 * same server.
	 */
	txn->status = rule->code;
	/* let's log the request time */
	s->logs.tv_request = now;

	if (rule->rdr_len >= 1 && *rule->rdr_str == '/' &&
	    (msg->flags & HTTP_MSGF_XFER_LEN) &&
	    !(msg->flags & HTTP_MSGF_TE_CHNK) && !txn->req.body_len &&
	    ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL ||
	     (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL)) {
		/* keep-alive possible */
		if (!(msg->flags & HTTP_MSGF_VER_11)) {
			if (unlikely(txn->flags & TX_USE_PX_CONN)) {
				memcpy(trash.str + trash.len, "\r\nProxy-Connection: keep-alive", 30);
				trash.len += 30;
			} else {
				memcpy(trash.str + trash.len, "\r\nConnection: keep-alive", 24);
				trash.len += 24;
			}
		}
		memcpy(trash.str + trash.len, "\r\n\r\n", 4);
		trash.len += 4;
		bo_inject(txn->rsp.chn, trash.str, trash.len);
		/* "eat" the request */
		bi_fast_delete(txn->req.chn->buf, msg->sov);
		msg->sov = 0;
		txn->req.chn->analysers = AN_REQ_HTTP_XFER_BODY;
		s->rep->analysers = AN_RES_HTTP_XFER_BODY;
		txn->req.msg_state = HTTP_MSG_CLOSED;
		txn->rsp.msg_state = HTTP_MSG_DONE;
	} else {
		/* keep-alive not possible */
		if (unlikely(txn->flags & TX_USE_PX_CONN)) {
			memcpy(trash.str + trash.len, "\r\nProxy-Connection: close\r\n\r\n", 29);
			trash.len += 29;
		} else {
			memcpy(trash.str + trash.len, "\r\nConnection: close\r\n\r\n", 23);
			trash.len += 23;
		}
		stream_int_retnclose(txn->req.chn->prod, &trash);
		txn->req.chn->analysers = 0;
	}

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;

	return 1;
}

/* This stream analyser runs all HTTP request processing which is common to
 * frontends and backends, which means blocking ACLs, filters, connection-close,
 * reqadd, stats and redirects. This is performed for the designated proxy.
 * It returns 1 if the processing can continue on next analysers, or zero if it
 * either needs more data or wants to immediately abort the request (eg: deny,
 * error, ...).
 */
int http_process_req_common(struct session *s, struct channel *req, int an_bit, struct proxy *px)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->req;
	struct acl_cond *cond;
	struct http_req_rule *http_req_last_rule = NULL;
	struct redirect_rule *rule;
	struct cond_wordlist *wl;
	int do_stats;

	if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
		/* we need more data */
		channel_dont_connect(req);
		return 0;
	}

	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		req->buf->i,
		req->analysers);

	/* first check whether we have some ACLs set to block this request */
	list_for_each_entry(cond, &px->block_cond, list) {
		int ret = acl_exec_cond(cond, px, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);

		ret = acl_pass(ret);
		if (cond->pol == ACL_COND_UNLESS)
			ret = !ret;

		if (ret) {
			txn->status = 403;
			/* let's log the request time */
			s->logs.tv_request = now;
			stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_403));
			session_inc_http_err_ctr(s);
			goto return_prx_cond;
		}
	}

	/* just in case we have some per-backend tracking */
	session_inc_be_http_req_ctr(s);

	/* evaluate http-request rules */
	http_req_last_rule = http_req_get_intercept_rule(px, &px->http_req_rules, s, txn);

	/* evaluate stats http-request rules only if http-request is OK */
	if (!http_req_last_rule) {
		do_stats = stats_check_uri(s->rep->prod, txn, px);
		if (do_stats)
			http_req_last_rule = http_req_get_intercept_rule(px, &px->uri_auth->http_req_rules, s, txn);
	}
	else
		do_stats = 0;

	/* return a 403 if either rule has blocked */
	if (txn->flags & (TX_CLDENY|TX_CLTARPIT)) {
		if (txn->flags & TX_CLDENY) {
			txn->status = 403;
			s->logs.tv_request = now;
			stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_403));
			session_inc_http_err_ctr(s);
			s->fe->fe_counters.denied_req++;
			if (an_bit == AN_REQ_HTTP_PROCESS_BE)
				s->be->be_counters.denied_req++;
			if (s->listener->counters)
				s->listener->counters->denied_req++;
			goto return_prx_cond;
		}
		/* When a connection is tarpitted, we use the tarpit timeout,
		 * which may be the same as the connect timeout if unspecified.
		 * If unset, then set it to zero because we really want it to
		 * eventually expire. We build the tarpit as an analyser.
		 */
		if (txn->flags & TX_CLTARPIT) {
			channel_erase(s->req);
			/* wipe the request out so that we can drop the connection early
			 * if the client closes first.
			 */
			channel_dont_connect(req);
			req->analysers = 0; /* remove switching rules etc... */
			req->analysers |= AN_REQ_HTTP_TARPIT;
			req->analyse_exp = tick_add_ifset(now_ms,  s->be->timeout.tarpit);
			if (!req->analyse_exp)
				req->analyse_exp = tick_add(now_ms, 0);
			session_inc_http_err_ctr(s);
			s->fe->fe_counters.denied_req++;
			if (s->fe != s->be)
				s->be->be_counters.denied_req++;
			if (s->listener->counters)
				s->listener->counters->denied_req++;
			return 1;
		}
	}

	/* try headers filters */
	if (px->req_exp != NULL) {
		if (apply_filters_to_request(s, req, px) < 0)
			goto return_bad_req;

		/* has the request been denied ? */
		if (txn->flags & TX_CLDENY) {
			/* no need to go further */
			txn->status = 403;
			/* let's log the request time */
			s->logs.tv_request = now;
			stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_403));
			session_inc_http_err_ctr(s);
			goto return_prx_cond;
		}

		/* When a connection is tarpitted, we use the tarpit timeout,
		 * which may be the same as the connect timeout if unspecified.
		 * If unset, then set it to zero because we really want it to
		 * eventually expire. We build the tarpit as an analyser.
		 */
		if (txn->flags & TX_CLTARPIT) {
			channel_erase(s->req);
			/* wipe the request out so that we can drop the connection early
			 * if the client closes first.
			 */
			channel_dont_connect(req);
			req->analysers = 0; /* remove switching rules etc... */
			req->analysers |= AN_REQ_HTTP_TARPIT;
			req->analyse_exp = tick_add_ifset(now_ms,  s->be->timeout.tarpit);
			if (!req->analyse_exp)
				req->analyse_exp = tick_add(now_ms, 0);
			session_inc_http_err_ctr(s);
			return 1;
		}
	}

	/* Until set to anything else, the connection mode is set as TUNNEL. It will
	 * only change if both the request and the config reference something else.
	 * Option httpclose by itself does not set a mode, it remains a tunnel mode
	 * in which headers are mangled. However, if another mode is set, it will
	 * affect it (eg: server-close/keep-alive + httpclose = close). Note that we
	 * avoid to redo the same work if FE and BE have the same settings (common).
	 * The method consists in checking if options changed between the two calls
	 * (implying that either one is non-null, or one of them is non-null and we
	 * are there for the first time.
	 */

	if ((!(txn->flags & TX_HDR_CONN_PRS) &&
	     (s->fe->options & (PR_O_KEEPALIVE|PR_O_SERVER_CLO|PR_O_HTTP_CLOSE|PR_O_FORCE_CLO))) ||
	    ((s->fe->options & (PR_O_KEEPALIVE|PR_O_SERVER_CLO|PR_O_HTTP_CLOSE|PR_O_FORCE_CLO)) !=
	     (s->be->options & (PR_O_KEEPALIVE|PR_O_SERVER_CLO|PR_O_HTTP_CLOSE|PR_O_FORCE_CLO)))) {
		int tmp = TX_CON_WANT_TUN;

		if ((s->fe->options|s->be->options) & PR_O_KEEPALIVE ||
		    ((s->fe->options2|s->be->options2) & PR_O2_FAKE_KA))
			tmp = TX_CON_WANT_KAL;
		if ((s->fe->options|s->be->options) & PR_O_SERVER_CLO)
			tmp = TX_CON_WANT_SCL;
		if ((s->fe->options|s->be->options) & PR_O_FORCE_CLO)
			tmp = TX_CON_WANT_CLO;

		if ((txn->flags & TX_CON_WANT_MSK) < tmp)
			txn->flags = (txn->flags & ~TX_CON_WANT_MSK) | tmp;

		if (!(txn->flags & TX_HDR_CONN_PRS)) {
			/* parse the Connection header and possibly clean it */
			int to_del = 0;
			if ((msg->flags & HTTP_MSGF_VER_11) ||
			    ((txn->flags & TX_CON_WANT_MSK) >= TX_CON_WANT_SCL &&
			     !((s->fe->options2|s->be->options2) & PR_O2_FAKE_KA)))
				to_del |= 2; /* remove "keep-alive" */
			if (!(msg->flags & HTTP_MSGF_VER_11))
				to_del |= 1; /* remove "close" */
			http_parse_connection_header(txn, msg, to_del);
		}

		/* check if client or config asks for explicit close in KAL/SCL */
		if (((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL ||
		     (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL) &&
		    ((txn->flags & TX_HDR_CONN_CLO) ||                         /* "connection: close" */
		     (!(msg->flags & HTTP_MSGF_VER_11) && !(txn->flags & TX_HDR_CONN_KAL)) || /* no "connection: k-a" in 1.0 */
		     ((s->fe->options|s->be->options) & PR_O_HTTP_CLOSE) ||    /* httpclose+any = forceclose */
		     !(msg->flags & HTTP_MSGF_XFER_LEN) ||                     /* no length known => close */
		     s->fe->state == PR_STSTOPPED))                            /* frontend is stopping */
		    txn->flags = (txn->flags & ~TX_CON_WANT_MSK) | TX_CON_WANT_CLO;
	}

	/* we can be blocked here because the request needs to be authenticated,
	 * either to pass or to access stats.
	 */
	if (http_req_last_rule && http_req_last_rule->action == HTTP_REQ_ACT_AUTH) {
		char *realm = http_req_last_rule->arg.auth.realm;

		if (!realm)
			realm = do_stats?STATS_DEFAULT_REALM:px->id;

		chunk_printf(&trash, (txn->flags & TX_USE_PX_CONN) ? HTTP_407_fmt : HTTP_401_fmt, realm);
		txn->status = 401;
		stream_int_retnclose(req->prod, &trash);
		/* on 401 we still count one error, because normal browsing
		 * won't significantly increase the counter but brute force
		 * attempts will.
		 */
		session_inc_http_err_ctr(s);
		goto return_prx_cond;
	}

	/* add request headers from the rule sets in the same order */
	list_for_each_entry(wl, &px->req_add, list) {
		if (wl->cond) {
			int ret = acl_exec_cond(wl->cond, px, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (((struct acl_cond *)wl->cond)->pol == ACL_COND_UNLESS)
				ret = !ret;
			if (!ret)
				continue;
		}

		if (unlikely(http_header_add_tail(&txn->req, &txn->hdr_idx, wl->s) < 0))
			goto return_bad_req;
	}

	if (http_req_last_rule && http_req_last_rule->action == HTTP_REQ_ACT_REDIR) {
		if (!http_apply_redirect_rule(http_req_last_rule->arg.redir, s, txn))
			goto return_bad_req;
		req->analyse_exp = TICK_ETERNITY;
		return 1;
	}

	if (unlikely(do_stats)) {
		/* process the stats request now */
		if (!http_handle_stats(s, req)) {
			/* we need more data, let's come back here later */
			req->analysers |= an_bit;
			channel_dont_connect(req);
		}
		return 1;
	}

	/* check whether we have some ACLs set to redirect this request */
	list_for_each_entry(rule, &px->redirect_rules, list) {
		if (rule->cond) {
			int ret;

			ret = acl_exec_cond(rule->cond, px, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
			if (!ret)
				continue;
		}
		if (!http_apply_redirect_rule(rule, s, txn))
			goto return_bad_req;

		req->analyse_exp = TICK_ETERNITY;
		return 1;
	}

	/* POST requests may be accompanied with an "Expect: 100-Continue" header.
	 * If this happens, then the data will not come immediately, so we must
	 * send all what we have without waiting. Note that due to the small gain
	 * in waiting for the body of the request, it's easier to simply put the
	 * CF_SEND_DONTWAIT flag any time. It's a one-shot flag so it will remove
	 * itself once used.
	 */
	req->flags |= CF_SEND_DONTWAIT;

	/* that's OK for us now, let's move on to next analysers */
	return 1;

 return_bad_req:
	/* We centralize bad requests processing here */
	if (unlikely(msg->msg_state == HTTP_MSG_ERROR) || msg->err_pos >= 0) {
		/* we detected a parsing error. We want to archive this request
		 * in the dedicated proxy area for later troubleshooting.
		 */
		http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
	}

	txn->req.msg_state = HTTP_MSG_ERROR;
	txn->status = 400;
	stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));

	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;

 return_prx_cond:
	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;

	req->analysers = 0;
	req->analyse_exp = TICK_ETERNITY;
	return 0;
}

/* This function performs all the processing enabled for the current request.
 * It returns 1 if the processing can continue on next analysers, or zero if it
 * needs more data, encounters an error, or wants to immediately abort the
 * request. It relies on buffers flags, and updates s->req->analysers.
 */
int http_process_request(struct session *s, struct channel *req, int an_bit)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->req;

	if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
		/* we need more data */
		channel_dont_connect(req);
		return 0;
	}

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		req->buf->i,
		req->analysers);

	if (s->fe->comp || s->be->comp)
		select_compression_request_header(s, req->buf);

	/*
	 * Right now, we know that we have processed the entire headers
	 * and that unwanted requests have been filtered out. We can do
	 * whatever we want with the remaining request. Also, now we
	 * may have separate values for ->fe, ->be.
	 */

	/*
	 * If HTTP PROXY is set we simply get remote server address
	 * parsing incoming request.
	 */
	if ((s->be->options & PR_O_HTTP_PROXY) && !(s->flags & SN_ADDR_SET)) {
		url2sa(req->buf->p + msg->sl.rq.u, msg->sl.rq.u_l, &s->req->cons->conn->addr.to);
	}

	/*
	 * 7: Now we can work with the cookies.
	 * Note that doing so might move headers in the request, but
	 * the fields will stay coherent and the URI will not move.
	 * This should only be performed in the backend.
	 */
	if ((s->be->cookie_name || s->be->appsession_name || s->fe->capture_name)
	    && !(txn->flags & (TX_CLDENY|TX_CLTARPIT)))
		manage_client_side_cookies(s, req);

	/*
	 * 8: the appsession cookie was looked up very early in 1.2,
	 * so let's do the same now.
	 */

	/* It needs to look into the URI unless persistence must be ignored */
	if ((txn->sessid == NULL) && s->be->appsession_name && !(s->flags & SN_IGNORE_PRST)) {
		get_srv_from_appsession(s, req->buf->p + msg->sl.rq.u, msg->sl.rq.u_l);
	}

	/* add unique-id if "header-unique-id" is specified */

	if (!LIST_ISEMPTY(&s->fe->format_unique_id))
		build_logline(s, s->unique_id, UNIQUEID_LEN, &s->fe->format_unique_id);

	if (s->fe->header_unique_id && s->unique_id) {
		chunk_printf(&trash, "%s: %s", s->fe->header_unique_id, s->unique_id);
		if (trash.len < 0)
			goto return_bad_req;
		if (unlikely(http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, trash.len) < 0))
		   goto return_bad_req;
	}

	/*
	 * 9: add X-Forwarded-For if either the frontend or the backend
	 * asks for it.
	 */
	if ((s->fe->options | s->be->options) & PR_O_FWDFOR) {
		struct hdr_ctx ctx = { .idx = 0 };
		if (!((s->fe->options | s->be->options) & PR_O_FF_ALWAYS) &&
			http_find_header2(s->be->fwdfor_hdr_len ? s->be->fwdfor_hdr_name : s->fe->fwdfor_hdr_name,
			                  s->be->fwdfor_hdr_len ? s->be->fwdfor_hdr_len : s->fe->fwdfor_hdr_len,
			                  req->buf->p, &txn->hdr_idx, &ctx)) {
			/* The header is set to be added only if none is present
			 * and we found it, so don't do anything.
			 */
		}
		else if (s->req->prod->conn->addr.from.ss_family == AF_INET) {
			/* Add an X-Forwarded-For header unless the source IP is
			 * in the 'except' network range.
			 */
			if ((!s->fe->except_mask.s_addr ||
			     (((struct sockaddr_in *)&s->req->prod->conn->addr.from)->sin_addr.s_addr & s->fe->except_mask.s_addr)
			     != s->fe->except_net.s_addr) &&
			    (!s->be->except_mask.s_addr ||
			     (((struct sockaddr_in *)&s->req->prod->conn->addr.from)->sin_addr.s_addr & s->be->except_mask.s_addr)
			     != s->be->except_net.s_addr)) {
				int len;
				unsigned char *pn;
				pn = (unsigned char *)&((struct sockaddr_in *)&s->req->prod->conn->addr.from)->sin_addr;

				/* Note: we rely on the backend to get the header name to be used for
				 * x-forwarded-for, because the header is really meant for the backends.
				 * However, if the backend did not specify any option, we have to rely
				 * on the frontend's header name.
				 */
				if (s->be->fwdfor_hdr_len) {
					len = s->be->fwdfor_hdr_len;
					memcpy(trash.str, s->be->fwdfor_hdr_name, len);
				} else {
					len = s->fe->fwdfor_hdr_len;
					memcpy(trash.str, s->fe->fwdfor_hdr_name, len);
				}
				len += sprintf(trash.str + len, ": %d.%d.%d.%d", pn[0], pn[1], pn[2], pn[3]);

				if (unlikely(http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, len) < 0))
					goto return_bad_req;
			}
		}
		else if (s->req->prod->conn->addr.from.ss_family == AF_INET6) {
			/* FIXME: for the sake of completeness, we should also support
			 * 'except' here, although it is mostly useless in this case.
			 */
			int len;
			char pn[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6,
				  (const void *)&((struct sockaddr_in6 *)(&s->req->prod->conn->addr.from))->sin6_addr,
				  pn, sizeof(pn));

			/* Note: we rely on the backend to get the header name to be used for
			 * x-forwarded-for, because the header is really meant for the backends.
			 * However, if the backend did not specify any option, we have to rely
			 * on the frontend's header name.
			 */
			if (s->be->fwdfor_hdr_len) {
				len = s->be->fwdfor_hdr_len;
				memcpy(trash.str, s->be->fwdfor_hdr_name, len);
			} else {
				len = s->fe->fwdfor_hdr_len;
				memcpy(trash.str, s->fe->fwdfor_hdr_name, len);
			}
			len += sprintf(trash.str + len, ": %s", pn);

			if (unlikely(http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, len) < 0))
				goto return_bad_req;
		}
	}

	/*
	 * 10: add X-Original-To if either the frontend or the backend
	 * asks for it.
	 */
	if ((s->fe->options | s->be->options) & PR_O_ORGTO) {

		/* FIXME: don't know if IPv6 can handle that case too. */
		if (s->req->prod->conn->addr.from.ss_family == AF_INET) {
			/* Add an X-Original-To header unless the destination IP is
			 * in the 'except' network range.
			 */
			conn_get_to_addr(s->req->prod->conn);

			if (s->req->prod->conn->addr.to.ss_family == AF_INET &&
			    ((!s->fe->except_mask_to.s_addr ||
			      (((struct sockaddr_in *)&s->req->prod->conn->addr.to)->sin_addr.s_addr & s->fe->except_mask_to.s_addr)
			      != s->fe->except_to.s_addr) &&
			     (!s->be->except_mask_to.s_addr ||
			      (((struct sockaddr_in *)&s->req->prod->conn->addr.to)->sin_addr.s_addr & s->be->except_mask_to.s_addr)
			      != s->be->except_to.s_addr))) {
				int len;
				unsigned char *pn;
				pn = (unsigned char *)&((struct sockaddr_in *)&s->req->prod->conn->addr.to)->sin_addr;

				/* Note: we rely on the backend to get the header name to be used for
				 * x-original-to, because the header is really meant for the backends.
				 * However, if the backend did not specify any option, we have to rely
				 * on the frontend's header name.
				 */
				if (s->be->orgto_hdr_len) {
					len = s->be->orgto_hdr_len;
					memcpy(trash.str, s->be->orgto_hdr_name, len);
				} else {
					len = s->fe->orgto_hdr_len;
					memcpy(trash.str, s->fe->orgto_hdr_name, len);
				}
				len += sprintf(trash.str + len, ": %d.%d.%d.%d", pn[0], pn[1], pn[2], pn[3]);

				if (unlikely(http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, len) < 0))
					goto return_bad_req;
			}
		}
	}

	/* 11: add "Connection: close" or "Connection: keep-alive" if needed and not yet set.
	 * If an "Upgrade" token is found, the header is left untouched in order not to have
	 * to deal with some servers bugs : some of them fail an Upgrade if anything but
	 * "Upgrade" is present in the Connection header.
	 */
	if (!(txn->flags & TX_HDR_CONN_UPG) &&
	    (((txn->flags & TX_CON_WANT_MSK) != TX_CON_WANT_TUN) ||
	     ((s->fe->options|s->be->options) & PR_O_HTTP_CLOSE))) {
		unsigned int want_flags = 0;

		if (msg->flags & HTTP_MSGF_VER_11) {
			if (((txn->flags & TX_CON_WANT_MSK) >= TX_CON_WANT_SCL ||
			    ((s->fe->options|s->be->options) & PR_O_HTTP_CLOSE)) &&
			    !((s->fe->options2|s->be->options2) & PR_O2_FAKE_KA))
				want_flags |= TX_CON_CLO_SET;
		} else {
			if (((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL &&
			     !((s->fe->options|s->be->options) & PR_O_HTTP_CLOSE)) ||
			    ((s->fe->options2|s->be->options2) & PR_O2_FAKE_KA))
				want_flags |= TX_CON_KAL_SET;
		}

		if (want_flags != (txn->flags & (TX_CON_CLO_SET|TX_CON_KAL_SET)))
			http_change_connection_header(txn, msg, want_flags);
	}


	/* If we have no server assigned yet and we're balancing on url_param
	 * with a POST request, we may be interested in checking the body for
	 * that parameter. This will be done in another analyser.
	 */
	if (!(s->flags & (SN_ASSIGNED|SN_DIRECT)) &&
	    s->txn.meth == HTTP_METH_POST && s->be->url_param_name != NULL &&
	    s->be->url_param_post_limit != 0 &&
	    (msg->flags & (HTTP_MSGF_CNT_LEN|HTTP_MSGF_TE_CHNK))) {
		channel_dont_connect(req);
		req->analysers |= AN_REQ_HTTP_BODY;
	}

	if (msg->flags & HTTP_MSGF_XFER_LEN) {
		req->analysers |= AN_REQ_HTTP_XFER_BODY;
#ifdef TCP_QUICKACK
		/* We expect some data from the client. Unless we know for sure
		 * we already have a full request, we have to re-enable quick-ack
		 * in case we previously disabled it, otherwise we might cause
		 * the client to delay further data.
		 */
		if ((s->listener->options & LI_O_NOQUICKACK) &&
		    ((msg->flags & HTTP_MSGF_TE_CHNK) ||
		     (msg->body_len > req->buf->i - txn->req.eoh - 2)))
			setsockopt(s->si[0].conn->t.sock.fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
	}

	/*************************************************************
	 * OK, that's finished for the headers. We have done what we *
	 * could. Let's switch to the DATA state.                    *
	 ************************************************************/
	req->analyse_exp = TICK_ETERNITY;
	req->analysers &= ~an_bit;

	/* if the server closes the connection, we want to immediately react
	 * and close the socket to save packets and syscalls.
	 */
	if (!(req->analysers & AN_REQ_HTTP_XFER_BODY))
		req->cons->flags |= SI_FL_NOHALF;

	s->logs.tv_request = now;
	/* OK let's go on with the BODY now */
	return 1;

 return_bad_req: /* let's centralize all bad requests */
	if (unlikely(msg->msg_state == HTTP_MSG_ERROR) || msg->err_pos >= 0) {
		/* we detected a parsing error. We want to archive this request
		 * in the dedicated proxy area for later troubleshooting.
		 */
		http_capture_bad_message(&s->fe->invalid_req, s, msg, msg->msg_state, s->fe);
	}

	txn->req.msg_state = HTTP_MSG_ERROR;
	txn->status = 400;
	req->analysers = 0;
	stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));

	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;
	return 0;
}

/* This function is an analyser which processes the HTTP tarpit. It always
 * returns zero, at the beginning because it prevents any other processing
 * from occurring, and at the end because it terminates the request.
 */
int http_process_tarpit(struct session *s, struct channel *req, int an_bit)
{
	struct http_txn *txn = &s->txn;

	/* This connection is being tarpitted. The CLIENT side has
	 * already set the connect expiration date to the right
	 * timeout. We just have to check that the client is still
	 * there and that the timeout has not expired.
	 */
	channel_dont_connect(req);
	if ((req->flags & (CF_SHUTR|CF_READ_ERROR)) == 0 &&
	    !tick_is_expired(req->analyse_exp, now_ms))
		return 0;

	/* We will set the queue timer to the time spent, just for
	 * logging purposes. We fake a 500 server error, so that the
	 * attacker will not suspect his connection has been tarpitted.
	 * It will not cause trouble to the logs because we can exclude
	 * the tarpitted connections by filtering on the 'PT' status flags.
	 */
	s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);

	txn->status = 500;
	if (!(req->flags & CF_READ_ERROR))
		stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_500));

	req->analysers = 0;
	req->analyse_exp = TICK_ETERNITY;

	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_T;
	return 0;
}

/* This function is an analyser which processes the HTTP request body. It looks
 * for parameters to be used for the load balancing algorithm (url_param). It
 * must only be called after the standard HTTP request processing has occurred,
 * because it expects the request to be parsed. It returns zero if it needs to
 * read more data, or 1 once it has completed its analysis.
 */
int http_process_request_body(struct session *s, struct channel *req, int an_bit)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &s->txn.req;
	long long limit = s->be->url_param_post_limit;

	/* We have to parse the HTTP request body to find any required data.
	 * "balance url_param check_post" should have been the only way to get
	 * into this. We were brought here after HTTP header analysis, so all
	 * related structures are ready.
	 */

	if (unlikely(msg->msg_state < HTTP_MSG_BODY))
		goto missing_data;

	if (msg->msg_state < HTTP_MSG_100_SENT) {
		/* If we have HTTP/1.1 and Expect: 100-continue, then we must
		 * send an HTTP/1.1 100 Continue intermediate response.
		 */
		if (msg->flags & HTTP_MSGF_VER_11) {
			struct hdr_ctx ctx;
			ctx.idx = 0;
			/* Expect is allowed in 1.1, look for it */
			if (http_find_header2("Expect", 6, req->buf->p, &txn->hdr_idx, &ctx) &&
			    unlikely(ctx.vlen == 12 && strncasecmp(ctx.line+ctx.val, "100-continue", 12) == 0)) {
				bo_inject(s->rep, http_100_chunk.str, http_100_chunk.len);
			}
		}
		msg->msg_state = HTTP_MSG_100_SENT;
	}

	if (msg->msg_state < HTTP_MSG_CHUNK_SIZE) {
		/* we have msg->sov which points to the first byte of message body.
		 * req->buf->p still points to the beginning of the message and msg->sol
		 * is still null. We must save the body in msg->next because it
		 * survives buffer re-alignments.
		 */
		msg->next = msg->sov;

		if (msg->flags & HTTP_MSGF_TE_CHNK)
			msg->msg_state = HTTP_MSG_CHUNK_SIZE;
		else
			msg->msg_state = HTTP_MSG_DATA;
	}

	if (msg->msg_state == HTTP_MSG_CHUNK_SIZE) {
		/* read the chunk size and assign it to ->chunk_len, then
		 * set ->sov and ->next to point to the body and switch to DATA or
		 * TRAILERS state.
		 */
		int ret = http_parse_chunk_size(msg);

		if (!ret)
			goto missing_data;
		else if (ret < 0) {
			session_inc_http_err_ctr(s);
			goto return_bad_req;
		}
	}

	/* Now we're in HTTP_MSG_DATA or HTTP_MSG_TRAILERS state.
	 * We have the first data byte is in msg->sov. We're waiting for at
	 * least <url_param_post_limit> bytes after msg->sov.
	 */

	if (msg->body_len < limit)
		limit = msg->body_len;

	if (req->buf->i - msg->sov >= limit)    /* we have enough bytes now */
		goto http_end;

 missing_data:
	/* we get here if we need to wait for more data */
	if (buffer_full(req->buf, global.tune.maxrewrite)) {
		session_inc_http_err_ctr(s);
		goto return_bad_req;
	}

	if ((req->flags & CF_READ_TIMEOUT) || tick_is_expired(req->analyse_exp, now_ms)) {
		txn->status = 408;
		stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_408));

		if (!(s->flags & SN_ERR_MASK))
			s->flags |= SN_ERR_CLITO;
		if (!(s->flags & SN_FINST_MASK))
			s->flags |= SN_FINST_D;
		goto return_err_msg;
	}

	/* we get here if we need to wait for more data */
	if (!(req->flags & (CF_SHUTR | CF_READ_ERROR)) && !buffer_full(req->buf, global.tune.maxrewrite)) {
		/* Not enough data. We'll re-use the http-request
		 * timeout here. Ideally, we should set the timeout
		 * relative to the accept() date. We just set the
		 * request timeout once at the beginning of the
		 * request.
		 */
		channel_dont_connect(req);
		if (!tick_isset(req->analyse_exp))
			req->analyse_exp = tick_add_ifset(now_ms, s->be->timeout.httpreq);
		return 0;
	}

 http_end:
	/* The situation will not evolve, so let's give up on the analysis. */
	s->logs.tv_request = now;  /* update the request timer to reflect full request */
	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;
	return 1;

 return_bad_req: /* let's centralize all bad requests */
	txn->req.msg_state = HTTP_MSG_ERROR;
	txn->status = 400;
	stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_R;

 return_err_msg:
	req->analysers = 0;
	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;
	return 0;
}

/* send a server's name with an outgoing request over an established connection.
 * Note: this function is designed to be called once the request has been scheduled
 * for being forwarded. This is the reason why it rewinds the buffer before
 * proceeding.
 */
int http_send_name_header(struct http_txn *txn, struct proxy* be, const char* srv_name) {

	struct hdr_ctx ctx;

	char *hdr_name = be->server_id_hdr_name;
	int hdr_name_len = be->server_id_hdr_len;
	struct channel *chn = txn->req.chn;
	char *hdr_val;
	unsigned int old_o, old_i;

	ctx.idx = 0;

	old_o = chn->buf->o;
	if (old_o) {
		/* The request was already skipped, let's restore it */
		b_rew(chn->buf, old_o);
	}

	old_i = chn->buf->i;
	while (http_find_header2(hdr_name, hdr_name_len, txn->req.chn->buf->p, &txn->hdr_idx, &ctx)) {
		/* remove any existing values from the header */
	        http_remove_header2(&txn->req, &txn->hdr_idx, &ctx);
	}

	/* Add the new header requested with the server value */
	hdr_val = trash.str;
	memcpy(hdr_val, hdr_name, hdr_name_len);
	hdr_val += hdr_name_len;
	*hdr_val++ = ':';
	*hdr_val++ = ' ';
	hdr_val += strlcpy2(hdr_val, srv_name, trash.str + trash.size - hdr_val);
	http_header_add_tail2(&txn->req, &txn->hdr_idx, trash.str, hdr_val - trash.str);

	if (old_o) {
		/* If this was a forwarded request, we must readjust the amount of
		 * data to be forwarded in order to take into account the size
		 * variations.
		 */
		b_adv(chn->buf, old_o + chn->buf->i - old_i);
	}

	return 0;
}

/* Terminate current transaction and prepare a new one. This is very tricky
 * right now but it works.
 */
void http_end_txn_clean_session(struct session *s)
{
	/* FIXME: We need a more portable way of releasing a backend's and a
	 * server's connections. We need a safer way to reinitialize buffer
	 * flags. We also need a more accurate method for computing per-request
	 * data.
	 */
	http_silent_debug(__LINE__, s);

	s->req->cons->flags |= SI_FL_NOLINGER | SI_FL_NOHALF;
	si_shutr(s->req->cons);
	si_shutw(s->req->cons);

	http_silent_debug(__LINE__, s);

	if (s->flags & SN_BE_ASSIGNED) {
		s->be->beconn--;
		if (unlikely(s->srv_conn))
			sess_change_server(s, NULL);
	}

	s->logs.t_close = tv_ms_elapsed(&s->logs.tv_accept, &now);
	session_process_counters(s);
	session_stop_backend_counters(s);

	if (s->txn.status) {
		int n;

		n = s->txn.status / 100;
		if (n < 1 || n > 5)
			n = 0;

		if (s->fe->mode == PR_MODE_HTTP) {
			s->fe->fe_counters.p.http.rsp[n]++;
			if (s->comp_algo && (s->flags & SN_COMP_READY))
				s->fe->fe_counters.p.http.comp_rsp++;
		}
		if ((s->flags & SN_BE_ASSIGNED) &&
		    (s->be->mode == PR_MODE_HTTP)) {
			s->be->be_counters.p.http.rsp[n]++;
			s->be->be_counters.p.http.cum_req++;
			if (s->comp_algo && (s->flags & SN_COMP_READY))
				s->be->be_counters.p.http.comp_rsp++;
		}
	}

	/* don't count other requests' data */
	s->logs.bytes_in  -= s->req->buf->i;
	s->logs.bytes_out -= s->rep->buf->i;

	/* let's do a final log if we need it */
	if (!LIST_ISEMPTY(&s->fe->logformat) && s->logs.logwait &&
	    !(s->flags & SN_MONITOR) &&
	    (!(s->fe->options & PR_O_NULLNOLOG) || s->req->total)) {
		s->do_log(s);
	}

	s->logs.accept_date = date; /* user-visible date for logging */
	s->logs.tv_accept = now;  /* corrected date for internal use */
	tv_zero(&s->logs.tv_request);
	s->logs.t_queue = -1;
	s->logs.t_connect = -1;
	s->logs.t_data = -1;
	s->logs.t_close = 0;
	s->logs.prx_queue_size = 0;  /* we get the number of pending conns before us */
	s->logs.srv_queue_size = 0; /* we will get this number soon */

	s->logs.bytes_in = s->req->total = s->req->buf->i;
	s->logs.bytes_out = s->rep->total = s->rep->buf->i;

	if (s->pend_pos)
		pendconn_free(s->pend_pos);

	if (objt_server(s->target)) {
		if (s->flags & SN_CURR_SESS) {
			s->flags &= ~SN_CURR_SESS;
			objt_server(s->target)->cur_sess--;
		}
		if (may_dequeue_tasks(objt_server(s->target), s->be))
			process_srv_queue(objt_server(s->target));
	}

	s->target = NULL;

	s->req->cons->state     = s->req->cons->prev_state = SI_ST_INI;
	s->req->cons->conn->t.sock.fd = -1; /* just to help with debugging */
	s->req->cons->conn->flags = CO_FL_NONE;
	s->req->cons->conn->err_code = CO_ER_NONE;
	s->req->cons->err_type  = SI_ET_NONE;
	s->req->cons->conn_retries = 0;  /* used for logging too */
	s->req->cons->err_loc   = NULL;
	s->req->cons->exp       = TICK_ETERNITY;
	s->req->cons->flags     = SI_FL_NONE;
	s->req->flags &= ~(CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CONNECT|CF_WRITE_ERROR|CF_STREAMER|CF_STREAMER_FAST|CF_NEVER_WAIT);
	s->rep->flags &= ~(CF_SHUTR|CF_SHUTR_NOW|CF_READ_ATTACHED|CF_READ_ERROR|CF_READ_NOEXP|CF_STREAMER|CF_STREAMER_FAST|CF_WRITE_PARTIAL|CF_NEVER_WAIT);
	s->flags &= ~(SN_DIRECT|SN_ASSIGNED|SN_ADDR_SET|SN_BE_ASSIGNED|SN_FORCE_PRST|SN_IGNORE_PRST);
	s->flags &= ~(SN_CURR_SESS|SN_REDIRECTABLE);

	if (s->flags & SN_COMP_READY)
		s->comp_algo->end(&s->comp_ctx);
	s->comp_algo = NULL;
	s->flags &= ~SN_COMP_READY;

	s->txn.meth = 0;
	http_reset_txn(s);
	s->txn.flags |= TX_NOT_FIRST | TX_WAIT_NEXT_RQ;
	if (s->fe->options2 & PR_O2_INDEPSTR)
		s->req->cons->flags |= SI_FL_INDEP_STR;

	if (s->fe->options2 & PR_O2_NODELAY) {
		s->req->flags |= CF_NEVER_WAIT;
		s->rep->flags |= CF_NEVER_WAIT;
	}

	/* if the request buffer is not empty, it means we're
	 * about to process another request, so send pending
	 * data with MSG_MORE to merge TCP packets when possible.
	 * Just don't do this if the buffer is close to be full,
	 * because the request will wait for it to flush a little
	 * bit before proceeding.
	 */
	if (s->req->buf->i) {
		if (s->rep->buf->o &&
		    !buffer_full(s->rep->buf, global.tune.maxrewrite) &&
		    bi_end(s->rep->buf) <= s->rep->buf->data + s->rep->buf->size - global.tune.maxrewrite)
			s->rep->flags |= CF_EXPECT_MORE;
	}

	/* we're removing the analysers, we MUST re-enable events detection */
	channel_auto_read(s->req);
	channel_auto_close(s->req);
	channel_auto_read(s->rep);
	channel_auto_close(s->rep);

	s->req->analysers = s->listener->analysers;
	s->rep->analysers = 0;

	http_silent_debug(__LINE__, s);
}


/* This function updates the request state machine according to the response
 * state machine and buffer flags. It returns 1 if it changes anything (flag
 * or state), otherwise zero. It ignores any state before HTTP_MSG_DONE, as
 * it is only used to find when a request/response couple is complete. Both
 * this function and its equivalent should loop until both return zero. It
 * can set its own state to DONE, CLOSING, CLOSED, TUNNEL, ERROR.
 */
int http_sync_req_state(struct session *s)
{
	struct channel *chn = s->req;
	struct http_txn *txn = &s->txn;
	unsigned int old_flags = chn->flags;
	unsigned int old_state = txn->req.msg_state;

	http_silent_debug(__LINE__, s);
	if (unlikely(txn->req.msg_state < HTTP_MSG_BODY))
		return 0;

	if (txn->req.msg_state == HTTP_MSG_DONE) {
		/* No need to read anymore, the request was completely parsed.
		 * We can shut the read side unless we want to abort_on_close,
		 * or we have a POST request. The issue with POST requests is
		 * that some browsers still send a CRLF after the request, and
		 * this CRLF must be read so that it does not remain in the kernel
		 * buffers, otherwise a close could cause an RST on some systems
		 * (eg: Linux).
		 */
		if (!(s->be->options & PR_O_ABRT_CLOSE) && txn->meth != HTTP_METH_POST)
			channel_dont_read(chn);

		/* if the server closes the connection, we want to immediately react
		 * and close the socket to save packets and syscalls.
		 */
		chn->cons->flags |= SI_FL_NOHALF;

		if (txn->rsp.msg_state == HTTP_MSG_ERROR)
			goto wait_other_side;

		if (txn->rsp.msg_state < HTTP_MSG_DONE) {
			/* The server has not finished to respond, so we
			 * don't want to move in order not to upset it.
			 */
			goto wait_other_side;
		}

		if (txn->rsp.msg_state == HTTP_MSG_TUNNEL) {
			/* if any side switches to tunnel mode, the other one does too */
			channel_auto_read(chn);
			txn->req.msg_state = HTTP_MSG_TUNNEL;
			chn->flags |= CF_NEVER_WAIT;
			goto wait_other_side;
		}

		/* When we get here, it means that both the request and the
		 * response have finished receiving. Depending on the connection
		 * mode, we'll have to wait for the last bytes to leave in either
		 * direction, and sometimes for a close to be effective.
		 */

		if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL) {
			/* Server-close mode : queue a connection close to the server */
			if (!(chn->flags & (CF_SHUTW|CF_SHUTW_NOW)))
				channel_shutw_now(chn);
		}
		else if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_CLO) {
			/* Option forceclose is set, or either side wants to close,
			 * let's enforce it now that we're not expecting any new
			 * data to come. The caller knows the session is complete
			 * once both states are CLOSED.
			 */
			if (!(chn->flags & (CF_SHUTW|CF_SHUTW_NOW))) {
				channel_shutr_now(chn);
				channel_shutw_now(chn);
			}
		}
		else {
			/* The last possible modes are keep-alive and tunnel. Since tunnel
			 * mode does not set the body analyser, we can't reach this place
			 * in tunnel mode, so we're left with keep-alive only.
			 * This mode is currently not implemented, we switch to tunnel mode.
			 */
			channel_auto_read(chn);
			txn->req.msg_state = HTTP_MSG_TUNNEL;
			chn->flags |= CF_NEVER_WAIT;
		}

		if (chn->flags & (CF_SHUTW|CF_SHUTW_NOW)) {
			/* if we've just closed an output, let's switch */
			chn->cons->flags |= SI_FL_NOLINGER;  /* we want to close ASAP */

			if (!channel_is_empty(chn)) {
				txn->req.msg_state = HTTP_MSG_CLOSING;
				goto http_msg_closing;
			}
			else {
				txn->req.msg_state = HTTP_MSG_CLOSED;
				goto http_msg_closed;
			}
		}
		goto wait_other_side;
	}

	if (txn->req.msg_state == HTTP_MSG_CLOSING) {
	http_msg_closing:
		/* nothing else to forward, just waiting for the output buffer
		 * to be empty and for the shutw_now to take effect.
		 */
		if (channel_is_empty(chn)) {
			txn->req.msg_state = HTTP_MSG_CLOSED;
			goto http_msg_closed;
		}
		else if (chn->flags & CF_SHUTW) {
			txn->req.msg_state = HTTP_MSG_ERROR;
			goto wait_other_side;
		}
	}

	if (txn->req.msg_state == HTTP_MSG_CLOSED) {
	http_msg_closed:
		goto wait_other_side;
	}

 wait_other_side:
	http_silent_debug(__LINE__, s);
	return txn->req.msg_state != old_state || chn->flags != old_flags;
}


/* This function updates the response state machine according to the request
 * state machine and buffer flags. It returns 1 if it changes anything (flag
 * or state), otherwise zero. It ignores any state before HTTP_MSG_DONE, as
 * it is only used to find when a request/response couple is complete. Both
 * this function and its equivalent should loop until both return zero. It
 * can set its own state to DONE, CLOSING, CLOSED, TUNNEL, ERROR.
 */
int http_sync_res_state(struct session *s)
{
	struct channel *chn = s->rep;
	struct http_txn *txn = &s->txn;
	unsigned int old_flags = chn->flags;
	unsigned int old_state = txn->rsp.msg_state;

	http_silent_debug(__LINE__, s);
	if (unlikely(txn->rsp.msg_state < HTTP_MSG_BODY))
		return 0;

	if (txn->rsp.msg_state == HTTP_MSG_DONE) {
		/* In theory, we don't need to read anymore, but we must
		 * still monitor the server connection for a possible close
		 * while the request is being uploaded, so we don't disable
		 * reading.
		 */
		/* channel_dont_read(chn); */

		if (txn->req.msg_state == HTTP_MSG_ERROR)
			goto wait_other_side;

		if (txn->req.msg_state < HTTP_MSG_DONE) {
			/* The client seems to still be sending data, probably
			 * because we got an error response during an upload.
			 * We have the choice of either breaking the connection
			 * or letting it pass through. Let's do the later.
			 */
			goto wait_other_side;
		}

		if (txn->req.msg_state == HTTP_MSG_TUNNEL) {
			/* if any side switches to tunnel mode, the other one does too */
			channel_auto_read(chn);
			txn->rsp.msg_state = HTTP_MSG_TUNNEL;
			chn->flags |= CF_NEVER_WAIT;
			goto wait_other_side;
		}

		/* When we get here, it means that both the request and the
		 * response have finished receiving. Depending on the connection
		 * mode, we'll have to wait for the last bytes to leave in either
		 * direction, and sometimes for a close to be effective.
		 */

		if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL) {
			/* Server-close mode : shut read and wait for the request
			 * side to close its output buffer. The caller will detect
			 * when we're in DONE and the other is in CLOSED and will
			 * catch that for the final cleanup.
			 */
			if (!(chn->flags & (CF_SHUTR|CF_SHUTR_NOW)))
				channel_shutr_now(chn);
		}
		else if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_CLO) {
			/* Option forceclose is set, or either side wants to close,
			 * let's enforce it now that we're not expecting any new
			 * data to come. The caller knows the session is complete
			 * once both states are CLOSED.
			 */
			if (!(chn->flags & (CF_SHUTW|CF_SHUTW_NOW))) {
				channel_shutr_now(chn);
				channel_shutw_now(chn);
			}
		}
		else {
			/* The last possible modes are keep-alive and tunnel. Since tunnel
			 * mode does not set the body analyser, we can't reach this place
			 * in tunnel mode, so we're left with keep-alive only.
			 * This mode is currently not implemented, we switch to tunnel mode.
			 */
			channel_auto_read(chn);
			txn->rsp.msg_state = HTTP_MSG_TUNNEL;
			chn->flags |= CF_NEVER_WAIT;
		}

		if (chn->flags & (CF_SHUTW|CF_SHUTW_NOW)) {
			/* if we've just closed an output, let's switch */
			if (!channel_is_empty(chn)) {
				txn->rsp.msg_state = HTTP_MSG_CLOSING;
				goto http_msg_closing;
			}
			else {
				txn->rsp.msg_state = HTTP_MSG_CLOSED;
				goto http_msg_closed;
			}
		}
		goto wait_other_side;
	}

	if (txn->rsp.msg_state == HTTP_MSG_CLOSING) {
	http_msg_closing:
		/* nothing else to forward, just waiting for the output buffer
		 * to be empty and for the shutw_now to take effect.
		 */
		if (channel_is_empty(chn)) {
			txn->rsp.msg_state = HTTP_MSG_CLOSED;
			goto http_msg_closed;
		}
		else if (chn->flags & CF_SHUTW) {
			txn->rsp.msg_state = HTTP_MSG_ERROR;
			s->be->be_counters.cli_aborts++;
			if (objt_server(s->target))
				objt_server(s->target)->counters.cli_aborts++;
			goto wait_other_side;
		}
	}

	if (txn->rsp.msg_state == HTTP_MSG_CLOSED) {
	http_msg_closed:
		/* drop any pending data */
		bi_erase(chn);
		channel_auto_close(chn);
		channel_auto_read(chn);
		goto wait_other_side;
	}

 wait_other_side:
	http_silent_debug(__LINE__, s);
	/* We force the response to leave immediately if we're waiting for the
	 * other side, since there is no pending shutdown to push it out.
	 */
	if (!channel_is_empty(chn))
		chn->flags |= CF_SEND_DONTWAIT;
	return txn->rsp.msg_state != old_state || chn->flags != old_flags;
}


/* Resync the request and response state machines. Return 1 if either state
 * changes.
 */
int http_resync_states(struct session *s)
{
	struct http_txn *txn = &s->txn;
	int old_req_state = txn->req.msg_state;
	int old_res_state = txn->rsp.msg_state;

	http_silent_debug(__LINE__, s);
	http_sync_req_state(s);
	while (1) {
		http_silent_debug(__LINE__, s);
		if (!http_sync_res_state(s))
			break;
		http_silent_debug(__LINE__, s);
		if (!http_sync_req_state(s))
			break;
	}
	http_silent_debug(__LINE__, s);
	/* OK, both state machines agree on a compatible state.
	 * There are a few cases we're interested in :
	 *  - HTTP_MSG_TUNNEL on either means we have to disable both analysers
	 *  - HTTP_MSG_CLOSED on both sides means we've reached the end in both
	 *    directions, so let's simply disable both analysers.
	 *  - HTTP_MSG_CLOSED on the response only means we must abort the
	 *    request.
	 *  - HTTP_MSG_CLOSED on the request and HTTP_MSG_DONE on the response
	 *    with server-close mode means we've completed one request and we
	 *    must re-initialize the server connection.
	 */

	if (txn->req.msg_state == HTTP_MSG_TUNNEL ||
	    txn->rsp.msg_state == HTTP_MSG_TUNNEL ||
	    (txn->req.msg_state == HTTP_MSG_CLOSED &&
	     txn->rsp.msg_state == HTTP_MSG_CLOSED)) {
		s->req->analysers = 0;
		channel_auto_close(s->req);
		channel_auto_read(s->req);
		s->rep->analysers = 0;
		channel_auto_close(s->rep);
		channel_auto_read(s->rep);
	}
	else if ((txn->req.msg_state >= HTTP_MSG_DONE &&
		  (txn->rsp.msg_state == HTTP_MSG_CLOSED || (s->rep->flags & CF_SHUTW))) ||
		 txn->rsp.msg_state == HTTP_MSG_ERROR ||
		 txn->req.msg_state == HTTP_MSG_ERROR) {
		s->rep->analysers = 0;
		channel_auto_close(s->rep);
		channel_auto_read(s->rep);
		s->req->analysers = 0;
		channel_abort(s->req);
		channel_auto_close(s->req);
		channel_auto_read(s->req);
		bi_erase(s->req);
	}
	else if (txn->req.msg_state == HTTP_MSG_CLOSED &&
		 txn->rsp.msg_state == HTTP_MSG_DONE &&
		 ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL)) {
		/* server-close: terminate this server connection and
		 * reinitialize a fresh-new transaction.
		 */
		http_end_txn_clean_session(s);
	}

	http_silent_debug(__LINE__, s);
	return txn->req.msg_state != old_req_state ||
		txn->rsp.msg_state != old_res_state;
}

/* This function is an analyser which forwards request body (including chunk
 * sizes if any). It is called as soon as we must forward, even if we forward
 * zero byte. The only situation where it must not be called is when we're in
 * tunnel mode and we want to forward till the close. It's used both to forward
 * remaining data and to resync after end of body. It expects the msg_state to
 * be between MSG_BODY and MSG_DONE (inclusive). It returns zero if it needs to
 * read more data, or 1 once we can go on with next request or end the session.
 * When in MSG_DATA or MSG_TRAILERS, it will automatically forward chunk_len
 * bytes of pending data + the headers if not already done (between sol and sov).
 * It eventually adjusts sol to match sov after the data in between have been sent.
 */
int http_request_forward_body(struct session *s, struct channel *req, int an_bit)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &s->txn.req;

	if (unlikely(msg->msg_state < HTTP_MSG_BODY))
		return 0;

	if ((req->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) ||
	    ((req->flags & CF_SHUTW) && (req->to_forward || req->buf->o))) {
		/* Output closed while we were sending data. We must abort and
		 * wake the other side up.
		 */
		msg->msg_state = HTTP_MSG_ERROR;
		http_resync_states(s);
		return 1;
	}

	/* in most states, we should abort in case of early close */
	channel_auto_close(req);

	/* Note that we don't have to send 100-continue back because we don't
	 * need the data to complete our job, and it's up to the server to
	 * decide whether to return 100, 417 or anything else in return of
	 * an "Expect: 100-continue" header.
	 */

	if (msg->msg_state < HTTP_MSG_CHUNK_SIZE) {
		/* we have msg->sov which points to the first byte of message body.
		 * req->buf->p still points to the beginning of the message and msg->sol
		 * is still null. We must save the body in msg->next because it
		 * survives buffer re-alignments.
		 */
		msg->next = msg->sov;

		if (msg->flags & HTTP_MSGF_TE_CHNK)
			msg->msg_state = HTTP_MSG_CHUNK_SIZE;
		else
			msg->msg_state = HTTP_MSG_DATA;
	}

	while (1) {
		unsigned int bytes;

		http_silent_debug(__LINE__, s);
		/* we may have some data pending between sol and sov */
		bytes = msg->sov - msg->sol;
		if (msg->chunk_len || bytes) {
			msg->sol = msg->sov;
			msg->next -= bytes; /* will be forwarded */
			msg->chunk_len += bytes;
			msg->chunk_len -= channel_forward(req, msg->chunk_len);
		}

		if (msg->msg_state == HTTP_MSG_DATA) {
			/* must still forward */
			if (req->to_forward)
				goto missing_data;

			/* nothing left to forward */
			if (msg->flags & HTTP_MSGF_TE_CHNK)
				msg->msg_state = HTTP_MSG_CHUNK_CRLF;
			else
				msg->msg_state = HTTP_MSG_DONE;
		}
		else if (msg->msg_state == HTTP_MSG_CHUNK_SIZE) {
			/* read the chunk size and assign it to ->chunk_len, then
			 * set ->sov and ->next to point to the body and switch to DATA or
			 * TRAILERS state.
			 */
			int ret = http_parse_chunk_size(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				session_inc_http_err_ctr(s);
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->fe->invalid_req, s, msg, HTTP_MSG_CHUNK_SIZE, s->be);
				goto return_bad_req;
			}
			/* otherwise we're in HTTP_MSG_DATA or HTTP_MSG_TRAILERS state */
		}
		else if (msg->msg_state == HTTP_MSG_CHUNK_CRLF) {
			/* we want the CRLF after the data */
			int ret = http_skip_chunk_crlf(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				session_inc_http_err_ctr(s);
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->fe->invalid_req, s, msg, HTTP_MSG_CHUNK_CRLF, s->be);
				goto return_bad_req;
			}
			/* we're in MSG_CHUNK_SIZE now */
		}
		else if (msg->msg_state == HTTP_MSG_TRAILERS) {
			int ret = http_forward_trailers(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				session_inc_http_err_ctr(s);
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->fe->invalid_req, s, msg, HTTP_MSG_TRAILERS, s->be);
				goto return_bad_req;
			}
			/* we're in HTTP_MSG_DONE now */
		}
		else {
			int old_state = msg->msg_state;

			/* other states, DONE...TUNNEL */
			/* for keep-alive we don't want to forward closes on DONE */
			if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL ||
			    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL)
				channel_dont_close(req);
			if (http_resync_states(s)) {
				/* some state changes occurred, maybe the analyser
				 * was disabled too.
				 */
				if (unlikely(msg->msg_state == HTTP_MSG_ERROR)) {
					if (req->flags & CF_SHUTW) {
						/* request errors are most likely due to
						 * the server aborting the transfer.
						 */
						goto aborted_xfer;
					}
					if (msg->err_pos >= 0)
						http_capture_bad_message(&s->fe->invalid_req, s, msg, old_state, s->be);
					goto return_bad_req;
				}
				return 1;
			}

			/* If "option abortonclose" is set on the backend, we
			 * want to monitor the client's connection and forward
			 * any shutdown notification to the server, which will
			 * decide whether to close or to go on processing the
			 * request.
			 */
			if (s->be->options & PR_O_ABRT_CLOSE) {
				channel_auto_read(req);
				channel_auto_close(req);
			}
			else if (s->txn.meth == HTTP_METH_POST) {
				/* POST requests may require to read extra CRLF
				 * sent by broken browsers and which could cause
				 * an RST to be sent upon close on some systems
				 * (eg: Linux).
				 */
				channel_auto_read(req);
			}

			return 0;
		}
	}

 missing_data:
	/* stop waiting for data if the input is closed before the end */
	if (req->flags & CF_SHUTR) {
		if (!(s->flags & SN_ERR_MASK))
			s->flags |= SN_ERR_CLICL;
		if (!(s->flags & SN_FINST_MASK)) {
			if (txn->rsp.msg_state < HTTP_MSG_ERROR)
				s->flags |= SN_FINST_H;
			else
				s->flags |= SN_FINST_D;
		}

		s->fe->fe_counters.cli_aborts++;
		s->be->be_counters.cli_aborts++;
		if (objt_server(s->target))
			objt_server(s->target)->counters.cli_aborts++;

		goto return_bad_req_stats_ok;
	}

	/* waiting for the last bits to leave the buffer */
	if (req->flags & CF_SHUTW)
		goto aborted_xfer;

	/* When TE: chunked is used, we need to get there again to parse remaining
	 * chunks even if the client has closed, so we don't want to set CF_DONTCLOSE.
	 */
	if (msg->flags & HTTP_MSGF_TE_CHNK)
		channel_dont_close(req);

	/* We know that more data are expected, but we couldn't send more that
	 * what we did. So we always set the CF_EXPECT_MORE flag so that the
	 * system knows it must not set a PUSH on this first part. Interactive
	 * modes are already handled by the stream sock layer. We must not do
	 * this in content-length mode because it could present the MSG_MORE
	 * flag with the last block of forwarded data, which would cause an
	 * additional delay to be observed by the receiver.
	 */
	if (msg->flags & HTTP_MSGF_TE_CHNK)
		req->flags |= CF_EXPECT_MORE;

	http_silent_debug(__LINE__, s);
	return 0;

 return_bad_req: /* let's centralize all bad requests */
	s->fe->fe_counters.failed_req++;
	if (s->listener->counters)
		s->listener->counters->failed_req++;
 return_bad_req_stats_ok:
	txn->req.msg_state = HTTP_MSG_ERROR;
	if (txn->status) {
		/* Note: we don't send any error if some data were already sent */
		stream_int_retnclose(req->prod, NULL);
	} else {
		txn->status = 400;
		stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_400));
	}
	req->analysers = 0;
	s->rep->analysers = 0; /* we're in data phase, we want to abort both directions */

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK)) {
		if (txn->rsp.msg_state < HTTP_MSG_ERROR)
			s->flags |= SN_FINST_H;
		else
			s->flags |= SN_FINST_D;
	}
	return 0;

 aborted_xfer:
	txn->req.msg_state = HTTP_MSG_ERROR;
	if (txn->status) {
		/* Note: we don't send any error if some data were already sent */
		stream_int_retnclose(req->prod, NULL);
	} else {
		txn->status = 502;
		stream_int_retnclose(req->prod, http_error_message(s, HTTP_ERR_502));
	}
	req->analysers = 0;
	s->rep->analysers = 0; /* we're in data phase, we want to abort both directions */

	s->fe->fe_counters.srv_aborts++;
	s->be->be_counters.srv_aborts++;
	if (objt_server(s->target))
		objt_server(s->target)->counters.srv_aborts++;

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_SRVCL;
	if (!(s->flags & SN_FINST_MASK)) {
		if (txn->rsp.msg_state < HTTP_MSG_ERROR)
			s->flags |= SN_FINST_H;
		else
			s->flags |= SN_FINST_D;
	}
	return 0;
}

/* This stream analyser waits for a complete HTTP response. It returns 1 if the
 * processing can continue on next analysers, or zero if it either needs more
 * data or wants to immediately abort the response (eg: timeout, error, ...). It
 * is tied to AN_RES_WAIT_HTTP and may may remove itself from s->rep->analysers
 * when it has nothing left to do, and may remove any analyser when it wants to
 * abort.
 */
int http_wait_for_response(struct session *s, struct channel *rep, int an_bit)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &txn->rsp;
	struct hdr_ctx ctx;
	int use_close_only;
	int cur_idx;
	int n;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		rep,
		rep->rex, rep->wex,
		rep->flags,
		rep->buf->i,
		rep->analysers);

	/*
	 * Now parse the partial (or complete) lines.
	 * We will check the response syntax, and also join multi-line
	 * headers. An index of all the lines will be elaborated while
	 * parsing.
	 *
	 * For the parsing, we use a 28 states FSM.
	 *
	 * Here is the information we currently have :
	 *   rep->buf->p             = beginning of response
	 *   rep->buf->p + msg->eoh  = end of processed headers / start of current one
	 *   rep->buf->p + rep->buf->i    = end of input data
	 *   msg->eol           = end of current header or line (LF or CRLF)
	 *   msg->next          = first non-visited byte
	 */

	/* There's a protected area at the end of the buffer for rewriting
	 * purposes. We don't want to start to parse the request if the
	 * protected area is affected, because we may have to move processed
	 * data later, which is much more complicated.
	 */
	if (buffer_not_empty(rep->buf) && msg->msg_state < HTTP_MSG_ERROR) {
		if (unlikely(channel_full(rep) ||
			     bi_end(rep->buf) < b_ptr(rep->buf, msg->next) ||
			     bi_end(rep->buf) > rep->buf->data + rep->buf->size - global.tune.maxrewrite)) {
			if (rep->buf->o) {
				/* some data has still not left the buffer, wake us once that's done */
				if (rep->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_WRITE_ERROR|CF_WRITE_TIMEOUT))
					goto abort_response;
				channel_dont_close(rep);
				rep->flags |= CF_READ_DONTWAIT; /* try to get back here ASAP */
				return 0;
			}
			if (rep->buf->i <= rep->buf->size - global.tune.maxrewrite)
				buffer_slow_realign(msg->chn->buf);
		}

		if (likely(msg->next < rep->buf->i))
			http_msg_analyzer(msg, &txn->hdr_idx);
	}

	/* 1: we might have to print this header in debug mode */
	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)) &&
		     (msg->msg_state >= HTTP_MSG_BODY || msg->msg_state == HTTP_MSG_ERROR))) {
		char *eol, *sol;

		sol = rep->buf->p;
		eol = sol + (msg->sl.st.l ? msg->sl.st.l : rep->buf->i);
		debug_hdr("srvrep", s, sol, eol);

		sol += hdr_idx_first_pos(&txn->hdr_idx);
		cur_idx = hdr_idx_first_idx(&txn->hdr_idx);

		while (cur_idx) {
			eol = sol + txn->hdr_idx.v[cur_idx].len;
			debug_hdr("srvhdr", s, sol, eol);
			sol = eol + txn->hdr_idx.v[cur_idx].cr + 1;
			cur_idx = txn->hdr_idx.v[cur_idx].next;
		}
	}

	/*
	 * Now we quickly check if we have found a full valid response.
	 * If not so, we check the FD and buffer states before leaving.
	 * A full response is indicated by the fact that we have seen
	 * the double LF/CRLF, so the state is >= HTTP_MSG_BODY. Invalid
	 * responses are checked first.
	 *
	 * Depending on whether the client is still there or not, we
	 * may send an error response back or not. Note that normally
	 * we should only check for HTTP status there, and check I/O
	 * errors somewhere else.
	 */

	if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
		/* Invalid response */
		if (unlikely(msg->msg_state == HTTP_MSG_ERROR)) {
			/* we detected a parsing error. We want to archive this response
			 * in the dedicated proxy area for later troubleshooting.
			 */
		hdr_response_bad:
			if (msg->msg_state == HTTP_MSG_ERROR || msg->err_pos >= 0)
				http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

			s->be->be_counters.failed_resp++;
			if (objt_server(s->target)) {
				objt_server(s->target)->counters.failed_resp++;
				health_adjust(objt_server(s->target), HANA_STATUS_HTTP_HDRRSP);
			}
		abort_response:
			channel_auto_close(rep);
			rep->analysers = 0;
			txn->status = 502;
			rep->prod->flags |= SI_FL_NOLINGER;
			bi_erase(rep);
			stream_int_retnclose(rep->cons, http_error_message(s, HTTP_ERR_502));

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_PRXCOND;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;

			return 0;
		}

		/* too large response does not fit in buffer. */
		else if (buffer_full(rep->buf, global.tune.maxrewrite)) {
			if (msg->err_pos < 0)
				msg->err_pos = rep->buf->i;
			goto hdr_response_bad;
		}

		/* read error */
		else if (rep->flags & CF_READ_ERROR) {
			if (msg->err_pos >= 0)
				http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

			s->be->be_counters.failed_resp++;
			if (objt_server(s->target)) {
				objt_server(s->target)->counters.failed_resp++;
				health_adjust(objt_server(s->target), HANA_STATUS_HTTP_READ_ERROR);
			}

			channel_auto_close(rep);
			rep->analysers = 0;
			txn->status = 502;
			rep->prod->flags |= SI_FL_NOLINGER;
			bi_erase(rep);
			stream_int_retnclose(rep->cons, http_error_message(s, HTTP_ERR_502));

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_SRVCL;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;
			return 0;
		}

		/* read timeout : return a 504 to the client. */
		else if (rep->flags & CF_READ_TIMEOUT) {
			if (msg->err_pos >= 0)
				http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

			s->be->be_counters.failed_resp++;
			if (objt_server(s->target)) {
				objt_server(s->target)->counters.failed_resp++;
				health_adjust(objt_server(s->target), HANA_STATUS_HTTP_READ_TIMEOUT);
			}

			channel_auto_close(rep);
			rep->analysers = 0;
			txn->status = 504;
			rep->prod->flags |= SI_FL_NOLINGER;
			bi_erase(rep);
			stream_int_retnclose(rep->cons, http_error_message(s, HTTP_ERR_504));

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_SRVTO;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;
			return 0;
		}

		/* client abort with an abortonclose */
		else if ((rep->flags & CF_SHUTR) && ((s->req->flags & (CF_SHUTR|CF_SHUTW)) == (CF_SHUTR|CF_SHUTW))) {
			s->fe->fe_counters.cli_aborts++;
			s->be->be_counters.cli_aborts++;
			if (objt_server(s->target))
				objt_server(s->target)->counters.cli_aborts++;

			rep->analysers = 0;
			channel_auto_close(rep);

			txn->status = 400;
			bi_erase(rep);
			stream_int_retnclose(rep->cons, http_error_message(s, HTTP_ERR_400));

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_CLICL;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;

			/* process_session() will take care of the error */
			return 0;
		}

		/* close from server, capture the response if the server has started to respond */
		else if (rep->flags & CF_SHUTR) {
			if (msg->msg_state >= HTTP_MSG_RPVER || msg->err_pos >= 0)
				http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

			s->be->be_counters.failed_resp++;
			if (objt_server(s->target)) {
				objt_server(s->target)->counters.failed_resp++;
				health_adjust(objt_server(s->target), HANA_STATUS_HTTP_BROKEN_PIPE);
			}

			channel_auto_close(rep);
			rep->analysers = 0;
			txn->status = 502;
			rep->prod->flags |= SI_FL_NOLINGER;
			bi_erase(rep);
			stream_int_retnclose(rep->cons, http_error_message(s, HTTP_ERR_502));

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_SRVCL;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;
			return 0;
		}

		/* write error to client (we don't send any message then) */
		else if (rep->flags & CF_WRITE_ERROR) {
			if (msg->err_pos >= 0)
				http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

			s->be->be_counters.failed_resp++;
			rep->analysers = 0;
			channel_auto_close(rep);

			if (!(s->flags & SN_ERR_MASK))
				s->flags |= SN_ERR_CLICL;
			if (!(s->flags & SN_FINST_MASK))
				s->flags |= SN_FINST_H;

			/* process_session() will take care of the error */
			return 0;
		}

		channel_dont_close(rep);
		return 0;
	}

	/* More interesting part now : we know that we have a complete
	 * response which at least looks like HTTP. We have an indicator
	 * of each header's length, so we can parse them quickly.
	 */

	if (unlikely(msg->err_pos >= 0))
		http_capture_bad_message(&s->be->invalid_rep, s, msg, msg->msg_state, s->fe);

	/*
	 * 1: get the status code
	 */
	n = rep->buf->p[msg->sl.st.c] - '0';
	if (n < 1 || n > 5)
		n = 0;
	/* when the client triggers a 4xx from the server, it's most often due
	 * to a missing object or permission. These events should be tracked
	 * because if they happen often, it may indicate a brute force or a
	 * vulnerability scan.
	 */
	if (n == 4)
		session_inc_http_err_ctr(s);

	if (objt_server(s->target))
		objt_server(s->target)->counters.p.http.rsp[n]++;

	/* check if the response is HTTP/1.1 or above */
	if ((msg->sl.st.v_l == 8) &&
	    ((rep->buf->p[5] > '1') ||
	     ((rep->buf->p[5] == '1') && (rep->buf->p[7] >= '1'))))
		msg->flags |= HTTP_MSGF_VER_11;

	/* "connection" has not been parsed yet */
	txn->flags &= ~(TX_HDR_CONN_PRS|TX_HDR_CONN_CLO|TX_HDR_CONN_KAL|TX_HDR_CONN_UPG|TX_CON_CLO_SET|TX_CON_KAL_SET);

	/* transfer length unknown*/
	msg->flags &= ~HTTP_MSGF_XFER_LEN;

	txn->status = strl2ui(rep->buf->p + msg->sl.st.c, msg->sl.st.c_l);

	/* Adjust server's health based on status code. Note: status codes 501
	 * and 505 are triggered on demand by client request, so we must not
	 * count them as server failures.
	 */
	if (objt_server(s->target)) {
		if (txn->status >= 100 && (txn->status < 500 || txn->status == 501 || txn->status == 505))
			health_adjust(objt_server(s->target), HANA_STATUS_HTTP_OK);
		else
			health_adjust(objt_server(s->target), HANA_STATUS_HTTP_STS);
	}

	/*
	 * 2: check for cacheability.
	 */

	switch (txn->status) {
	case 200:
	case 203:
	case 206:
	case 300:
	case 301:
	case 410:
		/* RFC2616 @13.4:
		 *   "A response received with a status code of
		 *    200, 203, 206, 300, 301 or 410 MAY be stored
		 *    by a cache (...) unless a cache-control
		 *    directive prohibits caching."
		 *
		 * RFC2616 @9.5: POST method :
		 *   "Responses to this method are not cacheable,
		 *    unless the response includes appropriate
		 *    Cache-Control or Expires header fields."
		 */
		if (likely(txn->meth != HTTP_METH_POST) &&
		    ((s->be->options & PR_O_CHK_CACHE) || (s->be->ck_opts & PR_CK_NOC)))
			txn->flags |= TX_CACHEABLE | TX_CACHE_COOK;
		break;
	default:
		break;
	}

	/*
	 * 3: we may need to capture headers
	 */
	s->logs.logwait &= ~LW_RESP;
	if (unlikely((s->logs.logwait & LW_RSPHDR) && txn->rsp.cap))
		capture_headers(rep->buf->p, &txn->hdr_idx,
				txn->rsp.cap, s->fe->rsp_cap);

	/* 4: determine the transfer-length.
	 * According to RFC2616 #4.4, amended by the HTTPbis working group,
	 * the presence of a message-body in a RESPONSE and its transfer length
	 * must be determined that way :
	 *
	 *   All responses to the HEAD request method MUST NOT include a
	 *   message-body, even though the presence of entity-header fields
	 *   might lead one to believe they do.  All 1xx (informational), 204
	 *   (No Content), and 304 (Not Modified) responses MUST NOT include a
	 *   message-body.  All other responses do include a message-body,
	 *   although it MAY be of zero length.
	 *
	 *   1. Any response which "MUST NOT" include a message-body (such as the
	 *      1xx, 204 and 304 responses and any response to a HEAD request) is
	 *      always terminated by the first empty line after the header fields,
	 *      regardless of the entity-header fields present in the message.
	 *
	 *   2. If a Transfer-Encoding header field (Section 9.7) is present and
	 *      the "chunked" transfer-coding (Section 6.2) is used, the
	 *      transfer-length is defined by the use of this transfer-coding.
	 *      If a Transfer-Encoding header field is present and the "chunked"
	 *      transfer-coding is not present, the transfer-length is defined by
	 *      the sender closing the connection.
	 *
	 *   3. If a Content-Length header field is present, its decimal value in
	 *      OCTETs represents both the entity-length and the transfer-length.
	 *      If a message is received with both a Transfer-Encoding header
	 *      field and a Content-Length header field, the latter MUST be ignored.
	 *
	 *   4. If the message uses the media type "multipart/byteranges", and
	 *      the transfer-length is not otherwise specified, then this self-
	 *      delimiting media type defines the transfer-length.  This media
	 *      type MUST NOT be used unless the sender knows that the recipient
	 *      can parse it; the presence in a request of a Range header with
	 *      multiple byte-range specifiers from a 1.1 client implies that the
	 *      client can parse multipart/byteranges responses.
	 *
	 *   5. By the server closing the connection.
	 */

	/* Skip parsing if no content length is possible. The response flags
	 * remain 0 as well as the chunk_len, which may or may not mirror
	 * the real header value, and we note that we know the response's length.
	 * FIXME: should we parse anyway and return an error on chunked encoding ?
	 */
	if (txn->meth == HTTP_METH_HEAD ||
	    (txn->status >= 100 && txn->status < 200) ||
	    txn->status == 204 || txn->status == 304) {
		msg->flags |= HTTP_MSGF_XFER_LEN;
		s->comp_algo = NULL;
		goto skip_content_length;
	}

	use_close_only = 0;
	ctx.idx = 0;
	while ((msg->flags & HTTP_MSGF_VER_11) &&
	       http_find_header2("Transfer-Encoding", 17, rep->buf->p, &txn->hdr_idx, &ctx)) {
		if (ctx.vlen == 7 && strncasecmp(ctx.line + ctx.val, "chunked", 7) == 0)
			msg->flags |= (HTTP_MSGF_TE_CHNK | HTTP_MSGF_XFER_LEN);
		else if (msg->flags & HTTP_MSGF_TE_CHNK) {
			/* bad transfer-encoding (chunked followed by something else) */
			use_close_only = 1;
			msg->flags &= ~(HTTP_MSGF_TE_CHNK | HTTP_MSGF_XFER_LEN);
			break;
		}
	}

	/* FIXME: below we should remove the content-length header(s) in case of chunked encoding */
	ctx.idx = 0;
	while (!(msg->flags & HTTP_MSGF_TE_CHNK) && !use_close_only &&
	       http_find_header2("Content-Length", 14, rep->buf->p, &txn->hdr_idx, &ctx)) {
		signed long long cl;

		if (!ctx.vlen) {
			msg->err_pos = ctx.line + ctx.val - rep->buf->p;
			goto hdr_response_bad;
		}

		if (strl2llrc(ctx.line + ctx.val, ctx.vlen, &cl)) {
			msg->err_pos = ctx.line + ctx.val - rep->buf->p;
			goto hdr_response_bad; /* parse failure */
		}

		if (cl < 0) {
			msg->err_pos = ctx.line + ctx.val - rep->buf->p;
			goto hdr_response_bad;
		}

		if ((msg->flags & HTTP_MSGF_CNT_LEN) && (msg->chunk_len != cl)) {
			msg->err_pos = ctx.line + ctx.val - rep->buf->p;
			goto hdr_response_bad; /* already specified, was different */
		}

		msg->flags |= HTTP_MSGF_CNT_LEN | HTTP_MSGF_XFER_LEN;
		msg->body_len = msg->chunk_len = cl;
	}

	if (s->fe->comp || s->be->comp)
		select_compression_response_header(s, rep->buf);

	/* FIXME: we should also implement the multipart/byterange method.
	 * For now on, we resort to close mode in this case (unknown length).
	 */
skip_content_length:

	/* end of job, return OK */
	rep->analysers &= ~an_bit;
	rep->analyse_exp = TICK_ETERNITY;
	channel_auto_close(rep);
	return 1;
}

/* This function performs all the processing enabled for the current response.
 * It normally returns 1 unless it wants to break. It relies on buffers flags,
 * and updates t->rep->analysers. It might make sense to explode it into several
 * other functions. It works like process_request (see indications above).
 */
int http_process_res_common(struct session *t, struct channel *rep, int an_bit, struct proxy *px)
{
	struct http_txn *txn = &t->txn;
	struct http_msg *msg = &txn->rsp;
	struct proxy *cur_proxy;
	struct cond_wordlist *wl;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		t,
		rep,
		rep->rex, rep->wex,
		rep->flags,
		rep->buf->i,
		rep->analysers);

	if (unlikely(msg->msg_state < HTTP_MSG_BODY))	/* we need more data */
		return 0;

	rep->analysers &= ~an_bit;
	rep->analyse_exp = TICK_ETERNITY;

	/* Now we have to check if we need to modify the Connection header.
	 * This is more difficult on the response than it is on the request,
	 * because we can have two different HTTP versions and we don't know
	 * how the client will interprete a response. For instance, let's say
	 * that the client sends a keep-alive request in HTTP/1.0 and gets an
	 * HTTP/1.1 response without any header. Maybe it will bound itself to
	 * HTTP/1.0 because it only knows about it, and will consider the lack
	 * of header as a close, or maybe it knows HTTP/1.1 and can consider
	 * the lack of header as a keep-alive. Thus we will use two flags
	 * indicating how a request MAY be understood by the client. In case
	 * of multiple possibilities, we'll fix the header to be explicit. If
	 * ambiguous cases such as both close and keepalive are seen, then we
	 * will fall back to explicit close. Note that we won't take risks with
	 * HTTP/1.0 clients which may not necessarily understand keep-alive.
	 * See doc/internals/connection-header.txt for the complete matrix.
	 */

	if (unlikely((txn->meth == HTTP_METH_CONNECT && txn->status == 200) ||
		     txn->status == 101)) {
		/* Either we've established an explicit tunnel, or we're
		 * switching the protocol. In both cases, we're very unlikely
		 * to understand the next protocols. We have to switch to tunnel
		 * mode, so that we transfer the request and responses then let
		 * this protocol pass unmodified. When we later implement specific
		 * parsers for such protocols, we'll want to check the Upgrade
		 * header which contains information about that protocol for
		 * responses with status 101 (eg: see RFC2817 about TLS).
		 */
		txn->flags = (txn->flags & ~TX_CON_WANT_MSK) | TX_CON_WANT_TUN;
	}
	else if ((txn->status >= 200) && !(txn->flags & TX_HDR_CONN_PRS) &&
		 ((txn->flags & TX_CON_WANT_MSK) != TX_CON_WANT_TUN ||
		  ((t->fe->options|t->be->options) & PR_O_HTTP_CLOSE))) {
		int to_del = 0;

		/* on unknown transfer length, we must close */
		if (!(msg->flags & HTTP_MSGF_XFER_LEN) &&
		    (txn->flags & TX_CON_WANT_MSK) != TX_CON_WANT_TUN)
			txn->flags = (txn->flags & ~TX_CON_WANT_MSK) | TX_CON_WANT_CLO;

		/* now adjust header transformations depending on current state */
		if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_TUN ||
		    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_CLO) {
			to_del |= 2; /* remove "keep-alive" on any response */
			if (!(msg->flags & HTTP_MSGF_VER_11))
				to_del |= 1; /* remove "close" for HTTP/1.0 responses */
		}
		else { /* SCL / KAL */
			to_del |= 1; /* remove "close" on any response */
			if (txn->req.flags & msg->flags & HTTP_MSGF_VER_11)
				to_del |= 2; /* remove "keep-alive" on pure 1.1 responses */
		}

		/* Parse and remove some headers from the connection header */
		http_parse_connection_header(txn, msg, to_del);

		/* Some keep-alive responses are converted to Server-close if
		 * the server wants to close.
		 */
		if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL) {
			if ((txn->flags & TX_HDR_CONN_CLO) ||
			    (!(txn->flags & TX_HDR_CONN_KAL) && !(msg->flags & HTTP_MSGF_VER_11)))
				txn->flags = (txn->flags & ~TX_CON_WANT_MSK) | TX_CON_WANT_SCL;
		}
	}

	if (1) {
		/*
		 * 3: we will have to evaluate the filters.
		 * As opposed to version 1.2, now they will be evaluated in the
		 * filters order and not in the header order. This means that
		 * each filter has to be validated among all headers.
		 *
		 * Filters are tried with ->be first, then with ->fe if it is
		 * different from ->be.
		 */

		cur_proxy = t->be;
		while (1) {
			struct proxy *rule_set = cur_proxy;

			/* try headers filters */
			if (rule_set->rsp_exp != NULL) {
				if (apply_filters_to_response(t, rep, rule_set) < 0) {
				return_bad_resp:
					if (objt_server(t->target)) {
						objt_server(t->target)->counters.failed_resp++;
						health_adjust(objt_server(t->target), HANA_STATUS_HTTP_RSP);
					}
					t->be->be_counters.failed_resp++;
				return_srv_prx_502:
					rep->analysers = 0;
					txn->status = 502;
					rep->prod->flags |= SI_FL_NOLINGER;
					bi_erase(rep);
					stream_int_retnclose(rep->cons, http_error_message(t, HTTP_ERR_502));
					if (!(t->flags & SN_ERR_MASK))
						t->flags |= SN_ERR_PRXCOND;
					if (!(t->flags & SN_FINST_MASK))
						t->flags |= SN_FINST_H;
					return 0;
				}
			}

			/* has the response been denied ? */
			if (txn->flags & TX_SVDENY) {
				if (objt_server(t->target))
					objt_server(t->target)->counters.failed_secu++;

				t->be->be_counters.denied_resp++;
				t->fe->fe_counters.denied_resp++;
				if (t->listener->counters)
					t->listener->counters->denied_resp++;

				goto return_srv_prx_502;
			}

			/* add response headers from the rule sets in the same order */
			list_for_each_entry(wl, &rule_set->rsp_add, list) {
				if (txn->status < 200)
					break;
				if (wl->cond) {
					int ret = acl_exec_cond(wl->cond, px, t, txn, SMP_OPT_DIR_RES|SMP_OPT_FINAL);
					ret = acl_pass(ret);
					if (((struct acl_cond *)wl->cond)->pol == ACL_COND_UNLESS)
						ret = !ret;
					if (!ret)
						continue;
				}
				if (unlikely(http_header_add_tail(&txn->rsp, &txn->hdr_idx, wl->s) < 0))
					goto return_bad_resp;
			}

			/* check whether we're already working on the frontend */
			if (cur_proxy == t->fe)
				break;
			cur_proxy = t->fe;
		}

		/*
		 * We may be facing a 100-continue response, in which case this
		 * is not the right response, and we're waiting for the next one.
		 * Let's allow this response to go to the client and wait for the
		 * next one.
		 */
		if (unlikely(txn->status == 100)) {
			hdr_idx_init(&txn->hdr_idx);
			msg->next -= channel_forward(rep, msg->next);
			msg->msg_state = HTTP_MSG_RPBEFORE;
			txn->status = 0;
			rep->analysers |= AN_RES_WAIT_HTTP | an_bit;
			return 1;
		}
		else if (unlikely(txn->status < 200))
			goto skip_header_mangling;

		/* we don't have any 1xx status code now */

		/*
		 * 4: check for server cookie.
		 */
		if (t->be->cookie_name || t->be->appsession_name || t->fe->capture_name ||
		    (t->be->options & PR_O_CHK_CACHE))
			manage_server_side_cookies(t, rep);


		/*
		 * 5: check for cache-control or pragma headers if required.
		 */
		if ((t->be->options & PR_O_CHK_CACHE) || (t->be->ck_opts & PR_CK_NOC))
			check_response_for_cacheability(t, rep);

		/*
		 * 6: add server cookie in the response if needed
		 */
		if (objt_server(t->target) && (t->be->ck_opts & PR_CK_INS) &&
		    !((txn->flags & TX_SCK_FOUND) && (t->be->ck_opts & PR_CK_PSV)) &&
		    (!(t->flags & SN_DIRECT) ||
		     ((t->be->cookie_maxidle || txn->cookie_last_date) &&
		      (!txn->cookie_last_date || (txn->cookie_last_date - date.tv_sec) < 0)) ||
		     (t->be->cookie_maxlife && !txn->cookie_first_date) ||  // set the first_date
		     (!t->be->cookie_maxlife && txn->cookie_first_date)) && // remove the first_date
		    (!(t->be->ck_opts & PR_CK_POST) || (txn->meth == HTTP_METH_POST)) &&
		    !(t->flags & SN_IGNORE_PRST)) {
			/* the server is known, it's not the one the client requested, or the
			 * cookie's last seen date needs to be refreshed. We have to
			 * insert a set-cookie here, except if we want to insert only on POST
			 * requests and this one isn't. Note that servers which don't have cookies
			 * (eg: some backup servers) will return a full cookie removal request.
			 */
			if (!objt_server(t->target)->cookie) {
				chunk_printf(&trash,
					      "Set-Cookie: %s=; Expires=Thu, 01-Jan-1970 00:00:01 GMT; path=/",
					      t->be->cookie_name);
			}
			else {
				chunk_printf(&trash, "Set-Cookie: %s=%s", t->be->cookie_name, objt_server(t->target)->cookie);

				if (t->be->cookie_maxidle || t->be->cookie_maxlife) {
					/* emit last_date, which is mandatory */
					trash.str[trash.len++] = COOKIE_DELIM_DATE;
					s30tob64((date.tv_sec+3) >> 2, trash.str + trash.len);
					trash.len += 5;

					if (t->be->cookie_maxlife) {
						/* emit first_date, which is either the original one or
						 * the current date.
						 */
						trash.str[trash.len++] = COOKIE_DELIM_DATE;
						s30tob64(txn->cookie_first_date ?
							 txn->cookie_first_date >> 2 :
							 (date.tv_sec+3) >> 2, trash.str + trash.len);
						trash.len += 5;
					}
				}
				chunk_appendf(&trash, "; path=/");
			}

			if (t->be->cookie_domain)
				chunk_appendf(&trash, "; domain=%s", t->be->cookie_domain);

			if (t->be->ck_opts & PR_CK_HTTPONLY)
				chunk_appendf(&trash, "; HttpOnly");

			if (t->be->ck_opts & PR_CK_SECURE)
				chunk_appendf(&trash, "; Secure");

			if (unlikely(http_header_add_tail2(&txn->rsp, &txn->hdr_idx, trash.str, trash.len) < 0))
				goto return_bad_resp;

			txn->flags &= ~TX_SCK_MASK;
			if (objt_server(t->target)->cookie && (t->flags & SN_DIRECT))
				/* the server did not change, only the date was updated */
				txn->flags |= TX_SCK_UPDATED;
			else
				txn->flags |= TX_SCK_INSERTED;

			/* Here, we will tell an eventual cache on the client side that we don't
			 * want it to cache this reply because HTTP/1.0 caches also cache cookies !
			 * Some caches understand the correct form: 'no-cache="set-cookie"', but
			 * others don't (eg: apache <= 1.3.26). So we use 'private' instead.
			 */
			if ((t->be->ck_opts & PR_CK_NOC) && (txn->flags & TX_CACHEABLE)) {

				txn->flags &= ~TX_CACHEABLE & ~TX_CACHE_COOK;

				if (unlikely(http_header_add_tail2(&txn->rsp, &txn->hdr_idx,
								   "Cache-control: private", 22) < 0))
					goto return_bad_resp;
			}
		}

		/*
		 * 7: check if result will be cacheable with a cookie.
		 * We'll block the response if security checks have caught
		 * nasty things such as a cacheable cookie.
		 */
		if (((txn->flags & (TX_CACHEABLE | TX_CACHE_COOK | TX_SCK_PRESENT)) ==
		     (TX_CACHEABLE | TX_CACHE_COOK | TX_SCK_PRESENT)) &&
		    (t->be->options & PR_O_CHK_CACHE)) {

			/* we're in presence of a cacheable response containing
			 * a set-cookie header. We'll block it as requested by
			 * the 'checkcache' option, and send an alert.
			 */
			if (objt_server(t->target))
				objt_server(t->target)->counters.failed_secu++;

			t->be->be_counters.denied_resp++;
			t->fe->fe_counters.denied_resp++;
			if (t->listener->counters)
				t->listener->counters->denied_resp++;

			Alert("Blocking cacheable cookie in response from instance %s, server %s.\n",
			      t->be->id, objt_server(t->target) ? objt_server(t->target)->id : "<dispatch>");
			send_log(t->be, LOG_ALERT,
				 "Blocking cacheable cookie in response from instance %s, server %s.\n",
				 t->be->id, objt_server(t->target) ? objt_server(t->target)->id : "<dispatch>");
			goto return_srv_prx_502;
		}

		/*
		 * 8: adjust "Connection: close" or "Connection: keep-alive" if needed.
		 * If an "Upgrade" token is found, the header is left untouched in order
		 * not to have to deal with some client bugs : some of them fail an upgrade
		 * if anything but "Upgrade" is present in the Connection header.
		 */
		if (!(txn->flags & TX_HDR_CONN_UPG) &&
		    (((txn->flags & TX_CON_WANT_MSK) != TX_CON_WANT_TUN) ||
		     ((t->fe->options|t->be->options) & PR_O_HTTP_CLOSE))) {
			unsigned int want_flags = 0;

			if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL ||
			    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL) {
				/* we want a keep-alive response here. Keep-alive header
				 * required if either side is not 1.1.
				 */
				if (!(txn->req.flags & msg->flags & HTTP_MSGF_VER_11))
					want_flags |= TX_CON_KAL_SET;
			}
			else {
				/* we want a close response here. Close header required if
				 * the server is 1.1, regardless of the client.
				 */
				if (msg->flags & HTTP_MSGF_VER_11)
					want_flags |= TX_CON_CLO_SET;
			}

			if (want_flags != (txn->flags & (TX_CON_CLO_SET|TX_CON_KAL_SET)))
				http_change_connection_header(txn, msg, want_flags);
		}

	skip_header_mangling:
		if ((msg->flags & HTTP_MSGF_XFER_LEN) ||
		    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_TUN)
			rep->analysers |= AN_RES_HTTP_XFER_BODY;

		/*************************************************************
		 * OK, that's finished for the headers. We have done what we *
		 * could. Let's switch to the DATA state.                    *
		 ************************************************************/

		t->logs.t_data = tv_ms_elapsed(&t->logs.tv_accept, &now);

		/* if the user wants to log as soon as possible, without counting
		 * bytes from the server, then this is the right moment. We have
		 * to temporarily assign bytes_out to log what we currently have.
		 */
		if (!LIST_ISEMPTY(&t->fe->logformat) && !(t->logs.logwait & LW_BYTES)) {
			t->logs.t_close = t->logs.t_data; /* to get a valid end date */
			t->logs.bytes_out = txn->rsp.eoh;
			t->do_log(t);
			t->logs.bytes_out = 0;
		}

		/* Note: we must not try to cheat by jumping directly to DATA,
		 * otherwise we would not let the client side wake up.
		 */

		return 1;
	}
	return 1;
}

/* This function is an analyser which forwards response body (including chunk
 * sizes if any). It is called as soon as we must forward, even if we forward
 * zero byte. The only situation where it must not be called is when we're in
 * tunnel mode and we want to forward till the close. It's used both to forward
 * remaining data and to resync after end of body. It expects the msg_state to
 * be between MSG_BODY and MSG_DONE (inclusive). It returns zero if it needs to
 * read more data, or 1 once we can go on with next request or end the session.
 * When in MSG_DATA or MSG_TRAILERS, it will automatically forward chunk_len
 * bytes of pending data + the headers if not already done (between sol and sov).
 * It eventually adjusts sol to match sov after the data in between have been sent.
 */
int http_response_forward_body(struct session *s, struct channel *res, int an_bit)
{
	struct http_txn *txn = &s->txn;
	struct http_msg *msg = &s->txn.rsp;
	unsigned int bytes;
	static struct buffer *tmpbuf = NULL;
	int compressing = 0;
	int consumed_data = 0;

	if (unlikely(msg->msg_state < HTTP_MSG_BODY))
		return 0;

	if ((res->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) ||
	    ((res->flags & CF_SHUTW) && (res->to_forward || res->buf->o)) ||
	    !s->req->analysers) {
		/* Output closed while we were sending data. We must abort and
		 * wake the other side up.
		 */
		msg->msg_state = HTTP_MSG_ERROR;
		http_resync_states(s);
		return 1;
	}

	/* in most states, we should abort in case of early close */
	channel_auto_close(res);

	/* this is the first time we need the compression buffer */
	if (s->comp_algo != NULL && tmpbuf == NULL) {
		if ((tmpbuf = pool_alloc2(pool2_buffer)) == NULL)
			goto aborted_xfer; /* no memory */
	}

	if (msg->msg_state < HTTP_MSG_CHUNK_SIZE) {
		/* we have msg->sov which points to the first byte of message body.
		 * rep->buf.p still points to the beginning of the message and msg->sol
		 * is still null. We forward the headers, we don't need them.
		 */
		channel_forward(res, msg->sov);
		msg->next = 0;
		msg->sov = 0;

		if (msg->flags & HTTP_MSGF_TE_CHNK)
			msg->msg_state = HTTP_MSG_CHUNK_SIZE;
		else
			msg->msg_state = HTTP_MSG_DATA;
	}

	if (s->comp_algo != NULL) {
		int ret = http_compression_buffer_init(s, res->buf, tmpbuf); /* init a buffer with headers */
		if (ret < 0)
			goto missing_data; /* not enough spaces in buffers */
		compressing = 1;
	}

	while (1) {
		http_silent_debug(__LINE__, s);
		/* we may have some data pending between sol and sov */
		if (s->comp_algo == NULL) {
			bytes = msg->sov - msg->sol;
			if (msg->chunk_len || bytes) {
				msg->sol = msg->sov;
				msg->next -= bytes; /* will be forwarded */
				msg->chunk_len += bytes;
				msg->chunk_len -= channel_forward(res, msg->chunk_len);
			}
		}

		if (msg->msg_state == HTTP_MSG_DATA) {
			int ret;

			/* must still forward */
			if (compressing) {
				consumed_data += ret = http_compression_buffer_add_data(s, res->buf, tmpbuf);
				if (ret < 0)
					goto aborted_xfer;
			}

			if (res->to_forward || msg->chunk_len)
				goto missing_data;

			/* nothing left to forward */
			if (msg->flags & HTTP_MSGF_TE_CHNK) {
				msg->msg_state = HTTP_MSG_CHUNK_CRLF;
			} else {
				msg->msg_state = HTTP_MSG_DONE;
				if (compressing && consumed_data) {
					http_compression_buffer_end(s, &res->buf, &tmpbuf, 1);
					compressing = 0;
				}
			}
		}
		else if (msg->msg_state == HTTP_MSG_CHUNK_SIZE) {
			/* read the chunk size and assign it to ->chunk_len, then
			 * set ->sov and ->next to point to the body and switch to DATA or
			 * TRAILERS state.
			 */
			int ret = http_parse_chunk_size(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->be->invalid_rep, s, msg, HTTP_MSG_CHUNK_SIZE, s->fe);
				goto return_bad_res;
			}
			if (compressing) {
				if (likely(msg->chunk_len > 0)) {
					/* skipping data if we are in compression mode */
					b_adv(res->buf, msg->next);
					msg->next = 0;
					msg->sov = 0;
					msg->sol = 0;
				} else {
					if (consumed_data) {
						http_compression_buffer_end(s, &res->buf, &tmpbuf, 1);
						compressing = 0;
					}
				}
			}
		/* otherwise we're in HTTP_MSG_DATA or HTTP_MSG_TRAILERS state */
		}
		else if (msg->msg_state == HTTP_MSG_CHUNK_CRLF) {
			/* we want the CRLF after the data */
			int ret = http_skip_chunk_crlf(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->be->invalid_rep, s, msg, HTTP_MSG_CHUNK_CRLF, s->fe);
				goto return_bad_res;
			}
			/* skipping data in buffer for compression */
			if (compressing) {
				b_adv(res->buf, msg->next);
				msg->next = 0;
				msg->sov = 0;
				msg->sol = 0;
			}
			/* we're in MSG_CHUNK_SIZE now */
		}
		else if (msg->msg_state == HTTP_MSG_TRAILERS) {
			int ret = http_forward_trailers(msg);

			if (ret == 0)
				goto missing_data;
			else if (ret < 0) {
				if (msg->err_pos >= 0)
					http_capture_bad_message(&s->be->invalid_rep, s, msg, HTTP_MSG_TRAILERS, s->fe);
				goto return_bad_res;
			}
			if (s->comp_algo != NULL) {
				/* forwarding trailers */
				channel_forward(res, msg->next);
				msg->next = 0;
			}
			/* we're in HTTP_MSG_DONE now */
		}
		else {
			int old_state = msg->msg_state;
			/* other states, DONE...TUNNEL */
			/* for keep-alive we don't want to forward closes on DONE */
			if ((txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL ||
			    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL)
				channel_dont_close(res);
			if (http_resync_states(s)) {
				http_silent_debug(__LINE__, s);
				/* some state changes occurred, maybe the analyser
				 * was disabled too.
				 */
				if (unlikely(msg->msg_state == HTTP_MSG_ERROR)) {
					if (res->flags & CF_SHUTW) {
						/* response errors are most likely due to
						 * the client aborting the transfer.
						 */
						goto aborted_xfer;
					}
					if (msg->err_pos >= 0)
						http_capture_bad_message(&s->be->invalid_rep, s, msg, old_state, s->fe);
					goto return_bad_res;
				}
				return 1;
			}
			return 0;
		}
	}

 missing_data:
	if (compressing && consumed_data) {
		http_compression_buffer_end(s, &res->buf, &tmpbuf, 0);
		compressing = 0;
	}

	if (res->flags & CF_SHUTW)
		goto aborted_xfer;

	/* stop waiting for data if the input is closed before the end. If the
	 * client side was already closed, it means that the client has aborted,
	 * so we don't want to count this as a server abort. Otherwise it's a
	 * server abort.
	 */
	if (res->flags & CF_SHUTR) {
		if ((res->flags & CF_SHUTW_NOW) || (s->req->flags & CF_SHUTR))
			goto aborted_xfer;
		if (!(s->flags & SN_ERR_MASK))
			s->flags |= SN_ERR_SRVCL;
		s->be->be_counters.srv_aborts++;
		if (objt_server(s->target))
			objt_server(s->target)->counters.srv_aborts++;
		goto return_bad_res_stats_ok;
	}

	/* we need to obey the req analyser, so if it leaves, we must too */
	if (!s->req->analysers)
		goto return_bad_res;

	/* forward any data pending between sol and sov */
	if (s->comp_algo == NULL) {
		bytes = msg->sov - msg->sol;
		if (msg->chunk_len || bytes) {
			msg->sol = msg->sov;
			msg->next -= bytes; /* will be forwarded */
			msg->chunk_len += bytes;
			msg->chunk_len -= channel_forward(res, msg->chunk_len);
		}
	}

	/* When TE: chunked is used, we need to get there again to parse remaining
	 * chunks even if the server has closed, so we don't want to set CF_DONTCLOSE.
	 * Similarly, with keep-alive on the client side, we don't want to forward a
	 * close.
	 */
	if ((msg->flags & HTTP_MSGF_TE_CHNK) || s->comp_algo ||
	    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_KAL ||
	    (txn->flags & TX_CON_WANT_MSK) == TX_CON_WANT_SCL)
		channel_dont_close(res);

	/* We know that more data are expected, but we couldn't send more that
	 * what we did. So we always set the CF_EXPECT_MORE flag so that the
	 * system knows it must not set a PUSH on this first part. Interactive
	 * modes are already handled by the stream sock layer. We must not do
	 * this in content-length mode because it could present the MSG_MORE
	 * flag with the last block of forwarded data, which would cause an
	 * additional delay to be observed by the receiver.
	 */
	if ((msg->flags & HTTP_MSGF_TE_CHNK) || s->comp_algo)
		res->flags |= CF_EXPECT_MORE;

	/* the session handler will take care of timeouts and errors */
	http_silent_debug(__LINE__, s);
	return 0;

 return_bad_res: /* let's centralize all bad responses */
	s->be->be_counters.failed_resp++;
	if (objt_server(s->target))
		objt_server(s->target)->counters.failed_resp++;

 return_bad_res_stats_ok:
	txn->rsp.msg_state = HTTP_MSG_ERROR;
	/* don't send any error message as we're in the body */
	stream_int_retnclose(res->cons, NULL);
	res->analysers = 0;
	s->req->analysers = 0; /* we're in data phase, we want to abort both directions */
	if (objt_server(s->target))
		health_adjust(objt_server(s->target), HANA_STATUS_HTTP_HDRRSP);

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_PRXCOND;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_D;
	return 0;

 aborted_xfer:
	txn->rsp.msg_state = HTTP_MSG_ERROR;
	/* don't send any error message as we're in the body */
	stream_int_retnclose(res->cons, NULL);
	res->analysers = 0;
	s->req->analysers = 0; /* we're in data phase, we want to abort both directions */

	s->fe->fe_counters.cli_aborts++;
	s->be->be_counters.cli_aborts++;
	if (objt_server(s->target))
		objt_server(s->target)->counters.cli_aborts++;

	if (!(s->flags & SN_ERR_MASK))
		s->flags |= SN_ERR_CLICL;
	if (!(s->flags & SN_FINST_MASK))
		s->flags |= SN_FINST_D;
	return 0;
}

/* Iterate the same filter through all request headers.
 * Returns 1 if this filter can be stopped upon return, otherwise 0.
 * Since it can manage the switch to another backend, it updates the per-proxy
 * DENY stats.
 */
int apply_filter_to_req_headers(struct session *t, struct channel *req, struct hdr_exp *exp)
{
	char term;
	char *cur_ptr, *cur_end, *cur_next;
	int cur_idx, old_idx, last_hdr;
	struct http_txn *txn = &t->txn;
	struct hdr_idx_elem *cur_hdr;
	int delta;

	last_hdr = 0;

	cur_next = req->buf->p + hdr_idx_first_pos(&txn->hdr_idx);
	old_idx = 0;

	while (!last_hdr) {
		if (unlikely(txn->flags & (TX_CLDENY | TX_CLTARPIT)))
			return 1;
		else if (unlikely(txn->flags & TX_CLALLOW) &&
			 (exp->action == ACT_ALLOW ||
			  exp->action == ACT_DENY ||
			  exp->action == ACT_TARPIT))
			return 0;

		cur_idx = txn->hdr_idx.v[old_idx].next;
		if (!cur_idx)
			break;

		cur_hdr  = &txn->hdr_idx.v[cur_idx];
		cur_ptr  = cur_next;
		cur_end  = cur_ptr + cur_hdr->len;
		cur_next = cur_end + cur_hdr->cr + 1;

		/* Now we have one header between cur_ptr and cur_end,
		 * and the next header starts at cur_next.
		 */

		/* The annoying part is that pattern matching needs
		 * that we modify the contents to null-terminate all
		 * strings before testing them.
		 */

		term = *cur_end;
		*cur_end = '\0';

		if (regexec(exp->preg, cur_ptr, MAX_MATCH, pmatch, 0) == 0) {
			switch (exp->action) {
			case ACT_SETBE:
				/* It is not possible to jump a second time.
				 * FIXME: should we return an HTTP/500 here so that
				 * the admin knows there's a problem ?
				 */
				if (t->be != t->fe)
					break;

				/* Swithing Proxy */
				session_set_backend(t, (struct proxy *)exp->replace);
				last_hdr = 1;
				break;

			case ACT_ALLOW:
				txn->flags |= TX_CLALLOW;
				last_hdr = 1;
				break;

			case ACT_DENY:
				txn->flags |= TX_CLDENY;
				last_hdr = 1;

				t->fe->fe_counters.denied_req++;
				if (t->fe != t->be)
					t->be->be_counters.denied_req++;
				if (t->listener->counters)
					t->listener->counters->denied_req++;

				break;

			case ACT_TARPIT:
				txn->flags |= TX_CLTARPIT;
				last_hdr = 1;

				t->fe->fe_counters.denied_req++;
				if (t->fe != t->be)
					t->be->be_counters.denied_req++;
				if (t->listener->counters)
					t->listener->counters->denied_req++;

				break;

			case ACT_REPLACE:
				trash.len = exp_replace(trash.str, cur_ptr, exp->replace, pmatch);
				delta = buffer_replace2(req->buf, cur_ptr, cur_end, trash.str, trash.len);
				/* FIXME: if the user adds a newline in the replacement, the
				 * index will not be recalculated for now, and the new line
				 * will not be counted as a new header.
				 */

				cur_end += delta;
				cur_next += delta;
				cur_hdr->len += delta;
				http_msg_move_end(&txn->req, delta);
				break;

			case ACT_REMOVE:
				delta = buffer_replace2(req->buf, cur_ptr, cur_next, NULL, 0);
				cur_next += delta;

				http_msg_move_end(&txn->req, delta);
				txn->hdr_idx.v[old_idx].next = cur_hdr->next;
				txn->hdr_idx.used--;
				cur_hdr->len = 0;
				cur_end = NULL; /* null-term has been rewritten */
				cur_idx = old_idx;
				break;

			}
		}
		if (cur_end)
			*cur_end = term; /* restore the string terminator */

		/* keep the link from this header to next one in case of later
		 * removal of next header.
		 */
		old_idx = cur_idx;
	}
	return 0;
}


/* Apply the filter to the request line.
 * Returns 0 if nothing has been done, 1 if the filter has been applied,
 * or -1 if a replacement resulted in an invalid request line.
 * Since it can manage the switch to another backend, it updates the per-proxy
 * DENY stats.
 */
int apply_filter_to_req_line(struct session *t, struct channel *req, struct hdr_exp *exp)
{
	char term;
	char *cur_ptr, *cur_end;
	int done;
	struct http_txn *txn = &t->txn;
	int delta;

	if (unlikely(txn->flags & (TX_CLDENY | TX_CLTARPIT)))
		return 1;
	else if (unlikely(txn->flags & TX_CLALLOW) &&
		 (exp->action == ACT_ALLOW ||
		  exp->action == ACT_DENY ||
		  exp->action == ACT_TARPIT))
		return 0;
	else if (exp->action == ACT_REMOVE)
		return 0;

	done = 0;

	cur_ptr = req->buf->p;
	cur_end = cur_ptr + txn->req.sl.rq.l;

	/* Now we have the request line between cur_ptr and cur_end */

	/* The annoying part is that pattern matching needs
	 * that we modify the contents to null-terminate all
	 * strings before testing them.
	 */

	term = *cur_end;
	*cur_end = '\0';

	if (regexec(exp->preg, cur_ptr, MAX_MATCH, pmatch, 0) == 0) {
		switch (exp->action) {
		case ACT_SETBE:
			/* It is not possible to jump a second time.
			 * FIXME: should we return an HTTP/500 here so that
			 * the admin knows there's a problem ?
			 */
			if (t->be != t->fe)
				break;

			/* Swithing Proxy */
			session_set_backend(t, (struct proxy *)exp->replace);
			done = 1;
			break;

		case ACT_ALLOW:
			txn->flags |= TX_CLALLOW;
			done = 1;
			break;

		case ACT_DENY:
			txn->flags |= TX_CLDENY;

			t->fe->fe_counters.denied_req++;
			if (t->fe != t->be)
				t->be->be_counters.denied_req++;
			if (t->listener->counters)
				t->listener->counters->denied_req++;

			done = 1;
			break;

		case ACT_TARPIT:
			txn->flags |= TX_CLTARPIT;

			t->fe->fe_counters.denied_req++;
			if (t->fe != t->be)
				t->be->be_counters.denied_req++;
			if (t->listener->counters)
				t->listener->counters->denied_req++;

			done = 1;
			break;

		case ACT_REPLACE:
			*cur_end = term; /* restore the string terminator */
			trash.len = exp_replace(trash.str, cur_ptr, exp->replace, pmatch);
			delta = buffer_replace2(req->buf, cur_ptr, cur_end, trash.str, trash.len);
			/* FIXME: if the user adds a newline in the replacement, the
			 * index will not be recalculated for now, and the new line
			 * will not be counted as a new header.
			 */

			http_msg_move_end(&txn->req, delta);
			cur_end += delta;
			cur_end = (char *)http_parse_reqline(&txn->req,
							     HTTP_MSG_RQMETH,
							     cur_ptr, cur_end + 1,
							     NULL, NULL);
			if (unlikely(!cur_end))
				return -1;

			/* we have a full request and we know that we have either a CR
			 * or an LF at <ptr>.
			 */
			txn->meth = find_http_meth(cur_ptr, txn->req.sl.rq.m_l);
			hdr_idx_set_start(&txn->hdr_idx, txn->req.sl.rq.l, *cur_end == '\r');
			/* there is no point trying this regex on headers */
			return 1;
		}
	}
	*cur_end = term; /* restore the string terminator */
	return done;
}



/*
 * Apply all the req filters of proxy <px> to all headers in buffer <req> of session <s>.
 * Returns 0 if everything is alright, or -1 in case a replacement lead to an
 * unparsable request. Since it can manage the switch to another backend, it
 * updates the per-proxy DENY stats.
 */
int apply_filters_to_request(struct session *s, struct channel *req, struct proxy *px)
{
	struct http_txn *txn = &s->txn;
	struct hdr_exp *exp;

	for (exp = px->req_exp; exp; exp = exp->next) {
		int ret;

		/*
		 * The interleaving of transformations and verdicts
		 * makes it difficult to decide to continue or stop
		 * the evaluation.
		 */

		if (txn->flags & (TX_CLDENY|TX_CLTARPIT))
			break;

		if ((txn->flags & TX_CLALLOW) &&
		    (exp->action == ACT_ALLOW || exp->action == ACT_DENY ||
		     exp->action == ACT_TARPIT || exp->action == ACT_PASS))
			continue;

		/* if this filter had a condition, evaluate it now and skip to
		 * next filter if the condition does not match.
		 */
		if (exp->cond) {
			ret = acl_exec_cond(exp->cond, px, s, txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (((struct acl_cond *)exp->cond)->pol == ACL_COND_UNLESS)
				ret = !ret;

			if (!ret)
				continue;
		}

		/* Apply the filter to the request line. */
		ret = apply_filter_to_req_line(s, req, exp);
		if (unlikely(ret < 0))
			return -1;

		if (likely(ret == 0)) {
			/* The filter did not match the request, it can be
			 * iterated through all headers.
			 */
			apply_filter_to_req_headers(s, req, exp);
		}
	}
	return 0;
}



/*
 * Try to retrieve the server associated to the appsession.
 * If the server is found, it's assigned to the session.
 */
void manage_client_side_appsession(struct session *t, const char *buf, int len) {
	struct http_txn *txn = &t->txn;
	appsess *asession = NULL;
	char *sessid_temp = NULL;

	if (len > t->be->appsession_len) {
		len = t->be->appsession_len;
	}

	if (t->be->options2 & PR_O2_AS_REQL) {
		/* request-learn option is enabled : store the sessid in the session for future use */
		if (txn->sessid != NULL) {
			/* free previously allocated memory as we don't need the session id found in the URL anymore */
			pool_free2(apools.sessid, txn->sessid);
		}

		if ((txn->sessid = pool_alloc2(apools.sessid)) == NULL) {
			Alert("Not enough memory process_cli():asession->sessid:malloc().\n");
			send_log(t->be, LOG_ALERT, "Not enough memory process_cli():asession->sessid:malloc().\n");
			return;
		}

		memcpy(txn->sessid, buf, len);
		txn->sessid[len] = 0;
	}

	if ((sessid_temp = pool_alloc2(apools.sessid)) == NULL) {
		Alert("Not enough memory process_cli():asession->sessid:malloc().\n");
		send_log(t->be, LOG_ALERT, "Not enough memory process_cli():asession->sessid:malloc().\n");
		return;
	}

	memcpy(sessid_temp, buf, len);
	sessid_temp[len] = 0;

	asession = appsession_hash_lookup(&(t->be->htbl_proxy), sessid_temp);
	/* free previously allocated memory */
	pool_free2(apools.sessid, sessid_temp);

	if (asession != NULL) {
		asession->expire = tick_add_ifset(now_ms, t->be->timeout.appsession);
		if (!(t->be->options2 & PR_O2_AS_REQL))
			asession->request_count++;

		if (asession->serverid != NULL) {
			struct server *srv = t->be->srv;

			while (srv) {
				if (strcmp(srv->id, asession->serverid) == 0) {
					if ((srv->state & SRV_RUNNING) ||
					    (t->be->options & PR_O_PERSIST) ||
					    (t->flags & SN_FORCE_PRST)) {
						/* we found the server and it's usable */
						txn->flags &= ~TX_CK_MASK;
						txn->flags |= (srv->state & SRV_RUNNING) ? TX_CK_VALID : TX_CK_DOWN;
						t->flags |= SN_DIRECT | SN_ASSIGNED;
						t->target = &srv->obj_type;

						break;
					} else {
						txn->flags &= ~TX_CK_MASK;
						txn->flags |= TX_CK_DOWN;
					}
				}
				srv = srv->next;
			}
		}
	}
}

/* Find the end of a cookie value contained between <s> and <e>. It works the
 * same way as with headers above except that the semi-colon also ends a token.
 * See RFC2965 for more information. Note that it requires a valid header to
 * return a valid result.
 */
char *find_cookie_value_end(char *s, const char *e)
{
	int quoted, qdpair;

	quoted = qdpair = 0;
	for (; s < e; s++) {
		if (qdpair)                    qdpair = 0;
		else if (quoted) {
			if (*s == '\\')        qdpair = 1;
			else if (*s == '"')    quoted = 0;
		}
		else if (*s == '"')            quoted = 1;
		else if (*s == ',' || *s == ';') return s;
	}
	return s;
}

/* Delete a value in a header between delimiters <from> and <next> in buffer
 * <buf>. The number of characters displaced is returned, and the pointer to
 * the first delimiter is updated if required. The function tries as much as
 * possible to respect the following principles :
 *  - replace <from> delimiter by the <next> one unless <from> points to a
 *    colon, in which case <next> is simply removed
 *  - set exactly one space character after the new first delimiter, unless
 *    there are not enough characters in the block being moved to do so.
 *  - remove unneeded spaces before the previous delimiter and after the new
 *    one.
 *
 * It is the caller's responsibility to ensure that :
 *   - <from> points to a valid delimiter or the colon ;
 *   - <next> points to a valid delimiter or the final CR/LF ;
 *   - there are non-space chars before <from> ;
 *   - there is a CR/LF at or after <next>.
 */
int del_hdr_value(struct buffer *buf, char **from, char *next)
{
	char *prev = *from;

	if (*prev == ':') {
		/* We're removing the first value, preserve the colon and add a
		 * space if possible.
		 */
		if (!http_is_crlf[(unsigned char)*next])
			next++;
		prev++;
		if (prev < next)
			*prev++ = ' ';

		while (http_is_spht[(unsigned char)*next])
			next++;
	} else {
		/* Remove useless spaces before the old delimiter. */
		while (http_is_spht[(unsigned char)*(prev-1)])
			prev--;
		*from = prev;

		/* copy the delimiter and if possible a space if we're
		 * not at the end of the line.
		 */
		if (!http_is_crlf[(unsigned char)*next]) {
			*prev++ = *next++;
			if (prev + 1 < next)
				*prev++ = ' ';
			while (http_is_spht[(unsigned char)*next])
				next++;
		}
	}
	return buffer_replace2(buf, prev, next, NULL, 0);
}

/*
 * Manage client-side cookie. It can impact performance by about 2% so it is
 * desirable to call it only when needed. This code is quite complex because
 * of the multiple very crappy and ambiguous syntaxes we have to support. it
 * highly recommended not to touch this part without a good reason !
 */
void manage_client_side_cookies(struct session *t, struct channel *req)
{
	struct http_txn *txn = &t->txn;
	int preserve_hdr;
	int cur_idx, old_idx;
	char *hdr_beg, *hdr_end, *hdr_next, *del_from;
	char *prev, *att_beg, *att_end, *equal, *val_beg, *val_end, *next;

	/* Iterate through the headers, we start with the start line. */
	old_idx = 0;
	hdr_next = req->buf->p + hdr_idx_first_pos(&txn->hdr_idx);

	while ((cur_idx = txn->hdr_idx.v[old_idx].next)) {
		struct hdr_idx_elem *cur_hdr;
		int val;

		cur_hdr  = &txn->hdr_idx.v[cur_idx];
		hdr_beg  = hdr_next;
		hdr_end  = hdr_beg + cur_hdr->len;
		hdr_next = hdr_end + cur_hdr->cr + 1;

		/* We have one full header between hdr_beg and hdr_end, and the
		 * next header starts at hdr_next. We're only interested in
		 * "Cookie:" headers.
		 */

		val = http_header_match2(hdr_beg, hdr_end, "Cookie", 6);
		if (!val) {
			old_idx = cur_idx;
			continue;
		}

		del_from = NULL;  /* nothing to be deleted */
		preserve_hdr = 0; /* assume we may kill the whole header */

		/* Now look for cookies. Conforming to RFC2109, we have to support
		 * attributes whose name begin with a '$', and associate them with
		 * the right cookie, if we want to delete this cookie.
		 * So there are 3 cases for each cookie read :
		 * 1) it's a special attribute, beginning with a '$' : ignore it.
		 * 2) it's a server id cookie that we *MAY* want to delete : save
		 *    some pointers on it (last semi-colon, beginning of cookie...)
		 * 3) it's an application cookie : we *MAY* have to delete a previous
		 *    "special" cookie.
		 * At the end of loop, if a "special" cookie remains, we may have to
		 * remove it. If no application cookie persists in the header, we
		 * *MUST* delete it.
		 *
		 * Note: RFC2965 is unclear about the processing of spaces around
		 * the equal sign in the ATTR=VALUE form. A careful inspection of
		 * the RFC explicitly allows spaces before it, and not within the
		 * tokens (attrs or values). An inspection of RFC2109 allows that
		 * too but section 10.1.3 lets one think that spaces may be allowed
		 * after the equal sign too, resulting in some (rare) buggy
		 * implementations trying to do that. So let's do what servers do.
		 * Latest ietf draft forbids spaces all around. Also, earlier RFCs
		 * allowed quoted strings in values, with any possible character
		 * after a backslash, including control chars and delimitors, which
		 * causes parsing to become ambiguous. Browsers also allow spaces
		 * within values even without quotes.
		 *
		 * We have to keep multiple pointers in order to support cookie
		 * removal at the beginning, middle or end of header without
		 * corrupting the header. All of these headers are valid :
		 *
		 * Cookie:NAME1=VALUE1;NAME2=VALUE2;NAME3=VALUE3\r\n
		 * Cookie:NAME1=VALUE1;NAME2_ONLY ;NAME3=VALUE3\r\n
		 * Cookie:    NAME1  =  VALUE 1  ; NAME2 = VALUE2 ; NAME3 = VALUE3\r\n
		 * |     |    |    | |  |      | |                                |
		 * |     |    |    | |  |      | |                     hdr_end <--+
		 * |     |    |    | |  |      | +--> next
		 * |     |    |    | |  |      +----> val_end
		 * |     |    |    | |  +-----------> val_beg
		 * |     |    |    | +--------------> equal
		 * |     |    |    +----------------> att_end
		 * |     |    +---------------------> att_beg
		 * |     +--------------------------> prev
		 * +--------------------------------> hdr_beg
		 */

		for (prev = hdr_beg + 6; prev < hdr_end; prev = next) {
			/* Iterate through all cookies on this line */

			/* find att_beg */
			att_beg = prev + 1;
			while (att_beg < hdr_end && http_is_spht[(unsigned char)*att_beg])
				att_beg++;

			/* find att_end : this is the first character after the last non
			 * space before the equal. It may be equal to hdr_end.
			 */
			equal = att_end = att_beg;

			while (equal < hdr_end) {
				if (*equal == '=' || *equal == ',' || *equal == ';')
					break;
				if (http_is_spht[(unsigned char)*equal++])
					continue;
				att_end = equal;
			}

			/* here, <equal> points to '=', a delimitor or the end. <att_end>
			 * is between <att_beg> and <equal>, both may be identical.
			 */

			/* look for end of cookie if there is an equal sign */
			if (equal < hdr_end && *equal == '=') {
				/* look for the beginning of the value */
				val_beg = equal + 1;
				while (val_beg < hdr_end && http_is_spht[(unsigned char)*val_beg])
					val_beg++;

				/* find the end of the value, respecting quotes */
				next = find_cookie_value_end(val_beg, hdr_end);

				/* make val_end point to the first white space or delimitor after the value */
				val_end = next;
				while (val_end > val_beg && http_is_spht[(unsigned char)*(val_end - 1)])
					val_end--;
			} else {
				val_beg = val_end = next = equal;
			}

			/* We have nothing to do with attributes beginning with '$'. However,
			 * they will automatically be removed if a header before them is removed,
			 * since they're supposed to be linked together.
			 */
			if (*att_beg == '$')
				continue;

			/* Ignore cookies with no equal sign */
			if (equal == next) {
				/* This is not our cookie, so we must preserve it. But if we already
				 * scheduled another cookie for removal, we cannot remove the
				 * complete header, but we can remove the previous block itself.
				 */
				preserve_hdr = 1;
				if (del_from != NULL) {
					int delta = del_hdr_value(req->buf, &del_from, prev);
					val_end  += delta;
					next     += delta;
					hdr_end  += delta;
					hdr_next += delta;
					cur_hdr->len += delta;
					http_msg_move_end(&txn->req, delta);
					prev     = del_from;
					del_from = NULL;
				}
				continue;
			}

			/* if there are spaces around the equal sign, we need to
			 * strip them otherwise we'll get trouble for cookie captures,
			 * or even for rewrites. Since this happens extremely rarely,
			 * it does not hurt performance.
			 */
			if (unlikely(att_end != equal || val_beg > equal + 1)) {
				int stripped_before = 0;
				int stripped_after = 0;

				if (att_end != equal) {
					stripped_before = buffer_replace2(req->buf, att_end, equal, NULL, 0);
					equal   += stripped_before;
					val_beg += stripped_before;
				}

				if (val_beg > equal + 1) {
					stripped_after = buffer_replace2(req->buf, equal + 1, val_beg, NULL, 0);
					val_beg += stripped_after;
					stripped_before += stripped_after;
				}

				val_end      += stripped_before;
				next         += stripped_before;
				hdr_end      += stripped_before;
				hdr_next     += stripped_before;
				cur_hdr->len += stripped_before;
				http_msg_move_end(&txn->req, stripped_before);
			}
			/* now everything is as on the diagram above */

			/* First, let's see if we want to capture this cookie. We check
			 * that we don't already have a client side cookie, because we
			 * can only capture one. Also as an optimisation, we ignore
			 * cookies shorter than the declared name.
			 */
			if (t->fe->capture_name != NULL && txn->cli_cookie == NULL &&
			    (val_end - att_beg >= t->fe->capture_namelen) &&
			    memcmp(att_beg, t->fe->capture_name, t->fe->capture_namelen) == 0) {
				int log_len = val_end - att_beg;

				if ((txn->cli_cookie = pool_alloc2(pool2_capture)) == NULL) {
					Alert("HTTP logging : out of memory.\n");
				} else {
					if (log_len > t->fe->capture_len)
						log_len = t->fe->capture_len;
					memcpy(txn->cli_cookie, att_beg, log_len);
					txn->cli_cookie[log_len] = 0;
				}
			}

			/* Persistence cookies in passive, rewrite or insert mode have the
			 * following form :
			 *
			 *    Cookie: NAME=SRV[|<lastseen>[|<firstseen>]]
			 *
			 * For cookies in prefix mode, the form is :
			 *
			 *    Cookie: NAME=SRV~VALUE
			 */
			if ((att_end - att_beg == t->be->cookie_len) && (t->be->cookie_name != NULL) &&
			    (memcmp(att_beg, t->be->cookie_name, att_end - att_beg) == 0)) {
				struct server *srv = t->be->srv;
				char *delim;

				/* if we're in cookie prefix mode, we'll search the delimitor so that we
				 * have the server ID between val_beg and delim, and the original cookie between
				 * delim+1 and val_end. Otherwise, delim==val_end :
				 *
				 * Cookie: NAME=SRV;          # in all but prefix modes
				 * Cookie: NAME=SRV~OPAQUE ;  # in prefix mode
				 * |      ||   ||  |      |+-> next
				 * |      ||   ||  |      +--> val_end
				 * |      ||   ||  +---------> delim
				 * |      ||   |+------------> val_beg
				 * |      ||   +-------------> att_end = equal
				 * |      |+-----------------> att_beg
				 * |      +------------------> prev
				 * +-------------------------> hdr_beg
				 */

				if (t->be->ck_opts & PR_CK_PFX) {
					for (delim = val_beg; delim < val_end; delim++)
						if (*delim == COOKIE_DELIM)
							break;
				} else {
					char *vbar1;
					delim = val_end;
					/* Now check if the cookie contains a date field, which would
					 * appear after a vertical bar ('|') just after the server name
					 * and before the delimiter.
					 */
					vbar1 = memchr(val_beg, COOKIE_DELIM_DATE, val_end - val_beg);
					if (vbar1) {
						/* OK, so left of the bar is the server's cookie and
						 * right is the last seen date. It is a base64 encoded
						 * 30-bit value representing the UNIX date since the
						 * epoch in 4-second quantities.
						 */
						int val;
						delim = vbar1++;
						if (val_end - vbar1 >= 5) {
							val = b64tos30(vbar1);
							if (val > 0)
								txn->cookie_last_date = val << 2;
						}
						/* look for a second vertical bar */
						vbar1 = memchr(vbar1, COOKIE_DELIM_DATE, val_end - vbar1);
						if (vbar1 && (val_end - vbar1 > 5)) {
							val = b64tos30(vbar1 + 1);
							if (val > 0)
								txn->cookie_first_date = val << 2;
						}
					}
				}

				/* if the cookie has an expiration date and the proxy wants to check
				 * it, then we do that now. We first check if the cookie is too old,
				 * then only if it has expired. We detect strict overflow because the
				 * time resolution here is not great (4 seconds). Cookies with dates
				 * in the future are ignored if their offset is beyond one day. This
				 * allows an admin to fix timezone issues without expiring everyone
				 * and at the same time avoids keeping unwanted side effects for too
				 * long.
				 */
				if (txn->cookie_first_date && t->be->cookie_maxlife &&
				    (((signed)(date.tv_sec - txn->cookie_first_date) > (signed)t->be->cookie_maxlife) ||
				     ((signed)(txn->cookie_first_date - date.tv_sec) > 86400))) {
					txn->flags &= ~TX_CK_MASK;
					txn->flags |= TX_CK_OLD;
					delim = val_beg; // let's pretend we have not found the cookie
					txn->cookie_first_date = 0;
					txn->cookie_last_date = 0;
				}
				else if (txn->cookie_last_date && t->be->cookie_maxidle &&
					 (((signed)(date.tv_sec - txn->cookie_last_date) > (signed)t->be->cookie_maxidle) ||
					  ((signed)(txn->cookie_last_date - date.tv_sec) > 86400))) {
					txn->flags &= ~TX_CK_MASK;
					txn->flags |= TX_CK_EXPIRED;
					delim = val_beg; // let's pretend we have not found the cookie
					txn->cookie_first_date = 0;
					txn->cookie_last_date = 0;
				}

				/* Here, we'll look for the first running server which supports the cookie.
				 * This allows to share a same cookie between several servers, for example
				 * to dedicate backup servers to specific servers only.
				 * However, to prevent clients from sticking to cookie-less backup server
				 * when they have incidentely learned an empty cookie, we simply ignore
				 * empty cookies and mark them as invalid.
				 * The same behaviour is applied when persistence must be ignored.
				 */
				if ((delim == val_beg) || (t->flags & (SN_IGNORE_PRST | SN_ASSIGNED)))
					srv = NULL;

				while (srv) {
					if (srv->cookie && (srv->cklen == delim - val_beg) &&
					    !memcmp(val_beg, srv->cookie, delim - val_beg)) {
						if ((srv->state & SRV_RUNNING) ||
						    (t->be->options & PR_O_PERSIST) ||
						    (t->flags & SN_FORCE_PRST)) {
							/* we found the server and we can use it */
							txn->flags &= ~TX_CK_MASK;
							txn->flags |= (srv->state & SRV_RUNNING) ? TX_CK_VALID : TX_CK_DOWN;
							t->flags |= SN_DIRECT | SN_ASSIGNED;
							t->target = &srv->obj_type;
							break;
						} else {
							/* we found a server, but it's down,
							 * mark it as such and go on in case
							 * another one is available.
							 */
							txn->flags &= ~TX_CK_MASK;
							txn->flags |= TX_CK_DOWN;
						}
					}
					srv = srv->next;
				}

				if (!srv && !(txn->flags & (TX_CK_DOWN|TX_CK_EXPIRED|TX_CK_OLD))) {
					/* no server matched this cookie or we deliberately skipped it */
					txn->flags &= ~TX_CK_MASK;
					if ((t->flags & (SN_IGNORE_PRST | SN_ASSIGNED)))
						txn->flags |= TX_CK_UNUSED;
					else
						txn->flags |= TX_CK_INVALID;
				}

				/* depending on the cookie mode, we may have to either :
				 * - delete the complete cookie if we're in insert+indirect mode, so that
				 *   the server never sees it ;
				 * - remove the server id from the cookie value, and tag the cookie as an
				 *   application cookie so that it does not get accidentely removed later,
				 *   if we're in cookie prefix mode
				 */
				if ((t->be->ck_opts & PR_CK_PFX) && (delim != val_end)) {
					int delta; /* negative */

					delta = buffer_replace2(req->buf, val_beg, delim + 1, NULL, 0);
					val_end  += delta;
					next     += delta;
					hdr_end  += delta;
					hdr_next += delta;
					cur_hdr->len += delta;
					http_msg_move_end(&txn->req, delta);

					del_from = NULL;
					preserve_hdr = 1; /* we want to keep this cookie */
				}
				else if (del_from == NULL &&
					 (t->be->ck_opts & (PR_CK_INS | PR_CK_IND)) == (PR_CK_INS | PR_CK_IND)) {
					del_from = prev;
				}
			} else {
				/* This is not our cookie, so we must preserve it. But if we already
				 * scheduled another cookie for removal, we cannot remove the
				 * complete header, but we can remove the previous block itself.
				 */
				preserve_hdr = 1;

				if (del_from != NULL) {
					int delta = del_hdr_value(req->buf, &del_from, prev);
					if (att_beg >= del_from)
						att_beg += delta;
					if (att_end >= del_from)
						att_end += delta;
					val_beg  += delta;
					val_end  += delta;
					next     += delta;
					hdr_end  += delta;
					hdr_next += delta;
					cur_hdr->len += delta;
					http_msg_move_end(&txn->req, delta);
					prev     = del_from;
					del_from = NULL;
				}
			}

			/* Look for the appsession cookie unless persistence must be ignored */
			if (!(t->flags & SN_IGNORE_PRST) && (t->be->appsession_name != NULL)) {
				int cmp_len, value_len;
				char *value_begin;

				if (t->be->options2 & PR_O2_AS_PFX) {
					cmp_len     = MIN(val_end - att_beg, t->be->appsession_name_len);
					value_begin = att_beg + t->be->appsession_name_len;
					value_len   = val_end - att_beg - t->be->appsession_name_len;
				} else {
					cmp_len     = att_end - att_beg;
					value_begin = val_beg;
					value_len   = val_end - val_beg;
				}

				/* let's see if the cookie is our appcookie */
				if (cmp_len == t->be->appsession_name_len &&
				    memcmp(att_beg, t->be->appsession_name, cmp_len) == 0) {
					manage_client_side_appsession(t, value_begin, value_len);
				}
			}

			/* continue with next cookie on this header line */
			att_beg = next;
		} /* for each cookie */

		/* There are no more cookies on this line.
		 * We may still have one (or several) marked for deletion at the
		 * end of the line. We must do this now in two ways :
		 *  - if some cookies must be preserved, we only delete from the
		 *    mark to the end of line ;
		 *  - if nothing needs to be preserved, simply delete the whole header
		 */
		if (del_from) {
			int delta;
			if (preserve_hdr) {
				delta = del_hdr_value(req->buf, &del_from, hdr_end);
				hdr_end = del_from;
				cur_hdr->len += delta;
			} else {
				delta = buffer_replace2(req->buf, hdr_beg, hdr_next, NULL, 0);

				/* FIXME: this should be a separate function */
				txn->hdr_idx.v[old_idx].next = cur_hdr->next;
				txn->hdr_idx.used--;
				cur_hdr->len = 0;
				cur_idx = old_idx;
			}
			hdr_next += delta;
			http_msg_move_end(&txn->req, delta);
		}

		/* check next header */
		old_idx = cur_idx;
	}
}


/* Iterate the same filter through all response headers contained in <rtr>.
 * Returns 1 if this filter can be stopped upon return, otherwise 0.
 */
int apply_filter_to_resp_headers(struct session *t, struct channel *rtr, struct hdr_exp *exp)
{
	char term;
	char *cur_ptr, *cur_end, *cur_next;
	int cur_idx, old_idx, last_hdr;
	struct http_txn *txn = &t->txn;
	struct hdr_idx_elem *cur_hdr;
	int delta;

	last_hdr = 0;

	cur_next = rtr->buf->p + hdr_idx_first_pos(&txn->hdr_idx);
	old_idx = 0;

	while (!last_hdr) {
		if (unlikely(txn->flags & TX_SVDENY))
			return 1;
		else if (unlikely(txn->flags & TX_SVALLOW) &&
			 (exp->action == ACT_ALLOW ||
			  exp->action == ACT_DENY))
			return 0;

		cur_idx = txn->hdr_idx.v[old_idx].next;
		if (!cur_idx)
			break;

		cur_hdr  = &txn->hdr_idx.v[cur_idx];
		cur_ptr  = cur_next;
		cur_end  = cur_ptr + cur_hdr->len;
		cur_next = cur_end + cur_hdr->cr + 1;

		/* Now we have one header between cur_ptr and cur_end,
		 * and the next header starts at cur_next.
		 */

		/* The annoying part is that pattern matching needs
		 * that we modify the contents to null-terminate all
		 * strings before testing them.
		 */

		term = *cur_end;
		*cur_end = '\0';

		if (regexec(exp->preg, cur_ptr, MAX_MATCH, pmatch, 0) == 0) {
			switch (exp->action) {
			case ACT_ALLOW:
				txn->flags |= TX_SVALLOW;
				last_hdr = 1;
				break;

			case ACT_DENY:
				txn->flags |= TX_SVDENY;
				last_hdr = 1;
				break;

			case ACT_REPLACE:
				trash.len = exp_replace(trash.str, cur_ptr, exp->replace, pmatch);
				delta = buffer_replace2(rtr->buf, cur_ptr, cur_end, trash.str, trash.len);
				/* FIXME: if the user adds a newline in the replacement, the
				 * index will not be recalculated for now, and the new line
				 * will not be counted as a new header.
				 */

				cur_end += delta;
				cur_next += delta;
				cur_hdr->len += delta;
				http_msg_move_end(&txn->rsp, delta);
				break;

			case ACT_REMOVE:
				delta = buffer_replace2(rtr->buf, cur_ptr, cur_next, NULL, 0);
				cur_next += delta;

				http_msg_move_end(&txn->rsp, delta);
				txn->hdr_idx.v[old_idx].next = cur_hdr->next;
				txn->hdr_idx.used--;
				cur_hdr->len = 0;
				cur_end = NULL; /* null-term has been rewritten */
				cur_idx = old_idx;
				break;

			}
		}
		if (cur_end)
			*cur_end = term; /* restore the string terminator */

		/* keep the link from this header to next one in case of later
		 * removal of next header.
		 */
		old_idx = cur_idx;
	}
	return 0;
}


/* Apply the filter to the status line in the response buffer <rtr>.
 * Returns 0 if nothing has been done, 1 if the filter has been applied,
 * or -1 if a replacement resulted in an invalid status line.
 */
int apply_filter_to_sts_line(struct session *t, struct channel *rtr, struct hdr_exp *exp)
{
	char term;
	char *cur_ptr, *cur_end;
	int done;
	struct http_txn *txn = &t->txn;
	int delta;


	if (unlikely(txn->flags & TX_SVDENY))
		return 1;
	else if (unlikely(txn->flags & TX_SVALLOW) &&
		 (exp->action == ACT_ALLOW ||
		  exp->action == ACT_DENY))
		return 0;
	else if (exp->action == ACT_REMOVE)
		return 0;

	done = 0;

	cur_ptr = rtr->buf->p;
	cur_end = cur_ptr + txn->rsp.sl.st.l;

	/* Now we have the status line between cur_ptr and cur_end */

	/* The annoying part is that pattern matching needs
	 * that we modify the contents to null-terminate all
	 * strings before testing them.
	 */

	term = *cur_end;
	*cur_end = '\0';

	if (regexec(exp->preg, cur_ptr, MAX_MATCH, pmatch, 0) == 0) {
		switch (exp->action) {
		case ACT_ALLOW:
			txn->flags |= TX_SVALLOW;
			done = 1;
			break;

		case ACT_DENY:
			txn->flags |= TX_SVDENY;
			done = 1;
			break;

		case ACT_REPLACE:
			*cur_end = term; /* restore the string terminator */
			trash.len = exp_replace(trash.str, cur_ptr, exp->replace, pmatch);
			delta = buffer_replace2(rtr->buf, cur_ptr, cur_end, trash.str, trash.len);
			/* FIXME: if the user adds a newline in the replacement, the
			 * index will not be recalculated for now, and the new line
			 * will not be counted as a new header.
			 */

			http_msg_move_end(&txn->rsp, delta);
			cur_end += delta;
			cur_end = (char *)http_parse_stsline(&txn->rsp,
							     HTTP_MSG_RPVER,
							     cur_ptr, cur_end + 1,
							     NULL, NULL);
			if (unlikely(!cur_end))
				return -1;

			/* we have a full respnse and we know that we have either a CR
			 * or an LF at <ptr>.
			 */
			txn->status = strl2ui(rtr->buf->p + txn->rsp.sl.st.c, txn->rsp.sl.st.c_l);
			hdr_idx_set_start(&txn->hdr_idx, txn->rsp.sl.st.l, *cur_end == '\r');
			/* there is no point trying this regex on headers */
			return 1;
		}
	}
	*cur_end = term; /* restore the string terminator */
	return done;
}



/*
 * Apply all the resp filters of proxy <px> to all headers in buffer <rtr> of session <s>.
 * Returns 0 if everything is alright, or -1 in case a replacement lead to an
 * unparsable response.
 */
int apply_filters_to_response(struct session *s, struct channel *rtr, struct proxy *px)
{
	struct http_txn *txn = &s->txn;
	struct hdr_exp *exp;

	for (exp = px->rsp_exp; exp; exp = exp->next) {
		int ret;

		/*
		 * The interleaving of transformations and verdicts
		 * makes it difficult to decide to continue or stop
		 * the evaluation.
		 */

		if (txn->flags & TX_SVDENY)
			break;

		if ((txn->flags & TX_SVALLOW) &&
		    (exp->action == ACT_ALLOW || exp->action == ACT_DENY ||
		     exp->action == ACT_PASS)) {
			exp = exp->next;
			continue;
		}

		/* if this filter had a condition, evaluate it now and skip to
		 * next filter if the condition does not match.
		 */
		if (exp->cond) {
			ret = acl_exec_cond(exp->cond, px, s, txn, SMP_OPT_DIR_RES|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (((struct acl_cond *)exp->cond)->pol == ACL_COND_UNLESS)
				ret = !ret;
			if (!ret)
				continue;
		}

		/* Apply the filter to the status line. */
		ret = apply_filter_to_sts_line(s, rtr, exp);
		if (unlikely(ret < 0))
			return -1;

		if (likely(ret == 0)) {
			/* The filter did not match the response, it can be
			 * iterated through all headers.
			 */
			apply_filter_to_resp_headers(s, rtr, exp);
		}
	}
	return 0;
}


/*
 * Manage server-side cookies. It can impact performance by about 2% so it is
 * desirable to call it only when needed. This function is also used when we
 * just need to know if there is a cookie (eg: for check-cache).
 */
void manage_server_side_cookies(struct session *t, struct channel *res)
{
	struct http_txn *txn = &t->txn;
	struct server *srv;
	int is_cookie2;
	int cur_idx, old_idx, delta;
	char *hdr_beg, *hdr_end, *hdr_next;
	char *prev, *att_beg, *att_end, *equal, *val_beg, *val_end, *next;

	/* Iterate through the headers.
	 * we start with the start line.
	 */
	old_idx = 0;
	hdr_next = res->buf->p + hdr_idx_first_pos(&txn->hdr_idx);

	while ((cur_idx = txn->hdr_idx.v[old_idx].next)) {
		struct hdr_idx_elem *cur_hdr;
		int val;

		cur_hdr  = &txn->hdr_idx.v[cur_idx];
		hdr_beg  = hdr_next;
		hdr_end  = hdr_beg + cur_hdr->len;
		hdr_next = hdr_end + cur_hdr->cr + 1;

		/* We have one full header between hdr_beg and hdr_end, and the
		 * next header starts at hdr_next. We're only interested in
		 * "Set-Cookie" and "Set-Cookie2" headers.
		 */

		is_cookie2 = 0;
		prev = hdr_beg + 10;
		val = http_header_match2(hdr_beg, hdr_end, "Set-Cookie", 10);
		if (!val) {
			val = http_header_match2(hdr_beg, hdr_end, "Set-Cookie2", 11);
			if (!val) {
				old_idx = cur_idx;
				continue;
			}
			is_cookie2 = 1;
			prev = hdr_beg + 11;
		}

		/* OK, right now we know we have a Set-Cookie* at hdr_beg, and
		 * <prev> points to the colon.
		 */
		txn->flags |= TX_SCK_PRESENT;

		/* Maybe we only wanted to see if there was a Set-Cookie (eg:
		 * check-cache is enabled) and we are not interested in checking
		 * them. Warning, the cookie capture is declared in the frontend.
		 */
		if (t->be->cookie_name == NULL &&
		    t->be->appsession_name == NULL &&
		    t->fe->capture_name == NULL)
			return;

		/* OK so now we know we have to process this response cookie.
		 * The format of the Set-Cookie header is slightly different
		 * from the format of the Cookie header in that it does not
		 * support the comma as a cookie delimiter (thus the header
		 * cannot be folded) because the Expires attribute described in
		 * the original Netscape's spec may contain an unquoted date
		 * with a comma inside. We have to live with this because
		 * many browsers don't support Max-Age and some browsers don't
		 * support quoted strings. However the Set-Cookie2 header is
		 * clean.
		 *
		 * We have to keep multiple pointers in order to support cookie
		 * removal at the beginning, middle or end of header without
		 * corrupting the header (in case of set-cookie2). A special
		 * pointer, <scav> points to the beginning of the set-cookie-av
		 * fields after the first semi-colon. The <next> pointer points
		 * either to the end of line (set-cookie) or next unquoted comma
		 * (set-cookie2). All of these headers are valid :
		 *
		 * Set-Cookie:    NAME1  =  VALUE 1  ; Secure; Path="/"\r\n
		 * Set-Cookie:NAME=VALUE; Secure; Expires=Thu, 01-Jan-1970 00:00:01 GMT\r\n
		 * Set-Cookie: NAME = VALUE ; Secure; Expires=Thu, 01-Jan-1970 00:00:01 GMT\r\n
		 * Set-Cookie2: NAME1 = VALUE 1 ; Max-Age=0, NAME2=VALUE2; Discard\r\n
		 * |          | |   | | |     | |          |                      |
		 * |          | |   | | |     | |          +-> next    hdr_end <--+
		 * |          | |   | | |     | +------------> scav
		 * |          | |   | | |     +--------------> val_end
		 * |          | |   | | +--------------------> val_beg
		 * |          | |   | +----------------------> equal
		 * |          | |   +------------------------> att_end
		 * |          | +----------------------------> att_beg
		 * |          +------------------------------> prev
		 * +-----------------------------------------> hdr_beg
		 */

		for (; prev < hdr_end; prev = next) {
			/* Iterate through all cookies on this line */

			/* find att_beg */
			att_beg = prev + 1;
			while (att_beg < hdr_end && http_is_spht[(unsigned char)*att_beg])
				att_beg++;

			/* find att_end : this is the first character after the last non
			 * space before the equal. It may be equal to hdr_end.
			 */
			equal = att_end = att_beg;

			while (equal < hdr_end) {
				if (*equal == '=' || *equal == ';' || (is_cookie2 && *equal == ','))
					break;
				if (http_is_spht[(unsigned char)*equal++])
					continue;
				att_end = equal;
			}

			/* here, <equal> points to '=', a delimitor or the end. <att_end>
			 * is between <att_beg> and <equal>, both may be identical.
			 */

			/* look for end of cookie if there is an equal sign */
			if (equal < hdr_end && *equal == '=') {
				/* look for the beginning of the value */
				val_beg = equal + 1;
				while (val_beg < hdr_end && http_is_spht[(unsigned char)*val_beg])
					val_beg++;

				/* find the end of the value, respecting quotes */
				next = find_cookie_value_end(val_beg, hdr_end);

				/* make val_end point to the first white space or delimitor after the value */
				val_end = next;
				while (val_end > val_beg && http_is_spht[(unsigned char)*(val_end - 1)])
					val_end--;
			} else {
				/* <equal> points to next comma, semi-colon or EOL */
				val_beg = val_end = next = equal;
			}

			if (next < hdr_end) {
				/* Set-Cookie2 supports multiple cookies, and <next> points to
				 * a colon or semi-colon before the end. So skip all attr-value
				 * pairs and look for the next comma. For Set-Cookie, since
				 * commas are permitted in values, skip to the end.
				 */
				if (is_cookie2)
					next = find_hdr_value_end(next, hdr_end);
				else
					next = hdr_end;
			}

			/* Now everything is as on the diagram above */

			/* Ignore cookies with no equal sign */
			if (equal == val_end)
				continue;

			/* If there are spaces around the equal sign, we need to
			 * strip them otherwise we'll get trouble for cookie captures,
			 * or even for rewrites. Since this happens extremely rarely,
			 * it does not hurt performance.
			 */
			if (unlikely(att_end != equal || val_beg > equal + 1)) {
				int stripped_before = 0;
				int stripped_after = 0;

				if (att_end != equal) {
					stripped_before = buffer_replace2(res->buf, att_end, equal, NULL, 0);
					equal   += stripped_before;
					val_beg += stripped_before;
				}

				if (val_beg > equal + 1) {
					stripped_after = buffer_replace2(res->buf, equal + 1, val_beg, NULL, 0);
					val_beg += stripped_after;
					stripped_before += stripped_after;
				}

				val_end      += stripped_before;
				next         += stripped_before;
				hdr_end      += stripped_before;
				hdr_next     += stripped_before;
				cur_hdr->len += stripped_before;
				http_msg_move_end(&txn->rsp, stripped_before);
			}

			/* First, let's see if we want to capture this cookie. We check
			 * that we don't already have a server side cookie, because we
			 * can only capture one. Also as an optimisation, we ignore
			 * cookies shorter than the declared name.
			 */
			if (t->fe->capture_name != NULL &&
			    txn->srv_cookie == NULL &&
			    (val_end - att_beg >= t->fe->capture_namelen) &&
			    memcmp(att_beg, t->fe->capture_name, t->fe->capture_namelen) == 0) {
				int log_len = val_end - att_beg;
				if ((txn->srv_cookie = pool_alloc2(pool2_capture)) == NULL) {
					Alert("HTTP logging : out of memory.\n");
				}
				else {
					if (log_len > t->fe->capture_len)
						log_len = t->fe->capture_len;
					memcpy(txn->srv_cookie, att_beg, log_len);
					txn->srv_cookie[log_len] = 0;
				}
			}

			srv = objt_server(t->target);
			/* now check if we need to process it for persistence */
			if (!(t->flags & SN_IGNORE_PRST) &&
			    (att_end - att_beg == t->be->cookie_len) && (t->be->cookie_name != NULL) &&
			    (memcmp(att_beg, t->be->cookie_name, att_end - att_beg) == 0)) {
				/* assume passive cookie by default */
				txn->flags &= ~TX_SCK_MASK;
				txn->flags |= TX_SCK_FOUND;
			
				/* If the cookie is in insert mode on a known server, we'll delete
				 * this occurrence because we'll insert another one later.
				 * We'll delete it too if the "indirect" option is set and we're in
				 * a direct access.
				 */
				if (t->be->ck_opts & PR_CK_PSV) {
					/* The "preserve" flag was set, we don't want to touch the
					 * server's cookie.
					 */
				}
				else if ((srv && (t->be->ck_opts & PR_CK_INS)) ||
				    ((t->flags & SN_DIRECT) && (t->be->ck_opts & PR_CK_IND))) {
					/* this cookie must be deleted */
					if (*prev == ':' && next == hdr_end) {
						/* whole header */
						delta = buffer_replace2(res->buf, hdr_beg, hdr_next, NULL, 0);
						txn->hdr_idx.v[old_idx].next = cur_hdr->next;
						txn->hdr_idx.used--;
						cur_hdr->len = 0;
						cur_idx = old_idx;
						hdr_next += delta;
						http_msg_move_end(&txn->rsp, delta);
						/* note: while both invalid now, <next> and <hdr_end>
						 * are still equal, so the for() will stop as expected.
						 */
					} else {
						/* just remove the value */
						int delta = del_hdr_value(res->buf, &prev, next);
						next      = prev;
						hdr_end  += delta;
						hdr_next += delta;
						cur_hdr->len += delta;
						http_msg_move_end(&txn->rsp, delta);
					}
					txn->flags &= ~TX_SCK_MASK;
					txn->flags |= TX_SCK_DELETED;
					/* and go on with next cookie */
				}
				else if (srv && srv->cookie && (t->be->ck_opts & PR_CK_RW)) {
					/* replace bytes val_beg->val_end with the cookie name associated
					 * with this server since we know it.
					 */
					delta = buffer_replace2(res->buf, val_beg, val_end, srv->cookie, srv->cklen);
					next     += delta;
					hdr_end  += delta;
					hdr_next += delta;
					cur_hdr->len += delta;
					http_msg_move_end(&txn->rsp, delta);

					txn->flags &= ~TX_SCK_MASK;
					txn->flags |= TX_SCK_REPLACED;
				}
				else if (srv && srv->cookie && (t->be->ck_opts & PR_CK_PFX)) {
					/* insert the cookie name associated with this server
					 * before existing cookie, and insert a delimiter between them..
					 */
					delta = buffer_replace2(res->buf, val_beg, val_beg, srv->cookie, srv->cklen + 1);
					next     += delta;
					hdr_end  += delta;
					hdr_next += delta;
					cur_hdr->len += delta;
					http_msg_move_end(&txn->rsp, delta);

					val_beg[srv->cklen] = COOKIE_DELIM;
					txn->flags &= ~TX_SCK_MASK;
					txn->flags |= TX_SCK_REPLACED;
				}
			}
			/* next, let's see if the cookie is our appcookie, unless persistence must be ignored */
			else if (!(t->flags & SN_IGNORE_PRST) && (t->be->appsession_name != NULL)) {
				int cmp_len, value_len;
				char *value_begin;

				if (t->be->options2 & PR_O2_AS_PFX) {
					cmp_len = MIN(val_end - att_beg, t->be->appsession_name_len);
					value_begin = att_beg + t->be->appsession_name_len;
					value_len = MIN(t->be->appsession_len, val_end - att_beg - t->be->appsession_name_len);
				} else {
					cmp_len = att_end - att_beg;
					value_begin = val_beg;
					value_len = MIN(t->be->appsession_len, val_end - val_beg);
				}

				if ((cmp_len == t->be->appsession_name_len) &&
				    (memcmp(att_beg, t->be->appsession_name, t->be->appsession_name_len) == 0)) {
					/* free a possibly previously allocated memory */
					pool_free2(apools.sessid, txn->sessid);

					/* Store the sessid in the session for future use */
					if ((txn->sessid = pool_alloc2(apools.sessid)) == NULL) {
						Alert("Not enough Memory process_srv():asession->sessid:malloc().\n");
						send_log(t->be, LOG_ALERT, "Not enough Memory process_srv():asession->sessid:malloc().\n");
						return;
					}
					memcpy(txn->sessid, value_begin, value_len);
					txn->sessid[value_len] = 0;
				}
			}
			/* that's done for this cookie, check the next one on the same
			 * line when next != hdr_end (only if is_cookie2).
			 */
		}
		/* check next header */
		old_idx = cur_idx;
	}

	if (txn->sessid != NULL) {
		appsess *asession = NULL;
		/* only do insert, if lookup fails */
		asession = appsession_hash_lookup(&(t->be->htbl_proxy), txn->sessid);
		if (asession == NULL) {
			size_t server_id_len;
			if ((asession = pool_alloc2(pool2_appsess)) == NULL) {
				Alert("Not enough Memory process_srv():asession:calloc().\n");
				send_log(t->be, LOG_ALERT, "Not enough Memory process_srv():asession:calloc().\n");
				return;
			}
			asession->serverid = NULL; /* to avoid a double free in case of allocation error */

			if ((asession->sessid = pool_alloc2(apools.sessid)) == NULL) {
				Alert("Not enough Memory process_srv():asession->sessid:malloc().\n");
				send_log(t->be, LOG_ALERT, "Not enough Memory process_srv():asession->sessid:malloc().\n");
				t->be->htbl_proxy.destroy(asession);
				return;
			}
			memcpy(asession->sessid, txn->sessid, t->be->appsession_len);
			asession->sessid[t->be->appsession_len] = 0;

			server_id_len = strlen(objt_server(t->target)->id) + 1;
			if ((asession->serverid = pool_alloc2(apools.serverid)) == NULL) {
				Alert("Not enough Memory process_srv():asession->serverid:malloc().\n");
				send_log(t->be, LOG_ALERT, "Not enough Memory process_srv():asession->sessid:malloc().\n");
				t->be->htbl_proxy.destroy(asession);
				return;
			}
			asession->serverid[0] = '\0';
			memcpy(asession->serverid, objt_server(t->target)->id, server_id_len);

			asession->request_count = 0;
			appsession_hash_insert(&(t->be->htbl_proxy), asession);
		}

		asession->expire = tick_add_ifset(now_ms, t->be->timeout.appsession);
		asession->request_count++;
	}
}


/*
 * Check if response is cacheable or not. Updates t->flags.
 */
void check_response_for_cacheability(struct session *t, struct channel *rtr)
{
	struct http_txn *txn = &t->txn;
	char *p1, *p2;

	char *cur_ptr, *cur_end, *cur_next;
	int cur_idx;

	if (!(txn->flags & TX_CACHEABLE))
		return;

	/* Iterate through the headers.
	 * we start with the start line.
	 */
	cur_idx = 0;
	cur_next = rtr->buf->p + hdr_idx_first_pos(&txn->hdr_idx);

	while ((cur_idx = txn->hdr_idx.v[cur_idx].next)) {
		struct hdr_idx_elem *cur_hdr;
		int val;

		cur_hdr  = &txn->hdr_idx.v[cur_idx];
		cur_ptr  = cur_next;
		cur_end  = cur_ptr + cur_hdr->len;
		cur_next = cur_end + cur_hdr->cr + 1;

		/* We have one full header between cur_ptr and cur_end, and the
		 * next header starts at cur_next. We're only interested in
		 * "Cookie:" headers.
		 */

		val = http_header_match2(cur_ptr, cur_end, "Pragma", 6);
		if (val) {
			if ((cur_end - (cur_ptr + val) >= 8) &&
			    strncasecmp(cur_ptr + val, "no-cache", 8) == 0) {
				txn->flags &= ~TX_CACHEABLE & ~TX_CACHE_COOK;
				return;
			}
		}

		val = http_header_match2(cur_ptr, cur_end, "Cache-control", 13);
		if (!val)
			continue;

		/* OK, right now we know we have a cache-control header at cur_ptr */

		p1 = cur_ptr + val; /* first non-space char after 'cache-control:' */

		if (p1 >= cur_end)	/* no more info */
			continue;

		/* p1 is at the beginning of the value */
		p2 = p1;

		while (p2 < cur_end && *p2 != '=' && *p2 != ',' && !isspace((unsigned char)*p2))
			p2++;

		/* we have a complete value between p1 and p2 */
		if (p2 < cur_end && *p2 == '=') {
			/* we have something of the form no-cache="set-cookie" */
			if ((cur_end - p1 >= 21) &&
			    strncasecmp(p1, "no-cache=\"set-cookie", 20) == 0
			    && (p1[20] == '"' || p1[20] == ','))
				txn->flags &= ~TX_CACHE_COOK;
			continue;
		}

		/* OK, so we know that either p2 points to the end of string or to a comma */
		if (((p2 - p1 ==  7) && strncasecmp(p1, "private", 7) == 0) ||
		    ((p2 - p1 ==  8) && strncasecmp(p1, "no-store", 8) == 0) ||
		    ((p2 - p1 ==  9) && strncasecmp(p1, "max-age=0", 9) == 0) ||
		    ((p2 - p1 == 10) && strncasecmp(p1, "s-maxage=0", 10) == 0)) {
			txn->flags &= ~TX_CACHEABLE & ~TX_CACHE_COOK;
			return;
		}

		if ((p2 - p1 ==  6) && strncasecmp(p1, "public", 6) == 0) {
			txn->flags |= TX_CACHEABLE | TX_CACHE_COOK;
			continue;
		}
	}
}


/*
 * Try to retrieve a known appsession in the URI, then the associated server.
 * If the server is found, it's assigned to the session.
 */
void get_srv_from_appsession(struct session *t, const char *begin, int len)
{
	char *end_params, *first_param, *cur_param, *next_param;
	char separator;
	int value_len;

	int mode = t->be->options2 & PR_O2_AS_M_ANY;

	if (t->be->appsession_name == NULL ||
	    (t->txn.meth != HTTP_METH_GET && t->txn.meth != HTTP_METH_POST && t->txn.meth != HTTP_METH_HEAD)) {
		return;
	}

	first_param = NULL;
	switch (mode) {
	case PR_O2_AS_M_PP:
		first_param = memchr(begin, ';', len);
		break;
	case PR_O2_AS_M_QS:
		first_param = memchr(begin, '?', len);
		break;
	}

	if (first_param == NULL) {
		return;
	}

	switch (mode) {
	case PR_O2_AS_M_PP:
		if ((end_params = memchr(first_param, '?', len - (begin - first_param))) == NULL) {
			end_params = (char *) begin + len;
		}
		separator = ';';
		break;
	case PR_O2_AS_M_QS:
		end_params = (char *) begin + len;
		separator = '&';
		break;
	default:
		/* unknown mode, shouldn't happen */
		return;
	}
	
	cur_param = next_param = end_params;
	while (cur_param > first_param) {
		cur_param--;
		if ((cur_param[0] == separator) || (cur_param == first_param)) {
			/* let's see if this is the appsession parameter */
			if ((cur_param + t->be->appsession_name_len + 1 < next_param) &&
				((t->be->options2 & PR_O2_AS_PFX) || cur_param[t->be->appsession_name_len + 1] == '=') &&
				(strncasecmp(cur_param + 1, t->be->appsession_name, t->be->appsession_name_len) == 0)) {
				/* Cool... it's the right one */
				cur_param += t->be->appsession_name_len + (t->be->options2 & PR_O2_AS_PFX ? 1 : 2);
				value_len = MIN(t->be->appsession_len, next_param - cur_param);
				if (value_len > 0) {
					manage_client_side_appsession(t, cur_param, value_len);
				}
				break;
			}
			next_param = cur_param;
		}
	}
#if defined(DEBUG_HASH)
	Alert("get_srv_from_appsession\n");
	appsession_hash_dump(&(t->be->htbl_proxy));
#endif
}

/*
 * In a GET, HEAD or POST request, check if the requested URI matches the stats uri
 * for the current backend.
 *
 * It is assumed that the request is either a HEAD, GET, or POST and that the
 * uri_auth field is valid.
 *
 * Returns 1 if stats should be provided, otherwise 0.
 */
int stats_check_uri(struct stream_interface *si, struct http_txn *txn, struct proxy *backend)
{
	struct uri_auth *uri_auth = backend->uri_auth;
	struct http_msg *msg = &txn->req;
	const char *uri = msg->chn->buf->p+ msg->sl.rq.u;
	const char *h;

	if (!uri_auth)
		return 0;

	if (txn->meth != HTTP_METH_GET && txn->meth != HTTP_METH_HEAD && txn->meth != HTTP_METH_POST)
		return 0;

	memset(&si->applet.ctx.stats, 0, sizeof(si->applet.ctx.stats));
	si->applet.ctx.stats.st_code = STAT_STATUS_INIT;
	si->applet.ctx.stats.flags |= STAT_FMT_HTML; /* assume HTML mode by default */

	/* check URI size */
	if (uri_auth->uri_len > msg->sl.rq.u_l)
		return 0;

	h = uri;
	if (memcmp(h, uri_auth->uri_prefix, uri_auth->uri_len) != 0)
		return 0;

	h += uri_auth->uri_len;
	while (h <= uri + msg->sl.rq.u_l - 3) {
		if (memcmp(h, ";up", 3) == 0) {
			si->applet.ctx.stats.flags |= STAT_HIDE_DOWN;
			break;
		}
		h++;
	}

	if (uri_auth->refresh) {
		h = uri + uri_auth->uri_len;
		while (h <= uri + msg->sl.rq.u_l - 10) {
			if (memcmp(h, ";norefresh", 10) == 0) {
				si->applet.ctx.stats.flags |= STAT_NO_REFRESH;
				break;
			}
			h++;
		}
	}

	h = uri + uri_auth->uri_len;
	while (h <= uri + msg->sl.rq.u_l - 4) {
		if (memcmp(h, ";csv", 4) == 0) {
			si->applet.ctx.stats.flags &= ~STAT_FMT_HTML;
			break;
		}
		h++;
	}

	h = uri + uri_auth->uri_len;
	while (h <= uri + msg->sl.rq.u_l - 8) {
		if (memcmp(h, ";st=", 4) == 0) {
			int i;
			h += 4;
			si->applet.ctx.stats.st_code = STAT_STATUS_UNKN;
			for (i = STAT_STATUS_INIT + 1; i < STAT_STATUS_SIZE; i++) {
				if (strncmp(stat_status_codes[i], h, 4) == 0) {
					si->applet.ctx.stats.st_code = i;
					break;
				}
			}
			break;
		}
		h++;
	}
	return 1;
}

/*
 * Capture a bad request or response and archive it in the proxy's structure.
 * By default it tries to report the error position as msg->err_pos. However if
 * this one is not set, it will then report msg->next, which is the last known
 * parsing point. The function is able to deal with wrapping buffers. It always
 * displays buffers as a contiguous area starting at buf->p.
 */
void http_capture_bad_message(struct error_snapshot *es, struct session *s,
                              struct http_msg *msg,
			      int state, struct proxy *other_end)
{
	struct channel *chn = msg->chn;
	int len1, len2;

	es->len = MIN(chn->buf->i, sizeof(es->buf));
	len1 = chn->buf->data + chn->buf->size - chn->buf->p;
	len1 = MIN(len1, es->len);
	len2 = es->len - len1; /* remaining data if buffer wraps */

	memcpy(es->buf, chn->buf->p, len1);
	if (len2)
		memcpy(es->buf + len1, chn->buf->data, len2);

	if (msg->err_pos >= 0)
		es->pos = msg->err_pos;
	else
		es->pos = msg->next;

	es->when = date; // user-visible date
	es->sid  = s->uniq_id;
	es->srv  = objt_server(s->target);
	es->oe   = other_end;
	es->src  = s->req->prod->conn->addr.from;
	es->state = state;
	es->ev_id = error_snapshot_id++;
	es->b_flags = chn->flags;
	es->s_flags = s->flags;
	es->t_flags = s->txn.flags;
	es->m_flags = msg->flags;
	es->b_out = chn->buf->o;
	es->b_wrap = chn->buf->data + chn->buf->size - chn->buf->p;
	es->b_tot = chn->total;
	es->m_clen = msg->chunk_len;
	es->m_blen = msg->body_len;
}

/* Return in <vptr> and <vlen> the pointer and length of occurrence <occ> of
 * header whose name is <hname> of length <hlen>. If <ctx> is null, lookup is
 * performed over the whole headers. Otherwise it must contain a valid header
 * context, initialised with ctx->idx=0 for the first lookup in a series. If
 * <occ> is positive or null, occurrence #occ from the beginning (or last ctx)
 * is returned. Occ #0 and #1 are equivalent. If <occ> is negative (and no less
 * than -MAX_HDR_HISTORY), the occurrence is counted from the last one which is
 * -1.
 * The return value is 0 if nothing was found, or non-zero otherwise.
 */
unsigned int http_get_hdr(const struct http_msg *msg, const char *hname, int hlen,
			  struct hdr_idx *idx, int occ,
			  struct hdr_ctx *ctx, char **vptr, int *vlen)
{
	struct hdr_ctx local_ctx;
	char *ptr_hist[MAX_HDR_HISTORY];
	int len_hist[MAX_HDR_HISTORY];
	unsigned int hist_ptr;
	int found;

	if (!ctx) {
		local_ctx.idx = 0;
		ctx = &local_ctx;
	}

	if (occ >= 0) {
		/* search from the beginning */
		while (http_find_header2(hname, hlen, msg->chn->buf->p, idx, ctx)) {
			occ--;
			if (occ <= 0) {
				*vptr = ctx->line + ctx->val;
				*vlen = ctx->vlen;
				return 1;
			}
		}
		return 0;
	}

	/* negative occurrence, we scan all the list then walk back */
	if (-occ > MAX_HDR_HISTORY)
		return 0;

	found = hist_ptr = 0;
	while (http_find_header2(hname, hlen, msg->chn->buf->p, idx, ctx)) {
		ptr_hist[hist_ptr] = ctx->line + ctx->val;
		len_hist[hist_ptr] = ctx->vlen;
		if (++hist_ptr >= MAX_HDR_HISTORY)
			hist_ptr = 0;
		found++;
	}
	if (-occ > found)
		return 0;
	/* OK now we have the last occurrence in [hist_ptr-1], and we need to
	 * find occurrence -occ, so we have to check [hist_ptr+occ].
	 */
	hist_ptr += occ;
	if (hist_ptr >= MAX_HDR_HISTORY)
		hist_ptr -= MAX_HDR_HISTORY;
	*vptr = ptr_hist[hist_ptr];
	*vlen = len_hist[hist_ptr];
	return 1;
}

/*
 * Print a debug line with a header. Always stop at the first CR or LF char,
 * so it is safe to pass it a full buffer if needed. If <err> is not NULL, an
 * arrow is printed after the line which contains the pointer.
 */
void debug_hdr(const char *dir, struct session *t, const char *start, const char *end)
{
	int max;
	chunk_printf(&trash, "%08x:%s.%s[%04x:%04x]: ", t->uniq_id, t->be->id,
		      dir, (unsigned  short)t->req->prod->conn->t.sock.fd,
		     (unsigned short)t->req->cons->conn->t.sock.fd);

	for (max = 0; start + max < end; max++)
		if (start[max] == '\r' || start[max] == '\n')
			break;

	UBOUND(max, trash.size - trash.len - 3);
	trash.len += strlcpy2(trash.str + trash.len, start, max + 1);
	trash.str[trash.len++] = '\n';
	if (write(1, trash.str, trash.len) < 0) /* shut gcc warning */;
}

/*
 * Initialize a new HTTP transaction for session <s>. It is assumed that all
 * the required fields are properly allocated and that we only need to (re)init
 * them. This should be used before processing any new request.
 */
void http_init_txn(struct session *s)
{
	struct http_txn *txn = &s->txn;
	struct proxy *fe = s->fe;

	txn->flags = 0;
	txn->status = -1;

	global.req_count++;

	txn->cookie_first_date = 0;
	txn->cookie_last_date = 0;

	txn->req.flags = 0;
	txn->req.sol = txn->req.eol = txn->req.eoh = 0; /* relative to the buffer */
	txn->req.next = 0;
	txn->rsp.flags = 0;
	txn->rsp.sol = txn->rsp.eol = txn->rsp.eoh = 0; /* relative to the buffer */
	txn->rsp.next = 0;
	txn->req.chunk_len = 0LL;
	txn->req.body_len = 0LL;
	txn->rsp.chunk_len = 0LL;
	txn->rsp.body_len = 0LL;
	txn->req.msg_state = HTTP_MSG_RQBEFORE; /* at the very beginning of the request */
	txn->rsp.msg_state = HTTP_MSG_RPBEFORE; /* at the very beginning of the response */
	txn->req.chn = s->req;
	txn->rsp.chn = s->rep;

	txn->auth.method = HTTP_AUTH_UNKNOWN;

	txn->req.err_pos = txn->rsp.err_pos = -2; /* block buggy requests/responses */
	if (fe->options2 & PR_O2_REQBUG_OK)
		txn->req.err_pos = -1;            /* let buggy requests pass */

	if (txn->req.cap)
		memset(txn->req.cap, 0, fe->nb_req_cap * sizeof(void *));

	if (txn->rsp.cap)
		memset(txn->rsp.cap, 0, fe->nb_rsp_cap * sizeof(void *));

	if (txn->hdr_idx.v)
		hdr_idx_init(&txn->hdr_idx);
}

/* to be used at the end of a transaction */
void http_end_txn(struct session *s)
{
	struct http_txn *txn = &s->txn;

	/* these ones will have been dynamically allocated */
	pool_free2(pool2_requri, txn->uri);
	pool_free2(pool2_capture, txn->cli_cookie);
	pool_free2(pool2_capture, txn->srv_cookie);
	pool_free2(apools.sessid, txn->sessid);
	pool_free2(pool2_uniqueid, s->unique_id);

	s->unique_id = NULL;
	txn->sessid = NULL;
	txn->uri = NULL;
	txn->srv_cookie = NULL;
	txn->cli_cookie = NULL;

	if (txn->req.cap) {
		struct cap_hdr *h;
		for (h = s->fe->req_cap; h; h = h->next)
			pool_free2(h->pool, txn->req.cap[h->index]);
		memset(txn->req.cap, 0, s->fe->nb_req_cap * sizeof(void *));
	}

	if (txn->rsp.cap) {
		struct cap_hdr *h;
		for (h = s->fe->rsp_cap; h; h = h->next)
			pool_free2(h->pool, txn->rsp.cap[h->index]);
		memset(txn->rsp.cap, 0, s->fe->nb_rsp_cap * sizeof(void *));
	}

}

/* to be used at the end of a transaction to prepare a new one */
void http_reset_txn(struct session *s)
{
	http_end_txn(s);
	http_init_txn(s);

	s->be = s->fe;
	s->logs.logwait = s->fe->to_log;
	session_del_srv_conn(s);
	s->target = NULL;
	/* re-init store persistence */
	s->store_count = 0;

	s->pend_pos = NULL;

	s->req->flags |= CF_READ_DONTWAIT; /* one read is usually enough */

	/* We must trim any excess data from the response buffer, because we
	 * may have blocked an invalid response from a server that we don't
	 * want to accidentely forward once we disable the analysers, nor do
	 * we want those data to come along with next response. A typical
	 * example of such data would be from a buggy server responding to
	 * a HEAD with some data, or sending more than the advertised
	 * content-length.
	 */
	if (unlikely(s->rep->buf->i))
		s->rep->buf->i = 0;

	s->req->rto = s->fe->timeout.client;
	s->req->wto = TICK_ETERNITY;

	s->rep->rto = TICK_ETERNITY;
	s->rep->wto = s->fe->timeout.client;

	s->req->rex = TICK_ETERNITY;
	s->req->wex = TICK_ETERNITY;
	s->req->analyse_exp = TICK_ETERNITY;
	s->rep->rex = TICK_ETERNITY;
	s->rep->wex = TICK_ETERNITY;
	s->rep->analyse_exp = TICK_ETERNITY;
}

void free_http_req_rules(struct list *r) {
	struct http_req_rule *tr, *pr;

	list_for_each_entry_safe(pr, tr, r, list) {
		LIST_DEL(&pr->list);
		if (pr->action == HTTP_REQ_ACT_AUTH)
			free(pr->arg.auth.realm);

		free(pr);
	}
}

struct http_req_rule *parse_http_req_cond(const char **args, const char *file, int linenum, struct proxy *proxy)
{
	struct http_req_rule *rule;
	int cur_arg;

	rule = (struct http_req_rule*)calloc(1, sizeof(struct http_req_rule));
	if (!rule) {
		Alert("parsing [%s:%d]: out of memory.\n", file, linenum);
		goto out_err;
	}

	if (!strcmp(args[0], "allow")) {
		rule->action = HTTP_REQ_ACT_ALLOW;
		cur_arg = 1;
	} else if (!strcmp(args[0], "deny")) {
		rule->action = HTTP_REQ_ACT_DENY;
		cur_arg = 1;
	} else if (!strcmp(args[0], "tarpit")) {
		rule->action = HTTP_REQ_ACT_TARPIT;
		cur_arg = 1;
	} else if (!strcmp(args[0], "auth")) {
		rule->action = HTTP_REQ_ACT_AUTH;
		cur_arg = 1;

		while(*args[cur_arg]) {
			if (!strcmp(args[cur_arg], "realm")) {
				rule->arg.auth.realm = strdup(args[cur_arg + 1]);
				cur_arg+=2;
				continue;
			} else
				break;
		}
	} else if (strcmp(args[0], "add-header") == 0 || strcmp(args[0], "set-header") == 0) {
		rule->action = *args[0] == 'a' ? HTTP_REQ_ACT_ADD_HDR : HTTP_REQ_ACT_SET_HDR;
		cur_arg = 1;

		if (!*args[cur_arg] || !*args[cur_arg+1] || *args[cur_arg+2]) {
			Alert("parsing [%s:%d]: 'http-request %s' expects exactly 2 arguments.\n",
			      file, linenum, args[0]);
			goto out_err;
		}

		rule->arg.hdr_add.name = strdup(args[cur_arg]);
		rule->arg.hdr_add.name_len = strlen(rule->arg.hdr_add.name);
		LIST_INIT(&rule->arg.hdr_add.fmt);
		parse_logformat_string(args[cur_arg + 1], proxy, &rule->arg.hdr_add.fmt, PR_MODE_HTTP);
		cur_arg += 2;
	} else if (strcmp(args[0], "redirect") == 0) {
		struct redirect_rule *redir;
		char *errmsg;

		if ((redir = http_parse_redirect_rule(file, linenum, proxy, (const char **)args + 1, &errmsg)) == NULL) {
			Alert("parsing [%s:%d] : error detected in %s '%s' while parsing 'http-request %s' rule : %s.\n",
			      file, linenum, proxy_type_str(proxy), proxy->id, args[0], errmsg);
			goto out_err;
		}

		/* this redirect rule might already contain a parsed condition which
		 * we'll pass to the http-request rule.
		 */
		rule->action = HTTP_REQ_ACT_REDIR;
		rule->arg.redir = redir;
		rule->cond = redir->cond;
		redir->cond = NULL;
		cur_arg = 2;
		return rule;
	} else {
		Alert("parsing [%s:%d]: 'http-request' expects 'allow', 'deny', 'auth', 'redirect', 'tarpit', 'add-header', 'set-header', but got '%s'%s.\n",
		      file, linenum, args[0], *args[0] ? "" : " (missing argument)");
		goto out_err;
	}

	if (strcmp(args[cur_arg], "if") == 0 || strcmp(args[cur_arg], "unless") == 0) {
		struct acl_cond *cond;
		char *errmsg = NULL;

		if ((cond = build_acl_cond(file, linenum, proxy, args+cur_arg, &errmsg)) == NULL) {
			Alert("parsing [%s:%d] : error detected while parsing an 'http-request %s' condition : %s.\n",
			      file, linenum, args[0], errmsg);
			free(errmsg);
			goto out_err;
		}
		rule->cond = cond;
	}
	else if (*args[cur_arg]) {
		Alert("parsing [%s:%d]: 'http-request %s' expects 'realm' for 'auth' or"
		      " either 'if' or 'unless' followed by a condition but found '%s'.\n",
		      file, linenum, args[0], args[cur_arg]);
		goto out_err;
	}

	return rule;
 out_err:
	free(rule);
	return NULL;
}

/* Parses a redirect rule. Returns the redirect rule on success or NULL on error,
 * with <err> filled with the error message.
 */
struct redirect_rule *http_parse_redirect_rule(const char *file, int linenum, struct proxy *curproxy,
                                               const char **args, char **errmsg)
{
	struct redirect_rule *rule;
	int cur_arg;
	int type = REDIRECT_TYPE_NONE;
	int code = 302;
	const char *destination = NULL;
	const char *cookie = NULL;
	int cookie_set = 0;
	unsigned int flags = REDIRECT_FLAG_NONE;
	struct acl_cond *cond = NULL;

	cur_arg = 0;
	while (*(args[cur_arg])) {
		if (strcmp(args[cur_arg], "location") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			type = REDIRECT_TYPE_LOCATION;
			cur_arg++;
			destination = args[cur_arg];
		}
		else if (strcmp(args[cur_arg], "prefix") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			type = REDIRECT_TYPE_PREFIX;
			cur_arg++;
			destination = args[cur_arg];
		}
		else if (strcmp(args[cur_arg], "scheme") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			type = REDIRECT_TYPE_SCHEME;
			cur_arg++;
			destination = args[cur_arg];
		}
		else if (strcmp(args[cur_arg], "set-cookie") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			cur_arg++;
			cookie = args[cur_arg];
			cookie_set = 1;
		}
		else if (strcmp(args[cur_arg], "clear-cookie") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			cur_arg++;
			cookie = args[cur_arg];
			cookie_set = 0;
		}
		else if (strcmp(args[cur_arg], "code") == 0) {
			if (!*args[cur_arg + 1])
				goto missing_arg;

			cur_arg++;
			code = atol(args[cur_arg]);
			if (code < 301 || code > 303) {
				memprintf(errmsg,
				          "'%s': unsupported HTTP code '%s' (must be a number between 301 and 303)",
				          args[cur_arg - 1], args[cur_arg]);
				return NULL;
			}
		}
		else if (!strcmp(args[cur_arg],"drop-query")) {
			flags |= REDIRECT_FLAG_DROP_QS;
		}
		else if (!strcmp(args[cur_arg],"append-slash")) {
			flags |= REDIRECT_FLAG_APPEND_SLASH;
		}
		else if (strcmp(args[cur_arg], "if") == 0 ||
			 strcmp(args[cur_arg], "unless") == 0) {
			cond = build_acl_cond(file, linenum, curproxy, (const char **)args + cur_arg, errmsg);
			if (!cond) {
				memprintf(errmsg, "error in condition: %s", *errmsg);
				return NULL;
			}
			break;
		}
		else {
			memprintf(errmsg,
			          "expects 'code', 'prefix', 'location', 'scheme', 'set-cookie', 'clear-cookie', 'drop-query' or 'append-slash' (was '%s')",
			          args[cur_arg]);
			return NULL;
		}
		cur_arg++;
	}

	if (type == REDIRECT_TYPE_NONE) {
		memprintf(errmsg, "redirection type expected ('prefix', 'location', or 'scheme')");
		return NULL;
	}

	rule = (struct redirect_rule *)calloc(1, sizeof(*rule));
	rule->cond = cond;
	rule->rdr_str = strdup(destination);
	rule->rdr_len = strlen(destination);
	if (cookie) {
		/* depending on cookie_set, either we want to set the cookie, or to clear it.
		 * a clear consists in appending "; path=/; Max-Age=0;" at the end.
		 */
		rule->cookie_len = strlen(cookie);
		if (cookie_set) {
			rule->cookie_str = malloc(rule->cookie_len + 10);
			memcpy(rule->cookie_str, cookie, rule->cookie_len);
			memcpy(rule->cookie_str + rule->cookie_len, "; path=/;", 10);
			rule->cookie_len += 9;
		} else {
			rule->cookie_str = malloc(rule->cookie_len + 21);
			memcpy(rule->cookie_str, cookie, rule->cookie_len);
			memcpy(rule->cookie_str + rule->cookie_len, "; path=/; Max-Age=0;", 21);
			rule->cookie_len += 20;
		}
	}
	rule->type = type;
	rule->code = code;
	rule->flags = flags;
	LIST_INIT(&rule->list);
	return rule;

 missing_arg:
	memprintf(errmsg, "missing argument for '%s'", args[cur_arg]);
	return NULL;
}

/************************************************************************/
/*        The code below is dedicated to ACL parsing and matching       */
/************************************************************************/


/* This function ensures that the prerequisites for an L7 fetch are ready,
 * which means that a request or response is ready. If some data is missing,
 * a parsing attempt is made. This is useful in TCP-based ACLs which are able
 * to extract data from L7. If <req_vol> is non-null during a request prefetch,
 * another test is made to ensure the required information is not gone.
 *
 * The function returns :
 *   0 if some data is missing or if the requested data cannot be fetched
 *  -1 if it is certain that we'll never have any HTTP message there
 *   1 if an HTTP message is ready
 */
static int
acl_prefetch_http(struct proxy *px, struct session *s, void *l7, unsigned int opt,
                  const struct arg *args, struct sample *smp, int req_vol)
{
	struct http_txn *txn = l7;
	struct http_msg *msg = &txn->req;

	/* Note: hdr_idx.v cannot be NULL in this ACL because the ACL is tagged
	 * as a layer7 ACL, which involves automatic allocation of hdr_idx.
	 */

	if (unlikely(!s || !txn))
		return 0;

	/* Check for a dependency on a request */
	smp->type = SMP_T_BOOL;

	if ((opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ) {
		if (unlikely(!s->req))
			return 0;

		if (unlikely(txn->req.msg_state < HTTP_MSG_BODY)) {
			if ((msg->msg_state == HTTP_MSG_ERROR) ||
			    buffer_full(s->req->buf, global.tune.maxrewrite)) {
				smp->data.uint = 0;
				return -1;
			}

			/* Try to decode HTTP request */
			if (likely(msg->next < s->req->buf->i))
				http_msg_analyzer(msg, &txn->hdr_idx);

			/* Still no valid request ? */
			if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
				if ((msg->msg_state == HTTP_MSG_ERROR) ||
				    buffer_full(s->req->buf, global.tune.maxrewrite)) {
					smp->data.uint = 0;
					return -1;
				}
				/* wait for final state */
				smp->flags |= SMP_F_MAY_CHANGE;
				return 0;
			}

			/* OK we just got a valid HTTP request. We have some minor
			 * preparation to perform so that further checks can rely
			 * on HTTP tests.
			 */
			txn->meth = find_http_meth(msg->chn->buf->p, msg->sl.rq.m_l);
			if (txn->meth == HTTP_METH_GET || txn->meth == HTTP_METH_HEAD)
				s->flags |= SN_REDIRECTABLE;

			if (unlikely(msg->sl.rq.v_l == 0) && !http_upgrade_v09_to_v10(txn)) {
				smp->data.uint = 0;
				return -1;
			}
		}

		if (req_vol && txn->rsp.msg_state != HTTP_MSG_RPBEFORE)
			return 0;  /* data might have moved and indexes changed */

		/* otherwise everything's ready for the request */
	}
	else {
		/* Check for a dependency on a response */
		if (txn->rsp.msg_state < HTTP_MSG_BODY)
			return 0;
	}

	/* everything's OK */
	return 1;
}

#define CHECK_HTTP_MESSAGE_FIRST() \
	do { int r = acl_prefetch_http(px, l4, l7, opt, args, smp, 1); if (r <= 0) return r; } while (0)

#define CHECK_HTTP_MESSAGE_FIRST_PERM() \
	do { int r = acl_prefetch_http(px, l4, l7, opt, args, smp, 0); if (r <= 0) return r; } while (0)


/* 1. Check on METHOD
 * We use the pre-parsed method if it is known, and store its number as an
 * integer. If it is unknown, we use the pointer and the length.
 */
static int acl_parse_meth(const char **text, struct acl_pattern *pattern, int *opaque, char **err)
{
	int len, meth;

	len  = strlen(*text);
	meth = find_http_meth(*text, len);

	pattern->val.i = meth;
	if (meth == HTTP_METH_OTHER) {
		pattern->ptr.str = strdup(*text);
		if (!pattern->ptr.str) {
			memprintf(err, "out of memory while loading pattern");
			return 0;
		}
		pattern->len = len;
	}
	return 1;
}

/* This function fetches the method of current HTTP request and stores
 * it in the global pattern struct as a chunk. There are two possibilities :
 *   - if the method is known (not HTTP_METH_OTHER), its identifier is stored
 *     in <len> and <ptr> is NULL ;
 *   - if the method is unknown (HTTP_METH_OTHER), <ptr> points to the text and
 *     <len> to its length.
 * This is intended to be used with acl_match_meth() only.
 */
static int
acl_fetch_meth(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
               const struct arg *args, struct sample *smp)
{
	int meth;
	struct http_txn *txn = l7;

	CHECK_HTTP_MESSAGE_FIRST_PERM();

	meth = txn->meth;
	smp->type = SMP_T_UINT;
	smp->data.uint = meth;
	if (meth == HTTP_METH_OTHER) {
		if (txn->rsp.msg_state != HTTP_MSG_RPBEFORE)
			/* ensure the indexes are not affected */
			return 0;
		smp->type = SMP_T_CSTR;
		smp->data.str.len = txn->req.sl.rq.m_l;
		smp->data.str.str = txn->req.chn->buf->p;
	}
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* See above how the method is stored in the global pattern */
static int acl_match_meth(struct sample *smp, struct acl_pattern *pattern)
{
	int icase;


	if (smp->type == SMP_T_UINT) {
		/* well-known method */
		if (smp->data.uint == pattern->val.i)
			return ACL_PAT_PASS;
		return ACL_PAT_FAIL;
	}

	/* Uncommon method, only HTTP_METH_OTHER is accepted now */
	if (pattern->val.i != HTTP_METH_OTHER)
		return ACL_PAT_FAIL;

	/* Other method, we must compare the strings */
	if (pattern->len != smp->data.str.len)
		return ACL_PAT_FAIL;

	icase = pattern->flags & ACL_PAT_F_IGNORE_CASE;
	if ((icase && strncasecmp(pattern->ptr.str, smp->data.str.str, smp->data.str.len) != 0) ||
	    (!icase && strncmp(pattern->ptr.str, smp->data.str.str, smp->data.str.len) != 0))
		return ACL_PAT_FAIL;
	return ACL_PAT_PASS;
}

/* 2. Check on Request/Status Version
 * We simply compare strings here.
 */
static int acl_parse_ver(const char **text, struct acl_pattern *pattern, int *opaque, char **err)
{
	pattern->ptr.str = strdup(*text);
	if (!pattern->ptr.str) {
		memprintf(err, "out of memory while loading pattern");
		return 0;
	}
	pattern->len = strlen(*text);
	return 1;
}

static int
acl_fetch_rqver(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	char *ptr;
	int len;

	CHECK_HTTP_MESSAGE_FIRST();

	len = txn->req.sl.rq.v_l;
	ptr = txn->req.chn->buf->p + txn->req.sl.rq.v;

	while ((len-- > 0) && (*ptr++ != '/'));
	if (len <= 0)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.str = ptr;
	smp->data.str.len = len;

	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

static int
acl_fetch_stver(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	char *ptr;
	int len;

	CHECK_HTTP_MESSAGE_FIRST();

	if (txn->rsp.msg_state < HTTP_MSG_BODY)
		return 0;

	len = txn->rsp.sl.st.v_l;
	ptr = txn->rsp.chn->buf->p;

	while ((len-- > 0) && (*ptr++ != '/'));
	if (len <= 0)
		return 0;

	smp->type = SMP_T_CSTR;
	smp->data.str.str = ptr;
	smp->data.str.len = len;

	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* 3. Check on Status Code. We manipulate integers here. */
static int
acl_fetch_stcode(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	char *ptr;
	int len;

	CHECK_HTTP_MESSAGE_FIRST();

	if (txn->rsp.msg_state < HTTP_MSG_BODY)
		return 0;

	len = txn->rsp.sl.st.c_l;
	ptr = txn->rsp.chn->buf->p + txn->rsp.sl.st.c;

	smp->type = SMP_T_UINT;
	smp->data.uint = __strl2ui(ptr, len);
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* 4. Check on URL/URI. A pointer to the URI is stored. */
static int
smp_fetch_url(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
              const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;

	CHECK_HTTP_MESSAGE_FIRST();

	smp->type = SMP_T_CSTR;
	smp->data.str.len = txn->req.sl.rq.u_l;
	smp->data.str.str = txn->req.chn->buf->p + txn->req.sl.rq.u;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

static int
smp_fetch_url_ip(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;

	CHECK_HTTP_MESSAGE_FIRST();

	/* Parse HTTP request */
	url2sa(txn->req.chn->buf->p + txn->req.sl.rq.u, txn->req.sl.rq.u_l, &l4->req->cons->conn->addr.to);
	if (((struct sockaddr_in *)&l4->req->cons->conn->addr.to)->sin_family != AF_INET)
		return 0;
	smp->type = SMP_T_IPV4;
	smp->data.ipv4 = ((struct sockaddr_in *)&l4->req->cons->conn->addr.to)->sin_addr;

	/*
	 * If we are parsing url in frontend space, we prepare backend stage
	 * to not parse again the same url ! optimization lazyness...
	 */
	if (px->options & PR_O_HTTP_PROXY)
		l4->flags |= SN_ADDR_SET;

	smp->flags = 0;
	return 1;
}

static int
smp_fetch_url_port(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                   const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;

	CHECK_HTTP_MESSAGE_FIRST();

	/* Same optimization as url_ip */
	url2sa(txn->req.chn->buf->p + txn->req.sl.rq.u, txn->req.sl.rq.u_l, &l4->req->cons->conn->addr.to);
	smp->type = SMP_T_UINT;
	smp->data.uint = ntohs(((struct sockaddr_in *)&l4->req->cons->conn->addr.to)->sin_port);

	if (px->options & PR_O_HTTP_PROXY)
		l4->flags |= SN_ADDR_SET;

	smp->flags = 0;
	return 1;
}

/* Fetch an HTTP header. A pointer to the beginning of the value is returned.
 * Accepts an optional argument of type string containing the header field name,
 * and an optional argument of type signed or unsigned integer to request an
 * explicit occurrence of the header. Note that in the event of a missing name,
 * headers are considered from the first one.
 */
static int
smp_fetch_hdr(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
              const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	struct hdr_idx *idx = &txn->hdr_idx;
	struct hdr_ctx *ctx = (struct hdr_ctx *)smp->ctx.a;
	const struct http_msg *msg = ((opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ) ? &txn->req : &txn->rsp;
	int occ = 0;
	const char *name_str = NULL;
	int name_len = 0;

	if (args) {
		if (args[0].type != ARGT_STR)
			return 0;
		name_str = args[0].data.str.str;
		name_len = args[0].data.str.len;

		if (args[1].type == ARGT_UINT || args[1].type == ARGT_SINT)
			occ = args[1].data.uint;
	}

	CHECK_HTTP_MESSAGE_FIRST();

	if (ctx && !(smp->flags & SMP_F_NOT_LAST))
		/* search for header from the beginning */
		ctx->idx = 0;

	if (!occ && !(opt & SMP_OPT_ITERATE))
		/* no explicit occurrence and single fetch => last header by default */
		occ = -1;

	if (!occ)
		/* prepare to report multiple occurrences for ACL fetches */
		smp->flags |= SMP_F_NOT_LAST;

	smp->type = SMP_T_CSTR;
	smp->flags |= SMP_F_VOL_HDR;
	if (http_get_hdr(msg, name_str, name_len, idx, occ, ctx, &smp->data.str.str, &smp->data.str.len))
		return 1;

	smp->flags &= ~SMP_F_NOT_LAST;
	return 0;
}

/* 6. Check on HTTP header count. The number of occurrences is returned.
 * Accepts exactly 1 argument of type string.
 */
static int
smp_fetch_hdr_cnt(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                  const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	struct hdr_idx *idx = &txn->hdr_idx;
	struct hdr_ctx ctx;
	const struct http_msg *msg = ((opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ) ? &txn->req : &txn->rsp;
	int cnt;

	if (!args || args->type != ARGT_STR)
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	ctx.idx = 0;
	cnt = 0;
	while (http_find_header2(args->data.str.str, args->data.str.len, msg->chn->buf->p, idx, &ctx))
		cnt++;

	smp->type = SMP_T_UINT;
	smp->data.uint = cnt;
	smp->flags = SMP_F_VOL_HDR;
	return 1;
}

/* Fetch an HTTP header's integer value. The integer value is returned. It
 * takes a mandatory argument of type string and an optional one of type int
 * to designate a specific occurrence. It returns an unsigned integer, which
 * may or may not be appropriate for everything.
 */
static int
smp_fetch_hdr_val(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                  const struct arg *args, struct sample *smp)
{
	int ret = smp_fetch_hdr(px, l4, l7, opt, args, smp);

	if (ret > 0) {
		smp->type = SMP_T_UINT;
		smp->data.uint = strl2ic(smp->data.str.str, smp->data.str.len);
	}

	return ret;
}

/* Fetch an HTTP header's IP value. takes a mandatory argument of type string
 * and an optional one of type int to designate a specific occurrence.
 * It returns an IPv4 or IPv6 address.
 */
static int
smp_fetch_hdr_ip(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp)
{
	int ret;

	while ((ret = smp_fetch_hdr(px, l4, l7, opt, args, smp)) > 0) {
		if (url2ipv4((char *)smp->data.str.str, &smp->data.ipv4)) {
			smp->type = SMP_T_IPV4;
			break;
		} else {
			struct chunk *temp = get_trash_chunk();
			if (smp->data.str.len < temp->size - 1) {
				memcpy(temp->str, smp->data.str.str, smp->data.str.len);
				temp->str[smp->data.str.len] = '\0';
				if (inet_pton(AF_INET6, temp->str, &smp->data.ipv6)) {
					smp->type = SMP_T_IPV6;
					break;
				}
			}
		}

		/* if the header doesn't match an IP address, fetch next one */
		if (!(smp->flags & SMP_F_NOT_LAST))
			return 0;
	}
	return ret;
}

/* 8. Check on URI PATH. A pointer to the PATH is stored. The path starts at
 * the first '/' after the possible hostname, and ends before the possible '?'.
 */
static int
smp_fetch_path(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
               const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	char *ptr, *end;

	CHECK_HTTP_MESSAGE_FIRST();

	end = txn->req.chn->buf->p + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
	ptr = http_get_path(txn);
	if (!ptr)
		return 0;

	/* OK, we got the '/' ! */
	smp->type = SMP_T_CSTR;
	smp->data.str.str = ptr;

	while (ptr < end && *ptr != '?')
		ptr++;

	smp->data.str.len = ptr - smp->data.str.str;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This produces a concatenation of the first occurrence of the Host header
 * followed by the path component if it begins with a slash ('/'). This means
 * that '*' will not be added, resulting in exactly the first Host entry.
 * If no Host header is found, then the path is returned as-is. The returned
 * value is stored in the trash so it does not need to be marked constant.
 */
static int
smp_fetch_base(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
               const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	char *ptr, *end, *beg;
	struct hdr_ctx ctx;

	CHECK_HTTP_MESSAGE_FIRST();

	ctx.idx = 0;
	if (!http_find_header2("Host", 4, txn->req.chn->buf->p + txn->req.sol, &txn->hdr_idx, &ctx) ||
	    !ctx.vlen)
		return smp_fetch_path(px, l4, l7, opt, args, smp);

	/* OK we have the header value in ctx.line+ctx.val for ctx.vlen bytes */
	memcpy(trash.str, ctx.line + ctx.val, ctx.vlen);
	smp->type = SMP_T_STR;
	smp->data.str.str = trash.str;
	smp->data.str.len = ctx.vlen;

	/* now retrieve the path */
	end = txn->req.chn->buf->p + txn->req.sol + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
	beg = http_get_path(txn);
	if (!beg)
		beg = end;

	for (ptr = beg; ptr < end && *ptr != '?'; ptr++);

	if (beg < ptr && *beg == '/') {
		memcpy(smp->data.str.str + smp->data.str.len, beg, ptr - beg);
		smp->data.str.len += ptr - beg;
	}

	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This produces a 32-bit hash of the concatenation of the first occurrence of
 * the Host header followed by the path component if it begins with a slash ('/').
 * This means that '*' will not be added, resulting in exactly the first Host
 * entry. If no Host header is found, then the path is used. The resulting value
 * is hashed using the url hash followed by a full avalanche hash and provides a
 * 32-bit integer value. This fetch is useful for tracking per-URL activity on
 * high-traffic sites without having to store whole paths.
 */
static int
smp_fetch_base32(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	struct hdr_ctx ctx;
	unsigned int hash = 0;
	char *ptr, *beg, *end;
	int len;

	CHECK_HTTP_MESSAGE_FIRST();

	ctx.idx = 0;
	if (http_find_header2("Host", 4, txn->req.chn->buf->p + txn->req.sol, &txn->hdr_idx, &ctx)) {
		/* OK we have the header value in ctx.line+ctx.val for ctx.vlen bytes */
		ptr = ctx.line + ctx.val;
		len = ctx.vlen;
		while (len--)
			hash = *(ptr++) + (hash << 6) + (hash << 16) - hash;
	}

	/* now retrieve the path */
	end = txn->req.chn->buf->p + txn->req.sol + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
	beg = http_get_path(txn);
	if (!beg)
		beg = end;

	for (ptr = beg; ptr < end && *ptr != '?'; ptr++);

	if (beg < ptr && *beg == '/') {
		while (beg < ptr)
			hash = *(beg++) + (hash << 6) + (hash << 16) - hash;
	}
	hash = full_hash(hash);

	smp->type = SMP_T_UINT;
	smp->data.uint = hash;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This concatenates the source address with the 32-bit hash of the Host and
 * URL as returned by smp_fetch_base32(). The idea is to have per-source and
 * per-url counters. The result is a binary block from 8 to 20 bytes depending
 * on the source address length. The URL hash is stored before the address so
 * that in environments where IPv6 is insignificant, truncating the output to
 * 8 bytes would still work.
 */
static int
smp_fetch_base32_src(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp)
{
	struct chunk *temp;

	if (!smp_fetch_base32(px, l4, l7, opt, args, smp))
		return 0;

	temp = get_trash_chunk();
	memcpy(temp->str + temp->len, &smp->data.uint, sizeof(smp->data.uint));
	temp->len += sizeof(smp->data.uint);

	switch (l4->si[0].conn->addr.from.ss_family) {
	case AF_INET:
		memcpy(temp->str + temp->len, &((struct sockaddr_in *)&l4->si[0].conn->addr.from)->sin_addr, 4);
		temp->len += 4;
		break;
	case AF_INET6:
		memcpy(temp->str + temp->len, &((struct sockaddr_in6 *)(&l4->si[0].conn->addr.from))->sin6_addr, 16);
		temp->len += 16;
		break;
	default:
		return 0;
	}

	smp->data.str = *temp;
	smp->type = SMP_T_BIN;
	return 1;
}

static int
acl_fetch_proto_http(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp)
{
	/* Note: hdr_idx.v cannot be NULL in this ACL because the ACL is tagged
	 * as a layer7 ACL, which involves automatic allocation of hdr_idx.
	 */

	CHECK_HTTP_MESSAGE_FIRST_PERM();

	smp->type = SMP_T_BOOL;
	smp->data.uint = 1;
	return 1;
}

/* return a valid test if the current request is the first one on the connection */
static int
acl_fetch_http_first_req(struct proxy *px, struct session *s, void *l7, unsigned int opt,
                         const struct arg *args, struct sample *smp)
{
	if (!s)
		return 0;

	smp->type = SMP_T_BOOL;
	smp->data.uint = !(s->txn.flags & TX_NOT_FIRST);
	return 1;
}

/* Accepts exactly 1 argument of type userlist */
static int
acl_fetch_http_auth(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                    const struct arg *args, struct sample *smp)
{

	if (!args || args->type != ARGT_USR)
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	if (!get_http_auth(l4))
		return 0;

	smp->type = SMP_T_BOOL;
	smp->data.uint = check_user(args->data.usr, 0, l4->txn.auth.user, l4->txn.auth.pass);
	return 1;
}

/* Accepts exactly 1 argument of type userlist */
static int
acl_fetch_http_auth_grp(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                        const struct arg *args, struct sample *smp)
{

	if (!args || args->type != ARGT_USR)
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	if (!get_http_auth(l4))
		return 0;

	/* acl_match_auth() will need several information at once */
	smp->ctx.a[0] = args->data.usr;      /* user list */
	smp->ctx.a[1] = l4->txn.auth.user;   /* user name */
	smp->ctx.a[2] = l4->txn.auth.pass;   /* password */

	/* if the user does not belong to the userlist or has a wrong password,
	 * report that it unconditionally does not match. Otherwise we return
	 * a non-zero integer which will be ignored anyway since all the params
	 * that acl_match_auth() will use are in test->ctx.a[0,1,2].
	 */
	smp->type = SMP_T_BOOL;
	smp->data.uint = check_user(args->data.usr, 0, l4->txn.auth.user, l4->txn.auth.pass);
	if (smp->data.uint)
		smp->type = SMP_T_UINT;

	return 1;
}

/* Try to find the next occurrence of a cookie name in a cookie header value.
 * The lookup begins at <hdr>. The pointer and size of the next occurrence of
 * the cookie value is returned into *value and *value_l, and the function
 * returns a pointer to the next pointer to search from if the value was found.
 * Otherwise if the cookie was not found, NULL is returned and neither value
 * nor value_l are touched. The input <hdr> string should first point to the
 * header's value, and the <hdr_end> pointer must point to the first character
 * not part of the value. <list> must be non-zero if value may represent a list
 * of values (cookie headers). This makes it faster to abort parsing when no
 * list is expected.
 */
static char *
extract_cookie_value(char *hdr, const char *hdr_end,
		  char *cookie_name, size_t cookie_name_l, int list,
		  char **value, int *value_l)
{
	char *equal, *att_end, *att_beg, *val_beg, *val_end;
	char *next;

	/* we search at least a cookie name followed by an equal, and more
	 * generally something like this :
	 * Cookie:    NAME1  =  VALUE 1  ; NAME2 = VALUE2 ; NAME3 = VALUE3\r\n
	 */
	for (att_beg = hdr; att_beg + cookie_name_l + 1 < hdr_end; att_beg = next + 1) {
		/* Iterate through all cookies on this line */

		while (att_beg < hdr_end && http_is_spht[(unsigned char)*att_beg])
			att_beg++;

		/* find att_end : this is the first character after the last non
		 * space before the equal. It may be equal to hdr_end.
		 */
		equal = att_end = att_beg;

		while (equal < hdr_end) {
			if (*equal == '=' || *equal == ';' || (list && *equal == ','))
				break;
			if (http_is_spht[(unsigned char)*equal++])
				continue;
			att_end = equal;
		}

		/* here, <equal> points to '=', a delimitor or the end. <att_end>
		 * is between <att_beg> and <equal>, both may be identical.
		 */

		/* look for end of cookie if there is an equal sign */
		if (equal < hdr_end && *equal == '=') {
			/* look for the beginning of the value */
			val_beg = equal + 1;
			while (val_beg < hdr_end && http_is_spht[(unsigned char)*val_beg])
				val_beg++;

			/* find the end of the value, respecting quotes */
			next = find_cookie_value_end(val_beg, hdr_end);

			/* make val_end point to the first white space or delimitor after the value */
			val_end = next;
			while (val_end > val_beg && http_is_spht[(unsigned char)*(val_end - 1)])
				val_end--;
		} else {
			val_beg = val_end = next = equal;
		}

		/* We have nothing to do with attributes beginning with '$'. However,
		 * they will automatically be removed if a header before them is removed,
		 * since they're supposed to be linked together.
		 */
		if (*att_beg == '$')
			continue;

		/* Ignore cookies with no equal sign */
		if (equal == next)
			continue;

		/* Now we have the cookie name between att_beg and att_end, and
		 * its value between val_beg and val_end.
		 */

		if (att_end - att_beg == cookie_name_l &&
		    memcmp(att_beg, cookie_name, cookie_name_l) == 0) {
			/* let's return this value and indicate where to go on from */
			*value = val_beg;
			*value_l = val_end - val_beg;
			return next + 1;
		}

		/* Set-Cookie headers only have the name in the first attr=value part */
		if (!list)
			break;
	}

	return NULL;
}

/* Iterate over all cookies present in a message. The context is stored in
 * smp->ctx.a[0] for the in-header position, smp->ctx.a[1] for the
 * end-of-header-value, and smp->ctx.a[2] for the hdr_idx. Depending on
 * the direction, multiple cookies may be parsed on the same line or not.
 * The cookie name is in args and the name length in args->data.str.len.
 * Accepts exactly 1 argument of type string. If the input options indicate
 * that no iterating is desired, then only last value is fetched if any.
 */
static int
smp_fetch_cookie(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                 const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	struct hdr_idx *idx = &txn->hdr_idx;
	struct hdr_ctx *ctx = (struct hdr_ctx *)&smp->ctx.a[2];
	const struct http_msg *msg;
	const char *hdr_name;
	int hdr_name_len;
	char *sol;
	int occ = 0;
	int found = 0;

	if (!args || args->type != ARGT_STR)
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	if ((opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ) {
		msg = &txn->req;
		hdr_name = "Cookie";
		hdr_name_len = 6;
	} else {
		msg = &txn->rsp;
		hdr_name = "Set-Cookie";
		hdr_name_len = 10;
	}

	if (!occ && !(opt & SMP_OPT_ITERATE))
		/* no explicit occurrence and single fetch => last cookie by default */
		occ = -1;

	/* OK so basically here, either we want only one value and it's the
	 * last one, or we want to iterate over all of them and we fetch the
	 * next one.
	 */

	sol = msg->chn->buf->p;
	if (!(smp->flags & SMP_F_NOT_LAST)) {
		/* search for the header from the beginning, we must first initialize
		 * the search parameters.
		 */
		smp->ctx.a[0] = NULL;
		ctx->idx = 0;
	}

	smp->flags |= SMP_F_VOL_HDR;

	while (1) {
		/* Note: smp->ctx.a[0] == NULL every time we need to fetch a new header */
		if (!smp->ctx.a[0]) {
			if (!http_find_header2(hdr_name, hdr_name_len, sol, idx, ctx))
				goto out;

			if (ctx->vlen < args->data.str.len + 1)
				continue;

			smp->ctx.a[0] = ctx->line + ctx->val;
			smp->ctx.a[1] = smp->ctx.a[0] + ctx->vlen;
		}

		smp->type = SMP_T_CSTR;
		smp->ctx.a[0] = extract_cookie_value(smp->ctx.a[0], smp->ctx.a[1],
						 args->data.str.str, args->data.str.len,
						 (opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
						 &smp->data.str.str,
						 &smp->data.str.len);
		if (smp->ctx.a[0]) {
			found = 1;
			if (occ >= 0) {
				/* one value was returned into smp->data.str.{str,len} */
				smp->flags |= SMP_F_NOT_LAST;
				return 1;
			}
		}
		/* if we're looking for last occurrence, let's loop */
	}
	/* all cookie headers and values were scanned. If we're looking for the
	 * last occurrence, we may return it now.
	 */
 out:
	smp->flags &= ~SMP_F_NOT_LAST;
	return found;
}

/* Iterate over all cookies present in a request to count how many occurrences
 * match the name in args and args->data.str.len. If <multi> is non-null, then
 * multiple cookies may be parsed on the same line.
 * Accepts exactly 1 argument of type string.
 */
static int
acl_fetch_cookie_cnt(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp)
{
	struct http_txn *txn = l7;
	struct hdr_idx *idx = &txn->hdr_idx;
	struct hdr_ctx ctx;
	const struct http_msg *msg;
	const char *hdr_name;
	int hdr_name_len;
	int cnt;
	char *val_beg, *val_end;
	char *sol;

	if (!args || args->type != ARGT_STR)
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	if ((opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ) {
		msg = &txn->req;
		hdr_name = "Cookie";
		hdr_name_len = 6;
	} else {
		msg = &txn->rsp;
		hdr_name = "Set-Cookie";
		hdr_name_len = 10;
	}

	sol = msg->chn->buf->p;
	val_end = val_beg = NULL;
	ctx.idx = 0;
	cnt = 0;

	while (1) {
		/* Note: val_beg == NULL every time we need to fetch a new header */
		if (!val_beg) {
			if (!http_find_header2(hdr_name, hdr_name_len, sol, idx, &ctx))
				break;

			if (ctx.vlen < args->data.str.len + 1)
				continue;

			val_beg = ctx.line + ctx.val;
			val_end = val_beg + ctx.vlen;
		}

		smp->type = SMP_T_CSTR;
		while ((val_beg = extract_cookie_value(val_beg, val_end,
						       args->data.str.str, args->data.str.len,
						       (opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
						       &smp->data.str.str,
						       &smp->data.str.len))) {
			cnt++;
		}
	}

	smp->data.uint = cnt;
	smp->flags |= SMP_F_VOL_HDR;
	return 1;
}

/* Fetch an cookie's integer value. The integer value is returned. It
 * takes a mandatory argument of type string. It relies on smp_fetch_cookie().
 */
static int
smp_fetch_cookie_val(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp)
{
	int ret = smp_fetch_cookie(px, l4, l7, opt, args, smp);

	if (ret > 0) {
		smp->type = SMP_T_UINT;
		smp->data.uint = strl2ic(smp->data.str.str, smp->data.str.len);
	}

	return ret;
}

/************************************************************************/
/*           The code below is dedicated to sample fetches              */
/************************************************************************/

/*
 * Given a path string and its length, find the position of beginning of the
 * query string. Returns NULL if no query string is found in the path.
 *
 * Example: if path = "/foo/bar/fubar?yo=mama;ye=daddy", and n = 22:
 *
 * find_query_string(path, n) points to "yo=mama;ye=daddy" string.
 */
static inline char *find_param_list(char *path, size_t path_l, char delim)
{
	char *p;

	p = memchr(path, delim, path_l);
	return p ? p + 1 : NULL;
}

static inline int is_param_delimiter(char c, char delim)
{
	return c == '&' || c == ';' || c == delim;
}

/*
 * Given a url parameter, find the starting position of the first occurence,
 * or NULL if the parameter is not found.
 *
 * Example: if query_string is "yo=mama;ye=daddy" and url_param_name is "ye",
 * the function will return query_string+8.
 */
static char*
find_url_param_pos(char* query_string, size_t query_string_l,
                   char* url_param_name, size_t url_param_name_l,
                   char delim)
{
	char *pos, *last;

	pos  = query_string;
	last = query_string + query_string_l - url_param_name_l - 1;

	while (pos <= last) {
		if (pos[url_param_name_l] == '=') {
			if (memcmp(pos, url_param_name, url_param_name_l) == 0)
				return pos;
			pos += url_param_name_l + 1;
		}
		while (pos <= last && !is_param_delimiter(*pos, delim))
			pos++;
		pos++;
	}
	return NULL;
}

/*
 * Given a url parameter name, returns its value and size into *value and
 * *value_l respectively, and returns non-zero. If the parameter is not found,
 * zero is returned and value/value_l are not touched.
 */
static int
find_url_param_value(char* path, size_t path_l,
                     char* url_param_name, size_t url_param_name_l,
                     char** value, int* value_l, char delim)
{
	char *query_string, *qs_end;
	char *arg_start;
	char *value_start, *value_end;

	query_string = find_param_list(path, path_l, delim);
	if (!query_string)
		return 0;

	qs_end = path + path_l;
	arg_start = find_url_param_pos(query_string, qs_end - query_string,
                                      url_param_name, url_param_name_l,
                                      delim);
	if (!arg_start)
		return 0;

	value_start = arg_start + url_param_name_l + 1;
	value_end = value_start;

	while ((value_end < qs_end) && !is_param_delimiter(*value_end, delim))
		value_end++;

	*value = value_start;
	*value_l = value_end - value_start;
	return value_end != value_start;
}

static int
smp_fetch_url_param(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                    const struct arg *args, struct sample *smp)
{
	char delim = '?';
	struct http_txn *txn = l7;
	struct http_msg *msg = &txn->req;

	if (!args || args[0].type != ARGT_STR ||
	    (args[1].type && args[1].type != ARGT_STR))
		return 0;

	CHECK_HTTP_MESSAGE_FIRST();

	if (args[1].type)
		delim = *args[1].data.str.str;

	if (!find_url_param_value(msg->chn->buf->p + msg->sl.rq.u, msg->sl.rq.u_l,
                                 args->data.str.str, args->data.str.len,
                                 &smp->data.str.str, &smp->data.str.len,
                                 delim))
		return 0;

	smp->type = SMP_T_CSTR;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* Return the signed integer value for the specified url parameter (see url_param
 * above).
 */
static int
smp_fetch_url_param_val(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                     const struct arg *args, struct sample *smp)
{
	int ret = smp_fetch_url_param(px, l4, l7, opt, args, smp);

	if (ret > 0) {
		smp->type = SMP_T_UINT;
		smp->data.uint = strl2ic(smp->data.str.str, smp->data.str.len);
	}

	return ret;
}

/* This function is used to validate the arguments passed to any "hdr" fetch
 * keyword. These keywords support an optional positive or negative occurrence
 * number. We must ensure that the number is greater than -MAX_HDR_HISTORY. It
 * is assumed that the types are already the correct ones. Returns 0 on error,
 * non-zero if OK. If <err> is not NULL, it will be filled with a pointer to an
 * error message in case of error, that the caller is responsible for freeing.
 * The initial location must either be freeable or NULL.
 */
static int val_hdr(struct arg *arg, char **err_msg)
{
	if (arg && arg[1].type == ARGT_SINT && arg[1].data.sint < -MAX_HDR_HISTORY) {
		memprintf(err_msg, "header occurrence must be >= %d", -MAX_HDR_HISTORY);
		return 0;
	}
	return 1;
}

/************************************************************************/
/*          All supported ACL keywords must be declared here.           */
/************************************************************************/

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {{ },{
	{ "base",            acl_parse_str,     smp_fetch_base,           acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, 0 },
	{ "base_beg",        acl_parse_str,     smp_fetch_base,           acl_match_beg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_dir",        acl_parse_str,     smp_fetch_base,           acl_match_dir,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_dom",        acl_parse_str,     smp_fetch_base,           acl_match_dom,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_end",        acl_parse_str,     smp_fetch_base,           acl_match_end,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_len",        acl_parse_int,     smp_fetch_base,           acl_match_len,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_reg",        acl_parse_reg,     smp_fetch_base,           acl_match_reg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "base_sub",        acl_parse_str,     smp_fetch_base,           acl_match_sub,     ACL_USE_L7REQ_VOLATILE, 0 },

	{ "cook",            acl_parse_str,     smp_fetch_cookie,         acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, ARG1(0,STR) },
	{ "cook_beg",        acl_parse_str,     smp_fetch_cookie,         acl_match_beg,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_cnt",        acl_parse_int,     acl_fetch_cookie_cnt,     acl_match_int,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_dir",        acl_parse_str,     smp_fetch_cookie,         acl_match_dir,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_dom",        acl_parse_str,     smp_fetch_cookie,         acl_match_dom,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_end",        acl_parse_str,     smp_fetch_cookie,         acl_match_end,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_len",        acl_parse_int,     smp_fetch_cookie,         acl_match_len,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_reg",        acl_parse_reg,     smp_fetch_cookie,         acl_match_reg,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_sub",        acl_parse_str,     smp_fetch_cookie,         acl_match_sub,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "cook_val",        acl_parse_int,     smp_fetch_cookie_val,     acl_match_int,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },

	{ "hdr",             acl_parse_str,     smp_fetch_hdr,            acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_beg",         acl_parse_str,     smp_fetch_hdr,            acl_match_beg,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_cnt",         acl_parse_int,     smp_fetch_hdr_cnt,        acl_match_int,     ACL_USE_L7REQ_VOLATILE, ARG1(0,STR) },
	{ "hdr_dir",         acl_parse_str,     smp_fetch_hdr,            acl_match_dir,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_dom",         acl_parse_str,     smp_fetch_hdr,            acl_match_dom,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_end",         acl_parse_str,     smp_fetch_hdr,            acl_match_end,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_ip",          acl_parse_ip,      smp_fetch_hdr_ip,         acl_match_ip,      ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_len",         acl_parse_int,     smp_fetch_hdr,            acl_match_len,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_reg",         acl_parse_reg,     smp_fetch_hdr,            acl_match_reg,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_sub",         acl_parse_str,     smp_fetch_hdr,            acl_match_sub,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "hdr_val",         acl_parse_int,     smp_fetch_hdr_val,        acl_match_int,     ACL_USE_L7REQ_VOLATILE, ARG2(0,STR,SINT), val_hdr },

	{ "http_auth",       acl_parse_nothing, acl_fetch_http_auth,      acl_match_nothing, ACL_USE_L7REQ_VOLATILE, ARG1(0,USR) },
	{ "http_auth_group", acl_parse_strcat,  acl_fetch_http_auth_grp,  acl_match_auth,    ACL_USE_L7REQ_VOLATILE, ARG1(0,USR) },
	{ "http_first_req",  acl_parse_nothing, acl_fetch_http_first_req, acl_match_nothing, ACL_USE_L7REQ_PERMANENT, 0 },

	{ "method",          acl_parse_meth,    acl_fetch_meth,           acl_match_meth,    ACL_USE_L7REQ_PERMANENT, 0 },

	{ "path",            acl_parse_str,     smp_fetch_path,           acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, 0 },
	{ "path_beg",        acl_parse_str,     smp_fetch_path,           acl_match_beg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_dir",        acl_parse_str,     smp_fetch_path,           acl_match_dir,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_dom",        acl_parse_str,     smp_fetch_path,           acl_match_dom,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_end",        acl_parse_str,     smp_fetch_path,           acl_match_end,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_len",        acl_parse_int,     smp_fetch_path,           acl_match_len,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_reg",        acl_parse_reg,     smp_fetch_path,           acl_match_reg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "path_sub",        acl_parse_str,     smp_fetch_path,           acl_match_sub,     ACL_USE_L7REQ_VOLATILE, 0 },

	{ "req_proto_http",  acl_parse_nothing, acl_fetch_proto_http,     acl_match_nothing, ACL_USE_L7REQ_PERMANENT, 0 },
	{ "req_ver",         acl_parse_ver,     acl_fetch_rqver,          acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, 0 },
	{ "resp_ver",        acl_parse_ver,     acl_fetch_stver,          acl_match_str,     ACL_USE_L7RTR_VOLATILE|ACL_MAY_LOOKUP, 0 },

	{ "scook",           acl_parse_str,     smp_fetch_cookie,         acl_match_str,     ACL_USE_L7RTR_VOLATILE|ACL_MAY_LOOKUP, ARG1(0,STR) },
	{ "scook_beg",       acl_parse_str,     smp_fetch_cookie,         acl_match_beg,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_cnt",       acl_parse_int,     acl_fetch_cookie_cnt,     acl_match_int,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_dir",       acl_parse_str,     smp_fetch_cookie,         acl_match_dir,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_dom",       acl_parse_str,     smp_fetch_cookie,         acl_match_dom,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_end",       acl_parse_str,     smp_fetch_cookie,         acl_match_end,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_len",       acl_parse_int,     smp_fetch_cookie,         acl_match_len,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_reg",       acl_parse_reg,     smp_fetch_cookie,         acl_match_reg,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_sub",       acl_parse_str,     smp_fetch_cookie,         acl_match_sub,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "scook_val",       acl_parse_int,     smp_fetch_cookie_val,     acl_match_int,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },

	{ "shdr",            acl_parse_str,     smp_fetch_hdr,            acl_match_str,     ACL_USE_L7RTR_VOLATILE|ACL_MAY_LOOKUP, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_beg",        acl_parse_str,     smp_fetch_hdr,            acl_match_beg,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_cnt",        acl_parse_int,     smp_fetch_hdr_cnt,        acl_match_int,     ACL_USE_L7RTR_VOLATILE, ARG1(0,STR) },
	{ "shdr_dir",        acl_parse_str,     smp_fetch_hdr,            acl_match_dir,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_dom",        acl_parse_str,     smp_fetch_hdr,            acl_match_dom,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_end",        acl_parse_str,     smp_fetch_hdr,            acl_match_end,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_ip",         acl_parse_ip,      smp_fetch_hdr_ip,         acl_match_ip,      ACL_USE_L7RTR_VOLATILE|ACL_MAY_LOOKUP, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_len",        acl_parse_int,     smp_fetch_hdr,            acl_match_len,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_reg",        acl_parse_reg,     smp_fetch_hdr,            acl_match_reg,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_sub",        acl_parse_str,     smp_fetch_hdr,            acl_match_sub,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },
	{ "shdr_val",        acl_parse_int,     smp_fetch_hdr_val,        acl_match_int,     ACL_USE_L7RTR_VOLATILE, ARG2(0,STR,SINT), val_hdr },

	{ "status",          acl_parse_int,     acl_fetch_stcode,         acl_match_int,     ACL_USE_L7RTR_PERMANENT, 0 },

	{ "url",             acl_parse_str,     smp_fetch_url,            acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, 0 },
	{ "url_beg",         acl_parse_str,     smp_fetch_url,            acl_match_beg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_dir",         acl_parse_str,     smp_fetch_url,            acl_match_dir,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_dom",         acl_parse_str,     smp_fetch_url,            acl_match_dom,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_end",         acl_parse_str,     smp_fetch_url,            acl_match_end,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_ip",          acl_parse_ip,      smp_fetch_url_ip,         acl_match_ip,      ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, 0 },
	{ "url_len",         acl_parse_int,     smp_fetch_url,            acl_match_len,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_port",        acl_parse_int,     smp_fetch_url_port,       acl_match_int,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_reg",         acl_parse_reg,     smp_fetch_url,            acl_match_reg,     ACL_USE_L7REQ_VOLATILE, 0 },
	{ "url_sub",         acl_parse_str,     smp_fetch_url,            acl_match_sub,     ACL_USE_L7REQ_VOLATILE, 0 },

	{ "urlp",            acl_parse_str,     smp_fetch_url_param,      acl_match_str,     ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, ARG1(1,STR) },
	{ "urlp_beg",        acl_parse_str,     smp_fetch_url_param,      acl_match_beg,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_dir",        acl_parse_str,     smp_fetch_url_param,      acl_match_dir,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_dom",        acl_parse_str,     smp_fetch_url_param,      acl_match_dom,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_end",        acl_parse_str,     smp_fetch_url_param,      acl_match_end,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_ip",         acl_parse_ip,      smp_fetch_url_param,      acl_match_ip,      ACL_USE_L7REQ_VOLATILE|ACL_MAY_LOOKUP, ARG1(1,STR) },
	{ "urlp_len",        acl_parse_int,     smp_fetch_url_param,      acl_match_len,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_reg",        acl_parse_reg,     smp_fetch_url_param,      acl_match_reg,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_sub",        acl_parse_str,     smp_fetch_url_param,      acl_match_sub,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },
	{ "urlp_val",        acl_parse_int,     smp_fetch_url_param_val,  acl_match_int,     ACL_USE_L7REQ_VOLATILE, ARG1(1,STR) },

	{ NULL, NULL, NULL, NULL },
}};

/************************************************************************/
/*         All supported pattern keywords must be declared here.        */
/************************************************************************/
/* Note: must not be declared <const> as its list will be overwritten */
static struct sample_fetch_kw_list sample_fetch_keywords = {{ },{
	{ "hdr",        smp_fetch_hdr,            ARG2(1,STR,SINT), val_hdr, SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ },
	{ "base",       smp_fetch_base,           0,                NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ },
	{ "base32",     smp_fetch_base32,         0,                NULL,    SMP_T_UINT, SMP_CAP_L7|SMP_CAP_REQ },
	{ "base32+src", smp_fetch_base32_src,     0,                NULL,    SMP_T_BIN,  SMP_CAP_L7|SMP_CAP_REQ },
	{ "path",       smp_fetch_path,           0,                NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ },
	{ "url",        smp_fetch_url,            0,                NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ },
	{ "url_ip",     smp_fetch_url_ip,         0,                NULL,    SMP_T_IPV4, SMP_CAP_L7|SMP_CAP_REQ },
	{ "url_port",   smp_fetch_url_port,       0,                NULL,    SMP_T_UINT, SMP_CAP_L7|SMP_CAP_REQ },
	{ "url_param",  smp_fetch_url_param,      ARG2(1,STR,STR),  NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ },
	{ "cookie",     smp_fetch_cookie,         ARG1(1,STR),      NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_REQ|SMP_CAP_RES },
	{ "set-cookie", smp_fetch_cookie,         ARG1(1,STR),      NULL,    SMP_T_CSTR, SMP_CAP_L7|SMP_CAP_RES }, /* deprecated */
	{ NULL, NULL, 0, 0, 0 },
}};


__attribute__((constructor))
static void __http_protocol_init(void)
{
	acl_register_keywords(&acl_kws);
	sample_register_fetches(&sample_fetch_keywords);
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
