#include "config.h"
#include "db.h"
#include "hmac.h"
#include "base64.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/**
 * Prepares a SQL statement using the provided database connection.
 *
 * @param conn   Pointer to the PostgreSQL connection.
 * @return       True if the statement is successfully prepared, false otherwise.
 */
bool prepare_statement(PGconn *conn)
{
  PGresult *res = PQprepare(conn, PREPARED_STMT_NAME, QUERY, 2, NULL);
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    fprintf(stderr, "[ERROR] failed to prepare statement: %s\n", PQerrorMessage(conn));
    PQclear(res);
    PQfinish(conn);
    return false;
  }
  PQclear(res);
  return true;
}

/**
 * Constructs the signature data for HMAC signing based on the action type.
 * The signature data format differs between "activation" and "password_recovery".
 *
 * @param output       Buffer to store the constructed signature data.
 * @param action       The action type (e.g., "activation" or "password_recovery").
 * @param secret       Pointer to the secret key.
 * @param secret_len   Length of the secret key.
 * @param code         Optional code (used for "password_recovery").
 * @return             Total length of the constructed signature data.
 */
static size_t construct_signature_data(char *output, const char *action,
                                       const unsigned char *secret, size_t secret_len,
                                       const char *code)
{
  size_t offset = 0;

  if (strcmp(action, "activation") == 0)
  {
    memcpy(output, "/activate", 9); // "/activate" is 9 bytes
    offset = 9;
    memcpy(output + offset, secret, secret_len);
    offset += secret_len;
  }
  else if (strcmp(action, "password_recovery") == 0)
  {
    memcpy(output, "/recover", 8); // "/recover" is 8 bytes
    offset = 8;
    memcpy(output + offset, secret, secret_len);
    offset += secret_len;
    memcpy(output + offset, code, 5); // "code" is 5 bytes
    offset += 5;
  }

  return offset; // Total length of the constructed data
}

/**
 * Fetches user actions from the database using a prepared statement.
 * Processes the fetched data by constructing HMAC-signed and Base64-encoded tokens.
 *
 * @param conn   Pointer to the PostgreSQL connection.
 * @param seen   The maximum number of rows to fetch in a single execution.
 * @return       Number of rows fetched and processed, or an error code on failure.
 */
int fetch_user_actions(PGconn *conn, int seen)
{
  static const char *params[2];
  static char limit[12];

  PGresult *res = NULL;
  int action_col, email_col, login_col, code_col, secret_col;
  char *action, *email, *login, *code, *secret_text;
  unsigned char *secret = NULL;
  size_t secret_len;
  int nrows;

  char signature_buffer[SIGNATURE_MAX_INPUT_SIZE];  // Input to sign
  unsigned char hmac_result[HMAC_RESULT_SIZE];      // HMAC output
  unsigned char combined_buffer[CONCATENATED_SIZE]; // secret + HMAC
  char base64_encoded[BASE64_ENCODED_SIZE];         // Base64-encoded output

  size_t hmac_len = 0;

  snprintf(limit, sizeof(limit), "%d", seen);
  params[0] = queue_name;
  params[1] = limit;

  res = PQexecPrepared(conn, PREPARED_STMT_NAME, 2, params, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    fprintf(stderr, "[ERROR] query execution failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    return -1;
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

  if (action_col == -1 || email_col == -1 || login_col == -1 ||
      code_col == -1 || secret_col == -1)
  {
    fprintf(stderr, "[ERROR] missing columns in the result set\n");
    PQclear(res);
    return -2;
  }

  size_t signature_len;

  for (int i = 0; i < nrows; i++)
  {
    action = PQgetvalue(res, i, action_col);
    email = PQgetvalue(res, i, email_col);
    login = PQgetvalue(res, i, login_col);
    code = PQgetvalue(res, i, code_col);
    secret_text = PQgetvalue(res, i, secret_col);

    secret = PQunescapeBytea((unsigned char *)secret_text, &secret_len);
    if (!secret || secret_len != 32)
    {
      fprintf(stderr, "[ERROR] PQunescapeBytea failed or invalid secret length for row %d\n", i);
      continue;
    }

    printf("%s,%s,%s,", action, email, login);

    signature_len = construct_signature_data(signature_buffer, action, secret, code);

    hmac_len = HMAC_RESULT_SIZE;
    if (!hmac_sign(signature_buffer, signature_len, hmac_result, &hmac_len))
    {
      fprintf(stderr, "[ERROR] HMAC signing failed\n");
      PQfreemem(secret);
      continue;
    }

    memcpy(combined_buffer, secret, secret_len);
    memcpy(combined_buffer + secret_len, hmac_result, hmac_len);

    if (!base64_urlencode(base64_encoded, sizeof(base64_encoded), combined_buffer, secret_len + hmac_len))
    {
      fprintf(stderr, "[ERROR] base64_urlencode failed\n");
      PQfreemem(secret);
      continue;
    }

    printf("%s,%s", base64_encoded, code);

    PQfreemem(secret);

    if (i < nrows - 1)
    {
      printf(",");
    }
  }

  printf("\n");
  fflush(stdout);
  PQclear(res);

  return nrows;
}

/**
 * Subscribes to a PostgreSQL channel to listen for notifications.
 * Uses the "LISTEN" command with an escaped channel name.
 *
 * @param conn   Pointer to the PostgreSQL connection.
 * @return       0 on success, -1 on failure, or -2 if escaping the channel fails.
 */
int do_listen(PGconn *conn)
{
  char *escaped_channel = PQescapeIdentifier(conn, channel_name, strlen(channel_name));
  if (!escaped_channel)
  {
    fprintf(stderr, "[ERROR] failed to escape channel name: %s\n", PQerrorMessage(conn));
    PQfinish(conn);
    return -2;
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
    return -1;
  }
  PQclear(res);

  fprintf(stderr, "[INFO] listening for notifications on channel: %s\n", channel_name);

  return 0;
}

/**
 * Reconnects to the database with retries after a connection failure.
 * Prepares the statement and reinitializes the listening channel upon reconnection.
 *
 * @param conn   Double pointer to the PostgreSQL connection.
 * @return       True if reconnection is successful, false otherwise.
 */
bool reconnect(PGconn **conn)
{
  fprintf(stderr, "[WARN] reconnecting to database...\n");
  PQfinish(*conn);

  for (int attempt = 1; attempt <= RECONNECT_MAX_ATTEMPTS; attempt++)
  {
    *conn = PQconnectdb(connstr);
    if (PQstatus(*conn) == CONNECTION_OK)
    {
      fprintf(stderr, "[INFO] reconnected successfully\n");
      if (do_listen(*conn) < 0)
      {
        return false;
      }
      return prepare_statement(*conn);
    }

    fprintf(stderr, "[ERROR] failed to reconnect (attempt %d/%d): %s\n", attempt, RECONNECT_MAX_ATTEMPTS, PQerrorMessage(*conn));
    PQfinish(*conn);

    if (attempt < RECONNECT_MAX_ATTEMPTS)
      sleep(RECONNECT_INTERVAL_SECONDS);
  }

  fprintf(stderr, "[ERROR] all reconnect attempts failed. exiting...\n");

  return false;
}
