# SHOW META

```sql
SHOW META [ LIKE pattern ]
```

<!-- example show meta -->
`SHOW META` is an SQL statement that shows additional meta-information about the latest query such as query time and keyword statistics. The syntax is:

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SELECT id,channel_id FROM records WHERE MATCH('one|two|three') limit 5;

SHOW META;
```

<!-- response SQL -->

```sql
+--------+----------------+
| id     | channel_id     |
+--------+----------------+
| 630768 | 1054702.000000 |
| 586645 | 1057204.000000 |
| 523391 | 1061514.000000 |
| 402383 | 1069381.000000 |
| 456106 | 1065936.000000 |
+--------+----------------+
5 rows in set (0.40 sec)

+---------------+--------+
| Variable_name | Value  |
+---------------+--------+
| total         | 1000   |
| total_found   | 311736 |
| time          | 0.407  |
| keyword[0]    | one    |
| docs[0]       | 265709 |
| hits[0]       | 538323 |
| keyword[1]    | two    |
| docs[1]       | 96044  |
| hits[1]       | 138576 |
| keyword[2]    | three  |
| docs[2]       | 43272  |
| hits[2]       | 69104  |
+---------------+--------+
12 rows in set (0.00 sec)
```

<!-- end -->

<!-- example show meta iostats cpustats -->
`SHOW META` can show IO and CPU counters, but they will only be available if searchd was started with `--iostats` and `--cpustats` switches respectively.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SELECT id,channel_id FROM records WHERE MATCH('one|two|three') limit 5;

SHOW META;
```

<!-- response SQL -->

```sql
+--------+----------------+
| id     | channel_id     |
+--------+----------------+
| 630768 | 1054702.000000 |
| 586645 | 1057204.000000 |
| 523391 | 1061514.000000 |
| 402383 | 1069381.000000 |
| 456106 | 1065936.000000 |
+--------+----------------+
5 rows in set (0.43 sec)

+-----------------------+---------+
| Variable_name         | Value   |
+-----------------------+---------+
| total                 | 1000    |
| total_found           | 311736  |
| time                  | 0.431   |
| cpu_time              | 431.096 |
| agents_cpu_time       | 0.000   |
| io_read_time          | 0.000   |
| io_read_ops           | 0       |
| io_read_kbytes        | 0.0     |
| io_write_time         | 0.000   |
| io_write_ops          | 0       |
| io_write_kbytes       | 0.0     |
| agent_io_read_time    | 0.000   |
| agent_io_read_ops     | 0       |
| agent_io_read_kbytes  | 0.0     |
| agent_io_write_time   | 0.000   |
| agent_io_write_ops    | 0       |
| agent_io_write_kbytes | 0.0     |
| keyword[0]            | one     |
| docs[0]               | 265709  |
| hits[0]               | 538323  |
| keyword[1]            | two     |
| docs[1]               | 96044   |
| hits[1]               | 138576  |
| keyword[2]            | three   |
| docs[2]               | 43272   |
| hits[2]               | 69104   |
+-----------------------+---------+
26 rows in set (0.00 sec)
```

<!-- end -->

<!-- example show meta predicted_time -->
Additional `predicted_time`, `dist_predicted_time`, `local_fetched_docs`, `local_fetched_hits`, `local_fetched_skips` and their respective `dist_fetched_*` counterparts will only be available if searchd was configured with [predicted time costs](../Server_settings/Searchd.md#predicted_time_costs) and query had `predicted_time` in the `OPTION` clause.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SELECT id,channel_id FROM records WHERE MATCH('one|two|three') limit 5 option max_predicted_time=100;

SHOW META;
```

<!-- response SQL -->

```sql
+--------+----------------+
| id     | channel_id     |
+--------+----------------+
| 630768 | 1054702.000000 |
| 586645 | 1057204.000000 |
| 523391 | 1061514.000000 |
| 402383 | 1069381.000000 |
| 456106 | 1065936.000000 |
+--------+----------------+
5 rows in set (0.41 sec)

+---------------------+--------+
| Variable_name       | Value  |
+---------------------+--------+
| total               | 1000   |
| total_found         | 311736 |
| time                | 0.405  |
| local_fetched_docs  | 405025 |
| local_fetched_hits  | 746003 |
| local_fetched_skips | 0      |
| predicted_time      | 81     |
| keyword[0]          | one    |
| docs[0]             | 265709 |
| hits[0]             | 538323 |
| keyword[1]          | two    |
| docs[1]             | 96044  |
| hits[1]             | 138576 |
| keyword[2]          | three  |
| docs[2]             | 43272  |
| hits[2]             | 69104  |
+---------------------+--------+
16 rows in set (0.00 sec)
```

<!-- end -->

<!-- example show meta single statement -->

`SHOW META` needs to run right after the query was executed in the **same** session. As some mysql connectors/libraries use connection pools, running `SHOW META` in a separate statement  an lead to unexpected results like getting meta from another query. In these cases (and recommended in general) is to run a multiple statement containing query + `SHOW META`. Some connectors/libraries support o multi-queries on same method for single statement, other may require usage of a dedicated method for multi-queries or setting specific options at connection setup.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SELECT id,channel_id FROM records WHERE MATCH('one|two|three') LIMIT 5; SHOW META;
```

<!-- response SQL -->

```sql
+--------+----------------+
| id     | channel_id     |
+--------+----------------+
| 630768 | 1054702.000000 |
| 586645 | 1057204.000000 |
| 523391 | 1061514.000000 |
| 402383 | 1069381.000000 |
| 456106 | 1065936.000000 |
+--------+----------------+
5 rows in set (0.41 sec)

+---------------+--------+
| Variable_name | Value  |
+---------------+--------+
| total         | 1000   |
| total_found   | 311736 |
| time          | 0.407  |
| keyword[0]    | one    |
| docs[0]       | 265709 |
| hits[0]       | 538323 |
| keyword[1]    | two    |
| docs[1]       | 96044  |
| hits[1]       | 138576 |
| keyword[2]    | three  |
| docs[2]       | 43272  |
| hits[2]       | 69104  |
+---------------+--------+
12 rows in set (0.00 sec)
```

<!-- end -->

<!-- example SHOW META LIKE -->

You can also use the optional LIKE clause. It lets you pick just the variables that match a pattern. The pattern syntax is that of regular SQL wildcards, that is, `%` means any number of any characters, and `_` means a single character.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SHOW META LIKE 'total%';
```

<!-- response SQL -->

```sql
+---------------+--------+
| Variable_name | Value  |
+---------------+--------+
| total         | 1000   |
| total_found   | 311736 |
+---------------+--------+
2 rows in set (0.00 sec)
```

<!-- end -->
	
## SHOW META and facets

<!-- example show meta facets -->

When using [faceted search](../Searching/Faceted_search.md), you can check `multiplier` field in `SHOW META` output to see how many queries were run in an optimized group.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
SELECT * FROM facetdemo FACET brand_id FACET price FACET categories;
SHOW META LIKE 'multiplier';
```

<!-- response SQL -->

```sql
+------+-------+----------+---------------------+-------------+-------------+---------------------------------------+------------+
| id   | price | brand_id | title               | brand_name  | property    | j                                     | categories |
+------+-------+----------+---------------------+-------------+-------------+---------------------------------------+------------+
|    1 |   306 |        1 | Product Ten Three   | Brand One   | Six_Ten     | {"prop1":66,"prop2":91,"prop3":"One"} | 10,11      |
...

+----------+----------+
| brand_id | count(*) |
+----------+----------+
|        1 |     1013 |
...

+-------+----------+
| price | count(*) |
+-------+----------+
|   306 |        7 |
...

+------------+----------+
| categories | count(*) |
+------------+----------+
|         10 |     2436 |
...

+---------------+-------+
| Variable_name | Value |
+---------------+-------+
| multiplier    | 4     |
+---------------+-------+
1 row in set (0.00 sec)
```

<!-- end -->


## SHOW META for PQ indexes

<!-- example show meta PQ -->

`SHOW META` can be used after executing a [CALL PQ](../Searching/Percolate_query.md#Performing-a-percolate-query-with-CALL-PQ)  statement. In this case, it provides a different output.

`SHOW META` after a `CALL PQ` statement contains: 
 
* `Total` - total time spent on matching the document(s)
* `Queries matched `- how many stored queries match the document(s)
* `Document matches` - how many documents matched the queries stored in the index
* `Total queries stored` - number of queries stored in the index
* `Term only queries` - how many queries in the index have terms. The rest of the queries have extended query syntax.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
CALL PQ ('pq', ('{"title":"angry", "gid":3 }')); SHOW META;
```

<!-- response SQL -->

```sql
+------+
| id   |
+------+
|    2 |
+------+
1 row in set (0.00 sec)

+-----------------------+-----------+
| Name                  | Value     |
+-----------------------+-----------+
| Total                 | 0.000 sec |
| Queries matched       | 1         |
| Queries failed        | 0         |
| Document matched      | 1         |
| Total queries stored  | 2         |
| Term only queries     | 2         |
| Fast rejected queries | 1         |
+-----------------------+-----------+
7 rows in set (0.00 sec)
```

<!-- end -->

<!-- example call pq verbose meta  -->

`CALL PQ` with a `verbose` option gives a more detailed output.

It includes the following additional entries:

 * `Setup` - time spent on initial setup of the matching process: parsing docs, setting options, etc.
 * `Queries failed` - number of queries that failed
 * `Fast rejected queries` - number of queries that were not fully evaluated, but quickly matched and rejected with filters or other conditions
 * `Time per query` - detailed times for each query
 * `Time of matched queries` - total time spent on queries that matched any documents


<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
CALL PQ ('pq', ('{"title":"angry", "gid":3 }'), 1 as verbose); SHOW META;
```

<!-- response SQL -->

```sql
+------+
| id   |
+------+
|    2 |
+------+
1 row in set (0.00 sec)

+-------------------------+-----------+
| Name                    | Value     |
+-------------------------+-----------+
| Total                   | 0.000 sec |
| Setup                   | 0.000 sec |
| Queries matched         | 1         |
| Queries failed          | 0         |
| Document matched        | 1         |
| Total queries stored    | 2         |
| Term only queries       | 2         |
| Fast rejected queries   | 1         |
| Time per query          | 69        |
| Time of matched queries | 69        |
+-------------------------+-----------+
10 rows in set (0.00 sec)
```

<!-- end -->
