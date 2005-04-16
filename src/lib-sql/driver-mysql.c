/* Copyright (C) 2003-2004 Timo Sirainen, Alex Howansky */

#include "lib.h"
#include "buffer.h"
#include "sql-api-private.h"

#ifdef HAVE_MYSQL
#include <stdlib.h>
#include <time.h>
#include <mysql.h>
#include <errmsg.h>

/* Minimum delay between reconnecting to same server */
#define CONNECT_MIN_DELAY 1
/* Maximum time to avoiding reconnecting to same server */
#define CONNECT_MAX_DELAY (60*30)
/* If no servers are connected but a query is requested, try reconnecting to
   next server which has been disconnected longer than this (with a single
   server setup this is really the "max delay" and the CONNECT_MAX_DELAY
   is never used). */
#define CONNECT_RESET_DELAY 15

struct mysql_db {
	struct sql_db api;

	pool_t pool;
	const char *user, *password, *dbname, *unix_socket;
	const char *ssl_cert, *ssl_key, *ssl_ca, *ssl_ca_path, *ssl_cipher;
	unsigned int port, client_flags;

	buffer_t *connections; /* struct mysql_connection[] */
	unsigned int next_query_connection;
};

struct mysql_connection {
	struct mysql_db *db;

	MYSQL *mysql;
	const char *host;

	unsigned int connect_delay;
	unsigned int connect_failure_count;

	time_t last_connect;
	unsigned int connected:1;
	unsigned int ssl_set:1;
};

struct mysql_result {
	struct sql_result api;
	struct mysql_connection *conn;

	MYSQL_RES *result;
        MYSQL_ROW row;

	MYSQL_FIELD *fields;
	unsigned int fields_count;
};

extern struct sql_result driver_mysql_result;
extern struct sql_result driver_mysql_error_result;

static int driver_mysql_connect(struct mysql_connection *conn)
{
	struct mysql_db *db = conn->db;
	const char *unix_socket, *host;
	time_t now;

	if (conn->connected)
		return TRUE;

	/* don't try reconnecting more than once a second */
	now = time(NULL);
	if (conn->last_connect + (time_t)conn->connect_delay > now)
		return FALSE;
	conn->last_connect = now;

	if (*conn->host == '/') {
		unix_socket = conn->host;
		host = NULL;
	} else {
		unix_socket = NULL;
		host = conn->host;
	}

	if (!conn->ssl_set && (db->ssl_ca != NULL || db->ssl_ca_path != NULL)) {
#ifdef HAVE_MYSQL_SSL
		mysql_ssl_set(conn->mysql, db->ssl_key, db->ssl_cert,
			      db->ssl_ca, db->ssl_ca_path
#ifdef HAVE_MYSQL_SSL_CIPHER
			      , db->ssl_cipher
#endif
			     );
		conn->ssl_set = TRUE;
#else
		i_fatal("mysql: SSL support not compiled in "
			"(remove ssl_ca and ssl_ca_path settings)");
#endif
	}

	if (mysql_real_connect(conn->mysql, host, db->user, db->password,
			       db->dbname, db->port, unix_socket,
			       db->client_flags) == NULL) {
		if (conn->connect_failure_count > 0) {
			/* increase delay between reconnections to this
			   server */
			conn->connect_delay *= 5;
			if (conn->connect_delay > CONNECT_MAX_DELAY)
				conn->connect_delay = CONNECT_MAX_DELAY;
		}
		conn->connect_failure_count++;

		i_error("mysql: Connect failed to %s (%s): %s - "
			"waiting for %u seconds before retry",
			host, db->dbname, mysql_error(conn->mysql),
			conn->connect_delay);
		return FALSE;
	} else {
		i_info("mysql: Connected to %s%s (%s)", host,
		       conn->ssl_set ? " using SSL" : "", db->dbname);

		conn->connect_failure_count = 0;
		conn->connect_delay = CONNECT_MIN_DELAY;
		conn->connected = TRUE;
		return TRUE;
	}
}

static void driver_mysql_connect_all(struct mysql_db *db)
{
	struct mysql_connection *conn;
	size_t i, size;

	conn = buffer_get_modifyable_data(db->connections, &size);
	size /= sizeof(*conn);
	for (i = 0; i < size; i++)
		(void)driver_mysql_connect(&conn[i]);
}

static void driver_mysql_connection_add(struct mysql_db *db, const char *host)
{
	struct mysql_connection *conn;

	conn = buffer_append_space_unsafe(db->connections, sizeof(*conn));
	conn->db = db;
	conn->host = p_strdup(db->pool, host);
	conn->mysql = mysql_init(NULL);
	if (conn->mysql == NULL)
		i_fatal("mysql_init() failed");

	conn->connect_delay = CONNECT_MIN_DELAY;
}

static void driver_mysql_connection_free(struct mysql_connection *conn)
{
	mysql_close(conn->mysql);
}

static void driver_mysql_parse_connect_string(struct mysql_db *db,
					      const char *connect_string)
{
	const char *const *args, *name, *value;
	const char **field;

	db->ssl_cipher = "HIGH";

	t_push();
	args = t_strsplit_spaces(connect_string, " ");
	for (; *args != NULL; args++) {
		value = strchr(*args, '=');
		if (value == NULL) {
			i_fatal("mysql: Missing value in connect string: %s",
				*args);
		}
		name = t_strdup_until(*args, value);
		value++;

		field = NULL;
		if (strcmp(name, "host") == 0 ||
		    strcmp(name, "hostaddr") == 0)
			driver_mysql_connection_add(db, value);
		else if (strcmp(name, "user") == 0)
			field = &db->user;
		else if (strcmp(name, "password") == 0)
			field = &db->password;
		else if (strcmp(name, "dbname") == 0)
			field = &db->dbname;
		else if (strcmp(name, "port") == 0)
			db->port = atoi(value);
		else if (strcmp(name, "client_flags") == 0)
			db->client_flags = atoi(value);
		else if (strcmp(name, "ssl_cert") == 0)
			field = &db->ssl_cert;
		else if (strcmp(name, "ssl_key") == 0)
			field = &db->ssl_key;
		else if (strcmp(name, "ssl_ca") == 0)
			field = &db->ssl_ca;
		else if (strcmp(name, "ssl_ca_path") == 0)
			field = &db->ssl_ca_path;
		else if (strcmp(name, "ssl_cipher") == 0)
			field = &db->ssl_cipher;
		else
			i_fatal("mysql: Unknown connect string: %s", name);

		if (field != NULL)
			*field = p_strdup(db->pool, value);
	}
	t_pop();

	if (db->connections->used == 0)
		i_fatal("mysql: No hosts given in connect string");
}

static struct sql_db *driver_mysql_init(const char *connect_string)
{
	struct mysql_db *db;
	pool_t pool;

	pool = pool_alloconly_create("mysql driver", 512);

	db = p_new(pool, struct mysql_db, 1);
	db->pool = pool;
	db->api = driver_mysql_db;
	db->connections =
		buffer_create_dynamic(pool,
				      sizeof(struct mysql_connection) * 6);

	driver_mysql_parse_connect_string(db, connect_string);
	driver_mysql_connect_all(db);
	return &db->api;
}

static void driver_mysql_deinit(struct sql_db *_db)
{
	struct mysql_db *db = (struct mysql_db *)_db;
	struct mysql_connection *conn;
	size_t i, size;

	conn = buffer_get_modifyable_data(db->connections, &size);
	size /= sizeof(*conn);
	for (i = 0; i < size; i++)
		(void)driver_mysql_connection_free(&conn[i]);

	pool_unref(db->pool);
}

static enum sql_db_flags
driver_mysql_get_flags(struct sql_db *db __attr_unused__)
{
	return SQL_DB_FLAG_BLOCKING;
}

static int driver_mysql_connection_do_query(struct mysql_connection *conn,
					    const char *query)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (!driver_mysql_connect(conn))
			return 0;

		if (mysql_query(conn->mysql, query) == 0)
			return 1;

		/* failed */
		switch (mysql_errno(conn->mysql)) {
		case CR_SERVER_GONE_ERROR:
		case CR_SERVER_LOST:
			/* connection lost - try immediate reconnect */
			conn->connected = FALSE;
			break;
		default:
			return -1;
		}
	}

	/* connected -> lost it -> connected -> lost again */
	return 0;
}

static int driver_mysql_do_query(struct mysql_db *db, const char *query,
				 struct mysql_connection **conn_r)
{
	struct mysql_connection *conn;
	size_t size;
	int reset, ret;
	unsigned int i, start;

	conn = buffer_get_modifyable_data(db->connections, &size);
	size /= sizeof(*conn);

	/* go through the connections in round robin. if the connection
	   isn't available, try next one that is. */
	start = db->next_query_connection % size;
	db->next_query_connection++;

	for (reset = 0;; reset++) {
		i = start;
		do {
			ret = driver_mysql_connection_do_query(&conn[i], query);
			if (ret != 0) {
				/* success / failure */
				*conn_r = &conn[i];
				return ret;
			}

			/* not connected, try next one */
			i = (i + 1) % size;
		} while (i != start);

		if (reset)
			break;

		/* none are connected. connect_delays may have gotten too high,
		   reset all of them to see if some are still alive. */
		for (i = 0; i < size; i++)
			conn[i].connect_delay = CONNECT_RESET_DELAY;
	}

	return 0;
}

static void driver_mysql_exec(struct sql_db *_db, const char *query)
{
	struct mysql_db *db = (struct mysql_db *)_db;
	struct mysql_connection *conn;

	(void)driver_mysql_do_query(db, query, &conn);
}

static void driver_mysql_query(struct sql_db *_db, const char *query,
			       sql_query_callback_t *callback, void *context)
{
	struct mysql_db *db = (struct mysql_db *)_db;
	struct mysql_connection *conn;
	struct mysql_result result;

	switch (driver_mysql_do_query(db, query, &conn)) {
	case 0:
		/* not connected */
		callback(&sql_not_connected_result, context);
		return;

	case 1:
		/* query ok */
		memset(&result, 0, sizeof(result));
		result.api = driver_mysql_result;
		result.api.db = _db;
		result.conn = conn;
		result.result = mysql_store_result(conn->mysql);
		if (result.result == NULL)
			break;

		callback(&result.api, context);
                mysql_free_result(result.result);
		return;
	case -1:
		/* error */
		break;
	}

	/* error */
	memset(&result, 0, sizeof(result));
	result.api = driver_mysql_error_result;
	result.api.db = _db;
	result.conn = conn;
	callback(&result.api, context);
}

static int driver_mysql_result_next_row(struct sql_result *_result)
{
	struct mysql_result *result = (struct mysql_result *)_result;

	result->row = mysql_fetch_row(result->result);
	if (result->row != NULL)
		return 1;

	return mysql_errno(result->conn->mysql) ? -1 : 0;
}

static void driver_mysql_result_fetch_fields(struct mysql_result *result)
{
	if (result->fields != NULL)
		return;

	result->fields_count = mysql_num_fields(result->result);
	result->fields = mysql_fetch_fields(result->result);
}

static unsigned int
driver_mysql_result_get_fields_count(struct sql_result *_result)
{
	struct mysql_result *result = (struct mysql_result *)_result;

        driver_mysql_result_fetch_fields(result);
	return result->fields_count;
}

static const char *
driver_mysql_result_get_field_name(struct sql_result *_result, unsigned int idx)
{
	struct mysql_result *result = (struct mysql_result *)_result;

	driver_mysql_result_fetch_fields(result);
	i_assert(idx < result->fields_count);
	return result->fields[idx].name;
}

static int driver_mysql_result_find_field(struct sql_result *_result,
					  const char *field_name)
{
	struct mysql_result *result = (struct mysql_result *)_result;
	unsigned int i;

	driver_mysql_result_fetch_fields(result);
	for (i = 0; i < result->fields_count; i++) {
		if (strcmp(result->fields[i].name, field_name) == 0)
			return i;
	}
	return -1;
}

static const char *
driver_mysql_result_get_field_value(struct sql_result *_result,
				    unsigned int idx)
{
	struct mysql_result *result = (struct mysql_result *)_result;

	return (const char *)result->row[idx];
}

static const char *
driver_mysql_result_find_field_value(struct sql_result *result,
				     const char *field_name)
{
	int idx;

	idx = driver_mysql_result_find_field(result, field_name);
	if (idx < 0)
		return NULL;
	return driver_mysql_result_get_field_value(result, idx);
}

static const char *const *
driver_mysql_result_get_values(struct sql_result *_result)
{
	struct mysql_result *result = (struct mysql_result *)_result;

	return (const char *const *)result->row;
}

static const char *driver_mysql_result_get_error(struct sql_result *_result)
{
	struct mysql_result *result = (struct mysql_result *)_result;

	return mysql_error(result->conn->mysql);
}

struct sql_db driver_mysql_db = {
	driver_mysql_init,
	driver_mysql_deinit,
	driver_mysql_get_flags,
	driver_mysql_exec,
	driver_mysql_query
};

struct sql_result driver_mysql_result = {
	NULL,

	driver_mysql_result_next_row,
	driver_mysql_result_get_fields_count,
	driver_mysql_result_get_field_name,
	driver_mysql_result_find_field,
	driver_mysql_result_get_field_value,
	driver_mysql_result_find_field_value,
	driver_mysql_result_get_values,
	driver_mysql_result_get_error
};

static int
driver_mysql_result_error_next_row(struct sql_result *result __attr_unused__)
{
	return -1;
}

struct sql_result driver_mysql_error_result = {
	NULL,

	driver_mysql_result_error_next_row,
	NULL, NULL, NULL, NULL, NULL, NULL,
	driver_mysql_result_get_error
};
#endif
