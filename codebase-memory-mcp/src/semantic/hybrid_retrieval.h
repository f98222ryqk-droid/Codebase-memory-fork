/*
 * hybrid_retrieval.h — Hybrid Retrieval: Graph Constraints + Local Embeddings
 *
 * Optimizes semantic search by using the structural topology graph as a
 * hard filter. Constrains the local vector search space (powered by
 * nomic-embed-code) to specific code boundaries (e.g., only inside a
 * specific module or call chain) before executing the query.
 *
 * Enhanced with Personalized PageRank for structural scoring:
 *   - Structural scores come from PPR instead of hardcoded placeholders
 *   - Score fusion uses RRF (Reciprocal Rank Fusion) in addition to
 *     weighted linear combination for more robust ranking
 *   - Token-efficient TOON output format for result serialization
 *
 * Architecture:
 * 1. Graph constraint parsing: identify the structural boundary
 * 2. Candidate filtering: use BFS/DFS to find all nodes within the boundary
 * 3. Vector search: only search within the filtered candidate set
 * 4. Personalized PageRank: compute structural importance from query seeds
 * 5. Score fusion: combine semantic + PPR + HITS via RRF + weighted linear
 * 6. Result ranking: return top-k results with all scores
 */
#ifndef CBM_HYBRID_RETRIEVAL_H
#define CBM_HYBRID_RETRIEVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
struct cbm_pr_result;  /* from semantic/pagerank.h */

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable hybrid retrieval (default: true) */
#ifndef CBM_HYBRID_RETRIEVAL_ENABLED
#define CBM_HYBRID_RETRIEVAL_ENABLED 1
#endif

/* Weight for structural score in linear fusion (0.0 - 1.0) */
#ifndef CBM_HYBRID_STRUCT_WEIGHT
#define CBM_HYBRID_STRUCT_WEIGHT 0.35f
#endif

/* Weight for semantic score in linear fusion (0.0 - 1.0) */
#ifndef CBM_HYBRID_SEM_WEIGHT
#define CBM_HYBRID_SEM_WEIGHT 0.65f
#endif

/* Maximum candidates to consider from graph traversal */
#ifndef CBM_HYBRID_MAX_CANDIDATES
#define CBM_HYBRID_MAX_CANDIDATES 10000
#endif

/* RRF (Reciprocal Rank Fusion) constant.
 * Higher K means smoother blending (less sensitive to rank position).
 * K=60 is the standard value from Cormack et al. (2009). */
#ifndef CBM_HYBRID_RRF_K
#define CBM_HYBRID_RRF_K 60
#endif

/* Fusion strategy: how to combine semantic and structural scores. */
typedef enum {
    CBM_HYBRID_FUSION_LINEAR = 0,  /* w_sem * sem + w_struct * struct */
    CBM_HYBRID_FUSION_RRF = 1,     /* Reciprocal Rank Fusion */
    CBM_HYBRID_FUSION_COMBINED = 2, /* Linear + RRF combined (default) */
} cbm_hybrid_fusion_strategy_t;

/* ── Constraint Types ────────────────────────────────────────────── */

typedef enum {
    CBM_HYBRID_CONSTRAINT_NONE = 0,
    CBM_HYBRID_CONSTRAINT_MODULE,       /* Search within a module/package */
    CBM_HYBRID_CONSTRAINT_CALL_CHAIN,   /* Search within a call chain */
    CBM_HYBRID_CONSTRAINT_FILE,         /* Search within a file */
    CBM_HYBRID_CONSTRAINT_DIRECTORY,    /* Search within a directory */
    CBM_HYBRID_CONSTRAINT_CLASS,        /* Search within a class hierarchy */
    CBM_HYBRID_CONSTRAINT_ROUTE,        /* Search within routes matching a pattern */
    CBM_HYBRID_CONSTRAINT_CUSTOM,       /* Custom constraint (node ID set) */
} cbm_hybrid_constraint_type_t;

/* ── Search Constraint ───────────────────────────────────────────── */

typedef struct {
    cbm_hybrid_constraint_type_t type;
    
    /* Parameters for the constraint */
    char *module_name;          /* Module/package name */
    char *file_path;            /* File path */
    char *directory_path;       /* Directory path */
    char *class_name;           /* Class name */
    char *route_pattern;        /* Route pattern (e.g., /api/...) */
    char *start_node_qn;        /* Starting node for call chain BFS */
    int max_depth;              /* Max BFS depth for call chain */
    
    /* Custom node set (for CBM_HYBRID_CONSTRAINT_CUSTOM) */
    int64_t *node_ids;
    int node_count;
} cbm_hybrid_constraint_t;

/* ── Search Result ───────────────────────────────────────────────── */

typedef struct {
    int64_t node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    
    /* Scores */
    float semantic_score;       /* Cosine similarity (0.0 - 1.0) */
    float structural_score;     /* PPR-based structural score (0.0 - 1.0) */
    float pagerank_score;       /* Raw PageRank value */
    float authority_score;      /* HITS authority value */
    float hub_score;            /* HITS hub value */
    float fused_score;          /* Combined score (0.0 - 1.0) */
    float rrf_score;            /* RRF component score */
    
    /* Context */
    int depth;                  /* Distance from constraint root */
    int sem_rank;               /* Rank by semantic score */
    int struct_rank;            /* Rank by structural score */
    char *relationship;         /* How this node relates to the constraint */
} cbm_hybrid_result_t;

/* ── Search Results ──────────────────────────────────────────────── */

typedef struct {
    cbm_hybrid_result_t *results;
    int count;
    int capacity;
    
    /* Search metadata */
    int candidates_considered;  /* Total candidates from graph filtering */
    int candidates_searched;    /* Candidates actually searched */
    double search_time_ms;      /* Total search time */
    double graph_time_ms;       /* Time for graph constraint */
    double vector_time_ms;      /* Time for vector search */
    double pagerank_time_ms;    /* Time for PageRank computation */
    
    /* PageRank metadata */
    int pagerank_iterations;    /* PageRank iterations used */
    bool pagerank_converged;    /* Whether PageRank converged */
    float effective_damping;    /* Damping factor used */
    
    /* Query info */
    char *query;                /* Original query */
    cbm_hybrid_constraint_t *constraint;  /* Applied constraint */
    cbm_hybrid_fusion_strategy_t fusion_strategy;  /* Fusion used */
} cbm_hybrid_results_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the hybrid retrieval subsystem. */
int cbm_hybrid_retrieval_init(void);

/* Shutdown the hybrid retrieval subsystem. */
void cbm_hybrid_retrieval_shutdown(void);

/* Create a search constraint. */
cbm_hybrid_constraint_t *cbm_hybrid_constraint_new(cbm_hybrid_constraint_type_t type);

/* Free a search constraint. */
void cbm_hybrid_constraint_free(cbm_hybrid_constraint_t *constraint);

/* Set constraint parameters. */
void cbm_hybrid_constraint_set_module(cbm_hybrid_constraint_t *c, const char *module);
void cbm_hybrid_constraint_set_file(cbm_hybrid_constraint_t *c, const char *file_path);
void cbm_hybrid_constraint_set_directory(cbm_hybrid_constraint_t *c, const char *dir_path);
void cbm_hybrid_constraint_set_class(cbm_hybrid_constraint_t *c, const char *class_name);
void cbm_hybrid_constraint_set_route(cbm_hybrid_constraint_t *c, const char *route_pattern);
void cbm_hybrid_constraint_set_call_chain(cbm_hybrid_constraint_t *c, const char *start_node_qn,
                                           int max_depth);

/* Perform hybrid search with a constraint and fusion strategy.
 * Returns heap-allocated results (caller frees with cbm_hybrid_results_free). */
cbm_hybrid_results_t *cbm_hybrid_search(cbm_store_t *store,
                                         const char *project,
                                         const char *query,
                                         const cbm_hybrid_constraint_t *constraint,
                                         int top_k);

/* Perform hybrid search with explicit fusion strategy. */
cbm_hybrid_results_t *cbm_hybrid_search_ex(cbm_store_t *store,
                                            const char *project,
                                            const char *query,
                                            const cbm_hybrid_constraint_t *constraint,
                                            int top_k,
                                            cbm_hybrid_fusion_strategy_t fusion);

/* Perform hybrid search without a constraint (pure semantic search).
 * Returns heap-allocated results (caller frees with cbm_hybrid_results_free). */
cbm_hybrid_results_t *cbm_hybrid_search_unconstrained(cbm_store_t *store,
                                                       const char *project,
                                                       const char *query,
                                                       int top_k);

/* Perform PageRank-enhanced hybrid search.
 * Takes pre-computed semantic search results as seeds for Personalized
 * PageRank, producing structurally-informed rankings. This is the
 * recommended entry point for retrieval. */
cbm_hybrid_results_t *cbm_hybrid_search_pagerank(cbm_store_t *store,
                                                  const char *project,
                                                  const char *query,
                                                  const int64_t *seed_ids,
                                                  const float *seed_scores,
                                                  int seed_count,
                                                  int top_k);

/* Free hybrid search results. */
void cbm_hybrid_results_free(cbm_hybrid_results_t *results);

/* Get candidate node IDs from a constraint.
 * Returns heap-allocated array of node IDs (caller frees). */
int64_t *cbm_hybrid_get_candidates(cbm_store_t *store,
                                    const char *project,
                                    const cbm_hybrid_constraint_t *constraint,
                                    int *count);

/* Calculate structural score for a node using PageRank.
 * If a PageRank result is available, uses PPR score.
 * Otherwise falls back to constraint-based proximity. */
float cbm_hybrid_structural_score(cbm_store_t *store,
                                    const char *project,
                                    int64_t node_id,
                                    const cbm_hybrid_constraint_t *constraint);

/* Fuse semantic and structural scores using linear combination. */
float cbm_hybrid_fuse_scores(float semantic, float structural);

/* Fuse scores using Reciprocal Rank Fusion (RRF).
 * RRF(d) = 1/(K + r_sem) + 1/(K + r_struct)
 * More robust than linear fusion when scores are on different scales. */
float cbm_hybrid_fuse_rrf(int sem_rank, int struct_rank);

/* Combined fusion: blend linear and RRF for best of both.
 * Returns 0.6 * linear_normalized + 0.4 * rrf_normalized. */
float cbm_hybrid_fuse_combined(float semantic, float structural,
                                int sem_rank, int struct_rank);

/* Sort results by fused score (descending). */
void cbm_hybrid_results_sort(cbm_hybrid_results_t *results);

/* ── Token-Efficient Output ─────────────────────────────────────── */

/* Format results in TOON (Token-Oriented Object Notation) for minimal
 * token consumption by LLM agents. Uses prefix grouping and compact
 * score formatting to cut 40-60% of tokens vs JSON. */
char *cbm_hybrid_results_toon(const cbm_hybrid_results_t *results);

/* Format results as JSON. */
char *cbm_hybrid_results_json(const cbm_hybrid_results_t *results);

/* Format results as human-readable text. */
char *cbm_hybrid_results_text(const cbm_hybrid_results_t *results);

#endif /* CBM_HYBRID_RETRIEVAL_H */
