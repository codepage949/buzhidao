#ifndef BUZHIDAO_PADDLE_BRIDGE_H
#define BUZHIDAO_PADDLE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct buzhi_ocr_engine buzhi_ocr_engine_t;

buzhi_ocr_engine_t* buzhi_ocr_create(
    const char* model_dir,
    int use_gpu,
    char** err
);

void buzhi_ocr_destroy(buzhi_ocr_engine_t* engine);

char* buzhi_ocr_run_image_file(
    buzhi_ocr_engine_t* engine,
    const char* image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
);

void buzhi_ocr_free_string(char* s);

#ifdef __cplusplus
}
#endif

#endif
