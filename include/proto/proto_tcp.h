/*
 * include/proto/proto_tcp.h
 * This file contains TCP socket protocol definitions.
 *
 * Copyright (C) 2000-2010 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _PROTO_PROTO_TCP_H
#define _PROTO_PROTO_TCP_H

#include <common/config.h>
#include <types/proto_tcp.h>
#include <types/task.h>
#include <proto/stick_table.h>

int tcp_bind_socket(int fd, int flags, struct sockaddr_storage *local, struct sockaddr_storage *remote);
void tcpv4_add_listener(struct listener *listener);
void tcpv6_add_listener(struct listener *listener);
int tcp_connect_server(struct connection *conn, int data, int delack);
int tcp_connect_probe(struct connection *conn);
int tcp_get_src(int fd, struct sockaddr *sa, socklen_t salen, int dir);
int tcp_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir);
int tcp_inspect_request(struct session *s, struct channel *req, int an_bit);
int tcp_inspect_response(struct session *s, struct channel *rep, int an_bit);
int tcp_exec_req_rules(struct session *s);
int smp_fetch_rdp_cookie(struct proxy *px, struct session *l4, void *l7, unsigned int opt, const struct arg *args, struct sample *smp);

/* Converts the INET/INET6 source address to a stick_table key usable for table
 * lookups. Returns either NULL if the source cannot be converted (eg: not
 * IPv4) or a pointer to the converted result in static_table_key in the
 * appropriate format (IP).
 */
static inline struct stktable_key *addr_to_stktable_key(struct sockaddr_storage *addr)
{
	switch (addr->ss_family) {
	case AF_INET:
		static_table_key->key = (void *)&((struct sockaddr_in *)addr)->sin_addr;
		break;
	case AF_INET6:
		static_table_key->key = (void *)&((struct sockaddr_in6 *)addr)->sin6_addr;
		break;
	default:
		return NULL;
	}
	return static_table_key;
}


#endif /* _PROTO_PROTO_TCP_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
