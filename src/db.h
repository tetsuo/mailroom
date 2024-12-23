#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include <stdbool.h>

int db_dump_csv(PGconn *conn, int seen);
int db_connect(PGconn **conn, int attempts, int wait_sec);

#endif // DB_H
