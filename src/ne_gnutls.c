/*
   neon SSL/TLS support using GNU TLS
   Copyright (C) 2002-2004, Joe Orton <joe@manyfish.co.uk>
   Copyright (C) 2004, Aleix Conchillo Flaque <aleix@member.fsf.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA

*/

#include "config.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <gnutls/gnutls.h>
#include <gnutls/pkcs12.h>

#include "ne_ssl.h"
#include "ne_string.h"
#include "ne_session.h"
#include "ne_i18n.h"

#include "ne_private.h"
#include "ne_privssl.h"

struct ne_ssl_dname_s {
    char *dn;
};

struct ne_ssl_certificate_s {
    ne_ssl_dname subj_dn, issuer_dn;
    gnutls_x509_crt subject;
    ne_ssl_certificate *issuer;
    char *identity;
};

struct ne_ssl_client_cert_s {
    gnutls_pkcs12 p12;
    int decrypted; /* non-zero if successfully decrypted. */
    ne_ssl_certificate cert;
    char *friendly_name;
};

char *ne_ssl_readable_dname(const ne_ssl_dname *name)
{
    return name->dn;
}

int ne_ssl_dname_cmp(const ne_ssl_dname *dn1, const ne_ssl_dname *dn2)
{
    return strcmp(dn1->dn, dn2->dn);
}

void ne_ssl_clicert_free(ne_ssl_client_cert *cc)
{
#warning incomplete
}

void ne_ssl_cert_validity(const ne_ssl_certificate *cert,
                          char *from, char *until)
{
#warning FIXME strftime not portable
    if (from) {
        time_t t = gnutls_x509_crt_get_activation_time(cert->subject);
        strftime(from, NE_SSL_VDATELEN, "%b %d %H:%M:%S %Y %Z", localtime(&t));
    }
    if (until) {
        time_t t = gnutls_x509_crt_get_expiration_time(cert->subject);
        strftime(until, NE_SSL_VDATELEN, "%b %d %H:%M:%S %Y %Z", localtime(&t));
    }
}

/* Returns a new buffer with X509 subject's (or issuer) distinguished name. */
static char *x509_get_dn(gnutls_x509_crt x5, int subject)
{
    int ret, len;
    char *dn;

    if (subject)
        ret = gnutls_x509_crt_get_dn(x5, NULL, &len);
    else
        ret = gnutls_x509_crt_get_issuer_dn(x5, NULL, &len);

    if (ret < 0 && ret != GNUTLS_E_SHORT_MEMORY_BUFFER)
        return NULL;

    len++;
    dn = ne_calloc(len);
    if (subject)
        gnutls_x509_crt_get_dn(x5, dn, &len);
    else
        gnutls_x509_crt_get_issuer_dn(x5, dn, &len);

    return dn;
}

/* Return non-zero if hostname from certificate (cn) matches hostname
 * used for session (hostname).  (Wildcard matching is no longer
 * mandated by RFC3280, but certs are deployed which use wildcards) */
static int match_hostname(char *cn, const char *hostname)
{
    const char *dot;
    NE_DEBUG(NE_DBG_SSL, "Match %s on %s...\n", cn, hostname);
    dot = strchr(hostname, '.');
    if (dot == NULL) {
	char *pnt = strchr(cn, '.');
	/* hostname is not fully-qualified; unqualify the cn. */
	if (pnt != NULL) {
	    *pnt = '\0';
	}
    }
    else if (strncmp(cn, "*.", 2) == 0) {
	hostname = dot + 1;
	cn += 2;
    }
    return !strcasecmp(cn, hostname);
}

/* Check certificate identity.  Returns zero if identity matches; 1 if
 * identity does not match, or <0 if the certificate had no identity.
 * If 'identity' is non-NULL, store the malloc-allocated identity in
 * *identity.  If 'server' is non-NULL, it must be the network address
 * of the server in use, and identity must be NULL. */
static int check_identity(const char *hostname, gnutls_x509_crt cert,
                          char **identity, const ne_inet_addr *server)
{
    char name[255];
    unsigned int critical;
    int ret, len, seq = 0;
    int match = 0, found = 0;

    do {
        len = sizeof name;
        ret = gnutls_x509_crt_get_subject_alt_name(cert, seq, name, &len,
                                                   &critical);
        switch (ret) {
        case GNUTLS_SAN_DNSNAME:
        {
            if (identity && !found) *identity = ne_strdup(name);
            match = match_hostname(name, hostname);
            found = 1;
            break;
        }
        case GNUTLS_SAN_IPADDRESS:
        {
            /* TODO */
        }
        }
    } while (ret == 0 && ret != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE);

    /* Check against the commonName if no DNS alt. names were found,
     * as per RFC3280. */
    if (!found) {
        /* TODO */
    }

    NE_DEBUG(NE_DBG_SSL, "Identity match: %s\n", match ? "good" : "bad");
    return match ? 0 : 1;
}

/* Populate an ne_ssl_certificate structure from an X509 object. */
static ne_ssl_certificate *populate_cert(ne_ssl_certificate *cert,
                                         gnutls_x509_crt x5)
{
    cert->subj_dn.dn = x509_get_dn(x5, 1);
    cert->issuer_dn.dn = x509_get_dn(x5, 0);
    cert->issuer = NULL;
    cert->subject = x5;
    cert->identity = NULL;
    check_identity("", x5, &cert->identity, NULL);
    return cert;
}

void ne_ssl_set_clicert(ne_session *sess, const ne_ssl_client_cert *cc)
{
#warning incomplete
}

ne_ssl_context *ne_ssl_context_create(int flags)
{
    ne_ssl_context *ctx = ne_malloc(sizeof *ctx);
    gnutls_certificate_allocate_credentials(&ctx->cred);
    return ctx;
}

int ne_ssl_context_keypair(ne_ssl_context *ctx, 
                           const char *cert, const char *key)
{
    gnutls_certificate_set_x509_key_file(ctx->cred, cert, key,
                                         GNUTLS_X509_FMT_PEM);
    return 0;
}

int ne_ssl_context_set_verify(ne_ssl_context *ctx, int required,
                              const char *ca_names, const char *verify_cas)
{
    if (verify_cas) {
        gnutls_certificate_set_x509_trust_file(ctx->cred, verify_cas,
                                               GNUTLS_X509_FMT_PEM);
    }
#warning argh
    return 0;
}


void ne_ssl_context_destroy(ne_ssl_context *ctx)
{
    gnutls_certificate_free_credentials(ctx->cred);
    ne_free(ctx);
}

/* For internal use only. */
int ne__negotiate_ssl(ne_request *req)
{
    ne_session *sess = ne_get_session(req);
    ne_ssl_context *ctx = sess->ssl_context;
    const gnutls_datum *chain;
    unsigned int chain_size;

    NE_DEBUG(NE_DBG_SSL, "Doing SSL negotiation.\n");

    if (ne_sock_connect_ssl(sess->socket, ctx)) {
	ne_set_error(sess, _("SSL negotiation failed: %s"),
		     ne_sock_error(sess->socket));
	return NE_ERROR;
    }

    return NE_OK;
}

const ne_ssl_dname *ne_ssl_cert_issuer(const ne_ssl_certificate *cert)
{
    return &cert->issuer_dn;
}

const ne_ssl_dname *ne_ssl_cert_subject(const ne_ssl_certificate *cert)
{
    return &cert->subj_dn;
}

const ne_ssl_certificate *ne_ssl_cert_signedby(const ne_ssl_certificate *cert)
{
    return cert->issuer;
}

const char *ne_ssl_cert_identity(const ne_ssl_certificate *cert)
{
    return cert->identity;
}

void ne_ssl_context_trustcert(ne_ssl_context *ctx, const ne_ssl_certificate *cert)
{
#warning incomplete
}

void ne_ssl_trust_default_ca(ne_session *sess)
{
#warning incomplete
}

/* Functions from GNU TLS manual examples.
 *
 * Helper functions to load a certificate and key
 * files into memory. They use mmap for simplicity.
 */
static gnutls_datum mmap_file(const char* filename)
{
    int fd;
    gnutls_datum mmaped_file = { NULL, 0 };
    struct stat stat_st;
    void* ptr;

    fd = open(filename, 0);
    if (fd == -1)
        return mmaped_file;

    fstat(fd, &stat_st);

    if ((ptr = mmap(NULL, stat_st.st_size,
                    PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED)
        return mmaped_file;

    mmaped_file.data = ptr;
    mmaped_file.size = stat_st.st_size;

    return mmaped_file;
}

static void munmap_file( gnutls_datum data)
{
    munmap(data.data, data.size);
}

ne_ssl_client_cert *ne_ssl_clicert_read(const char *filename)
{
    return NULL;
}

int ne_ssl_clicert_encrypted(const ne_ssl_client_cert *cc)
{
    return !cc->decrypted;
}

int ne_ssl_clicert_decrypt(ne_ssl_client_cert *cc, const char *password)
{
    return 0;
}

const ne_ssl_certificate *ne_ssl_clicert_owner(const ne_ssl_client_cert *cc)
{
    return &cc->cert;
}

const char *ne_ssl_clicert_name(ne_ssl_client_cert *ccert)
{
    return ccert->friendly_name;
}

ne_ssl_certificate *ne_ssl_cert_read(const char *filename)
{
    int ret;
    gnutls_datum data;
    gnutls_x509_crt x5;

    data = mmap_file(filename);
    if (data.data == NULL)
        return NULL;

    if (gnutls_x509_crt_init(&x5) != 0)
        return NULL;

    ret = gnutls_x509_crt_import(x5, &data, GNUTLS_X509_FMT_PEM);
    if (ret < 0) {
        gnutls_x509_crt_deinit(x5);
        return NULL;
    }

    return populate_cert(ne_calloc(sizeof(struct ne_ssl_certificate_s)), x5);
}

int ne_ssl_cert_write(const ne_ssl_certificate *cert, const char *filename)
{
    int ret;
    unsigned char buffer[10*1024];
    int len = sizeof buffer;

    FILE *fp = fopen(filename, "w");

    if (fp == NULL) return -1;

    if (gnutls_x509_crt_export(cert->subject, GNUTLS_X509_FMT_PEM, buffer,
                               &len) < 0) {
        fclose(fp);
        return -1;
    }

    if (fwrite(buffer, len, 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0)
        return -1;

    return 0;
}

void ne_ssl_cert_free(ne_ssl_certificate *cert)
{
    gnutls_x509_crt_deinit(cert->subject);
    if (cert->subj_dn.dn) ne_free(cert->subj_dn.dn);
    if (cert->issuer_dn.dn) ne_free(cert->issuer_dn.dn);
    if (cert->identity) ne_free(cert->identity);

    if (cert->issuer)
        ne_ssl_cert_free(cert->issuer);
    if (cert->identity)
        ne_free(cert->identity);
    ne_free(cert);
}

int ne_ssl_cert_cmp(const ne_ssl_certificate *c1, const ne_ssl_certificate *c2)
{
    char digest1[NE_SSL_DIGESTLEN], digest2[NE_SSL_DIGESTLEN];

    if (ne_ssl_cert_digest(c1, digest1) || ne_ssl_cert_digest(c2, digest2)) {
        return -1;
    }

    return strcmp(digest1, digest2);
}

/* The certificate import/export format is the base64 encoding of the
 * raw DER; PEM without the newlines and wrapping. */

ne_ssl_certificate *ne_ssl_cert_import(const char *data)
{
    int ret;
    size_t len;
    unsigned char *der;
    gnutls_datum buffer = { NULL, 0 };
    gnutls_x509_crt x5;

    if (gnutls_x509_crt_init(&x5) != 0)
        return NULL;

    /* decode the base64 to get the raw DER representation */
    len = ne_unbase64(data, &der);
    if (len == 0) return NULL;

    buffer.data = der;
    buffer.size = len;

    ret = gnutls_x509_crt_import(x5, &buffer, GNUTLS_X509_FMT_DER);
    if (ret < 0) {
        gnutls_x509_crt_deinit(x5);
        return NULL;
    }

    return populate_cert(ne_calloc(sizeof(struct ne_ssl_certificate_s)), x5);
}

char *ne_ssl_cert_export(const ne_ssl_certificate *cert)
{
    int ret;
    unsigned char *der;
    size_t len = 0;
    char *ret;

    /* find the length of the DER encoding. */
    if (gnutls_x509_crt_export(cert->subject, GNUTLS_X509_FMT_DER, NULL, &len) != 
        GNUTLS_E_SHORT_MEMORY_BUFFER) {
        return NULL;
    }
    
    der = ne_malloc(len);
    if (gnutls_x509_crt_export(cert->subject, GNUTLS_X509_FMT_DER, der, &len)) {
        ne_free(der);
        return NULL;
    }
    
    ret = ne_base64(der, len);
    ne_free(der);
    return ret;
}

int ne_ssl_cert_digest(const ne_ssl_certificate *cert, char *digest)
{
    int ret, j, len = 20;
    char sha1[20], *p;

    if (gnutls_x509_crt_get_fingerprint(cert->subject, GNUTLS_DIG_SHA,
                                        sha1, &len) < 0)
        return -1;

    for (j = 0, p = digest; j < 20; j++) {
        *p++ = NE_HEX2ASC((sha1[j] >> 4) & 0x0f);
        *p++ = NE_HEX2ASC(sha1[j] & 0x0f);
        *p++ = ':';
    }

    *--p = '\0';
    return 0;
}