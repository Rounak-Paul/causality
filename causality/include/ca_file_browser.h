// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#ifndef CA_FILE_BROWSER_H
#define CA_FILE_BROWSER_H

#include "causality.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * File browser window: navigate folders (back/forward/up/home, typed path), filter by extension and
 * pick files or a folder. It is a regular Causality window styled by the instance's --ca-* tokens, so
 * it follows the application's theme. One browser serves one request at a time; showing a new request
 * replaces the previous one.
 */
typedef struct Ca_FileBrowser Ca_FileBrowser;

typedef enum Ca_FileBrowserMode {
    CA_FILE_BROWSER_OPEN_FILES,     /* choose one or more existing files */
    CA_FILE_BROWSER_SELECT_FOLDER   /* choose a folder (the selected one, else the current one) */
} Ca_FileBrowserMode;

/* Extension filter; extensions are lowercase without the dot. Zero extensions matches every file. */
typedef struct Ca_FileFilter {
    const char *label;
    const char *const *extensions;
    size_t extension_count;
} Ca_FileFilter;

/*
 * Confirmation callback, invoked after the browser window has been asked to close.
 *
 * paths      UTF-8 absolute paths, borrowed for the duration of the call.
 * count      Number of paths, always > 0.
 * user_data  Pointer supplied in Ca_FileBrowserDesc.
 */
typedef void (*Ca_FileBrowserFn)(const char *const *paths, size_t count, void *user_data);

/*
 * Optional notification that the window closed. Fires once per request, after on_confirm when a
 * selection was made, and also on cancel or when the user closes the window.
 */
typedef void (*Ca_FileBrowserClosedFn)(void *user_data);

#define CA_FILE_BROWSER_MAX_FILTERS 8
#define CA_FILE_BROWSER_MAX_EXTENSIONS 32

typedef struct Ca_FileBrowserDesc {
    Ca_FileBrowserMode mode;
    const char *title;              /* window title; NULL picks one from mode */
    const char *confirm_label;      /* confirm button; NULL picks one from mode */
    const char *initial_path;       /* starting folder; NULL or unreadable starts at home */
    const Ca_FileFilter *filters;   /* copied; the first is active initially */
    size_t filter_count;            /* at most CA_FILE_BROWSER_MAX_FILTERS */
    bool multiple;                  /* OPEN_FILES: modifier-click selects several files */
    Ca_FileBrowserFn on_confirm;    /* required */
    Ca_FileBrowserClosedFn on_closed; /* optional */
    void *user_data;
} Ca_FileBrowserDesc;

/* Create a hidden browser on instance; NULL on failure. */
CA_API Ca_FileBrowser *ca_file_browser_create(Ca_Instance *instance);

/* Close the window, drop any pending request and free. NULL is safe. */
CA_API void ca_file_browser_destroy(Ca_FileBrowser *browser);

/* Open (or refocus) the browser window for a request; false on invalid input or window failure. */
CA_API bool ca_file_browser_show(Ca_FileBrowser *browser, const Ca_FileBrowserDesc *desc);

/* Close the window without confirming. */
CA_API void ca_file_browser_close(Ca_FileBrowser *browser);

/* Report whether the browser window is open. */
CA_API bool ca_file_browser_is_open(const Ca_FileBrowser *browser);

#endif
