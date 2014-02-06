/* OpenVAS
 * $Id$
 * Description: Header file for module network.
 *
 * Authors:
 * Renaud Deraison <deraison@nessus.org> (Original pre-fork development)
 *
 * Copyright:
 * Based on work Copyright (C) 1998 - 2007 Tenable Network Security, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef OPENVAS_NETWORK_H
#define OPENVAS_NETWORK_H

#include <sys/select.h>         /* at least for fd_set */
#include <netinet/in.h>         /* struct in_addr, struct in6_addr */
#include <errno.h>
#include <ctype.h>
#include <assert.h>

#include "../base/array.h"

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "arglists.h"

/* Ports related */

/**
 * @brief A port range.
 */
struct range
{
  gchar *comment;       /* Comment. */
  int end;              /* End port.  0 for single port. */
  int exclude;          /* Whether to exclude range. */
  gchar *id;            /* UUID. */
  int start;            /* Start port. */
  int type;             /* Port protocol. */
};
typedef struct range range_t;

/**
 * @brief Possible port types.
 *
 * Used in Manager database. If any symbol changes then a migrator must be
 * added to update existing data.
 */
typedef enum
{
  PORT_PROTOCOL_TCP = 0,
  PORT_PROTOCOL_UDP = 1,
  PORT_PROTOCOL_OTHER = 2
} port_protocol_t;

/* Plugin specific network functions */
int open_sock_tcp (struct arglist *, unsigned int, int);
int open_sock_udp (struct arglist *, unsigned int);
int open_sock_option (struct arglist *, unsigned int, int, int, int);
int recv_line (int, char *, size_t);
int nrecv (int, void *, int, int);
int socket_close (int);
int get_sock_infos (int sock, int *r_transport, void **r_tls_session);

int open_stream_connection (struct arglist *, unsigned int, int, int);
int open_stream_connection_ext (struct arglist *, unsigned int, int, int,
                                const char *);
int open_stream_connection_unknown_encaps (struct arglist *, unsigned int, int,
                                           int *);
int open_stream_connection_unknown_encaps5 (struct arglist *, unsigned int, int,
                                            int *, int *);
int open_stream_auto_encaps_ext (struct arglist *args, unsigned int port,
                                 int timeout, int force);
int open_stream_auto_encaps (struct arglist *, unsigned int, int);

int write_stream_connection (int, void *buf, int n);
int read_stream_connection (int, void *, int);
int read_stream_connection_min (int, void *, int, int);
int nsend (int, void *, int, int);
int close_stream_connection (int);

const char *get_encaps_name (int);
const char *get_encaps_through (int);

/* Additional functions -- should not be used by the plugins */
int open_sock_tcp_hn (const char *, unsigned int);
int open_sock_opt_hn (const char *, unsigned int, int, int, int);

#ifdef __GNUC__
void auth_printf (struct arglist *, char *, ...) __attribute__ ((format (printf, 2, 3)));       /* RATS: ignore */
#else
void auth_printf (struct arglist *, char *, ...);
#endif

void auth_send (struct arglist *, char *);
char *auth_gets (struct arglist *, char *, size_t);

int openvas_SSL_init ();

int stream_set_buffer (int, int);
int stream_get_buffer_sz (int);
int stream_get_err (int);

void *stream_get_ssl (int);

struct ovas_scanner_context_s;
typedef struct ovas_scanner_context_s *ovas_scanner_context_t;

ovas_scanner_context_t ovas_scanner_context_new (int encaps,
                                                 const char *certfile,
                                                 const char *keyfile,
                                                 const char *passwd,
                                                 const char *cacertfile,
                                                 int force_pubkey_auth);

void ovas_scanner_context_free (ovas_scanner_context_t);
int ovas_scanner_context_attach (ovas_scanner_context_t ctx, int soc);

int openvas_register_connection (int s, void *ssl,
                                 gnutls_certificate_credentials_t certcred);
int openvas_deregister_connection (int);
int openvas_get_socket_from_connection (int);
gnutls_session_t *ovas_get_tlssession_from_connection (int);

int stream_zero (fd_set *);
int stream_set (int, fd_set *);
int stream_isset (int, fd_set *);

struct in_addr socket_get_next_source_addr ();
struct in6_addr socket_get_next_source_v4_addr ();
struct in6_addr socket_get_next_source_v6_addr ();
int set_socket_source_addr (int, int, int);
void socket_source_init (struct in6_addr *, int family);

int os_send (int, void *, int, int);
int os_recv (int, void *, int, int);

int internal_send (int, char *, int);
int internal_recv (int, char **, int *, int *);

int fd_is_stream (int);
int stream_pending (int);

int stream_set_timeout (int, int);
int stream_set_options (int, int, int);

void convipv4toipv4mappedaddr (struct in_addr, struct in6_addr *);

int
validate_port_range (const char *);

array_t*
port_range_ranges (const char *);

#endif
