// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* file_browser.c — themed file browser window built from Causality widgets. */
#include "ca_file_browser.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#endif

#define HISTORY 32
#define PATH_CAPACITY 4096
#define ROW_LIMIT 1000
#define WINDOW_WIDTH 720
#define WINDOW_HEIGHT 520
#define LIST_ID "ca-file-browser-list"

typedef struct FileEntry {
    char *name;
    bool directory, selected;
    int64_t size;
} FileEntry;

typedef struct FileRow {
    struct Ca_FileBrowser *browser;
    size_t index;
} FileRow;

typedef struct StoredFilter {
    char label[64];
    char extensions[CA_FILE_BROWSER_MAX_EXTENSIONS][16];
    size_t count;
} StoredFilter;

struct Ca_FileBrowser {
    Ca_Instance *instance;
    Ca_Window *window;
    Ca_Div *root;
    Ca_FileBrowserMode mode;
    bool multiple, show_hidden;
    char title[96], confirm_label[32];
    Ca_FileBrowserFn on_confirm;
    Ca_FileBrowserClosedFn on_closed;
    void *user_data;
    StoredFilter filters[CA_FILE_BROWSER_MAX_FILTERS];
    const char *filter_labels[CA_FILE_BROWSER_MAX_FILTERS];
    size_t filter_count, active_filter;
    char current[PATH_CAPACITY], draft[PATH_CAPACITY];
    char history[HISTORY][PATH_CAPACITY];
    size_t history_count, history_pos;
    FileEntry *entries;
    size_t entry_count, entry_capacity, anchor;
    FileRow *rows;
    size_t row_capacity;
    char message[256];
};

typedef bool (*EntryFn)(const char *name, bool directory, void *user_data);

/* ---- Filesystem access (UTF-8 paths on every platform) ---- */

/** Returns whether c separates path components on this platform. */
static bool is_separator(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

/** Returns the length of the root prefix ("/" or "C:\"), zero for relative paths. */
static size_t root_length(const char *path)
{
#ifdef _WIN32
    if (path[0] && path[1] == ':' && is_separator(path[2])) return 3;
#endif
    return is_separator(path[0]) ? 1 : 0;
}

/**
 * Joins a directory and a name with the platform separator.
 * Returns false and leaves out empty when the result does not fit in cap.
 */
static bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    size_t dir_length = strlen(dir), name_length = strlen(name);
    bool separator = dir_length > 0 && !is_separator(dir[dir_length - 1]);
    size_t total = dir_length + (separator ? 1u : 0u) + name_length;
    if (total >= cap) return false;
    memcpy(out, dir, dir_length);
    size_t at = dir_length;
    if (separator) {
#ifdef _WIN32
        out[at++] = '\\';
#else
        out[at++] = '/';
#endif
    }
    memcpy(out + at, name, name_length);
    out[total] = '\0';
    return true;
}

#ifdef _WIN32

/** Converts UTF-8 to a heap-allocated wide string the caller frees. */
static wchar_t *widen(const char *utf8)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(wchar_t));
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, count) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

/** Returns whether a file or folder exists at path. */
static bool path_exists(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide) return false;
    bool ok = GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
    free(wide);
    return ok;
}

/** Returns the size of a regular file, or -1 for folders and unreadable paths. */
static int64_t file_size(const char *path)
{
    wchar_t *wide = widen(path);
    if (!wide) return -1;
    WIN32_FILE_ATTRIBUTE_DATA data;
    BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
    free(wide);
    if (!ok || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
}

/** Visits every entry of a folder; returns false when it cannot be opened. */
static bool list_directory(const char *path, EntryFn fn, void *user_data)
{
    char pattern[PATH_CAPACITY];
    if (!join_path(pattern, sizeof pattern, path, "*")) return false;
    wchar_t *wide = widen(pattern);
    if (!wide) return false;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(wide, &entry);
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    char name[1024];
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, (int)sizeof name, NULL, NULL) <= 0) continue;
        if (!fn(name, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user_data)) break;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return true;
}

/** Resolves path to an absolute UTF-8 path of at most cap bytes. */
static bool absolute_path(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    wchar_t *wide = widen(path);
    wchar_t *absolute = wide ? _wfullpath(NULL, wide, 0) : NULL;
    free(wide);
    int size = absolute ? WideCharToMultiByte(CP_UTF8, 0, absolute, -1, NULL, 0, NULL, NULL) : 0;
    bool ok = size > 0 && (size_t)size <= cap &&
              WideCharToMultiByte(CP_UTF8, 0, absolute, -1, out, size, NULL, NULL) != 0;
    free(absolute);
    return ok;
}

#else

/** Returns whether a file or folder exists at path. */
static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

/** Returns whether path is a folder, following symbolic links. */
static bool is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/** Returns the size of a regular file, or -1 for folders and unreadable paths. */
static int64_t file_size(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return -1;
    return (int64_t)info.st_size;
}

/** Visits every entry of a folder; returns false when it cannot be opened. */
static bool list_directory(const char *path, EntryFn fn, void *user_data)
{
    DIR *directory = opendir(path);
    if (!directory) return false;
    char child[PATH_CAPACITY];
    for (struct dirent *entry; (entry = readdir(directory)) != NULL;) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        bool folder = false;
#if defined(DT_DIR)
        if (entry->d_type == DT_DIR) folder = true;
        else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
#endif
            folder = join_path(child, sizeof child, path, name) && is_directory(child);
        if (!fn(name, folder, user_data)) break;
    }
    closedir(directory);
    return true;
}

/** Resolves path to an absolute UTF-8 path of at most cap bytes. */
static bool absolute_path(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    char *absolute = realpath(path, NULL);
    if (!absolute) return false;
    size_t size = strlen(absolute) + 1;
    bool ok = size <= cap;
    if (ok) memcpy(out, absolute, size);
    free(absolute);
    return ok;
}

#endif

/* ---- Browser logic ---- */

/** Writes the user's home folder, falling back to the filesystem root. */
static void home_folder(char *out, size_t capacity)
{
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
    const char *fallback = "C:\\";
#else
    const char *fallback = "/";
#endif
    if (!home || !home[0] || !absolute_path(home, out, capacity)) snprintf(out, capacity, "%s", fallback);
}

/** Applies the active extension filter to a file name. */
static bool accepted(const Ca_FileBrowser *b, const char *name)
{
    if (!b->filter_count) return true;
    const StoredFilter *filter = &b->filters[b->active_filter];
    if (!filter->count) return true;
    size_t length = strlen(name);
    for (size_t i = 0; i < filter->count; ++i) {
        size_t suffix = strlen(filter->extensions[i]);
        if (!suffix || length <= suffix + 1 || name[length - suffix - 1] != '.') continue;
        bool match = true;
        for (size_t j = 0; j < suffix && match; ++j) {
            unsigned char ch = (unsigned char)name[length - suffix + j];
            if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + 32);
            match = ch == (unsigned char)filter->extensions[i][j];
        }
        if (match) return true;
    }
    return false;
}

/** Compares ASCII case-insensitively with a bytewise tiebreak for stable ordering. */
static int compare_names(const char *a, const char *b)
{
    for (const char *x = a, *y = b;; ++x, ++y) {
        int p = (unsigned char)*x, q = (unsigned char)*y;
        if (p >= 'A' && p <= 'Z') p += 32;
        if (q >= 'A' && q <= 'Z') q += 32;
        if (p != q) return p - q;
        if (!p) return strcmp(a, b);
    }
}

/** Orders folders before files, then by name. */
static int compare_entries(const void *x, const void *y)
{
    const FileEntry *a = x, *b = y;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    return compare_names(a->name, b->name);
}

/** Releases every listed entry. */
static void clear_entries(Ca_FileBrowser *b)
{
    for (size_t i = 0; i < b->entry_count; ++i) free(b->entries[i].name);
    b->entry_count = 0;
}

typedef struct Listing {
    Ca_FileBrowser *browser;
    const char *folder;
    bool failed;
} Listing;

/** Collects one directory entry that passes the hidden-file, mode and extension filters. */
static bool collect(const char *name, bool directory, void *data)
{
    Listing *listing = data;
    Ca_FileBrowser *b = listing->browser;
    if ((!b->show_hidden && name[0] == '.') ||
        (!directory && (b->mode == CA_FILE_BROWSER_SELECT_FOLDER || !accepted(b, name))))
        return true;
    if (b->entry_count == b->entry_capacity) {
        size_t capacity = b->entry_capacity ? b->entry_capacity * 2 : 64;
        FileEntry *grown = capacity < SIZE_MAX / sizeof *grown ? realloc(b->entries, capacity * sizeof *grown) : NULL;
        if (!grown) {
            listing->failed = true;
            return false;
        }
        b->entries = grown;
        b->entry_capacity = capacity;
    }
    size_t size = strlen(name) + 1;
    char *copy = malloc(size), path[PATH_CAPACITY];
    if (!copy) {
        listing->failed = true;
        return false;
    }
    memcpy(copy, name, size);
    int64_t bytes = !directory && join_path(path, sizeof path, listing->folder, name) ? file_size(path) : -1;
    b->entries[b->entry_count++] = (FileEntry){copy, directory, false, bytes};
    return true;
}

/** Re-lists the current folder; on failure the listing is empty and the reason is shown. */
static void scan(Ca_FileBrowser *b)
{
    clear_entries(b);
    b->anchor = SIZE_MAX;
    b->message[0] = '\0';
    Listing listing = {b, b->current, false};
    if (!list_directory(b->current, collect, &listing))
        snprintf(b->message, sizeof b->message, "This folder cannot be read.");
    else if (listing.failed)
        snprintf(b->message, sizeof b->message, "Out of memory; the listing is incomplete.");
    if (b->entry_count) qsort(b->entries, b->entry_count, sizeof *b->entries, compare_entries);
    snprintf(b->draft, sizeof b->draft, "%s", b->current);
    if (b->root) ca_div_invalidate(b->root);
    if (b->window) ca_scroll_to_top(b->window, LIST_ID);
}

/** Shows a message and redraws without changing the folder. */
static void notify(Ca_FileBrowser *b, const char *message)
{
    snprintf(b->message, sizeof b->message, "%s", message);
    if (b->root) ca_div_invalidate(b->root);
}

/** Resolves path (expanding "~") to an absolute readable folder; false leaves the browser unchanged. */
static bool enter(Ca_FileBrowser *b, const char *path, bool record)
{
    char expanded[PATH_CAPACITY], absolute[PATH_CAPACITY];
    while (*path == ' ') ++path;
    if (path[0] == '~' && (!path[1] || is_separator(path[1]))) {
        char home[PATH_CAPACITY];
        home_folder(home, sizeof home);
        if (snprintf(expanded, sizeof expanded, "%s%s", home, path + 1) >= (int)sizeof expanded) return false;
    } else {
        snprintf(expanded, sizeof expanded, "%s", path);
    }
    if (!expanded[0] || !absolute_path(expanded, absolute, sizeof absolute) ||
        file_size(absolute) >= 0 || !path_exists(absolute))
        return false;
    snprintf(b->current, sizeof b->current, "%s", absolute);
    if (record) {
        size_t next = b->history_count ? b->history_pos + 1 : 0;
        if (next == HISTORY) {
            memmove(b->history, b->history + 1, sizeof b->history[0] * (HISTORY - 1));
            next = HISTORY - 1;
        }
        snprintf(b->history[next], PATH_CAPACITY, "%s", absolute);
        b->history_pos = next;
        b->history_count = next + 1;
    }
    scan(b);
    return true;
}

/** Navigates to the parent folder, stopping at the filesystem root. */
static void go_up(Ca_FileBrowser *b)
{
    char parent[PATH_CAPACITY];
    snprintf(parent, sizeof parent, "%s", b->current);
    size_t root = root_length(parent), length = strlen(parent);
    while (length > root && is_separator(parent[length - 1])) parent[--length] = '\0';
    while (length > root && !is_separator(parent[length - 1])) parent[--length] = '\0';
    while (length > root && is_separator(parent[length - 1])) parent[--length] = '\0';
    if (length && strcmp(parent, b->current)) enter(b, parent, true);
}

/** Revisits a history slot without recording a new one. */
static void revisit(Ca_FileBrowser *b, size_t slot)
{
    size_t previous = b->history_pos;
    b->history_pos = slot;
    if (!enter(b, b->history[slot], false)) {
        b->history_pos = previous;
        notify(b, "That folder is no longer available.");
    }
}

static void back_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Ca_FileBrowser *b = data;
    if (b->history_pos) revisit(b, b->history_pos - 1);
}

static void forward_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Ca_FileBrowser *b = data;
    if (b->history_pos + 1 < b->history_count) revisit(b, b->history_pos + 1);
}

static void up_clicked(Ca_Button *button, void *data)
{
    (void)button;
    go_up(data);
}

static void home_clicked(Ca_Button *button, void *data)
{
    (void)button;
    char home[PATH_CAPACITY];
    home_folder(home, sizeof home);
    enter(data, home, true);
}

static void refresh_clicked(Ca_Button *button, void *data)
{
    (void)button;
    scan(data);
}

static void draft_changed(Ca_TextInput *input, void *data)
{
    Ca_FileBrowser *b = data;
    snprintf(b->draft, sizeof b->draft, "%s", ca_input_text(input));
}

static void go_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Ca_FileBrowser *b = data;
    char typed[PATH_CAPACITY];
    snprintf(typed, sizeof typed, "%s", b->draft);
    if (!enter(b, typed, true)) notify(b, "No readable folder exists at that path.");
}

static void hidden_changed(Ca_Checkbox *checkbox, void *data)
{
    Ca_FileBrowser *b = data;
    b->show_hidden = ca_checkbox_get(checkbox);
    scan(b);
}

static void filter_changed(Ca_Select *select, void *data)
{
    Ca_FileBrowser *b = data;
    int index = ca_select_get(select);
    if (index >= 0 && (size_t)index < b->filter_count) {
        b->active_filter = (size_t)index;
        scan(b);
    }
}

/** Counts selected entries, optionally only files. */
static size_t selected_count(const Ca_FileBrowser *b, bool files_only)
{
    size_t count = 0;
    for (size_t i = 0; i < b->entry_count; ++i)
        count += b->entries[i].selected && (!files_only || !b->entries[i].directory);
    return count;
}

/** Reports that the window closed, once per request. */
static void report_closed(Ca_FileBrowser *b)
{
    Ca_FileBrowserClosedFn closed = b->on_closed;
    void *user_data = b->user_data;
    b->on_closed = NULL;
    if (closed) closed(user_data);
}

/** Closes the window and delivers the chosen paths; the browser state is not read after the callback. */
static void confirm(Ca_FileBrowser *b)
{
    size_t count = b->mode == CA_FILE_BROWSER_SELECT_FOLDER ? 1 : selected_count(b, true);
    if (!count || !b->on_confirm) return;
    char (*paths)[PATH_CAPACITY] = malloc(count * sizeof *paths);
    const char **list = malloc(count * sizeof *list);
    if (!paths || !list) {
        free(paths);
        free(list);
        notify(b, "Out of memory.");
        return;
    }
    size_t used = 0;
    if (b->mode == CA_FILE_BROWSER_SELECT_FOLDER) {
        const char *folder = NULL;
        for (size_t i = 0; i < b->entry_count && !folder; ++i)
            if (b->entries[i].selected) folder = b->entries[i].name;
        bool ok = folder ? join_path(paths[used], PATH_CAPACITY, b->current, folder) :
                  snprintf(paths[used], PATH_CAPACITY, "%s", b->current) < PATH_CAPACITY;
        if (ok) list[used++] = paths[0];
    } else {
        for (size_t i = 0; i < b->entry_count; ++i)
            if (b->entries[i].selected && !b->entries[i].directory &&
                join_path(paths[used], PATH_CAPACITY, b->current, b->entries[i].name)) {
                list[used] = paths[used];
                ++used;
            }
    }
    Ca_FileBrowserFn callback = b->on_confirm;
    void *user_data = b->user_data;
    ca_file_browser_close(b);
    if (used) callback(list, used, user_data);
    free(paths);
    free(list);
}

static void cancel_clicked(Ca_Button *button, void *data)
{
    (void)button;
    ca_file_browser_close(data);
}

static void confirm_clicked(Ca_Button *button, void *data)
{
    (void)button;
    confirm(data);
}

/**
 * Selects on click; clicking the sole selection again opens folders or confirms files.
 * With multiple selection, Cmd/Ctrl toggles a file and Shift extends a range of files.
 */
static void entry_clicked(Ca_Button *button, void *data)
{
    (void)button;
    FileRow *row = data;
    Ca_FileBrowser *b = row->browser;
    if (row->index >= b->entry_count) return;
    FileEntry *entry = &b->entries[row->index];
    int mods = b->window ? ca_window_mouse_mods(b->window) : 0;
    bool many = b->multiple && b->mode == CA_FILE_BROWSER_OPEN_FILES && !entry->directory;
    if (many && (mods & (CA_MOD_SUPER | CA_MOD_CONTROL))) {
        entry->selected = !entry->selected;
    } else if (many && (mods & CA_MOD_SHIFT) && b->anchor < b->entry_count) {
        size_t from = b->anchor < row->index ? b->anchor : row->index;
        size_t to = b->anchor < row->index ? row->index : b->anchor;
        for (size_t i = 0; i < b->entry_count; ++i)
            b->entries[i].selected = i >= from && i <= to && !b->entries[i].directory;
    } else if (entry->selected && selected_count(b, false) == 1) {
        if (entry->directory) {
            char path[PATH_CAPACITY];
            if (join_path(path, sizeof path, b->current, entry->name) && !enter(b, path, true))
                notify(b, "This folder cannot be opened.");
        } else {
            confirm(b);
        }
        return;
    } else {
        for (size_t i = 0; i < b->entry_count; ++i) b->entries[i].selected = i == row->index;
    }
    b->anchor = row->index;
    if (b->root) ca_div_invalidate(b->root);
}

/** Formats a byte count with binary units. */
static void format_size(int64_t size, char *out, size_t capacity)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = size < 0 ? 0 : (double)size;
    size_t unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    snprintf(out, capacity, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
}

/** Adds a toolbar icon button with a tooltip. */
static void icon_button(const char *icon, const char *tooltip, Ca_ClickFn click, void *data, bool disabled)
{
    Ca_Button *button = ca_btn_begin(&(Ca_BtnDesc){.text = icon, .style = "ca-fb-icon", .on_click = click,
                                                    .click_data = data, .disabled = disabled});
    ca_btn_end();
    ca_tooltip_for_widget(button, &(Ca_TooltipDesc){.text = tooltip});
}

/** Rebuilds the whole browser from controller state. */
static void build(Ca_Div *root, void *data)
{
    (void)root;
    Ca_FileBrowser *b = data;
    size_t shown = b->entry_count < ROW_LIMIT ? b->entry_count : ROW_LIMIT;
    if (shown > b->row_capacity) {
        FileRow *rows = realloc(b->rows, shown * sizeof *rows);
        if (rows) {
            b->rows = rows;
            b->row_capacity = shown;
        } else {
            shown = b->row_capacity;
        }
    }
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = "ca-fb"});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "ca-fb-bar"});
    icon_button(CA_ICON_NF_FA_ARROW_LEFT, "Back", back_clicked, b, b->history_pos == 0);
    icon_button(CA_ICON_NF_FA_ARROW_RIGHT, "Forward", forward_clicked, b, b->history_pos + 1 >= b->history_count);
    icon_button(CA_ICON_NF_FA_LEVEL_UP, "Parent folder", up_clicked, b, false);
    icon_button(CA_ICON_NF_FA_HOME, "Home folder", home_clicked, b, false);
    icon_button(CA_ICON_NF_FA_REFRESH, "Refresh", refresh_clicked, b, false);
    ca_input(&(Ca_InputDesc){.text = b->draft, .id = "ca-file-browser-path", .style = "ca-fb-path",
                             .on_change = draft_changed, .change_data = b});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Go", .style = "ca-fb-button", .on_click = go_clicked, .click_data = b});
    ca_btn_end();
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "ca-fb-bar"});
    if (b->filter_count > 1)
        ca_select(&(Ca_SelectDesc){.options = b->filter_labels, .option_count = (int)b->filter_count,
                                   .selected = (int)b->active_filter, .id = "ca-file-browser-filter",
                                   .on_change = filter_changed, .change_data = b});
    ca_checkbox(&(Ca_CheckboxDesc){.text = "Show hidden", .checked = b->show_hidden,
                                   .on_change = hidden_changed, .change_data = b});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = LIST_ID, .style = "ca-fb-list"});
    if (!b->entry_count)
        ca_text(&(Ca_TextDesc){.text = b->message[0] ? b->message : "Nothing to show here.", .style = "ca-fb-muted"});
    for (size_t i = 0; i < shown; ++i) {
        const FileEntry *entry = &b->entries[i];
        b->rows[i] = (FileRow){b, i};
        char label[320], size[32];
        snprintf(label, sizeof label, "%s  %s", entry->directory ? CA_ICON_NF_FA_FOLDER : CA_ICON_NF_FA_FILE, entry->name);
        ca_btn_begin(&(Ca_BtnDesc){.direction = CA_HORIZONTAL, .on_click = entry_clicked, .click_data = &b->rows[i],
                                   .style = entry->selected ? "ca-fb-row selected" : "ca-fb-row"});
        ca_text(&(Ca_TextDesc){.text = label, .style = "ca-fb-label"});
        if (!entry->directory) {
            format_size(entry->size, size, sizeof size);
            ca_text(&(Ca_TextDesc){.text = size, .style = "ca-fb-muted"});
        }
        ca_btn_end();
    }
    ca_div_end();
    char summary[320] = "";
    size_t files = selected_count(b, true);
    if (b->entry_count > shown)
        snprintf(summary, sizeof summary, "Showing %zu of %zu entries. Narrow with the filter.", shown, b->entry_count);
    else if (b->entry_count && b->message[0])
        snprintf(summary, sizeof summary, "%s", b->message);
    else if (b->mode == CA_FILE_BROWSER_SELECT_FOLDER)
        snprintf(summary, sizeof summary, "Click a folder twice to open it; confirm chooses the selected or current folder.");
    else if (files == 1) {
        for (size_t i = 0; i < b->entry_count; ++i)
            if (b->entries[i].selected && !b->entries[i].directory)
                snprintf(summary, sizeof summary, "%s", b->entries[i].name);
    } else if (files) {
        snprintf(summary, sizeof summary, "%zu files selected", files);
    } else {
        snprintf(summary, sizeof summary, b->multiple ? "Select files (Cmd/Ctrl or Shift for several)." :
                 "Select a file. Click it again to open it.");
    }
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = "ca-fb-footer"});
    ca_text(&(Ca_TextDesc){.text = summary, .style = "ca-fb-summary"});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Cancel", .style = "ca-fb-button", .on_click = cancel_clicked, .click_data = b});
    ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = b->confirm_label, .style = "ca-fb-button ca-fb-primary", .on_click = confirm_clicked,
                               .click_data = b, .disabled = b->mode == CA_FILE_BROWSER_OPEN_FILES && !files});
    ca_btn_end();
    ca_div_end();
    ca_div_end();
}

/** Forgets window handles when the OS or the user closes the browser. */
static void closed(Ca_Window *window, void *data)
{
    (void)window;
    Ca_FileBrowser *b = data;
    b->window = NULL;
    b->root = NULL;
    report_closed(b);
}

Ca_FileBrowser *ca_file_browser_create(Ca_Instance *instance)
{
    if (!instance) return NULL;
    Ca_FileBrowser *b = calloc(1, sizeof *b);
    if (b) b->instance = instance;
    return b;
}

void ca_file_browser_destroy(Ca_FileBrowser *b)
{
    if (!b) return;
    b->on_closed = NULL;
    if (b->window) ca_window_destroy(b->window);
    clear_entries(b);
    free(b->entries);
    free(b->rows);
    free(b);
}

bool ca_file_browser_show(Ca_FileBrowser *b, const Ca_FileBrowserDesc *desc)
{
    if (!b || !desc || !desc->on_confirm || desc->filter_count > CA_FILE_BROWSER_MAX_FILTERS ||
        (desc->filter_count && !desc->filters) ||
        (desc->mode != CA_FILE_BROWSER_OPEN_FILES && desc->mode != CA_FILE_BROWSER_SELECT_FOLDER))
        return false;
    for (size_t i = 0; i < desc->filter_count; ++i)
        if (desc->filters[i].extension_count > CA_FILE_BROWSER_MAX_EXTENSIONS ||
            (desc->filters[i].extension_count && !desc->filters[i].extensions))
            return false;
    bool folder = desc->mode == CA_FILE_BROWSER_SELECT_FOLDER;
    if (b->window) report_closed(b);
    b->mode = desc->mode;
    b->multiple = desc->multiple && !folder;
    b->on_confirm = desc->on_confirm;
    b->on_closed = desc->on_closed;
    b->user_data = desc->user_data;
    snprintf(b->title, sizeof b->title, "%s", desc->title ? desc->title : folder ? "Select folder" : "Open files");
    snprintf(b->confirm_label, sizeof b->confirm_label, "%s",
             desc->confirm_label ? desc->confirm_label : folder ? "Select folder" : "Open");
    b->filter_count = desc->filter_count;
    b->active_filter = 0;
    for (size_t i = 0; i < desc->filter_count; ++i) {
        StoredFilter *filter = &b->filters[i];
        snprintf(filter->label, sizeof filter->label, "%s", desc->filters[i].label ? desc->filters[i].label : "Files");
        filter->count = desc->filters[i].extension_count;
        for (size_t j = 0; j < filter->count; ++j)
            snprintf(filter->extensions[j], sizeof filter->extensions[j], "%s",
                     desc->filters[i].extensions[j] ? desc->filters[i].extensions[j] : "");
        b->filter_labels[i] = filter->label;
    }
    b->history_count = 0;
    b->history_pos = 0;
    char home[PATH_CAPACITY];
    if (!desc->initial_path || !enter(b, desc->initial_path, true)) {
        home_folder(home, sizeof home);
        if (!enter(b, home, true)) return false;
    }
    if (!b->window) {
        b->window = ca_window_create(b->instance, &(Ca_WindowDesc){.title = b->title, .width = WINDOW_WIDTH,
                                     .height = WINDOW_HEIGHT, .on_close = closed, .on_close_data = b});
        b->root = b->window ? ca_window_content_div(b->window) : NULL;
        if (!b->root || !ca_div_set_builder(b->root, build, b)) {
            if (b->window) ca_window_destroy(b->window);
            b->window = NULL;
            b->root = NULL;
            return false;
        }
    } else {
        ca_window_set_title(b->window, b->title);
        ca_window_focus(b->window);
        ca_div_invalidate(b->root);
    }
    return true;
}

void ca_file_browser_close(Ca_FileBrowser *b)
{
    if (b && b->window) ca_window_close(b->window);
}

bool ca_file_browser_is_open(const Ca_FileBrowser *b)
{
    return b && b->window;
}

