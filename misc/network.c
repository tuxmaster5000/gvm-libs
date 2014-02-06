/* OpenVAS
 * $Id$
 * Description: Network Functions.
 *
 * Authors:
 * Renaud Deraison <deraison@nessus.org> (Original pre-fork development)
 * Michel Arboi (Original pre-fork development)
 * Werner Koch <wk@gnupg.org>
 *
 * Copyright:
 * Based on work Copyright (C) 1998 - 2002 Renaud Deraison
 *               SSL Support Copyright (C) 2001 Michel Arboi
 * Copyright (C) 2012 Greenbone Networks GmbH
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

#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>              /* for FILE */
#include <sys/time.h>           /* for gettimeofday */

#include <glib.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "system.h"             /* for efree(), erealloc() */
#include "network.h"            /* for socket_close() */
#include "kb.h"                 /* for kb_item_get_str() */

#include "resolve.h"
#include "ids_send.h"
#include "plugutils.h" /* for OPENVAS_ENCAPS_IP */
#include "internal_com.h" /* for INTERNAL_COMM_MSG_TYPE_CTRL */
#include "support.h"
#include "openvas_logging.h"
#include "../nasl/nasl_ssh.h"   /* for nasl_ssh_internal_close */

#include <setjmp.h>

#define TIMEOUT 20

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#ifndef ExtFunc
#define ExtFunc
#endif

/*----------------------------------------------------------------*
 * Low-level connection management                                *
 *----------------------------------------------------------------*/

/** OpenVAS "FILE" structure */
typedef struct
{
  int fd;        /**< socket number, or whatever */
  int transport; /**< "transport" layer code when stream is encapsultated.
                   * Negative transport signals a free descriptor */
  char *priority;/**< Malloced "priority" string for certain transports.  */
  int timeout;   /**< timeout, in seconds. Special values: -2 for default */
  int options;   /**< Misc options - see ids_send.h */

  int port;

  gnutls_session_t tls_session;              /**< GnuTLS session */
  gnutls_certificate_credentials_t tls_cred; /**< GnuTLS credentials */

  pid_t pid;    /**< Owner - for debugging only */

  char *buf;    /**< NULL if unbuffered */
  int bufsz, bufcnt, bufptr;
  int last_err;
} openvas_connection;

/**
 * The role of this offset is:
 * 1. To detect bugs when the program tries to write to a bad fd
 * 2. See if a fd is a real socket or a "openvas descriptor". This is a
 * quick & dirty hack and should be changed!!!
 */
#define OPENVAS_FD_MAX 1024
#define OPENVAS_FD_OFF 1000000

static openvas_connection connections[OPENVAS_FD_MAX];


static void my_gnutls_transport_set_lowat_default (gnutls_session_t session);



/**
 * OPENVAS_STREAM(x) is TRUE if <x> is a OpenVAS-ified fd
 */
#define OPENVAS_STREAM(x) (((x - OPENVAS_FD_OFF) < OPENVAS_FD_MAX) && ((x - OPENVAS_FD_OFF) >=0))

/**
 * determine the openvas_connection* from the openvas fd
 */
#define OVAS_CONNECTION_FROM_FD(fd) (connections + ((fd) - OPENVAS_FD_OFF))

void
convipv4toipv4mappedaddr (struct in_addr inaddr, struct in6_addr *in6addr)
{
  in6addr->s6_addr32[0] = 0;
  in6addr->s6_addr32[1] = 0;
  in6addr->s6_addr32[2] = htonl (0xffff);
  in6addr->s6_addr32[3] = inaddr.s_addr;
}

static void
renice_myself ()
{
  static pid_t pid = 0;
  pid_t cpid = getpid ();

  if (pid != cpid)
    {
      int renice_result;
      if (nice (0) >= 10)
        return;
      pid = cpid;
      errno = 0;
      renice_result = nice (1);
      if (renice_result == -1 && errno != 0)
        {
          log_legacy_write ("Unable to renice process: %d", errno);
        }
    }
}

/**
 * Same as perror(), but prefixes the data by our pid.
 */
static int
pid_perror (const char *error)
{
  log_legacy_write ("[%d] %s : %s\n", getpid (), error, strerror (errno));
  return 0;
}

static void
pid_notice (const char *text)
{
  log_legacy_write ("[%d] %s\n", getpid (), text);
}


int
stream_get_err (int fd)
{
  openvas_connection *p;

  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }

  p = &(connections[fd - OPENVAS_FD_OFF]);
  return p->last_err;
}

/**
 * @brief Returns a free file descriptor.
 */
static int
get_connection_fd ()
{
  int i;

  for (i = 0; i < OPENVAS_FD_MAX; i++)
    {
      if (connections[i].transport <= 0)        /* Not used */
        {
          bzero (&(connections[i]), sizeof (connections[i]));
          connections[i].pid = getpid ();
          return i + OPENVAS_FD_OFF;
        }
    }
  log_legacy_write ("[%d] %s:%d : Out of OpenVAS file descriptors\n",
                    getpid (), __FILE__, __LINE__);
  errno = EMFILE;
  return -1;
}



static int
release_connection_fd (int fd)
{
  openvas_connection *p;

  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }

  p = &(connections[fd - OPENVAS_FD_OFF]);

  efree (&p->buf);

  /* TLS FIXME: we should call gnutls_bye somewhere.  OTOH, the OpenSSL
   * equivalent SSL_shutdown wasn't called anywhere in the OpenVAS
   * (libopenvas nor elsewhere) code either.
   */

/* So far, fd is always a socket. If this is changed in the future, this
 * code shall be fixed. */
  if (p->fd >= 0)
    {
#if DEBUG_SSL > 1
      log_legacy_write (
               "[%d] release_connection_fd: fd > 0 fd=%d\n", getpid (), p->fd);
#endif
      if (shutdown (p->fd, 2) < 0)
        {
#if DEBUG_SSL > 1
          /*
           * It's not uncommon to see that one fail, since a lot of
           * services close the connection before we ask them to
           * (ie: http), so we don't show this error by default
           */
          pid_perror ("release_connection_fd: shutdown()");
#endif
        }
      if (socket_close (p->fd) < 0)
        pid_perror ("release_connection_fd: close()");
    }

  if (p->tls_session != NULL)
    gnutls_deinit (p->tls_session);
  if (p->tls_cred != NULL)
    gnutls_certificate_free_credentials (p->tls_cred);

  efree (&p->priority);

  bzero (p, sizeof (*p));
  p->transport = -1;

  return 0;
}

/* ******** Compatibility function ******** */

/** @todo TLS FIXME: migrate this to TLS */
/** @todo Fix the voidness of the ssl parameter (problematic in 64bit env.)
  *       here or on caller-side */
/**
 * @param soc Socket to use.
 */
int
ovas_allocate_connection (int soc, void *ssl,
                          gnutls_certificate_credentials_t certcred)
{
  int fd;
  openvas_connection *p;

  if ((fd = get_connection_fd ()) < 0)
    return -1;
  p = OVAS_CONNECTION_FROM_FD (fd);

  p->tls_session = ssl;
  p->tls_cred = certcred;

  p->timeout = TIMEOUT;         /* default value */
  p->port = 0;                  /* just used for debug */
  p->fd = soc;
  p->transport = (ssl != NULL) ? OPENVAS_ENCAPS_TLSv1 : OPENVAS_ENCAPS_IP;
  p->priority = NULL;
  p->last_err = 0;

  return fd;
}

/**
 * @param soc Socket to use.
 */
int
openvas_register_connection (int soc, void *ssl,
                             gnutls_certificate_credentials_t certcred)
{
  return ovas_allocate_connection (soc, ssl, certcred);
}

int
openvas_deregister_connection (int fd)
{
  openvas_connection *p;
  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }

  p = connections + (fd - OPENVAS_FD_OFF);
  /* Fixme: Code duplicated from release_connection_fd.  Check usage
     of this function make sure that TLS stuff is also released in
     case it is used here.  */
  efree (&p->priority);
  bzero (p, sizeof (*p));
  p->transport = -1;
  return 0;
}

/*----------------------------------------------------------------*
 * High-level connection management                               *
 *----------------------------------------------------------------*/

static int __port_closed;

static int
unblock_socket (int soc)
{
  int flags = fcntl (soc, F_GETFL, 0);
  if (flags < 0)
    {
      pid_perror ("fcntl(F_GETFL)");
      return -1;
    }
  if (fcntl (soc, F_SETFL, O_NONBLOCK | flags) < 0)
    {
      pid_perror ("fcntl(F_SETFL,O_NONBLOCK)");
      return -1;
    }
  return 0;
}

static int
block_socket (int soc)
{
  int flags = fcntl (soc, F_GETFL, 0);
  if (flags < 0)
    {
      pid_perror ("fcntl(F_GETFL)");
      return -1;
    }
  if (fcntl (soc, F_SETFL, (~O_NONBLOCK) & flags) < 0)
    {
      pid_perror ("fcntl(F_SETFL,~O_NONBLOCK)");
      return -1;
    }
  return 0;
}

/*
 * Initialize the SSL library (error strings and algorithms) and try
 * to set the pseudo random generator to something less silly than the
 * default value: 1 according to SVID 3, BSD 4.3, ISO 9899 :-(
 */


void
tlserror (char *txt, int err)
{
  log_legacy_write ("[%d] %s: %s\n", getpid (), txt, gnutls_strerror (err));
}



/**
 * @brief Initializes SSL support.
 */
int
openvas_SSL_init ()
{
  static int initialized = 0;

  if (initialized)
    return 0;

  int ret = gnutls_global_init ();
  if (ret < 0)
    {
      tlserror ("gnutls_global_init", ret);
      return -1;
    }

  initialized = 1;

  return 0;
}


int
openvas_get_socket_from_connection (int fd)
{
  openvas_connection *fp;

  if (!OPENVAS_STREAM (fd))
    {
      log_legacy_write (
               "[%d] openvas_get_socket_from_connection: bad fd <%d>\n",
               getpid (), fd);
      log_legacy_fflush ();
      return fd;
    }
  fp = connections + (fd - OPENVAS_FD_OFF);
  if (fp->transport <= 0)
    {
      log_legacy_write (
               "openvas_get_socket_from_connection: fd <%d> is closed\n", fd);
      return -1;
    }
  return fp->fd;
}

gnutls_session_t *
ovas_get_tlssession_from_connection (int fd)
{
  openvas_connection *fp;

  if (!OPENVAS_STREAM (fd))
    return NULL;

  fp = connections + (fd - OPENVAS_FD_OFF);
  return &fp->tls_session;
}

/**
 * Sets the priorities for the GnuTLS session according to encaps, one
 * of the OPENVAS_ENCAPS_* constants.  PRIORITY is used to convey
 * custom priorities; it is only used if ENCAPS is set to
 * OPENVAS_ENCAPS_TLScustom.
 */
static int
set_gnutls_protocol (gnutls_session_t session, int encaps, const char *priority)
{
  const char * priorities;
  const char * errloc;
  int err;

  /*
   * MAC Priority: From gnutls 3.0 on this could be "MAC-ALL",
   * but older versions don't support this. Therefore
   * "+SHA1:+MD5" is applied.
   * COMPRESSION Priority: * From gnutls 3.0 on this could be "COMP-ALL",
   * but older versions don't support this. Therefore
   * "+COMP-DEFLATE:+COMP-NULL" is applied.
   */

  switch (encaps)
    {
    case OPENVAS_ENCAPS_SSLv3:
      priorities = ("NONE:+VERS-SSL3.0:+3DES-CBC:+ARCFOUR-128:+COMP-DEFLATE"
                    ":+COMP-NULL:+RSA:+DHE-RSA:+DHE-DSS:+SHA1:+MD5");
      break;
    case OPENVAS_ENCAPS_TLSv1:
      priorities = ("NONE:+VERS-TLS1.0:+AES-256-CBC:+AES-128-CBC:+3DES-CBC"
                    ":+ARCFOUR-128:+COMP-DEFLATE:+COMP-NULL"
		    ":+RSA:+DHE-RSA:+DHE-DSS:+SHA1:+MD5");
      break;
    case OPENVAS_ENCAPS_SSLv23:        /* Compatibility mode */
      priorities = ("NONE:+VERS-TLS1.0:+VERS-SSL3.0:+AES-256-CBC:+AES-128-CBC"
                    ":+3DES-CBC:+ARCFOUR-128:+COMP-DEFLATE:+COMP-NULL"
                    ":+RSA:+DHE-RSA:+DHE-DSS:+SHA1:+MD5");
      break;
    case OPENVAS_ENCAPS_TLScustom:
      if (!priority || !*priority)
        {
          pid_notice ("no priority string given for ENCAPS_TLScustom");
          return -1;
        }
      priorities = priority;
      break;

    default:
#if DEBUG_SSL > 0
      log_legacy_write ("*Bug* at %s:%d. Unknown transport %d\n", __FILE__,
                        __LINE__, encaps);
#endif
      /* Use same priorities as for OPENVAS_ENCAPS_SSLv23 */
      priorities = ("NONE:+VERS-TLS1.0:+VERS-SSL3.0:+AES-256-CBC:+AES-128-CBC"
                    ":+3DES-CBC:+ARCFOUR-128:+COMP-DEFLATE:+COMP-NULL"
                    ":+RSA:+DHE-RSA:+DHE-DSS:+SHA1:+MD5");
      break;
    }

  if ((err = gnutls_priority_set_direct (session, priorities, &errloc)))
    {
      log_legacy_write ("[%d] setting session priorities '%.20s': %s\n",
                        getpid (), errloc, gnutls_strerror (err));
      return -1;
    }

  return 0;
}

/**
 * Verifies the peer's certificate.  If the certificate is not valid or
 * cannot be verified, the function logs a diagnostics and
 * returns -1.  If the certificate was verified successfully the
 * function returns 0.  If the peer did not send a certificate, the
 * function also returns 0.
 */
static int
verify_peer_certificate (gnutls_session_t session)
{
  static struct
  {
    int flag;
    const char *message;
  } messages[] =
  {
    {
    GNUTLS_CERT_NOT_ACTIVATED, "The certificate is not yet valid"},
    {
    GNUTLS_CERT_EXPIRED, "The certificate has expired"},
    {
    GNUTLS_CERT_REVOKED, "The certificate has been revoked"},
    {
    GNUTLS_CERT_SIGNER_NOT_FOUND,
        "The certificate doesn't have a known issuer"},
    {
    GNUTLS_CERT_SIGNER_NOT_CA, "The certificate's issuer is not a CA"},
    {
    GNUTLS_CERT_INSECURE_ALGORITHM,
        "The certificate was signed using an insecure algorithm"},
    {
    GNUTLS_CERT_INVALID, "The certificate is invalid"},
    {
  0, NULL},};
  unsigned int status;
  int ret;
  int i;
  int any_error;

  ret = gnutls_certificate_verify_peers2 (session, &status);
  if (ret == GNUTLS_E_NO_CERTIFICATE_FOUND)
    /* The peer did not send a certificate.  We treat it as a valid
     * certificate in this function */
    return 0;
  if (ret < 0)
    {
      tlserror ("gnutls_certificate_verify_peers2", ret);
      return -1;
    }

  for (i = any_error = 0; messages[i].message != NULL; i++)
    if (status & messages[i].flag)
      any_error = 1;

  if (any_error)
    {
      log_legacy_write ("[%d] failed to verify the peer certificate:\n",
                        getpid ());
      for (i = 0; messages[i].message != NULL; i++)
        {
          if (status & messages[i].flag)
            log_legacy_write ("[%d]    %s\n",
                              getpid (), messages[i].message);
        }
    }

  if (status)
    return -1;

  return 0;
}


/** helper function copied from cli.c from GnuTLS
    Reads a file into a gnutls_datum_t
 **/
static gnutls_datum_t
load_file (const char *file)
{
  FILE *f;
  gnutls_datum_t loaded_file = { NULL, 0 };
  long filelen;
  void *ptr;

  if (!(f = fopen (file, "r")))
    return loaded_file;
  if (fseek (f, 0, SEEK_END) != 0 || (filelen = ftell (f)) < 0
      || fseek (f, 0, SEEK_SET) != 0 || !(ptr = emalloc ((size_t) filelen))
      || fread (ptr, 1, (size_t) filelen, f) < (size_t) filelen)
    {
      fclose (f);
      return loaded_file;
    }

  loaded_file.data = ptr;
  loaded_file.size = (unsigned int) filelen;
  fclose (f);
  return loaded_file;
}

/**
 * @brief Helper function copied from cli.c from GnuTLS.
 *
 * Frees the data read by load_file.  It's safe to call this function
 * twice on the same data.
 */
static void
unload_file (gnutls_datum_t * data)
{
  efree (&(data->data));
}


/**
 * @brief Loads a certificate and the corresponding private key from PEM files.
 *
 * The private key may be encrypted, in which case the password to
 * decrypt the key should be given as the passwd parameter.
 *
 * @return Returns 0 on success and -1 on failure.
 */
static int
load_cert_and_key (gnutls_certificate_credentials_t xcred, const char *cert,
                   const char *key, const char *passwd)
{
  gnutls_x509_crt_t x509_crt = NULL;
  gnutls_x509_privkey_t x509_key = NULL;
  gnutls_datum_t data = { NULL, 0 };
  int ret;
  int result = 0;

  data = load_file (cert);
  if (data.data == NULL)
    {
      log_legacy_write ("[%d] load_cert_and_key: Error loading cert file %s\n",
                        getpid (), cert);
      result = -1;
      goto cleanup;
    }

  ret = gnutls_x509_crt_init (&x509_crt);
  if (ret < 0)
    {
      tlserror ("gnutls_x509_crt_init", ret);
      /* x509_crt may be != NULL even if gnutls_x509_crt_init fails */
      x509_crt = NULL;
      result = -1;
      goto cleanup;
    }

  ret = gnutls_x509_crt_import (x509_crt, &data, GNUTLS_X509_FMT_PEM);
  if (ret < 0)
    {
      tlserror ("gnutls_x509_crt_import", ret);
      result = -1;
      goto cleanup;
    }

  unload_file (&data);

  data = load_file (key);
  if (data.data == NULL)
    {
      log_legacy_write ("[%d] load_cert_and_key: Error loading key file %s\n",
                        getpid (), key);
      result = -1;
      goto cleanup;
    }

  ret = gnutls_x509_privkey_init (&x509_key);
  if (ret < 0)
    {
      tlserror ("gnutls_x509_privkey_init", ret);
      /* x509_key may be != NULL even if gnutls_x509_privkey_init fails */
      x509_key = NULL;
      result = -1;
      goto cleanup;
    }

  if (passwd)
    {
      ret =
        gnutls_x509_privkey_import_pkcs8 (x509_key, &data, GNUTLS_X509_FMT_PEM,
                                          passwd, 0);
      if (ret < 0)
        {
          tlserror ("gnutls_x509_privkey_import_pkcs8", ret);
          result = -1;
          goto cleanup;
        }
    }
  else
    {
      ret = gnutls_x509_privkey_import (x509_key, &data, GNUTLS_X509_FMT_PEM);
      if (ret < 0)
        {
          tlserror ("gnutls_x509_privkey_import", ret);
          result = -1;
          goto cleanup;
        }
    }

  unload_file (&data);

  ret = gnutls_certificate_set_x509_key (xcred, &x509_crt, 1, x509_key);
  if (ret < 0)
    {
      tlserror ("gnutls_certificate_set_x509_key", ret);
      result = -1;
      goto cleanup;
    }

cleanup:

  unload_file (&data);
  if (x509_crt)
    gnutls_x509_crt_deinit (x509_crt);
  if (x509_key)
    gnutls_x509_privkey_deinit (x509_key);

  return result;
}

static int
open_SSL_connection (openvas_connection * fp, int timeout, const char *cert,
                     const char *key, const char *passwd, const char *cafile)
{
  int ret, err, d;
  time_t tictac;
  fd_set fdw, fdr;
  struct timeval to;

  openvas_SSL_init ();

  ret = gnutls_init (&(fp->tls_session), GNUTLS_CLIENT);
  if (ret < 0)
    {
      tlserror ("gnutls_init", ret);
      return -1;
    }

  /* set_gnutls_protocol handles OPENVAS_ENCAPS_SSLv2 by falling back
   * to OPENVAS_ENCAPS_SSLv23.  However, this function
   * (open_SSL_connection) is called only by open_stream_connection and
   * open_stream_connection will exit with an error code if called with
   * OPENVAS_ENCAPS_SSLv2, so it should never end up calling
   * open_SSL_connection with OPENVAS_ENCAPS_SSLv2.
   */
  if (set_gnutls_protocol (fp->tls_session, fp->transport, fp->priority) < 0)
    return -1;

  ret = gnutls_certificate_allocate_credentials (&(fp->tls_cred));
  if (ret < 0)
    {
      tlserror ("gnutls_certificate_allocate_credentials", ret);
      return -1;
    }
  ret =
    gnutls_credentials_set (fp->tls_session, GNUTLS_CRD_CERTIFICATE,
                            fp->tls_cred);
  if (ret < 0)
    {
      tlserror ("gnutls_credentials_set", ret);
      return -1;
    }

  if (cert != NULL && key != NULL)
    {
      if (load_cert_and_key (fp->tls_cred, cert, key, passwd) < 0)
        return -1;
    }

  if (cafile != NULL)
    {
      ret =
        gnutls_certificate_set_x509_trust_file (fp->tls_cred, cafile,
                                                GNUTLS_X509_FMT_PEM);
      if (ret < 0)
        {
          tlserror ("gnutls_certificate_set_x509_trust_file", ret);
          return -1;
        }
    }

  unblock_socket (fp->fd);
  /* for non-blocking sockets, gnutls < 2.12.0 requires a 0 lowat value */
#if GNUTLS_VERSION_NUMBER < 0x020c00
  gnutls_transport_set_lowat (fp->tls_session, 0);
#endif

  gnutls_transport_set_ptr (fp->tls_session,
                            (gnutls_transport_ptr_t) GSIZE_TO_POINTER (fp->fd));

  tictac = time (NULL);

  for (;;)
    {
      err = gnutls_handshake (fp->tls_session);

      if (err == 0)
        {
#ifdef DEBUG_SSL
          pid_notice ("gnutls_handshake succeeded");
#endif
          return 1;
        }

      if (err != GNUTLS_E_INTERRUPTED && err != GNUTLS_E_AGAIN)
        {
#ifdef DEBUG_SSL
          tlserror ("gnutls_handshake", err);
#endif
          return -1;
        }

      FD_ZERO (&fdr);
      FD_SET (fp->fd, &fdr);
      FD_ZERO (&fdw);
      FD_SET (fp->fd, &fdw);

      do
        {
          d = tictac + timeout - time (NULL);
          if (d <= 0)
            {
              fp->last_err = ETIMEDOUT;
              return -1;
            }
          to.tv_sec = d;
          to.tv_usec = 0;
          errno = 0;
          if ((ret = select (fp->fd + 1, &fdr, &fdw, NULL, &to)) <= 0)
            {
#if DEBUG_SSL > 1
              pid_perror ("select");
#endif
            }
        }
      while (ret < 0 && errno == EINTR);

      if (ret <= 0)
        {
          fp->last_err = ETIMEDOUT;
          return -1;
        }
    }
 /*NOTREACHED*/}


static void
set_ids_evasion_mode (struct arglist *args, openvas_connection * fp)
{
  struct kb_item **kb = plug_get_kb (args);
  char *ids_evasion_split = kb_item_get_str (kb, "NIDS/TCP/split");
  char *ids_evasion_inject = kb_item_get_str (kb, "NIDS/TCP/inject");
  char *ids_evasion_short_ttl = kb_item_get_str (kb, "NIDS/TCP/short_ttl");
  char *ids_evasion_fake_rst = kb_item_get_str (kb, "NIDS/TCP/fake_rst");
  int option = 0;


  /*
   * These first three options are mutually exclusive
   */
  if (ids_evasion_split != NULL && strcmp (ids_evasion_split, "yes") == 0)
    option = OPENVAS_CNX_IDS_EVASION_SPLIT;

  if (ids_evasion_inject != NULL && strcmp (ids_evasion_inject, "yes") == 0)
    option = OPENVAS_CNX_IDS_EVASION_INJECT;

  if (ids_evasion_short_ttl != NULL
      && strcmp (ids_evasion_short_ttl, "yes") == 0)
    option = OPENVAS_CNX_IDS_EVASION_SHORT_TTL;


  /*
   * This is not exclusive with the above
   */
  if (ids_evasion_fake_rst != NULL && strcmp (ids_evasion_fake_rst, "yes") == 0)
    option |= OPENVAS_CNX_IDS_EVASION_FAKE_RST;

  if (option)
    {
#ifdef SO_SNDLOWAT
      int n = 1;
      (void) setsockopt (fp->fd, SOL_SOCKET, SO_SNDLOWAT, (void *) &n,
                         sizeof (n));
#endif
      fp->options |= option;
    }
}


/* Extended version of open_stream_connection to allow passing a
   priority string.

   ABI_BREAK_NOTE: Merge this with open_stream_connection.  */
int
open_stream_connection_ext (struct arglist *args, unsigned int port,
                            int transport, int timeout, const char *priority)
{
  int fd;
  openvas_connection *fp;
  char *cert = NULL;
  char *key = NULL;
  char *passwd = NULL;
  char *cafile = NULL;

  if (!priority)
    priority = ""; /* To us an empty string is equivalent to NULL.  */

#if DEBUG_SSL > 2
  log_legacy_write (
           "[%d] open_stream_connection: TCP:%d transport:%d timeout:%d "
           " priority: '%s'\n",
           getpid (), port, transport, timeout, priority);
#endif

  if (timeout == -2)
    timeout = TIMEOUT;

  switch (transport)
    {
    case OPENVAS_ENCAPS_IP:

    case OPENVAS_ENCAPS_SSLv23:
    case OPENVAS_ENCAPS_SSLv3:
    case OPENVAS_ENCAPS_TLSv1:
    case OPENVAS_ENCAPS_TLScustom:
      break;

    case OPENVAS_ENCAPS_SSLv2:
    default:
      log_legacy_write (
               "open_stream_connection(): unsupported transport layer %d\n",
               transport);
      errno = EINVAL;
      return -1;
    }

  if ((fd = get_connection_fd ()) < 0)
    return -1;

  fp = &(connections[fd - OPENVAS_FD_OFF]);


  fp->transport = transport;
  efree (&fp->priority);
  if (*priority)
    fp->priority = estrdup (priority);
  fp->timeout = timeout;
  fp->port = port;
  fp->last_err = 0;
  set_ids_evasion_mode (args, fp);

  if (fp->options & OPENVAS_CNX_IDS_EVASION_FAKE_RST)
    fp->fd = ids_open_sock_tcp (args, port, fp->options, timeout);
  else
    fp->fd = open_sock_tcp (args, port, timeout);

  if (fp->fd < 0)
    goto failed;

  switch (transport)
    {
    case OPENVAS_ENCAPS_IP:
      break;
    case OPENVAS_ENCAPS_SSLv23:
    case OPENVAS_ENCAPS_SSLv3:
    case OPENVAS_ENCAPS_TLSv1:
    case OPENVAS_ENCAPS_TLScustom:
      renice_myself ();
      cert = kb_item_get_str (plug_get_kb (args), "SSL/cert");
      key = kb_item_get_str (plug_get_kb (args), "SSL/key");
      passwd = kb_item_get_str (plug_get_kb (args), "SSL/password");

      cafile = kb_item_get_str (plug_get_kb (args), "SSL/CA");

      /* fall through */

    case OPENVAS_ENCAPS_SSLv2:
      /* We do not need a client certificate in this case */
      if (open_SSL_connection (fp, timeout, cert, key, passwd, cafile) <= 0)
        goto failed;
      break;
    }

  return fd;

failed:
  release_connection_fd (fd);
  return -1;
}


int
open_stream_connection (struct arglist *args, unsigned int port,
                        int transport, int timeout)
{
  return open_stream_connection_ext (args, port, transport, timeout, NULL);
}


/**
 * @param delta_t time in micro-seconds
 */
int
open_stream_connection_unknown_encaps5 (struct arglist *args, unsigned int port,
                                        int timeout, int *p, int *delta_t)
{
  int fd;
  int i;
  struct timeval tv1, tv2;
  static int encaps[] = {
    OPENVAS_ENCAPS_SSLv2,
    OPENVAS_ENCAPS_TLSv1,
    OPENVAS_ENCAPS_SSLv3,
    OPENVAS_ENCAPS_IP
  };

#if DEBUG_SSL > 2
  log_legacy_write ("[%d] open_stream_connection_unknown_encaps: TCP:%d; %d\n",
                    getpid (), port, timeout);
#endif

  for (i = 0; i < sizeof (encaps) / sizeof (*encaps); i++)
    {
      if (delta_t != NULL)
        (void) gettimeofday (&tv1, NULL);
      if ((fd = open_stream_connection (args, port, encaps[i], timeout)) >= 0)
        {
          *p = encaps[i];
#if DEBUG_SSL > 2
          log_lecacy_write ("[%d] open_stream_connection_unknown_encaps: "
                            "TCP:%d -> transport=%d\n",
                            getpid (), port, *p);
#endif
          if (delta_t != NULL)
            {
              (void) gettimeofday (&tv2, NULL);
              *delta_t =
                (tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec -
                                                       tv1.tv_usec);
            }
          return fd;
        }
      else if (__port_closed)
        {
#if DEBUG_SSL > 2
          log_lecacy_write ("[%d] open_stream_connection_unknown_encaps: "
                            "TCP:%d -> closed\n",
                            getpid (), port);
#endif
          return -1;
        }
    }
  return -1;
}

int
open_stream_connection_unknown_encaps (struct arglist *args, unsigned int port,
                                       int timeout, int *p)
{
  return open_stream_connection_unknown_encaps5 (args, port, timeout, p, NULL);
}


/* Same as open_stream_auto_encaps but allows to force auto detection
   of the protocols if FORCE is true.  */
int
open_stream_auto_encaps_ext (struct arglist *args, unsigned int port,
                             int timeout, int force)
{
  int trp = force? 0 : plug_get_port_transport (args, port);
  int fd;

  if (trp == 0)
    {
      /* Try to connect while figuring out the used encapsulation mode.  */
      if ((fd =
           open_stream_connection_unknown_encaps (args, port, timeout,
                                                  &trp)) < 0)
        return -1;
      /* Store that encapsulation mode in the KB.  */
      plug_set_port_transport (args, port, trp);
      return fd;
    }
  else
    {
      fd = open_stream_connection (args, port, trp, timeout);
      return fd;
    }
 /*NOTREACHED*/
}


int
open_stream_auto_encaps (struct arglist *args, unsigned int port, int timeout)
{
  return open_stream_auto_encaps_ext (args, port, timeout, 0);
}

/*
 * Scanner socket functions
 */

struct ovas_scanner_context_s
{
  /** Transport encapsulation to use */
  int encaps;

  /** Whether to force public key authentication */
  int force_pubkey_auth;

  /** GnuTLS credentials */
  gnutls_certificate_credentials_t tls_cred;
};

/**
 * @brief Creates a new ovas_scanner_context_t.
 *
 * The parameter encaps should be
 * one of the OPENVAS_ENCAPS_* constants.  If any of the SSL
 * encapsulations are used, the parameters certfile, keyfile, and cafile
 * should be the filenames of the scanner certificate and corresponding
 * key and the CA certificate.  The optional passwd parameter is used as
 * the password to decrypt the keyfile if it is encrypted.
 *
 * The force_pubkey_auth parameter is a boolean controlling public key
 * authentication of the client.  If force_pubkey_auth is true, the
 * client must authenticate itself with a certificate.  Otherwise the
 * client will be asked for a certificate but doesn't have to present
 * one.
 */
ovas_scanner_context_t
ovas_scanner_context_new (int encaps, const char *certfile, const char *keyfile,
                          const char *passwd, const char *cafile,
                          int force_pubkey_auth)
{
  ovas_scanner_context_t ctx = NULL;

  if (openvas_SSL_init () < 0)
    return NULL;

  ctx = emalloc (sizeof (struct ovas_scanner_context_s));
  if (ctx == NULL)
    return NULL;

  ctx->encaps = encaps;
  ctx->force_pubkey_auth = force_pubkey_auth;

  if (ctx->encaps != OPENVAS_ENCAPS_IP)
    {
      int ret = gnutls_certificate_allocate_credentials (&(ctx->tls_cred));
      if (ret < 0)
        {
          tlserror ("gnutls_certificate_allocate_credentials", ret);
          ctx->tls_cred = NULL;
          goto fail;
        }

      if (certfile && keyfile)
        {
          if (load_cert_and_key (ctx->tls_cred, certfile, keyfile, passwd) < 0)
            goto fail;
        }

      if (cafile != NULL)
        {
          ret =
            gnutls_certificate_set_x509_trust_file (ctx->tls_cred, cafile,
                                                    GNUTLS_X509_FMT_PEM);
          if (ret < 0)
            {
              tlserror ("gnutls_certificate_set_x509_trust_file", ret);
              goto fail;
            }
        }
    }

  return ctx;


fail:
  ovas_scanner_context_free (ctx);
  return NULL;
}


/**
 * @brief Frees the ovas_scanner_context_t instance ctx.
 *
 * If ctx is NULL, nothing is done.
 *
 * @param ctx ovas_scanner_context_t to free.
 */
void
ovas_scanner_context_free (ovas_scanner_context_t ctx)
{
  if (ctx == NULL)
    return;

  if (ctx->tls_cred != NULL)
    gnutls_certificate_free_credentials (ctx->tls_cred);

  efree (&ctx);
}

/**
 * @brief Sets up SSL/TLS on the socket soc and returns a openvas file
 * @brief descriptor.
 *
 * The parameters for the SSL/TLS layer are taken from ctx.
 * Afterwards, the credentials of ctx are also referenced by the SSL/TLS
 * objects associated with the openvas file descriptor.  This means that
 * the context ctx must not be freed until the openvas file descriptor is
 * closed.
 *
 * If the context's force_pubkey_auth member is true (!= 0), the client
 * must provide a certificate.  If force_pubkey_auth is false, the
 * client certificate is optional.  In any case, if the client provides
 * a certificate, the certificate is verified.  If the verification
 * fails, ovas_scanner_context_attach returns -1.
 *
 * @return The openvas file descriptor on success and -1 on failure.
 */
int
ovas_scanner_context_attach (ovas_scanner_context_t ctx, int soc)
{
  int fd = -1;
  openvas_connection *fp = NULL;
  int ret;

  fd = ovas_allocate_connection (soc, GINT_TO_POINTER (ctx->encaps), NULL);
  if (fd < 0)
    return -1;

  fp = OVAS_CONNECTION_FROM_FD (fd);

  if (fp->transport != OPENVAS_ENCAPS_IP)
    {
      ret = gnutls_init (&(fp->tls_session), GNUTLS_SERVER);
      if (ret < 0)
        {
          tlserror ("gnutls_init", ret);
          goto fail;
        }
      my_gnutls_transport_set_lowat_default (fp->tls_session);

      ret = set_gnutls_protocol (fp->tls_session, fp->transport, NULL);
      if (ret < 0)
        {
          goto fail;
        }

      if (ctx->tls_cred)
        {
          /* *fp contains a field for the gnutls credentials.  We do not
           * set it here because ctx->tls_cred is owned by ctx and
           * copying it to fp->tls_cred would lead to it being freed
           * when the connection is closed. */
          ret =
            gnutls_credentials_set (fp->tls_session, GNUTLS_CRD_CERTIFICATE,
                                    ctx->tls_cred);
          if (ret < 0)
            {
              tlserror ("gnutls_credentials_set", ret);
              return -1;
            }
        }


      /* request client certificate if any. */
      gnutls_certificate_server_set_request (fp->tls_session,
                                             ctx->force_pubkey_auth ?
                                             GNUTLS_CERT_REQUIRE :
                                             GNUTLS_CERT_REQUEST);

      gnutls_transport_set_ptr (fp->tls_session,
                                (gnutls_transport_ptr_t)
                                GSIZE_TO_POINTER (fp->fd));
    retry:
      ret = gnutls_handshake (fp->tls_session);
      if (ret < 0)
        {
          if (ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
            goto retry;
#ifdef DEBUG_SSL
          tlserror ("gnutls_handshake", ret);
#endif
          goto fail;
        }

      if (verify_peer_certificate (fp->tls_session) < 0)
        {
          goto fail;
        }
    }

  return fd;

fail:
  release_connection_fd (fd);
  return -1;
}



/**
 * TLS: This function is only used in one place,
 * openvas-plugins/plugins/ssl_ciphers/ssl_ciphers.c:145 (function
 * plugin_run).  The code there prints information about the
 * certificates and the server's ciphers if sslv2 is used.  Some of the
 * functionality should perhaps be moved to openvas-libraries.
 */
void *
stream_get_ssl (int fd)
{
  return NULL;
}


int
stream_set_timeout (int fd, int timeout)
{
  int old;
  openvas_connection *fp;
  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return 0;
    }
  fp = &(connections[fd - OPENVAS_FD_OFF]);
  old = fp->timeout;
  fp->timeout = timeout;
  return old;
}

int
stream_set_options (int fd, int reset_opt, int set_opt)
{
  openvas_connection *fp;
  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }
  fp = &(connections[fd - OPENVAS_FD_OFF]);
  fp->options &= ~reset_opt;
  fp->options |= set_opt;
  return 0;
}


static int
read_stream_connection_unbuffered (int fd, void *buf0, int min_len, int max_len)
{
  int ret, realfd, trp, t;
  int total = 0, flag = 0, timeout = TIMEOUT, waitall = 0;
  unsigned char *buf = (unsigned char *) buf0;
  openvas_connection *fp = NULL;
  fd_set fdr, fdw;
  struct timeval tv;
  time_t now, then;

  int select_status;

#if 0
  log_lecacy_write ("read_stream_connection(%d, 0x%x, %d, %d)\n", fd, buf,
                    min_len, max_len);
#endif

  if (OPENVAS_STREAM (fd))
    {
      fp = &(connections[fd - OPENVAS_FD_OFF]);
      trp = fp->transport;
      realfd = fp->fd;
      fp->last_err = 0;
      if (fp->timeout != -2)
        timeout = fp->timeout;
    }
  else
    {
#if 0
      log_lecacy_write ("read_stream_connection[%d] : supposedly bad fd %d\n",
                        getpid (), fd);
#endif
      trp = OPENVAS_ENCAPS_IP;
      if (fd < 0 || fd > 1024)
        {
          errno = EBADF;
          return -1;
        }
      realfd = fd;
    }

#ifndef INCR_TIMEOUT
# define INCR_TIMEOUT	1
#endif

#ifdef MSG_WAITALL
  if (min_len == max_len || timeout <= 0)
    waitall = MSG_WAITALL;
#endif

  if (trp == OPENVAS_ENCAPS_IP)
    {
      for (t = 0; total < max_len && (timeout <= 0 || t < timeout);)
        {
          tv.tv_sec = INCR_TIMEOUT;     /* Not timeout! */
          tv.tv_usec = 0;
          FD_ZERO (&fdr);
          FD_SET (realfd, &fdr);
          if (select (realfd + 1, &fdr, NULL, NULL, timeout > 0 ? &tv : NULL) <=
              0)
            {
              t += INCR_TIMEOUT;
              /* Try to be smart */
              if (total > 0 && flag)
                return total;
              else if (total >= min_len)
                flag++;
            }
          else
            {
              errno = 0;
              ret = recv (realfd, buf + total, max_len - total, waitall);
              if (ret < 0)
                if (errno != EINTR)
                  {
                    fp->last_err = errno;
                    return total;
                  }
                else
                  ret = 0;
              else if (ret == 0)        /* EOF */
                {
                  fp->last_err = EPIPE;
                  return total;
                }
              /*ret > 0 */
              total += ret;
              if (min_len > 0 && total >= min_len)
                return total;
              flag = 0;
            }
        }
      if (t >= timeout)
        fp->last_err = ETIMEDOUT;
      return total;
    }

  switch (trp)
    {
      /* OPENVAS_ENCAPS_IP was treated before with the non-OpenVAS fd */
    case OPENVAS_ENCAPS_SSLv2:
    case OPENVAS_ENCAPS_SSLv23:
    case OPENVAS_ENCAPS_SSLv3:
    case OPENVAS_ENCAPS_TLSv1:
# if DEBUG_SSL > 0
      if (getpid () != fp->pid)
        {
          log_lecacy_write ("PID %d tries to use a SSL connection established "
                            "by PID %d\n",
                            getpid (), fp->pid);
          errno = EINVAL;
          return -1;
        }
# endif

      now = then = time (NULL);
      for (t = 0; timeout <= 0 || t < timeout; t = now - then)
        {
          now = time (NULL);
          tv.tv_sec = INCR_TIMEOUT;
          tv.tv_usec = 0;
          FD_ZERO (&fdr);
          FD_ZERO (&fdw);
          FD_SET (realfd, &fdr);
          FD_SET (realfd, &fdw);

          select_status = select (realfd + 1, &fdr, &fdw, NULL, &tv);

          if (select_status > 0)
            {
              /* TLS FIXME: handle rehandshake */
              ret =
                gnutls_record_recv (fp->tls_session, buf + total,
                                    max_len - total);
              if (ret > 0)
                {
                  total += ret;
                }

              if (total >= max_len)
                return total;

              if (ret != GNUTLS_E_INTERRUPTED && ret != GNUTLS_E_AGAIN)
                {
                  /* This branch also handles the case where ret == 0,
                   * i.e. that the connection has been closed.  This is
                   * for compatibility with the old OpenSSL based openvas
                   * code which treated SSL_ERROR_ZERO_RETURN as an
                   * error too.
                   */
#ifdef DEBUG_SSL
                  if (ret < 0)
                    {
                      tlserror ("gnutls_record_recv", ret);
                    }
                  else
                    {
                      log_lecacy_write ("gnutls_record_recv[%d]: EOF\n",
                                        getpid ());
                    }
#endif
                  fp->last_err = EPIPE;
                  return total;
                }
            }

          if (min_len <= 0)
            {
              /* Be smart */
              if (total > 0 && flag)
                return total;
              else
                flag++;
            }
          else if (total >= min_len)
            return total;
        }
      if (t >= timeout)
        fp->last_err = ETIMEDOUT;
      return total;

    default:
      if (fp->transport != -1 || fp->fd != 0)
        log_legacy_write ("Severe bug! Unhandled transport layer %d (fd=%d)\n",
                          fp->transport, fd);
      else
        log_legacy_write ("read_stream_connection_unbuffered: "
                          "fd=%d is closed\n",
                          fd);
      errno = EINVAL;
      return -1;
    }
 /*NOTREACHED*/
}

int
read_stream_connection_min (int fd, void *buf0, int min_len, int max_len)
{
  openvas_connection *fp;

  if (OPENVAS_STREAM (fd))
    {
      fp = &(connections[fd - OPENVAS_FD_OFF]);
      if (fp->buf != NULL)
        {
          int l1, l2;

          if (max_len == 1)
            min_len = 1;        /* avoid "magic read" later */
          l2 = max_len > fp->bufcnt ? fp->bufcnt : max_len;
          if (l2 > 0)
            {
              memcpy (buf0, fp->buf + fp->bufptr, l2);
              fp->bufcnt -= l2;
              if (fp->bufcnt == 0)
                {
                  fp->bufptr = 0;
                  fp->buf[0] = '\0';    /* debug */
                }
              else
                fp->bufptr += l2;
              if (l2 >= min_len || l2 >= max_len)
                return l2;
              max_len -= l2;
              min_len -= l2;
            }
          if (min_len > fp->bufsz)
            {
              l1 =
                read_stream_connection_unbuffered (fd, (char *) buf0 + l2,
                                                   min_len, max_len);
              if (l1 > 0)
                return l1 + l2;
              else
                return l2;
            }
          /* Fill buffer */
          l1 =
            read_stream_connection_unbuffered (fd, fp->buf, min_len, fp->bufsz);
          if (l1 <= 0)
            return l2;

          fp->bufcnt = l1;
          l1 = max_len > fp->bufcnt ? fp->bufcnt : max_len;
          memcpy ((char *) buf0 + l2, fp->buf + fp->bufptr, l1);
          fp->bufcnt -= l1;
          if (fp->bufcnt == 0)
            fp->bufptr = 0;
          else
            fp->bufptr += l1;
          return l1 + l2;
        }
    }
  return read_stream_connection_unbuffered (fd, buf0, min_len, max_len);
}

int
read_stream_connection (int fd, void *buf0, int len)
{
  return read_stream_connection_min (fd, buf0, -1, len);
}

static int
write_stream_connection4 (int fd, void *buf0, int n, int i_opt)
{
  int ret, count;
  unsigned char *buf = (unsigned char *) buf0;
  openvas_connection *fp;
  fd_set fdr, fdw;
  struct timeval tv;
  int e;

  if (!OPENVAS_STREAM (fd))
    {
#if DEBUG_SSL > 0
      log_lecacy_write ("write_stream_connection: fd <%d> invalid\n", fd);
# if 0
      abort ();
# endif
#endif
      errno = EINVAL;
      return -1;
    }

  fp = &(connections[fd - OPENVAS_FD_OFF]);
  fp->last_err = 0;

#if DEBUG_SSL > 8
  log_lecacy_write (
           "> write_stream_connection(%d, %s, %d, 0x%x) \tE=%d 0=0x%x\n", fd,
           buf, n, i_opt, fp->transport, fp->options);
#endif

  switch (fp->transport)
    {
    case OPENVAS_ENCAPS_IP:
      for (count = 0; count < n;)
        {
          if ((fp->options & OPENVAS_CNX_IDS_EVASION_SEND_MASK) != 0)
            {
              if (fp->options & OPENVAS_CNX_IDS_EVASION_SPLIT)
                /* IDS evasion */
                ret = send (fp->fd, buf + count, 1, i_opt);
              else
                /* i_opt ignored for ids_send */
                ret = ids_send (fp->fd, buf + count, n - count, fp->options);
            }
          else
            ret = send (fp->fd, buf + count, n - count, i_opt);

          if (ret <= 0)
            {
              if (ret < 0)
                fp->last_err = errno;
              else
                fp->last_err = EPIPE;
              break;
            }

          count += ret;
        }
      break;

    case OPENVAS_ENCAPS_SSLv2:
    case OPENVAS_ENCAPS_SSLv23:
    case OPENVAS_ENCAPS_SSLv3:
    case OPENVAS_ENCAPS_TLSv1:

      /* i_opt ignored for SSL */
      for (count = 0; count < n;)
        {
          ret = gnutls_record_send (fp->tls_session, buf + count, n - count);

          if (ret > 0)
            {
              count += ret;
            }
          else if (ret != GNUTLS_E_INTERRUPTED && ret != GNUTLS_E_AGAIN)
            {
              /* This branch also handles the case where ret == 0,
               * i.e. that the connection has been closed.  This is
               * for compatibility with the old openvas code which
               * treated SSL_ERROR_ZERO_RETURN as an error too.
               */
#ifdef DEBUG_SSL
              if (ret < 0)
                {
                  tlserror ("gnutls_record_send", ret);
                }
              else
                {
                  log_lecacy_write ("gnutls_record_send[%d]: EOF\n", getpid ());
                }
#endif
              fp->last_err = EPIPE;
              break;
            }

          if (fp->timeout >= 0)
            tv.tv_sec = fp->timeout;
          else
            tv.tv_sec = TIMEOUT;
          tv.tv_usec = 0;

          do
            {
              errno = 0;
              FD_ZERO (&fdr);
              FD_ZERO (&fdw);
              FD_SET (fp->fd, &fdr);
              FD_SET (fp->fd, &fdw);
              e = select (fp->fd + 1, &fdr, &fdw, NULL, &tv);
            }
          while (e < 0 && errno == EINTR);

          if (e <= 0)
            {
#if DEBUG_SSL > 0
              pid_perror ("select");
#endif
              fp->last_err = ETIMEDOUT;
              break;
            }
        }
      break;

    default:
      if (fp->transport != -1 || fp->fd != 0)
        log_legacy_write ("Severe bug! Unhandled transport layer %d (fd=%d)\n",
                          fp->transport, fd);
      else
        log_legacy_write ("read_stream_connection_unbuffered: fd=%d is "
                          "closed\n", fd);
      errno = EINVAL;
      return -1;
    }

  if (count == 0 && n > 0)
    return -1;
  else
    return count;
}

int
write_stream_connection (int fd, void *buf0, int n)
{
  return write_stream_connection4 (fd, buf0, n, 0);
}

int
nsend (int fd, void *data, int length, int i_opt)
{
  int n = 0;

  if (OPENVAS_STREAM (fd))
    {
      if (connections[fd - OPENVAS_FD_OFF].fd < 0)
        log_legacy_write ("OpenVAS file descriptor %d closed ?!\n", fd);
      else
        return write_stream_connection4 (fd, data, length, i_opt);
    }
#if DEBUG_SSL > 1
  else
    log_legacy_write ("nsend[%d]: fd=%d\n", getpid (), fd);
#endif
  /* Trying OS's send() */
  block_socket (fd);            /* ??? */
  do
    {
      struct timeval tv = {
        0, 5
      };
      fd_set wr;
      int e;

      FD_ZERO (&wr);
      FD_SET (fd, &wr);

      errno = 0;
      e = select (fd + 1, NULL, &wr, NULL, &tv);
      if (e > 0)
        n = os_send (fd, data, length, i_opt);
      else if (e < 0 && errno == EINTR)
        continue;
      else
        break;
    }
  while (n <= 0 && errno == EINTR);
  if (n < 0)
    log_legacy_write ("[%d] nsend():send %s\n", getpid (), strerror (errno));
  return n;
}

int
nrecv (int fd, void *data, int length, int i_opt)
{
  int e;
#if DEBUG_SSL > 8
  log_legacy_write ("nrecv: fd=%d len=%d\n", fd, length);
#endif
  if (OPENVAS_STREAM (fd))
    {
      if (connections[fd - OPENVAS_FD_OFF].fd < 0)
        log_legacy_write ("OpenVAS file descriptor %d closed ?!\n", fd);
      else
        return read_stream_connection (fd, data, length);
    }
  /* Trying OS's recv()
   *
   * Do *NOT* use os_recv() here, as it will be blocking until the exact
   * amount of requested data arrives
   */
  block_socket (fd);
  do
    {
      e = recv (fd, data, length, i_opt);
    }
  while (e < 0 && errno == EINTR);
  return e;
}

int
close_stream_connection (int fd)
{
#if DEBUG_SSL > 2
  openvas_connection *fp;
  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }
  fp = &(connections[fd - OPENVAS_FD_OFF]);
  log_legacy_write ("close_stream_connection TCP:%d (fd=%d)\n", fp->port, fd);
#endif

  if (0)
    ;
  else if (!nasl_ssh_internal_close (fd))
    return 0;
  else if (!OPENVAS_STREAM (fd))     /* Will never happen if debug is on! */
    {
      if (fd < 0 || fd > 1024)
        {
          errno = EINVAL;
          return -1;
        }
      shutdown (fd, 2);
      return socket_close (fd);
    }
  else
    return release_connection_fd (fd);
}


int
get_encaps (int fd)
{
  if (!OPENVAS_STREAM (fd))
    {
      log_legacy_write ("get_encaps() : bad argument\n");
      return -1;
    }
  return connections[fd - OPENVAS_FD_OFF].transport;
}


const char *
get_encaps_name (int code)
{
  static char str[100];
  switch (code)
    {
    case OPENVAS_ENCAPS_AUTO:
      return "auto";
    case OPENVAS_ENCAPS_IP:
      return "IP";
    case OPENVAS_ENCAPS_SSLv2:
      return "SSLv2";
    case OPENVAS_ENCAPS_SSLv23:
      return "SSLv23";
    case OPENVAS_ENCAPS_SSLv3:
      return "SSLv3";
    case OPENVAS_ENCAPS_TLSv1:
      return "TLSv1";
    case OPENVAS_ENCAPS_TLScustom:
      return "TLScustom";
    default:
      snprintf (str, sizeof (str), "[unknown transport layer - code %d (0x%x)]", code, code);   /* RATS: ignore */
      return str;
    }
}

const char *
get_encaps_through (int code)
{
  static char str[100];
  switch (code)
    {
    case OPENVAS_ENCAPS_IP:
      return "";
    case OPENVAS_ENCAPS_SSLv2:
    case OPENVAS_ENCAPS_SSLv23:
    case OPENVAS_ENCAPS_SSLv3:
    case OPENVAS_ENCAPS_TLSv1:
      return " through SSL";
    default:
      snprintf (str, sizeof (str), " through unknown transport layer - code %d (0x%x)", code, code);    /* RATS: ignore */
      return str;
    }
}

static int
open_socket (struct sockaddr *paddr, int type, int protocol,
             int timeout, int len)
{
  fd_set fd_w;
  struct timeval to;
  int soc, x;
  int opt;
  unsigned int opt_sz;
  int family;

  __port_closed = 0;

  if (paddr->sa_family == AF_INET)
    {
      family = AF_INET;
      if ((soc = socket (AF_INET, type, protocol)) < 0)
        {
          pid_perror ("socket");
          return -1;
        }
    }
  else
    {
      family = AF_INET6;
      if ((soc = socket (AF_INET6, type, protocol)) < 0)
        {
          pid_perror ("socket");
          return -1;
        }
    }

  if (timeout == -2)
    timeout = TIMEOUT;

  if (timeout > 0)
    if (unblock_socket (soc) < 0)
      {
        close (soc);
        return -1;
      }

  set_socket_source_addr (soc, 0, family);

  if (connect (soc, paddr, len) < 0)
    {
#if DEBUG_SSL > 2
      pid_perror ("connect");
#endif
    again:
      switch (errno)
        {
        case EINPROGRESS:
        case EAGAIN:
          FD_ZERO (&fd_w);
          FD_SET (soc, &fd_w);
          to.tv_sec = timeout;
          to.tv_usec = 0;
          x = select (soc + 1, NULL, &fd_w, NULL, &to);
          if (x == 0)
            {
#if DEBUG_SSL > 2
              pid_perror ("connect->select: timeout");
#endif
              socket_close (soc);
              errno = ETIMEDOUT;
              return -1;
            }
          else if (x < 0)
            {
              if (errno == EINTR)
                {
                  errno = EAGAIN;
                  goto again;
                }
              pid_perror ("select");
              socket_close (soc);
              return -1;
            }

          opt = 0;
          opt_sz = sizeof (opt);
          if (getsockopt (soc, SOL_SOCKET, SO_ERROR, &opt, &opt_sz) < 0)
            {
              pid_perror ("getsockopt");
              socket_close (soc);
              return -1;
            }
          if (opt == 0)
            break;
#if DEBUG_SSL > 2
          errno = opt;
          pid_perror ("SO_ERROR");
#endif
          /* no break; go on */
        default:
          __port_closed = 1;
          socket_close (soc);
          return -1;
        }
    }
#if DEBUG_SSL > 2
  pid_notice ("connect succeeded");
#endif
  block_socket (soc);
  return soc;
}



int
open_sock_opt_hn (const char *hostname, unsigned int port, int type,
                  int protocol, int timeout)
{
  struct sockaddr_in addr;
  struct sockaddr_in6 addr6;
  struct in6_addr in6addr;

  nn_resolve (hostname, &in6addr);
  /*if (IN6_ARE_ADDR_EQUAL(&addr6, &in6addr_any))
     {
     log_legacy_write ("open_sock_opt_hn: invalid socket address\n");
     return  -1;
     } */
  if (IN6_IS_ADDR_V4MAPPED (&in6addr))
    {
      bzero ((void *) &addr, sizeof (addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons ((unsigned short) port);
      addr.sin_addr.s_addr = in6addr.s6_addr32[3];
      return open_socket ((struct sockaddr *) &addr, type, protocol,
                          timeout, sizeof (struct sockaddr_in));
    }
  else
    {
      bzero ((void *) &addr6, sizeof (addr6));
      addr6.sin6_family = AF_INET6;
      addr6.sin6_port = htons ((unsigned short) port);
      memcpy (&addr6.sin6_addr, &in6addr, sizeof (struct in6_addr));
      return open_socket ((struct sockaddr *) &addr6, type, protocol,
                          timeout, sizeof (struct sockaddr_in6));
    }

}


int
open_sock_tcp_hn (const char *hostname, unsigned int port)
{
  return open_sock_opt_hn (hostname, port, SOCK_STREAM, IPPROTO_TCP, TIMEOUT);
}


int
open_sock_tcp (struct arglist *args, unsigned int port, int timeout)
{
  char name[32];
  int ret;
  int type;

  /* If we timed out against this port in the past, there's no need
   * to scan it again */
  snprintf (name, sizeof (name), "/tmp/ConnectTimeout/TCP/%d", port);   /* RATS: ignore */
  if (plug_get_key (args, name, &type))
    return -1;


  errno = 0;
  ret = open_sock_option (args, port, SOCK_STREAM, IPPROTO_TCP, timeout);
  if (ret < 0 && errno == ETIMEDOUT)
    plug_set_key (args, name, ARG_INT, (void *) 1);

  return ret;
}


int
open_sock_udp (struct arglist *args, unsigned int port)
{
  return open_sock_option (args, port, SOCK_DGRAM, IPPROTO_UDP, 0);
}


struct in_addr
_socket_get_next_source_addr (struct in_addr *addr)
{
  static struct in_addr *src_addrs = NULL;
  static int current_src_addr = 0;
  static pid_t current_src_addr_pid = 0;
  static int num_addrs = 0;
  struct in_addr ret;
  pid_t mypid;

  if (current_src_addr < 0)
    {
      ret.s_addr = INADDR_ANY;
      return ret;
    }

  if (src_addrs == NULL && current_src_addr == 0)
    {
      src_addrs = addr;
      if (src_addrs == NULL)
        {
          ret.s_addr = INADDR_ANY;
          current_src_addr = -1;
          return ret;
        }

      num_addrs = -1;
      while (src_addrs[++num_addrs].s_addr != 0);
    }


  mypid = getpid ();
  if (current_src_addr_pid != mypid)
    {
      current_src_addr_pid = mypid;
      current_src_addr = lrand48 () % (num_addrs);      /* RATS: ignore */
      if (src_addrs[current_src_addr].s_addr == 0)
        current_src_addr = 0;
    }

  return src_addrs[current_src_addr];
}

struct in6_addr
_socket_get_next_source_v4_addr (struct in6_addr *addr)
{
  static struct in6_addr *src_addrs = NULL;
  static int current_src_addr = 0;
  static pid_t current_src_addr_pid = 0;
  static int num_addrs = 0;
  struct in6_addr ret;
  pid_t mypid;

  if (current_src_addr < 0)
    {
      ret = in6addr_any;
      return ret;
    }

  if (src_addrs == NULL && current_src_addr == 0)
    {
      src_addrs = addr;
      if (src_addrs == NULL)
        {
          ret = in6addr_any;
          current_src_addr = -1;
          return ret;
        }

      num_addrs = -1;
      while (src_addrs[++num_addrs].s6_addr32[3] != 0);
      if (num_addrs == 0)
        {
          ret = in6addr_any;
          current_src_addr = -1;
          return ret;
        }
    }


  mypid = getpid ();
  if (current_src_addr_pid != mypid)
    {
      current_src_addr_pid = mypid;
      current_src_addr = lrand48 () % (num_addrs);      /* RATS: ignore */
      if (src_addrs[current_src_addr].s6_addr32[3] == 0)
        current_src_addr = 0;
    }

  return src_addrs[current_src_addr];
}

struct in6_addr
_socket_get_next_source_v6_addr (struct in6_addr *addr)
{
  static struct in6_addr *src_addrs = NULL;
  static int current_src_addr = 0;
  static pid_t current_src_addr_pid = 0;
  static int num_addrs = 0;
  struct in6_addr ret;
  pid_t mypid;

  if (current_src_addr < 0)
    {
      ret = in6addr_any;
      return ret;
    }

  if (src_addrs == NULL && current_src_addr == 0)
    {
      src_addrs = addr;
      if (src_addrs == NULL)
        {
          ret = in6addr_any;
          current_src_addr = -1;
          return ret;
        }

      num_addrs = 0;
      while (!IN6_ARE_ADDR_EQUAL (&src_addrs[num_addrs], &in6addr_any))
        num_addrs++;
      if (num_addrs == 0)
        {
          ret = in6addr_any;
          current_src_addr = -1;
          return ret;
        }
    }


  mypid = getpid ();
  if (current_src_addr_pid != mypid)
    {
      current_src_addr_pid = mypid;
      current_src_addr = lrand48 () % (num_addrs);      /* RATS: ignore */
      if (IN6_ARE_ADDR_EQUAL (&src_addrs[current_src_addr], &in6addr_any))
        current_src_addr = 0;
    }

  return src_addrs[current_src_addr];
}

struct in_addr
socket_get_next_source_addr ()
{
  return _socket_get_next_source_addr (NULL);
}

struct in6_addr
socket_get_next_source_v4_addr ()
{
  return _socket_get_next_source_v4_addr (NULL);
}

struct in6_addr
socket_get_next_source_v6_addr ()
{
  return _socket_get_next_source_v6_addr (NULL);
}

int
set_socket_source_addr (int soc, int port, int family)
{
  struct sockaddr_in bnd;
  struct sockaddr_in6 bnd6;
  int opt = 1;

  struct in6_addr src;

  if (family == AF_INET)
    src = _socket_get_next_source_v4_addr (NULL);
  else
    src = _socket_get_next_source_v6_addr (NULL);
  if (IN6_ARE_ADDR_EQUAL (&src, &in6addr_any) && port == 0)     /* No need to bind() */
    return 0;

  setsockopt (soc, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof (int));

  if (family == AF_INET)
    {
      bzero (&bnd, sizeof (bnd));

      bnd.sin_port = htons (port);
      bnd.sin_addr.s_addr = src.s6_addr32[3];
      bnd.sin_family = AF_INET;

      if (bind (soc, (struct sockaddr *) &bnd, sizeof (bnd)) < 0)
        {
          return -1;
        }
    }
  else
    {
      bzero (&bnd6, sizeof (bnd6));

      bnd6.sin6_port = htons (port);
      bnd6.sin6_family = AF_INET6;
      memcpy (&bnd6.sin6_addr, &src, sizeof (struct in6_addr));

      if (bind (soc, (struct sockaddr *) &bnd6, sizeof (bnd6)) < 0)
        {
          return -1;
        }
    }

  return 0;
}

void
socket_source_init (struct in6_addr *addr, int family)
{
  if (family == AF_INET)
    (void) _socket_get_next_source_v4_addr (addr);
  else
    (void) _socket_get_next_source_v6_addr (addr);
}


int
open_sock_option (struct arglist *args, unsigned int port, int type,
                  int protocol, int timeout)
{
  struct sockaddr_in addr;
  struct sockaddr_in6 addr6;
  struct in6_addr *t;

#if 0
  /*
   * MA 2004-08-15: IMHO, as this is often (always?) tested in the NASL scripts
   * this should not be here.
   * If it has to be somewhere else, I'd rather put it in libnasl (and add
   * a parameter to "force" the connection)
   */
  if (host_get_port_state (args, port) <= 0)
    return (-1);
#endif
  t = plug_get_host_ip (args);
  if (!t)
    {
      log_legacy_write ("ERROR ! NO ADDRESS ASSOCIATED WITH NAME\n");
      arg_dump (args, 0);
      return (-1);
    }
  if (IN6_ARE_ADDR_EQUAL (t, &in6addr_any))
    return (-1);
  if (IN6_IS_ADDR_V4MAPPED (t))
    {
      bzero ((void *) &addr, sizeof (addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons ((unsigned short) port);
      addr.sin_addr.s_addr = t->s6_addr32[3];
      return open_socket ((struct sockaddr *) &addr, type, protocol,
                          timeout, sizeof (struct sockaddr_in));
    }
  else
    {
      bzero ((void *) &addr6, sizeof (addr6));
      addr6.sin6_family = AF_INET6;
      addr6.sin6_port = htons ((unsigned short) port);
      memcpy (&addr6.sin6_addr, t, sizeof (struct in6_addr));
      return open_socket ((struct sockaddr *) &addr6, type, protocol,
                          timeout, sizeof (struct sockaddr_in6));
    }

}


/**
 * @brief Reads a text from the socket stream into the argument buffer, always
 * @brief appending a '\\0' byte.
 *
 * @param buf  Buffer to read into.
 *
 * @return Number of bytes read, without the trailing '\\0'.
 */
ExtFunc int
recv_line (int soc, char *buf, size_t bufsiz)
{
  int n, ret = 0;

  /* Dirty SSL hack */
  if (OPENVAS_STREAM (soc))
    {
      buf[0] = '\0';

      do
        {
          n = read_stream_connection_min (soc, buf + ret, 1, 1);
          switch (n)
            {
            case -1:
              if (ret == 0)
                return -1;
              else
                return ret;
              break;

            case 0:
              return ret;
              break;

            default:
              ret++;
            }
        }
      while (buf[ret - 1] != '\0' && buf[ret - 1] != '\n' && ret < bufsiz);

      if (ret > 0)
        {
          if (buf[ret - 1] != '\0')
            {
              if (ret < bufsiz)
                buf[ret] = '\0';
              else
                buf[bufsiz - 1] = '\0';
            }
        }

      return ret;
    }
  else
    {
      fd_set rd;
      struct timeval tv;

      do
        {
          int e;
        again:
          errno = 0;
          FD_ZERO (&rd);
          FD_SET (soc, &rd);
          tv.tv_sec = 5;
          tv.tv_usec = 0;
          e = select (soc + 1, &rd, NULL, NULL, &tv);
          if (e < 0 && errno == EINTR)
            goto again;
          if (e > 0)
            {
              n = recv (soc, buf + ret, 1, 0);
              switch (n)
                {
                case -1:
                  if (errno == EINTR)
                    continue;
                  if (ret == 0)
                    return -1;
                  else
                    return ret;
                  break;
                case 0:
                  return ret;
                  break;
                default:
                  ret++;
                }
            }
          else
            break;
          tv.tv_sec = 1;
          tv.tv_usec = 0;
        }
      while (buf[ret - 1] != '\0' && buf[ret - 1] != '\n' && ret < bufsiz);

      if (ret > 0)
        {
          if (buf[ret - 1] != '\0')
            {
              if (ret < bufsiz)
                buf[ret] = '\0';
              else
                buf[bufsiz - 1] = '\0';
            }
        }
    }

  return ret;
}

int
socket_close (int soc)
{
  return close (soc);
}

/**
 * @brief Writes data to the global socket of the thread.
 */
void
auth_printf (struct arglist *globals, char *data, ...)
{
  va_list param;
  char buffer[1048576];

  bzero (buffer, sizeof (buffer));

  va_start (param, data);
  /* RATS: ignore */
  vsnprintf (buffer, sizeof (buffer) - 1, data, param);

  va_end (param);
  auth_send (globals, buffer);
}


void
auth_send (struct arglist *globals, char *data)
{
  int soc = GPOINTER_TO_SIZE (arg_get_value (globals, "global_socket"));
  int confirm = GPOINTER_TO_SIZE (arg_get_value (globals, "confirm"));
  int n = 0;
  int length;
  int sent = 0;

  if (soc < 0)
    return;

#ifndef OPENVASNT
  signal (SIGPIPE, _exit);
#endif
  length = strlen (data);
  while (sent < length)
    {
      n = nsend (soc, data + sent, length - sent, 0);
      if (n < 0)
        {
          if ((errno == ENOMEM)
#ifdef ENOBUFS
              || (errno == ENOBUFS)
#endif
            )
            n = 0;
          else
            {
              goto out;
            }
        }
      else
        sent += n;
    }

  if (confirm)
    {
      /* If confirm is set, then we are a son
       * trying to report some message to our busy
       * father. So we wait until he told us he
       * took care of it. */
      char n;
      read_stream_connection_min (soc, &n, 1, 1);
    }
out:
#ifndef OPENVASNT
  signal (SIGPIPE, SIG_IGN);
#else
  ;
#endif
}

/**
 * @brief Reads data from the global socket of the thread.
 */
char *
auth_gets (struct arglist *globals, char *buf, size_t bufsiz)
{
  int soc = GPOINTER_TO_SIZE (arg_get_value (globals, "global_socket"));
  int n;
  /* bzero(buf, bufsiz); */
  n = recv_line (soc, buf, bufsiz);
  if (n <= 0)
    return NULL;

  return (buf);
}


/*
 * Select() routines
 */

int
stream_zero (fd_set * set)
{
  FD_ZERO (set);
  return 0;
}

int
stream_set (int fd, fd_set * set)
{
  int soc = openvas_get_socket_from_connection (fd);
  if (soc >= 0)
    FD_SET (soc, set);
  return soc;
}

int
stream_isset (int fd, fd_set * set)
{
  return FD_ISSET (openvas_get_socket_from_connection (fd), set);
}

int
fd_is_stream (int fd)
{
  return OPENVAS_STREAM (fd);   /* Should probably be smarter... */
}


int
stream_get_buffer_sz (int fd)
{
  openvas_connection *p;
  if (!OPENVAS_STREAM (fd))
    return -1;
  p = &(connections[fd - OPENVAS_FD_OFF]);
  return p->bufsz;
}


int
stream_set_buffer (int fd, int sz)
{
  openvas_connection *p;
  char *b;

  if (!OPENVAS_STREAM (fd))
    return -1;

  p = &(connections[fd - OPENVAS_FD_OFF]);
  if (sz < p->bufcnt)
    return -1;                  /* Do not want to lose data */

  if (sz == 0)
    {
      efree (&p->buf);
      p->bufsz = 0;
      return 0;
    }
  else if (p->buf == 0)
    {
      p->buf = malloc (sz);
      if (p->buf == NULL)
        return -1;
      p->bufsz = sz;
      p->bufptr = 0;
      p->bufcnt = 0;
      return 0;
    }
  else
    {
      if (p->bufcnt > 0)
        {
          memmove (p->buf, p->buf + p->bufptr, p->bufcnt);
          p->bufptr = 0;
        }
      b = realloc (p->buf, sz);
      if (b == NULL)
        return -1;
      p->bufsz = sz;
      return 0;
    }
 /*NOTREACHED*/}



/*------------------------------------------------------------------*/


int
os_send (int soc, void *buf, int len, int opt)
{
  char *buf0 = (char *) buf;
  int e, n;
  for (n = 0; n < len;)
    {
      errno = 0;
      e = send (soc, buf0 + n, len - n, opt);
      if (e < 0 && errno == EINTR)
        continue;
      else if (e <= 0)
        return -1;
      else
        n += e;
    }
  return n;
}

int
os_recv (int soc, void *buf, int len, int opt)
{
  char *buf0 = (char *) buf;
  int e, n;
  for (n = 0; n < len;)
    {
      errno = 0;
      e = recv (soc, buf0 + n, len - n, opt);
      if (e < 0 && errno == EINTR)
        continue;
      else if (e <= 0)
        return -1;
      else
        n += e;
    }
  return n;
}

/** @todo internal_send and internal_recv. could make an own module (ipc), in
 *        which the type constants could be moved, too. */

/**
 * internal_send() / internal_recv() :
 *
 * When processes are passing messages to each other, the format is
 * <length><msg>, with <length> being a long integer. The functions
 * internal_send() and internal_recv() encapsulate and decapsulate
 * the messages themselves.
 */
int
internal_send (int soc, char *data, int msg_type)
{
  int len;
  int e;
  int ack;

  if (data == NULL)
    data = "";

  e = os_send (soc, &msg_type, sizeof (len), 0);
  if (e < 0)
    return -1;

  if ((msg_type & INTERNAL_COMM_MSG_TYPE_CTRL) == 0)
    {
      len = strlen (data);

      e = os_send (soc, &len, sizeof (len), 0);
      if (e < 0)
        return -1;
      e = os_send (soc, data, len, 0);
      if (e < 0)
        return -1;
    }

  e = os_recv (soc, &ack, sizeof (ack), 0);
  if (e < 0)
    {
      log_legacy_write ("internal_send->os_recv(%d): %s\n", soc,
                        strerror (errno));
      return -1;
    }

  return 0;
}

/**
 * internal_send() / internal_recv() :
 *
 * When processes are passing messages to each other, the format is
 * <length><msg>, with <length> being a long integer. The functions
 * internal_send() and internal_recv() encapsulate and decapsulate
 * the messages themselves.
 */
int
internal_recv (int soc, char **data, int *data_sz, int *msg_type)
{
  int len = 0;
  int e;
  char *buf = *data;
  int sz = *data_sz;
  int type;
  int ack;

  if (buf == NULL)
    {
      sz = 65535;
      buf = emalloc (sz);
    }


  e = os_recv (soc, &type, sizeof (type), 0);
  if (e < 0)
    goto error;

  if ((type & INTERNAL_COMM_MSG_TYPE_CTRL) == 0)
    {
      e = os_recv (soc, &len, sizeof (len), 0);
      if (e < 0)
        goto error;

      if (len >= sz)
        {
          sz = len + 1;
          buf = erealloc (buf, sz);
        }

      if (len > 0)
        {
          e = os_recv (soc, buf, len, 0);
          if (e < 0)
            goto error;
          buf[len] = '\0';
        }

      if (data != NULL)
        *data = buf;
      if (data_sz != NULL)
        *data_sz = sz;
    }

  *msg_type = type;
  ack = INTERNAL_COMM_MSG_TYPE_CTRL | INTERNAL_COMM_CTRL_ACK;
  e = os_send (soc, &ack, sizeof (ack), 0);
  if (e < 0)
    goto error;


  return len;
error:
  efree (&buf);
  *data = NULL;
  *data_sz = 0;
  return -1;
}


int
stream_pending (int fd)
{
  openvas_connection *fp;
  if (!OPENVAS_STREAM (fd))
    {
      errno = EINVAL;
      return -1;
    }
  fp = &(connections[fd - OPENVAS_FD_OFF]);

  if (fp->bufcnt)
    return fp->bufcnt;
  else if (fp->transport != OPENVAS_ENCAPS_IP)
    return gnutls_record_check_pending (fp->tls_session);
  return 0;
}


/* This is a helper function for nasl_get_sock_info.  It is used to
   retrieve information about SOCK.  */
int
get_sock_infos (int sock, int *r_transport, void **r_tls_session)
{
  openvas_connection *fp;

  if (!OPENVAS_STREAM (sock))
    return ENOTSOCK;
  fp = &(connections[sock - OPENVAS_FD_OFF]);

  *r_transport = fp->transport;
  *r_tls_session = fp->tls_session;
  return 0;
}

/**
 * @brief Validate a port range string.
 *
 * Accepts ranges in form of "103,U:200-1024,3000-4000,T:3-4,U:7".
 *
 * @param[in]   port_range  A port range.
 *
 * @return 0 success, 1 failed.
 */
int
validate_port_range (const char* port_range)
{
  gchar **split, **point, *range, *range_start;

  if (!port_range)
    return 1;

  while (*port_range && isblank (*port_range)) port_range++;
  if (*port_range == '\0')
    return 1;

  /* Treat newlines like commas. */
  range = range_start = g_strdup (port_range);
  while (*range)
    {
      if (*range == '\n') *range = ',';
      range++;
    }

  split = g_strsplit (range_start, ",", 0);
  g_free (range_start);
  point = split;

  while (*point)
    {
      gchar *hyphen, *element;

      /* Strip off any outer whitespace. */

      element = g_strstrip (*point);

      /* Strip off any leading type specifier. */

      if ((strlen (element) >= 2)
          && ((element[0] == 'T') || (element[0] == 'U'))
          && (element[1] == ':'))
        element = element + 2;

      /* Look for a hyphen. */

      hyphen = strchr (element, '-');
      if (hyphen)
        {
          long int number1, number2;
          const char *first;
          char *end;

          hyphen++;

          /* Check the first number. */

          first = element;
          while (*first && isblank (*first)) first++;
          if (*first == '-')
            goto fail;

          errno = 0;
          number1 = strtol (first, &end, 10);
          while (*end && isblank (*end)) end++;
          if (errno || (*end != '-'))
            goto fail;
          if (number1 == 0)
            goto fail;
          if (number1 > 65535)
            goto fail;

          /* Check the second number. */

          while (*hyphen && isblank (*hyphen)) hyphen++;
          if (*hyphen == '\0')
            goto fail;

          errno = 0;
          number2 = strtol (hyphen, &end, 10);
          while (*end && isblank (*end)) end++;
          if (errno || *end)
            goto fail;
          if (number2 == 0)
            goto fail;
          if (number2 > 65535)
            goto fail;

          if (number1 > number2)
            goto fail;
        }
      else
        {
          long int number;
          const char *only;
          char *end;

          /* Check the single number. */

          only = element;
          while (*only && isblank (*only)) only++;
          /* Empty ranges are OK. */
          if (*only)
            {
              errno = 0;
              number = strtol (only, &end, 10);
              while (*end && isblank (*end)) end++;
              if (errno || *end)
                goto fail;
              if (number == 0)
                goto fail;
              if (number > 65535)
                goto fail;
            }
        }
      point += 1;
    }

  g_strfreev (split);
  return 0;

 fail:
  g_strfreev (split);
  return 1;
}

/**
 * @brief Create a range array from a port_range string.
 *
 * @param[out]  port_range  Valid port_range string.
 *
 * @return Range array.
 */
array_t*
port_range_ranges (const char *port_range)
{
  gchar **split, **point, *range_start, *current;
  array_t *ranges;
  int tcp;

  ranges = make_array ();

  while (*port_range && isblank (*port_range)) port_range++;

  /* Accepts T: and U: before any of the ranges.  This toggles the remaining
   * ranges, as in nmap.  Treats a leading naked range as TCP, whereas nmap
   * treats it as TCP and UDP. */

  /* Treat newlines like commas. */
  range_start = current = g_strdup (port_range);
  while (*current)
    {
      if (*current == '\n') *current = ',';
      current++;
    }

  tcp = 1;
  split = g_strsplit (range_start, ",", 0);
  g_free (range_start);
  point = split;

  while (*point)
    {
      gchar *hyphen, *element;
      range_t *range;

      element = g_strstrip (*point);
      if (strlen (element) >= 2)
        {
          if ((element[0] == 'T') && (element[1] == ':'))
            {
              tcp = 1;
              element = element + 2;
            }
          else if ((element[0] == 'U') && (element[1] == ':'))
            {
              tcp = 0;
              element = element + 2;
            }
          /* Else tcp stays as it is. */
        }

      /* Skip any space that followed the type specifier. */
      while (*element && isblank (*element)) element++;

      hyphen = strchr (element, '-');
      if (hyphen)
        {
          *hyphen = '\0';
          hyphen++;
          while (*hyphen && isblank (*hyphen)) hyphen++;
          assert (*hyphen);  /* Validation checks this. */

          /* A range. */

          range = (range_t*) g_malloc0 (sizeof (range_t));

          range->start = atoi (element);
          range->end = atoi (hyphen);
          range->type = tcp ? PORT_PROTOCOL_TCP : PORT_PROTOCOL_UDP;
          range->exclude = 0;

          array_add (ranges, range);
        }
      else if (*element)
        {
          /* A single port. */

          range = (range_t*) g_malloc0 (sizeof (range_t));

          range->start = atoi (element);
          range->end = range->start;
          range->type = tcp ? PORT_PROTOCOL_TCP : PORT_PROTOCOL_UDP;
          range->exclude = 0;

          array_add (ranges, range);
        }
      /* Else skip over empty range. */
      point += 1;
    }
  g_strfreev (split);
  return ranges;
}


/* GnuTLS 2.11.1 changed the semantics of set_lowat and 2.99.0 removed
   that function.  As a quick workaround we set it back to the old
   default.  gcc 4.4 has no diagnostic push pragma, thus we better put
   this function at the end of the file.  */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static void
my_gnutls_transport_set_lowat_default (gnutls_session_t session)
{
#if GNUTLS_VERSION_NUMBER >= 0x020b01 && GNUTLS_VERSION_NUMBER < 0x026300
  gnutls_transport_set_lowat (session, 1);
#endif
}
