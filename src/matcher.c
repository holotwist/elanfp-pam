#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "onnxruntime_c_api.h"
#include "matcher.h"

static const OrtApi *g_ort = NULL;
static OrtEnv *g_env = NULL;
static OrtSessionOptions *g_opts = NULL;
static OrtSession *g_session = NULL;
static OrtMemoryInfo *g_mem_info = NULL;

int matcher_init(const char *model_path) {
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) return -1;

    if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "elan_matcher", &g_env) != NULL) return -1;
    if (g_ort->CreateSessionOptions(&g_opts) != NULL) return -1;

    g_ort->SetIntraOpNumThreads(g_opts, 2);

    if (g_ort->CreateSession(g_env, model_path, g_opts, &g_session) != NULL) return -1;
    if (g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem_info) != NULL) return -1;

    return 0;
}

void matcher_cleanup(void) {
    if (g_mem_info && g_ort) { g_ort->ReleaseMemoryInfo(g_mem_info); g_mem_info = NULL; }
    if (g_session && g_ort)  { g_ort->ReleaseSession(g_session); g_session = NULL; }
    if (g_opts && g_ort)     { g_ort->ReleaseSessionOptions(g_opts); g_opts = NULL; }
    if (g_env && g_ort)      { g_ort->ReleaseEnv(g_env); g_env = NULL; }
}

int matcher_extract(const uint8_t *img_79x79, float *out_embedding) {
    if (!g_session || !g_ort) return -1;

    float input_tensor_values[79 * 79];
    for (int i = 0; i < 79 * 79; i++) {
        input_tensor_values[i] = ((float)img_79x79[i] / 255.0f - 0.5f) / 0.5f;
    }

    int64_t input_shape[4] = {1, 1, 79, 79};
    OrtValue *input_tensor = NULL;
    OrtStatus *st = g_ort->CreateTensorWithDataAsOrtValue(
        g_mem_info,
        input_tensor_values,
        sizeof(input_tensor_values),
        input_shape,
        4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_tensor
    );
    if (st != NULL) return -1;

    const char *input_names[] = {"input"};
    const char *output_names[] = {"embedding"};
    OrtValue *output_tensor = NULL;

    st = g_ort->Run(g_session, NULL, input_names, (const OrtValue * const *)&input_tensor, 1, output_names, 1, &output_tensor);
    if (st != NULL) {
        g_ort->ReleaseValue(input_tensor);
        return -1;
    }

    float *emb_data = NULL;
    g_ort->GetTensorMutableData(output_tensor, (void **)&emb_data);
    memcpy(out_embedding, emb_data, EMBEDDING_DIM * sizeof(float));

    g_ort->ReleaseValue(output_tensor);
    g_ort->ReleaseValue(input_tensor);
    return 0;
}

MatchResult matcher_verify(const float *probe_emb, const FingerprintProfile *profile) {
    MatchResult res = {
        .best_score = -1.0f,
        .second_score = -1.0f,
        .consensus_count = 0,
        .granted = 0,
        .reason = "No enrolled templates"
    };

    if (!profile || profile->count == 0) return res;

    for (int i = 0; i < profile->count; i++) {
        float dot = 0.0f;
        for (int d = 0; d < EMBEDDING_DIM; d++) {
            dot += probe_emb[d] * profile->embeddings[i][d];
        }

        if (dot >= THRESHOLD_CONSENSUS) res.consensus_count++;

        if (dot > res.best_score) {
            res.second_score = res.best_score;
            res.best_score = dot;
        } else if (dot > res.second_score) {
            res.second_score = dot;
        }
    }

    if (res.best_score >= THRESHOLD_DIRECT) {
        res.granted = 1;
        res.reason = "Direct match";
    } else if (res.best_score >= THRESHOLD_AMBIGUOUS && res.consensus_count >= 2) {
        res.granted = 1;
        res.reason = "Consensus match";
    } else if (res.best_score >= THRESHOLD_AMBIGUOUS) {
        res.granted = 0;
        res.reason = "Inconclusive consensus";
    } else {
        res.granted = 0;
        res.reason = "Low similarity score";
    }

    return res;
}

int profile_save(const char *filename, const FingerprintProfile *profile) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    size_t w = fwrite(profile, sizeof(FingerprintProfile), 1, f);
    fclose(f);
    return (w == 1) ? 0 : -1;
}

int profile_load(const char *filename, FingerprintProfile *profile) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    size_t r = fread(profile, sizeof(FingerprintProfile), 1, f);
    fclose(f);
    return (r == 1) ? 0 : -1;
}