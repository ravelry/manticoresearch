# Adding documents to a real-time index

> If you are looking for information about adding documents to a plain index please read section about [adding data from external storages](../Adding_data_from_external_storages/Plain_indexes_creation.md).

<!-- example insert -->
Adding documents in a real-time manner is only supported for [Real-Time](../Creating_an_index/Local_indexes/Real-time_index.md) and [percolate](../Creating_an_index/Local_indexes/Percolate_index.md) indexes. Corresponding SQL command or HTTP endpoint or a client's functions inserts new rows (documents) into an existing index with provided field values.

You can insert new documents with values for all fields of the index or only part of them. In this case the other fields will be filled with their default values (0 for scalar types, empty string for text types).

Expressions are not currently supported in `INSERT` and the values should be explicitly specified.

The ID field/value can be omitted as RT index supports [auto-id](../Adding_documents_to_an_index/Adding_documents_to_a_real-time_index.md#Auto-ID) functionality. You can use "0" as the id value to force automatic ID generation. Rows with duplicate IDs will not be overwritten by `INSERT`. You can use [REPLACE](../Updating_documents/REPLACE.md) for that.

<!-- intro -->
##### SQL:
<!-- request SQL -->
General syntax:

```sql
INSERT INTO <index name> [(column, ...)]
VALUES (value, ...)
[, (...)]
```

```sql
INSERT INTO products(title,price) VALUES ('Crossbody Bag with Tassel', 19.85);
INSERT INTO products(title) VALUES ('Crossbody Bag with Tassel');
INSERT INTO products VALUES (0,'Yellow bag', 4.95);
```
<!-- response SQL -->

```sql
Query OK, 1 rows affected (0.00 sec)
Query OK, 1 rows affected (0.00 sec)
Query OK, 1 rows affected (0.00 sec)
```

<!-- intro -->
##### HTTP:

<!-- request HTTP -->

```json
POST /insert -d
{
  "index":"products",
  "id":1,
  "doc":
  {
    "title" : "Crossbody Bag with Tassel",
    "price" : 19.85
  }
}

POST /insert
{
  "index":"products",
  "id":2,
  "doc":
  {
    "title" : "Crossbody Bag with Tassel"
  }
}

POST /insert -d
{
  "index":"products",
  "id":0,
  "doc":
  {
    "title" : "Yellow bag"
  }
}

```
<!-- response HTTP -->

```json
{
  "_index": "products",
  "_id": 1,
  "created": true,
  "result": "created",
  "status": 201
}
{
  "_index": "products",
  "_id": 2,
  "created": true,
  "result": "created",
  "status": 201
}
{
  "_index": "products",
  "_id": 0,
  "created": true,
  "result": "created",
  "status": 201
}
```

<!-- intro -->
##### PHP:

<!-- request PHP -->

```php
$index->addDocuments([
        ['id' => 1, 'title' => 'Crossbody Bag with Tassel', 'price' => 19.85]
]);
$index->addDocuments([
        ['id' => 2, 'title' => 'Crossbody Bag with Tassel']
]);
$index->addDocuments([
        ['id' => 0, 'title' => 'Yellow bag']
]);
```

<!-- intro -->
##### Python:

<!-- request Python -->

``` python
indexApi.insert({"index" : "test", "id" : 1, "doc" : {"title" : "Crossbody Bag with Tassel", "price" : 19.85}})
indexApi.insert({"index" : "test", "id" : 2, "doc" : {"title" : "Crossbody Bag with Tassel"}})
indexApi.insert({"index" : "test", "id" : 0, "doc" : {{"title" : "Yellow bag"}})
```
<!-- intro -->
##### Javascript:

<!-- request Javascript -->

``` javascript
res = await indexApi.insert({"index" : "test", "id" : 1, "doc" : {"title" : "Crossbody Bag with Tassel", "price" : 19.85}});
res = await indexApi.insert({"index" : "test", "id" : 2, "doc" : {"title" : "Crossbody Bag with Tassel"}});
res = await indexApi.insert({"index" : "test", "id" : 0, "doc" : {{"title" : "Yellow bag"}});
```

<!-- intro -->
##### java:

<!-- request Java -->

``` java
InsertDocumentRequest newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Crossbody Bag with Tassel");
    put("price",19.85);
}};
newdoc.index("products").id(1L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);

newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Crossbody Bag with Tassel");
}};
newdoc.index("products").id(2L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);

newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Yellow bag");
 }};
newdoc.index("products").id(0L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);

```

<!-- end -->

## Auto ID
<!-- example autoid -->
There is an auto ID generation functionality for column ID of documents inserted or replaced into an real-time or a [Percolate index](../Creating_an_index/Local_indexes/Percolate_index.md). The generator produces a unique ID of a document with some guarantees and should not be considered an auto-incremented ID.

The value of ID generated is guaranteed to be unique under the following conditions:
* [server_id](../Server_settings/Searchd.md#server_id) value of the current server is in range of 0 to 127 and is unique among nodes in the cluster or it uses the default value generated from MAC address as a seed
* system time does not change for the Manticore node between server restarts
* auto ID is generated fewer than 16 million times per second between search server restarts

The auto ID generator creates 64 bit integer for a document ID and uses the following schema:
* 0 to 23 bits is a counter that gets incremented on every call to auto ID generator
* 24 to 55 bits is a unix timestamp of the server start
* 56 to 63 bits is a server_id

This schema allows to be sure that the generated ID is unique among all nodes at the cluster and that data inserted into different cluster nodes does not create collisions between the nodes.

That is why the first ID from the generator used for auto ID is NOT 1 but a larger number. Also documents stream inserted into an index might have not sequential ID values if inserts into other indexes happen between the calls as the ID generator is single in the server and shared between all its indexes.

<!-- intro -->
##### SQL:
<!-- request SQL -->

```sql
INSERT INTO products(title,price) VALUES ('Crossbody Bag with Tassel', 19.85);
INSERT INTO products VALUES (0,'Yello bag', 4.95);
select * from products;
```
<!-- response SQL -->
```
+---------------------+-----------+---------------------------+
| id                  | price     | title                     |
+---------------------+-----------+---------------------------+
| 1657860156022587404 | 19.850000 | Crossbody Bag with Tassel |
| 1657860156022587405 |  4.950000 | Yello bag                 |
+---------------------+-----------+---------------------------+
```
<!-- intro -->
##### HTTP:
<!-- request HTTP -->

```json
POST /insert
{
  "index":"products",
  "id":0,
  "doc":
  {
    "title" : "Yellow bag"
  }
}

GET /search
{
  "index":"products",
  "query":{
    "query_string":""
  }
}
```
<!-- response HTTP -->

```json
{
  "took": 0,
  "timed_out": false,
  "hits": {
    "total": 1,
    "hits": [
      {
        "_id": "1657860156022587406",
        "_score": 1,
        "_source": {
          "price": 0,
          "title": "Yellow bag"
        }
      }
    ]
  }
}
```
<!-- intro -->
##### PHP:
<!-- request PHP -->

```php
$index->addDocuments([
        ['id' => 0, 'title' => 'Yellow bag']
]);
```

<!-- intro -->
##### Python:

<!-- request Python -->

```python
indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag"}})
```
<!-- intro -->

##### Javascript:

<!-- request Javascript -->

```javascript
res = await indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag"}});
```

<!-- intro -->
##### java:

<!-- request Java -->

``` java
newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Yellow bag");
 }};
newdoc.index("products").id(0L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);
```

<!-- end -->

<!-- example bulk_insert -->
## Bulk adding documents
You can insert into a real-time index not just a single document, but as many as you want. It's ok to insert into a real-time index in batches of tens of thousands of documents. What's important to know in this case:
* the larger the batch the higher is the latency of each insert operation
* the larger the batch the higher indexation speed you can expect
* each batch insert operation is considered a single [transaction](../Transactions.md) with atomicity guarantee, so you will either have all the new documents in the index at once or in case of a failure none of them will be added
* you might want to increase [max_packet_size](../Server_settings/Searchd.md#max_packet_size) value to allow bigger batches

<!-- intro -->
### Bulk insert examples
##### SQL:
<!-- request SQL -->
For bulk insert just provide more documents in brackets after VALUES(). The syntax is:

```sql
INSERT INTO <index name>[(column1, column2, ...)] VALUES ()[,(value1,[value2, ...])]
```

Optional column name list lets you explicitly specify values for some of the columns present in the index. All the other columns will be filled with their default values (0 for scalar types, empty string for string types).

For example:

```sql
INSERT INTO products(title,price) VALUES ('Crossbody Bag with Tassel', 19.85), ('microfiber sheet set', 19.99), ('Pet Hair Remover Glove', 7.99);
```
<!-- response SQL -->

```sql
Query OK, 3 rows affected (0.01 sec)
```

Expressions are not currently supported in `INSERT` and values should be explicitly specified.

<!-- intro -->
##### HTTP:
<!-- request HTTP -->
The syntax is in general the same as for [inserting a single document](../Quick_start_guide.md#Add-documents), just provide more lines one for each document and use `json/bulk` endpoint instead of `json/insert` and enclose each document into node "insert". Note that it also requires:
* Content-Type: application/x-ndjson
* The data itself should be formatted as a newline-delimited json (NDJSON). Basically it means that each line should contain exactly one json statement and end with a newline \n and maybe \r.

```json
POST /bulk 
-H "Content-Type: application/x-ndjson" -d '
{"insert": {"index":"products", "id":1, "doc":  {"title":"Crossbody Bag with Tassel","price" : 19.85}}}
{"insert":{"index":"products", "id":2, "doc":  {"title":"microfiber sheet set","price" : 19.99}}}
'
```
<!-- response HTTP -->
```json
{
  "items": [
    {
      "insert": {
        "_index": "products",
        "_id": 1,
        "created": true,
        "result": "created",
        "status": 201
      }
    },
    {
      "insert": {
        "_index": "products",
        "_id": 2,
        "created": true,
        "result": "created",
        "status": 201
      }
    }
  ],
  "errors": false
}
```

<!-- intro -->
##### PHP:
<!-- request PHP -->
Use method addDocuments():

```php
$index->addDocuments([
        ['id' => 1, 'title' => 'Crossbody Bag with Tassel', 'price' => 19.85],
        ['id' => 2, 'title' => 'microfiber sheet set', 'price' => 19.99],
        ['id' => 3, 'title' => 'Pet Hair Remover Glove', 'price' => 7.99]
]);
```

<!-- intro -->
##### Python:

<!-- request Python -->

```python
docs = [ \
    {"insert": {"index" : "products", "id" : 1, "doc" : {"title" : "Crossbody Bag with Tassel", "price" : 19.85}}}, \
    {"insert": {"index" : "products", "id" : 2, "doc" : {"title" : "microfiber sheet set", "price" : 19.99}}}, \
    {"insert": {"index" : "products", "id" : 3, "doc" : {"title" : "CPet Hair Remover Glove", "price" : 7.99}}}
]
res = indexApi.bulk('\n'.join(map(json.dumps,docs)))
```


<!-- intro -->
##### Javascript:

<!-- request Javascript -->

```javascript
let docs = [ 
    {"insert": {"index" : "products", "id" : 3, "doc" : {"title" : "Crossbody Bag with Tassel", "price" : 19.85}}}, 
    {"insert": {"index" : "products", "id" : 4, "doc" : {"title" : "microfiber sheet set", "price" : 19.99}}}, 
    {"insert": {"index" : "products", "id" : 5, "doc" : {"title" : "CPet Hair Remover Glove", "price" : 7.99}}}
];
res =  await indexApi.bulk(docs.map(e=>JSON.stringify(e)).join('\n'));
```
<!-- intro -->
##### java:

<!-- request Java -->

``` java
String body = "{\"insert\": {\"index\" : \"products\", \"id\" : 1, \"doc\" : {\"title\" : \"Crossbody Bag with Tassel\", \"price\" : 19.85}}}"+"\n"+
    "{\"insert\": {\"index\" : \"products\", \"id\" : 4, \"doc\" : {\"title\" : \"microfiber sheet set\", \"price\" : 19.99}}}"+"\n"+
    "{\"insert\": {\"index\" : \"products\", \"id\" : 5, \"doc\" : {\"title\" : \"CPet Hair Remover Glove\", \"price\" : 7.99}}}"+"\n";         
BulkResponse bulkresult = indexApi.bulk(body);
```

<!-- end -->
<!-- example MVA_insert -->
## Inserting multi-value attributes (MVA) values

Multi-value attributes (MVA) are inserted as arrays of numbers.
<!-- intro -->
### Examples
##### SQL
<!-- request SQL -->
```

INSERT INTO products(title, sizes) VALUES('shoes', (40,41,42,43));
```
<!-- intro -->
##### HTTP
<!-- request HTTP -->
```json

POST /insert
{
  "index":"products",
  "id":1,
  "doc":
  {
    "title" : "shoes",
    "sizes" : [40, 41, 42, 43]
  }
}
```
<!-- intro -->
##### PHP
<!-- request PHP -->

```php
$index->addDocument(
  ['title' => 'shoes', 'sizes' => [40,41,42,43]],
  1
);
```

<!-- intro -->
##### Python:

<!-- request Python -->

```python
indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag","sizes":[40,41,42,43]}})
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

```javascript
res = await indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag","sizes":[40,41,42,43]}});
```


<!-- intro -->
##### java:

<!-- request Java -->

``` java
newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Yellow bag");
    put("sizes",new int[]{40,41,42,43});
 }};
newdoc.index("products").id(0L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);
```
<!-- end -->


<!-- example JSON_insert -->
## Inserting JSON
JSON value can be inserted as as an [escaped](../Searching/Full_text_matching/Escaping.md) string (via SQL, HTTP, PHP) or as a JSON object (via HTTP).

<!-- intro -->
### Examples
##### SQL
<!-- request SQL -->
```

INSERT INTO products VALUES (1, 'shoes', '{"size": 41, "color": "red"}');
```
<!-- intro -->
##### HTTP
<!-- request HTTP -->
JSON value can be inserted as as JSON object
```json
POST /insert
{
  "index":"products",
  "id":1,
  "doc":
  {
    "title" : "shoes",
    "meta" : {
      "size": 41,
      "color": "red"
    }
  }
}
```

JSON value can be also inserted as a string containing escaped JSON:
```json
POST /insert
{
  "index":"products",
  "id":1,
  "doc":
  {
    "title" : "shoes",
    "meta" : "{\"size\": 41, \"color\": \"red\"}"
  }
}
```
<!-- intro -->
##### PHP
Consider JSON just as string:
<!-- request PHP -->

```php
$index->addDocument(
  ['title' => 'shoes', 'meta' => '{"size": 41, "color": "red"}'],
  1
);
```
<!-- intro -->
##### Python:

<!-- request Python -->
``` python
indexApi = api = manticoresearch.IndexApi(client)
indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag","meta":'{"size": 41, "color": "red"}'}})
```
<!-- intro -->
##### Javascript:

<!-- request Javascript -->
```javascript

res = await indexApi.insert({"index" : "products", "id" : 0, "doc" : {"title" : "Yellow bag","meta":'{"size": 41, "color": "red"}'}});
```
<!-- intro -->
##### java:

<!-- request Java -->

``` java
newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Yellow bag");
    put("meta", 
        new HashMap<String,Object>(){{
            put("size",41);
            put("color","red");
        }});
 }};
newdoc.index("products").id(0L).setDoc(doc); 
sqlresult = indexApi.insert(newdoc);
```

<!-- end -->
