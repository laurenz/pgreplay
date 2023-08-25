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
/* indicates whether input file is from polardb */
int polardb_audit = 0;

char monitor_sql[] =  
	" select 1 as id,'cpu_use' as monitor_name ,100 - split_part((select pg_sys_cpu_usage_info())::text,',', 4 )::decimal as num union"
	" select 2,'mem_use' as monitor_name , (( split_part(replace((select pg_sys_memory_info())::text,'(','') , ',',1)::decimal  - "
	" split_part(replace((select pg_sys_memory_info())::text,'(','') , ',',7)::decimal  )"
	" /  split_part(replace((select pg_sys_memory_info())::text,'(','') , ',',1)::decimal)::decimal(4,2)*100  as  num union"
	" select 3,'read_count' monitor_name, sum(io_count) num from (select split_part(pg_sys_io_analysis_info()::text,',',2)::bigint as io_count) as a union "
	" select 4,'write_count' monitor_name, sum(io_count) num from (select split_part(pg_sys_io_analysis_info()::text,',',3)::bigint as io_count) as a union "
	" select 5,'read_bytes' monitor_name, sum(io_count) num from (select split_part(pg_sys_io_analysis_info()::text,',',4)::bigint as io_count) as a union "
	" select 6,'write_bytes' monitor_name, sum(io_count) num from (select split_part(pg_sys_io_analysis_info()::text,',',5)::bigint as io_count) as a union"
	" select 7,'disk_space' monitor_name, sum(disk_space / 1024 / 1024) num from (select split_part(pg_sys_disk_info()::text,',',7)::bigint as disk_space) as a union"
	" select 8,'active_conn' as monitor_name, count(query) as num from pg_stat_activity where state = 'active' and backend_type = 'client backend'  union"
	" select 9,'total_conn' as monitor_name, count(query) as num from pg_stat_activity  union"
	" select 10,'load5_value' monitor_name,  split_part(pg_sys_load_avg_info()::text,',',2)::decimal as num union"
	" select 11,'load10_value' monitor_name,  split_part(pg_sys_load_avg_info()::text,',',3)::decimal as num  union"
	" select 12,'tps' as monitor_name,split_part(txid_current_snapshot()::text,':',1)::integer as num union"
	" select 13,'qps' as monitor_name,sum(calls) as num from pg_stat_statements  order by id;";

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
	fprintf(f, "   -P             (assume Polardb11 adult logfile)\n");
	fprintf(f, "   -m             (print monitor info,only support for polardb)\n");
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
	fprintf(f, "   -n             (dry-run, will replay file without running queries)\n\n");
	fprintf(f, "Debugging:\n");
	fprintf(f, "   -d <level>     (level between 1 and 3)\n");
	fprintf(f, "   -v             (prints version and exits)\n");
}

int main(int argc, char **argv) {
	int arg, parse_only = 0, replay_only = 0, port = -1, csv = 0,
		parse_opt = 0, replay_opt = 0, rc = 0, dry_run = 0, monitor_gap = 0;
	polardb_audit = 0;
	double factor = 1.0;
	char *host = NULL, *encoding = NULL, *endptr, *passwd = NULL,
		*outfilename = NULL, *infilename = NULL,
		*database_only = NULL, *username_only = NULL, *tmp = NULL,
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
	while (-1 != (arg = getopt(argc, argv, "vfro:h:p:W:s:E:d:cb:e:qjnX:D:U:Pm:"))) {
		switch (arg) {
			case 'v':
				version(stdout);
				return 0;
				break;
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
			case 'c':
				parse_opt = 1;

				csv = 1;
				break;
			case 'P':
				parse_opt = 1;
				polardb_audit = 1;
				break;
			case 'm':
				tmp = ('\0' == *optarg) ? NULL : optarg;
				monitor_gap = atoi(tmp);
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
			case 'j':
				replay_opt = 1;

				jump_enabled = 1;
				break;
			case 'n':
				replay_opt = 1;

				dry_run = 1;
				break;
			case 'X':
				replay_opt = 1;

				extra_connstr = optarg;
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

	if(polardb_audit){
		if(parse_only || replay_only){
			fprintf(stderr,"only support fisrt pattern for polardb (combined parse and replay)");
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
		if (0 == dry_run) {
			consumer = &database_consumer;
		} else {
			consumer = &database_consumer_dry_run;
		}
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

	const struct timeval * tmp_time = replay_get_time(item);
	struct timeval monitor_time = {tmp_time->tv_sec, tmp_time->tv_usec};
	if(polardb_audit && monitor_gap)
		monitor_connect_init(host, port, passwd);

	while ((0 == rc) && (end_item != item)) {

		int n = (*consumer)(item);

		switch (n) {
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
		if(polardb_audit && monitor_gap && replay_get_time(item)->tv_sec - monitor_time.tv_sec >= monitor_gap){
			monitor_connect_execute(monitor_sql);
			monitor_time.tv_sec = replay_get_time(item)->tv_sec;
		}
	}

	/* no statistics output if there was an error */
	if (1 == rc) {
		sf = NULL;
	}

	(*provider_finish)();
	(*consumer_finish)(dry_run);
	monitor_connect_finish();

	return rc;
}
