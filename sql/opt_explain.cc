/* Copyright (c) 2011, 2021, Oracle and/or its affiliates.

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
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/** @file "EXPLAIN <command>" implementation */ 

#include "opt_explain.h"
#include "sql_select.h"
#include "sql_optimizer.h" // JOIN
#include "sql_partition.h" // for make_used_partitions_str()
#include "sql_join_buffer.h" // JOIN_CACHE
#include "filesort.h"        // Filesort
#include "opt_explain_format.h"
#include "sql_base.h"      // lock_tables
#include "sql_acl.h"       // check_global_access, PROCESS_ACL
#include "debug_sync.h"    // DEBUG_SYNC
#include "opt_trace.h"     // Opt_trace_*
#include "sql_parse.h"     // is_explainable_query
#include "mysqld_thd_manager.h"  // Global_THD_manager

typedef qep_row::extra extra;

static bool mysql_explain_unit(THD *thd, SELECT_LEX_UNIT *unit);

const char *join_type_str[]={ "UNKNOWN","system","const","eq_ref","ref",
			      "ALL","range","index","fulltext",
			      "ref_or_null","unique_subquery","index_subquery",
                              "index_merge"
};

static const enum_query_type cond_print_flags=
  enum_query_type(QT_ORDINARY | QT_SHOW_SELECT_NUMBER);

static const char plan_not_ready[]= "Plan isn't ready yet";

/**
  A base for all Explain_* classes

  Explain_* classes collect and output EXPLAIN data.

  This class hierarchy is a successor of the old select_describe() function of 5.5.
*/

class Explain
{
protected:
  THD *const thd; ///< cached THD which runs the EXPLAIN command
  const CHARSET_INFO *const cs; ///< cached pointer to system_charset_info
  /**
     Cached SELECT_LEX of the explained query. Used for all explained stmts,
     including single-table UPDATE (provides way to access ORDER BY of
     UPDATE).
  */
  SELECT_LEX *const select_lex;

  Explain_format *const fmt; ///< shortcut for thd->lex->explain_format
  enum_parsing_context context_type; ///< associated value for struct. explain

  bool order_list; ///< if query block has ORDER BY

  const bool explain_other; ///< if we explain other thread than us

protected:
  class Lazy_condition: public Lazy
  {
    Item *const condition;
  public:
    Lazy_condition(Item *condition_arg): condition(condition_arg) {}
    virtual bool eval(String *ret)
    {
      ret->length(0);
      if (condition)
        condition->print(ret, cond_print_flags);
      return false;
    }
  };

  explicit Explain(enum_parsing_context context_type_arg,
                   THD *thd_arg, SELECT_LEX *select_lex_arg)
  : thd(thd_arg),
    cs(system_charset_info),
    select_lex(select_lex_arg),
    fmt(thd->lex->explain_format),
    context_type(context_type_arg),
    order_list(false),
    explain_other(thd_arg != select_lex_arg->master_unit()->thd)
  {
    if (explain_other)
      select_lex_arg->master_unit()
        ->thd->query_plan.assert_plan_is_locked_if_other();
  }

public:
  virtual ~Explain() {}

  bool send();

  /**
     Tells if it is allowed to print the WHERE / GROUP BY / etc
     clauses.
  */
  bool can_print_clauses() const
  {
    /*
      Certain implementations of Item::print() modify the item, so cannot be
      called by another thread which does not own the item. Moreover, the
      owning thread may be modifying the item at this moment (example:
      Item_in_subselect::finalize_materialization_transform() is done
      at first execution of the subquery, which happens after the parent query
      has a plan, and affects how the parent query would be printed).
    */
    return !explain_other;
  }

protected:
  /**
    Explain everything but subqueries
  */
  virtual bool shallow_explain();
  /**
    Explain the rest of things after the @c shallow_explain() call
  */
  bool explain_subqueries();
  bool mark_subqueries(Item *item, qep_row *destination);
  bool prepare_columns();

  /**
    Push a part of the "extra" column into formatter

    Traditional formatter outputs traditional_extra_tags[tag] as is.
    Hierarchical formatter outputs a property with the json_extra_tags[tag] name
    and a boolean value of true.

    @param      tag     type of the "extra" part

    @retval     false   Ok
    @retval     true    Error (OOM)
  */
  bool push_extra(Extra_tag tag)
  {
    extra *e= new extra(tag);
    return e == NULL || fmt->entry()->col_extra.push_back(e);
  }

  /**
    Push a part of the "extra" column into formatter

    @param      tag     type of the "extra" part
    @param      arg     for traditional formatter: rest of the part text,
                        for hierarchical format: string value of the property

    @retval     false   Ok
    @retval     true    Error (OOM)
  */
  bool push_extra(Extra_tag tag, const String &arg)
  {
    if (arg.is_empty())
      return push_extra(tag);
    extra *e= new extra(tag, arg.dup(thd->mem_root));
    return !e || !e->data || fmt->entry()->col_extra.push_back(e);
  }

  /**
    Push a part of the "extra" column into formatter

    @param      tag     type of the "extra" part
    @param      arg     for traditional formatter: rest of the part text,
                        for hierarchical format: string value of the property

    NOTE: arg must be a long-living string constant.

    @retval     false   Ok
    @retval     true    Error (OOM)
  */
  bool push_extra(Extra_tag tag, const char *arg)
  {
    extra *e= new extra(tag, arg);
    return !e || fmt->entry()->col_extra.push_back(e);
  }

  /*
    Rest of the functions are overloadable functions, those calculate and fill
    "col_*" fields with Items for further sending as EXPLAIN columns.

    "explain_*" functions return false on success and true on error (usually OOM).
  */
  virtual bool explain_id();
  virtual bool explain_select_type();
  virtual bool explain_table_name() { return false; }
  virtual bool explain_partitions() { return false; }
  virtual bool explain_join_type() { return false; }
  virtual bool explain_possible_keys() { return false; }
  /** fill col_key and and col_key_len fields together */
  virtual bool explain_key_and_len() { return false; }
  virtual bool explain_ref() { return false; }
  /** fill col_rows and col_filtered fields together */
  virtual bool explain_rows_and_filtered() { return false; }
  virtual bool explain_extra() { return false; }
  virtual bool explain_modify_flags() { return false; }

protected:
  /**
     Returns true if the WHERE, ORDER BY, GROUP BY, etc clauses can safely be
     traversed: it means that we can iterate through them (no element is
     added/removed/replaced); the internal details of an element can change
     though (in particular if that element is an Item_subselect).

     By default, if we are explaining another connection, this is not safe.
  */
  virtual bool can_walk_clauses() { return !explain_other; }
  virtual enum_parsing_context
  get_subquery_context(SELECT_LEX_UNIT *unit) const;
};

enum_parsing_context Explain::get_subquery_context(SELECT_LEX_UNIT *unit) const
{
  return unit->get_explain_marker();
}

/**
  Explain_no_table class outputs a trivial EXPLAIN row with "extra" column

  This class is intended for simple cases to produce EXPLAIN output
  with "No tables used", "No matching records" etc.
  Optionally it can output number of estimated rows in the "row"
  column.

  @note This class also produces EXPLAIN rows for inner units (if any).
*/

class Explain_no_table: public Explain
{
private:
  const char *message; ///< cached "message" argument
  const ha_rows rows; ///< HA_POS_ERROR or cached "rows" argument

public:
  Explain_no_table(THD *thd_arg, SELECT_LEX *select_lex_arg,
                   const char *message_arg,
                   enum_parsing_context context_type_arg= CTX_JOIN,
                   ha_rows rows_arg= HA_POS_ERROR)
  : Explain(context_type_arg, thd_arg, select_lex_arg),
    message(message_arg), rows(rows_arg)
  {
    if (can_walk_clauses())
      order_list= MY_TEST(select_lex_arg->order_list.elements);
  }

protected:
  virtual bool shallow_explain();

  virtual bool explain_rows_and_filtered();
  virtual bool explain_extra();
  virtual bool explain_modify_flags();
private:
  enum_parsing_context get_subquery_context(SELECT_LEX_UNIT *unit) const;
};


/**
  Explain_union_result class outputs EXPLAIN row for UNION
*/

class Explain_union_result : public Explain
{
public:
  Explain_union_result(THD *thd_arg, SELECT_LEX *select_lex_arg)
  : Explain(CTX_UNION_RESULT, thd_arg, select_lex_arg)
  {
    /* it's a UNION: */
    assert(select_lex_arg ==
           select_lex_arg->master_unit()->fake_select_lex);
    // Use optimized values from fake_select_lex's join
    order_list= MY_TEST(select_lex_arg->join->order);
    // A plan exists so the reads above are safe:
    assert(select_lex_arg->join->get_plan_state() != JOIN::NO_PLAN);
  }

protected:
  virtual bool explain_id();
  virtual bool explain_table_name();
  virtual bool explain_join_type();
  virtual bool explain_extra();
  /* purecov: begin deadcode */
  virtual bool can_walk_clauses()
  {
    assert(0);   // UNION result can't have conditions
    return true;                        // Because we know that we have a plan
  }
  /* purecov: end */
};



/**
  Common base class for Explain_join and Explain_table
*/

class Explain_table_base : public Explain {
protected:
  const TABLE *table;
  /**
     The QEP_TAB which we are currently explaining. It is NULL for the
     inserted table in INSERT/REPLACE SELECT.
     @note that you should never read quick() or condition(), they may change
     under your feet without holding the mutex ; read quick_optim() and
     condition_optim() instead.
  */
  QEP_TAB *tab;
  key_map usable_keys;

  Explain_table_base(enum_parsing_context context_type_arg,
                     THD *const thd_arg, SELECT_LEX *select_lex= NULL,
                     TABLE *const table_arg= NULL)
    : Explain(context_type_arg, thd_arg, select_lex), table(table_arg),
      tab(NULL)
  {}

  virtual bool explain_partitions();
  virtual bool explain_possible_keys();

  bool explain_key_parts(int key, uint key_parts);
  bool explain_key_and_len_quick(QUICK_SELECT_I *quick);
  bool explain_key_and_len_index(int key);
  bool explain_key_and_len_index(int key, uint key_length, uint key_parts);
  bool explain_extra_common(int quick_type,
                            uint keyno);
  bool explain_tmptable_and_filesort(bool need_tmp_table_arg,
                                     bool need_sort_arg);
};


/**
  Explain_join class produces EXPLAIN output for JOINs
*/

class Explain_join : public Explain_table_base
{
private:
  bool need_tmp_table; ///< add "Using temporary" to "extra" if true
  bool need_order; ///< add "Using filesort"" to "extra" if true
  const bool distinct; ///< add "Distinct" string to "extra" column if true

  JOIN *join; ///< current JOIN
  int quick_type; ///< current quick type, see anon. enum at QUICK_SELECT_I
  table_map used_tables; ///< accumulate used tables bitmap

public:
  Explain_join(THD *thd_arg, SELECT_LEX *select_lex_arg,
               bool need_tmp_table_arg, bool need_order_arg,
               bool distinct_arg)
  : Explain_table_base(CTX_JOIN, thd_arg, select_lex_arg),
    need_tmp_table(need_tmp_table_arg),
    need_order(need_order_arg), distinct(distinct_arg),
    join(select_lex_arg->join), used_tables(0)
  {
    assert(select_lex->join->thd == select_lex->master_unit()->thd);
    assert(join->get_plan_state() == JOIN::PLAN_READY);
    /* it is not UNION: */
    assert(join->select_lex != join->unit->fake_select_lex);
    order_list= MY_TEST(join->order);
  }

private:
  // Next 4 functions begin and end context for GROUP BY, ORDER BY and DISTINC
  bool begin_sort_context(Explain_sort_clause clause, enum_parsing_context ctx);
  bool end_sort_context(Explain_sort_clause clause, enum_parsing_context ctx);
  bool begin_simple_sort_context(Explain_sort_clause clause,
                                 enum_parsing_context ctx);
  bool end_simple_sort_context(Explain_sort_clause clause,
                               enum_parsing_context ctx);
  bool explain_qep_tab(size_t tab_num);

protected:
  virtual bool shallow_explain();

  virtual bool explain_table_name();
  virtual bool explain_join_type();
  virtual bool explain_key_and_len();
  virtual bool explain_ref();
  virtual bool explain_rows_and_filtered();
  virtual bool explain_extra();
  virtual bool explain_select_type();
  virtual bool explain_id();
  virtual bool explain_modify_flags();
  virtual bool can_walk_clauses()
  {
    return true;                        // Because we know that we have a plan
  }
};


/**
  Explain_table class produce EXPLAIN output for queries without top-level JOIN

  This class is a simplified version of the Explain_join class. It works in the
  context of queries which implementation lacks top-level JOIN object (EXPLAIN
  single-table UPDATE and DELETE).
*/

class Explain_table: public Explain_table_base
{
private:
  const uint       key;        ///< cached "key" number argument
  const ha_rows    limit;      ///< HA_POS_ERROR or cached "limit" argument
  const bool       need_tmp_table; ///< cached need_tmp_table argument
  const bool       need_sort;  ///< cached need_sort argument
  const enum_mod_type mod_type; ///< Table modification type
  const bool       used_key_is_modified; ///< UPDATE command updates used key
  const char *message; ///< cached "message" argument

public:
  Explain_table(THD *const thd_arg, SELECT_LEX *select_lex_arg,
                TABLE *const table_arg,
                QEP_TAB *tab_arg,
                uint key_arg, ha_rows limit_arg,
                bool need_tmp_table_arg, bool need_sort_arg,
                enum_mod_type mod_type_arg, bool used_key_is_modified_arg,
                const char *msg)
  : Explain_table_base(CTX_JOIN, thd_arg, select_lex_arg, table_arg),
    key(key_arg),
    limit(limit_arg),
    need_tmp_table(need_tmp_table_arg), need_sort(need_sort_arg),
    mod_type(mod_type_arg), used_key_is_modified(used_key_is_modified_arg),
    message(msg)
  {
    tab= tab_arg;
    usable_keys= table->possible_quick_keys;
    if (can_walk_clauses())
      order_list= MY_TEST(select_lex_arg->order_list.elements);
  }

  virtual bool explain_modify_flags();

private:
  virtual bool explain_tmptable_and_filesort(bool need_tmp_table_arg,
                                             bool need_sort_arg);
  virtual bool shallow_explain();

  virtual bool explain_ref();
  virtual bool explain_table_name();
  virtual bool explain_join_type();
  virtual bool explain_key_and_len();
  virtual bool explain_rows_and_filtered();
  virtual bool explain_extra();

  virtual bool can_walk_clauses()
  {
    return true;                        // Because we know that we have a plan
  }
};


/* Explain class functions ****************************************************/


bool Explain::shallow_explain()
{
  return prepare_columns() || fmt->flush_entry();
}


/**
  Qualify subqueries with WHERE/HAVING/ORDER BY/GROUP BY clause type marker

  @param item           Item tree to find subqueries
  @param destination    For WHERE clauses

  @note WHERE clause belongs to TABLE or QEP_TAB. The @c destination parameter
        provides a pointer to QEP data for such a table to associate a future
        subquery EXPLAIN output with table QEP provided.

  @retval false         OK
  @retval true          Error
*/

bool Explain::mark_subqueries(Item *item, qep_row *destination)
{
  if (item == NULL || !fmt->is_hierarchical())
    return false;

  item->compile(&Item::explain_subquery_checker,
                reinterpret_cast<uchar **>(&destination),
                &Item::explain_subquery_propagator,
                NULL);
  return false;
}

static bool explain_ref_key(Explain_format *fmt,
                            uint key_parts, store_key *key_copy[])
{
  if (key_parts == 0)
    return false;

  for (uint part_no= 0; part_no < key_parts; part_no++)
  {
    const store_key *const s_key= key_copy[part_no];
    if (s_key == NULL)
    {
      // Const keys don't need to be copied
      if (fmt->entry()->col_ref.push_back(store_key_const_item::static_name))
        return true; /* purecov: inspected */
    }
    else if (fmt->entry()->col_ref.push_back(s_key->name()))
      return true; /* purecov: inspected */
  }
  return false;
}


enum_parsing_context
Explain_no_table::get_subquery_context(SELECT_LEX_UNIT *unit) const
{
  const enum_parsing_context context= Explain::get_subquery_context(unit);
  if (context == CTX_OPTIMIZED_AWAY_SUBQUERY)
    return context;
  if (context == CTX_DERIVED)
    return context;
  else if (message != plan_not_ready)
    /*
      When zero result is given all subqueries are considered as optimized
      away.
    */
    return CTX_OPTIMIZED_AWAY_SUBQUERY;
  return context;
}


/**
  Traverses SQL clauses of this query specification to identify children
  subqueries, marks each of them with the clause they belong to.
  Then goes though all children subqueries and produces their EXPLAIN
  output, attached to the proper clause's context.

  @param        result  result stream

  @retval       false   Ok
  @retval       true    Error (OOM)
*/
bool Explain::explain_subqueries()
{
  for (SELECT_LEX_UNIT *unit= select_lex->first_inner_unit();
       unit;
       unit= unit->next_unit())
  {
    assert(explain_other || unit->is_optimized());
    SELECT_LEX *sl= unit->first_select();
    enum_parsing_context context= get_subquery_context(unit);
    if (context == CTX_NONE)
      context= CTX_OPTIMIZED_AWAY_SUBQUERY;

    if (fmt->begin_context(context, unit))
      return true;

    if (mysql_explain_unit(thd, unit))
      return true;

    /*
      This must be after mysql_explain_unit() so that JOIN::optimize() has run
      and had a chance to choose materialization.
    */
    if (fmt->is_hierarchical() &&
        (context == CTX_WHERE || context == CTX_HAVING ||
         context == CTX_SELECT_LIST ||
         context == CTX_GROUP_BY_SQ || context == CTX_ORDER_BY_SQ) &&
        (!explain_other ||
         (sl->join && sl->join->get_plan_state() != JOIN::NO_PLAN)) &&
        // Check below requires complete plan
        unit->item &&
        (unit->item->get_engine_for_explain()->engine_type() ==
         subselect_engine::HASH_SJ_ENGINE))
    {
      fmt->entry()->is_materialized_from_subquery= true;
      fmt->entry()->col_table_name.set_const("<materialized_subquery>");
      fmt->entry()->using_temporary= true;
      fmt->entry()->col_join_type.set_const(join_type_str[JT_EQ_REF]);
      fmt->entry()->col_key.set_const("<auto_key>");

      const subselect_hash_sj_engine * const engine=
        static_cast<const subselect_hash_sj_engine *>
        (unit->item->get_engine_for_explain());
      const QEP_TAB * const tmp_tab= engine->get_qep_tab();

      char buff_key_len[24];
      fmt->entry()->col_key_len.set(buff_key_len,
                                    longlong2str(tmp_tab->table()->key_info[0].key_length,
                                                 buff_key_len, 10) - buff_key_len);

      if (explain_ref_key(fmt, tmp_tab->ref().key_parts,
                          tmp_tab->ref().key_copy))
        return true;

      fmt->entry()->col_rows.set(1);
      /*
       The value to look up depends on the outer value, so the materialized
       subquery is dependent and not cacheable:
      */
      fmt->entry()->is_dependent= true;
      fmt->entry()->is_cacheable= false;
    }

    if (fmt->end_context(context))
      return true;
  }
  return false;
}


/**
  Pre-calculate table property values for further EXPLAIN output
*/
bool Explain::prepare_columns()
{
  return explain_id() ||
    explain_select_type() ||
    explain_table_name() ||
    explain_partitions() ||
    explain_join_type() ||
    explain_possible_keys() ||
    explain_key_and_len() ||
    explain_ref() ||
    explain_modify_flags() ||
    explain_rows_and_filtered() ||
    explain_extra();
}


/**
  Explain class main function

  This function:
    a) allocates a Query_result_send object (if no one pre-allocated available),
    b) calculates and sends whole EXPLAIN data.

  @return false if success, true if error
*/

bool Explain::send()
{
  DBUG_ENTER("Explain::send");

  if (fmt->begin_context(context_type, NULL))
    DBUG_RETURN(true);

  /* Don't log this into the slow query log */
  thd->server_status&= ~(SERVER_QUERY_NO_INDEX_USED |
                         SERVER_QUERY_NO_GOOD_INDEX_USED);

  bool ret= shallow_explain() || explain_subqueries();

  if (!ret)
    ret= fmt->end_context(context_type);

  DBUG_RETURN(ret);
}


bool Explain::explain_id()
{
  if (select_lex->select_number < INT_MAX)
    fmt->entry()->col_id.set(select_lex->select_number);
  return false;
}


bool Explain::explain_select_type()
{
  // ignore top-level SELECT_LEXes
  // Elaborate only when plan is ready
  if (select_lex->master_unit()->outer_select() &&
      select_lex->join &&
      select_lex->join->get_plan_state() != JOIN::NO_PLAN)
  {
    fmt->entry()->is_dependent= select_lex->is_dependent();
    if (select_lex->type() != SELECT_LEX::SLT_DERIVED)
      fmt->entry()->is_cacheable= select_lex->is_cacheable();
  }
  fmt->entry()->col_select_type.set(select_lex->type());
  return false;
}


/* Explain_no_table class functions *******************************************/


bool Explain_no_table::shallow_explain()
{
  return (fmt->begin_context(CTX_MESSAGE) ||
          Explain::shallow_explain() ||
          (can_walk_clauses() &&
           mark_subqueries(select_lex->where_cond(), fmt->entry())) ||
          fmt->end_context(CTX_MESSAGE));
}


bool Explain_no_table::explain_rows_and_filtered()
{
  /* Don't print estimated # of rows in table for INSERT/REPLACE. */
  if (rows == HA_POS_ERROR || fmt->entry()->mod_type == MT_INSERT ||
      fmt->entry()->mod_type == MT_REPLACE)
    return false;
  fmt->entry()->col_rows.set(rows);
  return false;
}


bool Explain_no_table::explain_extra()
{
  return fmt->entry()->col_message.set(message);
}


bool Explain_no_table::explain_modify_flags()
{
  THD *const query_thd= select_lex->master_unit()->thd;
  switch (query_thd->query_plan.get_command()) {
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_UPDATE:
    fmt->entry()->mod_type= MT_UPDATE;
    break;
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_DELETE:
    fmt->entry()->mod_type= MT_DELETE;
    break;
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_INSERT:
    fmt->entry()->mod_type= MT_INSERT;
    break;
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_REPLACE:
    fmt->entry()->mod_type= MT_REPLACE;
    break;
  default: ;
  }
  return false;
}


/* Explain_union_result class functions ****************************************/


bool Explain_union_result::explain_id()
{
  return false;
}


bool Explain_union_result::explain_table_name()
{
  // Get the last of UNION's selects
  SELECT_LEX *last_select=
    select_lex->master_unit()->first_select()->last_select();
  // # characters needed to print select_number of last select
  int last_length= (int)log10((double)last_select->select_number)+1;

  SELECT_LEX *sl= select_lex->master_unit()->first_select();
  size_t len= 6, lastop= 0;
  char table_name_buffer[NAME_LEN];
  memcpy(table_name_buffer, STRING_WITH_LEN("<union"));
  /*
    - len + lastop: current position in table_name_buffer
    - 6 + last_length: the number of characters needed to print
      '...,'<last_select->select_number>'>\0'
  */
  for (;
       sl && len + lastop + 6 + last_length < NAME_CHAR_LEN;
       sl= sl->next_select())
  {
    len+= lastop;
    lastop= my_snprintf(table_name_buffer + len, NAME_CHAR_LEN - len,
                        "%u,", sl->select_number);
  }
  if (sl || len + lastop >= NAME_CHAR_LEN)
  {
    memcpy(table_name_buffer + len, STRING_WITH_LEN("...,"));
    len+= 4;
    lastop= my_snprintf(table_name_buffer + len, NAME_CHAR_LEN - len,
                        "%u,", last_select->select_number);
  }
  len+= lastop;
  table_name_buffer[len - 1]= '>';  // change ',' to '>'

  return fmt->entry()->col_table_name.set(table_name_buffer, len);
}


bool Explain_union_result::explain_join_type()
{
  fmt->entry()->col_join_type.set_const(join_type_str[JT_ALL]);
  return false;
}


bool Explain_union_result::explain_extra()
{
  if (!fmt->is_hierarchical())
  {
    /*
     Currently we always use temporary table for UNION result
    */
    if (push_extra(ET_USING_TEMPORARY))
      return true;
    /*
      here we assume that the query will return at least two rows, so we
      show "filesort" in EXPLAIN. Of course, sometimes we'll be wrong
      and no filesort will be actually done, but executing all selects in
      the UNION to provide precise EXPLAIN information will hardly be
      appreciated :)
    */
    if (order_list)
    {
      return push_extra(ET_USING_FILESORT);
    }
  }
  return Explain::explain_extra();
}


/* Explain_table_base class functions *****************************************/


bool Explain_table_base::explain_partitions()
{
  if (table->part_info)
    return make_used_partitions_str(table->part_info,
                                    &fmt->entry()->col_partitions);
  return false;
}


bool Explain_table_base::explain_possible_keys()
{
  if (usable_keys.is_clear_all())
    return false;

  for (uint j= 0 ; j < table->s->keys ; j++)
  {
    if (usable_keys.is_set(j) &&
        fmt->entry()->col_possible_keys.push_back(table->key_info[j].name))
      return true;
  }
  return false;
}


bool Explain_table_base::explain_key_parts(int key, uint key_parts)
{
  KEY_PART_INFO *kp= table->key_info[key].key_part;
  for (uint i= 0; i < key_parts; i++, kp++)
    if (fmt->entry()->col_key_parts.push_back(kp->field->field_name))
      return true;
  return false;
}


bool Explain_table_base::explain_key_and_len_quick(QUICK_SELECT_I *quick)
{
  bool ret= false;
  StringBuffer<512> str_key(cs);
  StringBuffer<512> str_key_len(cs);

  if (quick->index != MAX_KEY)
    ret= explain_key_parts(quick->index,
                           quick->used_key_parts);
  quick->add_keys_and_lengths(&str_key, &str_key_len);
  return (ret || fmt->entry()->col_key.set(str_key) ||
          fmt->entry()->col_key_len.set(str_key_len));
}


bool Explain_table_base::explain_key_and_len_index(int key)
{
  assert(key != MAX_KEY);
  return explain_key_and_len_index(key, table->key_info[key].key_length,
                                   table->key_info[key].user_defined_key_parts);
}


bool Explain_table_base::explain_key_and_len_index(int key, uint key_length,
                                                   uint key_parts)
{
  assert(key != MAX_KEY);

  char buff_key_len[24];
  const KEY *key_info= table->key_info + key;
  const size_t length= longlong2str(key_length, buff_key_len, 10) - buff_key_len;
  const bool ret= explain_key_parts(key, key_parts);
  return (ret || fmt->entry()->col_key.set(key_info->name) ||
          fmt->entry()->col_key_len.set(buff_key_len, length));
}


bool Explain_table_base::explain_extra_common(int quick_type,
                                              uint keyno)
{
  if (((keyno != MAX_KEY &&
        keyno == table->file->pushed_idx_cond_keyno &&
        table->file->pushed_idx_cond) ||
       (tab && tab->cache_idx_cond)))
  {
    StringBuffer<160> buff(cs);
    if (fmt->is_hierarchical() && can_print_clauses())
    {
      if (table->file->pushed_idx_cond)
        table->file->pushed_idx_cond->print(&buff, cond_print_flags);
      else
        tab->cache_idx_cond->print(&buff, cond_print_flags);
    }
    if (push_extra(ET_USING_INDEX_CONDITION, buff))
      return true; /* purecov: inspected */
  }

  const TABLE* pushed_root= table->file->root_of_pushed_join();
  if (pushed_root && select_lex->join &&
      select_lex->join->get_plan_state() == JOIN::PLAN_READY)
  {
    char buf[128];
    size_t len;
    int pushed_id= 0;
    for (QEP_TAB* prev= select_lex->join->qep_tab; prev <= tab; prev++)
    {
      const TABLE* prev_root= prev->table()->file->root_of_pushed_join();
      if (prev_root == prev->table())
      {
        pushed_id++;
        if (prev_root == pushed_root)
          break;
      }
    }
    if (pushed_root == table)
    {
      uint pushed_count= tab->table()->file->number_of_pushed_joins();
      len= my_snprintf(buf, sizeof(buf)-1,
                       "Parent of %d pushed join@%d",
                       pushed_count, pushed_id);
    }
    else
    {
      len= my_snprintf(buf, sizeof(buf)-1,
                       "Child of '%s' in pushed join@%d",
                       tab->table()->file->parent_of_pushed_join()->alias,
                       pushed_id);
    }

    {
      StringBuffer<128> buff(cs);
      buff.append(buf,len);
      if (push_extra(ET_PUSHED_JOIN, buff))
        return true;
    }
  }

  switch (quick_type) {
  case QUICK_SELECT_I::QS_TYPE_ROR_UNION:
  case QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT:
  case QUICK_SELECT_I::QS_TYPE_INDEX_MERGE:
    {
      StringBuffer<32> buff(cs);
      tab->quick_optim()->add_info_string(&buff);
      if (fmt->is_hierarchical())
      {
        /*
          We are replacing existing col_key value with a quickselect info,
          but not the reverse:
        */
        assert(fmt->entry()->col_key.length);
        if (fmt->entry()->col_key.set(buff)) // keep col_key_len intact
          return true;
      }
      else
      {
        if (push_extra(ET_USING, buff))
          return true;
      }
    }
    break;
  default: ;
  }

  if (tab)
  {
    if (tab->dynamic_range())
    {
      StringBuffer<64> str(STRING_WITH_LEN("index map: 0x"), cs);
      /* 4 bits per 1 hex digit + terminating '\0' */
      char buf[MAX_KEY / 4 + 1];
      str.append(tab->keys().print(buf));
      if (push_extra(ET_RANGE_CHECKED_FOR_EACH_RECORD, str))
        return true;
    }
    else if (tab->condition_optim())
    {
      const Item *pushed_cond= table->file->pushed_cond;

      if (thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN) &&
          pushed_cond)
      {
        StringBuffer<64> buff(cs);
        if (can_print_clauses())
          ((Item *)pushed_cond)->print(&buff, cond_print_flags);
        //TIANMU UPGRADE BEGIN
        #if defined(TIANMU)
        table->file->explain_message(tab->condition_optim(), &buff);
        #endif
        //END
        if (push_extra(ET_USING_WHERE_WITH_PUSHED_CONDITION, buff))
          return true;
      }
      else
      {
        if (fmt->is_hierarchical() && can_print_clauses())
        {
          Lazy_condition *c= new Lazy_condition(tab->condition_optim());
          if (c == NULL)
            return true;
          fmt->entry()->col_attached_condition.set(c);
        }
        else if (push_extra(ET_USING_WHERE))
          return true;
      }
    }
  }
  if (table->reginfo.not_exists_optimize && push_extra(ET_NOT_EXISTS))
    return true;

  if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE)
  {
    uint mrr_flags=
      ((QUICK_RANGE_SELECT*)(tab->quick_optim()))->mrr_flags;

    /*
      During normal execution of a query, multi_range_read_init() is
      called to initialize MRR. If HA_MRR_SORTED is set at this point,
      multi_range_read_init() for any native MRR implementation will
      revert to default MRR if not HA_MRR_SUPPORT_SORTED.
      Calling multi_range_read_init() can potentially be costly, so it
      is not done when executing an EXPLAIN. We therefore simulate
      its effect here:
    */
    if (mrr_flags & HA_MRR_SORTED && !(mrr_flags & HA_MRR_SUPPORT_SORTED))
      mrr_flags|= HA_MRR_USE_DEFAULT_IMPL;

    if (!(mrr_flags & HA_MRR_USE_DEFAULT_IMPL) && push_extra(ET_USING_MRR))
      return true;
  }

  if (tab && tab->type() == JT_FT &&
      (table->file->ha_table_flags() & HA_CAN_FULLTEXT_HINTS))
  {
    /*
      Print info about FT hints.
    */
    StringBuffer<64> buff(cs);
    Ft_hints *ft_hints= tab->ft_func()->get_hints();
    bool not_first= false;
    if (ft_hints->get_flags() & FT_SORTED)
    {
      buff.append("sorted");
      not_first= true;
    }
    else if (ft_hints->get_flags() & FT_NO_RANKING)
    {
      buff.append("no_ranking");
      not_first= true;
    }
    if (ft_hints->get_op_type() != FT_OP_UNDEFINED &&
        ft_hints->get_op_type() != FT_OP_NO)
    {
      char buf[64];
      size_t len= 0;

      if (not_first)
        buff.append(", ");
      switch (ft_hints->get_op_type())
      {
        case FT_OP_GT:
          len= my_snprintf(buf, sizeof(buf) - 1,
                           "rank > %g", ft_hints->get_op_value());
          break;
        case FT_OP_GE:
          len= my_snprintf(buf, sizeof(buf) - 1,
                           "rank >= %g", ft_hints->get_op_value());
          break;
        default:
          assert(0);
      }

      buff.append(buf, len, cs);
      not_first= true;
    }
    
    if (ft_hints->get_limit() != HA_POS_ERROR)
    {
      char buf[64];
      size_t len= 0;

      if (not_first)
        buff.append(", ");

      len= my_snprintf(buf, sizeof(buf) - 1,
                       "limit = %d", ft_hints->get_limit());
      buff.append(buf, len, cs);
      not_first= true;
    }
    if (not_first)
      push_extra(ET_FT_HINTS, buff);

  }

  return false;
}

bool Explain_table_base::explain_tmptable_and_filesort(bool need_tmp_table_arg,
                                                       bool need_sort_arg)
{
  /*
    For hierarchical EXPLAIN we output "Using temporary" and
    "Using filesort" with related ORDER BY, GROUP BY or DISTINCT
  */
  if (fmt->is_hierarchical())
    return false; 

  if (need_tmp_table_arg && push_extra(ET_USING_TEMPORARY))
    return true;
  if (need_sort_arg && push_extra(ET_USING_FILESORT))
    return true;
  return false;
}


bool Explain_join::explain_modify_flags()
{
  THD::Query_plan const *query_plan= &table->in_use->query_plan;
  /*
    Because we are PLAN_READY, the following data structures are not changing
    and thus are safe to read.
  */
  switch (query_plan->get_command()) {
  case SQLCOM_UPDATE_MULTI:
    if (!bitmap_is_clear_all(&table->def_write_set) &&
        table->s->table_category != TABLE_CATEGORY_TEMPORARY)
      fmt->entry()->mod_type= MT_UPDATE;
    break;
  case SQLCOM_DELETE_MULTI:
    for (TABLE_LIST *at= query_plan->get_lex()->auxiliary_table_list.first;
         at;
         at= at->next_local)
    {
      if (at->correspondent_table->is_updatable() &&
          at->correspondent_table->updatable_base_table()->table == table)
      {
        fmt->entry()->mod_type= MT_DELETE;
        break;
      }
    }
    break;
  case SQLCOM_INSERT_SELECT:
    if (table == query_plan->get_lex()->insert_table_leaf->table)
      fmt->entry()->mod_type= MT_INSERT;
    break;
  case SQLCOM_REPLACE_SELECT:
    if (table == query_plan->get_lex()->insert_table_leaf->table)
      fmt->entry()->mod_type= MT_REPLACE;
    break;
  default: ;
  };
  return false;
}


/* Explain_join class functions ***********************************************/

bool Explain_join::begin_sort_context(Explain_sort_clause clause,
                                      enum_parsing_context ctx)
{
  const Explain_format_flags *flags= &join->explain_flags;
  return (flags->get(clause, ESP_EXISTS) &&
          !flags->get(clause, ESP_IS_SIMPLE) &&
          fmt->begin_context(ctx, NULL, flags));
}


bool Explain_join::end_sort_context(Explain_sort_clause clause,
                                    enum_parsing_context ctx)
{
  const Explain_format_flags *flags= &join->explain_flags;
  return (flags->get(clause, ESP_EXISTS) &&
          !flags->get(clause, ESP_IS_SIMPLE) &&
          fmt->end_context(ctx));
}


bool Explain_join::begin_simple_sort_context(Explain_sort_clause clause,
                                             enum_parsing_context ctx)
{
  const Explain_format_flags *flags= &join->explain_flags;
  return (flags->get(clause, ESP_IS_SIMPLE) &&
          fmt->begin_context(ctx, NULL, flags));
}


bool Explain_join::end_simple_sort_context(Explain_sort_clause clause,
                                           enum_parsing_context ctx)
{
  const Explain_format_flags *flags= &join->explain_flags;
  return (flags->get(clause, ESP_IS_SIMPLE) &&
          fmt->end_context(ctx));
}


bool Explain_join::shallow_explain()
{
  qep_row *join_entry= fmt->entry();

  join_entry->col_read_cost.set(join->best_read);

  LEX const*query_lex= join->thd->query_plan.get_lex();
  if (query_lex->insert_table_leaf &&
      query_lex->insert_table_leaf->select_lex == join->select_lex)
  {
    table= query_lex->insert_table_leaf->table;
    /*
      The target table for INSERT/REPLACE doesn't actually belong to join,
      thus tab is set to NULL. But in order to print it we add it to the
      list of plan rows. Explain printing code (traditional/json) will deal with
      it.
    */
    tab= NULL;
    if (fmt->begin_context(CTX_QEP_TAB) ||
        prepare_columns() ||
        fmt->flush_entry() ||
        fmt->end_context(CTX_QEP_TAB))
      return true; /* purecov: inspected */
  }

  if (begin_sort_context(ESC_ORDER_BY, CTX_ORDER_BY))
    return true; /* purecov: inspected */
  if (begin_sort_context(ESC_DISTINCT, CTX_DISTINCT))
    return true; /* purecov: inspected */
  if (begin_sort_context(ESC_GROUP_BY, CTX_GROUP_BY))
    return true; /* purecov: inspected */

  if (join->sort_cost > 0.0)
  {
    /*
      Due to begin_sort_context() calls above, fmt->entry() returns another
      context than stored in join_entry.
    */
    assert(fmt->entry() != join_entry || !fmt->is_hierarchical());
    fmt->entry()->col_read_cost.set(join->sort_cost);
  }

  if (begin_sort_context(ESC_BUFFER_RESULT, CTX_BUFFER_RESULT))
    return true; /* purecov: inspected */

  for (size_t t= 0,
       cnt= fmt->is_hierarchical() ? join->primary_tables : join->tables;
       t < cnt; t++)
  {
    if (explain_qep_tab(t))
      return true;
  }

  if (end_sort_context(ESC_BUFFER_RESULT, CTX_BUFFER_RESULT))
    return true;
  if (end_sort_context(ESC_GROUP_BY, CTX_GROUP_BY))
    return true;
  if (end_sort_context(ESC_DISTINCT, CTX_DISTINCT))
    return true;
  if (end_sort_context(ESC_ORDER_BY, CTX_ORDER_BY))
    return true;

  return false;
}


bool Explain_join::explain_qep_tab(size_t tabnum)
{
  tab= join->qep_tab + tabnum;
  if (!tab->position())
    return false;
  table= tab->table();
  usable_keys= tab->keys();
  quick_type= -1;

  if (tab->type() == JT_RANGE || tab->type() == JT_INDEX_MERGE)
  {
    assert(tab->quick_optim());
    quick_type= tab->quick_optim()->get_type();
  }

  if (tab->starts_weedout())
    fmt->begin_context(CTX_DUPLICATES_WEEDOUT);

  const bool first_non_const= tabnum == join->const_tables;
  
  if (first_non_const)
  {
    if (begin_simple_sort_context(ESC_ORDER_BY, CTX_SIMPLE_ORDER_BY))
      return true;
    if (begin_simple_sort_context(ESC_DISTINCT, CTX_SIMPLE_DISTINCT))
      return true;
    if (begin_simple_sort_context(ESC_GROUP_BY, CTX_SIMPLE_GROUP_BY))
      return true;
  }

  Semijoin_mat_exec *const sjm= tab->sj_mat_exec();
  const enum_parsing_context c= sjm ? CTX_MATERIALIZATION : CTX_QEP_TAB;

  if (fmt->begin_context(c) || prepare_columns())
    return true;

  fmt->entry()->query_block_id= table->pos_in_table_list->query_block_id();

  if (sjm)
  {
    if (sjm->is_scan)
    {
      fmt->entry()->col_rows.cleanup(); // TODO: set(something reasonable)
    }
    else
    {
      fmt->entry()->col_rows.set(1);
    }
  }

  if (fmt->flush_entry() ||
      (can_walk_clauses() &&
       mark_subqueries(tab->condition_optim(), fmt->entry())))
    return true;

  if (sjm && fmt->is_hierarchical())
  {
    for (size_t sjt= sjm->inner_table_index, end= sjt + sjm->table_count;
         sjt < end; sjt++)
    {
      if (explain_qep_tab(sjt))
        return true;
    }
  }

  if (fmt->end_context(c))
    return true;

  if (first_non_const)
  {
    if (end_simple_sort_context(ESC_GROUP_BY, CTX_SIMPLE_GROUP_BY))
      return true;
    if (end_simple_sort_context(ESC_DISTINCT, CTX_SIMPLE_DISTINCT))
      return true;
    if (end_simple_sort_context(ESC_ORDER_BY, CTX_SIMPLE_ORDER_BY))
      return true;
  }

  if (tab->finishes_weedout() &&
      fmt->end_context(CTX_DUPLICATES_WEEDOUT))
    return true;

  used_tables|= tab->table_ref->map();

  return false;
}


bool Explain_join::explain_table_name()
{
  if (table->pos_in_table_list->is_view_or_derived() && !fmt->is_hierarchical())
  {
    /* Derived table name generation */
    char table_name_buffer[NAME_LEN];
    const size_t len= my_snprintf(table_name_buffer,
                                  sizeof(table_name_buffer) - 1,
                                  "<derived%u>",
                                  table->pos_in_table_list->query_block_id());
    return fmt->entry()->col_table_name.set(table_name_buffer, len);
  }
  else
    return fmt->entry()->col_table_name.set(table->pos_in_table_list->alias);
}


bool Explain_join::explain_select_type()
{
  if (tab && sj_is_materialize_strategy(tab->get_sj_strategy()))
    fmt->entry()->col_select_type.set(st_select_lex::SLT_MATERIALIZED);
  else
    return Explain::explain_select_type();
  return false;
}


bool Explain_join::explain_id()
{
  if (tab && sj_is_materialize_strategy(tab->get_sj_strategy()))
    fmt->entry()->col_id.set(tab->sjm_query_block_id());
  else
    return Explain::explain_id();
  return false;
}


bool Explain_join::explain_join_type()
{
  fmt->entry()->col_join_type.set_const(join_type_str[tab ? tab->type() : JT_ALL]);
  return false;
}


bool Explain_join::explain_key_and_len()
{
  if (!tab)
    return false;
  if (tab->ref().key_parts)
    return explain_key_and_len_index(tab->ref().key, tab->ref().key_length,
                                     tab->ref().key_parts);
  else if (tab->type() == JT_INDEX_SCAN || tab->type() == JT_FT)
    return explain_key_and_len_index(tab->index());
  else if (tab->type() == JT_RANGE || tab->type() == JT_INDEX_MERGE ||
      ((tab->type() == JT_REF || tab->type() == JT_REF_OR_NULL) &&
       tab->quick_optim()))
    return explain_key_and_len_quick(tab->quick_optim());
  else
  {
    const TABLE_LIST *table_list= table->pos_in_table_list;
    if (table_list->schema_table &&
        table_list->schema_table->i_s_requested_object & OPTIMIZE_I_S_TABLE)
    {
      StringBuffer<512> str_key(cs);
      const char *f_name;
      int f_idx;
      if (table_list->has_db_lookup_value)
      {
        f_idx= table_list->schema_table->idx_field1;
        f_name= table_list->schema_table->fields_info[f_idx].field_name;
        str_key.append(f_name, strlen(f_name), cs);
      }
      if (table_list->has_table_lookup_value)
      {
        if (table_list->has_db_lookup_value)
          str_key.append(',');
        f_idx= table_list->schema_table->idx_field2;
        f_name= table_list->schema_table->fields_info[f_idx].field_name;
        str_key.append(f_name, strlen(f_name), cs);
      }
      if (str_key.length())
        return fmt->entry()->col_key.set(str_key);
    }
  }
  return false;
}


bool Explain_join::explain_ref()
{
  if (!tab)
    return false;
  return explain_ref_key(fmt, tab->ref().key_parts, tab->ref().key_copy);
}

static void human_readable_size(char *buf, int buf_len, double data_size)
{
  char size[]= " KMGTP";
  int i;
  for (i= 0; data_size > 1024 && i < 5; i++)
    data_size/= 1024;
  const char mult= i == 0 ? 0 : size[i];
  my_snprintf(buf, buf_len, "%llu%c", (ulonglong)data_size, mult);
  buf[buf_len - 1]= 0;
}


bool Explain_join::explain_rows_and_filtered()
{
  if (!tab || tab->table_ref->schema_table)
    return false;

  POSITION *const pos= tab->position();

  fmt->entry()->col_rows.set(static_cast<ulonglong>(pos->rows_fetched));
  fmt->entry()->col_filtered.
    set(pos->rows_fetched ?
        static_cast<float>(100.0 * tab->position()->filter_effect) :
        0.0f);
  // Print cost-related info
  double prefix_rows= pos->prefix_rowcount;
  fmt->entry()->col_prefix_rows.set(static_cast<ulonglong>(prefix_rows));
  double const cond_cost= join->cost_model()->row_evaluate_cost(prefix_rows);
  fmt->entry()->col_cond_cost.set(cond_cost < 0 ? 0 : cond_cost);

  fmt->entry()->col_read_cost.set(pos->read_cost < 0.0 ?
                                  0.0 : pos->read_cost);
  fmt->entry()->col_prefix_cost.set(pos->prefix_cost);

  // Calculate amount of data from this table per query
  char data_size_str[32];
  double data_size= prefix_rows * tab->table()->s->rec_buff_length;
  human_readable_size(data_size_str, sizeof(data_size_str), data_size);
  fmt->entry()->col_data_size_query.set(data_size_str);

  return false;
}


bool Explain_join::explain_extra()
{
  if (!tab)
    return false;
  if (tab->type() == JT_SYSTEM && tab->position()->rows_fetched == 0.0)
  {
    if (push_extra(ET_CONST_ROW_NOT_FOUND))
      return true;                              /* purecov: inspected */
  }
  else if (tab->type() == JT_CONST && tab->position()->rows_fetched == 0.0)
  {
    if (push_extra(ET_UNIQUE_ROW_NOT_FOUND))
      return true;                              /* purecov: inspected */
  }
  else if (tab->type() == JT_CONST && tab->position()->rows_fetched == 1.0 &&
           tab->table()->has_null_row())
  {
    if (push_extra(ET_IMPOSSIBLE_ON_CONDITION))
      return true;                              /* purecov: inspected */
  }
  else
  {
    uint keyno= MAX_KEY;
    if (tab->ref().key_parts)
      keyno= tab->ref().key;
    else if (tab->type() == JT_RANGE || tab->type() == JT_INDEX_MERGE)
      keyno = tab->quick_optim()->index;

    if (explain_extra_common(quick_type, keyno))
      return true;

    const TABLE_LIST *table_list= table->pos_in_table_list;
    if (table_list->schema_table &&
        table_list->schema_table->i_s_requested_object & OPTIMIZE_I_S_TABLE)
    {
      if (!table_list->table_open_method)
      {
        if (push_extra(ET_SKIP_OPEN_TABLE))
          return true;
      }
      else if (table_list->table_open_method == OPEN_FRM_ONLY)
      {
        if (push_extra(ET_OPEN_FRM_ONLY))
          return true;
      }
      else
      {
        if (push_extra(ET_OPEN_FULL_TABLE))
          return true;
      }
      
      StringBuffer<32> buff(cs);
      if (table_list->has_db_lookup_value &&
          table_list->has_table_lookup_value)
      {
        if (push_extra(ET_SCANNED_DATABASES, "0"))
          return true;
      }
      else if (table_list->has_db_lookup_value ||
               table_list->has_table_lookup_value)
      {
        if (push_extra(ET_SCANNED_DATABASES, "1"))
          return true;
      }
      else
      {
        if (push_extra(ET_SCANNED_DATABASES, "all"))
          return true;
      }
    }
    if (((tab->type() == JT_INDEX_SCAN || tab->type() == JT_CONST) &&
         table->covering_keys.is_set(tab->index())) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT &&
         !((QUICK_ROR_INTERSECT_SELECT*) tab->quick_optim())->need_to_fetch_row) ||
        /*
          Notice that table->key_read can change on the fly (grep
          for set_keyread); so EXPLAIN CONNECTION reads a changing variable,
          fortunately it's a bool and not a pointer and the consequences
          cannot be severe (at worst, wrong EXPLAIN).
        */
        table->key_read || tab->keyread_optim())
    {
      if (quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
      {
        QUICK_GROUP_MIN_MAX_SELECT *qgs=
          (QUICK_GROUP_MIN_MAX_SELECT *) tab->quick_optim();
        StringBuffer<64> buff(cs);
        qgs->append_loose_scan_type(&buff);
        if (push_extra(ET_USING_INDEX_FOR_GROUP_BY, buff))
          return true;
      }
      else
      {
        if (push_extra(ET_USING_INDEX))
          return true;
      }
    }

    if (explain_tmptable_and_filesort(need_tmp_table, need_order))
      return true;
    need_tmp_table= need_order= false;

    if (distinct && test_all_bits(used_tables,
                                  join->thd->query_plan.get_lex()->used_tables) &&
        push_extra(ET_DISTINCT))
      return true;

    if (tab->do_loosescan() && push_extra(ET_LOOSESCAN))
      return true;

    if (tab->starts_weedout())
    {
      if (!fmt->is_hierarchical() && push_extra(ET_START_TEMPORARY))
        return true;
    }
    if (tab->finishes_weedout())
    {
      if (!fmt->is_hierarchical() && push_extra(ET_END_TEMPORARY))
        return true;
    }
    else if (tab->do_firstmatch())
    {
      if (tab->firstmatch_return == PRE_FIRST_PLAN_IDX)
      {
        if (push_extra(ET_FIRST_MATCH))
          return true;
      }
      else
      {
        StringBuffer<64> buff(cs);
        TABLE *prev_table= join->qep_tab[tab->firstmatch_return].table();
        if (prev_table->pos_in_table_list->query_block_id() &&
            !fmt->is_hierarchical() &&
            prev_table->pos_in_table_list->is_derived())
        {
          char namebuf[NAME_LEN];
          /* Derived table name generation */
          size_t len= my_snprintf(namebuf, sizeof(namebuf)-1,
              "<derived%u>",
              prev_table->pos_in_table_list->query_block_id());
          buff.append(namebuf, len);
        }
        else
          buff.append(prev_table->pos_in_table_list->alias);
        if (push_extra(ET_FIRST_MATCH, buff))
          return true;
      }
    }

    if (tab->has_guarded_conds() && push_extra(ET_FULL_SCAN_ON_NULL_KEY))
      return true;

    if (tab->op && tab->op->type() == QEP_operation::OT_CACHE)
    {
      const JOIN_CACHE::enum_join_cache_type t=
        static_cast<JOIN_CACHE*>(tab->op)->cache_type();
      StringBuffer<64> buff(cs);
      if (t == JOIN_CACHE::ALG_BNL)
        buff.append("Block Nested Loop");
        else if (t == JOIN_CACHE::ALG_BKA)
        buff.append("Batched Key Access");
      else if (t == JOIN_CACHE::ALG_BKA_UNIQUE)
        buff.append("Batched Key Access (unique)");
      else
        assert(0); /* purecov: inspected */
      if (push_extra(ET_USING_JOIN_BUFFER, buff))
        return true;
    }
  }
  if (fmt->is_hierarchical() &&
      (!bitmap_is_clear_all(table->read_set) ||
       !bitmap_is_clear_all(table->write_set)))
  {
    Field **fld;
    for (fld= table->field; *fld; fld++)
    {
      if (!bitmap_is_set(table->read_set, (*fld)->field_index) &&
          !bitmap_is_set(table->write_set, (*fld)->field_index))
        continue;
      fmt->entry()->col_used_columns.push_back((*fld)->field_name);
    }
  }
  return false;
}


/* Explain_table class functions **********************************************/

bool Explain_table::explain_modify_flags()
{
  fmt->entry()->mod_type= mod_type;
  return false;
}


bool Explain_table::explain_tmptable_and_filesort(bool need_tmp_table_arg,
                                                  bool need_sort_arg)
{
  if (fmt->is_hierarchical())
  {
    /*
      For hierarchical EXPLAIN we output "using_temporary_table" and
      "using_filesort" with related ORDER BY, GROUP BY or DISTINCT
      (excluding the single-table UPDATE command that updates used key --
      in this case we output "using_temporary_table: for update"
      at the "table" node)
    */
    if (need_tmp_table_arg)
    {
      assert(used_key_is_modified || order_list);
      if (used_key_is_modified && push_extra(ET_USING_TEMPORARY, "for update"))
        return true;
    }
  }
  else
  {
    if (need_tmp_table_arg && push_extra(ET_USING_TEMPORARY))
      return true;

    if (need_sort_arg && push_extra(ET_USING_FILESORT))
      return true;
  }

  return false;
}


bool Explain_table::shallow_explain()
{
  Explain_format_flags flags;
  if (order_list)
  {
    flags.set(ESC_ORDER_BY, ESP_EXISTS);
    if (need_sort)
      flags.set(ESC_ORDER_BY, ESP_USING_FILESORT);
    if (!used_key_is_modified && need_tmp_table)
      flags.set(ESC_ORDER_BY, ESP_USING_TMPTABLE);
  }

  if (order_list && fmt->begin_context(CTX_ORDER_BY, NULL, &flags))
    return true;

  if (fmt->begin_context(CTX_QEP_TAB))
    return true;

  if (Explain::shallow_explain() ||
      (can_walk_clauses() &&
       mark_subqueries(select_lex->where_cond(), fmt->entry())))
    return true;

  if (fmt->end_context(CTX_QEP_TAB))
    return true;

  if (order_list && fmt->end_context(CTX_ORDER_BY))
    return true;

  return false;
}


bool Explain_table::explain_table_name()
{
  return fmt->entry()->col_table_name.set(table->alias);
}


bool Explain_table::explain_join_type()
{
  join_type jt;
  if (tab && tab->quick_optim())
    jt= calc_join_type(tab->quick_optim()->get_type());
  else if (key != MAX_KEY)
    jt= JT_INDEX_SCAN;
  else
    jt= JT_ALL;

  fmt->entry()->col_join_type.set_const(join_type_str[jt]);
  return false;
}


bool Explain_table::explain_ref()
{
  if (tab && tab->quick_optim())
  {
    int key_parts= tab->quick_optim()->used_key_parts;
    while(key_parts--)
    {
      fmt->entry()->col_ref.push_back("const");
    }
  }
  return false;
}


bool Explain_table::explain_key_and_len()
{
  if (tab && tab->quick_optim())
    return explain_key_and_len_quick(tab->quick_optim());
  else if (key != MAX_KEY)
    return explain_key_and_len_index(key);
  return false;
}


bool Explain_table::explain_rows_and_filtered()
{
  /* Don't print estimated # of rows in table for INSERT/REPLACE. */
  if (fmt->entry()->mod_type == MT_INSERT ||
      fmt->entry()->mod_type == MT_REPLACE)
    return false;

  ha_rows examined_rows=
    table->in_use->query_plan.get_modification_plan()->examined_rows;
  fmt->entry()->col_rows.set(static_cast<long long>(examined_rows));

  fmt->entry()->col_filtered.set(100.0);
  
  return false;
}


bool Explain_table::explain_extra()
{
  if (message)
    return fmt->entry()->col_message.set(message);

  uint keyno;
  int quick_type;
  if (tab && tab->quick_optim())
  {
    keyno= tab->quick_optim()->index;
    quick_type= tab->quick_optim()->get_type();
  }
  else
  {
    keyno= key;
    quick_type= -1;
  }

  return (explain_extra_common(quick_type, keyno) ||
          explain_tmptable_and_filesort(need_tmp_table, need_sort));
}


/******************************************************************************
  External function implementations
******************************************************************************/


/**
  Send a message as an "extra" column value

  This function forms the 1st row of the QEP output with a simple text message.
  This is useful to explain such trivial cases as "No tables used" etc.

  @note Also this function explains the rest of QEP (subqueries or joined
        tables if any).

  @param thd        current THD
  @param select_lex select_lex to explain
  @param message    text message for the "extra" column.
  @param ctx        current query context, CTX_JOIN in most cases.

  @return false if success, true if error
*/

bool explain_no_table(THD *thd, SELECT_LEX *select_lex, const char *message,
                      enum_parsing_context ctx)
{
  DBUG_ENTER("explain_no_table");
  const bool ret= Explain_no_table(thd, select_lex, message, ctx,
                                   HA_POS_ERROR).send();
  DBUG_RETURN(ret);
}


/**
  Check that we are allowed to explain all views in list.
  Because this function is called only when we have a complete plan, we know
  that:
  - views contained in merge-able views have been merged and brought up in
  the top list of tables, so we only need to scan this list
  - table_list is not changing while we are reading it.
  If we don't have a complete plan, EXPLAIN output does not contain table
  names, so we don't need to check views.

  @param table_list table to start with, usually lex->query_tables

  @returns
    true   Caller can't EXPLAIN query due to lack of rights on a view in the
           query
    false  Caller can EXPLAIN query
*/

static bool check_acl_for_explain(const TABLE_LIST *table_list)
{
  for (const TABLE_LIST *tbl= table_list; tbl; tbl= tbl->next_global)
  {
    if (tbl->is_view() && tbl->view_no_explain)
    {
      my_message(ER_VIEW_NO_EXPLAIN, ER(ER_VIEW_NO_EXPLAIN), MYF(0));
      return true;
    }
  }
  return false;
}


/**
  EXPLAIN handling for single-table UPDATE and DELETE queries

  Send to the client a QEP data set for single-table EXPLAIN UPDATE/DELETE
  queries. As far as single-table UPDATE/DELETE are implemented without
  the regular JOIN tree, we can't reuse explain_unit() directly,
  thus we deal with this single table in a special way and then call
  explain_unit() for subqueries (if any).

  @param thd            current THD
  @param plan           table modification plan
  @param select         Query's select lex

  @return false if success, true if error
*/

bool explain_single_table_modification(THD *ethd,
                                       const Modification_plan *plan,
                                       SELECT_LEX *select)
{
  DBUG_ENTER("explain_single_table_modification");
  Query_result_send result;
  const THD *const query_thd= select->master_unit()->thd;
  const bool other= (query_thd != ethd);
  bool ret;

  /**
    Prepare the self-allocated result object

    For queries with top-level JOIN the caller provides pre-allocated
    Query_result_send object. Then that JOIN object prepares the
    Query_result_send object calling result->prepare() in SELECT_LEX::prepare(),
    result->initalize_tables() in JOIN::optimize() and result->prepare2()
    in JOIN::exec().
    However without the presence of the top-level JOIN we have to
    prepare/initialize Query_result_send object manually.
  */
  List<Item> dummy;
  if (result.prepare(dummy, ethd->lex->unit) ||
      result.prepare2())
    DBUG_RETURN(true); /* purecov: inspected */

  ethd->lex->explain_format->send_headers(&result);

  if (!other)
  {
    for (SELECT_LEX_UNIT *unit= select->first_inner_unit();
         unit;
         unit= unit->next_unit())
    {
      // Derived tables and const subqueries are already optimized
      if (!unit->is_optimized() && unit->optimize(ethd))
        DBUG_RETURN(true);  /* purecov: inspected */
    }
  }


  if (!plan || plan->zero_result)
  {
    ret= Explain_no_table(ethd, select,
                          plan ? plan->message : plan_not_ready,
                          CTX_JOIN,
                          HA_POS_ERROR).send();
  }
  else
  {
    // Check access rights for views
    if (other &&
        check_acl_for_explain(query_thd->query_plan.get_lex()->query_tables))
      ret= true;
    else
      ret= Explain_table(ethd, select, plan->table,
                         plan->tab,
                         plan->key,
                         plan->limit,
                         plan->need_tmp_table,
                         plan->need_sort,
                         plan->mod_type,
                         plan->used_key_is_modified,
                         plan->message).send() ||
        ethd->is_error();
  }
  if (ret)
    result.abort_result_set();
  else
    result.send_eof();
  DBUG_RETURN(ret);
}


/**
  Explain select_lex's join.

  @param ethd        THD of explaining thread
  @param select_lex  explain join attached to given select_lex
  @param ctx         current explain context
*/

bool
explain_query_specification(THD *ethd, SELECT_LEX *select_lex,
                            enum_parsing_context ctx)
{
  Opt_trace_context * const trace= &ethd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_exec(trace, "join_explain");
  trace_exec.add_select_number(select_lex->select_number);
  Opt_trace_array trace_steps(trace, "steps");
  JOIN *join= select_lex->join;

  if (!join || join->get_plan_state() == JOIN::NO_PLAN)
    return explain_no_table(ethd, select_lex, plan_not_ready, ctx);

  const bool other= (join->thd != ethd);
  THD::Query_plan const *query_plan= &join->thd->query_plan;

  // Check access rights for views
  if (other && check_acl_for_explain(query_plan->get_lex()->query_tables))
    return true;

  THD_STAGE_INFO(ethd, stage_explaining);

  bool ret;

  switch (join->get_plan_state())
  {
    case JOIN::ZERO_RESULT:
    {
      ret= explain_no_table(ethd, select_lex, join->zero_result_cause,
                            ctx);
      break;
    }
    case JOIN::NO_TABLES:
    {
      if (query_plan->get_lex()->insert_table_leaf &&
          query_plan->get_lex()->insert_table_leaf->select_lex == select_lex)
      {
        // INSERT/REPLACE SELECT ... FROM dual
        ret= Explain_table(ethd, select_lex,
                           query_plan->get_lex()->insert_table_leaf->table,
                           NULL,
                           MAX_KEY,
                           HA_POS_ERROR,
                           false,
                           false,
                           (query_plan->get_lex()->sql_command == SQLCOM_INSERT_SELECT ?
                            MT_INSERT : MT_REPLACE),
                           false,
                           NULL).send() || ethd->is_error();
      }
      else
        ret= explain_no_table(ethd, select_lex, "No tables used", CTX_JOIN);

      break;
    }
    case JOIN::PLAN_READY:
    {
      /*
        (1) If this connection is explaining its own query
        (2) and it hasn't already prepared the JOIN's result,
        then we need to prepare it (for example, to materialize I_S tables).
      */
      if (!other && !join->is_executed() && join->prepare_result())
        return true; /* purecov: inspected */

      const Explain_format_flags *flags= &join->explain_flags;
      const bool need_tmp_table= flags->any(ESP_USING_TMPTABLE);
      const bool need_order= flags->any(ESP_USING_FILESORT);
      const bool distinct= flags->get(ESC_DISTINCT, ESP_EXISTS);

      if (select_lex == select_lex->master_unit()->fake_select_lex)
        ret= Explain_union_result(ethd, select_lex).send();
      else
        ret= Explain_join(ethd, select_lex, need_tmp_table, need_order,
                          distinct).send();
      break;
    }
    default:
      assert(0); /* purecov: inspected */
      ret= true;
  }
  assert(ret || !ethd->is_error());
  ret|= ethd->is_error();
  return ret;
}


/**
  EXPLAIN handling for SELECT, INSERT/REPLACE SELECT, and multi-table
  UPDATE/DELETE queries

  Send to the client a QEP data set for any DML statement that has a QEP
  represented completely by JOIN object(s).

  This function uses a specific Query_result object for sending explain
  output to the client.

  When explaining own query, the existing Query_result object (found
  in outermost SELECT_LEX_UNIT or SELECT_LEX) is used. However, if the
  Query_result is unsuitable for explanation (need_explain_interceptor()
  returns true), wrap the Query_result inside an Query_result_explain object.

  When explaining other query, create a Query_result_send object and prepare it
  as if it was a regular SELECT query.

  @note see explain_single_table_modification() for single-table
        UPDATE/DELETE EXPLAIN handling.

  @note Unlike handle_query(), explain_query() calls abort_result_set()
        itself in the case of failure (OOM etc.) since it may use
        an internally created Query_result object that has to be deleted
        before exiting the function.

  @param ethd    THD of the explaining session
  @param unit    query tree to explain

  @return false if success, true if error
*/

bool explain_query(THD *ethd, SELECT_LEX_UNIT *unit)
{
  DBUG_ENTER("explain_query");

  const THD *const query_thd= unit->thd; // THD of query to be explained
  const bool other= (ethd != query_thd);

  Query_result *explain_result= NULL;

  if (!other)
    explain_result= unit->query_result() ?
                    unit->query_result() : unit->first_select()->query_result();

  Query_result_explain explain_wrapper(unit, explain_result);

  if (other)  
  {
    if (!((explain_result= new Query_result_send)))
      DBUG_RETURN(true); /* purecov: inspected */
    List<Item> dummy;
    if (explain_result->prepare(dummy, ethd->lex->unit) ||
        explain_result->prepare2())
      DBUG_RETURN(true); /* purecov: inspected */
  }
  else
  {
    assert(unit->is_optimized());
    if (explain_result->need_explain_interceptor())
      explain_result= &explain_wrapper;
  }

  ethd->lex->explain_format->send_headers(explain_result);

  // Reset OFFSET/LIMIT for EXPLAIN output
  ethd->lex->unit->offset_limit_cnt= 0;
  ethd->lex->unit->select_limit_cnt= 0;

  const bool res= mysql_explain_unit(ethd, unit);
  /*
    1) The code which prints the extended description is not robust
       against malformed queries, so skip it if we have an error.
    2) The code also isn't thread-safe, skip if explaining other thread
    (see Explain::can_print_clauses())
    3) Currently only SELECT queries can be printed (TODO: fix this)
  */
  if (!res &&                                       // (1)
      !other &&                                     // (2)
      query_thd->query_plan.get_command() == SQLCOM_SELECT) // (3)
  {
    StringBuffer<1024> str;
    /*
      The warnings system requires input in utf8, see mysqld_show_warnings().
    */
    unit->print(&str, enum_query_type(QT_TO_SYSTEM_CHARSET |
                                      QT_SHOW_SELECT_NUMBER));
    str.append('\0');
    push_warning(ethd, Sql_condition::SL_NOTE, ER_YES, str.ptr());
  }

  if (res)
    explain_result->abort_result_set();
  else
    explain_result->send_eof();

  if (other)
    delete explain_result;

  DBUG_RETURN(res);
}


/**
  Explain UNION or subqueries of the unit

  If the unit is a UNION, explain it as a UNION. Otherwise explain nested
  subselects.

  @param ethd           THD of explaining thread
  @param unit           unit object, might not belong to ethd

  @return false if success, true if error
*/

bool mysql_explain_unit(THD *ethd, SELECT_LEX_UNIT *unit)
{
  DBUG_ENTER("mysql_explain_unit");
  bool res= false;
  if (unit->is_union())
    res= unit->explain(ethd);
  else
    res= explain_query_specification(ethd, unit->first_select(), CTX_JOIN);
  assert(res || !ethd->is_error());
  res|= ethd->is_error();
  DBUG_RETURN(res);
}

/**
  Callback function used by mysql_explain_other() to find thd based
  on the thread id.

  @note It acquires LOCK_thd_data mutex and LOCK_query_plan mutex,
  when it finds matching thd.
  It is the responsibility of the caller to release LOCK_thd_data.
  We release LOCK_query_plan in the DTOR.
*/
class Find_thd_query_lock: public Find_THD_Impl
{
public:
  explicit Find_thd_query_lock(my_thread_id value)
    : m_id(value), m_thd(NULL)
  {}
  ~Find_thd_query_lock()
  {
    if (m_thd)
      m_thd->unlock_query_plan();
  }
  virtual bool operator()(THD *thd)
  {
    if (thd->thread_id() == m_id)
    {
      mysql_mutex_lock(&thd->LOCK_thd_data);
      thd->lock_query_plan();
      m_thd= thd;
      return true;
    }
    return false;
  }
private:
  const my_thread_id m_id; ///< The thread id we are looking for.
  THD *m_thd;              ///< THD we found, having this ID.
};


/**
   Entry point for EXPLAIN CONNECTION: locates the connection by its ID, takes
   proper locks, explains its current statement, releases locks.
   @param  THD executing this function (== the explainer)
*/
void mysql_explain_other(THD *thd)
{
  bool res= false;
  THD *query_thd= NULL;
  bool send_ok= false;
  char *user;
  bool unlock_thd_data= false;
  THD::Query_plan *qp;
  DEBUG_SYNC(thd, "before_explain_other");
  /*
    Check for a super user, if:
    1) connected user don't have enough rights, or
    2) has switched to another user
    then it's not super user.
  */
  if (!(thd->m_main_security_ctx.check_access(
          GLOBAL_ACLS & ~GRANT_ACL)) || // (1)
      (0 != strcmp(thd->m_main_security_ctx.priv_user().str,        // (2)
                   thd->security_context()->priv_user().str) ||
       0 != my_strcasecmp(system_charset_info,
                          thd->m_main_security_ctx.priv_host().str,
                          thd->security_context()->priv_host().str)))
  {
    // Can see only connections of this user
    user= (char *) thd->security_context()->priv_user().str;
  }
  else
  {
    // Can see all connections
    user= NULL;
  }

  // Pick thread
  Find_thd_query_lock find_thd_query_lock(thd->lex->query_id);
  if (!thd->killed)
  {
    query_thd= Global_THD_manager::
               get_instance()->find_thd(&find_thd_query_lock);
    if (query_thd)
      unlock_thd_data= true;
  }

  if (!query_thd)
  {
    my_error(ER_NO_SUCH_THREAD, MYF(0), thd->lex->query_id);
    goto err;
  }

  qp= &query_thd->query_plan;

  if (query_thd->get_protocol()->connection_alive() &&
      !query_thd->system_thread && qp->get_command() != SQLCOM_END)
  {
    /*
      Don't explain:
      1) Prepared statements
      2) EXPLAIN to avoid clash in EXPLAIN code
      3) statements of stored routine
    */
    if (!qp->is_ps_query() &&                                        // (1)
        is_explainable_query(qp->get_command()) &&
        !qp->get_lex()->describe &&                                  // (2)
        qp->get_lex()->sphead == NULL)                               // (3)
    {
      Security_context *tmp_sctx= query_thd->security_context();
      assert(tmp_sctx->user().str);
      if (user && strcmp(tmp_sctx->user().str, user))
      {
        my_error(ER_ACCESS_DENIED_ERROR, MYF(0),
                 thd->security_context()->priv_user().str,
                 thd->security_context()->priv_host().str,
                 (thd->password ?
                  ER(ER_YES) :
                  ER(ER_NO)));
        goto err;
      }
      mysql_mutex_unlock(&query_thd->LOCK_thd_data);
      unlock_thd_data= false;
    }
    else
    {
      my_error(ER_EXPLAIN_NOT_SUPPORTED, MYF(0));
      goto err;
    }
  }
  else
  {
    send_ok= true;
    goto err;
  }
  DEBUG_SYNC(thd, "explain_other_got_thd");
  // Get topmost query
  switch(qp->get_command())
  {
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE_MULTI:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_SELECT:
      res= explain_query(thd, qp->get_lex()->unit);
      break;
    case SQLCOM_UPDATE:
    case SQLCOM_DELETE:
    case SQLCOM_INSERT:
    case SQLCOM_REPLACE:
      res= explain_single_table_modification(thd, qp->get_modification_plan(),
                                             qp->get_lex()->unit->first_select());
      break;
    default:
      assert(0); /* purecov: inspected */
      send_ok= true; /* purecov: inspected */
      break;
  }

err:
  if (unlock_thd_data)
    mysql_mutex_unlock(&query_thd->LOCK_thd_data);

  DEBUG_SYNC(thd, "after_explain_other");
  if (!res && send_ok)
    my_ok(thd, 0);
}


void Modification_plan::register_in_thd()
{
  thd->lock_query_plan();
  assert(thd->query_plan.get_modification_plan() == NULL);
  thd->query_plan.set_modification_plan(this);
  thd->unlock_query_plan();
}


/**
  Modification_plan's constructor, to represent that we will use an access
  method on the table.

  @details
  Create single table modification plan. The plan is registered in the
  given thd unless the modification is done in a sub-statement
  (function/trigger).

  @param thd_arg        owning thread
  @param mt             modification type - MT_INSERT/MT_UPDATE/etc
  @param tab_arg        Table to modify
  @param key_arg        MAX_KEY or and index number of the key that was chosen
                        to access table data.
  @param limit_arg      HA_POS_ERROR or LIMIT value.
  @param need_tmp_table_arg true if it requires temporary table --
                        "Using temporary"
                        string in the "extra" column.
  @param need_sort_arg  true if it requires filesort() -- "Using filesort"
                        string in the "extra" column.
  @param used_key_is_modified   UPDATE updates used key column
  @param rows           How many rows we plan to modify in the table.
*/

Modification_plan::Modification_plan(THD *thd_arg,
                                     enum_mod_type mt, QEP_TAB *tab_arg,
                                     uint key_arg,
                                     ha_rows limit_arg, bool need_tmp_table_arg,
                                     bool need_sort_arg,
                                     bool used_key_is_modified_arg,
                                     ha_rows rows) :
  thd(thd_arg), mod_type(mt), table(tab_arg->table()), tab(tab_arg),
  key(key_arg), limit(limit_arg),
  need_tmp_table(need_tmp_table_arg), need_sort(need_sort_arg),
  used_key_is_modified(used_key_is_modified_arg), message(NULL),
  zero_result(false), examined_rows(rows)
{
  assert(current_thd == thd);
  if (!thd->in_sub_stmt)
    register_in_thd();
}


/**
  Modification_plan's constructor, to convey a message in the "extra" column
  of EXPLAIN. This is for the case where this message is the main information
  (there is no access path to the table).

  @details
  Create minimal single table modification plan. The plan is registered in the
  given thd unless the modification is done in a sub-statement
  (function/trigger).

  @param thd_arg    Owning thread
  @param mt         Modification type - MT_INSERT/MT_UPDATE/etc
  @param table_arg  Table to modify
  @param message_arg Message
  @param zero_result_arg If we shortcut execution
  @param rows       How many rows we plan to modify in the table.
*/

Modification_plan::Modification_plan(THD *thd_arg,
                                     enum_mod_type mt, TABLE *table_arg,
                                     const char *message_arg,
                                     bool zero_result_arg,
                                     ha_rows rows) :
  thd(thd_arg), mod_type(mt), table(table_arg),
  tab(NULL), key(MAX_KEY), limit(HA_POS_ERROR), need_tmp_table(false),
  need_sort(false), used_key_is_modified(false), message(message_arg),
  zero_result(zero_result_arg), examined_rows(rows)
{
  assert(current_thd == thd);
  if (!thd->in_sub_stmt)
    register_in_thd();
};


Modification_plan::~Modification_plan()
{
  if (!thd->in_sub_stmt)
  {
    thd->lock_query_plan();
    assert(current_thd == thd &&
           thd->query_plan.get_modification_plan() == this);
    thd->query_plan.set_modification_plan(NULL);
    thd->unlock_query_plan();
  }
}
