#include "dngfix.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static void set_err(wchar_t *err, size_t err_cch, const wchar_t *fmt, ...)
{
    va_list ap;

    if (!err || err_cch == 0)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(err, err_cch, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void trim_ws(wchar_t *s)
{
    wchar_t *p = s;
    size_t len;

    if (!s)
        return;
    while (*p && iswspace(*p))
        p++;
    if (p != s)
        memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    len = wcslen(s);
    while (len > 0 && iswspace(s[len - 1]))
        s[--len] = L'\0';
}

static void ini_string(const wchar_t *path, const wchar_t *section,
                       const wchar_t *key, const wchar_t *def,
                       wchar_t *out, DWORD out_cch)
{
    GetPrivateProfileStringW(section, key, def ? def : L"",
                             out, out_cch, path);
    trim_ws(out);
}

static int parse_bool(const wchar_t *s, int def)
{
    if (!s || !*s)
        return def;
    if (!_wcsicmp(s, L"1") || !_wcsicmp(s, L"yes") ||
        !_wcsicmp(s, L"true") || !_wcsicmp(s, L"on") ||
        !_wcsicmp(s, L"enabled"))
        return 1;
    if (!_wcsicmp(s, L"0") || !_wcsicmp(s, L"no") ||
        !_wcsicmp(s, L"false") || !_wcsicmp(s, L"off") ||
        !_wcsicmp(s, L"disabled"))
        return 0;
    return def;
}

static int ini_bool(const wchar_t *path, const wchar_t *section,
                    const wchar_t *key, int def)
{
    wchar_t buf[64];
    ini_string(path, section, key, L"", buf, ARRAYSIZE(buf));
    return parse_bool(buf, def);
}

static long ini_long(const wchar_t *path, const wchar_t *section,
                     const wchar_t *key, long def)
{
    wchar_t buf[128];
    wchar_t *end = NULL;
    long value;

    ini_string(path, section, key, L"", buf, ARRAYSIZE(buf));
    if (!buf[0])
        return def;
    value = wcstol(buf, &end, 10);
    if (end == buf)
        return def;
    return value;
}

static double ini_double(const wchar_t *path, const wchar_t *section,
                         const wchar_t *key, double def)
{
    wchar_t buf[128];
    wchar_t *end = NULL;
    double value;

    ini_string(path, section, key, L"", buf, ARRAYSIZE(buf));
    if (!buf[0])
        return def;
    value = wcstod(buf, &end);
    if (end == buf)
        return def;
    return value;
}

static void parse_version(const wchar_t *text, unsigned char out[4])
{
    const wchar_t *p = text;
    int i;

    for (i = 0; i < 4; i++)
        out[i] = 0;
    if (!text)
        return;

    for (i = 0; i < 4 && *p; i++) {
        wchar_t *end = NULL;
        long value = wcstol(p, &end, 10);

        if (end == p)
            break;
        if (value < 0)
            value = 0;
        if (value > 255)
            value = 255;
        out[i] = (unsigned char)value;
        p = end;
        if (*p == L'.' || *p == L',')
            p++;
        else
            break;
    }
}

static void default_name_from_path(const wchar_t *path, wchar_t *out,
                                   size_t out_cch)
{
    const wchar_t *base = path;
    const wchar_t *slash;
    const wchar_t *dot;
    size_t len;

    if (!path || !out || out_cch == 0)
        return;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        slash = wcsrchr(path, L'/');
    if (slash)
        base = slash + 1;
    dot = wcsrchr(base, L'.');
    len = dot ? (size_t)(dot - base) : wcslen(base);
    if (len >= out_cch)
        len = out_cch - 1;
    memcpy(out, base, len * sizeof(wchar_t));
    out[len] = L'\0';
}

static int valid_suffix(const wchar_t *suffix)
{
    const wchar_t *p;

    if (!suffix || !*suffix)
        return 1;
    for (p = suffix; *p; p++) {
        if (*p == L'\\' || *p == L'/' || *p == L':' ||
            *p == L'*' || *p == L'?' || *p == L'"' ||
            *p == L'<' || *p == L'>' || *p == L'|')
            return 0;
    }
    return 1;
}

static int file_has_utf8_bom(const wchar_t *path)
{
    FILE *f;
    unsigned char b[3];
    size_t n;

    f = _wfopen(path, L"rb");
    if (!f)
        return 0;
    n = fread(b, 1, sizeof(b), f);
    fclose(f);
    return n == 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF;
}

static int copy_without_utf8_bom(const wchar_t *src, wchar_t *tmp,
                                 size_t tmp_cch)
{
    wchar_t temp_dir[MAX_PATH];
    FILE *in = NULL;
    FILE *out = NULL;
    unsigned char buffer[65536];
    size_t n;
    int ok = 0;

    if (GetTempPathW(ARRAYSIZE(temp_dir), temp_dir) == 0)
        return 0;
    if (GetTempFileNameW(temp_dir, L"dng", 0, tmp) == 0)
        return 0;

    in = _wfopen(src, L"rb");
    if (!in)
        goto done;
    out = _wfopen(tmp, L"wb");
    if (!out)
        goto done;
    if (fread(buffer, 1, 3, in) != 3)
        goto done;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n)
            goto done;
    }
    ok = 1;

done:
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (!ok)
        DeleteFileW(tmp);
    return ok;
}

void dng_profile_defaults(DngProfile *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    wcscpy_s(p->name, ARRAYSIZE(p->name), L"Built-in generic");
    wcscpy_s(p->description, ARRAYSIZE(p->description),
             L"Fallback metadata repair for Android DngCreator files");
    p->enabled = 1;
    p->priority = 0;

    p->fix_calibration = 1;
    p->fix_illuminants = 1;
    p->fix_version = 1;
    p->verify = 1;

    p->calibration_tolerance = 1e-6;
    p->illuminants_require_pairs = 1;
    p->illuminants_skip_huesatmap = 1;

    p->version_min[0] = 1;
    p->version_min[1] = 1;
    p->version_min[2] = 0;
    p->version_min[3] = 0;
    p->version_opcode[0] = 1;
    p->version_opcode[1] = 3;
    p->version_opcode[2] = 0;
    p->version_opcode[3] = 0;
    p->version_forward[0] = 1;
    p->version_forward[1] = 2;
    p->version_forward[2] = 0;
    p->version_forward[3] = 0;
    p->version_use_opcode_list = 1;
    p->version_use_forward_matrix = 1;

    wcscpy_s(p->output_suffix, ARRAYSIZE(p->output_suffix), L"_fixed");
    p->output_overwrite = 0;
    p->keep_unmodified = 0;
    p->max_file_mb = 4096;
}

static void load_cct_overrides(const wchar_t *path, DngProfile *p)
{
    wchar_t *section;
    const DWORD section_cch = 32768;
    DWORD got;
    wchar_t *line;

    section = (wchar_t *)calloc(section_cch, sizeof(wchar_t));
    if (!section)
        return;

    got = GetPrivateProfileSectionW(L"illuminants", section, section_cch, path);
    if (got > 0 && got < section_cch - 2) {
        for (line = section; *line; line += wcslen(line) + 1) {
            wchar_t *eq = wcschr(line, L'=');
            wchar_t *key;
            wchar_t *value;
            long code;
            long kelvin;
            wchar_t *end = NULL;
            int i;

            if (!eq)
                continue;
            *eq = L'\0';
            key = line;
            value = eq + 1;
            trim_ws(key);
            trim_ws(value);

            if (_wcsnicmp(key, L"cct.", 4) == 0)
                key += 4;
            else if (_wcsnicmp(key, L"cct_", 4) == 0)
                key += 4;
            else
                continue;

            code = wcstol(key, &end, 10);
            if (end == key || code <= 0 || code > 65535)
                continue;
            kelvin = wcstol(value, &end, 10);
            if (end == value || kelvin < 500 || kelvin > 100000)
                continue;

            for (i = 0; i < p->cct_override_count; i++) {
                if (p->cct_overrides[i].code == (unsigned short)code) {
                    p->cct_overrides[i].kelvin = (int)kelvin;
                    break;
                }
            }
            if (i == p->cct_override_count &&
                p->cct_override_count < DNG_MAX_CCT_OVERRIDES) {
                p->cct_overrides[i].code = (unsigned short)code;
                p->cct_overrides[i].kelvin = (int)kelvin;
                p->cct_override_count++;
            }
        }
    }
    free(section);
}

int dng_profile_load(const wchar_t *path, DngProfile *profile,
                     wchar_t *err, size_t err_cch)
{
    DWORD attrs;
    wchar_t buf[512];
    long value;

    if (err && err_cch)
        err[0] = L'\0';
    if (!path || !profile) {
        set_err(err, err_cch, L"invalid profile path");
        return -1;
    }

    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        set_err(err, err_cch, L"profile not found: %s", path);
        return -1;
    }

    if (file_has_utf8_bom(path)) {
        wchar_t tmp[MAX_PATH];
        wchar_t original_stem[128];
        wchar_t temp_stem[128];
        wchar_t load_err[512];
        DngProfile loaded;
        int rc;

        default_name_from_path(path, original_stem,
                               ARRAYSIZE(original_stem));
        if (!copy_without_utf8_bom(path, tmp, ARRAYSIZE(tmp))) {
            set_err(err, err_cch,
                    L"cannot read UTF-8 profile: %s", path);
            return -1;
        }
        default_name_from_path(tmp, temp_stem, ARRAYSIZE(temp_stem));
        rc = dng_profile_load(tmp, &loaded, load_err,
                              ARRAYSIZE(load_err));
        DeleteFileW(tmp);
        if (rc != 0) {
            set_err(err, err_cch, L"%s", load_err);
            return -1;
        }
        wcsncpy_s(loaded.path, ARRAYSIZE(loaded.path), path, _TRUNCATE);
        if (!_wcsicmp(loaded.name, temp_stem))
            wcsncpy_s(loaded.name, ARRAYSIZE(loaded.name),
                      original_stem, _TRUNCATE);
        *profile = loaded;
        return 0;
    }

    dng_profile_defaults(profile);
    wcsncpy_s(profile->path, ARRAYSIZE(profile->path), path, _TRUNCATE);

    default_name_from_path(path, buf, ARRAYSIZE(buf));
    if (!buf[0])
        wcscpy_s(buf, ARRAYSIZE(buf), L"Unnamed profile");
    ini_string(path, L"profile", L"name", buf,
               profile->name, ARRAYSIZE(profile->name));
    ini_string(path, L"profile", L"description", L"",
               profile->description, ARRAYSIZE(profile->description));
    profile->enabled = ini_bool(path, L"profile", L"enabled", 1);
    value = ini_long(path, L"profile", L"priority", 0);
    if (value < 0)
        value = 0;
    if (value > 10000)
        value = 10000;
    profile->priority = (int)value;

    ini_string(path, L"match", L"make", L"",
               profile->match_make, ARRAYSIZE(profile->match_make));
    ini_string(path, L"match", L"model", L"",
               profile->match_model, ARRAYSIZE(profile->match_model));
    ini_string(path, L"match", L"unique_model", L"",
               profile->match_unique_model,
               ARRAYSIZE(profile->match_unique_model));

    profile->fix_calibration =
        ini_bool(path, L"fixes", L"calibration", profile->fix_calibration);
    profile->fix_illuminants =
        ini_bool(path, L"fixes", L"illuminants", profile->fix_illuminants);
    profile->fix_version =
        ini_bool(path, L"fixes", L"version", profile->fix_version);
    profile->verify = ini_bool(path, L"fixes", L"verify", profile->verify);

    profile->calibration_tolerance =
        ini_double(path, L"calibration", L"singular_tolerance",
                   profile->calibration_tolerance);
    if (!(profile->calibration_tolerance > 0.0))
        profile->calibration_tolerance = 1e-6;

    profile->illuminants_require_pairs =
        ini_bool(path, L"illuminants", L"require_pairs",
                 profile->illuminants_require_pairs);
    profile->illuminants_skip_huesatmap =
        ini_bool(path, L"illuminants", L"skip_huesatmap",
                 profile->illuminants_skip_huesatmap);
    load_cct_overrides(path, profile);

    ini_string(path, L"version", L"minimum", L"",
               buf, ARRAYSIZE(buf));
    if (buf[0])
        parse_version(buf, profile->version_min);
    ini_string(path, L"version", L"opcode", L"",
               buf, ARRAYSIZE(buf));
    if (buf[0])
        parse_version(buf, profile->version_opcode);
    ini_string(path, L"version", L"forward_matrix", L"",
               buf, ARRAYSIZE(buf));
    if (buf[0])
        parse_version(buf, profile->version_forward);
    profile->version_use_opcode_list =
        ini_bool(path, L"version", L"use_opcode_list",
                 profile->version_use_opcode_list);
    profile->version_use_forward_matrix =
        ini_bool(path, L"version", L"use_forward_matrix",
                 profile->version_use_forward_matrix);

    ini_string(path, L"output", L"suffix", profile->output_suffix,
               profile->output_suffix, ARRAYSIZE(profile->output_suffix));
    if (!valid_suffix(profile->output_suffix)) {
        set_err(err, err_cch, L"invalid output suffix in %s", path);
        return -1;
    }
    profile->output_overwrite =
        ini_bool(path, L"output", L"overwrite", profile->output_overwrite);
    profile->keep_unmodified =
        ini_bool(path, L"output", L"keep_unmodified",
                 profile->keep_unmodified);
    value = ini_long(path, L"output", L"max_file_mb",
                     (long)profile->max_file_mb);
    if (value < 1)
        value = 1;
    if (value > 1048576)
        value = 1048576;
    profile->max_file_mb = (unsigned)value;

    return 0;
}

int dng_profiles_load_dir(const wchar_t *dir, DngProfile *profiles,
                          int max_profiles, wchar_t *err, size_t err_cch)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t pattern[MAX_PATH];
    int count = 0;

    if (err && err_cch)
        err[0] = L'\0';
    if (!dir || !*dir || !profiles || max_profiles <= 0)
        return 0;

    if (swprintf_s(pattern, ARRAYSIZE(pattern), L"%s\\*.ini", dir) < 0)
        return 0;

    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 0;

    do {
        wchar_t full[MAX_PATH];
        DngProfile loaded;
        wchar_t load_err[512];

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (swprintf_s(full, ARRAYSIZE(full), L"%s\\%s", dir,
                       fd.cFileName) < 0)
            continue;
        if (dng_profile_load(full, &loaded, load_err,
                             ARRAYSIZE(load_err)) == 0) {
            profiles[count++] = loaded;
            if (count >= max_profiles)
                break;
        } else if (err && err_cch && !err[0]) {
            set_err(err, err_cch, L"%s", load_err);
        }
    } while (FindNextFileW(find, &fd));

    FindClose(find);
    return count;
}

static int wildcard_match(const wchar_t *pattern, const wchar_t *text)
{
    while (*pattern) {
        if (*pattern == L'*') {
            while (*pattern == L'*')
                pattern++;
            if (!*pattern)
                return 1;
            for (; *text; text++) {
                if (wildcard_match(pattern, text))
                    return 1;
            }
            return 0;
        }
        if (*pattern == L'?') {
            if (!*text)
                return 0;
        } else if (towlower(*pattern) != towlower(*text)) {
            return 0;
        }
        pattern++;
        if (*text)
            text++;
    }
    return *text == L'\0';
}

static int pattern_specific(const wchar_t *pattern)
{
    const wchar_t *p;

    if (!pattern || !*pattern)
        return 0;
    for (p = pattern; *p; p++) {
        if (*p != L'*' && *p != L'?' && !iswspace(*p))
            return 1;
    }
    return 0;
}

int dng_profile_matches(const DngProfile *profile, const wchar_t *make,
                        const wchar_t *model, const wchar_t *unique_model)
{
    int score;

    if (!profile || !profile->enabled)
        return -1;

    score = profile->priority * 100;
    if (score < -1000000)
        score = -1000000;
    if (score > 1000000)
        score = 1000000;

    if (profile->match_make[0] &&
        !wildcard_match(profile->match_make, make ? make : L""))
        return -1;
    if (profile->match_model[0] &&
        !wildcard_match(profile->match_model, model ? model : L""))
        return -1;
    if (profile->match_unique_model[0] &&
        !wildcard_match(profile->match_unique_model,
                        unique_model ? unique_model : L""))
        return -1;

    if (pattern_specific(profile->match_make))
        score += 1;
    if (pattern_specific(profile->match_model))
        score += 2;
    if (pattern_specific(profile->match_unique_model))
        score += 4;
    if (score < 0)
        score = 0;
    return score;
}

int dng_profile_find_best(const DngProfile *profiles, int count,
                          const wchar_t *make, const wchar_t *model,
                          const wchar_t *unique_model)
{
    int best = -1;
    int best_score = -1;
    int i;

    for (i = 0; i < count; i++) {
        int score = dng_profile_matches(&profiles[i], make, model,
                                        unique_model);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}
