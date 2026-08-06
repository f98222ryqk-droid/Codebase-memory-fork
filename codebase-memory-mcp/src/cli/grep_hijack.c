/*
 * grep_hijack.c — Pre-Tool Interventions: intercept regex/file searches and
 * inject architectural context from the structural graph.
 *
 * This module implements the "grep hijack" feature that intercepts traditional
 * regex searches (grep, Glob, file searches) and enriches the results with
 * architectural context from the structural graph. This ensures AI agents
 * get graph-enhanced results even when they forget to use MCP tools.
 *
 * Architecture:
 * 1. Pattern analysis: extract meaningful identifiers from search patterns
 * 2. Graph query: search the structural graph for matching symbols
 * 3. Context injection: build rich context (call chains, dependencies, routes)
 * 4. Result merging: combine original results with injected context
 *
 * The module is designed to be non-blocking and fail-safe: any error results
 * in clean pass-through of the original search results.
 */
#include "cli/grep_hijack.h"
#include "foundation/constants.h"
#include "foundation/mem.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/compat.h"
#include "store/store.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────── */

enum {
    GH_TOKEN_MAX = 96,
    GH_CONTEXT_BUF = 8192,
    GH_ORIGINAL_MAX = 256 * 1024,  /* 256KB max original results */
    GH_RESULT_MAX = 512 * 1024,    /* 512KB max enriched results */
    GH_CALLER_CALLEE_LIMIT = 5,
};

/* ── State ───────────────────────────────────────────────────────── */

static atomic_int g_grep_hijack_initialized = 0;

/* ── Lifecycle ───────────────────────────────────────────────────── */

int cbm_grep_hijack_init(void) {
    atomic_store(&g_grep_hijack_initialized, 1);
    cbm_log_info("grep_hijack", "status", "initialized");
    return 0;
}

void cbm_grep_hijack_shutdown(void) {
    atomic_store(&g_grep_hijack_initialized, 0);
}

/* ── Tool detection ──────────────────────────────────────────────── */

bool cbm_grep_hijack_is_search_tool(const char *tool_name) {
    if (!tool_name) return false;
    
    /* Match common search tool names used by AI agents */
    return (strcmp(tool_name, "grep") == 0 ||
            strcmp(tool_name, "Grep") == 0 ||
            strcmp(tool_name, "Glob") == 0 ||
            strcmp(tool_name, "glob") == 0 ||
            strcmp(tool_name, "search") == 0 ||
            strcmp(tool_name, "Search") == 0 ||
            strcmp(tool_name, "file_search") == 0 ||
            strcmp(tool_name, "code_search") == 0);
}

/* ── Token extraction ────────────────────────────────────────────── */

char *cbm_grep_hijack_extract_token(const char *pattern) {
    if (!pattern) return NULL;
    
    size_t best_start = 0;
    size_t best_len = 0;
    size_t i = 0;
    size_t pat_len = strlen(pattern);
    
    while (i < pat_len) {
        /* Skip regex special chars and whitespace */
        if (isalpha((unsigned char)pattern[i]) || pattern[i] == '_') {
            size_t start = i;
            while (i < pat_len && (isalnum((unsigned char)pattern[i]) || pattern[i] == '_')) {
                i++;
            }
            size_t len = i - start;
            /* Prefer longer tokens, but cap at GH_TOKEN_MAX */
            if (len > best_len && len >= CBM_GREP_HIJACK_MIN_TOKEN) {
                best_len = len;
                best_start = start;
            }
        } else {
            i++;
        }
    }
    
    if (best_len < CBM_GREP_HIJACK_MIN_TOKEN) {
        return NULL;
    }
    
    size_t copy_len = best_len;
    if (copy_len > GH_TOKEN_MAX) {
        copy_len = GH_TOKEN_MAX;
    }
    
    char *token = malloc(copy_len + 1);
    if (!token) return NULL;
    
    memcpy(token, pattern + best_start, copy_len);
    token[copy_len] = '\0';
    return token;
}

/* ── Graph query ─────────────────────────────────────────────────── */

/* Query the graph for symbols matching a token. Returns JSON result string
 * (heap, caller frees) or NULL. */
static char *gh_query_graph(cbm_mcp_server_t *server, const char *project,
                            const char *token) {
    if (!server || !project || !token) return NULL;
    
    /* Build search_graph args: {"project":..,"name_pattern":".*token.*","limit":N} */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    
    char name_pattern[GH_TOKEN_MAX + 8];
    snprintf(name_pattern, sizeof(name_pattern), ".*%s.*", token);
    
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "name_pattern", name_pattern);
    yyjson_mut_obj_add_int(doc, root, "limit", CBM_GREP_HIJACK_MAX_SYMBOLS);
    yyjson_mut_obj_add_str(doc, root, "format", "json");
    
    char *args = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    
    if (!args) return NULL;
    
    char *result = cbm_mcp_handle_tool(server, "search_graph", args);
    free(args);
    return result;
}

/* ── Context builder ─────────────────────────────────────────────── */

/* Parse the search_graph JSON result and build a compact context string.
 * Returns heap-allocated context (caller frees) or NULL. */
static char *gh_parse_search_result(const char *envelope, const char *token) {
    if (!envelope || !token) return NULL;
    
    yyjson_doc *edoc = yyjson_read(envelope, strlen(envelope), 0);
    if (!edoc) return NULL;
    
    yyjson_val *eroot = yyjson_doc_get_root(edoc);
    yyjson_val *err = yyjson_obj_get(eroot, "isError");
    if (err && yyjson_is_true(err)) {
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    yyjson_val *content = yyjson_obj_get(eroot, "content");
    yyjson_val *item0 = (content && yyjson_is_arr(content)) ? yyjson_arr_get(content, 0) : NULL;
    if (!item0) {
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    yyjson_val *text_val = yyjson_obj_get(item0, "text");
    if (!text_val || !yyjson_is_str(text_val)) {
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    const char *inner = yyjson_get_str(text_val);
    yyjson_doc *idoc = yyjson_read(inner, strlen(inner), 0);
    if (!idoc) {
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    yyjson_val *iroot = yyjson_doc_get_root(idoc);
    yyjson_val *groups = yyjson_obj_get(iroot, "groups");
    if (!groups || !yyjson_is_arr(groups)) {
        yyjson_doc_free(idoc);
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    /* Count total results */
    size_t nres = 0;
    size_t gidx, gmax;
    yyjson_val *g;
    yyjson_arr_foreach(groups, gidx, gmax, g) {
        yyjson_val *rows = yyjson_obj_get(g, "rows");
        if (rows && yyjson_is_arr(rows)) {
            nres += yyjson_arr_size(rows);
        }
    }
    
    if (nres == 0) {
        yyjson_doc_free(idoc);
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    /* Build context string */
    char *ctx = malloc(GH_CONTEXT_BUF);
    if (!ctx) {
        yyjson_doc_free(idoc);
        yyjson_doc_free(edoc);
        return NULL;
    }
    
    int off = 0;
    off += snprintf(ctx + off, GH_CONTEXT_BUF - off,
                    "\n\n"
                    "═══════════════════════════════════════════════════════\n"
                    "  📊 Graph Context: %zu symbol(s) match \"%s\"\n"
                    "═══════════════════════════════════════════════════════\n",
                    nres, token);
    
    /* Iterate groups and rows */
    yyjson_arr_foreach(groups, gidx, gmax, g) {
        const char *prefix = NULL;
        yyjson_val *prefix_val = yyjson_obj_get(g, "qn_prefix");
        if (prefix_val && yyjson_is_str(prefix_val)) {
            prefix = yyjson_get_str(prefix_val);
        }
        
        const char *fp = NULL;
        yyjson_val *fp_val = yyjson_obj_get(g, "file");
        if (fp_val && yyjson_is_str(fp_val)) {
            fp = yyjson_get_str(fp_val);
        }
        
        yyjson_val *rows = yyjson_obj_get(g, "rows");
        if (!rows || !yyjson_is_arr(rows)) continue;
        
        size_t ridx, rmax;
        yyjson_val *row;
        yyjson_arr_foreach(rows, ridx, rmax, row) {
            if (off >= GH_CONTEXT_BUF - 256) break;
            
            yyjson_val *nm_val = yyjson_arr_get(row, 0);
            yyjson_val *lb_val = yyjson_arr_get(row, 1);
            
            const char *nm = (nm_val && yyjson_is_str(nm_val)) ? yyjson_get_str(nm_val) : "";
            const char *lb = (lb_val && yyjson_is_str(lb_val)) ? yyjson_get_str(lb_val) : "";
            
            char disp[GH_TOKEN_MAX + 256];
            if (prefix && prefix[0] && nm && nm[0]) {
                snprintf(disp, sizeof(disp), "%s.%s", prefix, nm);
            } else {
                snprintf(disp, sizeof(disp), "%s", nm);
            }
            
            off += snprintf(ctx + off, GH_CONTEXT_BUF - off,
                            "  • %-40s %-15s %s\n",
                            disp, lb && lb[0] ? lb : "", fp && fp[0] ? fp : "");
        }
    }
    
    off += snprintf(ctx + off, GH_CONTEXT_BUF - off,
                    "═══════════════════════════════════════════════════════\n"
                    "  💡 Use 'search_graph' or 'trace_path' for deeper analysis\n"
                    "═══════════════════════════════════════════════════════\n");
    
    yyjson_doc_free(idoc);
    yyjson_doc_free(edoc);
    return ctx;
}

/* ── Main processing ─────────────────────────────────────────────── */
/* (public API implementation) */

cbm_grep_hijack_result_t *cbm_grep_hijack_process(cbm_mcp_server_t *server,
                                                   const char *tool_name,
                                                   const char *pattern,
                                                   const char *original_results,
                                                   const char *project) {
    if (!atomic_load(&g_grep_hijack_initialized)) {
        return NULL;
    }
    
    if (!cbm_grep_hijack_is_search_tool(tool_name)) {
        return NULL;
    }
    
    if (!pattern || !original_results || !project) {
        return NULL;
    }
    
    /* Extract token from pattern */
    char *token = cbm_grep_hijack_extract_token(pattern);
    if (!token) {
        return NULL;
    }
    
    /* Query the graph */
    char *graph_result = gh_query_graph(server, project, token);
    if (!graph_result) {
        free(token);
        return NULL;
    }
    
    /* Parse and build context */
    char *context = gh_parse_search_result(graph_result, token);
    free(graph_result);
    
    if (!context) {
        free(token);
        return NULL;
    }
    
    /* Build enriched result */
    size_t orig_len = strlen(original_results);
    size_t ctx_len = strlen(context);
    size_t total_len = orig_len + ctx_len + 1;
    
    if (total_len > GH_RESULT_MAX) {
        /* Cap the enriched result */
        total_len = GH_RESULT_MAX;
    }
    
    cbm_grep_hijack_result_t *result = calloc(1, sizeof(cbm_grep_hijack_result_t));
    if (!result) {
        free(context);
        free(token);
        return NULL;
    }
    
    result->enriched_text = malloc(total_len);
    if (!result->enriched_text) {
        free(result);
        free(context);
        free(token);
        return NULL;
    }
    
    /* Combine original results with injected context */
    memcpy(result->enriched_text, original_results, orig_len);
    size_t ctx_copy = (orig_len + ctx_len > GH_RESULT_MAX) ? 
                      (GH_RESULT_MAX - orig_len) : ctx_len;
    memcpy(result->enriched_text + orig_len, context, ctx_copy);
    result->enriched_text[orig_len + ctx_copy] = '\0';
    
    result->symbols_found = 1;  /* At least one symbol matched */
    result->injected = true;
    
    free(context);
    free(token);
    
    cbm_log_info("grep_hijack", "injected", "true", "tool", tool_name);
    
    return result;
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

void cbm_grep_hijack_result_free(cbm_grep_hijack_result_t *result) {
    if (!result) return;
    free(result->enriched_text);
    free(result);
}
