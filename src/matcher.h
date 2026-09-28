#ifndef MATCHER_H
#define MATCHER_H

#include <stdint.h>

#define EMBEDDING_DIM 128
#define MAX_TEMPLATES 16
#define TARGET_ENROLL_TOUCHES 8

#define THRESHOLD_DIRECT    0.80f
#define THRESHOLD_AMBIGUOUS 0.68f
#define THRESHOLD_CONSENSUS 0.60f

typedef struct {
    float embeddings[MAX_TEMPLATES][EMBEDDING_DIM];
    int count;
} FingerprintProfile;

typedef struct {
    float best_score;
    float second_score;
    int consensus_count;
    int granted;
    const char *reason;
} MatchResult;

int matcher_init(const char *model_path);
void matcher_cleanup(void);
int matcher_extract(const uint8_t *img_79x79, float *out_embedding);
MatchResult matcher_verify(const float *probe_emb, const FingerprintProfile *profile);

int profile_save(const char *filename, const FingerprintProfile *profile);
int profile_load(const char *filename, FingerprintProfile *profile);

#endif