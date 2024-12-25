#include "log.h"
#include "db.h"
#include "hmac.h"
#include "base64.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIGNATURE_MAX_INPUT_SIZE 46
#define HMAC_RESULT_SIZE 32
#define CONCATENATED_SIZE 64
#define BASE64_ENCODED_SIZE 89
#define POSTGRES_PREPARED_STMT_NAME "1"

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

bool db_prepare_statement(PGconn *conn)
{
  PGresult *res = PQprepare(conn, POSTGRES_PREPARED_STMT_NAME, QUERY, 2, NULL);
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    PQclear(res);
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
                                       const unsigned char *secret, const char *code)
{
  size_t offset = 0;

  if (strcmp(action, "activation") == 0)
  {
    memcpy(output, "/activate", 9); // "/activate" is 9 bytes
    offset = 9;
    memcpy(output + offset, secret, 32);
    offset += 32;
  }
  else if (strcmp(action, "password_recovery") == 0)
  {
    memcpy(output, "/recover", 8); // "/recover" is 8 bytes
    offset = 8;
    memcpy(output + offset, secret, 32);
    offset += 32;
    memcpy(output + offset, code, 5); // "code" is 5 bytes
    offset += 5;
  }

  return offset; // Total length of the constructed data
}

int db_dequeue(PGconn *conn, const char *queue, int limit)
{
  static const char *params[2];
  static char limitstr[12];

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

  snprintf(limitstr, sizeof(limitstr), "%d", limit);
  params[0] = queue;
  params[1] = limitstr;

  res = PQexecPrepared(conn, POSTGRES_PREPARED_STMT_NAME, 2, params, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    log_printf("ERROR: skipping batch; query execution failed: %s", PQerrorMessage(conn));
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
    log_printf("FATAL: missing columns in the result set");
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
      log_printf("ERROR: skipping row; PQunescapeBytea failed or invalid secret length");
      continue;
    }

    if (strcmp(action, "activation") == 0)
    {
      printf("%d", 1);
    }
    else if (strcmp(action, "password_recovery") == 0)
    {
      printf("%d", 2);
    }
    else
    {
      printf("%d", 0);
    }

    printf(",%s,%s,", email, login);

    signature_len = construct_signature_data(signature_buffer, action, secret, code);

    hmac_len = HMAC_RESULT_SIZE;
    if (!hmac_sign(signature_buffer, signature_len, hmac_result, &hmac_len))
    {
      log_printf("ERROR: skipping row; HMAC signing failed");
      PQfreemem(secret);
      continue;
    }

    memcpy(combined_buffer, secret, 32);
    memcpy(combined_buffer + 32, hmac_result, hmac_len);

    if (!base64_urlencode(base64_encoded, sizeof(base64_encoded), combined_buffer, 32 + hmac_len))
    {
      log_printf("ERROR: skipping row; base64 encoding failed");
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

static bool db_listen(PGconn *conn, const char *channel)
{
  char *escaped_channel = PQescapeIdentifier(conn, channel, strlen(channel));
  if (!escaped_channel)
  {
    return false;
  }

  size_t command_len = strlen("LISTEN ") + strlen(escaped_channel) + 1;
  char listen_command[command_len];
  snprintf(listen_command, command_len, "LISTEN %s", escaped_channel);
  PQfreemem(escaped_channel);

  PGresult *res = PQexec(conn, listen_command);
  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    PQclear(res);
    return false;
  }
  PQclear(res);

  return true;
}

bool db_connect(PGconn **conn, const char *conninfo, const char *channel)
{
  *conn = PQconnectdb(conninfo);

  return PQstatus(*conn) == CONNECTION_OK &&
         db_listen(*conn, channel) &&
         db_prepare_statement(*conn);
}
