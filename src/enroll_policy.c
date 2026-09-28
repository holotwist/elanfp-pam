#include "enroll_policy.h"
#include <string.h>

void enroll_session_init(EnrollSession *s, int target_touches) {
    memset(s, 0, sizeof(*s));
    s->target_touches = target_touches;
}

const char *enroll_session_add(EnrollSession *s, const float *emb, const FrameQuality *q, bool *out_done) {
    *out_done = false;

    if (q->result != QUALITY_OK) {
        return "enroll-retry-scan";
    }

    // First touch, anchor touch requires core center placement
    if (s->profile.count == 0) {
        if (q->center_x < 18.0f || q->center_x > 60.0f ||
            q->center_y < 18.0f || q->center_y > 60.0f) {
            return "enroll-finger-not-centered";
        }
        memcpy(s->profile.embeddings[0], emb, sizeof(float) * EMBEDDING_DIM);
        s->profile.count = 1;
        return "enroll-stage-passed";
    }

    // Touches 1..N, compare against existing gallery
    float max_sim = -1.0f;
    for (int i = 0; i < s->profile.count; i++) {
        float dot = 0.0f;
        for (int d = 0; d < EMBEDDING_DIM; d++) {
            dot += emb[d] * s->profile.embeddings[i][d];
        }
        if (dot > max_sim) {
            max_sim = dot;
        }
    }

    // Identical position, finger was not shifted
    if (max_sim > SIM_MAX_DUPLICATE) {
        return "enroll-retry-scan";
    }

    // Outlier or wrong finger
    if (max_sim < SIM_MIN_OVERLAP) {
        return "enroll-retry-scan";
    }

    // Accept valid expansion
    memcpy(s->profile.embeddings[s->profile.count], emb, sizeof(float) * EMBEDDING_DIM);
    s->profile.count++;

    if (s->profile.count >= s->target_touches) {
        *out_done = true;
        return "enroll-completed";
    }

    return "enroll-stage-passed";
}