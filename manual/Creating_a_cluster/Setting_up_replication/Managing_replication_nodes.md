# Managing replication nodes 

<!-- example managing replication nodes 1 -->
`ALTER CLUSTER <cluster_name> UPDATE <nodes>` statement updates node lists on each node of the cluster to include every active node in the cluster. See [Joining a cluster](../../Creating_a_cluster/Setting_up_replication/Joining_a_replication_cluster.md) for more info on node lists.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
ALTER CLUSTER posts UPDATE nodes
```

<!-- request HTTP -->

```json
POST /sql -d "mode=raw&query=
ALTER CLUSTER posts UPDATE nodes
"
```

<!-- request PHP -->

```php
$params = [
  'cluster' => 'posts',
  'body' => [
     'operation' => 'update',
     
  ]
];
$response = $client->cluster()->alter($params); 
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('mode=raw&query=ALTER CLUSTER posts UPDATE nodes')
```

<!-- response Python -->
```python
{u'error': u'', u'total': 0, u'warning': u''}
```
<!-- intro -->
##### javascript:

<!-- request javascript -->

```javascript
res = await utilsApi.sql('mode=raw&query=ALTER CLUSTER posts UPDATE nodes');
```

<!-- response javascript -->
```javascript
{"total":0,"error":"","warning":""}
```

<!-- intro -->
##### java:

<!-- request Java -->

```java
utilsApi.sql("mode=raw&query=ALTER CLUSTER posts UPDATE nodes");
```
<!-- end -->


For example, when the cluster was initially created, the list of nodes used for rejoining the cluster was `10.10.0.1:9312,10.10.1.1:9312`. Since then other nodes joined the cluster and now we have the following active nodes: `10.10.0.1:9312,10.10.1.1:9312,10.15.0.1:9312,10.15.0.3:9312`.

But the list of nodes used for rejoining the cluster is still the same. Running the `ALTER CLUSTER ... UPDATE nodes` copies the list of active nodes to the list of nodes used to rejoin on restart. After this, the list of nodes used on restart includes all the active nodes in the cluster.

Both lists of nodes can be viewed using [Cluster status](../../Creating_a_cluster/Setting_up_replication/Replication_cluster_status.md) statement (`cluster_post_nodes_set` and `cluster_post_nodes_view`).
