#include "pgreplay.h"

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#ifdef WINDOWS
#	include <io.h>
#	include <winsock.h>
#	define FILE_MODE S_IRUSR | S_IWUSR
#else
#	ifdef HAVE_NETINET_IN_H
#		include <netinet/in.h>
#	endif
#	define FILE_MODE S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* input or output file */
static int filed=0;

/* functions to convert 64-bit integers between host and network byte order */
#ifndef htonll
#	ifdef WORDS_BIGENDIAN
#		define htonll(x) (x)
#		define ntohll(x) (x)
#	else
#		define htonll(x) ((((uint64_t)htonl(x)) << 32) + htonl(x >> 32))
#		define ntohll(x) ((((uint64_t)ntohl(x)) << 32) + ntohl(x >> 32))
#	endif
#endif

/* this length indicates a null value */
#define NULL_VALUE 0x80000000

/* wrapper functions for read and write */
static int do_write(const void * const buf, size_t count) {
	int rc = write(filed, buf, count);

	if (-1 == rc) {
		perror("Error writing to output file");
		return 0;
	} else if (count != rc) {
		fprintf(stderr, "Error: not all bytes written to output file\n");
		return 0;
	}

	return 1;
}

static int do_read(void *buf, size_t count, int *eof_indicator) {
	int rc = read(filed, buf, count);

	if (eof_indicator) {
		*eof_indicator = 0;
	}

	if (-1 == rc) {
		perror("Error reading from input file");
		return 0;
	} else if (eof_indicator && (0 == rc)) {
		*eof_indicator = 1;
	} else if (count != rc) {
		fprintf(stderr, "Error: unexpected end of file on input file\n");
		return 0;
	}

	return 1;
}

/* write a string to the output file */
static int write_string(char const * const s) {
	uint32_t u32, len;

	/* write length + NULL indicator (4 byte) */
	if (NULL == s) {
		len = NULL_VALUE;
	} else {
		len = strlen(s);
	}
	u32 = htonl(len);
	if (! do_write(&u32, 4)) {
		return 0;
	} else if (NULL != s) {
		/* write string */
		if (! do_write(s, len)) {
			return 0;
		}
	}

	return 1;
}

/* malloc and read a string from the input file */
static int read_string(char ** const s) {
	uint32_t u32, len;

	/* read length (4 byte) */
	if (! do_read(&u32, 4, NULL)) {
		return 0;
	}
	len = ntohl(u32);
	if (NULL_VALUE == len) {
		*s = NULL;
	} else {
		/* allocate the string */
		if (! (*s = malloc(len + 1))) {
			fprintf(stderr, "Cannot allocate %d bytes of memory\n", len + 1);
			return 0;
		} else {
			/* read string */
			if (! do_read(*s, len, NULL)) {
				return 0;
			}
			(*s)[len] = '\0';
		}
	}

	return 1;
}

int file_provider_init(const char *infile, int cvs, const char *begin_time, const char *end_time, const char *db_only, const char *usr_only) {
	int rc = 1;
	debug(3, "Entering file_provider_init%s\n", "");

	if (NULL == infile) {
		filed = 0;
#ifdef WINDOWS
		setmode(filed, O_BINARY);
#endif
	} else {
		if (-1 == (filed = open(infile, O_RDONLY
#ifdef WINDOWS
					| O_BINARY
#endif
					))) {
			perror("Error opening input file:");
			rc = 0;
		}
	}

	debug(3, "Leaving file_provider_init%s\n", "");

	return rc;
}

void file_provider_finish() {
	debug(3, "Entering file_provider_finish%s\n", "");

	if (0 != filed) {
		if (close(filed)) {
			perror("Error closing input file:");
		}
	}

	debug(3, "Leaving file_provider_finish%s\n", "");
}

replay_item * file_provider() {
	replay_item *r = NULL;
	uint16_t u16;
	uint32_t u32;
	uint64_t u64, session_id = 0;
	struct timeval tv;
	replay_type type = -1;
	int ok = 1, i = 0, eof;
	unsigned long count;
	char *user, *database, *statement, *name, **values, nl;

	debug(3, "Entering file_provider%s\n", "");

	/* read timestamp (8 byte) */
	if (! do_read(&u32, 4, &eof)) {
		ok = 0;
	} else {
		/* handle expected end-of-file condition */
		if (eof) {
			return end_item;
		}

		tv.tv_sec = ntohl(u32);
		if (! do_read(&u32, 4, NULL)) {
			ok = 0;
		} else {
			tv.tv_usec = ntohl(u32);
		}
	}

	/* read session_id (8 byte) */
	if (ok && do_read(&u64, 8, NULL)) {
		session_id = ntohll(u64);
	} else {
		ok = 0;
	}

	/* read type (1 byte) */
	if (ok) {
		u16 = 0;
		if (! do_read((char *)(&u16) + 1, 1, NULL)) {
			ok = 0;
		} else {
			type = ntohs(u16);
			if ((type < pg_connect) || (type > pg_cancel)) {
				fprintf(stderr, "Error: unknown type %u encountered\n", type);
				ok = 0;
			}
		}
	}

	/* read type specific stuff */
	if (ok) {
		switch (type) {
			case pg_connect:
				if (read_string(&user)) {
					if (read_string(&database)) {
						r = replay_create_connect(&tv, session_id, user, database);
						free(database);
					}
					free(user);
				}
				break;
			case pg_disconnect:
				r = replay_create_disconnect(&tv, session_id);
				break;
			case pg_execute:
				if (read_string(&statement)) {
					r = replay_create_execute(&tv, session_id, statement);
					free(statement);
				}
				break;
			case pg_prepare:
				if (read_string(&statement)) {
					if (read_string(&name)) {
						r = replay_create_prepare(&tv, session_id, name, statement);
						free(name);
					}
					free(statement);
				}
				break;
			case pg_exec_prepared:
				/* read statement name */
				if (read_string(&name)) {
					/* number of bind arguments (2 byte) */
					if (do_read(&u16, 2, NULL)) {
						count = ntohs(u16);
						if (NULL == (values = calloc(count, sizeof(char *)))) {
							fprintf(stderr, "Cannot allocate %lu bytes of memory\n", count * sizeof(char *));
						} else {
							/* read bind values */
							while (i < count) {
								if (read_string(values + i)) {
									++i;
								} else {
									break;
								}
							}
							if (i == count) {
								r = replay_create_exec_prepared(&tv, session_id, name, count, values);
							}
							while (--i >= 0) {
								if (values[i]) {
									free(values[i]);
								}
							}
							free(values);
						}
					}
					free(name);
				}
				break;
			case pg_cancel:
				r = replay_create_cancel(&tv, session_id);
				break;
		}
	}

	/* read new-line at the end of the record */
	if (r && do_read(&nl, 1, NULL) && ('\n' != nl)) {
		fprintf(stderr, "Error: missing new-line at end of line\n");
		if (r) {
			replay_free(r);
			r = NULL;
		}
	}

	if (r && (1 <= debug_level) && (end_item != r)) {
		replay_print_debug(r);
	}

	debug(3, "Leaving file_provider%s\n", "");

    return r;
}

int file_consumer_init(const char *outfile, const char *host, int port, const char *passwd, double factor) {
	debug(3, "Entering file_consumer_init%s\n", "");

	if ((NULL == outfile) || ('\0' == outfile[0])
			|| (('-' == outfile[0]) && ('\0' == outfile[1]))) {
		filed = 1;
#ifdef WINDOWS
		/* set stdout to binary mode */
		setmode(filed, O_BINARY);
#endif
	} else {
		if (-1 == (filed = open(outfile, O_WRONLY | O_CREAT | O_TRUNC
#ifdef WINDOWS
					| O_BINARY
#endif
					, FILE_MODE))) {
			perror("Error opening output file:");
			return 0;
		}
	}

	debug(3, "Leaving file_consumer_init%s\n", "");
	return 1;
}

void file_consumer_finish(int dry_run) {
	debug(3, "Entering file_consumer_finish%s\n", "");

	if (1 != filed) {
		if (close(filed)) {
			perror("Error closing output file:");
		}
	}

	debug(3, "Leaving file_consumer_finish%s\n", "");
}

int file_consumer(replay_item *item) {
	const struct timeval *tv = replay_get_time(item);
	uint16_t count;
	const replay_type type = replay_get_type(item);
	uint16_t u16, i;
	uint32_t u32;
	uint64_t u64;
	int rc = 1;
	const char * const *values;

	debug(3, "Entering file_consumer%s\n", "");

	/* write timestamp (8 byte) */
	u32 = htonl(tv->tv_sec);
	if (! do_write(&u32, 4)) {
		rc = -1;
	} else {
		u32 = htonl(tv->tv_usec);
		if (! do_write(&u32, 4)) {
			rc = -1;
		}
	}

	/* write session_id (8 byte) */
	if (1 == rc) {
		u64 = htonll(replay_get_session_id(item));
		if (! do_write(&u64, 8)) {
			rc = -1;
		}
	}

	/* write type (1 byte) */
	if (1 == rc) {
		u16 = htons((uint16_t) type);
		if (! do_write((char *)(&u16) + 1, 1)) {
			rc = -1;
		}
	}

	/* write type specific stuff */
	if (1 == rc) {
		switch (type) {
			case pg_connect:
				if (! write_string(replay_get_user(item))) {
					rc = -1;
				} else if (! write_string(replay_get_database(item))) {
					rc = -1;
				}
				break;
			case pg_disconnect:
				break;
			case pg_execute:
				if (! write_string(replay_get_statement(item))) {
					rc = -1;
				}
				break;
			case pg_prepare:
				if (! write_string(replay_get_statement(item))) {
					rc = -1;
				} else if (! write_string(replay_get_name(item))) {
					rc = -1;
				}
				break;
			case pg_exec_prepared:
				count = replay_get_valuecount(item);
				/* write statement name */
				if (! write_string(replay_get_name(item))) {
					rc = -1;
				} else {
					/* write count (2 byte) */
					u16 = htons(count);
					if (! do_write(&u16, 2)) {
						rc = -1;
					} else {
						/* write values */
						values = replay_get_values(item);
						for (i=0; i<count; ++i) {
							if (! write_string(values[i])) {
								rc = -1;
								break;
							}
						}
					}
				}
				break;
			case pg_cancel:
				break;
		}
	}

	/* write new-line (1 byte) */
	if (1 == rc) {
		if (! do_write("\n", 1)) {
			rc = -1;
		}
	}

	replay_free(item);

	debug(3, "Leaving file_consumer%s\n", "");
	return rc;
}
