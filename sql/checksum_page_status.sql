-- Page checksum status: the verifier must either compare successfully or
-- report that the cluster was initialized without data checksums.
CREATE TABLE page_status_reg (id integer PRIMARY KEY, payload text);
INSERT INTO page_status_reg VALUES (1, 'one'), (2, 'two');
CHECKPOINT;

SELECT pg_page_checksum_status('page_status_reg'::regclass, 0)
       IN ('ok', 'checksums_disabled') AS status_known;

SELECT pg_page_checksum('page_status_reg'::regclass, 0)
       BETWEEN 0 AND 65535 AS page_checksum_16bit;

DROP TABLE page_status_reg;

-- Whole-relation verification.  The output must not depend on whether the
-- cluster has data checksums: with them, every block of every fork is ok;
-- without them, the function fails with object_not_in_prerequisite_state.
CREATE FUNCTION page_status_relation_check(rel regclass) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    problem text;
BEGIN
    IF NOT current_setting('data_checksums')::boolean THEN
        BEGIN
            PERFORM * FROM pg_relation_page_checksum_status(rel);
            RETURN 'no error with data checksums disabled';
        EXCEPTION WHEN object_not_in_prerequisite_state THEN
            RETURN 'verified';
        END;
    END IF;

    -- Only mismatches by default.
    IF EXISTS (SELECT FROM pg_relation_page_checksum_status(rel)) THEN
        RETURN 'mismatch reported';
    END IF;

    -- include_ok returns each block of each existing fork exactly once.
    SELECT string_agg(format('%s: %s rows for %s blocks', f.fork, coalesce(s.n, 0),
                             f.nblocks), ', ')
    INTO problem
    FROM (SELECT fork,
                 pg_relation_size(rel, fork) / current_setting('block_size')::int
                     AS nblocks
          FROM unnest(ARRAY['main', 'fsm', 'vm', 'init']) AS fork) f
    LEFT JOIN (SELECT fork, count(DISTINCT blkno) AS n, max(blkno) AS maxblk,
                      count(*) AS nrows
               FROM pg_relation_page_checksum_status(rel, include_ok => true)
               GROUP BY fork) s ON s.fork = f.fork
    WHERE coalesce(s.n, 0) <> f.nblocks OR s.nrows <> s.n
       OR coalesce(s.maxblk + 1, 0) <> f.nblocks;
    IF problem IS NOT NULL THEN
        RETURN problem;
    END IF;

    SELECT string_agg(DISTINCT format('%s %s/%s/%s', status, fork,
                                      stored_checksum, calculated_checksum), ', ')
    INTO problem
    FROM pg_relation_page_checksum_status(rel, include_ok => true)
    WHERE status NOT IN ('ok', 'new')
       OR (status = 'ok' AND stored_checksum IS DISTINCT FROM calculated_checksum)
       OR (status = 'new' AND (stored_checksum IS NOT NULL
                               OR calculated_checksum IS NOT NULL));
    IF problem IS NOT NULL THEN
        RETURN problem;
    END IF;

    -- A fork name limits the check to that fork.
    IF EXISTS (SELECT FROM pg_relation_page_checksum_status(rel, 'main', true)
               WHERE fork <> 'main') THEN
        RETURN 'fork filter ignored';
    END IF;

    RETURN 'verified';
END
$$;

CREATE TABLE page_status_rel (id integer PRIMARY KEY, payload text);
INSERT INTO page_status_rel
SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g;
-- VACUUM creates the free space map and visibility map forks.
VACUUM page_status_rel;
CHECKPOINT;

SELECT page_status_relation_check('page_status_rel') AS heap;
SELECT page_status_relation_check('page_status_rel_pkey') AS index;

-- Unlogged relations have an init fork.
CREATE UNLOGGED TABLE page_status_unlogged (id integer PRIMARY KEY);
INSERT INTO page_status_unlogged SELECT generate_series(1, 100);
CHECKPOINT;
SELECT page_status_relation_check('page_status_unlogged') AS unlogged;

-- Temporary relations are read from their file as well.  A local buffer gets
-- its checksum only when it is written out, so a dirty one used to be
-- reported as a mismatch; pages not written yet are new.
CREATE TEMP TABLE page_status_temp AS
SELECT g AS id, repeat('t', 200) AS payload FROM generate_series(1, 2000) g;
SELECT page_status_relation_check('page_status_temp') AS temp_table;
SELECT pg_page_checksum_status('page_status_temp'::regclass, 0)
       IN ('ok', 'new', 'checksums_disabled') AS temp_page_status_known;

-- Empty relations and a NULL relation return no rows.
CREATE TABLE page_status_empty (id integer);
SELECT page_status_relation_check('page_status_empty') AS empty;
SELECT count(*) FROM pg_relation_page_checksum_status(NULL);

-- Argument errors do not depend on data checksums.
SELECT * FROM pg_relation_page_checksum_status('page_status_rel', 'bogus');
CREATE TABLE page_status_parted (id integer) PARTITION BY RANGE (id);
SELECT * FROM pg_relation_page_checksum_status('page_status_parted');
CREATE VIEW page_status_view AS SELECT 1 AS one;
SELECT * FROM pg_relation_page_checksum_status('page_status_view');

-- SELECT privilege on the relation is required.
CREATE ROLE regress_page_status_reader;
SET ROLE regress_page_status_reader;
SELECT * FROM pg_relation_page_checksum_status('page_status_rel');
RESET ROLE;
DROP ROLE regress_page_status_reader;

DROP VIEW page_status_view;
DROP TABLE page_status_rel, page_status_unlogged, page_status_temp,
           page_status_empty, page_status_parted;
DROP FUNCTION page_status_relation_check(regclass);
