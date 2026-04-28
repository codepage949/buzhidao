#ifndef BUZHIDAO_PADDLE_BRIDGE_H
#define BUZHIDAO_PADDLE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct buzhi_ocr_engine buzhi_ocr_engine_t;
typedef struct buzhi_ocr_point {
    float x;
    float y;
} buzhi_ocr_point_t;

typedef struct buzhi_ocr_detection {
    buzhi_ocr_point_t polygon[4];
    char* text;
} buzhi_ocr_detection_t;

typedef struct buzhi_ocr_debug_detection {
    buzhi_ocr_point_t polygon[4];
    char* text;
    float score;
    int accepted;
} buzhi_ocr_debug_detection_t;

typedef struct buzhi_ocr_result {
    buzhi_ocr_detection_t* detections;
    int detection_count;
    buzhi_ocr_debug_detection_t* debug_detections;
    int debug_detection_count;
} buzhi_ocr_result_t;

buzhi_ocr_engine_t* buzhi_ocr_create(
    const char* model_dir,
    int use_gpu,
    const char* source,
    char** err
);

void buzhi_ocr_destroy(buzhi_ocr_engine_t* engine);

int buzhi_ocr_warmup_predictors(
    buzhi_ocr_engine_t* engine,
    char** err
);

buzhi_ocr_result_t* buzhi_ocr_run_image_file_result(
    buzhi_ocr_engine_t* engine,
    const char* image_path,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
);

buzhi_ocr_result_t* buzhi_ocr_run_image_rgba_result(
    buzhi_ocr_engine_t* engine,
    const unsigned char* rgba,
    int width,
    int height,
    int stride,
    int det_resize_long,
    float score_thresh,
    int debug_trace,
    char** err
);

void buzhi_ocr_free_string(char* s);
void buzhi_ocr_free_result(buzhi_ocr_result_t* result);

#ifdef __cplusplus
}
#endif

#endif
