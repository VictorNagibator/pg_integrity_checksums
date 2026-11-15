-- Index expressions and partial-index predicates are user code.  A logical
-- index checksum must evaluate them as the owner of the table in a
-- security-restricted operation, never with the caller's privileges:
-- otherwise any role that can create a table and an index could run SQL as
-- the superuser who computes a database checksum.

CREATE ROLE regress_pic_owner;
CREATE SCHEMA pic_sec AUTHORIZATION regress_pic_owner;

SET ROLE regress_pic_owner;

-- Declared IMMUTABLE so it can be indexed, then made VOLATILE (PostgreSQL
-- does not re-check existing indexes), so that it may run commands.
CREATE FUNCTION pic_sec.trap(int) RETURNS int
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    IF (SELECT rolsuper FROM pg_roles WHERE rolname = current_user) THEN
        EXECUTE 'ALTER ROLE regress_pic_owner SUPERUSER';
    END IF;
    RAISE NOTICE 'index expression runs as %', current_user;
    RETURN $1;
END
$$;

CREATE TABLE pic_sec.t (a int);
INSERT INTO pic_sec.t VALUES (1);
CREATE INDEX pic_sec_t_expr ON pic_sec.t (pic_sec.trap(a));
CREATE INDEX pic_sec_t_pred ON pic_sec.t (a) WHERE pic_sec.trap(a) > 0;
ALTER FUNCTION pic_sec.trap(int) VOLATILE;

RESET ROLE;

-- Single index, serial and parallel, 32- and 64-bit.
SELECT pg_index_logical_checksum('pic_sec.pic_sec_t_expr') IS NOT NULL AS expr_ok;
SELECT pg_index_logical_checksum('pic_sec.pic_sec_t_pred', 'fnv64') IS NOT NULL AS pred_ok;
SET pg_integrity_checksums.workers = 2;
SELECT pg_index_logical_checksum('pic_sec.pic_sec_t_expr') IS NOT NULL AS parallel_ok;
RESET pg_integrity_checksums.workers;

-- The database checksum hashes a table and its indexes in one pass.
SELECT pg_database_logical_checksum() IS NOT NULL AS database_ok;

SELECT rolsuper AS escalated FROM pg_roles WHERE rolname = 'regress_pic_owner';

-- The caller's identity is restored afterwards.
SELECT current_user = session_user AS identity_restored;

-- SET inside user code does not leak into the caller's session.
SET ROLE regress_pic_owner;
CREATE FUNCTION pic_sec.setter(int) RETURNS int
LANGUAGE plpgsql IMMUTABLE AS $$
BEGIN
    PERFORM set_config('work_mem', '1234kB', false);
    RETURN $1;
END
$$;
CREATE INDEX pic_sec_t_setter ON pic_sec.t (pic_sec.setter(a));
RESET ROLE;
SET work_mem = '4MB';
SELECT pg_index_logical_checksum('pic_sec.pic_sec_t_setter') IS NOT NULL AS setter_ok;
SHOW work_mem;

DROP SCHEMA pic_sec CASCADE;
DROP ROLE regress_pic_owner;
