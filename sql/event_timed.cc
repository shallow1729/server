/* Copyright (C) 2000-2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "mysql_priv.h"
#include "event.h"
#include "event_priv.h"
#include "sp.h"



extern int yyparse(void *thd);

/*
 Init all member variables

 SYNOPSIS
   event_timed::init()
*/

void
event_timed::init()
{
  DBUG_ENTER("event_timed::init");

  m_db.str= m_name.str= m_body.str= m_comment.str= 0;
  m_db.length= m_name.length= m_body.length= m_comment.length= 0;
  
  set_zero_time(&m_starts, MYSQL_TIMESTAMP_DATETIME);
  set_zero_time(&m_ends, MYSQL_TIMESTAMP_DATETIME);
  set_zero_time(&m_execute_at, MYSQL_TIMESTAMP_DATETIME);
  set_zero_time(&m_last_executed, MYSQL_TIMESTAMP_DATETIME);

  m_definer_user.str= m_definer_host.str= 0;
  m_definer_user.length= m_definer_host.length= 0;
    
  DBUG_VOID_RETURN;
}


/*
 Set a name of the event

 SYNOPSIS
   event_timed::init_name()
     thd   THD
     name  the name extracted in the parser
*/

void
event_timed::init_name(THD *thd, sp_name *name)
{
  DBUG_ENTER("event_timed::init_name");
  uint n;			/* Counter for nul trimming */ 
  /* During parsing, we must use thd->mem_root */
  MEM_ROOT *root= thd->mem_root;

  /* We have to copy strings to get them into the right memroot */
  if (name)
  {
    m_db.length= name->m_db.length;
    if (name->m_db.length == 0)
      m_db.str= NULL;
    else
      m_db.str= strmake_root(root, name->m_db.str, name->m_db.length);
    m_name.length= name->m_name.length;
    m_name.str= strmake_root(root, name->m_name.str, name->m_name.length);

    if (name->m_qname.length == 0)
      name->init_qname(thd);
  }
  else if (thd->db)
  {
    m_db.length= thd->db_length;
    m_db.str= strmake_root(root, thd->db, m_db.length);
  }
  
  DBUG_PRINT("m_db", ("len=%d db=%s",m_db.length, m_db.str));  
  DBUG_PRINT("m_name", ("len=%d name=%s",m_name.length, m_name.str));  

  DBUG_VOID_RETURN;
}


/*
 Set body of the event - what should be executed.

 SYNOPSIS
   event_timed::init_body()
     thd   THD

  NOTE
    The body is extracted by copying all data between the
    start of the body set by another method and the current pointer in Lex.
*/

void
event_timed::init_body(THD *thd)
{
  DBUG_ENTER("event_timed::init_body");
  MEM_ROOT *root= thd->mem_root;

  m_body.length= thd->lex->ptr - m_body_begin;
  // Trim nuls at the end 
  while (m_body.length && m_body_begin[m_body.length-1] == '\0')
    m_body.length--;

  m_body.str= strmake_root(root, (char *)m_body_begin, m_body.length);

  DBUG_VOID_RETURN;
}


/*
 Set time for execution for one time events.

 SYNOPSIS
   event_timed::init_execute_at()
     expr   when (datetime)

 RETURNS
   0 - OK
   EVEX_PARSE_ERROR - fix_fields failed
   EVEX_BAD_PARAMS  - datetime is in the past
*/

int
event_timed::init_execute_at(THD *thd, Item *expr)
{
  my_bool not_used;
  TIME ltime;
  my_time_t my_time_tmp;

  TIME time_tmp;
  DBUG_ENTER("event_timed::init_execute_at");

  if (expr->fix_fields(thd, &expr))
    DBUG_RETURN(EVEX_PARSE_ERROR);

  if (expr->val_int() == MYSQL_TIMESTAMP_ERROR)
    DBUG_RETURN(EVEX_BAD_PARAMS);

  // let's check whether time is in the past
  thd->variables.time_zone->gmt_sec_to_TIME(&time_tmp, 
                              (my_time_t) thd->query_start()); 

  if (expr->val_int() < TIME_to_ulonglong_datetime(&time_tmp))
    DBUG_RETURN(EVEX_BAD_PARAMS);

  if ((not_used= expr->get_date(&ltime, TIME_NO_ZERO_DATE)))
    DBUG_RETURN(EVEX_BAD_PARAMS);

  /*
      This may result in a 1970-01-01 date if ltime is > 2037-xx-xx
      CONVERT_TZ has similar problem
  */
  my_tz_UTC->gmt_sec_to_TIME(&ltime, TIME_to_timestamp(thd,&ltime, &not_used));


  m_execute_at= ltime;
  DBUG_RETURN(0);
}


/*
 Set time for execution for transient events.

 SYNOPSIS
   event_timed::init_interval()
     expr      how much?
     interval  what is the interval

 RETURNS
   0 - OK
   EVEX_PARSE_ERROR - fix_fields failed
   EVEX_BAD_PARAMS  - Interval is not positive
*/

int
event_timed::init_interval(THD *thd, Item *expr, interval_type interval)
{
  longlong tmp;
  DBUG_ENTER("event_timed::init_interval");

  if (expr->fix_fields(thd, &expr))
    DBUG_RETURN(EVEX_PARSE_ERROR);

  if ((tmp= expr->val_int()) <= 0)
    DBUG_RETURN(EVEX_BAD_PARAMS);

  m_expr= tmp;
  m_interval= interval;
  DBUG_RETURN(0);
}


/*
 Set activation time.

 SYNOPSIS
   event_timed::init_starts()
     expr      how much?
     interval  what is the interval

 NOTES
  Note that activation time is not execution time.
  EVERY 5 MINUTE STARTS "2004-12-12 10:00:00" means that
  the event will be executed every 5 minutes but this will
  start at the date shown above. Expressions are possible :
  DATE_ADD(NOW(), INTERVAL 1 DAY)  -- start tommorow at
  same time.

 RETURNS
   0 - OK
   EVEX_PARSE_ERROR - fix_fields failed
*/

int
event_timed::init_starts(THD *thd, Item *starts)
{
  my_bool not_used;
  TIME ltime;
  my_time_t my_time_tmp;

  DBUG_ENTER("event_timed::init_starts");

  if (starts->fix_fields(thd, &starts))
    DBUG_RETURN(EVEX_PARSE_ERROR);

  if (starts->val_int() == MYSQL_TIMESTAMP_ERROR)
    DBUG_RETURN(EVEX_BAD_PARAMS);

  if ((not_used= starts->get_date(&ltime, TIME_NO_ZERO_DATE)))
    DBUG_RETURN(EVEX_BAD_PARAMS);

  /*
      This may result in a 1970-01-01 date if ltime is > 2037-xx-xx
      CONVERT_TZ has similar problem
  */
  my_tz_UTC->gmt_sec_to_TIME(&ltime, TIME_to_timestamp(thd,&ltime, &not_used));

  m_starts= ltime;
  DBUG_RETURN(0);
}


/*
 Set deactivation time.

 SYNOPSIS
   event_timed::init_ends()
     thd      THD
     ends  when?

 NOTES
  Note that activation time is not execution time.
  EVERY 5 MINUTE ENDS "2004-12-12 10:00:00" means that
  the event will be executed every 5 minutes but this will
  end at the date shown above. Expressions are possible :
  DATE_ADD(NOW(), INTERVAL 1 DAY)  -- end tommorow at
  same time.

 RETURNS
   0 - OK
   EVEX_PARSE_ERROR - fix_fields failed
   EVEX_BAD_PARAMS  - ENDS before STARTS
*/

int 
event_timed::init_ends(THD *thd, Item *ends)
{
  TIME ltime;
  my_time_t my_time_tmp;
  my_bool not_used;

  DBUG_ENTER("event_timed::init_ends");

  if (ends->fix_fields(thd, &ends))
    DBUG_RETURN(EVEX_PARSE_ERROR);

    // the field was already fixed in init_ends
  if ((not_used= ends->get_date(&ltime, TIME_NO_ZERO_DATE)))
    DBUG_RETURN(EVEX_BAD_PARAMS);

  /*
    This may result in a 1970-01-01 date if ltime is > 2037-xx-xx
    CONVERT_TZ has similar problem
  */
  my_tz_UTC->gmt_sec_to_TIME(&ltime, TIME_to_timestamp(thd, &ltime, &not_used));
 
  if (m_starts.year && my_time_compare(&m_starts, &ltime) != -1)
    DBUG_RETURN(EVEX_BAD_PARAMS);

  m_ends= ltime;
  DBUG_RETURN(0);
}


/*
 Sets comment.

 SYNOPSIS
   event_timed::init_comment()
     thd      THD - used for memory allocation
     comment  the string.
*/

void
event_timed::init_comment(THD *thd, LEX_STRING *comment)
{
  DBUG_ENTER("event_timed::init_comment");

  MEM_ROOT *root= thd->mem_root;
  m_comment.length= comment->length;
  m_comment.str= strmake_root(root, comment->str, comment->length);
  DBUG_PRINT("m_comment", ("len=%d",m_comment.length));

  DBUG_VOID_RETURN;
}


/*
 Inits definer (m_definer_user and m_definer_host) during
 parsing.

 SYNOPSIS
   event_timed::init_definer()
*/

int
event_timed::init_definer(THD *thd)
{
  DBUG_ENTER("event_timed::init_definer");

  m_definer_user.str= strdup_root(thd->mem_root, thd->security_ctx->priv_user);
  m_definer_user.length= strlen(thd->security_ctx->priv_user);

  m_definer_host.str= strdup_root(thd->mem_root, thd->security_ctx->priv_host);
  m_definer_host.length= strlen(thd->security_ctx->priv_host);

  DBUG_RETURN(0);
}


/*
 Loads an event from a row from mysql.event
 
 SYNOPSIS
   event_timed::load_from_row()
*/

int
event_timed::load_from_row(MEM_ROOT *mem_root, TABLE *table)
{
  longlong created;
  longlong modified;
  char *ptr;
  event_timed *et;
  uint len;
  bool res1, res2;

  DBUG_ENTER("event_timed::load_from_row");

  if (!table)
    goto error;

  et= this;
  
  if (table->s->fields != EVEX_FIELD_COUNT)
    goto error;

  if ((et->m_db.str= get_field(mem_root,
                          table->field[EVEX_FIELD_DB])) == NULL)
    goto error;

  et->m_db.length= strlen(et->m_db.str);

  if ((et->m_name.str= get_field(mem_root,
                          table->field[EVEX_FIELD_NAME])) == NULL)
    goto error;

  et->m_name.length= strlen(et->m_name.str);

  if ((et->m_body.str= get_field(mem_root,
                          table->field[EVEX_FIELD_BODY])) == NULL)
    goto error;

  et->m_body.length= strlen(et->m_body.str);

  if ((et->m_definer.str= get_field(mem_root,
                          table->field[EVEX_FIELD_DEFINER])) == NullS)
    goto error;
  et->m_definer.length= strlen(et->m_definer.str);

  ptr= strchr(et->m_definer.str, '@');

  if (! ptr)
    ptr= et->m_definer.str;		// Weird, isn't it?

  len= ptr - et->m_definer.str;

  et->m_definer_user.str= strmake_root(mem_root, et->m_definer.str, len);
  et->m_definer_user.length= len;
  len= et->m_definer.length - len - 1; //1 is because of @
  et->m_definer_host.str= strmake_root(mem_root, ptr + 1, len);//1: because of @
  et->m_definer_host.length= len;
  
  
  res1= table->field[EVEX_FIELD_STARTS]->
                     get_date(&et->m_starts, TIME_NO_ZERO_DATE);

  res2= table->field[EVEX_FIELD_ENDS]->
                     get_date(&et->m_ends, TIME_NO_ZERO_DATE);
  
  et->m_expr= table->field[EVEX_FIELD_INTERVAL_EXPR]->val_int();

  /*
    If res1 and res2 are true then both fields are empty.
	Hence if EVEX_FIELD_EXECUTE_AT is empty there is an error.
  */
  if (res1 && res2 && !et->m_expr && table->field[EVEX_FIELD_EXECUTE_AT]->
                get_date(&et->m_execute_at, TIME_NO_ZERO_DATE))
    goto error;

  /*
    In DB the values start from 1 but enum interval_type starts
    from 0
  */
  et->m_interval= (interval_type)
       ((ulonglong) table->field[EVEX_FIELD_TRANSIENT_INTERVAL]->val_int() - 1);

  et->m_modified= table->field[EVEX_FIELD_CREATED]->val_int();
  et->m_created= table->field[EVEX_FIELD_MODIFIED]->val_int();

  /*
    ToDo Andrey : Ask PeterG & Serg what to do in this case.
                  Whether on load last_executed_at should be loaded
                  or it must be 0ed. If last_executed_at is loaded
                  then an event can be scheduled for execution
                  instantly. Let's say an event has to be executed
                  every 15 mins. The server has been stopped for
                  more than this time and then started. If L_E_AT
                  is loaded from DB, execution at L_E_AT+15min
                  will be scheduled. However this time is in the past.
                  Hence immediate execution. Due to patch of
                  ::mark_last_executed() m_last_executed gets time_now
                  and not m_execute_at. If not like this a big
                  queue can be scheduled for times which are still in
                  the past (2, 3 and more executions which will be
                  consequent).
  */
  set_zero_time(&m_last_executed, MYSQL_TIMESTAMP_DATETIME);
#ifdef ANDREY_0
  table->field[EVEX_FIELD_LAST_EXECUTED]->
                     get_date(&et->m_last_executed, TIME_NO_ZERO_DATE);
#endif
  m_last_executed_changed= false;

  // ToDo : Andrey . Find a way not to allocate ptr on event_mem_root
  if ((ptr= get_field(mem_root, table->field[EVEX_FIELD_STATUS])) == NullS)
    goto error;
  
  DBUG_PRINT("load_from_row", ("Event [%s] is [%s]", et->m_name.str, ptr));
  et->m_status= (ptr[0]=='E'? MYSQL_EVENT_ENABLED:
                                     MYSQL_EVENT_DISABLED);

  // ToDo : Andrey . Find a way not to allocate ptr on event_mem_root
  if ((ptr= get_field(mem_root,
                  table->field[EVEX_FIELD_ON_COMPLETION])) == NullS)
    goto error;

  et->m_on_completion= (ptr[0]=='D'? MYSQL_EVENT_ON_COMPLETION_DROP:
                                     MYSQL_EVENT_ON_COMPLETION_PRESERVE);

  et->m_comment.str= get_field(mem_root, table->field[EVEX_FIELD_COMMENT]);
  if (et->m_comment.str != NullS)
    et->m_comment.length= strlen(et->m_comment.str);
  else
    et->m_comment.length= 0;
    
  DBUG_RETURN(0);
error:
  DBUG_RETURN(EVEX_GET_FIELD_FAILED);
}


bool
event_timed::compute_next_execution_time()
{
  TIME time_now;
  my_time_t now;
  int tmp;

  DBUG_ENTER("event_timed::compute_next_execution_time");

  if (m_status == MYSQL_EVENT_DISABLED)
  {
    DBUG_PRINT("compute_next_execution_time",
                  ("Event %s is DISABLED", m_name.str));
    goto ret;
  }
  //if one-time no need to do computation
  if (!m_expr)
  {
    //let's check whether it was executed
    if (m_last_executed.year)
    {
      DBUG_PRINT("compute_next_execution_time",
                ("One-time event %s was already executed", m_name.str));
      if (m_on_completion == MYSQL_EVENT_ON_COMPLETION_DROP)
      {
        DBUG_PRINT("compute_next_execution_time",
                          ("One-time event will be dropped."));
        m_dropped= true;
      }
      m_status= MYSQL_EVENT_DISABLED;
      m_status_changed= true;
    }
    goto ret;
  }
  time(&now);
  my_tz_UTC->gmt_sec_to_TIME(&time_now, now);
/*
  sql_print_information("[%s.%s]", m_db.str, m_name.str);
  sql_print_information("time_now : [%d-%d-%d %d:%d:%d ]", time_now.year, time_now.month, time_now.day, time_now.hour, time_now.minute, time_now.second);
  sql_print_information("m_starts : [%d-%d-%d %d:%d:%d ]", m_starts.year, m_starts.month, m_starts.day, m_starts.hour, m_starts.minute, m_starts.second);
  sql_print_information("m_ends   : [%d-%d-%d %d:%d:%d ]", m_ends.year, m_ends.month, m_ends.day, m_ends.hour, m_ends.minute, m_ends.second);
  sql_print_information("m_last_ex: [%d-%d-%d %d:%d:%d ]", m_last_executed.year, m_last_executed.month, m_last_executed.day, m_last_executed.hour, m_last_executed.minute, m_last_executed.second);
*/
  //if time_now is after m_ends don't execute anymore
  if (m_ends.year && (tmp= my_time_compare(&m_ends, &time_now)) == -1)
  {
    // time_now is after m_ends. don't execute anymore
    set_zero_time(&m_execute_at, MYSQL_TIMESTAMP_DATETIME);
    if (m_on_completion == MYSQL_EVENT_ON_COMPLETION_DROP)
      m_dropped= true;
    m_status= MYSQL_EVENT_DISABLED;
    m_status_changed= true;

    goto ret;
  }
  
  /* 
     Here time_now is before or equals m_ends if the latter is set.
     Let's check whether time_now is before m_starts.
     If so schedule for m_starts
  */
  if (m_starts.year && (tmp= my_time_compare(&time_now, &m_starts)) < 1)
  {
    if (tmp == 0 && my_time_compare(&m_starts, &m_last_executed) == 0)
    {
       /*
        time_now = m_starts = m_last_executed
        do nothing or we will schedule for second time execution at m_starts.
      */
    }
    else
    {
      //m_starts is in the future
      //time_now before m_starts. Scheduling for m_starts
      m_execute_at= m_starts;
      goto ret;
    }
  }
  
  if (m_starts.year && m_ends.year)
  {
    /* 
      Both m_starts and m_ends are set and time_now is between them (incl.)
      If m_last_executed is set then increase with m_expr. The new TIME is
      after m_ends set m_execute_at to 0. And check for m_on_completion
      If not set then schedule for now.
    */
    if (!m_last_executed.year)
      m_execute_at= time_now;
    else
    {
      my_time_t last, ll_ends;

      // There was previous execution     
      last= sec_since_epoch_TIME(&m_last_executed) + m_expr;
      ll_ends= sec_since_epoch_TIME(&m_ends);
      //now convert back to TIME
      //ToDo Andrey: maybe check for error here?
      if (ll_ends < last)
      {
        // Next execution after ends. No more executions
        set_zero_time(&m_execute_at, MYSQL_TIMESTAMP_DATETIME);
        if (m_on_completion == MYSQL_EVENT_ON_COMPLETION_DROP)
          m_dropped= true;
      }
      else
        my_tz_UTC->gmt_sec_to_TIME(&m_execute_at, last);
    }
    goto ret;
  }
  else if (!m_starts.year && !m_ends.year)
  {
    // both m_starts and m_ends are not set, se we schedule for the next
    // based on m_last_executed
    if (!m_last_executed.year)
       //m_last_executed not set. Schedule the event for now
      m_execute_at= time_now;
    else
      //ToDo Andrey: maybe check for error here?
      my_tz_UTC->gmt_sec_to_TIME(&m_execute_at, 
                   sec_since_epoch_TIME(&m_last_executed) + m_expr);
    goto ret;
  }
  else
  {
    //either m_starts or m_ends is set
    if (m_starts.year)
    {
      /*
        - m_starts is set.
        - m_starts is not in the future according to check made before
        Hence schedule for m_starts + m_expr in case m_last_executed
        is not set, otherwise to m_last_executed + m_expr
      */
      my_time_t last;

      //convert either m_last_executed or m_starts to seconds
      if (m_last_executed.year)
        last= sec_since_epoch_TIME(&m_last_executed) + m_expr;
      else
        last= sec_since_epoch_TIME(&m_starts);

      //now convert back to TIME
      //ToDo Andrey: maybe check for error here?
      my_tz_UTC->gmt_sec_to_TIME(&m_execute_at, last);
    }
    else
    {
      /*
        - m_ends is set
        - m_ends is after time_now or is equal
        Hence check for m_last_execute and increment with m_expr.
        If m_last_executed is not set then schedule for now
      */
      my_time_t last, ll_ends;

      if (!m_last_executed.year)
        m_execute_at= time_now;
      else
      {
        last= sec_since_epoch_TIME(&m_last_executed);
        ll_ends= sec_since_epoch_TIME(&m_ends);
        last+= m_expr;
        //now convert back to TIME
        //ToDo Andrey: maybe check for error here?
        if (ll_ends < last)
        {
          set_zero_time(&m_execute_at, MYSQL_TIMESTAMP_DATETIME);
          if (m_on_completion == MYSQL_EVENT_ON_COMPLETION_DROP)
            m_dropped= true;
        }
        else
          my_tz_UTC->gmt_sec_to_TIME(&m_execute_at, last);
      }
    }
    goto ret;
  }
ret:

  DBUG_RETURN(false);
}


void
event_timed::mark_last_executed()
{
  TIME time_now;
  my_time_t now;

  time(&now);
  my_tz_UTC->gmt_sec_to_TIME(&time_now, now);

  m_last_executed= time_now; // was m_execute_at
#ifdef ANDREY_0
  m_last_executed= m_execute_at;
#endif
  m_last_executed_changed= true;
}


bool
event_timed::drop(THD *thd)
{

  return (bool) evex_drop_event(thd, this, false);
}


bool
event_timed::update_fields(THD *thd)
{
  TABLE *table;
  int ret= 0;
  bool opened;

  DBUG_ENTER("event_timed::update_time_fields");

  DBUG_PRINT("enter", ("name: %*s", m_name.length, m_name.str));
 
  //no need to update if nothing has changed
  if (!(m_status_changed || m_last_executed_changed))
    goto done;
  
  if (!(table= evex_open_event_table(thd, TL_WRITE)))
    DBUG_RETURN(SP_OPEN_TABLE_FAILED);

  if ((ret= sp_db_find_routine_aux(thd, 0/*notype*/, m_db, m_name, table)))
    goto done;

  store_record(table,record[1]);
  table->timestamp_field_type= TIMESTAMP_NO_AUTO_SET; // Don't update create on row update.

  if (m_last_executed_changed)
  {
    table->field[EVEX_FIELD_LAST_EXECUTED]->set_notnull();
    table->field[EVEX_FIELD_LAST_EXECUTED]->store_time(&m_last_executed,
                           MYSQL_TIMESTAMP_DATETIME);
    m_last_executed_changed= false;
  }
  if (m_status_changed)
  {
    table->field[EVEX_FIELD_STATUS]->set_notnull();
    table->field[EVEX_FIELD_STATUS]->store((longlong)m_status);
    m_status_changed= false;
  }
    
  if ((table->file->update_row(table->record[1],table->record[0])))
    ret= EVEX_WRITE_ROW_FAILED;

done:
  close_thread_tables(thd);

  DBUG_RETURN(ret);
}


char *
event_timed::get_show_create_event(THD *thd, uint *length)
{
  char *dst, *ret;
  uint len, tmp_len;

  len = strlen("CREATE EVENT ") + m_db.length + strlen(".") + m_name.length +
        strlen(" ON SCHEDULE ") + strlen("EVERY 5 MINUTE ")
/*
	+ strlen("ON COMPLETION ")
	+ (m_on_completion==MYSQL_EVENT_ON_COMPLETION_DROP?
		         strlen("NOT PRESERVE "):strlen("PRESERVE "))
	+ (m_status==MYSQL_EVENT_ENABLED?
		         strlen("ENABLE "):strlen("DISABLE "))
	+ strlen("COMMENT \"") + m_comment.length + strlen("\" ")
*/
    + strlen("DO ") +
	+ m_body.length + strlen(";");
  
  ret= dst= (char*) alloc_root(thd->mem_root, len);
  memcpy(dst, "CREATE EVENT ", tmp_len= strlen("CREATE EVENT "));
  dst+= tmp_len;
  memcpy(dst, m_db.str, tmp_len=m_db.length);
  dst+= tmp_len;
  memcpy(dst, ".", tmp_len= strlen("."));
  dst+= tmp_len;
  memcpy(dst, m_name.str, tmp_len= m_name.length);
  dst+= tmp_len;
  memcpy(dst, " ON SCHEDULE ", tmp_len= strlen(" ON SCHEDULE "));
  dst+= tmp_len;
  memcpy(dst, "EVERY 5 MINUTE ", tmp_len= strlen("EVERY 5 MINUTE "));
  dst+= tmp_len;
/*
  memcpy(dst, "ON COMPLETION ", tmp_len =strlen("ON COMPLETION "));
  dst+= tmp_len;
  memcpy(dst, (m_on_completion==MYSQL_EVENT_ON_COMPLETION_DROP?
		         "NOT PRESERVE ":"PRESERVE "),
			 tmp_len =(m_on_completion==MYSQL_EVENT_ON_COMPLETION_DROP? 13:9));
  dst+= tmp_len;

  memcpy(dst, (m_status==MYSQL_EVENT_ENABLED?
		         "ENABLE  ":"DISABLE  "),
			 tmp_len= (m_status==MYSQL_EVENT_ENABLED? 8:9));
  dst+=tmp_len;

  memcpy(dst, "COMMENT \"", tmp_len= strlen("COMMENT \""));
  dst+= tmp_len;
  memcpy(dst, m_comment.str, tmp_len= m_comment.length);
  dst+= tmp_len;
  memcpy(dst, "\" ", tmp_len=2);
  dst+= tmp_len;
*/
  memcpy(dst, "DO ", tmp_len=3);
  dst+= tmp_len;

  memcpy(dst, m_body.str, tmp_len= m_body.length);
  dst+= tmp_len;
  memcpy(dst, ";", 1);
  ++dst;
  *dst= '\0';

  *length= len;
  
  return ret;
}


/*
   Executes the event (the underlying sp_head object);

   SYNOPSIS
     evex_fill_row()
       thd    THD
       mem_root  If != NULL use it to compile the event on it

   Returns 
          0  - success
       -100  - event in execution (parallel execution is impossible)
      others - retcodes of sp_head::execute_procedure()
      
*/

int
event_timed::execute(THD *thd, MEM_ROOT *mem_root)
{
  List<Item> empty_item_list;
  int ret= 0;
   
  DBUG_ENTER("event_timed::execute");

  VOID(pthread_mutex_lock(&LOCK_running));
  if (running) 
  {
    VOID(pthread_mutex_unlock(&LOCK_running));
    DBUG_RETURN(-100);
  }
  running= true;
  VOID(pthread_mutex_unlock(&LOCK_running));

  // TODO Andrey : make this as member variable and delete in destructor
  empty_item_list.empty();
  
  if (!m_sphead && (ret= compile(thd, mem_root)))
    goto done;
  
  ret= m_sphead->execute_procedure(thd, &empty_item_list);

  VOID(pthread_mutex_lock(&LOCK_running));
  running= false;
  VOID(pthread_mutex_unlock(&LOCK_running));

done:
  // Don't cache m_sphead if allocated on another mem_root
  if (mem_root && m_sphead)
  {
    delete m_sphead;
    m_sphead= 0;
  }

  DBUG_RETURN(ret);
}


int
event_timed::compile(THD *thd, MEM_ROOT *mem_root)
{
  MEM_ROOT *tmp_mem_root= 0;
  LEX *old_lex= thd->lex, lex;
  char *old_db;
  event_timed *ett;
  sp_name *spn;
  char *old_query;
  uint old_query_len;
  st_sp_chistics *p;
  
  DBUG_ENTER("event_timed::compile");
  // change the memory root for the execution time
  if (mem_root)
  {
    tmp_mem_root= thd->mem_root;
    thd->mem_root= mem_root;
  }
  old_query_len= thd->query_length;
  old_query= thd->query;
  old_db= thd->db;
  thd->db= m_db.str;
  thd->query= get_show_create_event(thd, &thd->query_length);
  DBUG_PRINT("event_timed::compile", ("query:%s",thd->query));

  thd->lex= &lex;
  lex_start(thd, (uchar*)thd->query, thd->query_length);
  lex.et_compile_phase= TRUE;
  if (yyparse((void *)thd) || thd->is_fatal_error)
  {
    //  Free lex associated resources
    //  QQ: Do we really need all this stuff here ?
    if (lex.sphead)
    {
      if (&lex != thd->lex)
        thd->lex->sphead->restore_lex(thd);
      delete lex.sphead;
      lex.sphead= 0;
    }
    // QQ: anything else ?
    lex_end(&lex);
    thd->lex= old_lex;
    DBUG_RETURN(-1);
  }
  
  m_sphead= lex.sphead;
  m_sphead->m_db= m_db;
  //copy also chistics since they will vanish otherwise we get 0x0 pointer
  // Todo : Handle sql_mode !!
  m_sphead->set_definer(m_definer.str,  m_definer.length);
  m_sphead->set_info(0, 0, &lex.sp_chistics, 0/*sql_mode*/);
  m_sphead->optimize();
  lex_end(&lex);
  thd->lex= old_lex;
  thd->query= old_query;
  thd->query_length= old_query_len;
  thd->db= old_db;
  /*
    Change the memory root for the execution time.
  */
  if (mem_root)
    thd->mem_root= tmp_mem_root;

  DBUG_RETURN(0);
}

