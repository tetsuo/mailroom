#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include <stdbool.h>

int db_dequeue(PGconn *conn, const char *queue, int limit, int max_chunk_size);
bool db_connect(PGconn **conn, const char *conninfo, const char *channel);
bool db_healthcheck(PGconn *conn);

#endif // DB_H
