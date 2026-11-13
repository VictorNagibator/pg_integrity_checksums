-- Logical checksums must not run user code under a buffer content lock.
--
-- A logical index checksum evaluates the index expressions on every visible
-- heap row; logical table, tuple and cell checksums detoast values and call
-- type send functions.  While a backend holds a buffer content lock its
-- interrupts are held off, so such code used to be uncancellable: a slow
-- expression ignored statement_timeout until it finished.  The scans now
-- keep only a pin while they hash.

CREATE FUNCTION buffer_locks_slow_key(integer) RETURNS integer
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    IF current_setting('pg_integrity_checksums_test.slow', true) = 'on' THEN
        PERFORM pg_sleep(20);
    END IF;
    RETURN $1;
END
$$;

CREATE TABLE buffer_locks (id integer);
CREATE INDEX buffer_locks_slow ON buffer_locks (buffer_locks_slow_key(id));
INSERT INTO buffer_locks VALUES (1);

SET pg_integrity_checksums_test.slow = on;

-- The checksum must be cancelled long before the 20 second sleep ends.
-- (statement_timeout is armed when a statement starts, so set it outside.)
SET statement_timeout = '300ms';
DO $$
DECLARE
    started timestamptz := clock_timestamp();
BEGIN
    PERFORM pg_index_logical_checksum('buffer_locks_slow'::regclass);
    RAISE NOTICE 'checksum was not cancelled';
EXCEPTION WHEN query_canceled THEN
    RAISE NOTICE 'cancelled promptly: %',
        clock_timestamp() - started < interval '10 seconds';
END
$$;
RESET statement_timeout;

RESET pg_integrity_checksums_test.slow;

-- Without the delay the checksum is computed as usual.
SELECT pg_index_logical_checksum('buffer_locks_slow'::regclass) IS NOT NULL
    AS has_checksum;

DROP TABLE buffer_locks;
DROP FUNCTION buffer_locks_slow_key(integer);
