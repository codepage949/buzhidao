#ifndef BUZHIDAO_PADDLE_BRIDGE_WARMUP_H
#define BUZHIDAO_PADDLE_BRIDGE_WARMUP_H

#include <string>

#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)
struct buzhi_ocr_engine;

bool warmup_det_predictor(buzhi_ocr_engine* engine, std::string* err);
bool warmup_cls_predictor(buzhi_ocr_engine* engine, std::string* err);
bool warmup_rec_predictor(buzhi_ocr_engine* engine, std::string* err);
#endif

#endif
