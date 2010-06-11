#include "pgreplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#ifdef HAVE_SYS_SELECT_H
#	include <sys/select.h>
#else
#	include <sys/types.h>
#	include <unistd.h>
#endif
#ifdef TIME_WITH_SYS_TIME
#	include <sys/time.h>
#	include <time.h>
#else
#	ifdef HAVE_SYS_TIME_H
#		include <sys/time.h>
#	else
#		include <time.h>
#	endif
#endif
#ifdef WINDOWS
#	include <windows.h>
#endif

/* time to nap between socket checks */
#define NAP_MICROSECONDS 1000

/* connect string */
static char *conn_string;

/* speed factor for replay */
static double replay_factor;

/* possible stati a connection can have */
typedef enum {
	idle = 0,
	conn_wait_write,
	conn_wait_read,
	wait_write,
	wait_read
} connstatus;

/* linked list element for list of open connections */
struct dbconn {
	uint64_t      session_id;
	PGconn        *db_conn;
	int           socket;
	connstatus    status;
	struct dbconn *next;
};

/* linked list of open connections */
static struct dbconn *connections = NULL;

/* remember timestamp of program start */
static struct timeval start_time;

/* remember timestamp of first statement */
static struct timeval first_stmt_time;

/* maximum seconds behind schedule */
static time_t secs_behind = 0;

/* processes (ignores) notices from the server */
static void ignore_notices(void *arg, const PGresult *res) {
}

/* encapsulates "select" call and error handling */

int do_select(int n, fd_set *rfds, fd_set *wfds, fd_set *xfds, struct timeval *timeout) {
	int rc;

	rc = select(n, rfds, wfds, xfds, timeout);
#ifdef WINDOWS
	if (SOCKET_ERROR == rc) {
		win_perror("Error in select()", 1);
		rc = -1;
	}
#else
	if (-1 == rc) {
		perror("Error in select()");
	}
#endif

	return rc;
}

/* checks if a certain socket can be read or written without blocking */

static int poll_socket(int socket, int for_read, char * const errmsg_prefix) {
	fd_set fds;
	struct timeval zero = { 0, 0 };

	FD_ZERO(&fds);
	FD_SET(socket, &fds);
	return do_select(socket + 1, for_read ? &fds : NULL, for_read ? NULL : &fds, NULL, &zero);
}

/* sleep routine that should work on all platforms */

static int do_sleep(struct timeval *delta) {
	debug(2, "Napping for %lu.%06lu seconds\n", (unsigned long)delta->tv_sec, (unsigned long)delta->tv_usec);
#ifdef WINDOWS
	Sleep((DWORD)delta->tv_sec * 1000 + (DWORD)(delta->tv_usec / 1000));
	return 0;
#else
	return do_select(0, NULL, NULL, NULL, delta);
#endif
}
	
int database_consumer_init(const char *ignore, const char *host, int port, const char *passwd, double factor) {
	int conn_string_len = 12;  /* port and '\0' */
	const char *p;
	char *p1;

	debug(3, "Entering database_consumer_init%s\n", "");

	/* get time of program start */
	if (-1 == gettimeofday(&start_time, NULL)) {
		perror("Error calling gettimeofday");
		return 0;
	}

	replay_factor = factor;

	/* calculate length of connect string */
	if (host) {
		conn_string_len += 8;
		for (p=host; '\0'!=*p; ++p) {
			if (('\'' == *p) || ('\\' == *p)) {
				conn_string_len += 2;
			} else {
				++conn_string_len;
			}
		}
	}
	if (passwd) {
		conn_string_len += 12;
		for (p=passwd; '\0'!=*p; ++p) {
			if (('\'' == *p) || ('\\' == *p)) {
				conn_string_len += 2;
			} else {
				++conn_string_len;
			}
		}
	}

	if (NULL == (conn_string = malloc(conn_string_len))) {
		fprintf(stderr, "Cannot allocate %d bytes of memory\n", conn_string_len);
		return 0;
	}
	/* write the port to the connection string if it is set */
	if (-1 == port) {
		conn_string[0] = '\0';
	} else {
		if (sprintf(conn_string, "port=%d", port) < 0) {
			perror("Error writing connect string:");
			free(conn_string);
			return 0;
		}
	}
	for (p1=conn_string; '\0'!=*p1; ++p1) {
		/* places p1 at the end of the string */
	}

	/* append host if necessary */
	if (host) {
		*(p1++) = ' ';
		*(p1++) = 'h';
		*(p1++) = 'o';
		*(p1++) = 's';
		*(p1++) = 't';
		*(p1++) = '=';
		*(p1++) = '\'';
		for (p=host; '\0'!=*p; ++p) {
			if (('\'' == *p) || ('\\' == *p)) {
				*(p1++) = '\\';
			}
			*(p1++) = *p;
		}
		*(p1++) = '\'';
		*p1 = '\0';
	}

	/* append password if necessary */
	if (passwd) {
		*(p1++) = ' ';
		*(p1++) = 'p';
		*(p1++) = 'a';
		*(p1++) = 's';
		*(p1++) = 's';
		*(p1++) = 'w';
		*(p1++) = 'o';
		*(p1++) = 'r';
		*(p1++) = 'd';
		*(p1++) = '=';
		*(p1++) = '\'';
		for (p=passwd; '\0'!=*p; ++p) {
			if (('\'' == *p) || ('\\' == *p)) {
				*(p1++) = '\\';
			}
			*(p1++) = *p;
		}
		*(p1++) = '\'';
		*p1 = '\0';
	}

	debug(2, "Database connect string: \"%s\"\n", conn_string);

	debug(3, "Leaving database_consumer_init%s\n", "");
	return 1;
}

void database_consumer_finish() {
	debug(3, "Entering database_consumer_finish%s\n", "");

	free(conn_string);

	if (NULL != connections) {
		fprintf(stderr, "Error: not all database connections closed\n");
	}

	debug(3, "Leaving database_consumer_finish%s\n", "");
}

int database_consumer(replay_item *item) {
	const uint64_t session_id = replay_get_session_id(item);
	const replay_type type = replay_get_type(item);
	int all_idle = 1, rc = 0;
	struct dbconn *conn = connections, *found_conn = NULL, *prev_conn = NULL;
	struct timeval target_time, now, delta;
	const struct timeval *stmt_time;
	struct timeval nap_time;
	static int fstmtm_set = 0;  /* have we already collected first_statement_time */
	double d;
	time_t i;
	char *connstr, *p1, errbuf[256];
	const char *user, *database, *p;
	PGcancel *cancel_request;
	PGresult *result;

	debug(3, "Entering database_consumer%s\n", "");

	/* loop through open connections and do what can be done */
	while ((-1 != rc) && (NULL != conn)) {
		/* if we find the connection for the current statement, remember it */
		if (session_id == conn->session_id) {
			found_conn = conn;
		}

		/* handle each connection according to status */
		switch(conn->status) {
			case idle:
				break;  /* nothing to do */

			case conn_wait_read:
			case conn_wait_write:
				/* in connection process */
				/* check if socket is still busy */
				switch (poll_socket(conn->socket, (conn_wait_read == conn->status), "Error polling socket during connect")) {
					case 0:
						/* socket still busy */
						debug(2, "Socket for session 0x" UINT64_FORMAT " busy for %s during connect\n", conn->session_id, (conn_wait_write == conn->status) ? "write" : "read");
						all_idle = 0;
						break;
					case 1:
						/* socket not busy, continue connect process */
						switch(PQconnectPoll(conn->db_conn)) {
							case PGRES_POLLING_WRITING:
								conn->status = conn_wait_write;
								all_idle = 0;
								break;
							case PGRES_POLLING_READING:
								conn->status = conn_wait_read;
								all_idle = 0;
								break;
							case PGRES_POLLING_OK:
								debug(2, "Connection for session 0x" UINT64_FORMAT " established\n", conn->session_id);
								conn->status = idle;
								break;
							default:
								fprintf(stderr, "Connection for session 0x" UINT64_FORMAT " failed: %s\n", conn->session_id, PQerrorMessage(conn->db_conn));
								rc = -1;
						}
						break;
					default:
						/* error happened in select() */
						rc = -1;
				}
				break;

			case wait_write:
				/* check if the socket is writable */
				switch (poll_socket(conn->socket, 0, "Error polling socket for write")) {
					case 0:
						/* socket still busy */
						debug(2, "Session 0x" UINT64_FORMAT " busy writing data\n", conn->session_id);
						all_idle = 0;
						break;
					case 1:
						/* try PQflush again */
						switch (PQflush(conn->db_conn)) {
							case 0:
								/* finished flushing all data */
								conn->status = wait_read;
								all_idle = 0;
								break;
							case 1:
								/* more data to flush */
								debug(2, "Session 0x" UINT64_FORMAT " needs to flush again\n", conn->session_id);
								all_idle = 0;
								break;
							default:
								fprintf(stderr, "Error flushing to database: %s\n", PQerrorMessage(conn->db_conn));
								rc = -1;
						}
						break;
					default:
						/* error in select() */
						rc = -1;
				}
				break;

			case wait_read:
				/* check if the socket is readable */
				switch (poll_socket(conn->socket, 1, "Error polling socket for read")) {
					case 0:
						/* socket still busy */
						debug(2, "Session 0x" UINT64_FORMAT " waiting for data\n", conn->session_id);
						all_idle = 0;
						break;
					case 1:
						/* read input from connection */
						if (! PQconsumeInput(conn->db_conn)) {
							fprintf(stderr, "Error reading from database: %s\n", PQerrorMessage(conn->db_conn));
							rc = -1;
						} else {
							/* check if we are done reading */
							if (! PQisBusy(conn->db_conn)) {
								/* read and discard all results */
								while (NULL != (result = PQgetResult(conn->db_conn))) {
									PQclear(result);
								}

								conn->status = idle;
							} else {
								/* more to read */
								all_idle = 0;
							}
						}
						break;
					default:
						/* error during select() */
						rc = -1;
				}
				break;
		}

		if (! found_conn) {
			/* remember previous item in list, useful for removing an item */
			prev_conn = conn;
		}

		conn = conn->next;
	}

	/* make sure we found a connection above (except for connect items) */
	if (1 == rc) {
		if ((pg_connect == type) && (NULL != found_conn)) {
			fprintf(stderr, "Error: connection for session 0x" UINT64_FORMAT " already exists\n", replay_get_session_id(item));
			rc = -1;
		} else if ((pg_connect != type) && (NULL == found_conn)) {
			fprintf(stderr, "Error: no connection found for session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));
			rc = -1;
		}
	}

	/* time when the statement originally ran */
	stmt_time = replay_get_time(item);

	/* set first_stmt_time if it is not yet set */
	if (! fstmtm_set) {
		first_stmt_time.tv_sec = stmt_time->tv_sec;
		first_stmt_time.tv_usec = stmt_time->tv_usec;

		fstmtm_set = 1;
	}

	/* get current time */
	if (-1 != rc) {
		if (-1 == gettimeofday(&now, NULL)) {
			fprintf(stderr, "Error: gettimeofday failed\n");
			rc = -1;
		}
	}

	/* determine if statement should already be consumed, sleep if necessary */
	if (-1 != rc) {
		/* calculate "target time" when item should be replayed:
		                        statement time - first statement time
		   program start time + -------------------------------------
		                                    replay factor            */

		/* timestamp of the statement */
		target_time.tv_sec = stmt_time->tv_sec;
		target_time.tv_usec = stmt_time->tv_usec;

		/* subtract time of first statement */
		if (target_time.tv_usec >= first_stmt_time.tv_usec) {
			target_time.tv_usec -= first_stmt_time.tv_usec;
			target_time.tv_sec -= first_stmt_time.tv_sec;
		} else {
			target_time.tv_usec = (1000000 + target_time.tv_usec) - first_stmt_time.tv_usec;
			target_time.tv_sec -= first_stmt_time.tv_sec + 1;
		}

		/* divide by replay_factor */
		if (replay_factor != 1.0) {
			/* - divide the seconds part by the factor
			   - divide the microsecond part by the factor and add the
			     fractional part (times 10^6) of the previous division
			   - if the result exceeds 10^6, subtract the excess and
			     add its 10^6th to the seconds part. */
			d = target_time.tv_sec / replay_factor;
			target_time.tv_sec = d;
			target_time.tv_usec = target_time.tv_usec / replay_factor +
				(d - target_time.tv_sec) * 1000000.0;
			i = target_time.tv_usec / 1000000;
			target_time.tv_usec -= i * 1000000;
			target_time.tv_sec += i;
		}

		/* add program start time */
		target_time.tv_usec += start_time.tv_usec;
		target_time.tv_sec += start_time.tv_sec;
		if (target_time.tv_usec > 1000000) {
			target_time.tv_usec -= 1000000;
			++target_time.tv_sec;
		}

		/* warn if we fall behind too much */
		if (secs_behind < now.tv_sec - target_time.tv_sec) {
			secs_behind = now.tv_sec - target_time.tv_sec;
			if (! (secs_behind % 10)) {
				printf("Execution is %lu seconds behind schedule\n", (unsigned long)secs_behind);
			}
		}

		if (((target_time.tv_sec > now.tv_sec) ||
				((target_time.tv_sec == now.tv_sec) && (target_time.tv_usec > now.tv_usec))) &&
				all_idle) {
			/* sleep if all is idle and the target time is in the future */

			/* calculate time to sleep (delta = target_time - now) */
			if (target_time.tv_usec > now.tv_usec) {
				delta.tv_sec = target_time.tv_sec - now.tv_sec;
				delta.tv_usec = target_time.tv_usec - now.tv_usec;
			} else {
				delta.tv_sec = target_time.tv_sec - now.tv_sec - 1;
				delta.tv_usec = 1000000 + target_time.tv_usec - now.tv_usec;
			}

			/* sleep */
			if (-1 == do_sleep(&delta)) {
				rc = -1;
			} else {
				/* then consume item */
				rc = 1;
			}
		} else if (((target_time.tv_sec < now.tv_sec) ||
				((target_time.tv_sec == now.tv_sec) && (target_time.tv_usec <= now.tv_usec))) &&
				((pg_connect == type) ||
				((pg_cancel == type) && (wait_read == found_conn->status)) ||
				(idle == found_conn->status))) {
			/* if the item is due and its connection is idle, consume it */
			/* cancel items will also be consumed if the connection is waiting for a resonse */
			rc = 1;
		} else {
			/* item cannot be consumed yet, nap a little */
			nap_time.tv_sec = 0;
			nap_time.tv_usec = NAP_MICROSECONDS;
			if (-1 == do_sleep(&nap_time)) {
				rc = -1;
			}
		}
	}

	/* send statement */
	if (1 == rc) {
		switch (type) {
			case pg_connect:
				debug(2, "Starting database connection for session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				/* allocate a connect string */
				user = replay_get_user(item);
				database = replay_get_database(item);
				if (NULL == (connstr = malloc(strlen(conn_string) + 2 * strlen(user) + 2 * strlen(database) + 18))) {
					fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(conn_string) + 2 * strlen(user) + 2 * strlen(database) + 18);
					rc = -1;
				} else {
					/* append user and password */
					strcpy(connstr, conn_string);
					p1 = connstr + strlen(connstr);
					*(p1++) = ' ';
					*(p1++) = 'u';
					*(p1++) = 's';
					*(p1++) = 'e';
					*(p1++) = 'r';
					*(p1++) = '=';
					*(p1++) = '\'';
					for (p=user; '\0'!=*p; ++p) {
						if (('\'' == *p) || ('\\' == *p)) {
							*(p1++) = '\\';
						}
						*(p1++) = *p;
					}
					*(p1++) = '\'';
					*(p1++) = ' ';
					*(p1++) = 'd';
					*(p1++) = 'b';
					*(p1++) = 'n';
					*(p1++) = 'a';
					*(p1++) = 'm';
					*(p1++) = 'e';
					*(p1++) = '=';
					*(p1++) = '\'';
					for (p=database; '\0'!=*p; ++p) {
						if (('\'' == *p) || ('\\' == *p)) {
							*(p1++) = '\\';
						}
						*(p1++) = *p;
					}
					*(p1++) = '\'';
					*p1 = '\0';

					/* allocate a struct dbconn */
					if (NULL == (found_conn = malloc(sizeof(struct dbconn)))) {
						fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)sizeof(struct dbconn));
						rc = -1;
					} else {
						/* initialize a connection */
						if (NULL == (found_conn->db_conn = PQconnectStart(connstr))) {
							fprintf(stderr, "Cannot allocate memory for database connection\n");
							rc = -1;
							free(found_conn);
						} else {
							if (CONNECTION_BAD == PQstatus(found_conn->db_conn)) {
								fprintf(stderr, "Error: connection to database failed: %s\n", PQerrorMessage(found_conn->db_conn));
								rc = -1;
								PQfinish(found_conn->db_conn);
								free(found_conn);
							} else {
								if (-1 == (found_conn->socket = PQsocket(found_conn->db_conn))) {
									fprintf(stderr, "Error: cannot get socket for database connection\n");
									rc = -1;
									PQfinish(found_conn->db_conn);
									free(found_conn);
								} else {
									/* set values in struct dbconn */

									found_conn->session_id = replay_get_session_id(item);
									found_conn->status = conn_wait_write;
									found_conn->next = connections;

									connections = found_conn;

									/* do not display notices */
									PQsetNoticeReceiver(found_conn->db_conn, ignore_notices, NULL);
								}
							}
						}
					}

					/* free connection sting */
					free(connstr);
				}
				break;
			case pg_disconnect:
				debug(2, "Disconnecting database connection for session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				PQfinish(found_conn->db_conn);

				/* remove struct dbconn from linked list */
				if (prev_conn) {
					prev_conn->next = found_conn->next;
				} else {
					connections = found_conn->next;
				}
				free(found_conn);

				break;
			case pg_execute:
				debug(2, "Sending simple statement on session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				if (! PQsendQuery(found_conn->db_conn, replay_get_statement(item))) {
					fprintf(stderr, "Error sending simple statement: %s\n", PQerrorMessage(found_conn->db_conn));
					rc = -1;
				} else {
					found_conn->status = wait_write;
				}
				break;
			case pg_prepare:
				debug(2, "Sending prepare request on session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				if (! PQsendPrepare(
						found_conn->db_conn,
						replay_get_name(item),
						replay_get_statement(item),
						0,
						NULL)) {
					fprintf(stderr, "Error sending prepare request: %s\n", PQerrorMessage(found_conn->db_conn));
					rc = -1;
				} else {
					found_conn->status = wait_write;
				}
				break;
			case pg_exec_prepared:
				debug(2, "Sending prepared statement execution on session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				if (! PQsendQueryPrepared(
						found_conn->db_conn,
						replay_get_name(item),
						replay_get_valuecount(item),
						replay_get_values(item),
						NULL,
						NULL,
						0)) {
					fprintf(stderr, "Error sending prepared statement execution: %s\n", PQerrorMessage(found_conn->db_conn));
					rc = -1;
				} else {
					found_conn->status = wait_write;
				}
				break;
			case pg_cancel:
				debug(2, "Sending cancel request on session 0x" UINT64_FORMAT "\n", replay_get_session_id(item));

				if (NULL == (cancel_request = PQgetCancel(found_conn->db_conn))) {
					fprintf(stderr, "Error creating cancel request\n");
					rc = -1;
				} else {
					if (! PQcancel(cancel_request, errbuf, 256)) {
						fprintf(stderr, "Error sending cancel request: %s\n", errbuf);
						rc = -1;
					}
					/* free cancel request */
					PQfreeCancel(cancel_request);
				}
				/* status remains unchanged */
				break;
		}

		replay_free(item);
	}

	/* try to flush the statement if necessary */
	if ((1 == rc) && (pg_disconnect != type) && (wait_write == found_conn->status)) {
		switch (PQflush(found_conn->db_conn)) {
			case 0:
				/* complete request sent */
				found_conn->status = wait_read;
				break;
			case 1:
				debug(2, "Will need to flush again%s\n", "");
				break;
			default:
				fprintf(stderr, "Error flushing to database: %s\n", PQerrorMessage(found_conn->db_conn));
				rc = -1;
		}
	}

	debug(3, "Leaving database_consumer%s\n", "");
	return rc;
}
