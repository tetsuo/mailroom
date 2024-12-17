#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdbool.h>

bool base64_urlencode(char *out, size_t out_size, const unsigned char *data, size_t data_len);

#endif // BASE64_H
