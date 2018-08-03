.. _percolate_query:

Percolate query
---------------
   
The percolate query is used to match documents against queries stored in a index. It is also called "search in reverse" as it works opposite to a regular search where documents are stored in an index and queries are issued against the index.

Queries are stored in a special RealTime index and they can be added, deleted and listed using INSERT/DELETE/SELECT statements similar way as it's done for a regular index.

Checking if a document matches any of the predefined criterias (queries) can be done with the ``CALL PQ`` function, which returns a list of the matched queries.
Note that it does not add documents to the percolate index. You need to use another index (regular or RealTime) in which you will insert documents to perform regular searches.

.. _percolate_query_index:

The percolate index
~~~~~~~~~~~~~~~~~~~

A percolate query works only for ``percolate`` index :ref:`type <type>`.  The percolate index is a modified  Real-Time index and shares a similar configuration,
however the declaration of fields and attributes can be omitted, in this case the index is created with a default field ``text`` and an integer attribute ``gid``.

.. code-block:: ini


    index pq
    {
        type = percolate
        path = path/index_name
        min_infix_len   = 4
    }

The percolate index has several particularities over regular Real-Time indexes:

The doc `id`  is named ``UID`` and has autoincrement functionality. 

The declared fields and attributes are not used for storing data, they define the document schema used by percolate queries and incoming documents.

Besided regular fields and attributes, the percolate index has 3 specific properties that are enabled by default and these are used in INSERT statements:

.. _percolate_query_query:

Query 
^^^^^

It holds the full text query as the value of a MATCH clause. If per field operators are used inside a query, the full text fields needs to be declared in the percolate index configuration.
If the stored query is supposed to be a full-scan (only attribute filtering, no full text query), the ``query`` value can be empty or simply omitted. 

.. _percolate_query_filters:

Filters
^^^^^^^

Filters is an optional string containing attribute filters and/or expressions in the same way they are defined in the WHERE clause,  like ``gid=10 and pages>4``. 
The attributes used here needs to be declared in the percolate index configuration. 


.. _percolate_query_tags:

Tags
^^^^

Optional, tags represent a list of string labels, separated by comma, which can be used for filtering queries in  ``SELECT`` statements on the percolate index or to delete queries using ``DELETE`` statement. 
The tags  can be returned in the CALL PQ result set.
	
.. _percolate_query_insert:

Store queries
~~~~~~~~~~~~~~~

To store a query the ``INSERT`` statement looks like

.. code-block:: sql

    INSERT INTO index_name (query[, tags, filters]) VALUES ( query_terms, tags_list, filters );
    INSERT INTO index_name (query, tags, filters) VALUES ( 'full text query terms', 'tags', 'filters' );
    INSERT INTO index_name (query) VALUES ( 'full text query terms');
    INSERT INTO index_name VALUES ( 'full text query terms', 'tags');
    INSERT INTO index_name VALUES ( 'full text query terms');

    
In case no schema declared for the ``INSERT`` statement te first field will be full-text ``query``
and the optional second field will be ``tags``.


.. _percolate_query_list:

List stored queries
~~~~~~~~~~~~~~~~~~~

To list stored queries in index the ``SELECT`` statement looks like

.. code-block:: sql


    SELECT * FROM index_name;
    SELECT * FROM index_name WHERE tags='tags list';
    SELECT * FROM index_name WHERE uid IN (11,35,101);

    
In case ``tags`` provided matching queries will be shown if any ``tags`` from the ``SELECT`` statement match tags in the stored query. In case ``uid`` provided range or
value list filter will be used to filter out stored queries.

The ``SELECT`` supports ``count(*)`` and ``count(*) alias`` to get number of of percolate queries. Any values are just ignored there however ``count(*)``
should provide the total amount of queries stored.

.. code-block:: sql


    mysql> select count(*) c from pq;
    +------+
    | c    |
    +------+
    |    3 |
    +------+


The ``SELECT`` supports ``LIMIT`` clause to narrow down the number of percolate queries.

.. code-block:: sql


    SELECT * FROM index_name LIMIT 5;
    SELECT * FROM index_name LIMIT 1300, 45;


.. _percolate_query_delete:

Delete queries
~~~~~~~~~~~~~~~~~~~~~~~

To delete a stored percolate query(es) in index the ``DELETE`` statement looks like

.. code-block:: sql


    DELETE FROM index_name WHERE id=1;
    DELETE FROM index_name WHERE tags='tags list';

    
In case ``tags`` provided the query will be deleted if any ``tags`` from the ``DELETE`` statement match any of its tags.

``TRUNCATE`` statement can also be used to delete all stored queries:

.. code-block:: sql

   TRUNCATE RTINDEX index_name;
   

.. _percolate_query_call:

CALL PQ
~~~~~~~

To search for queries matching a document(s) the ``CALL PQ`` statement is used which looks like

.. code-block:: sql


    CALL PQ ('index_name', 'single document', 0 as docs, 0 as docs_json, 0 as verbose);
    CALL PQ ('index_name', ('multiple documents', 'go this way'), 0 as docs_json );

    
The document in ``CALL PQ`` can be ``JSON`` encoded string or raw string. Fields and attributes mapping are allowed for ``JSON`` documents only.
``JSON`` documents has option to set document ``id`` field.

.. code-block:: sql


    CALL PQ ('pq', (
    '{"title":"header text", "body":"post context", "timestamp":11 }',
    '{"title":"short post", "counter":7 }'
    ), 1 as docs_json );
    CALL PQ ('pq', (
    '{"title":"short post", "counter":7, "uid":100 }',
    '{"title":"smallest doc", "gid":11, "uid":101 }'
    ), 1 as docs_json, 'uid' as docs_id );

    
``CALL PQ`` can have multiple options set as ``option_name``.

Here are default values for the options:

-  docs_json - 1 (enabled), to treat document(s) as ``JSON`` encoded string or raw string otherwise
-  docs - 0 (disabled), to provide per query documents matched at result set
-  verbose - 0 (disabled), to provide extended info on matching at :ref:`SHOW META <percolate_query_show_meta>`
-  query - 0 (disabled), to provide all query fields stored, such as query, tags, filters
-  docs_id - none (disabled), id alias name, to treat ``JSON`` named field as document id

The output of CALL PQ  return the following columns:

* UID  - the id of the stored query
* Documents -  if docs_id is not set, it will return the index of the documents as defined at input. If docs_id is set, the document indexes as replaced with the values of the field defined by docs_id 
* Query -  the stored full text query
* Tags -  the tags attached to the stored query
* Filters -  the filters attached to the stored query

Examples:

.. code-block:: sql

    mysql> CALL PQ('pq',('catch me if can','catch me'),0 AS docs_json,1 AS docs,1 AS verbose);
    +------+-----------+-------------+------+---------+
    | UID  | Documents | Query       | Tags | Filters |
    +------+-----------+-------------+------+---------+
    |    6 | 1,2       | catch me    |      |         |
    |    7 | 1         | catch me if | tag1 |         |
    +------+-----------+-------------+------+---------+
    2 rows in set (0.00 sec)

    mysql> call pq('pq1','{"text":"this search will be hard to find me","k":14,"id":100}','id' as docs_id, 1 as  docs_json,1 AS docs,1 as query);
    +------+-----------+-----------------+-----------+---------+
    | UID  | Documents | Query           | Tags      | Filters |
    +------+-----------+-----------------+-----------+---------+
    |    1 | 100       | @text search me | tag1,tag4 |  k>10   |
    +------+-----------+-----------------+-----------+---------+
    1 row in set (0.00 sec)


``CALL PQ`` performance is affected by :ref:`dist_threads`.

.. _percolate_query_show_meta:

Meta
~~~~

Meta information is kept for documents on "CALL PQ" and can be retrieved with ``SHOW META`` call.

``SHOW META`` output after ``CALL PQ`` looks like

.. code-block:: sql


    +-------------------------+-----------+
    | Name                    | Value     |
    +-------------------------+-----------+
    | Total                   | 0.010 sec |
    | Queries matched         | 950       |
    | Document matches        | 1500      |
    | Total queries stored    | 1000      |
    | Term only queries       | 998       |
    +-------------------------+-----------+

    
With entries: 
 
-  Total - total time spent for matching the document(s)
-  Queries matched - how many stored queries match the document(s)
-  Document matches - how many times the documents match the queries stored in the index
-  Total queries stored - how many queries are stored in the index at all
-  Term only queries - how many queries in the index have terms. The rest of the queries have extended query syntax

.. _percolate_query_reconfigure:

Reconfigure
~~~~~~~~~~~

As well as for RealTime indexes ``ALTER RECONFIGURE`` command is also supported for percolate query index. It allows to reconfigure ``percolate`` index on the fly without deleting
and repopulating the index with queries back.

.. code-block:: sql


    mysql> DESC pq1;
    +-------+--------+
    | Field | Type   |
    +-------+--------+
    | id    | bigint |
    | text  | field  |
    | body  | field  |
    | k     | uint   |
    +-------+--------+

    mysql> SELECT * FROM pq1;
    +------+-------+------+-------------+
    | UID  | Query | Tags | Filters     |
    +------+-------+------+-------------+
    |    1 | test  |      |  k=4        |
    |    2 | test  |      |  k IN (4,6) |
    |    3 | test  |      |             |
    +------+-------+------+-------------+

    
Add `JSON` attribute to the index config ``rt_attr_json = json_data``, then issue ``ALTER RECONFIGURE``

.. code-block:: sql

    mysql> ALTER RTINDEX pq1 RECONFIGURE;
	
    mysql> DESC pq1;
    +-----------+--------+
    | Field     | Type   |
    +-----------+--------+
    | id        | bigint |
    | text      | field  |
    | body      | field  |
    | k         | uint   |
    | json_data | json   |
    +-----------+--------+

    