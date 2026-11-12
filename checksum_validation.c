/*-------------------------------------------------------------------------
 *
 * checksum_validation.c
 *    Relation, page and tuple validation used by all checksum levels
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/syscache.h"

#include "checksum_internal.h"

/*-------------------------------------------------------------------------
 * Relation, page and tuple validation helpers
 *-------------------------------------------------------------------------
 */

/*
 * pg_integrity_checksums_require_select - enforce SELECT privilege and RLS
 *
 * The checksum functions read relation storage directly, without going
 * through the executor.  They must therefore perform the same privilege
 * check the executor would perform, and they cannot apply row-level
 * security policies: a relation protected by RLS must be rejected rather
 * than scanned behind the policy's back.
 *
 * Indexes carry no ACL of their own, so the privilege check for an index
 * is performed on the underlying heap relation.
 */
void
pg_integrity_checksums_require_select(Oid reloid)
{
    Oid         acl_relid = reloid;
    char        relkind = get_rel_relkind(reloid);
    AclResult   aclresult;
    ObjectType  objtype = OBJECT_TABLE;
    char       *relname;

    if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
        acl_relid = IndexGetRelation(reloid, false);

    aclresult = pg_class_aclcheck(acl_relid, GetUserId(), ACL_SELECT);
    if (aclresult != ACLCHECK_OK)
    {
        relname = get_rel_name(acl_relid);
        if (relname == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("relation with OID %u does not exist", acl_relid)));

        if (get_rel_relkind(acl_relid) == RELKIND_SEQUENCE)
            objtype = OBJECT_SEQUENCE;
        aclcheck_error(aclresult, objtype, relname);
    }

    /*
     * check_enable_rls() accounts for superusers, roles with BYPASSRLS and
     * table owners (including FORCE ROW LEVEL SECURITY).
     */
    if (check_enable_rls(acl_relid, GetUserId(), true) == RLS_ENABLED)
    {
        relname = get_rel_name(acl_relid);
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("cannot compute checksum of relation \"%s\" because row-level security is enabled",
                        relname ? relname : "?"),
                 errhint("Use a role with BYPASSRLS, the table owner, or disable RLS for this operation.")));
    }
}

/*
 * pg_integrity_checksums_check_relation_kind - reject unsupported relation kinds
 *
 * Ordinary tables and materialized views always pass.  Partitioned tables
 * and TOAST relations pass only when the caller says it can handle them;
 * indexes, sequences and every other kind raise an error.
 */
void
pg_integrity_checksums_check_relation_kind(Relation rel, bool allow_toast,
                                 bool allow_partitioned)
{
    char        relkind = rel->rd_rel->relkind;

    if (relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW)
        return;

    if (relkind == RELKIND_PARTITIONED_TABLE)
    {
        if (allow_partitioned)
            return;

        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("partitioned table \"%s\" has no storage of its own",
                        RelationGetRelationName(rel))));
    }

    if (relkind == RELKIND_TOASTVALUE)
    {
        if (allow_toast)
            return;

        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("TOAST relation \"%s\" is not a heap relation",
                        RelationGetRelationName(rel))));
    }

    switch (relkind)
    {
        case RELKIND_INDEX:
        case RELKIND_PARTITIONED_INDEX:
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("relation \"%s\" is an index, not a table",
                            RelationGetRelationName(rel))));
            break;
        case RELKIND_SEQUENCE:
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("relation \"%s\" is a sequence, not a table",
                            RelationGetRelationName(rel))));
            break;
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("relation \"%s\" is not a table with storage",
                            RelationGetRelationName(rel))));
            break;
    }
}

/*
 * pg_integrity_checksums_relation_has_storage - can pg_page_checksum() read pages?
 */
bool
pg_integrity_checksums_relation_has_storage(Relation rel)
{
    switch (rel->rd_rel->relkind)
    {
        case RELKIND_RELATION:
        case RELKIND_MATVIEW:
        case RELKIND_INDEX:
        case RELKIND_SEQUENCE:
        case RELKIND_TOASTVALUE:
            return true;
        default:
            return false;
    }
}

/*
 * pg_integrity_checksums_page_header_sane - cheap structural validation of a page
 *
 * This mirrors the header checks of PageIsVerified(), without the checksum
 * part.  It is used before following line pointers, so that a corrupted
 * page produces a clean error instead of crashing the backend that is
 * supposed to detect the corruption.
 */
bool
pg_integrity_checksums_page_header_sane(Page page)
{
    PageHeader  phdr = (PageHeader) page;

    return (phdr->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
        phdr->pd_lower >= SizeOfPageHeaderData &&
        phdr->pd_lower <= phdr->pd_upper &&
        phdr->pd_upper <= phdr->pd_special &&
        phdr->pd_special <= BLCKSZ &&
        phdr->pd_special == MAXALIGN(phdr->pd_special);
}

/*
 * pg_integrity_checksums_check_page - raise a clean error for an insane page header
 */
void
pg_integrity_checksums_check_page(Relation rel, BlockNumber blkno, Page page)
{
    if (PageIsNew(page))
        return;

    if (!pg_integrity_checksums_page_header_sane(page))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("corrupt page header in relation \"%s\" at block %u",
                        RelationGetRelationName(rel), blkno)));
}

/*
 * pg_integrity_checksums_item_is_sane - validate an item pointer against the page
 *
 * Besides ItemIdIsNormal(), check that the tuple lies between pd_upper and
 * pd_special and that its length does not run past the end of that area.
 */
bool
pg_integrity_checksums_item_is_sane(Page page, ItemId lp)
{
    PageHeader  phdr = (PageHeader) page;
    char       *item;
    Size        len;

    if (!ItemIdIsNormal(lp))
        return false;

    len = ItemIdGetLength(lp);
    item = (char *) PageGetItem(page, lp);

    if (item < (char *) page + phdr->pd_upper)
        return false;
    if (item + len > (char *) page + phdr->pd_special)
        return false;

    return true;
}

/*
 * pg_integrity_checksums_check_heap_tuple - validate the parts of a heap tuple header
 * that the checksum routines are about to follow.
 */
void
pg_integrity_checksums_check_heap_tuple(Relation rel, BlockNumber blkno,
                              OffsetNumber offnum, HeapTupleHeader tuple,
                              Size len)
{
    if (len < SizeofHeapTupleHeader ||
        tuple->t_hoff < SizeofHeapTupleHeader ||
        tuple->t_hoff > len)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("corrupt tuple header in relation \"%s\" at block %u, offset %u",
                        RelationGetRelationName(rel), blkno, offnum)));
}

/*
 * scan_relation_raw_pages - checksum a relation as a sequence of raw pages
 *
 * Used for TOAST relations, whose tuples are not HeapTupleHeader structures
 * and therefore cannot go through the regular heap tuple path.
 */
void
scan_relation_raw_pages(Relation rel, BlockNumber start, BlockNumber end,
                        ChecksumAccum *acc)
{
    BlockNumber blkno;
    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        uint32      page_hash;

        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                                    bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        page_hash = fnv1a_32_hash((const unsigned char *) page, BLCKSZ, 0);
        page_hash = combine_checksums(page_hash, blkno);
        checksum_accum_add(acc, page_hash);

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    FreeAccessStrategy(bstrategy);
}

void
scan_relation_raw_pages64(Relation rel, BlockNumber start, BlockNumber end,
                        ChecksumAccum64 *acc)
{
    BlockNumber blkno;
    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        Page        page;
        uint64      page_hash;

        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                                    bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buffer);

        page_hash = fnv1a_64_hash((const unsigned char *) page, BLCKSZ, 0);
        page_hash = combine_checksums_64(page_hash, blkno);
        checksum_accum64_add(acc, page_hash);

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    FreeAccessStrategy(bstrategy);
}
