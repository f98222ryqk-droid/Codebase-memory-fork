/*
 * pagerank.c — Personalized PageRank + HITS for code graph ranking.
 *
 * Implements graph-theoretic centrality measures for improved code retrieval
 * accuracy. The key insight: BM25/semantic search gives local relevance,
 * but PageRank gives global structural importance. Query-biased PageRank
 * combines both — a random surfer who teleports to query-relevant seeds
 * rather than uniformly at random.
 *
 * Algorithm details:
 *
 * Personalized PageRank (PPR):
 *   r = (1 - d) * v + d * M * r
 * where d = damping, v = personalization vector, M = weighted transition
 * matrix (column-stochastic). Solved by power iteration with early
 * termination on L1-norm convergence.
 *
 * HITS:
 *   a = M^T * h    (authorities = pages linked to by good hubs)
 *   h = M * a      (hubs = pages linking to good authorities)
 * Alternating update with L2 normalization each step.
 *
 * Edge-type weighting: The transition matrix is built with type-dependent
 * weights so CALLS edges contribute more than USAGE edges, reflecting
 * the strength of structural dependency in code.
 *
 * Adaptive damping: Sparse graphs (avg_degree < 2) get lower damping
 * to prevent rank concentration; dense graphs get higher damping to
 * leverage rich link structure.
 */
#include "semantic/pagerank.h"
#include "foundation/constants.h"
#include "foundation/compat.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include "store/store.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    PR_INIT_CAP = 4096,     /* Initial capacity for score arrays */
    PR_BATCH = 256,         /* Batch size for edge fetching */
    PR_TOP_REPORT = 20,     /* Number of top scores to log */
};

/* Small epsilon to avoid division by zero. */
static const double PR_EPS = 1e-12;

/* ── State ───────────────────────────────────────────────────────── */

static atomic_int g_pr_initialized = 0;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/* Auto-init on first use; no explicit init required. */
static void pr_ensure_init(void) {
    int expected = 0;
    atomic_compare_exchange_strong(&g_pr_initialized, &expected, 1);
}

/* ── Edge Weight Lookup ──────────────────────────────────────────── */

double cbm_pr_edge_weight(const char *edge_type) {
    if (!edge_type) return CBM_PR_W_DEFAULT;

    /* Fast path: check most common types first. */
    if (edge_type[0] == 'C') {
        if (strcmp(edge_type, "CALLS") == 0) return CBM_PR_W_CALLS;
        if (strcmp(edge_type, "CALL_REFERENCE") == 0) return CBM_PR_W_CALL_REFERENCE;
    } else if (edge_type[0] == 'I') {
        if (strcmp(edge_type, "IMPORTS") == 0) return CBM_PR_W_IMPORTS;
        if (strcmp(edge_type, "INHERITS") == 0) return CBM_PR_W_INHERITS;
        if (strcmp(edge_type, "IMPLEMENTS") == 0) return CBM_PR_W_IMPLEMENTS;
    } else if (edge_type[0] == 'U') {
        if (strcmp(edge_type, "USAGE") == 0) return CBM_PR_W_USAGE;
    } else if (edge_type[0] == 'H') {
        if (strcmp(edge_type, "HTTP_CALLS") == 0) return CBM_PR_W_HTTP_CALLS;
    }

    return CBM_PR_W_DEFAULT;
}

/* ── Adaptive Damping ───────────────────────────────────────────── */

double cbm_pr_adaptive_damping(double avg_out_degree) {
    /* Sigmoid-like mapping from average degree to damping.
     *
     * Low degree (< 2): damping ≈ 0.65 — more weight to personalization,
     *   preventing rank from concentrating on a few high-degree nodes
     *   in sparse graphs.
     *
     * Medium degree (~5): damping ≈ 0.80 — balanced.
     *
     * High degree (> 10): damping ≈ 0.88 — leverage rich link structure.
     *
     * Formula: d = d_min + (d_max - d_min) * sigmoid(k * (deg - mid))
     * where sigmoid(x) = 1 / (1 + exp(-x))
     */
    const double d_min = CBM_PR_DAMPING_MIN;
    const double d_max = 0.90;
    const double mid = 4.0;   /* midpoint degree */
    const double k = 0.8;     /* steepness */

    double x = k * (avg_out_degree - mid);
    double sig = 1.0 / (1.0 + exp(-x));

    double d = d_min + (d_max - d_min) * sig;

    /* Clamp to valid range. */
    if (d < d_min) d = d_min;
    if (d > d_max) d = d_max;

    return d;
}

/* ── Internal: Adjacency Representation ──────────────────────────── */

/* Sparse adjacency list entry: (target, weight). */
typedef struct {
    int64_t target;
    double weight;
} pr_adj_entry_t;

/* Per-node adjacency: outgoing edges with weights. */
typedef struct {
    int64_t node_id;
    pr_adj_entry_t *out;     /* Outgoing edges (caller frees) */
    int out_count;
    int out_cap;
    double out_weight_sum;   /* Sum of outgoing edge weights (for normalization) */
} pr_node_adj_t;

/* Full adjacency structure for the graph. */
typedef struct {
    pr_node_adj_t *nodes;    /* Array of node adjacencies */
    int node_count;
    int64_t *id_map;         /* node_id → index mapping (sorted) */
    int id_map_count;

    /* Reverse adjacency for HITS (indexed by target). */
    pr_adj_entry_t **rev_adj;  /* rev_adj[idx] = array of (source, weight) */
    int *rev_count;
    int *rev_cap;

    /* Graph statistics */
    int total_edges;
    double avg_out_degree;
} pr_graph_t;

/* Binary search in sorted id_map. Returns index or -1. */
static int pr_find_index(const pr_graph_t *g, int64_t node_id) {
    int lo = 0, hi = g->id_map_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g->id_map[mid] == node_id) return mid;
        if (g->id_map[mid] < node_id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Build adjacency from store. This is the expensive step — O(E) edges
 * loaded from SQLite. We build both forward and reverse adjacency
 * (reverse needed for HITS and for BFS-based structural scoring). */
static pr_graph_t *pr_build_graph(cbm_store_t *store, const char *project) {
    if (!store || !project) return NULL;

    pr_graph_t *g = calloc(1, sizeof(pr_graph_t));
    if (!g) return NULL;

    /* Step 1: Load all node IDs for the project.
     * We use the store's search with no filters to get all nodes. */
    cbm_search_params_t params;
    memset(&params, 0, sizeof(params));
    params.project = project;
    /* Include only label types that carry structural meaning. */
    params.label = NULL;  /* All labels */
    params.limit = 50000; /* Load up to 50K nodes */

    cbm_search_output_t output;
    memset(&output, 0, sizeof(output));

    if (cbm_store_search(store, &params, &output) != 0 || !output.results) {
        /* Fallback: try with empty result set. */
        g->node_count = 0;
        g->nodes = NULL;
        g->id_map = NULL;
        g->id_map_count = 0;
        return g;
    }

    int node_count = output.count;
    g->node_count = node_count;
    g->nodes = calloc((size_t)node_count, sizeof(pr_node_adj_t));
    g->id_map = malloc((size_t)node_count * sizeof(int64_t));
    if (!g->nodes || !g->id_map) {
        cbm_store_search_free(&output);
        free(g->nodes);
        free(g->id_map);
        free(g);
        return NULL;
    }

    /* Initialize adjacency entries from search results. */
    for (int i = 0; i < node_count; i++) {
        g->nodes[i].node_id = output.results[i].node.id;
        g->nodes[i].out = NULL;
        g->nodes[i].out_count = 0;
        g->nodes[i].out_cap = 0;
        g->nodes[i].out_weight_sum = 0.0;
        g->id_map[i] = output.results[i].node.id;
    }

    cbm_store_search_free(&output);

    /* Sort id_map for binary search. */
    /* Simple insertion sort (good enough for < 100K nodes). */
    for (int i = 1; i < node_count; i++) {
        int64_t key = g->id_map[i];
        int j = i - 1;
        while (j >= 0 && g->id_map[j] > key) {
            g->id_map[j + 1] = g->id_map[j];
            j--;
        }
        g->id_map[j + 1] = key;
    }
    g->id_map_count = node_count;

    /* Step 2: Load all edges and build adjacency. */
    /* We iterate over all edges in the project. For each edge,
     * we look up the source node index and add the target with weight. */
    int total_edges = 0;

    /* For each node, fetch its outgoing edges. */
    for (int i = 0; i < node_count; i++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;

        if (cbm_store_find_edges_by_source(store, g->nodes[i].node_id,
                                            &edges, &edge_count) == 0 && edges) {
            /* Allocate adjacency for this node. */
            g->nodes[i].out_cap = edge_count > 4 ? edge_count : 4;
            g->nodes[i].out = malloc((size_t)g->nodes[i].out_cap * sizeof(pr_adj_entry_t));
            if (!g->nodes[i].out) {
                free(edges);
                continue;
            }

            for (int e = 0; e < edge_count; e++) {
                double w = cbm_pr_edge_weight(edges[e].type);
                int target_idx = pr_find_index(g, edges[e].target_id);
                if (target_idx >= 0) {
                    /* Only include edges to nodes in our graph. */
                    pr_adj_entry_t *entry = &g->nodes[i].out[g->nodes[i].out_count++];
                    entry->target = edges[e].target_id;
                    entry->weight = w;
                    g->nodes[i].out_weight_sum += w;
                    total_edges++;
                }
            }
            free(edges);
        }
    }

    g->total_edges = total_edges;
    g->avg_out_degree = node_count > 0 ? (double)total_edges / node_count : 0.0;

    /* Step 3: Build reverse adjacency (for HITS). */
    g->rev_adj = calloc((size_t)node_count, sizeof(pr_adj_entry_t *));
    g->rev_count = calloc((size_t)node_count, sizeof(int));
    g->rev_cap = calloc((size_t)node_count, sizeof(int));
    if (!g->rev_adj || !g->rev_count || !g->rev_cap) {
        /* Non-fatal: HITS won't work but PageRank will. */
        return g;
    }

    for (int i = 0; i < node_count; i++) {
        for (int j = 0; j < g->nodes[i].out_count; j++) {
            int64_t target_id = g->nodes[i].out[j].target;
            int target_idx = pr_find_index(g, target_id);
            if (target_idx < 0) continue;

            /* Grow reverse adjacency if needed. */
            if (g->rev_count[target_idx] >= g->rev_cap[target_idx]) {
                int new_cap = g->rev_cap[target_idx] ? g->rev_cap[target_idx] * 2 : 4;
                pr_adj_entry_t *new_adj = realloc(g->rev_adj[target_idx],
                    (size_t)new_cap * sizeof(pr_adj_entry_t));
                if (!new_adj) continue;
                g->rev_adj[target_idx] = new_adj;
                g->rev_cap[target_idx] = new_cap;
            }

            pr_adj_entry_t *rev = &g->rev_adj[target_idx][g->rev_count[target_idx]++];
            rev->target = g->nodes[i].node_id;  /* Source becomes the "target" in reverse */
            rev->weight = g->nodes[i].out[j].weight;
        }
    }

    cbm_log_info("pagerank", "graph_built",
                 "nodes", "%d", node_count,
                 "edges", "%d", total_edges,
                 "avg_degree", "%.2f", g->avg_out_degree);

    return g;
}

/* Free adjacency graph. */
static void pr_graph_free(pr_graph_t *g) {
    if (!g) return;
    if (g->nodes) {
        for (int i = 0; i < g->node_count; i++) {
            free(g->nodes[i].out);
        }
        free(g->nodes);
    }
    if (g->rev_adj) {
        for (int i = 0; i < g->node_count; i++) {
            free(g->rev_adj[i]);
        }
        free(g->rev_adj);
    }
    free(g->rev_count);
    free(g->rev_cap);
    free(g->id_map);
    free(g);
}

/* ── Personalized PageRank ──────────────────────────────────────── */

cbm_pr_result_t *cbm_pagerank(cbm_store_t *store,
                               const char *project,
                               const int64_t *personalization_ids,
                               const float *personalization_weights,
                               int pers_count,
                               double damping,
                               double tolerance,
                               int max_iter) {
    pr_ensure_init();

    if (!store || !project) return NULL;

    /* Build adjacency graph. */
    pr_graph_t *g = pr_build_graph(store, project);
    if (!g || g->node_count == 0) {
        pr_graph_free(g);
        return NULL;
    }

    int N = g->node_count;

    /* Use defaults if not specified. */
    if (damping <= 0.0) damping = CBM_PR_DAMPING;
    if (tolerance <= 0.0) tolerance = CBM_PR_TOLERANCE;
    if (max_iter <= 0) max_iter = CBM_PR_MAX_ITER;

    /* Adaptive damping based on graph density. */
    double adaptive_d = cbm_pr_adaptive_damping(g->avg_out_degree);
    /* Blend user-specified damping with adaptive: user takes priority
     * but we nudge toward adaptive for extreme graphs. */
    if (damping == CBM_PR_DAMPING) {
        /* User didn't override — use fully adaptive. */
        damping = adaptive_d;
    }

    /* Allocate result. */
    cbm_pr_result_t *result = calloc(1, sizeof(cbm_pr_result_t));
    if (!result) { pr_graph_free(g); return NULL; }

    result->capacity = N;
    result->scores = calloc((size_t)N, sizeof(cbm_pr_scores_t));
    if (!result->scores) { pr_graph_free(g); free(result); return NULL; }

    /* Allocate rank vectors (current and next). */
    double *rank = calloc((size_t)N, sizeof(double));
    double *rank_next = calloc((size_t)N, sizeof(double));
    double *pers = calloc((size_t)N, sizeof(double));
    if (!rank || !rank_next || !pers) {
        free(rank); free(rank_next); free(pers);
        pr_graph_free(g); cbm_pr_result_free(result);
        return NULL;
    }

    /* Build personalization vector.
     * If no personalization given, use uniform (1/N).
     * If personalization given, map node_ids to indices and normalize. */
    if (personalization_ids && personalization_weights && pers_count > 0) {
        double pers_sum = 0.0;
        for (int i = 0; i < pers_count; i++) {
            int idx = pr_find_index(g, personalization_ids[i]);
            if (idx >= 0 && personalization_weights[i] > 0.0f) {
                pers[idx] = (double)personalization_weights[i];
                pers_sum += pers[idx];
            }
        }
        /* Normalize personalization to sum to 1.0. */
        if (pers_sum > PR_EPS) {
            for (int i = 0; i < N; i++) {
                pers[i] /= pers_sum;
            }
        } else {
            /* All seeds not found — fall back to uniform. */
            double inv_n = 1.0 / N;
            for (int i = 0; i < N; i++) pers[i] = inv_n;
        }
    } else {
        /* Uniform personalization (vanilla PageRank). */
        double inv_n = 1.0 / N;
        for (int i = 0; i < N; i++) pers[i] = inv_n;
    }

    /* Initialize rank vector to personalization (warm start). */
    memcpy(rank, pers, (size_t)N * sizeof(double));

    /* Power iteration: r_{t+1} = (1 - d) * v + d * M * r_t
     *
     * Where M is the weighted transition matrix:
     *   M[j][i] = w(i→j) / sum_k(w(i→k))  if edge i→j exists
     *           = 0                          otherwise
     *
     * For dangling nodes (no outgoing edges), we redistribute their
     * rank uniformly (standard PageRank convention). */
    double one_minus_d = 1.0 - damping;
    int iterations = 0;
    bool converged = false;
    double final_delta = 0.0;

    struct timespec ts_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int iter = 0; iter < max_iter; iter++) {
        iterations = iter + 1;

        /* Reset rank_next to (1 - d) * v (the teleport component). */
        for (int i = 0; i < N; i++) {
            rank_next[i] = one_minus_d * pers[i];
        }

        /* Accumulate d * M * r (the follow-links component). */
        double dangling_sum = 0.0;  /* Rank mass from dangling nodes */

        for (int i = 0; i < N; i++) {
            if (g->nodes[i].out_count == 0) {
                /* Dangling node: redistribute its rank uniformly. */
                dangling_sum += rank[i];
                continue;
            }

            double out_sum = g->nodes[i].out_weight_sum;
            if (out_sum < PR_EPS) continue;

            /* For each outgoing edge i→j, contribute:
             * d * rank[i] * w(i→j) / sum_k(w(i→k)) to rank_next[j] */
            double rank_contrib = damping * rank[i] / out_sum;

            for (int e = 0; e < g->nodes[i].out_count; e++) {
                int target_idx = pr_find_index(g, g->nodes[i].out[e].target);
                if (target_idx >= 0) {
                    rank_next[target_idx] += rank_contrib * g->nodes[i].out[e].weight;
                }
            }
        }

        /* Redistribute dangling mass uniformly (standard PageRank). */
        if (dangling_sum > PR_EPS) {
            double dangling_contrib = damping * dangling_sum / N;
            for (int i = 0; i < N; i++) {
                rank_next[i] += dangling_contrib;
            }
        }

        /* Check convergence: L1-norm of (rank_next - rank). */
        double delta = 0.0;
        for (int i = 0; i < N; i++) {
            delta += fabs(rank_next[i] - rank[i]);
        }

        /* Swap rank and rank_next. */
        double *tmp = rank;
        rank = rank_next;
        rank_next = tmp;

        if (delta < tolerance) {
            converged = true;
            final_delta = delta;
            break;
        }
        final_delta = delta;
    }

    struct timespec ts_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double compute_ms = ((double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0) +
                         ((double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0);

    /* Normalize rank to sum to 1.0 (fix floating point drift). */
    double rank_sum = 0.0;
    for (int i = 0; i < N; i++) rank_sum += rank[i];
    if (rank_sum > PR_EPS) {
        for (int i = 0; i < N; i++) rank[i] /= rank_sum;
    }

    /* Fill result scores. */
    result->count = 0;
    for (int i = 0; i < N; i++) {
        if (rank[i] > PR_EPS) {
            cbm_pr_scores_t *s = &result->scores[result->count++];
            s->node_id = g->nodes[i].node_id;
            s->pagerank = (float)rank[i];
            s->authority = 0.0f;   /* Filled by HITS if combined */
            s->hub = 0.0f;
            s->combined = (float)rank[i];  /* Default: just PageRank */
        }
    }

    result->iterations = iterations;
    result->converged = converged;
    result->final_delta = (float)final_delta;
    result->compute_time_ms = compute_ms;
    result->node_count = g->node_count;
    result->edge_count = g->total_edges;
    result->avg_out_degree = (float)g->avg_out_degree;
    result->effective_damping = damping;

    cbm_log_info("pagerank", "complete",
                 "iterations", "%d", iterations,
                 "converged", converged ? "true" : "false",
                 "delta", "%.2e", final_delta,
                 "time_ms", "%.1f", compute_ms,
                 "damping", "%.3f", damping);

    free(rank);
    free(rank_next);
    free(pers);
    pr_graph_free(g);

    return result;
}

cbm_pr_result_t *cbm_pagerank_uniform(cbm_store_t *store,
                                       const char *project,
                                       double damping,
                                       double tolerance,
                                       int max_iter) {
    return cbm_pagerank(store, project, NULL, NULL, 0,
                        damping, tolerance, max_iter);
}

cbm_pr_result_t *cbm_pagerank_query_biased(cbm_store_t *store,
                                            const char *project,
                                            const int64_t *seed_ids,
                                            const float *seed_scores,
                                            int seed_count,
                                            double damping,
                                            double tolerance,
                                            int max_iter) {
    if (!seed_ids || !seed_scores || seed_count <= 0) {
        /* No seeds — fall back to uniform. */
        return cbm_pagerank_uniform(store, project, damping, tolerance, max_iter);
    }

    /* Use lower damping by default for query-biased PPR:
     * more weight on the personalization (query) vector. */
    if (damping <= 0.0) damping = 0.70;  /* Lower than vanilla's 0.85 */

    return cbm_pagerank(store, project, seed_ids, seed_scores, seed_count,
                        damping, tolerance, max_iter);
}

/* ── HITS ───────────────────────────────────────────────────────── */

cbm_pr_result_t *cbm_hits(cbm_store_t *store,
                           const char *project,
                           double tolerance,
                           int max_iter) {
    pr_ensure_init();

    if (!store || !project) return NULL;

    /* Build adjacency graph. */
    pr_graph_t *g = pr_build_graph(store, project);
    if (!g || g->node_count == 0 || !g->rev_adj) {
        pr_graph_free(g);
        return NULL;
    }

    int N = g->node_count;

    if (tolerance <= 0.0) tolerance = CBM_HITS_TOLERANCE;
    if (max_iter <= 0) max_iter = CBM_HITS_MAX_ITER;

    /* Allocate result. */
    cbm_pr_result_t *result = calloc(1, sizeof(cbm_pr_result_t));
    if (!result) { pr_graph_free(g); return NULL; }

    result->capacity = N;
    result->scores = calloc((size_t)N, sizeof(cbm_pr_scores_t));
    if (!result->scores) { pr_graph_free(g); free(result); return NULL; }

    /* HITS vectors. */
    double *auth = calloc((size_t)N, sizeof(double));
    double *hub = calloc((size_t)N, sizeof(double));
    double *auth_next = calloc((size_t)N, sizeof(double));
    double *hub_next = calloc((size_t)N, sizeof(double));
    if (!auth || !hub || !auth_next || !hub_next) {
        free(auth); free(hub); free(auth_next); free(hub_next);
        pr_graph_free(g); cbm_pr_result_free(result);
        return NULL;
    }

    /* Initialize: uniform authority and hub scores. */
    double inv_sqrt_n = 1.0 / sqrt((double)N);
    for (int i = 0; i < N; i++) {
        auth[i] = inv_sqrt_n;
        hub[i] = inv_sqrt_n;
    }

    int iterations = 0;
    bool converged = false;
    double final_delta = 0.0;

    struct timespec ts_start;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int iter = 0; iter < max_iter; iter++) {
        iterations = iter + 1;

        /* Update authorities: a = M^T * h
         * For each node j, auth[j] = sum over all i→j of w(i→j) * hub[i] */
        memset(auth_next, 0, (size_t)N * sizeof(double));
        for (int j = 0; j < N; j++) {
            for (int r = 0; r < g->rev_count[j]; r++) {
                int64_t source_id = g->rev_adj[j][r].target;
                double w = g->rev_adj[j][r].weight;
                int src_idx = pr_find_index(g, source_id);
                if (src_idx >= 0) {
                    auth_next[j] += w * hub[src_idx];
                }
            }
        }

        /* Update hubs: h = M * a
         * For each node i, hub[i] = sum over all i→j of w(i→j) * auth[j] */
        memset(hub_next, 0, (size_t)N * sizeof(double));
        for (int i = 0; i < N; i++) {
            for (int e = 0; e < g->nodes[i].out_count; e++) {
                int target_idx = pr_find_index(g, g->nodes[i].out[e].target);
                if (target_idx >= 0) {
                    hub_next[i] += g->nodes[i].out[e].weight * auth_next[target_idx];
                }
            }
        }

        /* L2-normalize both vectors. */
        double auth_norm = 0.0, hub_norm = 0.0;
        for (int i = 0; i < N; i++) {
            auth_norm += auth_next[i] * auth_next[i];
            hub_norm += hub_next[i] * hub_next[i];
        }
        auth_norm = sqrt(auth_norm);
        hub_norm = sqrt(hub_norm);

        if (auth_norm > PR_EPS) {
            for (int i = 0; i < N; i++) auth_next[i] /= auth_norm;
        }
        if (hub_norm > PR_EPS) {
            for (int i = 0; i < N; i++) hub_next[i] /= hub_norm;
        }

        /* Check convergence: combined L1-norm change. */
        double delta = 0.0;
        for (int i = 0; i < N; i++) {
            delta += fabs(auth_next[i] - auth[i]);
            delta += fabs(hub_next[i] - hub[i]);
        }

        /* Swap. */
        double *tmp;
        tmp = auth; auth = auth_next; auth_next = tmp;
        tmp = hub; hub = hub_next; hub_next = tmp;

        if (delta < tolerance) {
            converged = true;
            final_delta = delta;
            break;
        }
        final_delta = delta;
    }

    struct timespec ts_end;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double compute_ms = ((double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0) +
                         ((double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0);

    /* Normalize to [0, 1] for output. */
    double auth_max = 0.0, hub_max = 0.0;
    for (int i = 0; i < N; i++) {
        if (auth[i] > auth_max) auth_max = auth[i];
        if (hub[i] > hub_max) hub_max = hub[i];
    }
    if (auth_max < PR_EPS) auth_max = 1.0;
    if (hub_max < PR_EPS) hub_max = 1.0;

    /* Fill result scores. */
    result->count = 0;
    for (int i = 0; i < N; i++) {
        float a_val = (float)(auth[i] / auth_max);
        float h_val = (float)(hub[i] / hub_max);
        if (a_val > (float)PR_EPS || h_val > (float)PR_EPS) {
            cbm_pr_scores_t *s = &result->scores[result->count++];
            s->node_id = g->nodes[i].node_id;
            s->pagerank = 0.0f;  /* Filled by PageRank if combined */
            s->authority = a_val;
            s->hub = h_val;
            s->combined = 0.5f * a_val + 0.5f * h_val;  /* Default 50/50 */
        }
    }

    result->iterations = iterations;
    result->converged = converged;
    result->final_delta = (float)final_delta;
    result->compute_time_ms = compute_ms;
    result->node_count = g->node_count;
    result->edge_count = g->total_edges;
    result->avg_out_degree = (float)g->avg_out_degree;

    cbm_log_info("hits", "complete",
                 "iterations", "%d", iterations,
                 "converged", converged ? "true" : "false",
                 "delta", "%.2e", final_delta,
                 "time_ms", "%.1f", compute_ms);

    free(auth); free(hub); free(auth_next); free(hub_next);
    pr_graph_free(g);

    return result;
}

/* ── Score Combination ──────────────────────────────────────────── */

int cbm_pr_combine(cbm_pr_result_t *pr,
                   const cbm_pr_result_t *hits,
                   double w_pr,
                   double w_auth,
                   double w_hub) {
    if (!pr || !hits) return -1;

    /* Default weights. */
    if (w_pr + w_auth + w_hub < PR_EPS) {
        w_pr = 0.5;
        w_auth = 0.3;
        w_hub = 0.2;
    }

    /* Normalize PageRank scores to [0, 1]. */
    float pr_max = 0.0f;
    for (int i = 0; i < pr->count; i++) {
        if (pr->scores[i].pagerank > pr_max) pr_max = pr->scores[i].pagerank;
    }
    if (pr_max < (float)PR_EPS) pr_max = 1.0f;

    /* Build a lookup for HITS scores by node_id.
     * Linear scan is fine for < 100K nodes; for larger graphs,
     * a hash table would be better. */
    for (int i = 0; i < pr->count; i++) {
        /* Normalize PageRank to [0, 1]. */
        float pr_norm = pr->scores[i].pagerank / pr_max;

        /* Find HITS scores for this node. */
        float auth_val = 0.0f;
        float hub_val = 0.0f;
        for (int j = 0; j < hits->count; j++) {
            if (hits->scores[j].node_id == pr->scores[i].node_id) {
                auth_val = hits->scores[j].authority;
                hub_val = hits->scores[j].hub;
                break;
            }
        }

        pr->scores[i].authority = auth_val;
        pr->scores[i].hub = hub_val;

        /* Weighted combination. */
        pr->scores[i].combined = (float)(w_pr * pr_norm +
                                         w_auth * auth_val +
                                         w_hub * hub_val);
    }

    return 0;
}

/* ── Lookup ─────────────────────────────────────────────────────── */

const cbm_pr_scores_t *cbm_pr_find(const cbm_pr_result_t *result,
                                    int64_t node_id) {
    if (!result || !result->scores) return NULL;

    /* Linear scan. For frequent lookups on large results,
     * a hash table would be better. */
    for (int i = 0; i < result->count; i++) {
        if (result->scores[i].node_id == node_id) {
            return &result->scores[i];
        }
    }
    return NULL;
}

float cbm_pr_combined_score(const cbm_pr_result_t *result, int64_t node_id) {
    const cbm_pr_scores_t *s = cbm_pr_find(result, node_id);
    return s ? s->combined : 0.0f;
}

/* ── Cleanup ────────────────────────────────────────────────────── */

void cbm_pr_result_free(cbm_pr_result_t *result) {
    if (!result) return;
    free(result->scores);
    free(result);
}
