/*
 * MbedTLS user config overrides for OmniDesk24.
 *
 * libdatachannel requires DTLS-SRTP support (mbedtls_ssl_dtls_srtp_*
 * functions and types).  These are disabled by default in MbedTLS 3.6
 * and require both MBEDTLS_SSL_PROTO_DTLS and MBEDTLS_SSL_DTLS_SRTP.
 *
 * MBEDTLS_SSL_PROTO_DTLS in turn requires MBEDTLS_SSL_PROTO_TLS1_2.
 */
#ifndef OMNIDESK_MBEDTLS_USER_CONFIG_H
#define OMNIDESK_MBEDTLS_USER_CONFIG_H

#if !defined(MBEDTLS_SSL_PROTO_TLS1_2)
#define MBEDTLS_SSL_PROTO_TLS1_2
#endif

#if !defined(MBEDTLS_SSL_PROTO_DTLS)
#define MBEDTLS_SSL_PROTO_DTLS
#endif

#if !defined(MBEDTLS_SSL_DTLS_SRTP)
#define MBEDTLS_SSL_DTLS_SRTP
#endif

#endif /* OMNIDESK_MBEDTLS_USER_CONFIG_H */
