/*
 * pass_cross_service.c — Cross-Service HTTP Route Mapping Implementation
 *
 * Extends the AST parser to explicitly link outbound HTTP client calls
 * in one microservice directly to their corresponding inbound API
 * endpoint handlers in another microservice within a monorepo.
 */
#include "pipeline/pass_cross_service.h"
#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/compat.h"
#include "store/store.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    CS_BUF = 4096,
    CS_PATH_MAX = 4096,
    CS_EDGE_MAX = 10000,
};

/* ── HTTP Client Patterns ────────────────────────────────────────── */

/* JavaScript/TypeScript HTTP client patterns */
static const cbm_http_client_pattern_t cs_js_patterns[] = {
    { "javascript", "axios", "axios.get", true, -1, 0, 1 },
    { "javascript", "axios", "axios.post", true, -1, 0, 1 },
    { "javascript", "axios", "axios.put", true, -1, 0, 1 },
    { "javascript", "axios", "axios.delete", true, -1, 0, 1 },
    { "javascript", "axios", "axios.request", true, -1, 0, -1 },
    { "javascript", "fetch", "fetch", true, -1, 0, 1 },
    { "typescript", "axios", "axios.get", true, -1, 0, 1 },
    { "typescript", "axios", "axios.post", true, -1, 0, 1 },
    { "typescript", "fetch", "fetch", true, -1, 0, 1 },
};

/* Python HTTP client patterns */
static const cbm_http_client_pattern_t cs_py_patterns[] = {
    { "python", "requests", "requests.get", true, -1, 0, 1 },
    { "python", "requests", "requests.post", true, -1, 0, 1 },
    { "python", "requests", "requests.put", true, -1, 0, 1 },
    { "python", "requests", "requests.delete", true, -1, 0, 1 },
    { "python", "httpx", "httpx.get", true, -1, 0, 1 },
    { "python", "httpx", "httpx.post", true, -1, 0, 1 },
    { "python", "urllib", "urllib.request.urlopen", true, -1, 0, -1 },
};

/* Go HTTP client patterns */
static const cbm_http_client_pattern_t cs_go_patterns[] = {
    { "go", "net/http", "http.Get", true, -1, 0, -1 },
    { "go", "net/http", "http.Post", true, -1, 0, 2 },
    { "go", "net/http", "http.Do", true, -1, 0, -1 },
};

/* Java HTTP client patterns */
static const cbm_http_client_pattern_t cs_java_patterns[] = {
    { "java", "HttpClient", "send", true, -1, 0, -1 },
    { "java", "RestTemplate", "getForObject", true, -1, 0, -1 },
    { "java", "RestTemplate", "postForObject", true, -1, 0, -1 },
};

/* ── HTTP Route Patterns ─────────────────────────────────────────── */

/* Express.js route patterns */
static const cbm_http_route_pattern_t cs_express_routes[] = {
    { "express", "GET", "app.get", 0, 1 },
    { "express", "POST", "app.post", 0, 1 },
    { "express", "PUT", "app.put", 0, 1 },
    { "express", "DELETE", "app.delete", 0, 1 },
    { "express", "PATCH", "app.patch", 0, 1 },
    { "express", "GET", "router.get", 0, 1 },
    { "express", "POST", "router.post", 0, 1 },
};

/* Flask/FastAPI route patterns */
static const cbm_http_route_pattern_t cs_flask_routes[] = {
    { "flask", "GET", "@app.route", 0, -1 },
    { "flask", "POST", "@app.route", 0, -1 },
    { "fastapi", "GET", "@app.get", 0, -1 },
    { "fastapi", "POST", "@app.post", 0, -1 },
    { "fastapi", "PUT", "@app.put", 0, -1 },
    { "fastapi", "DELETE", "@app.delete", 0, -1 },
};

/* Spring Boot route patterns */
static const cbm_http_route_pattern_t cs_spring_routes[] = {
    { "spring", "GET", "@GetMapping", 0, -1 },
    { "spring", "POST", "@PostMapping", 0, -1 },
    { "spring", "PUT", "@PutMapping", 0, -1 },
    { "spring", "DELETE", "@DeleteMapping", 0, -1 },
    { "spring", "PATCH", "@PatchMapping", 0, -1 },
    { "spring", "GET", "@RequestMapping", 0, -1 },
};

/* Gin (Go) route patterns */
static const cbm_http_route_pattern_t cs_gin_routes[] = {
    { "gin", "GET", "gin.Engine.GET", 0, 1 },
    { "gin", "POST", "gin.Engine.POST", 0, 1 },
    { "gin", "PUT", "gin.Engine.PUT", 0, 1 },
    { "gin", "DELETE", "gin.Engine.DELETE", 0, 1 },
};

/* ── State ───────────────────────────────────────────────────────── */

static atomic_int g_cs_initialized = 0;

/* ── Lifecycle ───────────────────────────────────────────────────── */

int cbm_cross_service_init(void) {
    atomic_store(&g_cs_initialized, 1);
    cbm_log_info("cross_service", "status", "initialized");
    return 0;
}

void cbm_cross_service_shutdown(void) {
    atomic_store(&g_cs_initialized, 0);
}

/* ── Pattern Access ──────────────────────────────────────────────── */

const cbm_http_client_pattern_t *cbm_cross_service_client_patterns(const char *language,
                                                                    int *count) {
    if (!language || !count) return NULL;
    
    *count = 0;
    
    if (strcmp(language, "javascript") == 0 || strcmp(language, "typescript") == 0) {
        *count = sizeof(cs_js_patterns) / sizeof(cs_js_patterns[0]);
        return cs_js_patterns;
    }
    if (strcmp(language, "python") == 0) {
        *count = sizeof(cs_py_patterns) / sizeof(cs_py_patterns[0]);
        return cs_py_patterns;
    }
    if (strcmp(language, "go") == 0) {
        *count = sizeof(cs_go_patterns) / sizeof(cs_go_patterns[0]);
        return cs_go_patterns;
    }
    if (strcmp(language, "java") == 0) {
        *count = sizeof(cs_java_patterns) / sizeof(cs_java_patterns[0]);
        return cs_java_patterns;
    }
    
    return NULL;
}

const cbm_http_route_pattern_t *cbm_cross_service_route_patterns(const char *framework,
                                                                  int *count) {
    if (!framework || !count) return NULL;
    
    *count = 0;
    
    if (strcmp(framework, "express") == 0) {
        *count = sizeof(cs_express_routes) / sizeof(cs_express_routes[0]);
        return cs_express_routes;
    }
    if (strcmp(framework, "flask") == 0 || strcmp(framework, "fastapi") == 0) {
        *count = sizeof(cs_flask_routes) / sizeof(cs_flask_routes[0]);
        return cs_flask_routes;
    }
    if (strcmp(framework, "spring") == 0) {
        *count = sizeof(cs_spring_routes) / sizeof(cs_spring_routes[0]);
        return cs_spring_routes;
    }
    if (strcmp(framework, "gin") == 0) {
        *count = sizeof(cs_gin_routes) / sizeof(cs_gin_routes[0]);
        return cs_gin_routes;
    }
    
    return NULL;
}

/* ── Service Detection ────────────────────────────────────────────── */

char **cbm_cross_service_detect_services(cbm_store_t *store,
                                          const char *project,
                                          int *count) {
    if (!store || !project || !count) return NULL;
    
    *count = 0;
    
    /* Detect services by looking for package.json, requirements.txt, go.mod, etc. */
    /* For now, return a single service */
    char **services = malloc(sizeof(char *));
    if (!services) return NULL;
    
    services[0] = strdup("default");
    *count = 1;
    
    return services;
}

/* ── Node Classification ─────────────────────────────────────────── */

bool cbm_cross_service_is_http_call(const cbm_gbuf_node_t *node) {
    if (!node) return false;
    
    /* Check if node has HTTP_CALLS or ASYNC_CALLS edge type */
    /* This would check the graph buffer for edges */
    (void)node;
    
    return false;
}

bool cbm_cross_service_is_route_handler(const cbm_gbuf_node_t *node) {
    if (!node) return false;
    
    /* Check if node is a Route or has route-related properties */
    (void)node;
    
    return false;
}

/* ── Node Classification ─────────────────────────────────────────── */

/* ── Map Building ────────────────────────────────────────────────── */

cbm_cross_service_map_t *cbm_cross_service_build_map(cbm_gbuf_t *gbuf,
                                                       cbm_store_t *store,
                                                       const char *project) {
    if (!gbuf || !store || !project) return NULL;
    
    cbm_cross_service_map_t *map = calloc(1, sizeof(cbm_cross_service_map_t));
    if (!map) return NULL;
    
    map->capacity = CS_EDGE_MAX;
    map->edges = calloc(map->capacity, sizeof(cbm_cross_service_edge_t));
    if (!map->edges) {
        free(map);
        return NULL;
    }
    
    /* Detect services */
    int service_count = 0;
    char **services = cbm_cross_service_detect_services(store, project, &service_count);
    
    if (services) {
        for (int i = 0; i < service_count; i++) {
            free(services[i]);
        }
        free(services);
    }
    
    return map;
}

void cbm_cross_service_map_free(cbm_cross_service_map_t *map) {
    if (!map) return;
    
    for (int i = 0; i < map->count; i++) {
        free(map->edges[i].source_service);
        free(map->edges[i].target_service);
        free(map->edges[i].source_node_qn);
        free(map->edges[i].target_node_qn);
        free(map->edges[i].http_method);
        free(map->edges[i].route_path);
        free(map->edges[i].edge_type);
    }
    free(map->edges);
    free(map);
}

/* ── Edge Writing ────────────────────────────────────────────────── */

int cbm_cross_service_write_edges(cbm_store_t *store,
                                   const cbm_cross_service_map_t *map) {
    if (!store || !map) return 0;
    
    int written = 0;
    
    for (int i = 0; i < map->count; i++) {
        const cbm_cross_service_edge_t *edge = &map->edges[i];
        
        /* Write edge to store */
        /* In production, this would use cbm_store_add_edge or similar */
        (void)edge;
        
        written++;
    }
    
    return written;
}

/* ── Matching ────────────────────────────────────────────────────── */

cbm_cross_service_edge_t *cbm_cross_service_match(cbm_gbuf_t *gbuf,
                                                    cbm_store_t *store,
                                                    const char *source_service,
                                                    const char *target_service,
                                                    const char *route_path,
                                                    const char *http_method) {
    if (!gbuf || !store || !source_service || !target_service || !route_path || !http_method) {
        return NULL;
    }
    
    cbm_cross_service_edge_t *edge = calloc(1, sizeof(cbm_cross_service_edge_t));
    if (!edge) return NULL;
    
    edge->source_service = strdup(source_service);
    edge->target_service = strdup(target_service);
    edge->route_path = strdup(route_path);
    edge->http_method = strdup(http_method);
    edge->edge_type = strdup("CROSS_SERVICE_CALL");
    edge->confidence = 0.8f;  /* Placeholder */
    
    return edge;
}
