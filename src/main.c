#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>

#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dngfix.h"
#include "Resource.h"
#include "git_version.h"

#define APP_TITLE L"DNGFix"

#define MAX_PROFILES_IN_LIST 64

#define IDM_ADD_FILES       40001
#define IDM_ADD_FOLDER      40002
#define IDM_CLEAR_LIST      40003
#define IDM_OUTPUT_FOLDER   40004
#define IDM_EXIT_APP        40005
#define IDM_DRY_RUN         40010
#define IDM_FIX             40011
#define IDM_RELOAD_PROFILES 40012
#define IDM_OPEN_PROFILES   40013
#define IDM_OPEN_OUTPUT     40014
#define IDM_ABOUT_APP       40020

#define IDC_PROFILE_COMBO   1001
#define IDC_FILE_LIST       1002
#define IDC_LOG             1003
#define IDC_OUTPUT_LABEL    1004
#define IDC_STATUS          1005
#define IDC_PROFILE_LABEL   1006
#define IDC_PROGRESS        1007
#define IDC_PROGRESS_LABEL  1008

static HINSTANCE g_inst;
static HWND g_main;
static HWND g_profile_combo;
static HWND g_file_list;
static HWND g_log;
static HWND g_output_label;
static HWND g_status;
static HWND g_progress;
static HWND g_progress_label;
static HFONT g_ui_font;
static HFONT g_log_font;

static wchar_t g_profile_dir[MAX_PATH];
static wchar_t g_output_dir[MAX_PATH];
static wchar_t g_profile_arg[MAX_PATH];

static DngProfile g_builtin;
static DngProfile g_profiles[MAX_PROFILES_IN_LIST];
static int g_profile_count;
static int g_initial_profile_index = -1;

static wchar_t **g_files;
static int g_file_count;
static int g_file_capacity;

static int g_busy;
static int g_cancel;
static int g_batch_dry;
static int g_force;
static int g_console_ready;
static FILE *g_cli_log;
static wchar_t g_startup_message[1024];

static void log_append(const wchar_t *fmt, ...);

// path/string util

static int path_is_dir(const wchar_t *path)
{
    DWORD attrs;

    if (!path || !*path)
        return 0;
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int path_exists(const wchar_t *path)
{
    return path && *path &&
           GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void path_dirname(const wchar_t *path, wchar_t *out, size_t out_cch)
{
    const wchar_t *slash;
    size_t len;

    if (!out || out_cch == 0)
        return;
    if (!path) {
        out[0] = L'\0';
        return;
    }
    slash = wcsrchr(path, L'\\');
    if (!slash)
        slash = wcsrchr(path, L'/');
    if (!slash) {
        out[0] = L'\0';
        return;
    }
    len = (size_t)(slash - path);
    if (len >= out_cch)
        len = out_cch - 1;
    memmove(out, path, len * sizeof(wchar_t));
    out[len] = L'\0';
}

static void path_basename(const wchar_t *path, wchar_t *out, size_t out_cch)
{
    const wchar_t *base = path;
    const wchar_t *slash;
    size_t len;

    if (!out || out_cch == 0)
        return;
    if (!path) {
        out[0] = L'\0';
        return;
    }
    slash = wcsrchr(path, L'\\');
    if (!slash)
        slash = wcsrchr(path, L'/');
    if (slash)
        base = slash + 1;
    len = wcslen(base);
    if (len >= out_cch)
        len = out_cch - 1;
    memmove(out, base, len * sizeof(wchar_t));
    out[len] = L'\0';
}

static int ends_with_i(const wchar_t *text, const wchar_t *suffix)
{
    size_t tlen = wcslen(text);
    size_t slen = wcslen(suffix);

    if (slen > tlen)
        return 0;
    return _wcsicmp(text + tlen - slen, suffix) == 0;
}

static int is_fixed_output_name(const wchar_t *name)
{
    wchar_t stem[MAX_PATH];
    const wchar_t *dot;
    size_t len;

    wcsncpy_s(stem, ARRAYSIZE(stem), name, _TRUNCATE);
    dot = wcsrchr(stem, L'.');
    if (dot && dot != stem)
        len = (size_t)(dot - stem);
    else
        len = wcslen(stem);
    if (len >= 6 && _wcsicmp(stem + len - 6, L"_fixed") == 0)
        return 1;
    return 0;
}

static void join_path(wchar_t *out, size_t out_cch,
                      const wchar_t *dir, const wchar_t *name)
{
    size_t len;

    if (!out || out_cch == 0)
        return;
    if (!dir || !*dir) {
        wcsncpy_s(out, out_cch, name ? name : L"", _TRUNCATE);
        return;
    }
    wcsncpy_s(out, out_cch, dir, _TRUNCATE);
    len = wcslen(out);
    if (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/') {
        if (len + 1 < out_cch) {
            out[len++] = L'\\';
            out[len] = L'\0';
        }
    }
    wcsncat_s(out, out_cch, name ? name : L"", _TRUNCATE);
}

static void get_app_dir(wchar_t *out, size_t out_cch)
{
    DWORD n;

    if (!out || out_cch == 0)
        return;
    out[0] = L'\0';
    n = GetModuleFileNameW(NULL, out, (DWORD)out_cch);
    if (n == 0)
        return;
    out[out_cch - 1] = L'\0';
    path_dirname(out, out, out_cch);
}

static void resolve_profile_dir(const wchar_t *override_dir)
{
    wchar_t tmp[MAX_PATH];
    wchar_t cwd[MAX_PATH];

    g_profile_dir[0] = L'\0';
    if (override_dir && *override_dir) {
        wcsncpy_s(g_profile_dir, ARRAYSIZE(g_profile_dir), override_dir,
                  _TRUNCATE);
        return;
    }

    get_app_dir(tmp, ARRAYSIZE(tmp));
    join_path(g_profile_dir, ARRAYSIZE(g_profile_dir), tmp, L"profiles");
    if (path_is_dir(g_profile_dir))
        return;

    if (GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd) != 0) {
        join_path(g_profile_dir, ARRAYSIZE(g_profile_dir), cwd, L"profiles");
        if (path_is_dir(g_profile_dir))
            return;
    }

    if (GetEnvironmentVariableW(L"APPDATA", tmp, ARRAYSIZE(tmp)) > 0) {
        join_path(g_profile_dir, ARRAYSIZE(g_profile_dir), tmp,
                  L"DNGFix\\profiles");
        if (path_is_dir(g_profile_dir))
            return;
    }

    get_app_dir(tmp, ARRAYSIZE(tmp));
    join_path(g_profile_dir, ARRAYSIZE(g_profile_dir), tmp, L"profiles");
}

static int profile_name_matches(const DngProfile *p, const wchar_t *name)
{
    wchar_t base[MAX_PATH];
    wchar_t stem[MAX_PATH];
    const wchar_t *dot;

    if (!_wcsicmp(p->name, name))
        return 1;
    path_basename(p->path, base, ARRAYSIZE(base));
    wcsncpy_s(stem, ARRAYSIZE(stem), base, _TRUNCATE);
    dot = wcsrchr(stem, L'.');
    if (dot && dot != stem)
        *((wchar_t *)dot) = L'\0';
    return _wcsicmp(stem, name) == 0;
}

static int find_profile_by_name(const wchar_t *name)
{
    int i;

    for (i = 0; i < g_profile_count; i++) {
        if (profile_name_matches(&g_profiles[i], name))
            return i;
    }
    return -1;
}

static void load_profiles(void)
{
    wchar_t err[512];

    g_profile_count = dng_profiles_load_dir(g_profile_dir, g_profiles,
                                            MAX_PROFILES_IN_LIST,
                                            err, ARRAYSIZE(err));
    if (err[0] && g_log)
        log_append(L"Profile warning: %s", err);

    g_initial_profile_index = -1;
    if (g_profile_arg[0]) {
        if (path_exists(g_profile_arg) && !path_is_dir(g_profile_arg)) {
            DngProfile loaded;
            wchar_t load_err[512];
            int already = 0;
            int i;

            for (i = 0; i < g_profile_count; i++) {
                if (!_wcsicmp(g_profiles[i].path, g_profile_arg)) {
                    g_initial_profile_index = i;
                    already = 1;
                    break;
                }
            }
            if (already)
                return;

            if (dng_profile_load(g_profile_arg, &loaded, load_err,
                                 ARRAYSIZE(load_err)) == 0) {
                if (g_profile_count < MAX_PROFILES_IN_LIST) {
                    g_profiles[g_profile_count] = loaded;
                    g_initial_profile_index = g_profile_count;
                    g_profile_count++;
                }
            } else {
                swprintf_s(g_startup_message,
                           ARRAYSIZE(g_startup_message),
                           L"Could not load profile %s: %s",
                           g_profile_arg, load_err);
            }
        } else {
            g_initial_profile_index = find_profile_by_name(g_profile_arg);
            if (g_initial_profile_index < 0) {
                swprintf_s(g_startup_message,
                           ARRAYSIZE(g_startup_message),
                           L"Profile not found: %s", g_profile_arg);
            }
        }
    }
}

// file listing

static void files_clear(void)
{
    int i;

    for (i = 0; i < g_file_count; i++) {
        free(g_files[i]);
        g_files[i] = NULL;
    }
    free(g_files);
    g_files = NULL;
    g_file_count = 0;
    g_file_capacity = 0;
}

static int files_contains(const wchar_t *path)
{
    int i;

    for (i = 0; i < g_file_count; i++) {
        if (_wcsicmp(g_files[i], path) == 0)
            return 1;
    }
    return 0;
}

static int files_add(const wchar_t *path)
{
    wchar_t *copy;

    if (!path || !*path || files_contains(path))
        return 0;
    if (g_file_count == g_file_capacity) {
        int new_cap = g_file_capacity ? g_file_capacity * 2 : 32;
        wchar_t **new_files = (wchar_t **)realloc(
            g_files, (size_t)new_cap * sizeof(wchar_t *));
        if (!new_files)
            return 0;
        g_files = new_files;
        g_file_capacity = new_cap;
    }
    copy = _wcsdup(path);
    if (!copy)
        return 0;
    g_files[g_file_count++] = copy;
    return 1;
}

static void add_folder_recursive(const wchar_t *dir, int depth)
{
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (depth > 32 || !dir || !*dir)
        return;
    if (swprintf_s(pattern, ARRAYSIZE(pattern), L"%s\\*", dir) < 0)
        return;
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t full[MAX_PATH];

        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (swprintf_s(full, ARRAYSIZE(full), L"%s\\%s", dir,
                       fd.cFileName) < 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            add_folder_recursive(full, depth + 1);
        } else if (ends_with_i(fd.cFileName, L".dng") &&
                   !is_fixed_output_name(fd.cFileName)) {
            files_add(full);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static void add_path(const wchar_t *path, int recursive)
{
    if (!path || !*path)
        return;
    if (path_is_dir(path)) {
        if (recursive)
            add_folder_recursive(path, 0);
        return;
    }
    if (path_exists(path))
        files_add(path);
}

static void list_add_file(int index)
{
    LVITEMW item;

    if (!g_file_list || index < 0 || index >= g_file_count)
        return;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = index;
    item.pszText = g_files[index];
    item.lParam = index;
    ListView_InsertItem(g_file_list, &item);
    ListView_SetItemText(g_file_list, index, 1, L"Pending");
    ListView_SetItemText(g_file_list, index, 2, L"");
}

static void list_set_status(int index, const wchar_t *status,
                            const wchar_t *detail)
{
    if (!g_file_list || index < 0 || index >= g_file_count)
        return;
    ListView_SetItemText(g_file_list, index, 1, (LPWSTR)status);
    ListView_SetItemText(g_file_list, index, 2, (LPWSTR)(detail ? detail : L""));
    ListView_EnsureVisible(g_file_list, index, FALSE);
    if (g_busy)
        UpdateWindow(g_file_list);
}

static void update_counts(void)
{
    wchar_t text[256];
    wchar_t out[64];

    if (!g_status)
        return;
    if (g_output_dir[0])
        wcsncpy_s(out, ARRAYSIZE(out), g_output_dir, _TRUNCATE);
    else
        wcscpy_s(out, ARRAYSIZE(out), L"next to source");
    swprintf_s(text, ARRAYSIZE(text), L"%d file(s)  |  Output: %s"
               L"%s", g_file_count, out,
               g_busy ? L"  |  working..." : L"");
    SetWindowTextW(g_status, text);
}

static void update_output_label(void)
{
    wchar_t text[MAX_PATH + 64];

    if (!g_output_label)
        return;
    if (g_output_dir[0]) {
        swprintf_s(text, ARRAYSIZE(text), L"Output folder: %s",
                   g_output_dir);
    } else {
        wcscpy_s(text, ARRAYSIZE(text),
                 L"Output folder: beside each source file");
    }
    SetWindowTextW(g_output_label, text);
}

// progress feedback (sits directly above the log)

static void progress_set_text(const wchar_t *fmt, ...)
{
    wchar_t text[512];
    va_list ap;

    if (!g_progress_label)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_progress_label, text);
    UpdateWindow(g_progress_label);
}

static void progress_begin(int total)
{
    if (total < 1)
        total = 1;
    if (g_progress) {
        SendMessageW(g_progress, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        UpdateWindow(g_progress);
    }
}

static void progress_show_file(int index, int total, const wchar_t *path,
                               int dry_run)
{
    wchar_t name[MAX_PATH];

    path_basename(path, name, ARRAYSIZE(name));
    if (g_progress) {
        SendMessageW(g_progress, PBM_SETPOS, index, 0);
        UpdateWindow(g_progress);
    }
    progress_set_text(L"%s %d of %d - %s",
                      dry_run ? L"Checking" : L"Fixing",
                      index + 1, total, name);
}

static void progress_finish(int total)
{
    if (g_progress) {
        SendMessageW(g_progress, PBM_SETRANGE32, 0,
                     (LPARAM)(total > 0 ? total : 1));
        SendMessageW(g_progress, PBM_SETPOS, total, 0);
        UpdateWindow(g_progress);
    }
}

static void progress_stop(int done)
{
    if (g_progress) {
        SendMessageW(g_progress, PBM_SETPOS, done, 0);
        UpdateWindow(g_progress);
    }
}

// keep the window painting (and close clickable) while a batch runs

static void pump_ui(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void request_cancel(void)
{
    if (!g_busy || g_cancel)
        return;
    g_cancel = 1;
    progress_set_text(L"Cancelling after the current file...");
    log_append(L"Cancelling after the current file...");
}

// log/debug

static void log_append(const wchar_t *fmt, ...)
{
    wchar_t body[4096];
    wchar_t line[4300];
    SYSTEMTIME st;
    va_list ap;
    int len;
    LRESULT text_len;

    if (!g_log)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(body, ARRAYSIZE(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    swprintf_s(line, ARRAYSIZE(line), L"[%02u:%02u:%02u] %s\r\n",
               (unsigned)st.wHour, (unsigned)st.wMinute,
               (unsigned)st.wSecond, body);

    text_len = SendMessageW(g_log, WM_GETTEXTLENGTH, 0, 0);
    if (text_len > 500000) {
        SendMessageW(g_log, EM_SETSEL, 0, 100000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    if (g_busy)
        UpdateWindow(g_log);
}

static void gui_engine_log(void *user, DngLogLevel level, const wchar_t *text)
{
    const wchar_t *prefix;

    (void)user;
    switch (level) {
    case DNG_LOG_FIX:
        prefix = g_batch_dry ? L"  would fix: " : L"  fix: ";
        break;
    case DNG_LOG_WARN:
        prefix = L"  warning: ";
        break;
    case DNG_LOG_ERROR:
        prefix = L"  ERROR: ";
        break;
    default:
        prefix = L"  ";
        break;
    }
    log_append(L"%s%s", prefix, text);
}

static void cli_log_write(const wchar_t *text)
{
    if (g_cli_log) {
        char utf8[16384];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8,
                                    (int)ARRAYSIZE(utf8) - 1, NULL, NULL);
        if (n > 0) {
            utf8[n - 1] = '\n';
            fwrite(utf8, 1, (size_t)n, g_cli_log);
            fflush(g_cli_log);
        }
    }
    if (g_console_ready)
        wprintf(L"%s\r\n", text);
}

static void cli_print(const wchar_t *fmt, ...)
{
    wchar_t text[4096];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    cli_log_write(text);
}

static void cli_engine_log(void *user, DngLogLevel level, const wchar_t *text)
{
    const wchar_t *prefix;

    (void)user;
    switch (level) {
    case DNG_LOG_FIX:
        prefix = L"        - ";
        break;
    case DNG_LOG_WARN:
        prefix = L"        ! ";
        break;
    case DNG_LOG_ERROR:
        prefix = L"        ERROR: ";
        break;
    default:
        prefix = L"        ";
        break;
    }
    cli_print(L"%s%s", prefix, text);
}

// profile combo

static int combo_selected_profile(void)
{
    int sel;
    LRESULT data;

    if (!g_profile_combo)
        return -1;
    sel = (int)ComboBox_GetCurSel(g_profile_combo);
    if (sel <= 0)
        return -1;
    data = ComboBox_GetItemData(g_profile_combo, sel);
    return (int)data;
}

static void populate_profile_combo(void)
{
    int i;
    int keep = -1;
    int selected = combo_selected_profile();

    if (!g_profile_combo)
        return;
    if (selected >= 0 && selected < g_profile_count)
        keep = selected;

    SendMessageW(g_profile_combo, CB_RESETCONTENT, 0, 0);
    ComboBox_AddString(g_profile_combo, L"Auto (match camera)");
    ComboBox_SetItemData(g_profile_combo, 0, (LPARAM)-1);
    for (i = 0; i < g_profile_count; i++) {
        int item = (int)SendMessageW(g_profile_combo, CB_ADDSTRING, 0,
                                     (LPARAM)g_profiles[i].name);
        if (item >= 0)
            ComboBox_SetItemData(g_profile_combo, item, (LPARAM)i);
    }

    if (keep >= 0)
        ComboBox_SetCurSel(g_profile_combo, keep + 1);
    else if (g_initial_profile_index >= 0 &&
             g_initial_profile_index < g_profile_count)
        ComboBox_SetCurSel(g_profile_combo, g_initial_profile_index + 1);
    else
        ComboBox_SetCurSel(g_profile_combo, 0);
}

// processing

static const DngProfile *profile_for_file(const wchar_t *path,
                                          wchar_t *err, size_t err_cch)
{
    int forced = combo_selected_profile();

    if (forced >= 0 && forced < g_profile_count)
        return &g_profiles[forced];

    {
        wchar_t make[128];
        wchar_t model[128];
        wchar_t unique_model[256];
        int index;

        if (dng_probe(path, make, ARRAYSIZE(make),
                      model, ARRAYSIZE(model),
                      unique_model, ARRAYSIZE(unique_model),
                      err, err_cch) != 0)
            return NULL;

        index = dng_profile_find_best(g_profiles, g_profile_count,
                                      make, model, unique_model);
        if (index >= 0)
            return &g_profiles[index];

        if (err && err_cch) {
            _snwprintf_s(err, err_cch, _TRUNCATE,
                         L"No device profile matched %s / %s; using the "
                         L"built-in generic profile",
                         make[0] ? make : L"(no make)",
                         model[0] ? model : L"(no model)");
        }
        return &g_builtin;
    }
}

static void set_buttons_enabled(BOOL enabled)
{
    EnableWindow(GetDlgItem(g_main, IDM_ADD_FILES), enabled);
    EnableWindow(GetDlgItem(g_main, IDM_ADD_FOLDER), enabled);
    EnableWindow(GetDlgItem(g_main, IDM_DRY_RUN), enabled);
    EnableWindow(GetDlgItem(g_main, IDM_FIX), enabled);
    EnableWindow(GetDlgItem(g_main, IDM_OUTPUT_FOLDER), enabled);
}

static void run_batch(int dry_run)
{
    int i;
    int fixed = 0;
    int clean = 0;
    int failed = 0;
    wchar_t summary[512];

    if (g_busy)
        return;
    if (g_file_count == 0) {
        MessageBoxW(g_main, L"Add one or more DNG files first.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_busy = 1;
    g_cancel = 0;
    g_batch_dry = dry_run;
    set_buttons_enabled(FALSE);
    update_counts();
    log_append(L"");
    log_append(dry_run ? L"Dry run: no files will be written."
                       : L"Repair started.");
    progress_begin(g_file_count);

    for (i = 0; i < g_file_count; i++) {
        const wchar_t *path = g_files[i];
        const DngProfile *profile;
        DngResult result;
        wchar_t err[512];
        wchar_t detail[512];

        progress_show_file(i, g_file_count, path, dry_run);
        pump_ui();
        if (g_cancel)
            break;
        err[0] = L'\0';
        profile = profile_for_file(path, err, ARRAYSIZE(err));
        if (!profile) {
            failed++;
            list_set_status(i, L"Failed", err[0] ? err : L"cannot read file");
            log_append(L"FAIL  %s", path);
            if (err[0])
                log_append(L"  ERROR: %s", err);
            update_counts();
            continue;
        }

        log_append(L"%s  [profile: %s]", path, profile->name);
        if (err[0])
            log_append(L"  note: %s", err);

        memset(&result, 0, sizeof(result));
        if (dng_process_file(path,
                             g_output_dir[0] ? g_output_dir : NULL,
                             profile, dry_run, 0,
                             gui_engine_log, NULL, &result) != 0) {
            failed++;
            list_set_status(i, L"Failed",
                            result.error[0] ? result.error : L"repair failed");
            log_append(L"FAIL  %s", path);
            update_counts();
            continue;
        }

        if (result.repairs > 0) {
            fixed++;
            if (dry_run) {
                swprintf_s(detail, ARRAYSIZE(detail),
                           L"%d repair(s) would be applied", result.repairs);
                list_set_status(i, L"Would fix", detail);
            } else {
                swprintf_s(detail, ARRAYSIZE(detail),
                           L"%d repair(s)%s", result.repairs,
                           result.skipped ? L", with warnings" : L"");
                list_set_status(i, L"Fixed", detail);
                if (result.wrote_output)
                    log_append(L"  wrote: %s", result.output_path);
            }
        } else {
            clean++;
            swprintf_s(detail, ARRAYSIZE(detail), L"nothing to fix%s",
                       result.skipped ? L" (warnings only)" : L"");
            list_set_status(i, L"Clean", detail);
        }
        update_counts();
    }

    if (g_cancel) {
        progress_stop(i);
        progress_set_text(L"Cancelled after %d of %d file(s).",
                          i, g_file_count);
        log_append(L"Cancelled after %d of %d file(s).", i, g_file_count);
    } else {
        progress_finish(g_file_count);
        swprintf_s(summary, ARRAYSIZE(summary),
                   L"Done: %d %s, %d already fine, %d failed%s",
                   fixed, dry_run ? L"would be fixed" : L"fixed",
                   clean, failed, dry_run ? L" (dry run)" : L"");
        progress_set_text(L"%s", summary);
        log_append(L"%s", summary);
    }

    g_cancel = 0;
    g_busy = 0;
    set_buttons_enabled(TRUE);
    update_counts();
}

// win dialogs

static void add_files_dialog(void)
{
    wchar_t *buffer;
    OPENFILENAMEW ofn;
    const DWORD buffer_cch = 65536;

    buffer = (wchar_t *)calloc(buffer_cch, sizeof(wchar_t));
    if (!buffer)
        return;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"DNG files (*.dng)\0*.dng\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = buffer_cch;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        wchar_t *p = buffer;
        wchar_t *name = p + wcslen(p) + 1;
        int before = g_file_count;

        if (*name) {
            while (*name) {
                wchar_t full[MAX_PATH];
                join_path(full, ARRAYSIZE(full), p, name);
                add_path(full, 0);
                name += wcslen(name) + 1;
            }
        } else {
            add_path(p, 0);
        }
        for (; before < g_file_count; before++)
            list_add_file(before);
        update_counts();
        if (g_file_count > 0)
            SetFocus(g_file_list);
    }
    free(buffer);
}

static int browse_for_folder(const wchar_t *title, wchar_t *out,
                             size_t out_cch)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    int ok = 0;

    if (!out || out_cch == 0)
        return 0;
    out[0] = L'\0';
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_main;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, out))
            ok = 1;
        CoTaskMemFree(pidl);
    }
    return ok;
}

static void choose_output_folder(void)
{
    wchar_t dir[MAX_PATH];

    if (browse_for_folder(L"Choose output folder for repaired DNG files",
                          dir, ARRAYSIZE(dir))) {
        wcsncpy_s(g_output_dir, ARRAYSIZE(g_output_dir), dir, _TRUNCATE);
        update_output_label();
        update_counts();
        log_append(L"Output folder set to: %s", g_output_dir);
    }
}

static void add_folder_dialog(void)
{
    wchar_t dir[MAX_PATH];
    int before = g_file_count;

    if (browse_for_folder(L"Add every DNG file in a folder",
                          dir, ARRAYSIZE(dir))) {
        int added;
        add_path(dir, 1);
        added = g_file_count - before;
        for (; before < g_file_count; before++)
            list_add_file(before);
        update_counts();
        log_append(L"Added %d file(s) from %s", added, dir);
    }
}

static void open_folder(const wchar_t *dir)
{
    if (!dir || !*dir) {
        MessageBoxW(g_main, L"No folder has been selected.", APP_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(g_main, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

static void show_about(void)
{
    wchar_t text[2048];

    swprintf_s(text, ARRAYSIZE(text),
               L"DNGFix %s\r\n\r\n"
               L"Repairs metadata in DNG files created by Android's "
               L"DngCreator without touching raw image data.\r\n\r\n"
               L"Profiles folder:\r\n%s\r\n\r\n"
               L"Output folder:\r\n%s\r\n\r\n"
               L"All repairs are applied to a copy; the source file is "
               L"never modified.",
               APP_VERSION,
               g_profile_dir[0] ? g_profile_dir : L"(not configured)",
               g_output_dir[0] ? g_output_dir : L"(beside source)");
    MessageBoxW(g_main, text, L"About DNGFix",
                MB_OK | MB_ICONINFORMATION);
}

// window layouts

static HFONT make_font(const wchar_t *face, int points)
{
    HDC dc = GetDC(NULL);
    int height;

    if (!dc)
        return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    height = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static HFONT make_ui_font(void)
{
    NONCLIENTMETRICSW metrics;

    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0)) {
        metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return make_font(L"Segoe UI", 9);
}

static HWND make_control(const wchar_t *class_name, const wchar_t *text,
                         DWORD style, DWORD ex_style, int id)
{
    return CreateWindowExW(ex_style, class_name, text,
                           WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id,
                           g_inst, NULL);
}

static void set_font(HWND hwnd, HFONT font)
{
    if (hwnd && font)
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

static void layout_controls(int width, int height)
{
    int margin = 10;
    int top_y = 10;
    int top_h = 24;
    int gap = 8;
    int button_h = 28;
    int list_y = top_y + top_h + gap;
    int bottom_y = height - margin - button_h;
    int log_h = 150;
    int progress_h = 18;
    int progress_gap = 6;
    int list_h = bottom_y - gap - log_h - progress_gap - progress_h - list_y;
    int progress_y;
    int log_y;
    int label_w = 52;
    int combo_w = 290;
    int output_w = 100;

    if (list_h < 90)
        list_h = 90;
    progress_y = list_y + list_h + gap;
    log_y = progress_y + progress_h + progress_gap;

    if (g_main) {
        HWND profile_label = GetDlgItem(g_main, IDC_PROFILE_LABEL);
        MoveWindow(profile_label, margin, top_y + 4, label_w, 20, TRUE);
        MoveWindow(g_profile_combo, margin + label_w, top_y, combo_w, 220,
                   TRUE);
        MoveWindow(g_output_label, margin + label_w + combo_w + gap,
                   top_y + 4,
                   width - (margin + label_w + combo_w + gap) -
                       (output_w + gap + margin),
                   20, TRUE);
        MoveWindow(GetDlgItem(g_main, IDM_OUTPUT_FOLDER),
                   width - margin - output_w, top_y, output_w, top_h, TRUE);
        MoveWindow(g_file_list, margin, list_y, width - margin * 2,
                   list_h, TRUE);
        MoveWindow(g_progress_label, margin, progress_y,
                   (width - margin * 2) * 55 / 100, progress_h, TRUE);
        MoveWindow(g_progress, margin + (width - margin * 2) * 55 / 100 + gap,
                   progress_y,
                   width - margin * 2 - (width - margin * 2) * 55 / 100 - gap,
                   progress_h, TRUE);
        MoveWindow(g_log, margin, log_y, width - margin * 2,
                   bottom_y - gap - log_y, TRUE);

        MoveWindow(GetDlgItem(g_main, IDM_ADD_FILES), margin, bottom_y,
                   100, button_h, TRUE);
        MoveWindow(GetDlgItem(g_main, IDM_ADD_FOLDER), margin + 108,
                   bottom_y, 100, button_h, TRUE);
        MoveWindow(GetDlgItem(g_main, IDM_DRY_RUN), margin + 216,
                   bottom_y, 100, button_h, TRUE);
        MoveWindow(GetDlgItem(g_main, IDM_FIX), margin + 324,
                   bottom_y, 100, button_h, TRUE);
        MoveWindow(g_status, margin + 432, bottom_y + 6,
                   width - (margin + 432) - margin, 20, TRUE);

        if (g_file_list) {
            RECT rc;
            GetClientRect(g_file_list, &rc);
            ListView_SetColumnWidth(g_file_list, 2,
                                    max(180, rc.right - 430 - 4));
        }
    }
}

static HMENU build_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU repair = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuW(file, MF_STRING, IDM_ADD_FILES,
                L"&Add DNG files...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_ADD_FOLDER,
                L"Add DNG &folder...\tCtrl+D");
    AppendMenuW(file, MF_STRING, IDM_CLEAR_LIST, L"&Clear list");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_OUTPUT_FOLDER,
                L"Choose &output folder...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT_APP, L"E&xit");

    AppendMenuW(repair, MF_STRING, IDM_DRY_RUN, L"&Dry run\tF5");
    AppendMenuW(repair, MF_STRING, IDM_FIX, L"&Fix DNG files\tF9");
    AppendMenuW(repair, MF_SEPARATOR, 0, NULL);
    AppendMenuW(repair, MF_STRING, IDM_RELOAD_PROFILES,
                L"&Reload profiles");
    AppendMenuW(repair, MF_STRING, IDM_OPEN_PROFILES,
                L"Open profiles folder");
    AppendMenuW(repair, MF_STRING, IDM_OPEN_OUTPUT, L"Open output folder");

    AppendMenuW(help, MF_STRING, IDM_ABOUT_APP, L"&About...");

    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)repair, L"&Repair");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"&Help");
    return menu;
}

static void create_ui(void)
{
    HWND label;
    LVCOLUMNW column;

    g_ui_font = make_ui_font();
    g_log_font = make_font(L"Consolas", 9);

    label = make_control(L"STATIC", L"Profile:", SS_LEFT, 0,
                         IDC_PROFILE_LABEL);
    set_font(label, g_ui_font);

    g_profile_combo = make_control(L"COMBOBOX", L"",
                                   CBS_DROPDOWNLIST | WS_VSCROLL |
                                       WS_TABSTOP,
                                   0, IDC_PROFILE_COMBO);
    set_font(g_profile_combo, g_ui_font);

    g_output_label = make_control(L"STATIC", L"",
                                  SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX,
                                  0, IDC_OUTPUT_LABEL);
    set_font(g_output_label, g_ui_font);

    make_control(L"BUTTON", L"Output...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                 IDM_OUTPUT_FOLDER);
    set_font(GetDlgItem(g_main, IDM_OUTPUT_FOLDER), g_ui_font);

    g_file_list = make_control(WC_LISTVIEWW, L"",
                               LVS_REPORT | LVS_SINGLESEL |
                                   LVS_SHOWSELALWAYS | WS_BORDER |
                                   WS_TABSTOP,
                               WS_EX_CLIENTEDGE, IDC_FILE_LIST);
    set_font(g_file_list, g_ui_font);
    ListView_SetExtendedListViewStyle(g_file_list,
                                      LVS_EX_FULLROWSELECT |
                                          LVS_EX_DOUBLEBUFFER |
                                          LVS_EX_GRIDLINES);
    memset(&column, 0, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = L"File";
    column.cx = 340;
    ListView_InsertColumn(g_file_list, 0, &column);
    column.pszText = L"Status";
    column.cx = 90;
    ListView_InsertColumn(g_file_list, 1, &column);
    column.pszText = L"Details";
    column.cx = 300;
    ListView_InsertColumn(g_file_list, 2, &column);

    g_log = make_control(L"EDIT", L"",
                         ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                             WS_VSCROLL | WS_TABSTOP,
                         WS_EX_CLIENTEDGE, IDC_LOG);
    set_font(g_log, g_log_font);
    SendMessageW(g_log, EM_SETLIMITTEXT, 1000000, 0);

    g_progress_label = make_control(L"STATIC", L"Ready.",
                                    SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX,
                                    0, IDC_PROGRESS_LABEL);
    set_font(g_progress_label, g_ui_font);

    g_progress = make_control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0,
                              IDC_PROGRESS);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 1);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);

    make_control(L"BUTTON", L"Add files...",
                 BS_PUSHBUTTON | WS_TABSTOP, 0, IDM_ADD_FILES);
    make_control(L"BUTTON", L"Add folder...",
                 BS_PUSHBUTTON | WS_TABSTOP, 0, IDM_ADD_FOLDER);
    make_control(L"BUTTON", L"Dry run",
                 BS_PUSHBUTTON | WS_TABSTOP, 0, IDM_DRY_RUN);
    make_control(L"BUTTON", L"Fix DNGs",
                 BS_PUSHBUTTON | WS_TABSTOP, 0, IDM_FIX);
    set_font(GetDlgItem(g_main, IDM_ADD_FILES), g_ui_font);
    set_font(GetDlgItem(g_main, IDM_ADD_FOLDER), g_ui_font);
    set_font(GetDlgItem(g_main, IDM_DRY_RUN), g_ui_font);
    set_font(GetDlgItem(g_main, IDM_FIX), g_ui_font);

    g_status = make_control(L"STATIC", L"",
                            SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, 0,
                            IDC_STATUS);
    set_font(g_status, g_ui_font);

    populate_profile_combo();
    update_output_label();
    update_counts();

    for (int i = 0; i < g_file_count; i++)
        list_add_file(i);

    log_append(L"DNGFix %s ready. Profiles: %s", APP_VERSION,
               g_profile_dir[0] ? g_profile_dir : L"(none)");
    if (g_startup_message[0]) {
        log_append(L"Profile note: %s", g_startup_message);
        g_startup_message[0] = L'\0';
    }
    if (g_profile_count == 0) {
        log_append(L"No profile files were loaded; the built-in generic "
                   L"profile will be used.");
    }
    if (g_file_count > 0)
        log_append(L"Loaded %d file(s) from the command line.", g_file_count);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_main = hwnd;
        create_ui();
        DragAcceptFiles(hwnd, TRUE);
        SetMenu(hwnd, build_menu());
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            layout_controls(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            mmi->ptMinTrackSize.x = 820;
            mmi->ptMinTrackSize.y = 560;
        }
        return 0;

    case WM_COMMAND:
        if (g_busy && LOWORD(wParam) != IDM_ABOUT_APP) {
            if (LOWORD(wParam) == IDM_EXIT_APP)
                request_cancel();
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDM_ADD_FILES:
            add_files_dialog();
            return 0;
        case IDM_ADD_FOLDER:
            add_folder_dialog();
            return 0;
        case IDM_CLEAR_LIST:
            files_clear();
            if (g_file_list)
                ListView_DeleteAllItems(g_file_list);
            update_counts();
            log_append(L"File list cleared.");
            return 0;
        case IDM_OUTPUT_FOLDER:
            choose_output_folder();
            return 0;
        case IDM_DRY_RUN:
            run_batch(1);
            return 0;
        case IDM_FIX:
            run_batch(0);
            return 0;
        case IDM_RELOAD_PROFILES:
            {
                wchar_t selected_name[128];
                int selected = combo_selected_profile();
                int was_auto = (selected < 0);

                selected_name[0] = L'\0';
                if (selected >= 0 && selected < g_profile_count)
                    wcsncpy_s(selected_name, ARRAYSIZE(selected_name),
                              g_profiles[selected].name, _TRUNCATE);
                g_initial_profile_index = -1;
                load_profiles();
                populate_profile_combo();
                if (was_auto) {
                    ComboBox_SetCurSel(g_profile_combo, 0);
                } else if (selected_name[0]) {
                    int i;
                    for (i = 0; i < g_profile_count; i++) {
                        if (!_wcsicmp(g_profiles[i].name, selected_name)) {
                            ComboBox_SetCurSel(g_profile_combo, i + 1);
                            break;
                        }
                    }
                }
                log_append(L"Reloaded %d profile(s).", g_profile_count);
            }
            return 0;
        case IDM_OPEN_PROFILES:
            open_folder(g_profile_dir);
            return 0;
        case IDM_OPEN_OUTPUT:
            if (!g_output_dir[0]) {
                MessageBoxW(hwnd,
                            L"Output is currently set to the source folder. "
                            L"Choose an output folder first.",
                            APP_TITLE, MB_OK | MB_ICONINFORMATION);
            } else {
                CreateDirectoryW(g_output_dir, NULL);
                open_folder(g_output_dir);
            }
            return 0;
        case IDM_ABOUT_APP:
            show_about();
            return 0;
        case IDM_EXIT_APP:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wParam;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            UINT i;
            int before = g_file_count;
            int added;

            if (g_busy) {
                DragFinish(drop);
                return 0;
            }

            for (i = 0; i < count; i++) {
                wchar_t path[MAX_PATH];
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path)) > 0)
                    add_path(path, 1);
            }
            DragFinish(drop);
            added = g_file_count - before;
            for (; before < g_file_count; before++)
                list_add_file(before);
            update_counts();
            if (added > 0)
                log_append(L"Added %d file(s) by drag and drop.", added);
            return 0;
        }

    case WM_CLOSE:
        if (g_busy) {
            request_cancel();
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        files_clear();
        if (g_ui_font)
            DeleteObject(g_ui_font);
        if (g_log_font)
            DeleteObject(g_log_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static int run_gui(void)
{
    WNDCLASSEXW wc;
    MSG msg;
    HACCEL accel;
    HRESULT ole_hr;
    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'O', IDM_ADD_FILES },
        { FVIRTKEY | FCONTROL, 'D', IDM_ADD_FOLDER },
        { FVIRTKEY, VK_F5, IDM_DRY_RUN },
        { FVIRTKEY, VK_F9, IDM_FIX }
    };

    ole_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_DNGFIX));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DNGFixMainWindow";
    if (!RegisterClassExW(&wc)) {
        if (SUCCEEDED(ole_hr))
            CoUninitialize();
        return 1;
    }

    g_main = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0,
                             980, 700, NULL, NULL, g_inst, NULL);
    if (!g_main) {
        if (SUCCEEDED(ole_hr))
            CoUninitialize();
        return 1;
    }

    accel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!accel || !TranslateAcceleratorW(g_main, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (accel)
        DestroyAcceleratorTable(accel);
    if (SUCCEEDED(ole_hr))
        CoUninitialize();
    return (int)msg.wParam;
}

// cli

static void attach_console(void)
{
    FILE *f;

    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
        _setmode(_fileno(stdout), _O_U16TEXT);
        _setmode(_fileno(stderr), _O_U16TEXT);
        g_console_ready = 1;
    }
}

static void print_usage(void)
{
    cli_print(L"DNGFix %s - repair metadata in DNG files", APP_VERSION);
    cli_print(L"");
    cli_print(L"usage:");
    cli_print(L"  DNGFix.exe                         open the graphical app");
    cli_print(L"  DNGFix.exe [options] FILE|FOLDER ...");
    cli_print(L"");
    cli_print(L"options:");
    cli_print(L"  -o, --output DIR       write repaired copies into DIR");
    cli_print(L"  -p, --profile NAME|INI force one profile (name or .ini path)");
    cli_print(L"      --profiles DIR     load profiles from DIR");
    cli_print(L"  -n, --dry-run          report only, write nothing");
    cli_print(L"  -f, --force            overwrite existing output files");
    cli_print(L"      --log FILE         also write a UTF-8 log to FILE");
    cli_print(L"      --gui              open the GUI with the given files");
    cli_print(L"  -h, --help             show this help");
    cli_print(L"");
    cli_print(L"profiles are INI files.  the default search path is");
    cli_print(L"<exe>\\profiles, then <cwd>\\profiles, then "
              L"%%APPDATA%%\\DNGFix\\profiles.");
}

static int cli_select_profile(const wchar_t *file, const DngProfile **profile,
                              wchar_t *err, size_t err_cch)
{
    if (g_initial_profile_index >= 0 &&
        g_initial_profile_index < g_profile_count) {
        *profile = &g_profiles[g_initial_profile_index];
        return 0;
    }

    {
        wchar_t make[128];
        wchar_t model[128];
        wchar_t unique_model[256];
        int index;

        if (dng_probe(file, make, ARRAYSIZE(make), model, ARRAYSIZE(model),
                      unique_model, ARRAYSIZE(unique_model),
                      err, err_cch) != 0)
            return -1;
        index = dng_profile_find_best(g_profiles, g_profile_count,
                                      make, model, unique_model);
        if (index >= 0) {
            *profile = &g_profiles[index];
            return 0;
        }
        if (err && err_cch) {
            _snwprintf_s(err, err_cch, _TRUNCATE,
                         L"no profile matched %s / %s; using builtin generic",
                         make[0] ? make : L"(no make)",
                         model[0] ? model : L"(no model)");
        }
        *profile = &g_builtin;
        return 0;
    }
}

static int run_cli(void)
{
    int i;
    int fixed = 0;
    int clean = 0;
    int failed = 0;

    if (g_file_count == 0) {
        cli_print(L"No input files.");
        return 1;
    }

    cli_print(L"DNGFix %s", APP_VERSION);
    if (g_profile_dir[0])
        cli_print(L"Profiles: %s", g_profile_dir);
    cli_print(L"");

    for (i = 0; i < g_file_count; i++) {
        const DngProfile *profile = NULL;
        DngResult result;
        wchar_t err[512];

        err[0] = L'\0';
        if (cli_select_profile(g_files[i], &profile,
                               err, ARRAYSIZE(err)) != 0) {
            failed++;
            cli_print(L"FAIL  %s: %s", g_files[i], err);
            continue;
        }
        if (err[0])
            cli_print(L"note  %s", err);
        cli_print(L"%s  [profile: %s]",
                  g_batch_dry ? L"DRY   " : L"RUN   ", profile->name);

        memset(&result, 0, sizeof(result));
        if (dng_process_file(g_files[i],
                             g_output_dir[0] ? g_output_dir : NULL,
                             profile, g_batch_dry, g_force,
                             cli_engine_log, NULL, &result) != 0) {
            failed++;
            cli_print(L"FAIL  %s", g_files[i]);
            continue;
        }
        if (result.repairs > 0) {
            fixed++;
            cli_print(L"%s  %s (%d repair(s))",
                      g_batch_dry ? L"WOULD FIX" : L"FIXED",
                      g_files[i], result.repairs);
        } else {
            clean++;
            cli_print(L"OK    %s (nothing to fix)", g_files[i]);
        }
    }

    cli_print(L"");
    cli_print(L"%d fixed, %d already fine, %d failed%s",
              fixed, clean, failed, g_batch_dry ? L" (dry run)" : L"");
    return failed ? 1 : 0;
}

// entrypoiny

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrev,
                      _In_ LPWSTR command_line, _In_ int show)
{
    int argc = 0;
    wchar_t **argv;
    int dry_run = 0;
    int force = 0;
    int gui = 0;
    int want_help = 0;
    int cli_mode;
    int i;
    wchar_t *output_arg = NULL;
    wchar_t *profiles_arg = NULL;
    wchar_t *profile_arg = NULL;
    wchar_t *log_arg = NULL;
    wchar_t *input_args[512];
    int input_count = 0;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev;
    (void)command_line;
    (void)show;

    g_inst = hInstance;
    SetProcessDPIAware();
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    dng_profile_defaults(&g_builtin);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        argc = 0;

    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];

        if (!_wcsicmp(a, L"--help") || !_wcsicmp(a, L"-h") ||
            !_wcsicmp(a, L"/?")) {
            want_help = 1;
        } else if (!_wcsicmp(a, L"--dry-run") || !_wcsicmp(a, L"-n")) {
            dry_run = 1;
        } else if (!_wcsicmp(a, L"--force") || !_wcsicmp(a, L"-f")) {
            force = 1;
        } else if (!_wcsicmp(a, L"--gui")) {
            gui = 1;
        } else if (!_wcsicmp(a, L"--output") || !_wcsicmp(a, L"-o")) {
            if (i + 1 < argc)
                output_arg = argv[++i];
        } else if (!_wcsicmp(a, L"--profile") || !_wcsicmp(a, L"-p")) {
            if (i + 1 < argc)
                profile_arg = argv[++i];
        } else if (!_wcsicmp(a, L"--profiles")) {
            if (i + 1 < argc)
                profiles_arg = argv[++i];
        } else if (!_wcsicmp(a, L"--log")) {
            if (i + 1 < argc)
                log_arg = argv[++i];
        } else if (a[0] == L'-' || a[0] == L'/') {
            attach_console();
            cli_print(L"Unknown option: %s", a);
            print_usage();
            LocalFree(argv);
            return 2;
        } else if (input_count < (int)ARRAYSIZE(input_args)) {
            input_args[input_count++] = argv[i];
        }
    }

    cli_mode = (input_count > 0 && !gui) || want_help;

    if (log_arg && cli_mode) {
        g_cli_log = _wfopen(log_arg, L"wb");
    }
    if (cli_mode)
        attach_console();

    if (want_help) {
        print_usage();
        if (g_cli_log)
            fclose(g_cli_log);
        LocalFree(argv);
        return 0;
    }

    if (output_arg)
        wcsncpy_s(g_output_dir, ARRAYSIZE(g_output_dir), output_arg,
                  _TRUNCATE);
    if (profiles_arg)
        resolve_profile_dir(profiles_arg);
    else
        resolve_profile_dir(NULL);
    if (profile_arg)
        wcsncpy_s(g_profile_arg, ARRAYSIZE(g_profile_arg), profile_arg,
                  _TRUNCATE);
    if (g_profile_arg[0] && !path_exists(g_profile_arg) &&
        ends_with_i(g_profile_arg, L".ini") && g_profile_dir[0]) {
        wchar_t candidate[MAX_PATH];
        join_path(candidate, ARRAYSIZE(candidate), g_profile_dir,
                  g_profile_arg);
        if (path_exists(candidate))
            wcsncpy_s(g_profile_arg, ARRAYSIZE(g_profile_arg), candidate,
                      _TRUNCATE);
    }
    if (g_profile_arg[0] && path_exists(g_profile_arg) &&
        !path_is_dir(g_profile_arg)) {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW(g_profile_arg, ARRAYSIZE(full), full, NULL) > 0)
            wcsncpy_s(g_profile_arg, ARRAYSIZE(g_profile_arg), full,
                      _TRUNCATE);
    }
    g_force = force;

    if (cli_mode) {
        for (i = 0; i < input_count; i++)
            add_path(input_args[i], 1);
        if (g_file_count == 0) {
            cli_print(L"No .dng files found in the supplied paths.");
            if (g_cli_log)
                fclose(g_cli_log);
            LocalFree(argv);
            return 1;
        }
        load_profiles();
        if (g_profile_arg[0] && g_initial_profile_index < 0) {
            cli_print(L"Profile error: %s",
                      g_startup_message[0] ? g_startup_message
                                           : L"profile not found");
            if (g_cli_log)
                fclose(g_cli_log);
            LocalFree(argv);
            return 2;
        }
        g_batch_dry = dry_run;
        {
            int rc = run_cli();
            if (g_cli_log)
                fclose(g_cli_log);
            LocalFree(argv);
            return rc;
        }
    }

    load_profiles();
    for (i = 0; i < input_count; i++)
        add_path(input_args[i], 1);
    {
        int rc = run_gui();
        if (g_cli_log)
            fclose(g_cli_log);
        LocalFree(argv);
        return rc;
    }
}
