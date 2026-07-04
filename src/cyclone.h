/*
 * cyclone.h — Public API for libcyclone
 *
 * libcyclone parses Cyclone (.cyc) binary pipeline descriptor files.
 * A CYC file describes a dataflow graph of signal processing nodes
 * connected by typed edges, with parameters, scheduling metadata,
 * and hierarchical sub-graphs.
 *
 * Reference: Cyclone Pipeline Descriptor Format Specification v1.0
 */

#ifndef CYCLONE_H
#define CYCLONE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ───────────────────────────────────────────────── */

typedef struct cy_context      cy_context;
typedef struct cy_pipeline     cy_pipeline;
typedef struct cy_node         cy_node;
typedef struct cy_port         cy_port;
typedef struct cy_edge         cy_edge;
typedef struct cy_type_desc    cy_type_desc;
typedef struct cy_param        cy_param;
typedef struct cy_sched_entry  cy_sched_entry;
typedef struct cy_group        cy_group;

/* ── Error codes ────────────────────────────────────────────────── */

#define CY_OK                   0
#define CY_ERR_NOMEM            1
#define CY_ERR_INVALID_MAGIC    2
#define CY_ERR_INVALID_VERSION  3
#define CY_ERR_TRUNCATED        4
#define CY_ERR_BAD_OFFSET       5
#define CY_ERR_BAD_COUNT        6
#define CY_ERR_BAD_TAG          7
#define CY_ERR_DUPLICATE_TABLE  8
#define CY_ERR_MISSING_TABLE    9
#define CY_ERR_BAD_STRING       10
#define CY_ERR_BAD_TYPE         11
#define CY_ERR_BAD_NODE         12
#define CY_ERR_BAD_PORT         13
#define CY_ERR_BAD_EDGE         14
#define CY_ERR_BAD_PARAM        15
#define CY_ERR_BAD_SCHEDULE     16
#define CY_ERR_BAD_GROUP        17
#define CY_ERR_CYCLE_DETECTED   18
#define CY_ERR_TYPE_MISMATCH    19
#define CY_ERR_OVERFLOW         20
#define CY_ERR_CHECKSUM         21
#define CY_ERR_BAD_CAPACITY     22
#define CY_ERR_LATENCY          23
#define CY_ERR_INTERNAL         24

/* ── Format constants ───────────────────────────────────────────── */

#define CY_MAGIC              0x00435943u  /* "CYC\0" little-endian */
#define CY_FORMAT_VERSION     1u
#define CY_MAX_TABLES         16u
#define CY_MAX_STRING_LEN     4096u
#define CY_MAX_NODES          65536u
#define CY_MAX_PORTS          262144u
#define CY_MAX_EDGES          524288u
#define CY_MAX_TYPES          4096u
#define CY_MAX_PARAMS         131072u
#define CY_MAX_SCHEDULE       65536u
#define CY_MAX_GROUPS         4096u
#define CY_MAX_GROUP_MEMBERS  1024u
#define CY_MAX_PORT_MAP       8192u
#define CY_MAX_FILE_SIZE      (16u * 1024u * 1024u)

/* Port directions */
#define CY_PORT_INPUT         0
#define CY_PORT_OUTPUT        1
#define CY_PORT_BIDI          2

/* Type kinds */
#define CY_TYPE_INT           0
#define CY_TYPE_FLOAT         1
#define CY_TYPE_DOUBLE        2
#define CY_TYPE_BOOL          3
#define CY_TYPE_STRING        4
#define CY_TYPE_BYTES         5
#define CY_TYPE_STRUCT        6
#define CY_TYPE_ARRAY         7
#define CY_TYPE_ENUM          8
#define CY_TYPE_TENSOR        9

/* Param kinds */
#define CY_PARAM_INT          0
#define CY_PARAM_FLOAT        1
#define CY_PARAM_STRING       2
#define CY_PARAM_BOOL         3
#define CY_PARAM_ENUM         4

/* Table tags (four-byte identifiers in table directory) */
#define CY_TAG_STR            0x00525453u  /* "STR\0" */
#define CY_TAG_TYP            0x00505954u  /* "TYP\0" */
#define CY_TAG_NOD            0x00444F4Eu  /* "NOD\0" */
#define CY_TAG_PRT            0x00545250u  /* "PRT\0" */
#define CY_TAG_EDG            0x00474445u  /* "EDG\0" */
#define CY_TAG_PRM            0x004D5250u  /* "PRM\0" */
#define CY_TAG_SCH            0x00484353u  /* "SCH\0" */
#define CY_TAG_GRP            0x00505247u  /* "GRP\0" */

/* Pipeline flags */
#define CY_FLAG_REALTIME      0x0001u
#define CY_FLAG_DETERMINISTIC 0x0002u
#define CY_FLAG_STREAMING     0x0004u
#define CY_FLAG_CHECKSUM      0x0008u

/* Edge flags */
#define CY_EDGE_BUFFERED      0x0001u
#define CY_EDGE_OPTIONAL      0x0002u
#define CY_EDGE_FEEDBACK      0x0004u
#define CY_EDGE_VALID         0x0010u
#define CY_EDGE_IMPLICIT      0x0020u

/* Node flags */
#define CY_NODE_SOURCE        0x0001u
#define CY_NODE_SINK          0x0002u
#define CY_NODE_STATEFUL      0x0004u
#define CY_NODE_PRUNED        0x0010u
#define CY_NODE_MERGED        0x0020u
#define CY_NODE_SPLIT         0x0040u

/* Group flags */
#define CY_GROUP_ISOLATED     0x0001u
#define CY_GROUP_REENTRANT    0x0002u
#define CY_GROUP_DISSOLVED    0x0004u

/* Schedule flags */
#define CY_SCHED_CRITICAL     0x0001u
#define CY_SCHED_OPTIONAL     0x0002u
#define CY_SCHED_BARRIER      0x0004u

/* Port flags */
#define CY_PORT_REQUIRED      0x0001u
#define CY_PORT_VARIADIC      0x0002u
#define CY_PORT_MONITOR       0x0004u

/* ── Context management ─────────────────────────────────────────── */

cy_context   *cy_context_create(void);
void          cy_context_destroy(cy_context *ctx);
int           cy_context_error(const cy_context *ctx);
const char   *cy_context_error_msg(const cy_context *ctx);
int           cy_context_warning_count(const cy_context *ctx);

/* ── Parsing ────────────────────────────────────────────────────── */

cy_pipeline  *cy_parse(cy_context *ctx, const uint8_t *data, size_t size);
void          cy_pipeline_destroy(cy_pipeline *pipe);

/* ── Pipeline queries ───────────────────────────────────────────── */

uint32_t      cy_pipeline_version(const cy_pipeline *pipe);
uint32_t      cy_pipeline_flags(const cy_pipeline *pipe);
uint32_t      cy_pipeline_node_count(const cy_pipeline *pipe);
uint32_t      cy_pipeline_edge_count(const cy_pipeline *pipe);
uint32_t      cy_pipeline_type_count(const cy_pipeline *pipe);
uint32_t      cy_pipeline_group_count(const cy_pipeline *pipe);
uint32_t      cy_pipeline_param_count(const cy_pipeline *pipe);

/* ── Node queries ───────────────────────────────────────────────── */

const cy_node       *cy_get_node(const cy_pipeline *pipe, uint32_t index);
const cy_node       *cy_find_node_by_name(const cy_pipeline *pipe,
                                          const char *name);
const char          *cy_node_name(const cy_node *node);
uint32_t             cy_node_id(const cy_node *node);
uint32_t             cy_node_port_count(const cy_node *node);
uint32_t             cy_node_flags(const cy_node *node);

/* ── Port queries ───────────────────────────────────────────────── */

const cy_port       *cy_get_port(const cy_pipeline *pipe, uint32_t index);
const cy_port       *cy_find_port(const cy_pipeline *pipe,
                                  uint32_t node_id, const char *name);
const char          *cy_port_name(const cy_port *port);
uint32_t             cy_port_id(const cy_port *port);
uint8_t              cy_port_direction(const cy_port *port);
const cy_type_desc  *cy_port_type(const cy_port *port);

/* ── Edge queries ───────────────────────────────────────────────── */

const cy_edge       *cy_get_edge(const cy_pipeline *pipe, uint32_t index);
uint32_t             cy_edge_src_port_id(const cy_edge *edge);
uint32_t             cy_edge_dst_port_id(const cy_edge *edge);
uint32_t             cy_edge_flags(const cy_edge *edge);
uint32_t             cy_edge_buffer_size(const cy_edge *edge);

/* ── Type queries ───────────────────────────────────────────────── */

const cy_type_desc  *cy_get_type(const cy_pipeline *pipe, uint32_t index);
const char          *cy_type_name(const cy_type_desc *td);
uint8_t              cy_type_kind(const cy_type_desc *td);
uint32_t             cy_type_size(const cy_type_desc *td);

/* ── Param queries ──────────────────────────────────────────────── */

const cy_param      *cy_get_param(const cy_pipeline *pipe, uint32_t index);
const char          *cy_param_name(const cy_param *p);
uint32_t             cy_param_node_id(const cy_param *p);
uint8_t              cy_param_kind(const cy_param *p);

/* ── Schedule queries ───────────────────────────────────────────── */

const cy_sched_entry *cy_get_schedule_entry(const cy_pipeline *pipe,
                                            uint32_t index);
uint32_t              cy_schedule_count(const cy_pipeline *pipe);
uint32_t              cy_sched_node_id(const cy_sched_entry *e);
uint32_t              cy_sched_order(const cy_sched_entry *e);

/* ── Group queries ──────────────────────────────────────────────── */

const cy_group      *cy_get_group(const cy_pipeline *pipe, uint32_t index);
const char          *cy_group_name(const cy_group *g);
uint32_t             cy_group_member_count(const cy_group *g);
uint32_t             cy_group_flags(const cy_group *g);

/* ── Traversal helpers ──────────────────────────────────────────── */

const cy_edge       *cy_find_edge_between(const cy_pipeline *pipe,
                                          uint32_t src_port_id,
                                          uint32_t dst_port_id);
uint32_t             cy_count_edges_for_node(const cy_pipeline *pipe,
                                             uint32_t node_id);

/* ── Formatting / debug output ──────────────────────────────────── */

int  cy_format_pipeline_summary(const cy_pipeline *pipe, char *buf,
                                size_t buf_size);
int  cy_format_node_detail(const cy_node *node, char *buf, size_t buf_size);
int  cy_format_node_ports(const cy_node *node, char *buf, size_t buf_size);
int  cy_format_edge_detail(const cy_edge *edge, char *buf, size_t buf_size);
int  cy_format_type_detail(const cy_type_desc *td, char *buf, size_t buf_size);
int  cy_format_param_value(const cy_param *p, char *buf, size_t buf_size);
int  cy_format_schedule_info(const cy_sched_entry *se, char *buf,
                             size_t buf_size);
int  cy_format_group_members(const cy_group *grp, char *buf, size_t buf_size);

/* ── Advanced queries ───────────────────────────────────────────── */

int  cy_get_node_schedule(const cy_pipeline *pipe, uint32_t node_id,
                          uint32_t *order, uint32_t *latency_min,
                          uint32_t *latency_max);
const cy_group *cy_find_group_for_node(const cy_pipeline *pipe,
                                       uint32_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* CYCLONE_H */
