#include "homehub_platform.h"
#include "homehub_seed.h"
#include "homehub_reminder_install.h"
#include "tkl_memory.h"
#include <stdio.h>

#if HOMEHUB_EMBED_TEST_VOICES
typedef enum { SEED_FAILED, SEED_WRITTEN, SEED_EXISTS } SeedResult;

static bool checked_exists(const char *path, BOOL_T *exists)
{
    /* Exact T5 TF backend: tkl_fs_is_exist -> bk_vfs_stat ->
     * fatfs_adapter._bk_fatfs_stat -> f_stat. FRESULT is returned unchanged,
     * not converted to POSIX errno. In the pinned platform ff.h, 4 is
     * FR_NO_FILE and 5 is FR_NO_PATH. Neither disk errors nor -1 mean absent. */
    enum { T5_FATFS_NO_FILE = 4, T5_FATFS_NO_PATH = 5 };
    *exists = FALSE;
    int result = tkl_fs_is_exist(path, exists);
    if (result == OPRT_OK && *exists) return true;
    if (result == T5_FATFS_NO_FILE || result == T5_FATFS_NO_PATH) {
        *exists = FALSE;
        return true;
    }
    return false;
}

static SeedResult seed_one(const HH_SeedAsset *asset)
{
    char path[HH_PATH_MAX + 1], temporary[HH_PATH_MAX + 16];
    TUYA_FILE file = NULL;
    BOOL_T exists = FALSE;
    SeedResult result = SEED_FAILED;

    if (!hh_wav_validate(asset->data, asset->bytes, NULL)) {
        PR_ERR("Built-in test voice is invalid: %s", asset->name);
        return SEED_FAILED;
    }
    snprintf(path, sizeof(path), HH_ROOT "/voices/%s", asset->name);
    snprintf(temporary, sizeof(temporary), "%s.seed.tmp", path);

    tal_mutex_lock(hh_fs_mutex);
    if (!checked_exists(path, &exists)) {
        PR_ERR("Test voice existence check failed: %s", asset->name);
        goto done;
    }
    if (exists) {
        result = SEED_EXISTS;
        goto done;
    }

    /* This temporary suffix is reserved for this first-boot seeder. Never use
     * hh_file_write_atomic here: its replacement semantics remove a target. */
    file = tkl_fopen(temporary, "wb");
    if (!file) goto done;
    if (tkl_fwrite((void *)asset->data, asset->bytes, file) != (int)asset->bytes ||
        tkl_fsync(tkl_fileno(file)) != OPRT_OK) goto done;
    int close_result = tkl_fclose(file);
    file = NULL;
    if (close_result != 0) goto done;

    /* Check again before rename. The FS mutex is held across the entire
     * operation, and this runs before all other voice writers at startup. */
    exists = FALSE;
    if (!checked_exists(path, &exists)) goto done;
    if (exists) {
        result = SEED_EXISTS;
        goto done;
    }
    if (tkl_fs_rename(temporary, path) != 0) goto done;
    result = SEED_WRITTEN;

done:
    if (file) tkl_fclose(file);
    if (result != SEED_WRITTEN) tkl_fs_remove(temporary);
    tal_mutex_unlock(hh_fs_mutex);
    return result;
}

bool hh_seed_voices(void)
{
    unsigned written = 0, skipped = 0, failed = 0;
    if (!hh_storage_ready) {
        PR_ERR("TF not mounted; built-in test voices were not written");
        return false;
    }
    bool reminder_ok = hh_reminder_install();
    bool departure_ok = hh_departure_install();
    bool arrival_ok = hh_arrival_install();
    for (size_t i = 0; i < hh_seed_asset_count; ++i) {
        SeedResult result = seed_one(&hh_seed_assets[i]);
        if (result == SEED_WRITTEN) ++written;
        else if (result == SEED_EXISTS) ++skipped;
        else { ++failed; PR_ERR("Built-in test voice write failed: %s", hh_seed_assets[i].name); }
    }
    PR_NOTICE("Mandarin test voices: %u written, %u kept existing, %u failed", written, skipped, failed);
    return reminder_ok && departure_ok && arrival_ok && failed == 0;
}
#else
bool hh_seed_voices(void)
{
    static const char *names[]={"arrive.wav","leave.wav","take_key.wav"};
    unsigned valid=0,failed=0;
    if(!hh_storage_ready){
        PR_ERR("TF not mounted; test voices were not checked");
        return false;
    }
    bool reminder_ok=hh_reminder_install();
    bool departure_ok=hh_departure_install();
    bool arrival_ok=hh_arrival_install();
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++){
        char path[HH_PATH_MAX+1];uint8_t *data=NULL;uint32_t bytes=0;
        snprintf(path,sizeof(path),HH_ROOT "/voices/%s",names[i]);
        bool ok=hh_file_read(path,&data,&bytes,HH_WAV_MAX)&&hh_wav_validate(data,bytes,NULL);
        if(data)tkl_system_psram_free(data);
        if(ok)++valid;else ++failed;
    }
    PR_NOTICE("TF test voices: %u valid, %u failed",valid,failed);
    return reminder_ok&&departure_ok&&arrival_ok&&failed==0;
}
#endif
