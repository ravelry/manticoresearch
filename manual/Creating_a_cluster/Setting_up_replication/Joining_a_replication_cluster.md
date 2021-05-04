# Joining a replication cluster 

<!-- example joining a replication cluster  1 -->
To join an existing cluster [name](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#Replication-cluster) and any working node should be set. In case of a single  cluster [path](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#Replication-cluster) might be omitted, [data_dir](../../Server_settings/Searchd.md#data_dir) will be used as the cluster path. For all subsequent clusters [path](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#Replication-cluster) needs to be set and it should be available.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
JOIN CLUSTER posts AT '10.12.1.35:9312'
```

<!-- request HTTP -->

```json
POST /sql -d "mode=raw&query=
JOIN CLUSTER posts AT '10.12.1.35:9312'
"
```

<!-- request PHP -->

```php
$params = [
  'cluster' => 'posts',
  'body' => [
      '10.12.1.35:9312'
  ]
];
$response = $client->cluster->join($params);
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('mode=raw&query=JOIN CLUSTER posts AT \'10.12.1.35:9312\'')
```

<!-- response Python -->
```python
{u'error': u'', u'total': 0, u'warning': u''}
```
<!-- intro -->
##### javascript:

<!-- request javascript -->

```javascript
res = await utilsApi.sql('mode=raw&query=JOIN CLUSTER posts AT \'10.12.1.35:9312\'');
```

<!-- response javascript -->
```javascript
{"total":0,"error":"","warning":""}
```

<!-- intro -->
##### java:

<!-- request Java -->

```java
utilsApi.sql("mode=raw&query=JOIN CLUSTER posts AT '10.12.1.35:9312'");
```
<!-- end -->


A node joins a cluster by getting the data from the node provided and, if successful, it updates node lists in all the other cluster nodes similar to [ALTER CLUSTER ... UPDATE nodes](../../Creating_a_cluster/Setting_up_replication/Managing_replication_nodes.md). This list is used to rejoin nodes to the cluster on restart.

There are two lists of nodes. One is used to rejoin nodes to the cluster  on restart, it is updated across all nodes same way as [ALTER CLUSTER ... UPDATE nodes](../../Creating_a_cluster/Setting_up_replication/Managing_replication_nodes.md) does. Join cluster does the same update automatically. [Cluster status](../../Creating_a_cluster/Setting_up_replication/Replication_cluster_status.md) shows this list as `cluster_post_nodes_set`. The second list is a list of all active nodes used for replication. This list doesn't require manual management. [ALTER CLUSTER ... UPDATE nodes](../../Creating_a_cluster/Setting_up_replication/Managing_replication_nodes.md) actually copies this list of nodes to the list of nodes used to rejoin on restart. [Cluster status](../../Creating_a_cluster/Setting_up_replication/Replication_cluster_status.md) shows this list as `cluster_post_nodes_view`.

<!-- example joining a replication cluster  2 -->
When nodes are located at different network segments or in different datacenters [nodes](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#Replication-cluster) option may be set explicitly. That allows to minimize traffic between nodes and to use gateway nodes for datacenters intercommunication. The following command joins an existing cluster using the [nodes](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#Replication-cluster) option.

> **Note:** that when this syntax is used, `cluster_post_nodes_set` list is not updated automatically. Use [ALTER CLUSTER ... UPDATE nodes](../../Creating_a_cluster/Setting_up_replication/Managing_replication_nodes.md) to update it.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
JOIN CLUSTER click_query 'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312' as nodes
```

<!-- request HTTP -->

```json
POST /sql -d "mode=raw&query=
JOIN CLUSTER click_query 'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312' as nodes
"
```

<!-- request PHP -->

```php
$params = [
  'cluster' => 'posts',
  'body' => [
      'nodes' => 'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312'
  ]
];
$response = $client->cluster->join($params);
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('mode=raw&query=JOIN CLUSTER click_query \'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312\' as nodes')
```

<!-- response Python -->
```python
{u'error': u'', u'total': 0, u'warning': u''}
```
<!-- intro -->
##### javascript:

<!-- request javascript -->

```javascript
res = await utilsApi.sql('mode=raw&query=JOIN CLUSTER click_query \'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312\' as nodes');
```

<!-- response javascript -->
```javascript
{"total":0,"error":"","warning":""}
```

<!-- intro -->
##### java:

<!-- request Java -->

```java
utilsApi.sql("mode=raw&query=JOIN CLUSTER click_query 'clicks_mirror1:9312;clicks_mirror2:9312;clicks_mirror3:9312' as nodes");
```
<!-- end -->

`JOIN CLUSTER` completes when a node receives all the necessary data to be in sync with all the other nodes in the cluster.
