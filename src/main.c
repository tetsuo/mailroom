#include "log.h"
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

#define ENV_BATCH_TIMEOUT 5000
#define ENV_BATCH_LIMIT 10
#define ENV_DB_CHANNEL_NAME "token_insert"
#define ENV_DB_QUEUE_NAME "user_action_queue"
#define ENV_DB_HEALTHCHECK_INTERVAL 270000

unsigned char hmac_secret[HMAC_SECRET_SIZE] = {0};
size_t hmac_secretlen = 0;

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
  log_printf("signal %d received. exiting...", sig);
  running = 0;
}

static bool is_valid_hmac_secrethex(const char *key)
{
  size_t expected_len = HMAC_SECRET_SIZE * 2;

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
    return default_val;
  }

  char *endptr;
  errno = 0;

  long parsed = strtol(val, &endptr, 10);

  if (errno == ERANGE)
  {
    log_printf("WARN: value for %s is out of range: %s, using default: %d", env_var, val, default_val);
    return default_val;
  }

  if (endptr == val || *endptr != '\0' || parsed < INT_MIN || parsed > INT_MAX)
  {
    log_printf("WARN: invalid value for %s: %s, using default: %d", env_var, val, default_val);
    return default_val;
  }

  return (int)parsed;
}

static size_t hex_to_bytes(unsigned char *b, size_t b_size, const char *hex)
{
  if (!b || !hex)
  {
    log_printf("PANIC: invalid hex string");
    return 0;
  }

  size_t hex_len = strlen(hex);
  if (hex_len % 2 != 0)
  {
    log_printf("PANIC: hex string must have an even length");
    return 0;
  }

  if (b_size < hex_len / 2)
  {
    log_printf("PANIC: buffer too small");
    return 0;
  }

  unsigned int byte;
  for (size_t i = 0; i < hex_len; i += 2)
  {
    if (sscanf(hex + i, "%2x", &byte) != 1)
    {
      log_printf("PANIC: invalid hex character");
      return 0;
    }
    b[i / 2] = (unsigned char)byte;
  }

  return hex_len / 2;
}

static int exit_code(PGconn *conn, int code)
{
  if (conn)
  {
    PQfinish(conn);
  }
  hmac_cleanup();
  return code;
}

static long get_current_time_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
  {
    perror("clock_gettime failed");
    exit(EXIT_FAILURE);
  }
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  const char *conninfo = getenv("MB_DATABASE_URL");
  if (!conninfo)
  {
    log_printf("MB_DATABASE_URL not set");
    return EXIT_FAILURE;
  }

  const char *hmac_secrethex = getenv("MB_SECRET_KEY");
  if (!hmac_secrethex)
  {
    log_printf("MB_SECRET_KEY not set");
    return EXIT_FAILURE;
  }

  if (!is_valid_hmac_secrethex(hmac_secrethex))
  {
    log_printf("MB_SECRET_KEY must be a 64-character hex string");
    return EXIT_FAILURE;
  }

  hmac_secretlen = hex_to_bytes(hmac_secret, sizeof(hmac_secret), hmac_secrethex);
  if (hmac_secretlen == 0)
  {
    log_printf("failed to decode MB_SECRET_KEY");
    return EXIT_FAILURE;
  }

  const char *channel_name = getenv("MB_CHANNEL_NAME");
  if (!channel_name)
  {
    channel_name = ENV_DB_CHANNEL_NAME;
  }

  const char *queue_name = getenv("MB_QUEUE_NAME");
  if (!queue_name)
  {
    queue_name = ENV_DB_QUEUE_NAME;
  }

  int healthcheck_ms = parse_env_int("MB_HEALTHCHECK_INTERVAL", ENV_DB_HEALTHCHECK_INTERVAL);
  int timeout_ms = parse_env_int("MB_BATCH_TIMEOUT", ENV_BATCH_TIMEOUT);

  if (healthcheck_ms < timeout_ms)
  {
    log_printf("MB_HEALTHCHECK_INTERVAL must be greater than MB_BATCH_TIMEOUT, or equal to it");
    return EXIT_FAILURE;
  }

  int batch_limit = parse_env_int("MB_BATCH_LIMIT", ENV_BATCH_LIMIT);

  log_printf("configured; channel=%s queue=%s limit=%d timeout=%dms healthcheck-interval=%dms", channel_name, queue_name, batch_limit, timeout_ms, healthcheck_ms);

  if (!hmac_init())
  {
    log_printf("PANIC: failed to init HMAC");
    return EXIT_FAILURE;
  }

  int result;

  PGconn *conn = NULL;

  fd_set active_fds, read_fds;
  int sock;

  struct timeval tv;
  int seen = 0;

  PGnotify *notify = NULL;
  int rc = 0;

  long start = get_current_time_ms();
  long now, elapsed, remaining_ms;

  long last_healthcheck = start;

  int ready = -1;

  while (running)
  {
    if (ready < 0)
    {
      if (conn)
      {
        PQfinish(conn);
      }

      if (!db_connect(&conn, conninfo, channel_name))
      {
        log_printf("ERROR: connection failed: %s", PQerrorMessage(conn));
        return exit_code(conn, EXIT_FAILURE);
      }

      log_printf("connected");

      while (running && (result = db_dequeue(conn, queue_name, batch_limit, batch_limit)) == batch_limit)
        ;

      if (result < 0)
      {
        return exit_code(conn, EXIT_FAILURE);
      }

      FD_ZERO(&active_fds);
      sock = PQsocket(conn);
      FD_SET(sock, &active_fds);

      seen = 0;
      ready = 0;
      last_healthcheck = get_current_time_ms();

      continue;
    }
    else if (ready > 0)
    {
      result = db_dequeue(conn, queue_name, seen, batch_limit);
      if (result == -2)
      {
        return exit_code(conn, EXIT_FAILURE);
      }
      else if (result == -1)
      {
        log_printf("WARN: forcing reconnect...");
        ready = -1;
        continue;
      }
      else if (result != seen)
      {
        log_printf("WARN: expected %d items to be processed, got %d", seen, result);
      }

      seen = 0;
      ready = 0;
      last_healthcheck = get_current_time_ms();
    }

    // Process any pending notifications before select()
    while (running && (notify = PQnotifies(conn)) != NULL)
    {
      PQfreemem(notify);
      if (seen == 0)
      {
        log_printf("NOTIFY called; waking up");
        start = get_current_time_ms(); // Received first notification; reset timer
      }
      seen++;
      PQconsumeInput(conn);
    }

    if (seen >= batch_limit)
    {
      log_printf("processing %d rows... (max reached)", seen);

      ready = 1;
      continue; // Skip select() and process immediately
    }

    now = get_current_time_ms();
    elapsed = now - start;
    remaining_ms = timeout_ms - elapsed;

    if (remaining_ms < 0)
    {
      remaining_ms = 0;
    }

    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;

    read_fds = active_fds;

    rc = select(sock + 1, &read_fds, NULL, NULL, &tv);

    if (rc < 0)
    {
      if (errno == EINTR)
      {
        if (!running)
        {
          break;
        }
        log_printf("WARN: select interrupted by signal");
        continue;
      }
      log_printf("ERROR: select failed: %s (socket=%d)", strerror(errno), sock);
      break;
    }
    else if (rc == 0)
    {                                // Timeout occurred;
      start = get_current_time_ms(); // Reset the timer

      if (seen > 0)
      {
        log_printf("processing %d rows... (timeout)", seen);

        ready = 1;
        continue;
      }

      if ((sock = PQsocket(conn)) < 0)
      {
        log_printf("WARN: socket closed; %s", PQerrorMessage(conn));
        ready = -1;
        continue;
      }

      if (now - last_healthcheck >= healthcheck_ms)
      {
        if (!db_healthcheck(conn))
        {
          ready = -1;
          continue;
        }
        else
        {
          last_healthcheck = start;
        }
      }
    }

    if (!FD_ISSET(sock, &read_fds))
    {
      continue;
    }

    do
    {
      if (!PQconsumeInput(conn))
      {
        log_printf("WARN: error consuming input: %s", PQerrorMessage(conn));
        if (PQstatus(conn) != CONNECTION_OK)
        {
          ready = -1;
          break;
        }
      }
    } while (running && PQisBusy(conn));
  }

  return exit_code(conn, EXIT_FAILURE);
}
