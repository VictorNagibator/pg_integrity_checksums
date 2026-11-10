-- Upgrade pg_integrity_checksums from 2.1.0 to 2.2.0.
-- Adds the generic checksum algorithm overloads and the digest conversion
-- helper. Existing 32-bit functions are unchanged.

CREATE OR REPLACE FUNCTION pg_data_checksum(bytea, bigint, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_data_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_data_checksum(bytea, bigint, text) IS
'Generic binary data checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes, both big-endian.';

CREATE OR REPLACE FUNCTION pg_text_checksum(text, bigint, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_text_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_text_checksum(text, bigint, text) IS
'Generic text checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes, both big-endian.';

CREATE OR REPLACE FUNCTION pg_checksum_bigint(bytea)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_checksum_bigint'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_checksum_bigint(bytea) IS
'Convert a 4- or 8-byte checksum digest to signed bigint. Four-byte values are sign-extended to match the legacy int32 functions.';

CREATE OR REPLACE FUNCTION pg_table_logical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_table_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_logical_checksum(regclass, text) IS
'Generic logical table checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_index_logical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_index_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_logical_checksum(regclass, text) IS
'Generic logical index checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_cell_checksum(regclass, tid, integer, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cell_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer, text) IS
'Generic logical cell checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_tuple_logical_checksum(regclass, tid, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_tuple_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_logical_checksum(regclass, tid, text) IS
'Generic logical tuple checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_database_logical_checksum(boolean, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_database_logical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_logical_checksum(boolean, boolean, text) IS
'Generic logical database checksum. Requires superuser. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_checksum_bigint(bytea, boolean)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_checksum_bigint'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_checksum_bigint(bytea, boolean) IS
'Convert a digest to bigint. The second argument selects unsigned interpretation for 4-byte fnv32 digests.';

CREATE OR REPLACE FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_tuple_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_tuple_physical_checksum(regclass, tid, boolean, text) IS
'Generic physical tuple checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_table_physical_checksum(regclass, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_table_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_table_physical_checksum(regclass, boolean, text) IS
'Generic physical table checksum. Returns an 8-byte digest for fnv32 and fnv64.';

CREATE OR REPLACE FUNCTION pg_index_physical_checksum(regclass, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_index_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_index_physical_checksum(regclass, text) IS
'Generic physical index checksum. Algorithm fnv32 returns 4 bytes, fnv64 returns 8 bytes.';

CREATE OR REPLACE FUNCTION pg_database_physical_checksum(boolean, boolean, text)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_database_physical_checksum_algo'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_database_physical_checksum(boolean, boolean, text) IS
'Generic physical database checksum. Requires superuser. Returns an 8-byte digest for fnv32 and fnv64.';

COMMENT ON FUNCTION pg_cell_checksum(regclass, tid, integer) IS
'Logical checksum of a cell value. Returns -1 (0xFFFFFFFF, CHECKSUM_NULL)
 for NULL values. Same value in same column always produces the same checksum.
 Independent of physical storage.';

COMMENT ON FUNCTION pg_page_checksum(regclass, integer) IS
'Calculates PostgreSQL''s built-in 16-bit page checksum. This function does not
 compare the result with the stored pd_checksum. It returns 0 for new
 (uninitialized) pages, so a zero result is not proof of a valid page. The
 block number is validated; an out-of-range block raises an error instead of
 extending the relation.';
