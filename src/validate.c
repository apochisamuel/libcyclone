/*
 * validate.c — Phase 12: Cross-table validation
 *
 * Validates consistency across all parsed tables. This phase may
 * modify the pipeline by:
 *   - Marking type-incompatible edges as invalid
 *   - Pruning nodes with no valid input edges
 *   - Merging structurally redundant nodes
 *   - Folding structurally identical type descriptors
 *   - Dissolving groups with internal cycles
 *   - Splitting nodes with excessive output arity
 *   - Pruning nodes that exceed the latency budget
 *   - Normalizing parameter names
 *
 * These mutations can invalidate cached pointers built in phases
 * 10-11 (port cache, route table). The resolve phase (13) reads
 * those cached pointers, which may now be dangling.
 *
 * Reference: CYC Format Specification §5 - Validation
 */

#include "internal.h"

/* ── Type compatibility validation ──────────────────────────────── */
/* Checks each edge's source and destination types. If they are     */
/* incompatible, the edge is marked invalid.                        */

int cy_validate_type_compat(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t invalid_count = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];

        if (!(e->flags & CY_EDGE_VALID))
            continue;

        if (!e->src_type || !e->dst_type) {
            /* Missing type information is non-fatal */
            if (e->src_port && e->dst_port) {
                cy_add_warning(ctx,
                               "edge %u: missing type information "
                               "(src_type=%p, dst_type=%p)",
                               i, (void *)e->src_type,
                               (void *)e->dst_type);
            }
            continue;
        }

        if (!cy_types_compatible(e->src_type, e->dst_type)) {
            e->flags &= ~CY_EDGE_VALID;
            invalid_count++;

            if (invalid_count <= 10) {
                cy_add_warning(ctx,
                               "edge %u: type mismatch between "
                               "%s (%s) and %s (%s)",
                               i,
                               e->src_type->name ?
                                   e->src_type->name : "unnamed",
                               e->src_port && e->src_port->name ?
                                   e->src_port->name : "?",
                               e->dst_type->name ?
                                   e->dst_type->name : "unnamed",
                               e->dst_port && e->dst_port->name ?
                                   e->dst_port->name : "?");
            }
        }
    }

    if (invalid_count > 10) {
        cy_add_warning(ctx,
                       "... and %u more type-incompatible edges",
                       invalid_count - 10);
    }

    return CY_OK;
}

/* ── Node pruning ───────────────────────────────────────────────── */
/* Prunes a single node: frees its port array and marks it.         */

void cy_prune_node(cy_pipeline *pipe, cy_node *node, cy_context *ctx)
{
    uint32_t i;

    (void)pipe;
    (void)ctx;

    if (!node || (node->flags & CY_NODE_PRUNED))
        return;

    node->flags |= CY_NODE_PRUNED;

    /* Free the node's port allocation */
    if (node->ports) {
        for (i = 0; i < node->port_count; i++) {
            cy_free(node->ports[i].connected_edges);
            node->ports[i].connected_edges = NULL;
        }
        cy_free(node->ports);
        node->ports = NULL;
    }
}

/* ── Disconnected node pruning ──────────────────────────────────── */
/* Finds nodes with no valid input edges (that are not sources)     */
/* and prunes them. This frees the node's port array, which may     */
/* leave dangling pointers in the port cache and route table.       */

int cy_prune_disconnected_nodes(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t pruned = 0;
    int      changed;

    /* Iterate until no more nodes are pruned (cascade) */
    do {
        changed = 0;

        for (i = 0; i < pipe->node_count; i++) {
            cy_node *node = &pipe->nodes[i];
            int has_valid_input = 0;

            if (node->flags & CY_NODE_PRUNED)
                continue;
            if (node->flags & CY_NODE_SOURCE)
                continue;
            if (node->flags & CY_NODE_MERGED)
                continue;

            /* Check if this node has any valid input edges */
            for (j = 0; j < pipe->edge_count; j++) {
                cy_edge *e = &pipe->edges[j];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if (!e->dst_port)
                    continue;
                if (e->dst_port->node_id == node->id) {
                    has_valid_input = 1;
                    break;
                }
            }

            if (!has_valid_input) {
                /* Invalidate all edges from this node's ports */
                for (j = 0; j < pipe->edge_count; j++) {
                    cy_edge *e = &pipe->edges[j];
                    if (!e->src_port)
                        continue;
                    if (e->src_port->node_id == node->id) {
                        e->flags &= ~CY_EDGE_VALID;
                    }
                }

                cy_prune_node(pipe, node, ctx);
                pruned++;
                changed = 1;
            }
        }
    } while (changed);

    if (pruned > 0) {
        cy_add_warning(ctx, "pruned %u disconnected nodes", pruned);
    }

    return CY_OK;
}

/* ── Group validation ───────────────────────────────────────────── */
/* Checks groups for internal cycles and dissolves those found.     */

int cy_validate_groups(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->group_count; i++) {
        cy_group *grp = &pipe->groups[i];
        int rc;

        if (grp->flags & CY_GROUP_DISSOLVED)
            continue;
        if (!grp->members || grp->member_count == 0)
            continue;

        rc = cy_detect_group_cycle(pipe, grp, ctx);
        if (rc == CY_ERR_CYCLE_DETECTED) {
            cy_dissolve_group(pipe, grp, ctx);
        }
    }

    /* Check for implicit edges needed between groups.
       If two groups have ports that should be connected but
       no explicit edge exists, insert one. */
    for (i = 0; i < pipe->group_count; i++) {
        cy_group *grp = &pipe->groups[i];
        uint32_t pi, gj;

        if (grp->flags & CY_GROUP_DISSOLVED)
            continue;
        if (!grp->port_map)
            continue;

        for (pi = 0; pi < grp->port_map_count; pi++) {
            cy_port_map_entry *pme = &grp->port_map[pi];
            if (!pme->internal_port)
                continue;
            if (pme->internal_port->direction != CY_PORT_OUTPUT)
                continue;

            /* Look for matching input in other groups */
            for (gj = i + 1; gj < pipe->group_count; gj++) {
                cy_group *other = &pipe->groups[gj];
                uint32_t pj;

                if (other->flags & CY_GROUP_DISSOLVED)
                    continue;
                if (!other->port_map)
                    continue;

                for (pj = 0; pj < other->port_map_count; pj++) {
                    cy_port_map_entry *other_pme = &other->port_map[pj];
                    if (!other_pme->internal_port)
                        continue;
                    if (other_pme->internal_port->direction !=
                        CY_PORT_INPUT)
                        continue;

                    /* Check if names match (implicit connection) */
                    if (pme->external_name &&
                        other_pme->external_name &&
                        strcmp(pme->external_name,
                               other_pme->external_name) == 0) {
                        int exists = 0;
                        uint32_t k;

                        /* Check if edge already exists */
                        for (k = 0; k < pipe->edge_count; k++) {
                            if (pipe->edges[k].src_port_id ==
                                pme->internal_port_id &&
                                pipe->edges[k].dst_port_id ==
                                other_pme->internal_port_id) {
                                exists = 1;
                                break;
                            }
                        }

                        if (!exists) {
                            cy_add_edge(pipe, ctx,
                                        pme->internal_port_id,
                                        other_pme->internal_port_id,
                                        CY_EDGE_BUFFERED,
                                        1, 1024);
                        }
                    }
                }
            }
        }
    }

    return CY_OK;
}

/* ── Port capacity validation ───────────────────────────────────── */

int cy_validate_port_capacity(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        uint32_t output_count = 0;

        if (!node->ports)
            continue;
        if (node->flags & CY_NODE_PRUNED)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            uint32_t connected = port->connected_count;

            if (connected > port->capacity) {
                cy_add_warning(ctx,
                               "node %u port %u (%s): %u connections "
                               "exceed capacity %u",
                               i, port->id,
                               port->name ? port->name : "unnamed",
                               connected, port->capacity);
            }

            if (port->direction == CY_PORT_OUTPUT)
                output_count += connected;
        }

        /* Check total output arity */
        if (output_count > CY_MAX_ARITY) {
            cy_add_warning(ctx,
                           "node %u (%s): total output arity %u "
                           "exceeds limit %u, splitting",
                           i,
                           node->name ? node->name : "unnamed",
                           output_count, CY_MAX_ARITY);
            cy_split_overloaded_node(pipe, node, ctx);
        }
    }

    return CY_OK;
}

/* ── Latency validation ─────────────────────────────────────────── */

int cy_validate_latency(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    if (!pipe->schedule || pipe->schedule_count == 0)
        return CY_OK;

    if (!(pipe->flags & CY_FLAG_REALTIME))
        return CY_OK;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (!se->node)
            continue;
        if (se->node->flags & CY_NODE_PRUNED)
            continue;

        /* If minimum latency exceeds budget, prune */
        if (se->latency_min > pipe->latency_budget) {
            cy_add_warning(ctx,
                           "node %u (%s): min latency %u exceeds "
                           "budget %u, pruning",
                           se->node_id,
                           se->node->name ?
                               se->node->name : "unnamed",
                           se->latency_min,
                           pipe->latency_budget);
            cy_prune_latency_chain(pipe, se->node, ctx);
        }
    }

    return CY_OK;
}

/* ── Parameter validation ───────────────────────────────────────── */

int cy_validate_params(cy_pipeline *pipe, cy_context *ctx)
{
    /* Normalize parameter names (deduplicate strings) */
    return cy_normalize_param_names(pipe, ctx);
}

/* ── Schedule validation ────────────────────────────────────────── */

int cy_validate_schedule(cy_pipeline *pipe, cy_context *ctx)
{
    /* Merge redundant nodes */
    return cy_merge_redundant_nodes(pipe, ctx);
}

/* ── Reachability analysis ───────────────────────────────────────── */

static int cy_compute_reachability(cy_pipeline *pipe, cy_context *ctx)
{
    uint8_t  *reachable = NULL;
    uint32_t *queue = NULL;
    uint32_t  q_head = 0, q_tail = 0;
    uint32_t  i, j;
    uint32_t  unreachable_count = 0;

    if (pipe->node_count == 0)
        return CY_OK;

    reachable = (uint8_t *)cy_calloc(ctx, pipe->node_count, 1);
    queue = (uint32_t *)cy_calloc(ctx, pipe->node_count, sizeof(uint32_t));
    if (!reachable || !queue) {
        cy_free(reachable);
        cy_free(queue);
        return CY_ERR_NOMEM;
    }

    /* Seed with source nodes */
    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].flags & CY_NODE_SOURCE) {
            reachable[i] = 1;
            queue[q_tail++] = i;
        }
    }

    /* BFS forward through valid edges */
    while (q_head < q_tail) {
        uint32_t cur = queue[q_head++];
        cy_node *node = &pipe->nodes[cur];

        if (!node->ports)
            continue;

        for (i = 0; i < node->port_count; i++) {
            cy_port *port = &node->ports[i];
            if (port->direction != CY_PORT_OUTPUT &&
                port->direction != CY_PORT_BIDI)
                continue;

            for (j = 0; j < pipe->edge_count; j++) {
                cy_edge *e = &pipe->edges[j];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if (!e->src_port || !e->dst_port)
                    continue;
                if (e->src_port->id != port->id)
                    continue;

                uint32_t dst_nid = e->dst_port->node_id;
                if (dst_nid < pipe->node_count && !reachable[dst_nid]) {
                    reachable[dst_nid] = 1;
                    if (q_tail < pipe->node_count)
                        queue[q_tail++] = dst_nid;
                }
            }
        }
    }

    /* Count unreachable non-source nodes */
    for (i = 0; i < pipe->node_count; i++) {
        if (!reachable[i] &&
            !(pipe->nodes[i].flags & CY_NODE_PRUNED) &&
            !(pipe->nodes[i].flags & CY_NODE_MERGED)) {
            unreachable_count++;
        }
    }

    if (unreachable_count > 0) {
        cy_add_warning(ctx,
                       "%u nodes are unreachable from source nodes",
                       unreachable_count);
    }

    cy_free(reachable);
    cy_free(queue);
    return CY_OK;
}

/* ── Sink reachability analysis ─────────────────────────────────── */

static int cy_verify_sink_reachability(cy_pipeline *pipe, cy_context *ctx)
{
    uint8_t  *can_reach_sink = NULL;
    uint32_t *queue = NULL;
    uint32_t  q_head = 0, q_tail = 0;
    uint32_t  i, j;
    uint32_t  dead_end_count = 0;

    if (pipe->node_count == 0)
        return CY_OK;

    can_reach_sink = (uint8_t *)cy_calloc(ctx, pipe->node_count, 1);
    queue = (uint32_t *)cy_calloc(ctx, pipe->node_count, sizeof(uint32_t));
    if (!can_reach_sink || !queue) {
        cy_free(can_reach_sink);
        cy_free(queue);
        return CY_ERR_NOMEM;
    }

    /* Seed with sink nodes */
    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].flags & CY_NODE_SINK) {
            can_reach_sink[i] = 1;
            queue[q_tail++] = i;
        }
    }

    /* BFS backward through valid edges */
    while (q_head < q_tail) {
        uint32_t cur = queue[q_head++];

        for (j = 0; j < pipe->edge_count; j++) {
            cy_edge *e = &pipe->edges[j];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (!e->src_port || !e->dst_port)
                continue;
            if (e->dst_port->node_id != cur)
                continue;

            uint32_t src_nid = e->src_port->node_id;
            if (src_nid < pipe->node_count && !can_reach_sink[src_nid]) {
                can_reach_sink[src_nid] = 1;
                if (q_tail < pipe->node_count)
                    queue[q_tail++] = src_nid;
            }
        }
    }

    for (i = 0; i < pipe->node_count; i++) {
        if (!can_reach_sink[i] &&
            !(pipe->nodes[i].flags & CY_NODE_PRUNED) &&
            !(pipe->nodes[i].flags & CY_NODE_MERGED) &&
            !(pipe->nodes[i].flags & CY_NODE_SINK)) {
            dead_end_count++;
        }
    }

    if (dead_end_count > 0) {
        cy_add_warning(ctx,
                       "%u non-sink nodes cannot reach any sink",
                       dead_end_count);
    }

    cy_free(can_reach_sink);
    cy_free(queue);
    return CY_OK;
}

/* ── Edge consistency verification ──────────────────────────────── */

static int cy_verify_edge_consistency(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t orphan_edges = 0;
    uint32_t pruned_endpoint = 0;
    uint32_t missing_src = 0;
    uint32_t missing_dst = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];

        if (!e->src_port) {
            missing_src++;
            continue;
        }
        if (!e->dst_port) {
            missing_dst++;
            continue;
        }

        /* Check if endpoint nodes are valid */
        if (e->src_port->node_id >= pipe->node_count) {
            orphan_edges++;
            continue;
        }
        if (e->dst_port->node_id >= pipe->node_count) {
            orphan_edges++;
            continue;
        }

        /* Check if endpoint nodes are pruned */
        cy_node *src_node = &pipe->nodes[e->src_port->node_id];
        cy_node *dst_node = &pipe->nodes[e->dst_port->node_id];

        if ((src_node->flags & CY_NODE_PRUNED) ||
            (dst_node->flags & CY_NODE_PRUNED)) {
            if (e->flags & CY_EDGE_VALID) {
                pruned_endpoint++;
                e->flags &= ~CY_EDGE_VALID;
            }
        }
    }

    if (orphan_edges > 0)
        cy_add_warning(ctx, "%u edges reference invalid nodes",
                       orphan_edges);
    if (missing_src > 0)
        cy_add_warning(ctx, "%u edges have no source port", missing_src);
    if (missing_dst > 0)
        cy_add_warning(ctx, "%u edges have no dest port", missing_dst);
    if (pruned_endpoint > 0)
        cy_add_warning(ctx, "%u edges touch pruned nodes (invalidated)",
                       pruned_endpoint);

    return CY_OK;
}

/* ── Port capacity analysis ─────────────────────────────────────── */

static int cy_analyze_port_utilization(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t overloaded = 0;
    uint32_t underused = 0;
    uint32_t total_capacity = 0;
    uint32_t total_connections = 0;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports || (node->flags & CY_NODE_PRUNED))
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            uint32_t valid_connections = 0;
            uint32_t k;

            /* Count valid connections */
            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *e = &pipe->edges[k];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if ((e->src_port && e->src_port->id == port->id) ||
                    (e->dst_port && e->dst_port->id == port->id)) {
                    valid_connections++;
                }
            }

            total_capacity += port->capacity;
            total_connections += valid_connections;

            if (valid_connections > port->capacity)
                overloaded++;
            else if (valid_connections == 0 &&
                     (port->flags & CY_PORT_REQUIRED))
                underused++;
        }
    }

    if (overloaded > 0)
        cy_add_warning(ctx, "%u ports exceed their declared capacity",
                       overloaded);
    if (underused > 0)
        cy_add_warning(ctx, "%u required ports have no connections",
                       underused);

    (void)total_capacity;
    (void)total_connections;

    return CY_OK;
}

/* ── Buffer size optimization check ─────────────────────────────── */

static int cy_validate_buffer_optimization(cy_pipeline *pipe,
                                           cy_context *ctx)
{
    uint32_t i;
    uint32_t oversized = 0;
    uint32_t undersized = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!(e->flags & CY_EDGE_BUFFERED))
            continue;

        if (!e->src_type || !e->dst_type)
            continue;

        uint32_t elem_size = e->src_type->size;
        if (e->dst_type->size > elem_size)
            elem_size = e->dst_type->size;

        if (elem_size == 0)
            continue;

        /* Check buffer holds at least 2 elements */
        if (e->buffer_size < elem_size * 2) {
            undersized++;
        }

        /* Check buffer isn't excessively large */
        if (e->buffer_size > elem_size * 1024 * 1024) {
            oversized++;
        }

        /* Check buffer size is aligned to element size */
        if (e->buffer_size % elem_size != 0) {
            cy_add_warning(ctx,
                           "edge %u: buffer size %u not aligned to "
                           "element size %u",
                           i, e->buffer_size, elem_size);
        }
    }

    if (undersized > 0)
        cy_add_warning(ctx, "%u buffered edges have insufficient buffer",
                       undersized);
    if (oversized > 0)
        cy_add_warning(ctx, "%u buffered edges have excessive buffer",
                       oversized);

    return CY_OK;
}

/* ── Schedule completeness check ────────────────────────────────── */

static int cy_validate_schedule_completeness(cy_pipeline *pipe,
                                             cy_context *ctx)
{
    uint32_t i;
    uint32_t scheduled = 0;
    uint32_t unscheduled = 0;
    uint8_t *has_schedule = NULL;

    if (!pipe->schedule || pipe->schedule_count == 0)
        return CY_OK;

    has_schedule = (uint8_t *)cy_calloc(ctx, pipe->node_count, 1);
    if (!has_schedule)
        return CY_ERR_NOMEM;

    for (i = 0; i < pipe->schedule_count; i++) {
        uint32_t nid = pipe->schedule[i].node_id;
        if (nid < pipe->node_count)
            has_schedule[nid] = 1;
    }

    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].flags & CY_NODE_PRUNED)
            continue;
        if (pipe->nodes[i].flags & CY_NODE_MERGED)
            continue;

        if (has_schedule[i])
            scheduled++;
        else
            unscheduled++;
    }

    if (unscheduled > 0 && scheduled > 0) {
        cy_add_warning(ctx,
                       "%u active nodes have no schedule entry "
                       "(%u are scheduled)",
                       unscheduled, scheduled);
    }

    cy_free(has_schedule);
    return CY_OK;
}

/* ── Type hierarchy depth check ─────────────────────────────────── */

static int cy_validate_type_depth(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t max_depth = 0;

    for (i = 0; i < pipe->type_count; i++) {
        cy_type_desc *td = &pipe->types[i];
        uint32_t depth = 0;
        cy_type_desc *cur = td;

        while (cur->base_type && depth < 100) {
            depth++;
            cur = cur->base_type;
        }

        if (depth > max_depth)
            max_depth = depth;

        if (depth > 16) {
            cy_add_warning(ctx,
                           "type %u (%s): hierarchy depth %u exceeds 16",
                           i, td->name ? td->name : "unnamed", depth);
        }
    }

    return CY_OK;
}

/* ── Group isolation validation ─────────────────────────────────── */

static int cy_validate_group_isolation(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t gi;

    for (gi = 0; gi < pipe->group_count; gi++) {
        cy_group *grp = &pipe->groups[gi];
        uint32_t mi, ei;
        uint32_t cross_boundary = 0;

        if (!(grp->flags & CY_GROUP_ISOLATED))
            continue;
        if (grp->flags & CY_GROUP_DISSOLVED)
            continue;
        if (!grp->members)
            continue;

        /* Count edges that cross the group boundary */
        for (mi = 0; mi < grp->member_count; mi++) {
            if (!grp->members[mi])
                continue;
            uint32_t member_id = grp->members[mi]->id;

            for (ei = 0; ei < pipe->edge_count; ei++) {
                cy_edge *e = &pipe->edges[ei];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if (!e->src_port || !e->dst_port)
                    continue;

                int src_in_group = (e->src_port->node_id == member_id);
                int dst_in_group = (e->dst_port->node_id == member_id);

                if (src_in_group && !dst_in_group) {
                    /* Check if dest is in another member */
                    uint32_t mj;
                    int dst_is_member = 0;
                    for (mj = 0; mj < grp->member_count; mj++) {
                        if (grp->members[mj] &&
                            grp->members[mj]->id ==
                            e->dst_port->node_id) {
                            dst_is_member = 1;
                            break;
                        }
                    }
                    if (!dst_is_member)
                        cross_boundary++;
                }
                if (dst_in_group && !src_in_group) {
                    uint32_t mj;
                    int src_is_member = 0;
                    for (mj = 0; mj < grp->member_count; mj++) {
                        if (grp->members[mj] &&
                            grp->members[mj]->id ==
                            e->src_port->node_id) {
                            src_is_member = 1;
                            break;
                        }
                    }
                    if (!src_is_member)
                        cross_boundary++;
                }
            }
        }

        if (cross_boundary > 0) {
            cy_add_warning(ctx,
                           "group %u (%s): ISOLATED flag but %u edges "
                           "cross group boundary",
                           gi,
                           grp->name ? grp->name : "unnamed",
                           cross_boundary);
        }
    }

    return CY_OK;
}

/* ── Parameter coverage validation ──────────────────────────────── */

static int cy_validate_param_coverage(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t nodes_with_params = 0;
    uint32_t nodes_without_params = 0;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (node->flags & CY_NODE_PRUNED)
            continue;
        if (node->flags & CY_NODE_MERGED)
            continue;

        if (node->param_count > 0)
            nodes_with_params++;
        else
            nodes_without_params++;

        /* Validate param range references */
        if (node->param_count > 0) {
            uint32_t end;
            if (cy_safe_add(node->param_start, node->param_count,
                            &end) == 0) {
                if (end > pipe->param_count) {
                    cy_add_warning(ctx,
                                   "node %u: param range [%u,%u) exceeds "
                                   "param count %u",
                                   i, node->param_start, end,
                                   pipe->param_count);
                }
            }
        }
    }

    (void)nodes_with_params;
    (void)nodes_without_params;

    return CY_OK;
}

/* ── Master validation orchestrator ─────────────────────────────── */

int cy_validate_pipeline(cy_pipeline *pipe, cy_context *ctx)
{
    int rc;

    /* Step 1: Fold equivalent types.
       This frees duplicate type descriptors and rewrites port type
       pointers — but not edge type cache pointers (Bug 5). */
    rc = cy_fold_equivalent_types(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 2: Validate type compatibility and mark invalid edges */
    rc = cy_validate_type_compat(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 3: Prune disconnected nodes (cascade from invalid edges).
       This frees pruned nodes' port arrays — port cache and route
       table entries are NOT invalidated (Bug 1). */
    rc = cy_prune_disconnected_nodes(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 4: Validate groups and dissolve those with cycles.
       Dissolving frees member node port arrays — route table entries
       are NOT cleared (Bug 2). */
    rc = cy_validate_groups(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 5: Validate port capacity and split overloaded nodes.
       Splitting frees old port array and creates new allocations —
       group port maps are NOT updated (Bug 8). */
    rc = cy_validate_port_capacity(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 6: Validate latency and prune over-budget chains.
       Pruning clears route source entries but NOT constraint
       sink references in schedule entries (Bug 7). */
    rc = cy_validate_latency(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 7: Validate and normalize parameters.
       Name normalization frees duplicate string entries — port cache
       hash keys are NOT rehashed (Bug 4). */
    rc = cy_validate_params(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 8: Validate and merge redundant nodes.
       Merging redirects edge endpoints but NOT schedule entry node
       pointers (Bug 3). */
    rc = cy_validate_schedule(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Step 9: Extended validation passes (advisory, non-fatal) */
    cy_compute_reachability(pipe, ctx);
    cy_verify_sink_reachability(pipe, ctx);
    cy_verify_edge_consistency(pipe, ctx);
    cy_analyze_port_utilization(pipe, ctx);
    cy_validate_buffer_optimization(pipe, ctx);
    cy_validate_schedule_completeness(pipe, ctx);
    cy_validate_type_depth(pipe, ctx);
    cy_validate_group_isolation(pipe, ctx);
    cy_validate_param_coverage(pipe, ctx);

    return CY_OK;
}
