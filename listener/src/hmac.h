#ifndef HMAC_H
#define HMAC_H

#include <stdbool.h>
#include <stddef.h>

bool hmac_init(void);
bool hmac_sign(const char *data, size_t data_len, unsigned char *hmac_result, size_t *hmac_len);
void hmac_cleanup(void);

#endif // HMAC_H
