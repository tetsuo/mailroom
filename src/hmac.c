#include "log.h"
#include "config.h"
#include "hmac.h"

#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
  EVP_MAC *mac;
  EVP_MAC_CTX *ctx;
} HMAC_Context;

static HMAC_Context *hmac_ctx = NULL;

static const OSSL_PARAM params[] = {
    {OSSL_MAC_PARAM_DIGEST, OSSL_PARAM_UTF8_STRING, (void *)"SHA256", 0, 0},
    {NULL, 0, NULL, 0, 0}};

/**
 * Initializes the HMAC context.
 * Allocates resources for the HMAC operation and sets up the OpenSSL HMAC engine
 * with SHA256 as the hash function.
 *
 * @return true on success, false on failure.
 */
bool hmac_init()
{
  hmac_ctx = OPENSSL_zalloc(sizeof(*hmac_ctx));
  if (!hmac_ctx)
  {
    log_printf("memory allocation failed for HMAC context");
    ERR_print_errors_fp(stderr);
    return false;
  }

  hmac_ctx->mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (!hmac_ctx->mac)
  {
    log_printf("failed to fetch HMAC implementation");
    ERR_print_errors_fp(stderr);
    OPENSSL_free(hmac_ctx);
    hmac_ctx = NULL;
    return false;
  }

  hmac_ctx->ctx = EVP_MAC_CTX_new(hmac_ctx->mac);
  if (!hmac_ctx->ctx)
  {
    log_printf("failed to create HMAC context");
    ERR_print_errors_fp(stderr);
    EVP_MAC_free(hmac_ctx->mac);
    OPENSSL_free(hmac_ctx);
    hmac_ctx = NULL;
    return false;
  }

  if (EVP_MAC_init(hmac_ctx->ctx, hmac_key, hmac_keylen, params) != 1)
  {
    log_printf("failed to initialize HMAC context");
    ERR_print_errors_fp(stderr);
    EVP_MAC_CTX_free(hmac_ctx->ctx);
    EVP_MAC_free(hmac_ctx->mac);
    OPENSSL_free(hmac_ctx);
    hmac_ctx = NULL;
    return false;
  }

  return true;
}

/**
 * Signs the given data using HMAC-SHA256.
 * Reinitializes the HMAC context for each signing operation and calculates the
 * HMAC of the input data.
 *
 * @param data          Input data to be signed.
 * @param data_len      Input data length.
 * @param hmac_result   Buffer to store the resulting HMAC.
 * @param hmac_len      Pointer to the size of the HMAC buffer; updated with the actual length.
 *
 * @return true on success, false on failure.
 */
bool hmac_sign(const char *data, size_t data_len, unsigned char *hmac_result, size_t *hmac_len)
{
  if (!hmac_ctx || !data || !hmac_result || !hmac_len)
  {
    log_printf("invalid arguments to hmac_sign");
    return false;
  }

  // Reinitialize with the original key (no params needed, digest already set)
  if (EVP_MAC_init(hmac_ctx->ctx, hmac_key, hmac_keylen, NULL) != 1)
  {
    log_printf("failed to reinitialize HMAC context");
    ERR_print_errors_fp(stderr);
    return false;
  }

  if (EVP_MAC_update(hmac_ctx->ctx, (const unsigned char *)data, data_len) != 1)
  {
    log_printf("failed to update HMAC context with data");
    ERR_print_errors_fp(stderr);
    return false;
  }

  // Determine required length first
  size_t req_len = 0;
  if (EVP_MAC_final(hmac_ctx->ctx, NULL, &req_len, 0) != 1)
  {
    log_printf("failed to get HMAC length");
    ERR_print_errors_fp(stderr);
    return false;
  }

  if (req_len > *hmac_len)
  {
    log_printf("provided buffer too small for HMAC result");
    return false;
  }

  if (EVP_MAC_final(hmac_ctx->ctx, hmac_result, &req_len, *hmac_len) != 1)
  {
    log_printf("failed to finalize HMAC computation");
    ERR_print_errors_fp(stderr);
    return false;
  }

  *hmac_len = req_len;
  return true;
}

/**
 * Cleans up the HMAC context.
 * Frees all resources associated with the HMAC operation and securely clears
 * the HMAC key to prevent leakage of sensitive data.
 */
void hmac_cleanup()
{
  if (hmac_ctx != NULL)
  {
    if (hmac_ctx->ctx)
      EVP_MAC_CTX_free(hmac_ctx->ctx);
    if (hmac_ctx->mac)
      EVP_MAC_free(hmac_ctx->mac);
    OPENSSL_free(hmac_ctx);
    hmac_ctx = NULL;
  }

  if (hmac_keylen > 0)
  {
    OPENSSL_cleanse(hmac_key, hmac_keylen);
    hmac_keylen = 0;
  }
}
