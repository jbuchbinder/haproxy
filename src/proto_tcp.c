/*
 * AF_INET/AF_INET6 SOCK_STREAM protocol layer (tcp)
 *
 * Copyright 2000-2013 Willy Tarreau <w@1wt.eu>
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
#include <time.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netinet/tcp.h>

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/errors.h>
#include <common/mini-clist.h>
#include <common/standard.h>

#include <types/global.h>
#include <types/server.h>

#include <proto/acl.h>
#include <proto/arg.h>
#include <proto/channel.h>
#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/listener.h>
#include <proto/log.h>
#include <proto/port_range.h>
#include <proto/protocol.h>
#include <proto/proto_tcp.h>
#include <proto/proxy.h>
#include <proto/sample.h>
#include <proto/session.h>
#include <proto/stick_table.h>
#include <proto/task.h>

#ifdef CONFIG_HAP_CTTPROXY
#include <import/ip_tproxy.h>
#endif

static int tcp_bind_listeners(struct protocol *proto, char *errmsg, int errlen);
static int tcp_bind_listener(struct listener *listener, char *errmsg, int errlen);

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_tcpv4 = {
	.name = "tcpv4",
	.sock_domain = AF_INET,
	.sock_type = SOCK_STREAM,
	.sock_prot = IPPROTO_TCP,
	.sock_family = AF_INET,
	.sock_addrlen = sizeof(struct sockaddr_in),
	.l3_addrlen = 32/8,
	.accept = &listener_accept,
	.connect = tcp_connect_server,
	.bind = tcp_bind_listener,
	.bind_all = tcp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.get_src = tcp_get_src,
	.get_dst = tcp_get_dst,
	.listeners = LIST_HEAD_INIT(proto_tcpv4.listeners),
	.nb_listeners = 0,
};

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_tcpv6 = {
	.name = "tcpv6",
	.sock_domain = AF_INET6,
	.sock_type = SOCK_STREAM,
	.sock_prot = IPPROTO_TCP,
	.sock_family = AF_INET6,
	.sock_addrlen = sizeof(struct sockaddr_in6),
	.l3_addrlen = 128/8,
	.accept = &listener_accept,
	.connect = tcp_connect_server,
	.bind = tcp_bind_listener,
	.bind_all = tcp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.get_src = tcp_get_src,
	.get_dst = tcp_get_dst,
	.listeners = LIST_HEAD_INIT(proto_tcpv6.listeners),
	.nb_listeners = 0,
};


/* Binds ipv4/ipv6 address <local> to socket <fd>, unless <flags> is set, in which
 * case we try to bind <remote>. <flags> is a 2-bit field consisting of :
 *  - 0 : ignore remote address (may even be a NULL pointer)
 *  - 1 : use provided address
 *  - 2 : use provided port
 *  - 3 : use both
 *
 * The function supports multiple foreign binding methods :
 *   - linux_tproxy: we directly bind to the foreign address
 *   - cttproxy: we bind to a local address then nat.
 * The second one can be used as a fallback for the first one.
 * This function returns 0 when everything's OK, 1 if it could not bind, to the
 * local address, 2 if it could not bind to the foreign address.
 */
int tcp_bind_socket(int fd, int flags, struct sockaddr_storage *local, struct sockaddr_storage *remote)
{
	struct sockaddr_storage bind_addr;
	int foreign_ok = 0;
	int ret;

#ifdef CONFIG_HAP_LINUX_TPROXY
	static int ip_transp_working = 1;
	static int ip6_transp_working = 1;
	switch (local->ss_family) {
	case AF_INET:
		if (flags && ip_transp_working) {
			if (setsockopt(fd, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) == 0
			    || setsockopt(fd, SOL_IP, IP_FREEBIND, &one, sizeof(one)) == 0)
				foreign_ok = 1;
			else
				ip_transp_working = 0;
		}
		break;
	case AF_INET6:
		if (flags && ip6_transp_working) {
			if (setsockopt(fd, SOL_IPV6, IPV6_TRANSPARENT, &one, sizeof(one)) == 0)
				foreign_ok = 1;
			else
				ip6_transp_working = 0;
		}
		break;
	}
#endif
	if (flags) {
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.ss_family = remote->ss_family;
		switch (remote->ss_family) {
		case AF_INET:
			if (flags & 1)
				((struct sockaddr_in *)&bind_addr)->sin_addr = ((struct sockaddr_in *)remote)->sin_addr;
			if (flags & 2)
				((struct sockaddr_in *)&bind_addr)->sin_port = ((struct sockaddr_in *)remote)->sin_port;
			break;
		case AF_INET6:
			if (flags & 1)
				((struct sockaddr_in6 *)&bind_addr)->sin6_addr = ((struct sockaddr_in6 *)remote)->sin6_addr;
			if (flags & 2)
				((struct sockaddr_in6 *)&bind_addr)->sin6_port = ((struct sockaddr_in6 *)remote)->sin6_port;
			break;
		default:
			/* we don't want to try to bind to an unknown address family */
			foreign_ok = 0;
		}
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (foreign_ok) {
		if (is_addr(&bind_addr)) {
			ret = bind(fd, (struct sockaddr *)&bind_addr, get_addr_len(&bind_addr));
			if (ret < 0)
				return 2;
		}
	}
	else {
		if (is_addr(local)) {
			ret = bind(fd, (struct sockaddr *)local, get_addr_len(local));
			if (ret < 0)
				return 1;
		}
	}

	if (!flags)
		return 0;

#ifdef CONFIG_HAP_CTTPROXY
	if (!foreign_ok && remote->ss_family == AF_INET) {
		struct in_tproxy itp1, itp2;
		memset(&itp1, 0, sizeof(itp1));

		itp1.op = TPROXY_ASSIGN;
		itp1.v.addr.faddr = ((struct sockaddr_in *)&bind_addr)->sin_addr;
		itp1.v.addr.fport = ((struct sockaddr_in *)&bind_addr)->sin_port;

		/* set connect flag on socket */
		itp2.op = TPROXY_FLAGS;
		itp2.v.flags = ITP_CONNECT | ITP_ONCE;

		if (setsockopt(fd, SOL_IP, IP_TPROXY, &itp1, sizeof(itp1)) != -1 &&
		    setsockopt(fd, SOL_IP, IP_TPROXY, &itp2, sizeof(itp2)) != -1) {
			foreign_ok = 1;
		}
	}
#endif
	if (!foreign_ok)
		/* we could not bind to a foreign address */
		return 2;

	return 0;
}


/*
 * This function initiates a TCP connection establishment to the target assigned
 * to connection <conn> using (si->{target,addr.to}). A source address may be
 * pointed to by conn->addr.from in case of transparent proxying. Normal source
 * bind addresses are still determined locally (due to the possible need of a
 * source port). conn->target may point either to a valid server or to a backend,
 * depending on conn->target. Only OBJ_TYPE_PROXY and OBJ_TYPE_SERVER are
 * supported. The <data> parameter is a boolean indicating whether there are data
 * waiting for being sent or not, in order to adjust data write polling and on
 * some platforms, the ability to avoid an empty initial ACK. The <delack> argument
 * allows the caller to force using a delayed ACK when establishing the connection :
 *   - 0 = no delayed ACK unless data are advertised and backend has tcp-smart-connect
 *   - 1 = delayed ACK if backend has tcp-smart-connect, regardless of data
 *   - 2 = delayed ACK regardless of backend options
 *
 * It can return one of :
 *  - SN_ERR_NONE if everything's OK
 *  - SN_ERR_SRVTO if there are no more servers
 *  - SN_ERR_SRVCL if the connection was refused by the server
 *  - SN_ERR_PRXCOND if the connection has been limited by the proxy (maxconn)
 *  - SN_ERR_RESOURCE if a system resource is lacking (eg: fd limits, ports, ...)
 *  - SN_ERR_INTERNAL for any other purely internal errors
 * Additionnally, in the case of SN_ERR_RESOURCE, an emergency log will be emitted.
 *
 * The connection's fd is inserted only when SN_ERR_NONE is returned, otherwise
 * it's invalid and the caller has nothing to do.
 */

int tcp_connect_server(struct connection *conn, int data, int delack)
{
	int fd;
	struct server *srv;
	struct proxy *be;
	struct conn_src *src;

	switch (obj_type(conn->target)) {
	case OBJ_TYPE_PROXY:
		be = objt_proxy(conn->target);
		srv = NULL;
		break;
	case OBJ_TYPE_SERVER:
		srv = objt_server(conn->target);
		be = srv->proxy;
		break;
	default:
		return SN_ERR_INTERNAL;
	}

	if ((fd = conn->t.sock.fd = socket(conn->addr.to.ss_family, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		qfprintf(stderr, "Cannot get a server socket.\n");

		if (errno == ENFILE)
			send_log(be, LOG_EMERG,
				 "Proxy %s reached system FD limit at %d. Please check system tunables.\n",
				 be->id, maxfd);
		else if (errno == EMFILE)
			send_log(be, LOG_EMERG,
				 "Proxy %s reached process FD limit at %d. Please check 'ulimit-n' and restart.\n",
				 be->id, maxfd);
		else if (errno == ENOBUFS || errno == ENOMEM)
			send_log(be, LOG_EMERG,
				 "Proxy %s reached system memory limit at %d sockets. Please check system tunables.\n",
				 be->id, maxfd);
		/* this is a resource error */
		return SN_ERR_RESOURCE;
	}

	if (fd >= global.maxsock) {
		/* do not log anything there, it's a normal condition when this option
		 * is used to serialize connections to a server !
		 */
		Alert("socket(): not enough free sockets. Raise -n argument. Giving up.\n");
		close(fd);
		return SN_ERR_PRXCOND; /* it is a configuration limit */
	}

	if ((fcntl(fd, F_SETFL, O_NONBLOCK)==-1) ||
	    (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == -1)) {
		qfprintf(stderr,"Cannot set client socket to non blocking mode.\n");
		close(fd);
		return SN_ERR_INTERNAL;
	}

	if (be->options & PR_O_TCP_SRV_KA)
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

	/* allow specific binding :
	 * - server-specific at first
	 * - proxy-specific next
	 */
	if (srv && srv->conn_src.opts & CO_SRC_BIND)
		src = &srv->conn_src;
	else if (be->conn_src.opts & CO_SRC_BIND)
		src = &be->conn_src;
	else
		src = NULL;

	if (src) {
		int ret, flags = 0;

		if (is_addr(&conn->addr.from)) {
			switch (src->opts & CO_SRC_TPROXY_MASK) {
			case CO_SRC_TPROXY_ADDR:
			case CO_SRC_TPROXY_CLI:
				flags = 3;
				break;
			case CO_SRC_TPROXY_CIP:
			case CO_SRC_TPROXY_DYN:
				flags = 1;
				break;
			}
		}

#ifdef SO_BINDTODEVICE
		/* Note: this might fail if not CAP_NET_RAW */
		if (src->iface_name)
			setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, src->iface_name, src->iface_len + 1);
#endif

		if (src->sport_range) {
			int attempts = 10; /* should be more than enough to find a spare port */
			struct sockaddr_storage sa;

			ret = 1;
			sa = src->source_addr;

			do {
				/* note: in case of retry, we may have to release a previously
				 * allocated port, hence this loop's construct.
				 */
				port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
				fdinfo[fd].port_range = NULL;

				if (!attempts)
					break;
				attempts--;

				fdinfo[fd].local_port = port_range_alloc_port(src->sport_range);
				if (!fdinfo[fd].local_port)
					break;

				fdinfo[fd].port_range = src->sport_range;
				set_host_port(&sa, fdinfo[fd].local_port);

				ret = tcp_bind_socket(fd, flags, &sa, &conn->addr.from);
			} while (ret != 0); /* binding NOK */
		}
		else {
			ret = tcp_bind_socket(fd, flags, &src->source_addr, &conn->addr.from);
		}

		if (unlikely(ret != 0)) {
			port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
			fdinfo[fd].port_range = NULL;
			close(fd);

			if (ret == 1) {
				Alert("Cannot bind to source address before connect() for backend %s. Aborting.\n",
				      be->id);
				send_log(be, LOG_EMERG,
					 "Cannot bind to source address before connect() for backend %s.\n",
					 be->id);
			} else {
				Alert("Cannot bind to tproxy source address before connect() for backend %s. Aborting.\n",
				      be->id);
				send_log(be, LOG_EMERG,
					 "Cannot bind to tproxy source address before connect() for backend %s.\n",
					 be->id);
			}
			return SN_ERR_RESOURCE;
		}
	}

#if defined(TCP_QUICKACK)
	/* disabling tcp quick ack now allows the first request to leave the
	 * machine with the first ACK. We only do this if there are pending
	 * data in the buffer.
	 */
	if (delack == 2 || ((delack || data) && (be->options2 & PR_O2_SMARTCON)))
                setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof(zero));
#endif

	if (global.tune.server_sndbuf)
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &global.tune.server_sndbuf, sizeof(global.tune.server_sndbuf));

	if (global.tune.server_rcvbuf)
                setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &global.tune.server_rcvbuf, sizeof(global.tune.server_rcvbuf));

	if ((connect(fd, (struct sockaddr *)&conn->addr.to, get_addr_len(&conn->addr.to)) == -1) &&
	    (errno != EINPROGRESS) && (errno != EALREADY) && (errno != EISCONN)) {

		if (errno == EAGAIN || errno == EADDRINUSE || errno == EADDRNOTAVAIL) {
			char *msg;
			if (errno == EAGAIN || errno == EADDRNOTAVAIL)
				msg = "no free ports";
			else
				msg = "local address already in use";

			qfprintf(stderr,"Connect() failed for backend %s: %s.\n", be->id, msg);
			port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
			fdinfo[fd].port_range = NULL;
			close(fd);
			send_log(be, LOG_ERR, "Connect() failed for backend %s: %s.\n", be->id, msg);
			return SN_ERR_RESOURCE;
		} else if (errno == ETIMEDOUT) {
			//qfprintf(stderr,"Connect(): ETIMEDOUT");
			port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
			fdinfo[fd].port_range = NULL;
			close(fd);
			return SN_ERR_SRVTO;
		} else {
			// (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EACCES || errno == EPERM)
			//qfprintf(stderr,"Connect(): %d", errno);
			port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
			fdinfo[fd].port_range = NULL;
			close(fd);
			return SN_ERR_SRVCL;
		}
	}

	fdtab[fd].owner = conn;
	conn->flags  = CO_FL_WAIT_L4_CONN; /* connection in progress */
	conn->flags |= CO_FL_ADDR_TO_SET;

	fdtab[fd].iocb = conn_fd_handler;
	fd_insert(fd);
	conn_sock_want_send(conn);  /* for connect status */

	if (conn_xprt_init(conn) < 0) {
		fd_delete(fd);
		return SN_ERR_RESOURCE;
	}

	if (data)
		conn_data_want_send(conn);  /* prepare to send data if any */

	return SN_ERR_NONE;  /* connection is OK */
}


/*
 * Retrieves the source address for the socket <fd>, with <dir> indicating
 * if we're a listener (=0) or an initiator (!=0). It returns 0 in case of
 * success, -1 in case of error. The socket's source address is stored in
 * <sa> for <salen> bytes.
 */
int tcp_get_src(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	if (dir)
		return getsockname(fd, sa, &salen);
	else
		return getpeername(fd, sa, &salen);
}


/*
 * Retrieves the original destination address for the socket <fd>, with <dir>
 * indicating if we're a listener (=0) or an initiator (!=0). In the case of a
 * listener, if the original destination address was translated, the original
 * address is retrieved. It returns 0 in case of success, -1 in case of error.
 * The socket's source address is stored in <sa> for <salen> bytes.
 */
int tcp_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	if (dir)
		return getpeername(fd, sa, &salen);
#if defined(TPROXY) && defined(SO_ORIGINAL_DST)
	else if (getsockopt(fd, SOL_IP, SO_ORIGINAL_DST, sa, &salen) == 0)
		return 0;
#endif
	else
		return getsockname(fd, sa, &salen);
}

/* This is the callback which is set when a connection establishment is pending
 * and we have nothing to send, or if we have an init function we want to call
 * once the connection is established. It updates the FD polling status. It
 * returns 0 if it fails in a fatal way or needs to poll to go further, otherwise
 * it returns non-zero and removes itself from the connection's flags (the bit is
 * provided in <flag> by the caller).
 */
int tcp_connect_probe(struct connection *conn)
{
	int fd = conn->t.sock.fd;

	if (conn->flags & CO_FL_ERROR)
		return 0;

	if (!(conn->flags & CO_FL_WAIT_L4_CONN))
		return 1; /* strange we were called while ready */

	/* stop here if we reached the end of data */
	if ((fdtab[fd].ev & (FD_POLL_IN|FD_POLL_HUP)) == FD_POLL_HUP)
		goto out_error;

	/* We have no data to send to check the connection, and
	 * getsockopt() will not inform us whether the connection
	 * is still pending. So we'll reuse connect() to check the
	 * state of the socket. This has the advantage of giving us
	 * the following info :
	 *  - error
	 *  - connecting (EALREADY, EINPROGRESS)
	 *  - connected (EISCONN, 0)
	 */
	if (connect(fd, (struct sockaddr *)&conn->addr.to, get_addr_len(&conn->addr.to)) < 0) {
		if (errno == EALREADY || errno == EINPROGRESS) {
			__conn_sock_stop_recv(conn);
			__conn_sock_poll_send(conn);
			return 0;
		}

		if (errno && errno != EISCONN)
			goto out_error;

		/* otherwise we're connected */
	}

	/* The FD is ready now, we'll mark the connection as complete and
	 * forward the event to the transport layer which will notify the
	 * data layer.
	 */
	conn->flags &= ~CO_FL_WAIT_L4_CONN;
	return 1;

 out_error:
	/* Write error on the file descriptor. Report it to the connection
	 * and disable polling on this FD.
	 */

	conn->flags |= CO_FL_ERROR;
	__conn_sock_stop_both(conn);
	return 0;
}


/* This function tries to bind a TCPv4/v6 listener. It may return a warning or
 * an error message in <errmsg> if the message is at most <errlen> bytes long
 * (including '\0'). Note that <errmsg> may be NULL if <errlen> is also zero.
 * The return value is composed from ERR_ABORT, ERR_WARN,
 * ERR_ALERT, ERR_RETRYABLE and ERR_FATAL. ERR_NONE indicates that everything
 * was alright and that no message was returned. ERR_RETRYABLE means that an
 * error occurred but that it may vanish after a retry (eg: port in use), and
 * ERR_FATAL indicates a non-fixable error. ERR_WARN and ERR_ALERT do not alter
 * the meaning of the error, but just indicate that a message is present which
 * should be displayed with the respective level. Last, ERR_ABORT indicates
 * that it's pointless to try to start other listeners. No error message is
 * returned if errlen is NULL.
 */
int tcp_bind_listener(struct listener *listener, char *errmsg, int errlen)
{
	__label__ tcp_return, tcp_close_return;
	int fd, err;
	int ext, ready;
	socklen_t ready_len;
	const char *msg = NULL;

	/* ensure we never return garbage */
	if (errlen)
		*errmsg = 0;

	if (listener->state != LI_ASSIGNED)
		return ERR_NONE; /* already bound */

	err = ERR_NONE;

	/* if the listener already has an fd assigned, then we were offered the
	 * fd by an external process (most likely the parent), and we don't want
	 * to create a new socket. However we still want to set a few flags on
	 * the socket.
	 */
	fd = listener->fd;
	ext = (fd >= 0);

	if (!ext && (fd = socket(listener->addr.ss_family, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot create listening socket";
		goto tcp_return;
	}

	if (fd >= global.maxsock) {
		err |= ERR_FATAL | ERR_ABORT | ERR_ALERT;
		msg = "not enough free sockets (raise '-n' parameter)";
		goto tcp_close_return;
	}

	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot make socket non-blocking";
		goto tcp_close_return;
	}

	if (!ext && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
		/* not fatal but should be reported */
		msg = "cannot do so_reuseaddr";
		err |= ERR_ALERT;
	}

	if (listener->options & LI_O_NOLINGER)
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &nolinger, sizeof(struct linger));

#ifdef SO_REUSEPORT
	/* OpenBSD supports this. As it's present in old libc versions of Linux,
	 * it might return an error that we will silently ignore.
	 */
	if (!ext)
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
#ifdef CONFIG_HAP_LINUX_TPROXY
	if (!ext && (listener->options & LI_O_FOREIGN)) {
		switch (listener->addr.ss_family) {
		case AF_INET:
			if ((setsockopt(fd, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) == -1)
			    && (setsockopt(fd, SOL_IP, IP_FREEBIND, &one, sizeof(one)) == -1)) {
				msg = "cannot make listening socket transparent";
				err |= ERR_ALERT;
			}
		break;
		case AF_INET6:
			if (setsockopt(fd, SOL_IPV6, IPV6_TRANSPARENT, &one, sizeof(one)) == -1) {
				msg = "cannot make listening socket transparent";
				err |= ERR_ALERT;
			}
		break;
		}
	}
#endif
#ifdef SO_BINDTODEVICE
	/* Note: this might fail if not CAP_NET_RAW */
	if (!ext && listener->interface) {
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
			       listener->interface, strlen(listener->interface) + 1) == -1) {
			msg = "cannot bind listener to device";
			err |= ERR_WARN;
		}
	}
#endif
#if defined(TCP_MAXSEG)
	if (listener->maxseg > 0) {
		if (setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG,
			       &listener->maxseg, sizeof(listener->maxseg)) == -1) {
			msg = "cannot set MSS";
			err |= ERR_WARN;
		}
	}
#endif
#if defined(TCP_DEFER_ACCEPT)
	if (listener->options & LI_O_DEF_ACCEPT) {
		/* defer accept by up to one second */
		int accept_delay = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &accept_delay, sizeof(accept_delay)) == -1) {
			msg = "cannot enable DEFER_ACCEPT";
			err |= ERR_WARN;
		}
	}
#endif
#if defined(TCP_FASTOPEN)
	if (listener->options & LI_O_TCP_FO) {
		/* TFO needs a queue length, let's use the configured backlog */
		int qlen = listener->backlog ? listener->backlog : listener->maxconn;
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) == -1) {
			msg = "cannot enable TCP_FASTOPEN";
			err |= ERR_WARN;
		}
	}
#endif
#if defined(IPV6_V6ONLY)
	if (listener->options & LI_O_V6ONLY)
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	else if (listener->options & LI_O_V4V6)
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
#endif

	if (!ext && bind(fd, (struct sockaddr *)&listener->addr, listener->proto->sock_addrlen) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot bind socket";
		goto tcp_close_return;
	}

	ready = 0;
	ready_len = sizeof(ready);
	if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &ready, &ready_len) == -1)
		ready = 0;

	if (!(ext && ready) && /* only listen if not already done by external process */
	    listen(fd, listener->backlog ? listener->backlog : listener->maxconn) == -1) {
		err |= ERR_RETRYABLE | ERR_ALERT;
		msg = "cannot listen to socket";
		goto tcp_close_return;
	}

#if defined(TCP_QUICKACK)
	if (listener->options & LI_O_NOQUICKACK)
		setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof(zero));
#endif

	/* the socket is ready */
	listener->fd = fd;
	listener->state = LI_LISTEN;

	fdtab[fd].owner = listener; /* reference the listener instead of a task */
	fdtab[fd].iocb = listener->proto->accept;
	fd_insert(fd);

 tcp_return:
	if (msg && errlen) {
		char pn[INET6_ADDRSTRLEN];

		addr_to_str(&listener->addr, pn, sizeof(pn));
		snprintf(errmsg, errlen, "%s [%s:%d]", msg, pn, get_host_port(&listener->addr));
	}
	return err;

 tcp_close_return:
	close(fd);
	goto tcp_return;
}

/* This function creates all TCP sockets bound to the protocol entry <proto>.
 * It is intended to be used as the protocol's bind_all() function.
 * The sockets will be registered but not added to any fd_set, in order not to
 * loose them across the fork(). A call to enable_all_listeners() is needed
 * to complete initialization. The return value is composed from ERR_*.
 */
static int tcp_bind_listeners(struct protocol *proto, char *errmsg, int errlen)
{
	struct listener *listener;
	int err = ERR_NONE;

	list_for_each_entry(listener, &proto->listeners, proto_list) {
		err |= tcp_bind_listener(listener, errmsg, errlen);
		if (err & ERR_ABORT)
			break;
	}

	return err;
}

/* Add listener to the list of tcpv4 listeners. The listener's state
 * is automatically updated from LI_INIT to LI_ASSIGNED. The number of
 * listeners is updated. This is the function to use to add a new listener.
 */
void tcpv4_add_listener(struct listener *listener)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->proto = &proto_tcpv4;
	LIST_ADDQ(&proto_tcpv4.listeners, &listener->proto_list);
	proto_tcpv4.nb_listeners++;
}

/* Add listener to the list of tcpv4 listeners. The listener's state
 * is automatically updated from LI_INIT to LI_ASSIGNED. The number of
 * listeners is updated. This is the function to use to add a new listener.
 */
void tcpv6_add_listener(struct listener *listener)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->proto = &proto_tcpv6;
	LIST_ADDQ(&proto_tcpv6.listeners, &listener->proto_list);
	proto_tcpv6.nb_listeners++;
}

/* This function performs the TCP request analysis on the current request. It
 * returns 1 if the processing can continue on next analysers, or zero if it
 * needs more data, encounters an error, or wants to immediately abort the
 * request. It relies on buffers flags, and updates s->req->analysers. The
 * function may be called for frontend rules and backend rules. It only relies
 * on the backend pointer so this works for both cases.
 */
int tcp_inspect_request(struct session *s, struct channel *req, int an_bit)
{
	struct tcp_rule *rule;
	struct stksess *ts;
	struct stktable *t;
	int partial;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		req->buf->i,
		req->analysers);

	/* We don't know whether we have enough data, so must proceed
	 * this way :
	 * - iterate through all rules in their declaration order
	 * - if one rule returns MISS, it means the inspect delay is
	 *   not over yet, then return immediately, otherwise consider
	 *   it as a non-match.
	 * - if one rule returns OK, then return OK
	 * - if one rule returns KO, then return KO
	 */

	if ((req->flags & CF_SHUTR) || buffer_full(req->buf, global.tune.maxrewrite) ||
	    !s->be->tcp_req.inspect_delay || tick_is_expired(req->analyse_exp, now_ms))
		partial = SMP_OPT_FINAL;
	else
		partial = 0;

	list_for_each_entry(rule, &s->be->tcp_req.inspect_rules, list) {
		int ret = ACL_PAT_PASS;

		if (rule->cond) {
			ret = acl_exec_cond(rule->cond, s->be, s, &s->txn, SMP_OPT_DIR_REQ | partial);
			if (ret == ACL_PAT_MISS) {
				channel_dont_connect(req);
				/* just set the request timeout once at the beginning of the request */
				if (!tick_isset(req->analyse_exp) && s->be->tcp_req.inspect_delay)
					req->analyse_exp = tick_add_ifset(now_ms, s->be->tcp_req.inspect_delay);
				return 0;
			}

			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			/* we have a matching rule. */
			if (rule->action == TCP_ACT_REJECT) {
				channel_abort(req);
				channel_abort(s->rep);
				req->analysers = 0;

				s->be->be_counters.denied_req++;
				s->fe->fe_counters.denied_req++;
				if (s->listener->counters)
					s->listener->counters->denied_req++;

				if (!(s->flags & SN_ERR_MASK))
					s->flags |= SN_ERR_PRXCOND;
				if (!(s->flags & SN_FINST_MASK))
					s->flags |= SN_FINST_R;
				return 0;
			}
			else if ((rule->action == TCP_ACT_TRK_SC1 && !s->stkctr[0].entry) ||
			         (rule->action == TCP_ACT_TRK_SC2 && !s->stkctr[1].entry)) {
				/* Note: only the first valid tracking parameter of each
				 * applies.
				 */
				struct stktable_key *key;

				t = rule->act_prm.trk_ctr.table.t;
				key = stktable_fetch_key(t, s->be, s, &s->txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->act_prm.trk_ctr.expr);

				if (key && (ts = stktable_get_entry(t, key))) {
					if (rule->action == TCP_ACT_TRK_SC1) {
						session_track_stkctr(&s->stkctr[0], t, ts);
						if (s->fe != s->be)
							s->flags |= SN_BE_TRACK_SC1;
					} else {
						session_track_stkctr(&s->stkctr[1], t, ts);
						if (s->fe != s->be)
							s->flags |= SN_BE_TRACK_SC2;
					}
				}
			}
			else {
				/* otherwise accept */
				break;
			}
		}
	}

	/* if we get there, it means we have no rule which matches, or
	 * we have an explicit accept, so we apply the default accept.
	 */
	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;
	return 1;
}

/* This function performs the TCP response analysis on the current response. It
 * returns 1 if the processing can continue on next analysers, or zero if it
 * needs more data, encounters an error, or wants to immediately abort the
 * response. It relies on buffers flags, and updates s->rep->analysers. The
 * function may be called for backend rules.
 */
int tcp_inspect_response(struct session *s, struct channel *rep, int an_bit)
{
	struct tcp_rule *rule;
	int partial;

	DPRINTF(stderr,"[%u] %s: session=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%d analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		rep,
		rep->rex, rep->wex,
		rep->flags,
		rep->buf->i,
		rep->analysers);

	/* We don't know whether we have enough data, so must proceed
	 * this way :
	 * - iterate through all rules in their declaration order
	 * - if one rule returns MISS, it means the inspect delay is
	 *   not over yet, then return immediately, otherwise consider
	 *   it as a non-match.
	 * - if one rule returns OK, then return OK
	 * - if one rule returns KO, then return KO
	 */

	if (rep->flags & CF_SHUTR || tick_is_expired(rep->analyse_exp, now_ms))
		partial = SMP_OPT_FINAL;
	else
		partial = 0;

	list_for_each_entry(rule, &s->be->tcp_rep.inspect_rules, list) {
		int ret = ACL_PAT_PASS;

		if (rule->cond) {
			ret = acl_exec_cond(rule->cond, s->be, s, &s->txn, SMP_OPT_DIR_RES | partial);
			if (ret == ACL_PAT_MISS) {
				/* just set the analyser timeout once at the beginning of the response */
				if (!tick_isset(rep->analyse_exp) && s->be->tcp_rep.inspect_delay)
					rep->analyse_exp = tick_add_ifset(now_ms, s->be->tcp_rep.inspect_delay);
				return 0;
			}

			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			/* we have a matching rule. */
			if (rule->action == TCP_ACT_REJECT) {
				channel_abort(rep);
				channel_abort(s->req);
				rep->analysers = 0;

				s->be->be_counters.denied_resp++;
				s->fe->fe_counters.denied_resp++;
				if (s->listener->counters)
					s->listener->counters->denied_resp++;

				if (!(s->flags & SN_ERR_MASK))
					s->flags |= SN_ERR_PRXCOND;
				if (!(s->flags & SN_FINST_MASK))
					s->flags |= SN_FINST_D;
				return 0;
			}
			else {
				/* otherwise accept */
				break;
			}
		}
	}

	/* if we get there, it means we have no rule which matches, or
	 * we have an explicit accept, so we apply the default accept.
	 */
	rep->analysers &= ~an_bit;
	rep->analyse_exp = TICK_ETERNITY;
	return 1;
}


/* This function performs the TCP layer4 analysis on the current request. It
 * returns 0 if a reject rule matches, otherwise 1 if either an accept rule
 * matches or if no more rule matches. It can only use rules which don't need
 * any data.
 */
int tcp_exec_req_rules(struct session *s)
{
	struct tcp_rule *rule;
	struct stksess *ts;
	struct stktable *t = NULL;
	int result = 1;
	int ret;

	list_for_each_entry(rule, &s->fe->tcp_req.l4_rules, list) {
		ret = ACL_PAT_PASS;

		if (rule->cond) {
			ret = acl_exec_cond(rule->cond, s->fe, s, NULL, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			/* we have a matching rule. */
			if (rule->action == TCP_ACT_REJECT) {
				s->fe->fe_counters.denied_conn++;
				if (s->listener->counters)
					s->listener->counters->denied_conn++;

				if (!(s->flags & SN_ERR_MASK))
					s->flags |= SN_ERR_PRXCOND;
				if (!(s->flags & SN_FINST_MASK))
					s->flags |= SN_FINST_R;
				result = 0;
				break;
			}
			else if ((rule->action == TCP_ACT_TRK_SC1 && !s->stkctr[0].entry) ||
			         (rule->action == TCP_ACT_TRK_SC2 && !s->stkctr[1].entry)) {
				/* Note: only the first valid tracking parameter of each
				 * applies.
				 */
				struct stktable_key *key;

				t = rule->act_prm.trk_ctr.table.t;
				key = stktable_fetch_key(t, s->be, s, &s->txn, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->act_prm.trk_ctr.expr);

				if (key && (ts = stktable_get_entry(t, key))) {
					if (rule->action == TCP_ACT_TRK_SC1)
						session_track_stkctr(&s->stkctr[0], t, ts);
					else
						session_track_stkctr(&s->stkctr[1], t, ts);
				}
			}
			else {
				/* otherwise it's an accept */
				break;
			}
		}
	}
	return result;
}

/* Parse a tcp-response rule. Return a negative value in case of failure */
static int tcp_parse_response_rule(char **args, int arg, int section_type,
				  struct proxy *curpx, struct proxy *defpx,
				  struct tcp_rule *rule, char **err,
                                  unsigned int where)
{
	if (curpx == defpx || !(curpx->cap & PR_CAP_BE)) {
		memprintf(err, "%s %s is only allowed in 'backend' sections",
		          args[0], args[1]);
		return -1;
	}

	if (strcmp(args[arg], "accept") == 0) {
		arg++;
		rule->action = TCP_ACT_ACCEPT;
	}
	else if (strcmp(args[arg], "reject") == 0) {
		arg++;
		rule->action = TCP_ACT_REJECT;
	}
	else {
		memprintf(err,
		          "'%s %s' expects 'accept' or 'reject' in %s '%s' (got '%s')",
		          args[0], args[1], proxy_type_str(curpx), curpx->id, args[arg]);
		return -1;
	}

	if (strcmp(args[arg], "if") == 0 || strcmp(args[arg], "unless") == 0) {
		if ((rule->cond = build_acl_cond(NULL, 0, curpx, (const char **)args+arg, err)) == NULL) {
			memprintf(err,
			          "'%s %s %s' : error detected in %s '%s' while parsing '%s' condition : %s",
			          args[0], args[1], args[2], proxy_type_str(curpx), curpx->id, args[arg], *err);
			return -1;
		}
	}
	else if (*args[arg]) {
		memprintf(err,
			 "'%s %s %s' only accepts 'if' or 'unless', in %s '%s' (got '%s')",
			 args[0], args[1], args[2], proxy_type_str(curpx), curpx->id, args[arg]);
		return -1;
	}
	return 0;
}



/* Parse a tcp-request rule. Return a negative value in case of failure */
static int tcp_parse_request_rule(char **args, int arg, int section_type,
                                  struct proxy *curpx, struct proxy *defpx,
                                  struct tcp_rule *rule, char **err,
                                  unsigned int where)
{
	if (curpx == defpx) {
		memprintf(err, "%s %s is not allowed in 'defaults' sections",
		          args[0], args[1]);
		return -1;
	}

	if (!strcmp(args[arg], "accept")) {
		arg++;
		rule->action = TCP_ACT_ACCEPT;
	}
	else if (!strcmp(args[arg], "reject")) {
		arg++;
		rule->action = TCP_ACT_REJECT;
	}
	else if (strcmp(args[arg], "track-sc1") == 0 || strcmp(args[arg], "track-sc2") == 0) {
		struct sample_expr *expr;
		int kw = arg;

		arg++;

		curpx->conf.args.ctx = ARGC_TRK;
		expr = sample_parse_expr(args, &arg, trash.str, trash.size, &curpx->conf.args);
		if (!expr) {
			memprintf(err,
			          "'%s %s %s' : %s",
			          args[0], args[1], args[kw], trash.str);
			return -1;
		}

		if (!(expr->fetch->val & where)) {
			memprintf(err,
			          "'%s %s %s' : fetch method '%s' extracts information from '%s', none of which is available here",
			          args[0], args[1], args[kw], args[arg], sample_src_names(expr->fetch->use));
			free(expr);
			return -1;
		}

		/* check if we need to allocate an hdr_idx struct for HTTP parsing */
		curpx->http_needed |= !!(expr->fetch->use & SMP_USE_HTTP_ANY);

		if (strcmp(args[arg], "table") == 0) {
			arg++;
			if (!args[arg]) {
				memprintf(err,
					  "'%s %s %s' : missing table name",
					  args[0], args[1], args[kw]);
				free(expr);
				return -1;
			}
			/* we copy the table name for now, it will be resolved later */
			rule->act_prm.trk_ctr.table.n = strdup(args[arg]);
			arg++;
		}
		rule->act_prm.trk_ctr.expr = expr;

		if (args[kw][8] == '1')
			rule->action = TCP_ACT_TRK_SC1;
		else
			rule->action = TCP_ACT_TRK_SC2;
	}
	else {
		memprintf(err,
		          "'%s %s' expects 'accept', 'reject', 'track-sc1' "
		          "or 'track-sc2' in %s '%s' (got '%s')",
		          args[0], args[1], proxy_type_str(curpx), curpx->id, args[arg]);
		return -1;
	}

	if (strcmp(args[arg], "if") == 0 || strcmp(args[arg], "unless") == 0) {
		if ((rule->cond = build_acl_cond(NULL, 0, curpx, (const char **)args+arg, err)) == NULL) {
			memprintf(err,
			          "'%s %s %s' : error detected in %s '%s' while parsing '%s' condition : %s",
			          args[0], args[1], args[2], proxy_type_str(curpx), curpx->id, args[arg], *err);
			return -1;
		}
	}
	else if (*args[arg]) {
		memprintf(err,
			 "'%s %s %s' only accepts 'if' or 'unless', in %s '%s' (got '%s')",
			 args[0], args[1], args[2], proxy_type_str(curpx), curpx->id, args[arg]);
		return -1;
	}
	return 0;
}

/* This function should be called to parse a line starting with the "tcp-response"
 * keyword.
 */
static int tcp_parse_tcp_rep(char **args, int section_type, struct proxy *curpx,
                             struct proxy *defpx, const char *file, int line,
                             char **err)
{
	const char *ptr = NULL;
	unsigned int val;
	int warn = 0;
	int arg;
	struct tcp_rule *rule;
	unsigned int where;
	const struct acl *acl;
	const char *kw;

	if (!*args[1]) {
		memprintf(err, "missing argument for '%s' in %s '%s'",
		          args[0], proxy_type_str(curpx), curpx->id);
		return -1;
	}

	if (strcmp(args[1], "inspect-delay") == 0) {
		if (curpx == defpx || !(curpx->cap & PR_CAP_BE)) {
			memprintf(err, "%s %s is only allowed in 'backend' sections",
			          args[0], args[1]);
			return -1;
		}

		if (!*args[2] || (ptr = parse_time_err(args[2], &val, TIME_UNIT_MS))) {
			memprintf(err,
			          "'%s %s' expects a positive delay in milliseconds, in %s '%s'",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
			if (ptr)
				memprintf(err, "%s (unexpected character '%c')", *err, *ptr);
			return -1;
		}

		if (curpx->tcp_rep.inspect_delay) {
			memprintf(err, "ignoring %s %s (was already defined) in %s '%s'",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
			return 1;
		}
		curpx->tcp_rep.inspect_delay = val;
		return 0;
	}

	rule = calloc(1, sizeof(*rule));
	LIST_INIT(&rule->list);
	arg = 1;
	where = 0;

	if (strcmp(args[1], "content") == 0) {
		arg++;

		if (curpx->cap & PR_CAP_FE)
			where |= SMP_VAL_FE_RES_CNT;
		if (curpx->cap & PR_CAP_BE)
			where |= SMP_VAL_BE_RES_CNT;

		if (tcp_parse_response_rule(args, arg, section_type, curpx, defpx, rule, err, where) < 0)
			goto error;

		acl = rule->cond ? acl_cond_conflicts(rule->cond, where) : NULL;
		if (acl) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' will never match in '%s %s' because it only involves keywords that are incompatible with '%s'",
					  acl->name, args[0], args[1], sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl will never match in '%s %s' because it uses keyword '%s' which is incompatible with '%s'",
					  args[0], args[1],
					  LIST_ELEM(acl->expr.n, struct acl_expr *, list)->kw,
					  sample_ckp_names(where));

			warn++;
		}
		else if (rule->cond && acl_cond_kw_conflicts(rule->cond, where, &acl, &kw)) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' involves keyword '%s' which is incompatible with '%s'",
					  acl->name, kw, sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl involves keyword '%s' which is incompatible with '%s'",
					  kw, sample_ckp_names(where));
			warn++;
		}

		LIST_ADDQ(&curpx->tcp_rep.inspect_rules, &rule->list);
	}
	else {
		memprintf(err,
		          "'%s' expects 'inspect-delay' or 'content' in %s '%s' (got '%s')",
		          args[0], proxy_type_str(curpx), curpx->id, args[1]);
		goto error;
	}

	return warn;
 error:
	free(rule);
	return -1;
}


/* This function should be called to parse a line starting with the "tcp-request"
 * keyword.
 */
static int tcp_parse_tcp_req(char **args, int section_type, struct proxy *curpx,
                             struct proxy *defpx, const char *file, int line,
                             char **err)
{
	const char *ptr = NULL;
	unsigned int val;
	int warn = 0;
	int arg;
	struct tcp_rule *rule;
	unsigned int where;
	const struct acl *acl;
	const char *kw;

	if (!*args[1]) {
		if (curpx == defpx)
			memprintf(err, "missing argument for '%s' in defaults section", args[0]);
		else
			memprintf(err, "missing argument for '%s' in %s '%s'",
			          args[0], proxy_type_str(curpx), curpx->id);
		return -1;
	}

	if (!strcmp(args[1], "inspect-delay")) {
		if (curpx == defpx) {
			memprintf(err, "%s %s is not allowed in 'defaults' sections",
			          args[0], args[1]);
			return -1;
		}

		if (!*args[2] || (ptr = parse_time_err(args[2], &val, TIME_UNIT_MS))) {
			memprintf(err,
			          "'%s %s' expects a positive delay in milliseconds, in %s '%s'",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
			if (ptr)
				memprintf(err, "%s (unexpected character '%c')", *err, *ptr);
			return -1;
		}

		if (curpx->tcp_req.inspect_delay) {
			memprintf(err, "ignoring %s %s (was already defined) in %s '%s'",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
			return 1;
		}
		curpx->tcp_req.inspect_delay = val;
		return 0;
	}

	rule = calloc(1, sizeof(*rule));
	LIST_INIT(&rule->list);
	arg = 1;
	where = 0;

	if (strcmp(args[1], "content") == 0) {
		arg++;

		if (curpx->cap & PR_CAP_FE)
			where |= SMP_VAL_FE_REQ_CNT;
		if (curpx->cap & PR_CAP_BE)
			where |= SMP_VAL_BE_REQ_CNT;

		if (tcp_parse_request_rule(args, arg, section_type, curpx, defpx, rule, err, where) < 0)
			goto error;

		acl = rule->cond ? acl_cond_conflicts(rule->cond, where) : NULL;
		if (acl) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' will never match in '%s %s' because it only involves keywords that are incompatible with '%s'",
					  acl->name, args[0], args[1], sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl will never match in '%s %s' because it uses keyword '%s' which is incompatible with '%s'",
					  args[0], args[1],
					  LIST_ELEM(acl->expr.n, struct acl_expr *, list)->kw,
					  sample_ckp_names(where));

			warn++;
		}
		else if (rule->cond && acl_cond_kw_conflicts(rule->cond, where, &acl, &kw)) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' involves keyword '%s' which is incompatible with '%s'",
					  acl->name, kw, sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl involves keyword '%s' which is incompatible with '%s'",
					  kw, sample_ckp_names(where));
			warn++;
		}

		LIST_ADDQ(&curpx->tcp_req.inspect_rules, &rule->list);
	}
	else if (strcmp(args[1], "connection") == 0) {
		arg++;

		if (!(curpx->cap & PR_CAP_FE)) {
			memprintf(err, "%s %s is not allowed because %s %s is not a frontend",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
			goto error;
		}

		where |= SMP_VAL_FE_CON_ACC;

		if (tcp_parse_request_rule(args, arg, section_type, curpx, defpx, rule, err, where) < 0)
			goto error;

		acl = rule->cond ? acl_cond_conflicts(rule->cond, where) : NULL;
		if (acl) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' will never match in '%s %s' because it only involves keywords that are incompatible with '%s'",
					  acl->name, args[0], args[1], sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl will never match in '%s %s' because it uses keyword '%s' which is incompatible with '%s'",
					  args[0], args[1],
					  LIST_ELEM(acl->expr.n, struct acl_expr *, list)->kw,
					  sample_ckp_names(where));

			warn++;
		}
		else if (rule->cond && acl_cond_kw_conflicts(rule->cond, where, &acl, &kw)) {
			if (acl->name && *acl->name)
				memprintf(err,
					  "acl '%s' involves keyword '%s' which is incompatible with '%s'",
					  acl->name, kw, sample_ckp_names(where));
			else
				memprintf(err,
					  "anonymous acl involves keyword '%s' which is incompatible with '%s'",
					  kw, sample_ckp_names(where));
			warn++;
		}

		LIST_ADDQ(&curpx->tcp_req.l4_rules, &rule->list);
	}
	else {
		if (curpx == defpx)
			memprintf(err,
			          "'%s' expects 'inspect-delay', 'connection', or 'content' in defaults section (got '%s')",
			          args[0], args[1]);
		else
			memprintf(err,
			          "'%s' expects 'inspect-delay', 'connection', or 'content' in %s '%s' (got '%s')",
			          args[0], args[1], proxy_type_str(curpx), curpx->id);
		goto error;
	}

	return warn;
 error:
	free(rule);
	return -1;
}


/************************************************************************/
/*       All supported sample fetch functions must be declared here     */
/************************************************************************/

/* fetch the connection's source IPv4/IPv6 address */
static int
smp_fetch_src(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
              const struct arg *args, struct sample *smp)
{
	switch (l4->si[0].conn->addr.from.ss_family) {
	case AF_INET:
		smp->data.ipv4 = ((struct sockaddr_in *)&l4->si[0].conn->addr.from)->sin_addr;
		smp->type = SMP_T_IPV4;
		break;
	case AF_INET6:
		smp->data.ipv6 = ((struct sockaddr_in6 *)(&l4->si[0].conn->addr.from))->sin6_addr;
		smp->type = SMP_T_IPV6;
		break;
	default:
		return 0;
	}

	smp->flags = 0;
	return 1;
}

/* set temp integer to the connection's source port */
static int
smp_fetch_sport(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                const struct arg *args, struct sample *smp)
{
	smp->type = SMP_T_UINT;
	if (!(smp->data.uint = get_host_port(&l4->si[0].conn->addr.from)))
		return 0;

	smp->flags = 0;
	return 1;
}

/* fetch the connection's destination IPv4/IPv6 address */
static int
smp_fetch_dst(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
              const struct arg *args, struct sample *smp)
{
	conn_get_to_addr(l4->si[0].conn);

	switch (l4->si[0].conn->addr.to.ss_family) {
	case AF_INET:
		smp->data.ipv4 = ((struct sockaddr_in *)&l4->si[0].conn->addr.to)->sin_addr;
		smp->type = SMP_T_IPV4;
		break;
	case AF_INET6:
		smp->data.ipv6 = ((struct sockaddr_in6 *)(&l4->si[0].conn->addr.to))->sin6_addr;
		smp->type = SMP_T_IPV6;
		break;
	default:
		return 0;
	}

	smp->flags = 0;
	return 1;
}

/* set temp integer to the frontend connexion's destination port */
static int
smp_fetch_dport(struct proxy *px, struct session *l4, void *l7, unsigned int opt,
                const struct arg *args, struct sample *smp)
{
	conn_get_to_addr(l4->si[0].conn);

	smp->type = SMP_T_UINT;
	if (!(smp->data.uint = get_host_port(&l4->si[0].conn->addr.to)))
		return 0;

	smp->flags = 0;
	return 1;
}

#ifdef IPV6_V6ONLY
/* parse the "v4v6" bind keyword */
static int bind_parse_v4v6(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET6)
			l->options |= LI_O_V4V6;
	}

	return 0;
}

/* parse the "v6only" bind keyword */
static int bind_parse_v6only(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET6)
			l->options |= LI_O_V6ONLY;
	}

	return 0;
}
#endif

#ifdef CONFIG_HAP_LINUX_TPROXY
/* parse the "transparent" bind keyword */
static int bind_parse_transparent(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET || l->addr.ss_family == AF_INET6)
			l->options |= LI_O_FOREIGN;
	}

	return 0;
}
#endif

#ifdef TCP_DEFER_ACCEPT
/* parse the "defer-accept" bind keyword */
static int bind_parse_defer_accept(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET || l->addr.ss_family == AF_INET6)
			l->options |= LI_O_DEF_ACCEPT;
	}

	return 0;
}
#endif

#ifdef TCP_FASTOPEN
/* parse the "tfo" bind keyword */
static int bind_parse_tfo(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET || l->addr.ss_family == AF_INET6)
			l->options |= LI_O_TCP_FO;
	}

	return 0;
}
#endif

#ifdef TCP_MAXSEG
/* parse the "mss" bind keyword */
static int bind_parse_mss(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;
	int mss;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing MSS value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	mss = atoi(args[cur_arg + 1]);
	if (!mss || abs(mss) > 65535) {
		memprintf(err, "'%s' : expects an MSS with and absolute value between 1 and 65535", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET || l->addr.ss_family == AF_INET6)
			l->maxseg = mss;
	}

	return 0;
}
#endif

#ifdef SO_BINDTODEVICE
/* parse the "mss" bind keyword */
static int bind_parse_interface(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing interface name", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind) {
		if (l->addr.ss_family == AF_INET || l->addr.ss_family == AF_INET6)
			l->interface = strdup(args[cur_arg + 1]);
	}

	global.last_checks |= LSTCHK_NETADM;
	return 0;
}
#endif

static struct cfg_kw_list cfg_kws = {{ },{
	{ CFG_LISTEN, "tcp-request",  tcp_parse_tcp_req },
	{ CFG_LISTEN, "tcp-response", tcp_parse_tcp_rep },
	{ 0, NULL, NULL },
}};


/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {{ },{
	{ "dst",      NULL, acl_parse_ip,  acl_match_ip  },
	{ "dst_port", NULL, acl_parse_int, acl_match_int },
	{ "src",      NULL, acl_parse_ip,  acl_match_ip  },
	{ "src_port", NULL, acl_parse_int, acl_match_int },
	{ /* END */ },
}};


/* Note: must not be declared <const> as its list will be overwritten.
 * Note: fetches that may return multiple types must be declared as the lowest
 * common denominator, the type that can be casted into all other ones. For
 * instance v4/v6 must be declared v4.
 */
static struct sample_fetch_kw_list sample_fetch_keywords = {{ },{
	{ "dst",      smp_fetch_dst,   0, NULL, SMP_T_IPV4, SMP_USE_L4CLI },
	{ "dst_port", smp_fetch_dport, 0, NULL, SMP_T_UINT, SMP_USE_L4CLI },
	{ "src",      smp_fetch_src,   0, NULL, SMP_T_IPV4, SMP_USE_L4CLI },
	{ "src_port", smp_fetch_sport, 0, NULL, SMP_T_UINT, SMP_USE_L4CLI },
	{ /* END */ },
}};

/************************************************************************/
/*           All supported bind keywords must be declared here.         */
/************************************************************************/

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted, doing so helps
 * all code contributors.
 * Optional keywords are also declared with a NULL ->parse() function so that
 * the config parser can report an appropriate error when a known keyword was
 * not enabled.
 */
static struct bind_kw_list bind_kws = { "TCP", { }, {
#ifdef TCP_DEFER_ACCEPT
	{ "defer-accept",  bind_parse_defer_accept, 0 }, /* wait for some data for 1 second max before doing accept */
#endif
#ifdef SO_BINDTODEVICE
	{ "interface",     bind_parse_interface,    1 }, /* specifically bind to this interface */
#endif
#ifdef TCP_MAXSEG
	{ "mss",           bind_parse_mss,          1 }, /* set MSS of listening socket */
#endif
#ifdef TCP_FASTOPEN
	{ "tfo",           bind_parse_tfo,          0 }, /* enable TCP_FASTOPEN of listening socket */
#endif
#ifdef CONFIG_HAP_LINUX_TPROXY
	{ "transparent",   bind_parse_transparent,  0 }, /* transparently bind to the specified addresses */
#endif
#ifdef IPV6_V6ONLY
	{ "v4v6",          bind_parse_v4v6,         0 }, /* force socket to bind to IPv4+IPv6 */
	{ "v6only",        bind_parse_v6only,       0 }, /* force socket to bind to IPv6 only */
#endif
	/* the versions with the NULL parse function*/
	{ "defer-accept",  NULL,  0 },
	{ "interface",     NULL,  1 },
	{ "mss",           NULL,  1 },
	{ "transparent",   NULL,  0 },
	{ "v4v6",          NULL,  0 },
	{ "v6only",        NULL,  0 },
	{ NULL, NULL, 0 },
}};

__attribute__((constructor))
static void __tcp_protocol_init(void)
{
	protocol_register(&proto_tcpv4);
	protocol_register(&proto_tcpv6);
	sample_register_fetches(&sample_fetch_keywords);
	cfg_register_keywords(&cfg_kws);
	acl_register_keywords(&acl_kws);
	bind_register_keywords(&bind_kws);
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
