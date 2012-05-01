/*
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MIT
 *
 * Portions created by VMware are Copyright (c) 2007-2012 VMware, Inc.
 * All Rights Reserved.
 *
 * Portions created by Tony Garnock-Jones are Copyright (c) 2009-2010
 * VMware, Inc. and Tony Garnock-Jones. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ***** END LICENSE BLOCK *****
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "amqp.h"
#include "amqp_framing.h"
#include "amqp_private.h"

#include <assert.h>

static const char *client_error_strings[ERROR_MAX] = {
  "could not allocate memory", /* ERROR_NO_MEMORY */
  "received bad AMQP data", /* ERROR_BAD_AQMP_DATA */
  "unknown AMQP class id", /* ERROR_UNKOWN_CLASS */
  "unknown AMQP method id", /* ERROR_UNKOWN_METHOD */
  "unknown host", /* ERROR_GETHOSTBYNAME_FAILED */
  "incompatible AMQP version", /* ERROR_INCOMPATIBLE_AMQP_VERSION */
  "connection closed unexpectedly", /* ERROR_CONNECTION_CLOSED */
  "could not parse AMQP URL", /* ERROR_BAD_AMQP_URL */
};

char *amqp_error_string(int err)
{
  const char *str;
  int category = (err & ERROR_CATEGORY_MASK);
  err = (err & ~ERROR_CATEGORY_MASK);

  switch (category) {
  case ERROR_CATEGORY_CLIENT:
    if (err < 1 || err > ERROR_MAX)
      str = "(undefined librabbitmq error)";
    else
      str = client_error_strings[err - 1];
    break;

  case ERROR_CATEGORY_OS:
    return amqp_os_error_string(err);

  default:
    str = "(undefined error category)";
  }

  return strdup(str);
}

void amqp_abort(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	abort();
}

const amqp_bytes_t amqp_empty_bytes = { 0, NULL };
const amqp_table_t amqp_empty_table = { 0, NULL };
const amqp_array_t amqp_empty_array = { 0, NULL };

#define RPC_REPLY(replytype)						\
  (state->most_recent_api_result.reply_type == AMQP_RESPONSE_NORMAL	\
   ? (replytype *) state->most_recent_api_result.reply.decoded		\
   : NULL)

int amqp_basic_publish(amqp_connection_state_t state,
		       amqp_channel_t channel,
		       amqp_bytes_t exchange,
		       amqp_bytes_t routing_key,
		       amqp_boolean_t mandatory,
		       amqp_boolean_t immediate,
		       amqp_basic_properties_t const *properties,
		       amqp_bytes_t body)
{
  amqp_frame_t f;
  size_t body_offset;
  size_t usable_body_payload_size = state->frame_max - (HEADER_SIZE + FOOTER_SIZE);
  int res;

  amqp_basic_publish_t m;
  amqp_basic_properties_t default_properties;

  m.exchange = exchange;
  m.routing_key = routing_key;
  m.mandatory = mandatory;
  m.immediate = immediate;

  res = amqp_send_method(state, channel, AMQP_BASIC_PUBLISH_METHOD, &m);
  if (res < 0)
    return res;

  if (properties == NULL) {
    memset(&default_properties, 0, sizeof(default_properties));
    properties = &default_properties;
  }

  f.frame_type = AMQP_FRAME_HEADER;
  f.channel = channel;
  f.payload.properties.class_id = AMQP_BASIC_CLASS;
  f.payload.properties.body_size = body.len;
  f.payload.properties.decoded = (void *) properties;

  res = amqp_send_frame(state, &f);
  if (res < 0)
    return res;

  body_offset = 0;
  while (1) {
    int remaining = body.len - body_offset;
    assert(remaining >= 0);

    if (remaining == 0)
      break;

    f.frame_type = AMQP_FRAME_BODY;
    f.channel = channel;
    f.payload.body_fragment.bytes = amqp_offset(body.bytes, body_offset);
    if (remaining >= usable_body_payload_size) {
      f.payload.body_fragment.len = usable_body_payload_size;
    } else {
      f.payload.body_fragment.len = remaining;
    }

    body_offset += f.payload.body_fragment.len;
    res = amqp_send_frame(state, &f);
    if (res < 0)
      return res;
  }

  return 0;
}

amqp_rpc_reply_t amqp_channel_close(amqp_connection_state_t state,
				    amqp_channel_t channel,
				    int code)
{
  char codestr[13];
  amqp_method_number_t replies[2] = { AMQP_CHANNEL_CLOSE_OK_METHOD, 0};
  amqp_channel_close_t req;

  req.reply_code = code;
  req.reply_text.bytes = codestr;
  req.reply_text.len = sprintf(codestr, "%d", code);
  req.class_id = 0;
  req.method_id = 0;

  return amqp_simple_rpc(state, channel, AMQP_CHANNEL_CLOSE_METHOD,
			 replies, &req);
}

amqp_rpc_reply_t amqp_connection_close(amqp_connection_state_t state,
				       int code)
{
  char codestr[13];
  amqp_method_number_t replies[2] = { AMQP_CONNECTION_CLOSE_OK_METHOD, 0};
  amqp_channel_close_t req;

  req.reply_code = code;
  req.reply_text.bytes = codestr;
  req.reply_text.len = sprintf(codestr, "%d", code);
  req.class_id = 0;
  req.method_id = 0;

  return amqp_simple_rpc(state, 0, AMQP_CONNECTION_CLOSE_METHOD,
			 replies, &req);
}

int amqp_basic_ack(amqp_connection_state_t state,
		   amqp_channel_t channel,
		   uint64_t delivery_tag,
		   amqp_boolean_t multiple)
{
  amqp_basic_ack_t m;
  m.delivery_tag = delivery_tag;
  m.multiple = multiple;
  return amqp_send_method(state, channel, AMQP_BASIC_ACK_METHOD, &m);
}

amqp_rpc_reply_t amqp_basic_get(amqp_connection_state_t state,
				amqp_channel_t channel,
				amqp_bytes_t queue,
				amqp_boolean_t no_ack)
{
  amqp_method_number_t replies[] = { AMQP_BASIC_GET_OK_METHOD,
				     AMQP_BASIC_GET_EMPTY_METHOD,
				     0 };
  amqp_basic_get_t req;
  req.ticket = 0;
  req.queue = queue;
  req.no_ack = no_ack;

  state->most_recent_api_result = amqp_simple_rpc(state, channel,
						  AMQP_BASIC_GET_METHOD,
						  replies, &req);
  return state->most_recent_api_result;
}

int amqp_basic_reject(amqp_connection_state_t state,
		      amqp_channel_t channel,
		      uint64_t delivery_tag,
		      amqp_boolean_t requeue)
{
  amqp_basic_reject_t req;
  req.delivery_tag = delivery_tag;
  req.requeue = requeue;
  return amqp_send_method(state, channel, AMQP_BASIC_REJECT_METHOD, &req);
}


/* ------------------------------------------------------------------------------------------------------------------------------------ */

#ifdef WITH_OPENSSL
#include <signal.h>
#include <limits.h>

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef SSIZE_MAX
#define SSIZE_MAX ((ssize_t) (SIZE_MAX / 2))
#endif


static int amqp_ssl_set_error(amqp_connection_state_t state, int err, const char *str);

static int amqp_ssl_init_done = 0;



void amqp_ssl_init()

{

	char 				buffer[1024];

	int 				val;


  	if (! amqp_ssl_init_done)
  	{
	   amqp_ssl_init_done = 1;

    	   SSL_library_init(); OpenSSL_add_all_algorithms(); SSL_load_error_strings();

    	   if (! RAND_load_file("/dev/urandom", 1024))
    	   {
      	      RAND_seed(buffer, sizeof(buffer));

      	      while (! RAND_status())
      	      {
	         val = rand();
        	 RAND_seed(&val, sizeof(val));
      	      }
    	   }
  	}

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

static void sp_handler(int val)

{
	/* Does nothing... just exists */
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

static int password_cb(char *buf, int num, int rwflag, void *ud)

{

	if (num < strlen((char *) ud) + 1)
    	{
	   return 0;
	}


	return (strlen(strcpy(buf, (char *) ud)));

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

char *amqp_ssl_error(amqp_connection_state_t state)

{

	return (state->errstr);

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

int amqp_ssl_connect(amqp_connection_state_t state, const char *host, int port)

{

	int 				sockfd;

	int 				rv;

	char 				name[256];


	sockfd = amqp_open_socket(host, port);
  	amqp_set_sockfd(state, sockfd);


    	state->ssl = SSL_new(state->ctx);
    	state->bio = BIO_new_socket(state->sockfd, BIO_NOCLOSE);
	SSL_set_bio(state->ssl, state->bio, state->bio);

	if ((rv = SSL_connect(state->ssl)) <= 0)
	{
	   return (amqp_ssl_set_error(state, SSL_get_error(state->ssl, rv), "SSL_connect()"));
	}


	if ((state->ssl_flags & AMQP_SSL_REQUIRE_SERVER_AUTHENTICATION))
	{
       	   if (SSL_get_verify_result(state->ssl) != X509_V_OK)
      	   {
	      return (amqp_ssl_set_error(state, -1, "SSL certificate presented by peer cannot be verified"));
	   }


 	   if (! (state->ssl_flags & AMQP_SSL_SKIP_HOST_CHECK))
	   {
	      X509 *peer;

	      if (! (peer = SSL_get_peer_certificate(state->ssl)))
	      {
	         return (amqp_ssl_set_error(state, -1, "No SSL certificate was presented by peer"));
	      }

	      /* This could be done better, but will suffice for now */

    	      X509_NAME_get_text_by_NID(X509_get_subject_name(peer), NID_commonName, name, sizeof(name));

	      X509_free(peer);

	      if (strcasecmp(name, host))
	      {
	         return (amqp_ssl_set_error(state, -1, "Common name doesn't match host name"));
	      }
	   }
	}


#if 0 				/* Included for future reference (stick with blocking I/O for now) */
	{
	   in flags;
#ifdef __VMS
  	   flags = 1;
  	   ioctl(state->sockfd, FIONBIO, &flags);
#else
  	   flags = fcntl(state->sockfd, F_GETFL, 0);
  	   if (flags == -1)
      	      flags = 0;
  	   fcntl(state->sockfd, F_SETFL, flags | O_NONBLOCK);
#endif
	}
#endif

	return (0);

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

int amqp_ssl_context(amqp_connection_state_t state, unsigned short flags, const char *keyfile, const char *password, const char *cafile)

{

	if (! amqp_ssl_init_done)
	{
	   amqp_ssl_init();
	}

   	signal(SIGPIPE, sp_handler);


	state->ssl_flags = flags;

	ERR_clear_error();

    	if (! (state->ctx = SSL_CTX_new(SSLv23_method())))
	{
	   return (amqp_ssl_set_error(state, -1, "Can't set up SSL context"));
	}


    	if (! SSL_CTX_use_certificate_chain_file(state->ctx, keyfile))
	{
	   return (amqp_ssl_set_error(state, -1, "Can't read certificate key file"));
	}


    	if (password != NULL)
    	{
	   SSL_CTX_set_default_passwd_cb_userdata(state->ctx, (void *) strdup(password)); 	/* Allocates memory (only done once) */

	   SSL_CTX_set_default_passwd_cb(state->ctx, password_cb);
	}


	if (! SSL_CTX_use_PrivateKey_file(state->ctx, keyfile, SSL_FILETYPE_PEM))
      	{
	   return (amqp_ssl_set_error(state, -1, "Can't read key file"));
	}

	if (cafile != NULL)
	{
    	   if (! SSL_CTX_load_verify_locations(state->ctx, cafile, 0))
	   {
	      return (amqp_ssl_set_error(state, -1, "Can't read CA file"));
	   }
	}

#if (OPENSSL_VERSION_NUMBER < 0x00905100L)
    	SSL_CTX_set_verify_depth(state->ctx, 1);
#endif

    	return (0);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */


static int amqp_ssl_set_error(amqp_connection_state_t state, int err, const char *str)

{

	int 				tmp = errno;		/* Keep last errno  for possible future reference */

	char * 				errstr;


	/* Minimum SSL error code is 0; maximum is 8 (ssl.h) */

	if (err == -1)
	{
	   if (tmp)
	   {
	      sprintf(state->errstr, "%s: %s", str, strerror(tmp));
	   }
	   else
	   {
	      strcpy(state->errstr, str);
	   }
	}
	else
	{
    	   switch (err)
	   {
              case SSL_ERROR_NONE:
                 errstr = "No error";
                 break;

              case SSL_ERROR_SSL:
                 errstr = "Internal OpenSSL error or protocol error";
                 break;

              case SSL_ERROR_WANT_READ:
                 errstr = "OpenSSL functions requested a read";
                 break;

              case SSL_ERROR_WANT_WRITE:
                 errstr = "OpenSSL functions requested a write";
                 break;

              case SSL_ERROR_WANT_X509_LOOKUP:
                 errstr = "OpenSSL requested a X509 lookup which didn't arrive";
                 break;

	      case SSL_ERROR_SYSCALL:				/* errno is likely to be relevant */
                 errstr = "Underlying syscall error";
                 break;

              case SSL_ERROR_ZERO_RETURN:
                 errstr = "Underlying socket operation returned zero";
                 break;

              case SSL_ERROR_WANT_CONNECT:
                 errstr = "OpenSSL functions wanted a connect";
                 break;

              default:
                 errstr = "Unknown OpenSSL error";
	         break;
    	   }

	   if (tmp)
	   {
	      sprintf(state->errstr, "%s: %s (%s)", str, errstr, strerror(tmp));
	   }
	   else
	   {
	      sprintf(state->errstr, "%s: %s", str, errstr);
	   }
	}


    	errno = tmp ? tmp : EIO; 				/* Go with a generic I/O error if nothing else */

	return (-1);

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

int amqp_ssl_recv(amqp_connection_state_t state, char *buf, size_t nb)

{

	int 				err;
	int 				len;


	len = SSL_read(state->ssl, buf, nb);

    	if (! len)
    	{
           switch ((err = SSL_get_error(state->ssl, len)))
           {
              case SSL_ERROR_SYSCALL:
                 if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR))
                 {
                    case SSL_ERROR_WANT_READ:
                       errno = EWOULDBLOCK;

                    return (0);
                 }

              case SSL_ERROR_SSL:
                 if (errno == EAGAIN)
	         {
                    return (0);
	         }

              default:
                 return (amqp_ssl_set_error(state, err, "SSL_read()"));
           }
    	}


    	return (len);
}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

int amqp_ssl_send(amqp_connection_state_t state, char *buf, size_t nb)

{

	int 				err;
	int 				len;


	len = SSL_write(state->ssl, buf, nb);

    	if (! len)
    	{
           switch ((err = SSL_get_error(state->ssl, len)))
           {
              case SSL_ERROR_SYSCALL:
                 if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
                 {
                    case SSL_ERROR_WANT_WRITE:
                    case SSL_ERROR_WANT_READ:
                        errno = EWOULDBLOCK;

                    return (0);
                }

             case SSL_ERROR_SSL:
                if (errno == EAGAIN)
	        {
                    return (0);
	        }

             default:
                return (amqp_ssl_set_error(state, err, "SSL_write()"));
           }
    	}


	return (len);

}

/* ------------------------------------------------------------------------------------------------------------------------------------ */

#ifdef __VMS
static void *mempcpy(void *dst, const void *src, size_t len)

{

  	return ((char *) memcpy(dst, src, len) + len);

}
#endif

int amqp_ssl_writev(amqp_connection_state_t state, const struct iovec *vec, int num)

{

	char *				buffer;

	char *				bp;

	int 				total;
	int 				chunk;

	int 				n;
	int 				i;


	total = 0;

    	for (i = 0; i < num; i++)
	{
           if ((SSIZE_MAX - total) < vec[i].iov_len)
	   {
              return (-1);
           }

           total += vec[i].iov_len;
    	}


    	if ((buffer = (char *) malloc(total * sizeof(char))) == NULL)
	{
	   sprintf(state->errstr, "malloc(): %s [%s, %d]", strerror(errno), __FILE__, __LINE__);

	   return (-1);
	}


    	n = total; bp = buffer;

    	for (i = 0; i < num; i++)
    	{
      	   chunk = MIN(vec[i].iov_len, n); bp = mempcpy (bp, vec[i].iov_base, chunk);

           n -= chunk;

	   if (n == 0)
	   {
    	      break;
	   }
    	}


	n = amqp_ssl_send(state, buffer, total);

    	free(buffer);

    	return (n);

}

#endif



