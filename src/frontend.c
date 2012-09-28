/*
 * Frontend variables and functions.
 *
 * Copyright 2000-2011 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <common/chunk.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/acl.h>
#include <proto/arg.h>
#include <proto/channel.h>
#include <proto/fd.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/hdr_idx.h>
#include <proto/proto_tcp.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/session.h>
#include <proto/stream_interface.h>
#include <proto/task.h>

/* Finish a session accept() for a proxy (TCP or HTTP). It returns a negative
 * value in case of a critical failure which must cause the listener to be
 * disabled, a positive value in case of success, or zero if it is a success
 * but the session must be closed ASAP (eg: monitoring).
 */
int frontend_accept(struct session *s)
{
	int cfd = si_fd(&s->si[0]);

	tv_zero(&s->logs.tv_request);
	s->logs.t_queue = -1;
	s->logs.t_connect = -1;
	s->logs.t_data = -1;
	s->logs.t_close = 0;
	s->logs.bytes_in = s->logs.bytes_out = 0;
	s->logs.prx_queue_size = 0;  /* we get the number of pending conns before us */
	s->logs.srv_queue_size = 0; /* we will get this number soon */

	/* FIXME: the logs are horribly complicated now, because they are
	 * defined in <p>, <p>, and later <be> and <be>.
	 */
	s->do_log = sess_log;

	/* default error reporting function, may be changed by analysers */
	s->srv_error = default_srv_error;

	/* Adjust some socket options */
	if (s->listener->addr.ss_family == AF_INET || s->listener->addr.ss_family == AF_INET6) {
		if (setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
			       (char *) &one, sizeof(one)) == -1)
			goto out_return;

		if (s->fe->options & PR_O_TCP_CLI_KA)
			setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE,
				   (char *) &one, sizeof(one));

		if (s->fe->options & PR_O_TCP_NOLING)
			setsockopt(cfd, SOL_SOCKET, SO_LINGER,
				   (struct linger *) &nolinger, sizeof(struct linger));
#if defined(TCP_MAXSEG)
		if (s->listener->maxseg < 0) {
			/* we just want to reduce the current MSS by that value */
			int mss;
			socklen_t mss_len = sizeof(mss);
			if (getsockopt(cfd, IPPROTO_TCP, TCP_MAXSEG, &mss, &mss_len) == 0) {
				mss += s->listener->maxseg; /* remember, it's < 0 */
				setsockopt(cfd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
			}
		}
#endif
	}

	if (global.tune.client_sndbuf)
		setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &global.tune.client_sndbuf, sizeof(global.tune.client_sndbuf));

	if (global.tune.client_rcvbuf)
		setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &global.tune.client_rcvbuf, sizeof(global.tune.client_rcvbuf));

	if (s->fe->mode == PR_MODE_HTTP) {
		/* the captures are only used in HTTP frontends */
		if (unlikely(s->fe->nb_req_cap > 0 &&
			     (s->txn.req.cap = pool_alloc2(s->fe->req_cap_pool)) == NULL))
			goto out_return;	/* no memory */

		if (unlikely(s->fe->nb_rsp_cap > 0 &&
			     (s->txn.rsp.cap = pool_alloc2(s->fe->rsp_cap_pool)) == NULL))
			goto out_free_reqcap;	/* no memory */
	}

	if (s->fe->acl_requires & ACL_USE_L7_ANY) {
		/* we have to allocate header indexes only if we know
		 * that we may make use of them. This of course includes
		 * (mode == PR_MODE_HTTP).
		 */
		s->txn.hdr_idx.size = global.tune.max_http_hdr;

		if (unlikely((s->txn.hdr_idx.v = pool_alloc2(pool2_hdr_idx)) == NULL))
			goto out_free_rspcap; /* no memory */

		/* and now initialize the HTTP transaction state */
		http_init_txn(s);
	}

	if ((s->fe->mode == PR_MODE_TCP || s->fe->mode == PR_MODE_HTTP)
	    && (!LIST_ISEMPTY(&s->fe->logsrvs))) {
		if (likely(s->fe->to_log)) {
			/* we have the client ip */
			if (s->logs.logwait & LW_CLIP)
				if (!(s->logs.logwait &= ~LW_CLIP))
					s->do_log(s);
		}
		else {
			char pn[INET6_ADDRSTRLEN], sn[INET6_ADDRSTRLEN];

			conn_get_from_addr(&s->req->prod->conn);
			conn_get_to_addr(&s->req->prod->conn);

			switch (addr_to_str(&s->req->prod->conn.addr.from, pn, sizeof(pn))) {
			case AF_INET:
			case AF_INET6:
				addr_to_str(&s->req->prod->conn.addr.to, sn, sizeof(sn));
				send_log(s->fe, LOG_INFO, "Connect from %s:%d to %s:%d (%s/%s)\n",
					 pn, get_host_port(&s->req->prod->conn.addr.from),
					 sn, get_host_port(&s->req->prod->conn.addr.to),
					 s->fe->id, (s->fe->mode == PR_MODE_HTTP) ? "HTTP" : "TCP");
				break;
			case AF_UNIX:
				/* UNIX socket, only the destination is known */
				send_log(s->fe, LOG_INFO, "Connect to unix:%d (%s/%s)\n",
					 s->listener->luid,
					 s->fe->id, (s->fe->mode == PR_MODE_HTTP) ? "HTTP" : "TCP");
				break;
			}
		}
	}

	if (unlikely((global.mode & MODE_DEBUG) && (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)))) {
		char pn[INET6_ADDRSTRLEN];
		int len = 0;

		conn_get_from_addr(&s->req->prod->conn);

		switch (addr_to_str(&s->req->prod->conn.addr.from, pn, sizeof(pn))) {
		case AF_INET:
		case AF_INET6:
			len = sprintf(trash, "%08x:%s.accept(%04x)=%04x from [%s:%d]\n",
				      s->uniq_id, s->fe->id, (unsigned short)s->listener->fd, (unsigned short)cfd,
				      pn, get_host_port(&s->req->prod->conn.addr.from));
			break;
		case AF_UNIX:
			/* UNIX socket, only the destination is known */
			len = sprintf(trash, "%08x:%s.accept(%04x)=%04x from [unix:%d]\n",
				      s->uniq_id, s->fe->id, (unsigned short)s->listener->fd, (unsigned short)cfd,
				      s->listener->luid);
			break;
		}

		if (write(1, trash, len) < 0) /* shut gcc warning */;
	}

	if (s->fe->mode == PR_MODE_HTTP)
		s->req->flags |= CF_READ_DONTWAIT; /* one read is usually enough */

	/* note: this should not happen anymore since there's always at least the switching rules */
	if (!s->req->analysers) {
		channel_auto_connect(s->req);  /* don't wait to establish connection */
		channel_auto_close(s->req);    /* let the producer forward close requests */
	}

	s->req->rto = s->fe->timeout.client;
	s->rep->wto = s->fe->timeout.client;

	/* everything's OK, let's go on */
	return 1;

	/* Error unrolling */
 out_free_rspcap:
	pool_free2(s->fe->rsp_cap_pool, s->txn.rsp.cap);
 out_free_reqcap:
	pool_free2(s->fe->req_cap_pool, s->txn.req.cap);
 out_return:
	return -1;
}

/* This handshake handler waits a PROXY protocol header at the beginning of the
 * raw data stream. The header looks like this :
 *
 *   "PROXY" <SP> PROTO <SP> SRC3 <SP> DST3 <SP> SRC4 <SP> <DST4> "\r\n"
 *
 * There must be exactly one space between each field. Fields are :
 *  - PROTO : layer 4 protocol, which must be "TCP4" or "TCP6".
 *  - SRC3  : layer 3 (eg: IP) source address in standard text form
 *  - DST3  : layer 3 (eg: IP) destination address in standard text form
 *  - SRC4  : layer 4 (eg: TCP port) source address in standard text form
 *  - DST4  : layer 4 (eg: TCP port) destination address in standard text form
 *
 * This line MUST be at the beginning of the buffer and MUST NOT wrap.
 *
 * The header line is small and in all cases smaller than the smallest normal
 * TCP MSS. So it MUST always be delivered as one segment, which ensures we
 * can safely use MSG_PEEK and avoid buffering.
 *
 * Once the data is fetched, the values are set in the connection's address
 * fields, and data are removed from the socket's buffer. The function returns
 * zero if it needs to wait for more data or if it fails, or 1 if it completed
 * and removed itself.
 */
int conn_recv_proxy(struct connection *conn, int flag)
{
	char *line, *end;
	int len;

	/* we might have been called just after an asynchronous shutr */
	if (conn->flags & CO_FL_SOCK_RD_SH)
		goto fail;

	do {
		len = recv(conn->t.sock.fd, trash, trashlen, MSG_PEEK);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				conn_sock_poll_recv(conn);
				return 0;
			}
			goto fail;
		}
	} while (0);

	if (len < 6)
		goto missing;

	line = trash;
	end = trash + len;

	/* Decode a possible proxy request, fail early if it does not match */
	if (strncmp(line, "PROXY ", 6) != 0)
		goto fail;

	line += 6;
	if (len < 18) /* shortest possible line */
		goto missing;

	if (!memcmp(line, "TCP4 ", 5) != 0) {
		u32 src3, dst3, sport, dport;

		line += 5;

		src3 = inetaddr_host_lim_ret(line, end, &line);
		if (line == end)
			goto missing;
		if (*line++ != ' ')
			goto fail;

		dst3 = inetaddr_host_lim_ret(line, end, &line);
		if (line == end)
			goto missing;
		if (*line++ != ' ')
			goto fail;

		sport = read_uint((const char **)&line, end);
		if (line == end)
			goto missing;
		if (*line++ != ' ')
			goto fail;

		dport = read_uint((const char **)&line, end);
		if (line > end - 2)
			goto missing;
		if (*line++ != '\r')
			goto fail;
		if (*line++ != '\n')
			goto fail;

		/* update the session's addresses and mark them set */
		((struct sockaddr_in *)&conn->addr.from)->sin_family      = AF_INET;
		((struct sockaddr_in *)&conn->addr.from)->sin_addr.s_addr = htonl(src3);
		((struct sockaddr_in *)&conn->addr.from)->sin_port        = htons(sport);

		((struct sockaddr_in *)&conn->addr.to)->sin_family        = AF_INET;
		((struct sockaddr_in *)&conn->addr.to)->sin_addr.s_addr   = htonl(dst3);
		((struct sockaddr_in *)&conn->addr.to)->sin_port          = htons(dport);
		conn->flags |= CO_FL_ADDR_FROM_SET | CO_FL_ADDR_TO_SET;
	}
	else if (!memcmp(line, "TCP6 ", 5) != 0) {
		u32 sport, dport;
		char *src_s;
		char *dst_s, *sport_s, *dport_s;
		struct in6_addr src3, dst3;

		line += 5;

		src_s = line;
		dst_s = sport_s = dport_s = NULL;
		while (1) {
			if (line > end - 2) {
				goto missing;
			}
			else if (*line == '\r') {
				*line = 0;
				line++;
				if (*line++ != '\n')
					goto fail;
				break;
			}

			if (*line == ' ') {
				*line = 0;
				if (!dst_s)
					dst_s = line + 1;
				else if (!sport_s)
					sport_s = line + 1;
				else if (!dport_s)
					dport_s = line + 1;
			}
			line++;
		}

		if (!dst_s || !sport_s || !dport_s)
			goto fail;

		sport = read_uint((const char **)&sport_s,dport_s - 1);
		if (*sport_s != 0)
			goto fail;

		dport = read_uint((const char **)&dport_s,line - 2);
		if (*dport_s != 0)
			goto fail;

		if (inet_pton(AF_INET6, src_s, (void *)&src3) != 1)
			goto fail;

		if (inet_pton(AF_INET6, dst_s, (void *)&dst3) != 1)
			goto fail;

		/* update the session's addresses and mark them set */
		((struct sockaddr_in6 *)&conn->addr.from)->sin6_family      = AF_INET6;
		memcpy(&((struct sockaddr_in6 *)&conn->addr.from)->sin6_addr, &src3, sizeof(struct in6_addr));
		((struct sockaddr_in6 *)&conn->addr.from)->sin6_port        = htons(sport);

		((struct sockaddr_in6 *)&conn->addr.to)->sin6_family        = AF_INET6;
		memcpy(&((struct sockaddr_in6 *)&conn->addr.to)->sin6_addr, &dst3, sizeof(struct in6_addr));
		((struct sockaddr_in6 *)&conn->addr.to)->sin6_port          = htons(dport);
		conn->flags |= CO_FL_ADDR_FROM_SET | CO_FL_ADDR_TO_SET;
	}
	else {
		goto fail;
	}

	/* remove the PROXY line from the request. For this we re-read the
	 * exact line at once. If we don't get the exact same result, we
	 * fail.
	 */
	len = line - trash;
	do {
		int len2 = recv(conn->t.sock.fd, trash, len, 0);
		if (len2 < 0 && errno == EINTR)
			continue;
		if (len2 != len)
			goto fail;
	} while (0);

	conn->flags &= ~flag;
	return 1;

 missing:
	/* Missing data. Since we're using MSG_PEEK, we can only poll again if
	 * we have not read anything. Otherwise we need to fail because we won't
	 * be able to poll anymore.
	 */
 fail:
	conn_sock_stop_both(conn);
	conn->flags |= CO_FL_ERROR;
	conn->flags &= ~flag;
	return 0;
}

/* Makes a PROXY protocol line from the two addresses. The output is sent to
 * buffer <buf> for a maximum size of <buf_len> (including the trailing zero).
 * It returns the number of bytes composing this line (including the trailing
 * LF), or zero in case of failure (eg: not enough space). It supports TCP4,
 * TCP6 and "UNKNOWN" formats.
 */
int make_proxy_line(char *buf, int buf_len, struct sockaddr_storage *src, struct sockaddr_storage *dst)
{
	int ret = 0;

	if (src->ss_family == dst->ss_family && src->ss_family == AF_INET) {
		ret = snprintf(buf + ret, buf_len - ret, "PROXY TCP4 ");
		if (ret >= buf_len)
			return 0;

		/* IPv4 src */
		if (!inet_ntop(src->ss_family, &((struct sockaddr_in *)src)->sin_addr, buf + ret, buf_len - ret))
			return 0;

		ret += strlen(buf + ret);
		if (ret >= buf_len)
			return 0;

		buf[ret++] = ' ';

		/* IPv4 dst */
		if (!inet_ntop(dst->ss_family, &((struct sockaddr_in *)dst)->sin_addr, buf + ret, buf_len - ret))
			return 0;

		ret += strlen(buf + ret);
		if (ret >= buf_len)
			return 0;

		/* source and destination ports */
		ret += snprintf(buf + ret, buf_len - ret, " %u %u\r\n",
				ntohs(((struct sockaddr_in *)src)->sin_port),
				ntohs(((struct sockaddr_in *)dst)->sin_port));
		if (ret >= buf_len)
			return 0;
	}
	else if (src->ss_family == dst->ss_family && src->ss_family == AF_INET6) {
		ret = snprintf(buf + ret, buf_len - ret, "PROXY TCP6 ");
		if (ret >= buf_len)
			return 0;

		/* IPv6 src */
		if (!inet_ntop(src->ss_family, &((struct sockaddr_in6 *)src)->sin6_addr, buf + ret, buf_len - ret))
			return 0;

		ret += strlen(buf + ret);
		if (ret >= buf_len)
			return 0;

		buf[ret++] = ' ';

		/* IPv6 dst */
		if (!inet_ntop(dst->ss_family, &((struct sockaddr_in6 *)dst)->sin6_addr, buf + ret, buf_len - ret))
			return 0;

		ret += strlen(buf + ret);
		if (ret >= buf_len)
			return 0;

		/* source and destination ports */
		ret += snprintf(buf + ret, buf_len - ret, " %u %u\r\n",
				ntohs(((struct sockaddr_in6 *)src)->sin6_port),
				ntohs(((struct sockaddr_in6 *)dst)->sin6_port));
		if (ret >= buf_len)
			return 0;
	}
	else {
		/* unknown family combination */
		ret = snprintf(buf, buf_len, "PROXY UNKNOWN\r\n");
		if (ret >= buf_len)
			return 0;
	}
	return ret;
}

/* set temp integer to the id of the frontend */
static int
acl_fetch_fe_id(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                const struct arg *args, struct sample *smp)
{
	smp->flags = SMP_F_VOL_SESS;
	smp->type = SMP_T_UINT;
	smp->data.uint = l4->fe->uuid;
	return 1;
}

/* set temp integer to the number of connections per second reaching the frontend.
 * Accepts exactly 1 argument. Argument is a frontend, other types will cause
 * an undefined behaviour.
 */
static int
acl_fetch_fe_sess_rate(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                       const struct arg *args, struct sample *smp)
{
	smp->flags = SMP_F_VOL_TEST;
	smp->type = SMP_T_UINT;
	smp->data.uint = read_freq_ctr(&args->data.prx->fe_sess_per_sec);
	return 1;
}

/* set temp integer to the number of concurrent connections on the frontend
 * Accepts exactly 1 argument. Argument is a frontend, other types will cause
 * an undefined behaviour.
 */
static int
acl_fetch_fe_conn(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                  const struct arg *args, struct sample *smp)
{
	smp->flags = SMP_F_VOL_TEST;
	smp->type = SMP_T_UINT;
	smp->data.uint = args->data.prx->feconn;
	return 1;
}


/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {{ },{
	{ "fe_conn",      acl_parse_int, acl_fetch_fe_conn,      acl_match_int, ACL_USE_NOTHING, ARG1(1,FE) },
	{ "fe_id",        acl_parse_int, acl_fetch_fe_id,        acl_match_int, ACL_USE_NOTHING, 0 },
	{ "fe_sess_rate", acl_parse_int, acl_fetch_fe_sess_rate, acl_match_int, ACL_USE_NOTHING, ARG1(1,FE) },
	{ NULL, NULL, NULL, NULL },
}};


__attribute__((constructor))
static void __frontend_init(void)
{
	acl_register_keywords(&acl_kws);
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
