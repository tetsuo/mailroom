#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include <stdbool.h>

int db_dequeue(PGconn *conn, const char *queue, int limit);
bool db_connect(PGconn **conn, const char *conninfo, const char *channel);

#endif // DB_H
