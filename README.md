# libcyclone

A C library for parsing Cyclone (.cyc) binary pipeline descriptor files.

## Overview

libcyclone parses the CYC binary format, which describes dataflow graphs of
signal processing nodes connected by typed edges. The format supports:

- **Nodes** — Processing stages with typed I/O ports
- **Edges** — Directed connections between ports with type compatibility checks
- **Types** — Data type descriptors for port typing and edge validation
- **Parameters** — Per-node configuration with range constraints
- **Schedule** — Execution ordering with latency constraints
- **Groups** — Hierarchical subgraph organization with boundary port maps

## Building

```bash
# Standard build
bash build.sh

# With custom compiler/flags
CC=clang CFLAGS="-fsanitize=address -g" bash build.sh

# Using make
make
make fuzzers LIB_FUZZING_ENGINE="-fsanitize=fuzzer"
```

## Usage

```c
#include "cyclone.h"

cy_context *ctx = cy_context_create();
cy_pipeline *pipe = cy_parse(ctx, data, size);

if (!pipe) {
    fprintf(stderr, "Parse error: %s\n", cy_context_error_msg(ctx));
    cy_context_destroy(ctx);
    return 1;
}

uint32_t nc = cy_pipeline_node_count(pipe);
for (uint32_t i = 0; i < nc; i++) {
    const cy_node *n = cy_get_node(pipe, i);
    printf("Node %u: %s\n", cy_node_id(n), cy_node_name(n));
}

cy_pipeline_destroy(pipe);
cy_context_destroy(ctx);
```

## File Format

A CYC file consists of a 32-byte header, a table directory, and data sections:

| Offset | Size | Field         |
|--------|------|---------------|
| 0      | 4    | Magic "CYC\0" |
| 4      | 4    | Version       |
| 8      | 4    | Flags         |
| 12     | 4    | Table count   |
| 16     | 4    | Total size    |
| 20     | 4    | Checksum      |
| 24     | 8    | Reserved      |

The table directory follows the header with 16-byte entries (tag, offset,
length, count) for each data section. Required sections are STR (strings),
TYP (types), NOD (nodes), PRT (ports), and EDG (edges). Optional sections
include PRM (parameters), SCH (schedule), and GRP (groups).

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for details on the multi-phase
parsing pipeline and cross-table reference resolution.

## License

MIT License. See [LICENSE](LICENSE).
