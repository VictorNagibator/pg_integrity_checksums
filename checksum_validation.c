/*-------------------------------------------------------------------------
 *
 * checksum_validation.c
 *    Relation, page and tuple validation used by all checksum levels
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/gist.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
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
 * pg_integrity_checksums_mask_page - copy a page without its unstable state
 *
 * A page in shared buffers carries state that changes although nothing about
 * its content changed, so hashing the raw bytes made physical checksums
 * depend on who read the page first and on whether it was evicted:
 *
 * - pd_checksum is set on a private copy when the buffer is written out
 *   (PageSetChecksumCopy), so the value in shared buffers is stale until the
 *   page is evicted and read again;
 * - pd_lsn moves when setting a hint bit emits a full-page image;
 * - pd_prune_xid and the pd_flags hint bits are set and cleared
 *   opportunistically;
 * - index scans mark line pointers LP_DEAD and set "page has garbage" flags
 *   (B-tree, hash, GiST) under a share lock, as hints;
 * - heap (TOAST) tuples get their infomask hint bits set by readers.
 *
 * Copy BLCKSZ bytes of "page" into "dst" and clear that state in the copy.
 * Bits outside PD_VALID_FLAG_BITS are kept, so a corrupt header still changes
 * the checksum.  Line pointers and tuples are only touched when the header
 * passes the sanity checks; otherwise the raw copy is hashed.
 */
void
pg_integrity_checksums_mask_page(Page page, char *dst, ChecksumPageKind kind,
                                 Oid relam)
{
    PageHeader  phdr = (PageHeader) dst;
    OffsetNumber maxoff;
    OffsetNumber offnum;

    memcpy(dst, page, BLCKSZ);

    if (PageIsNew((Page) dst))
        return;

    memset(&phdr->pd_lsn, 0, sizeof(phdr->pd_lsn));
    phdr->pd_checksum = 0;
    phdr->pd_prune_xid = InvalidTransactionId;
    phdr->pd_flags &= ~PD_VALID_FLAG_BITS;

    if (!pg_integrity_checksums_page_header_sane(page))
        return;

    maxoff = PageGetMaxOffsetNumber((Page) dst);

    for (offnum = FirstOffsetNumber; offnum <= maxoff;
         offnum = OffsetNumberNext(offnum))
    {
        ItemId      lp = PageGetItemId((Page) dst, offnum);

        if (kind == CHECKSUM_PAGE_INDEX)
        {
            /*
             * In an index, LP_DEAD only says "known dead to everyone"; the
             * tuple stays in place until a cleanup removes it.  (In a heap,
             * LP_DEAD is a pruning result and is real state.)
             */
            if (ItemIdIsDead(lp) && ItemIdHasStorage(lp))
                lp->lp_flags = LP_NORMAL;
        }
        else if (ItemIdIsNormal(lp) &&
                 pg_integrity_checksums_item_is_sane((Page) dst, lp) &&
                 ItemIdGetLength(lp) >= SizeofHeapTupleHeader)
        {
            HeapTupleHeader htup = (HeapTupleHeader) PageGetItem((Page) dst, lp);

            htup->t_infomask &= ~HEAP_HINT_BITS;
        }
    }

    if (kind != CHECKSUM_PAGE_INDEX)
        return;

    /* Access-method "page has garbage" hints live in the special space. */
    if (relam == BTREE_AM_OID &&
        PageGetSpecialSize((Page) dst) == MAXALIGN(sizeof(BTPageOpaqueData)))
    {
        BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer((Page) dst);

        opaque->btpo_flags &= ~BTP_HAS_GARBAGE;
    }
    else if (relam == HASH_AM_OID &&
             PageGetSpecialSize((Page) dst) == MAXALIGN(sizeof(HashPageOpaqueData)))
    {
        HashPageOpaque opaque = (HashPageOpaque) PageGetSpecialPointer((Page) dst);

        opaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
    }
    else if (relam == GIST_AM_OID &&
             PageGetSpecialSize((Page) dst) == MAXALIGN(sizeof(GISTPageOpaqueData)))
    {
        GISTPageOpaque opaque = (GISTPageOpaque) PageGetSpecialPointer((Page) dst);

        opaque->flags &= ~F_HAS_GARBAGE;
    }
}

/*
 * scan_relation_raw_pages_common - checksum a relation as a sequence of raw pages
 *
 * Used for TOAST relations, which are checksummed page by page rather than
 * through the visible-tuple path.  Pages are hashed through
 * pg_integrity_checksums_mask_page(), so hint bits and the stale in-buffer
 * pd_checksum do not leak into the result.  Only the accumulator that
 * matches "algo" is used.
 */
static void
scan_relation_raw_pages_common(Relation rel, BlockNumber start,
                               BlockNumber end, ChecksumAlgo algo,
                               ChecksumAccum *acc32, ChecksumAccum64 *acc64)
{
    BlockNumber blkno;
    BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);
    PGAlignedBlock masked;

    for (blkno = start; blkno < end; blkno++)
    {
        Buffer      buffer;
        uint64      page_hash;

        buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                                    bstrategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        pg_integrity_checksums_mask_page(BufferGetPage(buffer), masked.data,
                                         CHECKSUM_PAGE_HEAP, InvalidOid);
        page_hash = hash_bytes_algo(0, masked.data, BLCKSZ, algo);
        page_hash = combine_hash(page_hash, blkno, algo);

        if (algo == CHECKSUM_ALGO_FNV32)
            checksum_accum_add(acc32, (uint32) page_hash);
        else
            checksum_accum64_add(acc64, page_hash);

        UnlockReleaseBuffer(buffer);

        if ((blkno & 63) == 0)
            CHECK_FOR_INTERRUPTS();
    }

    FreeAccessStrategy(bstrategy);
}

/*
 * scan_relation_raw_pages - 32-bit raw-page checksum of a block range
 */
void
scan_relation_raw_pages(Relation rel, BlockNumber start, BlockNumber end,
                        ChecksumAccum *acc)
{
    scan_relation_raw_pages_common(rel, start, end, CHECKSUM_ALGO_FNV32, acc,
                                   NULL);
}

/*
 * scan_relation_raw_pages64 - 64-bit raw-page checksum of a block range
 */
void
scan_relation_raw_pages64(Relation rel, BlockNumber start, BlockNumber end,
                          ChecksumAccum64 *acc)
{
    scan_relation_raw_pages_common(rel, start, end, CHECKSUM_ALGO_FNV64, NULL,
                                   acc);
}
