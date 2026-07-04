/*
 * nodes.c — Phase 4: Node table parsing
 *
 * Nodes represent processing stages in the pipeline. Each node has
 * a type, a set of ports (parsed separately in ports.c), parameters,
 * and scheduling metadata. Nodes may belong to groups for hierarchical
 * organization. Later validation phases may split overloaded nodes
 * or merge redundant ones.
 *
 * Reference: CYC Format Specification §3.3 - Node Table
 */

#include "internal.h"

/* ── Node flag validation ───────────────────────────────────────── */

static int cy_validate_node_flags(uint32_t flags, uint32_t id,
                                  cy_context *ctx)
{
    uint32_t known_flags = CY_NODE_SOURCE | CY_NODE_SINK |
                           CY_NODE_STATEFUL | CY_NODE_PRUNED |
                           CY_NODE_MERGED | CY_NODE_SPLIT;

    if (flags & ~known_flags) {
        cy_add_warning(ctx, "node %u: unknown flags 0x%08x",
                       id, flags & ~known_flags);
    }

    /* Source and sink are mutually exclusive */
    if ((flags & CY_NODE_SOURCE) && (flags & CY_NODE_SINK)) {
        cy_add_warning(ctx, "node %u: both SOURCE and SINK flags set",
                       id);
    }

    return CY_OK;
}

/* ── Node type resolution ───────────────────────────────────────── */

static int cy_resolve_node_types(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];

        if (node->type_id >= pipe->type_count) {
            cy_add_warning(ctx,
                           "node %u (%s): type_id %u out of range "
                           "(count=%u)",
                           i,
                           node->name ? node->name : "unnamed",
                           node->type_id, pipe->type_count);
            node->type = NULL;
            continue;
        }

        node->type = &pipe->types[node->type_id];
    }

    return CY_OK;
}

/* ── Duplicate node name detection ──────────────────────────────── */

static int cy_check_duplicate_node_names(cy_pipeline *pipe,
                                         cy_context *ctx)
{
    uint32_t i, j;
    uint32_t dup_count = 0;

    for (i = 0; i < pipe->node_count; i++) {
        if (!pipe->nodes[i].name)
            continue;
        for (j = i + 1; j < pipe->node_count; j++) {
            if (!pipe->nodes[j].name)
                continue;
            if (strcmp(pipe->nodes[i].name, pipe->nodes[j].name) == 0) {
                dup_count++;
                if (dup_count <= 5) {
                    cy_add_warning(ctx,
                                   "duplicate node name '%s' at "
                                   "indices %u and %u",
                                   pipe->nodes[i].name, i, j);
                }
            }
        }
    }

    if (dup_count > 5) {
        cy_add_warning(ctx, "... and %u more duplicate node names",
                       dup_count - 5);
    }

    return CY_OK;
}

/* ── Node parameter range validation ────────────────────────────── */

static int cy_validate_node_param_refs(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        uint32_t end;

        if (node->param_count == 0)
            continue;

        if (cy_safe_add(node->param_start, node->param_count,
                        &end) != 0) {
            cy_add_warning(ctx,
                           "node %u: param range overflow "
                           "(start=%u, count=%u)",
                           i, node->param_start, node->param_count);
            node->param_count = 0;
            continue;
        }

        if (end > pipe->param_count) {
            cy_add_warning(ctx,
                           "node %u: param range [%u, %u) exceeds "
                           "param count %u",
                           i, node->param_start, end,
                           pipe->param_count);
            node->param_count = 0;
        }
    }

    return CY_OK;
}

/* ── Node splitting (for overloaded nodes) ──────────────────────── */
/* When a node has more output edges than its declared maximum,     */
/* split it into two nodes with partitioned ports.                  */

int cy_split_overloaded_node(cy_pipeline *pipe, cy_node *node,
                             cy_context *ctx)
{
    uint32_t total_outputs = 0;
    uint32_t split_point;
    uint32_t i;
    cy_port *new_ports_a = NULL;
    cy_port *new_ports_b = NULL;
    cy_node *new_node = NULL;
    uint32_t count_a, count_b;
    uint32_t old_node_count;

    /* Count output ports */
    for (i = 0; i < node->port_count; i++) {
        if (node->ports[i].direction == CY_PORT_OUTPUT)
            total_outputs++;
    }

    if (total_outputs <= CY_MAX_ARITY)
        return CY_OK;

    /* Split at midpoint of output ports */
    split_point = node->port_count / 2;
    if (split_point == 0)
        split_point = 1;

    count_a = split_point;
    count_b = node->port_count - split_point;

    /* Allocate new port arrays for each half */
    new_ports_a = (cy_port *)cy_calloc(ctx, count_a, sizeof(cy_port));
    if (!new_ports_a)
        return CY_ERR_NOMEM;

    new_ports_b = (cy_port *)cy_calloc(ctx, count_b, sizeof(cy_port));
    if (!new_ports_b) {
        cy_free(new_ports_a);
        return CY_ERR_NOMEM;
    }

    /* Copy ports to new arrays */
    memcpy(new_ports_a, node->ports,
           count_a * sizeof(cy_port));
    memcpy(new_ports_b, node->ports + count_a,
           count_b * sizeof(cy_port));

    /* Copy connected_edges arrays for each port */
    for (i = 0; i < count_a; i++) {
        new_ports_a[i].connected_edges = NULL;
        new_ports_a[i].connected_count = 0;
        new_ports_a[i].connected_cap = 0;
    }
    for (i = 0; i < count_b; i++) {
        new_ports_b[i].connected_edges = NULL;
        new_ports_b[i].connected_count = 0;
        new_ports_b[i].connected_cap = 0;
    }

    /* Expand node array to add the split node */
    old_node_count = pipe->node_count;
    pipe->nodes = (cy_node *)cy_realloc(ctx, pipe->nodes,
                                        (old_node_count + 1) *
                                        sizeof(cy_node));
    if (!pipe->nodes) {
        cy_free(new_ports_a);
        cy_free(new_ports_b);
        return CY_ERR_NOMEM;
    }

    /* The original node may have moved due to realloc; find it */
    node = NULL;
    for (i = 0; i < old_node_count; i++) {
        if (pipe->nodes[i].ports != new_ports_a &&
            pipe->nodes[i].ports != new_ports_b &&
            pipe->nodes[i].port_count == count_a + count_b) {
            /* This check is approximate; match by original port count */
            node = &pipe->nodes[i];
            break;
        }
    }

    if (!node) {
        /* Fallback: use first node with matching total port count */
        for (i = 0; i < old_node_count; i++) {
            if (pipe->nodes[i].port_count == count_a + count_b) {
                node = &pipe->nodes[i];
                break;
            }
        }
    }

    if (!node) {
        cy_free(new_ports_a);
        cy_free(new_ports_b);
        return CY_ERR_INTERNAL;
    }

    /* Free original port array */
    for (i = 0; i < node->port_count; i++) {
        cy_free(node->ports[i].connected_edges);
    }
    cy_free(node->ports);

    /* Set up the new split node */
    new_node = &pipe->nodes[old_node_count];
    memset(new_node, 0, sizeof(cy_node));
    new_node->id = old_node_count;
    new_node->name_id = node->name_id;
    new_node->name = node->name;
    new_node->type_id = node->type_id;
    new_node->type = node->type;
    new_node->ports = new_ports_b;
    new_node->port_count = count_b;
    new_node->flags = node->flags | CY_NODE_SPLIT;
    new_node->max_port_capacity = node->max_port_capacity;
    new_node->group_id = node->group_id;

    /* Update original node with first half */
    node->ports = new_ports_a;
    node->port_count = count_a;
    node->flags |= CY_NODE_SPLIT;

    /* Update edge source/dest pointers for affected edges */
    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];
        uint32_t p;

        /* Check if edge endpoints were in the second half */
        for (p = 0; p < count_b; p++) {
            if (e->src_port_id == new_ports_b[p].id)
                e->src_port = &new_ports_b[p];
            if (e->dst_port_id == new_ports_b[p].id)
                e->dst_port = &new_ports_b[p];
        }
        /* Update first-half references */
        for (p = 0; p < count_a; p++) {
            if (e->src_port_id == new_ports_a[p].id)
                e->src_port = &new_ports_a[p];
            if (e->dst_port_id == new_ports_a[p].id)
                e->dst_port = &new_ports_a[p];
        }
    }

    /* Update port index */
    for (i = 0; i < count_a; i++) {
        uint32_t pid = new_ports_a[i].id;
        if (pid < pipe->port_count)
            pipe->port_index[pid] = &new_ports_a[i];
    }
    for (i = 0; i < count_b; i++) {
        uint32_t pid = new_ports_b[i].id;
        if (pid < pipe->port_count)
            pipe->port_index[pid] = &new_ports_b[i];
    }

    pipe->node_count = old_node_count + 1;

    cy_add_warning(ctx, "split overloaded node %u (%s) into %u + %u ports",
                   node->id,
                   node->name ? node->name : "unnamed",
                   count_a, count_b);

    return CY_OK;
}

/* ── Node merging (redundant node elimination) ──────────────────── */

static int cy_nodes_equivalent(const cy_node *a, const cy_node *b)
{
    if (a->type_id != b->type_id)
        return 0;
    if (a->port_count != b->port_count)
        return 0;
    if (a->param_count != b->param_count)
        return 0;
    if ((a->flags & 0x0F) != (b->flags & 0x0F))
        return 0;
    return 1;
}

int cy_merge_redundant_nodes(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j, k;
    uint32_t merged = 0;

    if (pipe->node_count < 2)
        return CY_OK;

    for (i = 0; i < pipe->node_count && i < CY_MAX_MERGE_SCAN; i++) {
        cy_node *primary = &pipe->nodes[i];

        if (primary->flags & CY_NODE_PRUNED)
            continue;
        if (primary->flags & CY_NODE_MERGED)
            continue;

        for (j = i + 1; j < pipe->node_count; j++) {
            cy_node *dup = &pipe->nodes[j];

            if (dup->flags & CY_NODE_PRUNED)
                continue;
            if (dup->flags & CY_NODE_MERGED)
                continue;

            if (!cy_nodes_equivalent(primary, dup))
                continue;

            /* Redirect edges from dup's ports to primary's ports */
            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *e = &pipe->edges[k];
                uint32_t p;

                for (p = 0; p < dup->port_count; p++) {
                    if (e->src_port_id == dup->ports[p].id &&
                        p < primary->port_count) {
                        e->src_port_id = primary->ports[p].id;
                        e->src_port = &primary->ports[p];
                    }
                    if (e->dst_port_id == dup->ports[p].id &&
                        p < primary->port_count) {
                        e->dst_port_id = primary->ports[p].id;
                        e->dst_port = &primary->ports[p];
                    }
                }
            }

            /* Free the duplicate node's ports */
            if (dup->ports) {
                for (k = 0; k < dup->port_count; k++) {
                    cy_free(dup->ports[k].connected_edges);
                    dup->ports[k].connected_edges = NULL;
                }
                cy_free(dup->ports);
            }

            dup->flags |= CY_NODE_MERGED;
            merged++;
        }
    }

    if (merged > 0) {
        cy_add_warning(ctx, "merged %u redundant nodes", merged);
    }

    return CY_OK;
}

/* ── Main node table parser ─────────────────────────────────────── */

int cy_parse_nodes(cy_reader *r, cy_pipeline *pipe,
                   const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;

    if (count == 0) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "node table is empty");
        return CY_ERR_BAD_COUNT;
    }

    if (count > CY_MAX_NODES) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "node count %u exceeds limit %u",
                     count, CY_MAX_NODES);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_NODE_REC_SIZE, dir->length,
                                  r->ctx, "node table");
    if (rc != CY_OK)
        return rc;

    /* Allocate node array */
    pipe->nodes = (cy_node *)cy_calloc(r->ctx, count, sizeof(cy_node));
    if (!pipe->nodes)
        return CY_ERR_NOMEM;

    /* Seek to node table */
    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    /* Read node records */
    for (i = 0; i < count; i++) {
        cy_node *node = &pipe->nodes[i];

        node->id                = i;
        node->name_id           = cy_read_u32(r);
        node->type_id           = cy_read_u32(r);
        node->port_start        = cy_read_u32(r);
        node->port_count        = cy_read_u16(r);
        node->max_port_capacity = cy_read_u16(r);
        node->param_start       = cy_read_u32(r);
        node->param_count       = cy_read_u16(r);
        node->group_id          = cy_read_u16(r);
        node->flags             = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Resolve name */
        node->name = cy_resolve_string(pipe, node->name_id);
        if (!node->name) {
            cy_add_warning(r->ctx,
                           "node %u: name_id %u not in string table",
                           i, node->name_id);
        }

        /* Validate flags */
        cy_validate_node_flags(node->flags, i, r->ctx);

        /* Ports are allocated in parse_ports (Phase 5) */
        node->ports = NULL;
    }

    pipe->node_count = count;

    /* Resolve node type references */
    rc = cy_resolve_node_types(pipe, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Check for duplicate names */
    cy_check_duplicate_node_names(pipe, r->ctx);

    /* Validate param references (deferred until params are parsed) */
    if (pipe->param_count > 0)
        cy_validate_node_param_refs(pipe, r->ctx);

    return CY_OK;
}

/* ── Node connectivity analysis ─────────────────────────────────── */

int cy_analyze_node_connectivity(const cy_pipeline *pipe,
                                 uint32_t node_id,
                                 uint32_t *in_edges,
                                 uint32_t *out_edges,
                                 uint32_t *bidir_edges)
{
    uint32_t i;
    uint32_t in_count = 0, out_count = 0, bidi_count = 0;

    if (!pipe || !in_edges || !out_edges || !bidir_edges)
        return -1;

    if (node_id >= pipe->node_count)
        return -1;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;

        int is_src = (e->src_port->node_id == node_id);
        int is_dst = (e->dst_port->node_id == node_id);

        if (is_src && is_dst)
            bidi_count++;  /* self-loop */
        else if (is_src)
            out_count++;
        else if (is_dst)
            in_count++;
    }

    *in_edges = in_count;
    *out_edges = out_count;
    *bidir_edges = bidi_count;
    return 0;
}

/* ── Node resource consumption estimation ───────────────────────── */

uint64_t cy_estimate_node_memory(const cy_pipeline *pipe,
                                 uint32_t node_id)
{
    uint64_t total = 0;
    uint32_t i;

    if (!pipe || node_id >= pipe->node_count)
        return 0;

    const cy_node *node = &pipe->nodes[node_id];

    /* Port memory */
    total += (uint64_t)node->port_count * sizeof(cy_port);

    /* Param memory */
    total += (uint64_t)node->param_count * sizeof(cy_param);

    /* Buffer memory (sum of all outgoing edge buffers) */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (e->src_port && e->src_port->node_id == node_id) {
            if (e->flags & CY_EDGE_BUFFERED)
                total += e->buffer_size;
        }
    }

    /* Schedule buffer allocation */
    if (pipe->schedule) {
        for (i = 0; i < pipe->schedule_count; i++) {
            if (pipe->schedule[i].node_id == node_id) {
                total += pipe->schedule[i].buffer_alloc;
                break;
            }
        }
    }

    return total;
}

/* ── Node similarity score computation ──────────────────────────── */

int cy_compute_node_similarity(const cy_pipeline *pipe,
                               uint32_t node_a, uint32_t node_b,
                               double *similarity)
{
    uint32_t i;
    int score = 0;
    int max_score = 0;

    if (!pipe || !similarity)
        return -1;
    if (node_a >= pipe->node_count || node_b >= pipe->node_count)
        return -1;

    const cy_node *a = &pipe->nodes[node_a];
    const cy_node *b = &pipe->nodes[node_b];

    /* Type match */
    max_score += 4;
    if (a->type_id == b->type_id)
        score += 4;
    else if (a->type && b->type && a->type->kind == b->type->kind)
        score += 2;

    /* Port count match */
    max_score += 3;
    if (a->port_count == b->port_count)
        score += 3;
    else if (a->port_count > 0 && b->port_count > 0) {
        uint32_t diff = a->port_count > b->port_count
                        ? a->port_count - b->port_count
                        : b->port_count - a->port_count;
        uint32_t maxp = a->port_count > b->port_count
                        ? a->port_count : b->port_count;
        if (maxp > 0 && diff * 4 <= maxp)
            score += 1;
    }

    /* Param count match */
    max_score += 2;
    if (a->param_count == b->param_count)
        score += 2;

    /* Flag match (masking out runtime flags) */
    max_score += 2;
    if ((a->flags & 0x0F) == (b->flags & 0x0F))
        score += 2;

    /* Port type distribution match */
    if (a->ports && b->ports) {
        uint32_t a_in = 0, a_out = 0;
        uint32_t b_in = 0, b_out = 0;

        for (i = 0; i < a->port_count; i++) {
            if (a->ports[i].direction == CY_PORT_INPUT) a_in++;
            else if (a->ports[i].direction == CY_PORT_OUTPUT) a_out++;
        }
        for (i = 0; i < b->port_count; i++) {
            if (b->ports[i].direction == CY_PORT_INPUT) b_in++;
            else if (b->ports[i].direction == CY_PORT_OUTPUT) b_out++;
        }

        max_score += 3;
        if (a_in == b_in) score += 1;
        if (a_out == b_out) score += 1;
        if (a_in == b_in && a_out == b_out) score += 1;
    }

    *similarity = max_score > 0
                  ? (double)score / (double)max_score
                  : 0.0;
    return 0;
}

/* ── Find all sink nodes reachable from a given node ────────────── */

int cy_find_reachable_sinks(const cy_pipeline *pipe,
                            uint32_t node_id,
                            uint32_t *sinks,
                            uint32_t max_count,
                            uint32_t *sink_count)
{
    uint8_t  *visited = NULL;
    uint32_t *queue = NULL;
    uint32_t  q_head = 0, q_tail = 0;
    uint32_t  found = 0;
    uint32_t  i;

    if (!pipe || !sinks || !sink_count)
        return -1;

    visited = (uint8_t *)calloc(pipe->node_count, 1);
    queue = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    if (!visited || !queue) {
        free(visited);
        free(queue);
        return -1;
    }

    visited[node_id] = 1;
    queue[q_tail++] = node_id;

    while (q_head < q_tail) {
        uint32_t cur = queue[q_head++];

        if (pipe->nodes[cur].flags & CY_NODE_SINK) {
            if (found < max_count)
                sinks[found] = cur;
            found++;
        }

        for (i = 0; i < pipe->edge_count; i++) {
            const cy_edge *e = &pipe->edges[i];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (!e->src_port || !e->dst_port)
                continue;
            if (e->src_port->node_id != cur)
                continue;

            uint32_t next = e->dst_port->node_id;
            if (next < pipe->node_count && !visited[next]) {
                visited[next] = 1;
                queue[q_tail++] = next;
            }
        }
    }

    *sink_count = found;
    free(visited);
    free(queue);
    return 0;
}
