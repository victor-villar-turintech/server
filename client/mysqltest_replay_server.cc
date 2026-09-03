/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Replay-server mode of mysqltest.  See mysqltest_replay_server.h for what it
  does and for the interface it shares with mysqltest.cc.
*/

#include "client_priv.h"
#include <m_ctype.h>
#include "mysqltest_replay_server.h"

/*
  ReplayTest mode: names and literals shared with the outside world.

  Everything the replay code has to spell exactly right is defined here rather
  than at the point of use: the environment variables are set by
  mariadb-test-run.pl, the system variables and the @-variable are those the
  recorded context script uses, and the file names are what a test run leaves
  in vardir / next to the .result file.
*/
/* Environment variables exported by mtr --replay-server & friends */
#define REPLAY_ENV_SOCKET         "REPLAY_SERVER_SOCKET"
#define REPLAY_ENV_TRACE          "REPLAY_SERVER_TRACE"
#define REPLAY_ENV_NO_CLEANUP     "REPLAY_SERVER_NO_CLEANUP"
/* Log of everything sent to the replay server, relative to MYSQLTEST_VARDIR */
#define REPLAY_QUERY_LOG_SUBPATH  "/log/replay_queries.log"
/* Marker written to that log before each replayed context script */
#define REPLAY_LOG_SESSION_MARKER "### REPLAY SESSION ###"
/* Extensions of the two optimizer_trace dumps, replacing the .result one */
#define REPLAY_TRACE_EXT_ORIGINAL ".opt_trace.original"
#define REPLAY_TRACE_EXT_REPLAY   ".opt_trace.replay"
/* Database mysqltest connects the replay server with, and returns to after
   cleanup. Never dropped by the cleanup, see replay_capture_snapshot(). */
#define REPLAY_DEFAULT_DB         "test"
/* System variable that makes the test server record an optimizer context */
#define REPLAY_RECORD_SYSVAR      "optimizer_record_context"
/* System variable that feeds a recorded context to the replay server */
#define REPLAY_CONTEXT_SYSVAR     "optimizer_replay_context"
/* User variable the recorded script keeps the context in */
#define REPLAY_CONTEXT_USERVAR    "@opt_context"
/* User variable holding the test server's optimizer_trace setting while
   REPLAY_ENV_TRACE has it forced on for one EXPLAIN. Deliberately not the one
   enable_optimizer_trace() uses, so that a test doing --enable_optimizer_trace
   under REPLAY_ENV_TRACE still restores its own value. */
#define REPLAY_SAVED_TRACE_USERVAR "@mtr_save_opt_trace"
/* Where the recorded context and the optimizer traces are read from */
#define REPLAY_IS_CONTEXT_TABLE   "information_schema.optimizer_context"
#define REPLAY_IS_TRACE_TABLE     "information_schema.optimizer_trace"
/* Schemas the cleanup must never look at, and the extra ones that are only
   excluded from the database list - their contents are still tracked */
#define REPLAY_ENGINE_SCHEMAS     "'information_schema','performance_schema'"
#define REPLAY_KEPT_SCHEMAS       REPLAY_ENGINE_SCHEMAS ",'mysql','sys','" \
                                  REPLAY_DEFAULT_DB "'"
/* The recorded context script separates its statements with this */
static const char replay_stmt_separator[]= ";\n";
#define REPLAY_STMT_SEPARATOR_LEN (sizeof(replay_stmt_separator) - 1)
/* Scope keywords accepted by "disable_replay <scope> <reason>" */
static const LEX_CSTRING replay_scope_next_query=
  {STRING_WITH_LEN("next_query")};
static const LEX_CSTRING replay_scope_testfile= {STRING_WITH_LEN("testfile")};
#define DISABLE_REPLAY_SYNTAX "Syntax: disable_replay next_query|testfile " \
                              "<reason>"

/* ReplayTest mode variables */
static MYSQL *replay_server_mysql= NULL;
static const char *replay_server_socket= NULL;
static my_bool replay_test_mode= FALSE;
static FILE *replay_log_file= NULL;
static const char *replay_log_path= NULL;
static my_bool replay_server_trace= FALSE;
static FILE *replay_opt_trace_original_file= NULL;
static FILE *replay_opt_trace_replay_file= NULL;
static const char *replay_opt_trace_original_path= NULL;
static const char *replay_opt_trace_replay_path= NULL;
/* Set just before invoking replay-side EXPLAIN; used as header in trace dumps. */
static const char *replay_current_explain= NULL;
static size_t replay_current_explain_len= 0;
/* When non-NULL, the replay-side helpers append the replay server's
   optimizer_trace into this buffer so the caller can decide whether to
   actually flush it to disk. */
static DYNAMIC_STRING *replay_trace_capture_buf= NULL;
/* Substrings whose presence in the optimizer_context script disables replay
   for the current EXPLAIN; the primary server's result is used as-is. */
static const char *replay_context_stop_words[]= {
  "subquery_runs",
  NULL
};
static my_bool disable_replay_next_query= FALSE;
static char disable_replay_reason[512]= {0};
static my_bool disable_replay_testfile= FALSE;
static char disable_replay_testfile_reason[512]= {0};
/* Restore the replay server to its baseline state after each replay run.
   Cleared by REPLAY_SERVER_NO_CLEANUP, which leaves the objects created by
   the last replay run in place for post-mortem inspection. */
static my_bool replay_cleanup= TRUE;
/* One table or view on the replay server. Both members are my_malloc'ed. */
struct st_replay_object
{
  char *db;
  char *name;
};
/* The state the replay server had when we connected to it: the databases
   (system ones excluded) and the tables/views present at that point. Set up
   by replay_capture_baseline(), both kept sorted for bsearch(). */
static my_bool replay_baseline_valid= FALSE;
static DYNAMIC_ARRAY replay_baseline_dbs;      /* of char*                */
static DYNAMIC_ARRAY replay_baseline_objects;  /* of st_replay_object     */

/*
  ReplayTest mode helper functions
*/

static void replay_capture_baseline();

/*
  Ensure connection to replay server is established
  Returns 0 on success, non-zero on error
*/
static int ensure_replay_server_connection()
{
  DBUG_ENTER("ensure_replay_server_connection");
  
  if (replay_server_mysql)
    DBUG_RETURN(0);
  
  replay_server_mysql= mysql_init(NULL);
  if (!replay_server_mysql)
  {
    fprintf(stdout, "ReplayTest: Failed to initialize MySQL handle for replay server\n");
    DBUG_RETURN(1);
  }
  
  if (!mysql_real_connect(replay_server_mysql, NULL, NULL, NULL,
                          REPLAY_DEFAULT_DB, 0,
                          replay_server_socket, CLIENT_MULTI_STATEMENTS))
  {
    fprintf(stdout, "ReplayTest: Failed to connect to replay server at socket '%s': %d %s\n",
            replay_server_socket, mysql_errno(replay_server_mysql),
            mysql_error(replay_server_mysql));
    mysql_close(replay_server_mysql);
    replay_server_mysql= NULL;
    DBUG_RETURN(1);
  }
  
  verbose_msg("ReplayTest: Connected to replay server (database: %s)",
              REPLAY_DEFAULT_DB);

  /*
    Replay server runs against arbitrary recorded contexts; disable FK
    checks so trace-driven CREATE/INSERT side effects don't fail on
    referential integrity.
  */
  if (mysql_real_query(replay_server_mysql,
                       STRING_WITH_LEN("SET foreign_key_checks=0")))
  {
    fprintf(stdout,
            "ReplayTest: Warning - failed to set foreign_key_checks=0 "
            "on replay server: %d %s\n",
            mysql_errno(replay_server_mysql),
            mysql_error(replay_server_mysql));
  }

  /*
    REPLAY_SERVER_TRACE: enable optimizer_trace once, session-scoped, so all
    subsequent replayed EXPLAINs leave a trace in I_S.optimizer_trace.
  */
  if (replay_server_trace &&
      mysql_real_query(replay_server_mysql,
                       STRING_WITH_LEN("SET optimizer_trace='enabled=on'")))
  {
    fprintf(stdout,
            "ReplayTest: Warning - failed to enable optimizer_trace on "
            "replay server: %d %s\n",
            mysql_errno(replay_server_mysql),
            mysql_error(replay_server_mysql));
  }

  /*
    Remember what the server looks like before any replay script has run, so
    that replay_restore_baseline() can undo what the scripts create.
  */
  if (replay_cleanup)
    replay_capture_baseline();

  DBUG_RETURN(0);
}


/*
  Log the start of a new replay session
*/
static void log_replay_session_start()
{
  if (replay_log_file)
  {
    fprintf(replay_log_file, "%s\n", REPLAY_LOG_SESSION_MARKER);
    fflush(replay_log_file);
  }
}

/*
  Log a query being sent to the replay server
*/
static void log_replay_query(const char *query, size_t query_len)
{
  if (replay_log_file)
  {
    fprintf(replay_log_file, "%.*s;\n", (int)query_len, query);
    fflush(replay_log_file);
  }
}


/*
  Append the "Warnings:" block of the last statement run on `conn` to `ds`,
  in the same format run_query_normal() uses. Does nothing when the test has
  warnings disabled or when there are none.
*/
static void replay_append_warnings(MYSQL *conn, DYNAMIC_STRING *ds)
{
  DYNAMIC_STRING ds_warn;

  if (disable_warnings)
    return;

  init_dynamic_string(&ds_warn, "", 256, 256);
  if (append_warnings(&ds_warn, conn) || ds_warn.length)
  {
    dynstr_append_mem(ds, STRING_WITH_LEN("Warnings:\n"));
    dynstr_append_mem(ds, ds_warn.str, ds_warn.length);
  }
  dynstr_free(&ds_warn);
}


/*
  REPLAY_SERVER_TRACE helper: fetch the current optimizer_trace from `conn`
  and append the trace JSON (one row per result row, one newline each) into
  `out`. On error appends a "-- ERROR: <msg>\n" line. Drains any extra result
  sets so the connection is left clean.
*/
static void capture_optimizer_trace(MYSQL *conn, DYNAMIC_STRING *out)
{
  if (!conn || !out)
    return;
  if (mysql_real_query(conn,
                       STRING_WITH_LEN("SELECT trace FROM "
                                       REPLAY_IS_TRACE_TABLE)))
  {
    const char *err= mysql_error(conn);
    dynstr_append_mem(out, STRING_WITH_LEN("-- ERROR: "));
    dynstr_append_mem(out, err, strlen(err));
    dynstr_append_mem(out, "\n", 1);
    return;
  }
  MYSQL_RES *res= mysql_store_result(conn);
  if (res)
  {
    MYSQL_ROW row;
    while ((row= mysql_fetch_row(res)))
    {
      ulong *lens= mysql_fetch_lengths(res);
      if (row[0])
        dynstr_append_mem(out, row[0], lens[0]);
      dynstr_append_mem(out, "\n", 1);
    }
    mysql_free_result(res);
  }
  /* Drain any extra result sets (multi-statement safety). */
  while (mysql_next_result(conn) == 0)
  {
    MYSQL_RES *r= mysql_store_result(conn);
    if (r) mysql_free_result(r);
  }
}


/*
  REPLAY_SERVER_TRACE helper: append a trace block (header line followed by
  the previously-captured trace contents) to the file `*fpp`, lazy-opening it
  from `path` on first use.
*/
static void flush_trace_block(FILE **fpp, const char *path,
                              const char *header_query, size_t header_query_len,
                              const DYNAMIC_STRING *trace)
{
  if (!path || !trace)
    return;
  if (!*fpp)
  {
    *fpp= fopen(path, "a");
    if (!*fpp)
    {
      fprintf(stderr,
              "ReplayTest: Warning - could not open optimizer_trace dump "
              "file %s: %s\n", path, strerror(errno));
      return;
    }
  }
  fprintf(*fpp, "-- EXPLAIN: %.*s\n",
          (int)header_query_len, header_query ? header_query : "");
  if (trace->length)
    fwrite(trace->str, 1, trace->length, *fpp);
  fputc('\n', *fpp);
  fflush(*fpp);
}


/*
  Restoring the replay server between replay runs
  ===============================================

  The replay server is a single instance shared by every test of an mtr run.
  Each replay script creates databases, tables and views on it and never
  removes them, so without cleanup the leftovers accumulate for the whole
  run and can influence subsequent replays.

  The approach: right after connecting, take a snapshot of the databases and
  of the tables/views that are present - the "baseline". After each replay
  script, take another snapshot and drop everything that is not in the
  baseline. This puts the server back into the baseline state, which is why
  the baseline only has to be taken once.

  Known limitations. They are harmless when the replay server starts out
  pristine, which is the case under mtr --replay-server:
   - an object that a script re-created under a baseline name keeps the
     script's definition, it is not restored to the baseline one;
   - a baseline object that a script dropped is not re-created;
   - system variables are not restored. Every script begins by setting the
     full set of variables it cares about, but a variable that script N sets
     and script N+1 does not mention keeps script N's value.
*/

static int cmp_replay_string(const void *a, const void *b)
{
  return strcmp(*(const char* const *) a, *(const char* const *) b);
}


static int cmp_replay_object(const void *a, const void *b)
{
  const struct st_replay_object *o1= (const struct st_replay_object*) a;
  const struct st_replay_object *o2= (const struct st_replay_object*) b;
  int res= strcmp(o1->db, o2->db);
  return res ? res : strcmp(o1->name, o2->name);
}


/* Release a snapshot produced by replay_capture_snapshot() */

static void free_replay_snapshot(DYNAMIC_ARRAY *dbs, DYNAMIC_ARRAY *objects)
{
  uint i;
  for (i= 0; i < dbs->elements; i++)
    my_free(*(char**) dynamic_array_ptr(dbs, i));
  for (i= 0; i < objects->elements; i++)
  {
    struct st_replay_object *obj=
      (struct st_replay_object*) dynamic_array_ptr(objects, i);
    my_free(obj->db);
    my_free(obj->name);
  }
  delete_dynamic(dbs);
  delete_dynamic(objects);
}


/* Read and discard whatever the replay server still has pending */

static void replay_drain_results()
{
  do
  {
    MYSQL_RES *res= mysql_store_result(replay_server_mysql);
    if (res)
      mysql_free_result(res);
  } while (mysql_next_result(replay_server_mysql) == 0);
}


/*
  Take a snapshot of the replay server: the non-system databases into `dbs`
  and every table/view/sequence outside of the engine-provided schemas into
  `objects`. Both arrays are initialized here and are to be released with
  free_replay_snapshot(); on return they are sorted.

  Only TABLE_SCHEMA and TABLE_NAME are read from I_S.TABLES. That keeps the
  query at SKIP_OPEN_TABLE, i.e. a plain scan of the datadir; asking for
  TABLE_TYPE as well would make the server parse every .frm. Views are
  therefore not told apart from tables here - the cleanup just issues both
  DROP VIEW and DROP TABLE, the same way the replay script itself does.

  The `test` database and the system schemas are left out of the database
  list so that they can never be dropped. Their contents are still tracked:
  a script whose default database is `test` creates its tables there.

  @return 0 on success. On error the arrays are released and left empty; a
          partial snapshot must never be used to decide what to drop.
*/

static int replay_capture_snapshot(DYNAMIC_ARRAY *dbs, DYNAMIC_ARRAY *objects)
{
  static const char db_query[]=
    "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME "
    "NOT IN (" REPLAY_KEPT_SCHEMAS ")";
  static const char obj_query[]=
    "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES WHERE "
    "TABLE_SCHEMA NOT IN (" REPLAY_ENGINE_SCHEMAS ")";
  MYSQL_RES *res;
  MYSQL_ROW row;
  DBUG_ENTER("replay_capture_snapshot");

  my_init_dynamic_array(PSI_NOT_INSTRUMENTED, dbs, sizeof(char*),
                        16, 16, MYF(0));
  my_init_dynamic_array(PSI_NOT_INSTRUMENTED, objects,
                        sizeof(struct st_replay_object), 64, 64, MYF(0));

  if (mysql_real_query(replay_server_mysql, db_query, sizeof(db_query) - 1) ||
      !(res= mysql_store_result(replay_server_mysql)))
    goto error;

  while ((row= mysql_fetch_row(res)))
  {
    char *db;
    if (!row[0] ||
        !(db= my_strdup(PSI_NOT_INSTRUMENTED, row[0], MYF(MY_WME))))
    {
      mysql_free_result(res);
      goto error;
    }
    if (insert_dynamic(dbs, &db))
    {
      my_free(db);
      mysql_free_result(res);
      goto error;
    }
  }
  mysql_free_result(res);
  replay_drain_results();

  if (mysql_real_query(replay_server_mysql, obj_query, sizeof(obj_query) - 1) ||
      !(res= mysql_store_result(replay_server_mysql)))
    goto error;

  while ((row= mysql_fetch_row(res)))
  {
    struct st_replay_object obj;
    if (!row[0] || !row[1] ||
        !(obj.db= my_strdup(PSI_NOT_INSTRUMENTED, row[0], MYF(MY_WME))))
    {
      mysql_free_result(res);
      goto error;
    }
    if (!(obj.name= my_strdup(PSI_NOT_INSTRUMENTED, row[1], MYF(MY_WME))))
    {
      my_free(obj.db);
      mysql_free_result(res);
      goto error;
    }
    if (insert_dynamic(objects, &obj))
    {
      my_free(obj.db);
      my_free(obj.name);
      mysql_free_result(res);
      goto error;
    }
  }
  mysql_free_result(res);
  replay_drain_results();

  my_qsort(dbs->buffer, dbs->elements, sizeof(char*), cmp_replay_string);
  my_qsort(objects->buffer, objects->elements,
           sizeof(struct st_replay_object), cmp_replay_object);
  DBUG_RETURN(0);

error:
  fprintf(stdout,
          "ReplayTest: Warning - failed to read the replay server state: "
          "%d %s\n",
          mysql_errno(replay_server_mysql), mysql_error(replay_server_mysql));
  replay_drain_results();
  free_replay_snapshot(dbs, objects);
  DBUG_RETURN(1);
}


/*
  Record the state the replay server is in before any replay script has run.
  On failure the cleanup stays disabled: with an empty baseline every object
  on the server, including the ones in the mysql schema, would look new.
*/

static void replay_capture_baseline()
{
  DBUG_ENTER("replay_capture_baseline");

  if (replay_baseline_valid)
    DBUG_VOID_RETURN;

  if (replay_capture_snapshot(&replay_baseline_dbs, &replay_baseline_objects))
  {
    fprintf(stdout, "ReplayTest: Warning - no baseline could be taken, the "
                    "replay server will not be cleaned up\n");
    DBUG_VOID_RETURN;
  }
  replay_baseline_valid= TRUE;
  verbose_msg("ReplayTest: baseline is %lu database(s), %lu table(s)/view(s)",
              (ulong) replay_baseline_dbs.elements,
              (ulong) replay_baseline_objects.elements);
  DBUG_VOID_RETURN;
}


/* Append `name` to ds as a quoted identifier */

static void append_quoted_ident(DYNAMIC_STRING *ds, const char *name)
{
  const char *p;
  dynstr_append_mem(ds, "`", 1);
  for (p= name; *p; p++)
  {
    if (*p == '`')
      dynstr_append_mem(ds, "``", 2);
    else
      dynstr_append_mem(ds, p, 1);
  }
  dynstr_append_mem(ds, "`", 1);
}


/*
  Run one cleanup statement on the replay server. Failures are reported but
  are not fatal: cleanup is best-effort, a failed drop must not abort a test.
*/

static void replay_exec_cleanup_query(const char *query, size_t len)
{
  log_replay_query(query, len);
  if (mysql_real_query(replay_server_mysql, query, (ulong) len))
  {
    fprintf(stdout, "ReplayTest: Warning - cleanup query failed: %.*s: %s\n",
            (int) len, query, mysql_error(replay_server_mysql));
    print_test_location(stdout, "ReplayTest: ");
    return;
  }
  replay_drain_results();
}


/*
  Undo what the replay script that has just finished did to the replay
  server: drop the databases, tables and views it left behind, forget the
  context it stored in @opt_context, and return to a known default database.

  Must run after the optimizer_trace of the replayed EXPLAIN has been
  captured: the statements issued here overwrite it.
*/

static void replay_restore_baseline()
{
  DYNAMIC_ARRAY cur_dbs, cur_objects, extra_dbs;
  DYNAMIC_STRING stmt;
  uint i, dropped_objects= 0;
  DBUG_ENTER("replay_restore_baseline");

  if (replay_capture_snapshot(&cur_dbs, &cur_objects))
  {
    fprintf(stdout, "ReplayTest: Warning - skipping cleanup of the replay "
                    "server\n");
    print_test_location(stdout, "ReplayTest: ");
    DBUG_VOID_RETURN;
  }

  /*
    The databases that were not there when we took the baseline. The names
    are borrowed from cur_dbs, and since cur_dbs is sorted, so is extra_dbs
    - the loop below bsearch()es it.
  */
  my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &extra_dbs, sizeof(char*),
                        16, 16, MYF(0));
  for (i= 0; i < cur_dbs.elements; i++)
  {
    char **db= (char**) dynamic_array_ptr(&cur_dbs, i);
    if (!bsearch(db, replay_baseline_dbs.buffer,
                 replay_baseline_dbs.elements, sizeof(char*),
                 cmp_replay_string) &&
        insert_dynamic(&extra_dbs, db))
    {
      /* Out of memory. The tables of this database are dropped one by one
         below, only the empty database itself stays behind. */
      fprintf(stdout, "ReplayTest: Warning - out of memory, database '%s' "
                      "is left on the replay server\n", *db);
    }
  }

  /*
    The script may have set foreign_key_checks back to 1, and the objects
    are dropped in no particular order.
  */
  replay_exec_cleanup_query(STRING_WITH_LEN("SET foreign_key_checks=0"));

  init_dynamic_string(&stmt, "", 256, 256);

  /*
    Collect the tables and views that are not in the baseline into one
    comma-separated list. The ones that sit in a database that is about to
    go away are left to DROP DATABASE.
  */
  for (i= 0; i < cur_objects.elements; i++)
  {
    struct st_replay_object *obj=
      (struct st_replay_object*) dynamic_array_ptr(&cur_objects, i);

    if (bsearch(obj, replay_baseline_objects.buffer,
                replay_baseline_objects.elements,
                sizeof(struct st_replay_object), cmp_replay_object) ||
        bsearch(&obj->db, extra_dbs.buffer, extra_dbs.elements,
                sizeof(char*), cmp_replay_string))
      continue;

    if (dropped_objects++)
      dynstr_append_mem(&stmt, ", ", 2);
    append_quoted_ident(&stmt, obj->db);
    dynstr_append_mem(&stmt, ".", 1);
    append_quoted_ident(&stmt, obj->name);
  }

  if (dropped_objects)
  {
    /*
      We did not ask the server which of these are views and which are
      tables - that would have made the snapshot open every .frm - so try
      both, the same way the replay script itself does. With IF EXISTS a
      name of the wrong kind only produces a warning, so one statement per
      kind is enough for the whole list.
    */
    DYNAMIC_STRING drop;
    init_dynamic_string(&drop, "DROP VIEW IF EXISTS ", stmt.length + 32, 128);
    dynstr_append_mem(&drop, stmt.str, stmt.length);
    replay_exec_cleanup_query(drop.str, drop.length);

    dynstr_set(&drop, "DROP TABLE IF EXISTS ");
    dynstr_append_mem(&drop, stmt.str, stmt.length);
    replay_exec_cleanup_query(drop.str, drop.length);
    dynstr_free(&drop);
  }

  for (i= 0; i < extra_dbs.elements; i++)
  {
    dynstr_set(&stmt, "DROP DATABASE IF EXISTS ");
    append_quoted_ident(&stmt, *(char**) dynamic_array_ptr(&extra_dbs, i));
    replay_exec_cleanup_query(stmt.str, stmt.length);
  }

  if (dropped_objects || extra_dbs.elements)
    verbose_msg("ReplayTest: cleanup dropped %u table(s)/view(s) and "
                "%lu database(s)",
                dropped_objects, (ulong) extra_dbs.elements);

  /*
    The script leaves the whole recorded context in @opt_context. It is
    large and it stays in the connection's memory until the next script
    overwrites it.
  */
  replay_exec_cleanup_query(STRING_WITH_LEN("SET " REPLAY_CONTEXT_USERVAR
                                            "=NULL"));

  /*
    The script's USE may have selected a database that we have just dropped.
    Get back to a database that exists: run_explain_directly_on_replay()
    runs its EXPLAIN with whatever default database is current.
  */
  replay_exec_cleanup_query(STRING_WITH_LEN("USE " REPLAY_DEFAULT_DB));

  dynstr_free(&stmt);
  delete_dynamic(&extra_dbs);
  free_replay_snapshot(&cur_dbs, &cur_objects);
  DBUG_VOID_RETURN;
}


/* TRUE if the `tok_len` bytes at `tok` are exactly the keyword `word` */

static my_bool replay_token_eq(const char *tok, size_t tok_len,
                               const LEX_CSTRING *word)
{
  return tok_len == word->length && !strncmp(tok, word->str, tok_len);
}


/*
  Handle the argument of the "disable_replay <scope> <reason>" command.

  Syntax:
    disable_replay next_query <arbitrary reason text>
    disable_replay testfile   <arbitrary reason text>

  The first token of `arg` must be "next_query" or "testfile".  Everything
  after the scope token is the reason string (spaces allowed).

  - "next_query": one-shot; the next SQL query executed via run_query_normal()
    bypasses replay-server processing (if it is EXPLAIN). The flag is consumed
    by that one query regardless of whether it is EXPLAIN.
  - "testfile": sticky; disables replay-server processing for every EXPLAIN
    until mysqltest exits.

  Any syntax violation is a hard error for the caller: the message to die()
  with is returned, NULL means success.
*/
const char *replay_do_disable(const char *arg, const char *end)
{
  const char *p= arg;
  const char *tok;
  size_t tok_len;
  size_t reason_len;
  my_bool is_testfile;
  char *reason_buf;
  size_t reason_buf_size;
  DBUG_ENTER("replay_do_disable");

  /* Skip leading whitespace */
  while (p < end && my_isspace(charset_info, *p))
    p++;

  tok= p;
  while (p < end && !my_isspace(charset_info, *p))
    p++;
  tok_len= (size_t)(p - tok);

  if (replay_token_eq(tok, tok_len, &replay_scope_next_query))
    is_testfile= FALSE;
  else if (replay_token_eq(tok, tok_len, &replay_scope_testfile))
    is_testfile= TRUE;
  else
    DBUG_RETURN(DISABLE_REPLAY_SYNTAX);

  /* Skip whitespace between the scope token and the reason */
  while (p < end && my_isspace(charset_info, *p))
    p++;

  if (p >= end)
    DBUG_RETURN(DISABLE_REPLAY_SYNTAX "  (reason missing)");

  if (is_testfile)
  {
    reason_buf= disable_replay_testfile_reason;
    reason_buf_size= sizeof(disable_replay_testfile_reason);
  }
  else
  {
    reason_buf= disable_replay_reason;
    reason_buf_size= sizeof(disable_replay_reason);
  }

  /* Copy reason, trim trailing whitespace */
  reason_len= (size_t)(end - p);
  while (reason_len > 0 &&
         my_isspace(charset_info, p[reason_len - 1]))
    reason_len--;

  if (reason_len >= reason_buf_size)
    reason_len= reason_buf_size - 1;
  memcpy(reason_buf, p, reason_len);
  reason_buf[reason_len]= '\0';

  if (is_testfile)
  {
    disable_replay_testfile= TRUE;
    verbose_msg("disable_replay: replay disabled for the rest of this test "
                "file (reason: %s)", disable_replay_testfile_reason);
  }
  else
  {
    disable_replay_next_query= TRUE;
    verbose_msg("disable_replay: next query will bypass replay server "
                "(reason: %s)", disable_replay_reason);
  }

  DBUG_RETURN(NULL);
}


/*
  Execute queries from SQL script on replay server
  Split by replay_stmt_separator and execute each query
  Append output from last query to ds
*/
static void execute_replay_queries(const char *sql_script, DYNAMIC_STRING *ds)
{
  DYNAMIC_STRING result;
  const char *p= sql_script;
  const char *query_start= p;
  my_bool found_explain= FALSE;
  DBUG_ENTER("execute_replay_queries");
  
  verbose_msg("ReplayTest: SQL script from optimizer_context:\n%s", sql_script);
  
  log_replay_session_start();
  
  init_dynamic_string(&result, "", 1024, 1024);
  
  /* Single-pass execution: stop at first EXPLAIN FORMAT=JSON */
  while (*p)
  {
    if (!strncmp(p, replay_stmt_separator, REPLAY_STMT_SEPARATOR_LEN))
    {
      size_t query_len= p - query_start;
      const char *q= query_start;
      const char *q_end= query_start + query_len;
      
      /* Skip leading whitespace */
      while (q < q_end && my_isspace(charset_info, *q))
        q++;
      
      if (q < q_end)
      {
        /* Check if this is EXPLAIN FORMAT=JSON */
        my_bool is_explain= is_explain_query(query_start, query_len);
        
        verbose_msg("ReplayTest: Executing query on replay server (%s): %.*s",
                    is_explain ? "EXPLAIN - will stop after this" : "intermediate",
                    (int)query_len, query_start);
        
        /* Log the query */
        log_replay_query(query_start, query_len);
        
        /* Execute the query */
        if (mysql_real_query(replay_server_mysql, query_start,
                             (ulong) query_len))
        {
          char buf[512];
          size_t len=
              my_snprintf(buf, sizeof(buf), "ReplayTest: Query error: %.*s: %s\n",
                          (int)query_len, query_start,
                          mysql_error(replay_server_mysql));
          fputs(buf, stdout);
          print_test_location(stdout, "ReplayTest: ");
          verbose_msg("%s", buf);

          /* Add failure marker to result output */
          dynstr_append_mem(&result, buf, len);
          
          goto cleanup;
        }

        /* Process results */
        do
        {
          MYSQL_RES *res= mysql_store_result(replay_server_mysql);
          if (res)
          {
            /* Capture output only if this is EXPLAIN FORMAT=JSON */
            if (is_explain)
            {
              MYSQL_FIELD *fields= mysql_fetch_fields(res);
              uint num_fields= mysql_num_fields(res);
              MYSQL_ROW row;
              
              if (!display_result_vertically)
              {
                append_table_headings(&result, fields, num_fields);
              }
              
              while ((row= mysql_fetch_row(res)))
              {
                uint i;
                ulong *lengths= mysql_fetch_lengths(res);
                for (i= 0; i < num_fields; i++)
                  append_field(&result, i, &fields[i],
                              row[i], lengths[i], !row[i]);
                if (!display_result_vertically)
                  dynstr_append_mem(&result, "\n", 1);
              }
            }
            mysql_free_result(res);
          }
          if (mysql_errno(replay_server_mysql))
          {
            char buf[512];
            size_t len=
                my_snprintf(buf, sizeof(buf), "ReplayTest: Query error: %.*s :%s\n",
                            (int)query_len, query_start,
                            mysql_error(replay_server_mysql));
            fputs(buf, stdout);
            if (is_explain)
              dynstr_append_mem(&result, buf, len);
          }
        } while (mysql_next_result(replay_server_mysql) == 0);

        /* Collect warnings from the EXPLAIN query (replay server) */
        if (is_explain)
          replay_append_warnings(replay_server_mysql, &result);

        /* If this was EXPLAIN, we're done - stop processing */
        if (is_explain)
        {
          found_explain= TRUE;
          /* REPLAY_SERVER_TRACE: capture replay server's optimizer_trace
             before the cleanup reset below overwrites it. The caller decides
             later whether to actually persist it. */
          if (replay_server_trace && replay_trace_capture_buf)
            capture_optimizer_trace(replay_server_mysql,
                                    replay_trace_capture_buf);
          verbose_msg("ReplayTest: Found EXPLAIN, stopping script execution");
          break;
        }
      }
      
      p+= REPLAY_STMT_SEPARATOR_LEN;
      query_start= p;
    }
    else
    {
      p++;
    }
  }
  
  /* Handle final query if we haven't found EXPLAIN yet */
  if (!found_explain && query_start < p)
  {
    const char *q= query_start;
    const char *q_end= p;
    while (q < q_end && my_isspace(charset_info, *q))
      q++;
    if (q < q_end)
    {
      size_t query_len= p - query_start;
      my_bool is_explain= is_explain_query(query_start, query_len);
      
      verbose_msg("ReplayTest: Executing final query on replay server (%s): %.*s",
                  is_explain ? "EXPLAIN" : "last query",
                  (int)query_len, query_start);
      
      /* Log the query */
      log_replay_query(query_start, query_len);

      if (mysql_real_query(replay_server_mysql, query_start,
                           (ulong) query_len))
      {
        fprintf(stdout, "ReplayTest: Query failed on replay server: %d %s\n",
                mysql_errno(replay_server_mysql),
                mysql_error(replay_server_mysql));
        fprintf(stdout, "ReplayTest: Failed query was: %.*s\n", (int)query_len, query_start);
        print_test_location(stdout, "ReplayTest: ");
        goto cleanup;
      }
      
      do
      {
        MYSQL_RES *res= mysql_store_result(replay_server_mysql);
        if (res)
        {
          MYSQL_FIELD *fields= mysql_fetch_fields(res);
          uint num_fields= mysql_num_fields(res);
          MYSQL_ROW row;
          
          if (!display_result_vertically)
          {
            append_table_headings(&result, fields, num_fields);
          }
          
          while ((row= mysql_fetch_row(res)))
          {
            uint i;
            ulong *lengths= mysql_fetch_lengths(res);
            for (i= 0; i < num_fields; i++)
              append_field(&result, i, &fields[i],
                          row[i], lengths[i], !row[i]);
            if (!display_result_vertically)
              dynstr_append_mem(&result, "\n", 1);
          }
          mysql_free_result(res);
        }
      } while (mysql_next_result(replay_server_mysql) == 0);

      if (is_explain)
      {
        /* Collect warnings from the EXPLAIN query (replay server) */
        replay_append_warnings(replay_server_mysql, &result);
        found_explain= TRUE;
        /* REPLAY_SERVER_TRACE: capture replay server's optimizer_trace
           BEFORE the cleanup reset below overwrites it. The caller decides
           later whether to actually persist it. */
        if (replay_server_trace && replay_trace_capture_buf)
          capture_optimizer_trace(replay_server_mysql,
                                  replay_trace_capture_buf);
      }
    }
  }
  
  if (!found_explain)
  {
    verbose_msg("ReplayTest: Warning - no EXPLAIN FORMAT=JSON found in script");
  }

cleanup:
  /* Preserve accumulated output (EXPLAIN / last-query) in ds BEFORE cleanup query */
  dynstr_append_mem(ds, result.str, result.length);
  dynstr_free(&result);

  /* Reset optimizer_replay_context on the replay server, regardless of errors.
     Drain and discard any output so ds is not affected. */
  if (replay_server_mysql)
  {
    if (mysql_real_query(replay_server_mysql,
                         STRING_WITH_LEN("SET " REPLAY_CONTEXT_SYSVAR "=''")))
    {
      fprintf(stdout, "ReplayTest: Warning - failed to reset %s: %d %s\n",
              REPLAY_CONTEXT_SYSVAR,
              mysql_errno(replay_server_mysql),
              mysql_error(replay_server_mysql));
    }
    else
      replay_drain_results();

    /* Drop what this script has created, so that the next replay run starts
       from the same state as this one did. */
    if (replay_cleanup && replay_baseline_valid)
      replay_restore_baseline();
  }

  DBUG_VOID_RETURN;
}


/*
  Run an EXPLAIN query directly on the replay server (no context replay),
  appending its output (headings + rows + warnings) to ds.

  This is the fallback used when the test server produced an empty
  optimizer_context for an EXPLAIN query.
*/
static void run_explain_directly_on_replay(const char *query, size_t query_len,
                                           DYNAMIC_STRING *ds)
{
  DBUG_ENTER("run_explain_directly_on_replay");

  if (ensure_replay_server_connection() != 0)
  {
    fprintf(stdout, "ReplayTest: Failed to connect to replay server\n");
    DBUG_VOID_RETURN;
  }

  if (mysql_real_query(replay_server_mysql, query, (ulong)query_len))
  {
    char buf[512];
    size_t len= my_snprintf(
        buf, sizeof(buf),
        "ReplayTest: Direct EXPLAIN failed on replay server: %d %s\n",
        mysql_errno(replay_server_mysql), mysql_error(replay_server_mysql));
    fputs(buf, stdout);
    fprintf(stdout, "ReplayTest: Failed query was: %.*s\n",
            (int)query_len, query);
    dynstr_append_mem(ds, buf, len);
    DBUG_VOID_RETURN;
  }

  do
  {
    MYSQL_RES *res= mysql_store_result(replay_server_mysql);
    if (res)
    {
      MYSQL_FIELD *fields= mysql_fetch_fields(res);
      uint num_fields= mysql_num_fields(res);
      MYSQL_ROW row;

      if (!display_result_vertically)
        append_table_headings(ds, fields, num_fields);

      while ((row= mysql_fetch_row(res)))
      {
        uint i;
        ulong *lengths= mysql_fetch_lengths(res);
        for (i= 0; i < num_fields; i++)
          append_field(ds, i, &fields[i], row[i], lengths[i], !row[i]);
        if (!display_result_vertically)
          dynstr_append_mem(ds, "\n", 1);
      }
      mysql_free_result(res);
    }
  } while (mysql_next_result(replay_server_mysql) == 0);

  /* Append warnings from the EXPLAIN query */
  replay_append_warnings(replay_server_mysql, ds);

  /* REPLAY_SERVER_TRACE: capture the replay server's optimizer_trace for
     this directly-run EXPLAIN. The caller decides later whether to flush. */
  if (replay_server_trace && replay_trace_capture_buf)
    capture_optimizer_trace(replay_server_mysql,
                            replay_trace_capture_buf);

  DBUG_VOID_RETURN;
}


/*
  Run one statement on the test server and discard whatever it returns.

  Used for the SET statements that bracket a replayed EXPLAIN. Returns TRUE
  on error; `errmsg` is then printed if it is not NULL - pass NULL for the
  statements whose failure we deliberately ignore.
*/
static my_bool replay_exec_on_test_server(MYSQL *mysql, const char *stmt,
                                          size_t len, const char *errmsg)
{
  MYSQL_RES *res;
  if (mysql_real_query(mysql, stmt, (ulong) len))
  {
    if (errmsg)
      fprintf(stdout, "ReplayTest: %s: %d %s\n", errmsg,
              mysql_errno(mysql), mysql_error(mysql));
    return TRUE;
  }
  if ((res= mysql_store_result(mysql)))
    mysql_free_result(res);
  return FALSE;
}


/*
  Undo on the test server what replay_hook_pre_query() set up for this
  EXPLAIN: stop recording optimizer contexts and put optimizer_trace back to
  the value saved in @mtr_save_opt_trace.
*/
void replay_undo_test_server_setup(MYSQL *mysql)
{
  replay_exec_on_test_server(mysql,
                       STRING_WITH_LEN("SET " REPLAY_RECORD_SYSVAR "=0"),
                       NULL);
  if (replay_server_trace)
    replay_exec_on_test_server(mysql,
                       STRING_WITH_LEN("SET optimizer_trace="
                                       REPLAY_SAVED_TRACE_USERVAR),
                       NULL);
}


/*
  Make the EXPLAIN query available to the replay-side helpers and, when
  REPLAY_SERVER_TRACE is on, grab the test server's optimizer_trace for it and
  arm the replay-side capture buffer.

  Must be called before anything else runs on the test server's connection,
  as that would overwrite the trace we are after.
*/
static void replay_arm_trace_capture(MYSQL *mysql, const char *query,
                                     size_t query_len,
                                     DYNAMIC_STRING *orig_trace,
                                     DYNAMIC_STRING *replay_trace)
{
  replay_current_explain= query;
  replay_current_explain_len= query_len;
  if (replay_server_trace)
  {
    capture_optimizer_trace(mysql, orig_trace);
    replay_trace_capture_buf= replay_trace;
  }
}


static void replay_disarm_trace_capture()
{
  replay_current_explain= NULL;
  replay_current_explain_len= 0;
  replay_trace_capture_buf= NULL;
}


/*
  Collect the test server's own EXPLAIN output - headings, rows and warnings -
  into `ds`, formatted the way the replay side formats it so that the two can
  be compared. Consumes *res.
*/
static void replay_collect_primary_explain(MYSQL *mysql, MYSQL_RES **res,
                                           MYSQL_FIELD *fields,
                                           uint num_fields,
                                           DYNAMIC_STRING *ds)
{
  if (!display_result_vertically)
    append_table_headings(ds, fields, num_fields);
  append_result(ds, *res);
  mysql_free_result(*res);
  *res= 0;

  replay_append_warnings(mysql, ds);
}


enum replay_context_status
{
  REPLAY_CONTEXT_FOUND,   /* a context script was recorded, `script` has it */
  REPLAY_CONTEXT_EMPTY,   /* the EXPLAIN recorded no context                */
  REPLAY_CONTEXT_ERROR    /* the context could not be read at all           */
};


/*
  Read the optimizer context that the test server recorded for the EXPLAIN
  that has just run, into `script`.
*/
static enum replay_context_status replay_fetch_context(MYSQL *mysql,
                                                      DYNAMIC_STRING *script)
{
  MYSQL_RES *res;
  MYSQL_ROW row;
  enum replay_context_status status= REPLAY_CONTEXT_EMPTY;
  DBUG_ENTER("replay_fetch_context");

  fprintf(stdout, "ReplayTest: Loading context \n");
  if (mysql_real_query(mysql, STRING_WITH_LEN("SELECT context FROM "
                                              REPLAY_IS_CONTEXT_TABLE)))
  {
    fprintf(stdout, "ReplayTest: Failed to query %s: %d %s\n",
            REPLAY_IS_CONTEXT_TABLE, mysql_errno(mysql), mysql_error(mysql));
    DBUG_RETURN(REPLAY_CONTEXT_ERROR);
  }

  if (!(res= mysql_store_result(mysql)))
    DBUG_RETURN(REPLAY_CONTEXT_EMPTY);

  if (mysql_num_rows(res) > 0 && (row= mysql_fetch_row(res)) && row[0])
  {
    dynstr_set(script, row[0]);
    status= REPLAY_CONTEXT_FOUND;
  }
  mysql_free_result(res);
  DBUG_RETURN(status);
}


/*
  If the recorded context contains a marker that we know cannot be replayed
  correctly, return that marker, otherwise NULL.
*/
static const char *replay_context_stop_word(const char *script)
{
  for (const char **sw= replay_context_stop_words; *sw; sw++)
    if (strstr(script, *sw))
      return *sw;
  return NULL;
}


static my_bool replay_explains_match(const DYNAMIC_STRING *a,
                                     const DYNAMIC_STRING *b)
{
  return a->length == b->length && memcmp(a->str, b->str, a->length) == 0;
}


/*
  Produce, in `ds_replay`, the EXPLAIN output of the replay server for the
  EXPLAIN that has just run on the test server.

    ds_primary       the test server's own output, used as the fallback and as
                     the comparison base for the trace dump
    ds_orig_trace,
    ds_replay_trace  receive the optimizer traces of the two servers, but only
                     when REPLAY_SERVER_TRACE is on

  On return the test server is back in the state it was in before
  replay_hook_pre_query() ran.
*/
static void replay_explain_on_replay_server(MYSQL *mysql, const char *query,
                                            size_t query_len,
                                            const DYNAMIC_STRING *ds_primary,
                                            DYNAMIC_STRING *ds_replay,
                                            DYNAMIC_STRING *ds_orig_trace,
                                            DYNAMIC_STRING *ds_replay_trace)
{
  DYNAMIC_STRING script;
  enum replay_context_status status;
  const char *stop_word= NULL;
  DBUG_ENTER("replay_explain_on_replay_server");

  init_dynamic_string(&script, "", 1024, 1024);
  status= replay_fetch_context(mysql, &script);

  if (status == REPLAY_CONTEXT_FOUND)
    stop_word= replay_context_stop_word(script.str);

  if (status == REPLAY_CONTEXT_ERROR)
  {
    /*
      We could not read the context, so there is nothing to replay: leave
      `ds_replay` empty and let the test fail on the missing EXPLAIN output
      rather than pass on the test server's result.
    */
  }
  else if (stop_word)
  {
    /* Something we know we cannot replay: report the test server's result. */
    verbose_msg("ReplayTest: stop word '%s' found in optimizer_context, "
                "using primary EXPLAIN result", stop_word);
    dynstr_append_mem(ds_replay, ds_primary->str, ds_primary->length);
  }
  else
  {
    replay_arm_trace_capture(mysql, query, query_len,
                             ds_orig_trace, ds_replay_trace);
    if (status == REPLAY_CONTEXT_FOUND)
    {
      if (ensure_replay_server_connection() == 0)
        execute_replay_queries(script.str, ds_replay);
      else
        fprintf(stdout, "ReplayTest: Failed to connect to replay server\n");
    }
    else
    {
      /* No context recorded: run the EXPLAIN on the replay server as it is. */
      verbose_msg("ReplayTest: empty optimizer_context, running EXPLAIN "
                  "directly on replay server");
      run_explain_directly_on_replay(query, query_len, ds_replay);
    }
    replay_disarm_trace_capture();
  }

  replay_undo_test_server_setup(mysql);
  dynstr_free(&script);
  DBUG_VOID_RETURN;
}


/*
  Consume the one-shot "disable_replay next_query" flag. It applies to exactly
  one query executed through run_query_normal(), whether or not it is an
  EXPLAIN. Returns TRUE if this query is the one it applies to.
*/
static my_bool replay_consume_disable_next_query()
{
  if (!disable_replay_next_query)
    return FALSE;
  verbose_msg("ReplayTest: replay disabled for this query (reason: %s)",
              disable_replay_reason);
  disable_replay_next_query= FALSE;
  disable_replay_reason[0]= '\0';
  return TRUE;
}


/*
  Pre-query hook of the replay-server mode.  See mysqltest_replay_server.h.
*/
my_bool replay_hook_pre_query(MYSQL *mysql, my_bool complete_query,
                              const char *query, size_t query_len)
{
  DBUG_ENTER("replay_hook_pre_query");
  my_bool disabled= replay_consume_disable_next_query();

  if (!replay_test_mode || disabled || disable_replay_testfile ||
      !complete_query || !is_explain_query(query, query_len))
    DBUG_RETURN(FALSE);

  verbose_msg("ReplayTest: Detected EXPLAIN FORMAT=JSON query, "
              "activating replay mode");

  /*
    Clear any context left over from an earlier query (e.g. a prior EXPLAIN
    whose context must not leak into this one), then record ours.
  */
  replay_exec_on_test_server(mysql,
                       STRING_WITH_LEN("SET " REPLAY_RECORD_SYSVAR "=0"),
                       NULL);
  if (replay_exec_on_test_server(mysql,
                       STRING_WITH_LEN("SET " REPLAY_RECORD_SYSVAR "=1"),
                       "Failed to set " REPLAY_RECORD_SYSVAR))
    DBUG_RETURN(FALSE);

  /*
    REPLAY_SERVER_TRACE: enable optimizer_trace on the test server too, saving
    its current value into REPLAY_SAVED_TRACE_USERVAR so that
    replay_undo_test_server_setup() can restore it. The replay side is enabled
    once per connection in ensure_replay_server_connection().
  */
  if (replay_server_trace)
    replay_exec_on_test_server(mysql,
              STRING_WITH_LEN("SET " REPLAY_SAVED_TRACE_USERVAR
                              "=@@optimizer_trace, "
                              "optimizer_trace='enabled=on'"),
              "Warning - failed to enable optimizer_trace on test server");

  DBUG_RETURN(TRUE);
}


/*
  Result hook of the replay-server mode: replace the EXPLAIN result of the test
  server with the one the replay server produces from the recorded optimizer
  context, and, under REPLAY_SERVER_TRACE, dump both optimizer traces when the
  two EXPLAINs disagree.
*/
void replay_hook_result(MYSQL *mysql, MYSQL_RES **res,
                        MYSQL_FIELD *fields, uint num_fields,
                        const char *query, size_t query_len,
                        DYNAMIC_STRING *ds)
{
  DYNAMIC_STRING ds_primary, ds_replay, ds_orig_trace, ds_replay_trace;
  DBUG_ENTER("replay_hook_result");

  init_dynamic_string(&ds_primary, "", 1024, 1024);
  init_dynamic_string(&ds_replay, "", 1024, 1024);
  init_dynamic_string(&ds_orig_trace, "", 1024, 1024);
  init_dynamic_string(&ds_replay_trace, "", 1024, 1024);

  replay_collect_primary_explain(mysql, res, fields, num_fields, &ds_primary);
  replay_explain_on_replay_server(mysql, query, query_len, &ds_primary,
                                  &ds_replay, &ds_orig_trace,
                                  &ds_replay_trace);

  /* The replay-side output is what the test sees. */
  dynstr_append_mem(ds, ds_replay.str, ds_replay.length);

  /*
    REPLAY_SERVER_TRACE: the traces are only worth keeping when the two
    EXPLAINs (rows + warnings) differ.
  */
  if (replay_server_trace && !replay_explains_match(&ds_primary, &ds_replay))
  {
    flush_trace_block(&replay_opt_trace_original_file,
                      replay_opt_trace_original_path,
                      query, query_len, &ds_orig_trace);
    flush_trace_block(&replay_opt_trace_replay_file,
                      replay_opt_trace_replay_path,
                      query, query_len, &ds_replay_trace);
  }

  dynstr_free(&ds_primary);
  dynstr_free(&ds_replay);
  dynstr_free(&ds_orig_trace);
  dynstr_free(&ds_replay_trace);
  DBUG_VOID_RETURN;
}


/*
  Set up replay-server mode from the environment.  Does nothing at all unless
  REPLAY_SERVER_SOCKET names a replay server, which is the ordinary case.
*/
void replay_init(const char *result_file_name)
{
  const char *vardir;
  const char *no_cleanup_env;
  const char *trace_env;
  DBUG_ENTER("replay_init");

  replay_server_socket= getenv(REPLAY_ENV_SOCKET);
  if (!replay_server_socket || !replay_server_socket[0])
    DBUG_VOID_RETURN;

  replay_test_mode= TRUE;
  verbose_msg("ReplayTest mode enabled, replay server socket: %s",
              replay_server_socket);

  /* Initialize replay query log file */
  if ((vardir= getenv("MYSQLTEST_VARDIR")))
  {
    size_t path_len= strlen(vardir) + sizeof(REPLAY_QUERY_LOG_SUBPATH);
    char *log_path= (char*) my_malloc(PSI_NOT_INSTRUMENTED, path_len, MYF(0));
    if (log_path)
    {
      snprintf(log_path, path_len, "%s" REPLAY_QUERY_LOG_SUBPATH, vardir);
      replay_log_path= log_path;
      /* Use append mode - MTR cleans var directory on each run */
      replay_log_file= fopen(replay_log_path, "a");
      if (!replay_log_file)
        fprintf(stderr, "Warning: Could not open replay log file: %s\n",
                replay_log_path);
      else
        verbose_msg("ReplayTest: Logging queries to %s", replay_log_path);
    }
  }

  /* REPLAY_ENV_NO_CLEANUP: keep whatever the replay scripts create on
     the replay server, for post-mortem inspection. */
  no_cleanup_env= getenv(REPLAY_ENV_NO_CLEANUP);
  if (no_cleanup_env && no_cleanup_env[0])
  {
    replay_cleanup= FALSE;
    fprintf(stderr, "mysqltest: %s is ON, the replay server will not be "
                    "cleaned up between runs\n", REPLAY_ENV_NO_CLEANUP);
  }

  /* REPLAY_ENV_TRACE: also dump optimizer_trace from both servers. */
  trace_env= getenv(REPLAY_ENV_TRACE);
  if (trace_env && trace_env[0])
  {
    replay_server_trace= TRUE;
    fprintf(stderr, "mysqltest: %s is ON\n", REPLAY_ENV_TRACE);
    if (result_file_name)
    {
      char buf[FN_REFLEN];
      fn_format(buf, result_file_name, "", REPLAY_TRACE_EXT_ORIGINAL,
                MY_REPLACE_EXT);
      replay_opt_trace_original_path=
          my_strdup(PSI_NOT_INSTRUMENTED, buf, MYF(MY_WME));
      fn_format(buf, result_file_name, "", REPLAY_TRACE_EXT_REPLAY,
                MY_REPLACE_EXT);
      replay_opt_trace_replay_path=
          my_strdup(PSI_NOT_INSTRUMENTED, buf, MYF(MY_WME));
      /* Start each test run with a fresh trace dump; the files are
         re-created lazily by flush_trace_block() if and when an EXPLAIN
         diverges. */
      (void) my_delete(replay_opt_trace_original_path, MYF(0));
      (void) my_delete(replay_opt_trace_replay_path, MYF(0));
      verbose_msg("ReplayTest: optimizer_trace dumps -> %s , %s",
                  replay_opt_trace_original_path,
                  replay_opt_trace_replay_path);
      fprintf(stderr, "mysqltest: %s: %s\n", REPLAY_ENV_TRACE,
              replay_opt_trace_original_path);
    }
    else
    {
      fprintf(stderr,
              "Warning: %s is set but no --result-file was given; "
              "optimizer_trace dumps will be skipped.\n", REPLAY_ENV_TRACE);
    }
  }
  DBUG_VOID_RETURN;
}


/*
  Close the connection to the replay server and release everything
  replay_init() and the replay run allocated.
*/
void replay_free(void)
{
  DBUG_ENTER("replay_free");

  if (replay_server_mysql)
  {
    mysql_close(replay_server_mysql);
    replay_server_mysql= NULL;
  }

  if (replay_baseline_valid)
  {
    free_replay_snapshot(&replay_baseline_dbs, &replay_baseline_objects);
    replay_baseline_valid= FALSE;
  }

  if (replay_log_file)
  {
    fclose(replay_log_file);
    replay_log_file= NULL;
  }

  if (replay_log_path)
  {
    my_free((void*)replay_log_path);
    replay_log_path= NULL;
  }

  if (replay_opt_trace_original_file)
  {
    fclose(replay_opt_trace_original_file);
    replay_opt_trace_original_file= NULL;
  }
  if (replay_opt_trace_replay_file)
  {
    fclose(replay_opt_trace_replay_file);
    replay_opt_trace_replay_file= NULL;
  }
  if (replay_opt_trace_original_path)
  {
    my_free((void*)replay_opt_trace_original_path);
    replay_opt_trace_original_path= NULL;
  }
  if (replay_opt_trace_replay_path)
  {
    my_free((void*)replay_opt_trace_replay_path);
    replay_opt_trace_replay_path= NULL;
  }
  DBUG_VOID_RETURN;
}
