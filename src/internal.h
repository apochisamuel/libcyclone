/*
 * internal.h — Internal declarations for libcyclone
 *
 * Contains structure definitions, internal constants, and function
 * prototypes used across compilation units.
 */

#ifndef CYCLONE_INTERNAL_H
#define CYCLONE_INTERNAL_H

#include "cyclone.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── On-disk record sizes ───────────────────────────────────────── */

#define CY_FILE_HEADER_SIZE    32u
#define CY_TABLE_DIR_ENTRY_SIZE 16u
#define CY_STRING_REC_SIZE      8u   /* offset(4) + length(2) + flags(2) */
#define CY_TYPE_REC_SIZE       24u
#define CY_NODE_REC_SIZE       28u
#define CY_PORT_REC_SIZE       20u
#define CY_EDGE_REC_SIZE       20u
#define CY_PARAM_REC_SIZE      36u
#define CY_SCHED_REC_SIZE      24u
#define CY_GROUP_REC_SIZE      20u
#define CY_MEMBER_REC_SIZE      4u
#define CY_PORTMAP_REC_SIZE     8u

/* ── Internal limits ────────────────────────────────────────────── */

#define CY_PORT_CACHE_BUCKETS  512u
#define CY_MAX_WARNINGS        64u
#define CY_ERROR_MSG_SIZE      256u
#define CY_PRUNE_THRESHOLD     0.5
#define CY_MAX_MERGE_SCAN      256u

/* ── Validation thresholds ──────────────────────────────────────── */

#define CY_LATENCY_BUDGET_DEFAULT  100000u
#define CY_MIN_CAPACITY            1u
#define CY_MAX_ARITY               128u
#define CY_GROUP_CYCLE_LIMIT       32u

/* ── Reader ─────────────────────────────────────────────────────── */

typedef struct cy_reader {
    const uint8_t  *data;
    size_t          size;
    size_t          pos;
    cy_context     *ctx;
} cy_reader;

void      cy_reader_init(cy_reader *r, cy_context *ctx,
                         const uint8_t *data, size_t size);
int       cy_reader_seek(cy_reader *r, size_t offset);
int       cy_reader_available(const cy_reader *r, size_t need);
uint8_t   cy_read_u8(cy_reader *r);
uint16_t  cy_read_u16(cy_reader *r);
uint32_t  cy_read_u32(cy_reader *r);
int64_t   cy_read_i64(cy_reader *r);
double    cy_read_f64(cy_reader *r);
int       cy_read_bytes(cy_reader *r, void *dst, size_t len);
uint32_t  cy_peek_u32(cy_reader *r);
size_t    cy_reader_pos(const cy_reader *r);
size_t    cy_reader_remaining(const cy_reader *r);

/* ── Context ────────────────────────────────────────────────────── */

struct cy_context {
    int       error_code;
    char      error_msg[CY_ERROR_MSG_SIZE];
    int       warning_count;
    char      warnings[CY_MAX_WARNINGS][CY_ERROR_MSG_SIZE];
    uint32_t  flags;
};

void  cy_set_error(cy_context *ctx, int code, const char *fmt, ...);
void  cy_add_warning(cy_context *ctx, const char *fmt, ...);
void  cy_clear_error(cy_context *ctx);

/* ── Memory helpers ─────────────────────────────────────────────── */

void *cy_alloc(cy_context *ctx, size_t size);
void *cy_calloc(cy_context *ctx, size_t count, size_t elem_size);
void *cy_realloc(cy_context *ctx, void *ptr, size_t new_size);
void  cy_free(void *ptr);

/* ── String table entry (on-disk reference, resolved to pointer) ─ */

typedef struct cy_string_entry {
    uint32_t  offset;
    uint16_t  length;
    uint16_t  flags;
} cy_string_entry;

/* ── Type descriptor ────────────────────────────────────────────── */

struct cy_type_desc {
    uint32_t       id;
    uint32_t       name_id;
    const char    *name;         /* resolved pointer into string table */
    uint8_t        kind;
    uint32_t       size;
    uint16_t       flags;
    uint32_t       element_count;
    uint32_t       base_type_id;
    cy_type_desc  *base_type;    /* resolved pointer into types array */
};

/* ── Port ───────────────────────────────────────────────────────── */

struct cy_port {
    uint32_t       id;
    uint32_t       name_id;
    const char    *name;         /* resolved pointer into string table */
    uint32_t       node_id;
    uint32_t       type_id;
    cy_type_desc  *type;         /* resolved pointer into types array */
    uint8_t        direction;
    uint16_t       flags;
    uint16_t       capacity;
    cy_edge      **connected_edges;
    uint32_t       connected_count;
    uint32_t       connected_cap;
};

/* ── Node ───────────────────────────────────────────────────────── */

struct cy_node {
    uint32_t       id;
    uint32_t       name_id;
    const char    *name;         /* resolved pointer into string table */
    uint32_t       type_id;
    cy_type_desc  *type;         /* resolved pointer into types array */
    cy_port       *ports;        /* owned allocation for this node */
    uint32_t       port_count;
    uint32_t       port_start;   /* first port id in global index */
    uint32_t       param_start;
    uint32_t       param_count;
    uint32_t       flags;
    uint16_t       max_port_capacity;
    uint16_t       group_id;
};

/* ── Edge ───────────────────────────────────────────────────────── */

struct cy_edge {
    uint32_t       id;
    uint32_t       src_port_id;
    uint32_t       dst_port_id;
    cy_port       *src_port;     /* resolved pointer into node's port array */
    cy_port       *dst_port;     /* resolved pointer into node's port array */
    cy_type_desc  *src_type;     /* cached type descriptor for source port */
    cy_type_desc  *dst_type;     /* cached type descriptor for dest port */
    uint32_t       flags;
    uint16_t       weight;
    uint32_t       buffer_size;
};

/* ── Parameter ──────────────────────────────────────────────────── */

struct cy_param {
    uint32_t    id;
    uint32_t    name_id;
    const char *name;            /* resolved pointer into string table */
    uint32_t    node_id;
    uint8_t     kind;
    uint16_t    flags;
    union {
        int64_t      i;
        double       f;
        const char  *s;          /* resolved string pointer */
    } value;
    union {
        int64_t      i;
        double       f;
    } min_val;
    union {
        int64_t      i;
        double       f;
    } max_val;
};

/* ── Schedule entry ─────────────────────────────────────────────── */

struct cy_sched_entry {
    uint32_t    id;
    uint32_t    node_id;
    cy_node    *node;            /* resolved pointer into nodes array */
    uint32_t    order;
    uint32_t    latency_min;
    uint32_t    latency_max;
    uint32_t    buffer_alloc;
    uint16_t    flags;
    uint32_t    constraint_sink_id;
    cy_node    *constraint_sink; /* resolved pointer into nodes array */
};

/* ── Port map entry (maps group boundary to internal port) ─────── */

typedef struct cy_port_map_entry {
    uint32_t    external_name_id;
    const char *external_name;   /* resolved pointer into string table */
    uint32_t    internal_port_id;
    cy_port    *internal_port;   /* resolved pointer into node port array */
} cy_port_map_entry;

/* ── Group ──────────────────────────────────────────────────────── */

struct cy_group {
    uint32_t            id;
    uint32_t            name_id;
    const char         *name;           /* resolved pointer into string table */
    cy_node           **members;        /* array of node pointers */
    uint32_t            member_count;
    cy_port_map_entry  *port_map;       /* group boundary port mappings */
    uint32_t            port_map_count;
    uint32_t            flags;
};

/* ── Port cache (hash table for port lookup by node+name) ──────── */

typedef struct cy_port_cache_entry {
    uint32_t                     node_id;
    const char                  *name;   /* pointer into string table (key) */
    cy_port                     *port;   /* resolved port pointer */
    struct cy_port_cache_entry  *next;
} cy_port_cache_entry;

typedef struct cy_port_cache {
    cy_port_cache_entry  *buckets[CY_PORT_CACHE_BUCKETS];
    cy_port_cache_entry  *entries;       /* flat array backing store */
    uint32_t              entry_count;
    uint32_t              entry_cap;
} cy_port_cache;

/* ── Route table (edge routing indexed by port) ─────────────────── */

typedef struct cy_route_entry {
    cy_port                  *src_port;
    cy_port                  *dst_port;
    cy_edge                  *edge;
    struct cy_route_entry    *next_by_src;
    struct cy_route_entry    *next_by_dst;
} cy_route_entry;

typedef struct cy_route_table {
    cy_route_entry  **by_src;           /* indexed by port id */
    cy_route_entry  **by_dst;           /* indexed by port id */
    uint32_t          index_size;       /* number of port slots */
    cy_route_entry   *entries;          /* flat array of all entries */
    uint32_t          entry_count;
} cy_route_table;

/* ── Pipeline (top-level parsed result) ─────────────────────────── */

struct cy_pipeline {
    /* Primary parsed tables */
    char              **strings;
    uint32_t            string_count;
    char               *string_data;     /* backing store for all strings */

    cy_type_desc       *types;
    uint32_t            type_count;

    cy_node            *nodes;
    uint32_t            node_count;

    cy_port           **port_index;      /* global port id → cy_port* */
    uint32_t            port_count;

    cy_edge            *edges;
    uint32_t            edge_count;
    uint32_t            edge_cap;

    cy_param           *params;
    uint32_t            param_count;

    cy_sched_entry     *schedule;
    uint32_t            schedule_count;

    cy_group           *groups;
    uint32_t            group_count;

    /* Derived caches */
    cy_port_cache      *port_cache;
    cy_route_table     *route_table;

    /* Metadata */
    uint32_t            version;
    uint32_t            flags;
    uint32_t            checksum;
    uint32_t            latency_budget;
};

/* ── Table directory entry (parsed from file header) ────────────── */

typedef struct cy_table_dir {
    uint32_t  tag;
    uint32_t  offset;
    uint32_t  length;
    uint32_t  count;
} cy_table_dir;

/* ── Phase function prototypes ──────────────────────────────────── */

/* Phase 1: Header parsing */
int  cy_parse_header(cy_reader *r, cy_pipeline *pipe,
                     cy_table_dir *dirs, uint32_t *dir_count);

/* Phase 2: String table */
int  cy_parse_strings(cy_reader *r, cy_pipeline *pipe,
                      const cy_table_dir *dir);

/* Phase 3: Type descriptors */
int  cy_parse_types(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir);

/* Phase 4: Nodes */
int  cy_parse_nodes(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir);

/* Phase 5: Ports */
int  cy_parse_ports(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir);

/* Phase 6: Edges */
int  cy_parse_edges(cy_reader *r, cy_pipeline *pipe,
                    const cy_table_dir *dir);

/* Phase 7: Parameters */
int  cy_parse_params(cy_reader *r, cy_pipeline *pipe,
                     const cy_table_dir *dir);

/* Phase 8: Schedule */
int  cy_parse_schedule(cy_reader *r, cy_pipeline *pipe,
                       const cy_table_dir *dir);

/* Phase 9: Groups */
int  cy_parse_groups(cy_reader *r, cy_pipeline *pipe,
                     const cy_table_dir *dir);

/* Phase 10: Port cache construction */
int  cy_build_port_cache(cy_pipeline *pipe, cy_context *ctx);

/* Phase 11: Route table construction */
int  cy_build_route_table(cy_pipeline *pipe, cy_context *ctx);

/* Phase 12: Validation (may prune/merge/dissolve) */
int  cy_validate_pipeline(cy_pipeline *pipe, cy_context *ctx);

/* Phase 13: Final cross-reference resolution */
int  cy_resolve_references(cy_pipeline *pipe, cy_context *ctx);

/* ── Validation sub-functions (phase 12) ────────────────────────── */

int  cy_validate_type_compat(cy_pipeline *pipe, cy_context *ctx);
int  cy_prune_disconnected_nodes(cy_pipeline *pipe, cy_context *ctx);
int  cy_validate_groups(cy_pipeline *pipe, cy_context *ctx);
int  cy_validate_port_capacity(cy_pipeline *pipe, cy_context *ctx);
int  cy_validate_latency(cy_pipeline *pipe, cy_context *ctx);
int  cy_validate_params(cy_pipeline *pipe, cy_context *ctx);
int  cy_validate_schedule(cy_pipeline *pipe, cy_context *ctx);

/* ── Validation helpers ─────────────────────────────────────────── */

int  cy_types_compatible(const cy_type_desc *src, const cy_type_desc *dst);
int  cy_detect_group_cycle(cy_pipeline *pipe, cy_group *grp,
                           cy_context *ctx);
void cy_dissolve_group(cy_pipeline *pipe, cy_group *grp, cy_context *ctx);
void cy_prune_node(cy_pipeline *pipe, cy_node *node, cy_context *ctx);
int  cy_merge_redundant_nodes(cy_pipeline *pipe, cy_context *ctx);
int  cy_fold_equivalent_types(cy_pipeline *pipe, cy_context *ctx);
int  cy_normalize_param_names(cy_pipeline *pipe, cy_context *ctx);
void cy_prune_latency_chain(cy_pipeline *pipe, cy_node *node,
                            cy_context *ctx);
int  cy_split_overloaded_node(cy_pipeline *pipe, cy_node *node,
                              cy_context *ctx);
int  cy_add_edge(cy_pipeline *pipe, cy_context *ctx,
                 uint32_t src_port_id, uint32_t dst_port_id,
                 uint32_t flags, uint16_t weight, uint32_t buf_size);

/* ── Resolve sub-functions (phase 13) ───────────────────────────── */

int  cy_resolve_param_bindings(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_type_constraints(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_schedule_order(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_group_interfaces(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_edge_weights(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_latency_bounds(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_cross_group_refs(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_port_capacities(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_edge_buffer_layout(cy_pipeline *pipe, cy_context *ctx);
int  cy_resolve_schedule_barriers(cy_pipeline *pipe, cy_context *ctx);

/* ── Port cache operations ──────────────────────────────────────── */

cy_port_cache  *cy_port_cache_create(cy_context *ctx, uint32_t capacity);
void            cy_port_cache_destroy(cy_port_cache *cache);
int             cy_port_cache_insert(cy_port_cache *cache, uint32_t node_id,
                                     const char *name, cy_port *port);
cy_port        *cy_port_cache_lookup(const cy_port_cache *cache,
                                     uint32_t node_id, const char *name);
uint32_t        cy_port_cache_hash(uint32_t node_id, const char *name);

/* ── Route table operations ─────────────────────────────────────── */

cy_route_table *cy_route_table_create(cy_context *ctx, uint32_t port_count,
                                      uint32_t edge_count);
void            cy_route_table_destroy(cy_route_table *rt);
int             cy_route_table_add(cy_route_table *rt, cy_port *src,
                                   cy_port *dst, cy_edge *edge);
cy_route_entry *cy_route_by_src(const cy_route_table *rt, uint32_t port_id);
cy_route_entry *cy_route_by_dst(const cy_route_table *rt, uint32_t port_id);

/* ── Utility ────────────────────────────────────────────────────── */

const char *cy_resolve_string(const cy_pipeline *pipe, uint32_t string_id);
uint32_t    cy_hash_string(const char *s);
int         cy_safe_multiply(uint32_t a, uint32_t b, uint32_t *result);
int         cy_safe_add(uint32_t a, uint32_t b, uint32_t *result);
int         cy_validate_range(cy_reader *r, uint32_t offset,
                              uint32_t length, const char *table_name);
int         cy_validate_record_count(uint32_t count, uint32_t rec_size,
                                     uint32_t section_length,
                                     cy_context *ctx,
                                     const char *table_name);
char       *cy_read_string_at(cy_reader *r, cy_context *ctx,
                              uint32_t offset, uint16_t length);
int         cy_reader_align(cy_reader *r, size_t alignment);

/* ── Format helpers ─────────────────────────────────────────────── */

int  cy_format_type_kind(uint8_t kind, char *buf, size_t size);
int  cy_format_port_direction(uint8_t dir, char *buf, size_t size);
int  cy_format_edge_info(const cy_edge *e, const cy_pipeline *pipe,
                         char *buf, size_t size);
int  cy_format_node_ports(const cy_node *node, char *buf, size_t size);
int  cy_format_group_members(const cy_group *grp, char *buf, size_t size);
int  cy_format_schedule_info(const cy_sched_entry *se, char *buf,
                             size_t size);
int  cy_format_param_value(const cy_param *p, char *buf, size_t size);

#endif /* CYCLONE_INTERNAL_H */
