#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "storage.h"

int storage_init_dir(const char *username) {
    char path[512];
    mkdir(STORAGE_BASE_DIR, 0700);
    snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE_DIR, username);
    return mkdir(path, 0700);
}

int storage_get_path(char *out, size_t sz, const char *username, const char *finger) {
    if (!out || !username || !finger) return -1;
    return snprintf(out, sz, "%s/%s/%s.dat", STORAGE_BASE_DIR, username, finger);
}

int storage_save_profile(const char *username, const char *finger, const FingerprintProfile *profile) {
    storage_init_dir(username);
    char path[512];
    storage_get_path(path, sizeof(path), username, finger);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(profile, sizeof(FingerprintProfile), 1, f);
    fclose(f);
    chmod(path, 0600);
    return (w == 1) ? 0 : -1;
}

int storage_load_profile(const char *username, const char *finger, FingerprintProfile *profile) {
    char path[512];
    storage_get_path(path, sizeof(path), username, finger);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(profile, sizeof(FingerprintProfile), 1, f);
    fclose(f);
    return (r == 1) ? 0 : -1;
}

int storage_list_fingers(const char *username, char ***out_fingers, size_t *out_count) {
    *out_fingers = NULL;
    *out_count = 0;

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", STORAGE_BASE_DIR, username);

    DIR *d = opendir(dir_path);
    if (!d) return 0;

    char **list = NULL;
    size_t count = 0;
    struct dirent *entry;

    static const char *const VALID_FINGERS[] = {
        "left-thumb", "left-index-finger", "left-middle-finger",
        "left-ring-finger", "left-little-finger", "right-thumb",
        "right-index-finger", "right-middle-finger", "right-ring-finger",
        "right-little-finger", NULL
    };

    while ((entry = readdir(d)) != NULL) {
        char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".dat") == 0) {
            size_t name_len = ext - entry->d_name;
            char *finger = strndup(entry->d_name, name_len);
            if (!finger) continue;

            bool valid = false;
            for (int i = 0; VALID_FINGERS[i]; i++) {
                if (strcmp(finger, VALID_FINGERS[i]) == 0) { valid = true; break; }
            }
            if (!valid) { free(finger); continue; }

            char **new_list = realloc(list, sizeof(char *) * (count + 1));
            if (!new_list) {
                free(finger);
                continue;
            }
            list = new_list;
            list[count++] = finger;
        }
    }
    closedir(d);

    *out_fingers = list;
    *out_count = count;
    return 0;
}

void storage_free_list(char **fingers, size_t count) {
    if (!fingers) return;
    for (size_t i = 0; i < count; i++) free(fingers[i]);
    free(fingers);
}

int storage_delete_finger(const char *username, const char *finger) {
    char path[512];
    storage_get_path(path, sizeof(path), username, finger);
    return unlink(path);
}

int storage_delete_user(const char *username) {
    char **fingers = NULL;
    size_t count = 0;
    storage_list_fingers(username, &fingers, &count);

    char path[512];
    for (size_t i = 0; i < count; i++) {
        storage_get_path(path, sizeof(path), username, fingers[i]);
        unlink(path);
    }
    storage_free_list(fingers, count);

    snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE_DIR, username);
    return rmdir(path);
}