/*
 * format.c — Formatting and debug output utilities
 *
 * Provides human-readable string representations of pipeline
 * components for debugging and inspection.
 *
 * Reference: CYC Format Specification §8 - Diagnostic Output
 */

#include "internal.h"

/* ── Type kind formatting ───────────────────────────────────────── */

int cy_format_type_kind(uint8_t kind, char *buf, size_t size)
{
    const char *name;

    switch (kind) {
    case CY_TYPE_INT:    name = "int";    break;
    case CY_TYPE_FLOAT:  name = "float";  break;
    case CY_TYPE_DOUBLE: name = "double"; break;
    case CY_TYPE_BOOL:   name = "bool";   break;
    case CY_TYPE_STRING: name = "string"; break;
    case CY_TYPE_BYTES:  name = "bytes";  break;
    case CY_TYPE_STRUCT: name = "struct"; break;
    case CY_TYPE_ARRAY:  name = "array";  break;
    case CY_TYPE_ENUM:   name = "enum";   break;
    case CY_TYPE_TENSOR: name = "tensor"; break;
    default:             name = "unknown"; break;
    }

    return snprintf(buf, size, "%s", name);
}

/* ── Port direction formatting ──────────────────────────────────── */

int cy_format_port_direction(uint8_t dir, char *buf, size_t size)
{
    const char *name;

    switch (dir) {
    case CY_PORT_INPUT:  name = "input";  break;
    case CY_PORT_OUTPUT: name = "output"; break;
    case CY_PORT_BIDI:   name = "bidi";   break;
    default:             name = "unknown"; break;
    }

    return snprintf(buf, size, "%s", name);
}

/* ── Edge info formatting ───────────────────────────────────────── */

int cy_format_edge_info(const cy_edge *e, const cy_pipeline *pipe,
                        char *buf, size_t size)
{
    const char *src_name = "?";
    const char *dst_name = "?";
    const char *src_node_name = "?";
    const char *dst_node_name = "?";

    if (!e)
        return snprintf(buf, size, "(null edge)");

    if (e->src_port) {
        if (e->src_port->name)
            src_name = e->src_port->name;
        if (e->src_port->node_id < pipe->node_count &&
            pipe->nodes[e->src_port->node_id].name)
            src_node_name = pipe->nodes[e->src_port->node_id].name;
    }
    if (e->dst_port) {
        if (e->dst_port->name)
            dst_name = e->dst_port->name;
        if (e->dst_port->node_id < pipe->node_count &&
            pipe->nodes[e->dst_port->node_id].name)
            dst_node_name = pipe->nodes[e->dst_port->node_id].name;
    }

    return snprintf(buf, size,
                    "edge %u: %s.%s -> %s.%s [w=%u buf=%u%s%s%s]",
                    e->id,
                    src_node_name, src_name,
                    dst_node_name, dst_name,
                    e->weight, e->buffer_size,
                    (e->flags & CY_EDGE_BUFFERED)  ? " BUF" : "",
                    (e->flags & CY_EDGE_FEEDBACK)   ? " FB"  : "",
                    (e->flags & CY_EDGE_VALID)      ? ""     : " INV");
}

/* ── Node port listing ──────────────────────────────────────────── */

int cy_format_node_ports(const cy_node *node, char *buf, size_t size)
{
    int written = 0;
    uint32_t i;
    char dir_buf[16];

    if (!node)
        return snprintf(buf, size, "(null node)");

    written += snprintf(buf + written, size - written,
                        "node %u (%s): %u ports\n",
                        node->id,
                        node->name ? node->name : "unnamed",
                        node->port_count);

    if (!node->ports)
        return written;

    for (i = 0; i < node->port_count && (size_t)written < size - 1; i++) {
        cy_port *port = &node->ports[i];
        cy_format_port_direction(port->direction, dir_buf,
                                 sizeof(dir_buf));

        written += snprintf(buf + written, size - written,
                            "  port %u: %s (%s) cap=%u conn=%u\n",
                            port->id,
                            port->name ? port->name : "unnamed",
                            dir_buf,
                            port->capacity,
                            port->connected_count);
    }

    return written;
}

/* ── Group member listing ───────────────────────────────────────── */

int cy_format_group_members(const cy_group *grp, char *buf, size_t size)
{
    int written = 0;
    uint32_t i;

    if (!grp)
        return snprintf(buf, size, "(null group)");

    written += snprintf(buf + written, size - written,
                        "group %u (%s): %u members%s\n",
                        grp->id,
                        grp->name ? grp->name : "unnamed",
                        grp->member_count,
                        (grp->flags & CY_GROUP_DISSOLVED) ?
                            " [dissolved]" : "");

    if (!grp->members)
        return written;

    for (i = 0; i < grp->member_count && (size_t)written < size - 1; i++) {
        if (grp->members[i]) {
            written += snprintf(buf + written, size - written,
                                "  node %u: %s%s\n",
                                grp->members[i]->id,
                                grp->members[i]->name ?
                                    grp->members[i]->name : "unnamed",
                                (grp->members[i]->flags & CY_NODE_PRUNED) ?
                                    " [pruned]" : "");
        } else {
            written += snprintf(buf + written, size - written,
                                "  (null member)\n");
        }
    }

    return written;
}

/* ── Schedule info formatting ───────────────────────────────────── */

int cy_format_schedule_info(const cy_sched_entry *se, char *buf,
                            size_t size)
{
    if (!se)
        return snprintf(buf, size, "(null schedule entry)");

    return snprintf(buf, size,
                    "sched %u: node %u order=%u lat=[%u,%u] "
                    "buf=%u%s%s%s",
                    se->id, se->node_id, se->order,
                    se->latency_min, se->latency_max,
                    se->buffer_alloc,
                    (se->flags & CY_SCHED_CRITICAL) ? " CRIT"    : "",
                    (se->flags & CY_SCHED_OPTIONAL) ? " OPT"     : "",
                    (se->flags & CY_SCHED_BARRIER)  ? " BARRIER" : "");
}

/* ── Parameter value formatting ─────────────────────────────────── */

int cy_format_param_value(const cy_param *p, char *buf, size_t size)
{
    if (!p)
        return snprintf(buf, size, "(null param)");

    switch (p->kind) {
    case CY_PARAM_INT:
        return snprintf(buf, size,
                        "param %u (%s): int %lld [%lld, %lld]",
                        p->id,
                        p->name ? p->name : "unnamed",
                        (long long)p->value.i,
                        (long long)p->min_val.i,
                        (long long)p->max_val.i);

    case CY_PARAM_FLOAT:
        return snprintf(buf, size,
                        "param %u (%s): float %f [%f, %f]",
                        p->id,
                        p->name ? p->name : "unnamed",
                        p->value.f,
                        p->min_val.f,
                        p->max_val.f);

    case CY_PARAM_STRING:
        return snprintf(buf, size,
                        "param %u (%s): string \"%s\"",
                        p->id,
                        p->name ? p->name : "unnamed",
                        p->value.s ? p->value.s : "(null)");

    case CY_PARAM_BOOL:
        return snprintf(buf, size,
                        "param %u (%s): bool %s",
                        p->id,
                        p->name ? p->name : "unnamed",
                        p->value.i ? "true" : "false");

    case CY_PARAM_ENUM:
        return snprintf(buf, size,
                        "param %u (%s): enum %lld",
                        p->id,
                        p->name ? p->name : "unnamed",
                        (long long)p->value.i);

    default:
        return snprintf(buf, size,
                        "param %u (%s): unknown kind %u",
                        p->id,
                        p->name ? p->name : "unnamed",
                        p->kind);
    }
}

/* ── Type detail formatting ─────────────────────────────────────── */

int cy_format_type_detail(const cy_type_desc *td, char *buf,
                          size_t size)
{
    char kind_buf[16];

    if (!td)
        return snprintf(buf, size, "(null type)");

    cy_format_type_kind(td->kind, kind_buf, sizeof(kind_buf));

    return snprintf(buf, size,
                    "type %u (%s): %s size=%u flags=0x%04x%s%s",
                    td->id,
                    td->name ? td->name : "unnamed",
                    kind_buf,
                    td->size,
                    td->flags,
                    td->base_type ? " (derived)" : "",
                    (td->flags & 0x8000) ? " [folded]" : "");
}

/* ── Pipeline summary ───────────────────────────────────────────── */

int cy_format_pipeline_summary(const cy_pipeline *pipe, char *buf,
                               size_t buf_size)
{
    int written = 0;
    uint32_t active_nodes = 0;
    uint32_t valid_edges = 0;
    uint32_t active_groups = 0;
    uint32_t i;

    if (!pipe)
        return snprintf(buf, buf_size, "(null pipeline)");

    /* Count active entities */
    for (i = 0; i < pipe->node_count; i++) {
        if (!(pipe->nodes[i].flags & CY_NODE_PRUNED) &&
            !(pipe->nodes[i].flags & CY_NODE_MERGED))
            active_nodes++;
    }

    for (i = 0; i < pipe->edge_count; i++) {
        if (pipe->edges[i].flags & CY_EDGE_VALID)
            valid_edges++;
    }

    for (i = 0; i < pipe->group_count; i++) {
        if (!(pipe->groups[i].flags & CY_GROUP_DISSOLVED))
            active_groups++;
    }

    written += snprintf(buf + written, buf_size - written,
                        "Pipeline v%u (flags=0x%08x)\n",
                        pipe->version, pipe->flags);

    written += snprintf(buf + written, buf_size - written,
                        "  Strings:    %u\n", pipe->string_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Types:      %u\n", pipe->type_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Nodes:      %u (%u active)\n",
                        pipe->node_count, active_nodes);
    written += snprintf(buf + written, buf_size - written,
                        "  Ports:      %u\n", pipe->port_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Edges:      %u (%u valid)\n",
                        pipe->edge_count, valid_edges);
    written += snprintf(buf + written, buf_size - written,
                        "  Params:     %u\n", pipe->param_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Schedule:   %u\n", pipe->schedule_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Groups:     %u (%u active)\n",
                        pipe->group_count, active_groups);

    if (pipe->port_cache) {
        written += snprintf(buf + written, buf_size - written,
                            "  Port cache: %u entries\n",
                            pipe->port_cache->entry_count);
    }

    if (pipe->route_table) {
        written += snprintf(buf + written, buf_size - written,
                            "  Routes:     %u entries\n",
                            pipe->route_table->entry_count);
    }

    /* Node detail formatting */
    int cy_format_node_detail(const cy_node *node, char *buf,
                              size_t buf_size);

    return written;
}

/* ── Node detail formatting ─────────────────────────────────────── */

int cy_format_node_detail(const cy_node *node, char *buf,
                          size_t buf_size)
{
    int written = 0;

    if (!node)
        return snprintf(buf, buf_size, "(null node)");

    written += snprintf(buf + written, buf_size - written,
                        "Node %u: %s\n",
                        node->id,
                        node->name ? node->name : "unnamed");
    written += snprintf(buf + written, buf_size - written,
                        "  Type:     %s (id=%u)\n",
                        node->type && node->type->name ?
                            node->type->name : "unnamed",
                        node->type_id);
    written += snprintf(buf + written, buf_size - written,
                        "  Ports:    %u\n", node->port_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Params:   %u (start=%u)\n",
                        node->param_count, node->param_start);
    written += snprintf(buf + written, buf_size - written,
                        "  Flags:    0x%08x%s%s%s%s%s\n",
                        node->flags,
                        (node->flags & CY_NODE_SOURCE)  ? " SRC"    : "",
                        (node->flags & CY_NODE_SINK)    ? " SINK"   : "",
                        (node->flags & CY_NODE_PRUNED)  ? " PRUNED" : "",
                        (node->flags & CY_NODE_MERGED)  ? " MERGED" : "",
                        (node->flags & CY_NODE_SPLIT)   ? " SPLIT"  : "");
    written += snprintf(buf + written, buf_size - written,
                        "  Group:    %u\n", node->group_id);

    return written;
}

/* ── Edge detail formatting ─────────────────────────────────────── */

int cy_format_edge_detail(const cy_edge *edge, char *buf,
                          size_t buf_size)
{
    int written = 0;

    if (!edge)
        return snprintf(buf, buf_size, "(null edge)");

    written += snprintf(buf + written, buf_size - written,
                        "Edge %u:\n", edge->id);
    written += snprintf(buf + written, buf_size - written,
                        "  Source:   port %u", edge->src_port_id);
    if (edge->src_port && edge->src_port->name)
        written += snprintf(buf + written, buf_size - written,
                            " (%s)", edge->src_port->name);
    written += snprintf(buf + written, buf_size - written, "\n");

    written += snprintf(buf + written, buf_size - written,
                        "  Dest:     port %u", edge->dst_port_id);
    if (edge->dst_port && edge->dst_port->name)
        written += snprintf(buf + written, buf_size - written,
                            " (%s)", edge->dst_port->name);
    written += snprintf(buf + written, buf_size - written, "\n");

    written += snprintf(buf + written, buf_size - written,
                        "  Weight:   %u\n", edge->weight);
    written += snprintf(buf + written, buf_size - written,
                        "  Buffer:   %u\n", edge->buffer_size);
    written += snprintf(buf + written, buf_size - written,
                        "  Flags:    0x%08x%s%s%s%s\n",
                        edge->flags,
                        (edge->flags & CY_EDGE_BUFFERED) ? " BUF"  : "",
                        (edge->flags & CY_EDGE_FEEDBACK) ? " FB"   : "",
                        (edge->flags & CY_EDGE_VALID)    ? " OK"   : "",
                        (edge->flags & CY_EDGE_IMPLICIT) ? " IMPL" : "");

    if (edge->src_type) {
        char kind[16];
        cy_format_type_kind(edge->src_type->kind, kind, sizeof(kind));
        written += snprintf(buf + written, buf_size - written,
                            "  Src type: %s (%s, %u bytes)\n",
                            edge->src_type->name ?
                                edge->src_type->name : "unnamed",
                            kind, edge->src_type->size);
    }
    if (edge->dst_type) {
        char kind[16];
        cy_format_type_kind(edge->dst_type->kind, kind, sizeof(kind));
        written += snprintf(buf + written, buf_size - written,
                            "  Dst type: %s (%s, %u bytes)\n",
                            edge->dst_type->name ?
                                edge->dst_type->name : "unnamed",
                            kind, edge->dst_type->size);
    }

    return written;
}

/* ── DOT graph export ───────────────────────────────────────────── */

int cy_format_dot_graph(const cy_pipeline *pipe, char *buf,
                        size_t buf_size)
{
    int written = 0;
    uint32_t i;

    if (!pipe)
        return snprintf(buf, buf_size, "// null pipeline\n");

    written += snprintf(buf + written, buf_size - written,
                        "digraph pipeline {\n");
    written += snprintf(buf + written, buf_size - written,
                        "  rankdir=LR;\n");
    written += snprintf(buf + written, buf_size - written,
                        "  node [shape=box];\n\n");

    /* Emit node definitions */
    for (i = 0; i < pipe->node_count && (size_t)written < buf_size - 64;
         i++) {
        const cy_node *n = &pipe->nodes[i];
        const char *shape = "box";
        const char *style = "";

        if (n->flags & CY_NODE_PRUNED)
            continue;
        if (n->flags & CY_NODE_MERGED)
            continue;

        if (n->flags & CY_NODE_SOURCE)
            shape = "ellipse";
        if (n->flags & CY_NODE_SINK)
            shape = "doubleoctagon";
        if (n->flags & CY_NODE_SPLIT)
            style = ",style=dashed";

        written += snprintf(buf + written, buf_size - written,
                            "  n%u [label=\"%s\\n(%u ports)\""
                            ",shape=%s%s];\n",
                            i,
                            n->name ? n->name : "unnamed",
                            n->port_count,
                            shape, style);
    }

    written += snprintf(buf + written, buf_size - written, "\n");

    /* Emit edges */
    for (i = 0; i < pipe->edge_count && (size_t)written < buf_size - 64;
         i++) {
        const cy_edge *e = &pipe->edges[i];
        const char *color = "black";
        const char *style_str = "";

        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;

        if (e->flags & CY_EDGE_FEEDBACK) {
            color = "red";
            style_str = ",style=dashed";
        }
        if (e->flags & CY_EDGE_IMPLICIT) {
            color = "gray";
            style_str = ",style=dotted";
        }

        written += snprintf(buf + written, buf_size - written,
                            "  n%u -> n%u [label=\"w=%u\""
                            ",color=%s%s];\n",
                            e->src_port->node_id,
                            e->dst_port->node_id,
                            e->weight,
                            color, style_str);
    }

    /* Emit group subgraphs */
    for (i = 0; i < pipe->group_count && (size_t)written < buf_size - 64;
         i++) {
        const cy_group *g = &pipe->groups[i];
        uint32_t mi;

        if (g->flags & CY_GROUP_DISSOLVED)
            continue;
        if (!g->members)
            continue;

        written += snprintf(buf + written, buf_size - written,
                            "\n  subgraph cluster_%u {\n"
                            "    label=\"%s\";\n"
                            "    style=dashed;\n"
                            "    color=blue;\n",
                            i,
                            g->name ? g->name : "unnamed");

        for (mi = 0; mi < g->member_count; mi++) {
            if (g->members[mi]) {
                written += snprintf(buf + written, buf_size - written,
                                    "    n%u;\n",
                                    g->members[mi]->id);
            }
        }

        written += snprintf(buf + written, buf_size - written,
                            "  }\n");
    }

    written += snprintf(buf + written, buf_size - written, "}\n");
    return written;
}

/* ── Route table formatting ─────────────────────────────────────── */

int cy_format_route_table(const cy_pipeline *pipe, char *buf,
                          size_t buf_size)
{
    int written = 0;
    uint32_t i;

    if (!pipe || !pipe->route_table)
        return snprintf(buf, buf_size, "(no route table)");

    written += snprintf(buf + written, buf_size - written,
                        "Route Table (%u entries, %u port slots)\n",
                        pipe->route_table->entry_count,
                        pipe->route_table->index_size);

    for (i = 0; i < pipe->route_table->entry_count &&
                (size_t)written < buf_size - 128; i++) {
        cy_route_entry *re = &pipe->route_table->entries[i];

        if (!re->src_port || !re->dst_port || !re->edge)
            continue;

        written += snprintf(buf + written, buf_size - written,
                            "  [%u] port %u (%s) -> port %u (%s) "
                            "via edge %u\n",
                            i,
                            re->src_port->id,
                            re->src_port->name ?
                                re->src_port->name : "?",
                            re->dst_port->id,
                            re->dst_port->name ?
                                re->dst_port->name : "?",
                            re->edge->id);
    }

    return written;
}

/* ── Port cache formatting ──────────────────────────────────────── */

int cy_format_port_cache(const cy_pipeline *pipe, char *buf,
                         size_t buf_size)
{
    int written = 0;
    uint32_t i;
    uint32_t used_buckets = 0;

    if (!pipe || !pipe->port_cache)
        return snprintf(buf, buf_size, "(no port cache)");

    for (i = 0; i < CY_PORT_CACHE_BUCKETS; i++) {
        if (pipe->port_cache->buckets[i])
            used_buckets++;
    }

    written += snprintf(buf + written, buf_size - written,
                        "Port Cache (%u entries, %u/%u buckets used)\n",
                        pipe->port_cache->entry_count,
                        used_buckets, CY_PORT_CACHE_BUCKETS);

    for (i = 0; i < pipe->port_cache->entry_count &&
                (size_t)written < buf_size - 128; i++) {
        cy_port_cache_entry *ce = &pipe->port_cache->entries[i];

        written += snprintf(buf + written, buf_size - written,
                            "  [%u] node %u + \"%s\" -> port %u\n",
                            i,
                            ce->node_id,
                            ce->name ? ce->name : "(null)",
                            ce->port ? ce->port->id : 0);
    }

    return written;
}

/* ── Latency analysis formatting ────────────────────────────────── */

int cy_format_latency_analysis(const cy_pipeline *pipe, char *buf,
                               size_t buf_size)
{
    int written = 0;
    uint32_t i;
    uint32_t critical_count = 0;
    uint32_t constrained_count = 0;
    uint64_t total_latency = 0;
    uint32_t max_latency = 0;

    if (!pipe || !pipe->schedule)
        return snprintf(buf, buf_size, "(no schedule data)");

    for (i = 0; i < pipe->schedule_count; i++) {
        const cy_sched_entry *se = &pipe->schedule[i];

        total_latency += se->latency_max;
        if (se->latency_max > max_latency)
            max_latency = se->latency_max;

        if (se->flags & CY_SCHED_CRITICAL)
            critical_count++;
        if (se->constraint_sink_id != 0xFFFFFFFF)
            constrained_count++;
    }

    written += snprintf(buf + written, buf_size - written,
                        "Latency Analysis\n");
    written += snprintf(buf + written, buf_size - written,
                        "  Entries:       %u\n",
                        pipe->schedule_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Critical:      %u\n", critical_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Constrained:   %u\n", constrained_count);
    written += snprintf(buf + written, buf_size - written,
                        "  Max latency:   %u\n", max_latency);
    written += snprintf(buf + written, buf_size - written,
                        "  Total latency: %llu\n",
                        (unsigned long long)total_latency);
    written += snprintf(buf + written, buf_size - written,
                        "  Budget:        %u\n",
                        pipe->latency_budget);

    written += snprintf(buf + written, buf_size - written,
                        "\n  Per-entry breakdown:\n");

    for (i = 0; i < pipe->schedule_count &&
                (size_t)written < buf_size - 128; i++) {
        const cy_sched_entry *se = &pipe->schedule[i];
        const char *node_name = "(null)";

        if (se->node && se->node->name)
            node_name = se->node->name;

        written += snprintf(buf + written, buf_size - written,
                            "    [%u] node %u (%s): order=%u "
                            "lat=[%u,%u] buf=%u",
                            i, se->node_id, node_name,
                            se->order, se->latency_min,
                            se->latency_max, se->buffer_alloc);

        if (se->flags & CY_SCHED_CRITICAL)
            written += snprintf(buf + written, buf_size - written,
                                " CRITICAL");
        if (se->constraint_sink_id != 0xFFFFFFFF)
            written += snprintf(buf + written, buf_size - written,
                                " sink=%u", se->constraint_sink_id);

        written += snprintf(buf + written, buf_size - written, "\n");
    }

    return written;
}

/* ── Comprehensive pipeline report ──────────────────────────────── */

int cy_format_full_report(const cy_pipeline *pipe, char *buf,
                          size_t buf_size)
{
    int written = 0;

    if (!pipe)
        return snprintf(buf, buf_size, "(null pipeline)\n");

    /* Pipeline summary */
    written += cy_format_pipeline_summary(pipe, buf + written,
                                          buf_size - written);
    written += snprintf(buf + written, buf_size - written, "\n");

    /* Node details */
    {
        uint32_t i;
        written += snprintf(buf + written, buf_size - written,
                            "--- Nodes ---\n");
        for (i = 0; i < pipe->node_count &&
                    (size_t)written < buf_size - 256; i++) {
            if (pipe->nodes[i].flags & CY_NODE_PRUNED)
                continue;
            written += cy_format_node_detail(&pipe->nodes[i],
                                             buf + written,
                                             buf_size - written);
        }
    }

    /* Edge details */
    {
        uint32_t i;
        written += snprintf(buf + written, buf_size - written,
                            "\n--- Edges ---\n");
        for (i = 0; i < pipe->edge_count &&
                    (size_t)written < buf_size - 256; i++) {
            if (!(pipe->edges[i].flags & CY_EDGE_VALID))
                continue;
            written += cy_format_edge_detail(&pipe->edges[i],
                                             buf + written,
                                             buf_size - written);
        }
    }

    /* Type details */
    {
        uint32_t i;
        written += snprintf(buf + written, buf_size - written,
                            "\n--- Types ---\n");
        for (i = 0; i < pipe->type_count &&
                    (size_t)written < buf_size - 128; i++) {
            written += cy_format_type_detail(&pipe->types[i],
                                             buf + written,
                                             buf_size - written);
            written += snprintf(buf + written, buf_size - written,
                                "\n");
        }
    }

    /* Group details */
    {
        uint32_t i;
        written += snprintf(buf + written, buf_size - written,
                            "\n--- Groups ---\n");
        for (i = 0; i < pipe->group_count &&
                    (size_t)written < buf_size - 256; i++) {
            written += cy_format_group_members(&pipe->groups[i],
                                               buf + written,
                                               buf_size - written);
        }
    }

    return written;
}
