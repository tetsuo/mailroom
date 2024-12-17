#ifndef DB_H
#define DB_H

#include <libpq-fe.h>
#include <stdbool.h>

bool prepare_statement(PGconn *conn);
int fetch_user_actions(PGconn *conn, int seen);
int do_listen(PGconn *conn);
bool reconnect(PGconn **conn);

#endif // DB_H
