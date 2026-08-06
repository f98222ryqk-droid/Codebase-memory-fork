/*
 * hybrid_retrieval.h — Hybrid Retrieval: Graph Constraints + Local Embeddings
 *
 * Optimizes semantic search by using the structural topology graph as a
 * hard filter. Constrains the local vector search space (powered by
 * nomic-embed-code) to specific code boundaries (e.g., only inside a
 * specific module or call chain) before executing the query.
 *
 * This slashes search latencies and blocks false-confidence hallucinations
 * by ensuring results are both semantically similar AND structurally
 * related.
 *
 * Architecture:
 * 1. Graph constraint parsing: identify the structural boundary (module, call chain, etc.)
 * 2. Candidate filtering: use BFS/DFS to find all nodes within the boundary
 * 3. Vector search: only search within the filtered candidate set
 * 4. Score fusion: combine structural proximity with semantic similarity
 * 5. Result ranking: return top-k results with both scores
 */
#ifndef CBM_HYBRID_RETRIEVAL_H
#define CBM_HYBRID_RETRIEVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable hybrid retrieval (default: true) */
#ifndef CBM_HYBRID_RETRIEVAL_ENABLED
#define CBM_HYBRID_RETRIEVAL_ENABLED 1
#endif

/* Weight for structural score in fusion (0.0 - 1.0) */
#ifndef CBM_HYBRID_STRUCT_WEIGHT
#define CBM_HYBRID_STRUCT_WEIGHT 0.3f
#endif

/* Weight for semantic score in fusion (0.0 - 1.0) */
#ifndef CBM_HYBRID_SEM_WEIGHT
#define CBM_HYBRID_SEM_WEIGHT 0.7f
#endif

/* Maximum candidates to consider from graph traversal */
#ifndef CBM_HYBRID_MAX_CANDIDATES
#define CBM_HYBRID_MAX_CANDIDATES 10000
#endif

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
    float structural_score;     /* Structural proximity (0.0 - 1.0) */
    float fused_score;          /* Combined score (0.0 - 1.0) */
    
    /* Context */
    int depth;                  /* Distance from constraint root */
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
    
    /* Query info */
    char *query;                /* Original query */
    cbm_hybrid_constraint_t *constraint;  /* Applied constraint */
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

/* Perform hybrid search with a constraint.
 * Returns heap-allocated results (caller frees with cbm_hybrid_results_free). */
cbm_hybrid_results_t *cbm_hybrid_search(cbm_store_t *store,
                                         const char *project,
                                         const char *query,
                                         const cbm_hybrid_constraint_t *constraint,
                                         int top_k);

/* Perform hybrid search without a constraint (pure semantic search).
 * Returns heap-allocated results (caller frees with cbm_hybrid_results_free). */
cbm_hybrid_results_t *cbm_hybrid_search_unconstrained(cbm_store_t *store,
                                                       const char *project,
                                                       const char *query,
                                                       int top_k);

/* Free hybrid search results. */
void cbm_hybrid_results_free(cbm_hybrid_results_t *results);

/* Get candidate node IDs from a constraint.
 * Returns heap-allocated array of node IDs (caller frees). */
int64_t *cbm_hybrid_get_candidates(cbm_store_t *store,
                                    const char *project,
                                    const cbm_hybrid_constraint_t *constraint,
                                    int *count);

/* Calculate structural score for a node relative to a constraint. */
float cbm_hybrid_structural_score(cbm_store_t *store,
                                    const char *project,
                                    int64_t node_id,
                                    const cbm_hybrid_constraint_t *constraint);

/* Fuse semantic and structural scores. */
float cbm_hybrid_fuse_scores(float semantic, float structural);

/* Format results as JSON. */
char *cbm_hybrid_results_json(const cbm_hybrid_results_t *results);

/* Format results as human-readable text. */
char *cbm_hybrid_results_text(const cbm_hybrid_results_t *results);

#endif /* CBM_HYBRID_RETRIEVAL_H */
