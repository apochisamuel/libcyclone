/*
 * routing.c — Phase 11: Route table construction
 *
 * The route table indexes edges by their source and destination
 * ports, enabling fast traversal of the dataflow graph. Each
 * route entry stores pointers to the source port, destination port,
 * and edge. Entries are organized in linked lists per port ID.
 *
 * Reference: CYC Format Specification §4.2 - Edge Routing
 */

#include "internal.h"

/* ── Route table lifecycle ──────────────────────────────────────── */

cy_route_table *cy_route_table_create(cy_context *ctx, uint32_t port_count,
                                      uint32_t edge_count)
{
    cy_route_table *rt;

    rt = (cy_route_table *)cy_alloc(ctx, sizeof(cy_route_table));
    if (!rt)
        return NULL;

    memset(rt, 0, sizeof(cy_route_table));

    if (port_count == 0)
        port_count = 1;

    rt->by_src = (cy_route_entry **)cy_calloc(ctx, port_count,
                                               sizeof(cy_route_entry *));
    if (!rt->by_src) {
        cy_free(rt);
        return NULL;
    }

    rt->by_dst = (cy_route_entry **)cy_calloc(ctx, port_count,
                                               sizeof(cy_route_entry *));
    if (!rt->by_dst) {
        cy_free(rt->by_src);
        cy_free(rt);
        return NULL;
    }

    rt->index_size = port_count;

    if (edge_count == 0)
        edge_count = 1;

    rt->entries = (cy_route_entry *)cy_calloc(ctx, edge_count,
                                              sizeof(cy_route_entry));
    if (!rt->entries) {
        cy_free(rt->by_dst);
        cy_free(rt->by_src);
        cy_free(rt);
        return NULL;
    }

    rt->entry_count = 0;
    return rt;
}

void cy_route_table_destroy(cy_route_table *rt)
{
    if (!rt)
        return;
    cy_free(rt->by_src);
    cy_free(rt->by_dst);
    cy_free(rt->entries);
    cy_free(rt);
}

/* ── Add route entry ────────────────────────────────────────────── */

int cy_route_table_add(cy_route_table *rt, cy_port *src,
                       cy_port *dst, cy_edge *edge)
{
    cy_route_entry *entry;

    if (!rt || !src || !dst || !edge)
        return -1;

    entry = &rt->entries[rt->entry_count];
    entry->src_port = src;
    entry->dst_port = dst;
    entry->edge = edge;

    /* Link into source port chain */
    if (src->id < rt->index_size) {
        entry->next_by_src = rt->by_src[src->id];
        rt->by_src[src->id] = entry;
    } else {
        entry->next_by_src = NULL;
    }

    /* Link into destination port chain */
    if (dst->id < rt->index_size) {
        entry->next_by_dst = rt->by_dst[dst->id];
        rt->by_dst[dst->id] = entry;
    } else {
        entry->next_by_dst = NULL;
    }

    rt->entry_count++;
    return 0;
}

/* ── Route lookups ──────────────────────────────────────────────── */

cy_route_entry *cy_route_by_src(const cy_route_table *rt, uint32_t port_id)
{
    if (!rt || port_id >= rt->index_size)
        return NULL;
    return rt->by_src[port_id];
}

cy_route_entry *cy_route_by_dst(const cy_route_table *rt, uint32_t port_id)
{
    if (!rt || port_id >= rt->index_size)
        return NULL;
    return rt->by_dst[port_id];
}

/* ── Route counting ─────────────────────────────────────────────── */

static uint32_t cy_count_routes_from(const cy_route_table *rt,
                                     uint32_t port_id)
{
    uint32_t count = 0;
    const cy_route_entry *e;

    if (!rt || port_id >= rt->index_size)
        return 0;

    e = rt->by_src[port_id];
    while (e) {
        count++;
        e = e->next_by_src;
    }
    return count;
}

static uint32_t cy_count_routes_to(const cy_route_table *rt,
                                   uint32_t port_id)
{
    uint32_t count = 0;
    const cy_route_entry *e;

    if (!rt || port_id >= rt->index_size)
        return 0;

    e = rt->by_dst[port_id];
    while (e) {
        count++;
        e = e->next_by_dst;
    }
    return count;
}

/* ── Fan-out analysis ───────────────────────────────────────────── */

static void cy_analyze_fanout(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t max_fanout = 0;
    uint32_t max_fanin = 0;
    uint32_t total_fanout = 0;
    uint32_t port_with_max_fanout = 0;
    uint32_t port_with_max_fanin = 0;

    if (!pipe->route_table)
        return;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            uint32_t fo = cy_count_routes_from(pipe->route_table,
                                               port->id);
            uint32_t fi = cy_count_routes_to(pipe->route_table,
                                             port->id);

            total_fanout += fo;

            if (fo > max_fanout) {
                max_fanout = fo;
                port_with_max_fanout = port->id;
            }
            if (fi > max_fanin) {
                max_fanin = fi;
                port_with_max_fanin = port->id;
            }
        }
    }

    if (max_fanout > 16) {
        cy_add_warning(ctx,
                       "high fan-out: port %u has %u outgoing routes",
                       port_with_max_fanout, max_fanout);
    }

    if (max_fanin > 16) {
        cy_add_warning(ctx,
                       "high fan-in: port %u has %u incoming routes",
                       port_with_max_fanin, max_fanin);
    }

    (void)total_fanout;
}

/* ── Connectivity validation ────────────────────────────────────── */

static void cy_validate_connectivity(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t disconnected_inputs = 0;
    uint32_t disconnected_outputs = 0;

    if (!pipe->route_table)
        return;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;
        if (node->flags & CY_NODE_PRUNED)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];

            if (port->direction == CY_PORT_INPUT &&
                !(port->flags & CY_PORT_MONITOR)) {
                uint32_t routes = cy_count_routes_to(
                    pipe->route_table, port->id);
                if (routes == 0 && !(node->flags & CY_NODE_SOURCE))
                    disconnected_inputs++;
            }

            if (port->direction == CY_PORT_OUTPUT) {
                uint32_t routes = cy_count_routes_from(
                    pipe->route_table, port->id);
                if (routes == 0 && !(node->flags & CY_NODE_SINK))
                    disconnected_outputs++;
            }
        }
    }

    if (disconnected_inputs > 0) {
        cy_add_warning(ctx,
                       "%u non-source input ports have no incoming routes",
                       disconnected_inputs);
    }

    if (disconnected_outputs > 0) {
        cy_add_warning(ctx,
                       "%u non-sink output ports have no outgoing routes",
                       disconnected_outputs);
    }
}

/* ── Build route table from pipeline ────────────────────────────── */

int cy_build_route_table(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    if (pipe->edge_count == 0) {
        pipe->route_table = NULL;
        return CY_OK;
    }

    pipe->route_table = cy_route_table_create(ctx, pipe->port_count,
                                              pipe->edge_count);
    if (!pipe->route_table)
        return CY_ERR_NOMEM;

    /* Insert all valid edges into the route table */
    for (i = 0; i < pipe->edge_count; i++) {
        cy_edge *e = &pipe->edges[i];

        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;

        cy_route_table_add(pipe->route_table,
                           e->src_port, e->dst_port, e);
    }

    /* Analysis and validation */
    cy_analyze_fanout(pipe, ctx);
    cy_validate_connectivity(pipe, ctx);

    return CY_OK;
}

/* ── Multi-hop path discovery ───────────────────────────────────── */

int cy_route_find_all_paths(const cy_route_table *rt,
                            const cy_pipeline *pipe,
                            uint32_t src_port_id,
                            uint32_t dst_port_id,
                            uint32_t max_hops)
{
    uint32_t i;
    uint32_t paths_found = 0;

    if (!rt || !pipe || max_hops == 0)
        return 0;

    /* Direct route check */
    {
        const cy_route_entry *re = cy_route_by_src(rt, src_port_id);
        while (re) {
            if (re->dst_port && re->dst_port->id == dst_port_id)
                paths_found++;
            re = re->next_by_src;
        }
    }

    /* Two-hop routes through intermediate ports */
    if (max_hops >= 2) {
        const cy_route_entry *re1 = cy_route_by_src(rt, src_port_id);
        while (re1) {
            if (!re1->dst_port) {
                re1 = re1->next_by_src;
                continue;
            }

            uint32_t mid_node_id = re1->dst_port->node_id;
            if (mid_node_id >= pipe->node_count) {
                re1 = re1->next_by_src;
                continue;
            }

            /* Find output ports on the intermediate node */
            const cy_node *mid_node = &pipe->nodes[mid_node_id];
            if (!mid_node->ports) {
                re1 = re1->next_by_src;
                continue;
            }

            for (i = 0; i < mid_node->port_count; i++) {
                if (mid_node->ports[i].direction != CY_PORT_OUTPUT &&
                    mid_node->ports[i].direction != CY_PORT_BIDI)
                    continue;

                const cy_route_entry *re2 = cy_route_by_src(
                    rt, mid_node->ports[i].id);
                while (re2) {
                    if (re2->dst_port &&
                        re2->dst_port->id == dst_port_id)
                        paths_found++;
                    re2 = re2->next_by_src;
                }
            }

            re1 = re1->next_by_src;
        }
    }

    /* Three-hop routes */
    if (max_hops >= 3) {
        const cy_route_entry *re1 = cy_route_by_src(rt, src_port_id);
        while (re1) {
            if (!re1->dst_port) {
                re1 = re1->next_by_src;
                continue;
            }

            uint32_t mid1_id = re1->dst_port->node_id;
            if (mid1_id >= pipe->node_count) {
                re1 = re1->next_by_src;
                continue;
            }

            const cy_node *mid1 = &pipe->nodes[mid1_id];
            if (!mid1->ports) {
                re1 = re1->next_by_src;
                continue;
            }

            uint32_t pi1;
            for (pi1 = 0; pi1 < mid1->port_count; pi1++) {
                if (mid1->ports[pi1].direction != CY_PORT_OUTPUT &&
                    mid1->ports[pi1].direction != CY_PORT_BIDI)
                    continue;

                const cy_route_entry *re2 = cy_route_by_src(
                    rt, mid1->ports[pi1].id);
                while (re2) {
                    if (!re2->dst_port) {
                        re2 = re2->next_by_src;
                        continue;
                    }

                    uint32_t mid2_id = re2->dst_port->node_id;
                    if (mid2_id >= pipe->node_count) {
                        re2 = re2->next_by_src;
                        continue;
                    }

                    const cy_node *mid2 = &pipe->nodes[mid2_id];
                    if (!mid2->ports) {
                        re2 = re2->next_by_src;
                        continue;
                    }

                    uint32_t pi2;
                    for (pi2 = 0; pi2 < mid2->port_count; pi2++) {
                        if (mid2->ports[pi2].direction !=
                            CY_PORT_OUTPUT &&
                            mid2->ports[pi2].direction !=
                            CY_PORT_BIDI)
                            continue;

                        const cy_route_entry *re3 = cy_route_by_src(
                            rt, mid2->ports[pi2].id);
                        while (re3) {
                            if (re3->dst_port &&
                                re3->dst_port->id == dst_port_id)
                                paths_found++;
                            re3 = re3->next_by_src;
                        }
                    }

                    re2 = re2->next_by_src;
                }
            }

            re1 = re1->next_by_src;
        }
    }

    return (int)paths_found;
}

/* ── Route table consistency check ──────────────────────────────── */

int cy_route_verify_integrity(const cy_route_table *rt,
                              const cy_pipeline *pipe,
                              cy_context *ctx)
{
    uint32_t i;
    uint32_t orphan_src = 0;
    uint32_t orphan_dst = 0;
    uint32_t null_edge = 0;

    if (!rt || !pipe)
        return CY_OK;

    for (i = 0; i < rt->entry_count; i++) {
        cy_route_entry *re = &rt->entries[i];

        if (!re->edge) {
            null_edge++;
            continue;
        }

        if (!re->src_port) {
            orphan_src++;
            continue;
        }

        if (!re->dst_port) {
            orphan_dst++;
            continue;
        }

        /* Verify port IDs are within bounds */
        if (re->src_port->id >= pipe->port_count) {
            cy_add_warning(ctx,
                           "route entry %u: src port id %u out of range",
                           i, re->src_port->id);
        }

        if (re->dst_port->id >= pipe->port_count) {
            cy_add_warning(ctx,
                           "route entry %u: dst port id %u out of range",
                           i, re->dst_port->id);
        }

        /* Verify the edge's port IDs match the route entry */
        if (re->edge->src_port != re->src_port) {
            cy_add_warning(ctx,
                           "route entry %u: edge src_port mismatch",
                           i);
        }
        if (re->edge->dst_port != re->dst_port) {
            cy_add_warning(ctx,
                           "route entry %u: edge dst_port mismatch",
                           i);
        }
    }

    if (orphan_src > 0)
        cy_add_warning(ctx, "route table: %u entries with null src",
                       orphan_src);
    if (orphan_dst > 0)
        cy_add_warning(ctx, "route table: %u entries with null dst",
                       orphan_dst);
    if (null_edge > 0)
        cy_add_warning(ctx, "route table: %u entries with null edge",
                       null_edge);

    return CY_OK;
}

/* ── Bandwidth estimation through route ─────────────────────────── */

uint64_t cy_route_estimate_bandwidth(const cy_route_table *rt,
                                     uint32_t port_id, int direction)
{
    uint64_t total = 0;
    const cy_route_entry *re;

    if (!rt)
        return 0;

    if (direction == 0) {
        /* Outgoing bandwidth */
        re = cy_route_by_src(rt, port_id);
        while (re) {
            if (re->edge && re->dst_port && re->dst_port->type) {
                uint32_t type_size = re->dst_port->type->size;
                total += (uint64_t)type_size * re->edge->weight;
            }
            re = re->next_by_src;
        }
    } else {
        /* Incoming bandwidth */
        re = cy_route_by_dst(rt, port_id);
        while (re) {
            if (re->edge && re->src_port && re->src_port->type) {
                uint32_t type_size = re->src_port->type->size;
                total += (uint64_t)type_size * re->edge->weight;
            }
            re = re->next_by_dst;
        }
    }

    return total;
}
