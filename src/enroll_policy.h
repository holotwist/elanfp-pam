#ifndef ENROLL_POLICY_H
#define ENROLL_POLICY_H

#include <stdbool.h>
#include "matcher.h"
#include "quality.h"

#define SIM_MIN_OVERLAP   0.48f
#define SIM_MAX_DUPLICATE 0.965f

typedef struct {
    FingerprintProfile profile;
    int target_touches;
} EnrollSession;

void enroll_session_init(EnrollSession *s, int target_touches);

// Evaluates probe embedding and quality, updates profile, returns fprint status string
const char *enroll_session_add(EnrollSession *s, const float *emb, const FrameQuality *q, bool *out_done);

#endif