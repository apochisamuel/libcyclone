/*
 * query.c — Public query API
 *
 * Provides read-only accessors for all parsed pipeline data.
 * These functions take pointers to opaque types and return
 * field values or const pointers to related objects.
 *
 * Reference: CYC Format Specification §7 - Query Interface
 */

#include "internal.h"

/* ── Pipeline queries ───────────────────────────────────────────── */

uint32_t cy_pipeline_version(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->version;
}

uint32_t cy_pipeline_flags(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->flags;
}

uint32_t cy_pipeline_node_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->node_count;
}

uint32_t cy_pipeline_edge_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->edge_count;
}

uint32_t cy_pipeline_type_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->type_count;
}

uint32_t cy_pipeline_group_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->group_count;
}

uint32_t cy_pipeline_param_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->param_count;
}

uint32_t cy_schedule_count(const cy_pipeline *pipe)
{
    if (!pipe)
        return 0;
    return pipe->schedule_count;
}

/* ── Node queries ───────────────────────────────────────────────── */

const cy_node *cy_get_node(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->nodes)
        return NULL;
    if (index >= pipe->node_count)
        return NULL;
    return &pipe->nodes[index];
}

const cy_node *cy_find_node_by_name(const cy_pipeline *pipe,
                                    const char *name)
{
    uint32_t i;

    if (!pipe || !pipe->nodes || !name)
        return NULL;

    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].name &&
            strcmp(pipe->nodes[i].name, name) == 0) {
            return &pipe->nodes[i];
        }
    }

    return NULL;
}

const char *cy_node_name(const cy_node *node)
{
    if (!node)
        return NULL;
    return node->name;
}

uint32_t cy_node_id(const cy_node *node)
{
    if (!node)
        return 0;
    return node->id;
}

uint32_t cy_node_port_count(const cy_node *node)
{
    if (!node)
        return 0;
    return node->port_count;
}

uint32_t cy_node_flags(const cy_node *node)
{
    if (!node)
        return 0;
    return node->flags;
}

/* ── Port queries ───────────────────────────────────────────────── */

const cy_port *cy_get_port(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->port_index)
        return NULL;
    if (index >= pipe->port_count)
        return NULL;
    return pipe->port_index[index];
}

const cy_port *cy_find_port(const cy_pipeline *pipe,
                            uint32_t node_id, const char *name)
{
    if (!pipe || !pipe->port_cache)
        return NULL;

    return cy_port_cache_lookup(pipe->port_cache, node_id, name);
}

const char *cy_port_name(const cy_port *port)
{
    if (!port)
        return NULL;
    return port->name;
}

uint32_t cy_port_id(const cy_port *port)
{
    if (!port)
        return 0;
    return port->id;
}

uint8_t cy_port_direction(const cy_port *port)
{
    if (!port)
        return 0;
    return port->direction;
}

const cy_type_desc *cy_port_type(const cy_port *port)
{
    if (!port)
        return NULL;
    return port->type;
}

/* ── Edge queries ───────────────────────────────────────────────── */

const cy_edge *cy_get_edge(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->edges)
        return NULL;
    if (index >= pipe->edge_count)
        return NULL;
    return &pipe->edges[index];
}

uint32_t cy_edge_src_port_id(const cy_edge *edge)
{
    if (!edge)
        return 0;
    return edge->src_port_id;
}

uint32_t cy_edge_dst_port_id(const cy_edge *edge)
{
    if (!edge)
        return 0;
    return edge->dst_port_id;
}

uint32_t cy_edge_flags(const cy_edge *edge)
{
    if (!edge)
        return 0;
    return edge->flags;
}

uint32_t cy_edge_buffer_size(const cy_edge *edge)
{
    if (!edge)
        return 0;
    return edge->buffer_size;
}

/* ── Type queries ───────────────────────────────────────────────── */

const cy_type_desc *cy_get_type(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->types)
        return NULL;
    if (index >= pipe->type_count)
        return NULL;
    return &pipe->types[index];
}

const char *cy_type_name(const cy_type_desc *td)
{
    if (!td)
        return NULL;
    return td->name;
}

uint8_t cy_type_kind(const cy_type_desc *td)
{
    if (!td)
        return 0;
    return td->kind;
}

uint32_t cy_type_size(const cy_type_desc *td)
{
    if (!td)
        return 0;
    return td->size;
}

/* ── Param queries ──────────────────────────────────────────────── */

const cy_param *cy_get_param(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->params)
        return NULL;
    if (index >= pipe->param_count)
        return NULL;
    return &pipe->params[index];
}

const char *cy_param_name(const cy_param *p)
{
    if (!p)
        return NULL;
    return p->name;
}

uint32_t cy_param_node_id(const cy_param *p)
{
    if (!p)
        return 0;
    return p->node_id;
}

uint8_t cy_param_kind(const cy_param *p)
{
    if (!p)
        return 0;
    return p->kind;
}

/* ── Schedule queries ───────────────────────────────────────────── */

const cy_sched_entry *cy_get_schedule_entry(const cy_pipeline *pipe,
                                            uint32_t index)
{
    if (!pipe || !pipe->schedule)
        return NULL;
    if (index >= pipe->schedule_count)
        return NULL;
    return &pipe->schedule[index];
}

uint32_t cy_sched_node_id(const cy_sched_entry *e)
{
    if (!e)
        return 0;
    return e->node_id;
}

uint32_t cy_sched_order(const cy_sched_entry *e)
{
    if (!e)
        return 0;
    return e->order;
}

/* ── Group queries ──────────────────────────────────────────────── */

const cy_group *cy_get_group(const cy_pipeline *pipe, uint32_t index)
{
    if (!pipe || !pipe->groups)
        return NULL;
    if (index >= pipe->group_count)
        return NULL;
    return &pipe->groups[index];
}

const char *cy_group_name(const cy_group *g)
{
    if (!g)
        return NULL;
    return g->name;
}

uint32_t cy_group_member_count(const cy_group *g)
{
    if (!g)
        return 0;
    return g->member_count;
}

uint32_t cy_group_flags(const cy_group *g)
{
    if (!g)
        return 0;
    return g->flags;
}

/* ── Traversal helpers ──────────────────────────────────────────── */

const cy_edge *cy_find_edge_between(const cy_pipeline *pipe,
                                    uint32_t src_port_id,
                                    uint32_t dst_port_id)
{
    uint32_t i;

    if (!pipe || !pipe->edges)
        return NULL;

    for (i = 0; i < pipe->edge_count; i++) {
        if (pipe->edges[i].src_port_id == src_port_id &&
            pipe->edges[i].dst_port_id == dst_port_id) {
            return &pipe->edges[i];
        }
    }

    return NULL;
}

uint32_t cy_count_edges_for_node(const cy_pipeline *pipe,
                                 uint32_t node_id)
{
    uint32_t i;
    uint32_t count = 0;

    if (!pipe || !pipe->edges)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (e->src_port && e->src_port->node_id == node_id)
            count++;
        if (e->dst_port && e->dst_port->node_id == node_id)
            count++;
    }

    return count;
}

const cy_group *cy_find_group_for_node(const cy_pipeline *pipe,
                                       uint32_t node_id)
{
    uint32_t gi, mi;

    if (!pipe || !pipe->groups)
        return NULL;

    for (gi = 0; gi < pipe->group_count; gi++) {
        const cy_group *g = &pipe->groups[gi];
        if (!g->members)
            continue;
        for (mi = 0; mi < g->member_count; mi++) {
            if (g->members[mi] && g->members[mi]->id == node_id)
                return g;
        }
    }

    return NULL;
}

int cy_get_node_schedule(const cy_pipeline *pipe, uint32_t node_id,
                         uint32_t *order, uint32_t *latency_min,
                         uint32_t *latency_max)
{
    uint32_t i;

    if (!pipe || !pipe->schedule)
        return -1;

    for (i = 0; i < pipe->schedule_count; i++) {
        if (pipe->schedule[i].node_id == node_id) {
            if (order)
                *order = pipe->schedule[i].order;
            if (latency_min)
                *latency_min = pipe->schedule[i].latency_min;
            if (latency_max)
                *latency_max = pipe->schedule[i].latency_max;
            return 0;
        }
    }

    return -1;
}

/* ── Downstream node enumeration ────────────────────────────────── */

uint32_t cy_get_downstream_nodes(const cy_pipeline *pipe,
                                 uint32_t node_id,
                                 uint32_t *out_nodes,
                                 uint32_t max_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !pipe->edges || !out_nodes)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->src_port->node_id != node_id)
            continue;

        uint32_t dst_nid = e->dst_port->node_id;

        /* Deduplicate */
        uint32_t j;
        int already_found = 0;
        for (j = 0; j < found; j++) {
            if (out_nodes[j] == dst_nid) {
                already_found = 1;
                break;
            }
        }

        if (!already_found && found < max_count) {
            out_nodes[found] = dst_nid;
            found++;
        }
    }

    return found;
}

/* ── Upstream node enumeration ──────────────────────────────────── */

uint32_t cy_get_upstream_nodes(const cy_pipeline *pipe,
                               uint32_t node_id,
                               uint32_t *out_nodes,
                               uint32_t max_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !pipe->edges || !out_nodes)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->dst_port->node_id != node_id)
            continue;

        uint32_t src_nid = e->src_port->node_id;

        uint32_t j;
        int already_found = 0;
        for (j = 0; j < found; j++) {
            if (out_nodes[j] == src_nid) {
                already_found = 1;
                break;
            }
        }

        if (!already_found && found < max_count) {
            out_nodes[found] = src_nid;
            found++;
        }
    }

    return found;
}

/* ── Node fan-in/fan-out computation ────────────────────────────── */

uint32_t cy_node_fanin(const cy_pipeline *pipe, uint32_t node_id)
{
    uint32_t i;
    uint32_t count = 0;

    if (!pipe || !pipe->edges)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (e->dst_port && e->dst_port->node_id == node_id)
            count++;
    }

    return count;
}

uint32_t cy_node_fanout(const cy_pipeline *pipe, uint32_t node_id)
{
    uint32_t i;
    uint32_t count = 0;

    if (!pipe || !pipe->edges)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (e->src_port && e->src_port->node_id == node_id)
            count++;
    }

    return count;
}

/* ── BFS shortest path between nodes ────────────────────────────── */

int cy_find_path(const cy_pipeline *pipe,
                 uint32_t src_node_id, uint32_t dst_node_id,
                 uint32_t *path, uint32_t max_path_len,
                 uint32_t *path_len)
{
    uint32_t *queue = NULL;
    uint32_t *parent = NULL;
    uint8_t  *visited = NULL;
    uint32_t  q_head = 0, q_tail = 0;
    uint32_t  i;
    int       found = 0;

    if (!pipe || !path || !path_len)
        return -1;
    if (src_node_id >= pipe->node_count ||
        dst_node_id >= pipe->node_count)
        return -1;

    queue = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    parent = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    visited = (uint8_t *)calloc(pipe->node_count, 1);
    if (!queue || !parent || !visited) {
        free(queue);
        free(parent);
        free(visited);
        return -1;
    }

    for (i = 0; i < pipe->node_count; i++)
        parent[i] = UINT32_MAX;

    visited[src_node_id] = 1;
    queue[q_tail++] = src_node_id;

    while (q_head < q_tail && !found) {
        uint32_t cur = queue[q_head++];

        if (cur == dst_node_id) {
            found = 1;
            break;
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
                parent[next] = cur;
                queue[q_tail++] = next;
            }
        }
    }

    if (!found) {
        free(queue);
        free(parent);
        free(visited);
        *path_len = 0;
        return -1;
    }

    /* Reconstruct path backwards from dst to src */
    uint32_t temp_path[256];
    uint32_t temp_len = 0;
    uint32_t cur = dst_node_id;

    while (cur != src_node_id && temp_len < 256) {
        temp_path[temp_len++] = cur;
        if (parent[cur] == UINT32_MAX)
            break;
        cur = parent[cur];
    }
    temp_path[temp_len++] = src_node_id;

    /* Reverse into output */
    *path_len = 0;
    for (i = 0; i < temp_len && i < max_path_len; i++) {
        path[i] = temp_path[temp_len - 1 - i];
        (*path_len)++;
    }

    free(queue);
    free(parent);
    free(visited);
    return 0;
}

/* ── Critical path analysis (longest path through DAG) ──────────── */

int cy_find_critical_path(const cy_pipeline *pipe,
                          uint32_t *path, uint32_t max_path_len,
                          uint32_t *path_len)
{
    uint32_t *dist = NULL;
    uint32_t *pred = NULL;
    uint32_t  i, j;
    uint32_t  max_dist = 0;
    uint32_t  max_node = 0;
    int       changed;

    if (!pipe || !path || !path_len)
        return -1;

    dist = (uint32_t *)calloc(pipe->node_count, sizeof(uint32_t));
    pred = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    if (!dist || !pred) {
        free(dist);
        free(pred);
        return -1;
    }

    for (i = 0; i < pipe->node_count; i++)
        pred[i] = UINT32_MAX;

    /* Bellman-Ford style longest path (works for DAG in O(V*E)) */
    for (j = 0; j < pipe->node_count; j++) {
        changed = 0;
        for (i = 0; i < pipe->edge_count; i++) {
            const cy_edge *e = &pipe->edges[i];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (!e->src_port || !e->dst_port)
                continue;

            uint32_t u = e->src_port->node_id;
            uint32_t v = e->dst_port->node_id;
            if (u >= pipe->node_count || v >= pipe->node_count)
                continue;

            uint32_t new_dist = dist[u] + 1;
            if (new_dist > dist[v]) {
                dist[v] = new_dist;
                pred[v] = u;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }

    /* Find the node with maximum distance */
    for (i = 0; i < pipe->node_count; i++) {
        if (dist[i] > max_dist) {
            max_dist = dist[i];
            max_node = i;
        }
    }

    /* Reconstruct critical path */
    uint32_t temp[256];
    uint32_t temp_len = 0;
    uint32_t cur = max_node;

    while (cur != UINT32_MAX && temp_len < 256) {
        temp[temp_len++] = cur;
        cur = pred[cur];
    }

    *path_len = 0;
    for (i = 0; i < temp_len && i < max_path_len; i++) {
        path[i] = temp[temp_len - 1 - i];
        (*path_len)++;
    }

    free(dist);
    free(pred);
    return 0;
}

/* ── Pipeline statistics computation ────────────────────────────── */

typedef struct {
    uint32_t total_nodes;
    uint32_t active_nodes;
    uint32_t source_nodes;
    uint32_t sink_nodes;
    uint32_t total_edges;
    uint32_t valid_edges;
    uint32_t feedback_edges;
    uint32_t total_ports;
    uint32_t input_ports;
    uint32_t output_ports;
    uint32_t total_params;
    uint32_t total_groups;
    uint32_t active_groups;
    uint32_t max_fanin;
    uint32_t max_fanout;
    uint64_t total_buffer_bytes;
    uint32_t critical_path_length;
} cy_pipeline_stats;

int cy_compute_pipeline_stats(const cy_pipeline *pipe,
                              cy_pipeline_stats *stats)
{
    uint32_t i, j;

    if (!pipe || !stats)
        return -1;

    memset(stats, 0, sizeof(cy_pipeline_stats));

    stats->total_nodes = pipe->node_count;
    stats->total_edges = pipe->edge_count;
    stats->total_params = pipe->param_count;
    stats->total_groups = pipe->group_count;

    /* Node statistics */
    for (i = 0; i < pipe->node_count; i++) {
        const cy_node *n = &pipe->nodes[i];

        if (!(n->flags & CY_NODE_PRUNED) &&
            !(n->flags & CY_NODE_MERGED))
            stats->active_nodes++;

        if (n->flags & CY_NODE_SOURCE)
            stats->source_nodes++;
        if (n->flags & CY_NODE_SINK)
            stats->sink_nodes++;

        if (n->ports) {
            for (j = 0; j < n->port_count; j++) {
                stats->total_ports++;
                if (n->ports[j].direction == CY_PORT_INPUT)
                    stats->input_ports++;
                else if (n->ports[j].direction == CY_PORT_OUTPUT)
                    stats->output_ports++;
            }
        }

        /* Compute fan-in and fan-out */
        uint32_t fi = cy_node_fanin(pipe, i);
        uint32_t fo = cy_node_fanout(pipe, i);
        if (fi > stats->max_fanin)
            stats->max_fanin = fi;
        if (fo > stats->max_fanout)
            stats->max_fanout = fo;
    }

    /* Edge statistics */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];

        if (e->flags & CY_EDGE_VALID)
            stats->valid_edges++;
        if (e->flags & CY_EDGE_FEEDBACK)
            stats->feedback_edges++;
        if (e->flags & CY_EDGE_BUFFERED)
            stats->total_buffer_bytes += e->buffer_size;
    }

    /* Group statistics */
    for (i = 0; i < pipe->group_count; i++) {
        if (!(pipe->groups[i].flags & CY_GROUP_DISSOLVED))
            stats->active_groups++;
    }

    /* Critical path */
    {
        uint32_t cp[256];
        uint32_t cp_len = 0;
        if (cy_find_critical_path(pipe, cp, 256, &cp_len) == 0)
            stats->critical_path_length = cp_len;
    }

    return 0;
}

/* ── Topological sort ───────────────────────────────────────────── */

int cy_topological_sort(const cy_pipeline *pipe,
                        uint32_t *sorted, uint32_t max_count,
                        uint32_t *sorted_count)
{
    uint32_t *in_degree = NULL;
    uint32_t *queue = NULL;
    uint32_t  q_head = 0, q_tail = 0;
    uint32_t  i, j;
    uint32_t  count = 0;

    if (!pipe || !sorted || !sorted_count)
        return -1;

    in_degree = (uint32_t *)calloc(pipe->node_count, sizeof(uint32_t));
    queue = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    if (!in_degree || !queue) {
        free(in_degree);
        free(queue);
        return -1;
    }

    /* Compute in-degrees */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (e->flags & CY_EDGE_FEEDBACK)
            continue;
        if (!e->dst_port)
            continue;
        uint32_t dst = e->dst_port->node_id;
        if (dst < pipe->node_count)
            in_degree[dst]++;
    }

    /* Seed queue with zero in-degree nodes */
    for (i = 0; i < pipe->node_count; i++) {
        if (in_degree[i] == 0 &&
            !(pipe->nodes[i].flags & CY_NODE_PRUNED))
            queue[q_tail++] = i;
    }

    /* Process */
    while (q_head < q_tail && count < max_count) {
        uint32_t cur = queue[q_head++];
        sorted[count++] = cur;

        for (j = 0; j < pipe->edge_count; j++) {
            const cy_edge *e = &pipe->edges[j];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (e->flags & CY_EDGE_FEEDBACK)
                continue;
            if (!e->src_port || !e->dst_port)
                continue;
            if (e->src_port->node_id != cur)
                continue;

            uint32_t next = e->dst_port->node_id;
            if (next < pipe->node_count) {
                in_degree[next]--;
                if (in_degree[next] == 0)
                    queue[q_tail++] = next;
            }
        }
    }

    *sorted_count = count;

    free(in_degree);
    free(queue);
    return 0;
}

/* ── Cycle detection ────────────────────────────────────────────── */

int cy_has_cycle(const cy_pipeline *pipe)
{
    uint32_t sorted[1024];
    uint32_t sorted_count = 0;
    uint32_t active_count = 0;
    uint32_t i;

    if (!pipe)
        return 0;

    /* Count active (non-pruned) nodes */
    for (i = 0; i < pipe->node_count; i++) {
        if (!(pipe->nodes[i].flags & CY_NODE_PRUNED))
            active_count++;
    }

    if (active_count == 0)
        return 0;

    cy_topological_sort(pipe, sorted,
                        active_count < 1024 ? active_count : 1024,
                        &sorted_count);

    /* If topo sort didn't include all active nodes, there's a cycle */
    return (sorted_count < active_count) ? 1 : 0;
}

/* ── Node depth computation (from sources) ──────────────────────── */

int cy_compute_node_depths(const cy_pipeline *pipe,
                           uint32_t *depths)
{
    uint32_t i;
    int changed;
    uint32_t iteration;

    if (!pipe || !depths)
        return -1;

    for (i = 0; i < pipe->node_count; i++)
        depths[i] = UINT32_MAX;

    /* Source nodes have depth 0 */
    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].flags & CY_NODE_SOURCE)
            depths[i] = 0;
    }

    /* Relax edges until convergence */
    for (iteration = 0; iteration < pipe->node_count; iteration++) {
        changed = 0;
        for (i = 0; i < pipe->edge_count; i++) {
            const cy_edge *e = &pipe->edges[i];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (!e->src_port || !e->dst_port)
                continue;

            uint32_t u = e->src_port->node_id;
            uint32_t v = e->dst_port->node_id;
            if (u >= pipe->node_count || v >= pipe->node_count)
                continue;
            if (depths[u] == UINT32_MAX)
                continue;

            uint32_t new_depth = depths[u] + 1;
            if (new_depth < depths[v]) {
                depths[v] = new_depth;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }

    return 0;
}

/* ── Port connection summary for a node ─────────────────────────── */

int cy_get_port_connections(const cy_pipeline *pipe,
                            uint32_t port_id,
                            uint32_t *connected_port_ids,
                            uint32_t max_count,
                            uint32_t *conn_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !connected_port_ids || !conn_count)
        return -1;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;

        if (e->src_port && e->src_port->id == port_id &&
            e->dst_port && found < max_count) {
            connected_port_ids[found++] = e->dst_port->id;
        }
        if (e->dst_port && e->dst_port->id == port_id &&
            e->src_port && found < max_count) {
            connected_port_ids[found++] = e->src_port->id;
        }
    }

    *conn_count = found;
    return 0;
}

/* ── Strongly connected component detection (Tarjan) ────────────── */

typedef struct {
    const cy_pipeline *pipe;
    uint32_t *index_arr;
    uint32_t *lowlink;
    uint8_t  *on_stack;
    uint32_t *stack;
    uint32_t  stack_top;
    uint32_t  current_index;
    uint32_t  scc_count;
    uint32_t *scc_id;
} cy_tarjan_state;

static void cy_tarjan_strongconnect(cy_tarjan_state *state, uint32_t v)
{
    uint32_t i;
    const cy_pipeline *pipe = state->pipe;

    state->index_arr[v] = state->current_index;
    state->lowlink[v] = state->current_index;
    state->current_index++;

    state->stack[state->stack_top++] = v;
    state->on_stack[v] = 1;

    /* Visit successors */
    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->src_port->node_id != v)
            continue;

        uint32_t w = e->dst_port->node_id;
        if (w >= pipe->node_count)
            continue;

        if (state->index_arr[w] == UINT32_MAX) {
            cy_tarjan_strongconnect(state, w);
            if (state->lowlink[w] < state->lowlink[v])
                state->lowlink[v] = state->lowlink[w];
        } else if (state->on_stack[w]) {
            if (state->index_arr[w] < state->lowlink[v])
                state->lowlink[v] = state->index_arr[w];
        }
    }

    /* Root of an SCC */
    if (state->lowlink[v] == state->index_arr[v]) {
        uint32_t w;
        do {
            state->stack_top--;
            w = state->stack[state->stack_top];
            state->on_stack[w] = 0;
            state->scc_id[w] = state->scc_count;
        } while (w != v);
        state->scc_count++;
    }
}

int cy_find_sccs(const cy_pipeline *pipe,
                 uint32_t *scc_ids, uint32_t *scc_count)
{
    cy_tarjan_state state;
    uint32_t i;

    if (!pipe || !scc_ids || !scc_count)
        return -1;

    state.pipe = pipe;
    state.current_index = 0;
    state.stack_top = 0;
    state.scc_count = 0;

    state.index_arr = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    state.lowlink = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    state.on_stack = (uint8_t *)calloc(pipe->node_count, 1);
    state.stack = (uint32_t *)malloc(pipe->node_count * sizeof(uint32_t));
    state.scc_id = scc_ids;

    if (!state.index_arr || !state.lowlink ||
        !state.on_stack || !state.stack) {
        free(state.index_arr);
        free(state.lowlink);
        free(state.on_stack);
        free(state.stack);
        return -1;
    }

    for (i = 0; i < pipe->node_count; i++) {
        state.index_arr[i] = UINT32_MAX;
        state.lowlink[i] = UINT32_MAX;
    }

    for (i = 0; i < pipe->node_count; i++) {
        if (state.index_arr[i] == UINT32_MAX &&
            !(pipe->nodes[i].flags & CY_NODE_PRUNED)) {
            cy_tarjan_strongconnect(&state, i);
        }
    }

    *scc_count = state.scc_count;

    free(state.index_arr);
    free(state.lowlink);
    free(state.on_stack);
    free(state.stack);

    return 0;
}

/* ── Edge weight aggregation by node pair ───────────────────────── */

uint64_t cy_total_edge_weight_between(const cy_pipeline *pipe,
                                      uint32_t src_node, uint32_t dst_node)
{
    uint32_t i;
    uint64_t total = 0;

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
            total += e->weight;
        }
    }

    return total;
}

/* ── Parameter aggregation for node type ─────────────────────────── */

uint32_t cy_count_params_by_kind(const cy_pipeline *pipe,
                                 uint32_t node_id, uint8_t kind)
{
    uint32_t i;
    uint32_t count = 0;

    if (!pipe || !pipe->params)
        return 0;

    for (i = 0; i < pipe->param_count; i++) {
        if (pipe->params[i].node_id == node_id &&
            pipe->params[i].kind == kind)
            count++;
    }

    return count;
}

/* ── Type usage analysis ────────────────────────────────────────── */

uint32_t cy_count_type_usage(const cy_pipeline *pipe, uint32_t type_id)
{
    uint32_t i, j;
    uint32_t count = 0;

    if (!pipe)
        return 0;

    for (i = 0; i < pipe->node_count; i++) {
        const cy_node *n = &pipe->nodes[i];
        if (!n->ports)
            continue;
        for (j = 0; j < n->port_count; j++) {
            if (n->ports[j].type_id == type_id)
                count++;
        }
    }

    return count;
}
