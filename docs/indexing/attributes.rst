.. _attributes:

Attributes
==========

Attributes are additional values associated with each document that can
be used to perform additional filtering and sorting during search.

It is often desired to additionally process full-text search results
based not only on matching document ID and its rank, but on a number of
other per-document values as well. For instance, one might need to sort
news search results by date and then relevance, or search through
products within specified price range, or limit blog search to posts
made by selected users, or group results by month. To do that
efficiently, Manticore allows to attach a number of additional *attributes*
to each document, and store their values in the full-text index. It's
then possible to use stored values to filter, sort, or group full-text
matches.

Attributes, unlike the fields, are not full-text indexed. They are
stored in the index, but it is not possible to search them as full-text,
and attempting to do so results in an error.

For example, it is impossible to use the extended matching mode
expression ``@column 1`` to match documents where column is 1, if column
is an attribute, and this is still true even if the numeric digits are
normally indexed.

Attributes can be used for filtering, though, to restrict returned rows,
as well as sorting or :ref:`result
grouping <grouping_clustering_search_results>`; it is entirely
possible to sort results purely based on attributes, and ignore the
search relevance tools. Additionally, attributes are returned from the
search daemon, while the indexed text is not.

A good example for attributes would be a forum posts table. Assume that
only title and content fields need to be full-text searchable - but that
sometimes it is also required to limit search to a certain author or a
sub-forum (ie. search only those rows that have some specific values of
author_id or forum_id columns in the SQL table); or to sort matches by
post_date column; or to group matching posts by month of the post_date
and calculate per-group match counts.

This can be achieved by specifying all the mentioned columns (excluding
title and content, that are full-text fields) as attributes, indexing
them, and then using API calls to setup filtering, sorting, and
grouping. Here as an example.

.. _Example sphinx.conf part:

Example sphinx.conf part:
~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: none


    ...
    sql_query = SELECT id, title, content, \
        author_id, forum_id, post_date FROM my_forum_posts
    sql_attr_uint = author_id
    sql_attr_uint = forum_id
    sql_attr_timestamp = post_date
    ...

.. _Example application code (in PHP):

Example application code (in PHP):
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: php


    // only search posts by author whose ID is 123
    $cl->SetFilter ( "author_id", array ( 123 ) );

    // only search posts in sub-forums 1, 3 and 7
    $cl->SetFilter ( "forum_id", array ( 1,3,7 ) );

    // sort found posts by posting date in descending order
    $cl->SetSortMode ( SPH_SORT_ATTR_DESC, "post_date" );

Attributes are named. Attribute names are case insensitive. Attributes
are *not* full-text indexed; they are stored in the index as is.
Currently supported attribute types are:

-  unsigned integers (1-bit to 32-bit wide);

-  signed big integers (64-bit wide);

-  UNIX timestamps;

-  floating point values (32-bit, IEEE 754 single precision);

-  :ref:`strings <sql_attr_string>`;

-  :ref:`JSON <sql_attr_json>`;

-  :ref:`MVA <mva_multi-valued_attributes>`, multi-value attributes
   (variable-length lists of 32-bit unsigned or 64-bit signed integers).

The complete set of per-document attribute values is sometimes referred
to as *docinfo*. Docinfos are stored separately from the main full-text
index data in ``.spa`` file.

A copy of ``.spa`` file (with all the
attribute values for all the documents) is kept in RAM by ``searchd`` at
all times (via mmap()). This is for performance reasons; random disk I/O may be too slow otherwise
in most cases.
