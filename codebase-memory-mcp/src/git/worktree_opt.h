/*
 * worktree_opt.h — Git Worktree Optimization
 *
 * Optimizes how the engine indexes Git worktrees. When indexing a worktree,
 * this module:
 * 1. Recognizes the canonical (parent) repository
 * 2. Reuses identical repository content instead of creating a duplicated full index
 * 3. Safely isolates worktree-specific changes
 *
 * Architecture:
 * - Worktree detection via git rev-parse --git-dir vs --git-common-dir
 * - Content-addressed deduplication using SHA-256 file hashes
 * - Shared storage for canonical content, isolated storage for worktree-specific changes
 * - Reference counting for shared content lifecycle
 */
#ifndef CBM_WORKTREE_OPT_H
#define CBM_WORKTREE_OPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_pipeline cbm_pipeline_t;

/* ── Worktree info ──────────────────────────────────────────────── */

typedef struct {
    bool is_worktree;           /* true if this is a worktree */
    bool is_bare;               /* true if bare repository */
    char *worktree_root;        /* absolute path to this worktree */
    char *canonical_root;       /* absolute path to the canonical repo */
    char *git_dir;              /* .git directory for this worktree */
    char *common_dir;           /* shared .git directory */
    char *branch;               /* current branch */
    char *branch_slug;          /* sanitized branch name */
    uint64_t content_share_pct; /* % of content shared with canonical (0-100) */
} cbm_worktree_info_t;

/* ── Worktree index strategy ─────────────────────────────────────── */

typedef enum {
    CBM_WT_INDEX_FULL = 0,      /* Full independent index (fallback) */
    CBM_WT_INDEX_SHARED = 1,    /* Share canonical content, index worktree diff */
    CBM_WT_INDEX_HYBRID = 2,    /* Share unchanged files, re-index changed only */
} cbm_worktree_index_strategy_t;

/* ── Worktree content mapping ────────────────────────────────────── */

typedef struct {
    char *rel_path;             /* relative path from repo root */
    char *sha256;               /* file content hash */
    bool is_shared;             /* true if identical to canonical */
    bool is_worktree_only;      /* true if exists only in worktree */
    int64_t mtime_ns;           /* modification time */
    int64_t size;               /* file size */
} cbm_worktree_file_entry_t;

typedef struct {
    cbm_worktree_file_entry_t *entries;
    int count;
    int shared_count;           /* files identical to canonical */
    int worktree_only_count;    /* files unique to worktree */
    int modified_count;         /* files changed from canonical */
} cbm_worktree_content_map_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Detect worktree information for a given path.
 * Returns heap-allocated info (caller frees with cbm_worktree_info_free). */
cbm_worktree_info_t *cbm_worktree_detect(const char *path);

/* Free worktree info. NULL-safe. */
void cbm_worktree_info_free(cbm_worktree_info_t *info);

/* Build a content map comparing worktree to canonical repo.
 * Returns heap-allocated map (caller frees with cbm_worktree_content_map_free). */
cbm_worktree_content_map_t *cbm_worktree_build_content_map(
    const cbm_worktree_info_t *wt_info, cbm_store_t *canonical_store);

/* Free content map. NULL-safe. */
void cbm_worktree_content_map_free(cbm_worktree_content_map_t *map);

/* Choose the optimal indexing strategy for a worktree. */
cbm_worktree_index_strategy_t cbm_worktree_choose_strategy(
    const cbm_worktree_info_t *wt_info,
    const cbm_worktree_content_map_t *content_map);

/* Execute worktree-optimized indexing.
 * This is the main entry point for worktree indexing.
 * Returns 0 on success. */
int cbm_worktree_index(cbm_pipeline_t *pipeline, const cbm_worktree_info_t *wt_info,
                       cbm_store_t *canonical_store);

/* Check if a path is inside a worktree. */
bool cbm_worktree_contains_path(const cbm_worktree_info_t *wt_info, const char *path);

/* Get the project name for a worktree (includes branch disambiguation). */
char *cbm_worktree_project_name(const cbm_worktree_info_t *wt_info);

/* Get the canonical project name (without branch). */
char *cbm_worktree_canonical_project_name(const cbm_worktree_info_t *wt_info);

/* Initialize the worktree optimization subsystem. */
int cbm_worktree_opt_init(void);

/* Shutdown the worktree optimization subsystem. */
void cbm_worktree_opt_shutdown(void);

#endif /* CBM_WORKTREE_OPT_H */
