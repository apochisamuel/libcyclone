/*
 * resolve.c — Phase 13: Final cross-reference resolution
 *
 * Resolves late-bound parameter bindings, type constraints,
 * schedule ordering, group interfaces, edge weights, latency
 * bounds, and cross-group references. This phase reads cached
 * pointers that may have been invalidated during validation.
 *
 * Reference: CYC Format Specification §6 - Resolution
 */

#include "internal.h"

/* ── Parameter binding resolution ───────────────────────────────── */
/* Iterates the port cache to find ports whose parameters need      */
/* binding. Reads port pointers from the cache — if a port was      */
/* freed during pruning (Bug 1) or its hash key string was freed    */
/* during param normalization (Bug 4), this dereferences freed      */
/* memory.                                                          */

int cy_resolve_param_bindings(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t bound = 0;
    uint32_t unbound = 0;

    if (!pipe->port_cache || pipe->param_count == 0)
        return CY_OK;

    /* For each parameter, look up its target port in the cache */
    for (i = 0; i < pipe->param_count; i++) {
        cy_param *p = &pipe->params[i];
        cy_node  *node;
        int       param_bound = 0;

        if (p->node_id >= pipe->node_count)
            continue;

        node = &pipe->nodes[p->node_id];
        if (node->flags & CY_NODE_PRUNED)
            continue;

        /* Try to find a port on this node matching the param name
           by looking it up in the port cache */
        if (p->name) {
            cy_port *cached_port = cy_port_cache_lookup(
                pipe->port_cache, node->id, p->name);

            if (cached_port) {
                /* Read the port's type to validate param compatibility */
                if (cached_port->type) {
                    uint8_t tk = cached_port->type->kind;

                    /* Check param kind matches port type */
                    if (p->kind == CY_PARAM_INT &&
                        tk != CY_TYPE_INT && tk != CY_TYPE_ENUM) {
                        cy_add_warning(ctx,
                                       "param %u (%s): int param bound "
                                       "to non-int port type %u",
                                       i, p->name, tk);
                    }
                    if (p->kind == CY_PARAM_FLOAT &&
                        tk != CY_TYPE_FLOAT && tk != CY_TYPE_DOUBLE) {
                        cy_add_warning(ctx,
                                       "param %u (%s): float param bound "
                                       "to non-float port type %u",
                                       i, p->name, tk);
                    }
                }

                param_bound = 1;
                bound++;
            }
        }

        /* Fallback: direct port array scan */
        if (!param_bound && node->ports) {
            for (j = 0; j < node->port_count; j++) {
                if (node->ports[j].name && p->name &&
                    strcmp(node->ports[j].name, p->name) == 0) {
                    param_bound = 1;
                    bound++;
                    break;
                }
            }
        }

        if (!param_bound)
            unbound++;
    }

    if (unbound > 0) {
        cy_add_warning(ctx, "%u parameters could not be bound to ports",
                       unbound);
    }

    return CY_OK;
}

/* ── Type constraint resolution ─────────────────────────────────── */
/* Reads edge type cache pointers (src_type, dst_type) that were    */
/* set during edge parsing. If type folding freed a duplicate type  */
/* descriptor but didn't update edge type refs (Bug 5), this reads  */
/* a freed type descriptor.                                         */

int cy_resolve_type_constraints(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t constrained = 0;
    uint32_t violations = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];

        if (!(e->flags & CY_EDGE_VALID))
            continue;

        if (!e->src_type || !e->dst_type)
            continue;

        /* Read cached type size for buffer size calculation */
        uint32_t src_size = e->src_type->size;
        uint32_t dst_size = e->dst_type->size;

        if (e->flags & CY_EDGE_BUFFERED) {
            /* Buffer must hold at least one element of each type */
            uint32_t min_buffer = src_size > dst_size
                                  ? src_size : dst_size;

            if (e->buffer_size > 0 && e->buffer_size < min_buffer) {
                cy_add_warning(ctx,
                               "edge %u: buffer size %u < minimum %u "
                               "for types of size %u/%u",
                               i, e->buffer_size, min_buffer,
                               src_size, dst_size);
                violations++;
            }
        }

        /* Verify type compatibility using cached pointers */
        if (!cy_types_compatible(e->src_type, e->dst_type)) {
            violations++;
        }

        /* Check type name agreement */
        if (e->src_type->name && e->dst_type->name) {
            if (e->src_type->kind == CY_TYPE_STRUCT &&
                e->dst_type->kind == CY_TYPE_STRUCT) {
                if (strcmp(e->src_type->name,
                           e->dst_type->name) != 0) {
                    cy_add_warning(ctx,
                                   "edge %u: struct type names differ "
                                   "('%s' vs '%s')",
                                   i, e->src_type->name,
                                   e->dst_type->name);
                }
            }
        }

        constrained++;
    }

    if (violations > 0) {
        cy_add_warning(ctx,
                       "%u type constraint violations in %u edges",
                       violations, constrained);
    }

    return CY_OK;
}

/* ── Schedule order resolution ──────────────────────────────────── */
/* Reads schedule entry node pointers. If a redundant node was      */
/* merged and freed (Bug 3), the schedule entry's node pointer      */
/* is dangling.                                                     */

int cy_resolve_schedule_order(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t  i, j;
    uint32_t *order_map = NULL;
    uint32_t  resolved = 0;

    if (!pipe->schedule || pipe->schedule_count == 0)
        return CY_OK;

    /* Build topological order from schedule entries */
    order_map = (uint32_t *)cy_calloc(ctx, pipe->schedule_count,
                                      sizeof(uint32_t));
    if (!order_map)
        return CY_ERR_NOMEM;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (!se->node)
            continue;

        /* Read the node's name for ordering context */
        const char *node_name = se->node->name;
        uint32_t node_flags = se->node->flags;

        /* Skip pruned or merged nodes */
        if (node_flags & CY_NODE_PRUNED)
            continue;

        /* Analyze port connectivity for schedule ordering.
           Count output ports to determine parallelism factor. */
        if (se->node->ports) {
            uint32_t out_count = 0;
            uint32_t pi;
            for (pi = 0; pi < se->node->port_count; pi++) {
                if (se->node->ports[pi].direction == CY_PORT_OUTPUT)
                    out_count++;
            }
            if (out_count == 0 && !(node_flags & CY_NODE_SINK)) {
                cy_add_warning(ctx,
                               "schedule entry %u: node %u has no "
                               "output ports",
                               i, se->node_id);
            }
        }

        /* Check for dependency ordering violations */
        for (j = 0; j < i; j++) {
            cy_sched_entry *prev = &pipe->schedule[j];
            if (!prev->node)
                continue;

            /* If there's an edge from prev->node to se->node,
               prev must have lower order */
            if (prev->order > se->order) {
                uint32_t k;
                for (k = 0; k < pipe->edge_count; k++) {
                    cy_edge *e = &pipe->edges[k];
                    if (!(e->flags & CY_EDGE_VALID))
                        continue;
                    if (!e->src_port || !e->dst_port)
                        continue;
                    if (e->src_port->node_id == prev->node_id &&
                        e->dst_port->node_id == se->node_id) {
                        cy_add_warning(ctx,
                                       "schedule: dependency from "
                                       "node %u to node %u violates "
                                       "order (%u > %u)",
                                       prev->node_id, se->node_id,
                                       prev->order, se->order);
                    }
                }
            }
        }

        order_map[i] = se->order;
        resolved++;

        (void)node_name;
    }

    cy_free(order_map);

    return CY_OK;
}

/* ── Group interface resolution ─────────────────────────────────── */
/* Reads group port map entries, which contain cy_port pointers.    */
/* If a node was split and its old port array freed (Bug 8), the    */
/* port map's internal_port pointer is dangling.                    */

int cy_resolve_group_interfaces(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t gi, pi;
    uint32_t resolved = 0;
    uint32_t broken = 0;

    for (gi = 0; gi < pipe->group_count; gi++) {
        cy_group *grp = &pipe->groups[gi];

        if (grp->flags & CY_GROUP_DISSOLVED)
            continue;
        if (!grp->port_map)
            continue;

        for (pi = 0; pi < grp->port_map_count; pi++) {
            cy_port_map_entry *pme = &grp->port_map[pi];

            if (!pme->internal_port) {
                broken++;
                continue;
            }

            /* Dereference the cached port pointer to read its
               type for interface validation */
            if (pme->internal_port->type) {
                uint32_t type_size = pme->internal_port->type->size;
                uint8_t  type_kind = pme->internal_port->type->kind;

                /* Validate interface type is suitable for
                   cross-group transport */
                if (type_kind == CY_TYPE_STRUCT && type_size > 65536) {
                    cy_add_warning(ctx,
                                   "group %u port_map %u: large struct "
                                   "type (%u bytes) on group boundary",
                                   gi, pi, type_size);
                }
            }

            /* Verify port direction is appropriate for its
               position on the group boundary */
            uint8_t dir = pme->internal_port->direction;
            const char *port_name = pme->internal_port->name;

            if (dir == CY_PORT_BIDI) {
                cy_add_warning(ctx,
                               "group %u port_map %u (%s): "
                               "bidirectional port on group boundary",
                               gi, pi,
                               port_name ? port_name : "unnamed");
            }

            resolved++;

            (void)port_name;
        }
    }

    if (broken > 0) {
        cy_add_warning(ctx,
                       "%u group port map entries have null ports",
                       broken);
    }

    return CY_OK;
}

/* ── Edge weight resolution ─────────────────────────────────────── */
/* Reads port back-pointers (connected_edges array). If cy_add_edge */
/* reallocated the edge array (Bug 6), the port back-pointers       */
/* reference the old (freed) array.                                 */

int cy_resolve_edge_weights(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint64_t total_weight = 0;
    uint32_t weighted_ports = 0;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;
        if (node->flags & CY_NODE_PRUNED)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            uint32_t k;
            uint64_t port_weight = 0;

            if (!port->connected_edges)
                continue;

            /* Sum weights of all edges connected to this port */
            for (k = 0; k < port->connected_count; k++) {
                cy_edge *e = port->connected_edges[k];
                if (!e)
                    continue;
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                port_weight += e->weight;
            }

            if (port_weight > 0) {
                total_weight += port_weight;
                weighted_ports++;
            }
        }
    }

    if (weighted_ports > 0 && total_weight == 0) {
        cy_add_warning(ctx,
                       "all %u weighted ports have zero total weight",
                       weighted_ports);
    }

    return CY_OK;
}

/* ── Latency bound resolution ───────────────────────────────────── */
/* Reads schedule constraint_sink node pointers. If latency pruning */
/* freed a node but didn't clear constraint sink refs (Bug 7), this */
/* dereferences a freed node.                                       */

int cy_resolve_latency_bounds(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t constrained = 0;
    uint32_t violations = 0;

    if (!pipe->schedule || pipe->schedule_count == 0)
        return CY_OK;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (!se->constraint_sink)
            continue;
        if (!se->node)
            continue;

        /* Read the constraint sink's schedule data.
           The sink must complete within the latency window
           of the source entry. */
        const char *sink_name = se->constraint_sink->name;
        uint32_t sink_id = se->constraint_sink->id;

        /* Verify constraint sink has sufficient output capacity.
           Sum the port capacities to estimate throughput budget. */
        uint32_t sink_output_cap = 0;
        if (se->constraint_sink->ports &&
            se->constraint_sink->port_count > 0) {
            uint32_t pi;
            for (pi = 0; pi < se->constraint_sink->port_count; pi++) {
                if (se->constraint_sink->ports[pi].direction ==
                    CY_PORT_OUTPUT) {
                    sink_output_cap +=
                        se->constraint_sink->ports[pi].capacity;
                }
            }
        }

        uint32_t sink_flags = se->constraint_sink->flags;

        /* Skip if either end is pruned */
        if (sink_flags & CY_NODE_PRUNED) {
            cy_add_warning(ctx,
                           "schedule %u: constraint sink node %u (%s) "
                           "was pruned",
                           i, sink_id,
                           sink_name ? sink_name : "unnamed");
            continue;
        }

        if (se->node->flags & CY_NODE_PRUNED)
            continue;

        if (sink_output_cap == 0 &&
            !(sink_flags & CY_NODE_SINK)) {
            cy_add_warning(ctx,
                           "schedule %u: constraint sink %u has "
                           "no output capacity",
                           i, sink_id);
        }

        /* Check that sink's latency fits within constraint window */
        uint32_t j;
        for (j = 0; j < pipe->schedule_count; j++) {
            if (pipe->schedule[j].node_id == sink_id) {
                uint32_t sink_lat = pipe->schedule[j].latency_max;
                uint32_t budget = se->latency_max - se->latency_min;

                if (sink_lat > budget) {
                    cy_add_warning(ctx,
                                   "schedule %u: sink node %u latency "
                                   "%u exceeds constraint budget %u",
                                   i, sink_id, sink_lat, budget);
                    violations++;
                }
                break;
            }
        }

        constrained++;

        (void)sink_name;
    }

    if (violations > 0) {
        cy_add_warning(ctx,
                       "%u latency constraint violations in "
                       "%u constraints",
                       violations, constrained);
    }

    return CY_OK;
}

/* ── Cross-group reference resolution ───────────────────────────── */
/* Walks the route table for edges crossing group boundaries.       */
/* If a group was dissolved and its member ports freed (Bug 2),      */
/* route table entries point to freed port memory.                   */

int cy_resolve_cross_group_refs(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t cross_group = 0;
    uint32_t intra_group = 0;

    if (!pipe->route_table || pipe->group_count == 0)
        return CY_OK;

    /* Iterate all route table entries looking for cross-group edges */
    for (i = 0; i < pipe->route_table->entry_count; i++) {
        cy_route_entry *re = &pipe->route_table->entries[i];

        if (!re->src_port || !re->dst_port)
            continue;

        /* Read the port's node_id to determine group membership */
        uint32_t src_node_id = re->src_port->node_id;
        uint32_t dst_node_id = re->dst_port->node_id;

        /* Find group membership for each node */
        uint16_t src_group = 0;
        uint16_t dst_group = 0;

        if (src_node_id < pipe->node_count)
            src_group = pipe->nodes[src_node_id].group_id;
        if (dst_node_id < pipe->node_count)
            dst_group = pipe->nodes[dst_node_id].group_id;

        if (src_group != dst_group &&
            src_group != 0 && dst_group != 0) {
            cross_group++;

            /* Validate cross-group edge has appropriate buffering */
            if (re->edge && !(re->edge->flags & CY_EDGE_BUFFERED)) {
                cy_add_warning(ctx,
                               "edge %u: cross-group edge from "
                               "group %u to group %u without buffering",
                               re->edge->id,
                               src_group - 1, dst_group - 1);
            }

            /* Read port type information for interface checking */
            if (re->src_port->type && re->dst_port->type) {
                if (!cy_types_compatible(re->src_port->type,
                                         re->dst_port->type)) {
                    cy_add_warning(ctx,
                                   "cross-group edge type mismatch "
                                   "between groups %u and %u",
                                   src_group - 1, dst_group - 1);
                }
            }
        } else {
            intra_group++;
        }
    }

    if (cross_group > 0) {
        cy_add_warning(ctx, "%u cross-group edges, %u intra-group edges",
                       cross_group, intra_group);
    }

    return CY_OK;
}

/* ── Master resolution orchestrator ─────────────────────────────── */

int cy_resolve_references(cy_pipeline *pipe, cy_context *ctx)
{
    int rc;

    /* Resolve parameter bindings (may hit Bug 1, Bug 4) */
    rc = cy_resolve_param_bindings(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve type constraints (may hit Bug 5) */
    rc = cy_resolve_type_constraints(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve schedule order (may hit Bug 3) */
    rc = cy_resolve_schedule_order(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve group interfaces (may hit Bug 8) */
    rc = cy_resolve_group_interfaces(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve edge weights (may hit Bug 6) */
    rc = cy_resolve_edge_weights(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve latency bounds (may hit Bug 7) */
    rc = cy_resolve_latency_bounds(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve cross-group references (may hit Bug 2) */
    rc = cy_resolve_cross_group_refs(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    /* Additional resolution passes */
    rc = cy_resolve_port_capacities(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_resolve_edge_buffer_layout(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    rc = cy_resolve_schedule_barriers(pipe, ctx);
    if (rc != CY_OK)
        return rc;

    return CY_OK;
}

/* ── Port capacity resolution ───────────────────────────────────── */

int cy_resolve_port_capacities(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t adjusted = 0;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports || (node->flags & CY_NODE_PRUNED))
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            uint32_t actual_connections = 0;
            uint32_t k;

            /* Count actual valid connections */
            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *e = &pipe->edges[k];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;

                if (port->direction == CY_PORT_OUTPUT ||
                    port->direction == CY_PORT_BIDI) {
                    if (e->src_port && e->src_port->id == port->id)
                        actual_connections++;
                }
                if (port->direction == CY_PORT_INPUT ||
                    port->direction == CY_PORT_BIDI) {
                    if (e->dst_port && e->dst_port->id == port->id)
                        actual_connections++;
                }
            }

            /* If actual connections exceed declared capacity,
               adjust capacity to match reality (with warning) */
            if (actual_connections > port->capacity) {
                cy_add_warning(ctx,
                               "port %u (%s) on node %u: adjusting "
                               "capacity %u -> %u to match connections",
                               port->id,
                               port->name ? port->name : "unnamed",
                               node->id,
                               port->capacity, actual_connections);
                port->capacity = (uint16_t)(actual_connections > 0xFFFF
                                            ? 0xFFFF
                                            : actual_connections);
                adjusted++;
            }

            /* Validate monitor ports have no outgoing edges */
            if (port->flags & CY_PORT_MONITOR) {
                for (k = 0; k < pipe->edge_count; k++) {
                    cy_edge *e = &pipe->edges[k];
                    if (e->src_port && e->src_port->id == port->id &&
                        (e->flags & CY_EDGE_VALID)) {
                        cy_add_warning(ctx,
                                       "port %u (%s): monitor port "
                                       "has outgoing edge %u",
                                       port->id,
                                       port->name ?
                                           port->name : "unnamed",
                                       e->id);
                    }
                }
            }
        }
    }

    if (adjusted > 0) {
        cy_add_warning(ctx, "adjusted %u port capacities", adjusted);
    }

    return CY_OK;
}

/* ── Edge buffer layout resolution ──────────────────────────────── */

int cy_resolve_edge_buffer_layout(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint64_t total_buffer = 0;
    uint32_t buffered_count = 0;
    uint32_t type_mismatch_buffers = 0;

    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!(e->flags & CY_EDGE_BUFFERED))
            continue;

        buffered_count++;
        total_buffer += e->buffer_size;

        /* Resolve buffer alignment from type information */
        if (e->src_type && e->dst_type) {
            uint32_t src_align = e->src_type->size;
            uint32_t dst_align = e->dst_type->size;
            uint32_t max_align = src_align > dst_align
                                 ? src_align : dst_align;

            if (max_align > 0 && e->buffer_size > 0) {
                uint32_t aligned_size = e->buffer_size;
                if (aligned_size % max_align != 0) {
                    aligned_size = ((aligned_size / max_align) + 1) *
                                   max_align;
                    if (aligned_size != e->buffer_size) {
                        cy_add_warning(ctx,
                                       "edge %u: buffer %u not "
                                       "aligned to element size %u, "
                                       "would need %u",
                                       i, e->buffer_size,
                                       max_align, aligned_size);
                    }
                }
            }

            /* Check type sizes match for direct transport */
            if (e->src_type->size != e->dst_type->size &&
                e->src_type->kind == e->dst_type->kind) {
                type_mismatch_buffers++;
            }
        }

        /* Validate feedback edge buffer is large enough for
           at least one full cycle's worth of data */
        if (e->flags & CY_EDGE_FEEDBACK) {
            if (e->src_type && e->buffer_size < e->src_type->size * 2) {
                cy_add_warning(ctx,
                               "edge %u: feedback buffer %u too small "
                               "for type size %u (need 2x)",
                               i, e->buffer_size, e->src_type->size);
            }
        }
    }

    if (total_buffer > 0) {
        cy_add_warning(ctx,
                       "total buffer allocation: %llu bytes across "
                       "%u edges",
                       (unsigned long long)total_buffer,
                       buffered_count);
    }

    return CY_OK;
}

/* ── Schedule barrier resolution ────────────────────────────────── */

int cy_resolve_schedule_barriers(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t barriers = 0;
    uint32_t barrier_violations = 0;

    if (!pipe->schedule || pipe->schedule_count == 0)
        return CY_OK;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];
        if (!(se->flags & CY_SCHED_BARRIER))
            continue;
        if (!se->node)
            continue;

        barriers++;

        /* All nodes scheduled before this barrier must complete
           before any node after the barrier can start */
        for (j = 0; j < pipe->schedule_count; j++) {
            cy_sched_entry *other = &pipe->schedule[j];
            if (!other->node)
                continue;
            if (other->order <= se->order)
                continue;

            /* Check there's no edge from a post-barrier node
               back to a pre-barrier node (ignoring feedback) */
            uint32_t k;
            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *e = &pipe->edges[k];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if (e->flags & CY_EDGE_FEEDBACK)
                    continue;
                if (!e->src_port || !e->dst_port)
                    continue;

                if (e->src_port->node_id == other->node_id) {
                    /* Find the dst node's schedule entry */
                    uint32_t m;
                    for (m = 0; m < pipe->schedule_count; m++) {
                        if (pipe->schedule[m].node_id ==
                            e->dst_port->node_id) {
                            if (pipe->schedule[m].order < se->order) {
                                barrier_violations++;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    if (barrier_violations > 0) {
        cy_add_warning(ctx,
                       "%u barrier violations across %u barriers",
                       barrier_violations, barriers);
    }

    return CY_OK;
}
