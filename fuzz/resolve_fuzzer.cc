/*
 * resolve_fuzzer.cc — Validation and resolution fuzzer
 *
 * Exercises the full parsing pipeline with emphasis on the
 * validation (phase 12) and resolution (phase 13) phases.
 * This is where cross-table pointer invalidation bugs manifest.
 * After parsing, aggressively queries all resolved data.
 */

#include <cstdint>
#include <cstddef>

extern "C" {
#include "cyclone.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cy_context *ctx = cy_context_create();
    if (!ctx)
        return 0;

    cy_pipeline *pipe = cy_parse(ctx, data, size);
    if (pipe) {
        uint32_t nc = cy_pipeline_node_count(pipe);
        uint32_t ec = cy_pipeline_edge_count(pipe);
        uint32_t tc = cy_pipeline_type_count(pipe);
        uint32_t gc = cy_pipeline_group_count(pipe);
        uint32_t pc = cy_pipeline_param_count(pipe);
        uint32_t sc = cy_schedule_count(pipe);

        /* Deep node inspection */
        char buf[4096];
        for (uint32_t i = 0; i < nc && i < 32; i++) {
            const cy_node *n = cy_get_node(pipe, i);
            if (!n)
                continue;

            cy_format_node_detail(n, buf, sizeof(buf));
            cy_format_node_ports(n, buf, sizeof(buf));

            /* Access node's port data */
            for (uint32_t p = 0; p < cy_node_port_count(n) && p < 16; p++) {
                const cy_port *port = cy_get_port(pipe, p);
                if (port) {
                    const cy_type_desc *pt = cy_port_type(port);
                    if (pt) {
                        cy_format_type_detail(pt, buf, sizeof(buf));
                    }
                }
            }

            /* Find node in schedule */
            uint32_t order, lat_min, lat_max;
            cy_get_node_schedule(pipe, i, &order, &lat_min, &lat_max);

            /* Find group membership */
            (void)cy_find_group_for_node(pipe, i);
        }

        /* Param inspection */
        for (uint32_t i = 0; i < pc && i < 32; i++) {
            const cy_param *p = cy_get_param(pipe, i);
            if (p) {
                (void)cy_param_name(p);
                (void)cy_param_node_id(p);
                (void)cy_param_kind(p);
                cy_format_param_value(p, buf, sizeof(buf));
            }
        }

        /* Schedule inspection */
        for (uint32_t i = 0; i < sc && i < 32; i++) {
            const cy_sched_entry *se = cy_get_schedule_entry(pipe, i);
            if (se) {
                (void)cy_sched_node_id(se);
                (void)cy_sched_order(se);
                cy_format_schedule_info(se, buf, sizeof(buf));
            }
        }

        /* Group inspection */
        for (uint32_t i = 0; i < gc && i < 16; i++) {
            const cy_group *g = cy_get_group(pipe, i);
            if (g) {
                (void)cy_group_name(g);
                (void)cy_group_member_count(g);
                (void)cy_group_flags(g);
                cy_format_group_members(g, buf, sizeof(buf));
            }
        }

        /* Type inspection */
        for (uint32_t i = 0; i < tc && i < 32; i++) {
            const cy_type_desc *t = cy_get_type(pipe, i);
            if (t)
                cy_format_type_detail(t, buf, sizeof(buf));
        }

        /* Full summary */
        cy_format_pipeline_summary(pipe, buf, sizeof(buf));

        /* Port lookup by name (exercises port cache) */
        for (uint32_t i = 0; i < nc && i < 8; i++) {
            const cy_node *n = cy_get_node(pipe, i);
            if (n && cy_node_name(n)) {
                (void)cy_find_port(pipe, i, cy_node_name(n));
            }
        }

        cy_pipeline_destroy(pipe);
    }

    cy_context_destroy(ctx);
    return 0;
}
