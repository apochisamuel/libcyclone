/*
 * context.c — Context management, error reporting, memory helpers
 *
 * The context object tracks error state and accumulated warnings
 * throughout the parsing pipeline. Each phase reports issues through
 * the context rather than returning error details directly.
 */

#include "internal.h"
#include <stdarg.h>

/* ── Context lifecycle ──────────────────────────────────────────── */

cy_context *cy_context_create(void)
{
    cy_context *ctx = (cy_context *)calloc(1, sizeof(cy_context));
    if (!ctx)
        return NULL;
    ctx->error_code = CY_OK;
    ctx->error_msg[0] = '\0';
    ctx->warning_count = 0;
    ctx->flags = 0;
    return ctx;
}

void cy_context_destroy(cy_context *ctx)
{
    if (ctx)
        free(ctx);
}

int cy_context_error(const cy_context *ctx)
{
    if (!ctx)
        return CY_ERR_INTERNAL;
    return ctx->error_code;
}

const char *cy_context_error_msg(const cy_context *ctx)
{
    if (!ctx)
        return "null context";
    if (ctx->error_code == CY_OK)
        return "no error";
    return ctx->error_msg;
}

int cy_context_warning_count(const cy_context *ctx)
{
    if (!ctx)
        return 0;
    return ctx->warning_count;
}

/* ── Error reporting ────────────────────────────────────────────── */

void cy_set_error(cy_context *ctx, int code, const char *fmt, ...)
{
    va_list ap;

    if (!ctx)
        return;
    if (ctx->error_code != CY_OK)
        return;     /* preserve first error */

    ctx->error_code = code;

    va_start(ap, fmt);
    vsnprintf(ctx->error_msg, CY_ERROR_MSG_SIZE, fmt, ap);
    va_end(ap);
}

void cy_add_warning(cy_context *ctx, const char *fmt, ...)
{
    va_list ap;
    int idx;

    if (!ctx)
        return;
    if ((uint32_t)ctx->warning_count >= CY_MAX_WARNINGS)
        return;

    idx = ctx->warning_count;
    va_start(ap, fmt);
    vsnprintf(ctx->warnings[idx], CY_ERROR_MSG_SIZE, fmt, ap);
    va_end(ap);
    ctx->warning_count++;
}

void cy_clear_error(cy_context *ctx)
{
    if (!ctx)
        return;
    ctx->error_code = CY_OK;
    ctx->error_msg[0] = '\0';
}

/* ── Memory helpers ─────────────────────────────────────────────── */

void *cy_alloc(cy_context *ctx, size_t size)
{
    void *p;

    if (size == 0)
        size = 1;

    p = malloc(size);
    if (!p) {
        cy_set_error(ctx, CY_ERR_NOMEM,
                     "allocation failed (%zu bytes)", size);
        return NULL;
    }
    return p;
}

void *cy_calloc(cy_context *ctx, size_t count, size_t elem_size)
{
    void *p;

    if (count == 0)
        count = 1;
    if (elem_size == 0)
        elem_size = 1;

    p = calloc(count, elem_size);
    if (!p) {
        cy_set_error(ctx, CY_ERR_NOMEM,
                     "calloc failed (%zu x %zu bytes)", count, elem_size);
        return NULL;
    }
    return p;
}

void *cy_realloc(cy_context *ctx, void *ptr, size_t new_size)
{
    void *p;

    if (new_size == 0)
        new_size = 1;

    p = realloc(ptr, new_size);
    if (!p) {
        cy_set_error(ctx, CY_ERR_NOMEM,
                     "realloc failed (%zu bytes)", new_size);
        return NULL;
    }
    return p;
}

void cy_free(void *ptr)
{
    free(ptr);
}

/* ── Arithmetic safety ──────────────────────────────────────────── */

int cy_safe_multiply(uint32_t a, uint32_t b, uint32_t *result)
{
    uint64_t product = (uint64_t)a * (uint64_t)b;
    if (product > UINT32_MAX)
        return -1;
    *result = (uint32_t)product;
    return 0;
}

int cy_safe_add(uint32_t a, uint32_t b, uint32_t *result)
{
    uint64_t sum = (uint64_t)a + (uint64_t)b;
    if (sum > UINT32_MAX)
        return -1;
    *result = (uint32_t)sum;
    return 0;
}

/* ── String resolution ──────────────────────────────────────────── */

const char *cy_resolve_string(const cy_pipeline *pipe, uint32_t string_id)
{
    if (!pipe || !pipe->strings)
        return NULL;
    if (string_id >= pipe->string_count)
        return NULL;
    return pipe->strings[string_id];
}

uint32_t cy_hash_string(const char *s)
{
    uint32_t h = 5381;
    if (!s)
        return h;
    while (*s) {
        h = ((h << 5) + h) ^ (uint8_t)*s;
        s++;
    }
    return h;
}

/* ── Pipeline allocation and teardown ───────────────────────────── */

static void cy_free_port_cache(cy_port_cache *cache)
{
    if (!cache)
        return;
    cy_free(cache->entries);
    cy_free(cache);
}

static void cy_free_route_table(cy_route_table *rt)
{
    if (!rt)
        return;
    cy_free(rt->by_src);
    cy_free(rt->by_dst);
    cy_free(rt->entries);
    cy_free(rt);
}

void cy_pipeline_destroy(cy_pipeline *pipe)
{
    uint32_t i;

    if (!pipe)
        return;

    /* Free per-node port arrays */
    if (pipe->nodes) {
        for (i = 0; i < pipe->node_count; i++) {
            cy_node *n = &pipe->nodes[i];
            if (n->ports) {
                uint32_t j;
                for (j = 0; j < n->port_count; j++) {
                    cy_free(n->ports[j].connected_edges);
                }
                cy_free(n->ports);
            }
        }
        cy_free(pipe->nodes);
    }

    /* Free port index */
    cy_free(pipe->port_index);

    /* Free edges */
    cy_free(pipe->edges);

    /* Free type descriptors */
    cy_free(pipe->types);

    /* Free params */
    cy_free(pipe->params);

    /* Free schedule */
    cy_free(pipe->schedule);

    /* Free groups */
    if (pipe->groups) {
        for (i = 0; i < pipe->group_count; i++) {
            cy_free(pipe->groups[i].members);
            cy_free(pipe->groups[i].port_map);
        }
        cy_free(pipe->groups);
    }

    /* Free string table */
    cy_free(pipe->strings);
    cy_free(pipe->string_data);

    /* Free derived caches */
    cy_free_port_cache(pipe->port_cache);
    cy_free_route_table(pipe->route_table);

    cy_free(pipe);
}

/* ── Top-level parse orchestrator ───────────────────────────────── */

cy_pipeline *cy_parse(cy_context *ctx, const uint8_t *data, size_t size)
{
    cy_pipeline   *pipe = NULL;
    cy_reader      reader;
    cy_table_dir   dirs[CY_MAX_TABLES];
    uint32_t       dir_count = 0;
    uint32_t       i;

    if (!ctx)
        return NULL;

    cy_clear_error(ctx);

    if (!data || size < CY_FILE_HEADER_SIZE) {
        cy_set_error(ctx, CY_ERR_TRUNCATED,
                     "input too small (%zu bytes, need %u)",
                     size, CY_FILE_HEADER_SIZE);
        return NULL;
    }

    if (size > CY_MAX_FILE_SIZE) {
        cy_set_error(ctx, CY_ERR_OVERFLOW,
                     "input exceeds maximum file size (%zu > %u)",
                     size, CY_MAX_FILE_SIZE);
        return NULL;
    }

    pipe = (cy_pipeline *)cy_calloc(ctx, 1, sizeof(cy_pipeline));
    if (!pipe)
        return NULL;

    cy_reader_init(&reader, ctx, data, size);

    /* Phase 1: Parse file header and table directory */
    if (cy_parse_header(&reader, pipe, dirs, &dir_count) != CY_OK)
        goto fail;

    /* Locate each table section and parse in order */
    for (i = 0; i < dir_count; i++) {
        switch (dirs[i].tag) {
        case CY_TAG_STR:
            /* Phase 2: String table */
            if (cy_parse_strings(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_TYP:
            /* Phase 3: Type descriptors */
            if (cy_parse_types(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_NOD:
            /* Phase 4: Nodes */
            if (cy_parse_nodes(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_PRT:
            /* Phase 5: Ports */
            if (cy_parse_ports(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_EDG:
            /* Phase 6: Edges */
            if (cy_parse_edges(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_PRM:
            /* Phase 7: Parameters */
            if (cy_parse_params(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_SCH:
            /* Phase 8: Schedule */
            if (cy_parse_schedule(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        case CY_TAG_GRP:
            /* Phase 9: Groups */
            if (cy_parse_groups(&reader, pipe, &dirs[i]) != CY_OK)
                goto fail;
            break;
        default:
            cy_add_warning(ctx, "unknown table tag 0x%08x at directory "
                           "index %u", dirs[i].tag, i);
            break;
        }
    }

    /* Phase 10: Build port lookup cache */
    if (cy_build_port_cache(pipe, ctx) != CY_OK)
        goto fail;

    /* Phase 11: Build edge routing table */
    if (cy_build_route_table(pipe, ctx) != CY_OK)
        goto fail;

    /* Phase 12: Validate cross-table consistency */
    if (cy_validate_pipeline(pipe, ctx) != CY_OK)
        goto fail;

    /* Phase 13: Resolve final cross-references */
    if (cy_resolve_references(pipe, ctx) != CY_OK)
        goto fail;

    return pipe;

fail:
    cy_pipeline_destroy(pipe);
    return NULL;
}
