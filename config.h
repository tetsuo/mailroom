#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <signal.h>

#define DEFAULT_DB_CONNSTR "host=localhost user=postgres dbname=postgres"
#define DEFAULT_CHANNEL "token_insert"
#define DEFAULT_QUEUE "user_action_queue"
#define DEFAULT_EVENT_THRESHOLD 10
#define DEFAULT_TIMEOUT_MS 5000

#define PREPARED_STMT_NAME "fetch_actions"
#define RECONNECT_MAX_ATTEMPTS 3
#define RECONNECT_INTERVAL_SECONDS 3

#define HMAC_KEY_SIZE 32
#define SIGNATURE_MAX_INPUT_SIZE 56
#define HMAC_RESULT_SIZE 32
#define CONCATENATED_SIZE 74
#define BASE64_ENCODED_SIZE 101

// Variables defined in main.c
extern const char *queue_name;
extern const char *channel_name;
extern const char *connstr;

extern unsigned char hmac_key[HMAC_KEY_SIZE];
extern size_t hmac_key_len;

#endif // CONFIG_H
