/*
     This file is part of libmicrohttpd
     Copyright (C) 2007 Christian Grothoff
     Copyright (C) 2015-2021 Karlson2k (Evgeny Grin)

     libmicrohttpd is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
     option) any later version.

     libmicrohttpd is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with libmicrohttpd; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/

/**
 * @file daemontest_get_chunked.c
 * @brief  Testcase for libmicrohttpd GET operations with chunked content encoding
 * @author Christian Grothoff
 * @author Karlson2k (Evgeny Grin)
 */

#include "MHD_config.h"
#include "platform.h"
#include <curl/curl.h>
#include <microhttpd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef WINDOWS
#include <unistd.h>
#endif

#include "mhd_has_in_name.h"

#if defined(MHD_CPU_COUNT) && (MHD_CPU_COUNT + 0) < 2
#undef MHD_CPU_COUNT
#endif
#if ! defined(MHD_CPU_COUNT)
#define MHD_CPU_COUNT 2
#endif

#define HDR_CHUNKED_ENCODING MHD_HTTP_HEADER_TRANSFER_ENCODING ": chunked"
#define RESP_FOOTER_NAME "Footer"
#define RESP_FOOTER_VALUE "working"
#define RESP_FOOTER RESP_FOOTER_NAME ": " RESP_FOOTER_VALUE

/**
 * Use "Connection: close" header?
 */
int conn_close;

struct headers_check_result
{
  int found_chunked;
  int found_footer;
};

size_t
lcurl_hdr_callback (char *buffer, size_t size, size_t nitems,
                    void *userdata)
{
  const size_t data_size = size * nitems;
  struct headers_check_result *check_res =
    (struct headers_check_result *) userdata;

  if ((data_size == strlen (HDR_CHUNKED_ENCODING) + 2) &&
      (0 == memcmp (buffer, HDR_CHUNKED_ENCODING "\r\n", data_size)))
    check_res->found_chunked = 1;
  if ((data_size == strlen (RESP_FOOTER) + 2) &&
      (0 == memcmp (buffer, RESP_FOOTER "\r\n", data_size)))
    check_res->found_footer = 1;

  return data_size;
}


struct CBC
{
  char *buf;
  size_t pos;
  size_t size;
};


static size_t
copyBuffer (void *ptr,
            size_t size,
            size_t nmemb,
            void *ctx)
{
  struct CBC *cbc = ctx;

  if (cbc->pos + size * nmemb > cbc->size)
    return 0;                   /* overflow */
  memcpy (&cbc->buf[cbc->pos], ptr, size * nmemb);
  cbc->pos += size * nmemb;
  return size * nmemb;
}


/**
 * MHD content reader callback that returns data in chunks.
 */
static ssize_t
crc (void *cls,
     uint64_t pos,
     char *buf,
     size_t max)
{
  struct MHD_Response **responseptr = cls;

  if (pos == 128 * 10)
  {
    MHD_add_response_footer (*responseptr,
                             RESP_FOOTER_NAME,
                             RESP_FOOTER_VALUE);
    return MHD_CONTENT_READER_END_OF_STREAM;
  }
  if (max < 128)
    abort ();                   /* should not happen in this testcase... */
  memset (buf, 'A' + (pos / 128), 128);
  return 128;
}


/**
 * Dummy function that frees the "responseptr".
 */
static void
crcf (void *ptr)
{
  free (ptr);
}


static enum MHD_Result
ahc_echo (void *cls,
          struct MHD_Connection *connection,
          const char *url,
          const char *method,
          const char *version,
          const char *upload_data, size_t *upload_data_size, void **ptr)
{
  static int aptr;
  const char *me = cls;
  struct MHD_Response *response;
  struct MHD_Response **responseptr;
  enum MHD_Result ret;

  (void) url;
  (void) version;              /* Unused. Silent compiler warning. */
  (void) upload_data;
  (void) upload_data_size;     /* Unused. Silent compiler warning. */

  if (0 != strcmp (me, method))
    return MHD_NO;              /* unexpected method */
  if (&aptr != *ptr)
  {
    /* do never respond on first call */
    *ptr = &aptr;
    return MHD_YES;
  }
  responseptr = malloc (sizeof (struct MHD_Response *));
  if (NULL == responseptr)
    _exit (99);
  response = MHD_create_response_from_callback (MHD_SIZE_UNKNOWN,
                                                1024,
                                                &crc,
                                                responseptr,
                                                &crcf);
  if (NULL == response)
    abort ();
  if (conn_close)
  { /* Enforce chunked response even for non-Keep-Alive */
    if (MHD_NO == MHD_add_response_header (response,
                                           MHD_HTTP_HEADER_TRANSFER_ENCODING,
                                           "chunked"))
      abort ();
  }

  *responseptr = response;
  ret = MHD_queue_response (connection,
                            MHD_HTTP_OK,
                            response);
  MHD_destroy_response (response);
  return ret;
}


static int
validate (struct CBC cbc, int ebase)
{
  int i;
  char buf[128];

  if (cbc.pos != 128 * 10)
  {
    fprintf (stderr,
             "Got %u bytes instead of 1280!\n",
             (unsigned int) cbc.pos);
    return ebase;
  }

  for (i = 0; i < 10; i++)
  {
    memset (buf, 'A' + i, 128);
    if (0 != memcmp (buf, &cbc.buf[i * 128], 128))
    {
      fprintf (stderr,
               "Got  `%.*s'\nWant `%.*s'\n",
               128, buf, 128, &cbc.buf[i * 128]);
      return ebase * 2;
    }
  }
  return 0;
}


static int
testInternalGet ()
{
  struct MHD_Daemon *d;
  CURL *c;
  char buf[2048];
  struct CBC cbc;
  CURLcode errornum;
  int port;
  struct curl_slist *h_list = NULL;
  struct headers_check_result hdr_check;

  if (MHD_NO != MHD_is_feature_supported (MHD_FEATURE_AUTODETECT_BIND_PORT))
    port = 0;
  else
    port = 1170;

  cbc.buf = buf;
  cbc.size = 2048;
  cbc.pos = 0;
  d = MHD_start_daemon (MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
                        port, NULL, NULL, &ahc_echo, "GET", MHD_OPTION_END);
  if (d == NULL)
    return 1;
  if (0 == port)
  {
    const union MHD_DaemonInfo *dinfo;
    dinfo = MHD_get_daemon_info (d, MHD_DAEMON_INFO_BIND_PORT);
    if ((NULL == dinfo) || (0 == dinfo->port) )
    {
      MHD_stop_daemon (d); return 32;
    }
    port = (int) dinfo->port;
  }
  hdr_check.found_chunked = 0;
  hdr_check.found_footer = 0;
  c = curl_easy_init ();
  curl_easy_setopt (c, CURLOPT_URL, "http://127.0.0.1/hello_world");
  curl_easy_setopt (c, CURLOPT_PORT, (long) port);
  curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, &copyBuffer);
  curl_easy_setopt (c, CURLOPT_WRITEDATA, &cbc);
  curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt (c, CURLOPT_TIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt (c, CURLOPT_HEADERFUNCTION, lcurl_hdr_callback);
  curl_easy_setopt (c, CURLOPT_HEADERDATA, &hdr_check);
  curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
  if (conn_close)
  {
    h_list = curl_slist_append (h_list, "Connection: close");
    if (NULL == h_list)
      abort ();
    curl_easy_setopt (c, CURLOPT_HTTPHEADER, h_list);
  }
  if (CURLE_OK != (errornum = curl_easy_perform (c)))
  {
    fprintf (stderr,
             "curl_easy_perform failed: `%s'\n",
             curl_easy_strerror (errornum));
    curl_easy_cleanup (c);
    curl_slist_free_all (h_list);
    MHD_stop_daemon (d);
    return 2;
  }
  curl_easy_cleanup (c);
  curl_slist_free_all (h_list);
  MHD_stop_daemon (d);
  if (1 != hdr_check.found_chunked)
  {
    fprintf (stderr,
             "Chunked encoding header was not found in the response\n");
    return 8;
  }
  if (1 != hdr_check.found_footer)
  {
    fprintf (stderr,
             "The specified footer was not found in the response\n");
    return 16;
  }
  return validate (cbc, 4);
}


static int
testMultithreadedGet ()
{
  struct MHD_Daemon *d;
  CURL *c;
  char buf[2048];
  struct CBC cbc;
  CURLcode errornum;
  int port;
  struct curl_slist *h_list = NULL;
  struct headers_check_result hdr_check;

  if (MHD_NO != MHD_is_feature_supported (MHD_FEATURE_AUTODETECT_BIND_PORT))
    port = 0;
  else
    port = 1171;

  cbc.buf = buf;
  cbc.size = 2048;
  cbc.pos = 0;
  d = MHD_start_daemon (MHD_USE_THREAD_PER_CONNECTION
                        | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
                        port, NULL, NULL, &ahc_echo, "GET", MHD_OPTION_END);
  if (d == NULL)
    return 16;
  if (0 == port)
  {
    const union MHD_DaemonInfo *dinfo;
    dinfo = MHD_get_daemon_info (d, MHD_DAEMON_INFO_BIND_PORT);
    if ((NULL == dinfo) || (0 == dinfo->port) )
    {
      MHD_stop_daemon (d); return 32;
    }
    port = (int) dinfo->port;
  }
  c = curl_easy_init ();
  curl_easy_setopt (c, CURLOPT_URL, "http://127.0.0.1/hello_world");
  curl_easy_setopt (c, CURLOPT_PORT, (long) port);
  curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, &copyBuffer);
  curl_easy_setopt (c, CURLOPT_WRITEDATA, &cbc);
  curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt (c, CURLOPT_TIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
  hdr_check.found_chunked = 0;
  hdr_check.found_footer = 0;
  curl_easy_setopt (c, CURLOPT_HEADERFUNCTION, lcurl_hdr_callback);
  curl_easy_setopt (c, CURLOPT_HEADERDATA, &hdr_check);
  if (conn_close)
  {
    h_list = curl_slist_append (h_list, "Connection: close");
    if (NULL == h_list)
      abort ();
    curl_easy_setopt (c, CURLOPT_HTTPHEADER, h_list);
  }
  if (CURLE_OK != (errornum = curl_easy_perform (c)))
  {
    fprintf (stderr,
             "curl_easy_perform failed: `%s'\n",
             curl_easy_strerror (errornum));
    curl_easy_cleanup (c);
    curl_slist_free_all (h_list);
    MHD_stop_daemon (d);
    return 32;
  }
  curl_easy_cleanup (c);
  curl_slist_free_all (h_list);
  MHD_stop_daemon (d);
  if (1 != hdr_check.found_chunked)
  {
    fprintf (stderr,
             "Chunked encoding header was not found in the response\n");
    return 8;
  }
  if (1 != hdr_check.found_footer)
  {
    fprintf (stderr,
             "The specified footer was not found in the response\n");
    return 16;
  }
  return validate (cbc, 64);
}


static int
testMultithreadedPoolGet ()
{
  struct MHD_Daemon *d;
  CURL *c;
  char buf[2048];
  struct CBC cbc;
  CURLcode errornum;
  int port;
  struct curl_slist *h_list = NULL;
  struct headers_check_result hdr_check;

  if (MHD_NO != MHD_is_feature_supported (MHD_FEATURE_AUTODETECT_BIND_PORT))
    port = 0;
  else
    port = 1172;

  cbc.buf = buf;
  cbc.size = 2048;
  cbc.pos = 0;
  d = MHD_start_daemon (MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
                        port, NULL, NULL, &ahc_echo, "GET",
                        MHD_OPTION_THREAD_POOL_SIZE, MHD_CPU_COUNT,
                        MHD_OPTION_END);
  if (d == NULL)
    return 16;
  if (0 == port)
  {
    const union MHD_DaemonInfo *dinfo;
    dinfo = MHD_get_daemon_info (d, MHD_DAEMON_INFO_BIND_PORT);
    if ((NULL == dinfo) || (0 == dinfo->port) )
    {
      MHD_stop_daemon (d); return 32;
    }
    port = (int) dinfo->port;
  }
  c = curl_easy_init ();
  curl_easy_setopt (c, CURLOPT_URL, "http://127.0.0.1/hello_world");
  curl_easy_setopt (c, CURLOPT_PORT, (long) port);
  curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, &copyBuffer);
  curl_easy_setopt (c, CURLOPT_WRITEDATA, &cbc);
  curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt (c, CURLOPT_TIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
  hdr_check.found_chunked = 0;
  hdr_check.found_footer = 0;
  curl_easy_setopt (c, CURLOPT_HEADERFUNCTION, lcurl_hdr_callback);
  curl_easy_setopt (c, CURLOPT_HEADERDATA, &hdr_check);
  if (conn_close)
  {
    h_list = curl_slist_append (h_list, "Connection: close");
    if (NULL == h_list)
      abort ();
    curl_easy_setopt (c, CURLOPT_HTTPHEADER, h_list);
  }
  if (CURLE_OK != (errornum = curl_easy_perform (c)))
  {
    fprintf (stderr,
             "curl_easy_perform failed: `%s'\n",
             curl_easy_strerror (errornum));
    curl_easy_cleanup (c);
    curl_slist_free_all (h_list);
    MHD_stop_daemon (d);
    return 32;
  }
  curl_easy_cleanup (c);
  curl_slist_free_all (h_list);
  MHD_stop_daemon (d);
  if (1 != hdr_check.found_chunked)
  {
    fprintf (stderr,
             "Chunked encoding header was not found in the response\n");
    return 8;
  }
  if (1 != hdr_check.found_footer)
  {
    fprintf (stderr,
             "The specified footer was not found in the response\n");
    return 16;
  }
  return validate (cbc, 64);
}


static int
testExternalGet ()
{
  struct MHD_Daemon *d;
  CURL *c;
  char buf[2048];
  struct CBC cbc;
  CURLM *multi;
  CURLMcode mret;
  fd_set rs;
  fd_set ws;
  fd_set es;
  MHD_socket maxsock;
#ifdef MHD_WINSOCK_SOCKETS
  int maxposixs; /* Max socket number unused on W32 */
#else  /* MHD_POSIX_SOCKETS */
#define maxposixs maxsock
#endif /* MHD_POSIX_SOCKETS */
  int running;
  struct CURLMsg *msg;
  time_t start;
  struct timeval tv;
  int port;
  struct curl_slist *h_list = NULL;
  struct headers_check_result hdr_check;

  if (MHD_NO != MHD_is_feature_supported (MHD_FEATURE_AUTODETECT_BIND_PORT))
    port = 0;
  else
    port = 1173;

  multi = NULL;
  cbc.buf = buf;
  cbc.size = 2048;
  cbc.pos = 0;
  d = MHD_start_daemon (MHD_USE_ERROR_LOG,
                        port, NULL, NULL, &ahc_echo, "GET", MHD_OPTION_END);
  if (d == NULL)
    return 256;
  if (0 == port)
  {
    const union MHD_DaemonInfo *dinfo;
    dinfo = MHD_get_daemon_info (d, MHD_DAEMON_INFO_BIND_PORT);
    if ((NULL == dinfo) || (0 == dinfo->port) )
    {
      MHD_stop_daemon (d); return 32;
    }
    port = (int) dinfo->port;
  }
  c = curl_easy_init ();
  curl_easy_setopt (c, CURLOPT_URL, "http://127.0.0.1/hello_world");
  curl_easy_setopt (c, CURLOPT_PORT, (long) port);
  curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, &copyBuffer);
  curl_easy_setopt (c, CURLOPT_WRITEDATA, &cbc);
  curl_easy_setopt (c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt (c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt (c, CURLOPT_TIMEOUT, 150L);
  curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
  hdr_check.found_chunked = 0;
  hdr_check.found_footer = 0;
  curl_easy_setopt (c, CURLOPT_HEADERFUNCTION, lcurl_hdr_callback);
  curl_easy_setopt (c, CURLOPT_HEADERDATA, &hdr_check);
  if (conn_close)
  {
    h_list = curl_slist_append (h_list, "Connection: close");
    if (NULL == h_list)
      abort ();
    curl_easy_setopt (c, CURLOPT_HTTPHEADER, h_list);
  }

  multi = curl_multi_init ();
  if (multi == NULL)
  {
    curl_easy_cleanup (c);
    curl_slist_free_all (h_list);
    MHD_stop_daemon (d);
    return 512;
  }
  mret = curl_multi_add_handle (multi, c);
  if (mret != CURLM_OK)
  {
    curl_multi_cleanup (multi);
    curl_easy_cleanup (c);
    curl_slist_free_all (h_list);
    MHD_stop_daemon (d);
    return 1024;
  }
  start = time (NULL);
  while ((time (NULL) - start < 5) && (multi != NULL))
  {
    maxsock = MHD_INVALID_SOCKET;
    maxposixs = -1;
    FD_ZERO (&rs);
    FD_ZERO (&ws);
    FD_ZERO (&es);
    curl_multi_perform (multi, &running);
    mret = curl_multi_fdset (multi, &rs, &ws, &es, &maxposixs);
    if (mret != CURLM_OK)
    {
      curl_multi_remove_handle (multi, c);
      curl_multi_cleanup (multi);
      curl_easy_cleanup (c);
      curl_slist_free_all (h_list);
      MHD_stop_daemon (d);
      return 2048;
    }
    if (MHD_YES != MHD_get_fdset (d, &rs, &ws, &es, &maxsock))
    {
      curl_multi_remove_handle (multi, c);
      curl_multi_cleanup (multi);
      curl_easy_cleanup (c);
      curl_slist_free_all (h_list);
      MHD_stop_daemon (d);
      return 4096;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    if (-1 == select (maxposixs + 1, &rs, &ws, &es, &tv))
    {
#ifdef MHD_POSIX_SOCKETS
      if (EINTR != errno)
        abort ();
#else
      if ((WSAEINVAL != WSAGetLastError ()) || (0 != rs.fd_count) || (0 !=
                                                                      ws.
                                                                      fd_count)
          || (0 != es.fd_count) )
        abort ();
      Sleep (1000);
#endif
    }
    curl_multi_perform (multi, &running);
    if (running == 0)
    {
      msg = curl_multi_info_read (multi, &running);
      if (msg == NULL)
        break;
      if (msg->msg == CURLMSG_DONE)
      {
        if (msg->data.result != CURLE_OK)
          printf ("%s failed at %s:%d: `%s'\n",
                  "curl_multi_perform",
                  __FILE__,
                  __LINE__, curl_easy_strerror (msg->data.result));
        curl_multi_remove_handle (multi, c);
        curl_multi_cleanup (multi);
        curl_easy_cleanup (c);
        c = NULL;
        multi = NULL;
      }
    }
    MHD_run (d);
  }
  if (multi != NULL)
  {
    curl_multi_remove_handle (multi, c);
    curl_easy_cleanup (c);
    curl_multi_cleanup (multi);
    curl_slist_free_all (h_list);
  }
  MHD_stop_daemon (d);
  if (1 != hdr_check.found_chunked)
  {
    fprintf (stderr,
             "Chunked encoding header was not found in the response\n");
    return 8;
  }
  if (1 != hdr_check.found_footer)
  {
    fprintf (stderr,
             "The specified footer was not found in the response\n");
    return 16;
  }
  return validate (cbc, 8192);
}


int
main (int argc, char *const *argv)
{
  unsigned int errorCount = 0;
  (void) argc; (void) argv; /* Unused. Silent compiler warning. */

  if (0 != curl_global_init (CURL_GLOBAL_WIN32))
    return 2;
  conn_close = has_in_name (argv[0], "_close");
  if (MHD_YES == MHD_is_feature_supported (MHD_FEATURE_THREADS))
  {
    errorCount += testInternalGet ();
    errorCount += testMultithreadedGet ();
    errorCount += testMultithreadedPoolGet ();
  }
  errorCount += testExternalGet ();
  if (errorCount != 0)
    fprintf (stderr, "Error (code: %u)\n", errorCount);
  curl_global_cleanup ();
  return errorCount != 0;       /* 0 == pass */
}
