-- The tuple and cell functions take a TID.  The TID of a deleted row, of an
-- old version of an updated row, or of a row that another transaction has
-- inserted but not committed still points at that row's data.  Such tuples
-- are not visible to the caller's snapshot and must give NULL, like a TID
-- without a tuple: otherwise a checksum of data the caller cannot SELECT
-- leaks it (a 32-bit checksum of an integer is reversed by trying values).
CREATE TABLE vis_t (id int, secret int) WITH (autovacuum_enabled = off);
INSERT INTO vis_t VALUES (1, 42), (2, 7);

-- Visible rows have checksums (all functions, both algorithms).
SELECT pg_tuple_logical_checksum('vis_t', '(0,1)') IS NOT NULL AS logical_visible,
       pg_tuple_logical_checksum('vis_t', '(0,1)', 'fnv64') IS NOT NULL AS logical64_visible,
       pg_tuple_physical_checksum('vis_t', '(0,1)') IS NOT NULL AS physical_visible,
       pg_tuple_physical_checksum('vis_t', '(0,1)', true, 'fnv64') IS NOT NULL AS physical64_visible,
       pg_cell_checksum('vis_t', '(0,1)', 2) IS NOT NULL AS cell_visible,
       pg_cell_checksum('vis_t', '(0,1)', 2, 'fnv64') IS NOT NULL AS cell64_visible;

-- A committed delete: the dead tuple (0,1) is still on the page.
DELETE FROM vis_t WHERE id = 1;
SELECT pg_tuple_logical_checksum('vis_t', '(0,1)') IS NULL AS logical_deleted,
       pg_tuple_logical_checksum('vis_t', '(0,1)', 'fnv64') IS NULL AS logical64_deleted,
       pg_tuple_physical_checksum('vis_t', '(0,1)') IS NULL AS physical_deleted,
       pg_tuple_physical_checksum('vis_t', '(0,1)', true, 'fnv64') IS NULL AS physical64_deleted,
       pg_cell_checksum('vis_t', '(0,1)', 2) IS NULL AS cell_deleted,
       pg_cell_checksum('vis_t', '(0,1)', 2, 'fnv64') IS NULL AS cell64_deleted;

-- An update: the old version is invisible, the new one is visible.
UPDATE vis_t SET secret = 8 WHERE id = 2;
SELECT ctid FROM vis_t WHERE id = 2;
SELECT pg_cell_checksum('vis_t', '(0,2)', 2) IS NULL AS old_version_null,
       pg_cell_checksum('vis_t', '(0,3)', 2) IS NOT NULL AS new_version_visible;

-- Inside a transaction, a row deleted by an earlier command is invisible to
-- later ones, and a row inserted by it is visible.
BEGIN;
DELETE FROM vis_t WHERE id = 2;
INSERT INTO vis_t VALUES (3, 9);
SELECT pg_tuple_logical_checksum('vis_t', '(0,3)') IS NULL AS own_delete_null,
       pg_tuple_logical_checksum('vis_t', '(0,4)') IS NOT NULL AS own_insert_visible;
ROLLBACK;

-- After the rollback the deleted row is visible again and the inserted one
-- never existed for anyone else.
SELECT pg_tuple_logical_checksum('vis_t', '(0,3)') IS NOT NULL AS rolled_back_delete_visible,
       pg_tuple_logical_checksum('vis_t', '(0,4)') IS NULL AS aborted_insert_null,
       pg_tuple_physical_checksum('vis_t', '(0,4)') IS NULL AS aborted_insert_physical_null;

-- A row lock is stored in xmax: it changes the physical checksums of the row
-- and the table (documented) but not the logical ones.
CREATE TEMP TABLE vis_before AS
SELECT pg_tuple_physical_checksum('vis_t', '(0,3)', false) AS tuple_physical,
       pg_table_physical_checksum('vis_t', false) AS table_physical,
       pg_tuple_logical_checksum('vis_t', '(0,3)') AS tuple_logical,
       pg_table_logical_checksum('vis_t') AS table_logical;
BEGIN;
SELECT id FROM vis_t WHERE id = 2 FOR KEY SHARE;
COMMIT;
SELECT b.tuple_physical <> pg_tuple_physical_checksum('vis_t', '(0,3)', false) AS lock_changes_tuple_physical,
       b.table_physical <> pg_table_physical_checksum('vis_t', false) AS lock_changes_table_physical,
       b.tuple_logical = pg_tuple_logical_checksum('vis_t', '(0,3)') AS lock_keeps_tuple_logical,
       b.table_logical = pg_table_logical_checksum('vis_t') AS lock_keeps_table_logical
FROM vis_before b;
DROP TABLE vis_before;

DROP TABLE vis_t;
