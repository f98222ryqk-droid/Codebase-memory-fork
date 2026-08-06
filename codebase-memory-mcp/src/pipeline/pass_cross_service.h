/*
 * pass_cross_service.h — Cross-Service HTTP Route Mapping
 *
 * Extends the AST parser to explicitly link outbound HTTP client calls
 * (e.g., Axios, fetch, Python requests) in one microservice directly
 * to their corresponding inbound API endpoint handlers in another
 * microservice within a monorepo.
 *
 * Architecture:
 * 1. Identify HTTP client call sites (outbound) in each service
 * 2. Match them to HTTP route handlers (inbound) in other services
 * 3. Create explicit CROSS_SERVICE_CALL edges between services
 * 4. Support framework-specific patterns (Express, Flask, Spring, etc.)
 *
 * This extends the existing pass_route_nodes.c and pass_cross_repo.h
 * infrastructure with deeper cross-service linkage.
 */
#ifndef CBM_PASS_CROSS_SERVICE_H
#define CBM_PASS_CROSS_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "graph_buffer/graph_buffer.h"

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_pipeline cbm_pipeline_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable cross-service mapping (default: true) */
#ifndef CBM_CROSS_SERVICE_ENABLED
#define CBM_CROSS_SERVICE_ENABLED 1
#endif

/* Maximum number of cross-service edges to create per run */
#ifndef CBM_CROSS_SERVICE_MAX_EDGES
#define CBM_CROSS_SERVICE_MAX_EDGES 10000
#endif

/* ── HTTP Client Patterns ────────────────────────────────────────── */

/* HTTP client call patterns by language/framework */
typedef struct {
    const char *language;       /* "javascript", "python", "go", etc. */
    const char *framework;      /* "axios", "fetch", "requests", "http", etc. */
    const char *pattern;        /* function/method name pattern */
    bool has_url_param;         /* true if first param is URL */
    int method_arg_index;       /* argument index for HTTP method (-1 if none) */
    int url_arg_index;          /* argument index for URL */
    int options_arg_index;      /* argument index for options/headers */
} cbm_http_client_pattern_t;

/* ── HTTP Route Patterns ─────────────────────────────────────────── */

/* HTTP route handler patterns by framework */
typedef struct {
    const char *framework;      /* "express", "flask", "spring", "gin", etc. */
    const char *method;         /* "GET", "POST", "PUT", "DELETE", etc. */
    const char *pattern;        /* decorator/function pattern */
    int path_arg_index;         /* argument index for route path */
    int handler_arg_index;      /* argument index for handler function */
} cbm_http_route_pattern_t;

/* ── Cross-Service Edge ──────────────────────────────────────────── */

typedef struct {
    char *source_service;       /* source microservice name */
    char *target_service;       /* target microservice name */
    char *source_node_qn;       /* qualified name of source node */
    char *target_node_qn;       /* qualified name of target node */
    char *http_method;          /* GET, POST, PUT, DELETE, etc. */
    char *route_path;           /* canonicalized route path */
    char *edge_type;            /* CROSS_SERVICE_CALL, CROSS_HTTP_CALLS, etc. */
    float confidence;           /* match confidence (0.0 - 1.0) */
} cbm_cross_service_edge_t;

/* ── Cross-Service Map ───────────────────────────────────────────── */

typedef struct {
    cbm_cross_service_edge_t *edges;
    int count;
    int capacity;
    
    /* Statistics */
    int total_matches;
    int high_confidence_matches;
    int low_confidence_matches;
    int services_connected;
} cbm_cross_service_map_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the cross-service mapping subsystem. */
int cbm_cross_service_init(void);

/* Shutdown the cross-service mapping subsystem. */
void cbm_cross_service_shutdown(void);

/* Detect HTTP client call patterns for a language. */
const cbm_http_client_pattern_t *cbm_cross_service_client_patterns(const char *language,
                                                                    int *count);

/* Detect HTTP route patterns for a framework. */
const cbm_http_route_pattern_t *cbm_cross_service_route_patterns(const char *framework,
                                                                  int *count);

/* Build a cross-service map from the graph buffer.
 * Returns heap-allocated map (caller frees with cbm_cross_service_map_free). */
cbm_cross_service_map_t *cbm_cross_service_build_map(cbm_gbuf_t *gbuf,
                                                       cbm_store_t *store,
                                                       const char *project);

/* Free a cross-service map. NULL-safe. */
void cbm_cross_service_map_free(cbm_cross_service_map_t *map);

/* Write cross-service edges to the store.
 * Returns number of edges written. */
int cbm_cross_service_write_edges(cbm_store_t *store,
                                   const cbm_cross_service_map_t *map);

/* Match an outbound HTTP call to an inbound route handler.
 * Returns heap-allocated edge (caller frees) or NULL. */
cbm_cross_service_edge_t *cbm_cross_service_match(cbm_gbuf_t *gbuf,
                                                    cbm_store_t *store,
                                                    const char *source_service,
                                                    const char *target_service,
                                                    const char *route_path,
                                                    const char *http_method);

/* Get all services in a monorepo. */
char **cbm_cross_service_detect_services(cbm_store_t *store,
                                          const char *project,
                                          int *count);

/* Check if a node is an HTTP client call. */
bool cbm_cross_service_is_http_call(const cbm_gbuf_node_t *node);

/* Check if a node is an HTTP route handler. */
bool cbm_cross_service_is_route_handler(const cbm_gbuf_node_t *node);

#endif /* CBM_PASS_CROSS_SERVICE_H */
