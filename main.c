#include "pgreplay.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#ifdef WINDOWS
#	include <windows.h>
#endif

/* from getopt */
extern char *optarg;

int debug_level = 0;

/* destination of statistics output */
FILE *sf;

/* if 1, backslash will escape the following single quote in string literal */
int backslash_quote = 0;

/* if 1, replay will skip idle intervals instead of sleeping */
int jump_enabled = 0;

/* extra connect options specified with the -X option */
char *extra_connstr;

/* wrapper for setenv, returns 0 on success and -1 on error */
static int do_setenv(const char *name, const char *value) {
	int rc;

#ifdef WINDOWS
	if (0 == SetEnvironmentVariable(name, value)) {
		win_perror("Error setting environment variable", 0);
		rc = -1;
	} else {
		rc = 0;
	}
#else
	if (-1 == (rc = setenv(name, value, 1))) {
		fprintf(stderr, "Error setting environment variable\n");
	}
#endif

	return rc;
}

static void version(FILE *f) {
	fprintf(f, "pgreplay %s\n", VERSION);
}

static void help(FILE *f) {
	fprintf(f, "\n");
	version(f);
	fprintf(f, "==============\n");
	fprintf(f, "\nUsage: pgreplay [<parse options>] [<replay options>] [<infile>]\n");
	fprintf(f, "       pgreplay -f [<parse options>] [-o <outfile>] [<infile>]\n");
	fprintf(f, "       pgreplay -r [<replay options>] [<infile>]\n\n");
	fprintf(f, " The first form parses a PostgreSQL log file and replays the\n");
	fprintf(f, "statements against a database.\n");
	fprintf(f, " The second form parses a PostgreSQL log file and writes the\n");
	fprintf(f, "contents to a \"replay file\" that can be replayed with -r.\n");
	fprintf(f, " The third form replays a file generated with -f.\n\n");
	fprintf(f, "Parse options:\n");
	fprintf(f, "   -c             (assume CSV logfile)\n");
	fprintf(f, "   -b <timestamp> (start time for parsing logfile)\n");
	fprintf(f, "   -e <timestamp> (end time for parsing logfile)\n");
	fprintf(f, "   -q             ( \\' in string literal is a single quote)\n\n");
	fprintf(f, "   -D <database>  (database name to use as filter for parsing logfile)\n");
	fprintf(f, "   -U <username>  (username to use as filter for parsing logfile)\n");
	fprintf(f, "Replay options:\n");
	fprintf(f, "   -h <hostname>\n");
	fprintf(f, "   -p <port>\n");
	fprintf(f, "   -W <password>  (must be the same for all users)\n");
	fprintf(f, "   -s <factor>    (speed factor for replay)\n");
	fprintf(f, "   -E <encoding>  (server encoding)\n");
	fprintf(f, "   -j             (skip idle time during replay)\n");
	fprintf(f, "   -X <options>   (extra libpq connect options)\n\n");
	fprintf(f, "Debugging:\n");
	fprintf(f, "   -d <level>     (level between 1 and 3)\n");
	fprintf(f, "   -v             (prints version and exits)\n");
}

int main(int argc, char **argv) {
	int arg, parse_only = 0, replay_only = 0, port = -1, csv = 0,
		parse_opt = 0, replay_opt = 0, rc = 0;
	double factor = 1.0;
	char *host = NULL, *encoding = NULL, *endptr, *passwd = NULL,
		*outfilename = NULL, *infilename = NULL,
		*database_only = NULL, *username_only = NULL,
		start_time[24] = { '\0' }, end_time[24] = { '\0' };
	const char *errmsg;
	unsigned long portnr = 0l, debug = 0l, length;
	replay_item_provider *provider;
	replay_item_provider_init *provider_init;
	replay_item_provider_finish *provider_finish;
	replay_item_consumer *consumer;
	replay_item_consumer_init *consumer_init;
	replay_item_consumer_finish *consumer_finish;
	replay_item *item = NULL;

	/* initialize errno to avoid bogus error messages */
	errno = 0;

	/* parse arguments */
	opterr = 0;
	while (-1 != (arg = getopt(argc, argv, "vfro:h:p:W:s:E:d:cb:e:qjX:D:U:"))) {
		switch (arg) {
			case 'f':
				parse_only = 1;
				if (replay_only) {
					fprintf(stderr, "Error: options -p and -r are mutually exclusive\n");
					help(stderr);
					return 1;
				}
				break;
			case 'r':
				replay_only = 1;
				if (parse_only) {
					fprintf(stderr, "Error: options -p and -r are mutually exclusive\n");
					help(stderr);
					return 1;
				}
				break;
			case 'o':
				outfilename = ('\0' == *optarg) ? NULL : optarg;
				break;
			case 'h':
				replay_opt = 1;

				host = ('\0' == *optarg) ? NULL : optarg;
				break;
			case 'p':
				replay_opt = 1;

				portnr = strtoul(optarg, &endptr, 0);
				if (('\0' == *optarg) || ('\0' != *endptr)) {
					fprintf(stderr, "Not a valid port number: \"%s\"\n", optarg);
					help(stderr);
					return 1;
				}
				if ((portnr < 1) || (65535 < portnr)) {
					fprintf(stderr, "Port number must be between 1 and 65535\n");
					help(stderr);
					return 1;
				}
				port = (int)portnr;
				break;
			case 'W':
				replay_opt = 1;

				passwd = ('\0' == *optarg) ? NULL : optarg;
				break;
			case 's':
				replay_opt = 1;

				factor = strtod(optarg, &endptr);
				if (('\0' == *optarg) || ('\0' != *endptr)) {
					fprintf(stderr, "Not a valid floating point number: \"%s\"\n", optarg);
					help(stderr);
					return 1;
				}
				if (0 != errno) {
					perror("Error converting speed factor");
					help(stderr);
					return 1;
				}
				if (factor <= 0.0) {
					fprintf(stderr, "Factor must be greater than 0\n");
					help(stderr);
					return 1;
				}
				break;
			case 'E':
				replay_opt = 1;

				encoding = ('\0' == *optarg) ? NULL : optarg;
				break;
			case 'd':
				debug = strtoul(optarg, &endptr, 0);
				if (('\0' == *optarg) || ('\0' != *endptr)) {
					fprintf(stderr, "Not a valid debug level: \"%s\"\n", optarg);
					help(stderr);
					return 1;
				}
				if ((debug < 0) || (3 < debug)) {
					fprintf(stderr, "Debug level must be between 0 and 3\n");
					help(stderr);
					return 1;
				}
				debug_level = (int)debug;
				break;
			case 'v':
				version(stdout);
				return 0;
				break;
			case 'c':
				parse_opt = 1;

				csv = 1;
				break;
			case 'b':
				parse_opt = 1;

				if (NULL == (errmsg = parse_time(optarg, NULL))) {
					strncpy(start_time, optarg, 23);
				} else {
					fprintf(stderr, "Error in begin timestamp: %s\n", errmsg);
					help(stderr);
					return 1;
				}
				break;
			case 'e':
				parse_opt = 1;

				if (NULL == (errmsg = parse_time(optarg, NULL))) {
					strncpy(end_time, optarg, 23);
				} else {
					fprintf(stderr, "Error in end timestamp: %s\n", errmsg);
					help(stderr);
					return 1;
				}
				break;
			case 'q':
				backslash_quote = 1;
				break;
			case 'D':
				parse_opt = 1;

				if (NULL == database_only) {
					length = strlen(optarg) + 3;
					database_only = malloc(length);
					if (NULL != database_only)
						strcpy(database_only, "\\");
				} else {
					length = strlen(database_only) + strlen(optarg) + 2;
					database_only = realloc(database_only, length);
				}
				if (NULL == database_only) {
					fprintf(stderr, "Cannot allocate %lu bytes of memory\n", length);
					return 1;
				}

				strcat(database_only, optarg);
				strcat(database_only, "\\");
				break;
			case 'U':
				parse_opt = 1;

				if (NULL == username_only) {
					length = strlen(optarg) + 3;
					username_only = malloc(length);
					if (NULL != username_only)
						strcpy(username_only, "\\");
				} else {
					length = strlen(username_only) + strlen(optarg) + 2;
					username_only = realloc(username_only, length);
				}
				if (NULL == username_only) {
					fprintf(stderr, "Cannot allocate %lu bytes of memory\n", length);
					return 1;
				}

				strcat(username_only, optarg);
				strcat(username_only, "\\");
				break;
			case 'j':
				replay_opt = 1;

				jump_enabled = 1;
				break;
			case 'X':
				replay_opt = 1;

				extra_connstr = optarg;
				break;
			case '?':
				if (('?' == optopt) || ('h' == optopt)) {
					help(stdout);
					return 0;
				} else {
					fprintf(stderr, "Error: unknown option -%c\n", optopt);
					help(stderr);
					return 1;
				}
				break;
		}
	}

	if (optind + 1 < argc) {
		fprintf(stderr, "More than one argument given\n");
		help(stderr);
		return 1;
	}

	if (optind + 1 == argc) {
		infilename = argv[optind];
	}

	if (parse_only && replay_opt) {
		fprintf(stderr, "Error: cannot specify replay option with -f\n");
		help(stderr);
		return 1;
	}

	if (replay_only && parse_opt) {
		fprintf(stderr, "Error: cannot specify parse option with -r\n");
		help(stderr);
		return 1;
	}

	if (NULL != outfilename) {
		if (! parse_only) {
			fprintf(stderr, "Error: option -o is only allowed with -f\n");
			help(stderr);
			return 1;
		}
	}

	/* set default encoding */
	if (NULL != encoding) {
		if (-1 == do_setenv("PGCLIENTENCODING", encoding)) {
			return 1;
		}
	}

	/* figure out destination for statistics output */
	if (parse_only && (NULL == outfilename)) {
		sf = stderr;  /* because replay file will go to stdout */
	} else {
		sf = stdout;
	}

	/* configure main loop */

	if (replay_only) {
		provider_init = &file_provider_init;
		provider = &file_provider;
		provider_finish = &file_provider_finish;
	} else {
		provider_init = &parse_provider_init;
		provider = &parse_provider;
		provider_finish = &parse_provider_finish;
	}

	if (parse_only) {
		consumer_init = &file_consumer_init;
		consumer_finish = &file_consumer_finish;
		consumer = &file_consumer;
	} else {
		consumer_init = &database_consumer_init;
		consumer_finish = &database_consumer_finish;
		consumer = &database_consumer;
	}

	/* main loop */

	if (! (*provider_init)(
			infilename,
			csv,
			(('\0' == start_time[0]) ? NULL : start_time),
			(('\0' == end_time[0]) ? NULL : end_time),
			database_only,
			username_only
		))
	{
		rc = 1;
	}

	if ((0 == rc) && (*consumer_init)(outfilename, host, port, passwd, factor)) {
		/* try to get first item */
		if (! (item = (*provider)())) {
			rc = 1;
		}
	} else {
		rc = 1;
	}

	while ((0 == rc) && (end_item != item)) {
		switch ((*consumer)(item)) {
			case 0:     /* item not consumed */
				break;
			case 1:     /* item consumed */
				if (! (item = (*provider)())) {
					rc = 1;
				}
				break;
			default:    /* error occurred */
				rc = 1;
		}
	}

	/* no statistics output if there was an error */
	if (1 == rc) {
		sf = NULL;
	}

	(*provider_finish)();
	(*consumer_finish)();

	return rc;
}
