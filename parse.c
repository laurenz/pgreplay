#include "pgreplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
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

/* long enough to contain the beginning of a log line */
#define BUFLEN 1024
/* separates log line entries */
#define SEPCHAR '|'

/* types of log entries */
typedef enum {
	log_debug5,
	log_debug4,
	log_debug3,
	log_debug2,
	log_debug1,
	log_info,
	log_notice,
	log_warning,
	log_error,
	log_log,
	log_fatal,
	log_panic,
	log_unknown
} log_type;

/* type for functions parsing the next log entry */
typedef int (parse_log_entry_func)(struct timeval *, char *, char *, uint64_t *, log_type *, char **, char **);
/* functions for parsing stderr and CSV log entries */
static parse_log_entry_func parse_errlog_entry;
static parse_log_entry_func parse_csvlog_entry;
static parse_log_entry_func * const parse_log_entry[2] = {
	&parse_errlog_entry,
	&parse_csvlog_entry
};

/* used to remember prepared statements */
struct prep_stmt {
	char *name;
	struct prep_stmt *next;
};
/* used to remember "open" connections */
struct connection {
	uint64_t session_id;
	struct connection *next;
	struct prep_stmt *statements;
};
/* hash structure for "open" connections */
static struct connection * open_conn[256] = { NULL };

/* indicates whether we are parsing a CSV file */
static int csv;
/* start and end timestamp for parsing log entries */
static const char *start_time, *end_time;
/* database and username filters for parsing log entries */
static const char *database_only, *username_only;
/* file which we parse */
static int infile;
/* line number for error messages */
static unsigned long lineno = 0;
/* offset for time values (what mktime(3) makes of 2000-01-01 00:00:00)
   used to make timestamps independent of local time and broken mktime */
static time_t epoch;
/* time of the first and last statement that we parse */
static struct timeval first_stmt_time, last_stmt_time;

/* statistics */
static unsigned long stat_simple = 0;     /* simple statements */
static unsigned long stat_copy = 0;       /* copy statements */
static unsigned long stat_param = 0;      /* parametrized statements */
static unsigned long stat_named = 0;      /* different named statements */
static unsigned long stat_execnamed = 0;  /* named statement executions */
static unsigned long stat_fastpath = 0;   /* fast-path function calls */
static unsigned long stat_cancel = 0;     /* cancel requests */

/* a version of strcpy that handles overlapping strings well */
static char *overlap_strcpy(char *dest, const char *src) {
	register char c;

	while((c = *(src++))) {
		*(dest++) = c;
	}
	*dest = '\0';

	return dest;
}

/* convert a string to a log_type */
static log_type to_log_type(const char* s) {
	/* compare in order of expected likelyhood for performance */
	if (! strcmp(s, "LOG")) {
		return log_log;
	} else if (! strcmp(s, "ERROR")) {
		return log_error;
	} else if (! strcmp(s, "STATEMENT")) {
		return log_unknown;
	} else if (! strcmp(s, "DETAIL")) {
		return log_unknown;
	} else if (! strcmp(s, "HINT")) {
		return log_unknown;
	} else if (! strcmp(s, "FATAL")) {
		return log_fatal;
	} else if (! strcmp(s, "WARNING")) {
		return log_warning;
	} else if (! strcmp(s, "NOTICE")) {
		return log_notice;
	} else if (! strcmp(s, "INFO")) {
		return log_info;
	} else if (! strcmp(s, "PANIC")) {
		return log_panic;
	} else if (! strcmp(s, "DEBUG1")) {
		return log_debug1;
	} else if (! strcmp(s, "DEBUG2")) {
		return log_debug2;
	} else if (! strcmp(s, "DEBUG3")) {
		return log_debug3;
	} else if (! strcmp(s, "DEBUG4")) {
		return log_debug4;
	} else if (! strcmp(s, "DEBUG5")) {
		return log_debug5;
	} else {
		return log_unknown;
	}
}

/* Parses a timestamp (ignoring the time zone part).
   If "dest" is not null, the parsed time will be returned there.
   Return value is NULL on success, else an error message */

const char * parse_time(const char *source, struct timeval *dest) {
	int i;
	static struct tm tm;  /* initialize with zeros */
	char s[24] = { '\0' };  /* modifiable copy of source */
	static char errmsg[BUFLEN];
	/* format of timestamp part */
	static const char format[]="nnnn-nn-nn nn:nn:nn.nnn";

	/* check timestamp for validity */
	if (!source) {
		strcpy(errmsg, "NULL passed as timestamp string");
		return errmsg;
	}

	if (strlen(source) < strlen(format)) {
		sprintf(errmsg, "timestamp string is less than %lu characters long", (unsigned long)strlen(format));
		return errmsg;
	}

	if (strlen(source) >= BUFLEN) {
		sprintf(errmsg, "timestamp string is more than %d characters long", BUFLEN-1);
		return errmsg;
	}

	for (i=0; i<strlen(format); ++i) {
		switch (format[i]) {
			case 'n':
				if ((source[i] < '0') || (source[i] > '9')) {
					sprintf(errmsg, "character %d in timestamp string is '%c', expected digit", i+1, source[i]);
					return errmsg;
				} else
					s[i] = source[i];
				break;
			default:
				if (source[i] != format[i]) {
					sprintf(errmsg, "character %d in timestamp string is '%c', expected '%c'", i+1, source[i], format[i]);
					return errmsg;
				} else
					s[i] = '\0';  /* tokenize parts */
		}
	}

	/* parse time into 'tm' */
	tm.tm_year  = atoi(s) - 1900;
	tm.tm_mon   = atoi(s + 5) - 1;
	tm.tm_mday  = atoi(s + 8);
	tm.tm_hour  = atoi(s + 11);
	tm.tm_min   = atoi(s + 14);
	tm.tm_sec   = atoi(s + 17);
	tm.tm_isdst = 0;  /* ignore daylight savings time */

	if (dest) {
		dest->tv_sec  = mktime(&tm) - epoch;
		dest->tv_usec = atoi(s + 20) * 1000;
	}

	return NULL;
}

static char * parse_session(const char *source, uint64_t *dest) {
	char s[BUFLEN];  /* modifiable copy of source */
	static char errmsg[BUFLEN];
	char *s1 = NULL, c;
	uint32_t part1, part2;
	int i;

	/* check input for validity */
	if (!source) {
		strcpy(errmsg, "NULL passed as session id string");
		return errmsg;
	}

	if (strlen(source) > BUFLEN -1) {
		sprintf(errmsg, "session id string is more than %d characters long", BUFLEN);
		return errmsg;
	}

	for (i=0; i<=strlen(source); ++i) {
		c = source[i];
		if (('.' == c) && (! s1)) {
			s[i] = '\0';
			s1 = s + i + 1;
		} else if (((c < '0') || (c > '9')) && ((c < 'a') || (c > 'f')) && ('\0' != c)) {
			sprintf(errmsg, "character %d in session id string is '%c', expected hex digit", i+1, c);
			return errmsg;
		} else
			s[i] = c;
	}

	if (! s1) {
		strcpy(errmsg, "Missing \".\" in session id string");
		return errmsg;
	}

	if ((strlen(s) > 8) || (strlen(s1) > 8)) {
		strcpy(errmsg, "none of the parts of a session id string may be longer than 8 hex digits");
		return errmsg;
	}

	/* convert both parts */
	sscanf(s, UINT32_FORMAT, &part1);
	sscanf(s1, UINT32_FORMAT, &part2);
	*dest = (((uint64_t)part1) << 32) + part2;

	return NULL;
}

/* reads one log entry from the input file
   the result is a malloc'ed string that must be freed
   a return value of NULL means that there was an error */

static char * read_log_line() {
	char *line, buf[BUFLEN] = { '\0' }, *p;
	int len, escaped = 0, nl_found = 0, line_size = 0, i, l;
	ssize_t bytes_read;
	/* this will contain stuff we have read from the file but not used yet */
	static char peekbuf[BUFLEN] = { '\0' };
	static int peeklen = 0;

	debug(3, "Entering read_log_line, current line number %lu\n", lineno+1);

	/* pre-allocate the result to length 1 */
	if (NULL == (line = malloc(1))) {
		fprintf(stderr, "Cannot allocate 1 byte of memory\n");
		return NULL;
	}
	*line = '\0';

	while (! nl_found) {
		/* if there were any chars left from the last invokation, use them first */
		len = peeklen;
		if (len) {
			strcpy(buf, peekbuf);
			peekbuf[0] = '\0';
		}
		peeklen = 0;

		/* read from file until buf is full (at most) */
		if (len < BUFLEN - 1) {
			if (-1 == (bytes_read = read(infile, buf + len, BUFLEN - 1 - len))) {
				perror("Error reading from input file");
				return NULL;
			}
			len += bytes_read;
			buf[len] = '\0';
		}

		/* if there is still nothing, we're done */
		if (0 == len) {
			debug(2, "Encountered EOF%s\n", "");
			debug(3, "Leaving read_log_line%s\n", "");
			return line;
		}

		/* search the string for unescaped newlines */
		for (p=buf; *p!='\0'; ++p) {
			if (csv && ('"' == *p)) {
				escaped = !escaped;
			}
			/* keep up with line count */
			lineno += ('\n' == *p);

			/* look for unescaped newline */
			if (!escaped && ('\n' == *p)) {
				/* if a newline is found, truncate the string
				   and prepend the rest to peekbuf */
				l = len - (++p - buf);
				/* right shift peekbuf by l */
				for (i=peeklen; i>=0; --i) {
					peekbuf[l+i] = peekbuf[i];
				}
				strncpy(peekbuf, p, l);
				*p = '\0';
				peeklen += len - (p - buf);
				len = p - buf;
				if (csv) {
					/* for a CSV file, this must be the end of the log entry */
					nl_found = 1;
					break;  /* out from the for loop */
				} else {
					/* in a stderr log file, we must check for a
					   continuation line (newline + tab) */
					/* first, make sure there is something to peek at */
					if (0 == peeklen) {
						/* try to read one more byte from the file */
						if (-1 == (bytes_read = read(infile, peekbuf, 1))) {
							perror("Error reading from input file");
							return NULL;
						}
						if (0 == bytes_read) {
							/* EOF means end of log entry */
							nl_found = 1;
							break;  /* out from the for loop */
						} else {
							peeklen = bytes_read;
							peekbuf[peeklen] = '\0';
						}
					}
					/* then check for a continuation tab */
					if ('\t' == *peekbuf) {
						/* continuation line, remove tab
						   and copy peekbuf back to buf */
						strncpy(p--, peekbuf + 1, BUFLEN - 1 - len);
						if (peeklen > BUFLEN - len) {
							overlap_strcpy(peekbuf, peekbuf + (BUFLEN - len));
							peeklen = peeklen - BUFLEN + len;
							len = BUFLEN - 1;
						} else {
							*peekbuf = '\0';
							len += peeklen - 1;
							peeklen = 0;
						}
						buf[len] = '\0';
					} else {
						/* end of log entry reached */
						nl_found = 1;
						break;  /* out from the for loop */
					}
				}
			}
		}
		/* extend result line and append buf */
		line_size += len;
		if (NULL == (p = realloc(line, line_size+1))) {
			fprintf(stderr, "Cannot allocate %d bytes of memory\n", line_size);
			free(line); line = NULL;
			return NULL;
		}
		line = p;
		strcat(line, buf);
		*buf = '\0';
		len = 0;
	}

	/* remove trailing newline in result if present */
	if ('\n' == line[line_size - 1]) {
		line[line_size - 1] = '\0';
	}

	debug(3, "Leaving read_log_line%s\n", "");

	return line;
}

/* parses the next stderr log entry (and maybe a detail message after that)
   timestamp, user, database, session ID, log message type, log message
   and detail message are returned in the respective parameters
   "message" and "detail" are malloc'ed if they are not NULL
   return values: -1 (error), 0 (end-of-file), or 1 (success) */

static int parse_errlog_entry(struct timeval *time, char *user, char *database, uint64_t *session_id, log_type *type, char **message, char **detail) {
	char *line = NULL, *part2, *part3, *part4, *part5, *part6;
	const char *errmsg;
	int i, skip_line = 0;
	static int dump_found = 0;
	/* if not NULL, contains the next log entry to parse */
	static char* keepline = NULL;

	debug(3, "Entering parse_errlog_entry%s\n", "");

	/* initialize message and detail with NULL */
	*message = NULL;
	*detail = NULL;

	/* use cached line or read next line from log file */
	if (keepline) {
		line = keepline;
		keepline = NULL;
	} else {
		/* read lines until we are between start_time and end_time */
		do {
			if (line) {
				free(line);
			}
			if (NULL == (line = read_log_line())) {
				return -1;
			}

			/* is it the start of a memory dump? */
			if (0 == strncmp(line, "TopMemoryContext: ", 18)) {
				fprintf(stderr, "Found memory dump in line %lu\n", lineno);
				dump_found = 1;
				skip_line = 1;
			} else {
				/* if there is a dump and the line starts blank,
				   assume the line is part of the dump
				*/
				if (dump_found && (' ' == *line)) {
					skip_line = 1;
				} else {
					skip_line = 0;
				}
			}
		} while (('\0' != *line)
			&& (skip_line
				|| (start_time && (strncmp(line, start_time, 23) < 0))));
	}

	/* check for EOF */
	if (('\0' == *line) || (end_time && (strncmp(line, end_time, 23) > 0))) {
		free(line);
		debug(3, "Leaving parse_errlog_entry%s\n", "");
		return 0;
	}

	/* split line on | in six pieces: time, user, database, session ID, log entry type, rest */
	if (NULL == (part2 = strchr(line, SEPCHAR))) {
		fprintf(stderr, "Error parsing line %lu: no \"%c\" found - log_line_prefix may be wrong\n", lineno, SEPCHAR);
		free(line);
		return -1;
	} else {
		*(part2++) = '\0';
	}

	if (NULL == (part3 = strchr(part2, SEPCHAR))) {
		fprintf(stderr, "Error parsing line %lu: second \"%c\" not found - log_line_prefix may be wrong\n", lineno, SEPCHAR);
		free(line);
		return -1;
	} else {
		*(part3++) = '\0';
	}

	if (NULL == (part4 = strchr(part3, SEPCHAR))) {
		fprintf(stderr, "Error parsing line %lu: third \"%c\" not found - log_line_prefix may be wrong\n", lineno, SEPCHAR);
		free(line);
		return -1;
	} else {
		*(part4++) = '\0';
	}

	if (NULL == (part5 = strchr(part4, SEPCHAR))) {
		fprintf(stderr, "Error parsing line %lu: fourth \"%c\" not found - log_line_prefix may be wrong\n", lineno, SEPCHAR);
		free(line);
		return -1;
	} else {
		*(part5++) = '\0';
	}

	if (NULL == (part6 = strstr(part5, ":  "))) {
		fprintf(stderr, "Error parsing line %lu: log message does not begin with a log type\n", lineno);
		free(line);
		return -1;
	} else {
		*part6 = '\0';
		part6 += 3;
	}

	/* first part is the time, parse it into parameter */
	if ((errmsg = parse_time(line, time))) {
		fprintf(stderr, "Error parsing line %lu: %s\n", lineno, errmsg);
		free(line);
		return -1;
	}

	/* second part is the username, copy to parameter */
	if (NAMELEN < strlen(part2)) {
		fprintf(stderr, "Error parsing line %lu: username exceeds %d characters\n", lineno, NAMELEN);
		free(line);
		return -1;
	} else {
		strcpy(user, part2);
	}

	/* third part is the database, copy to parameter */
	if (NAMELEN < strlen(part3)) {
		fprintf(stderr, "Error parsing line %lu: database name exceeds %d characters\n", lineno, NAMELEN);
		free(line);
		return -1;
	} else {
		strcpy(database, part3);
	}

	/* fourth part is the session ID, copy to parameter */
	if ((errmsg = parse_session(part4, session_id))) {
		fprintf(stderr, "Error parsing line %lu: %s\n", lineno, errmsg);
		free(line);
		return -1;
	}

	/* fifth part is the log type, copy to parameter */
	*type = to_log_type(part5);

	/* sixth part is the log message */
	overlap_strcpy(line, part6);
	*message = line;

	/* read the next log entry so that we can peek at it */
	line = NULL;
	do {
		if (NULL != line) {
			free(line);
		}
		if (NULL == (line = read_log_line())) {
			free(*message);
			*message = NULL;
			return -1;
		}

		/* is it the start of a memory dump? */
		if (0 == strncmp(line, "TopMemoryContext: ", 18)) {
			fprintf(stderr, "Found memory dump in line %lu\n", lineno);
			dump_found = 1;
			skip_line = 1;
		} else {
			/* if there is a dump and the line starts blank,
			   assume the line is part of the dump
			*/
			if (dump_found && (' ' == *line)) {
				skip_line = 1;
			} else {
				skip_line = 0;
			}
		}
	} while (('\0' != *line) && skip_line);

	if ('\0' == *line) {
		/* EOF, that's ok */
		keepline = line;
	} else {
		/* skip four | to the fifth part */
		part2 = line;
		for (i=0; i<4; ++i) {
			if (NULL == (part2 = strchr(part2, SEPCHAR))) {
				fprintf(stderr, "Error parsing line %lu: only %d \"%c\" found - log_line_prefix may be wrong\n", lineno, i, SEPCHAR);
				free(*message);
				free(line);
				*message = NULL;
				return -1;
			} else {
				++part2;
			}
		}

		/* check if it is a DETAIL */
		if (strncmp(part2, "DETAIL:  ", 9)) {
			/* if not, remember the line for the next pass */
			keepline = line;
		} else {
			debug(2, "Found a DETAIL message%s\n", "");

			/* set the return parameter to the detail message */
			overlap_strcpy(line, part2 + 9);
			*detail = line;
		}
	}

	debug(3, "Leaving parse_errlog_entry%s\n", "");
	return 1;
}

/* parses the next CSV log entry
   timestamp, user, database, session ID, log message type, log message
   and detail message are returned in the respective parameters
   "message" is malloc'ed, "detail" not
   return values: -1 (error), 0 (end-of-file), or 1 (success) */

static int parse_csvlog_entry(struct timeval *time, char *user, char *database, uint64_t *session_id, log_type *type, char **message, char **detail) {
	char *line = NULL, *part[16], *p1, *p2;
	const char *errmsg;
	int i, escaped = 0;

	debug(3, "Entering parse_csvlog_entry%s\n", "");

	/* initialize message and detail with NULL */
	*message = NULL;
	*detail = NULL;

	/* read next line after start timestamp from log file */
	do {
		if (line) {
			free(line);
		}
		if (NULL == (line = read_log_line())) {
			return -1;
		}
	} while (('\0' != *line)
		&& (start_time && (strncmp(line, start_time, 23) < 0)));

	/* check for EOF */
	if (('\0' == *line) || (end_time && (strncmp(line, end_time, 23) > 0))) {
		free(line);
		debug(3, "Leaving parse_errlog_entry%s\n", "");
		return 0;
	}

	/* parse first 15 parts from the CSV record */
	part[0] = p1 = line;
	for (i=1; i<16; ++i) {
		p2 = p1;
		/* copy p1 to p2 until we hit an unescaped comma,
		   remove escaping double quotes */
		while (escaped || (',' != *p1)) {
			switch (*p1) {
				case '\0':
					fprintf(stderr, "Error parsing line %lu: comma number %d not found (or unmatched quotes)\n", lineno, i);
					free(line);
					return -1;
				case '"':
					/* don't copy the first double quote */
					if (!escaped && (p1 != part[i-1])) {
						*(p2++) = '"';
					}
					++p1;
					escaped = !escaped;
					break;
				default:
					*(p2++) = *(p1++);
			}
		}
		*p2 = '\0';
		part[i] = ++p1;
	}

	/* first part is the time, parse it into parameter */
	if ((errmsg = parse_time(part[0], time))) {
		fprintf(stderr, "Error parsing line %lu: %s\n", lineno, errmsg);
		free(line);
		return -1;
	}

	/* second part is the username, copy to parameter */
	if (NAMELEN < strlen(part[1])) {
		fprintf(stderr, "Error parsing line %lu: username exceeds %d characters\n", lineno, NAMELEN);
		free(line);
		return -1;
	} else {
		strcpy(user, part[1]);
	}

	/* third part is the database, copy to parameter */
	if (NAMELEN < strlen(part[2])) {
		fprintf(stderr, "Error parsing line %lu: database name exceeds %d characters\n", lineno, NAMELEN);
		free(line);
		return -1;
	} else {
		strcpy(database, part[2]);
	}

	/* sixth part is the session ID, copy to parameter */
	if ((errmsg = parse_session(part[5], session_id))) {
		fprintf(stderr, "Error parsing line %lu: %s\n", lineno, errmsg);
		free(line);
		return -1;
	}

	/* twelfth part is the log type, copy to parameter */
	*type = to_log_type(part[11]);

	/* fourteenth part is the message, assign to output parameter */
	overlap_strcpy(line, part[13]);
	*message = line;

	/* detail is the fifteenth part of the line, if not empty */
	*detail = part[14];
	if ('\0' == **detail) {
		*detail = NULL;
	}

	debug(3, "Leaving parse_csvlog_entry%s\n", "");
	return 1;
}

/* add (malloc) the prepared statement name to the list of
   prepared statements for the connection
   returns 0 if the statement already existed, 1 if it was added and -1 if there was an error */

static int add_pstmt(struct connection * conn, char const *name) {
	struct prep_stmt *pstmt = conn->statements;
	int rc;

	debug(3, "Entering add_pstmt for statement \"%s\"\n", name);

	if ('\0' == *name) {
		/* the empty statement will never be stored, but should be prepared */
		rc = 1;

		/* count for statistics */
		++stat_param;
	} else {
		while (pstmt && strcmp(pstmt->name, name)) {
			pstmt = pstmt->next;
		}

		if (pstmt) {
			/* statement already prepared */
			debug(2, "Prepared statement is already in list%s\n", "");
			rc = 0;
		} else {
			debug(2, "Adding prepared statement to list%s\n", "");
			/* add statement name to linked list */
			if (NULL == (pstmt = malloc(sizeof(struct prep_stmt)))) {
				fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)sizeof(struct prep_stmt));
				return -1;
			}
			if (NULL == (pstmt->name = malloc(strlen(name) + 1))) {
				fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)strlen(name) + 1);
				free(pstmt);
				return -1;
			}
			strcpy(pstmt->name, name);
			pstmt->next = conn->statements;
			conn->statements = pstmt;
			rc = 1;

			/* count for statistics */
			++stat_named;
		}
		/* count for statistics */
		++stat_execnamed;
	}

	debug(3, "Leaving add_pstmt%s\n", "");
	return rc;
}

/* remove (free) the prepared statement name to the list of
   prepared statements for the connection */

static void remove_pstmt(struct connection * conn, char const *name) {
	struct prep_stmt *pstmt = conn->statements, *pstmt2 = NULL;

	debug(3, "Entering remove_pstmt for statement \"%s\"\n", name);

	while (pstmt && strcmp(pstmt->name, name)) {
		pstmt2 = pstmt;  /*remember previous */
		pstmt = pstmt->next;
	}

	if (pstmt) {
		if (pstmt2) {
			pstmt2->next = pstmt->next;
		} else {
			conn->statements = pstmt->next;
		}
		free(pstmt->name);
		free(pstmt);
	} else {
		debug(2, "Prepared statement not found%s\n", "");
	}

	debug(3, "Leaving remove_pstmt%s\n", "");
	return;
}

static void remove_all_pstmts(struct connection * conn) {
	struct prep_stmt *pstmt = conn->statements, *pstmt2 = NULL;

	debug(3, "Entering remove_all_pstmts%s\n", "");

	while(pstmt) {
		pstmt2 = pstmt;
		pstmt = pstmt->next;
		free(pstmt2->name);
		free(pstmt2);
	}

	debug(3, "Leaving remove_all_pstmts%s\n", "");
	return;
}

/* remove all "COPY" and "SET client_encoding" statements;
   for DEALLOCATE statements, try to remove prepared statement */

/* maximum number of tokens we need to analyze a statement */
#define MAX_TOKENS 3

static int filter_bad_statements(char *line, struct connection *conn) {
	char *statement = line, *p = line, token[MAX_TOKENS][NAMELEN + 1],
		*q = NULL, *quote, *h;
	int comment_depth, tokens = 0, ok = 1, i, nameindex, quotelen;

	debug(3, "Entering filter_bad_statements%s\n", "");

	for (i=0; i<MAX_TOKENS; ++i) {
		token[i][0] = '\0';
	}

	while (ok) {
		if (('\0' == *p) || (';' == *p)) {
			/* end of a statement found */
			if (tokens > 0) {
				/* count parsed simple statements */
				++stat_simple;

				/* remove statements that won't work */

				if (! strcmp("copy", token[0])) {
					fprintf(stderr, "Warning: COPY statement ignored in line %lu\n", lineno);
					/* replace statement with blanks */
					while (statement < p) {
						*(statement++) = ' ';
					}

					/* count for statistics */
					++stat_copy;
				} else if ((tokens > 1) && (! strcmp("set", token[0])) && (! strcmp("client_encoding", token[1]))) {
					fprintf(stderr, "Warning: \"SET client_encoding\" statement ignored in line %lu\n", lineno);
					/* replace statement with blanks */
					while (statement < p) {
						*(statement++) = ' ';
					}
				} else if (! strcmp("deallocate", token[0])) {
					/* there coule be a "prepare" in the second token, should be ignored */
					if (strcmp("prepare", token[1])) {
						nameindex = 1;
					} else {
						nameindex = 2;
					}
					if (strcmp("all", token[nameindex])) {
						/* deallocate single statement */
						debug(2, "Deallocating prepared statement \"%s\"\n", token[nameindex]);
						remove_pstmt(conn, token[nameindex]);
					} else {
						/* deallocate all prepared statements */
						debug(2, "Deallocating all prepared statements%s\n", "");
						remove_all_pstmts(conn);
					}
				}
			}

			/* break out of loop if end-of-line is reached */
			if ('\0' == *p) {
				break;
			}

			/* else prepare for next statement */
			statement = ++p;
			for (i=0; i<MAX_TOKENS; ++i) {
				token[i][0] = '\0';
			}
			tokens = 0;
		} else if ((('E' == *p) || ('e' == *p)) && ('\'' == p[1])) {
			/* special string constant; skip to end */
			++p;
			while ('\0' != *(++p)) {
				if ('\'' == *p) {
					if ('\'' == p[1]) {
						/* regular escaped apostrophe */
						++p;
					} else {
						break;
					}
				}
				if (('\\' == *p) && (('\'' == p[1]) || ('\\' == p[1]))) {
					/* backslash escaped apostrophe or backslash */
					++p;
				}
			}
			if ('\0' == *p) {
				fprintf(stderr, "Error: string literal not closed near line %lu\n"
					"Hint: retry with%s the -q option\n",
					lineno, (backslash_quote ? "out" : ""));
				ok = 0;
			} else {
				++p;
			}
		} else if ('\'' == *p) {
			/* simple string constant; skip to end */
			while ('\0' != *(++p)) {
				if ('\'' == *p) {
					if ('\'' == p[1]) {
						/* regular escaped apostrophe */
						++p;
					} else {
						break;
					}
				}
				if (backslash_quote && ('\\' == *p) && (('\'' == p[1]) || ('\\' == p[1]))) {
					/* backslash escaped apostrophe or backslash */
					++p;
				}
			}
			if ('\0' == *p) {
				fprintf(stderr, "Error: string literal not closed near line %lu\n"
					"Hint: retry with%s the -q option\n",
					lineno, (backslash_quote ? "out" : ""));
				ok = 0;
			} else {
				++p;
			}
		} else if (('$' == *p) && (('0' > *(p+1)) || (('9' < *(p+1))))) {
			/* dollar quoted string constant; skip to end */
			quote = p++;
			while (('$' != *p) && ('\0' != *p)) {
				++p;
			}
			if ('\0' == *p) {
				fprintf(stderr, "Error: end of dollar quote not found in line %lu\n", lineno);
				ok = 0;
			} else {
				quotelen = p - quote;
				*p = '\0';
				h = p;
				do {
					h = strstr(++h, quote);
				} while ((NULL != h) && ('$' != *(h + quotelen)));
				*p = '$';
				if (NULL == h) {
					fprintf(stderr, "Error: end of dollar quoted string found in line %lu\n", lineno);
					ok = 0;
				} else {
					p = h + (quotelen + 1);
				}
			}
		} else if (('-' == *p) && ('-' == p[1])) {
			/* comment; skip to end of line or statement */
			while (('\n' != *p) && ('\0' != *p)) {
				++p;
			}
		} else if (('/' == *p) && ('*' == p[1])) {
			/* comment, skip to matching end */
			p += 2;
			comment_depth = 1;  /* comments can be nested */
			while (0 != comment_depth) {
				if ('\0' == *p) {
					fprintf(stderr, "Error: comment not closed in line %lu\n", lineno);
					ok = 0;
					break;
				} else if (('*' == *p) && ('/' == p[1])) {
					--comment_depth;
					p += 2;
				} else if (('/' == *p) && ('*' == p[1])) {
					++comment_depth;
					p += 2;
				} else {
					++p;
				}
			}
		} else if ('"' == *p) {
			/* quoted identifier, copy to token if necessary */
			if (tokens < MAX_TOKENS) {
				q = token[tokens];
			}
			while (1) {
				++p;
				if ('\0' == *p) {
					fprintf(stderr, "Error: quoted identifier not closed in line %lu\n", lineno);
					ok = 0;
					break;
				} else if ('"' == *p) {
					if ('"' == p[1]) {
						/* double " means a single " in a quoted identifier */
						if ((tokens < MAX_TOKENS) && (q - token[tokens] < NAMELEN)) {
							*(q++) = '"';
						}
						++p;
					} else {
						/* end of token */
						if (tokens < MAX_TOKENS) {
							*q = '\0';
							++tokens;
						}
						++p;
						break;
					}
				} else {
					/* normal character, copy to token */
					if ((tokens < MAX_TOKENS) && (q - token[tokens] < NAMELEN)) {
						*(q++) = *p;
					}
				}
			}
		} else if ((('A' <= *p) && ('Z' >= *p))
				|| (('a' <= *p) && ('z' >= *p))
				|| (127 < (unsigned char)(*p))  /* consider > 127 as letter */
				|| ('_' == *p)) {
			/* normal identifier, copy to token if necessary */
			if (tokens < MAX_TOKENS) {
				q = token[tokens];
			}
			while ((('A' <= *p) && ('Z' >= *p))
					|| (('a' <= *p) && ('z' >= *p))
					|| (('0' <= *p) && ('9' >= *p))
					|| (127 < (unsigned char)(*p))  /* consider > 127 as letter */
					|| ('_' == *p) || ('$' == *p)) {
				if ((tokens < MAX_TOKENS) && (q - token[tokens] < NAMELEN)) {
					/* convert to lowercase */
					*(q++) = *p + ('a' - 'A') * ((*p >= 'A') && (*p <= 'Z'));
				}
				++p;
			}
			*q = '\0';
			++tokens;
		} else {
			/* everything else is considered unimportant */
			++p;
		}
	}

	debug(3, "Leaving filter_bad_statements%s\n", "");
	return ok;
}

/* check if there is a connection for this session_id
   if not, a new connection replay_item is generated and returned in r
   return values: found or created hash entry for success, NULL for failure */

static struct connection *add_connection(replay_item **r, struct timeval *time, const char *user, const char *database, uint64_t session_id) {
	unsigned char hash = hash_session(session_id);
	struct connection *conn = open_conn[hash];

	debug(3, "Entering add_connection for session 0x" UINT64_FORMAT "\n", session_id);

	while (conn && (conn->session_id != session_id)) {
		conn = conn->next;
	}

	if (conn) {
		/* session already exists */
		*r = NULL;
	} else {
		/* session doesn't exist yet; create it and add it to hash table */
		if (NULL == (*r = replay_create_connect(time, session_id, user, database))) {
			/* something went wrong */
			return NULL;
		} else {
			if ((conn = malloc(sizeof(struct connection)))) {
				conn->next = open_conn[hash];
				conn->statements = NULL;
				conn->session_id = session_id;
				open_conn[hash] = conn;
			} else {
				fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)sizeof(struct connection));
				replay_free(*r);
				return NULL;
			}
		}
	}

	debug(3, "Leaving add_connection%s\n", "");
	return conn;
}

/* remove a connection from the hash structure
   returns 1 for success, 0 for failure        */

static int remove_connection(uint64_t session_id) {
	unsigned char hash = hash_session(session_id);
	struct connection *conn = open_conn[hash], *conn2 = NULL;

	debug(3, "Entering remove_connection for session 0x" UINT64_FORMAT "\n", session_id);

	while (conn && (conn->session_id != session_id)) {
		conn2 = conn; /* remember previous */
		conn = conn->next;
	}

	if (conn) {
		remove_all_pstmts(conn);
		if (conn2) {
			conn2->next = conn->next;
		} else {
			open_conn[hash] = conn->next;
		}
		free(conn);
	}

	debug(3, "Leaving remove_connection%s\n", "");

	return (NULL != conn);
}

/* tokenizes the "line" into single arguments, pointers to which
   are stored in "result" (which is malloc'ed and must be freed)
   returns the number of arguments and -1 if there is an error
   Note that the strings that "result" points to are *not* malloc'ed,
   but part of "line" */

static int parse_bind_args(char *** const result, char *line) {
	int count = 0;
	char *p, *p1;

	*result = NULL;

	debug(3, "Entering parse_bind_args%s\n", "");

	/* count the dollar signs in the line = upper limit for number of arguments */
	for (p=line; '\0'!=*p; ++p) {
		if ('$' == *p) {
			++count;
		}
	}
	/* if there is none, we are done */
	if (! count) {
		return 0;
	}

	/* allocate result */
	if (NULL == (*result = calloc(count, sizeof(char **)))) {
		fprintf(stderr, "Cannot allocate %lu bytes of memory\n", (unsigned long)sizeof(char **));
		return -1;
	}

	/* reset count - now we will really count */
	count = 0;

	/* loop until end of the line */
	p = line;
	while ('\0' != *p) {
		/* expect "$<number> = " */
		if ('$' != *(p++)) {
			fprintf(stderr, "Error: no dollar sign found in argument %d in line %lu\n", count, lineno);
			free(*result);
			*result = NULL;
			return -1;
		}
		while (('0' <= *p) && ('9' >= *p)) {
			++p;
		}
		if ((' ' != p[0]) || ('=' != p[1]) || (' ' != p[2])) {
			fprintf(stderr, "Error: bad format in argument %d in line %lu\n", count, lineno);
			free(*result);
			*result = NULL;
			return -1;
		}
		p += 3;

		if ('\'' == *p) {
			/* normal argument in single quotes */
			(*result)[count] = p1 = ++p;

			/* eliminate double single quotes and search for ending single quote */
			while (*p1) {
				if ('\0' == *p) {
					fprintf(stderr, "Error: unexpected end of line in argument %d in line %lu\n", count, lineno);
					free(*result);
					*result = NULL;
					return -1;
				}
				if ('\'' == *p) {
					/* single quote encountered */
					if ('\'' == p[1]) {
						/* double single quote, skip one */
						*(p1++) = '\'';
						++p;
					} else {
						/* end of argument */
						*p1 = '\0';
					}
				} else {
					/* normal character */
					*(p1++) = *p;
				}
				++p;
			}
		} else {
			/* must me NULL argument */
			if (('N' == p[0]) && ('U' == p[1]) && ('L' == p[2]) && ('L' == p[3])) {
				(*result)[count] = NULL;
				p += 4;
			} else {
				fprintf(stderr, "Error: expected NULL in argument %d in line %lu\n", count, lineno);
				free(*result);
				*result = NULL;
				return -1;
			}
		}

		/* skip ", " if present */
		if ((',' == p[0]) && (' ' == p[1])) {
			p += 2;
		}

		++count;
	}

	debug(2, "Parsed %d arguments\n", count);
	debug(3, "Leaving parse_bind_args%s\n", "");

	return count;
}

static void print_parse_statistics() {
	int hours, minutes;
	double seconds;
	struct timeval delta;

	fprintf(sf, "\nParse statistics\n");
	fprintf(sf, "================\n\n");
	fprintf(sf, "Log lines read: %lu\n", lineno);
	fprintf(sf, "Total SQL statements processed: %lu\n", stat_simple + stat_param + stat_execnamed);
	fprintf(sf, "Simple SQL statements processed: %lu\n", stat_simple);
	if (stat_copy) {
		fprintf(sf, "(includes %lu ignored copy statements)\n", stat_copy);
	}
	fprintf(sf, "Parametrized SQL statements processed: %lu\n", stat_param);
	fprintf(sf, "Named prepared SQL statements executions processed: %lu\n", stat_execnamed);
	if (stat_named) {
		fprintf(sf, "Different named prepared SQL statements processed: %lu\n", stat_named);
		fprintf(sf, "(average reuse count %.3f)\n", (double)(stat_execnamed - stat_named) / stat_named);
	}
	fprintf(sf, "Cancel requests processed: %lu\n", stat_cancel);
	fprintf(sf, "Fast-path function calls ignored: %lu\n", stat_fastpath);

	/* calculate lengh of the recorded workload */
	timersub(&last_stmt_time, &first_stmt_time, &delta);
	hours = delta.tv_sec / 3600;
	delta.tv_sec -= hours * 3600;
	minutes = delta.tv_sec / 60;
	delta.tv_sec -= minutes * 60;
	seconds = delta.tv_usec / 1000000.0 + delta.tv_sec;

	fprintf(sf, "Duration of recorded workload:");
	if (hours > 0) {
		fprintf(sf, " %d hours", hours);
	}
	if (minutes > 0) {
		fprintf(sf, " %d minutes", minutes);
	}
	fprintf(sf, " %.3f seconds\n", seconds);
}

int parse_provider_init(const char *in, int parse_csv, const char *begin, const char *end, const char *db_only, const char *usr_only) {
	static struct tm tm;  /* initialize with zeros */
	int rc = 1;

	debug(3, "Entering parse_provider_init%s\n", "");

	if (NULL == in) {
		infile = 0;
	} else {
		if (-1 == (infile = open(in, O_RDONLY))) {
			perror("Error opening input file");
			rc = 0;
		}
	}

	csv = parse_csv;
	start_time = begin;
	end_time = end;
	database_only = db_only;
	username_only = usr_only;

	/* initialize epoch with 2000-01-01 00:00:00 */
	tm.tm_year  = 2000 - 1900;
	tm.tm_mon   = 1 - 1;
	tm.tm_mday  = 1;
	tm.tm_hour  = 0;
	tm.tm_min   = 0;
	tm.tm_sec   = 0;
	tm.tm_isdst = 0;  /* ignore daylight savings time */
	epoch = mktime(&tm);

	debug(3, "Leaving parse_provider_init%s\n", "");

	return rc;
}

void parse_provider_finish() {
	debug(3, "Entering parse_provider_finish%s\n", "");

	if (0 != infile) {
		if (close(infile)) {
			perror("Error closing input file:");
		}
	}

	if (sf) {
		print_parse_statistics();
	}

	debug(3, "Leaving parse_provider_finish%s\n", "");
}

/* the replay_item is malloc'ed and must be freed with replay_free()
   will return NULL if an error occurred */

replay_item * parse_provider() {
	replay_item *r = NULL;
	char *message = NULL, *detail = NULL, *statement = NULL, *namep = NULL, name[NAMELEN + 1], **args, user[NAMELEN + 1], database[NAMELEN + 1], quote_name[NAMELEN + 3];
	log_type logtype;
	uint64_t session_id;
	int count, i;
	/* possible stati: -1 = error, 0 = looking for log line, 1 = interesting line found
	                   2 = using cached value, 3 = EOF */
	int status = 0;
	replay_type type = -1;  /* -1 is an impossible value */
	struct timeval time;
	/* remember time from last parsed line */
	static struct timeval oldtime = { 0, 0 };
	struct connection *conn = NULL;
	/* queue of up to two replay items */
	static replay_item *queue[2] = { NULL, NULL };
	static int first_stmt_time_set = 0;

	debug(3, "Entering parse_provider%s\n", "");

	if (queue[0]) {
		/* if there is something in the queue, return it */
		debug(2, "Queue is not empty, returning top element%s\n", "");
		r = queue[0];
		queue[0] = queue[1];
		queue[1] = NULL;

		status = 2;
	}

	/* read a log entry until we find an interesting one */
	while (0 == status) {
		switch ((*parse_log_entry[csv])(&time, user, database, &session_id, &logtype, &message, &detail)) {
			case 0:
				/* EOF encountered */
				status = 3;
				break;
			case 1:
				/* Do not process line that does not match any of the specified users and databases */
				memset(quote_name, '\0', NAMELEN + 3);
				if ('\0' != *database) {
					quote_name[0] = '\\';
					strcat(quote_name, database);
					strcat(quote_name, "\\");
				}
				if ((NULL != database_only) && (NULL == strstr(database_only, quote_name))) {
					debug(2, "Database \"%s\" does not match filter, skipped log entry\n", database);
					free(message);
					if (! csv && detail) {
						free(detail);
					}
					break;
				}
				memset(quote_name, '\0', NAMELEN + 3);
				if ('\0' != *user) {
					quote_name[0] = '\\';
					strcat(quote_name, user);
					strcat(quote_name, "\\");
				}
				if ((NULL != username_only) && (NULL == strstr(username_only, quote_name))) {
					debug(2, "User \"%s\" does not match filter, skipped log entry\n", user);
					free(message);
					if (! csv && detail) {
						free(detail);
					}
					break;
				}

				/* check line prefix to determine type */
				if ((log_log == logtype) && (! strncmp(message, "connection authorized: ", 23))) {
					debug(2, "Connection 0x" UINT64_FORMAT " found\n", session_id);
					type = pg_connect;
					statement = message + 23;
					status = 1;
				} else if ((log_log == logtype) && (! strncmp(message, "disconnection: ", 15))) {
					debug(2, "Disconnection found%s\n", "");
					type = pg_disconnect;
					statement = message + 15;
					status = 1;
				} else if ((log_log == logtype) && (! strncmp(message, "statement: ", 11))) {
					debug(2, "Simple statement found%s\n", "");
					type = pg_execute;
					statement = message + 11;
					status = 1;
				} else if ((log_log == logtype) && (! strncmp(message, "execute ", 8))) {
					debug(2, "Prepared statement execution found%s\n", "");
					type = pg_exec_prepared;
					namep = message + 8;
					if ((statement = strchr(namep, ':'))) {
						/* split in name and statement */
						*(statement++) = '\0';
						++statement;
						/* check for unnamed statement, change name to empty string */
						if (! strcmp(namep, "<unnamed>")) {
							*namep = '\0';
						}
						status = 1;
					} else {
						fprintf(stderr, "Error: missing statement name in line %lu\n", lineno);
						status = -1;
					}
				} else if ((log_error == logtype) && (! strncmp(message, "canceling statement due to user request", 39))) {
					debug(2, "Cancel request found%s\n", "");
					type = pg_cancel;
					status = 1;
				} else if ((log_log == logtype) && (! strncmp(message, "fastpath function call: ", 24))) {
					free(message);
					if (! csv && detail) {
						free(detail);
					}
					fprintf(stderr, "Warning: fast-path function call ignored in line %lu\n", lineno);

					/* count for statistics */
					++stat_fastpath;
				} else {
					free(message);
					if (! csv && detail) {
						free(detail);
					}
					debug(2, "Skipped log entry%s\n", "");
				}
				break;
			default:
				/* something went wrong */
				status = -1;
		}
	}

	/* if everything is ok so far, search for a connection in our list
       add one if there is none so far, store the "connect" replay_item in r */
	if (1 == status) {
		/* we need a connection in any case */
		if ((conn = add_connection(&r, &time, user, database, session_id))) {
			if (r && (pg_connect != type)) {
				debug(2, "Automatically generated connection%s\n", "");
			}
			if ((!r) && (pg_connect == type)) {
				/* if the connection already existed and we read
				   a connect line, that is an error */
				fprintf(stderr, "Error: duplicate session ID 0x" UINT64_FORMAT "\n", session_id);
				status = -1;
			}
		} else {
			/* error occurred */
			status = -1;
		}
	}

	/* if everything is ok so far, process line according to type
	   result will be stored in queue[0] and queue[1] */
	if (1 == status) {
		switch (type) {
			case pg_connect:
				/* we are status! */
				break;
			case pg_disconnect:
				if (NULL == (queue[0] = replay_create_disconnect(&time, session_id))) {
					status = -1;
				}
				/* remove connection from hash structure */
				if (! remove_connection(session_id)) {
					/* can't happen */
					fprintf(stderr, "Error: cannot remove connection " UINT64_FORMAT " from hash table\n", session_id);
					status = -1;
				}
				break;
			case pg_execute:
				if (filter_bad_statements(statement, conn)) {
					if (NULL == (queue[0] = replay_create_execute(&time, session_id, statement))) {
						status = -1;
					}
				} else {
					status = -1;
				}
				break;
			case pg_exec_prepared:
				/* we don't need to filter, since it can only be a single statement
				   and neither COPY nor SET can be prepared statements */

				/* make a persistent copy of the statement name */
				strcpy(name, namep);

				/* see if this is a new statement */
				switch (add_pstmt(conn, name)) {
					case 1:  /* new */
						if (NULL == (queue[0] = replay_create_prepare(&time, session_id, name, statement))) {
							status = -1;
						}
						/* the detail message is read later */
						break;
					case 0:  /* statement already exists */
						/* the detail message is read later */
						break;
					default:  /* error */
						status = -1;
				}
				break;
			case pg_cancel:
				if (NULL == (queue[0] = replay_create_cancel(&time, session_id))) {
					status = -1;
				}

				/* count for statistics */
				++stat_cancel;

				break;
			default:
				/* can't happen */
				fprintf(stderr, "Error: impossible type parsing line %lu\n", lineno);
				status = -1;
		}
	}

	/* read and process the DETAIL message for a prepared statement */
	if ((1 == status) && (pg_exec_prepared == type)) {

		if (! detail) {
			/* no DETAIL message --> statement has no parameters */
			debug(2, "Prepared statement \"%s\" has no bind arguments\n", name);
			if (NULL == (queue[1] = replay_create_exec_prepared(&time, session_id, name, 0, NULL))) {
				status = -1;
			}
		} else if (strncmp(detail, "parameters: ", 12)) {
			fprintf(stderr, "Error: no parameters for prepared statement at line %lu\n", lineno);
			status = -1;
		} else {
			debug(2, "Reading bind arguments for prepared statement \"%s\"\n", name);
			statement = detail + 12;
			if (-1 == (count = parse_bind_args(&args, statement))) {
				status = -1;
			} else {
				if (NULL == (queue[1] = replay_create_exec_prepared(&time, session_id, name, count, args))) {
					status = -1;
				}
				free(args);
			}
		}
	}

	if (! csv && detail) {
		free(detail);
	}
	if (message) {
		free(message);
	}

	/* if EOF, close all connections that are still open */
	if (3 == status) {
		/* search for entry in connection hash table */
		for (i=0; i<256; ++i) {
			if (open_conn[i]) {
				/* entry found, create disconnect item */
				debug(2, "End-of-file encountered, creating disconnect item for session 0x" UINT64_FORMAT "\n", open_conn[i]->session_id);
				if (NULL == (r = replay_create_disconnect(&oldtime, open_conn[i]->session_id))) {
					status = -1;
				}
				remove_connection(open_conn[i]->session_id);
				break;
			}
		}

		if ((! r) && (3 == status)) {
			debug(2, "End-of-file encountered, signal end%s\n", "");
			r = end_item;
		}
	}

	if (1 == status) {
		/* condense queue */
		if (! queue[0]) {
			queue[0] = queue[1];
			queue[1] = NULL;
		}
		if (! r) {
			r = queue[0];
			queue[0] = queue[1];
			queue[1] = NULL;
		}
		if (! queue[0]) {
			queue[0] = queue[1];
			queue[1] = NULL;
		}

		/* remember time */
		oldtime.tv_sec  = replay_get_time(r)->tv_sec;
		oldtime.tv_usec = replay_get_time(r)->tv_usec;

		last_stmt_time.tv_sec = replay_get_time(r)->tv_sec;
		last_stmt_time.tv_usec = replay_get_time(r)->tv_usec;
		if (! first_stmt_time_set) {
			first_stmt_time.tv_sec = replay_get_time(r)->tv_sec;
			first_stmt_time.tv_usec = replay_get_time(r)->tv_usec;

			first_stmt_time_set = 1;
		}
	}

	if (-1 == status) {
		/* free items in queue */
		if (r) {
			replay_free(r);
			r = NULL;
		}
		if (queue[0]) {
			replay_free(queue[0]);
			queue[0] = NULL;
		}
		if (queue[1]) {
			replay_free(queue[1]);
			queue[1] = NULL;
		}
	}

	if (r && (1 <= debug_level) && (end_item != r)) {
		replay_print_debug(r);
	}
	debug(3, "Leaving parse_provider%s\n", "");
	return r;
}
