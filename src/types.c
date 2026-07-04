/*
 * types.c — Phase 3: Type descriptor parsing
 *
 * Type descriptors define the data types that flow through edges
 * in the pipeline. Each port has an associated type, and edge
 * validation checks type compatibility between connected ports.
 * Types may reference base types for derived/array/struct variants.
 *
 * Reference: CYC Format Specification §3.2 - Type Descriptors
 */

#include "internal.h"

/* ── Type kind validation ───────────────────────────────────────── */

static int cy_is_valid_type_kind(uint8_t kind)
{
    switch (kind) {
    case CY_TYPE_INT:
    case CY_TYPE_FLOAT:
    case CY_TYPE_DOUBLE:
    case CY_TYPE_BOOL:
    case CY_TYPE_STRING:
    case CY_TYPE_BYTES:
    case CY_TYPE_STRUCT:
    case CY_TYPE_ARRAY:
    case CY_TYPE_ENUM:
    case CY_TYPE_TENSOR:
        return 1;
    default:
        return 0;
    }
}

/* ── Type size validation ───────────────────────────────────────── */

static int cy_validate_type_size(uint8_t kind, uint32_t size,
                                 cy_context *ctx, uint32_t id)
{
    switch (kind) {
    case CY_TYPE_BOOL:
        if (size != 1) {
            cy_add_warning(ctx, "type %u: bool type with size %u "
                           "(expected 1)", id, size);
        }
        break;
    case CY_TYPE_INT:
        if (size != 1 && size != 2 && size != 4 && size != 8) {
            cy_add_warning(ctx, "type %u: int type with non-standard "
                           "size %u", id, size);
        }
        break;
    case CY_TYPE_FLOAT:
        if (size != 4) {
            cy_add_warning(ctx, "type %u: float type with size %u "
                           "(expected 4)", id, size);
        }
        break;
    case CY_TYPE_DOUBLE:
        if (size != 8) {
            cy_add_warning(ctx, "type %u: double type with size %u "
                           "(expected 8)", id, size);
        }
        break;
    case CY_TYPE_STRING:
    case CY_TYPE_BYTES:
        /* Variable-length types, size is maximum or 0 for unbounded */
        break;
    case CY_TYPE_STRUCT:
        if (size == 0) {
            cy_add_warning(ctx, "type %u: struct with zero size", id);
        }
        break;
    case CY_TYPE_ARRAY:
        /* Size represents element count * element size */
        break;
    case CY_TYPE_ENUM:
        if (size != 1 && size != 2 && size != 4) {
            cy_add_warning(ctx, "type %u: enum with non-standard "
                           "size %u", id, size);
        }
        break;
    case CY_TYPE_TENSOR:
        /* Size represents total byte size of tensor data */
        break;
    }
    return CY_OK;
}

/* ── Type compatibility checking ────────────────────────────────── */

int cy_types_compatible(const cy_type_desc *src, const cy_type_desc *dst)
{
    if (!src || !dst)
        return 0;

    /* Identical type descriptors are always compatible */
    if (src == dst)
        return 1;

    /* Same kind and size is compatible */
    if (src->kind == dst->kind && src->size == dst->size)
        return 1;

    /* Numeric promotion: int → float, float → double */
    if (src->kind == CY_TYPE_INT && dst->kind == CY_TYPE_FLOAT)
        return 1;
    if (src->kind == CY_TYPE_FLOAT && dst->kind == CY_TYPE_DOUBLE)
        return 1;
    if (src->kind == CY_TYPE_INT && dst->kind == CY_TYPE_DOUBLE)
        return 1;

    /* Bytes accepts any source type (raw transport) */
    if (dst->kind == CY_TYPE_BYTES)
        return 1;

    /* Array of compatible element type */
    if (src->kind == CY_TYPE_ARRAY && dst->kind == CY_TYPE_ARRAY) {
        if (src->base_type && dst->base_type)
            return cy_types_compatible(src->base_type, dst->base_type);
    }

    /* Struct compatibility requires same name */
    if (src->kind == CY_TYPE_STRUCT && dst->kind == CY_TYPE_STRUCT) {
        if (src->name && dst->name)
            return (strcmp(src->name, dst->name) == 0) ? 1 : 0;
    }

    /* Tensor compatibility: same element type, any shape */
    if (src->kind == CY_TYPE_TENSOR && dst->kind == CY_TYPE_TENSOR) {
        if (src->base_type && dst->base_type)
            return cy_types_compatible(src->base_type, dst->base_type);
        return 1;
    }

    return 0;
}

/* ── Type structural equality (for folding) ─────────────────────── */

static int cy_types_structurally_equal(const cy_type_desc *a,
                                       const cy_type_desc *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->size != b->size)
        return 0;
    if (a->flags != b->flags)
        return 0;
    if (a->element_count != b->element_count)
        return 0;

    /* Compare names if both have them */
    if (a->name && b->name) {
        if (strcmp(a->name, b->name) != 0)
            return 0;
    } else if (a->name || b->name) {
        return 0;
    }

    /* Compare base types recursively */
    if (a->base_type && b->base_type)
        return cy_types_structurally_equal(a->base_type, b->base_type);
    if (a->base_type || b->base_type)
        return 0;

    return 1;
}

/* ── Type folding: merge structurally identical descriptors ──────── */
/* Keeps the canonical (lower-id) descriptor, frees duplicates,     */
/* and rewrites port type pointers to point to the canonical one.    */

int cy_fold_equivalent_types(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j, k;
    uint32_t folded = 0;

    if (!pipe->types || pipe->type_count < 2)
        return CY_OK;

    for (i = 0; i < pipe->type_count; i++) {
        cy_type_desc *canonical = &pipe->types[i];

        if (canonical->flags & 0x8000)  /* already folded away */
            continue;

        for (j = i + 1; j < pipe->type_count; j++) {
            cy_type_desc *dup = &pipe->types[j];

            if (dup->flags & 0x8000)
                continue;

            if (!cy_types_structurally_equal(canonical, dup))
                continue;

            /* Rewrite port type pointers from dup → canonical */
            for (k = 0; k < pipe->node_count; k++) {
                cy_node *node = &pipe->nodes[k];
                uint32_t p;
                if (!node->ports)
                    continue;
                for (p = 0; p < node->port_count; p++) {
                    if (node->ports[p].type == dup)
                        node->ports[p].type = canonical;
                }
            }

            /*
             * Rewrite node type references from dup → canonical.
             * Nodes referencing this type for their processing
             * category are updated.
             */
            for (k = 0; k < pipe->node_count; k++) {
                if (pipe->nodes[k].type == dup)
                    pipe->nodes[k].type = canonical;
            }

            /* Mark duplicate as folded */
            dup->flags |= 0x8000;
            folded++;
        }
    }

    if (folded > 0) {
        cy_add_warning(ctx, "folded %u equivalent type descriptors",
                       folded);

        /* Compact the type array: remove folded entries and
           reallocate to save memory. Port and node type pointers
           are updated to the new array locations. */
        uint32_t new_count = 0;
        for (i = 0; i < pipe->type_count; i++) {
            if (!(pipe->types[i].flags & 0x8000))
                new_count++;
        }

        if (new_count < pipe->type_count && new_count > 0) {
            cy_type_desc *new_types;
            uint32_t *id_map;  /* old id → new id */
            uint32_t new_idx = 0;

            new_types = (cy_type_desc *)cy_calloc(ctx, new_count,
                                                   sizeof(cy_type_desc));
            id_map = (uint32_t *)cy_calloc(ctx, pipe->type_count,
                                           sizeof(uint32_t));
            if (!new_types || !id_map) {
                cy_free(new_types);
                cy_free(id_map);
                return CY_OK;  /* non-fatal: skip compaction */
            }

            /* Copy non-folded types to new array */
            for (i = 0; i < pipe->type_count; i++) {
                if (pipe->types[i].flags & 0x8000) {
                    id_map[i] = 0;  /* folded types map to 0 */
                    continue;
                }
                new_types[new_idx] = pipe->types[i];
                new_types[new_idx].id = new_idx;
                id_map[i] = new_idx;
                new_idx++;
            }

            /* Update port type pointers to new array */
            for (k = 0; k < pipe->node_count; k++) {
                cy_node *node = &pipe->nodes[k];
                uint32_t p;
                if (!node->ports)
                    continue;
                for (p = 0; p < node->port_count; p++) {
                    if (node->ports[p].type) {
                        uint32_t old_id = node->ports[p].type_id;
                        if (old_id < pipe->type_count) {
                            uint32_t new_id = id_map[old_id];
                            node->ports[p].type = &new_types[new_id];
                            node->ports[p].type_id = new_id;
                        }
                    }
                }
            }

            /* Update node type references */
            for (k = 0; k < pipe->node_count; k++) {
                if (pipe->nodes[k].type) {
                    uint32_t old_id = pipe->nodes[k].type_id;
                    if (old_id < pipe->type_count) {
                        uint32_t new_id = id_map[old_id];
                        pipe->nodes[k].type = &new_types[new_id];
                        pipe->nodes[k].type_id = new_id;
                    }
                }
            }

            /* Update base_type pointers within new array */
            for (i = 0; i < new_count; i++) {
                if (new_types[i].base_type_id != 0xFFFFFFFF &&
                    new_types[i].base_type_id < pipe->type_count) {
                    uint32_t new_id = id_map[new_types[i].base_type_id];
                    new_types[i].base_type = &new_types[new_id];
                    new_types[i].base_type_id = new_id;
                }
            }

            /* Replace types array (frees old array).
               Edge src_type/dst_type pointers still reference
               the old array — they are NOT updated here. */
            cy_free(pipe->types);
            pipe->types = new_types;
            pipe->type_count = new_count;

            cy_free(id_map);
        }
    }

    return CY_OK;
}

/* ── Base type resolution ───────────────────────────────────────── */

static int cy_resolve_base_types(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->type_count; i++) {
        cy_type_desc *td = &pipe->types[i];

        if (td->base_type_id == 0xFFFFFFFF) {
            td->base_type = NULL;
            continue;
        }

        if (td->base_type_id >= pipe->type_count) {
            cy_add_warning(ctx,
                           "type %u: base_type_id %u out of range "
                           "(count=%u)",
                           i, td->base_type_id, pipe->type_count);
            td->base_type = NULL;
            continue;
        }

        if (td->base_type_id == i) {
            cy_add_warning(ctx,
                           "type %u: self-referencing base type", i);
            td->base_type = NULL;
            continue;
        }

        td->base_type = &pipe->types[td->base_type_id];
    }

    return CY_OK;
}

/* ── Cycle detection in type hierarchy ──────────────────────────── */

static int cy_detect_type_cycle(cy_type_desc *types, uint32_t count,
                                uint32_t start, cy_context *ctx)
{
    cy_type_desc *slow, *fast;
    uint32_t      steps = 0;

    slow = &types[start];
    fast = &types[start];

    while (fast && fast->base_type) {
        slow = slow->base_type;
        fast = fast->base_type;
        if (fast)
            fast = fast->base_type;
        steps++;

        if (steps > count) {
            cy_set_error(ctx, CY_ERR_BAD_TYPE,
                         "type hierarchy cycle detected starting "
                         "at type %u", start);
            return CY_ERR_BAD_TYPE;
        }

        if (slow == fast && slow != NULL) {
            cy_set_error(ctx, CY_ERR_BAD_TYPE,
                         "type hierarchy cycle detected involving "
                         "type %u", slow->id);
            return CY_ERR_BAD_TYPE;
        }
    }

    return CY_OK;
}

/* ── Main type table parser ─────────────────────────────────────── */

int cy_parse_types(cy_reader *r, cy_pipeline *pipe,
                   const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;

    if (count == 0) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "type table is empty");
        return CY_ERR_BAD_COUNT;
    }

    if (count > CY_MAX_TYPES) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "type count %u exceeds limit %u",
                     count, CY_MAX_TYPES);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_TYPE_REC_SIZE, dir->length,
                                  r->ctx, "type table");
    if (rc != CY_OK)
        return rc;

    /* Allocate type array */
    pipe->types = (cy_type_desc *)cy_calloc(r->ctx, count,
                                            sizeof(cy_type_desc));
    if (!pipe->types)
        return CY_ERR_NOMEM;

    /* Seek to type table section */
    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    /* Read type records */
    for (i = 0; i < count; i++) {
        cy_type_desc *td = &pipe->types[i];

        td->id            = i;
        td->name_id       = cy_read_u32(r);
        td->kind          = cy_read_u8(r);
        /* 3 bytes padding */
        cy_read_u8(r);
        cy_read_u16(r);
        td->size          = cy_read_u32(r);
        td->flags         = cy_read_u16(r);
        td->element_count = cy_read_u16(r);
        td->base_type_id  = cy_read_u32(r);
        /* 4 bytes reserved */
        cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Validate kind */
        if (!cy_is_valid_type_kind(td->kind)) {
            cy_add_warning(r->ctx,
                           "type %u: unknown kind %u, treating as bytes",
                           i, td->kind);
            td->kind = CY_TYPE_BYTES;
        }

        /* Resolve name from string table */
        td->name = cy_resolve_string(pipe, td->name_id);
        if (!td->name) {
            cy_add_warning(r->ctx,
                           "type %u: name_id %u not in string table",
                           i, td->name_id);
        }

        /* Validate size for this kind */
        cy_validate_type_size(td->kind, td->size, r->ctx, i);
    }

    pipe->type_count = count;

    /* Resolve base type references */
    rc = cy_resolve_base_types(pipe, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Check for cycles in type hierarchy */
    for (i = 0; i < count; i++) {
        if (pipe->types[i].base_type) {
            rc = cy_detect_type_cycle(pipe->types, count, i, r->ctx);
            if (rc != CY_OK)
                return rc;
        }
    }

    return CY_OK;
}

/* ── Type depth computation ─────────────────────────────────────── */

uint32_t cy_type_hierarchy_depth(const cy_type_desc *td)
{
    uint32_t depth = 0;
    const cy_type_desc *cur = td;

    while (cur && cur->base_type && depth < 256) {
        depth++;
        cur = cur->base_type;
    }

    return depth;
}

/* ── Type leaf resolution ───────────────────────────────────────── */

const cy_type_desc *cy_type_resolve_leaf(const cy_type_desc *td)
{
    uint32_t depth = 0;
    const cy_type_desc *cur = td;

    while (cur && cur->base_type && depth < 256) {
        cur = cur->base_type;
        depth++;
    }

    return cur;
}

/* ── Type alignment computation ─────────────────────────────────── */

uint32_t cy_type_alignment(const cy_type_desc *td)
{
    if (!td)
        return 1;

    switch (td->kind) {
    case CY_TYPE_BOOL:   return 1;
    case CY_TYPE_INT:
        if (td->size <= 1) return 1;
        if (td->size <= 2) return 2;
        if (td->size <= 4) return 4;
        return 8;
    case CY_TYPE_FLOAT:  return 4;
    case CY_TYPE_DOUBLE: return 8;
    case CY_TYPE_ENUM:   return td->size > 0 ? td->size : 4;
    case CY_TYPE_TENSOR: return 16;
    case CY_TYPE_STRUCT:
        /* Struct alignment is the max of its members;
           since we don't have member info, use size heuristic */
        if (td->size >= 8) return 8;
        if (td->size >= 4) return 4;
        if (td->size >= 2) return 2;
        return 1;
    case CY_TYPE_ARRAY:
        if (td->base_type)
            return cy_type_alignment(td->base_type);
        return 4;
    default:
        return 1;
    }
}

/* ── Type memory footprint with alignment ───────────────────────── */

uint32_t cy_type_aligned_size(const cy_type_desc *td)
{
    uint32_t size, align;

    if (!td)
        return 0;

    size = td->size;
    align = cy_type_alignment(td);

    if (align == 0)
        return size;

    return ((size + align - 1) / align) * align;
}

/* ── Type coercion distance ─────────────────────────────────────── */
/* Returns how many implicit conversions are needed to convert      */
/* from src to dst. Returns -1 if no coercion path exists.         */

int cy_type_coercion_distance(const cy_type_desc *src,
                              const cy_type_desc *dst)
{
    if (!src || !dst)
        return -1;

    /* Same type = zero distance */
    if (src == dst)
        return 0;

    if (src->kind == dst->kind && src->size == dst->size)
        return 0;

    /* Int → Float = 1 step */
    if (src->kind == CY_TYPE_INT && dst->kind == CY_TYPE_FLOAT)
        return 1;

    /* Int → Double = 1 step */
    if (src->kind == CY_TYPE_INT && dst->kind == CY_TYPE_DOUBLE)
        return 1;

    /* Float → Double = 1 step */
    if (src->kind == CY_TYPE_FLOAT && dst->kind == CY_TYPE_DOUBLE)
        return 1;

    /* Int → Int widening = 1 step */
    if (src->kind == CY_TYPE_INT && dst->kind == CY_TYPE_INT &&
        src->size < dst->size)
        return 1;

    /* Bool → Int = 1 step */
    if (src->kind == CY_TYPE_BOOL && dst->kind == CY_TYPE_INT)
        return 1;

    /* Enum → Int = 1 step */
    if (src->kind == CY_TYPE_ENUM && dst->kind == CY_TYPE_INT)
        return 1;

    /* Any → Bytes = 2 steps (serialize) */
    if (dst->kind == CY_TYPE_BYTES)
        return 2;

    /* Array → Array (if element types coercible) */
    if (src->kind == CY_TYPE_ARRAY && dst->kind == CY_TYPE_ARRAY) {
        if (src->base_type && dst->base_type) {
            int elem_dist = cy_type_coercion_distance(src->base_type,
                                                      dst->base_type);
            if (elem_dist >= 0)
                return elem_dist + 1;
        }
    }

    /* Tensor → Tensor (if element types coercible) */
    if (src->kind == CY_TYPE_TENSOR && dst->kind == CY_TYPE_TENSOR) {
        if (src->base_type && dst->base_type) {
            int elem_dist = cy_type_coercion_distance(src->base_type,
                                                      dst->base_type);
            if (elem_dist >= 0)
                return elem_dist + 1;
        }
        /* Same element kind = 1 step (reshape) */
        if (!src->base_type && !dst->base_type)
            return 1;
    }

    /* No coercion path */
    return -1;
}

/* ── Type equivalence class computation ─────────────────────────── */

int cy_compute_type_equiv_classes(const cy_pipeline *pipe,
                                  uint32_t *class_ids,
                                  uint32_t *class_count)
{
    uint32_t i, j;
    uint32_t next_class = 0;

    if (!pipe || !class_ids || !class_count)
        return -1;

    for (i = 0; i < pipe->type_count; i++)
        class_ids[i] = UINT32_MAX;

    for (i = 0; i < pipe->type_count; i++) {
        if (class_ids[i] != UINT32_MAX)
            continue;

        class_ids[i] = next_class;

        /* Find all types equivalent to this one */
        for (j = i + 1; j < pipe->type_count; j++) {
            if (class_ids[j] != UINT32_MAX)
                continue;
            if (cy_types_compatible(&pipe->types[i], &pipe->types[j]) &&
                cy_types_compatible(&pipe->types[j], &pipe->types[i])) {
                class_ids[j] = next_class;
            }
        }

        next_class++;
    }

    *class_count = next_class;
    return 0;
}
