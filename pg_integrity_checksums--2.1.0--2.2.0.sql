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
