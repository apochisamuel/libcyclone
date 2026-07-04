/*
 * header.c — Phase 1: File header and table directory parsing
 *
 * Reads the CYC file header (magic, version, flags, table count)
 * and the table directory entries that describe the location and
 * size of each data section within the file.
 *
 * Reference: CYC Format Specification §2.1 - File Header
 */

#include "internal.h"

/* ── Header field validation ────────────────────────────────────── */

static int cy_validate_magic(uint32_t magic, cy_context *ctx)
{
    if (magic != CY_MAGIC) {
        cy_set_error(ctx, CY_ERR_INVALID_MAGIC,
                     "invalid magic: 0x%08x (expected 0x%08x)",
                     magic, CY_MAGIC);
        return CY_ERR_INVALID_MAGIC;
    }
    return CY_OK;
}

static int cy_validate_version(uint32_t version, cy_context *ctx)
{
    if (version == 0 || version > CY_FORMAT_VERSION) {
        cy_set_error(ctx, CY_ERR_INVALID_VERSION,
                     "unsupported version: %u (max supported: %u)",
                     version, CY_FORMAT_VERSION);
        return CY_ERR_INVALID_VERSION;
    }
    return CY_OK;
}

static int cy_validate_table_count(uint32_t count, cy_context *ctx)
{
    if (count == 0) {
        cy_set_error(ctx, CY_ERR_BAD_COUNT,
                     "file contains no table directory entries");
        return CY_ERR_BAD_COUNT;
    }
    if (count > CY_MAX_TABLES) {
        cy_set_error(ctx, CY_ERR_BAD_COUNT,
                     "table count %u exceeds maximum %u",
                     count, CY_MAX_TABLES);
        return CY_ERR_BAD_COUNT;
    }
    return CY_OK;
}

/* ── Checksum computation ───────────────────────────────────────── */

static uint32_t cy_compute_checksum(const uint8_t *data, size_t size,
                                    size_t checksum_offset)
{
    uint32_t sum = 0;
    size_t   i;

    for (i = 0; i < size; i++) {
        /* Skip the checksum field itself (4 bytes at checksum_offset) */
        if (i >= checksum_offset && i < checksum_offset + 4)
            continue;
        sum = (sum << 1) | (sum >> 31);  /* rotate left */
        sum += data[i];
    }
    return sum;
}

/* ── Table tag validation ───────────────────────────────────────── */

static int cy_is_known_tag(uint32_t tag)
{
    switch (tag) {
    case CY_TAG_STR:
    case CY_TAG_TYP:
    case CY_TAG_NOD:
    case CY_TAG_PRT:
    case CY_TAG_EDG:
    case CY_TAG_PRM:
    case CY_TAG_SCH:
    case CY_TAG_GRP:
        return 1;
    default:
        return 0;
    }
}

static const char *cy_tag_name(uint32_t tag)
{
    switch (tag) {
    case CY_TAG_STR: return "STR (strings)";
    case CY_TAG_TYP: return "TYP (types)";
    case CY_TAG_NOD: return "NOD (nodes)";
    case CY_TAG_PRT: return "PRT (ports)";
    case CY_TAG_EDG: return "EDG (edges)";
    case CY_TAG_PRM: return "PRM (params)";
    case CY_TAG_SCH: return "SCH (schedule)";
    case CY_TAG_GRP: return "GRP (groups)";
    default:         return "unknown";
    }
}

/* ── Duplicate tag detection ────────────────────────────────────── */

static int cy_check_duplicate_tags(const cy_table_dir *dirs,
                                   uint32_t count, cy_context *ctx)
{
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (dirs[i].tag == dirs[j].tag) {
                cy_set_error(ctx, CY_ERR_DUPLICATE_TABLE,
                             "duplicate table tag %s at directory "
                             "indices %u and %u",
                             cy_tag_name(dirs[i].tag), i, j);
                return CY_ERR_DUPLICATE_TABLE;
            }
        }
    }
    return CY_OK;
}

/* ── Table overlap detection ────────────────────────────────────── */

static int cy_check_table_overlaps(const cy_table_dir *dirs,
                                   uint32_t count, cy_context *ctx)
{
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        uint64_t a_start = dirs[i].offset;
        uint64_t a_end   = a_start + dirs[i].length;

        for (j = i + 1; j < count; j++) {
            uint64_t b_start = dirs[j].offset;
            uint64_t b_end   = b_start + dirs[j].length;

            if (a_start < b_end && b_start < a_end) {
                cy_set_error(ctx, CY_ERR_BAD_OFFSET,
                             "table sections %s and %s overlap: "
                             "[%u,%u) and [%u,%u)",
                             cy_tag_name(dirs[i].tag),
                             cy_tag_name(dirs[j].tag),
                             (uint32_t)a_start, (uint32_t)a_end,
                             (uint32_t)b_start, (uint32_t)b_end);
                return CY_ERR_BAD_OFFSET;
            }
        }
    }
    return CY_OK;
}

/* ── Required table check ───────────────────────────────────────── */

static int cy_check_required_tables(const cy_table_dir *dirs,
                                    uint32_t count, cy_context *ctx)
{
    uint32_t required_tags[] = {
        CY_TAG_STR, CY_TAG_TYP, CY_TAG_NOD, CY_TAG_PRT, CY_TAG_EDG
    };
    uint32_t required_count = sizeof(required_tags) / sizeof(required_tags[0]);
    uint32_t ri, di;
    int      found;

    for (ri = 0; ri < required_count; ri++) {
        found = 0;
        for (di = 0; di < count; di++) {
            if (dirs[di].tag == required_tags[ri]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            cy_set_error(ctx, CY_ERR_MISSING_TABLE,
                         "required table %s not found in directory",
                         cy_tag_name(required_tags[ri]));
            return CY_ERR_MISSING_TABLE;
        }
    }
    return CY_OK;
}

/* ── Sort directory by canonical parse order ────────────────────── */

static int cy_tag_order(uint32_t tag)
{
    switch (tag) {
    case CY_TAG_STR: return 0;
    case CY_TAG_TYP: return 1;
    case CY_TAG_NOD: return 2;
    case CY_TAG_PRT: return 3;
    case CY_TAG_EDG: return 4;
    case CY_TAG_PRM: return 5;
    case CY_TAG_SCH: return 6;
    case CY_TAG_GRP: return 7;
    default:         return 99;
    }
}

static void cy_sort_directory(cy_table_dir *dirs, uint32_t count)
{
    uint32_t     i, j;
    cy_table_dir tmp;

    /* Simple insertion sort; count is small (max 16) */
    for (i = 1; i < count; i++) {
        tmp = dirs[i];
        j = i;
        while (j > 0 && cy_tag_order(dirs[j - 1].tag) >
                         cy_tag_order(tmp.tag)) {
            dirs[j] = dirs[j - 1];
            j--;
        }
        dirs[j] = tmp;
    }
}

/* ── Main header parse function ─────────────────────────────────── */

int cy_parse_header(cy_reader *r, cy_pipeline *pipe,
                    cy_table_dir *dirs, uint32_t *dir_count)
{
    uint32_t magic, version, flags, table_count;
    uint32_t total_size, checksum, reserved0, reserved1;
    uint32_t dir_space;
    uint32_t computed_checksum;
    uint32_t i;
    int      rc;

    /* Read 32-byte file header */
    magic       = cy_read_u32(r);
    version     = cy_read_u32(r);
    flags       = cy_read_u32(r);
    table_count = cy_read_u32(r);
    total_size  = cy_read_u32(r);
    checksum    = cy_read_u32(r);
    reserved0   = cy_read_u32(r);
    reserved1   = cy_read_u32(r);

    if (r->ctx->error_code != CY_OK)
        return r->ctx->error_code;

    /* Validate header fields */
    rc = cy_validate_magic(magic, r->ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_validate_version(version, r->ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_validate_table_count(table_count, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Verify total size against actual data */
    if (total_size > r->size) {
        cy_add_warning(r->ctx,
                       "header total_size %u exceeds actual data size %zu",
                       total_size, r->size);
    }

    /* Verify checksum if the flag is set */
    if (flags & CY_FLAG_CHECKSUM) {
        computed_checksum = cy_compute_checksum(r->data, r->size, 20);
        if (computed_checksum != checksum) {
            cy_set_error(r->ctx, CY_ERR_CHECKSUM,
                         "checksum mismatch: computed 0x%08x, "
                         "stored 0x%08x",
                         computed_checksum, checksum);
            return CY_ERR_CHECKSUM;
        }
    }

    /* Check space for table directory */
    if (cy_safe_multiply(table_count, CY_TABLE_DIR_ENTRY_SIZE,
                         &dir_space) != 0) {
        cy_set_error(r->ctx, CY_ERR_OVERFLOW,
                     "table directory size overflow");
        return CY_ERR_OVERFLOW;
    }

    if (!cy_reader_available(r, dir_space)) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "table directory truncated: need %u bytes at "
                     "offset %zu",
                     dir_space, cy_reader_pos(r));
        return CY_ERR_TRUNCATED;
    }

    /* Read table directory entries */
    for (i = 0; i < table_count; i++) {
        dirs[i].tag    = cy_read_u32(r);
        dirs[i].offset = cy_read_u32(r);
        dirs[i].length = cy_read_u32(r);
        dirs[i].count  = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Validate section bounds */
        rc = cy_validate_range(r, dirs[i].offset, dirs[i].length,
                               cy_tag_name(dirs[i].tag));
        if (rc != CY_OK)
            return rc;

        if (!cy_is_known_tag(dirs[i].tag)) {
            cy_add_warning(r->ctx,
                           "unknown table tag 0x%08x at index %u, "
                           "offset %u",
                           dirs[i].tag, i, dirs[i].offset);
        }
    }

    /* Structural validation */
    rc = cy_check_duplicate_tags(dirs, table_count, r->ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_check_table_overlaps(dirs, table_count, r->ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_check_required_tables(dirs, table_count, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Sort by canonical parse order */
    cy_sort_directory(dirs, table_count);

    /* Store metadata in pipeline */
    pipe->version       = version;
    pipe->flags         = flags;
    pipe->checksum      = checksum;
    pipe->latency_budget = CY_LATENCY_BUDGET_DEFAULT;

    *dir_count = table_count;

    (void)reserved0;
    (void)reserved1;

    return CY_OK;
}
