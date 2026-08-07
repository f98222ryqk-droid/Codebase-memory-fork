/*
 * pagerank.h — Personalized PageRank + HITS for code graph ranking.
 *
 * Provides graph-theoretic centrality measures that capture the global
 * importance of code nodes beyond local BM25/semantic similarity:
 *
 *   1. Personalized PageRank (PPR): random-walk centrality with query-biased
 *      teleportation. Unlike vanilla PageRank (uniform teleport), PPR biases
 *      the walk toward query-relevant seed nodes, so "important" means
 *      "important AND relevant to this query" — exactly what code retrieval
 *      needs.
 *
 *   2. HITS (Hyperlink-Induced Topic Search): computes hub and authority
 *      scores. In code graphs, authorities are highly-called utilities;
 *      hubs are orchestrators that call many things. Both are useful
 *      ranking signals.
 *
 * Edge-type weighting: CALLS edges carry more weight than IMPORTS or USAGE,
 * reflecting that call dependencies are stronger structural bonds.
 *
 * All computation is iterative with configurable convergence tolerance
 * and early termination. Designed for graphs with 10K–500K nodes.
 */
#ifndef CBM_PAGERANK_H
#define CBM_PAGERANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Default damping factor for PageRank (probability of following a link
 * vs. teleporting). 0.85 is the classic value; lower values (0.6–0.7)
 * give more weight to the personalization vector, which is better for
 * query-focused retrieval. */
#ifndef CBM_PR_DAMPING
#define CBM_PR_DAMPING 0.85
#endif

/* Minimum damping factor (for sparse graphs — reduces random surfer). */
#ifndef CBM_PR_DAMPING_MIN
#define CBM_PR_DAMPING_MIN 0.60
#endif

/* Convergence tolerance: L1-norm change per iteration. */
#ifndef CBM_PR_TOLERANCE
#define CBM_PR_TOLERANCE 1e-6
#endif

/* Maximum PageRank iterations before forced termination. */
#ifndef CBM_PR_MAX_ITER
#define CBM_PR_MAX_ITER 100
#endif

/* Maximum HITS iterations. */
#ifndef CBM_HITS_MAX_ITER
#define CBM_HITS_MAX_ITER 50
#endif

/* HITS convergence tolerance. */
#ifndef CBM_HITS_TOLERANCE
#define CBM_HITS_TOLERANCE 1e-6
#endif

/* ── Edge-Type Weights ──────────────────────────────────────────── */
/* These weights reflect the strength of structural bonds in code graphs.
 * CALLS is the strongest: a function that calls you depends on your API.
 * IMPORTS is weaker: file-level dependency, may be transitive.
 * INHERITS/IMPLEMENTS are moderate: type hierarchy bonds.
 * USAGE is weakest: reference without invocation. */

#ifndef CBM_PR_W_CALLS
#define CBM_PR_W_CALLS 1.0
#endif

#ifndef CBM_PR_W_HTTP_CALLS
#define CBM_PR_W_HTTP_CALLS 0.9
#endif

#ifndef CBM_PR_W_IMPORTS
#define CBM_PR_W_IMPORTS 0.6
#endif

#ifndef CBM_PR_W_INHERITS
#define CBM_PR_W_INHERITS 0.7
#endif

#ifndef CBM_PR_W_IMPLEMENTS
#define CBM_PR_W_IMPLEMENTS 0.7
#endif

#ifndef CBM_PR_W_USAGE
#define CBM_PR_W_USAGE 0.4
#endif

#ifndef CBM_PR_W_CALL_REFERENCE
#define CBM_PR_W_CALL_REFERENCE 0.8
#endif

/* Default weight for unknown edge types. */
#ifndef CBM_PR_W_DEFAULT
#define CBM_PR_W_DEFAULT 0.5
#endif

/* ── Data Structures ────────────────────────────────────────────── */

/* A single node's PageRank + HITS scores. */
typedef struct {
    int64_t node_id;
    float pagerank;       /* Personalized PageRank score (sum ≈ 1.0) */
    float authority;      /* HITS authority score (L2-normalized) */
    float hub;            /* HITS hub score (L2-normalized) */
    float combined;       /* Weighted combination of all scores */
} cbm_pr_scores_t;

/* Full result from a PageRank + HITS computation. */
typedef struct {
    cbm_pr_scores_t *scores;    /* Array of per-node scores */
    int count;                  /* Number of nodes with non-zero scores */
    int capacity;               /* Allocated capacity */

    /* Convergence metadata */
    int iterations;             /* Iterations actually used */
    bool converged;             /* Whether convergence was reached */
    float final_delta;          /* Final L1-norm change */
    double compute_time_ms;     /* Computation time */

    /* Graph statistics */
    int node_count;             /* Total nodes in the graph */
    int edge_count;             /* Total edges in the graph */
    float avg_out_degree;       /* Average out-degree */
    float effective_damping;    /* Damping factor used (after adaptation) */
} cbm_pr_result_t;

/* ── Edge Weight Lookup ─────────────────────────────────────────── */

/* Get the weight for an edge type string. Returns CBM_PR_W_DEFAULT
 * for unknown types. */
double cbm_pr_edge_weight(const char *edge_type);

/* ── Personalized PageRank ──────────────────────────────────────── */

/* Run Personalized PageRank on the code graph.
 *
 * personalization: array of (node_id, weight) pairs forming the
 *   teleport distribution. Weights should be non-negative and will
 *   be normalized to sum to 1.0. Pass NULL for uniform teleport
 *   (vanilla PageRank).
 * pers_count: number of entries in personalization array.
 * damping: damping factor (0.0–1.0). Pass 0.0 to use the default.
 * tolerance: convergence tolerance. Pass 0.0 to use the default.
 * max_iter: max iterations. Pass 0 to use the default.
 *
 * Returns heap-allocated result (caller frees with cbm_pr_result_free).
 * Returns NULL on error. */
cbm_pr_result_t *cbm_pagerank(cbm_store_t *store,
                               const char *project,
                               const int64_t *personalization_ids,
                               const float *personalization_weights,
                               int pers_count,
                               double damping,
                               double tolerance,
                               int max_iter);

/* Convenience: run PageRank with uniform personalization (vanilla PageRank).
 * Ranks all nodes by global structural importance. */
cbm_pr_result_t *cbm_pagerank_uniform(cbm_store_t *store,
                                       const char *project,
                                       double damping,
                                       double tolerance,
                                       int max_iter);

/* Convenience: run query-biased PageRank.
 * Seeds are BM25/semantic search hits; teleport distribution is
 * proportional to their search scores. This is the primary entry
 * point for retrieval: nodes that are both structurally central
 * AND near the query-relevant seeds rank highest. */
cbm_pr_result_t *cbm_pagerank_query_biased(cbm_store_t *store,
                                            const char *project,
                                            const int64_t *seed_ids,
                                            const float *seed_scores,
                                            int seed_count,
                                            double damping,
                                            double tolerance,
                                            int max_iter);

/* ── HITS (Hubs and Authorities) ────────────────────────────────── */

/* Run HITS on the code graph to compute hub and authority scores.
 *
 * Uses the same edge-type weighting as PageRank.
 * Can be run standalone or combined with PageRank results via
 * cbm_pr_combine_scores().
 *
 * Returns heap-allocated result (caller frees). */
cbm_pr_result_t *cbm_hits(cbm_store_t *store,
                           const char *project,
                           double tolerance,
                           int max_iter);

/* ── Score Combination ──────────────────────────────────────────── */

/* Combine PageRank and HITS scores into a single ranking.
 *
 * w_pr: weight for PageRank component (default 0.5)
 * w_auth: weight for authority component (default 0.3)
 * w_hub: weight for hub component (default 0.2)
 *
 * The combined score is: w_pr * pagerank + w_auth * authority + w_hub * hub
 * (after normalizing each component to [0, 1]).
 *
 * Updates pr->scores[i].combined in place. Also sets pr->authority and
 * pr->hub from the hits result. Returns 0 on success, -1 on error. */
int cbm_pr_combine(cbm_pr_result_t *pr,
                   const cbm_pr_result_t *hits,
                   double w_pr,
                   double w_auth,
                   double w_hub);

/* ── Adaptive Damping ───────────────────────────────────────────── */

/* Compute an adaptive damping factor based on graph density.
 *
 * Sparse graphs (avg_degree < 2) benefit from lower damping (0.65–0.70)
 * to give more weight to the personalization vector, preventing
 * rank concentration on a few high-degree nodes.
 *
 * Dense graphs (avg_degree > 10) benefit from higher damping (0.85–0.90)
 * since the link structure carries more information.
 *
 * Returns damping in [CBM_PR_DAMPING_MIN, 0.90]. */
double cbm_pr_adaptive_damping(double avg_out_degree);

/* ── Lookup ─────────────────────────────────────────────────────── */

/* Find a node's PageRank score by node_id.
 * Returns pointer into the scores array (valid until result is freed),
 * or NULL if not found. */
const cbm_pr_scores_t *cbm_pr_find(const cbm_pr_result_t *result,
                                    int64_t node_id);

/* Get a node's combined score by node_id. Returns 0.0 if not found. */
float cbm_pr_combined_score(const cbm_pr_result_t *result, int64_t node_id);

/* ── Cleanup ────────────────────────────────────────────────────── */

/* Free a PageRank result. */
void cbm_pr_result_free(cbm_pr_result_t *result);

#endif /* CBM_PAGERANK_H */
