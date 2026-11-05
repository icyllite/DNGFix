#include "dngfix.h"

#include <libraw/libraw_const.h>
#include <libraw/libraw_types.h>

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

extern libraw_data_t *libraw_init(unsigned int flags);
extern int libraw_open_wfile(libraw_data_t *raw, const wchar_t *path);
extern int libraw_unpack(libraw_data_t *raw);
extern void libraw_close(libraw_data_t *raw);
extern const char *libraw_strerror(int errorcode);
extern int libraw_get_raw_width(libraw_data_t *raw);
extern int libraw_get_raw_height(libraw_data_t *raw);
extern libraw_iparams_t *libraw_get_iparams(libraw_data_t *raw);

static void set_err(wchar_t *err, size_t err_cch, const wchar_t *fmt, ...)
{
    va_list ap;

    if (!err || err_cch == 0)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(err, err_cch, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void ascii_to_wide(const char *src, wchar_t *dst, size_t dst_cch)
{
    int n;

    if (!dst || dst_cch == 0)
        return;
    dst[0] = L'\0';
    if (!src || !*src)
        return;

    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1,
                            dst, (int)dst_cch);
    if (n <= 0)
        MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)dst_cch);
}

int dng_libraw_verify(const wchar_t *path,
                      wchar_t *info, size_t info_cch,
                      wchar_t *err, size_t err_cch)
{
    libraw_data_t *raw;
    libraw_iparams_t *id;
    wchar_t why[256];
    wchar_t make[64];
    wchar_t model[64];
    int width;
    int height;
    int rc;

    if (info && info_cch)
        info[0] = L'\0';
    if (err && err_cch)
        err[0] = L'\0';

    if (!path || !*path) {
        set_err(err, err_cch, L"no output file to verify");
        return -1;
    }

    raw = libraw_init(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK);
    if (!raw) {
        set_err(err, err_cch, L"LibRaw could not create its context");
        return -1;
    }

    rc = libraw_open_wfile(raw, path);
    if (rc != LIBRAW_SUCCESS) {
        ascii_to_wide(libraw_strerror(rc), why, ARRAYSIZE(why));
        set_err(err, err_cch, L"LibRaw could not open the repaired DNG: %s",
                why);
        libraw_close(raw);
        return -1;
    }

    id = libraw_get_iparams(raw);
    if (!id || id->raw_count == 0) {
        set_err(err, err_cch,
                L"LibRaw found no raw image in the repaired DNG");
        libraw_close(raw);
        return -1;
    }

    width = libraw_get_raw_width(raw);
    height = libraw_get_raw_height(raw);
    if (width <= 0 || height <= 0) {
        set_err(err, err_cch,
                L"LibRaw reported invalid raw dimensions (%dx%d)",
                width, height);
        libraw_close(raw);
        return -1;
    }

    rc = libraw_unpack(raw);
    if (rc != LIBRAW_SUCCESS) {
        ascii_to_wide(libraw_strerror(rc), why, ARRAYSIZE(why));
        set_err(err, err_cch, L"LibRaw could not unpack the raw data: %s",
                why);
        libraw_close(raw);
        return -1;
    }
    if (!raw->rawdata.raw_alloc) {
        set_err(err, err_cch,
                L"LibRaw unpacked the repaired DNG but produced no raw data");
        libraw_close(raw);
        return -1;
    }

    ascii_to_wide(id->make, make, ARRAYSIZE(make));
    ascii_to_wide(id->model, model, ARRAYSIZE(model));
    if (info && info_cch) {
        if (make[0] && model[0]) {
            if (swprintf_s(info, info_cch, L"%s %s, %dx%d raw pixels",
                           make, model, width, height) < 0)
                info[0] = L'\0';
        } else if (make[0] || model[0]) {
            if (swprintf_s(info, info_cch, L"%s, %dx%d raw pixels",
                           make[0] ? make : model, width, height) < 0)
                info[0] = L'\0';
        } else {
            if (swprintf_s(info, info_cch, L"%dx%d raw pixels",
                           width, height) < 0)
                info[0] = L'\0';
        }
    }

    libraw_close(raw);
    return 0;
}
