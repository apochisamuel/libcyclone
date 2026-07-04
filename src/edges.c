/*
 * edges.c — Phase 6: Edge table parsing
 *
 * Edges connect output ports to input ports, forming the dataflow
 * graph. Each edge references source and destination ports by ID,
 * carries type information cached from the connected ports, and has
 * attributes for buffering, weight, and feedback classification.
 * New edges may be inserted during validation (implicit group edges).
 *
 * Reference: CYC Format Specification §3.5 - Edge Table
 */

#include "internal.h"

/* ── Edge flag validation ───────────────────────────────────────── */

static int cy_validate_edge_flags(uint32_t flags, uint32_t id,
                                  cy_context *ctx)
{
    uint32_t known = CY_EDGE_BUFFERED | CY_EDGE_OPTIONAL |
                     CY_EDGE_FEEDBACK | CY_EDGE_VALID |
                     CY_EDGE_IMPLICIT;

    if (flags & ~known) {
        cy_add_warning(ctx, "edge %u: unknown flags 0x%08x",
                       id, flags & ~known);
    }
    return CY_OK;
}

/* ── Port endpoint resolution ───────────────────────────────────── */

static cy_port *cy_resolve_port_by_id(cy_pipeline *pipe, uint32_t port_id,
                                      cy_context *ctx, uint32_t edge_id,
                                      const char *role)
{
    cy_port *port;

    if (port_id >= pipe->port_count) {
        cy_add_warning(ctx,
                       "edge %u: %s port_id %u out of range "
                       "(port_count=%u)",
                       edge_id, role, port_id, pipe->port_count);
        return NULL;
    }

    port = pipe->port_index[port_id];
    if (!port) {
        cy_add_warning(ctx,
                       "edge %u: %s port_id %u has no resolved port",
                       edge_id, role, port_id);
        return NULL;
    }

    return port;
}

/* ── Register edge with port back-pointer ───────────────────────── */

static int cy_register_edge_with_port(cy_port *port, cy_edge *edge,
                                      cy_context *ctx)
{
    if (!port || !edge)
        return CY_OK;

    if (port->connected_count >= port->connected_cap) {
        uint32_t new_cap = port->connected_cap == 0
                           ? 4
                           : port->connected_cap * 2;
        cy_edge **new_arr;

        new_arr = (cy_edge **)cy_realloc(ctx, port->connected_edges,
                                         new_cap * sizeof(cy_edge *));
        if (!new_arr)
            return CY_ERR_NOMEM;

        port->connected_edges = new_arr;
        port->connected_cap = new_cap;
    }

    port->connected_edges[port->connected_count] = edge;
    port->connected_count++;
    return CY_OK;
}

/* ── Direction compatibility check ──────────────────────────────── */

static int cy_validate_edge_directions(const cy_port *src,
                                       const cy_port *dst,
                                       uint32_t edge_id, cy_context *ctx)
{
    /* Source must be output or bidirectional */
    if (src->direction == CY_PORT_INPUT) {
        cy_add_warning(ctx,
                       "edge %u: source port %u is an input port",
                       edge_id, src->id);
        return 0;
    }

    /* Destination must be input or bidirectional */
    if (dst->direction == CY_PORT_OUTPUT) {
        cy_add_warning(ctx,
                       "edge %u: destination port %u is an output port",
                       edge_id, dst->id);
        return 0;
    }

    return 1;
}

/* ── Self-loop detection ────────────────────────────────────────── */

static int cy_check_self_loop(const cy_port *src, const cy_port *dst,
                              uint32_t edge_id, cy_context *ctx)
{
    if (src->node_id == dst->node_id) {
        cy_add_warning(ctx,
                       "edge %u: self-loop on node %u (port %u → %u)",
                       edge_id, src->node_id, src->id, dst->id);
        return 1;
    }
    return 0;
}

/* ── Duplicate edge detection ───────────────────────────────────── */

static int cy_is_duplicate_edge(const cy_edge *edges, uint32_t count,
                                uint32_t src_port_id, uint32_t dst_port_id)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (edges[i].src_port_id == src_port_id &&
            edges[i].dst_port_id == dst_port_id) {
            return 1;
        }
    }
    return 0;
}

/* ── Add edge (used during validation for implicit edges) ──────── */
/* May reallocate the edge array, invalidating all cy_edge pointers */

int cy_add_edge(cy_pipeline *pipe, cy_context *ctx,
                uint32_t src_port_id, uint32_t dst_port_id,
                uint32_t flags, uint16_t weight, uint32_t buf_size)
{
    cy_edge *e;
    cy_port *src_port, *dst_port;

    /* Grow edge array if needed */
    if (pipe->edge_count >= pipe->edge_cap) {
        uint32_t new_cap = pipe->edge_cap == 0
                           ? 16
                           : pipe->edge_cap * 2;
        cy_edge *new_edges;

        new_edges = (cy_edge *)cy_realloc(ctx, pipe->edges,
                                          new_cap * sizeof(cy_edge));
        if (!new_edges)
            return CY_ERR_NOMEM;

        pipe->edges = new_edges;
        pipe->edge_cap = new_cap;
    }

    /* Initialize the new edge */
    e = &pipe->edges[pipe->edge_count];
    memset(e, 0, sizeof(cy_edge));

    e->id          = pipe->edge_count;
    e->src_port_id = src_port_id;
    e->dst_port_id = dst_port_id;
    e->flags       = flags | CY_EDGE_VALID | CY_EDGE_IMPLICIT;
    e->weight      = weight;
    e->buffer_size = buf_size;

    /* Resolve port endpoints */
    src_port = cy_resolve_port_by_id(pipe, src_port_id, ctx,
                                     e->id, "src");
    dst_port = cy_resolve_port_by_id(pipe, dst_port_id, ctx,
                                     e->id, "dst");

    e->src_port = src_port;
    e->dst_port = dst_port;

    /* Cache type descriptors from ports */
    if (src_port && src_port->type)
        e->src_type = src_port->type;
    if (dst_port && dst_port->type)
        e->dst_type = dst_port->type;

    pipe->edge_count++;

    return CY_OK;
}

/* ── Edge buffer size validation ────────────────────────────────── */

static void cy_validate_buffer_sizes(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];

        if (!(e->flags & CY_EDGE_BUFFERED) && e->buffer_size > 0) {
            cy_add_warning(ctx,
                           "edge %u: buffer_size %u set but BUFFERED "
                           "flag not set",
                           i, e->buffer_size);
        }

        if ((e->flags & CY_EDGE_BUFFERED) && e->buffer_size == 0) {
            cy_add_warning(ctx,
                           "edge %u: BUFFERED flag set but buffer_size "
                           "is zero",
                           i);
        }
    }
}

/* ── Weight distribution analysis ───────────────────────────────── */

static void cy_analyze_edge_weights(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t zero_weight = 0;
    uint32_t max_weight = 0;
    uint64_t total_weight = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        uint16_t w = pipe->edges[i].weight;
        if (w == 0) {
            zero_weight++;
        }
        if (w > max_weight)
            max_weight = w;
        total_weight += w;
    }

    if (zero_weight > pipe->edge_count / 2) {
        cy_add_warning(ctx,
                       "majority of edges (%u/%u) have zero weight",
                       zero_weight, pipe->edge_count);
    }

    (void)max_weight;
    (void)total_weight;
}

/* ── Feedback edge validation ───────────────────────────────────── */

static void cy_validate_feedback_edges(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t feedback_count = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        if (pipe->edges[i].flags & CY_EDGE_FEEDBACK) {
            feedback_count++;

            /* Feedback edges should be buffered */
            if (!(pipe->edges[i].flags & CY_EDGE_BUFFERED)) {
                cy_add_warning(ctx,
                               "edge %u: FEEDBACK without BUFFERED "
                               "may cause deadlock",
                               i);
            }
        }
    }

    if (feedback_count > pipe->edge_count / 4) {
        cy_add_warning(ctx,
                       "high feedback edge ratio (%u/%u) may indicate "
                       "scheduling issues",
                       feedback_count, pipe->edge_count);
    }
}

/* ── Main edge table parser ─────────────────────────────────────── */

int cy_parse_edges(cy_reader *r, cy_pipeline *pipe,
                   const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;

    if (count > CY_MAX_EDGES) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "edge count %u exceeds limit %u",
                     count, CY_MAX_EDGES);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_EDGE_REC_SIZE, dir->length,
                                  r->ctx, "edge table");
    if (rc != CY_OK)
        return rc;

    /* Allocate edge array with extra capacity for implicit edges */
    pipe->edge_cap = count + 16;
    pipe->edges = (cy_edge *)cy_calloc(r->ctx, pipe->edge_cap,
                                       sizeof(cy_edge));
    if (!pipe->edges)
        return CY_ERR_NOMEM;

    /* Seek to edge table */
    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    /* Read edge records */
    for (i = 0; i < count; i++) {
        cy_edge *e = &pipe->edges[i];
        cy_port *src, *dst;

        e->id          = i;
        e->src_port_id = cy_read_u32(r);
        e->dst_port_id = cy_read_u32(r);
        e->flags       = cy_read_u32(r);
        e->weight      = cy_read_u16(r);
        /* 2 bytes padding */
        cy_read_u16(r);
        e->buffer_size = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Validate flags */
        cy_validate_edge_flags(e->flags, i, r->ctx);

        /* Mark edge as valid by default */
        e->flags |= CY_EDGE_VALID;

        /* Resolve source port */
        src = cy_resolve_port_by_id(pipe, e->src_port_id, r->ctx,
                                    i, "src");
        e->src_port = src;

        /* Resolve destination port */
        dst = cy_resolve_port_by_id(pipe, e->dst_port_id, r->ctx,
                                    i, "dst");
        e->dst_port = dst;

        /* Cache type descriptors from connected ports */
        if (src && src->type)
            e->src_type = src->type;
        if (dst && dst->type)
            e->dst_type = dst->type;

        /* Validate directions */
        if (src && dst) {
            cy_validate_edge_directions(src, dst, i, r->ctx);
            cy_check_self_loop(src, dst, i, r->ctx);
        }

        /* Check for duplicate edges */
        if (i > 0 && cy_is_duplicate_edge(pipe->edges, i,
                                          e->src_port_id,
                                          e->dst_port_id)) {
            cy_add_warning(r->ctx,
                           "edge %u: duplicate connection from port %u "
                           "to port %u",
                           i, e->src_port_id, e->dst_port_id);
        }

        /* Register back-pointers with ports */
        if (src) {
            rc = cy_register_edge_with_port(src, e, r->ctx);
            if (rc != CY_OK)
                return rc;
        }
        if (dst) {
            rc = cy_register_edge_with_port(dst, e, r->ctx);
            if (rc != CY_OK)
                return rc;
        }
    }

    pipe->edge_count = count;

    /* Post-parse validation */
    cy_validate_buffer_sizes(pipe, r->ctx);
    cy_analyze_edge_weights(pipe, r->ctx);
    cy_validate_feedback_edges(pipe, r->ctx);

    return CY_OK;
}

/* ── Edge adjacency matrix construction ─────────────────────────── */

int cy_build_adjacency_matrix(const cy_pipeline *pipe,
                              uint8_t *matrix,
                              uint32_t node_count)
{
    uint32_t i;

    if (!pipe || !matrix)
        return -1;
    if (node_count < pipe->node_count)
        return -1;

    memset(matrix, 0, (size_t)node_count * node_count);

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;

        uint32_t src = e->src_port->node_id;
        uint32_t dst = e->dst_port->node_id;

        if (src < node_count && dst < node_count) {
            matrix[src * node_count + dst] = 1;
        }
    }

    return 0;
}

/* ── Edge statistics by type ────────────────────────────────────── */

int cy_edge_stats_by_type(const cy_pipeline *pipe,
                          uint32_t type_id,
                          uint32_t *edge_count,
                          uint64_t *total_weight,
                          uint64_t *total_buffer)
{
    uint32_t i;
    uint32_t count = 0;
    uint64_t weight = 0;
    uint64_t buffer = 0;

    if (!pipe || !edge_count || !total_weight || !total_buffer)
        return -1;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;

        int matches = 0;
        if (e->src_type && e->src_type->id == type_id)
            matches = 1;
        if (e->dst_type && e->dst_type->id == type_id)
            matches = 1;

        if (matches) {
            count++;
            weight += e->weight;
            buffer += e->buffer_size;
        }
    }

    *edge_count = count;
    *total_weight = weight;
    *total_buffer = buffer;
    return 0;
}

/* ── Parallel edge detection ────────────────────────────────────── */

uint32_t cy_count_parallel_edges(const cy_pipeline *pipe,
                                 uint32_t src_node, uint32_t dst_node)
{
    uint32_t i;
    uint32_t count = 0;

    if (!pipe || !pipe->edges)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->src_port->node_id == src_node &&
            e->dst_port->node_id == dst_node) {
            count++;
        }
    }

    return count;
}

/* ── Edge transitivity check ────────────────────────────────────── */

int cy_edge_is_transitive(const cy_pipeline *pipe, uint32_t edge_id)
{
    const cy_edge *target;
    uint32_t src_node, dst_node;
    uint32_t i, j;

    if (!pipe || edge_id >= pipe->edge_count)
        return 0;

    target = &pipe->edges[edge_id];
    if (!target->src_port || !target->dst_port)
        return 0;

    src_node = target->src_port->node_id;
    dst_node = target->dst_port->node_id;

    /* Check if there's a 2-hop path from src to dst
       that makes this direct edge redundant */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e1 = &pipe->edges[i];
        if (i == edge_id || !(e1->flags & CY_EDGE_VALID))
            continue;
        if (!e1->src_port || !e1->dst_port)
            continue;
        if (e1->src_port->node_id != src_node)
            continue;

        uint32_t mid_node = e1->dst_port->node_id;

        for (j = 0; j < pipe->edge_count; j++) {
            const cy_edge *e2 = &pipe->edges[j];
            if (j == edge_id || !(e2->flags & CY_EDGE_VALID))
                continue;
            if (!e2->src_port || !e2->dst_port)
                continue;
            if (e2->src_port->node_id == mid_node &&
                e2->dst_port->node_id == dst_node) {
                return 1;  /* Transitive: src→mid→dst exists */
            }
        }
    }

    return 0;
}
