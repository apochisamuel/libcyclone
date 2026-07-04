/*
 * edge_fuzzer.cc — Edge parsing and type caching fuzzer
 *
 * Exercises the edge parsing path with emphasis on type caching,
 * back-pointer registration, and edge addition during validation.
 * After parsing, exercises the query API to access edge data.
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
        uint32_t ec = cy_pipeline_edge_count(pipe);
        uint32_t nc = cy_pipeline_node_count(pipe);

        /* Iterate edges and read type/port data */
        for (uint32_t i = 0; i < ec && i < 64; i++) {
            const cy_edge *e = cy_get_edge(pipe, i);
            if (!e)
                continue;

            uint32_t src = cy_edge_src_port_id(e);
            uint32_t dst = cy_edge_dst_port_id(e);
            uint32_t flags = cy_edge_flags(e);
            uint32_t buf = cy_edge_buffer_size(e);

            (void)src;
            (void)dst;
            (void)flags;
            (void)buf;

            /* Access source and dest ports through pipeline */
            const cy_port *sp = cy_get_port(pipe, src);
            const cy_port *dp = cy_get_port(pipe, dst);

            if (sp) {
                (void)cy_port_name(sp);
                (void)cy_port_direction(sp);
                const cy_type_desc *st = cy_port_type(sp);
                if (st) {
                    (void)cy_type_name(st);
                    (void)cy_type_size(st);
                }
            }
            if (dp) {
                (void)cy_port_name(dp);
                (void)cy_port_direction(dp);
            }
        }

        /* Exercise cross-node edge counting */
        for (uint32_t i = 0; i < nc && i < 16; i++) {
            (void)cy_count_edges_for_node(pipe, i);
        }

        /* Exercise edge lookup */
        if (ec > 0) {
            const cy_edge *first = cy_get_edge(pipe, 0);
            if (first) {
                (void)cy_find_edge_between(pipe,
                    cy_edge_src_port_id(first),
                    cy_edge_dst_port_id(first));
            }
        }

        char buf[2048];
        for (uint32_t i = 0; i < ec && i < 8; i++) {
            const cy_edge *e = cy_get_edge(pipe, i);
            if (e) {
                cy_format_edge_detail(e, buf, sizeof(buf));
            }
        }

        cy_pipeline_destroy(pipe);
    }

    cy_context_destroy(ctx);
    return 0;
}
