/*
 * params.c — Phase 7: Parameter table parsing
 *
 * Parameters are per-node configuration values with type, range
 * constraints, and default values. Each parameter has a name
 * (referencing the string table) and belongs to a specific node.
 * During validation, parameter names may be normalized to eliminate
 * duplicate string table entries.
 *
 * Reference: CYC Format Specification §3.6 - Parameter Table
 */

#include "internal.h"

/* ── Parameter kind validation ──────────────────────────────────── */

static int cy_is_valid_param_kind(uint8_t kind)
{
    switch (kind) {
    case CY_PARAM_INT:
    case CY_PARAM_FLOAT:
    case CY_PARAM_STRING:
    case CY_PARAM_BOOL:
    case CY_PARAM_ENUM:
        return 1;
    default:
        return 0;
    }
}

/* ── Range validation ───────────────────────────────────────────── */

static int cy_validate_param_range(const cy_param *p, cy_context *ctx)
{
    switch (p->kind) {
    case CY_PARAM_INT:
        if (p->min_val.i > p->max_val.i) {
            cy_add_warning(ctx,
                           "param %u (%s): int min %lld > max %lld",
                           p->id,
                           p->name ? p->name : "unnamed",
                           (long long)p->min_val.i,
                           (long long)p->max_val.i);
            return 0;
        }
        if (p->value.i < p->min_val.i || p->value.i > p->max_val.i) {
            cy_add_warning(ctx,
                           "param %u (%s): default %lld outside "
                           "range [%lld, %lld]",
                           p->id,
                           p->name ? p->name : "unnamed",
                           (long long)p->value.i,
                           (long long)p->min_val.i,
                           (long long)p->max_val.i);
        }
        break;

    case CY_PARAM_FLOAT:
        if (p->min_val.f > p->max_val.f) {
            cy_add_warning(ctx,
                           "param %u (%s): float min %f > max %f",
                           p->id,
                           p->name ? p->name : "unnamed",
                           p->min_val.f, p->max_val.f);
            return 0;
        }
        if (p->value.f < p->min_val.f || p->value.f > p->max_val.f) {
            cy_add_warning(ctx,
                           "param %u (%s): default %f outside "
                           "range [%f, %f]",
                           p->id,
                           p->name ? p->name : "unnamed",
                           p->value.f,
                           p->min_val.f, p->max_val.f);
        }
        break;

    case CY_PARAM_BOOL:
        if (p->value.i != 0 && p->value.i != 1) {
            cy_add_warning(ctx,
                           "param %u (%s): bool value %lld not 0 or 1",
                           p->id,
                           p->name ? p->name : "unnamed",
                           (long long)p->value.i);
        }
        break;

    case CY_PARAM_ENUM:
        if (p->value.i < 0) {
            cy_add_warning(ctx,
                           "param %u (%s): enum value %lld is negative",
                           p->id,
                           p->name ? p->name : "unnamed",
                           (long long)p->value.i);
        }
        break;

    default:
        break;
    }

    return 1;
}

/* ── Node ownership validation ──────────────────────────────────── */

static int cy_validate_param_ownership(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *p = &pipe->params[i];

        if (p->node_id >= pipe->node_count) {
            cy_add_warning(ctx,
                           "param %u (%s): node_id %u out of range "
                           "(node_count=%u)",
                           i,
                           p->name ? p->name : "unnamed",
                           p->node_id, pipe->node_count);
        }
    }

    return CY_OK;
}

/* ── Parameter name deduplication ───────────────────────────────── */
/* Finds params across different nodes that share the same name     */
/* string. Keeps one copy, frees duplicates, rewrites param name    */
/* pointers. Port cache hash keys are string pointers and may       */
/* reference the same string table entries — those are left as-is.  */

int cy_normalize_param_names(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t deduped = 0;

    if (pipe->param_count < 2)
        return CY_OK;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *primary = &pipe->params[i];
        if (!primary->name)
            continue;

        for (j = i + 1; j < pipe->param_count; j++) {
            cy_param *dup = &pipe->params[j];
            if (!dup->name)
                continue;

            /* Different params on different nodes with the same name */
            if (primary->node_id == dup->node_id)
                continue;

            if (strcmp(primary->name, dup->name) != 0)
                continue;

            /* Both reference the string table; if they point to
               different copies of the same string, deduplicate by
               freeing the duplicate string and pointing to the
               primary's copy. */
            if (primary->name != dup->name) {
                /* The duplicate's name_id referenced a separate
                   string table entry. Free the separate copy and
                   point to the canonical one. */
                if (dup->name_id < pipe->string_count &&
                    pipe->strings[dup->name_id] != NULL) {
                    cy_free(pipe->strings[dup->name_id]);
                    pipe->strings[dup->name_id] = (char *)primary->name;
                }
                dup->name = primary->name;
                deduped++;
            }
        }
    }

    if (deduped > 0) {
        cy_add_warning(ctx,
                       "deduplicated %u parameter name strings", deduped);
    }

    return CY_OK;
}

/* ── Cross-node param conflict detection ────────────────────────── */

static int cy_check_param_conflicts(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *a = &pipe->params[i];
        if (!a->name)
            continue;

        for (j = i + 1; j < pipe->param_count; j++) {
            cy_param *b = &pipe->params[j];
            if (!b->name)
                continue;

            /* Same node, same name = conflict */
            if (a->node_id == b->node_id &&
                strcmp(a->name, b->name) == 0) {
                cy_add_warning(ctx,
                               "node %u: duplicate param name '%s' "
                               "at indices %u and %u",
                               a->node_id, a->name, i, j);
            }
        }
    }

    return CY_OK;
}

/* ── String parameter resolution ────────────────────────────────── */

static int cy_resolve_string_params(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *p = &pipe->params[i];

        if (p->kind != CY_PARAM_STRING)
            continue;

        /* The value.i field holds a string table index for string params */
        if (p->value.i >= 0 && (uint32_t)p->value.i < pipe->string_count) {
            p->value.s = pipe->strings[(uint32_t)p->value.i];
        } else {
            p->value.s = NULL;
            cy_add_warning(ctx,
                           "param %u (%s): string value index %lld "
                           "out of range",
                           i,
                           p->name ? p->name : "unnamed",
                           (long long)p->value.i);
        }
    }

    return CY_OK;
}

/* ── Main parameter table parser ────────────────────────────────── */

int cy_parse_params(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;

    if (count == 0) {
        pipe->param_count = 0;
        pipe->params = NULL;
        return CY_OK;
    }

    if (count > CY_MAX_PARAMS) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "param count %u exceeds limit %u",
                     count, CY_MAX_PARAMS);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_PARAM_REC_SIZE, dir->length,
                                  r->ctx, "param table");
    if (rc != CY_OK)
        return rc;

    pipe->params = (cy_param *)cy_calloc(r->ctx, count, sizeof(cy_param));
    if (!pipe->params)
        return CY_ERR_NOMEM;

    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    for (i = 0; i < count; i++) {
        cy_param *p = &pipe->params[i];

        p->id      = i;
        p->name_id = cy_read_u32(r);
        p->node_id = cy_read_u32(r);
        p->kind    = cy_read_u8(r);
        /* 1 byte padding */
        cy_read_u8(r);
        p->flags   = cy_read_u16(r);

        /* Read value (8 bytes) */
        if (p->kind == CY_PARAM_FLOAT) {
            p->value.f = cy_read_f64(r);
        } else {
            p->value.i = cy_read_i64(r);
        }

        /* Read min (8 bytes) */
        if (p->kind == CY_PARAM_FLOAT) {
            p->min_val.f = cy_read_f64(r);
        } else {
            p->min_val.i = cy_read_i64(r);
        }

        /* Read max (8 bytes) */
        if (p->kind == CY_PARAM_FLOAT) {
            p->max_val.f = cy_read_f64(r);
        } else {
            p->max_val.i = cy_read_i64(r);
        }

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Validate kind */
        if (!cy_is_valid_param_kind(p->kind)) {
            cy_add_warning(r->ctx,
                           "param %u: invalid kind %u, treating as int",
                           i, p->kind);
            p->kind = CY_PARAM_INT;
        }

        /* Resolve name */
        p->name = cy_resolve_string(pipe, p->name_id);
        if (!p->name) {
            cy_add_warning(r->ctx,
                           "param %u: name_id %u not in string table",
                           i, p->name_id);
        }

        /* Validate range */
        cy_validate_param_range(p, r->ctx);
    }

    pipe->param_count = count;

    /* Validate ownership */
    cy_validate_param_ownership(pipe, r->ctx);

    /* Resolve string-valued parameters */
    cy_resolve_string_params(pipe, r->ctx);

    /* Check for conflicts */
    cy_check_param_conflicts(pipe, r->ctx);

    return CY_OK;
}

/* ── Parameter distribution analysis ────────────────────────────── */

int cy_analyze_param_distribution(const cy_pipeline *pipe,
                                  uint32_t *by_kind,
                                  uint32_t max_kinds)
{
    uint32_t i;

    if (!pipe || !by_kind)
        return -1;

    memset(by_kind, 0, max_kinds * sizeof(uint32_t));

    for (i = 0; i < pipe->param_count; i++) {
        uint8_t kind = pipe->params[i].kind;
        if (kind < max_kinds)
            by_kind[kind]++;
    }

    return 0;
}

/* ── Parameter value range analysis ─────────────────────────────── */

int cy_param_range_analysis(const cy_pipeline *pipe, uint32_t node_id,
                            double *min_range, double *max_range,
                            double *avg_range)
{
    uint32_t i;
    uint32_t count = 0;
    double total_range = 0.0;
    double min_r = 1e308;
    double max_r = 0.0;

    if (!pipe || !min_range || !max_range || !avg_range)
        return -1;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *p = &pipe->params[i];
        double range;

        if (p->node_id != node_id)
            continue;

        switch (p->kind) {
        case CY_PARAM_INT:
            range = (double)(p->max_val.i - p->min_val.i);
            break;
        case CY_PARAM_FLOAT:
            range = p->max_val.f - p->min_val.f;
            break;
        default:
            continue;
        }

        if (range < 0)
            range = -range;

        total_range += range;
        if (range < min_r)
            min_r = range;
        if (range > max_r)
            max_r = range;
        count++;
    }

    *min_range = count > 0 ? min_r : 0.0;
    *max_range = max_r;
    *avg_range = count > 0 ? total_range / count : 0.0;

    return 0;
}

/* ── Parameter sensitivity analysis ─────────────────────────────── */

int cy_param_sensitivity_check(const cy_pipeline *pipe,
                               uint32_t *sensitive_params,
                               uint32_t max_count,
                               uint32_t *found_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !sensitive_params || !found_count)
        return -1;

    for (i = 0; i < pipe->param_count; i++) {
        cy_param *p = &pipe->params[i];
        int is_sensitive = 0;

        /* A parameter is "sensitive" if its value is near the
           boundary of its valid range */
        switch (p->kind) {
        case CY_PARAM_INT:
            {
                int64_t range = p->max_val.i - p->min_val.i;
                if (range > 0) {
                    int64_t dist_from_min = p->value.i - p->min_val.i;
                    int64_t dist_from_max = p->max_val.i - p->value.i;
                    int64_t margin = range / 10;
                    if (margin == 0) margin = 1;
                    if (dist_from_min < margin ||
                        dist_from_max < margin) {
                        is_sensitive = 1;
                    }
                }
            }
            break;
        case CY_PARAM_FLOAT:
            {
                double range = p->max_val.f - p->min_val.f;
                if (range > 0) {
                    double dist_min = p->value.f - p->min_val.f;
                    double dist_max = p->max_val.f - p->value.f;
                    double margin = range * 0.05;
                    if (dist_min < margin || dist_max < margin) {
                        is_sensitive = 1;
                    }
                }
            }
            break;
        default:
            break;
        }

        if (is_sensitive && found < max_count) {
            sensitive_params[found] = i;
            found++;
        }
    }

    *found_count = found;
    return 0;
}

/* ── Cross-node parameter correlation ───────────────────────────── */

int cy_find_correlated_params(const cy_pipeline *pipe,
                              uint32_t param_id,
                              uint32_t *correlated,
                              uint32_t max_count,
                              uint32_t *found_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !correlated || !found_count)
        return -1;
    if (param_id >= pipe->param_count)
        return -1;

    cy_param *target = &pipe->params[param_id];
    if (!target->name)
        return 0;

    /* Find params on other nodes with the same name and kind */
    for (i = 0; i < pipe->param_count; i++) {
        if (i == param_id)
            continue;

        cy_param *other = &pipe->params[i];
        if (!other->name)
            continue;
        if (other->node_id == target->node_id)
            continue;
        if (other->kind != target->kind)
            continue;

        if (strcmp(other->name, target->name) == 0) {
            if (found < max_count) {
                correlated[found] = i;
                found++;
            }
        }
    }

    *found_count = found;
    return 0;
}
