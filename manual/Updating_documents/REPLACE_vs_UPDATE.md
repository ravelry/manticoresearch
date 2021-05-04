# REPLACE vs UPDATE 

You can change existing data in an RT or PQ index by either updating or replacing it.

[UPDATE](../Updating_documents/UPDATE.md) replaces attribute values  of existing documents with new values. Full-text fields cannot be updated. If you need to change the content of a full-text field, use [REPLACE](../Updating_documents/REPLACE.md).

[REPLACE](../Updating_documents/REPLACE.md) works similar to  [INSERT](../Adding_documents_to_an_index/Adding_documents_to_a_real-time_index.md) except that if an old document has the same ID as a new document, the old document is marked as deleted before the new document is inserted. Note that the old document is not physically deleted from an index, this only happens chunks are merged in an index, e.g. as a result of an [OPTIMIZE](../Securing_and_compacting_an_index/Compacting_an_index.md).