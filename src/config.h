#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <signal.h>

#define HMAC_SECRET_SIZE 32

extern unsigned char hmac_secret[HMAC_SECRET_SIZE];
extern size_t hmac_secretlen;

#endif // CONFIG_H
