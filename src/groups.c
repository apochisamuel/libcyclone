/*
 * groups.c — Phase 9: Group table parsing
 *
 * Groups define hierarchical subgraphs within the pipeline. Each
 * group has member nodes and a port map that exposes internal ports
 * at the group boundary. Groups may be dissolved during validation
 * if internal cycles are detected.
 *
 * Reference: CYC Format Specification §3.8 - Group Table
 */

#include "internal.h"

/* ── Group flag validation ──────────────────────────────────────── */

static int cy_validate_group_flags(uint32_t flags, uint32_t id,
                                   cy_context *ctx)
{
    uint32_t known = CY_GROUP_ISOLATED | CY_GROUP_REENTRANT |
                     CY_GROUP_DISSOLVED;

    if (flags & ~known) {
        cy_add_warning(ctx, "group %u: unknown flags 0x%08x",
                       id, flags & ~known);
    }

    if ((flags & CY_GROUP_ISOLATED) && (flags & CY_GROUP_REENTRANT)) {
        cy_add_warning(ctx,
                       "group %u: both ISOLATED and REENTRANT flags",
                       id);
    }

    return CY_OK;
}

/* ── Member list parsing ────────────────────────────────────────── */

static int cy_parse_member_list(cy_reader *r, cy_pipeline *pipe,
                                cy_group *grp, uint32_t member_offset,
                                uint32_t member_count)
{
    uint32_t i;
    int      rc;

    if (member_count == 0) {
        grp->members = NULL;
        grp->member_count = 0;
        return CY_OK;
    }

    if (member_count > CY_MAX_GROUP_MEMBERS) {
        cy_add_warning(r->ctx,
                       "group %u: member count %u exceeds limit %u, "
                       "clamping",
                       grp->id, member_count, CY_MAX_GROUP_MEMBERS);
        member_count = CY_MAX_GROUP_MEMBERS;
    }

    grp->members = (cy_node **)cy_calloc(r->ctx, member_count,
                                         sizeof(cy_node *));
    if (!grp->members)
        return CY_ERR_NOMEM;

    rc = cy_reader_seek(r, member_offset);
    if (rc != CY_OK)
        return rc;

    for (i = 0; i < member_count; i++) {
        uint32_t node_id = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        if (node_id >= pipe->node_count) {
            cy_add_warning(r->ctx,
                           "group %u: member node_id %u out of range",
                           grp->id, node_id);
            grp->members[i] = NULL;
            continue;
        }

        grp->members[i] = &pipe->nodes[node_id];

        /* Set the node's group_id back-reference */
        pipe->nodes[node_id].group_id = grp->id + 1;  /* 1-indexed */
    }

    grp->member_count = member_count;
    return CY_OK;
}

/* ── Port map parsing ───────────────────────────────────────────── */

static int cy_parse_port_map(cy_reader *r, cy_pipeline *pipe,
                             cy_group *grp, uint32_t map_offset,
                             uint32_t map_count)
{
    uint32_t i;
    int      rc;

    if (map_count == 0) {
        grp->port_map = NULL;
        grp->port_map_count = 0;
        return CY_OK;
    }

    if (map_count > CY_MAX_PORT_MAP) {
        cy_add_warning(r->ctx,
                       "group %u: port_map count %u exceeds limit %u",
                       grp->id, map_count, CY_MAX_PORT_MAP);
        map_count = CY_MAX_PORT_MAP;
    }

    grp->port_map = (cy_port_map_entry *)cy_calloc(r->ctx, map_count,
                                                    sizeof(cy_port_map_entry));
    if (!grp->port_map)
        return CY_ERR_NOMEM;

    rc = cy_reader_seek(r, map_offset);
    if (rc != CY_OK)
        return rc;

    for (i = 0; i < map_count; i++) {
        cy_port_map_entry *pme = &grp->port_map[i];

        pme->external_name_id = cy_read_u32(r);
        pme->internal_port_id = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Resolve external name */
        pme->external_name = cy_resolve_string(pipe,
                                               pme->external_name_id);
        if (!pme->external_name) {
            cy_add_warning(r->ctx,
                           "group %u port_map %u: external_name_id "
                           "%u not in string table",
                           grp->id, i, pme->external_name_id);
        }

        /* Resolve internal port pointer */
        if (pme->internal_port_id < pipe->port_count &&
            pipe->port_index[pme->internal_port_id] != NULL) {
            pme->internal_port =
                pipe->port_index[pme->internal_port_id];
        } else {
            cy_add_warning(r->ctx,
                           "group %u port_map %u: internal_port_id "
                           "%u not resolved",
                           grp->id, i, pme->internal_port_id);
            pme->internal_port = NULL;
        }
    }

    grp->port_map_count = map_count;
    return CY_OK;
}

/* ── Internal cycle detection within a group ────────────────────── */

int cy_detect_group_cycle(cy_pipeline *pipe, cy_group *grp,
                          cy_context *ctx)
{
    uint32_t i, j, k;
    uint8_t *visited = NULL;
    uint8_t *in_stack = NULL;
    int      has_cycle = 0;

    if (!grp->members || grp->member_count < 2)
        return CY_OK;

    visited = (uint8_t *)cy_calloc(ctx, pipe->node_count, 1);
    in_stack = (uint8_t *)cy_calloc(ctx, pipe->node_count, 1);
    if (!visited || !in_stack) {
        cy_free(visited);
        cy_free(in_stack);
        return CY_ERR_NOMEM;
    }

    /* Simple cycle detection using adjacency within the group */
    for (i = 0; i < grp->member_count && !has_cycle; i++) {
        cy_node *start;
        uint32_t stack[CY_GROUP_CYCLE_LIMIT];
        int top = 0;

        if (!grp->members[i])
            continue;
        start = grp->members[i];
        if (visited[start->id])
            continue;

        stack[0] = start->id;
        in_stack[start->id] = 1;

        while (top >= 0 && !has_cycle) {
            uint32_t cur_id = stack[top];
            int found_next = 0;

            visited[cur_id] = 1;

            /* Find unvisited neighbors within the group */
            for (j = 0; j < pipe->edge_count && !found_next; j++) {
                cy_edge *e = &pipe->edges[j];
                if (!e->src_port || !e->dst_port)
                    continue;
                if (e->src_port->node_id != cur_id)
                    continue;

                uint32_t dst_nid = e->dst_port->node_id;

                /* Check if destination is in this group */
                int in_group = 0;
                for (k = 0; k < grp->member_count; k++) {
                    if (grp->members[k] &&
                        grp->members[k]->id == dst_nid) {
                        in_group = 1;
                        break;
                    }
                }

                if (!in_group)
                    continue;

                if (in_stack[dst_nid]) {
                    /* Cycle detected! */
                    has_cycle = 1;
                    break;
                }

                if (!visited[dst_nid] &&
                    top + 1 < (int)CY_GROUP_CYCLE_LIMIT) {
                    top++;
                    stack[top] = dst_nid;
                    in_stack[dst_nid] = 1;
                    found_next = 1;
                }
            }

            if (!found_next) {
                in_stack[cur_id] = 0;
                top--;
            }
        }

        /* Clear in_stack */
        for (j = 0; j < pipe->node_count; j++)
            in_stack[j] = 0;
    }

    cy_free(visited);
    cy_free(in_stack);

    if (has_cycle) {
        cy_add_warning(ctx,
                       "group %u (%s): internal cycle detected",
                       grp->id,
                       grp->name ? grp->name : "unnamed");
        return CY_ERR_CYCLE_DETECTED;
    }

    return CY_OK;
}

/* ── Group dissolution ──────────────────────────────────────────── */
/* Dissolves a group by freeing its member nodes' subgraph-local   */
/* port allocations. The route table entries pointing to dissolved  */
/* ports are not cleared (this is the root cause for Bug 2).       */

void cy_dissolve_group(cy_pipeline *pipe, cy_group *grp, cy_context *ctx)
{
    uint32_t i, j;

    if (!grp || (grp->flags & CY_GROUP_DISSOLVED))
        return;

    grp->flags |= CY_GROUP_DISSOLVED;

    /* For each member node, free its ports (they are subgraph-local
       allocations). This invalidates any cached pointers to these
       ports in the route table, port cache, or edge structs. */
    for (i = 0; i < grp->member_count; i++) {
        cy_node *member = grp->members[i];
        if (!member)
            continue;
        if (member->flags & CY_NODE_PRUNED)
            continue;

        /* Invalidate edges touching this node */
        for (j = 0; j < pipe->edge_count; j++) {
            cy_edge *e = &pipe->edges[j];
            if (e->src_port &&
                e->src_port->node_id == member->id) {
                e->flags &= ~CY_EDGE_VALID;
            }
            if (e->dst_port &&
                e->dst_port->node_id == member->id) {
                e->flags &= ~CY_EDGE_VALID;
            }
        }

        /* Free the node's port allocation */
        if (member->ports) {
            for (j = 0; j < member->port_count; j++) {
                cy_free(member->ports[j].connected_edges);
                member->ports[j].connected_edges = NULL;
            }
            cy_free(member->ports);
            member->ports = NULL;
        }

        member->flags |= CY_NODE_PRUNED;
    }

    cy_add_warning(ctx,
                   "dissolved group %u (%s) with %u members",
                   grp->id,
                   grp->name ? grp->name : "unnamed",
                   grp->member_count);
}

/* ── Group membership overlap check ─────────────────────────────── */

static int cy_check_membership_overlap(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t gi, gj, mi, mj;

    for (gi = 0; gi < pipe->group_count; gi++) {
        cy_group *ga = &pipe->groups[gi];
        if (!ga->members)
            continue;

        for (gj = gi + 1; gj < pipe->group_count; gj++) {
            cy_group *gb = &pipe->groups[gj];
            if (!gb->members)
                continue;

            for (mi = 0; mi < ga->member_count; mi++) {
                if (!ga->members[mi])
                    continue;
                for (mj = 0; mj < gb->member_count; mj++) {
                    if (!gb->members[mj])
                        continue;
                    if (ga->members[mi]->id == gb->members[mj]->id) {
                        cy_add_warning(ctx,
                                       "node %u is member of both "
                                       "group %u and group %u",
                                       ga->members[mi]->id, gi, gj);
                    }
                }
            }
        }
    }

    return CY_OK;
}

/* ── Validate port map references ───────────────────────────────── */

static int cy_validate_port_map_membership(cy_pipeline *pipe,
                                           cy_context *ctx)
{
    uint32_t gi, pi, mi;

    for (gi = 0; gi < pipe->group_count; gi++) {
        cy_group *grp = &pipe->groups[gi];
        if (!grp->port_map)
            continue;

        for (pi = 0; pi < grp->port_map_count; pi++) {
            cy_port_map_entry *pme = &grp->port_map[pi];
            int found = 0;

            if (!pme->internal_port)
                continue;

            /* Check that the port belongs to a member node */
            for (mi = 0; mi < grp->member_count; mi++) {
                if (!grp->members[mi])
                    continue;
                if (pme->internal_port->node_id ==
                    grp->members[mi]->id) {
                    found = 1;
                    break;
                }
            }

            if (!found) {
                cy_add_warning(ctx,
                               "group %u port_map %u: internal port %u "
                               "belongs to node %u which is not a member",
                               gi, pi, pme->internal_port_id,
                               pme->internal_port->node_id);
            }
        }
    }

    return CY_OK;
}

/* ── Main group table parser ────────────────────────────────────── */

int cy_parse_groups(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir)
{
    uint32_t count = dir->count;
    uint32_t i;
    int      rc;
    if (count == 0) {
        pipe->group_count = 0;
        pipe->groups = NULL;
        return CY_OK;
    }

    if (count > CY_MAX_GROUPS) {
        cy_set_error(r->ctx, CY_ERR_BAD_COUNT,
                     "group count %u exceeds limit %u",
                     count, CY_MAX_GROUPS);
        return CY_ERR_BAD_COUNT;
    }

    rc = cy_validate_record_count(count, CY_GROUP_REC_SIZE, dir->length,
                                  r->ctx, "group table");
    if (rc != CY_OK)
        return rc;

    pipe->groups = (cy_group *)cy_calloc(r->ctx, count, sizeof(cy_group));
    if (!pipe->groups)
        return CY_ERR_NOMEM;

    /* Read group header records */
    rc = cy_reader_seek(r, dir->offset);
    if (rc != CY_OK)
        return rc;

    for (i = 0; i < count; i++) {
        cy_group *grp = &pipe->groups[i];
        uint32_t member_offset, member_count;
        uint32_t map_offset, map_count;

        grp->id        = i;
        grp->name_id   = cy_read_u32(r);
        member_offset  = cy_read_u32(r);
        member_count   = cy_read_u16(r);
        map_count      = cy_read_u16(r);
        map_offset     = cy_read_u32(r);
        grp->flags     = cy_read_u32(r);

        if (r->ctx->error_code != CY_OK)
            return r->ctx->error_code;

        /* Resolve name */
        grp->name = cy_resolve_string(pipe, grp->name_id);

        /* Validate flags */
        cy_validate_group_flags(grp->flags, i, r->ctx);

        /* Convert relative offsets to absolute */
        if (cy_safe_add(dir->offset, member_offset,
                        &member_offset) != 0) {
            cy_add_warning(r->ctx,
                           "group %u: member offset overflow", i);
            member_count = 0;
        }

        if (cy_safe_add(dir->offset, map_offset,
                        &map_offset) != 0) {
            cy_add_warning(r->ctx,
                           "group %u: port map offset overflow", i);
            map_count = 0;
        }

        /* Save reader position and parse member list */
        {
            size_t saved_pos = cy_reader_pos(r);

            rc = cy_parse_member_list(r, pipe, grp,
                                      member_offset, member_count);
            if (rc != CY_OK)
                return rc;

            rc = cy_parse_port_map(r, pipe, grp, map_offset, map_count);
            if (rc != CY_OK)
                return rc;

            /* Restore position for next group record */
            cy_reader_seek(r, saved_pos);
        }
    }

    pipe->group_count = count;

    /* Validate membership */
    cy_check_membership_overlap(pipe, r->ctx);
    cy_validate_port_map_membership(pipe, r->ctx);

    return CY_OK;
}

/* ── Group boundary edge analysis ───────────────────────────────── */

int cy_group_boundary_edges(const cy_pipeline *pipe, uint32_t group_id,
                            uint32_t *incoming, uint32_t *outgoing,
                            uint32_t *internal)
{
    uint32_t i, mi;
    uint32_t in_count = 0, out_count = 0, int_count = 0;

    if (!pipe || !incoming || !outgoing || !internal)
        return -1;
    if (group_id >= pipe->group_count)
        return -1;

    const cy_group *grp = &pipe->groups[group_id];
    if (!grp->members)
        return 0;

    for (i = 0; i < pipe->edge_count; i++) {
        const cy_edge *e = &pipe->edges[i];
        if (!(e->flags & CY_EDGE_VALID))
            continue;
        if (!e->src_port || !e->dst_port)
            continue;

        int src_in_group = 0;
        int dst_in_group = 0;

        for (mi = 0; mi < grp->member_count; mi++) {
            if (!grp->members[mi])
                continue;
            if (e->src_port->node_id == grp->members[mi]->id)
                src_in_group = 1;
            if (e->dst_port->node_id == grp->members[mi]->id)
                dst_in_group = 1;
        }

        if (src_in_group && dst_in_group)
            int_count++;
        else if (src_in_group && !dst_in_group)
            out_count++;
        else if (!src_in_group && dst_in_group)
            in_count++;
    }

    *incoming = in_count;
    *outgoing = out_count;
    *internal = int_count;
    return 0;
}
