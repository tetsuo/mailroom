#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libpq-fe.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <limits.h>

#ifdef __linux__
#include <sys/types.h>
#include <sys/select.h>
#endif

#define DEFAULT_DB_CONNSTR "host=localhost user=postgres dbname=postgres"
#define DEFAULT_CHANNEL "token_insert"
#define DEFAULT_QUEUE "user_action_queue"
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

static void prepare_statement(PGconn *conn)
{
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

static int fetch_user_actions(PGconn *conn, const char *queue_name, int seen)
{
  static const char *paramValues[2];
  static char limit[12];

  PGresult *res = NULL;
  int action_col, email_col, login_col, code_col, secret_col;
  char *action, *email, *login, *code, *secret_text;
  unsigned char *decodedSecret = NULL;
  size_t decodedSecretLen;
  int nrows;

  snprintf(limit, sizeof(limit), "%d", seen);
  paramValues[0] = queue_name;
  paramValues[1] = limit;

  res = PQexecPrepared(conn, "fetch_actions", 2, paramValues, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    fprintf(stderr, "[ERROR] query execution failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    return 0;
  }

  nrows = PQntuples(res);
  if (nrows == 0)
  {
    PQclear(res);
    return 0;
  }

  action_col = PQfnumber(res, "action");
  email_col = PQfnumber(res, "email");
  login_col = PQfnumber(res, "login");
  code_col = PQfnumber(res, "code");
  secret_col = PQfnumber(res, "secret");

  for (int i = 0; i < nrows; i++)
  {
    action = PQgetvalue(res, i, action_col);
    email = PQgetvalue(res, i, email_col);
    login = PQgetvalue(res, i, login_col);
    code = PQgetvalue(res, i, code_col);
    secret_text = PQgetvalue(res, i, secret_col);

    decodedSecret = PQunescapeBytea((unsigned char *)secret_text, &decodedSecretLen);
    if (!decodedSecret)
    {
      fprintf(stderr, "[ERROR] PQunescapeBytea failed for row %d\n", i);
      continue;
    }

    if (i > 0)
      printf(",");

    printf("%s,%s,%s,", action, email, login);

    for (size_t j = 0; j < decodedSecretLen; j++)
    {
      printf("%02x", decodedSecret[j]);
    }

    printf(",%s", code);

    PQfreemem(decodedSecret);
  }

  printf("\n");
  fflush(stdout);
  PQclear(res);

  return nrows;
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

  size_t command_len = strlen("LISTEN ") + strlen(escaped_channel) + 1;
  char listen_command[command_len];
  snprintf(listen_command, command_len, "LISTEN %s", escaped_channel);
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

static void reconnect(PGconn **conn, const char *connstr, const char *channel_name)
{
  fprintf(stderr, "[WARN] reconnecting to database...\n");
  PQfinish(*conn);

  for (int attempt = 1; attempt <= 3; attempt++)
  {
    *conn = PQconnectdb(connstr);
    if (PQstatus(*conn) == CONNECTION_OK)
    {
      fprintf(stderr, "[INFO] reconnected successfully\n");
      do_listen(*conn, channel_name);
      prepare_statement(*conn);
      return;
    }

    fprintf(stderr, "[ERROR] failed to reconnect (attempt %d/3): %s\n", attempt, PQerrorMessage(*conn));
    PQfinish(*conn);

    if (attempt < 3)
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

  const char *channel_name = getenv("CHANNEL_NAME");
  if (!channel_name)
  {
    channel_name = DEFAULT_CHANNEL;
    fprintf(stderr, "[WARN] environment variable CHANNEL_NAME not set. default: %s\n", channel_name);
  }

  const char *queue_name = getenv("QUEUE_NAME");
  if (!queue_name)
  {
    queue_name = DEFAULT_QUEUE;
    fprintf(stderr, "[WARN] environment variable QUEUE_NAME not set. default: %s\n", queue_name);
  }

  int event_threshold = parse_env_int("EVENT_THRESHOLD", DEFAULT_EVENT_THRESHOLD);
  int timeout_ms = parse_env_int("TIMEOUT_MS", DEFAULT_TIMEOUT_MS);

  PGconn *conn = PQconnectdb(connstr);
  if (PQstatus(conn) != CONNECTION_OK)
  {
    fprintf(stderr, "[ERROR] failed to connect to database: %s\n", PQerrorMessage(conn));
    return EXIT_FAILURE;
  }

  do_listen(conn, channel_name);
  prepare_statement(conn);

  // Fetch actions at startup with limit of 16 rows
  while (fetch_user_actions(conn, queue_name, 16) == 16)
    ; // Continue fetching until fewer than limit rows are returned

  fd_set active_fds, read_fds;
  FD_ZERO(&active_fds);
  int sock = PQsocket(conn);
  FD_SET(sock, &active_fds);

  struct timeval tv;
  int seen = 0;

  while (running)
  {
    read_fds = active_fds; // Copy the active fd_set
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(sock + 1, &read_fds, NULL, NULL, &tv);
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
        fetch_user_actions(conn, queue_name, seen);
        seen = 0; // Reset count
      }
      continue;
    }

    if (!PQconsumeInput(conn))
    {
      reconnect(&conn, connstr, channel_name);

      // Fetch actions after reconnecting
      while (fetch_user_actions(conn, queue_name, 16) == 16)
        ; // Continue fetching until fewer than limit rows are returned

      sock = PQsocket(conn);
      FD_ZERO(&active_fds);
      FD_SET(sock, &active_fds);
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
        fetch_user_actions(conn, queue_name, seen);
        seen = 0; // Reset
      }
    }
  }

  PQfinish(conn);
  return EXIT_SUCCESS;
}
