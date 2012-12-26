/*
 * include/proto/stream_interface.h
 * This file contains stream_interface function prototypes
 *
 * Copyright (C) 2000-2012 Willy Tarreau - w@1wt.eu
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

#ifndef _PROTO_STREAM_INTERFACE_H
#define _PROTO_STREAM_INTERFACE_H

#include <stdlib.h>

#include <common/config.h>
#include <types/session.h>
#include <types/stream_interface.h>
#include <proto/channel.h>
#include <proto/connection.h>


/* main event functions used to move data between sockets and buffers */
int stream_int_check_timeouts(struct stream_interface *si);
void stream_int_report_error(struct stream_interface *si);
void stream_int_retnclose(struct stream_interface *si, const struct chunk *msg);
int conn_si_send_proxy(struct connection *conn, unsigned int flag);
int stream_int_shutr(struct stream_interface *si);
int stream_int_shutw(struct stream_interface *si);
void stream_sock_read0(struct stream_interface *si);

extern struct si_ops si_embedded_ops;
extern struct si_ops si_conn_ops;
extern struct data_cb si_conn_cb;

struct task *stream_int_register_handler(struct stream_interface *si,
					 struct si_applet *app);
void stream_int_unregister_handler(struct stream_interface *si);

static inline const struct protocol *si_ctrl(struct stream_interface *si)
{
	return si->conn->ctrl;
}

static inline void si_prepare_conn(struct stream_interface *si, const struct protocol *ctrl, const struct xprt_ops *xprt)
{
	si->ops = &si_conn_ops;
	conn_prepare(si->conn, &si_conn_cb, ctrl, xprt, si);
}

static inline void si_takeover_conn(struct stream_interface *si, const struct protocol *ctrl, const struct xprt_ops *xprt)
{
	si->ops = &si_conn_ops;
	conn_assign(si->conn, &si_conn_cb, ctrl, xprt, si);
}

static inline void si_prepare_embedded(struct stream_interface *si)
{
	si->ops = &si_embedded_ops;
	conn_prepare(si->conn, NULL, NULL, NULL, si);
}

/* Sends a shutr to the connection using the data layer */
static inline void si_shutr(struct stream_interface *si)
{
	if (stream_int_shutr(si))
		conn_data_stop_recv(si->conn);
}

/* Sends a shutw to the connection using the data layer */
static inline void si_shutw(struct stream_interface *si)
{
	if (stream_int_shutw(si))
		conn_data_stop_send(si->conn);
}

/* Calls the data state update on the stream interfaace */
static inline void si_update(struct stream_interface *si)
{
	si->ops->update(si);
}

/* Calls chk_rcv on the connection using the data layer */
static inline void si_chk_rcv(struct stream_interface *si)
{
	si->ops->chk_rcv(si);
}

/* Calls chk_snd on the connection using the data layer */
static inline void si_chk_snd(struct stream_interface *si)
{
	si->ops->chk_snd(si);
}

/* Calls chk_snd on the connection using the ctrl layer */
static inline int si_connect(struct stream_interface *si)
{
	int ret;

	if (unlikely(!si_ctrl(si) || !si_ctrl(si)->connect))
		return SN_ERR_INTERNAL;

	ret = si_ctrl(si)->connect(si->conn, !channel_is_empty(si->ob), !!si->send_proxy_ofs);
	if (ret != SN_ERR_NONE)
		return ret;

	/* needs src ip/port for logging */
	if (si->flags & SI_FL_SRC_ADDR)
		conn_get_from_addr(si->conn);

	/* Prepare to send a few handshakes related to the on-wire protocol. */
	if (si->send_proxy_ofs)
		si->conn->flags |= CO_FL_SI_SEND_PROXY;

	/* we need to be notified about connection establishment */
	si->conn->flags |= CO_FL_WAKE_DATA;

	/* we're in the process of establishing a connection */
	si->state = SI_ST_CON;

	return ret;
}

/* for debugging, reports the stream interface state name */
static inline const char *si_state_str(int state)
{
	switch (state) {
	case SI_ST_INI: return "INI";
	case SI_ST_REQ: return "REQ";
	case SI_ST_QUE: return "QUE";
	case SI_ST_TAR: return "TAR";
	case SI_ST_ASS: return "ASS";
	case SI_ST_CON: return "CON";
	case SI_ST_CER: return "CER";
	case SI_ST_EST: return "EST";
	case SI_ST_DIS: return "DIS";
	case SI_ST_CLO: return "CLO";
	default:        return "???";
	}
}

#endif /* _PROTO_STREAM_INTERFACE_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
