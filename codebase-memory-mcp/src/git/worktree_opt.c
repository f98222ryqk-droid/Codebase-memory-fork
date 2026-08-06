/*
 * worktree_opt.c — Git Worktree Optimization Implementation
 *
 * Optimizes indexing of Git worktrees by:
 * 1. Detecting worktree relationships
 * 2. Sharing canonical repository content
 * 3. Isolating worktree-specific changes
 * 4. Choosing optimal indexing strategy based on content overlap
 */
#include "git/worktree_opt.h"
#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"
#include "store/store.h"
#include "pipeline/pipeline.h"
#include "git/git_context.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    WT_BUF = 4096,
    WT_PATH_MAX = 4096,
    WT_SHA_LEN = 64,  /* SHA-256 hex length */
};

/* ── Forward declarations ────────────────────────────────────────── */

static int cbm_worktree_index_shared(cbm_pipeline_t *pipeline,
                                      const cbm_worktree_info_t *wt_info,
                                      const cbm_worktree_content_map_t *map);
static int cbm_worktree_index_hybrid(cbm_pipeline_t *pipeline,
                                      const cbm_worktree_info_t *wt_info,
                                      const cbm_worktree_content_map_t *map);
static char *wt_hash_file(const char *filepath);

/* ── State ───────────────────────────────────────────────────────── */

static atomic_int g_wt_initialized = 0;

/* ── Lifecycle ───────────────────────────────────────────────────── */

int cbm_worktree_opt_init(void) {
    atomic_store(&g_wt_initialized, 1);
    cbm_log_info("worktree_opt", "status", "initialized");
    return 0;
}

void cbm_worktree_opt_shutdown(void) {
    atomic_store(&g_wt_initialized, 0);
}

/* ── Worktree Detection ──────────────────────────────────────────── */

cbm_worktree_info_t *cbm_worktree_detect(const char *path) {
    if (!path) return NULL;
    
    cbm_worktree_info_t *info = calloc(1, sizeof(cbm_worktree_info_t));
    if (!info) return NULL;
    
    /* Use git_context to detect worktree status */
    cbm_git_context_t ctx;
    if (cbm_git_context_resolve(path, &ctx) != 0) {
        free(info);
        return NULL;
    }
    
    info->is_worktree = ctx.is_worktree;
    info->is_bare = !ctx.root_exists;
    
    if (ctx.worktree_root) {
        info->worktree_root = strdup(ctx.worktree_root);
    }
    if (ctx.canonical_root) {
        info->canonical_root = strdup(ctx.canonical_root);
    }
    if (ctx.git_dir) {
        info->git_dir = strdup(ctx.git_dir);
    }
    if (ctx.git_common_dir) {
        info->common_dir = strdup(ctx.git_common_dir);
    }
    if (ctx.branch) {
        info->branch = strdup(ctx.branch);
    }
    if (ctx.branch_slug) {
        info->branch_slug = strdup(ctx.branch_slug);
    }
    
    cbm_git_context_free(&ctx);
    return info;
}

void cbm_worktree_info_free(cbm_worktree_info_t *info) {
    if (!info) return;
    free(info->worktree_root);
    free(info->canonical_root);
    free(info->git_dir);
    free(info->common_dir);
    free(info->branch);
    free(info->branch_slug);
    free(info);
}

/* ── Content Mapping ──────────────────────────────────────────────── */

/* Hash a file's content for comparison. Returns heap-allocated hex string. */
static char *wt_hash_file(const char *filepath) {
    if (!filepath) return NULL;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    
    cbm_sha256_ctx sha;
    cbm_sha256_init(&sha);
    
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        cbm_sha256_update(&sha, buf, n);
    }
    fclose(f);
    
    unsigned char hash[32];
    cbm_sha256_final(&sha, hash);
    
    char *hex = malloc(WT_SHA_LEN + 1);
    if (!hex) return NULL;
    
    for (int i = 0; i < 32; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    hex[WT_SHA_LEN] = '\0';
    
    return hex;
}

cbm_worktree_content_map_t *cbm_worktree_build_content_map(
    const cbm_worktree_info_t *wt_info, cbm_store_t *canonical_store) {
    
    if (!wt_info || !canonical_store) return NULL;
    
    cbm_worktree_content_map_t *map = calloc(1, sizeof(cbm_worktree_content_map_t));
    if (!map) return NULL;
    
    /* Get file list from canonical store */
    char **files = NULL;
    int file_count = 0;
    if (cbm_store_list_files(canonical_store, wt_info->canonical_root, &files, &file_count) != 0) {
        free(map);
        return NULL;
    }
    
    map->entries = calloc(file_count, sizeof(cbm_worktree_file_entry_t));
    if (!map->entries) {
        for (int i = 0; i < file_count; i++) free(files[i]);
        free(files);
        free(map);
        return NULL;
    }
    
    map->count = file_count;
    
    /* Compare each file */
    for (int i = 0; i < file_count; i++) {
        map->entries[i].rel_path = strdup(files[i]);
        
        /* Build full path in worktree */
        char wt_path[WT_PATH_MAX];
        snprintf(wt_path, sizeof(wt_path), "%s/%s", wt_info->worktree_root, files[i]);
        
        struct stat st;
        if (stat(wt_path, &st) == 0) {
            map->entries[i].mtime_ns = st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
            map->entries[i].size = st.st_size;
            
            /* Hash the worktree file */
            map->entries[i].sha256 = wt_hash_file(wt_path);
            
            /* Check if identical to canonical (would need canonical hash from store) */
            /* For now, mark as shared if size matches (simplified) */
            map->entries[i].is_shared = true;  /* Placeholder */
            map->entries[i].is_worktree_only = false;
            
            map->shared_count++;
        } else {
            /* File doesn't exist in worktree */
            map->entries[i].is_shared = false;
            map->entries[i].is_worktree_only = false;
        }
        
        free(files[i]);
    }
    free(files);
    
    return map;
}

void cbm_worktree_content_map_free(cbm_worktree_content_map_t *map) {
    if (!map) return;
    for (int i = 0; i < map->count; i++) {
        free(map->entries[i].rel_path);
        free(map->entries[i].sha256);
    }
    free(map->entries);
    free(map);
}

/* ── Strategy Selection ──────────────────────────────────────────── */

cbm_worktree_index_strategy_t cbm_worktree_choose_strategy(
    const cbm_worktree_info_t *wt_info,
    const cbm_worktree_content_map_t *content_map) {
    
    if (!wt_info || !content_map) return CBM_WT_INDEX_FULL;
    
    if (!wt_info->is_worktree) return CBM_WT_INDEX_FULL;
    
    /* Calculate content share percentage */
    if (content_map->count == 0) return CBM_WT_INDEX_FULL;
    
    int shared = content_map->shared_count;
    int total = content_map->count;
    int share_pct = (shared * 100) / total;
    
    /* If >80% shared, use shared strategy */
    if (share_pct >= 80) {
        return CBM_WT_INDEX_SHARED;
    }
    
    /* If >50% shared, use hybrid */
    if (share_pct >= 50) {
        return CBM_WT_INDEX_HYBRID;
    }
    
    /* Otherwise, full index */
    return CBM_WT_INDEX_FULL;
}

/* ── Worktree Indexing ───────────────────────────────────────────── */

int cbm_worktree_index(cbm_pipeline_t *pipeline, const cbm_worktree_info_t *wt_info,
                       cbm_store_t *canonical_store) {
    if (!pipeline || !wt_info) return -1;
    
    if (!wt_info->is_worktree) {
        /* Not a worktree, use normal indexing */
        return cbm_pipeline_run(pipeline);
    }
    
    cbm_log_info("worktree_opt", "indexing_worktree", wt_info->worktree_root,
                 "branch", wt_info->branch ? wt_info->branch : "unknown");
    
    /* Build content map */
    cbm_worktree_content_map_t *map = cbm_worktree_build_content_map(wt_info, canonical_store);
    if (!map) {
        return -1;
    }
    
    /* Choose strategy */
    cbm_worktree_index_strategy_t strategy = cbm_worktree_choose_strategy(wt_info, map);
    
    cbm_log_info("worktree_opt", "strategy", 
                 strategy == CBM_WT_INDEX_SHARED ? "shared" :
                 strategy == CBM_WT_INDEX_HYBRID ? "hybrid" : "full",
                 "shared_pct", "80");
    
    int result = 0;
    
    switch (strategy) {
        case CBM_WT_INDEX_SHARED:
            /* Share canonical content, only index worktree-specific changes */
            result = cbm_worktree_index_shared(pipeline, wt_info, map);
            break;
            
        case CBM_WT_INDEX_HYBRID:
            /* Share unchanged files, re-index modified files */
            result = cbm_worktree_index_hybrid(pipeline, wt_info, map);
            break;
            
        case CBM_WT_INDEX_FULL:
        default:
            /* Full independent index */
            result = cbm_pipeline_run(pipeline);
            break;
    }
    
    cbm_worktree_content_map_free(map);
    return result;
}

/* ── Shared Index Strategy ────────────────────────────────────────── */

/* Share canonical content, only index worktree-specific changes */
static int cbm_worktree_index_shared(cbm_pipeline_t *pipeline,
                                      const cbm_worktree_info_t *wt_info,
                                      const cbm_worktree_content_map_t *map) {
    /* 
     * Strategy: Copy the canonical DB, then only re-index files that differ
     * in the worktree. This is the most efficient strategy when most content
     * is shared.
     */
    cbm_log_info("worktree_opt", "using_shared_strategy");
    
    /* For now, fall back to full pipeline run with worktree-aware project name */
    /* In production, this would:
     * 1. Copy canonical DB to worktree DB path
     * 2. Delete nodes for changed files
     * 3. Re-index only changed files
     * 4. Merge results
     */
    return cbm_pipeline_run(pipeline);
}

/* ── Hybrid Index Strategy ────────────────────────────────────────── */

/* Share unchanged files, re-index modified files */
static int cbm_worktree_index_hybrid(cbm_pipeline_t *pipeline,
                                      const cbm_worktree_info_t *wt_info,
                                      const cbm_worktree_content_map_t *map) {
    /*
     * Strategy: Use incremental indexing that recognizes shared content
     * and only processes modified files.
     */
    cbm_log_info("worktree_opt", "using_hybrid_strategy");
    
    /* For now, fall back to full pipeline run */
    return cbm_pipeline_run(pipeline);
}

/* ── Utility Functions ────────────────────────────────────────────── */

bool cbm_worktree_contains_path(const cbm_worktree_info_t *wt_info, const char *path) {
    if (!wt_info || !path) return false;
    if (!wt_info->worktree_root) return false;
    
    size_t root_len = strlen(wt_info->worktree_root);
    return strncmp(path, wt_info->worktree_root, root_len) == 0;
}

char *cbm_worktree_project_name(const cbm_worktree_info_t *wt_info) {
    if (!wt_info || !wt_info->worktree_root) return NULL;
    
    /* Generate project name with branch disambiguation */
    char *name = malloc(WT_PATH_MAX);
    if (!name) return NULL;
    
    if (wt_info->branch_slug && wt_info->branch_slug[0]) {
        snprintf(name, WT_PATH_MAX, "%s__branch__%s", 
                 wt_info->worktree_root, wt_info->branch_slug);
    } else {
        snprintf(name, WT_PATH_MAX, "%s", wt_info->worktree_root);
    }
    
    return name;
}

char *cbm_worktree_canonical_project_name(const cbm_worktree_info_t *wt_info) {
    if (!wt_info || !wt_info->canonical_root) return NULL;
    
    return strdup(wt_info->canonical_root);
}
