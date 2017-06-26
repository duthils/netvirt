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

#include "inet.h"
#include "switch.h"

RB_HEAD(vnet_peer_tree, dtls_peer);
RB_HEAD(vnet_lladdr_tree, lladdr);

struct vnetwork {
	RB_ENTRY(vnetwork)	 entry;
	struct vnet_peer_tree	 peers;
	struct vnet_lladdr_tree	 arpcache;
	passport_t		*passport;
	SSL_CTX			*ctx;
	char			*uid;
	uint32_t		 active_node;
};

struct lladdr {
	RB_ENTRY(lladdr)	 entry;
	uint8_t			 macaddr[ETHER_ADDR_LEN];
	struct dtls_peer	*peer;
};

enum dtls_state {
	DTLS_LISTEN,
	DTLS_ACCEPT,
	DTLS_ESTABLISHED
};

struct dtls_peer {
	RB_ENTRY(dtls_peer)	 entry;
	RB_ENTRY(dtls_peer)	 vn_entry;
	struct sockaddr_storage  ss;
	struct event		*timer;
	enum dtls_state		 state;
	socklen_t		 ss_len;
	SSL			*ssl;
	struct vnetwork		*vnet;
	uint8_t			 macaddr[ETHER_ADDR_LEN];
};

RB_HEAD(vnet_tree, vnetwork);
RB_HEAD(dtls_peer_tree, dtls_peer);

static struct vnet_tree		 vnetworks;
static struct dtls_peer_tree	 dtls_peers;
static EC_KEY			*ecdh;
static SSL_CTX			*ctx;
static struct event		*ev_udplisten;
static struct addrinfo		*ai;
static int			 cookie_initialized;
static unsigned char		 cookie_secret[16];

static void		 info_cb(const SSL *, int, int);
static int		 servername_cb(SSL *, int *, void *);
static int		 generate_cookie(SSL *, unsigned char *, unsigned int *);
static int		 verify_cookie(SSL *, unsigned char *, unsigned int);
static int		 cert_verify_cb(int, X509_STORE_CTX *);
static struct dtls_peer	*dtls_peer_new(int);
static void		 dtls_peer_free(struct dtls_peer *);
static int		 dtls_handle(struct dtls_peer *);
static void		 dtls_peer_timeout_cb(int, short, void *);
static int		 dtls_peer_cmp(const struct dtls_peer *,
			    const struct dtls_peer *);
static int		 vnetwork_cmp(const struct vnetwork *,
    const struct vnetwork *);
RB_PROTOTYPE_STATIC(vnet_tree, vnetwork, entry, vnetwork_cmp);
RB_PROTOTYPE_STATIC(vnet_peer_tree, dtls_peer, vn_entry, dtls_peer_cmp);
RB_PROTOTYPE_STATIC(dtls_peer_tree, dtls_peer, entry, dtls_peer_cmp);
RB_PROTOTYPE_STATIC(vnet_lladdr_tree, lladdr, entry, lladdr_cmp);

int
lladdr_cmp(const struct lladdr *a, const struct lladdr *b)
{
	return memcmp(a->macaddr, b->macaddr, ETHER_ADDR_LEN);
}

int
vnetwork_cmp(const struct vnetwork *a, const struct vnetwork *b)
{
	return strcmp(a->uid, b->uid);
}

struct vnetwork
*vnetwork_lookup(const char *uid)
{
	struct vnetwork	match;

	match.uid = (char *)uid;
	return RB_FIND(vnet_tree, &vnetworks, &match);
}

void
vnetwork_free(struct vnetwork *vnet)
{
	struct lladdr	*lladdr;

	if (vnet == NULL)
		return;

	while ((lladdr = RB_ROOT(&vnet->arpcache)) != NULL)
		free(lladdr);
	pki_passport_destroy(vnet->passport);
	SSL_CTX_free(vnet->ctx);
	free(vnet->uid);
	free(vnet);
}

int
vnetwork_create(char *uid, char *cert, char *pvkey, char *cacert)
{
	struct vnetwork *vnet;

	if ((vnet = malloc(sizeof(*vnet))) == NULL) {
		log_warnx("%s: malloc", __func__);
		return (-1);
	}

	RB_INIT(&vnet->peers);
	RB_INIT(&vnet->arpcache);
	vnet->uid = strdup(uid);
	vnet->passport = pki_passport_load_from_memory(cert, pvkey, cacert);
	vnet->active_node = 0;
	vnet->ctx = NULL;

	RB_INSERT(vnet_tree, &vnetworks, vnet);

	return (0);
}

int
dtls_peer_cmp(const struct dtls_peer *a, const struct dtls_peer *b)
{
	if (a->ss_len < b->ss_len)
		return (-1);
	if (b->ss_len > b->ss_len)
		return (1);
	return (memcmp(&a->ss, &b->ss, a->ss_len));
}

struct dtls_peer *
dtls_peer_new(int sock)
{
	BIO			*bio = NULL;
	struct dtls_peer	*p = NULL;

	if ((p = malloc(sizeof(*p))) == NULL) {
		log_warn("%s: malloc", __func__);
		goto error;
	}

	p->ssl = NULL;
	p->vnet = NULL;
	p->ss_len = 0;
	p->state = DTLS_LISTEN;

	if ((p->timer = evtimer_new(ev_base, dtls_peer_timeout_cb, p)) == NULL)
		goto error;

	if ((p->ssl = SSL_new(ctx)) == NULL ||
	    SSL_set_app_data(p->ssl, p) != 1) {
		log_warnx("%s: SSL_new", __func__);
		goto error;
	}

	SSL_set_info_callback(p->ssl, info_cb);
	SSL_set_accept_state(p->ssl);
	SSL_set_verify(p->ssl,
	    SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
	    cert_verify_cb);

	if ((bio = BIO_new_dgram(sock, BIO_NOCLOSE)) == NULL) {
		log_warnx("%s: BIO_new_dgram", __func__);
		goto error;
	}

	SSL_set_bio(p->ssl, bio, bio);

	return (p);

error:
	dtls_peer_free(p);
	return (NULL);
}

void
dtls_peer_free(struct dtls_peer *p)
{
	if (p == NULL)
		return;

	if (p->ss_len != 0)
		RB_REMOVE(dtls_peer_tree, &dtls_peers, p);
	if (p->vnet != NULL)
		RB_REMOVE(vnet_peer_tree, &p->vnet->peers, p);
	SSL_free(p->ssl);
	free(p);
}

void
link_switch_recv(struct dtls_peer *p, uint8_t *frame, size_t len)
{
	printf("link switch\n");
	inet_print_addr(frame);
}

int
dtls_handle(struct dtls_peer *p)
{
	struct timeval		 tv;
	struct sockaddr		 caddr;
	enum dtls_state		 next_state;
	int			 ret;
	char			 buf[1500] = {0};

	for (;;) {
		switch (p->state) {
		case DTLS_LISTEN:
			ret = DTLSv1_listen(p->ssl, &caddr);
			next_state = DTLS_ACCEPT;
			break;

		case DTLS_ACCEPT:
			ret = SSL_accept(p->ssl);
			next_state = DTLS_ESTABLISHED;
			break;

		case DTLS_ESTABLISHED:
			if ((ret = SSL_read(p->ssl, buf, sizeof(buf))) > 0)
				link_switch_recv(p, buf, ret);
			next_state = DTLS_ESTABLISHED;
			break;

		default:
			log_warnx("invalid DTLS peer state");
			return (-1);
		}

		switch (SSL_get_error(p->ssl, ret)) {
		case SSL_ERROR_NONE:
			break;

		case SSL_ERROR_WANT_WRITE:
			return (0);

		case SSL_ERROR_WANT_READ:
			if (DTLSv1_get_timeout(p->ssl, &tv) == 1 &&
			    evtimer_add(p->timer, &tv) < 0) {
				return (-1);
			}
			return (0);

		default:
			printf("something else...%d\n", SSL_get_error(p->ssl, ret));
			// XXX logs... and disconnect
			return (0);
		}

		p->state = next_state;
	}

	return (0);
}

void
dtls_peer_timeout_cb(int fd, short event, void *arg)
{
	struct dtls_peer	*p = arg;

	DTLSv1_handle_timeout(p->ssl);

	if (dtls_handle(p) < 0)
		dtls_peer_free(p);
}

int
cert_verify_cb(int ok, X509_STORE_CTX *store)
{
	X509		*cert;
	X509_NAME	*name;
	char		 buf[256];

	cert = X509_STORE_CTX_get_current_cert(store);
	name = X509_get_subject_name(cert);
	X509_NAME_get_text_by_NID(name, NID_commonName, buf, 256);

	printf("CN: %s\n", buf);

	return (ok);
}

void
info_cb(const SSL *ssl, int where, int ret)
{
	struct dtls_peer	*p;

	if ((where & SSL_CB_HANDSHAKE_DONE) == 0)
		return;

	p = SSL_get_app_data(ssl);
	RB_INSERT(vnet_peer_tree, &p->vnet->peers, p);

	printf("connected !\n");
}

int
servername_cb(SSL *ssl, int *ad, void *arg)
{
	struct vnetwork		*vnet;
	struct dtls_peer	*p;
	static EC_KEY		*ecdh;
	const char		*servername;

	if ((servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name))
	    == NULL) {
		log_warnx("%s: no servername received", __func__);
		return (SSL_TLSEXT_ERR_ALERT_FATAL);
	}

	printf(">>> name %s\n", servername);

	if ((vnet = vnetwork_lookup(servername)) == NULL)
		return (SSL_TLSEXT_ERR_ALERT_FATAL);

	if (vnet->ctx == NULL) {

		if ((vnet->ctx = SSL_CTX_new(DTLSv1_method())) == NULL)
			log_warnx("%s: SSL_CTX_new", __func__);

		SSL_CTX_set_read_ahead(vnet->ctx, 1);

		SSL_CTX_set_options(vnet->ctx, SSL_OP_SINGLE_ECDH_USE);
		if (SSL_CTX_set_cipher_list(vnet->ctx, "ECDHE-ECDSA-AES256-SHA")
		    == 0)
			log_warnx("%s: SSL_CTX_set_cipher_list", __func__);

		if ((ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1))
		    == NULL)
			fatalx("%s: EC_KEY_new_by_curve_name", __func__);

		SSL_CTX_set_tmp_ecdh(vnet->ctx, ecdh);

		/* Load the trusted certificate store into our SSL_CTX */
		SSL_CTX_set_cert_store(vnet->ctx, vnet->passport->cacert_store);

	} else
		ctx = vnet->ctx;

	SSL_set_SSL_CTX(ssl, vnet->ctx);
	SSL_use_certificate(ssl, vnet->passport->certificate);
	SSL_use_PrivateKey(ssl, vnet->passport->keyring);

	p = SSL_get_app_data(ssl);
	p->vnet = vnet;

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

void
udplisten_cb(int sock, short what, void *ctx)
{
	struct dtls_peer	*p, needle;
	char			 buf[2000];

	needle.ss_len = sizeof(struct dtls_peer);
	char s[INET6_ADDRSTRLEN];

	recvfrom(sock, NULL, 0, MSG_PEEK, (struct sockaddr *)&needle.ss,
	    &needle.ss_len);

	printf("got packet from %s :: %d\n",
		inet_ntop(needle.ss.ss_family,
		&((struct sockaddr_in*)&needle.ss)->sin_addr, s, sizeof(s)),
		ntohs(&((struct sockaddr_in*)&needle.ss)->sin_port));

	if ((p = RB_FIND(dtls_peer_tree, &dtls_peers, &needle)) == NULL) {
		if ((p = dtls_peer_new(sock)) == NULL)
			goto error;
		else {
			p->ss = needle.ss;
			p->ss_len = needle.ss_len;
			RB_INSERT(dtls_peer_tree, &dtls_peers, p);
		}
	}

	if (dtls_handle(p) < 0 )
		goto error;

	return;

error:
	recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&needle.ss,
	    &needle.ss_len);

	dtls_peer_free(p);
	return;
}

void
switch_init(json_t *config)
{
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

	if (json_unpack(config, "{s:s}", "cert", &cert) < 0)
		fatalx("%s: 'cert' not found in config", __func__);

	if (json_unpack(config, "{s:s}", "pvkey", &pkey) < 0)
		fatalx("%s: 'pvkey' not found in config", __func__);

	if (json_unpack(config, "{s:s}", "cacert", &cacert) < 0)
		fatalx("%s: 'cacert' not found in config", __func__);

	SSL_load_error_strings();
	SSL_library_init();

	if (!RAND_poll())
		fatalx("%s: RAND_poll", __func__);

	if ((ctx = SSL_CTX_new(DTLSv1_method())) == NULL)
		fatalx("%s: SSL_CTX_new", __func__);

	SSL_CTX_set_read_ahead(ctx, 1);

	SSL_CTX_set_cookie_generate_cb(ctx, generate_cookie);
	SSL_CTX_set_cookie_verify_cb(ctx, verify_cookie);
	SSL_CTX_set_tlsext_servername_callback(ctx, servername_cb);
	SSL_CTX_set_tlsext_servername_arg(ctx, NULL);

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

	EC_KEY_free(ecdh);
	ERR_remove_state(0);
	ERR_free_strings();
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
}

RB_GENERATE_STATIC(vnet_tree, vnetwork, entry, vnetwork_cmp);
RB_GENERATE_STATIC(vnet_peer_tree, dtls_peer, vn_entry, dtls_peer_cmp);
RB_GENERATE_STATIC(dtls_peer_tree, dtls_peer, entry, dtls_peer_cmp);
RB_GENERATE_STATIC(vnet_lladdr_tree, lladdr, entry, lladdr_cmp);
