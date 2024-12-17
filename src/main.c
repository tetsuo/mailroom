#include "config.h"
#include "db.h"
#include "hmac.h"
#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libpq-fe.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <openssl/crypto.h>

#ifdef __linux__
#include <sys/types.h>
#include <sys/select.h>
#endif

const char *queue_name = NULL;
const char *channel_name = NULL;
const char *connstr = NULL;

unsigned char hmac_key[HMAC_KEY_SIZE] = {0};
size_t hmac_key_len = 0;

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
  fprintf(stderr, "[WARN] signal %d received. exiting...\n", sig);
  running = 0;
}

static bool is_valid_hmac_key(const char *key)
{
  size_t expected_len = HMAC_KEY_SIZE * 2;

  if (!key || strlen(key) != expected_len)
    return false;

  for (size_t i = 0; i < expected_len; i++)
  {
    if (!isxdigit((unsigned char)key[i]))
      return false;
  }

  return true;
}

static int parse_env_int(const char *env_var, int default_val)
{
  const char *val = getenv(env_var);
  if (!val)
  {
    fprintf(stderr, "[WARN] environment variable %s not set. default: %d\n", env_var, default_val);
    return default_val;
  }

  char *endptr;
  errno = 0;

  long parsed = strtol(val, &endptr, 10);

  if (errno == ERANGE)
  {
    fprintf(stderr, "[WARN] value for %s is out of range: %s, using default: %d\n", env_var, val, default_val);
    return default_val;
  }

  if (endptr == val || *endptr != '\0' || parsed < INT_MIN || parsed > INT_MAX)
  {
    fprintf(stderr, "[WARN] invalid value for %s: %s, using default: %d\n", env_var, val, default_val);
    return default_val;
  }

  return (int)parsed;
}

static size_t hex_to_bytes(unsigned char *b, size_t b_size, const char *hex)
{
  if (!b || !hex)
  {
    fprintf(stderr, "[ERROR] invalid input\n");
    return 0;
  }

  size_t hex_len = strlen(hex);
  if (hex_len % 2 != 0)
  {
    fprintf(stderr, "[ERROR] hex string must have an even length\n");
    return 0;
  }

  if (b_size < hex_len / 2)
  {
    fprintf(stderr, "[ERROR] byte array is too small\n");
    return 0;
  }

  unsigned int byte;
  for (size_t i = 0; i < hex_len; i += 2)
  {
    if (sscanf(hex + i, "%2x", &byte) != 1)
    {
      fprintf(stderr, "[ERROR] invalid hex character\n");
      return 0;
    }
    b[i / 2] = (unsigned char)byte;
  }

  return hex_len / 2;
}

int main(void)
{
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  connstr = getenv("DB_CONNSTR");
  if (!connstr)
  {
    connstr = DEFAULT_DB_CONNSTR;
    fprintf(stderr, "[WARN] environment variable DB_CONNSTR not set. default: %s\n", connstr);
  }

  channel_name = getenv("CHANNEL_NAME");
  if (!channel_name)
  {
    channel_name = DEFAULT_CHANNEL;
    fprintf(stderr, "[WARN] environment variable CHANNEL_NAME not set. default: %s\n", channel_name);
  }

  queue_name = getenv("QUEUE_NAME");
  if (!queue_name)
  {
    queue_name = DEFAULT_QUEUE;
    fprintf(stderr, "[WARN] environment variable QUEUE_NAME not set. default: %s\n", queue_name);
  }

  const char *hmac_key_hex = getenv("HMAC_KEY");
  if (!hmac_key_hex)
  {
    fprintf(stderr, "[ERROR] environment variable HMAC_KEY not set\n");
    return EXIT_FAILURE;
  }

  if (!is_valid_hmac_key(hmac_key_hex))
  {
    fprintf(stderr, "[ERROR] environment variable HMAC_KEY must be a 64-character hex string\n");
    return EXIT_FAILURE;
  }

  hmac_key_len = hex_to_bytes(hmac_key, sizeof(hmac_key), hmac_key_hex);
  if (hmac_key_len == 0)
  {
    fprintf(stderr, "[ERROR] failed to decode HMAC_KEY\n");
    return EXIT_FAILURE;
  }

  if (!hmac_init())
  {
    fprintf(stderr, "[ERROR] failed to init HMAC\n");
    OPENSSL_cleanse(hmac_key, hmac_key_len);
    return EXIT_FAILURE;
  }

  int event_threshold = parse_env_int("EVENT_THRESHOLD", DEFAULT_EVENT_THRESHOLD);
  int timeout_ms = parse_env_int("TIMEOUT_MS", DEFAULT_TIMEOUT_MS);

  PGconn *conn = PQconnectdb(connstr);
  if (PQstatus(conn) != CONNECTION_OK)
  {
    fprintf(stderr, "[ERROR] failed to connect to database: %s\n", PQerrorMessage(conn));
    hmac_cleanup();
    return EXIT_FAILURE;
  }

  if (do_listen(conn) < 0)
  {
    hmac_cleanup();
    return EXIT_FAILURE;
  }

  if (!prepare_statement(conn))
  {
    hmac_cleanup();
    return EXIT_FAILURE;
  }

  // Fetch actions at startup with limit of event_threshold rows
  while (fetch_user_actions(conn, event_threshold) == event_threshold)
    ; // Continue fetching until fewer than limit rows are returned

  fd_set active_fds, read_fds;
  FD_ZERO(&active_fds);
  int sock = PQsocket(conn);
  FD_SET(sock, &active_fds);

  struct timeval tv;
  int seen = 0;

  PGnotify *notify = NULL;
  int rc = 0;

  while (running)
  {
    read_fds = active_fds;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(sock + 1, &read_fds, NULL, NULL, &tv);
    if (rc < 0)
    {
      fprintf(stderr, "[ERROR] select failed: %s\n", strerror(errno));
      break;
    }

    if (rc == 0)
    {
      // Timeout occurred
      if (seen > 0)
      {
        fetch_user_actions(conn, seen);
        seen = 0; // Reset count
      }
      continue;
    }

    if (!PQconsumeInput(conn))
    {
      if (!reconnect(&conn))
      {
        hmac_cleanup();
        return EXIT_FAILURE;
      }

      // Fetch actions after reconnecting
      while (fetch_user_actions(conn, event_threshold) == event_threshold)
        ;

      sock = PQsocket(conn);
      FD_ZERO(&active_fds);
      FD_SET(sock, &active_fds);
      continue;
    }

    while ((notify = PQnotifies(conn)) != NULL)
    {
      seen++;
      PQfreemem(notify);

      // Fetch actions immediately if threshold is reached
      if (seen >= event_threshold)
      {
        fetch_user_actions(conn, seen);
        seen = 0; // Reset
      }
    }
  }

  PQfinish(conn);
  hmac_cleanup();
  return EXIT_SUCCESS;
}
