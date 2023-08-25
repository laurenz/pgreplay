pgreplay - record and replay real-life database workloads
=========================================================

pgreplay reads a PostgreSQL log file (*not* a WAL file), extracts the
SQL statements and executes them in the same order and with the original
timing against a PostgreSQL database.

If the execution of statements gets behind schedule, warning messages
are issued that indicate that the server cannot handle the load in a
timely fashion.

A final report gives you a useful statistical analysis of your workload
and its execution.

The idea is to replay a real-world database workload as exactly as possible.

This is useful for performance tests, particularly in the following
situations:
- You want to compare the performance of your PostgreSQL application
  on different hardware or different operating systems.
- You want to upgrade your database and want to make sure that the new
  database version does not suffer from performance regressions that
  affect you.

Moreover, pgreplay can give you some feeling as to how your application
*might* scale by allowing you to try to replay the workload at a higher
speed (if that is possible; see
[implementation details](#implementation-details) below).
Be warned, though, that 500 users working at double speed is not really
the same as 1000 users working at normal speed.

While pgreplay will find out if your database application will encounter
performance problems, it does not provide a lot of help in the analysis of
the cause of these problems.  Combine pgreplay with a specialized analysis
program like [pgBadger](https://pgbadger.darold.net/) for that.

As an additional feature, pgreplay lets you split the replay in two
parts: you can parse the log file and create a "replay file", which
contains just the statements to be replayed and is hopefully much
smaller than the original log file.  
Such a replay file can then be run against a database.

pgreplay is written by Laurenz Albe and is inspired by "Playr"
which never made it out of Beta.

Installation
============

pgreplay needs PostgreSQL 8.0 or better.

It is supposed to compile without warnings and run on all platforms
supported by PostgreSQL.  
Since I only got to test it on Linux, AIX, FreeBSD and Windows, there may be
problems with other platforms. I am interested in reports and fixes for
these platforms.  
On Windows, only the MinGW build environment is supported (I have no
other compiler). That means that there is currently no 64-bit build
for Windows (but a 32-bit executable should work fine anywhere).

To build pgreplay, you will need the `pg_config` utility. If you installed
PostgreSQL using installation packages, you will probably have to install
the development package that contains `pg_config` and the header files.

If `pg_config` is on the `PATH`, the installation process will look like this:

- unpack the tarball
- `./configure`
- `make`
- `make test`     (optional, described below)
- `make install`  (as superuser)

If your PostgreSQL installation is in a nonstandard directory, you
will have to use the `--with-postgres=<path to location of pg_config>`
option of `configure`.

Unless you link it statically, pgreplay requires the PostgreSQL client 
shared library on the system where it is run.

The following utilities are only necessary if you intend to develop pgreplay:
- autoconf 2.62 or better to generate `configure`
- GNU tar to `make tarball` (unless you want to roll it by hand)
- groff to make the HTML documentation with `make html`

Docker
------

The `Dockerfile` provided with the software can be used as a starting
point for creating a container that runs pgreplay.  Adapt is as necessary.

Here are commands to build and run the container:

```
# build the image
docker build -t laurenz/pgreplay -f Dockerfile .

# and run it
docker run --rm -ti -v $(pwd):/app -w /app laurenz/pgreplay pgreplay -h
```

Testing
-------

You can run a test on pgreplay before installing by running `make test`.
This will parse sample log files and check that the result is as
expected.

Then an attempt is made to replay the log files and check if that
works as expected.  For this you need psql installed and a PostgreSQL server
running (on this or another machine) so that the following command
will succeed:

    psql -U postgres -d postgres -l

You can set up the `PGPORT` and `PGHOST` environment variables and a password
file for the user if necessary.

There have to be a login roles named `hansi` and `postgres` in the database,
and both users must be able to connect without a password.  Only `postgres`
will be used to run actual SQL statements.  The regression test will create
a table `runtest` and use it, and it will drop the table when it is done.

Usage
=====

First, you will need to record your real-life workload.
For that, set the following parameters in `postgresql.conf`:

- `log_min_messages = error`  (or more)  
   (if you know that you have no cancel requests, `log` will do)
- `log_min_error_statement = log`  (or more)
- `log_connections = on`
- `log_disconnections = on`
- `log_line_prefix = '%m|%u|%d|%c|'`  (if you don't use CSV logging)
- `log_statement = 'all'`
- `lc_messages` must be set to English (the encoding does not matter)
- `bytea_output = escape`  (from version 9.0 on, only if you want to replay
                            the log on 8.4 or earlier)

It is highly recommended that you use CSV logging, because anything that
the PostgreSQL server or any loaded modules write to standard error will
be written to the stderr log and might confuse the parser.

Then let your users have their way with the database.

Make sure that you have a `pg_dumpall` of the database cluster from the time
of the start of your log file (or use the `-b` option with the time of your
backup).  Alternatively, you can use point-in-time-recovery to clone your
database at the appropriate time.

When you are done, restore the database (in the "before" state) to the
machine where you want to perform the load test and run pgreplay against
that database.

Try to create a scenario as similar to your production system as
possible (except for the change you want to test, of course).  For example,
if your clients connect over the network, run pgreplay on a different
machine from where the database server is running.

Since passwords are not logged (and pgreplay consequently has no way of
knowing them), you have two options: either change `pg_hba.conf` on the
test database to allow `trust` authentication or (if that is unacceptable)
create a password file as described by the PostgreSQL documentation.
Alternatively, you can change the passwords of all application users
to one single password that you supply to pgreplay with the `-W` option.

Limitations
===========

pgreplay can only replay what is logged by PostgreSQL.
This leads to some limitations:

- `COPY` statements will not be replayed, because the copy data are not logged.
  I could have supported `COPY TO` statements, but that would have imposed a
  requirement that the directory structure on the replay system must be
  identical to the original machine.
  And if your application runs on the same machine as your database and they
  interact on the file system, pgreplay will probably not help you much
  anyway.
- Fast-path API function calls are not logged and will not be replayed.
  Unfortunately, this includes the Large Object API.
- Since the log file is always written in the database encoding (which you
  can specify with the `-E` switch of pgreplay), all `SET client_encoding`
  statements will be ignored.
- If your cluster contains databases with different encoding, the log file
  will have mixed encoding as well.  You cannot use pgreplay well in such
  an environment, because many statements against databases whose
  encoding does not match the `-E` switch will fail.
- Since the preparation time of prepared statements is not logged (unless
  `log_min_messages` is `debug2` or more), these statements will be prepared
  immediately before they are first executed during replay.
- All parameters of prepared statements are logged as strings, no matter
  what type was originally specified during bind.
  This can cause errors during replay with expressions like `$1 + $2`,
  which will cause the error `operator is not unique: unknown + unknown`.

While pgreplay makes sure that commands are sent to the server in the
order in which they were originally executed, there is no way to guarantee
that they will be executed in the same order during replay:  Network
delay, processor contention and other factors may cause a later command
to "overtake" an earlier one.  While this does not matter if the
commands don't affect each other, it can lead to SQL statements hitting
locks unexpectedly, causing replay to deadlock and "hang".
This is particularly likely if many different sessions change the same data
repeatedly in short intervals.

You can work around this problem by canceling the waiting statement with
pg_cancel_backend.  Replay should continue normally after that.

Implementation details
======================

pgreplay will track the "session ID" associated with each log entry (the
session ID uniquely identifies a database connection).
For each new session ID, a new database connection will be opened during
replay.  Each statement will be sent on the corresponding connection, so
transactions are preserved and concurrent sessions cannot get in each
other's way.

The order of statements in the log file is strictly preserved, so there
cannot be any race conditions caused by different execution speeds on
separate connections.  On the other hand, that means that long running
queries on one connection may stall execution on concurrent connections,
but that's all you can get if you want to reproduce the exact same
workload on a system that behaves differently.

As an example, consider this (simplified) log file:

    session 1|connect
    session 2|connect
    session 1|statement: BEGIN
    session 1|statement: SELECT something(1)
    session 2|statement: BEGIN
    session 2|statement: SELECT something(2)
    session 1|statement: SELECT something(3)
    session 2|statement: ROLLBACK
    session 2|disconnect
    session 1|statement: COMMIT
    session 2|disconnect

This will cause two database connections to be opened, so the `ROLLBACK` in
session 2 will not affect session 1.
If `SELECT something(2)` takes longer than expected (longer than it did in
the original), that will not stall the execution of `SELECT something(3)`
because it runs on a different connection.  The `ROLLBACK`, however, has to
wait for the completion of the long statement.  Since the order of statements
is preserved, the `COMMIT` on session 1 will have to wait until the `ROLLBACK`
on session 2 has started (but it does not have to wait for the completion of
the `ROLLBACK`).

pgreplay is implemented in C and makes heavy use of asynchronous command
processing (which is the reason why it is implemented in C).
This way a single process can handle many concurrent connections, which
makes it possible to get away without multithreading or multiprocessing.

This avoids the need for synchronization and many portability problems.
But since TINSTAAFL, the choice of C brings along its own portability
problems.  Go figure.

Replay file format
------------------

The replay file is a binary file, integer numbers are stored in network
byte order.

Each record in the replay file corresponds to one database operation
and is constructed as follows:
- 4-byte `unsigned int`: log file timestamp in seconds since 2000-01-01
- 4-byte `unsigned int`: fractional part of log file timestamp in microseconds
- 8-byte `unsigned int`: session id
- 1-byte `unsigned int`: type of the database action:
  - 0 is connect
  - 1 is disconnect
  - 2 is simple statement execution
  - 3 is statement preparation
  - 4 is execution of a prepared statement
  - 5 is cancel request
- The remainder of the record is specific to the action, strings are stored
  with a preceeding 4-byte unsigned int that contains the length.
  Read the source for details.
- Each record is terminated by a new-line character (byte 0x0A).


Using for polardb
======
- make sure pg_stat_statements, plpgsql, system_stats was installed.system_stats install methods refer to https://github.com/EnterpriseDB/system_stats.
```sql
postgres=# create extension plpgsql;
postgres=# create extension pg_stat_statements;
postgres=# create extension system_stats;
postgres=# \dx
                                     List of installed extensions
        Name        | Version |   Schema   |                        Description                        
--------------------+---------+------------+-----------------------------------------------------------
 pg_stat_statements | 1.6     | public     | track execution statistics of all SQL statements executed
 plpgsql            | 1.0     | pg_catalog | PL/pgSQL procedural language
 system_stats       | 1.0     | public     | EnterpriseDB system statistics for PostgreSQL
(3 rows)
```

- postgres.conf configure
```sh
# log setting for gpreplay 
log_min_messages = error 
# (if you know that you have no cancel requests, log will do)
log_min_error_statement = log 
log_connections = on
log_disconnections = on
log_line_prefix = '%t|%u|%d|%c|%N|%L|' 
log_statement = 'all' 
# lc_messages must be set to English (the encoding does not matter)
bytea_output = escape 
# (from version 9.0 on, only if you want to replay the log on 8.4 or earlier)
# polar_auditlog_max_query_length = 49152
polar_auditlog_max_query_length_limit = false 
```
- run read audit log from $PGDATA/log/replay_xxx.log and replay sql to databse, reporting monitor info every 3 seconds.
```sh
./pgreplay -P -m 3   -h 127.0.0.1 -p 5432 -W benchmarksql $PGDATA/log/replay_xxx.log
```
Support
=======

If you have a problem or question, the preferred option is to [open an
issue](https://github.com/laurenz/pgreplay/issues).
This requires a GitHub account.

Professional support can be bought from
[CYBERTEC PostgreSQL International GmbH](https://www.cybertec-postgresql.com/).

TODO list
=========

Nothing currently.  Tell me if you have good ideas.
