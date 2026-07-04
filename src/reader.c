/*
 * reader.c — Low-level binary reading primitives
 *
 * Provides bounds-checked reading of integers, floats, and byte
 * sequences from a flat buffer. All multi-byte values are read
 * in little-endian byte order per the CYC format specification.
 */

#include "internal.h"

/* ── Initialization ─────────────────────────────────────────────── */

void cy_reader_init(cy_reader *r, cy_context *ctx,
                    const uint8_t *data, size_t size)
{
    r->data = data;
    r->size = size;
    r->pos  = 0;
    r->ctx  = ctx;
}

/* ── Position control ───────────────────────────────────────────── */

int cy_reader_seek(cy_reader *r, size_t offset)
{
    if (offset > r->size) {
        cy_set_error(r->ctx, CY_ERR_BAD_OFFSET,
                     "seek past end: offset %zu exceeds size %zu",
                     offset, r->size);
        return CY_ERR_BAD_OFFSET;
    }
    r->pos = offset;
    return CY_OK;
}

int cy_reader_available(const cy_reader *r, size_t need)
{
    return (r->pos + need <= r->size) ? 1 : 0;
}

size_t cy_reader_pos(const cy_reader *r)
{
    return r->pos;
}

size_t cy_reader_remaining(const cy_reader *r)
{
    if (r->pos >= r->size)
        return 0;
    return r->size - r->pos;
}

/* ── Integer readers (little-endian) ────────────────────────────── */

uint8_t cy_read_u8(cy_reader *r)
{
    uint8_t val;

    if (r->pos + 1 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_u8: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val = r->data[r->pos];
    r->pos += 1;
    return val;
}

uint16_t cy_read_u16(cy_reader *r)
{
    uint16_t val;

    if (r->pos + 2 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_u16: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val  = (uint16_t)r->data[r->pos];
    val |= (uint16_t)r->data[r->pos + 1] << 8;
    r->pos += 2;
    return val;
}

uint32_t cy_read_u32(cy_reader *r)
{
    uint32_t val;

    if (r->pos + 4 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_u32: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val  = (uint32_t)r->data[r->pos];
    val |= (uint32_t)r->data[r->pos + 1] << 8;
    val |= (uint32_t)r->data[r->pos + 2] << 16;
    val |= (uint32_t)r->data[r->pos + 3] << 24;
    r->pos += 4;
    return val;
}

int64_t cy_read_i64(cy_reader *r)
{
    uint64_t val;

    if (r->pos + 8 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_i64: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val  = (uint64_t)r->data[r->pos];
    val |= (uint64_t)r->data[r->pos + 1] << 8;
    val |= (uint64_t)r->data[r->pos + 2] << 16;
    val |= (uint64_t)r->data[r->pos + 3] << 24;
    val |= (uint64_t)r->data[r->pos + 4] << 32;
    val |= (uint64_t)r->data[r->pos + 5] << 40;
    val |= (uint64_t)r->data[r->pos + 6] << 48;
    val |= (uint64_t)r->data[r->pos + 7] << 56;
    r->pos += 8;

    return (int64_t)val;
}

double cy_read_f64(cy_reader *r)
{
    union {
        uint64_t u;
        double   d;
    } convert;
    uint64_t val;

    if (r->pos + 8 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_f64: position %zu past end %zu",
                     r->pos, r->size);
        return 0.0;
    }

    val  = (uint64_t)r->data[r->pos];
    val |= (uint64_t)r->data[r->pos + 1] << 8;
    val |= (uint64_t)r->data[r->pos + 2] << 16;
    val |= (uint64_t)r->data[r->pos + 3] << 24;
    val |= (uint64_t)r->data[r->pos + 4] << 32;
    val |= (uint64_t)r->data[r->pos + 5] << 40;
    val |= (uint64_t)r->data[r->pos + 6] << 48;
    val |= (uint64_t)r->data[r->pos + 7] << 56;
    r->pos += 8;

    convert.u = val;
    return convert.d;
}

/* ── Byte sequence reader ───────────────────────────────────────── */

int cy_read_bytes(cy_reader *r, void *dst, size_t len)
{
    if (len == 0)
        return CY_OK;

    if (r->pos + len > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "read_bytes: need %zu at position %zu, have %zu",
                     len, r->pos, r->size - r->pos);
        return CY_ERR_TRUNCATED;
    }

    memcpy(dst, r->data + r->pos, len);
    r->pos += len;
    return CY_OK;
}

/* ── Peek (read without advancing position) ─────────────────────── */

uint32_t cy_peek_u32(cy_reader *r)
{
    uint32_t val;

    if (r->pos + 4 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "peek_u32: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val  = (uint32_t)r->data[r->pos];
    val |= (uint32_t)r->data[r->pos + 1] << 8;
    val |= (uint32_t)r->data[r->pos + 2] << 16;
    val |= (uint32_t)r->data[r->pos + 3] << 24;
    /* position not advanced */
    return val;
}

/* ── Alignment helpers ──────────────────────────────────────────── */

static size_t cy_align_up(size_t val, size_t alignment)
{
    if (alignment == 0)
        return val;
    return (val + alignment - 1) & ~(alignment - 1);
}

int cy_reader_align(cy_reader *r, size_t alignment)
{
    size_t aligned = cy_align_up(r->pos, alignment);
    if (aligned > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "alignment to %zu at position %zu exceeds size %zu",
                     alignment, r->pos, r->size);
        return CY_ERR_TRUNCATED;
    }
    r->pos = aligned;
    return CY_OK;
}

/* ── String reading (length-prefixed, null-terminated copy) ─────── */

char *cy_read_string_at(cy_reader *r, cy_context *ctx,
                        uint32_t offset, uint16_t length)
{
    char *str;
    size_t saved_pos;

    if (length == 0) {
        str = (char *)cy_alloc(ctx, 1);
        if (str)
            str[0] = '\0';
        return str;
    }

    if (length > CY_MAX_STRING_LEN) {
        cy_set_error(ctx, CY_ERR_BAD_STRING,
                     "string length %u exceeds maximum %u",
                     length, CY_MAX_STRING_LEN);
        return NULL;
    }

    if ((size_t)offset + (size_t)length > r->size) {
        cy_set_error(ctx, CY_ERR_BAD_OFFSET,
                     "string at offset %u length %u exceeds file size %zu",
                     offset, length, r->size);
        return NULL;
    }

    str = (char *)cy_alloc(ctx, (size_t)length + 1);
    if (!str)
        return NULL;

    saved_pos = r->pos;
    r->pos = offset;
    memcpy(str, r->data + r->pos, length);
    str[length] = '\0';
    r->pos = saved_pos;

    return str;
}

/* ── Validation of offsets and ranges ───────────────────────────── */

int cy_validate_range(cy_reader *r, uint32_t offset, uint32_t length,
                      const char *table_name)
{
    uint64_t end = (uint64_t)offset + (uint64_t)length;

    if (end > r->size) {
        cy_set_error(r->ctx, CY_ERR_BAD_OFFSET,
                     "%s: range [%u, %u) exceeds file size %zu",
                     table_name, offset, (uint32_t)end, r->size);
        return CY_ERR_BAD_OFFSET;
    }
    return CY_OK;
}

int cy_validate_record_count(uint32_t count, uint32_t rec_size,
                             uint32_t section_length,
                             cy_context *ctx, const char *table_name)
{
    uint32_t expected;

    if (cy_safe_multiply(count, rec_size, &expected) != 0) {
        cy_set_error(ctx, CY_ERR_OVERFLOW,
                     "%s: record count %u * size %u overflows",
                     table_name, count, rec_size);
        return CY_ERR_OVERFLOW;
    }

    if (expected > section_length) {
        cy_set_error(ctx, CY_ERR_BAD_COUNT,
                     "%s: %u records * %u bytes = %u exceeds section "
                     "length %u",
                     table_name, count, rec_size, expected, section_length);
        return CY_ERR_BAD_COUNT;
    }

    return CY_OK;
}

/* ── Batch reading utilities ────────────────────────────────────── */

int cy_read_u32_array(cy_reader *r, uint32_t *arr, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        arr[i] = cy_read_u32(r);
        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;
    }
    return CY_OK;
}

int cy_read_u16_array(cy_reader *r, uint16_t *arr, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        arr[i] = cy_read_u16(r);
        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;
    }
    return CY_OK;
}

int cy_read_u8_array(cy_reader *r, uint8_t *arr, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        arr[i] = cy_read_u8(r);
        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;
    }
    return CY_OK;
}

/* ── Skip functions ─────────────────────────────────────────────── */

int cy_reader_skip(cy_reader *r, size_t bytes)
{
    if (r->pos + bytes > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "skip: %zu bytes at position %zu exceeds size %zu",
                     bytes, r->pos, r->size);
        return CY_ERR_TRUNCATED;
    }
    r->pos += bytes;
    return CY_OK;
}

int cy_reader_skip_to_alignment(cy_reader *r, size_t alignment)
{
    return cy_reader_align(r, alignment);
}

/* ── Data pattern detection ─────────────────────────────────────── */

int cy_reader_find_pattern(const cy_reader *r, const uint8_t *pattern,
                           size_t pattern_len, size_t *found_offset)
{
    size_t i;

    if (!r || !pattern || pattern_len == 0 || !found_offset)
        return -1;

    for (i = r->pos; i + pattern_len <= r->size; i++) {
        size_t j;
        int match = 1;

        for (j = 0; j < pattern_len; j++) {
            if (r->data[i + j] != pattern[j]) {
                match = 0;
                break;
            }
        }

        if (match) {
            *found_offset = i;
            return 0;
        }
    }

    return -1;
}

/* ── Checksum computation on subrange ───────────────────────────── */

uint32_t cy_reader_checksum_range(const cy_reader *r,
                                  size_t start, size_t length)
{
    uint32_t sum = 0;
    size_t end;
    size_t i;

    if (!r || start >= r->size)
        return 0;

    end = start + length;
    if (end > r->size)
        end = r->size;

    for (i = start; i < end; i++) {
        sum = (sum << 1) | (sum >> 31);
        sum += r->data[i];
    }

    return sum;
}

/* ── Range overlap detection ────────────────────────────────────── */

int cy_ranges_overlap(uint32_t a_start, uint32_t a_len,
                      uint32_t b_start, uint32_t b_len)
{
    uint64_t a_end = (uint64_t)a_start + a_len;
    uint64_t b_end = (uint64_t)b_start + b_len;

    if (a_start < b_end && b_start < a_end)
        return 1;
    return 0;
}

/* ── Bounds-checked pointer computation ─────────────────────────── */

const uint8_t *cy_reader_ptr_at(const cy_reader *r, size_t offset,
                                size_t length)
{
    if (offset + length > r->size)
        return NULL;
    return r->data + offset;
}

/* ── Multi-byte peek functions ──────────────────────────────────── */

uint16_t cy_peek_u16(cy_reader *r)
{
    uint16_t val;

    if (r->pos + 2 > r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "peek_u16: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }

    val  = (uint16_t)r->data[r->pos];
    val |= (uint16_t)r->data[r->pos + 1] << 8;
    return val;
}

uint8_t cy_peek_u8(cy_reader *r)
{
    if (r->pos >= r->size) {
        cy_set_error(r->ctx, CY_ERR_TRUNCATED,
                     "peek_u8: position %zu past end %zu",
                     r->pos, r->size);
        return 0;
    }
    return r->data[r->pos];
}
