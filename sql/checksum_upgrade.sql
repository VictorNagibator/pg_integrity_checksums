-- The upgrade path must produce the same SQL objects as a fresh install.
--
-- Install the oldest supported version, update to the default version and
-- compare every function's signature, attributes, privileges and comment
-- with a fresh CREATE EXTENSION.  Also check the attributes promised for the
-- pure utility functions.
--
-- This test drops and re-creates the extension, so it runs last.

CREATE TEMP VIEW checksum_functions AS
SELECT p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')'
           AS signature,
       p.prorettype::regtype::text AS returns,
       p.provolatile,
       p.proparallel,
       p.proisstrict,
       p.prosrc,
       coalesce(p.proacl::text, 'default') AS acl,
       coalesce(obj_description(p.oid, 'pg_proc'), '') AS comment
FROM pg_proc p
JOIN pg_depend d ON d.classid = 'pg_proc'::regclass AND d.objid = p.oid
JOIN pg_extension e ON e.oid = d.refobjid
WHERE e.extname = 'pg_integrity_checksums' AND d.deptype = 'e';

-- Fresh install of the default version.
CREATE TEMP TABLE fresh AS SELECT * FROM checksum_functions;

-- The whole upgrade chain from the oldest supported version.
DROP EXTENSION pg_integrity_checksums;
CREATE EXTENSION pg_integrity_checksums VERSION '2.0.0';
ALTER EXTENSION pg_integrity_checksums UPDATE;

SELECT extversion = (SELECT default_version FROM pg_available_extensions
                     WHERE name = 'pg_integrity_checksums')
       AS updated_to_default
FROM pg_extension WHERE extname = 'pg_integrity_checksums';

-- Differences in either direction; no rows means the paths agree.
(SELECT 'only after upgrade' AS side, * FROM checksum_functions
 EXCEPT SELECT 'only after upgrade', * FROM fresh)
UNION ALL
(SELECT 'only in fresh install', * FROM fresh
 EXCEPT SELECT 'only in fresh install', * FROM checksum_functions)
ORDER BY 2, 1;

SELECT count(*) > 0 AS has_functions FROM fresh;

-- Pure utility functions are IMMUTABLE and PARALLEL SAFE; everything that
-- reads relations stays VOLATILE and PARALLEL UNSAFE.
SELECT signature, provolatile, proparallel
FROM fresh
WHERE signature ~ '^pg_(data|text)_checksum|^pg_checksum_bigint'
   OR provolatile <> 'v' OR proparallel <> 'u'
ORDER BY signature;

-- Leave the default version installed, as other tests expect.
DROP EXTENSION pg_integrity_checksums;
CREATE EXTENSION pg_integrity_checksums;
