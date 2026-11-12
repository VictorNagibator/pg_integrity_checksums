-- Upgrade pg_integrity_checksums from 2.3.0 to 2.4.0.
--
-- The generic database-level overloads (with the algorithm argument) were
-- executable by PUBLIC, unlike the 32-bit functions.  They already required a
-- superuser in C; revoke the default grant as well, for consistency.
--
-- The C library also changes how some logical checksums are computed; see
-- the Changes file and "Upgrading to 2.4" in docs/installation.md.  Install
-- the new library before running this script (a normal package upgrade does).

REVOKE EXECUTE ON FUNCTION pg_database_logical_checksum(boolean, boolean, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_database_physical_checksum(boolean, boolean, text) FROM PUBLIC;
