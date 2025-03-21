# ==== Purpose ====
#
# Auxiliary file used in include/rpl/deprecated/show_binlog_events.inc and
# include/rpl/deprecated/show_relaylog_events.inc.
#
# ==== Usage ====
#
# See include/rpl/deprecated/show_binlog_events.inc

--let $include_filename= rpl/deprecated/show_events.inc
if ($is_relay_log)
{
  if ($rpl_channel_name)
  {
    --let $include_filename= $include_filename [FOR CHANNEL $rpl_channel_name]
  }
}
--source include/begin_include_file.inc

--let $_se_old_statement= $statement

--let $statement= SHOW BINLOG EVENTS
--let $_se_for_channel=
if ($is_relay_log)
{
  --let $statement= SHOW RELAYLOG EVENTS
  if ($rpl_channel_name)
  {
    --let $_se_for_channel= FOR CHANNEL $rpl_channel_name
  }
}
--let $statement_masked= $statement

if ($binlog_file)
{
  --let $_binlog_file= $binlog_file
  if ($binlog_file == 'LAST')
  {
    if ($is_relay_log)
    {
      --source include/rpl/save_relay_log_file_position.inc
      --let $_binlog_file= $relay_log_file
    }
    if (!$is_relay_log)
    {
      --let $_binlog_file= query_get_value(SHOW BINARY LOG STATUS, File, 1)
    }
  }
  --let $statement= $statement IN '$_binlog_file'
  --let $statement_masked= $statement_masked IN <FILE>
}

--let $_binlog_start= $binlog_start

if (!$_binlog_start)
{
  # If $binlog_start is not set, we will set it as the second event's
  # position.  The first event(Description Event) is always ignored. For
  # description event's length might be changed because of adding new events,
  # 'SHOW BINLOG EVENTS LIMIT 1' is used to get the right value.
  --let $_binlog_start= query_get_value($statement LIMIT 1 $_se_for_channel, End_log_pos, 1)
}

--let $statement= $statement FROM $_binlog_start
--let $statement_masked= $statement_masked FROM <POSITION>

if ($binlog_limit != '')
{
  --let $statement= $statement LIMIT $binlog_limit
  --let $statement_masked= $statement_masked LIMIT $binlog_limit
}

if ($show_binlog_events_verbose)
{
  --echo ---- [$CURRENT_CONNECTION] $statement_masked ----
}

if ($rpl_debug)
{
  --echo statement='$statement'
}

--let $statement= $statement $_se_for_channel

# Execute the statement and write to $output_file
--let $output_file= GENERATE
--source include/write_result_to_file.inc

# Filter the file through the following script.
--delimiter ||
if ($show_binlog_events_mask_columns != '')
{
  --let $mask_columns = $show_binlog_events_mask_columns
}
if ($show_binlog_events_mask_columns == '')
{
  --let $mask_columns = 2,4,5
  if ($keep_server_id)
  {
    --let $mask_columns= 2, 5
  }
}
let $script=
  s{/\* xid=.* \*/}{/\* XID # \*/};
  s{COMMIT /\* XID # \*/}{COMMIT /\* XID \*/};
  s{table_id: [0-9]+}{table_id: #};
  s{file_id=[0-9]+}{file_id=#};
  s{block_len=[0-9]+}{block_len=#};
  s{Server ver:.*DOLLAR}{SERVER_VERSION, BINLOG_VERSION};
  s{SQL_LOAD-[a-z,0-9,-]*.[a-z]*}{SQL_LOAD-<SERVER UUID>-<MASTER server-id>-<file-id>.<extension>};
  s{rand_seed1=[0-9]*,rand_seed2=[0-9]*}{rand_seed1=<seed 1>,rand_seed2=<seed 2>};
  s{((?:master|slave|slave-relay)-bin\.[0-9]{6};pos=)[0-9]+DOLLAR}{DOLLAR1POS};
  s{(binlog\.[0-9]{6};pos=)[0-9]+DOLLAR}{DOLLAR1POS};
  s{SONAME ".*"}{SONAME "LIB"};
  s{DOLLARmysqltest_vardir}{MYSQLTEST_VARDIR}g;
||
--let $pre_script= my DOLLARmysqltest_vardir = DOLLARENV{'MYSQLTEST_VARDIR'};

if (!$keep_transaction_payload_events)
{
  --let $script= $script DOLLAR_ = '' if (m{\t(?:Transaction_payload)\t});
}
if ($keep_transaction_payload_events)
{
  let $pre_script= $pre_script
    my DOLLARdec_size_regex = '(decompressed_size=[0-9]+ bytes)';
  ||
  --let $script= $script s{DOLLARdec_size_regex}{decompressed_size=## bytes};
}

if (!$keep_view_change_events)
{
  --let $script= $script DOLLAR_ = '' if (m{\t(?:View_change)\t});
}

if (!$keep_gtid_events)
{
  --let $script= $script DOLLAR_ = '' if (m{\t(?:Gtid|Previous_gtids|Anonymous_Gtid)\t});
}
if ($keep_gtid_events)
{
  let $pre_script= $pre_script
    my DOLLARuuid_regex = '[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}';
    my DOLLARuuid_gno_regex = DOLLARuuid_regex.'(?::[0-9]+(?:-[0-9]+)?)+';
  ||
  --let $script= $script s{DOLLARuuid_gno_regex(?:,DOLLARuuid_gno_regex)*}{Gtid_set};
}
# To mask binlog and relaylog commit events
if ($mask_binlog_commit_events)
{
  --let $script= $script s{ \/\* XID \*\/}{};
  --let $script= $script s{Xid}{Query};
}
# To mask ANONYMOUS_GTID events as GTID events
if ($mask_anonymous_gtid_events)
{
  --let $script= $script s{Anonymous_Gtid}{Gtid};
  --let $script= $script s{ANONYMOUS}{Gtid_set};
}
# To mask random password generated with create/alter user
if ($mask_user_password_events)
{
 --let $script= $script s{'caching_sha2_password' AS '(.*)'}{'caching_sha2_password' AS '<non-deterministic-password-hash>'}g;
 --let $script= $script s{SET PASSWORD FOR .*}{SET PASSWORD FOR '<some_user>'='<non-deterministic-password-hash>'};
}
# Skip 'AS <..> WITH <..>' clause in the GRANT statement in case the
# partial revokes is ON 
if ($mask_grant_as_events)
{
 --let $script= $script s{ AS .* WITH ROLE .*}{};
}

if (!$keep_ddl_xid)
{
  --let $script= $script s{ /\* XID # \*/}{};
}
--delimiter ;

#--let $select_columns= 1 3 6
--let $input_file= $output_file
--source include/filter_file.inc

# Write to result file
--cat_file $output_file

# Remove the file
--remove_file $output_file

--let $statement= $_se_old_statement

--let $include_filename= rpl/deprecated/show_events.inc
--source include/end_include_file.inc
