/*
   Copyright (c) 2000, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* Copy data from a textfile to table */
/* 2006-12 Erik Wetterberg : LOAD XML added */

#include "sql_load.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"          // fill_record_n_invoke_before_triggers
#include <my_dir.h>
#include "sql_view.h"                           // check_key_in_view
#include "sql_insert.h" // check_that_all_fields_are_given_values,
                        // prepare_triggers_for_insert_stmt,
                        // write_record
#include "auth_common.h"// INSERT_ACL, UPDATE_ACL
#include "log_event.h"  // Delete_file_log_event,
                        // Execute_load_query_log_event,
                        // LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F
#include <m_ctype.h>
#include "rpl_mi.h"
#include "rpl_slave.h"
#include "table_trigger_dispatcher.h"  // Table_trigger_dispatcher
#include "sql_show.h"
#include "item_timefunc.h"  // Item_func_now_local
#include "rpl_rli.h"     // Relay_log_info
#include "log.h"

#include "pfs_file_provider.h"
#include "mysql/psi/mysql_file.h"

#include <algorithm>

#include "../storage/tianmu/handler/ha_my_tianmu.h" // tianmu code

using std::min;
using std::max;

class XML_TAG {
public:
  int level;
  String field;
  String value;
  XML_TAG(int l, String f, String v);
};


XML_TAG::XML_TAG(int l, String f, String v)
{
  level= l;
  field.append(f);
  value.append(v);
}


#define GET (stack_pos != stack ? *--stack_pos : my_b_get(&cache))
#define PUSH(A) *(stack_pos++)=(A)

class READ_INFO {
  File	file;
  uchar	*buffer,			/* Buffer for read text */
	*end_of_buff;			/* Data in bufferts ends here */
  uint	buff_length;			/* Length of buffer */
  const uchar *field_term_ptr, *line_term_ptr;
  const char *line_start_ptr, *line_start_end;
  size_t	field_term_length,line_term_length,enclosed_length;
  int	field_term_char,line_term_char,enclosed_char,escape_char;
  int	*stack,*stack_pos;
  bool	found_end_of_line,start_of_line,eof;
  bool  need_end_io_cache;
  IO_CACHE cache;
  int level; /* for load xml */

public:
  bool error,line_cuted,found_null,enclosed;
  uchar	*row_start,			/* Found row starts here */
	*row_end;			/* Found row ends here */
  const CHARSET_INFO *read_charset;

  READ_INFO(File file,uint tot_length,const CHARSET_INFO *cs,
	    const String &field_term,
            const String &line_start,
            const String &line_term,
	    const String &enclosed,
            int escape,bool get_it_from_net, bool is_fifo);
  ~READ_INFO();
  int read_field();
  int read_fixed_length(void);
  int next_line(void);
  char unescape(char chr);
  int terminator(const uchar *ptr, size_t length);
  bool find_start_of_fields();
  /* load xml */
  List<XML_TAG> taglist;
  int read_value(int delim, String *val);
  int read_xml();
  int clear_level(int level);

  /*
    We need to force cache close before destructor is invoked to log
    the last read block
  */
  void end_io_cache()
  {
    ::end_io_cache(&cache);
    need_end_io_cache = 0;
  }

  /*
    Either this method, or we need to make cache public
    Arg must be set from mysql_load() since constructor does not see
    either the table or THD value
  */
  void set_io_cache_arg(void* arg) { cache.arg = arg; }

  /**
    skip all data till the eof.
  */
  void skip_data_till_eof()
  {
    while (GET != my_b_EOF)
      ;
  }
};

static int read_fixed_length(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                             List<Item> &fields_vars, List<Item> &set_fields,
                             List<Item> &set_values, READ_INFO &read_info,
			     ulong skip_lines);
static int read_sep_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                          List<Item> &fields_vars, List<Item> &set_fields,
                          List<Item> &set_values, READ_INFO &read_info,
			  const String &enclosed, ulong skip_lines);

static int read_xml_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                          List<Item> &fields_vars, List<Item> &set_fields,
                          List<Item> &set_values, READ_INFO &read_info,
                          ulong skip_lines);

#ifndef EMBEDDED_LIBRARY
static bool write_execute_load_query_log_event(THD *thd, sql_exchange* ex,
                                               const char* db_arg, /* table's database */
                                               const char* table_name_arg,
                                               bool is_concurrent,
                                               enum enum_duplicates duplicates,
                                               bool transactional_table,
                                               int errocode);
#endif /* EMBEDDED_LIBRARY */

/*
  Execute LOAD DATA query

  SYNOPSYS
    mysql_load()
      thd - current thread
      ex  - sql_exchange object representing source file and its parsing rules
      table_list  - list of tables to which we are loading data
      fields_vars - list of fields and variables to which we read
                    data from file
      set_fields  - list of fields mentioned in set clause
      set_values  - expressions to assign to fields in previous list
      handle_duplicates - indicates whenever we should emit error or
                          replace row if we will meet duplicates.
      read_file_from_client - is this LOAD DATA LOCAL ?

  RETURN VALUES
    TRUE - error / FALSE - success
*/

int mysql_load(THD *thd,sql_exchange *ex,TABLE_LIST *table_list,
	        List<Item> &fields_vars, List<Item> &set_fields,
                List<Item> &set_values,
                enum enum_duplicates handle_duplicates,
                bool read_file_from_client)
{
  char name[FN_REFLEN];
  File file;
  int error= 0;
  const String *field_term= ex->field.field_term;
  const String *escaped=    ex->field.escaped;
  const String *enclosed=   ex->field.enclosed;
  bool is_fifo=0;
  SELECT_LEX *select= thd->lex->select_lex;
#ifndef EMBEDDED_LIBRARY
  LOAD_FILE_INFO lf_info;
  THD::killed_state killed_status= THD::NOT_KILLED;
  bool is_concurrent;
  bool transactional_table;
#endif
  const char *db = table_list->db;			// This is never null
  /*
    If path for file is not defined, we will use the current database.
    If this is not set, we will use the directory where the table to be
    loaded is located
  */
  const char *tdb= thd->db().str ? thd->db().str : db; //Result is never null
  ulong skip_lines= ex->skip_lines;
  DBUG_ENTER("mysql_load");
  if (mysql_bin_log.is_open())
  {
    lf_info.thd = thd;
    lf_info.wrote_create_file = 0;
    lf_info.last_pos_in_file = HA_POS_ERROR;
    lf_info.log_delayed= true;
  }
  //END

  /*
    Bug #34283
    mysqlbinlog leaves tmpfile after termination if binlog contains
    load data infile, so in mixed mode we go to row-based for
    avoiding the problem.
  */
  thd->set_current_stmt_binlog_format_row_if_mixed();

#ifdef EMBEDDED_LIBRARY
  read_file_from_client  = 0; //server is always in the same process 
#endif

  if (escaped->length() > 1 || enclosed->length() > 1)
  {
    my_message(ER_WRONG_FIELD_TERMINATORS,ER(ER_WRONG_FIELD_TERMINATORS),
	       MYF(0));
    DBUG_RETURN(TRUE);
  }

  /* Report problems with non-ascii separators */
  if (!escaped->is_ascii() || !enclosed->is_ascii() ||
      !field_term->is_ascii() ||
      !ex->line.line_term->is_ascii() || !ex->line.line_start->is_ascii())
  {
    push_warning(thd, Sql_condition::SL_WARNING,
                 WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED,
                 ER(WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED));
  } 

  if (open_and_lock_tables(thd, table_list, 0))
    DBUG_RETURN(true);

  THD_STAGE_INFO(thd, stage_executing);
  if (select->setup_tables(thd, table_list, false))
    DBUG_RETURN(true);

  if (run_before_dml_hook(thd))
    DBUG_RETURN(true);

  if (table_list->is_view() && select->resolve_derived(thd, false))
    DBUG_RETURN(true);                   /* purecov: inspected */

  TABLE_LIST *const insert_table_ref=
    table_list->is_updatable() &&        // View must be updatable
    !table_list->is_multiple_tables() && // Multi-table view not allowed
    !table_list->is_derived() ?          // derived tables not allowed
    table_list->updatable_base_table() : NULL;

  if (insert_table_ref == NULL ||
      check_key_in_view(thd, table_list, insert_table_ref))
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias, "LOAD");
    DBUG_RETURN(TRUE);
  }
  if (select->derived_table_count &&
      select->check_view_privileges(thd, INSERT_ACL, SELECT_ACL))
    DBUG_RETURN(true);                   /* purecov: inspected */

  if (table_list->is_merged())
  {
    if (table_list->prepare_check_option(thd))
      DBUG_RETURN(TRUE);

    if (handle_duplicates == DUP_REPLACE &&
        table_list->prepare_replace_filter(thd))
      DBUG_RETURN(true);
  }

  // Pass the check option down to the underlying table:
  insert_table_ref->check_option= table_list->check_option;
  /*
    Let us emit an error if we are loading data to table which is used
    in subselect in SET clause like we do it for INSERT.

    The main thing to fix to remove this restriction is to ensure that the
    table is marked to be 'used for insert' in which case we should never
    mark this table as 'const table' (ie, one that has only one row).
  */
  if (unique_table(thd, insert_table_ref, table_list->next_global, 0))
  {
    my_error(ER_UPDATE_TABLE_USED, MYF(0), table_list->table_name);
    DBUG_RETURN(TRUE);
  }

  TABLE *const table= insert_table_ref->table;

  for (Field **cur_field= table->field; *cur_field; ++cur_field)
    (*cur_field)->reset_warnings();

#ifndef EMBEDDED_LIBRARY
  transactional_table= table->file->has_transactions();
  is_concurrent= (table_list->lock_type == TL_WRITE_CONCURRENT_INSERT);
#endif

  if (!fields_vars.elements)
  {
    Field_iterator_table_ref field_iterator;
    field_iterator.set(table_list);
    for (; !field_iterator.end_of_fields(); field_iterator.next())
    {
      Item *item;
      if (!(item= field_iterator.create_item(thd)))
        DBUG_RETURN(TRUE);

      if (item->field_for_view_update() == NULL)
      {
        my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), item->item_name.ptr());
        DBUG_RETURN(true);
      }
      fields_vars.push_back(item->real_item());
    }
    bitmap_set_all(table->write_set);
    /*
      Let us also prepare SET clause, altough it is probably empty
      in this case.
    */
    if (setup_fields(thd, Ref_ptr_array(), set_fields, INSERT_ACL, NULL,
                     false, true) ||
        setup_fields(thd, Ref_ptr_array(), set_values, SELECT_ACL, NULL,
                     false, false))
      DBUG_RETURN(TRUE);
  }
  else
  {						// Part field list
    /*
      Because fields_vars may contain user variables,
      pass false for column_update in first call below.
    */
    if (setup_fields(thd, Ref_ptr_array(), fields_vars, INSERT_ACL, NULL,
                     false, false) ||
        setup_fields(thd, Ref_ptr_array(), set_fields, INSERT_ACL, NULL,
                     false, true))
      DBUG_RETURN(TRUE);

    /*
      Special updatability test is needed because fields_vars may contain
      a mix of column references and user variables.
    */
    Item *item;
    List_iterator<Item> it(fields_vars);
    while ((item= it++))
    {
      if ((item->type() == Item::FIELD_ITEM ||
           item->type() == Item::REF_ITEM) &&
          item->field_for_view_update() == NULL)
      {
        my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), item->item_name.ptr());
        DBUG_RETURN(true);
      }
    }
    /* We explicitly ignore the return value */
    (void)check_that_all_fields_are_given_values(thd, table, table_list);
    /* Fix the expressions in SET clause */
    if (setup_fields(thd, Ref_ptr_array(), set_values, SELECT_ACL, NULL,
                     false, false))
      DBUG_RETURN(TRUE);
  }

  if (!Tianmu::DBHandler::ha_my_tianmu_load(thd, ex, table_list,
                                          (void *)&lf_info)) {
    DBUG_RETURN(FALSE);
  }

  const int escape_char= (escaped->length() && (ex->escaped_given() ||
                          !(thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)))
                          ? (*escaped)[0] : INT_MAX;

  /*
    * LOAD DATA INFILE fff INTO TABLE xxx SET columns2
    sets all columns, except if file's row lacks some: in that case,
    defaults are set by read_fixed_length() and read_sep_field(),
    not by COPY_INFO.
    * LOAD DATA INFILE fff INTO TABLE xxx (columns1) SET columns2=
    may need a default for columns other than columns1 and columns2.
  */
  const bool manage_defaults= fields_vars.elements != 0;
  COPY_INFO info(COPY_INFO::INSERT_OPERATION,
                 &fields_vars, &set_fields,
                 manage_defaults,
                 handle_duplicates, escape_char);

  if (info.add_function_default_columns(table, table->write_set))
    DBUG_RETURN(TRUE);

  prepare_triggers_for_insert_stmt(table);

  uint tot_length=0;
  bool use_blobs= 0, use_vars= 0;
  List_iterator_fast<Item> it(fields_vars);
  Item *item;

  while ((item= it++))
  {
    Item *real_item= item->real_item();

    if (real_item->type() == Item::FIELD_ITEM)
    {
      Field *field= ((Item_field*)real_item)->field;
      if (field->flags & BLOB_FLAG)
      {
        use_blobs= 1;
        tot_length+= 256;			// Will be extended if needed
      }
      else
        tot_length+= field->field_length;
    }
    else if (item->type() == Item::STRING_ITEM)
      use_vars= 1;
  }
  if (use_blobs && !ex->line.line_term->length() && !field_term->length())
  {
    my_message(ER_BLOBS_AND_NO_TERMINATED,ER(ER_BLOBS_AND_NO_TERMINATED),
	       MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (use_vars && !field_term->length() && !enclosed->length())
  {
    my_error(ER_LOAD_FROM_FIXED_SIZE_ROWS_TO_VAR, MYF(0));
    DBUG_RETURN(TRUE);
  }

#ifndef EMBEDDED_LIBRARY
  if (read_file_from_client)
  {
    (void)net_request_file(thd->get_protocol_classic()->get_net(),
                           ex->file_name);
    file = -1;
  }
  else
#endif
  {
    if (!dirname_length(ex->file_name))
    {
      strxnmov(name, FN_REFLEN-1, mysql_real_data_home, tdb, NullS);
      (void) fn_format(name, ex->file_name, name, "",
		       MY_RELATIVE_PATH | MY_UNPACK_FILENAME);
    }
    else
    {
      (void) fn_format(name, ex->file_name, mysql_real_data_home, "",
                       MY_RELATIVE_PATH | MY_UNPACK_FILENAME |
                       MY_RETURN_REAL_PATH);
    }

    if ((thd->system_thread &
         (SYSTEM_THREAD_SLAVE_SQL | SYSTEM_THREAD_SLAVE_WORKER)) != 0)
    {
#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
      Relay_log_info* rli= thd->rli_slave->get_c_rli();

      if (strncmp(rli->slave_patternload_file, name,
                  rli->slave_patternload_file_size))
      {
        /*
          LOAD DATA INFILE in the slave SQL Thread can only read from 
          --slave-load-tmpdir". This should never happen. Please, report a bug.
        */

        sql_print_error("LOAD DATA INFILE in the slave SQL Thread can only read from --slave-load-tmpdir. " \
                        "Please, report a bug.");
        my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--slave-load-tmpdir");
        DBUG_RETURN(TRUE);
      }
#else
      /*
        This is impossible and should never happen.
      */
      assert(FALSE); 
#endif
    }
    else if (!is_secure_file_path(name))
    {
      /* Read only allowed from within dir specified by secure_file_priv */
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
      DBUG_RETURN(TRUE);
    }

#if !defined(_WIN32)
    MY_STAT stat_info;
    if (!my_stat(name, &stat_info, MYF(MY_WME)))
      DBUG_RETURN(TRUE);

    // if we are not in slave thread, the file must be:
    if (!thd->slave_thread &&
        !((stat_info.st_mode & S_IFLNK) != S_IFLNK &&   // symlink
          ((stat_info.st_mode & S_IFREG) == S_IFREG ||  // regular file
           (stat_info.st_mode & S_IFIFO) == S_IFIFO)))  // named pipe
    {
      my_error(ER_TEXTFILE_NOT_READABLE, MYF(0), name);
      DBUG_RETURN(TRUE);
    }
    if ((stat_info.st_mode & S_IFIFO) == S_IFIFO)
      is_fifo= 1;
#endif
    if ((file= mysql_file_open(key_file_load,
                               name, O_RDONLY, MYF(MY_WME))) < 0)

      DBUG_RETURN(TRUE);
  }

  READ_INFO read_info(file,tot_length,
                      ex->cs ? ex->cs : thd->variables.collation_database,
		      *field_term,*ex->line.line_start, *ex->line.line_term,
                      *enclosed,
		      info.escape_char, read_file_from_client, is_fifo);
  if (read_info.error)
  {
    if (file >= 0)
      mysql_file_close(file, MYF(0));           // no files in net reading
    DBUG_RETURN(TRUE);				// Can't allocate buffers
  }

#ifndef EMBEDDED_LIBRARY
  if (mysql_bin_log.is_open())
  {
    lf_info.thd = thd;
    lf_info.wrote_create_file = 0;
    lf_info.last_pos_in_file = HA_POS_ERROR;
    lf_info.log_delayed= transactional_table;
    read_info.set_io_cache_arg((void*) &lf_info);
  }
#endif /*!EMBEDDED_LIBRARY*/

  thd->count_cuted_fields= CHECK_FIELD_WARN;		/* calc cuted fields */
  thd->cuted_fields=0L;
  /* Skip lines if there is a line terminator */
  if (ex->line.line_term->length() && ex->filetype != FILETYPE_XML)
  {
    /* ex->skip_lines needs to be preserved for logging */
    while (skip_lines > 0)
    {
      skip_lines--;
      if (read_info.next_line())
	break;
    }
  }

  if (!(error=MY_TEST(read_info.error)))
  {

    table->next_number_field=table->found_next_number_field;
    if (thd->lex->is_ignore() ||
	handle_duplicates == DUP_REPLACE)
      table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
    if (handle_duplicates == DUP_REPLACE &&
        (!table->triggers ||
         !table->triggers->has_delete_triggers()))
        table->file->extra(HA_EXTRA_WRITE_CAN_REPLACE);
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
      table->file->ha_start_bulk_insert((ha_rows) 0);
    table->copy_blobs=1;

    if (ex->filetype == FILETYPE_XML) /* load xml */
      error= read_xml_field(thd, info, insert_table_ref, fields_vars,
                            set_fields, set_values, read_info,
                            skip_lines);
    else if (!field_term->length() && !enclosed->length())
      error= read_fixed_length(thd, info, insert_table_ref, fields_vars,
                               set_fields, set_values, read_info,
			       skip_lines);
    else
      error= read_sep_field(thd, info, insert_table_ref, fields_vars,
                            set_fields, set_values, read_info,
			    *enclosed, skip_lines);
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES &&
        table->file->ha_end_bulk_insert() && !error)
    {
      table->file->print_error(my_errno(), MYF(0));
      error= 1;
    }
    table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
    table->file->extra(HA_EXTRA_WRITE_CANNOT_REPLACE);
    table->next_number_field=0;
  }
  if (file >= 0)
    mysql_file_close(file, MYF(0));
  free_blobs(table);				/* if pack_blob was used */
  table->copy_blobs=0;
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;
  /* 
     simulated killing in the middle of per-row loop
     must be effective for binlogging
  */
  DBUG_EXECUTE_IF("simulate_kill_bug27571",
                  {
                    error=1;
                    thd->killed= THD::KILL_QUERY;
                  };);

#ifndef EMBEDDED_LIBRARY
  killed_status= (error == 0) ? THD::NOT_KILLED : thd->killed;
#endif

  /*
    We must invalidate the table in query cache before binlog writing and
    ha_autocommit_...
  */
  query_cache.invalidate_single(thd, insert_table_ref, false);
  if (error)
  {
    if (read_file_from_client)
      read_info.skip_data_till_eof();

#ifndef EMBEDDED_LIBRARY
    if (mysql_bin_log.is_open())
    {
      {
	/*
	  Make sure last block (the one which caused the error) gets
	  logged.  This is needed because otherwise after write of (to
	  the binlog, not to read_info (which is a cache))
	  Delete_file_log_event the bad block will remain in read_info
	  (because pre_read is not called at the end of the last
	  block; remember pre_read is called whenever a new block is
	  read from disk).  At the end of mysql_load(), the destructor
	  of read_info will call end_io_cache() which will flush
	  read_info, so we will finally have this in the binlog:

	  Append_block # The last successfull block
	  Delete_file
	  Append_block # The failing block
	  which is nonsense.
	  Or could also be (for a small file)
	  Create_file  # The failing block
	  which is nonsense (Delete_file is not written in this case, because:
	  Create_file has not been written, so Delete_file is not written, then
	  when read_info is destroyed end_io_cache() is called which writes
	  Create_file.
	*/
	read_info.end_io_cache();
	/* If the file was not empty, wrote_create_file is true */
	if (lf_info.wrote_create_file)
	{
          int errcode= query_error_code(thd, killed_status == THD::NOT_KILLED);

          /* since there is already an error, the possible error of
             writing binary log will be ignored */
	  if (thd->get_transaction()->cannot_safely_rollback(
	      Transaction_ctx::STMT))
            (void) write_execute_load_query_log_event(thd, ex,
                                                      table_list->db, 
                                                      table_list->table_name,
                                                      is_concurrent,
                                                      handle_duplicates,
                                                      transactional_table,
                                                      errcode);
	  else
	  {
	    Delete_file_log_event d(thd, db, transactional_table);
	    (void) mysql_bin_log.write_event(&d);
	  }
	}
      }
    }
#endif /*!EMBEDDED_LIBRARY*/
    error= -1;				// Error on read
    goto err;
  }

  my_snprintf(name, sizeof(name),
              ER(ER_LOAD_INFO),
              (long) info.stats.records, (long) info.stats.deleted,
              (long) (info.stats.records - info.stats.copied),
              (long) thd->get_stmt_da()->current_statement_cond_count());

#ifndef EMBEDDED_LIBRARY
  if (mysql_bin_log.is_open())
  {
    /*
      We need to do the job that is normally done inside
      binlog_query() here, which is to ensure that the pending event
      is written before tables are unlocked and before any other
      events are written.  We also need to update the table map
      version for the binary log to mark that table maps are invalid
      after this point.
     */
    if (thd->is_current_stmt_binlog_format_row())
      error= thd->binlog_flush_pending_rows_event(TRUE, transactional_table);
    else
    {
      /*
        As already explained above, we need to call end_io_cache() or the last
        block will be logged only after Execute_load_query_log_event (which is
        wrong), when read_info is destroyed.
      */
      read_info.end_io_cache();
      if (lf_info.wrote_create_file)
      {
        int errcode= query_error_code(thd, killed_status == THD::NOT_KILLED);
        error= write_execute_load_query_log_event(thd, ex,
                                                  table_list->db, table_list->table_name,
                                                  is_concurrent,
                                                  handle_duplicates,
                                                  transactional_table,
                                                  errcode);
      }

      /*
        Flushing the IO CACHE while writing the execute load query log event
        may result in error (for instance, because the max_binlog_size has been 
        reached, and rotation of the binary log failed).
      */
      error= error || mysql_bin_log.get_log_file()->error;
    }
    if (error)
      goto err;
  }
#endif /*!EMBEDDED_LIBRARY*/

  /* ok to client sent only after binlog write and engine commit */
  my_ok(thd, info.stats.copied + info.stats.deleted, 0L, name);
err:
  assert(table->file->has_transactions() ||
         !(info.stats.copied || info.stats.deleted) ||
         thd->get_transaction()->cannot_safely_rollback(Transaction_ctx::STMT));
  table->file->ha_release_auto_increment();
  table->auto_increment_field_not_null= FALSE;
  DBUG_RETURN(error);
}


#ifndef EMBEDDED_LIBRARY

/* Not a very useful function; just to avoid duplication of code */
static bool write_execute_load_query_log_event(THD *thd, sql_exchange* ex,
                                               const char* db_arg,  /* table's database */
                                               const char* table_name_arg,
                                               bool is_concurrent,
                                               enum enum_duplicates duplicates,
                                               bool transactional_table,
                                               int errcode)
{
  char                *load_data_query,
                      *end,
                      *fname_start,
                      *fname_end,
                      *p= NULL;
  size_t               pl= 0;
  List<Item>           fv;
  Item                *item;
  String              *str;
  String               pfield, pfields;
  int                  n;
  const char          *tbl= table_name_arg;
  const char          *tdb= (thd->db().str != NULL ? thd->db().str : db_arg);
  String              string_buf;
  if (thd->db().str == NULL || strcmp(db_arg, thd->db().str))
  {
    /*
      If used database differs from table's database,
      prefix table name with database name so that it
      becomes a FQ name.
     */
    string_buf.set_charset(system_charset_info);
    append_identifier(thd, &string_buf, db_arg, strlen(db_arg));
    string_buf.append(".");
  }
  append_identifier(thd, &string_buf, table_name_arg,
                    strlen(table_name_arg));
  tbl= string_buf.c_ptr_safe();
  Load_log_event       lle(thd, ex, tdb, tbl, fv, is_concurrent,
                           duplicates, thd->lex->is_ignore(),
                           transactional_table);

  /*
    force in a LOCAL if there was one in the original.
  */
  if (thd->lex->local_file)
    lle.set_fname_outside_temp_buf(ex->file_name, strlen(ex->file_name));

  /*
    prepare fields-list and SET if needed; print_query won't do that for us.
  */
  if (!thd->lex->load_field_list.is_empty())
  {
    List_iterator<Item> li(thd->lex->load_field_list);

    pfields.append(" (");
    n= 0;

    while ((item= li++))
    {
      if (n++)
        pfields.append(", ");
      if (item->type() == Item::FIELD_ITEM ||
                 item->type() == Item::REF_ITEM)
        append_identifier(thd, &pfields, item->item_name.ptr(),
                          strlen(item->item_name.ptr()));
      else
        item->print(&pfields, QT_ORDINARY);
    }
    pfields.append(")");
  }

  if (!thd->lex->load_update_list.is_empty())
  {
    List_iterator<Item> lu(thd->lex->load_update_list);
    List_iterator<String> ls(thd->lex->load_set_str_list);

    pfields.append(" SET ");
    n= 0;

    while ((item= lu++))
    {
      str= ls++;
      if (n++)
        pfields.append(", ");
      append_identifier(thd, &pfields, item->item_name.ptr(),
                        strlen(item->item_name.ptr()));
      // Extract exact Item value
      str->copy();
      pfields.append(str->ptr());
      str->mem_free();
    }
    /*
      Clear the SET string list once the SET command is reconstructed
      as we donot require the list anymore.
    */
    thd->lex->load_set_str_list.empty();
  }

  p= pfields.c_ptr_safe();
  pl= strlen(p);

  if (!(load_data_query= (char *)thd->alloc(lle.get_query_buffer_length() + 1 + pl)))
    return TRUE;

  lle.print_query(FALSE, ex->cs ? ex->cs->csname : NULL,
                  load_data_query, &end,
                  &fname_start, &fname_end);

  strcpy(end, p);
  end += pl;

  Execute_load_query_log_event
    e(thd, load_data_query, end-load_data_query,
      static_cast<uint>(fname_start - load_data_query - 1),
      static_cast<uint>(fname_end - load_data_query),
      (duplicates == DUP_REPLACE) ? binary_log::LOAD_DUP_REPLACE :
      (thd->lex->is_ignore() ? binary_log::LOAD_DUP_IGNORE :
                               binary_log::LOAD_DUP_ERROR),
      transactional_table, FALSE, FALSE, errcode);
  return mysql_bin_log.write_event(&e);
}

#endif

/****************************************************************************
** Read of rows of fixed size + optional garbage + optional newline
****************************************************************************/

static int
read_fixed_length(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                  List<Item> &fields_vars, List<Item> &set_fields,
                  List<Item> &set_values, READ_INFO &read_info,
                  ulong skip_lines)
{
  List_iterator_fast<Item> it(fields_vars);
  TABLE *table= table_list->table;
  bool err;
  DBUG_ENTER("read_fixed_length");

  while (!read_info.read_fixed_length())
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }
    if (skip_lines)
    {
      /*
	We could implement this with a simple seek if:
	- We are not using DATA INFILE LOCAL
	- escape character is  ""
	- line starting prefix is ""
      */
      skip_lines--;
      continue;
    }
    it.rewind();
    uchar *pos=read_info.row_start;

    restore_record(table, s->default_values);
    /*
      Check whether default values of the fields not specified in column list
      are correct or not.
    */
    if (validate_default_values_of_unset_fields(thd, table))
    {
      read_info.error= true;
      break;
    }

    Item *item;
    while ((item= it++))
    {
      /*
        There is no variables in fields_vars list in this format so
        this conversion is safe (no need to check for STRING_ITEM).
      */
      assert(item->real_item()->type() == Item::FIELD_ITEM);
      Item_field *sql_field= static_cast<Item_field*>(item->real_item());
      Field *field= sql_field->field;                  
      if (field == table->next_number_field)
        table->auto_increment_field_not_null= TRUE;
      /*
        No fields specified in fields_vars list can be null in this format.
        Mark field as not null, we should do this for each row because of
        restore_record...
      */
      field->set_notnull();

      if (pos == read_info.row_end)
      {
        thd->cuted_fields++;			/* Not enough fields */
        push_warning_printf(thd, Sql_condition::SL_WARNING,
                            ER_WARN_TOO_FEW_RECORDS,
                            ER(ER_WARN_TOO_FEW_RECORDS),
                            thd->get_stmt_da()->current_row_for_condition());
        if (field->type() == FIELD_TYPE_TIMESTAMP && !field->maybe_null())
        {
          // Specific of TIMESTAMP NOT NULL: set to CURRENT_TIMESTAMP.
          Item_func_now_local::store_in(field);
        }
      }
      else
      {
	uint length;
	uchar save_chr;
	if ((length=(uint) (read_info.row_end-pos)) >
	    field->field_length)
	  length=field->field_length;
	save_chr=pos[length]; pos[length]='\0'; // Safeguard aganst malloc
        field->store((char*) pos,length,read_info.read_charset);
	pos[length]=save_chr;
	if ((pos+=length) > read_info.row_end)
	  pos= read_info.row_end;	/* Fills rest with space */
      }
    }
    if (pos != read_info.row_end)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_WARN_TOO_MANY_RECORDS,
                          ER(ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_condition());
    }

    if (thd->killed ||
        fill_record_n_invoke_before_triggers(thd, &info, set_fields,
                                             set_values, table,
                                             TRG_EVENT_INSERT,
                                             table->s->fields))
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }

    err= write_record(thd, table, &info, NULL);
    table->auto_increment_field_not_null= FALSE;
    if (err)
      DBUG_RETURN(1);
   
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    if (read_info.next_line())			// Skip to next line
      break;
    if (read_info.line_cuted)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_WARN_TOO_MANY_RECORDS,
                          ER(ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_condition());
    }
    thd->get_stmt_da()->inc_current_row_for_condition();
continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error));
}


class Field_tmp_nullability_guard
{
public:
  explicit Field_tmp_nullability_guard(Item *item)
   :m_field(NULL)
  {
    if (item->type() == Item::FIELD_ITEM)
    {
      m_field= ((Item_field *) item)->field;
      /*
        Enable temporary nullability for items that corresponds
        to table fields.
      */
      m_field->set_tmp_nullable();
    }
  }

  ~Field_tmp_nullability_guard()
  {
    if (m_field)
      m_field->reset_tmp_nullable();
  }

private:
  Field *m_field;
};


static int
read_sep_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
               List<Item> &fields_vars, List<Item> &set_fields,
               List<Item> &set_values, READ_INFO &read_info,
	       const String &enclosed, ulong skip_lines)
{
  List_iterator_fast<Item> it(fields_vars);
  Item *item;
  TABLE *table= table_list->table;
  size_t enclosed_length;
  bool err;
  DBUG_ENTER("read_sep_field");

  enclosed_length=enclosed.length();

  for (;;it.rewind())
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }

    restore_record(table, s->default_values);
    /*
      Check whether default values of the fields not specified in column list
      are correct or not.
    */
    if (validate_default_values_of_unset_fields(thd, table))
    {
      read_info.error= true;
      break;
    }

    while ((item= it++))
    {
      uint length;
      uchar *pos;
      Item *real_item;

      if (read_info.read_field())
	break;

      /* If this line is to be skipped we don't want to fill field or var */
      if (skip_lines)
        continue;

      pos=read_info.row_start;
      length=(uint) (read_info.row_end-pos);

      real_item= item->real_item();

      Field_tmp_nullability_guard fld_tmp_nullability_guard(real_item);

      if ((!read_info.enclosed &&
	  (enclosed_length && length == 4 &&
           !memcmp(pos, STRING_WITH_LEN("NULL")))) ||
	  (length == 1 && read_info.found_null))
      {

        if (real_item->type() == Item::FIELD_ITEM)
        {
          Field *field= ((Item_field *)real_item)->field;
          if (field->reset())                   // Set to 0
          {
            my_error(ER_WARN_NULL_TO_NOTNULL, MYF(0), field->field_name,
                     thd->get_stmt_da()->current_row_for_condition());
            DBUG_RETURN(1);
          }
          if (!field->real_maybe_null() &&
              field->type() == FIELD_TYPE_TIMESTAMP)
          {
            // Specific of TIMESTAMP NOT NULL: set to CURRENT_TIMESTAMP.
            Item_func_now_local::store_in(field);
          }
          else
          {
            /*
              Set field to NULL. Later we will clear temporary nullability flag
              and check NOT NULL constraint.
            */
            field->set_null();
          }
	}
        else if (item->type() == Item::STRING_ITEM)
        {
          assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
          ((Item_user_var_as_out_param *)item)->set_null_value(
                                                  read_info.read_charset);
        }

	continue;
      }

      if (real_item->type() == Item::FIELD_ITEM)
      {
        Field *field= ((Item_field *)real_item)->field;
        field->set_notnull();
        read_info.row_end[0]=0;			// Safe to change end marker
        if (field == table->next_number_field)
          table->auto_increment_field_not_null= TRUE;
        field->store((char*) pos, length, read_info.read_charset);
      }
      else if (item->type() == Item::STRING_ITEM)
      {
        assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
        ((Item_user_var_as_out_param *)item)->set_value((char*) pos, length,
                                                        read_info.read_charset);
      }
    }

    if (thd->is_error())
      read_info.error= true;

    if (read_info.error)
      break;
    if (skip_lines)
    {
      skip_lines--;
      continue;
    }
    if (item)
    {
      /* Have not read any field, thus input file is simply ended */
      if (item == fields_vars.head())
	break;
      for (; item ; item= it++)
      {
        Item *real_item= item->real_item();
        if (real_item->type() == Item::FIELD_ITEM)
        {
          Field *field= ((Item_field *)real_item)->field;
          /*
            We set to 0. But if the field is DEFAULT NULL, the "null bit"
            turned on by restore_record() above remains so field will be NULL.
          */
          if (field->reset())
          {
            my_error(ER_WARN_NULL_TO_NOTNULL, MYF(0),field->field_name,
                     thd->get_stmt_da()->current_row_for_condition());
            DBUG_RETURN(1);
          }
          if (field->type() == FIELD_TYPE_TIMESTAMP && !field->maybe_null())
            // Specific of TIMESTAMP NOT NULL: set to CURRENT_TIMESTAMP.
            Item_func_now_local::store_in(field);
          /*
            QQ: We probably should not throw warning for each field.
            But how about intention to always have the same number
            of warnings in THD::cuted_fields (and get rid of cuted_fields
            in the end ?)
          */
          thd->cuted_fields++;
          push_warning_printf(thd, Sql_condition::SL_WARNING,
                              ER_WARN_TOO_FEW_RECORDS,
                              ER(ER_WARN_TOO_FEW_RECORDS),
                              thd->get_stmt_da()->current_row_for_condition());
        }
        else if (item->type() == Item::STRING_ITEM)
        {
          assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
          ((Item_user_var_as_out_param *)item)->set_null_value(
                                                  read_info.read_charset);
        }
      }
    }

    if (thd->killed ||
        fill_record_n_invoke_before_triggers(thd, &info, set_fields,
                                             set_values, table,
                                             TRG_EVENT_INSERT,
                                             table->s->fields))
      DBUG_RETURN(1);

    if (!table->triggers)
    {
      /*
        If there is no trigger for the table then check the NOT NULL constraint
        for every table field.

        For the table that has BEFORE-INSERT trigger installed checking for
        NOT NULL constraint is done inside function
        fill_record_n_invoke_before_triggers() after all trigger instructions
        has been executed.
      */
      it.rewind();

      while ((item= it++))
      {
        Item *real_item= item->real_item();
        if (real_item->type() == Item::FIELD_ITEM)
          ((Item_field *) real_item)->field->check_constraints(ER_WARN_NULL_TO_NOTNULL);
      }
    }

    if (thd->is_error())
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }

    err= write_record(thd, table, &info, NULL);
    table->auto_increment_field_not_null= FALSE;
    if (err)
      DBUG_RETURN(1);
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    if (read_info.next_line())			// Skip to next line
      break;
    if (read_info.line_cuted)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_WARN_TOO_MANY_RECORDS, ER(ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_condition());
      if (thd->killed)
        DBUG_RETURN(1);
    }
    thd->get_stmt_da()->inc_current_row_for_condition();
continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error));
}


/****************************************************************************
** Read rows in xml format
****************************************************************************/
static int
read_xml_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
               List<Item> &fields_vars, List<Item> &set_fields,
               List<Item> &set_values, READ_INFO &read_info,
               ulong skip_lines)
{
  List_iterator_fast<Item> it(fields_vars);
  Item *item;
  TABLE *table= table_list->table;
  const CHARSET_INFO *cs= read_info.read_charset;
  DBUG_ENTER("read_xml_field");

  for ( ; ; it.rewind())
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }
    
    // read row tag and save values into tag list
    if (read_info.read_xml())
      break;
    
    List_iterator_fast<XML_TAG> xmlit(read_info.taglist);
    xmlit.rewind();
    XML_TAG *tag= NULL;
    
#ifndef NDEBUG
    DBUG_PRINT("read_xml_field", ("skip_lines=%d", (int) skip_lines));
    while ((tag= xmlit++))
    {
      DBUG_PRINT("read_xml_field", ("got tag:%i '%s' '%s'",
                                    tag->level, tag->field.c_ptr(),
                                    tag->value.c_ptr()));
    }
#endif
    
    restore_record(table, s->default_values);
    /*
      Check whether default values of the fields not specified in column list
      are correct or not.
    */
    if (validate_default_values_of_unset_fields(thd, table))
    {
      read_info.error= true;
      break;
    }
    
    while ((item= it++))
    {
      /* If this line is to be skipped we don't want to fill field or var */
      if (skip_lines)
        continue;
      
      /* find field in tag list */
      xmlit.rewind();
      tag= xmlit++;
      
      while(tag && strcmp(tag->field.c_ptr(), item->item_name.ptr()) != 0)
        tag= xmlit++;
      
      item= item->real_item();

      if (!tag) // found null
      {
        if (item->type() == Item::FIELD_ITEM)
        {
          Field *field= (static_cast<Item_field*>(item))->field;
          field->reset();
          field->set_null();
          if (field == table->next_number_field)
            table->auto_increment_field_not_null= TRUE;
          if (!field->maybe_null())
          {
            if (field->type() == FIELD_TYPE_TIMESTAMP)
              // Specific of TIMESTAMP NOT NULL: set to CURRENT_TIMESTAMP.
              Item_func_now_local::store_in(field);
            else if (field != table->next_number_field)
              field->set_warning(Sql_condition::SL_WARNING,
                                 ER_WARN_NULL_TO_NOTNULL, 1);
          }
        }
        else
        {
          assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
          ((Item_user_var_as_out_param *) item)->set_null_value(cs);
        }
        continue;
      }

      if (item->type() == Item::FIELD_ITEM)
      {
        Field *field= ((Item_field *)item)->field;
        field->set_notnull();
        if (field == table->next_number_field)
          table->auto_increment_field_not_null= TRUE;
        field->store((char *) tag->value.ptr(), tag->value.length(), cs);
      }
      else
      {
        assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
        ((Item_user_var_as_out_param *) item)->set_value(
                                                 (char *) tag->value.ptr(), 
                                                 tag->value.length(), cs);
      }
    }
    
    if (read_info.error)
      break;
    
    if (skip_lines)
    {
      skip_lines--;
      continue;
    }
    
    if (item)
    {
      /* Have not read any field, thus input file is simply ended */
      if (item == fields_vars.head())
        break;
      
      for ( ; item; item= it++)
      {
        if (item->type() == Item::FIELD_ITEM)
        {
          /*
            QQ: We probably should not throw warning for each field.
            But how about intention to always have the same number
            of warnings in THD::cuted_fields (and get rid of cuted_fields
            in the end ?)
          */
          thd->cuted_fields++;
          push_warning_printf(thd, Sql_condition::SL_WARNING,
                              ER_WARN_TOO_FEW_RECORDS,
                              ER(ER_WARN_TOO_FEW_RECORDS),
                              thd->get_stmt_da()->current_row_for_condition());
        }
        else
        {
          assert(NULL != dynamic_cast<Item_user_var_as_out_param*>(item));
          ((Item_user_var_as_out_param *)item)->set_null_value(cs);
        }
      }
    }

    if (thd->killed ||
        fill_record_n_invoke_before_triggers(thd, &info, set_fields,
                                             set_values, table,
                                             TRG_EVENT_INSERT,
                                             table->s->fields))
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }
    
    if (write_record(thd, table, &info, NULL))
      DBUG_RETURN(1);
    
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    thd->get_stmt_da()->inc_current_row_for_condition();
    continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error) || thd->is_error());
} /* load xml end */


/* Unescape all escape characters, mark \N as null */

char
READ_INFO::unescape(char chr)
{
  /* keep this switch synchornous with the ESCAPE_CHARS macro */
  switch(chr) {
  case 'n': return '\n';
  case 't': return '\t';
  case 'r': return '\r';
  case 'b': return '\b';
  case '0': return 0;				// Ascii null
  case 'Z': return '\032';			// Win32 end of file
  case 'N': found_null=1;

    /* fall through */
  default:  return chr;
  }
}


/*
  Read a line using buffering
  If last line is empty (in line mode) then it isn't outputed
*/


READ_INFO::READ_INFO(File file_par, uint tot_length, const CHARSET_INFO *cs,
                     const String &field_term,
                     const String &line_start,
                     const String &line_term,
                     const String &enclosed_par,
                     int escape, bool get_it_from_net, bool is_fifo)
  :file(file_par), buff_length(tot_length), escape_char(escape),
   found_end_of_line(false), eof(false), need_end_io_cache(false),
   error(false), line_cuted(false), found_null(false), read_charset(cs)
{
  /*
    Field and line terminators must be interpreted as sequence of unsigned char.
    Otherwise, non-ascii terminators will be negative on some platforms,
    and positive on others (depending on the implementation of char).
  */
  field_term_ptr=
    static_cast<const uchar*>(static_cast<const void*>(field_term.ptr()));
  field_term_length= field_term.length();
  line_term_ptr=
    static_cast<const uchar*>(static_cast<const void*>(line_term.ptr()));
  line_term_length= line_term.length();

  level= 0; /* for load xml */
  if (line_start.length() == 0)
  {
    line_start_ptr=0;
    start_of_line= 0;
  }
  else
  {
    line_start_ptr= line_start.ptr();
    line_start_end=line_start_ptr+line_start.length();
    start_of_line= 1;
  }
  /* If field_terminator == line_terminator, don't use line_terminator */
  if (field_term_length == line_term_length &&
      !memcmp(field_term_ptr,line_term_ptr,field_term_length))
  {
    line_term_length=0;
    line_term_ptr= NULL;
  }
  enclosed_char= (enclosed_length=enclosed_par.length()) ?
    (uchar) enclosed_par[0] : INT_MAX;
  field_term_char= field_term_length ? field_term_ptr[0] : INT_MAX;
  line_term_char= line_term_length ? line_term_ptr[0] : INT_MAX;


  /* Set of a stack for unget if long terminators */
  size_t length= max<size_t>(cs->mbmaxlen, max(field_term_length, line_term_length)) + 1;
  set_if_bigger(length,line_start.length());
  stack=stack_pos=(int*) sql_alloc(sizeof(int)*length);

  if (!(buffer=(uchar*) my_malloc(key_memory_READ_INFO,
                                  buff_length+1, MYF(MY_WME))))
    error= true; /* purecov: inspected */
  else
  {
    end_of_buff=buffer+buff_length;
    if (init_io_cache(&cache,(get_it_from_net) ? -1 : file, 0,
		      (get_it_from_net) ? READ_NET :
		      (is_fifo ? READ_FIFO : READ_CACHE),0L,1,
		      MYF(MY_WME)))
    {
      my_free(buffer); /* purecov: inspected */
      buffer= NULL;
      error= true;
    }
    else
    {
      /*
	init_io_cache() will not initialize read_function member
	if the cache is READ_NET. So we work around the problem with a
	manual assignment
      */
      need_end_io_cache = 1;

#ifndef EMBEDDED_LIBRARY
      if (get_it_from_net)
	cache.read_function = _my_b_net_read;

      if (mysql_bin_log.is_open())
	cache.pre_read = cache.pre_close =
	  (IO_CACHE_CALLBACK) log_loaded_block;
#endif
    }
  }
}


READ_INFO::~READ_INFO()
{
  if (need_end_io_cache)
    ::end_io_cache(&cache);

  if (buffer != NULL)
    my_free(buffer);
  List_iterator<XML_TAG> xmlit(taglist);
  XML_TAG *t;
  while ((t= xmlit++))
    delete(t);
}


/**
  The logic here is similar with my_mbcharlen, except for GET and PUSH

  @param[in]  cs  charset info
  @param[in]  chr the first char of sequence
  @param[out] len the length of multi-byte char
*/
#define GET_MBCHARLEN(cs, chr, len)                                           \
  do {                                                                        \
    len= my_mbcharlen((cs), (chr));                                           \
    if (len == 0 && my_mbmaxlenlen((cs)) == 2)                                \
    {                                                                         \
      int chr1= GET;                                                          \
      if (chr1 != my_b_EOF)                                                   \
      {                                                                       \
        len= my_mbcharlen_2((cs), (chr), chr1);                               \
        /* Character is gb18030 or invalid (len = 0) */                       \
        assert(len == 0 || len == 2 || len == 4);                       \
      }                                                                       \
      if (len != 0)                                                           \
        PUSH(chr1);                                                           \
    }                                                                         \
  } while (0)


inline int READ_INFO::terminator(const uchar *ptr, size_t length)
{
  int chr=0;					// Keep gcc happy
  size_t i;
  for (i=1 ; i < length ; i++)
  {
    chr= GET;
    if (chr != *++ptr)
    {
      break;
    }
  }
  if (i == length)
    return 1;
  PUSH(chr);
  while (i-- > 1)
    PUSH(*--ptr);
  return 0;
}


int READ_INFO::read_field()
{
  int chr,found_enclosed_char;
  uchar *to,*new_buffer;

  found_null=0;
  if (found_end_of_line)
    return 1;					// One have to call next_line

  /* Skip until we find 'line_start' */

  if (start_of_line)
  {						// Skip until line_start
    start_of_line=0;
    if (find_start_of_fields())
      return 1;
  }
  if ((chr=GET) == my_b_EOF)
  {
    found_end_of_line=eof=1;
    return 1;
  }
  to=buffer;
  if (chr == enclosed_char)
  {
    found_enclosed_char=enclosed_char;
    *to++=(uchar) chr;				// If error
  }
  else
  {
    found_enclosed_char= INT_MAX;
    PUSH(chr);
  }

  for (;;)
  {
    bool escaped_mb= false;
    while ( to < end_of_buff)
    {
      chr = GET;
      if (chr == my_b_EOF)
	goto found_eof;
      if (chr == escape_char)
      {
	if ((chr=GET) == my_b_EOF)
	{
	  *to++= (uchar) escape_char;
	  goto found_eof;
	}
        /*
          When escape_char == enclosed_char, we treat it like we do for
          handling quotes in SQL parsing -- you can double-up the
          escape_char to include it literally, but it doesn't do escapes
          like \n. This allows: LOAD DATA ... ENCLOSED BY '"' ESCAPED BY '"'
          with data like: "fie""ld1", "field2"
         */
        if (escape_char != enclosed_char || chr == escape_char)
        {
          uint ml;
          GET_MBCHARLEN(read_charset, chr, ml);
          /*
            For escaped multibyte character, push back the first byte,
            and will handle it below.
            Because multibyte character's second byte is possible to be
            0x5C, per Query_result_export::send_data, both head byte and
            tail byte are escaped for such characters. So mark it if the
            head byte is escaped and will handle it below.
          */
          if (ml == 1)
            *to++= (uchar) unescape((char) chr);
          else
          {
            escaped_mb= true;
            PUSH(chr);
          }
          continue;
        }
        PUSH(chr);
        chr= escape_char;
      }
      if (chr == line_term_char && found_enclosed_char == INT_MAX)
      {
	if (terminator(line_term_ptr,line_term_length))
	{					// Maybe unexpected linefeed
	  enclosed=0;
	  found_end_of_line=1;
	  row_start=buffer;
	  row_end=  to;
	  return 0;
	}
      }
      if (chr == found_enclosed_char)
      {
	if ((chr=GET) == found_enclosed_char)
	{					// Remove dupplicated
	  *to++ = (uchar) chr;
	  continue;
	}
	// End of enclosed field if followed by field_term or line_term
	if (chr == my_b_EOF ||
	    (chr == line_term_char && terminator(line_term_ptr,
						line_term_length)))
	{					// Maybe unexpected linefeed
	  enclosed=1;
	  found_end_of_line=1;
	  row_start=buffer+1;
	  row_end=  to;
	  return 0;
	}
	if (chr == field_term_char &&
	    terminator(field_term_ptr,field_term_length))
	{
	  enclosed=1;
	  row_start=buffer+1;
	  row_end=  to;
	  return 0;
	}
	/*
	  The string didn't terminate yet.
	  Store back next character for the loop
	*/
	PUSH(chr);
	/* copy the found term character to 'to' */
	chr= found_enclosed_char;
      }
      else if (chr == field_term_char && found_enclosed_char == INT_MAX)
      {
	if (terminator(field_term_ptr,field_term_length))
	{
	  enclosed=0;
	  row_start=buffer;
	  row_end=  to;
	  return 0;
	}
      }

      uint ml;
      GET_MBCHARLEN(read_charset, chr, ml);
      if (ml == 0)
      {
        *to= '\0';
        my_error(ER_INVALID_CHARACTER_STRING, MYF(0),
                 read_charset->csname, buffer);
        error= true;
        return 1;
      }


      if (ml > 1 &&
          to + ml <= end_of_buff)
      {
        uchar* p= to;
        *to++ = chr;

        for (uint i= 1; i < ml; i++)
        {
          chr= GET;
          if (chr == my_b_EOF)
          {
            /*
             Need to back up the bytes already ready from illformed
             multi-byte char 
            */
            to-= i;
            goto found_eof;
          }
          else if (chr == escape_char && escaped_mb)
          {
            // Unescape the second byte if it is escaped.
            chr= GET;
            chr= (uchar) unescape((char) chr);
          }
          *to++ = chr;
        }
        if (escaped_mb)
          escaped_mb= false;
        if (my_ismbchar(read_charset,
                        (const char *)p,
                        (const char *)to))
          continue;
        for (uint i= 0; i < ml; i++)
          PUSH(*--to);
        chr= GET;
      }
      else if (ml > 1)
      {
        // Buffer is too small, exit while loop, and reallocate.
        PUSH(chr);
        break;
      }
      *to++ = (uchar) chr;
    }
    /*
    ** We come here if buffer is too small. Enlarge it and continue
    */
    if (!(new_buffer=(uchar*) my_realloc(key_memory_READ_INFO,
                                         (char*) buffer,buff_length+1+IO_SIZE,
					MYF(MY_WME))))
      return (error= true);
    to=new_buffer + (to-buffer);
    buffer=new_buffer;
    buff_length+=IO_SIZE;
    end_of_buff=buffer+buff_length;
  }

found_eof:
  enclosed=0;
  found_end_of_line=eof=1;
  row_start=buffer;
  row_end=to;
  return 0;
}

/*
  Read a row with fixed length.

  NOTES
    The row may not be fixed size on disk if there are escape
    characters in the file.

  IMPLEMENTATION NOTE
    One can't use fixed length with multi-byte charset **

  RETURN
    0  ok
    1  error
*/

int READ_INFO::read_fixed_length()
{
  int chr;
  uchar *to;
  if (found_end_of_line)
    return 1;					// One have to call next_line

  if (start_of_line)
  {						// Skip until line_start
    start_of_line=0;
    if (find_start_of_fields())
      return 1;
  }

  to=row_start=buffer;
  while (to < end_of_buff)
  {
    if ((chr=GET) == my_b_EOF)
      goto found_eof;
    if (chr == escape_char)
    {
      if ((chr=GET) == my_b_EOF)
      {
	*to++= (uchar) escape_char;
	goto found_eof;
      }
      *to++ =(uchar) unescape((char) chr);
      continue;
    }
    if (chr == line_term_char)
    {
      if (terminator(line_term_ptr,line_term_length))
      {						// Maybe unexpected linefeed
	found_end_of_line=1;
	row_end=  to;
	return 0;
      }
    }
    *to++ = (uchar) chr;
  }
  row_end=to;					// Found full line
  return 0;

found_eof:
  found_end_of_line=eof=1;
  row_start=buffer;
  row_end=to;
  return to == buffer ? 1 : 0;
}


int READ_INFO::next_line()
{
  line_cuted=0;
  start_of_line= line_start_ptr != 0;
  if (found_end_of_line || eof)
  {
    found_end_of_line=0;
    return eof;
  }
  found_end_of_line=0;
  if (!line_term_length)
    return 0;					// No lines
  for (;;)
  {
    int chr = GET;
    uint ml;
    if (chr == my_b_EOF)
    {
      eof= 1;
      return 1;
    }
   GET_MBCHARLEN(read_charset, chr, ml);
   if (ml > 1)
   {
       for (uint i=1;
            chr != my_b_EOF && i < ml;
            i++)
	   chr = GET;
       if (chr == escape_char)
	   continue;
   }
   if (chr == my_b_EOF)
   {
      eof=1;
      return 1;
    }
    if (chr == escape_char)
    {
      line_cuted=1;
      if (GET == my_b_EOF)
	return 1;
      continue;
    }
    if (chr == line_term_char && terminator(line_term_ptr,line_term_length))
      return 0;
    line_cuted=1;
  }
}


bool READ_INFO::find_start_of_fields()
{
  int chr;
 try_again:
  do
  {
    if ((chr=GET) == my_b_EOF)
    {
      found_end_of_line=eof=1;
      return 1;
    }
  } while ((char) chr != line_start_ptr[0]);
  for (const char *ptr=line_start_ptr+1 ; ptr != line_start_end ; ptr++)
  {
    chr=GET;					// Eof will be checked later
    if ((char) chr != *ptr)
    {						// Can't be line_start
      PUSH(chr);
      while (--ptr != line_start_ptr)
      {						// Restart with next char
	PUSH( *ptr);
      }
      goto try_again;
    }
  }
  return 0;
}


/*
  Clear taglist from tags with a specified level
*/
int READ_INFO::clear_level(int level_arg)
{
  DBUG_ENTER("READ_INFO::read_xml clear_level");
  List_iterator<XML_TAG> xmlit(taglist);
  xmlit.rewind();
  XML_TAG *tag;
  
  while ((tag= xmlit++))
  {
     if(tag->level >= level_arg)
     {
       xmlit.remove();
       delete tag;
     }
  }
  DBUG_RETURN(0);
}


/*
  Convert an XML entity to Unicode value.
  Return -1 on error;
*/
static int
my_xml_entity_to_char(const char *name, size_t length)
{
  if (length == 2)
  {
    if (!memcmp(name, "gt", length))
      return '>';
    if (!memcmp(name, "lt", length))
      return '<';
  }
  else if (length == 3)
  {
    if (!memcmp(name, "amp", length))
      return '&';
  }
  else if (length == 4)
  {
    if (!memcmp(name, "quot", length))
      return '"';
    if (!memcmp(name, "apos", length))
      return '\'';
  }
  return -1;
}


/**
  @brief Convert newline, linefeed, tab to space
  
  @param chr    character
  
  @details According to the "XML 1.0" standard,
           only space (#x20) characters, carriage returns,
           line feeds or tabs are considered as spaces.
           Convert all of them to space (#x20) for parsing simplicity.
*/
static int
my_tospace(int chr)
{
  return (chr == '\t' || chr == '\r' || chr == '\n') ? ' ' : chr;
}


/*
  Read an xml value: handle multibyte and xml escape
*/
int READ_INFO::read_value(int delim, String *val)
{
  int chr;
  String tmp;

  for (chr= GET; my_tospace(chr) != delim && chr != my_b_EOF;)
  {
    uint ml;
    GET_MBCHARLEN(read_charset, chr, ml);
    if (ml == 0)
    {
      chr= my_b_EOF;
      val->length(0);
      return chr;
    }

    if (ml > 1)
    {
      DBUG_PRINT("read_xml",("multi byte"));

      for (uint i= 1; i < ml; i++)
      {
        val->append(chr);
        /*
          Don't use my_tospace() in the middle of a multi-byte character
          TODO: check that the multi-byte sequence is valid.
        */
        chr= GET; 
        if (chr == my_b_EOF)
          return chr;
      }
    }
    if(chr == '&')
    {
      tmp.length(0);
      for (chr= my_tospace(GET) ; chr != ';' ; chr= my_tospace(GET))
      {
        if (chr == my_b_EOF)
          return chr;
        tmp.append(chr);
      }
      if ((chr= my_xml_entity_to_char(tmp.ptr(), tmp.length())) >= 0)
        val->append(chr);
      else
      {
        val->append('&');
        val->append(tmp);
        val->append(';'); 
      }
    }
    else
      val->append(chr);
    chr= GET;
  }            
  return my_tospace(chr);
}


/*
  Read a record in xml format
  tags and attributes are stored in taglist
  when tag set in ROWS IDENTIFIED BY is closed, we are ready and return
*/
int READ_INFO::read_xml()
{
  DBUG_ENTER("READ_INFO::read_xml");
  int chr, chr2, chr3;
  int delim= 0;
  String tag, attribute, value;
  bool in_tag= false;
  
  tag.length(0);
  attribute.length(0);
  value.length(0);
  
  for (chr= my_tospace(GET); chr != my_b_EOF ; )
  {
    switch(chr){
    case '<':  /* read tag */
        /* TODO: check if this is a comment <!-- comment -->  */
      chr= my_tospace(GET);
      if(chr == '!')
      {
        chr2= GET;
        chr3= GET;
        
        if(chr2 == '-' && chr3 == '-')
        {
          chr2= 0;
          chr3= 0;
          chr= my_tospace(GET);
          
          while(chr != '>' || chr2 != '-' || chr3 != '-')
          {
            if(chr == '-')
            {
              chr3= chr2;
              chr2= chr;
            }
            else if (chr2 == '-')
            {
              chr2= 0;
              chr3= 0;
            }
            chr= my_tospace(GET);
            if (chr == my_b_EOF)
              goto found_eof;
          }
          break;
        }
      }
      
      tag.length(0);
      while(chr != '>' && chr != ' ' && chr != '/' && chr != my_b_EOF)
      {
        if(chr != delim) /* fix for the '<field name =' format */
          tag.append(chr);
        chr= my_tospace(GET);
      }
      
      // row tag should be in ROWS IDENTIFIED BY '<row>' - stored in line_term 
      if((tag.length() == line_term_length -2) &&
         (memcmp(tag.ptr(), line_term_ptr + 1, tag.length()) == 0))
      {
        DBUG_PRINT("read_xml", ("start-of-row: %i %s %s", 
                                level,tag.c_ptr_safe(), line_term_ptr));
      }
      
      if(chr == ' ' || chr == '>')
      {
        level++;
        clear_level(level + 1);
      }
      
      if (chr == ' ')
        in_tag= true;
      else 
        in_tag= false;
      break;
      
    case ' ': /* read attribute */
      while(chr == ' ')  /* skip blanks */
        chr= my_tospace(GET);
      
      if(!in_tag)
        break;
      
      while(chr != '=' && chr != '/' && chr != '>' && chr != my_b_EOF)
      {
        attribute.append(chr);
        chr= my_tospace(GET);
      }
      break;
      
    case '>': /* end tag - read tag value */
      in_tag= false;
      /* Skip all whitespaces */
      while (' ' == (chr= my_tospace(GET)))
      {
      }
      /*
        Push the first non-whitespace char back to Stack. This char would be
        read in the upcoming call to read_value()
       */
      PUSH(chr);
      chr= read_value('<', &value);
      if(chr == my_b_EOF)
        goto found_eof;
      
      /* save value to list */
      if(tag.length() > 0 && value.length() > 0)
      {
        DBUG_PRINT("read_xml", ("lev:%i tag:%s val:%s",
                                level,tag.c_ptr_safe(), value.c_ptr_safe()));
        taglist.push_front( new XML_TAG(level, tag, value));
      }
      tag.length(0);
      value.length(0);
      attribute.length(0);
      break;
      
    case '/': /* close tag */
      chr= my_tospace(GET);
      /* Decrease the 'level' only when (i) It's not an */
      /* (without space) empty tag i.e. <tag/> or, (ii) */
      /* It is of format <row col="val" .../>           */
      if(chr != '>' || in_tag)
      {
        level--;
        in_tag= false;
      }
      if(chr != '>')   /* if this is an empty tag <tag   /> */
        tag.length(0); /* we should keep tag value          */
      while(chr != '>' && chr != my_b_EOF)
      {
        tag.append(chr);
        chr= my_tospace(GET);
      }
      
      if((tag.length() == line_term_length -2) &&
         (memcmp(tag.ptr(), line_term_ptr + 1, tag.length()) == 0))
      {
         DBUG_PRINT("read_xml", ("found end-of-row %i %s", 
                                 level, tag.c_ptr_safe()));
         DBUG_RETURN(0); //normal return
      }
      chr= my_tospace(GET);
      break;   
      
    case '=': /* attribute name end - read the value */
      //check for tag field and attribute name
      if(!memcmp(tag.c_ptr_safe(), STRING_WITH_LEN("field")) &&
         !memcmp(attribute.c_ptr_safe(), STRING_WITH_LEN("name")))
      {
        /*
          this is format <field name="xx">xx</field>
          where actual fieldname is in attribute
        */
        delim= my_tospace(GET);
        tag.length(0);
        attribute.length(0);
        chr= '<'; /* we pretend that it is a tag */
        level--;
        break;
      }
      
      //check for " or '
      chr= GET;
      if (chr == my_b_EOF)
        goto found_eof;
      if(chr == '"' || chr == '\'')
      {
        delim= chr;
      }
      else
      {
        delim= ' '; /* no delimiter, use space */
        PUSH(chr);
      }
      
      chr= read_value(delim, &value);
      if(attribute.length() > 0 && value.length() > 0)
      {
        DBUG_PRINT("read_xml", ("lev:%i att:%s val:%s\n",
                                level + 1,
                                attribute.c_ptr_safe(),
                                value.c_ptr_safe()));
        taglist.push_front(new XML_TAG(level + 1, attribute, value));
      }
      attribute.length(0);
      value.length(0);
      if (chr != ' ')
        chr= my_tospace(GET);
      break;
    
    default:
      chr= my_tospace(GET);
    } /* end switch */
  } /* end while */
  
found_eof:
  DBUG_PRINT("read_xml",("Found eof"));
  eof= 1;
  DBUG_RETURN(1);
}
