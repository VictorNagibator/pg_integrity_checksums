-- Robustness of logical checksums that evaluate index definitions and of
-- database scans on a database that changes while it is being scanned.

-- 1. Index expressions that are not PARALLEL SAFE.  A user-defined function
-- is PARALLEL UNSAFE unless declared otherwise; running it in parallel mode
-- fails (here: SET is not allowed during a parallel operation).  With
-- workers configured, such an index must be computed serially and give the
-- same value.
CREATE FUNCTION rob_unsafe(int) RETURNS int
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    PERFORM set_config('rob.x', '1', true);
    RETURN $1;
END
$$;

CREATE TABLE rob_big AS SELECT g AS a FROM generate_series(1, 100000) g;
CREATE INDEX rob_big_expr ON rob_big (rob_unsafe(a));
CREATE INDEX rob_big_pred ON rob_big (a) WHERE rob_unsafe(a) > 0;
CREATE INDEX rob_big_plain ON rob_big (a);

SET pg_integrity_checksums.workers = 0;
SELECT pg_index_logical_checksum('rob_big_expr') AS expr32 \gset
SELECT pg_index_logical_checksum('rob_big_pred', 'fnv64') AS pred64 \gset
SELECT pg_database_logical_checksum() AS db32 \gset
SELECT pg_database_logical_checksum(false, true, 'fnv64') AS db64 \gset

SET pg_integrity_checksums.workers = 2;
SELECT pg_index_logical_checksum('rob_big_expr') = :expr32 AS expr_serial_fallback,
       pg_index_logical_checksum('rob_big_pred', 'fnv64') = :'pred64' AS pred_serial_fallback;
-- The database scan computes the unsafe indexes outside the parallel scan.
SELECT pg_database_logical_checksum() = :db32 AS db_parallel_same,
       pg_database_logical_checksum(false, true, 'fnv64') = :'db64' AS db64_parallel_same;
RESET pg_integrity_checksums.workers;

DROP TABLE rob_big;
DROP FUNCTION rob_unsafe(int);

-- 2. A relation dropped while a database checksum runs.  The relations are
-- enumerated with the statement snapshot; one dropped before the scan
-- reaches it is left out with a NOTICE instead of failing the checksum, even
-- in strict mode.  To drop it at the right moment, an index expression of a
-- table that is scanned first drops a table that is scanned later (tables
-- are scanned in OID order without workers).
CREATE FUNCTION rob_dropper(int) RETURNS int
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    RETURN $1;
END
$$;
CREATE TABLE rob_trap (a int);
INSERT INTO rob_trap VALUES (1);
CREATE INDEX rob_trap_idx ON rob_trap (rob_dropper(a));
CREATE TABLE rob_victim (a int);
INSERT INTO rob_victim VALUES (1);
CREATE INDEX rob_victim_idx ON rob_victim (a);
CREATE OR REPLACE FUNCTION rob_dropper(int) RETURNS int
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    EXECUTE 'DROP TABLE IF EXISTS rob_victim';
    RETURN $1;
END
$$;

SHOW pg_integrity_checksums.strict;
SET pg_integrity_checksums.workers = 0;
-- The NOTICE names the dropped relation by OID, which varies between runs.
SET client_min_messages = warning;
SELECT pg_database_logical_checksum() IS NOT NULL AS checksum_with_drop;
RESET client_min_messages;
SELECT to_regclass('rob_victim') IS NULL AS victim_dropped;
-- Afterwards the checksum is that of the database without the table.
SELECT pg_database_logical_checksum() IS NOT NULL AS checksum_after_drop;
RESET pg_integrity_checksums.workers;

DROP TABLE rob_trap;
DROP FUNCTION rob_dropper(int);
