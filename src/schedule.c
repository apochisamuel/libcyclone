/*
 * schedule.c — Phase 8: Schedule table parsing
 *
 * Schedule entries define the execution order of nodes within the
 * pipeline. Each entry specifies a node, its position in the
 * execution order, buffer allocation size, and latency constraints.
 * Latency constraints reference a "sink" node that must complete
 * within a specified time window of the scheduled node.
 *
 * Reference: CYC Format Specification §3.7 - Schedule Table
 */

#include "internal.h"

/* ── Schedule flag validation ───────────────────────────────────── */

static int cy_validate_schedule_flags(uint16_t flags, uint32_t id,
                                      cy_context *ctx)
{
    uint16_t known = CY_SCHED_CRITICAL | CY_SCHED_OPTIONAL |
                     CY_SCHED_BARRIER;

    if (flags & ~known) {
        cy_add_warning(ctx, "schedule entry %u: unknown flags 0x%04x",
                       id, flags & ~known);
    }

    if ((flags & CY_SCHED_CRITICAL) && (flags & CY_SCHED_OPTIONAL)) {
        cy_add_warning(ctx,
                       "schedule entry %u: both CRITICAL and OPTIONAL",
                       id);
    }

    return CY_OK;
}

/* ── Node reference resolution ──────────────────────────────────── */

static int cy_resolve_schedule_nodes(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        /* Resolve primary node reference */
        if (se->node_id >= pipe->node_count) {
            cy_add_warning(ctx,
                           "schedule entry %u: node_id %u out of range",
                           i, se->node_id);
            se->node = NULL;
        } else {
            se->node = &pipe->nodes[se->node_id];
        }

        /* Resolve constraint sink reference */
        if (se->constraint_sink_id == 0xFFFFFFFF) {
            se->constraint_sink = NULL;
        } else if (se->constraint_sink_id >= pipe->node_count) {
            cy_add_warning(ctx,
                           "schedule entry %u: constraint_sink_id %u "
                           "out of range",
                           i, se->constraint_sink_id);
            se->constraint_sink = NULL;
        } else {
            se->constraint_sink = &pipe->nodes[se->constraint_sink_id];
        }
    }

    return CY_OK;
}

/* ── Latency range validation ───────────────────────────────────── */

static int cy_validate_latency_ranges(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint32_t invalid_count = 0;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (se->latency_min > se->latency_max) {
            cy_add_warning(ctx,
                           "schedule entry %u: latency_min %u > "
                           "latency_max %u",
                           i, se->latency_min, se->latency_max);
            invalid_count++;
        }

        if (se->latency_max > 0 &&
            se->latency_max > pipe->latency_budget * 2) {
            cy_add_warning(ctx,
                           "schedule entry %u: latency_max %u exceeds "
                           "2x pipeline budget %u",
                           i, se->latency_max,
                           pipe->latency_budget);
        }
    }

    if (invalid_count > 0) {
        cy_add_warning(ctx, "%u schedule entries have invalid "
                       "latency ranges", invalid_count);
    }

    return CY_OK;
}

/* ── Execution order validation ─────────────────────────────────── */

static int cy_validate_execution_order(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t max_order = 0;

    /* Check for duplicate order values */
    for (i = 0; i < pipe->schedule_count; i++) {
        if (pipe->schedule[i].order > max_order)
            max_order = pipe->schedule[i].order;

        for (j = i + 1; j < pipe->schedule_count; j++) {
            if (pipe->schedule[i].order == pipe->schedule[j].order &&
                pipe->schedule[i].node_id != pipe->schedule[j].node_id) {
                cy_add_warning(ctx,
                               "schedule entries %u and %u share "
                               "order %u",
                               i, j, pipe->schedule[i].order);
            }
        }
    }

    /* Check for gaps in execution order */
    if (max_order > pipe->schedule_count * 2) {
        cy_add_warning(ctx,
                       "execution order has large gaps "
                       "(max order %u, entry count %u)",
                       max_order, pipe->schedule_count);
    }

    return CY_OK;
}

/* ── Buffer allocation analysis ─────────────────────────────────── */

static void cy_analyze_buffer_allocs(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;
    uint64_t total_alloc = 0;
    uint32_t zero_alloc = 0;

    for (i = 0; i < pipe->schedule_count; i++) {
        total_alloc += pipe->schedule[i].buffer_alloc;
        if (pipe->schedule[i].buffer_alloc == 0)
            zero_alloc++;
    }

    if (total_alloc > 1024 * 1024 * 512) {  /* 512 MB */
        cy_add_warning(ctx,
                       "total scheduled buffer allocation %llu bytes "
                       "exceeds 512 MB",
                       (unsigned long long)total_alloc);
    }

    if (zero_alloc > 0 && zero_alloc < pipe->schedule_count) {
        cy_add_warning(ctx,
                       "%u schedule entries have zero buffer allocation",
                       zero_alloc);
    }
}

/* ── Constraint pair validation ─────────────────────────────────── */

static int cy_validate_constraint_pairs(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (!se->constraint_sink)
            continue;

        /* Constraint sink should not be the same as the source */
        if (se->constraint_sink_id == se->node_id) {
            cy_add_warning(ctx,
                           "schedule entry %u: self-referencing "
                           "latency constraint",
                           i);
        }

        /* Constraint sink's order should be after the source */
        if (se->constraint_sink && se->node) {
            uint32_t j;
            int sink_found = 0;

            for (j = 0; j < pipe->schedule_count; j++) {
                if (pipe->schedule[j].node_id ==
                    se->constraint_sink_id) {
                    if (pipe->schedule[j].order <= se->order) {
                        cy_add_warning(ctx,
                                       "schedule entry %u: constraint "
                                       "sink order %u <= source order %u",
                                       i, pipe->schedule[j].order,
                                       se->order);
                    }
                    sink_found = 1;
                    break;
                }
            }

            if (!sink_found) {
                cy_add_warning(ctx,
                               "schedule entry %u: constraint sink "
                               "node %u not in schedule",
                               i, se->constraint_sink_id);
            }
        }
    }

    return CY_OK;
}

/* ── Latency chain pruning ──────────────────────────────────────── */
/* Removes nodes whose minimum latency exceeds the pipeline budget. */
/* Prunes the node and its downstream dependencies that become      */
/* unreachable. Route table source entries for pruned nodes are      */
/* cleared, but constraint sink references remain.                  */

void cy_prune_latency_chain(cy_pipeline *pipe, cy_node *node,
                            cy_context *ctx)
{
    uint32_t i, j;

    if (!node || (node->flags & CY_NODE_PRUNED))
        return;

    /* Mark the node as pruned */
    node->flags |= CY_NODE_PRUNED;

    /* Invalidate edges originating from this node and cascade */
    for (j = 0; j < pipe->edge_count; j++) {
        cy_edge *e = &pipe->edges[j];
        if (!e->src_port || !e->dst_port)
            continue;
        if (e->src_port->node_id != node->id)
            continue;
        if (!(e->flags & CY_EDGE_VALID))
            continue;

        e->flags &= ~CY_EDGE_VALID;

        /* If destination node now has no valid input edges,
           recursively prune it too */
        {
            cy_node *dst_node = NULL;
            uint32_t k;
            int has_valid_input = 0;

            for (k = 0; k < pipe->node_count; k++) {
                if (pipe->nodes[k].id == e->dst_port->node_id) {
                    dst_node = &pipe->nodes[k];
                    break;
                }
            }

            if (!dst_node)
                continue;

            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *other = &pipe->edges[k];
                if (!(other->flags & CY_EDGE_VALID))
                    continue;
                if (!other->dst_port)
                    continue;
                if (other->dst_port->node_id == dst_node->id) {
                    has_valid_input = 1;
                    break;
                }
            }

            if (!has_valid_input &&
                !(dst_node->flags & CY_NODE_SOURCE)) {
                cy_prune_latency_chain(pipe, dst_node, ctx);
            }
        }
    }

    /* Clear route table source entries for this node's ports */
    if (pipe->route_table && node->ports) {
        for (i = 0; i < node->port_count; i++) {
            uint32_t pid = node->ports[i].id;
            if (pid < pipe->route_table->index_size) {
                pipe->route_table->by_src[pid] = NULL;
            }
        }
    }

    /* Free port allocations to reclaim buffer memory.
       The PRUNED flag prevents further access. */
    if (node->ports) {
        for (i = 0; i < node->port_count; i++) {
            cy_free(node->ports[i].connected_edges);
        }
        cy_free(node->ports);
    }

    cy_add_warning(ctx, "pruned node %u (%s) from latency chain",
                   node->id, node->name ? node->name : "unnamed");
}

/* ── Main schedule table parser ─────────────────────────────────── */

int cy_parse_schedule(cy_reader *r, cy_pipeline *pipe,
                      const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;

    if (count == 0) {
        pipe->schedule_count = 0;
        pipe->schedule = NULL;
        return CY_OK;
    }

    if (count > CY_MAX_SCHEDULE) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "schedule count %u exceeds limit %u",
                     count, CY_MAX_SCHEDULE);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_SCHED_REC_SIZE, dir->length,
                                  r->ctx, "schedule table");
    if (rc != CY_OK)
        return rc;

    pipe->schedule = (cy_sched_entry *)cy_calloc(r->ctx, count,
                                                 sizeof(cy_sched_entry));
    if (!pipe->schedule)
        return CY_ERR_NOMEM;

    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    for (i = 0; i < count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        se->id                 = i;
        se->node_id            = cy_read_u32(r);
        se->order              = cy_read_u32(r);
        se->latency_min        = cy_read_u32(r);
        se->latency_max        = cy_read_u32(r);
        se->buffer_alloc       = cy_read_u32(r);
        se->flags              = cy_read_u16(r);
        /* 2 bytes reserved */
        cy_read_u16(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Handle constraint sink: packed after the schedule records */
        se->constraint_sink_id = 0xFFFFFFFF;
        se->constraint_sink    = NULL;

        /* Validate flags */
        cy_validate_schedule_flags(se->flags, i, r->ctx);
    }

    pipe->schedule_count = count;

    /* Read constraint sink IDs from after the main records.
       The remaining section data after count * CY_SCHED_REC_SIZE
       contains pairs of (entry_index: u32, sink_node_id: u32). */
    {
        uint32_t rec_end;
        uint32_t pair_space;

        if (cy_safe_multiply(count, CY_SCHED_REC_SIZE,
                             &rec_end) != 0) {
            return CY_OK;  /* overflow; skip constraints */
        }

        pair_space = dir->length - (uint32_t)rec_end;
        if (pair_space >= 8) {
            uint32_t num_pairs = pair_space / 8;
            uint32_t p;

            rc = cy_reader_seek(r, dir->offset + (uint32_t)rec_end);
            if (rc != CY_OK)
                return CY_OK;  /* non-fatal: constraints are optional */

            for (p = 0; p < num_pairs; p++) {
                uint32_t entry_idx = cy_read_u32(r);
                uint32_t sink_id   = cy_read_u32(r);

                if (r->ctx->error_code != CY_OK)
                    break;

                if (entry_idx < count) {
                    pipe->schedule[entry_idx].constraint_sink_id = sink_id;
                }
            }
        }
    }

    /* Resolve node references */
    rc = cy_resolve_schedule_nodes(pipe, r->ctx);
    if (rc != CY_OK)
        return rc;

    /* Validate */
    cy_validate_latency_ranges(pipe, r->ctx);
    cy_validate_execution_order(pipe, r->ctx);
    cy_validate_constraint_pairs(pipe, r->ctx);
    cy_analyze_buffer_allocs(pipe, r->ctx);

    return CY_OK;
}

/* ── Schedule dependency graph construction ─────────────────────── */

int cy_build_schedule_deps(const cy_pipeline *pipe,
                           uint32_t *dep_matrix,
                           uint32_t max_entries)
{
    uint32_t i, j;

    if (!pipe || !dep_matrix || !pipe->schedule)
        return -1;

    if (max_entries < pipe->schedule_count)
        return -1;

    memset(dep_matrix, 0,
           (size_t)pipe->schedule_count * pipe->schedule_count *
           sizeof(uint32_t));

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se_i = &pipe->schedule[i];
        if (!se_i->node)
            continue;

        for (j = 0; j < pipe->schedule_count; j++) {
            cy_sched_entry *se_j = &pipe->schedule[j];
            if (!se_j->node || i == j)
                continue;

            /* Check if there's a data dependency (edge) from i to j */
            uint32_t k;
            for (k = 0; k < pipe->edge_count; k++) {
                cy_edge *e = &pipe->edges[k];
                if (!(e->flags & CY_EDGE_VALID))
                    continue;
                if (!e->src_port || !e->dst_port)
                    continue;

                if (e->src_port->node_id == se_i->node_id &&
                    e->dst_port->node_id == se_j->node_id) {
                    dep_matrix[i * pipe->schedule_count + j] = 1;
                }
            }
        }
    }

    return 0;
}

/* ── Schedule slack computation ─────────────────────────────────── */

int cy_compute_schedule_slack(const cy_pipeline *pipe,
                              uint32_t *slack_values)
{
    uint32_t i;

    if (!pipe || !pipe->schedule || !slack_values)
        return -1;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        if (se->latency_max > se->latency_min) {
            slack_values[i] = se->latency_max - se->latency_min;
        } else {
            slack_values[i] = 0;
        }

        /* Adjust slack based on constraint relationships */
        if (se->constraint_sink && se->constraint_sink_id != 0xFFFFFFFF) {
            uint32_t j;
            for (j = 0; j < pipe->schedule_count; j++) {
                if (pipe->schedule[j].node_id == se->constraint_sink_id) {
                    uint32_t sink_slack = pipe->schedule[j].latency_max -
                                         pipe->schedule[j].latency_min;
                    /* Effective slack is the minimum of own slack
                       and constrained sink slack */
                    if (sink_slack < slack_values[i])
                        slack_values[i] = sink_slack;
                    break;
                }
            }
        }
    }

    return 0;
}

/* ── Schedule critical path identification ──────────────────────── */

int cy_find_schedule_critical_entries(const cy_pipeline *pipe,
                                     uint32_t *critical_indices,
                                     uint32_t max_count,
                                     uint32_t *critical_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!pipe || !pipe->schedule || !critical_indices || !critical_count)
        return -1;

    for (i = 0; i < pipe->schedule_count; i++) {
        cy_sched_entry *se = &pipe->schedule[i];

        int is_critical = 0;

        /* Explicitly marked critical */
        if (se->flags & CY_SCHED_CRITICAL)
            is_critical = 1;

        /* Zero slack = implicitly critical */
        if (se->latency_max == se->latency_min && se->latency_max > 0)
            is_critical = 1;

        /* Barrier nodes are always critical */
        if (se->flags & CY_SCHED_BARRIER)
            is_critical = 1;

        if (is_critical && found < max_count) {
            critical_indices[found] = i;
            found++;
        }
    }

    *critical_count = found;
    return 0;
}

/* ── Schedule timeline estimation ───────────────────────────────── */

int cy_estimate_schedule_timeline(const cy_pipeline *pipe,
                                  uint32_t *start_times,
                                  uint32_t *end_times)
{
    uint32_t i, j;

    if (!pipe || !pipe->schedule || !start_times || !end_times)
        return -1;

    /* Initialize: each node starts at time 0 */
    for (i = 0; i < pipe->schedule_count; i++) {
        start_times[i] = 0;
        end_times[i] = pipe->schedule[i].latency_max;
    }

    /* Forward pass: push start times based on dependencies */
    for (i = 0; i < pipe->schedule_count; i++) {
        /* Process in order of schedule entry order field */
        uint32_t min_order_idx = 0;
        uint32_t min_order = UINT32_MAX;

        for (j = 0; j < pipe->schedule_count; j++) {
            if (pipe->schedule[j].order < min_order &&
                start_times[j] == 0 && j > 0) {
                min_order = pipe->schedule[j].order;
                min_order_idx = j;
            }
        }

        cy_sched_entry *se = &pipe->schedule[min_order_idx];
        if (!se->node)
            continue;

        /* Find all predecessor completion times */
        uint32_t max_pred_end = 0;
        for (j = 0; j < pipe->edge_count; j++) {
            cy_edge *e = &pipe->edges[j];
            if (!(e->flags & CY_EDGE_VALID))
                continue;
            if (!e->src_port || !e->dst_port)
                continue;
            if (e->dst_port->node_id != se->node_id)
                continue;

            /* Find the schedule entry for the source node */
            uint32_t k;
            for (k = 0; k < pipe->schedule_count; k++) {
                if (pipe->schedule[k].node_id ==
                    e->src_port->node_id) {
                    if (end_times[k] > max_pred_end)
                        max_pred_end = end_times[k];
                    break;
                }
            }
        }

        start_times[min_order_idx] = max_pred_end;
        end_times[min_order_idx] = max_pred_end + se->latency_max;
    }

    return 0;
}
