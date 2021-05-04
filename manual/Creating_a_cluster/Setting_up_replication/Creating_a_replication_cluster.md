# Creating a replication cluster 

<!-- example creating a replication cluster 1 -->
To create a replication cluster you should set at least its [name](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#name).

In case of a single cluster or if the cluster you are creating is the first one [path](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#path) option may be  omitted, in this case [data_dir](../../Server_settings/Searchd.md#data_dir) option will be used as the cluster path. For all subsequent clusters you need to specify [path](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#path) and this path should be available. [nodes](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#nodes) option may be also set to enumerate all the nodes in the cluster.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
CREATE CLUSTER posts
CREATE CLUSTER click_query '/var/data/click_query/' as path
CREATE CLUSTER click_query '/var/data/click_query/' as path, 'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312' as nodes
```

<!-- request HTTP -->

```json
POST /sql -d "mode=raw&query=
CREATE CLUSTER posts
"
POST /sql -d "mode=raw&query=
CREATE CLUSTER click_query '/var/data/click_query/' as path
"
POST /sql -d "mode=raw&query=
CREATE CLUSTER click_query '/var/data/click_query/' as path, 'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312' as nodes
"
```

<!-- request PHP -->

```php
$params = [
    'cluster' => 'posts',
    ]
];
$response = $client->cluster()->create($params);
$params = [
    'cluster' => 'click_query',
    'body' => [
        'path' => '/var/data/click_query/'
    ]    
    ]
];
$response = $client->cluster()->create($params);
$params = [
    'cluster' => 'click_query',
    'body' => [
        'path' => '/var/data/click_query/',
        'nodes' => 'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312'
    ]    
    ]
];
$response = $client->cluster()->create($params);
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('mode=raw&query=CREATE CLUSTER posts')
utilsApi.sql('mode=raw&query=CREATE CLUSTER click_query \'/var/data/click_query/\' as path')
utilsApi.sql('mode=raw&query=CREATE CLUSTER click_query \'/var/data/click_query/\' as path, \'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312\' as nodes')

```

<!-- response Python -->
```python
{u'error': u'', u'total': 0, u'warning': u''}
```
<!-- intro -->
##### javascript:

<!-- request javascript -->

```javascript
res = await utilsApi.sql('mode=raw&query=CREATE CLUSTER posts');
res = await utilsApi.sql('mode=raw&query=CREATE CLUSTER click_query \'/var/data/click_query/\' as path');
res = await utilsApi.sql('mode=raw&query=CREATE CLUSTER click_query \'/var/data/click_query/\' as path, \'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312\' as nodes');
```

<!-- response javascript -->
```javascript
{"total":0,"error":"","warning":""}
```

<!-- intro -->
##### java:

<!-- request Java -->

```java
utilsApi.sql("mode=raw&query=CREATE CLUSTER posts");
utilsApi.sql("mode=raw&query=CREATE CLUSTER click_query '/var/data/click_query/' as path");
utilsApi.sql("mode=raw&query=CREATE CLUSTER click_query '/var/data/click_query/' as path, 'clicks_mirror1:9312,clicks_mirror2:9312,clicks_mirror3:9312' as nodes");
```
<!-- end -->

If a cluster is created without the [nodes](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#nodes) option, the first node that gets joined to the cluster will be saved as [nodes](../../Creating_a_cluster/Setting_up_replication/Setting_up_replication.md#nodes).

