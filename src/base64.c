#include "base64.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <stdio.h>

/**
 * Encodes binary data into a URL-safe Base64 format.
 * Replaces '+' with '-', '/' with '_', and omits padding '='.
 *
 * @param out       Buffer to store the encoded output.
 * @param out_size  Size of the output buffer.
 * @param data      Input binary data to encode.
 * @param data_len  Length of the input binary data.
 * @return          True if encoding is successful, false otherwise.
 */
bool base64_urlencode(char *out, size_t out_size, const unsigned char *data, size_t data_len)
{
  if (data == NULL || data_len == 0 || out == NULL || out_size == 0)
    return false;

  // Calculate the maximum base64 size (with padding)
  // For an input of 64 bytes, this is exactly 88 chars.
  size_t required_size = ((data_len + 2) / 3) * 4;
  // Check if we have space for the base64 output plus the null terminator
  if (required_size + 1 > out_size)
  {
    return false;
  }

  BIO *b64 = BIO_new(BIO_f_base64());
  BIO *bmem = BIO_new(BIO_s_mem());
  if (!b64 || !bmem)
  {
    if (b64)
      BIO_free(b64);
    if (bmem)
      BIO_free(bmem);
    return false;
  }

  // No newlines in output
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_push(b64, bmem);

  if (BIO_write(b64, data, (int)data_len) != (int)data_len || BIO_flush(b64) < 1)
  {
    BIO_free_all(b64);
    return false;
  }

  BUF_MEM *bptr = NULL;
  BIO_get_mem_ptr(b64, &bptr);

  // Convert base64 output to URL-safe in 'out', removing padding
  size_t out_len = 0;
  for (size_t i = 0; i < bptr->length && out_len < out_size - 1; i++)
  {
    char c = bptr->data[i];
    if (c == '+')
    {
      out[out_len++] = '-';
    }
    else if (c == '/')
    {
      out[out_len++] = '_';
    }
    else if (c == '=')
    {
      // Stop on padding; we don't include '=' in URL-safe output
      break;
    }
    else
    {
      out[out_len++] = c;
    }
  }

  // Null-terminate safely
  out[out_len] = '\0';

  BIO_free_all(b64);
  return true;
}
