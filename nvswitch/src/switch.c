/*
 * NetVirt - Network Virtualization Platform
 * Copyright (C) 2009-2017 Mind4Networks inc.
 * Nicolas J. Bouliane <nib@m4nt.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/tree.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <err.h>
#include <string.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>

#include <event2/event.h>

#include <log.h>
#include <pki.h>

#include "switch.h"
#include "vnetwork.h"

struct dtls_peer {
	RB_ENTRY(dtls_peer)	 entry;
	SSL			*ssl;
	struct sockaddr_storage  ss;
	socklen_t		 ss_len;
};

RB_HEAD(dtls_peer_tree, dtls_peer);

static struct dtls_peer_tree	 dtls_peers;
static SSL_CTX			*ctx;
static passport_t		*passport;
static struct event		*ev_udplisten;
static struct addrinfo		*ai;
static int			 cookie_initialized;
static unsigned char		 cookie_secret[16];

static struct dtls_peer	*dtls_peer_new();
static void		 dtls_peer_free(struct dtls_peer *);
static int		 dtls_peer_cmp(const struct dtls_peer *,
			    const struct dtls_peer *);
int
dtls_peer_cmp(const struct dtls_peer *a, const struct dtls_peer *b)
{

}

struct dtls_peer *
dtls_peer_new()
{

}

void
dtls_peer_free(struct dtls_peer *p)
{

}

int
certverify_cb(int ok, X509_STORE_CTX *store)
{
	X509		*cert;
	X509_NAME	*name;
	char		 buf[256];

	printf("certverify_cb\n");
/*
	cert = X509_STORE_CTX_get_current_cert(store);
	name = X509_get_subject_name(cert);
	X509_NAME_get_text_by_NID(name, NID_commonName, buf, 256);

	printf("CN: %s\n", buf);
*/
	return (ok);
}

int
servername_cb(SSL *ssl, int *ad, void *arg)
{
	struct vnetwork	*vnet;
	const char	*servername;

	printf("servername_cb\n");

	if ((servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) == NULL)
		return (SSL_TLSEXT_ERR_NOACK);

	printf(">>> name %s\n", servername);

	if ((vnet = vnetwork_lookup(servername)) == NULL) {
		printf("vnet is NULL!\n");
	}

	/* Load the trusted certificate store into our SSL_CTX */
	SSL_CTX_set_cert_store(ctx, vnet->passport->cacert_store);

	SSL_set_SSL_CTX(ssl, ctx);

	/* Set the certificate and key */
	SSL_use_certificate(ssl, vnet->passport->certificate);
	SSL_use_PrivateKey(ssl, vnet->passport->keyring);

	return (SSL_TLSEXT_ERR_OK);
}

/* generate_cookie and verify_cookie
 * taken from openssl apps/s_cb.c
 */
int
generate_cookie(SSL *ssl, unsigned char *cookie, unsigned int *cookie_len)
{
	unsigned char	*buffer;
	unsigned char	 result[EVP_MAX_MD_SIZE];
	unsigned int	 length, resultlength;

	printf("generate cookie\n");
	union {
		struct sockaddr sa;
		struct sockaddr_in s4;
#if OPENSSL_USE_IPV6
		struct sockaddr_in6 s6;
#endif
	} peer;

	/* Initialize a random secret */
	if (cookie_initialized == 0) {
		if (RAND_bytes(cookie_secret, sizeof(cookie_secret)) <= 0)
			return (0);
		cookie_initialized = 1;
	}

	/* Read peer information */
	(void)BIO_dgram_get_peer(SSL_get_rbio(ssl), &peer);

	/* Create buffer with peer's address and port */
	length = 0;
	switch (peer.sa.sa_family) {
	case AF_INET:
		length += sizeof(struct in_addr);
		length += sizeof(peer.s4.sin_port);
		break;
#if OPENSSL_USE_IPV6
	case AF_INET6:
		length += sizeof(struct in6_addr);
		length += sizeof(peer.s6.sin6_port);
		break;
#endif
	default:
		return (0);
	}

	if ((buffer = OPENSSL_malloc(length)) == NULL)
		return (0);

	switch (peer.sa.sa_family) {
	case AF_INET:
		memcpy(buffer, &peer.s4.sin_port, sizeof(peer.s4.sin_port));
		memcpy(buffer + sizeof(peer.s4.sin_port),
		    &peer.s4.sin_addr, sizeof(struct in_addr));
		break;
#if OPENSSL_USE_IPV6
	case AF_INET6:
		memcpy(buffer, &peer.s6.sin6_port, sizeof(peer.s6.sin6_port));
		memcpy(buffer + sizeof(peer.s6.sin6_port),
		    &peer.s6.sin6_addr, sizeof(struct in6_addr));
		break;
#endif
	default:
		return (0);
	}

	/* Calculate HMAC of buffer using the secret */
	HMAC(EVP_sha1(), cookie_secret, sizeof(cookie_secret),
	    buffer, length, result, &resultlength);
	OPENSSL_free(buffer);

	memcpy(cookie, result, resultlength);
	*cookie_len = resultlength;

    return (1);
}

int
verify_cookie(SSL *ssl, unsigned char *cookie, unsigned int cookie_len)
{
	unsigned char	*buffer;
	unsigned char	 result[EVP_MAX_MD_SIZE];
	unsigned int	 length, resultlength;

	printf("verify cookie\n");
	union {
		struct sockaddr sa;
		struct sockaddr_in s4;
#if OPENSSL_USE_IPV6
		struct sockaddr_in6 s6;
#endif
	} peer;

	/* If secret isn't initialized yet, the cookie can't be valid */
	if (cookie_initialized == 0)
		return (0);

	/* Read peer information */
	(void)BIO_dgram_get_peer(SSL_get_rbio(ssl), &peer);

	/* Create buffer with peer's address and port */
	length = 0;
	switch (peer.sa.sa_family) {
	case AF_INET:
		length += sizeof(struct in_addr);
		length += sizeof(peer.s4.sin_port);
		break;
#if OPENSSL_USE_IPV6
	case AF_INET6:
		length += sizeof(struct in6_addr);
		length += sizeof(peer.s6.sin6_port);
		break;
#endif
	default:
		return (0);
	}

	if ((buffer = OPENSSL_malloc(length)) == NULL)
		return (0);

	switch (peer.sa.sa_family) {
	case AF_INET:
		memcpy(buffer, &peer.s4.sin_port, sizeof(peer.s4.sin_port));
		memcpy(buffer + sizeof(peer.s4.sin_port),
		    &peer.s4.sin_addr, sizeof(struct in_addr));
		break;
#if OPENSSL_USE_IPV6
	case AF_INET6:
		memcpy(buffer, &peer.s6.sin6_port, sizeof(peer.s6.sin6_port));
		memcpy(buffer + sizeof(peer.s6.sin6_port),
		    &peer.s6.sin6_addr, sizeof(struct in6_addr));
		break;
#endif
	default:
		return (0);
	}

	/* Calculate HMAC of buffer using the secret */
	HMAC(EVP_sha1(), cookie_secret, sizeof(cookie_secret),
	    buffer, length, result, &resultlength);
	OPENSSL_free(buffer);

	if (cookie_len == resultlength
	    && memcmp(result, cookie, resultlength) == 0)
		return (1);

    return (0);
}

int t = 0;
SSL		*ssl = NULL;
BIO		*bio = NULL;

void
udplisten_cb(int sock, short what, void *ctx)
{
	struct sockaddr	 caddr;
	int		 ret;
	char		 buf[1500] = {0};

	printf("udplisten_cb\n");

	struct sockaddr_storage client;
	socklen_t len = sizeof(client);
	char s[INET6_ADDRSTRLEN];

	recvfrom(sock, NULL, 0, MSG_PEEK, (struct sockaddr *)&client, &len);
	printf("got packet from %s :: %d\n",
		inet_ntop(client.ss_family,
		&((struct sockaddr_in*)&client)->sin_addr, s, sizeof(s)),
		ntohs(&((struct sockaddr_in*)&client)->sin_port));

	// dtls_peer = FIND() ...
	// if exist ... readto...
		// else try to handshake

	if (t == 1) {
		printf("SSL READ !\n");
		ret = SSL_read(ssl, buf, sizeof(buf));
		printf("buf %s\n", buf);
		return;
	}

	if (ssl == NULL) {
		printf("create ssl !\n");
		if ((ssl = SSL_new(ctx)) == NULL) {
			log_warnx("%s: SSL_new", __func__);
			goto error;
		}

		SSL_set_accept_state(ssl);
		SSL_set_verify(ssl,
		    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, certverify_cb);

		if ((bio = BIO_new_dgram(sock, BIO_NOCLOSE)) == NULL) {
			log_warnx("%s: BIO_new_dgram", __func__);
			goto error;
		}

		SSL_set_bio(ssl, bio, bio);
	}

	if ((ret = DTLSv1_listen(ssl, &caddr)) <=0 ) {
		switch (SSL_get_error(ssl, ret)) {
		case SSL_ERROR_NONE:
			printf("no error...\n");
			break;
		case SSL_ERROR_WANT_READ:
			printf("want read...\n");
			// XXX handle timeout here
			break;
		default:
			printf("default...?\n");
			break;
		}

		goto out;
	}

	SSL_accept(ssl);
	t = 1;

out:
	return;

error:
	SSL_free(ssl);
	ssl = NULL;
	BIO_free(bio);
	bio = NULL;
}

void
switch_init(json_t *config)
{
	EC_KEY		*ecdh;
	struct addrinfo	 hints;
	int		 status;
	int		 sock;
	int		 flag;
	const char	*ip;
	const char	*port;
	const char	*cert;
	const char	*pkey;
	const char	*cacert;

	if (json_unpack(config, "{s:s}", "switch_ip", &ip) < 0)
		fatalx("%s: switch_ip not found in config", __func__);

	if (json_unpack(config, "{s:s}", "switch_port", &port) < 0)
		fatalx("%s: switch_port not found config", __func__);

	if (json_unpack(config, "{s:s}", "certificate", &cert) < 0)
		fatalx("%s: certificate not found in config", __func__);

	if (json_unpack(config, "{s:s}", "privatekey", &pkey) < 0)
		fatalx("%s: privatekey not found in config", __func__);

	if (json_unpack(config, "{s:s}", "cacert", &cacert) < 0)
		fatalx("%s: cacert not found in config", __func__);

	SSL_load_error_strings();
	SSL_library_init();

	if (!RAND_poll())
		fatalx("%s: RAND_poll", __func__);

	if ((ctx = SSL_CTX_new(DTLSv1_server_method())) == NULL)
		fatalx("%s: SSL_CTX_new", __func__);

	SSL_CTX_set_read_ahead(ctx, 1);

	SSL_CTX_set_options(ctx, SSL_OP_SINGLE_ECDH_USE);

	SSL_CTX_set_cookie_generate_cb(ctx, generate_cookie);
	SSL_CTX_set_cookie_verify_cb(ctx, verify_cookie);
	SSL_CTX_set_tlsext_servername_callback(ctx, servername_cb);
	SSL_CTX_set_tlsext_servername_arg(ctx, NULL);

	if ((ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1)) == NULL)
		fatalx("%s: EC_KEY_new_by_curve_name", __func__);

	SSL_CTX_set_tmp_ecdh(ctx, ecdh);
	EC_KEY_free(ecdh);

	if (SSL_CTX_set_cipher_list(ctx, "ECDH-ECDSA-AES256-SHA") == 0)
		fatalx("%s: SSL_CTX_set_cipher_list", __func__);


	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	if ((status = getaddrinfo(ip, port, &hints, &ai)) != 0)
		fatalx("%s: getaddrinfo: %s", __func__, gai_strerror(status));

	if ((sock = socket(ai->ai_family, ai->ai_socktype,
	    ai->ai_protocol)) < 0)
		fatal("%s: socket", __func__);

	flag = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0)
		fatal("%s: setsockopt", __func__);

	if (evutil_make_socket_nonblocking(sock) > 0)
		fatalx("%s: evutil_make_socket_nonblocking", __func__);

	if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0)
		fatal("%s: bind", __func__);

	if ((ev_udplisten = event_new(ev_base, sock,
	    EV_READ | EV_PERSIST, udplisten_cb, ctx)) == NULL)
		fatal("%s: event_new", __func__);
	event_add(ev_udplisten, NULL);
}

void
switch_fini()
{
	SSL_CTX_free(ctx);
	freeaddrinfo(ai);
	pki_passport_destroy(passport);
	if (ev_udplisten != NULL)
		evsignal_del(ev_udplisten);
	event_base_free(ev_base);

	ERR_remove_state(0);
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}

RB_GENERATE_STATIC(dtls_peer_tree, dtls_peer, entry, dtls_peer_cmp);
