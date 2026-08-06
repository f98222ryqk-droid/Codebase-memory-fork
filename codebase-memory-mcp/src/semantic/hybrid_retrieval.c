/*
 * hybrid_retrieval.c — Hybrid Retrieval: Graph Constraints + Local Embeddings
 *
 * Implementation of hybrid search that combines structural graph constraints
 * with semantic vector search for more accurate and relevant results.
 */
#include "semantic/hybrid_retrieval.h"
#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/compat.h"
#include "store/store.h"
#include "semantic/semantic.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    HR_BUF = 4096,
    HR_RESULTS_MAX = 100,
    HR_CANDIDATES_MAX = 10000,
};

/* ── State ───────────────────────────────────────────────────────── */

static atomic_int g_hr_initialized = 0;

/* ── Lifecycle ───────────────────────────────────────────────────── */

int cbm_hybrid_retrieval_init(void) {
    atomic_store(&g_hr_initialized, 1);
    cbm_log_info("hybrid_retrieval", "status", "initialized");
    return 0;
}

void cbm_hybrid_retrieval_shutdown(void) {
    atomic_store(&g_hr_initialized, 0);
}

/* ── Constraint Management ───────────────────────────────────────── */

cbm_hybrid_constraint_t *cbm_hybrid_constraint_new(cbm_hybrid_constraint_type_t type) {
    cbm_hybrid_constraint_t *c = calloc(1, sizeof(cbm_hybrid_constraint_t));
    if (!c) return NULL;
    
    c->type = type;
    c->max_depth = 3;  /* Default BFS depth */
    
    return c;
}

void cbm_hybrid_constraint_free(cbm_hybrid_constraint_t *c) {
    if (!c) return;
    free(c->module_name);
    free(c->file_path);
    free(c->directory_path);
    free(c->class_name);
    free(c->route_pattern);
    free(c->start_node_qn);
    free(c->node_ids);
    free(c);
}

void cbm_hybrid_constraint_set_module(cbm_hybrid_constraint_t *c, const char *module) {
    if (!c) return;
    free(c->module_name);
    c->module_name = module ? strdup(module) : NULL;
}

void cbm_hybrid_constraint_set_file(cbm_hybrid_constraint_t *c, const char *file_path) {
    if (!c) return;
    free(c->file_path);
    c->file_path = file_path ? strdup(file_path) : NULL;
}

void cbm_hybrid_constraint_set_directory(cbm_hybrid_constraint_t *c, const char *dir_path) {
    if (!c) return;
    free(c->directory_path);
    c->directory_path = dir_path ? strdup(dir_path) : NULL;
}

void cbm_hybrid_constraint_set_class(cbm_hybrid_constraint_t *c, const char *class_name) {
    if (!c) return;
    free(c->class_name);
    c->class_name = class_name ? strdup(class_name) : NULL;
}

void cbm_hybrid_constraint_set_route(cbm_hybrid_constraint_t *c, const char *route_pattern) {
    if (!c) return;
    free(c->route_pattern);
    c->route_pattern = route_pattern ? strdup(route_pattern) : NULL;
}

void cbm_hybrid_constraint_set_call_chain(cbm_hybrid_constraint_t *c, const char *start_node_qn,
                                           int max_depth) {
    if (!c) return;
    free(c->start_node_qn);
    c->start_node_qn = start_node_qn ? strdup(start_node_qn) : NULL;
    c->max_depth = max_depth > 0 ? max_depth : 3;
}

/* ── Candidate Filtering ─────────────────────────────────────────── */

int64_t *cbm_hybrid_get_candidates(cbm_store_t *store,
                                    const char *project,
                                    const cbm_hybrid_constraint_t *constraint,
                                    int *count) {
    if (!store || !project || !constraint || !count) return NULL;
    
    *count = 0;
    int cap = HR_CANDIDATES_MAX;
    int64_t *candidates = malloc(cap * sizeof(int64_t));
    if (!candidates) return NULL;
    
    switch (constraint->type) {
        case CBM_HYBRID_CONSTRAINT_NONE:
            /* No constraint - return all nodes (limited) */
            break;
            
        case CBM_HYBRID_CONSTRAINT_FILE: {
            /* Find all nodes in the specified file */
            cbm_node_t *nodes = NULL;
            int node_count = 0;
            if (cbm_store_find_nodes_by_file(store, project, constraint->file_path,
                                              &nodes, &node_count) == 0) {
                for (int i = 0; i < node_count && *count < cap; i++) {
                    candidates[(*count)++] = nodes[i].id;
                }
            }
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_MODULE: {
            /* Find all nodes with qualified_name starting with module prefix */
            cbm_node_t *nodes = NULL;
            int node_count = 0;
            if (cbm_store_find_nodes_by_prefix(store, project, constraint->module_name,
                                                &nodes, &node_count) == 0) {
                for (int i = 0; i < node_count && *count < cap; i++) {
                    candidates[(*count)++] = nodes[i].id;
                }
            }
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_CALL_CHAIN: {
            /* BFS from start node */
            /* In production, this would use cbm_store_bfs */
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_DIRECTORY: {
            /* Find all nodes in files under the directory */
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_CLASS: {
            /* Find all nodes in the class hierarchy */
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_ROUTE: {
            /* Find all nodes matching the route pattern */
            break;
        }
            
        case CBM_HYBRID_CONSTRAINT_CUSTOM: {
            /* Use the provided node IDs */
            for (int i = 0; i < constraint->node_count && *count < cap; i++) {
                candidates[(*count)++] = constraint->node_ids[i];
            }
            break;
        }
    }
    
    return candidates;
}

/* ── Scoring ─────────────────────────────────────────────────────── */

float cbm_hybrid_structural_score(cbm_store_t *store,
                                    const char *project,
                                    int64_t node_id,
                                    const cbm_hybrid_constraint_t *constraint) {
    if (!store || !project || !constraint) return 0.0f;
    
    /* Calculate structural proximity based on constraint type */
    switch (constraint->type) {
        case CBM_HYBRID_CONSTRAINT_NONE:
            return 1.0f;  /* No constraint = full score */
            
        case CBM_HYBRID_CONSTRAINT_FILE:
        case CBM_HYBRID_CONSTRAINT_MODULE:
            /* Score based on how deep in the module hierarchy */
            return 0.8f;  /* Placeholder */
            
        case CBM_HYBRID_CONSTRAINT_CALL_CHAIN: {
            /* Score based on BFS depth from start node */
            /* In production, this would calculate actual shortest path */
            return 0.6f;  /* Placeholder */
        }
            
        default:
            return 0.5f;
    }
}

float cbm_hybrid_fuse_scores(float semantic, float structural) {
    float w_sem = CBM_HYBRID_SEM_WEIGHT;
    float w_struct = CBM_HYBRID_STRUCT_WEIGHT;
    
    /* Weighted combination */
    float fused = (semantic * w_sem) + (structural * w_struct);
    
    /* Boost when both scores are high (reduces false positives) */
    if (semantic > 0.8f && structural > 0.8f) {
        fused *= 1.1f;
    }
    
    /* Cap at 1.0 */
    if (fused > 1.0f) fused = 1.0f;
    
    return fused;
}

/* ── Search ──────────────────────────────────────────────────────── */

cbm_hybrid_results_t *cbm_hybrid_search(cbm_store_t *store,
                                         const char *project,
                                         const char *query,
                                         const cbm_hybrid_constraint_t *constraint,
                                         int top_k) {
    if (!atomic_load(&g_hr_initialized)) return NULL;
    if (!store || !project || !query) return NULL;
    
    cbm_hybrid_results_t *results = calloc(1, sizeof(cbm_hybrid_results_t));
    if (!results) return NULL;
    
    results->query = strdup(query);
    results->constraint = constraint ? cbm_hybrid_constraint_new(constraint->type) : NULL;
    results->capacity = top_k > 0 ? top_k : HR_RESULTS_MAX;
    results->results = calloc(results->capacity, sizeof(cbm_hybrid_result_t));
    
    struct timespec start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Step 1: Get candidates from graph constraint */
    int candidate_count = 0;
    int64_t *candidates = cbm_hybrid_get_candidates(store, project, constraint, &candidate_count);
    results->candidates_considered = candidate_count;
    
    struct timespec graph_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &graph_end);
    results->graph_time_ms = ((double)(graph_end.tv_sec - start.tv_sec) * 1000.0) +
                              ((double)(graph_end.tv_nsec - start.tv_nsec) / 1000000.0);
    
    /* Step 2: Perform semantic search on candidates */
    /* In production, this would:
     * 1. Generate embedding for query using nomic-embed-code
     * 2. Search only within candidate node embeddings
     * 3. Calculate semantic scores
     */
    
    /* For now, create placeholder results */
    if (candidates && candidate_count > 0) {
        int result_count = candidate_count < results->capacity ? candidate_count : results->capacity;
        
        for (int i = 0; i < result_count; i++) {
            cbm_hybrid_result_t *r = &results->results[i];
            r->node_id = candidates[i];
            r->name = strdup("placeholder");
            r->qualified_name = strdup("placeholder");
            r->file_path = strdup("placeholder");
            r->label = strdup("Function");
            r->semantic_score = 0.9f - (i * 0.01f);  /* Decreasing scores */
            r->structural_score = cbm_hybrid_structural_score(store, project, candidates[i], constraint);
            r->fused_score = cbm_hybrid_fuse_scores(r->semantic_score, r->structural_score);
            results->count++;
        }
    }
    
    if (candidates) free(candidates);
    
    struct timespec vector_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &vector_end);
    results->vector_time_ms = ((double)(vector_end.tv_sec - graph_end.tv_sec) * 1000.0) +
                               ((double)(vector_end.tv_nsec - graph_end.tv_nsec) / 1000000.0);
    results->search_time_ms = ((double)(vector_end.tv_sec - start.tv_sec) * 1000.0) +
                               ((double)(vector_end.tv_nsec - start.tv_nsec) / 1000000.0);
    
    cbm_log_info("hybrid_retrieval", "search_complete",
                 "results", "0",
                 "candidates", "0",
                 "time_ms", "0");
    
    return results;
}

cbm_hybrid_results_t *cbm_hybrid_search_unconstrained(cbm_store_t *store,
                                                       const char *project,
                                                       const char *query,
                                                       int top_k) {
    return cbm_hybrid_search(store, project, query, NULL, top_k);
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

void cbm_hybrid_results_free(cbm_hybrid_results_t *results) {
    if (!results) return;
    
    for (int i = 0; i < results->count; i++) {
        free(results->results[i].name);
        free(results->results[i].qualified_name);
        free(results->results[i].file_path);
        free(results->results[i].label);
        free(results->results[i].relationship);
    }
    free(results->results);
    
    free(results->query);
    cbm_hybrid_constraint_free(results->constraint);
    free(results);
}

/* ── Formatting ──────────────────────────────────────────────────── */

char *cbm_hybrid_results_json(const cbm_hybrid_results_t *results) {
    if (!results) return NULL;
    
    char *json = malloc(65536);
    if (!json) return NULL;
    
    int off = 0;
    off += snprintf(json + off, 65536 - off,
                    "{\n"
                    "  \"query\": \"%s\",\n"
                    "  \"count\": %d,\n"
                    "  \"candidates_considered\": %d,\n"
                    "  \"search_time_ms\": %.2f,\n"
                    "  \"results\": [\n",
                    results->query ? results->query : "",
                    results->count,
                    results->candidates_considered,
                    results->search_time_ms);
    
    for (int i = 0; i < results->count && off < 65000; i++) {
        cbm_hybrid_result_t *r = &results->results[i];
        off += snprintf(json + off, 65536 - off,
                        "    {\n"
                        "      \"node_id\": %lld,\n"
                        "      \"name\": \"%s\",\n"
                        "      \"qualified_name\": \"%s\",\n"
                        "      \"file_path\": \"%s\",\n"
                        "      \"label\": \"%s\",\n"
                        "      \"semantic_score\": %.4f,\n"
                        "      \"structural_score\": %.4f,\n"
                        "      \"fused_score\": %.4f\n"
                        "    }%s\n",
                        (long long)r->node_id,
                        r->name ? r->name : "",
                        r->qualified_name ? r->qualified_name : "",
                        r->file_path ? r->file_path : "",
                        r->label ? r->label : "",
                        r->semantic_score,
                        r->structural_score,
                        r->fused_score,
                        i < results->count - 1 ? "," : "");
    }
    
    off += snprintf(json + off, 65536 - off, "  ]\n}\n");
    
    return json;
}

char *cbm_hybrid_results_text(const cbm_hybrid_results_t *results) {
    if (!results) return NULL;
    
    char *text = malloc(32768);
    if (!text) return NULL;
    
    int off = 0;
    off += snprintf(text + off, 32768 - off,
                    "Hybrid Search Results\n"
                    "=====================\n"
                    "Query: %s\n"
                    "Results: %d\n"
                    "Candidates: %d\n"
                    "Time: %.2fms\n\n",
                    results->query ? results->query : "",
                    results->count,
                    results->candidates_considered,
                    results->search_time_ms);
    
    for (int i = 0; i < results->count && off < 32500; i++) {
        cbm_hybrid_result_t *r = &results->results[i];
        off += snprintf(text + off, 32768 - off,
                        "%d. %s (%s)\n"
                        "   File: %s\n"
                        "   Scores: semantic=%.3f structural=%.3f fused=%.3f\n\n",
                        i + 1,
                        r->name ? r->name : "",
                        r->label ? r->label : "",
                        r->file_path ? r->file_path : "",
                        r->semantic_score,
                        r->structural_score,
                        r->fused_score);
    }
    
    return text;
}
