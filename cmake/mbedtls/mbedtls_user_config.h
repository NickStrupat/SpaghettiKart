/* Additions to mbedTLS's default configuration for libdatachannel (cmake/dependencies/common.cmake), which uses it for
 * the encryption of WebRTC connections. */
#define MBEDTLS_SSL_DTLS_SRTP
