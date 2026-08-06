/*
 * incremental_parse.h — Incremental Parsing Overhaul
 *
 * Fine-tunes the background watcher to utilize Tree-sitter's native
 * incremental parsing capabilities. Instead of re-parsing an altered file
 * entirely, this module:
 *
 * 1. Tracks which files have changed via the watcher
 * 2. Uses tree-sitter's incremental parsing to update only dirty AST nodes
 * 3. Pushes only the changed nodes/edges to the SQLite WAL layer
 * 4. Minimizes memory allocations and database writes
 *
 * Architecture:
 * - Dirty node tracking: identify which AST nodes have changed
 * - Partial re-extraction: only extract definitions from changed nodes
 * - Delta graph buffer: maintain a buffer of changes, not full re-parse
 * - WAL-optimized writes: batch SQLite writes for changed content only
 */
#ifndef CBM_INCREMENTAL_PARSE_H
#define CBM_INCREMENTAL_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_pipeline cbm_pipeline_t;
typedef struct cbm_watcher cbm_watcher_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable incremental parsing (default: true) */
#ifndef CBM_INCREMENTAL_PARSE_ENABLED
#define CBM_INCREMENTAL_PARSE_ENABLED 1
#endif

/* Maximum number of files to incrementally parse per cycle */
#ifndef CBM_INCREMENTAL_PARSE_MAX_FILES
#define CBM_INCREMENTAL_PARSE_MAX_FILES 50
#endif

/* Maximum number of dirty nodes per file */
#ifndef CBM_INCREMENTAL_PARSE_MAX_DIRTY_NODES
#define CBM_INCREMENTAL_PARSE_MAX_DIRTY_NODES 100
#endif

/* ── Dirty Node Tracking ────────────────────────────────────────── */

typedef struct {
    uint32_t start_byte;        /* start byte in source */
    uint32_t end_byte;          /* end byte in source */
    uint32_t start_point_row;   /* start line */
    uint32_t start_point_col;   /* start column */
    uint32_t end_point_row;     /* end line */
    uint32_t end_point_col;     /* end column */
    bool is_removed;            /* true if node was deleted */
    bool is_added;              /* true if node was added */
} cbm_dirty_range_t;

typedef struct {
    char *rel_path;             /* relative file path */
    char *language;             /* language identifier */
    cbm_dirty_range_t *dirty_ranges;
    int dirty_count;
    int dirty_capacity;
    
    /* Old and new AST for comparison */
    void *old_ast;              /* tree_sitter tree (opaque) */
    void *new_ast;              /* tree_sitter tree (opaque) */
    
    /* Source content */
    char *old_source;           /* previous source */
    char *new_source;           /* current source */
    size_t old_source_len;
    size_t new_source_len;
} cbm_dirty_file_t;

/* ── Incremental Parse Result ────────────────────────────────────── */

typedef struct {
    char *rel_path;             /* file path */
    int nodes_added;            /* new nodes created */
    int nodes_removed;          /* nodes deleted */
    int nodes_modified;         /* nodes updated */
    int edges_added;            /* new edges created */
    int edges_removed;          /* edges deleted */
    int edges_modified;         /* edges updated */
    
    /* Performance metrics */
    double parse_time_ms;       /* time for incremental parse */
    double extract_time_ms;     /* time for extraction */
    double db_time_ms;          /* time for database writes */
    size_t bytes_changed;       /* total bytes changed */
} cbm_incremental_result_t;

/* ── Incremental Parse Context ───────────────────────────────────── */

typedef struct {
    cbm_dirty_file_t *files;
    int file_count;
    int file_capacity;
    
    cbm_incremental_result_t *results;
    int result_count;
    
    /* Statistics */
    int total_nodes_added;
    int total_nodes_removed;
    int total_nodes_modified;
    int total_edges_added;
    int total_edges_removed;
    int total_edges_modified;
    
    /* Performance */
    double total_parse_time_ms;
    double total_extract_time_ms;
    double total_db_time_ms;
} cbm_incremental_ctx_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the incremental parsing subsystem. */
int cbm_incremental_parse_init(void);

/* Shutdown the incremental parsing subsystem. */
void cbm_incremental_parse_shutdown(void);

/* Create a new incremental parse context. */
cbm_incremental_ctx_t *cbm_incremental_ctx_new(void);

/* Free an incremental parse context. */
void cbm_incremental_ctx_free(cbm_incremental_ctx_t *ctx);

/* Add a dirty file to the context. */
int cbm_incremental_add_file(cbm_incremental_ctx_t *ctx,
                              const char *rel_path,
                              const char *language,
                              const char *old_source,
                              size_t old_source_len,
                              const char *new_source,
                              size_t new_source_len);

/* Detect dirty ranges between old and new source.
 * Returns heap-allocated array of dirty ranges (caller frees). */
cbm_dirty_range_t *cbm_incremental_detect_dirty(cbm_dirty_file_t *file,
                                                  int *count);

/* Perform incremental parse on a single file.
 * Returns heap-allocated result (caller frees). */
cbm_incremental_result_t *cbm_incremental_parse_file(cbm_store_t *store,
                                                       cbm_dirty_file_t *file);

/* Perform incremental parse on all dirty files in context.
 * Returns 0 on success. */
int cbm_incremental_parse_all(cbm_store_t *store, cbm_incremental_ctx_t *ctx);

/* Apply incremental results to the database.
 * Returns 0 on success. */
int cbm_incremental_apply_results(cbm_store_t *store,
                                    cbm_incremental_ctx_t *ctx);

/* Get the incremental parse statistics as JSON. */
char *cbm_incremental_stats_json(cbm_incremental_ctx_t *ctx);

/* Check if incremental parsing is available for a language. */
bool cbm_incremental_parse_available(const char *language);

/* Compare two tree-sitter ASTs and find changed nodes.
 * Returns heap-allocated array of dirty ranges (caller frees). */
cbm_dirty_range_t *cbm_incremental_compare_ast(void *old_tree, void *new_tree,
                                                 int *count);

/* Extract definitions from a specific AST node range. */
int cbm_incremental_extract_node(cbm_store_t *store,
                                   const char *rel_path,
                                   const char *language,
                                   void *node,
                                   cbm_incremental_result_t *result);

#endif /* CBM_INCREMENTAL_PARSE_H */
