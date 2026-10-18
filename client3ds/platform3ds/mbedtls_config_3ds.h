/* mbedTLS 3.6 LTS configuration for the 3DS Spotify client.
 *
 * TLS 1.2 client only. TLS 1.3 is off on purpose: in 3.6 it drags in the PSA
 * crypto core, and Spotify's access point and spclient both negotiate 1.2.
 *
 * Nothing here touches the filesystem or mbedTLS's own sockets -- libctru
 * provides BSD sockets and the certificate bundle is linked in as a blob, so
 * MBEDTLS_FS_IO and MBEDTLS_NET_C stay out.
 */
#ifndef MBEDTLS_CONFIG_3DS_H
#define MBEDTLS_CONFIG_3DS_H

/* ---------------------------------------------------------------- platform */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME                /* newlib time(), backed by libctru's RTC */
#define MBEDTLS_HAVE_TIME_DATE           /* so notBefore/notAfter are enforced */
/* mbedTLS only knows POSIX clock_gettime and Win32; plat3ds.c supplies
 * mbedtls_ms_time() from osGetTime() instead. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* The ARM11 has no entropy source newlib knows about; mbedtls_hardware_poll in
 * entropy3ds.c forwards to PS_GenerateRandomBytes, the console's own CSPRNG. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ------------------------------------------------------------------- crypto */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CBC          /* required by TLS 1.2 CBC suites */
#define MBEDTLS_CIPHER_MODE_CTR          /* audio files are AES-128-CTR */
#define MBEDTLS_GCM_C
#define MBEDTLS_GCM_LARGE_TABLE          /* GHASH is the hot loop on CDN reads */
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C                    /* Spotify names a Cast speaker by md5(name) */
#define MBEDTLS_SHA1_C                   /* X.509 signatures still in the wild */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_DHM_C                    /* the access point handshake is DH */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21                /* RSASSA-PSS certificate signatures */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_HKDF_C

/* X25519 and P-256 cover every Spotify host; the rest only cost code size. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* -------------------------------------------------------------------- X.509 */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C              /* the trust anchors are linked as PEM */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---------------------------------------------------------------------- TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* Spotify's edge demands SNI */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SESSION_TICKETS          /* a resumed handshake skips the ECDHE */

#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* A 16 KB inbound record is the protocol maximum and servers do use it; we
 * never send anything near that, so the outbound buffer stays small. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096

/* Sliding window for modular exponentiation. The default (6) keeps 64
 * MPI pointers plus big temporaries live on the stack during the RSA
 * verify of the handshake; on these thread stacks that was the overflow
 * that killed the fetch. 2 trades a little CPU for a much flatter stack. */
#define MBEDTLS_MPI_WINDOW_SIZE     2

/* --------------------------------------------------------- diagnostics only */
/* Both are compiled out of the shipping build; the TLS probe needs them. */
#ifdef PROBE_BUILD
#define MBEDTLS_ERROR_C
#define MBEDTLS_DEBUG_C
#endif

#endif /* MBEDTLS_CONFIG_3DS_H */
