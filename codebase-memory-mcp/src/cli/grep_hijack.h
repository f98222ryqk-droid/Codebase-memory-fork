/*
 * grep_hijack.h — Pre-Tool Interventions: intercept regex/file searches and
 * inject architectural context from the structural graph.
 *
 * When an AI agent calls standard grep or file searches, this module
 * intercepts the search string, queries the local structural graph, and
 * injects rich architectural context directly into the grep results.
 * This stops agents from forgetting about MCP tools by making traditional
 * searches "just work" with graph-enhanced results.
 *
 * The hijack operates as a pre-tool hook that:
 * 1. Intercepts grep/Glob/search tool calls before they execute
 * 2. Extracts meaningful search tokens from the pattern
 * 3. Queries the structural graph for matching symbols
 * 4. Injects architectural context (call chains, dependencies, routes)
 *    into the search results
 *
 * Cardinal rule: this NEVER blocks the original search. Every error results
 * in clean pass-through of the original results.
 */
#ifndef CBM_GREP_HIJACK_H
#define CBM_GREP_HIJACK_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct cbm_store cbm_store_t;
typedef struct cbm_mcp_server cbm_mcp_server_t;

/* ── Configuration ──────────────────────────────────────────────── */

/* Enable/disable grep hijacking (default: true) */
#ifndef CBM_GREP_HIJACK_ENABLED
#define CBM_GREP_HIJACK_ENABLED 1
#endif

/* Maximum number of graph symbols to inject per hijack */
#ifndef CBM_GREP_HIJACK_MAX_SYMBOLS
#define CBM_GREP_HIJACK_MAX_SYMBOLS 8
#endif

/* Minimum token length to trigger graph lookup (avoids noisy short patterns) */
#ifndef CBM_GREP_HIJACK_MIN_TOKEN
#define CBM_GREP_HIJACK_MIN_TOKEN 4
#endif

/* Maximum depth for call chain context (callers + callees) */
#ifndef CBM_GREP_HIJACK_MAX_CHAIN_DEPTH
#define CBM_GREP_HIJACK_MAX_CHAIN_DEPTH 2
#endif

/* ── Hijack result ──────────────────────────────────────────────── */

typedef struct {
    char *enriched_text;    /* Original results + injected context (heap) */
    int symbols_found;      /* Number of graph symbols matched */
    bool injected;          /* Whether context was actually injected */
} cbm_grep_hijack_result_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the grep hijack subsystem. Must be called before any other
 * grep_hijack function. Returns 0 on success. */
int cbm_grep_hijack_init(void);

/* Shutdown and free all resources. NULL-safe. */
void cbm_grep_hijack_shutdown(void);

/* Process a search tool call and optionally inject graph context.
 *
 * server: MCP server handle (for store access)
 * tool_name: the search tool being invoked ("grep", "Grep", "Glob", etc.)
 * pattern: the search pattern or glob
 * original_results: the text results from the actual search (heap)
 * project: project name (heap, owned by caller)
 *
 * Returns a result struct with enriched_text (caller frees) or NULL on error.
 * On error or no-match, original_results is returned unmodified. */
cbm_grep_hijack_result_t *cbm_grep_hijack_process(cbm_mcp_server_t *server,
                                                   const char *tool_name,
                                                   const char *pattern,
                                                   const char *original_results,
                                                   const char *project);

/* Free a hijack result. NULL-safe. */
void cbm_grep_hijack_result_free(cbm_grep_hijack_result_t *result);

/* Check if a tool name is a search tool that should be hijacked. */
bool cbm_grep_hijack_is_search_tool(const char *tool_name);

/* Extract the longest identifier-like token from a search pattern.
 * Returns heap-allocated token (caller frees) or NULL. */
char *cbm_grep_hijack_extract_token(const char *pattern);

/* Build architectural context for a matched symbol.
 * Returns heap-allocated context string (caller frees) or NULL. */
char *cbm_grep_hijack_build_context(cbm_store_t *store, const char *project,
                                    const char *symbol_name, int depth);

#endif /* CBM_GREP_HIJACK_H */
