/*
 * strings.c — Phase 2: String table parsing
 *
 * The string table stores interned name strings for nodes, ports,
 * types, parameters, and groups. Each entry is a record with an
 * offset into the string data section and a length. Strings are
 * stored as raw bytes (UTF-8 expected) and resolved to pointers
 * during parsing. Later phases reference strings by index.
 *
 * Reference: CYC Format Specification §3.1 - String Table
 */

#include "internal.h"

/* ── String encoding validation ─────────────────────────────────── */

static int cy_validate_utf8_byte(uint8_t b, int *remaining)
{
    if (*remaining > 0) {
        if ((b & 0xC0) != 0x80)
            return 0;
        (*remaining)--;
        return 1;
    }

    if (b < 0x80) {
        *remaining = 0;
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        *remaining = 1;
        return 1;
    }
    if ((b & 0xF0) == 0xE0) {
        *remaining = 2;
        return 1;
    }
    if ((b & 0xF8) == 0xF0) {
        *remaining = 3;
        return 1;
    }
    return 0;
}

static int cy_validate_string_encoding(const char *str, uint16_t length)
{
    int      remaining = 0;
    uint16_t i;

    for (i = 0; i < length; i++) {
        if (!cy_validate_utf8_byte((uint8_t)str[i], &remaining))
            return 0;
    }

    return (remaining == 0) ? 1 : 0;
}

/* ── Control character detection ────────────────────────────────── */

static int cy_string_has_control_chars(const char *str, uint16_t length)
{
    uint16_t i;

    for (i = 0; i < length; i++) {
        uint8_t c = (uint8_t)str[i];
        /* Allow tab, newline, carriage return */
        if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D)
            return 1;
        if (c == 0x7F)
            return 1;
    }
    return 0;
}

/* ── String deduplication check ─────────────────────────────────── */

static int cy_find_duplicate_string(char **strings, uint32_t count,
                                    const char *str)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (strings[i] && strcmp(strings[i], str) == 0)
            return (int)i;
    }
    return -1;
}

/* ── String normalization (trim trailing whitespace) ────────────── */

static void cy_normalize_string(char *str, uint16_t *length)
{
    uint16_t len = *length;

    while (len > 0 && (str[len - 1] == ' '  ||
                       str[len - 1] == '\t' ||
                       str[len - 1] == '\n' ||
                       str[len - 1] == '\r')) {
        len--;
    }
    str[len] = '\0';
    *length = len;
}

/* ── Main string table parser ───────────────────────────────────── */

int cy_parse_strings(cy_reader *r, cy_pipeline *pipe,
                     const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;
    uint32_t rec_region_size;
    uint32_t data_base_offset;
    cy_string_entry *entries = NULL;

    if (count == 0) {
        pipe->string_count = 0;
        pipe->strings = NULL;
        return CY_OK;
    }

    if (count > 1000000u) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "string table count %u exceeds reasonable limit",
                     count);
        return CY_ERR_BAD_COUNT;
    }

    /* Calculate record region size */
    if (cy_safe_multiply(count, CY_STRING_REC_SIZE, &rec_region_size) != 0) {
        cy_set_error(r->ctx, CY_ERR_OVERFLOW,
                     "string table record region overflow");
        return CY_ERR_OVERFLOW;
    }

    if (rec_region_size > dir->length) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "string records (%zu bytes) exceed section "
                     "length (%u)",
                     rec_region_size, dir->length);
        return CY_ERR_BAD_COUNT;
    }

    /* String data follows the record array */
    data_base_offset = dir->offset + (uint32_t)rec_region_size;

    /* Allocate record array */
    entries = (cy_string_entry *)cy_calloc(r->ctx, count,
                                           sizeof(cy_string_entry));
    if (!entries)
        return CY_ERR_NOMEM;

    /* Allocate string pointer array */
    pipe->strings = (char **)cy_calloc(r->ctx, count, sizeof(char *));
    if (!pipe->strings) {
        cy_free(entries);
        return CY_ERR_NOMEM;
    }

    /* Seek to record region */
    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK) {
        cy_free(entries);
        return rc;
    }

    /* Read all string records */
    for (i = 0; i < count; i++) {
        entries[i].offset = cy_read_u32(r);
        entries[i].length = cy_read_u16(r);
        entries[i].flags  = cy_read_u16(r);

        if (r->ctx->error_code != CY_OK) {
            cy_free(entries);
            return r->ctx->error_code;
        }
    }

    /* Resolve each string record to an allocated copy */
    for (i = 0; i < count; i++) {
        uint32_t abs_offset;
        uint16_t length = entries[i].length;
        char    *str;

        if (length > CY_MAX_STRING_LEN) {
            cy_add_warning(r->ctx,
                           "string %u: length %u exceeds limit, "
                           "clamping to %u",
                           i, length, CY_MAX_STRING_LEN);
            length = CY_MAX_STRING_LEN;
        }

        /* The offset in the record is relative to the data region */
        if (cy_safe_add(data_base_offset, entries[i].offset,
                        &abs_offset) != 0) {
            cy_add_warning(r->ctx,
                           "string %u: offset overflow, skipping", i);
            pipe->strings[i] = NULL;
            continue;
        }

        if ((uint64_t)abs_offset + length > r->size) {
            cy_add_warning(r->ctx,
                           "string %u: data at [%u, %u) exceeds file, "
                           "skipping",
                           i, abs_offset, abs_offset + length);
            pipe->strings[i] = NULL;
            continue;
        }

        str = (char *)cy_alloc(r->ctx, (size_t)length + 1);
        if (!str) {
            cy_free(entries);
            return CY_ERR_NOMEM;
        }

        memcpy(str, r->data + abs_offset, length);
        str[length] = '\0';

        /* Normalize whitespace */
        cy_normalize_string(str, &length);

        /* Validate encoding */
        if (!cy_validate_string_encoding(str, length)) {
            cy_add_warning(r->ctx,
                           "string %u: invalid UTF-8 encoding", i);
        }

        /* Check for control characters */
        if (cy_string_has_control_chars(str, length)) {
            cy_add_warning(r->ctx,
                           "string %u: contains control characters", i);
        }

        pipe->strings[i] = str;
    }

    pipe->string_count = count;

    /* Log deduplication statistics */
    {
        uint32_t dup_count = 0;
        for (i = 0; i < count; i++) {
            if (pipe->strings[i]) {
                int dup = cy_find_duplicate_string(pipe->strings, i,
                                                   pipe->strings[i]);
                if (dup >= 0)
                    dup_count++;
            }
        }
        if (dup_count > 0) {
            cy_add_warning(r->ctx,
                           "string table contains %u duplicate entries",
                           dup_count);
        }
    }

    cy_free(entries);
    return CY_OK;
}

/* ── String table statistics ────────────────────────────────────── */

int cy_string_table_stats(const cy_pipeline *pipe,
                          uint32_t *total_bytes,
                          uint32_t *avg_length,
                          uint32_t *max_length,
                          uint32_t *null_count)
{
    uint32_t i;
    uint64_t total = 0;
    uint32_t maxlen = 0;
    uint32_t nulls = 0;

    if (!pipe || !pipe->strings)
        return -1;

    for (i = 0; i < pipe->string_count; i++) {
        if (!pipe->strings[i]) {
            nulls++;
            continue;
        }

        uint32_t len = (uint32_t)strlen(pipe->strings[i]);
        total += len;
        if (len > maxlen)
            maxlen = len;
    }

    if (total_bytes)  *total_bytes = (uint32_t)total;
    if (avg_length)   *avg_length = pipe->string_count > nulls
                                    ? (uint32_t)(total / (pipe->string_count - nulls))
                                    : 0;
    if (max_length)   *max_length = maxlen;
    if (null_count)   *null_count = nulls;

    return 0;
}

/* ── String prefix matching ─────────────────────────────────────── */

int cy_find_strings_by_prefix(const cy_pipeline *pipe,
                              const char *prefix,
                              uint32_t *out_indices,
                              uint32_t max_count,
                              uint32_t *found_count)
{
    uint32_t i;
    uint32_t found = 0;
    size_t   prefix_len;

    if (!pipe || !pipe->strings || !prefix || !out_indices || !found_count)
        return -1;

    prefix_len = strlen(prefix);

    for (i = 0; i < pipe->string_count; i++) {
        if (!pipe->strings[i])
            continue;

        if (strncmp(pipe->strings[i], prefix, prefix_len) == 0) {
            if (found < max_count) {
                out_indices[found] = i;
            }
            found++;
        }
    }

    *found_count = found;
    return 0;
}

/* ── String reference counting ──────────────────────────────────── */

int cy_count_string_references(const cy_pipeline *pipe,
                               uint32_t string_id,
                               uint32_t *ref_count)
{
    uint32_t i, j;
    uint32_t count = 0;

    if (!pipe || !ref_count)
        return -1;

    if (string_id >= pipe->string_count)
        return -1;

    /* Check node name references */
    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].name_id == string_id)
            count++;
    }

    /* Check type name references */
    for (i = 0; i < pipe->type_count; i++) {
        if (pipe->types[i].name_id == string_id)
            count++;
    }

    /* Check port name references */
    for (i = 0; i < pipe->node_count; i++) {
        if (!pipe->nodes[i].ports)
            continue;
        for (j = 0; j < pipe->nodes[i].port_count; j++) {
            if (pipe->nodes[i].ports[j].name_id == string_id)
                count++;
        }
    }

    /* Check param name references */
    for (i = 0; i < pipe->param_count; i++) {
        if (pipe->params[i].name_id == string_id)
            count++;
    }

    /* Check group name references */
    for (i = 0; i < pipe->group_count; i++) {
        if (pipe->groups[i].name_id == string_id)
            count++;
    }

    *ref_count = count;
    return 0;
}

/* ── String table compaction analysis ───────────────────────────── */

int cy_analyze_string_compaction(const cy_pipeline *pipe,
                                 uint32_t *reclaimable,
                                 uint32_t *unique_strings)
{
    uint32_t i, j;
    uint32_t dup_bytes = 0;
    uint32_t unique = 0;
    uint8_t *is_dup = NULL;

    if (!pipe || !pipe->strings || !reclaimable || !unique_strings)
        return -1;

    is_dup = (uint8_t *)calloc(pipe->string_count, 1);
    if (!is_dup)
        return -1;

    for (i = 0; i < pipe->string_count; i++) {
        if (!pipe->strings[i] || is_dup[i])
            continue;

        unique++;
        for (j = i + 1; j < pipe->string_count; j++) {
            if (!pipe->strings[j] || is_dup[j])
                continue;
            if (strcmp(pipe->strings[i], pipe->strings[j]) == 0) {
                is_dup[j] = 1;
                dup_bytes += (uint32_t)strlen(pipe->strings[j]) + 1;
            }
        }
    }

    *reclaimable = dup_bytes;
    *unique_strings = unique;

    free(is_dup);
    return 0;
}
