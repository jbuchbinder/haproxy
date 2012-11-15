/*
 * include/types/server.h
 * This file defines everything related to servers.
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

#ifndef _TYPES_SERVER_H
#define _TYPES_SERVER_H

#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#endif

#include <common/config.h>
#include <common/mini-clist.h>
#include <eb32tree.h>

#include <types/connection.h>
#include <types/counters.h>
#include <types/freq_ctr.h>
#include <types/obj_type.h>
#include <types/port_range.h>
#include <types/proxy.h>
#include <types/queue.h>
#include <types/task.h>
#include <types/checks.h>


/* server flags */
#define SRV_RUNNING	0x0001	/* the server is UP */
#define SRV_BACKUP	0x0002	/* this server is a backup server */
#define SRV_MAPPORTS	0x0004	/* this server uses mapped ports */
#define SRV_BIND_SRC	0x0008	/* this server uses a specific source address */
#define SRV_CHECKED	0x0010	/* this server needs to be checked */
#define SRV_GOINGDOWN	0x0020	/* this server says that it's going down (404) */
#define SRV_WARMINGUP	0x0040	/* this server is warming up after a failure */
#define SRV_MAINTAIN	0x0080	/* this server is in maintenance mode */
#define SRV_TPROXY_ADDR	0x0100	/* bind to this non-local address to reach this server */
#define SRV_TPROXY_CIP	0x0200	/* bind to the client's IP address to reach this server */
#define SRV_TPROXY_CLI	0x0300	/* bind to the client's IP+port to reach this server */
#define SRV_TPROXY_DYN	0x0400	/* bind to a dynamically computed non-local address */
#define SRV_TPROXY_MASK	0x0700	/* bind to a non-local address to reach this server */
#define SRV_SEND_PROXY	0x0800	/* this server talks the PROXY protocol */
#define SRV_NON_STICK	0x1000	/* never add connections allocated to this server to a stick table */

/* function which act on servers need to return various errors */
#define SRV_STATUS_OK       0   /* everything is OK. */
#define SRV_STATUS_INTERNAL 1   /* other unrecoverable errors. */
#define SRV_STATUS_NOSRV    2   /* no server is available */
#define SRV_STATUS_FULL     3   /* the/all server(s) are saturated */
#define SRV_STATUS_QUEUED   4   /* the/all server(s) are saturated but the connection was queued */

/* bits for s->result used for health-checks */
#define SRV_CHK_UNKNOWN 0x0000   /* initialized to this by default */
#define SRV_CHK_ERROR   0x0001   /* error encountered during the check; has precedence */
#define SRV_CHK_RUNNING 0x0002   /* server seen as running */
#define SRV_CHK_DISABLE 0x0004   /* server returned a "disable" code */

/* various constants */
#define SRV_UWGHT_RANGE 256
#define SRV_UWGHT_MAX   (SRV_UWGHT_RANGE - 1)
#define SRV_EWGHT_RANGE (SRV_UWGHT_RANGE * BE_WEIGHT_SCALE)
#define SRV_EWGHT_MAX   (SRV_UWGHT_MAX   * BE_WEIGHT_SCALE)

#ifdef USE_OPENSSL
/* server ssl options */
#define SRV_SSL_O_NONE         0x0000
#define SRV_SSL_O_NO_VMASK     0x000F /* force version mask */
#define SRV_SSL_O_NO_SSLV3     0x0001 /* disable SSLv3 */
#define SRV_SSL_O_NO_TLSV10    0x0002 /* disable TLSv1.0 */
#define SRV_SSL_O_NO_TLSV11    0x0004 /* disable TLSv1.1 */
#define SRV_SSL_O_NO_TLSV12    0x0008 /* disable TLSv1.2 */
/* 0x000F reserved for 'no' protocol version options */
#define SRV_SSL_O_USE_VMASK    0x00F0 /* force version mask */
#define SRV_SSL_O_USE_SSLV3    0x0010 /* force SSLv3 */
#define SRV_SSL_O_USE_TLSV10   0x0020 /* force TLSv1.0 */
#define SRV_SSL_O_USE_TLSV11   0x0040 /* force TLSv1.1 */
#define SRV_SSL_O_USE_TLSV12   0x0080 /* force TLSv1.2 */
/* 0x00F0 reserved for 'force' protocol version options */
#define SRV_SSL_O_NO_TLS_TICKETS 0x0100 /* disable session resumption tickets */
#endif

/* A tree occurrence is a descriptor of a place in a tree, with a pointer back
 * to the server itself.
 */
struct server;
struct tree_occ {
	struct server *server;
	struct eb32_node node;
};

struct server {
	enum obj_type obj_type;                 /* object type == OBJ_TYPE_SERVER */
	struct server *next;
	int state;				/* server state (SRV_*) */
	int prev_state;				/* server state before last change (SRV_*) */
	int cklen;				/* the len of the cookie, to speed up checks */
	int rdr_len;				/* the length of the redirection prefix */
	char *cookie;				/* the id set in the cookie */
	char *rdr_pfx;				/* the redirection prefix */

	struct proxy *proxy;			/* the proxy this server belongs to */
	int served;				/* # of active sessions currently being served (ie not pending) */
	int cur_sess;				/* number of currently active sessions (including syn_sent) */
	unsigned maxconn, minconn;		/* max # of active sessions (0 = unlimited), min# for dynamic limit. */
	int nbpend;				/* number of pending connections */
	int maxqueue;				/* maximum number of pending connections allowed */
	struct freq_ctr sess_per_sec;		/* sessions per second on this server */
	struct srvcounters counters;		/* statistics counters */

	struct list pendconns;			/* pending connections */
	struct list actconns;			/* active connections */
	struct task *warmup;                    /* the task dedicated to the warmup when slowstart is set */

	int iface_len;				/* bind interface name length */
	char *iface_name;			/* bind interface name or NULL */
	struct port_range *sport_range;		/* optional per-server TCP source ports */

	struct server *tracknext, *track;	/* next server in a tracking list, tracked server */
	char *trackit;				/* temporary variable to make assignment deferrable */
	int health;				/* 0->rise-1 = bad; rise->rise+fall-1 = good */
	int consecutive_errors;			/* current number of consecutive errors */
	int rise, fall;				/* time in iterations */
	int consecutive_errors_limit;		/* number of consecutive errors that triggers an event */
	short observe, onerror;			/* observing mode: one of HANA_OBS_*; what to do on error: on of ANA_ONERR_* */
	short onmarkeddown;			/* what to do when marked down: one of HANA_ONMARKEDDOWN_* */
	short onmarkedup;			/* what to do when marked up: one of HANA_ONMARKEDUP_* */
	int inter, fastinter, downinter;	/* checks: time in milliseconds */
	int slowstart;				/* slowstart time in seconds (ms in the conf) */
	int result;				/* health-check result : SRV_CHK_* */

	char *id;				/* just for identification */
	unsigned iweight,uweight, eweight;	/* initial weight, user-specified weight, and effective weight */
	unsigned wscore;			/* weight score, used during srv map computation */
	unsigned prev_eweight;			/* eweight before last change */
	unsigned rweight;			/* remainer of weight in the current LB tree */
	unsigned npos, lpos;			/* next and last positions in the LB tree */
	struct eb32_node lb_node;               /* node used for tree-based load balancing */
	struct eb_root *lb_tree;                /* we want to know in what tree the server is */
	struct server *next_full;               /* next server in the temporary full list */
	unsigned lb_nodes_tot;                  /* number of allocated lb_nodes (C-HASH) */
	unsigned lb_nodes_now;                  /* number of lb_nodes placed in the tree (C-HASH) */
	struct tree_occ *lb_nodes;              /* lb_nodes_tot * struct tree_occ */

	/* warning, these structs are huge, keep them at the bottom */
	struct sockaddr_storage addr;		/* the address to connect to */
	struct sockaddr_storage source_addr;	/* the address to which we want to bind for connect() */
#if defined(CONFIG_HAP_CTTPROXY) || defined(CONFIG_HAP_LINUX_TPROXY)
	struct sockaddr_storage tproxy_addr;	/* non-local address we want to bind to for connect() */
	char *bind_hdr_name;			/* bind to this header name if defined */
	int bind_hdr_len;			/* length of the name of the header above */
	int bind_hdr_occ;			/* occurrence number of header above: >0 = from first, <0 = from end, 0=disabled */
#endif
	struct protocol *proto;	                /* server address protocol */
	struct xprt_ops *xprt;                  /* transport-layer operations */
	unsigned down_time;			/* total time the server was down */
	time_t last_change;			/* last time, when the state was changed */

	int puid;				/* proxy-unique server ID, used for SNMP, and "first" LB algo */

	struct {                                /* health-check specific configuration */
		struct connection *conn;        /* connection state for health checks */
		struct protocol *proto;	        /* server address protocol for health checks */
		struct xprt_ops *xprt;          /* transport layer operations for health checks */
		struct sockaddr_storage addr;   /* the address to check, if different from <addr> */
		short port;                     /* the port to use for the health checks */
		struct buffer *bi, *bo;         /* input and output buffers to send/recv check */
		struct task *task;              /* the task associated to the health check processing, NULL if disabled */
		struct timeval start;           /* last health check start time */
		long duration;                  /* time in ms took to finish last health check */
		short status, code;             /* check result, check code */
		char desc[HCHK_DESC_LEN];       /* health check descritpion */
		int use_ssl;                    /* use SSL for health checks */
		int send_proxy;                 /* send a PROXY protocol header with checks */
	} check;

#ifdef USE_OPENSSL
	int use_ssl;				/* ssl enabled */
	struct {
		SSL_CTX *ctx;
		SSL_SESSION *reused_sess;
		char *ciphers;			/* cipher suite to use if non-null */
		int options;			/* ssl options */
		int verify;			/* verify method (set of SSL_VERIFY_* flags) */
		char *ca_file;			/* CAfile to use on verify */
		char *crl_file;			/* CRLfile to use on verify */
		char *client_crt;		/* client certificate to send */
	} ssl_ctx;
#endif
	struct {
		const char *file;		/* file where the section appears */
		int line;			/* line where the section appears */
		struct eb32_node id;		/* place in the tree of used IDs */
	} conf;					/* config information */
};

/* Descriptor for a "server" keyword. The ->parse() function returns 0 in case of
 * success, or a combination of ERR_* flags if an error is encountered. The
 * function pointer can be NULL if not implemented. The function also has an
 * access to the current "server" config line. The ->skip value tells the parser
 * how many words have to be skipped after the keyword. If the function needs to
 * parse more keywords, it needs to update cur_arg.
 */
struct srv_kw {
	const char *kw;
	int (*parse)(char **args, int *cur_arg, struct proxy *px, struct server *srv, char **err);
	int skip; /* nb min of args to skip, for use when kw is not handled */
	int default_ok; /* non-zero if kw is supported in default-server section */
};

/*
 * A keyword list. It is a NULL-terminated array of keywords. It embeds a
 * struct list in order to be linked to other lists, allowing it to easily
 * be declared where it is needed, and linked without duplicating data nor
 * allocating memory. It is also possible to indicate a scope for the keywords.
 */
struct srv_kw_list {
	const char *scope;
	struct list list;
	struct srv_kw kw[VAR_ARRAY];
};

#endif /* _TYPES_SERVER_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
