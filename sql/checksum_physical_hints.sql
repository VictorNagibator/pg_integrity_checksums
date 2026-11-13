-- Physical index checksums must not change when an index scan sets hints.
--
-- B-tree, hash and GiST index scans mark entries of dead heap tuples LP_DEAD
-- and set a "page has garbage" flag, under a share lock and without any
-- change to the index content.  Those hints, like pd_lsn and the in-buffer
-- pd_checksum, are masked out of the physical checksum.

CREATE TABLE phys_hints (id integer PRIMARY KEY, p point)
    WITH (autovacuum_enabled = off);
CREATE INDEX phys_hints_hash ON phys_hints USING hash (id);
CREATE INDEX phys_hints_gist ON phys_hints USING gist (p);
INSERT INTO phys_hints SELECT i, point(i, i) FROM generate_series(1, 3000) i;

-- Dead tuples whose index entries the scans below can mark LP_DEAD.
DELETE FROM phys_hints WHERE id <= 1500;

CREATE TEMP TABLE phys_hints_before AS
SELECT pg_index_physical_checksum('phys_hints_pkey') AS btree,
       pg_index_physical_checksum('phys_hints_hash') AS hash,
       pg_index_physical_checksum('phys_hints_gist') AS gist,
       pg_index_physical_checksum('phys_hints_pkey', 'fnv64') AS btree64;

-- Plain reads through each index.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM phys_hints WHERE id <= 1500;
SELECT count(*) FROM phys_hints WHERE id = 10;
SELECT count(*) FROM phys_hints WHERE p <@ box '((0,0),(1500,1500))';
SELECT count(*) FROM phys_hints WHERE id <= 1500;
SELECT count(*) FROM phys_hints WHERE p <@ box '((0,0),(1500,1500))';
RESET enable_seqscan;
RESET enable_bitmapscan;

SELECT b.btree = pg_index_physical_checksum('phys_hints_pkey') AS btree_stable,
       b.hash = pg_index_physical_checksum('phys_hints_hash') AS hash_stable,
       b.gist = pg_index_physical_checksum('phys_hints_gist') AS gist_stable,
       b.btree64 = pg_index_physical_checksum('phys_hints_pkey', 'fnv64')
           AS btree64_stable
FROM phys_hints_before b;

-- A real change still changes every physical index checksum.
INSERT INTO phys_hints VALUES (100000, point(-1, -1));

SELECT b.btree <> pg_index_physical_checksum('phys_hints_pkey') AS btree_changed,
       b.hash <> pg_index_physical_checksum('phys_hints_hash') AS hash_changed,
       b.gist <> pg_index_physical_checksum('phys_hints_gist') AS gist_changed
FROM phys_hints_before b;

DROP TABLE phys_hints;
