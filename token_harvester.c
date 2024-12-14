#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libpq-fe.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#ifdef __linux__
#include <sys/types.h>
#include <sys/select.h>
#endif

#define DEFAULT_DB_CONNSTR "host=localhost user=postgres dbname=postgres"
#define DEFAULT_CHANNEL "token_insert"
#define DEFAULT_EVENT_THRESHOLD 10
#define DEFAULT_TIMEOUT_MS 5000

static const char *QUERY =
    "WITH token_data AS ( "
    "    SELECT "
    "        t.account, "
    "        t.secret, "
    "        t.code, "
    "        t.expires_at, "
    "        t.id, "
    "        t.action, "
    "        a.email, "
    "        a.login "
    "    FROM "
    "        jobs "
    "    JOIN tokens t "
    "        ON t.id > jobs.last_seq "
    "        AND t.expires_at > EXTRACT(EPOCH FROM NOW()) "
    "        AND t.consumed_at IS NULL "
    "        AND t.action IN ('activation', 'password_recovery') "
    "    JOIN accounts a "
    "        ON a.id = t.account "
    "        AND ( "
    "            (t.action = 'activation' AND a.status = 'provisioned') "
    "            OR (t.action = 'password_recovery' AND a.status = 'active') "
    "        ) "
    "    WHERE "
    "        jobs.job_type = $1 "
    "    ORDER BY id ASC "
    "    LIMIT $2 "
    "), "
    "updated_jobs AS ( "
    "    UPDATE "
    "        jobs "
    "    SET "
    "        last_seq = (SELECT MAX(id) FROM token_data) "
    "    WHERE "
    "        job_type = $1 "
    "        AND EXISTS (SELECT 1 FROM token_data) "
    "    RETURNING last_seq "
    ") "
    "SELECT "
    "    td.action, "
    "    td.email, "
    "    td.login, "
    "    td.secret, "
    "    td.code "
    "FROM "
    "    token_data td";

static volatile sig_atomic_t running = 1;
static const char *channel_name = NULL;
static int event_threshold = DEFAULT_EVENT_THRESHOLD;
static int timeout_ms = DEFAULT_TIMEOUT_MS;

static void prepare_statement(PGconn *conn)
{
  // Prepare the statement once connected (or after reconnect)
  PGresult *res = PQprepare(conn, "fetch_actions", QUERY, 2, NULL);
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    fprintf(stderr, "[ERROR] failed to prepare statement: %s\n", PQerrorMessage(conn));
    PQclear(res);
    PQfinish(conn);
    exit(EXIT_FAILURE);
  }
  PQclear(res);
}

static void fetch_user_actions(PGconn *conn, int seen)
{
  const char *paramValues[2];
  char limit[12];
  snprintf(limit, sizeof(limit), "%d", seen);

  paramValues[0] = "user_action_queue"; // $1
  paramValues[1] = limit;               // $2

  // Now we use the prepared statement
  PGresult *res = PQexecPrepared(
      conn,
      "fetch_actions",
      2,
      paramValues,
      NULL, // lengths (not needed for text)
      NULL, // formats (text mode)
      0     // text results
  );

  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    fprintf(stderr, "[ERROR] query execution failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    return;
  }

  int nrows = PQntuples(res);
  if (nrows > 0)
  {
    for (int i = 0; i < nrows; i++)
    {
      if (i > 0)
        printf(",");

      for (int col = 0; col < PQnfields(res); col++)
      {
        if (col > 0)
          printf(",");
        printf("%s", PQgetvalue(res, i, col));
      }
    }
    printf("\n");
  }

  fflush(stdout);
  PQclear(res);
}

static void do_listen(PGconn *conn, const char *chan_name)
{
  char *escaped_channel = PQescapeIdentifier(conn, chan_name, strlen(chan_name));
  if (!escaped_channel)
  {
    fprintf(stderr, "[ERROR] failed to escape channel name: %s\n", PQerrorMessage(conn));
    PQfinish(conn);
    exit(EXIT_FAILURE);
  }

  char listen_command[256];
  snprintf(listen_command, sizeof(listen_command), "LISTEN %s", escaped_channel);
  PQfreemem(escaped_channel);

  PGresult *res = PQexec(conn, listen_command);
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    fprintf(stderr, "[ERROR] failed to listen for notifications: %s\n", PQerrorMessage(conn));
    PQclear(res);
    PQfinish(conn);
    exit(EXIT_FAILURE);
  }
  PQclear(res);

  fprintf(stderr, "[INFO] listening for notifications on channel: %s\n", chan_name);
}

static void reconnect(PGconn **conn, const char *connstr)
{
  fprintf(stderr, "[WARN] reconnecting to database...\n");
  PQfinish(*conn);

  int max_attempts = 3;
  for (int attempt = 0; attempt < max_attempts; attempt++)
  {
    *conn = PQconnectdb(connstr);
    if (PQstatus(*conn) == CONNECTION_OK)
    {
      fprintf(stderr, "[INFO] reconnected successfully\n");
      do_listen(*conn, channel_name);
      // Re-prepare the statement after reconnect
      prepare_statement(*conn);
      return;
    }

    fprintf(stderr, "[ERROR] failed to reconnect (attempt %d/%d): %s\n",
            attempt + 1, max_attempts, PQerrorMessage(*conn));
    PQfinish(*conn);
    sleep(3);
  }

  fprintf(stderr, "[ERROR] all reconnect attempts failed\n");
  exit(EXIT_FAILURE);
}

static void signal_handler(int sig)
{
  fprintf(stderr, "[WARN] signal %d received. exiting...\n", sig);
  running = 0;
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
  long parsed = strtol(val, &endptr, 10);
  if (endptr == val || *endptr != '\0' || parsed <= 0)
  {
    fprintf(stderr, "[WARN] invalid value for %s: %s, using default: %d\n", env_var, val, default_val);
    return default_val;
  }

  return (int)parsed;
}

int main(void)
{
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  const char *connstr = getenv("DB_CONNSTR");
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

  event_threshold = parse_env_int("EVENT_THRESHOLD", DEFAULT_EVENT_THRESHOLD);
  timeout_ms = parse_env_int("TIMEOUT_MS", DEFAULT_TIMEOUT_MS);

  PGconn *conn = PQconnectdb(connstr);
  if (PQstatus(conn) != CONNECTION_OK)
  {
    fprintf(stderr, "[ERROR] failed to connect to database: %s\n", PQerrorMessage(conn));
    return EXIT_FAILURE;
  }

  // Issue the initial LISTEN
  do_listen(conn, channel_name);

  // Prepare the statement here once the connection is established
  prepare_statement(conn);

  fd_set fds;
  struct timeval tv;
  int seen = 0;

  // Main event loop
  while (running)
  {
    FD_ZERO(&fds);
    FD_SET(PQsocket(conn), &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(PQsocket(conn) + 1, &fds, NULL, NULL, &tv);
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
      fprintf(stderr, "[ERROR] connection error: %s\n", PQerrorMessage(conn));
      reconnect(&conn, connstr);
      continue;
    }

    PGnotify *notify;
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
  return EXIT_SUCCESS;
}
