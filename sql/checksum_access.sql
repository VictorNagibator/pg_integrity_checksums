-- Privilege tests for table, tuple, cell, page and index checksums.
-- Column-level SELECT is deliberately not sufficient: the functions read the
-- whole relation through the storage layer.

CREATE TABLE test_access_t (id integer PRIMARY KEY, v text);
INSERT INTO test_access_t SELECT gs, 'value_' || gs FROM generate_series(1, 100) gs;
CREATE INDEX test_access_v_idx ON test_access_t (v);

CREATE ROLE checksum_access_role LOGIN;
GRANT SELECT (id) ON test_access_t TO checksum_access_role;

SET ROLE checksum_access_role;

DO $$
DECLARE
    denied boolean;
BEGIN
    denied := false;
    BEGIN
        PERFORM pg_table_logical_checksum('test_access_t'::regclass);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'table logical checksum accepted column-only SELECT';
    END IF;

    denied := false;
    BEGIN
        PERFORM pg_table_physical_checksum('test_access_t'::regclass, false);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'table physical checksum accepted column-only SELECT';
    END IF;

    denied := false;
    BEGIN
        PERFORM pg_cell_checksum('test_access_t'::regclass, '(0,1)'::tid, 1);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'cell checksum accepted column-only SELECT';
    END IF;

    denied := false;
    BEGIN
        PERFORM pg_page_checksum('test_access_t'::regclass, 0);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'page checksum accepted column-only SELECT';
    END IF;

    denied := false;
    BEGIN
        PERFORM pg_index_logical_checksum('test_access_v_idx'::regclass);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'index logical checksum accepted column-only SELECT';
    END IF;

    denied := false;
    BEGIN
        PERFORM pg_index_physical_checksum('test_access_v_idx'::regclass);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'index physical checksum accepted column-only SELECT';
    END IF;
END;
$$;

RESET ROLE;
GRANT SELECT ON test_access_t TO checksum_access_role;
SET ROLE checksum_access_role;

SELECT
    pg_table_logical_checksum('test_access_t'::regclass) IS NOT NULL
        AS table_logical_allowed,
    pg_table_physical_checksum('test_access_t'::regclass, false) IS NOT NULL
        AS table_physical_allowed,
    pg_page_checksum('test_access_t'::regclass, 0) IS NOT NULL
        AS page_allowed,
    pg_index_logical_checksum('test_access_v_idx'::regclass) IS NOT NULL
        AS index_logical_allowed,
    pg_index_physical_checksum('test_access_v_idx'::regclass) IS NOT NULL
        AS index_physical_allowed;

SELECT
    pg_tuple_logical_checksum('test_access_t'::regclass, ctid) IS NOT NULL
        AS tuple_logical_allowed,
    pg_cell_checksum('test_access_t'::regclass, ctid, 1) IS NOT NULL
        AS cell_allowed
FROM test_access_t
ORDER BY id
LIMIT 1;

RESET ROLE;
REVOKE SELECT ON test_access_t FROM checksum_access_role;
SET ROLE checksum_access_role;

DO $$
DECLARE
    denied boolean;
BEGIN
    denied := false;
    BEGIN
        PERFORM pg_table_logical_checksum('test_access_t'::regclass);
    EXCEPTION WHEN insufficient_privilege THEN
        denied := true;
    END;
    IF NOT denied THEN
        RAISE EXCEPTION 'revoked SELECT was not enforced';
    END IF;
END;
$$;

RESET ROLE;
DROP OWNED BY checksum_access_role;
DROP ROLE checksum_access_role;
DROP TABLE test_access_t;
