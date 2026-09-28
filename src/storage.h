#ifndef STORAGE_H
#define STORAGE_H

#include "matcher.h"

#define STORAGE_BASE_DIR "/var/lib/fprint"

int storage_init_dir(const char *username);
int storage_get_path(char *out, size_t sz, const char *username, const char *finger);
int storage_save_profile(const char *username, const char *finger, const FingerprintProfile *profile);
int storage_load_profile(const char *username, const char *finger, FingerprintProfile *profile);
int storage_list_fingers(const char *username, char ***out_fingers, size_t *out_count);
void storage_free_list(char **fingers, size_t count);
int storage_delete_finger(const char *username, const char *finger);
int storage_delete_user(const char *username);

#endif