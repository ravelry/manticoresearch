# Binary logging

Binary logs are essentially a recovery mechanism for [Real-Time](../Creating_an_index/Local_indexes/Real-time_index.md) index data and also of attributes updates of plain indexes that would otherwise only be stored in RAM until flush. With binary logs enabled, ``searchd`` writes every given transaction to the binlog file, and uses that for recovery after an unclean shutdown. On clean shutdown, RAM chunks are saved to disk, and then all the binlog files are unlinked.

## Enabling binary logging

In [RT mode](../Creating_an_index/Local_indexes.md#Online-schema-management-%28RT-mode%29) binary logging is enabled by default and the binary log files are written inside the `data_dir` folder.

Binary logging can be disabled by setting `binlog_path` to empty:

```ini
searchd {
...
    binlog_path = # disable logging
...
```
Disabling binary logging improves performance for Real-Time indexes, but puts their data at risk.

The directive can be used to set a custom path:

```ini
searchd {
...
    binlog_path = /var/data
...
```

In Plain mode it is recommended to be explicitly defined. Otherwise, the default path, which in most cases is the same as working folder, may point to the folder with no write access (for example, /usr/local/var/data). In this case, the searchd will not start at all.

## Operations

When logging is enabled, every transaction committed  into RT index gets written into a log file. Logs are then automatically replayed on startup after an unclean shutdown, recovering the logged changes.

### Log size
During normal operation, a new binlog file will be opened every time when ``binlog_max_log_size`` limit is reached. Older, already closed binlog files are kept until all of the transactions stored in them (from all indexes) are flushed as a disk chunk. Setting the limit to 0 pretty much prevents binlog from being unlinked at all while ``searchd`` is running; however, it will still be unlinked on clean shutdown. By default, there is no limit of the log file size. 

```ini
binlog_max_log_size = 16M
```

### Binary flushing strategies

There are 3 different binlog flushing strategies, controlled by directive `binlog_flush`:
 
* 0, flush and sync every second. Best performance, but up to 1 second worth of committed transactions can be lost both on server crash, or OS/hardware crash.
* 1, flush and sync every transaction. Worst performance, but every committed transaction data is guaranteed to be saved.
* 2, flush every transaction, sync every second. Good performance, and every committed transaction is guaranteed to be saved in case of server crash. However, in case of OS/hardware crash up to 1 second worth of committed transactions can be lost.

For those familiar with MySQL and InnoDB, this directive is entirely similar to innodb_flush_log_at_trx_commit. Default mode is flush every transaction, sync every second (mode 2).

```ini
searchd {
...
    binlog_flush = 1 # ultimate safety, low speed
...
}
```

### Recovery

On recovery after an unclean shutdown, binlogs are replayed and all logged transactions since the last good on-disk state are restored. Transactions are checksummed so in case of binlog file corruption garbage data will **not** be replayed; such a broken transaction will be detected and will stop replay. Transactions also start with a magic marker and timestamped, so in case of binlog damage in the middle of the file, it is technically possible to skip broken  transactions and keep replaying from the next good one, and/or it is possible to replay transactions until a given timestamp (point-in-time recovery), but none of that is implemented yet.


### Flushing RT RAM chunks

Intensive updating of a small RT index that fully fits into a RAM chunk will lead to an ever-growing binlog that can never be unlinked until clean shutdown. Binlogs are essentially append-only deltas against the last known good saved state on disk, and unless RAM chunk gets saved, they can not be unlinked. An ever-growing binlog is not very good for disk use and crash recovery time. To avoid this, you can configure ``searchd`` to perform a periodic RAM chunk flush to fix that problem using `rt_flush_period`directive. With periodic flushes enabled, ``searchd`` will keep a separate thread, checking whether RT indexes RAM chunks need to be written back to disk. Once that happens, the respective binlogs can be (and are) safely unlinked.

```ini
searchd {
...
    rt_flush_period = 3600 # 1 hour
...
}
```
By default the RT flush period is set to 10 hours.

Note that ``rt_flush_period`` only controls the frequency at which the *checks* happen. There are no *guarantees* that the particular RAM chunk will get saved. For instance, it does not make sense to regularly re-save a huge RAM chunk that only gets a few rows worth of updates. The search server determine whether to actually perform the flush with a few heuristics.
