# Merging indexes

Merging two existing **plain** indexes can be more efficient than indexing the data from scratch and desired in some cases (such as merging 'main' and 'delta' indexes instead of simply reindexing 'main' in the 'main+delta' partitioning scheme). So `indexer` has an option to do that. Merging indexes is normally faster than reindexing, but still **not** instant on huge indexes. Basically, it will need to read the contents of the both indexes once and write the result once. Merging 100 GB and 1 GB index, for example, will result in 202 GB of I/O (but that's still likely less than the indexing from scratch requires).

The basic command syntax is as follows:

```bash
sudo -u manticore indexer --merge DSTINDEX SRCINDEX [--rotate] [--drop-src]
```

Unless `--drop-src` is specified only the DSTINDEX index will be affected: the contents of SRCINDEX will be merged into it.

`--rotate` switch will be required if DSTINDEX is already being served by `searchd`.

The typical usage pattern is to merge a smaller update from SRCINDEX into DSTINDEX. Thus, when merging attributes the values from SRCINDEX will win if duplicate document IDs are encountered. Note, however, that the "old" keywords will **not** be automatically removed in such cases. For example, if there's a keyword "old" associated with document 123 in DSTINDEX, and a keyword "new" associated with it in SRCINDEX, document 123 will be found by *both* keywords after the merge. You can supply an explicit condition to remove documents from DSTINDEX to mitigate that; the relevant switch is `--merge-dst-range`:

```bash
sudo -u manticore indexer --merge main delta --merge-dst-range deleted 0 0
```

This switch lets you apply filters to the destination index along with merging. There can be several filters; all of their conditions must be met in order to include the document in the resulting merged index. In the example above, the filter passes only those records where 'deleted' is 0, eliminating all records that were flagged as deleted.

`--drop-src` allows dropping SRCINDEX after the merge and before rotating the indexes, which is important in case you specify DSTINDEX in `killlist_target` of DSTINDEX, otherwise when rotating the indexes the documents that have been merged into DSTINDEX may be suppressed by SRCINDEX.
