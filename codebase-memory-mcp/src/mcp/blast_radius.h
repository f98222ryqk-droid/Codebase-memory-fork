/*
 * blast_radius.h — Blast Radius & Change Impact Verification
 *
 * Analyzes git diffs to map out the explicit "blast radius" of a code
 * modification. Knowing what a change might break upstream before committing
 * is a massive token-saver for automated agents.
 *
 * Architecture:
 * 1. Parse git diff to identify changed symbols (functions, classes, etc.)
 * 2. Query the structural graph for all upstream dependents
 * 3. Calculate impact scores based on call chain depth and fan-out
 * 4. Generate a comprehensive impact report with risk assessment
 *
 * The module integrates with the existing detect_changes tool and extends
 * it with deeper analysis capabilities.
 */
#ifndef CBM_BLAST_RADIUS_H
#define CBM_BLAST_RADIUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_mcp_server cbm_mcp_server_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable blast radius analysis (default: true) */
#ifndef CBM_BLAST_RADIUS_ENABLED
#define CBM_BLAST_RADIUS_ENABLED 1
#endif

/* Maximum depth for upstream traversal */
#ifndef CBM_BLAST_RADIUS_MAX_DEPTH
#define CBM_BLAST_RADIUS_MAX_DEPTH 5
#endif

/* Maximum number of impacted symbols to report */
#ifndef CBM_BLAST_RADIUS_MAX_IMPACTED
#define CBM_BLAST_RADIUS_MAX_IMPACTED 100
#endif

/* Impact score thresholds */
#define CBM_BLAST_RADIUS_CRITICAL_THRESHOLD 0.8f
#define CBM_BLAST_RADIUS_HIGH_THRESHOLD 0.6f
#define CBM_BLAST_RADIUS_MEDIUM_THRESHOLD 0.4f

/* ── Data Structures ─────────────────────────────────────────────── */

/* A changed symbol (function, class, method, etc.) */
typedef struct {
    char *name;                 /* symbol name */
    char *qualified_name;       /* fully qualified name */
    char *file_path;            /* file path */
    int start_line;             /* start line in file */
    int end_line;               /* end line in file */
    char *change_type;          /* "added", "modified", "deleted" */
    char *symbol_type;          /* "Function", "Class", "Method", etc. */
} cbm_changed_symbol_t;

/* An impacted symbol (upstream dependent) */
typedef struct {
    char *name;                 /* symbol name */
    char *qualified_name;       /* fully qualified name */
    char *file_path;            /* file path */
    char *symbol_type;          /* "Function", "Class", "Method", etc. */
    int depth;                  /* distance from changed symbol */
    float impact_score;         /* 0.0 - 1.0 */
    char *relationship;         /* "CALLS", "IMPORTS", "INHERITS", etc. */
    char *risk_level;           /* "critical", "high", "medium", "low" */
} cbm_impacted_symbol_t;

/* Blast radius analysis result */
typedef struct {
    cbm_changed_symbol_t *changed_symbols;
    int changed_count;
    
    cbm_impacted_symbol_t *impacted_symbols;
    int impacted_count;
    
    float overall_risk_score;   /* 0.0 - 1.0 */
    char *overall_risk_level;   /* "critical", "high", "medium", "low" */
    
    int critical_count;         /* number of critical impacts */
    int high_count;             /* number of high impacts */
    int medium_count;           /* number of medium impacts */
    int low_count;              /* number of low impacts */
    
    char *summary_json;         /* JSON summary for MCP response */
} cbm_blast_radius_result_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the blast radius subsystem. */
int cbm_blast_radius_init(void);

/* Shutdown the blast radius subsystem. */
void cbm_blast_radius_shutdown(void);

/* Analyze the blast radius of changes in a git diff.
 *
 * server: MCP server handle
 * project: project name
 * diff_text: git diff text (from git diff command)
 *
 * Returns heap-allocated result (caller frees with cbm_blast_radius_result_free). */
cbm_blast_radius_result_t *cbm_blast_radius_analyze(cbm_mcp_server_t *server,
                                                      const char *project,
                                                      const char *diff_text);

/* Analyze blast radius from a list of changed files.
 *
 * server: MCP server handle
 * project: project name
 * changed_files: array of changed file paths
 * file_count: number of changed files
 *
 * Returns heap-allocated result (caller frees with cbm_blast_radius_result_free). */
cbm_blast_radius_result_t *cbm_blast_radius_analyze_files(cbm_mcp_server_t *server,
                                                           const char *project,
                                                           const char **changed_files,
                                                           int file_count);

/* Free a blast radius result. NULL-safe. */
void cbm_blast_radius_result_free(cbm_blast_radius_result_t *result);

/* Format a blast radius result as JSON for MCP response.
 * Returns heap-allocated JSON string (caller frees). */
char *cbm_blast_radius_format_json(const cbm_blast_radius_result_t *result);

/* Format a blast radius result as human-readable text.
 * Returns heap-allocated text (caller frees). */
char *cbm_blast_radius_format_text(const cbm_blast_radius_result_t *result);

/* Calculate impact score for a symbol based on its graph position. */
float cbm_blast_radius_calculate_impact(cbm_store_t *store, const char *project,
                                         const char *qualified_name, int depth);

/* Get the risk level string for an impact score. */
const char *cbm_blast_radius_risk_level(float impact_score);

#endif /* CBM_BLAST_RADIUS_H */
