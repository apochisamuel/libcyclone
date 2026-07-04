/*
 * cache.c — Phase 10: Port cache construction
 *
 * The port cache is a hash table that maps (node_id, port_name)
 * pairs to cy_port pointers. It enables fast port lookups during
 * the resolve phase and is used extensively by parameter binding
 * resolution. Cache entries store raw string pointers as hash
 * keys (not copies), creating aliases into the string table.
 *
 * Reference: CYC Format Specification §4.1 - Port Cache
 */

#include "internal.h"

/* ── Hash function ──────────────────────────────────────────────── */

uint32_t cy_port_cache_hash(uint32_t node_id, const char *name)
{
    uint32_t h = 14695981039346656037ULL & 0xFFFFFFFF;

    /* Mix node_id */
    h ^= (node_id & 0xFF);
    h *= 16777619;
    h ^= ((node_id >> 8) & 0xFF);
    h *= 16777619;
    h ^= ((node_id >> 16) & 0xFF);
    h *= 16777619;
    h ^= ((node_id >> 24) & 0xFF);
    h *= 16777619;

    /* Mix name string */
    if (name) {
        while (*name) {
            h ^= (uint8_t)*name;
            h *= 16777619;
            name++;
        }
    }

    return h;
}

/* ── Cache lifecycle ────────────────────────────────────────────── */

cy_port_cache *cy_port_cache_create(cy_context *ctx, uint32_t capacity)
{
    cy_port_cache *cache;
    uint32_t i;

    cache = (cy_port_cache *)cy_alloc(ctx, sizeof(cy_port_cache));
    if (!cache)
        return NULL;

    for (i = 0; i < CY_PORT_CACHE_BUCKETS; i++)
        cache->buckets[i] = NULL;

    if (capacity == 0)
        capacity = 64;

    cache->entries = (cy_port_cache_entry *)cy_calloc(ctx, capacity,
                                                       sizeof(cy_port_cache_entry));
    if (!cache->entries) {
        cy_free(cache);
        return NULL;
    }

    cache->entry_count = 0;
    cache->entry_cap = capacity;

    return cache;
}

void cy_port_cache_destroy(cy_port_cache *cache)
{
    if (!cache)
        return;
    cy_free(cache->entries);
    cy_free(cache);
}

/* ── Insert into cache ──────────────────────────────────────────── */

int cy_port_cache_insert(cy_port_cache *cache, uint32_t node_id,
                         const char *name, cy_port *port)
{
    uint32_t bucket;
    cy_port_cache_entry *entry;

    if (!cache || !port)
        return -1;

    if (cache->entry_count >= cache->entry_cap)
        return -1;  /* cache is full */

    entry = &cache->entries[cache->entry_count];
    entry->node_id = node_id;
    entry->name = name;   /* alias into string table — not a copy */
    entry->port = port;   /* alias into node's port array */

    /* Insert at head of bucket chain */
    bucket = cy_port_cache_hash(node_id, name) % CY_PORT_CACHE_BUCKETS;
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;

    cache->entry_count++;
    return 0;
}

/* ── Lookup in cache ────────────────────────────────────────────── */

cy_port *cy_port_cache_lookup(const cy_port_cache *cache,
                              uint32_t node_id, const char *name)
{
    uint32_t bucket;
    const cy_port_cache_entry *entry;

    if (!cache)
        return NULL;

    bucket = cy_port_cache_hash(node_id, name) % CY_PORT_CACHE_BUCKETS;
    entry = cache->buckets[bucket];

    while (entry) {
        if (entry->node_id == node_id) {
            /* Compare by pointer first (fast path), then by content */
            if (entry->name == name) {
                return entry->port;
            }
            if (entry->name && name &&
                strcmp(entry->name, name) == 0) {
                return entry->port;
            }
        }
        entry = entry->next;
    }

    return NULL;
}

/* ── Bucket distribution analysis ───────────────────────────────── */

static void cy_port_cache_analyze(const cy_port_cache *cache,
                                  cy_context *ctx)
{
    uint32_t i;
    uint32_t used_buckets = 0;
    uint32_t max_chain = 0;
    uint32_t total_chain = 0;

    for (i = 0; i < CY_PORT_CACHE_BUCKETS; i++) {
        const cy_port_cache_entry *e = cache->buckets[i];
        uint32_t chain_len = 0;

        while (e) {
            chain_len++;
            e = e->next;
        }

        if (chain_len > 0)
            used_buckets++;
        if (chain_len > max_chain)
            max_chain = chain_len;
        total_chain += chain_len;
    }

    if (max_chain > 8) {
        cy_add_warning(ctx,
                       "port cache: max chain length %u "
                       "(used %u/%u buckets, %u entries)",
                       max_chain, used_buckets,
                       CY_PORT_CACHE_BUCKETS, total_chain);
    }
}

/* ── Build port cache from pipeline data ────────────────────────── */

int cy_build_port_cache(cy_pipeline *pipe, cy_context *ctx)
{
    uint32_t i, j;
    uint32_t total_ports = 0;

    /* Count total ports for capacity estimate */
    for (i = 0; i < pipe->node_count; i++) {
        if (pipe->nodes[i].ports)
            total_ports += pipe->nodes[i].port_count;
    }

    if (total_ports == 0) {
        pipe->port_cache = NULL;
        return CY_OK;
    }

    /* Create cache with capacity for all ports */
    pipe->port_cache = cy_port_cache_create(ctx, total_ports + 16);
    if (!pipe->port_cache)
        return CY_ERR_NOMEM;

    /* Insert all ports into cache */
    for (i = 0; i < pipe->node_count; i++) {
        cy_node *node = &pipe->nodes[i];
        if (!node->ports)
            continue;
        if (node->flags & CY_NODE_PRUNED)
            continue;

        for (j = 0; j < node->port_count; j++) {
            cy_port *port = &node->ports[j];
            int rc;

            if (!port->name)
                continue;

            rc = cy_port_cache_insert(pipe->port_cache,
                                      node->id, port->name, port);
            if (rc != 0) {
                cy_add_warning(ctx,
                               "port cache insertion failed for "
                               "node %u port %u (%s)",
                               node->id, port->id,
                               port->name);
            }
        }
    }

    /* Analyze distribution */
    cy_port_cache_analyze(pipe->port_cache, ctx);

    return CY_OK;
}

/* ── Cache iteration ────────────────────────────────────────────── */

int cy_port_cache_iterate(const cy_port_cache *cache,
                          int (*callback)(uint32_t node_id,
                                         const char *name,
                                         cy_port *port,
                                         void *user_data),
                          void *user_data)
{
    uint32_t i;

    if (!cache || !callback)
        return -1;

    for (i = 0; i < cache->entry_count; i++) {
        cy_port_cache_entry *entry = &cache->entries[i];
        int rc = callback(entry->node_id, entry->name,
                          entry->port, user_data);
        if (rc != 0)
            return rc;
    }

    return 0;
}

/* ── Cache statistics computation ───────────────────────────────── */

typedef struct {
    uint32_t total_entries;
    uint32_t used_buckets;
    uint32_t max_chain;
    uint32_t empty_buckets;
    double   avg_chain;
    double   load_factor;
} cy_cache_stats;

int cy_port_cache_get_stats(const cy_port_cache *cache,
                            cy_cache_stats *stats)
{
    uint32_t i;
    uint32_t total_chain = 0;

    if (!cache || !stats)
        return -1;

    memset(stats, 0, sizeof(cy_cache_stats));
    stats->total_entries = cache->entry_count;

    for (i = 0; i < CY_PORT_CACHE_BUCKETS; i++) {
        const cy_port_cache_entry *e = cache->buckets[i];
        uint32_t chain_len = 0;

        while (e) {
            chain_len++;
            e = e->next;
        }

        if (chain_len > 0)
            stats->used_buckets++;
        else
            stats->empty_buckets++;

        if (chain_len > stats->max_chain)
            stats->max_chain = chain_len;

        total_chain += chain_len;
    }

    if (stats->used_buckets > 0) {
        stats->avg_chain = (double)total_chain /
                           (double)stats->used_buckets;
    }

    stats->load_factor = (double)cache->entry_count /
                         (double)CY_PORT_CACHE_BUCKETS;

    return 0;
}

/* ── Cache lookup by node (all ports for a given node) ──────────── */

uint32_t cy_port_cache_find_by_node(const cy_port_cache *cache,
                                    uint32_t node_id,
                                    cy_port **out_ports,
                                    uint32_t max_count)
{
    uint32_t i;
    uint32_t found = 0;

    if (!cache || !out_ports)
        return 0;

    for (i = 0; i < cache->entry_count; i++) {
        cy_port_cache_entry *entry = &cache->entries[i];
        if (entry->node_id == node_id && found < max_count) {
            out_ports[found++] = entry->port;
        }
    }

    return found;
}

/* ── Cache invalidation by node ─────────────────────────────────── */

uint32_t cy_port_cache_invalidate_node(cy_port_cache *cache,
                                       uint32_t node_id)
{
    uint32_t i;
    uint32_t invalidated = 0;

    if (!cache)
        return 0;

    /* Walk through all buckets and remove entries for this node */
    for (i = 0; i < CY_PORT_CACHE_BUCKETS; i++) {
        cy_port_cache_entry **pp = &cache->buckets[i];
        while (*pp) {
            if ((*pp)->node_id == node_id) {
                *pp = (*pp)->next;
                invalidated++;
            } else {
                pp = &(*pp)->next;
            }
        }
    }

    return invalidated;
}

/* ── Cache bulk validation ──────────────────────────────────────── */

int cy_port_cache_validate(const cy_port_cache *cache,
                           const cy_pipeline *pipe,
                           cy_context *ctx)
{
    uint32_t i;
    uint32_t invalid_port = 0;
    uint32_t invalid_name = 0;
    uint32_t stale_node = 0;

    if (!cache || !pipe)
        return CY_OK;

    for (i = 0; i < cache->entry_count; i++) {
        const cy_port_cache_entry *entry = &cache->entries[i];

        /* Check if the node_id is valid */
        if (entry->node_id >= pipe->node_count) {
            stale_node++;
            continue;
        }

        /* Check if the node is pruned (stale cache entry) */
        if (pipe->nodes[entry->node_id].flags & CY_NODE_PRUNED) {
            stale_node++;
            continue;
        }

        /* Verify port pointer is non-null */
        if (!entry->port) {
            invalid_port++;
            continue;
        }

        /* Verify name pointer is non-null */
        if (!entry->name) {
            invalid_name++;
            continue;
        }

        /* Verify port's node_id matches the cache key */
        if (entry->port->node_id != entry->node_id) {
            cy_add_warning(ctx,
                           "cache entry %u: port node_id %u != "
                           "cache key node_id %u",
                           i, entry->port->node_id,
                           entry->node_id);
        }
    }

    if (stale_node > 0)
        cy_add_warning(ctx, "port cache: %u stale entries for "
                       "pruned/invalid nodes", stale_node);
    if (invalid_port > 0)
        cy_add_warning(ctx, "port cache: %u entries with null port",
                       invalid_port);
    if (invalid_name > 0)
        cy_add_warning(ctx, "port cache: %u entries with null name",
                       invalid_name);

    return CY_OK;
}
