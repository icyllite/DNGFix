#ifndef DNGFIX_H
#define DNGFIX_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define DNG_MAX_CCT_OVERRIDES 64

typedef struct {
    unsigned short code;
    int kelvin;
} DngCctOverride;

typedef struct {
    wchar_t path[MAX_PATH];
    wchar_t name[128];
    wchar_t description[256];

    int enabled;
    int priority;

    wchar_t match_make[64];
    wchar_t match_model[64];
    wchar_t match_unique_model[128];

    int fix_calibration;
    int fix_illuminants;
    int fix_version;
    int verify;

    double calibration_tolerance;

    int illuminants_require_pairs;
    int illuminants_skip_huesatmap;

    DngCctOverride cct_overrides[DNG_MAX_CCT_OVERRIDES];
    int cct_override_count;

    unsigned char version_min[4];
    unsigned char version_opcode[4];
    unsigned char version_forward[4];
    int version_use_opcode_list;
    int version_use_forward_matrix;

    wchar_t output_suffix[64];
    int output_overwrite;
    int keep_unmodified;
    unsigned max_file_mb;
} DngProfile;

typedef enum {
    DNG_LOG_INFO = 0,
    DNG_LOG_FIX,
    DNG_LOG_WARN,
    DNG_LOG_ERROR
} DngLogLevel;

typedef void (*DngLogFn)(void *user, DngLogLevel level, const wchar_t *message);

typedef struct {
    int repairs;
    int skipped;
    int wrote_output;
    wchar_t output_path[MAX_PATH];
    wchar_t error[512];
} DngResult;

/* fill a profile with safe generic defaults. */
void dng_profile_defaults(DngProfile *profile);

/* load one INI profile.  returns 0 on success, -1 on failure. */
int dng_profile_load(const wchar_t *path, DngProfile *profile,
                     wchar_t *err, size_t err_cch);

/* load every *.ini in a directory.  returns the number loaded (>= 0). */
int dng_profiles_load_dir(const wchar_t *dir, DngProfile *profiles,
                          int max_profiles, wchar_t *err, size_t err_cch);

int dng_profile_matches(const DngProfile *profile, const wchar_t *make,
                        const wchar_t *model, const wchar_t *unique_model);

int dng_profile_find_best(const DngProfile *profiles, int count,
                          const wchar_t *make, const wchar_t *model,
                          const wchar_t *unique_model);

/* read Make / Model / UniqueCameraModel without modifying anything. */
int dng_probe(const wchar_t *path,
              wchar_t *make, size_t make_cch,
              wchar_t *model, size_t model_cch,
              wchar_t *unique_model, size_t unique_cch,
              wchar_t *err, size_t err_cch);

/*
 * fix one file.
 */
int dng_process_file(const wchar_t *src, const wchar_t *output_dir,
                     const DngProfile *profile, int dry_run, int force,
                     DngLogFn log, void *user, DngResult *result);

#endif /* DNGFIX_H */
