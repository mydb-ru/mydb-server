# Run the load generator to populate the table and generate concurrent
# updates.  After the load generator is complete, verify the tables on the
# master and the slave are consistent
--sync_slave_with_master

--connection master
--let $master_host = 127.0.0.1

CREATE USER 'rocksdb_stress_test'@'localhost' IDENTIFIED WITH 'caching_sha2_password';
GRANT ALL ON *.* TO 'rocksdb_stress_test'@'localhost';

let $exec =
  python $MYSQL_TEST_DIR/suite/rocksdb_stress/t/load_generator.py
  -L $MYSQL_TMP_DIR/load_generator.log -H $master_host -t $table
  -P $MASTER_MYPORT -n $num_records -m $max_records
  -l $num_loaders -c $num_checkers -r $num_requests
  -E $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  -D $reap_delay -u rocksdb_stress_test;

exec $exec;

--exec rm $MYSQL_TMP_DIR/load_generator.log

source include/wait_until_connected_again.inc;

--let $master_checksum = query_get_value(CHECKSUM TABLE $table, Checksum, 1)

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

--connection slave
--let $slave_checksum = query_get_value(CHECKSUM TABLE $table, Checksum, 1)

let $not_same = `SELECT $master_checksum-$slave_checksum`;
if ($not_same)
{
  --die "The checksums of table $table for the master and slave do not match!"
}

# Cleanup
--connection master
DROP USER 'rocksdb_stress_test'@'localhost';
--let $cleanup = DROP TABLE $table
eval $cleanup;

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

--connection slave
--source include/rpl/stop_replica.inc
# For stress tests sometimes the replication thread can not connect to master
# temporarily. This is either because the master crashed and it is recovering
# or the master is too busy and could not service the slave's requests.
# mtr's internal check requires that there be no errors in slave status.
# restarting replication clears the errors.
--source include/rpl/start_replica.inc
--source include/rpl/stop_replica.inc
--connection master
--source include/rpl/stop_dump_threads.inc
