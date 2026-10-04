#pragma once

#include "buddy.h"
#include "nvs.h"

/**
 * Initialize profile subsystem: load or generate device identity, seed the
 * profile defaults on a fresh device, load privacy mode.
 *
 * The profile itself is not cached — each field is one NVS entry and is read on
 * demand, so nothing has to be reloaded after an edit.
 */
esp_err_t buddy_profile_init(void);

/**
 * Read the stored 8-byte profile hash (the beacon's "is this the same person I
 * met" marker).  Returns false when nothing has been saved yet, in which case
 * hash_out is zeroed.
 */
bool buddy_profile_get_hash(uint8_t hash_out[8]);

/**
 * Read / write a single profile field, addressed by one of the BUDDY_PROF_KEY_*
 * names.  Prefer these over buddy_profile_get()/set() whenever only one field is
 * wanted: the whole-profile calls read or write every key.
 *
 * @param key      BUDDY_PROF_KEY_NAME, _BIO, _APPEARANCE, _BELONGINGS,
 *                 _TRAITS or _TECH_LEVEL.
 * @param out/size Destination buffer and its size (get), or the NUL-terminated
 *                 value (set; NULL or "" removes the key).
 */
esp_err_t buddy_profile_get_field(const char *key, char *out, size_t size);
esp_err_t buddy_profile_set_field(const char *key, const char *value);

/**
 * Read several fields over a single NVS open.  Same BUDDY_PROF_KEY_* names as
 * buddy_profile_get_field(); the three arrays must be `count` long and are
 * indexed together.
 *
 * @return how many fields were present.
 */
size_t buddy_profile_get_fields(const char **keys, char *const *outs,
                                const size_t *sizes, size_t count);
