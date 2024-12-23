#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include <stdbool.h>

int db_dump_csv(PGconn *conn, const char *queue, int limit);
int db_connect(PGconn **conn, const char *conninfo, const char *channel, int attempts, int wait_sec);

#endif // DB_H
