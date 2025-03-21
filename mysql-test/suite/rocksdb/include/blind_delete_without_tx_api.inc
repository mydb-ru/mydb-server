--source include/have_rocksdb.inc
--source include/rpl/init_source_replica.inc

--eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation
set @save_rocksdb_blind_delete_primary_key=@@session.rocksdb_blind_delete_primary_key;
set @save_rocksdb_master_skip_tx_api=@@session.rocksdb_master_skip_tx_api;

create table t1 (id int primary key, value int, value2 varchar(200)) Engine=RocksDB;
create table t2 (id int primary key, value int, value2 varchar(200), index(value)) Engine=RocksDB;

--disable_query_log
let $t = 1;
while ($t <= 2) {
  let $i = 1;
  while ($i <= 10000) {
    let $insert = INSERT INTO t$t VALUES($i, $i, REPEAT('x', 150));
    inc $i;
    eval $insert;
  }
  inc $t;
}
--enable_query_log

SET session rocksdb_blind_delete_primary_key=1;
select variable_value into @c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
# Deleting 1000 rows from t1
--disable_query_log
let $i = 1;
while ($i <= 1000) {
  let $insert = DELETE FROM t1 WHERE id=$i;
  inc $i;
  eval $insert;
}
--enable_query_log
select variable_value-@c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
SELECT count(*) FROM t1;

--source include/rpl/sync_to_replica.inc
--source include/rpl/connection_replica.inc
--eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation
SELECT count(*) FROM t1;
--source include/rpl/connection_source.inc

# Deleting 1000 rows from t2 (blind delete disabled because of secondary key)
select variable_value into @c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
--disable_query_log
let $i = 1;
while ($i <= 1000) {
  let $insert = DELETE FROM t2 WHERE id=$i;
  inc $i;
  eval $insert;
}
--enable_query_log
select variable_value-@c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
SELECT count(*) FROM t2;

SET session rocksdb_master_skip_tx_api=1;

select variable_value into @c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
--disable_query_log
let $t = 1;
while ($t <= 2) {
  let $i = 1001;
  while ($i <= 2000) {
    let $insert = DELETE FROM t$t WHERE id=$i;
    inc $i;
    eval $insert;
  }
  inc $t;
}
--enable_query_log
select variable_value-@c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
SELECT count(*) FROM t1;
SELECT count(*) FROM t2;
--source include/rpl/sync_to_replica.inc
--source include/rpl/connection_replica.inc
SELECT count(*) FROM t1;
SELECT count(*) FROM t2;
--source include/rpl/connection_source.inc


# Range Deletes (blind delete disabled)
select variable_value into @c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
DELETE FROM t1 WHERE id BETWEEN 3001 AND 4000;
DELETE FROM t2 WHERE id BETWEEN 3001 AND 4000;
select variable_value-@c from performance_schema.global_status where variable_name='rocksdb_rows_deleted_blind';
SELECT count(*) FROM t1;
SELECT count(*) FROM t2;
--source include/rpl/sync_to_replica.inc
--source include/rpl/connection_replica.inc
SELECT count(*) FROM t1;
SELECT count(*) FROM t2;
--source include/rpl/connection_source.inc


# Deleting same keys (slaves stop)
DELETE FROM t1 WHERE id = 10;
SELECT count(*) FROM t1;
--source include/rpl/connection_replica.inc
call mtr.add_suppression("Replica SQL.*Could not execute Delete_rows event on table test.t1.*Error_code.*");
call mtr.add_suppression("Replica: Can't find record in 't1'.*");
call mtr.add_suppression("The replica coordinator and worker threads are stopped, possibly leaving data in inconsistent state");
# wait until we have the expected error
--let $slave_sql_errno= convert_error(ER_KEY_NOT_FOUND)
--source include/rpl/wait_for_applier_error.inc

--source include/rpl/connection_replica.inc
set @save_rocksdb_read_free_rpl=@@global.rocksdb_read_free_rpl;
set global rocksdb_read_free_rpl=PK_SK;
--source include/rpl/start_replica.inc
--source include/rpl/connection_source.inc
--source include/rpl/sync_to_replica.inc
--source include/rpl/connection_replica.inc
SELECT count(*) FROM t1;
--source include/rpl/connection_source.inc


# cleanup
--source include/rpl/connection_replica.inc
set global rocksdb_read_free_rpl=@save_rocksdb_read_free_rpl;
--source include/rpl/connection_source.inc
SET session rocksdb_blind_delete_primary_key=@save_rocksdb_blind_delete_primary_key;
SET session rocksdb_master_skip_tx_api=@save_rocksdb_master_skip_tx_api;

DROP TABLE t1, t2;
--source include/rpl/deinit.inc
