#include "dngfix.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TAG_MAKE             0x010F
#define TAG_MODEL            0x0110
#define TAG_DNG_VERSION      50706
#define TAG_DNG_BACK_VERSION 50707
#define TAG_UNIQUE_MODEL     50708

#define TAG_COLOR_MATRIX_1   50721
#define TAG_COLOR_MATRIX_2   50722
#define TAG_CALIBRATION_1    50723
#define TAG_CALIBRATION_2    50724
#define TAG_REDUCTION_1      50725
#define TAG_REDUCTION_2      50726
#define TAG_FORWARD_1        50964
#define TAG_FORWARD_2        50965
#define TAG_ILLUMINANT_1     50778
#define TAG_ILLUMINANT_2     50779
#define TAG_HUE_SAT_1        50938
#define TAG_HUE_SAT_2        50939
#define TAG_OPCODE_1         51008
#define TAG_OPCODE_2         51009
#define TAG_OPCODE_3         51022

#define MAX_IFD_ENTRIES      10000
#define MAX_ASCII_VALUE      (1024 * 1024)
#define MAX_MATRIX_BYTES     (16 * 1024 * 1024)

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint64_t entry_off;
    uint64_t value_off;
    uint64_t size;
} DngEntry;

typedef struct {
    FILE *f;
    int little;
    int writable;
    int64_t size;
    DngEntry *entries;
    size_t count;
    wchar_t err[512];
} Tiff;

typedef struct {
    const DngEntry *a;
    const DngEntry *b;
    uint64_t size;
} SwapPair;

typedef struct {
    uint16_t a;
    uint16_t b;
    const wchar_t *name;
} MatrixPairDef;

static const MatrixPairDef k_matrix_pairs[] = {
    { TAG_COLOR_MATRIX_1, TAG_COLOR_MATRIX_2, L"ColorMatrix" },
    { TAG_CALIBRATION_1,  TAG_CALIBRATION_2,  L"CameraCalibration" },
    { TAG_REDUCTION_1,    TAG_REDUCTION_2,    L"ReductionMatrix" },
    { TAG_FORWARD_1,      TAG_FORWARD_2,      L"ForwardMatrix" }
};

typedef struct {
    unsigned short code;
    int kelvin;
} CctEntry;

static const CctEntry k_builtin_cct[] = {
    { 1, 5200 }, { 2, 4200 }, { 3, 2850 }, { 4, 5500 },
    { 9, 5500 }, { 10, 6500 }, { 11, 7500 }, { 12, 6430 },
    { 13, 5000 }, { 14, 4150 }, { 15, 3500 }, { 16, 3000 },
    { 17, 2856 }, { 18, 4874 }, { 19, 6774 }, { 20, 5500 },
    { 21, 6504 }, { 22, 7504 }, { 23, 5003 }, { 24, 3200 }
};

// utils

static void set_err(wchar_t *err, size_t err_cch, const wchar_t *fmt, ...)
{
    va_list ap;

    if (!err || err_cch == 0)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(err, err_cch, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static uint16_t get16(const unsigned char *p, int little)
{
    if (little)
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const unsigned char *p, int little)
{
    if (little)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put16(unsigned char *p, uint16_t v, int little)
{
    if (little) {
        p[0] = (unsigned char)(v & 0xFF);
        p[1] = (unsigned char)(v >> 8);
    } else {
        p[0] = (unsigned char)(v >> 8);
        p[1] = (unsigned char)(v & 0xFF);
    }
}

static void put32(unsigned char *p, uint32_t v, int little)
{
    if (little) {
        p[0] = (unsigned char)(v & 0xFF);
        p[1] = (unsigned char)((v >> 8) & 0xFF);
        p[2] = (unsigned char)((v >> 16) & 0xFF);
        p[3] = (unsigned char)((v >> 24) & 0xFF);
    } else {
        p[0] = (unsigned char)((v >> 24) & 0xFF);
        p[1] = (unsigned char)((v >> 16) & 0xFF);
        p[2] = (unsigned char)((v >> 8) & 0xFF);
        p[3] = (unsigned char)(v & 0xFF);
    }
}

static int type_size(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

static int version_cmp(const unsigned char a[4], const unsigned char b[4])
{
    int i;

    for (i = 0; i < 4; i++) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}

static void version_max(unsigned char dst[4], const unsigned char src[4])
{
    if (version_cmp(src, dst) > 0)
        memcpy(dst, src, 4);
}

static void version_text(const unsigned char v[4], wchar_t *out,
                         size_t out_cch)
{
    swprintf_s(out, out_cch, L"%u.%u.%u.%u",
               (unsigned)v[0], (unsigned)v[1],
               (unsigned)v[2], (unsigned)v[3]);
}

static void log_msg(DngLogFn log, void *user, int quiet, DngLogLevel level,
                    const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;

    if (!log)
        return;
    if (quiet && level != DNG_LOG_ERROR)
        return;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    log(user, level, buf);
}

static void tiff_error(Tiff *t, const wchar_t *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf_s(t->err, ARRAYSIZE(t->err), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// dng reader

static int tf_seek(Tiff *t, uint64_t off)
{
    if (off > (uint64_t)INT64_MAX) {
        tiff_error(t, L"offset is too large");
        return -1;
    }
    if (_fseeki64(t->f, (__int64)off, SEEK_SET) != 0) {
        tiff_error(t, L"seek failed (error %d)", errno);
        return -1;
    }
    return 0;
}

static int tf_read(Tiff *t, uint64_t off, void *buf, size_t len)
{
    if (off > (uint64_t)t->size || len > (uint64_t)t->size - off) {
        tiff_error(t, L"truncated file (read past end)");
        return -1;
    }
    if (tf_seek(t, off) != 0)
        return -1;
    if (len && fread(buf, 1, len, t->f) != len) {
        tiff_error(t, L"read failed (error %d)", errno);
        return -1;
    }
    return 0;
}

static int tf_write(Tiff *t, uint64_t off, const void *buf, size_t len)
{
    if (!t->writable) {
        tiff_error(t, L"internal error: write on read-only file");
        return -1;
    }
    if (off > (uint64_t)t->size || len > (uint64_t)t->size - off) {
        tiff_error(t, L"write would exceed end of file");
        return -1;
    }
    if (tf_seek(t, off) != 0)
        return -1;
    if (len && fwrite(buf, 1, len, t->f) != len) {
        tiff_error(t, L"write failed (error %d)", errno);
        return -1;
    }
    return 0;
}

static const DngEntry *tf_find(const Tiff *t, uint16_t tag)
{
    size_t i;

    for (i = 0; i < t->count; i++) {
        if (t->entries[i].tag == tag)
            return &t->entries[i];
    }
    return NULL;
}

static void tiff_close(Tiff *t)
{
    if (t->f) {
        fclose(t->f);
        t->f = NULL;
    }
    free(t->entries);
    t->entries = NULL;
    t->count = 0;
}

static int tiff_open(const wchar_t *path, int writable, Tiff *t)
{
    unsigned char head[8];
    unsigned char count_bytes[2];
    uint32_t ifd_off;
    uint32_t n;
    unsigned char *raw = NULL;
    uint64_t needed;
    size_t i;

    memset(t, 0, sizeof(*t));
    t->writable = writable;
    t->f = _wfopen(path, writable ? L"r+b" : L"rb");
    if (!t->f) {
        set_err(t->err, ARRAYSIZE(t->err),
                L"cannot open %s (error %d)", path, errno);
        return -1;
    }

    if (_fseeki64(t->f, 0, SEEK_END) != 0) {
        tiff_error(t, L"cannot determine file size");
        goto fail;
    }
    t->size = _ftelli64(t->f);
    if (t->size < 8) {
        tiff_error(t, L"file is too small to be a TIFF/DNG");
        goto fail;
    }

    if (tf_read(t, 0, head, sizeof(head)) != 0)
        goto fail;
    if (head[0] == 'I' && head[1] == 'I')
        t->little = 1;
    else if (head[0] == 'M' && head[1] == 'M')
        t->little = 0;
    else {
        tiff_error(t, L"not a TIFF/DNG file (bad byte order marker)");
        goto fail;
    }
    if (get16(head + 2, t->little) != 42) {
        tiff_error(t, L"unsupported TIFF variant (BigTIFF is not supported)");
        goto fail;
    }
    ifd_off = get32(head + 4, t->little);
    if ((uint64_t)ifd_off + 2 > (uint64_t)t->size) {
        tiff_error(t, L"IFD offset is outside the file");
        goto fail;
    }
    if (tf_read(t, ifd_off, count_bytes, 2) != 0)
        goto fail;
    n = get16(count_bytes, t->little);

    needed = (uint64_t)ifd_off + 2 + (uint64_t)n * 12;
    if (needed > (uint64_t)t->size) {
        tiff_error(t, L"IFD entry table is truncated");
        goto fail;
    }
    if (n > MAX_IFD_ENTRIES) {
        tiff_error(t, L"IFD has too many entries (%u)", (unsigned)n);
        goto fail;
    }
    if (n == 0) {
        tiff_error(t, L"IFD has no entries");
        goto fail;
    }

    t->entries = (DngEntry *)calloc(n, sizeof(DngEntry));
    raw = (unsigned char *)malloc((size_t)n * 12);
    if (!t->entries || !raw) {
        tiff_error(t, L"out of memory");
        goto fail;
    }
    if (tf_read(t, (uint64_t)ifd_off + 2, raw, (size_t)n * 12) != 0)
        goto fail;

    for (i = 0; i < n; i++) {
        const unsigned char *p = raw + i * 12;
        DngEntry *e = &t->entries[i];
        int ts;

        e->tag = get16(p, t->little);
        e->type = get16(p + 2, t->little);
        e->count = get32(p + 4, t->little);
        e->entry_off = (uint64_t)ifd_off + 2 + (uint64_t)i * 12;

        ts = type_size(e->type);
        if (e->count && ts == 0) {
            tiff_error(t, L"tag %u has unknown TIFF type %u",
                       (unsigned)e->tag, (unsigned)e->type);
            goto fail;
        }
        if (ts && e->count > UINT32_MAX / (uint32_t)ts) {
            tiff_error(t, L"tag %u value size overflows", (unsigned)e->tag);
            goto fail;
        }
        e->size = (uint64_t)ts * e->count;

        if (e->size <= 4) {
            e->value_off = e->entry_off + 8;
        } else {
            e->value_off = get32(p + 8, t->little);
            if (e->value_off == 0) {
                tiff_error(t, L"tag %u has a null value offset",
                           (unsigned)e->tag);
                goto fail;
            }
        }
        if (e->size > 0 &&
            (e->value_off > (uint64_t)t->size ||
             e->size > (uint64_t)t->size - e->value_off)) {
            tiff_error(t, L"tag %u value lies outside the file",
                       (unsigned)e->tag);
            goto fail;
        }
        t->count = i + 1;
    }

    free(raw);
    return 0;

fail:
    free(raw);
    tiff_close(t);
    return -1;
}

static int read_ascii_tag(Tiff *t, uint16_t tag, wchar_t *out, size_t out_cch)
{
    const DngEntry *e;
    char *buf;
    size_t len;
    int n;

    if (!out || out_cch == 0)
        return -1;
    out[0] = L'\0';

    e = tf_find(t, tag);
    if (!e || e->type != 2 || e->count == 0)
        return -1;
    if (e->size > MAX_ASCII_VALUE) {
        tiff_error(t, L"tag %u ASCII value is unreasonably large",
                   (unsigned)tag);
        return -1;
    }

    buf = (char *)malloc((size_t)e->size + 1);
    if (!buf) {
        tiff_error(t, L"out of memory");
        return -1;
    }
    if (tf_read(t, e->value_off, buf, (size_t)e->size) != 0) {
        free(buf);
        return -1;
    }
    buf[e->size] = '\0';
    len = 0;
    while (len < (size_t)e->size && buf[len] != '\0')
        len++;
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' ||
                       buf[len - 1] == '\r' || buf[len - 1] == '\n'))
        len--;

    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)len,
                            out, (int)out_cch - 1);
    if (n <= 0)
        n = MultiByteToWideChar(CP_ACP, 0, buf, (int)len,
                                out, (int)out_cch - 1);
    if (n < 0)
        n = 0;
    out[n] = L'\0';
    free(buf);
    return 0;
}

int dng_probe(const wchar_t *path,
              wchar_t *make, size_t make_cch,
              wchar_t *model, size_t model_cch,
              wchar_t *unique_model, size_t unique_cch,
              wchar_t *err, size_t err_cch)
{
    Tiff t;

    if (err && err_cch)
        err[0] = L'\0';
    if (make && make_cch)
        make[0] = L'\0';
    if (model && model_cch)
        model[0] = L'\0';
    if (unique_model && unique_cch)
        unique_model[0] = L'\0';

    if (!path) {
        set_err(err, err_cch, L"no input file");
        return -1;
    }
    if (tiff_open(path, 0, &t) != 0) {
        set_err(err, err_cch, L"%s", t.err);
        return -1;
    }

    read_ascii_tag(&t, TAG_MAKE, make, make_cch);
    read_ascii_tag(&t, TAG_MODEL, model, model_cch);
    read_ascii_tag(&t, TAG_UNIQUE_MODEL, unique_model, unique_cch);
    tiff_close(&t);
    return 0;
}

// calibration matrix repair thing

static int square_root_count(uint32_t count, size_t *n)
{
    size_t r;

    if (count == 0)
        return 0;
    r = (size_t)sqrt((double)count);
    while ((r + 1) * (r + 1) <= (size_t)count)
        r++;
    while (r > 0 && r * r > (size_t)count)
        r--;
    if (r * r != (size_t)count)
        return 0;
    *n = r;
    return 1;
}

static int matrix_from_entry(Tiff *t, const DngEntry *e, double **values,
                             size_t *n, int *zero_denominator)
{
    unsigned char *raw;
    double *m;
    size_t count;
    size_t i;
    int is_signed;

    *values = NULL;
    *n = 0;
    *zero_denominator = 0;

    if (!e || (e->type != 5 && e->type != 10))
        return 0;
    if (!square_root_count(e->count, n))
        return 0;
    if (!*n || e->size > MAX_MATRIX_BYTES)
        return 0;

    count = (size_t)e->count;
    raw = (unsigned char *)malloc((size_t)e->size);
    m = (double *)malloc(count * sizeof(double));
    if (!raw || !m) {
        free(raw);
        free(m);
        tiff_error(t, L"out of memory");
        return -1;
    }
    if (tf_read(t, e->value_off, raw, (size_t)e->size) != 0) {
        free(raw);
        free(m);
        return -1;
    }

    is_signed = (e->type == 10);
    for (i = 0; i < count; i++) {
        const unsigned char *p = raw + i * 8;
        double numerator;
        double denominator;

        if (is_signed) {
            numerator = (double)(int32_t)get32(p, t->little);
            denominator = (double)(int32_t)get32(p + 4, t->little);
        } else {
            numerator = (double)get32(p, t->little);
            denominator = (double)get32(p + 4, t->little);
        }
        if (denominator == 0.0) {
            *zero_denominator = 1;
            m[i] = 0.0;
        } else {
            m[i] = numerator / denominator;
        }
    }

    free(raw);
    *values = m;
    return 1;
}

static int matrix_is_singular(const double *a, size_t n, double tolerance)
{
    double *m;
    double det = 1.0;
    double pivot_tol = tolerance * 1e-3;
    size_t r, c, k;

    m = (double *)malloc(n * n * sizeof(double));
    if (!m)
        return 1;
    memcpy(m, a, n * n * sizeof(double));

    if (pivot_tol <= 0.0)
        pivot_tol = 1e-12;

    for (c = 0; c < n; c++) {
        size_t p = c;
        double best = fabs(m[c * n + c]);

        for (r = c + 1; r < n; r++) {
            double v = fabs(m[r * n + c]);
            if (v > best) {
                best = v;
                p = r;
            }
        }
        if (best < pivot_tol) {
            free(m);
            return 1;
        }
        if (p != c) {
            for (k = 0; k < n; k++) {
                double tmp = m[c * n + k];
                m[c * n + k] = m[p * n + k];
                m[p * n + k] = tmp;
            }
            det = -det;
        }
        det *= m[c * n + c];
        for (r = c + 1; r < n; r++) {
            double f = m[r * n + c] / m[c * n + c];
            for (k = c; k < n; k++)
                m[r * n + k] -= f * m[c * n + k];
        }
    }
    free(m);
    return fabs(det) < tolerance;
}

static int write_identity_matrix(Tiff *t, const DngEntry *e, size_t n)
{
    unsigned char *buf;
    size_t k;
    int is_signed = (e->type == 10);

    if (n * n != (size_t)e->count || n > 1024)
        return -1;
    buf = (unsigned char *)calloc(1, (size_t)e->size);
    if (!buf) {
        tiff_error(t, L"out of memory");
        return -1;
    }
    for (k = 0; k < n * n; k++) {
        int diagonal = ((k / n) == (k % n));
        if (is_signed) {
            put32(buf + k * 8, diagonal ? 1u : 0u, t->little);
            put32(buf + k * 8 + 4, 1u, t->little);
        } else {
            put32(buf + k * 8, diagonal ? 1u : 0u, t->little);
            put32(buf + k * 8 + 4, 1u, t->little);
        }
    }
    if (tf_write(t, e->value_off, buf, (size_t)e->size) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

static int fix_calibration(Tiff *t, const DngProfile *p, int apply, int quiet,
                           DngLogFn log, void *user,
                           int *repairs, int *skipped)
{
    uint16_t tags[2] = { TAG_CALIBRATION_1, TAG_CALIBRATION_2 };
    int i;

    (void)skipped;
    for (i = 0; i < 2; i++) {
        const DngEntry *e = tf_find(t, tags[i]);
        double *values = NULL;
        size_t n = 0;
        int zero_den = 0;
        int rc;

        if (!e)
            continue;
        rc = matrix_from_entry(t, e, &values, &n, &zero_den);
        if (rc < 0)
            return -1;
        if (rc == 0)
            continue;

        if (zero_den || matrix_is_singular(values, n,
                                           p->calibration_tolerance)) {
            if (apply && write_identity_matrix(t, e, n) != 0) {
                free(values);
                return -1;
            }
            log_msg(log, user, quiet, DNG_LOG_FIX,
                    L"CameraCalibration%d was singular -> identity",
                    i + 1);
            (*repairs)++;
        }
        free(values);
    }
    return 0;
}

// illuminant repair thing

static int read_short1(Tiff *t, uint16_t tag, uint16_t *value)
{
    const DngEntry *e = tf_find(t, tag);
    unsigned char b[2];

    if (!e || e->type != 3 || e->count != 1 || e->size != 2)
        return 0;
    if (tf_read(t, e->value_off, b, sizeof(b)) != 0)
        return -1;
    *value = get16(b, t->little);
    return 1;
}

static int cct_for(const DngProfile *p, uint16_t code, int *kelvin)
{
    int i;

    for (i = 0; i < p->cct_override_count; i++) {
        if (p->cct_overrides[i].code == code) {
            *kelvin = p->cct_overrides[i].kelvin;
            return 1;
        }
    }
    for (i = 0; i < (int)ARRAYSIZE(k_builtin_cct); i++) {
        if (k_builtin_cct[i].code == code) {
            *kelvin = k_builtin_cct[i].kelvin;
            return 1;
        }
    }
    return 0;
}

static int fix_illuminants(Tiff *t, const DngProfile *p, int apply, int quiet,
                           DngLogFn log, void *user,
                           int *repairs, int *skipped)
{
    const DngEntry *e1 = tf_find(t, TAG_ILLUMINANT_1);
    const DngEntry *e2 = tf_find(t, TAG_ILLUMINANT_2);
    uint16_t c1, c2;
    int k1, k2;
    SwapPair todo[ARRAYSIZE(k_matrix_pairs)];
    int todo_count = 0;
    size_t i;
    unsigned char b[2];

    if (!e1 || !e2)
        return 0;
    {
        int r1 = read_short1(t, TAG_ILLUMINANT_1, &c1);
        int r2 = read_short1(t, TAG_ILLUMINANT_2, &c2);
        if (r1 < 0 || r2 < 0)
            return -1;
        if (r1 != 1 || r2 != 1)
            return 0;
    }
    if (!cct_for(p, c1, &k1) || !cct_for(p, c2, &k2))
        return 0;
    if (k1 <= k2)
        return 0;

    if (p->illuminants_skip_huesatmap &&
        (tf_find(t, TAG_HUE_SAT_1) || tf_find(t, TAG_HUE_SAT_2))) {
        log_msg(log, user, quiet, DNG_LOG_WARN,
                L"SKIPPED illuminant swap: file has ProfileHueSatMap");
        (*skipped)++;
        return 0;
    }

    for (i = 0; i < ARRAYSIZE(k_matrix_pairs); i++) {
        const DngEntry *a = tf_find(t, k_matrix_pairs[i].a);
        const DngEntry *bent = tf_find(t, k_matrix_pairs[i].b);

        if (!a && !bent)
            continue;
        if (!a || !bent) {
            if (p->illuminants_require_pairs) {
                log_msg(log, user, quiet, DNG_LOG_WARN,
                        L"SKIPPED illuminant swap: %s tags %u/%u are only "
                        L"half present",
                        k_matrix_pairs[i].name,
                        (unsigned)k_matrix_pairs[i].a,
                        (unsigned)k_matrix_pairs[i].b);
                (*skipped)++;
                return 0;
            }
            log_msg(log, user, quiet, DNG_LOG_WARN,
                    L"ignoring incomplete %s pair", k_matrix_pairs[i].name);
            (*skipped)++;
            continue;
        }
        if (a->type != bent->type || a->count != bent->count ||
            a->size != bent->size || a->size <= 4) {
            if (p->illuminants_require_pairs) {
                log_msg(log, user, quiet, DNG_LOG_WARN,
                        L"SKIPPED illuminant swap: %s tags %u/%u differ in "
                        L"shape",
                        k_matrix_pairs[i].name,
                        (unsigned)k_matrix_pairs[i].a,
                        (unsigned)k_matrix_pairs[i].b);
                (*skipped)++;
                return 0;
            }
            log_msg(log, user, quiet, DNG_LOG_WARN,
                    L"ignoring %s pair with mismatched shape",
                    k_matrix_pairs[i].name);
            (*skipped)++;
            continue;
        }
        todo[todo_count].a = a;
        todo[todo_count].b = bent;
        todo[todo_count].size = a->size;
        todo_count++;
    }

    if (apply) {
        int j;

        for (j = 0; j < todo_count; j++) {
            size_t size = (size_t)todo[j].size;
            unsigned char *buf;

            if (todo[j].a->value_off == todo[j].b->value_off)
                continue;
            if (size > MAX_MATRIX_BYTES) {
                tiff_error(t, L"matrix value is unreasonably large");
                return -1;
            }
            buf = (unsigned char *)malloc(size * 2);
            if (!buf) {
                tiff_error(t, L"out of memory");
                return -1;
            }
            if (tf_read(t, todo[j].a->value_off, buf, size) != 0 ||
                tf_read(t, todo[j].b->value_off, buf + size, size) != 0) {
                free(buf);
                return -1;
            }
            if (tf_write(t, todo[j].a->value_off, buf + size, size) != 0 ||
                tf_write(t, todo[j].b->value_off, buf, size) != 0) {
                free(buf);
                return -1;
            }
            free(buf);
        }

        put16(b, c2, t->little);
        if (tf_write(t, e1->value_off, b, sizeof(b)) != 0)
            return -1;
        put16(b, c1, t->little);
        if (tf_write(t, e2->value_off, b, sizeof(b)) != 0)
            return -1;
    }

    log_msg(log, user, quiet, DNG_LOG_FIX,
            L"illuminants were %u (%dK) then %u (%dK) -> swapped to %u then "
            L"%u (%d matrix pair(s) swapped to match)",
            (unsigned)c1, k1, (unsigned)c2, k2,
            (unsigned)c2, (unsigned)c1, todo_count);
    (*repairs)++;
    return 0;
}

// version repairer

static int fix_version(Tiff *t, const DngProfile *p, int apply, int quiet,
                       DngLogFn log, void *user,
                       int *repairs, int *skipped)
{
    const DngEntry *back = tf_find(t, TAG_DNG_BACK_VERSION);
    const DngEntry *dng = tf_find(t, TAG_DNG_VERSION);
    unsigned char current[4];
    unsigned char required[4];
    unsigned char file_version[4];
    wchar_t cur_text[32];
    wchar_t need_text[32];

    if (!back || !dng)
        return 0;
    if (back->type != 1 || back->count != 4 || back->size != 4 ||
        dng->type != 1 || dng->count != 4 || dng->size != 4)
        return 0;

    if (tf_read(t, back->value_off, current, 4) != 0)
        return -1;
    if (tf_read(t, dng->value_off, file_version, 4) != 0)
        return -1;

    memcpy(required, p->version_min, 4);
    if (p->version_use_opcode_list &&
        (tf_find(t, TAG_OPCODE_1) || tf_find(t, TAG_OPCODE_2) ||
         tf_find(t, TAG_OPCODE_3)))
        version_max(required, p->version_opcode);
    if (p->version_use_forward_matrix &&
        (tf_find(t, TAG_FORWARD_1) || tf_find(t, TAG_FORWARD_2)))
        version_max(required, p->version_forward);

    if (version_cmp(current, required) >= 0)
        return 0;

    version_text(current, cur_text, ARRAYSIZE(cur_text));
    version_text(required, need_text, ARRAYSIZE(need_text));

    if (version_cmp(required, file_version) > 0) {
        log_msg(log, user, quiet, DNG_LOG_WARN,
                L"SKIPPED version bump: file uses features newer than its "
                L"DNGVersion (needs %s, declares %u.%u.%u.%u)",
                need_text, (unsigned)file_version[0], (unsigned)file_version[1],
                (unsigned)file_version[2], (unsigned)file_version[3]);
        (*skipped)++;
        return 0;
    }

    if (apply && tf_write(t, back->value_off, required, 4) != 0)
        return -1;
    log_msg(log, user, quiet, DNG_LOG_FIX,
            L"DNGBackwardVersion %s -> %s", cur_text, need_text);
    (*repairs)++;
    return 0;
}

// driver fix

static int run_fixes(Tiff *t, const DngProfile *p, int apply, int quiet,
                     DngLogFn log, void *user,
                     int *repairs, int *skipped)
{
    *repairs = 0;
    *skipped = 0;

    if (p->fix_calibration &&
        fix_calibration(t, p, apply, quiet, log, user,
                        repairs, skipped) != 0)
        return -1;
    if (p->fix_illuminants &&
        fix_illuminants(t, p, apply, quiet, log, user,
                        repairs, skipped) != 0)
        return -1;
    if (p->fix_version &&
        fix_version(t, p, apply, quiet, log, user,
                    repairs, skipped) != 0)
        return -1;
    return 0;
}

// file output utils

static int file_size_bytes(const wchar_t *path, uint64_t *size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
        *size = 0;
        return -1;
    }
    *size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return 0;
}

static int path_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int ensure_directory(const wchar_t *dir)
{
    wchar_t tmp[MAX_PATH];
    size_t len;
    size_t i;
    size_t start = 0;

    if (!dir || !*dir)
        return 1;
    wcsncpy_s(tmp, ARRAYSIZE(tmp), dir, _TRUNCATE);
    len = wcslen(tmp);
    while (len > 3 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/'))
        tmp[--len] = L'\0';

    if (len > 2 && tmp[0] == L'\\' && tmp[1] == L'\\') {
        int separators = 0;
        for (i = 2; i < len; i++) {
            if (tmp[i] == L'\\' || tmp[i] == L'/') {
                if (++separators == 2) {
                    start = i + 1;
                    break;
                }
            }
        }
    }

    for (i = start; i < len; i++) {
        if ((tmp[i] == L'\\' || tmp[i] == L'/') && i > 1 &&
            tmp[i - 1] != L':') {
            wchar_t c = tmp[i];
            tmp[i] = L'\0';
            if (!CreateDirectoryW(tmp, NULL)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    tmp[i] = c;
                    return 0;
                }
            }
            tmp[i] = c;
        }
    }
    if (!CreateDirectoryW(tmp, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
            return 0;
    }
    return 1;
}

static int same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t fa[MAX_PATH];
    wchar_t fb[MAX_PATH];
    DWORD na, nb;

    na = GetFullPathNameW(a, ARRAYSIZE(fa), fa, NULL);
    nb = GetFullPathNameW(b, ARRAYSIZE(fb), fb, NULL);
    if (na == 0 || nb == 0 || na >= ARRAYSIZE(fa) || nb >= ARRAYSIZE(fb))
        return 0;
    return _wcsicmp(fa, fb) == 0;
}

static int make_output_path(const wchar_t *src, const wchar_t *output_dir,
                            const wchar_t *suffix, wchar_t *out,
                            size_t out_cch)
{
    wchar_t dir[MAX_PATH];
    wchar_t name[MAX_PATH];
    wchar_t stem[MAX_PATH];
    wchar_t ext[64];
    const wchar_t *base;
    const wchar_t *slash;
    const wchar_t *dot;
    size_t stem_len;
    size_t dir_len;

    dir[0] = L'\0';
    if (output_dir && *output_dir) {
        wcsncpy_s(dir, ARRAYSIZE(dir), output_dir, _TRUNCATE);
    } else {
        slash = wcsrchr(src, L'\\');
        if (!slash)
            slash = wcsrchr(src, L'/');
        if (slash) {
            size_t len = (size_t)(slash - src);
            if (len >= ARRAYSIZE(dir))
                return -1;
            memcpy(dir, src, len * sizeof(wchar_t));
            dir[len] = L'\0';
        }
    }

    slash = wcsrchr(src, L'\\');
    if (!slash)
        slash = wcsrchr(src, L'/');
    base = slash ? slash + 1 : src;
    wcsncpy_s(name, ARRAYSIZE(name), base, _TRUNCATE);

    dot = wcsrchr(name, L'.');
    if (dot && dot != name) {
        stem_len = (size_t)(dot - name);
        wcsncpy_s(ext, ARRAYSIZE(ext), dot, _TRUNCATE);
    } else {
        stem_len = wcslen(name);
        wcscpy_s(ext, ARRAYSIZE(ext), L".dng");
    }
    if (stem_len >= ARRAYSIZE(stem))
        return -1;
    memcpy(stem, name, stem_len * sizeof(wchar_t));
    stem[stem_len] = L'\0';

    dir_len = wcslen(dir);
    while (dir_len > 0 && (dir[dir_len - 1] == L'\\' ||
                           dir[dir_len - 1] == L'/'))
        dir[--dir_len] = L'\0';

    if (dir_len > 0) {
        if (swprintf_s(out, out_cch, L"%s\\%s%s%s", dir, stem,
                       suffix ? suffix : L"", ext) < 0)
            return -1;
    } else {
        if (swprintf_s(out, out_cch, L"%s%s%s", stem,
                       suffix ? suffix : L"", ext) < 0)
            return -1;
    }
    return 0;
}

static void delete_file_quiet(const wchar_t *path)
{
    if (!path || !*path)
        return;
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path);
}

static void fail_process(DngResult *result, DngLogFn log, void *user,
                         const wchar_t *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf_s(result->error, ARRAYSIZE(result->error), _TRUNCATE,
                  fmt, ap);
    va_end(ap);
    if (log)
        log(user, DNG_LOG_ERROR, result->error);
}

// pub process entrypoint

int dng_process_file(const wchar_t *src, const wchar_t *output_dir,
                     const DngProfile *profile, int dry_run, int force,
                     DngLogFn log, void *user, DngResult *result)
{
    DngProfile fallback;
    const DngProfile *p = profile;
    Tiff t;
    uint64_t size = 0;
    int repairs = 0;
    int skipped = 0;
    wchar_t dst[MAX_PATH];
    int dst_created = 0;
    int overwrite;

    if (!result)
        return -1;
    memset(result, 0, sizeof(*result));

    if (!p) {
        dng_profile_defaults(&fallback);
        p = &fallback;
    }
    if (!src || !*src) {
        fail_process(result, log, user, L"no input file");
        return -1;
    }

    if (file_size_bytes(src, &size) != 0) {
        fail_process(result, log, user,
                     L"cannot read input file: %s", src);
        return -1;
    }
    if (p->max_file_mb &&
        size > (uint64_t)p->max_file_mb * 1024ull * 1024ull) {
        fail_process(result, log, user,
                     L"file is larger than the profile limit of %u MB: %s",
                     p->max_file_mb, src);
        return -1;
    }

    if (dry_run) {
        if (tiff_open(src, 0, &t) != 0) {
            fail_process(result, log, user, L"%s", t.err);
            return -1;
        }
        if (run_fixes(&t, p, 0, 0, log, user, &repairs, &skipped) != 0) {
            wchar_t msg[512];
            wcsncpy_s(msg, ARRAYSIZE(msg), t.err, _TRUNCATE);
            tiff_close(&t);
            fail_process(result, log, user, L"%s", msg);
            return -1;
        }
        tiff_close(&t);
        result->repairs = repairs;
        result->skipped = skipped;
        return 0;
    }

    if (make_output_path(src, output_dir, p->output_suffix,
                         dst, ARRAYSIZE(dst)) != 0) {
        fail_process(result, log, user,
                     L"output path is too long for: %s", src);
        return -1;
    }
    if (same_path(src, dst)) {
        fail_process(result, log, user,
                     L"output would overwrite the input; choose another "
                     L"output folder or suffix");
        return -1;
    }

    overwrite = force || p->output_overwrite;
    if (path_exists(dst) && !overwrite) {
        fail_process(result, log, user,
                     L"output already exists (enable overwrite or choose "
                     L"another folder): %s", dst);
        return -1;
    }
    if (output_dir && *output_dir && !ensure_directory(output_dir)) {
        fail_process(result, log, user,
                     L"cannot create output folder: %s", output_dir);
        return -1;
    }

    if (path_exists(dst))
        SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src, dst, overwrite ? FALSE : TRUE)) {
        fail_process(result, log, user,
                     L"cannot create output copy (error %lu): %s",
                     (unsigned long)GetLastError(), dst);
        return -1;
    }
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    dst_created = 1;

    if (tiff_open(dst, 1, &t) != 0) {
        wchar_t msg[512];
        wcsncpy_s(msg, ARRAYSIZE(msg), t.err, _TRUNCATE);
        delete_file_quiet(dst);
        fail_process(result, log, user, L"%s", msg);
        return -1;
    }

    if (run_fixes(&t, p, 1, 0, log, user, &repairs, &skipped) != 0) {
        wchar_t msg[512];
        wcsncpy_s(msg, ARRAYSIZE(msg), t.err, _TRUNCATE);
        tiff_close(&t);
        delete_file_quiet(dst);
        fail_process(result, log, user, L"%s", msg);
        return -1;
    }
    if (fflush(t.f) != 0) {
        tiff_close(&t);
        delete_file_quiet(dst);
        fail_process(result, log, user,
                     L"could not flush repaired file (error %d)", errno);
        return -1;
    }
    tiff_close(&t);

    result->repairs = repairs;
    result->skipped = skipped;

    if (repairs == 0) {
        if (!p->keep_unmodified && dst_created)
            delete_file_quiet(dst);
        if (p->keep_unmodified && dst_created) {
            result->wrote_output = 1;
            wcsncpy_s(result->output_path, ARRAYSIZE(result->output_path),
                      dst, _TRUNCATE);
        }
        return 0;
    }

    if (p->verify) {
        int leftover = 0;
        int verify_skipped = 0;

        if (tiff_open(dst, 0, &t) != 0) {
            wchar_t msg[512];
            wcsncpy_s(msg, ARRAYSIZE(msg), t.err, _TRUNCATE);
            delete_file_quiet(dst);
            fail_process(result, log, user, L"%s", msg);
            return -1;
        }
        if (run_fixes(&t, p, 0, 1, log, user, &leftover,
                      &verify_skipped) != 0) {
            wchar_t msg[512];
            wcsncpy_s(msg, ARRAYSIZE(msg), t.err, _TRUNCATE);
            tiff_close(&t);
            delete_file_quiet(dst);
            fail_process(result, log, user, L"%s", msg);
            return -1;
        }
        tiff_close(&t);
        if (leftover > 0) {
            delete_file_quiet(dst);
            fail_process(result, log, user,
                         L"verification failed: %d issue(s) still remain",
                         leftover);
            return -1;
        }
    }

    result->wrote_output = 1;
    wcsncpy_s(result->output_path, ARRAYSIZE(result->output_path),
              dst, _TRUNCATE);
    return 0;
}
