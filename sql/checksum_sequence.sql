-- Logical sequence checksums: definition plus last_value/is_called.

CREATE SEQUENCE seq_a;
CREATE SEQUENCE seq_b;

-- Not null, and repeatable while nothing changes.
SELECT pg_sequence_logical_checksum('seq_a') IS NOT NULL AS not_null;
SELECT pg_sequence_logical_checksum('seq_a') = pg_sequence_logical_checksum('seq_a')
       AS repeatable;

-- Identity is name and OID independent: equal definitions and states match.
SELECT pg_sequence_logical_checksum('seq_a') = pg_sequence_logical_checksum('seq_b')
       AS fresh_equal;

-- nextval() changes the checksum.
CREATE TEMP TABLE seq_before AS
    SELECT pg_sequence_logical_checksum('seq_a') AS c;
SELECT nextval('seq_a');
SELECT pg_sequence_logical_checksum('seq_a') <> (SELECT c FROM seq_before)
       AS changed_by_nextval;

-- log_cnt is ignored: nextval() pre-logs values (log_cnt > 0), setval()
-- resets log_cnt to 0, and the two sequences are still equal.
SELECT setval('seq_b', 1, true);
SELECT (SELECT log_cnt FROM seq_a) <> (SELECT log_cnt FROM seq_b) AS log_cnt_differs,
       pg_sequence_logical_checksum('seq_a') = pg_sequence_logical_checksum('seq_b')
       AS equal_after_setval;

-- is_called is part of the state.
SELECT setval('seq_a', 5, true);
SELECT setval('seq_b', 5, false);
SELECT pg_sequence_logical_checksum('seq_a') <> pg_sequence_logical_checksum('seq_b')
       AS is_called_matters;
SELECT setval('seq_b', 5, true);
SELECT pg_sequence_logical_checksum('seq_a') = pg_sequence_logical_checksum('seq_b')
       AS same_state_equal;

-- Every definition parameter is part of the checksum.
CREATE TEMP TABLE seq_defs (param text, changed boolean);
DO $$
DECLARE
    base integer;
    alter_cmd text;
BEGIN
    FOREACH alter_cmd IN ARRAY ARRAY[
        'AS integer', 'START 2', 'INCREMENT 2', 'MINVALUE 0',
        'MAXVALUE 1000', 'CACHE 10', 'CYCLE']
    LOOP
        EXECUTE 'CREATE SEQUENCE seq_def';
        PERFORM setval('seq_def', 5, true);
        base := pg_sequence_logical_checksum('seq_def');
        EXECUTE 'ALTER SEQUENCE seq_def ' || alter_cmd;
        -- ALTER SEQUENCE ... START does not move last_value, but keep the
        -- state identical anyway.
        PERFORM setval('seq_def', 5, true);
        INSERT INTO seq_defs
            VALUES (alter_cmd, pg_sequence_logical_checksum('seq_def') <> base);
        EXECUTE 'DROP SEQUENCE seq_def';
    END LOOP;
END
$$;
SELECT param, changed FROM seq_defs ORDER BY param;

-- A pinned value: the encoding must be identical on every PostgreSQL
-- version and byte order.  Changing it invalidates user baselines.
CREATE SEQUENCE seq_fixed START 10 INCREMENT 5;
SELECT setval('seq_fixed', 42, true);
SELECT pg_sequence_logical_checksum('seq_fixed') AS fnv32,
       pg_sequence_logical_checksum('seq_fixed', 'fnv32') AS fnv32_bytes,
       pg_sequence_logical_checksum('seq_fixed', 'fnv64') AS fnv64_bytes;
SELECT pg_sequence_logical_checksum('seq_fixed', 'fnv32')
       = int4send(pg_sequence_logical_checksum('seq_fixed')) AS fnv32_overload_agrees;

-- Sequences behind serial and identity columns.
CREATE TABLE seq_owner (id serial, ident bigint GENERATED ALWAYS AS IDENTITY, v text);
INSERT INTO seq_owner (v) VALUES ('a'), ('b');
SELECT pg_sequence_logical_checksum(pg_get_serial_sequence('seq_owner', 'id')::regclass)
       IS NOT NULL AS serial_ok,
       pg_sequence_logical_checksum(pg_get_serial_sequence('seq_owner', 'ident')::regclass)
       IS NOT NULL AS identity_ok;

-- Temporary sequences of the own session.
CREATE TEMP SEQUENCE seq_temp;
CREATE SEQUENCE seq_c;
SELECT pg_sequence_logical_checksum('seq_temp') = pg_sequence_logical_checksum('seq_c')
       AS temp_matches_fresh;

-- Errors.
SELECT pg_sequence_logical_checksum('seq_owner');
SELECT pg_sequence_logical_checksum('seq_a', 'md5');
SELECT pg_sequence_logical_checksum(NULL::regclass) IS NULL AS strict_null;

-- SELECT is required; USAGE (enough for nextval) is not.
CREATE ROLE checksum_seq_role;
GRANT USAGE ON SEQUENCE seq_a TO checksum_seq_role;
GRANT SELECT ON SEQUENCE seq_b TO checksum_seq_role;
SET ROLE checksum_seq_role;
SELECT pg_sequence_logical_checksum('seq_a');
SELECT pg_sequence_logical_checksum('seq_b') IS NOT NULL AS select_ok;
RESET ROLE;

DROP OWNED BY checksum_seq_role;
DROP ROLE checksum_seq_role;
DROP TABLE seq_owner;
DROP SEQUENCE seq_a, seq_b, seq_c, seq_fixed;
