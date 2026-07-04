# libcyclone Architecture

## Parse Pipeline

libcyclone processes CYC files through 13 sequential phases:

### Input Phases (1-9): Parse on-disk tables

1. **header.c** — File header: magic, version, flags, table directory
2. **strings.c** — String table: interned names, UTF-8 validation
3. **types.c** — Type descriptors: kind, size, base type hierarchy
4. **nodes.c** — Node table: processing stages with type/port refs
5. **ports.c** — Port table: typed I/O per node, per-node allocation
6. **edges.c** — Edge table: port connections, type caching, back-pointers
7. **params.c** — Parameter table: per-node config, range validation
8. **schedule.c** — Schedule table: execution order, latency constraints
9. **groups.c** — Group table: hierarchical subgraphs, port maps

### Cache Phases (10-11): Build derived data structures

10. **cache.c** — Port lookup cache: hash table (node_id + name → port*)
11. **routing.c** — Edge routing: linked lists indexed by port ID

### Validation Phase (12): Cross-table consistency

12. **validate.c** — Orchestrates:
    - Type folding (merge identical type descriptors)
    - Type compatibility checking (mark invalid edges)
    - Disconnected node pruning (cascade from invalid edges)
    - Group cycle detection and dissolution
    - Port capacity validation and node splitting
    - Latency budget enforcement and chain pruning
    - Parameter name normalization
    - Redundant node merging

### Resolution Phase (13): Final cross-reference binding

13. **resolve.c** — Resolves:
    - Parameter bindings (port cache lookup)
    - Type constraints (edge type cache)
    - Schedule order (topological sort)
    - Group interfaces (port map resolution)
    - Edge weights (port back-pointer traversal)
    - Latency bounds (constraint sink resolution)
    - Cross-group references (route table traversal)

## Memory Ownership

- **Nodes** own their port arrays (`node->ports = malloc(...)`)
- **Pipeline** owns all other arrays (types, edges, params, etc.)
- **Port cache** and **route table** store alias pointers (not copies)
- **Edges** cache type descriptor pointers from connected ports
- **Schedule entries** store node pointers
- **Groups** store node pointer arrays and port map entries

## Key Data Flow

```
String Table ←── names ──→ Nodes, Ports, Types, Params, Groups
Type Table   ←── types ──→ Ports, Edges (cached)
Node Array   ←── nodes ──→ Schedule, Groups
Port Arrays  ←── ports ──→ Port Cache, Route Table, Edge endpoints
Edge Array   ←── edges ──→ Port back-pointers, Route Table
```

## Support Files

- **context.c** — Error reporting, memory helpers, parse orchestrator
- **reader.c** — Bounds-checked binary reading primitives
- **query.c** — Public read-only accessor functions
- **format.c** — Human-readable formatting for debug output
