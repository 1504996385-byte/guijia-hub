#ifndef HOMEHUB_SEED_H
#define HOMEHUB_SEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HOMEHUB_EMBED_TEST_VOICES
/* Legacy host/first-boot tools retain embedded mode. Production CMake sets
 * this explicitly and defaults to reading the WAVs already on the TF card. */
#define HOMEHUB_EMBED_TEST_VOICES 1
#endif

/* Built-in ordinary Mandarin test voices, not recordings of family members. */
typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t bytes;
} HH_SeedAsset;

#if HOMEHUB_EMBED_TEST_VOICES
extern const HH_SeedAsset hh_seed_assets[];
extern const size_t hh_seed_asset_count;
#endif

/* Call once after mounting TF and before starting audio/network consumers.
 * arrive/leave and legacy test destinations are never replaced.
 * The requested take_key.wav revision is installed separately after backing
 * up a different valid old file; unreadable/unknown files are not overwritten.
 * TF-only mode then validates all three prompts. New reminder resources are
 * limited to this explicit migration, not the full legacy test-voice set. */
bool hh_seed_voices(void);

#endif
