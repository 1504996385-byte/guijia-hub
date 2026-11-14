#include "homehub_reminder_install.h"
#include "homehub_platform.h"
#include "homehub_reminder_asset.h"
#include "homehub_departure_asset.h"
#include "homehub_arrival_asset.h"
#include "tkl_memory.h"
#include <stdio.h>
#include <string.h>

typedef enum { PATH_UNKNOWN, PATH_ABSENT, PATH_PRESENT } PathState;

static PathState path_state(const char *path)
{
    BOOL_T exists = 0;
    tal_mutex_lock(hh_fs_mutex);
    int rc = tkl_fs_is_exist(path, &exists);
    tal_mutex_unlock(hh_fs_mutex);
    if (rc == OPRT_OK && exists) return PATH_PRESENT;
    /* Pinned T5 FatFs returns FR_NO_FILE / FR_NO_PATH unchanged. An I/O
     * failure, or success without a positive existence result, is not absence. */
    if ((rc == 4 || rc == 5) && !exists) return PATH_ABSENT;
    return PATH_UNKNOWN;
}

static uint32_t crc32(const uint8_t *data, uint32_t bytes)
{
    uint32_t crc = UINT32_MAX;
    while (bytes--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool read_matches(const char *path, const uint8_t *expected, uint32_t bytes)
{
    uint8_t *data = NULL;
    uint32_t size = 0;
    bool ok = hh_file_read(path, &data, &size, HH_WAV_MAX) &&
              hh_wav_validate(data, size, NULL) && size == bytes && !memcmp(data, expected, bytes);
    if (data) tkl_system_psram_free(data);
    return ok;
}

static bool install_asset(const char *name,const char *label,const uint8_t *asset,
                          uint32_t asset_size,uint32_t checksum,unsigned version)
{
    uint8_t *old = NULL;
    uint32_t old_size = 0;
    bool ok = false, saved_backup = false, current = false;
    const char *stage = "mount";
    char path[HH_PATH_MAX + 1],backup[HH_PATH_MAX + 1];
    (void)label;(void)version; /* Diagnostic macros can be disabled in host builds. */
    int path_length=snprintf(path,sizeof(path),HH_ROOT "/voices/%s.wav",name);
    if(path_length<0||path_length>=(int)sizeof(path))goto done;
    if (!hh_storage_ready) goto done;
    stage = "asset";
    if (!hh_wav_validate(asset, asset_size, NULL) ||
        hh_checksum(asset, asset_size) != checksum) goto done;

    stage = "primary-stat";
    PathState original = path_state(path);
    if (original == PATH_UNKNOWN) goto done;
    if (original == PATH_PRESENT) {
        stage = "primary-read";
        if (!hh_file_read(path, &old, &old_size, HH_WAV_MAX) ||
            !hh_wav_validate(old, old_size, NULL)) goto done;
        if (old_size == asset_size && !memcmp(old, asset, old_size)) {
            ok = current = true;
            goto done;
        }

        int length = snprintf(backup, sizeof(backup), HH_ROOT "/voices/%s.%08x.%u.bak.wav",
                              name, (unsigned)crc32(old, old_size), (unsigned)old_size);
        if (length < 0 || length >= (int)sizeof(backup)) goto done;
        stage = "backup-stat";
        PathState prior_backup = path_state(backup);
        if (prior_backup == PATH_UNKNOWN) goto done;
        stage = "backup-write";
        /* A CRC/length collision or unreadable existing backup is preserved.
         * Reuse requires a full valid WAV and exact byte-for-byte identity. */
        if (prior_backup == PATH_ABSENT && !hh_file_write_atomic(backup, old, old_size)) goto done;
        stage = "backup-verify";
        if (!read_matches(backup, old, old_size)) goto done;
        saved_backup = true;
    }

    /* Startup runs before all voice consumers/writers. Recheck nonetheless:
     * an unknown or changed target must never be treated as our old version. */
    stage = "primary-recheck";
    if (path_state(path) != original ||
        (original == PATH_PRESENT && !read_matches(path, old, old_size))) goto done;
    stage = "primary-write";
    /* The FAT replacement helper can remove the old target before a failed
     * rename. Its verified versioned backup remains; the next boot retries
     * installing this embedded asset even when the primary is now missing. */
    if (!hh_file_write_atomic(path, asset, asset_size)) goto done;
    stage = "primary-verify";
    if (!read_matches(path, asset, asset_size)) goto done;
    ok = true;
done:
    if (old) tkl_system_psram_free(old);
    if (ok) {
        PR_NOTICE("HH %s asset version=%u current=%u updated=%u backup=%u bytes=%u",
                  label, version, current ? 1u : 0u, current ? 0u : 1u,
                  saved_backup ? 1u : 0u, (unsigned)asset_size);
    } else {
        PR_ERR("HH %s asset version=%u installed=0 stage=%s", label, version, stage);
    }
    return ok;
}

bool hh_reminder_install(void)
{
    return install_asset("take_key","reminder",hh_reminder_wav,hh_reminder_wav_size,
                         hh_reminder_wav_checksum,HH_REMINDER_ASSET_VERSION);
}
bool hh_departure_install(void)
{
    return install_asset("leave","departure",hh_departure_wav,hh_departure_wav_size,
                         hh_departure_wav_checksum,HH_DEPARTURE_ASSET_VERSION);
}
bool hh_arrival_install(void)
{
    return install_asset("arrive","arrival",hh_arrival_wav,hh_arrival_wav_size,
                         hh_arrival_wav_checksum,HH_ARRIVAL_ASSET_VERSION);
}
