# Facebook MySQL has slave_exec_mode=SEMI_STRICT which allows a slave to
# 'absorb' or ignore key not found errors. Percona Server does not.
# This test has been modified to only test that the slave SQL thread stops on
# error as it should when running with the default slave_exec_mode=STRICT
--source include/have_myisam.inc
--source include/have_rocksdb.inc
--source include/have_debug.inc
--source include/have_debug_sync.inc
--source include/rpl/init_source_replica.inc

connection slave;
call mtr.add_suppression("Replica SQL for channel '': Worker.*failed executing transaction");
call mtr.add_suppression("The replica coordinator and worker threads are stopped, possibly leaving data in inconsistent state");
connection master;

create table t0 (a int) engine=myisam;
insert into t0 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);
create table t1(a int) engine=myisam;
insert into t1 select A.a + B.a* 10 + C.a * 100 from t0 A, t0 B, t0 C;
create table t2 (
  pk int primary key,
  kp1 int,
  kp2 int,
  col1 int,
  key (kp1,kp2)
) engine=rocksdb;
insert into t2 select a,a,a,a from t1;
create table t3 like t2;
set @orig_transaction_isolation=@@session.transaction_isolation;
set session transaction_isolation='READ-COMMITTED';
insert into t3 select * from t2;
set session transaction_isolation=@orig_transaction_isolation;

--source include/rpl/sync_to_replica.inc

connection slave;
let $old_debug = `select @@global.debug`;
set global debug= 'd,dbug.rocksdb.get_row_by_rowid';
--source include/rpl/stop_replica.inc
--source include/rpl/start_replica.inc

connection master;
update t2 set col1=100 where kp1 between 1 and 3 and mod(kp2,2)=0;

connection slave;
set debug_sync= 'now WAIT_FOR Reached';
eval set global debug = '$old_debug';
set sql_log_bin=0;
delete from t2 where pk=2;
delete from t2 where pk=3;
set debug_sync= 'now SIGNAL signal.rocksdb.get_row_by_rowid_let_running';

# Error is found as expected.
# Do NOT show slave sql error as it will contain test specific binlog coords
let $show_slave_sql_error = 0;
--source include/rpl/wait_for_applier_error.inc

# fix up the table so we can easily let slave to continue
set sql_log_bin=0;
insert into t2 select a,a,a,a from t1 where a in (2,3);
--source include/rpl/start_replica.inc

connection master;
--source include/rpl/sync_to_replica.inc

# Cleanup
connection master;
drop table t0, t1, t2, t3;
--source include/rpl/deinit.inc
