-- Generic checksum API and FNV-1a 64-bit primitives.

SELECT encode(pg_data_checksum('hello'::bytea, 0, 'fnv32'), 'hex') = '4f9f2cab' AS fnv32_ok;

SELECT encode(pg_data_checksum('hello'::bytea, 0, 'fnv64'), 'hex') = 'a430d84680aabd0b' AS fnv64_ok;

SELECT pg_checksum_bigint(pg_data_checksum('hello'::bytea, 0, 'fnv64')) = -6615550055289275125 AS bigint_ok;

SELECT pg_checksum_bigint(pg_data_checksum('hello'::bytea, 0, 'fnv32')) = pg_data_checksum('hello'::bytea, 0) AS legacy_ok;

SELECT length(pg_data_checksum('hello'::bytea, 0, 'fnv32')) = 4 AND length(pg_data_checksum('hello'::bytea, 0, 'fnv64')) = 8 AS lengths_ok;

SELECT encode(pg_text_checksum('hello', 0, 'fnv64'), 'hex') = encode(pg_data_checksum('hello'::bytea, 0, 'fnv64'), 'hex') AS text_ok;
