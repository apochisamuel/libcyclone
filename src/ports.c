/*
 * ports.c — Phase 5: Port table parsing
 *
 * Ports are typed I/O connectors on nodes. Each port has a direction
 * (input, output, or bidirectional), a data type reference, a capacity
 * limit, and flags. Ports are read from a flat on-disk table, then
 * organized into per-node arrays. The port_index provides global
 * port ID to pointer mapping for edge resolution.
 *
 * Reference: CYC Format Specification §3.4 - Port Table
 */

#include "internal.h"

/* ── Port direction validation ──────────────────────────────────── */

static int cy_is_valid_direction(uint8_t dir)
{
    return (dir == CY_PORT_INPUT ||
            dir == CY_PORT_OUTPUT ||
            dir == CY_PORT_BIDI) ? 1 : 0;
}

/* ── Port capacity validation ───────────────────────────────────── */

static int cy_validate_port_capacity_field(uint16_t capacity, uint32_t id,
                                           cy_context *ctx)
{
    if (capacity == 0) {
        cy_add_warning(ctx, "port %u: zero capacity, defaulting to 1",
                       id);
        return 0;
    }
    return 1;
}

/* ── Temporary port record for on-disk data ─────────────────────── */

typedef struct {
    uint32_t id;
    uint32_t name_id;
    uint32_t node_id;
    uint32_t type_id;
    uint8_t  direction;
    uint16_t flags;
    uint16_t capacity;
} cy_port_record;

/* ── Count ports per node ───────────────────────────────────────── */

static int cy_count_ports_per_node(const cy_port_record *records,
                                   uint32_t port_count,
                                   cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    /* First pass: count ports for each node using port_start/port_count
       from the node records. If those don't match actual port records,
       use the port records' node_id as fallback. */
    for (i = 0; i < port_count; i++) {
        uint32_t nid = records[i].node_id;

        if (nid >= pipe->node_count) {
            cy_add_warning(ctx, "port %u: node_id %u out of range "
                           "(node_count=%u)", i, nid, pipe->node_count);
        }
    }

    return CY_OK;
}

/* ── Allocate per-node port arrays ──────────────────────────────── */

static int cy_allocate_node_ports(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];

        if (node->port_count == 0) {
            node->ports = NULL;
            continue;
        }

        node->ports = (cy_port *)cy_calloc(ctx, node->port_count,
                                           sizeof(cy_port));
        if (!node->ports)
            return CY_ERR_NOMEM;
    }

    return CY_OK;
}

/* ── Build global port index ────────────────────────────────────── */

static int cy_build_port_index(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t total_ports = 0;

    /* Count total ports across all nodes */
    for (i = 0; i < pipe->node_count; i++) {
        uint32_t new_total;
        if (cy_safe_add(total_ports, pipe->nodes[i].port_count,
                        &new_total) != 0) {
            cy_set_error(ctx, CY_ERR_OVERFLOW,
                         "total port count overflow");
            return CY_ERR_OVERFLOW;
        }
        total_ports = new_total;
    }

    if (total_ports == 0) {
        pipe->port_index = NULL;
        pipe->port_count = 0;
        return CY_OK;
    }

    if (total_ports > CY_MAX_PORTS) {
        cy_set_error(ctx, CY_ERR_BAD_COUNT,
                     "total port count %u exceeds limit %u",
                     total_ports, CY_MAX_PORTS);
        return CY_ERR_BAD_COUNT;
    }

    pipe->port_index = (cy_port **)cy_calloc(ctx, total_ports,
                                             sizeof(cy_port *));
    if (!pipe->port_index)
        return CY_ERR_NOMEM;

    /* Populate index from per-node arrays */
    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        for (j = 0; j < node->port_count; j++) {
            uint32_t pid = node->ports[j].id;
            if (pid < total_ports) {
                pipe->port_index[pid] = &node->ports[j];
            }
        }
    }

    pipe->port_count = total_ports;
    return CY_OK;
}

/* ── Port type resolution ───────────────────────────────────────── */

static int cy_resolve_port_types(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];

            if (port->type_id >= pipe->type_count) {
                cy_add_warning(ctx,
                               "port %u (node %u): type_id %u out of "
                               "range (count=%u)",
                               port->id, i, port->type_id,
                               pipe->type_count);
                port->type = NULL;
                continue;
            }

            port->type = &pipe->types[port->type_id];
        }
    }

    return CY_OK;
}

/* ── Port name uniqueness within a node ─────────────────────────── */

static int cy_check_port_names_unique(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j, k;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports || node->port_count < 2)
            continue;

        for (j = 0; j < node->port_count; j++) {
            if (!node->ports[j].name)
                continue;
            for (k = j + 1; k < node->port_count; k++) {
                if (!node->ports[k].name)
                    continue;
                if (strcmp(node->ports[j].name,
                           node->ports[k].name) == 0) {
                    cy_add_warning(ctx,
                                   "node %u (%s): duplicate port "
                                   "name '%s' at indices %u and %u",
                                   i,
                                   node->name ? node->name : "unnamed",
                                   node->ports[j].name, j, k);
                }
            }
        }
    }

    return CY_OK;
}

/* ── Direction statistics ───────────────────────────────────────── */

static void cy_log_port_statistics(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t total_in = 0, total_out = 0, total_bidi = 0;

    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;
        for (j = 0; j < node->port_count; j++) {
            switch (node->ports[j].direction) {
            case CY_PORT_INPUT:  total_in++;   break;
            case CY_PORT_OUTPUT: total_out++;  break;
            case CY_PORT_BIDI:   total_bidi++; break;
            }
        }
    }

    if (total_in == 0 && pipe->node_count > 1) {
        cy_add_warning(ctx, "no input ports found across %u nodes",
                       pipe->node_count);
    }
    if (total_out == 0 && pipe->node_count > 1) {
        cy_add_warning(ctx, "no output ports found across %u nodes",
                       pipe->node_count);
    }
}

/* ── Main port table parser ─────────────────────────────────────── */

int cy_parse_ports(cy_reader *r, cy_pipeline *pipe,
                   const cy_table_dir *dir)
{
    uint32_t        count = dir->count;
    uint32_t        i;
    int             rc;
    cy_port_record *records = NULL;
    uint32_t        port_idx;

    if (count == 0) {
        /* Ports are optional (nodes might have zero ports) */
        pipe->port_count = 0;
        pipe->port_index = NULL;
        return CY_OK;
    }

    if (count > CY_MAX_PORTS) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "port count %u exceeds limit %u",
                     count, CY_MAX_PORTS);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_PORT_REC_SIZE, dir->length,
                                  r->ctx, "port table");
    if (rc != CY_OK)
        return rc;

    /* Read port records into temporary array */
    records = (cy_port_record *)cy_calloc(r->ctx, count,
                                          sizeof(cy_port_record));
    if (!records)
        return CY_ERR_NOMEM;

    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK) {
        cy_free(records);
        return rc;
    }

    for (i = 0; i < count; i++) {
        records[i].id        = i;
        records[i].name_id   = cy_read_u32(r);
        records[i].node_id   = cy_read_u32(r);
        records[i].type_id   = cy_read_u32(r);
        records[i].direction = cy_read_u8(r);
        /* 1 byte padding */
        cy_read_u8(r);
        records[i].flags     = cy_read_u16(r);
        records[i].capacity  = cy_read_u16(r);
        /* 2 bytes reserved */
        cy_read_u16(r);

        if (r->ctx->error_code != CY_OK) {
            cy_free(records);
            return r->ctx->error_code;
        }

        /* Validate direction */
        if (!cy_is_valid_direction(records[i].direction)) {
            cy_add_warning(r->ctx,
                           "port %u: invalid direction %u, "
                           "defaulting to input",
                           i, records[i].direction);
            records[i].direction = CY_PORT_INPUT;
        }

        /* Validate capacity */
        if (!cy_validate_port_capacity_field(records[i].capacity, i,
                                             r->ctx)) {
            records[i].capacity = 1;
        }
    }

    /* Count ports per node (for validation) */
    cy_count_ports_per_node(records, count, pipe, r->ctx);

    /* Allocate per-node port arrays using the on-disk port_start and
       port_count from the node records */
    rc = cy_allocate_node_ports(pipe, r->ctx);
    if (rc != CY_OK) {
        cy_free(records);
        return rc;
    }

    /* Distribute port records into per-node arrays.
       The on-disk node record specified port_start and port_count
       to indicate which ports belong to each node. We use these
       to populate the per-node port arrays. */
    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports || node->port_count == 0)
            continue;

        for (port_idx = 0; port_idx < node->port_count; port_idx++) {
            uint32_t global_idx;
            cy_port *port;

            if (cy_safe_add(node->port_start, port_idx,
                            &global_idx) != 0)
                continue;

            if (global_idx >= count)
                continue;

            port = &node->ports[port_idx];
            port->id        = records[global_idx].id;
            port->name_id   = records[global_idx].name_id;
            port->node_id   = node->id;
            port->type_id   = records[global_idx].type_id;
            port->direction = records[global_idx].direction;
            port->flags     = records[global_idx].flags;
            port->capacity  = records[global_idx].capacity;

            /* Resolve name from string table */
            port->name = cy_resolve_string(pipe, port->name_id);
            if (!port->name) {
                cy_add_warning(r->ctx,
                               "port %u: name_id %u not in string table",
                               port->id, port->name_id);
            }

            /* Initialize connected edges (populated during edge parse) */
            port->connected_edges = NULL;
            port->connected_count = 0;
            port->connected_cap = 0;
        }
    }

    cy_free(records);

    /* Build global port index */
    rc = cy_build_port_index(pipe, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Resolve port type pointers */
    rc = cy_resolve_port_types(pipe, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Check port name uniqueness within each node */
    cy_check_port_names_unique(pipe, r->ctx);

    /* Log port statistics */
    cy_log_port_statistics(pipe, r->ctx);

    return CY_OK;
}

/* ── Port type compatibility matrix ─────────────────────────────── */

int cy_build_port_type_matrix(const cy_pipeline *pipe,
                              uint32_t *compat_matrix,
                              uint32_t max_types)
{
    uint32_t i, j;

    if (!pipe || !compat_matrix || max_types < pipe->type_count)
        return -1;

    memset(compat_matrix, 0,
           (size_t)max_types * max_types * sizeof(uint32_t));

    /* Build compatibility matrix from actual port connections */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->src_port->type_id >= max_types ||
            e->dst_port->type_id >= max_types)
            continue;

        compat_matrix[e->src_port->type_id * max_types +
                      e->dst_port->type_id]++;
    }

    /* Also mark theoretical compatibility from type system */
    for (i = 0; i < pipe->type_count; i++) {
        for (j = 0; j < pipe->type_count; j++) {
            if (cy_types_compatible(&pipe->types[i], &pipe->types[j])) {
                if (compat_matrix[i * max_types + j] == 0)
                    compat_matrix[i * max_types + j] = 0xFFFFFFFF;
            }
        }
    }

    return 0;
}

/* ── Port utilization report ────────────────────────────────────── */

int cy_port_utilization_report(const cy_pipeline *pipe,
                               uint32_t *unused_in,
                               uint32_t *unused_out,
                               uint32_t *saturated)
{
    uint32_t i, j, k;
    uint32_t u_in = 0, u_out = 0, sat = 0;

    if (!pipe || !unused_in || !unused_out || !saturated)
        return -1;

    for (i = 0; i < pipe->node_count; i++) {
        const cy_node *node = &pipe->nodes[i];
        if (!node->ports || (node->flags & CY_NODE_PRUNED))
            continue;

        for (j = 0; j < node->port_count; j++) {
            const cy_port *port = &node->ports[j];
            uint32_t connections = 0;

            for (k = 0; k < pipe->edge_count; k++) {
                const cy_edge *e = &pipe->edges[k];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;

                if ((e->src_port && e->src_port->id == port->id) ||
                    (e->dst_port && e->dst_port->id == port->id))
                    connections++;
            }

            if (connections == 0) {
                if (port->direction == CY_PORT_INPUT)
                    u_in++;
                else if (port->direction == CY_PORT_OUTPUT)
                    u_out++;
            }

            if (connections >= port->capacity && port->capacity > 0)
                sat++;
        }
    }

    *unused_in = u_in;
    *unused_out = u_out;
    *saturated = sat;
    return 0;
}
