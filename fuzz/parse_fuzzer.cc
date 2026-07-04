/*
 * parse_fuzzer.cc — General CYC file parse fuzzer
 *
 * Feeds arbitrary byte sequences to cy_parse() to exercise the
 * full parsing pipeline including header, table directory, and
 * all data section parsers.
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
        /* Exercise query API on successfully parsed pipelines */
        uint32_t nc = cy_pipeline_node_count(pipe);
        uint32_t ec = cy_pipeline_edge_count(pipe);
        uint32_t tc = cy_pipeline_type_count(pipe);

        for (uint32_t i = 0; i < nc && i < 32; i++) {
            const cy_node *n = cy_get_node(pipe, i);
            if (n) {
                (void)cy_node_name(n);
                (void)cy_node_port_count(n);
                (void)cy_node_flags(n);
            }
        }

        for (uint32_t i = 0; i < ec && i < 32; i++) {
            const cy_edge *e = cy_get_edge(pipe, i);
            if (e) {
                (void)cy_edge_flags(e);
                (void)cy_edge_buffer_size(e);
            }
        }

        for (uint32_t i = 0; i < tc && i < 32; i++) {
            const cy_type_desc *t = cy_get_type(pipe, i);
            if (t) {
                (void)cy_type_name(t);
                (void)cy_type_kind(t);
            }
        }

        char buf[4096];
        cy_format_pipeline_summary(pipe, buf, sizeof(buf));

        cy_pipeline_destroy(pipe);
    }

    cy_context_destroy(ctx);
    return 0;
}
