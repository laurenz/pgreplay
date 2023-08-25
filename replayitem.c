#include "pgreplay.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>

/* special replay_item that signals end-of-file */
static replay_item end_replay_item = {{0, 0}, 0, -1, 0, NULL};
replay_item * const end_item = &end_replay_item;

/* create common part of a replay_item */
static replay_item *replay_create(const struct timeval *time, uint64_t session_id, replay_type type, uint16_t count) {
	replay_item *r;

	r = malloc(sizeof(struct replay_item));
	if (NULL == r) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)sizeof(struct replay_item));
		return NULL;
	}
	r->time.tv_sec  = time->tv_sec;
	r->time.tv_usec = time->tv_usec;
	r->session_id   = session_id;
	r->type         = type;
	r->count        = count;
	if (0 == count) {
		r->data = NULL;
	} else {
		r->data = calloc(count, sizeof(char *));
		if (NULL == r->data) {
			fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)count * sizeof(char *));
			free(r);
			return NULL;
		}
	}

	return r;
}

replay_item *replay_create_connect(const struct timeval *time, uint64_t session_id, const char *user, const char *database) {
	replay_item *r;

	debug(3, "Entering replay_create_connect%s\n", "");

	r = replay_create(time, session_id, pg_connect, 2);
	if (NULL == r) {
		return NULL;
	}

	(r->data)[0] = malloc(strlen(user) + 1);
	if (NULL == (r->data)[0]) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(user) + 1);
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[0], user);

	(r->data)[1] = malloc(strlen(database) + 1);
	if (NULL == (r->data)[1]) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(user) + 1);
		free((r->data)[0]);
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[1], database);

	debug(3, "Leaving replay_create_connect%s\n", "");
	return r;
}

replay_item *replay_create_disconnect(const struct timeval *time, uint64_t session_id) {
	replay_item *r;

	debug(3, "Entering replay_create_disconnect%s\n", "");

	r = replay_create(time, session_id, pg_disconnect, 0);
	if (NULL == r) {
		return NULL;
	}
	debug(3, "Leaving replay_create_disconnect%s\n", "");
	return r;
}

replay_item *replay_create_execute(const struct timeval *time, uint64_t session_id, const char *statement) {
	replay_item *r;

	debug(3, "Entering replay_create_execute%s\n", "");

	r = replay_create(time, session_id, pg_execute, 1);
	if (NULL == r) {
		return NULL;
	}

	(r->data)[0] = malloc(strlen(statement) + 1);
	if (NULL == (r->data)[0]) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(statement) + 1);
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[0], statement);

	debug(3, "Leaving replay_create_execute%s\n", "");
	return r;
}

replay_item *replay_create_prepare(const struct timeval *time, uint64_t session_id, const char *name, const char *statement) {
	replay_item *r;

	debug(3, "Entering replay_create_prepare%s\n", "");

	r = replay_create(time, session_id, pg_prepare, 2);
	if (NULL == r) {
		return NULL;
	}

	(r->data)[0] = malloc(strlen(statement) + 1);
	if (NULL == (r->data)[0]) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(statement) + 1);
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[0], statement);

	(r->data)[1] = malloc(strlen(name) + 1);
	if (NULL == (r->data)[1]) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(name) + 1);
		free((r->data)[0]);
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[1], name);

	debug(3, "Leaving replay_create_prepare%s\n", "");
	return r;
}

replay_item *replay_create_exec_prepared(const struct timeval *time, uint64_t session_id, const char*name, uint16_t count, char * const *values) {
	replay_item *r;
	int i;

	debug(3, "Entering replay_create_exec_prepared%s\n", "");

	r = replay_create(time, session_id, pg_exec_prepared, count + 1);
	if (NULL == r) {
		return NULL;
	}

	(r->data)[0] = malloc(strlen(name) + 1);
	if (NULL == (r->data)[0]) {
		free(r->data);
		free(r);
		return NULL;
	}
	strcpy((r->data)[0], name);

	for (i=1; i<count+1; ++i) {
		if (values[i-1]) {
			(r->data)[i] = malloc(strlen(values[i-1]) + 1);
			if (NULL == (r->data)[i]) {
				fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(values[i-1]) + 1);
				for (--i; i>=0; --i) {
					if ((r->data)[i]) {
						free((r->data)[i]);
					}
				}
				free(r->data);
				free(r);
				return NULL;
			}
			strcpy((r->data)[i], values[i-1]);
		} else {
			(r->data)[i] = NULL;
		}
	}

	debug(3, "Leaving replay_create_exec_prepared%s\n", "");
	return r;
}

replay_item *replay_create_cancel(const struct timeval *time, uint64_t session_id) {
	replay_item *r;

	debug(3, "Entering replay_create_cancel%s\n", "");

	r = replay_create(time, session_id, pg_cancel, 0);
	if (NULL == r) {
		return NULL;
	}
	debug(3, "Leaving replay_create_cancel%s\n", "");
	return r;
}

void replay_free(replay_item *r) {
	int i;

	debug(3, "Entering replay_free%s\n", "");

	assert((pg_connect == r->type) || (pg_disconnect == r->type) || (pg_execute == r->type) || (pg_prepare == r->type) || (pg_exec_prepared == r->type) || (pg_cancel == r->type));

	for (i=0; i<r->count; ++i) {
		if ((r->data)[i]) {
			free((r->data)[i]);
		}
	}
	if (r->count) {
		free(r->data);
	}

	if(polardb_audit){
		free(r->search_path);
		free(r->prepare_parse);
	}
	free(r);

	debug(3, "Leaving replay_free%s\n", "");
}

replay_type replay_get_type(const replay_item *r) {
	return r->type;
}

uint64_t replay_get_session_id(const replay_item *r) {
	return r->session_id;
}

const struct timeval * replay_get_time(const replay_item *r) {
	return &(r->time);
}

const char * replay_get_statement(const replay_item *r) {
	assert((pg_execute == r->type) || (pg_prepare == r->type));

	return (r->data)[0];
}

const char * replay_get_name(const replay_item *r) {
	assert((pg_prepare == r->type) || (pg_exec_prepared == r->type));

	return (pg_prepare == r->type) ? (r->data)[1] : (r->data)[0];
}

const char * replay_get_user(const replay_item *r) {
	assert(pg_connect == r->type);

	return (r->data)[0];
}

const char * replay_get_database(const replay_item *r) {
	assert(pg_connect == r->type);

	return (r->data)[1];
}

int replay_get_valuecount(const replay_item *r) {
	assert(pg_exec_prepared == r->type);

	return r->count - 1;
}

const char * const * replay_get_values(const replay_item *r) {
	assert(pg_exec_prepared == r->type);

	return (const char * const *)((r->data) + 1);
}

/* maximal part of a value for display */
#define SAMPLE_SIZE 100

void replay_print_debug(const replay_item *r) {
	replay_type type;
	int i;
	char valuepart[SAMPLE_SIZE+4], *p;

	valuepart[SAMPLE_SIZE] = '.';
	valuepart[SAMPLE_SIZE+1] = '.';
	valuepart[SAMPLE_SIZE+2] = '.';
	valuepart[SAMPLE_SIZE+3] = '\0';

	debug(1, "---------------------------%s\n", "");
	debug(1, "Item: time       = %lu.%06lu\n", (unsigned long)r->time.tv_sec, (unsigned long)r->time.tv_usec);
	debug(1, "      session id = 0x" UINT64_FORMAT "\n", r->session_id);
	type = r->type;
	debug(1, "      type       = %s\n",
		(pg_connect == type) ? "connect" :
			((pg_disconnect == type) ? "disconnect" :
				((pg_execute == type) ? "execute" :
					((pg_prepare == type) ? "prepare" :
						((pg_exec_prepared == type) ? "exec_prepared" :
							((pg_cancel == type) ? "cancel" : "unknown")
						)
					)
				)
			)
	);
	switch (type) {
		case pg_connect:
			debug(1, "      user       = %s\n", replay_get_user(r));
			debug(1, "      database   = %s\n", replay_get_database(r));
		case pg_disconnect:
		case pg_cancel:
			break;
		case pg_prepare:
			debug(1, "      name       = %s\n", replay_get_name(r));
		case pg_execute:
			debug(1, "      statement  = %s\n", replay_get_statement(r));
			break;
		case pg_exec_prepared:
			debug(1, "      name       = %s\n", replay_get_name(r));
			for (i=0; i<replay_get_valuecount(r); ++i) {
				/* print only the first SAMPLE_SIZE bytes of the argument */
				if (replay_get_values(r)[i]) {
					strncpy(valuepart, replay_get_values(r)[i], SAMPLE_SIZE);
					p = valuepart;
				} else {
					p = NULL;
				}
				debug(1, "      $%d         = %s\n", i + 1, (NULL == p) ? "(null)" : p);
			}
			break;
	}
	debug(1, "---------------------------%s\n", "");
}
