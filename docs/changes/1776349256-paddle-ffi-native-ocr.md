# Paddle FFI Native OCR 경로 전환

## 구현 계획

- Python sidecar 호출 없이 Rust 바이너리 내에서 바로 Paddle Inference를 호출하도록
  `paddle-ffi` 피처를 추가한다.
- `OCR_BACKEND` 환경변수와 기본 PaddleOCR 캐시 경로 탐색 결과로 Python sidecar / Paddle
  FFI 백엔드를 선택한다.
- 모델 구조가 달라져도 동작할 수 있도록 Paddle 모델/사전/설정 탐색 로직을 강화한다.

## 구현 내용

- `app/Cargo.toml`
  - `paddle-ffi` 기능 플래그 추가.
- `app/src/ocr/mod.rs`
  - `OcrBackend`를 enum으로 확장해 `PythonSidecar`와 `PaddleFfi`를 분리.
  - `OCR_BACKEND`(`paddle_ffi`, `python_sidecar`)와 모델 디렉터리 존재 여부 기반으로
    백엔드 선택.
  - Paddle 경로 사용 시 OCR 입력 이미지를 임시 BMP로 저장해 FFI 엔진을 호출.
- `app/src/ocr/paddle_ffi.rs` (신규)
  - C ABI 래퍼(`buzhi_ocr_*`) 추가 및 JSON 응답 파싱.
  - `ImageFormat` 경로로 단일 이미지 처리 테스트 보강.
- `app/src/lib.rs`
- 시작 시 기본 PaddleOCR 캐시(`~/.paddlex/official_models`, Windows는 `%USERPROFILE%\\.paddlex\\official_models`) 기반 `resolve_paddle_model_dir` 모델 루트 탐색.
  - 공식 하위 폴더명(`PP-OCRv5_server_det` 등)과 `det/cls/rec` 형태 모두 인식하도록 검사 범위 확장.
  - 모델 필수 파일 검사(`has_required_paddle_model_files`, `has_stem_files`,
    `find_named_submodel_dir`, `has_stem_files_in_dir`) 테스트 추가.
- `app/build.rs`
  - `paddle-ffi` 빌드 시 `native/paddle_bridge/bridge.cc` 컴파일 연결.
  - 외부 설정 변수 의존을 제거하고 `app/.paddle_inference`를 기본 탐색 루트로 고정.
  - 패키지 레이아웃 유효성 검사 배치를 `{root}/include+lib`,
    `{root}/paddle/include+lib`, `{root}/paddle_inference/include+lib`로 통합.
  - MSVC UTF-8 컴파일 경고 방지를 위해 `/utf-8` 플래그 반영.
  - `rerun-if-changed` 항목에 네이티브 브릿지 파일 추가.
- `app/native/paddle_bridge/bridge.cc` (신규)
  - Paddle 모델 탐색/로드, det/cls/rec 추론 파이프라인 C++ 구현.
  - BMP 로드, 전처리(CHW, 정규화), NMS/box 처리, JSON 출력까지 포함.
- `app/src/lib.rs`
  - `find_named_submodel_dir` 후보 정렬을 이름 기반 정렬로 보정해 C++ 탐색 순서와 정합.
- `app/native/paddle_bridge/bridge.cc`
  - 인식 사전 후보 파일을 `inference.yaml`/`inference.yml`까지 추가로 탐색하도록 보완.
  - `NormalizeImage.scale`를 이중 적용하던 전처리 버그 수정.
  - det/cls/rec 전처리 모두 `pixel * scale`로 맞춰 sidecar와 동일한 입력 분포를 사용.

## sidecar(FastAPI)와 FFI(bridge.cc) 결과 정합성 작업 진행

- 비교 대상 수집
  - sidecar: `ocr_server/ocr_server.py` 실제 호출
  - Rust sidecar 게이트: `app/src/ocr/python_sidecar.rs`
  - FFI 파서/엔진: `app/native/paddle_bridge/bridge.cc`, `app/src/lib.rs`
- 요청 파라미터 비교(핵심)
  - sidecar 서버 생성(초기화): `PaddleOCR(use_doc_orientation_classify=False, use_doc_unwarping=False, use_textline_orientation=True, device=resolve_ocr_device(), lang=<요청 언어>)`
  - FFI 엔진 생성: `buzhi_ocr_create(model_dir, use_gpu=OCR_SERVER_DEVICE)` + `BUZHIDAO_PADDLE_FFI_SOURCE=normalize_source(언어)` 전달
  - sidecar 추론 호출: `ocr.predict(image_path, text_rec_score_thresh=score_thresh)`
  - FFI 추론 호출: `buzhi_ocr_run_image_file(image_path, det_resize_long=<backend_resize>, score_thresh=score_thresh, debug_trace=<cfg>)`
  - `det_resize_long` 초기 값:
    - Python sidecar: Rust 이미지 resize 단계에서 `engine.resize_width_before_ocr()` 값이 960이면 선행 리사이즈 수행 후 PaddleOCR 내부 로직 처리
    - Paddle FFI: `engine.resize_width_before_ocr()`를 0으로 두어 `bridge.cc`의 `run_pipeline`에서 `det_cfg.resize_long`(모델 기본) 사용
- sidecar 실제 호출 파라미터
  - 생성 시: `use_doc_orientation_classify=False`, `use_doc_unwarping=False`,
    `use_textline_orientation=True`, `device=resolve_ocr_device()`, `lang`.
  - 추론 시: `ocr.predict(..., text_rec_score_thresh=<요청 score_thresh>)`
  - warmup 시 score는 `0.5`로 고정 호출됨.
- Rust 기본값 연결
  - `score_thresh` 기본은 앱 설정의 `ocr.score_thresh`이며 현재 기본값 0.8.
  - sidecar는 요청마다 점수를 던지므로 FFI가 동일 동작하려면 점수 전달체인(요청 → 설정 → 기본값) 재확인 필요.

### 모델 탐색 정합성(핵심)

- `lib.rs`와 `bridge.cc` 모델 탐색은 det/cls/rec를 같은 루트(또는 하위 서브폴더)에서
  함께 찾는 구조를 전제로 동작.
- `load_model_preprocess_cfg`는 config 탐색 우선순위를 사용하며 현재 순서는
  `config.json`, `config.yaml`, `config.yml`, `inference.json`, `inference_config.json`,
  `inference.yaml`, `inference.yml`.
- 로컬 `~/.paddlex/official_models` 확인 결과, 각 폴더 루트는 `inference.json`을 직접
  가지며(현재 `base_code` + `program` 수준), 상위 루트(`.paddlex/official_models`)에서
  `PP-OCRv5_server_det`, `PP-OCRv5_server_rec`, `PP-LCNet_x1_0_doc_ori`,
  `PP-LCNet_x1_0_textline_ori`를 탐색해 det/rec/cls를 각각 매핑하는 방식이 동작한다.
- 현재 포맷은 `inference.json`만으로는 det/rec/cls threshold/resize/normalize 메타데이터를
  제공하지 않으므로 `load_model_preprocess_cfg`는 기본값(예: `DET_RESIZE_LONG=960`,
  `DET_THRESH=0.3` 등) 중심으로 동작한다.
- `find_named_submodel_dir` 정렬 보정은 유지되어 후보 순위/선호 규칙은 C++와 Rust 쪽 해석
  방식이 동일 계열이며, **구조상 큰 편차는 현재로서는 낮은 것으로 확인됨**.

### 최종 재검증 상태(2026-04-17)

- `bridge.cc` 컴파일: `cargo test --features paddle-ffi --no-run` 통과(컴파일 단계).
- `ocr::paddle_ffi` 샘플 실행:
  - 초기에는 런타임에서 `STATUS_DLL_NOT_FOUND`가 발생했으나, 테스트 실행 시
    `app/.paddle_inference/lib`를 `PATH`에 추가하면 실행 가능함을 확인.
  - `app/testdata/ocr/test.png` 기준 FFI 결과가 초기 `detections=[]`
    에서 수정 후 실제 검출 2건으로 바뀜.
  - 검출 예시:
    - `百度热搜上升热点` (`score=0.974451`)
    - `记国家主会共研` (`score=0.701375`)
- sidecar 실행 확인은 `ocr_server/ocr_server.py`에서 `paddleocr` import가 없어 즉시 실패
  (`ModuleNotFoundError: No module named 'paddleocr'`)로 블로킹.
- 번들된 `ocr_server/dist/ocr_server/ocr_server.exe`는 CLI 실행 자체는 가능하지만 현재 셸 캡처에서
  단건 JSON 출력이 비어 있어, 동일 이미지에 대한 sidecar 결과 본문 수집은 추가 확인이 필요함.
- 따라서 **FFI vs sidecar 실측 산출물 비교는 현재 환경에서 모두 blocked**이며, 구조·파서 일치도 비교는
  수행 완료 상태. 다음 단계는 sidecar 출력 수집 경로를 복구한 뒤 동일 입력 이미지(현재 `app/testdata/ocr/test.png`)로
  양측 점수/박스/텍스트 비교가 필요.

### inference 설정 파싱/기본값

- `bridge.cc`의 `ModelPreprocessCfg` 기본값이 존재하고, 실제 설정은 config 파일 우선순위대로
  첫번째 읽기 파일에서 적용됨.
- 일부 모델 디렉터리에서 `inference.yml`(확장자 `yml`)만 있는 경우가 있어 현재 탐색 목록과의
  오탐/누락 가능성을 재점검 필요.
- FFI/sidecar 정합성 목적의 정밀 동기화 포인트:
  - `PaddleFfiEngine` 호출 시 `det_resize_long`을 960 고정값에서 백엔드별 정책으로 분기.
  - `OcrBackend::resize_width_before_ocr`에서 `PythonSidecar`는 `OCR_SERVER_RESIZE_WIDTH`,
    `PaddleFfi`는 `0`을 반환하도록 변경.
  - `det_resize_long=0`은 `bridge.cc`에서 `run_pipeline`으로 전달되어 모델별 기본 `det_cfg.resize_long`
    사용.

## 회귀 대응 포인트

- Paddle 3.x PIR 형식(`inference.json`)에 맞춘 모델 로딩 경로 처리.
- Paddle 소스 트리 경로가 들어왔을 때 발생하는 링크 실패를 경고로 제한.
- Paddle 3.0 API 타입 변경(`std::string*` vs `char**`)에 따른 ABI 호출 수정.
- 빌드 디버깅 시 `model_dir`, `dll` 로딩 경로를 감지 가능한 로그를 남김.

## 중국어 샘플 OCR 확인

- `BUZHIDAO_PADDLE_FFI_SOURCE=zh`(또는 `ch`, `cn`)로 설정하면 중국어 모델이 기본
  선택되도록 반영.
- `1.png`를 FFI로 OCR한 결과는 `det_resize_long=0` 기준에서 3개 박스 검출에 대해 텍스트가
  정상적으로 추출됨.
  - `近日，粉色蓝每因凯值走红，鲜果被炒至40元一片知难觉正规现货。该品种开非新物`
  - `种，因种植条件苛刻。产量稀少价格品贵。网传具花青素含量五倍于普通监每系遥言，`
  - `热错树品多为管销暖头，不建试蒙建旨目种植。`

## 테스트

- `cargo test --features paddle-ffi -- --nocapture --exact ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다`
  - 환경:
    - `BUZHIDAO_RUN_FFI_SAMPLE_TEST=1`
    - `BUZHIDAO_PADDLE_FFI_SOURCE=zh` (또는 `ch`, `cn`)
    - `OCR_BACKEND=paddle_ffi`
    - 기본 PaddleOCR 캐시 경로에 공식 모델 루트 존재
    - `BUZHIDAO_FFI_TEST_IMAGE=<샘플 이미지 경로>`
  - `BUZHIDAO_FFI_TEST_IMAGE` 미지정 시 기본값은 `CARGO_MANIFEST_DIR/1.png`이며 없으면 테스트가 스킵됩니다.
  - 결과: 샘플 이미지에 대해 `3`개 OCR 블록과 인식 텍스트 출력 확인.
- `cargo test -p buzhidao --features paddle-ffi`
  - 결과: Python sidecar 및 Paddle FFI 단위 테스트 통과.
  - `cargo test -p buzhidao --features paddle-ffi --lib paddle_모델_검색은_캐시_경로_에서_탐색한다`
  - 결과: 기본 캐시 경로 탐색 테스트 통과.
- `cargo test -p buzhidao`
  - 결과: 기존 Python sidecar 경로 안정성 유지.

### Paddle Inference(v3) 구성

- 기본 동작은 `app/.paddle_inference`를 기준으로 `paddle_inference` 배포 루트를 고정 사용한다.
- v3 바이너리를 받았을 때는 압축 구조에 맞춰 `include`/`lib`가 존재하도록 배치하고
  `app/.paddle_inference` 하위(`include+lib` 또는 `paddle/include+lib` 또는
  `paddle_inference/include+lib`)로 정리한다.
- 패키지가 없는 상태면 `scripts/setup_paddle_inference.py`가 OS별 최신 v3 배포 URL을
  사용해 `app/.paddle_inference`에 아카이브를 내려받고 정리한 뒤 테스트로
  `cargo test -p buzhidao --features paddle-ffi` 통과를 확인하면 된다.
- Linux/macOS는 `scripts/setup_paddle_inference.py`로 동일한 정리 규칙을 적용해 사용할 수 있다.

## 보류/진행 상태

- `inference.yml` 단독 사용 및 단일 스테이지 루트 지정 시나리오의 동작 검증이 추가로 필요함.
- `OCR_SERVER_RESIZE_WIDTH` 전달값 고정 이슈는 해소되었으나, inference 단독 YAML 시나리오와
  드문 폴더 명명 규칙 혼합은 추가 검증이 필요.

## 후속 작업 (진행중)

- sidecar 호출 체인 점수 파라미터를 FFI로 동일 전달되도록 `score_thresh` 전파 정책 재정렬.
- `inference.yml`/`inference.yaml` 혼용 처리와 `inference.yml` 단독 환경에서 파싱 누락
  리스크 정리 및 보완.

## 최신 정합성 상태 (2026-04-17 추가)

- sidecar 실제 기준 출력(`ocr_server.exe --image app/testdata/ocr/test.png --source ch --score-thresh 0.1`)은 `7`개:
  - `百度热搜上升热点`
  - `小听`
  - `4月的北京，风和日暖，春意盎然。15日上午，北京人民大会堂西大厅华灯璀璨，中国`
  - `共产党党旗和越南共产党党旗并排而立，五星红旗和金星红旗相映成辉。在歌颂两国深`
  - `情厚谊的《越南一中国》乐曲声中，中共中央总书记、国家主席习近平同越共中央总书`
  - `记、国家主席苏林步入会场，共同会见参加“红色研学之旅”的中越青年代表。`
  - `1`
- FFI는 현재 긴 본문 `4`줄 텍스트를 sidecar와 동일하게 복원한다.
- 이번에 반영한 sidecar 정합화 포인트:
  - `rec` 전처리를 PaddleX `OCRReisizeNormImg`와 동일한 규약으로 변경
  - 기본 폭 `320`, 동적 폭 `max(320, ceil(48 * wh_ratio))`, 최대 폭 `3200`
  - `BGR / 255 -> CHW -> (x - 0.5) / 0.5`
  - `config.json`의 `Hpi.backend_configs.paddle_infer.trt_dynamic_shapes.x`에서 `rec` 최대 폭 파싱
- 검증 결과:
  - FFI는 현재 본문 `4`줄에 대해 sidecar와 동일 문자열, 동일한 수준의 score(`~0.99`)를 출력
  - 누락된 `3`개는 모두 작은 박스(`70x40`, `239x39`, `30x32`)이며 `det_boxes=7` 상태에서
    `rec` 결과가 `text=''`, `score=0.0`으로 비어 탈락
- 현재 남은 핵심 차이:
  - `bridge.cc`의 `db_postprocess`는 연결요소 기반 axis-aligned box 생성
  - sidecar/PaddleX는 contour 기반 `DBPostProcess` + min-area rect/quad를 사용
  - 긴 수평 라인은 현재 방식으로도 맞지만, 작은 기울어진 제목/아이콘/숫자는 이 차이로
    `rec` 입력 crop이 sidecar와 달라져 비어 버리는 것으로 보임
- 다음 작업은 이미지별 보정이 아니라, `DBPostProcess`를 PaddleX와 같은 contour/min-area-rect
  방식으로 치환해 작은 `3`개까지 sidecar와 동일하게 맞추는 것이다.

### 이번 턴 기준 정리

- sidecar와 FFI가 이미 동일해진 부분
  - 모델 선택: `PP-OCRv5_server_det` / `PP-LCNet_x1_0_textline_ori` / `PP-OCRv5_server_rec`
  - det resize 정책: `limit_side_len=64`, `limit_type=min`, `max_side_limit=4000`
  - det threshold: `thresh=0.3`, `box_thresh=0.6`, `unclip_ratio=1.5`
  - rec normalize: `(x / 255 - 0.5) / 0.5`
  - rec dynamic width 규약: base width `320`, max width `3200`
  - 긴 본문 4줄 recognition 결과
- 아직 동일하지 않은 부분
  - small box 3개(`百度热搜上升热点`, `小听`, `1`)가 FFI에서 최종 누락
  - 현재 FFI 로그상 해당 box들은 detector에서는 검출되지만 recognition에서 `text=''`, `score=0.0`
  - 따라서 남은 작업의 초점은 `rec` decode가 아니라 `det postprocess -> crop` 정합성

### 현재 결론

- FFI를 특정 테스트 이미지에 맞춰 보정한 것은 아니다.
- sidecar 내부 규약을 직접 추적해 다음 항목들을 sidecar와 동일하게 맞췄다:
  - `NormalizeImage.scale` 이중 적용 제거
  - PaddleX OCR pipeline의 det 기본 파라미터 반영
  - PaddleX `OCRReisizeNormImg` 전처리 규약 반영
  - `trt_dynamic_shapes` 기반 rec 최대 폭 해석 반영
- 남은 불일치는 구조적으로 `DBPostProcess` 구현 차이로 보는 것이 타당하다.

## 마무리

- 문서 상으로 현재 FFI 경로 전환, 모델 스캔/선택, C++ 브릿지 동작 정합성이
  반영됨.
- 로컬 개발 환경 절대경로는 문서/테스트 기록에 남기지 않고 환경변수 예시만
  안내.

## 최신 정합성 상태 (2026-04-17 추가 2)

- `bridge.cc`의 `db_postprocess`를 axis-aligned 연결요소 박스에서, hull 기반 oriented quad 생성으로 교체했다.
- 구성 요소:
  - 연결요소의 boundary point 수집
  - convex hull 계산
  - hull 기준 min-area-rect 계산
  - rect 기준 unclip 확장
  - 최종 polygon을 quad 순서(top-left, top-right, bottom-right, bottom-left)로 정렬
- `crop_to_bbox`도 bounding box copy가 아니라 quad warp crop으로 교체했다.
  - 기존: axis-aligned 사각형 잘라내기
  - 변경: 검출 quad를 따라 bilinear warp로 직사각형 OCR 입력 생성
- `score_box`는 bounding box 평균이 아니라 quad 내부 샘플 평균으로 변경했다.
- 의도:
  - 테스트 이미지 전용 보정이 아니라 sidecar/PaddleX의 `DBPostProcess -> quad -> crop` 체인과 구조를 맞추기 위함
  - 작은 회전/기울기 박스에서 detector는 맞는데 recognition이 비던 문제를 직접 해소하는 방향
- 추가로 `rec` 패딩 영역 초기값을 정규화 후 기준(`-1`)으로 수정했다.
  - 기존 FFI는 동적 폭 오른쪽 패딩이 `0`
  - PaddleX 규약상 `(0 / 255 - 0.5) / 0.5 = -1`
  - 작은 박스처럼 패딩 비중이 큰 경우 recognition blank에 직접 영향
- `textline orientation` 적용도 sidecar 규약에 맞춰 보정했다.
  - 기존 FFI는 `cls label == 1`이면 score와 무관하게 즉시 180도 회전
  - 수정 후 `cls score >= 0.9`일 때만 회전
  - 작은 박스에서 오분류 저신뢰 회전이 recognition blank로 이어질 가능성을 제거
- `load_model_preprocess_cfg()`의 inline `character_dict` 파싱 경로에서 ASCII space 추가가 누락되던 버그를 수정했다.
  - 배열형 `character_dict`를 읽으면 조기 `return`
  - 그 결과 `CTCLabelDecode`용 dict가 `18383`에 머물렀음
  - 수정 후 inline dict도 `use_space_char=True` 규약대로 ASCII space를 추가
- crop 경로를 직선 보간 사변형 샘플링에서 perspective transform 기반 warp로 교체했다.
- point ordering도 단순 y/x 정렬에서 centroid-angle 기반 clockwise 정렬로 보강했다.
- warp 후 세로 비율이 큰 경우(`h / w >= 1.5`) 90도 회전을 적용해 PaddleOCR의 회전 crop 규약에 더 가깝게 맞췄다.
- 추가 디버그로 남은 2개 박스는 `rec` logits가 실제로 전 timestep에서 `blank(0)` 우세라는 점을 확인했다.
- 즉 남은 차이는 CTC decode 후처리가 아니라, 그보다 앞단의 입력 표현 차이일 가능성이 높다.
- 현재 유력 가설은 FFI 경로가 원본 `PNG`를 직접 읽지 않고 임시 `BMP`로 변환해 전달하는 점이다.
- `bridge.cc`에 Windows GDI+ 기반 이미지 로더를 추가해 `PNG/JPEG/...` 원본 포맷을 직접 읽을 수 있게 했다.
- Rust FFI 경로도 임시 `BMP` 대신 임시 `PNG`를 저장하도록 변경했다.
- 샘플 테스트는 더 이상 `PNG -> BMP` 변환을 거치지 않고 원본 경로를 그대로 FFI에 전달한다.
- 원본 `PNG` 직독 경로로도 재검증했지만 FFI 결과는 여전히 `5`개였다.
- 따라서 `PNG -> BMP` 변환은 핵심 원인이 아니며, 남은 차이는 작은 박스 crop의 실제 픽셀 분포/샘플링 정합성 쪽으로 더 좁혀졌다.

## 2026-04-17 추가 진행 상황

### 이번 턴 변경
- `bridge.cc`
  - `crop_to_bbox`를 OpenCV `getPerspectiveTransform + warpPerspective(INTER_CUBIC, BORDER_REPLICATE)`에 더 가깝게 유지한 상태에서 rejected/low-score crop BMP 덤프를 추가했다.
  - `resize_bilinear`를 기존 `align_corners` 방식에서 OpenCV `cv2.resize`와 같은 half-pixel center 규약으로 교체했다.
    - 기존: `(src-1)/(dst-1)` 계열
    - 변경: `(x + 0.5) * scale - 0.5` + replicate clamp

### 현재 실측 결과 (`app/testdata/ocr/test.png`, `score_thresh=0.1`)
- sidecar: 7건
  - `百度热搜上升热点`
  - `小听`
  - 본문 4줄
  - `1`
- FFI: 7건
  - `百度热搜上升热点`
  - 본문 4줄
  - `ののジ`
  - `Y`

즉, 개수는 sidecar와 같아졌지만 최종 텍스트는 아직 동일하지 않다.

### 이번 턴에서 확인한 사실
- FFI dump crop와 Python/OpenCV로 같은 polygon을 `INTER_CUBIC + BORDER_REPLICATE`로 잘라낸 기준 crop의 픽셀 차이는 매우 작다.
  - `small1(70x41)`: 채널 평균 차이 약 `0.23 / 0.24 / 0.22`, 최대 차이 `4`
  - `small2(31x32)`: 채널 평균 차이 약 `0.10 / 0.09 / 0.14`, 최대 차이 `4`
- 따라서 `원본 full-image -> first crop warp` 자체는 이제 큰 원인이 아닐 가능성이 높다.
- 그러나 작은 2개 박스를 crop 이미지 자체로 다시 넣어보면 차이가 남는다.
  - FFI에 `low_score_1101_270_31x32.bmp`를 직접 넣으면 `det_boxes=1` 후 `rec`가 빈 문자열로 떨어진다.
  - FFI에 `low_score_1153_28_70x41.bmp`를 직접 넣어도 `det_boxes=1` 후 `rec`가 빈 문자열로 떨어진다.
- sidecar 재검증:
  - sidecar server에 `low_score_1101_270_31x32.bmp`를 넣으면 `1`을 정상 복원한다.
  - sidecar server에 `low_score_1153_28_70x41.bmp`를 넣으면 `听`을 복원한다.

### 현재 해석
- `1` 박스:
  - FFI crop 자체는 sidecar가 읽을 수 있다.
  - 따라서 남은 차이는 FFI의 `det -> second crop -> rec` 경로에 있다.
  - 특히 작은 박스에서의 `DBPostProcess/box geometry` 또는 `second-stage crop` 정합성이 아직 sidecar와 다르다.
- `小听` 박스:
  - FFI crop를 sidecar에 다시 넣어도 `小听` 전체가 아니라 `听`만 읽는다.
  - 즉 이 케이스는 첫 번째 crop에서 이미 좌측 문자가 충분히 보존되지 않았을 가능성이 있다.
  - 좌상단 작은 box에서 contour/min-area-rect/score_box/expand 규약이 여전히 sidecar와 미세하게 다를 수 있다.

### 다음 우선순위
1. `db_postprocess`를 connected-component/hull 기반이 아니라 contour tracing 기반으로 더 직접 맞추기
2. 작은 box에서 `score_box`, `min_side`, `expand_rect(unclip)` 규약을 sidecar/PaddleOCR 쪽과 다시 대조하기
3. 필요하면 first-stage crop뿐 아니라 second-stage crop도 파일로 덤프해 sidecar와 단계별 비교하기

## 2026-04-17 추가 진행 상황 2

### 이번 턴에서 유지된 변경
- `bridge.cc`
  - `db_postprocess`의 connected component 연결성을 `4-neighbor`에서 `8-neighbor`로 올렸다.
  - boundary cell corner를 그대로 hull에 넣는 대신, component contour를 edge-chain으로 trace하는 보조 경로를 추가했다.
  - crop dump를 `first_stage_*`와 `low_score_*`로 분리해 단계별 비교가 가능하도록 했다.
- 의도:
  - sidecar/OpenCV의 contour 기반 postprocess와 구조를 더 가깝게 만들고,
  - original image 기준 first-stage crop와 crop-image 재투입 시의 second-stage 차이를 같은 파일 세트로 추적하기 위함

### 재확인 결과
- baseline은 유지된다.
  - sidecar: `7`건
  - FFI: `7`건
- 여전히 다른 텍스트:
  - sidecar: `小听`, `1`
  - FFI: `ののジ`, `Y`
- 즉 이번 턴 변경만으로 최종 문자열 정합성은 개선되지 않았다.

### crop 재투입으로 좁혀진 사실
- `first_stage_1101_270_31x32.bmp`를 FFI에 다시 넣으면:
  - `det_boxes=1`
  - second-stage crop는 `16x21`
  - polygon은 거의 axis-aligned
  - `rec`는 blank 우세로 최종 탈락
- 반면 같은 crop를 sidecar에 넣으면 `1`을 복원한다.
- 따라서 `1` 케이스는 여전히 `DBPostProcess -> second-stage crop` 정합성 문제다.

- `first_stage_1153_28_70x41.bmp`를 FFI에 다시 넣으면:
  - FFI는 `听听听听`를 낸다.
- 같은 crop를 sidecar에 넣으면:
  - sidecar는 `听`만 복원한다.
- 즉 `小听` 케이스는 first-stage crop coverage뿐 아니라, crop-image 재투입 이후의 detector/box geometry도 sidecar와 아직 다르다.

### 시도했다가 되돌린 변경
- contour/hull을 직접 `unclip`한 뒤 mini-box를 다시 잡는 근사 버전을 한번 적용해 봤다.
- 그러나 이 구현은 본문 긴 줄들의 crop 높이를 과도하게 줄여 전체 인식 품질을 크게 악화시켰다.
- sidecar 규약을 제대로 복제한 것이 아니라는 판단으로 즉시 되돌렸다.
- 현재 브랜치에는 이 regression은 남아 있지 않고, 다시 안정 baseline(`7`건, 본문 4줄 정상, 소형 2개만 불일치) 상태다.

### 현재 결론
- first-stage warp와 OpenCV crop 차이는 이미 매우 작다.
- 남은 핵심은 여전히 `DBPostProcess`의 box 산출 규약이다.
- 특히 작은 component에서:
  - sidecar는 기울어진 mini-box를 내고
  - FFI는 axis-aligned에 가까운 box를 내는 경향이 남아 있다.
- 다음 단계는 `connected component + custom hull`을 더 밀어붙이는 것이 아니라,
  sidecar/PaddleOCR의 `findContours -> get_mini_boxes -> unclip -> get_mini_boxes` 순서를 더 직접 복제하는 쪽이 맞다.

## 2026-04-17 추가 진행 상황 3

### 이번에 확인한 regression / revert
- `trace_component_contour`를 chain-code contour tracing 쪽으로 더 직접 바꾸는 시도를 했지만,
  small box가 오히려 더 나빠졌다.
  - `1`이 다시 탈락
  - 상단 소형 박스도 더 악화
- `boundary pixel center`를 더 직접 반영해 `mini-box`를 잡는 시도도 regression이었다.
  - 작은 박스 정합성은 개선되지 않았고 baseline `7`건 상태를 해쳤다.
- hull 기준 polygon offset으로 `unclip`을 근사하는 시도도 regression이었다.
  - `1`이 다시 누락
  - 상단 박스는 `ののジ`에서 `の`로 더 악화
- 위 변경들은 모두 즉시 되돌렸고, 현재 브랜치는 다시 안정 baseline 상태다.

### 현재 baseline 재정리
- sidecar: `7`건
  - `百度热搜上升热点`
  - `小听`
  - 본문 `4`줄
  - `1`
- FFI: `7`건
  - `百度热搜上升热点`
  - 본문 `4`줄
  - `ののジ`
  - `Y`

즉 개수 mismatch는 해소됐지만, 작은 박스 `2`개 텍스트는 아직 sidecar와 동일하지 않다.

### PaddleOCR 공식 `DBPostProcess`와 직접 대조한 결과
- 공식 구현은 다음 순서를 따른다.
  1. `findContours`
  2. `get_mini_boxes(contour)`
  3. `box_score_fast(pred, points)`
  4. `unclip(points, ratio)` with `pyclipper.PyclipperOffset(... JT_ROUND ...)`
  5. `get_mini_boxes(unclipped)`
  6. `sside < min_size + 2` 필터
  7. `round(... / width * dest_width)` 스케일링
- 참고 소스:
  - `https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/postprocess/db_postprocess.py`

### 공식 규약 대조로 좁혀진 결론
- `get_mini_boxes`의 point ordering / short-side 판정을 PaddleOCR 쪽에 가깝게 맞춰도
  small box 결과는 바뀌지 않았다.
- `box_score_fast` 스타일 점수 계산과 최종 `round` 스케일링까지 공식 방식으로 바꾸면
  오히려 `5`건까지 후퇴하는 regression이 발생했다.
- 따라서 남은 차이는 `score_box` 미세조정이나 point ordering이 아니라,
  거의 `unclip` 단계로 수렴한다.
- 공식 구현은 `pyclipper`의 `JT_ROUND` offset을 사용하지만,
  현재 FFI는 여전히 rect-expand 기반 근사 `unclip`을 사용한다.
- small box `2`개는 바로 여기서 polygon geometry가 달라지는 패턴으로 보인다.

### 현재 작업 결론
- 이미지별 보정이 아니라 sidecar/PaddleOCR의 실제 postprocess 순서를 맞추는 방향으로 진행 중이다.
- 현재 남은 핵심 작업은 `bridge.cc`의 `unclip`을 공식 구현과 더 가까운 polygon offset 경로로 교체하는 것이다.
- `score_box`, `mini-box`, contour tracing을 따로 미세조정하는 방식은 현재까지는 의미 있는 개선보다 regression 가능성이 더 컸다.

## 2026-04-17 추가 진행 상황 4

### `unclip` 단독 교체 실험
- `bridge.cc`의 `unclip`을 rect-expand 근사 대신 round-join polygon offset 방식으로 한 차례 교체했다.
- 의도는 PaddleOCR `pyclipper JT_ROUND`에 더 가까운 geometry를 넣어 small box `2`개의 최종 문자열을 움직이는 것이었다.
- 하지만 현재 FFI 파이프라인에서 `unclip` 입력은 이미 `mini-box(rect_pts)`이며, 이 상태에서 offset 방식만 바꿔도 실측 출력은 변하지 않았다.

### 실측 결과
- sidecar: `7`건
  - `百度热搜上升热点`
  - `小听`
  - 본문 `4`줄
  - `1`
- FFI: 여전히 `7`건
  - `百度热搜上升热点`
  - 본문 `4`줄
  - `ののジ`
  - `Y`

즉 `unclip` 단독 교체는 결과를 전혀 움직이지 못했고, 해당 변경은 즉시 되돌렸다.

### 이 실험으로 좁혀진 결론
- 남은 차이를 `unclip` 하나만의 문제로 보는 가설은 현재 기준으로 약해졌다.
- 핵심은 여전히 `findContours/contour shape -> first mini-box` 쪽의 geometry이며,
  현재 FFI가 그 이전 단계에서 이미 sidecar와 다른 rect를 만들고 있을 가능성이 더 크다.
- 따라서 다음 우선순위는:
  1. `unclip` 미세조정보다 `contour -> get_mini_boxes` 입력 shape를 sidecar와 더 직접 맞추기
  2. small crop 재투입 기준으로 second-stage polygon을 sidecar와 수치 비교하기

## 2026-04-17 추가 진행 상황 5

### `contour-only` mini-box 입력 실험
- 현재 FFI는 `connected component cell center + traced contour`를 함께 모아 hull을 만들고,
  그 hull에서 `min_area_rect -> get_mini_box`를 구한다.
- 이 혼합 경로가 sidecar의 `findContours(contour)`와 다르므로,
  `trace_component_contour()` 결과만 직접 `min_area_rect`에 넣는 실험을 했다.

### 결과
- 이 변경은 regression이었다.
- FFI 결과가 `7`건에서 `6`건으로 후퇴했고,
  기존에 맞던 상단 headline `百度热搜上升热点`까지 탈락했다.
- small box `2`개(`ののジ`, `Y`)도 그대로여서 목표 불일치는 전혀 줄지 않았다.
- 따라서 해당 변경은 즉시 되돌렸고, 현재 브랜치는 다시 baseline 상태다.

### 의미
- 공식 순서만 흉내 내는 것으로는 부족하고,
  현재 FFI의 `trace_component_contour()`가 sidecar/OpenCV `findContours()`와 충분히 같은 contour를 만들지 못한다는 뜻이다.
- 즉 다음 작업은 `contour만 쓰기`가 아니라,
  `trace_component_contour()` 자체를 OpenCV contour 규약에 더 가깝게 만드는 쪽이어야 한다.

## 2026-04-17 추가 진행 상황 6

### small-stage PCA 기울기 실험
- 남은 차이가 `second-stage small crop db_postprocess`에 집중되어 있으므로,
  `bridge.cc`에서 `small-stage + 세로로 긴 component`에 한해 `PCA` 기반 기울기 rect를 만드는 실험을 했다.
- 목적은 하단 숫자 박스가 sidecar처럼 axis-aligned rect가 아니라 기울어진 quad로 나오게 만드는 것이었다.
- 이 변경은 full-image 1차 det에는 적용하지 않았고,
  dumped small crop 재투입 케이스에서만 제한적으로 시험했다.

### 결과
- 숫자 박스 polygon은 실제로 sidecar 쪽에 더 가까워졌다.
- FFI second-stage numeric polygon 변화:
  - 기존: `[[8.765625,5.698925],[25.140625,5.698925],[25.140625,26.967743],[8.765625,26.967743]]`
  - 실험: `[[7.371733,5.951253],[23.912786,5.323678],[25.629314,26.749752],[9.088261,27.377325]]`
- sidecar second-stage numeric polygon 기준:
  - `[[7,5],[21,3],[24,27],[10,28]]`
- 즉 geometry는 axis-aligned에서 벗어나 sidecar와 더 비슷해졌지만,
  최종 `rec` 결과는 여전히 blank였고 `1`로 복원되지 않았다.
- 상단 small crop도 의미 있는 개선은 없었고, `听听听听`가 그대로 유지됐다.

### 결론
- 남은 차이는 `polygon angle` 하나만의 문제가 아니다.
- second-stage에서 polygon을 sidecar 쪽으로 기울여도 `rec`가 그대로 실패하므로,
  현재 병목은 `contour -> mini-box`뿐 아니라 그 이후 `crop/warp`까지 포함한 입력 정합성이다.
- 이 실험은 목표 달성 없이 heuristic만 늘리는 방향이어서 즉시 되돌렸고,
  현재 브랜치는 다시 stable baseline(`7`건, 오인식 `ののジ`, `Y`)으로 복구돼 있다.

## 2026-04-17 추가 진행 상황 7

### `crop_to_bbox` 가설 재검증
- `preprocess_rec` stride 버그 수정 이후:
  - FFI full-image 결과는 `7`건까지 sidecar와 맞고,
  - 남은 불일치는 상단 small box 하나(`小听` vs `听`)만 남았다.
- 이 시점에서 남은 원인을 `crop_to_bbox(custom warpPerspective)`로 의심했고,
  다음 실험을 수행했다.

### 직접 반증된 사실
- sidecar top crop 기준 FFI second-stage polygon:
  - `[[9.501953,3.275391],[68.154297,3.275391],[68.154297,36.943359],[9.501953,36.943359]]`
- 위 polygon으로 Python/OpenCV(`warpPerspective`, `INTER_LINEAR`, `BORDER_REPLICATE`, `dst=w-1/h-1`) crop를 만들면:
  - 출력 파일 `ffi_poly_cv2.png`
  - sidecar 결과: `小听`
- 그런데 같은 `ffi_poly_cv2.png`를 FFI에 직접 넣어도:
  - FFI 결과는 여전히 `听`

즉, **남은 마지막 불일치는 `crop_to_bbox`가 아니라 `same crop -> second-stage det/db_postprocess` 경로**다.

### 현재 의미
- `crop_to_bbox`를 double precision / half-pixel 보정 / axis-aligned fast path로 바꿔도
  최종 결과는 바뀌지 않았다.
- 따라서 `warpPerspective`가 마지막 병목이라는 가설은 현재 기준으로 반증됐다.
- 실제 남은 차이는:
  - sidecar는 이미 맞는 crop에서도 `小听`를 유지하고
  - FFI는 같은 crop를 다시 넣었을 때 second-stage detector가 더 큰 box를 만들고 `听`로 축소한다.

### 현재 최신 실측
- sidecar on `ffi_poly_cv2.png`:
  - polygon: `[[0,0],[56,0],[56,31],[0,31]]`
  - text: `小听`
- FFI on same `ffi_poly_cv2.png`:
  - polygon: `[[0.0,0.0],[56.685936,0.0],[56.685936,32.535938],[0.0,32.535938]]`
  - text: `听`

### 정리
- `crop_to_bbox`는 더 이상 주된 타깃이 아니다.
- 남은 차이는 **이미 올바른 crop가 주어진 상태에서도 발생하는 second-stage `DBPostProcess` 정합성**이다.
- 다음 작업은 `crop/warp`가 아니라:
  1. same-crop 기준 sidecar/FFI second-stage polygon을 수치 비교
  2. `findContours -> get_mini_boxes -> unclip -> get_mini_boxes`의 small-crop 경로를 sidecar에 더 직접 맞추기

## 2026-04-17 추가 진행 상황 8

### sidecar Python 내부 predictor 직접 대조
- 번들 exe 로그만으로는 한계가 있어, 로컬 `ocr_server/.venv`의 Python 환경에서
  `PaddleOCR -> paddlex_pipeline -> text_rec_model` 내부를 직접 따라가 raw `rec` 출력을 확인했다.
- 확인 경로:
  - `PaddleOCR(...).paddlex_pipeline._pipeline.text_rec_model`
  - 전처리: `ReadImage(format="RGB") -> OCRReisizeNormImg -> ToBatch`
  - 추론: `predictor.run()` 경로를 타는 `rec.infer(x=[...])`
- 문제 crop(`ffi_poly_sidecar_box.png`)에 대해 sidecar Python raw top-1 시퀀스는 실제로 초반 timestep에서 `小`,
  뒤에서 `听`를 포함한다.
  - 즉 sidecar는 postprocess 이전의 raw logits 단계에서 이미 `小听` 정보를 가지고 있다.

### FFI `decode_ctc` raw top-1 로그 대조
- `bridge.cc`의 `decode_ctc`에 env 기반 디버그 로그를 추가해, non-empty text에 대해서도
  timestep별 top-1 index/score를 출력하도록 했다.
- 같은 crop를 FFI에 넣으면 raw top-1 시퀀스에는 `听`만 나타나고 `小`는 아예 나타나지 않는다.
- 따라서 남은 차이는:
  - `CTC decode` 규약 차이
  - dict 길이/space 처리
  - crop 후처리 문자열 조립
  가 아니라, **`rec predictor output` 자체의 차이**다.

### predictor 옵션 정렬 시도
- sidecar Python 내부 `pp_option`을 직접 확인해 CPU 기본값을 bridge 쪽과 맞췄다.
  - `EnableMKLDNN()`
  - `SetMkldnnCacheCapacity(10)`
  - `SetCpuMathLibraryNumThreads(10)`
  - `EnableNewIR(true)`
  - `EnableNewExecutor(true)`
  - `SetOptimizationLevel(3)`
  - `EnableMemoryOptim(true)`
- `SwitchUseFeedFetchOps(false)`도 제거해 sidecar 기본 생성 경로와 더 비슷하게 맞췄다.
- 그러나 문제 crop과 full-image 결과는 바뀌지 않았다.
  - sidecar: top small box `小听`
  - FFI: top small box `听`

### `crop_to_bbox` axis-aligned fast path 버그 수정
- 문제 crop의 second-stage polygon이 사실상 이미지 전체 정수 축정렬 박스인 경우에도,
  기존 FFI는 bilinear 샘플링으로 다시 늘려 그리면서 입력을 미세하게 흐리게 만들고 있었다.
- `bridge.cc`에서 integer axis-aligned box를 직접 pixel copy하는 fast path를 추가했다.
- 수정 후 FFI가 덤프한 `rec_input`과 OpenCV 기준 `rec_input`의 차이는 사실상 사라졌다.
  - `REC_DIFF max=1`
  - `REC_DIFF mean≈0.018`

### 현재 결론
- 여기까지 맞춰진 항목:
  - `crop`
  - `warp`
  - `resize`
  - `rec_input image`
  - `CTC decode`
  - predictor CPU 옵션(`mkldnn`, thread, new IR/executor)
- 그런데도 같은 crop, 같은 수준의 `rec_input`에서:
  - sidecar Python/PaddleX predictor는 `小听` 쪽 raw logits를 낸다.
  - C++ FFI Paddle Inference predictor는 `听`만 남긴다.
- 즉 남은 마지막 불일치는 **전처리나 `DBPostProcess`가 아니라, 동일 `rec` 입력에 대한 predictor raw output 차이**로 좁혀졌다.

### 다음 작업
1. sidecar Python의 raw `rec` logits를 파일로 덤프
2. FFI `run_rec` raw output도 같은 형식으로 덤프
3. timestep/class 단위로 직접 비교해 어느 시점부터 출력이 갈라지는지 확인

## 2026-04-17 추가 진행 상황 9

### 최종 원인
- 마지막 불일치(`小听` vs `听`)는 predictor 런타임 자체의 차이가 아니었다.
- raw logits / 입력 텐서를 직접 덤프해 비교한 결과, 실제 차이는 `preprocess_rec()`의 우측 패딩 처리였다.
- sidecar Python `OCRReisizeNormImg -> ToBatch` 경로는 우측 패딩 영역을 `0.0`으로 유지한다.
- 기존 FFI는:
  - 처음에는 패딩 기본값을 `-1`로 채우고 있었고,
  - 이후 `0`으로 바꾼 뒤에도 캔버스 전체 폭(`prepared.width == dynamic_w`)을 다시 순회하며
    패딩 zero pixel을 정규화해 `-1`로 덮어쓰고 있었다.
- 즉 `rec_input` 이미지 BMP는 비슷해 보여도, 실제 predictor에 들어가는 `x` 텐서는 sidecar와 달랐다.

### 수정 내용
- `bridge.cc`
  - `run_rec` raw logits / input tensor dump 추가
  - `preprocess_rec()` 패딩 기본값을 `0.0`으로 변경
  - alpha가 `0`인 패딩 픽셀은 정규화 루프에서 건너뛰도록 수정
- 이 수정 후:
  - FFI 문제 crop `ffi_poly_sidecar_box.png` 결과가 `听`에서 `小听`으로 바뀜
  - raw top-1도 sidecar와 같은 패턴으로 복원됨
    - timestep 2: `小`
    - timestep 8: `听`

### 최종 실측 결과
- 기준 이미지: `app/testdata/ocr/test.png`
- sidecar: `7`건
  - `百度热搜上升热点`
  - `小听`
  - 본문 `4`줄
  - `1`
- FFI: `7`건
  - `百度热搜上升热点`
  - `小听`
  - 본문 `4`줄
  - `1`

즉 현재 기준 이미지에서는 FFI가 sidecar와 동일한 OCR 결과를 낸다.

### 이번 작업의 의미
- 최종 차이는 테스트 이미지 전용 보정이 아니라, sidecar 내부 `rec` 전처리 규약을 끝까지 따라간 결과였다.
- 특히 `rec_input image`가 같아 보여도 실제 tensor `x`가 다를 수 있다는 점이 핵심이었다.
- 이번 수정으로 남아 있던 마지막 mismatch는 `DBPostProcess`가 아니라 `rec padding tensor semantics`였음이 확인됐다.

## 2026-04-17 추가 진행 상황 10

### 추가 검증 이미지 결과
- 기준 이미지를 `test.png` 하나로 두지 않고 `test2.png`, `test3.png`로 범위를 넓혀 sidecar와 재비교했다.
- 결과:
  - `test.png`: sidecar와 FFI가 동일
  - `test3.png`: 초기에는 sidecar `27`, FFI `24`
  - `test2.png`: sidecar `14`, FFI `12`
- 즉 `test.png`에서 마지막 `rec padding` mismatch를 해소했더라도, 일반화 기준으로는 아직 sidecar와 완전히 동일하지 않았다.

### `DetResizeForTest` 규약 차이 수정
- 추가 비교 과정에서 `test3.png`의 큰 차이가 `DBPostProcess` 이전, det 입력 크기 정렬 규약에서 시작된다는 점을 확인했다.
- sidecar/PaddleX `DetResizeForTest.resize_image_type0()`는 `32` align 시 `ceil`이 아니라 `round(size / 32) * 32`를 사용한다.
- 기존 FFI는 det 입력을 `ceil to 32`로 정렬하고 있었고, 이 때문에 같은 원본 이미지에서도 sidecar와 다른 det 입력 shape가 만들어졌다.
  - 예: `test3.png`
    - sidecar det input: `1696x1216`
    - 기존 FFI det input: `1696x1248`
- `bridge.cc`의 `resize_for_det()`를 sidecar 규약과 같은 `round(size / 32) * 32`로 수정했다.

### 수정 후 결과
- `test3.png`
  - sidecar: `27`
  - FFI: `27`
  - 기존에 인접 긴 텍스트와 합쳐지던 `5`, `2` 박스는 분리되었다.
  - 남은 대표 차이는 작은 아이콘 한 건으로 좁혀졌다.
    - sidecar: `💯`
    - FFI: `🙌`
- `test.png`
  - sidecar와 동일 상태 유지
- `test2.png`
  - 여전히 sidecar `14`, FFI `12`

### predictor / det map 직접 비교로 좁혀진 사실
- `test2.png`, `test3.png` 모두에서 sidecar와 FFI의 det 입력 텐서 및 det 출력 맵 차이는 매우 작다.
  - `test2.png`
    - `input_diff_mean ~= 0.000157`
    - `pred_diff_mean ~= 0.000109`
  - `test3.png`
    - `input_diff_mean ~= 0.000267`
    - `pred_diff_mean ~= 0.000203`
- 즉 남은 차이는 대규모 det predictor 불일치보다는, 이후 단계의 박스 해석 또는 작은 crop 인식 쪽에 더 가깝다.

### `test2.png` 원인 재분류
- 처음에는 `test2.png`의 남은 차이도 `db_postprocess`라고 봤지만, 추가 trace로 더 정확한 상태를 확인했다.
- 현재 FFI도 `0`, `舌` 후보 box 자체는 이미 잡고 있다.
  - `db_postprocess: boxes=19`
  - small candidate polygon도 accept 상태까지 간다.
- 그러나 그 뒤 recognition에서 둘 다 blank로 탈락한다.
  - `0` 후보: `crop=112x51`, `text=''`
  - `舌` 후보: `crop=111x49`, `text=''`
- 따라서 `test2.png`의 남은 차이는:
  - box를 못 찾는 문제가 아니라
  - **small symbol / short text crop를 `rec`가 sidecar처럼 복원하지 못하는 문제**다.

### 현재 상태 요약
- `test.png`
  - sidecar와 동일
- `test3.png`
  - box 개수는 sidecar와 동일(`27`)
  - 남은 대표 차이: 작은 아이콘 `💯` vs `🙌`
- `test2.png`
  - sidecar `14`, FFI `12`
  - 대표 차이:
    - `0` 누락
    - `舌` 누락
    - 일부 짧은 문자열/기호 조합의 recognition 차이

### 현재 결론
- 이번 단계에서도 테스트 이미지 맞춤 보정은 하지 않았다.
- 실제로 sidecar가 쓰는 `DetResizeForTest` 규약을 따라 det 입력 정렬을 수정했고, 그 결과 `test3.png`처럼 일반적인 박스 분리 불일치는 크게 줄었다.
- 현재 남은 핵심은 두 갈래다.
  1. `test2.png`의 작은 심볼/짧은 문자열 crop에 대한 `rec` 정합성
  2. `test3.png`의 작은 아이콘류(`💯`) recognition 정합성

## 2026-04-17 추가 진행 상황 11

### `test2.png`의 small symbol 경로 재분석
- `test2.png`에서 남아 있던 `0`, `舌` 누락은 단순히 box를 못 찾는 문제가 아니었다.
- FFI trace 기준으로 현재 `db_postprocess`는 해당 후보 box를 이미 accept한다.
  - `0` 후보 polygon
  - `舌` 후보 polygon
- 그러나 FFI는 그 뒤 `rec`에서 blank로 탈락했다.
  - `0` 후보: `crop=112x51`, `text=''`
  - `舌` 후보: `crop=111x49`, `text=''`

### sidecar 내부 full pipeline 직접 추적
- 번들 exe 출력이나 저장된 crop 파일만으로는 충분하지 않아, sidecar Python 내부 `_OCRPipeline`을 직접 따라가 각 단계의 in-memory crop / cls / rec를 확인했다.
- `test2.png`에서 sidecar 내부 결과:
  - polygon `[[14,19],[124,19],[124,67],[14,67]]`
    - crop `109x47`
    - `cls=0_degree`
    - batch `rec="听0"`, `score≈0.19`
  - polygon `[[15,506],[125,506],[125,554],[15,554]]`
    - crop `109x48`
    - `cls=0_degree`
    - batch `rec="舌"`, `score≈0.406`
- 즉 sidecar full pipeline 안에서는 해당 small symbol이 실제로 살아 있다.

### 저장 파일 재입력과 full pipeline 내부 경로는 다르다
- sidecar internal crop를 PNG/BMP로 저장한 뒤 `text_rec_model.predict(file)`로 다시 넣으면 blank가 나온다.
- FFI가 저장한 `rec_input_*.bmp`를 sidecar `text_rec_model`에 직접 넣어도 blank였다.
- 따라서 이전에 사용하던 “저장된 crop 파일 재입력” 비교는 full pipeline 내부 in-memory 동작을 정확히 대변하지 못한다.
- 남은 차이는 저장 파일 기준이 아니라, sidecar 내부 crop 배열과 FFI 내부 crop 배열/텐서를 직접 비교해야만 좁혀진다.

### 결정적인 발견: sidecar `rec`는 batch/single 실행 결과가 다르다
- sidecar `TextRecPredictor`를 직접 검증한 결과, 같은 crop라도:
  - full pipeline처럼 batch로 넣으면 non-empty text가 복원되고
  - 같은 crop를 single item으로 `text_rec_model`에 넣으면 blank가 된다.
- 실제 확인:
  - `0` box crop: batch `听0`, single `''`
  - `舌0` box crop: batch `舌0`, single `''`
  - `舌` box crop: batch `舌`, single `''`
- 원인:
  - sidecar `TextRecPredictor.process()`는 `ToBatch`에서 배치 내 최대 폭까지 zero-pad 후 predictor를 실행한다.
  - 즉 sidecar의 실제 `rec` 동작에는 **batch width padding semantics**가 포함되어 있다.

### naive `rec batch` 이식 시도와 revert
- 이 차이를 따라가기 위해 `bridge.cc`에 crop별 단건 `run_rec()` 대신 detection batch 전체를 한 번에 묶는 `run_rec_batch()`를 시험 구현했다.
- 결과는 regression이었다.
  - `test2.png`
    - blank였던 `0`, `舌`가 비blank가 되기는 했지만
    - `舌工0`, `舌叶`, `吾叫人飞`, `智个召` 같은 오인식이 크게 늘었다.
  - `test3.png`
    - 기존 `27`건에서 `26`건으로 후퇴
    - `💯`가 다시 탈락
- 따라서 이 구현은 즉시 되돌렸고, 현재 브랜치는 다시 baseline 상태다.

### 현재 결론 업데이트
- 남은 차이의 한 축은 확실히 `rec batch semantics`다.
- 하지만 “모든 detection을 한 배치로 그대로 묶는 것”은 sidecar 동작 복제가 아니다.
- sidecar 쪽은 다음이 함께 작동한다.
  1. width sorting
  2. batch sampler / chunking
  3. batch max width zero-padding
  4. predictor 실행
- 따라서 다음 단계는 naive batch가 아니라, sidecar의 실제 batch 구성 규약을 그대로 이식하는 것이다.

## 2026-04-17 추가 진행 상황 12

### `test.png` 회귀 원인 복구
- `rec batch semantics`를 다시 파기 전에 현재 baseline을 재확인하는 과정에서, `test.png`가 다시 크게 깨진 상태를 발견했다.
- 원인은 전처리나 `DBPostProcess`가 아니라 **잘못된 `rec` 모델 선택**이었다.
- FFI 생성 로그 기준 회귀 시점 상태:
  - `source_hint=en`
  - `rec=en_PP-OCRv5_mobile_rec`
- 즉 중국어 입력인데 영어 `rec` 모델을 잡고 있었고, 이 때문에 `test.png`가 `) T`, `#`, `"` 같은 출력으로 무너졌다.

### 수정
- 기존 구현은 `BUZHIDAO_PADDLE_FFI_SOURCE` 환경변수에 의존해 source/lang hint를 bridge로 전달했다.
- 이 경로가 생성 시점에 안정적으로 반영되지 않아 잘못된 모델 선택이 발생했다.
- 수정 내용:
  - `bridge.h`
    - `buzhi_ocr_create(model_dir, use_gpu, source, err)`로 시그니처 확장
  - `bridge.cc`
    - `source`를 직접 받아 `selected_lang` 계산 후 det/cls/rec 모델 선택에 사용
  - `app/src/ocr/paddle_ffi.rs`
    - Rust FFI 호출 시 `state.source`를 `CString`으로 직접 전달
- 결과:
  - `source_hint=ch`
  - `rec=PP-OCRv5_server_rec`
  - `test.png`는 다시 sidecar와 같은 `7`건 정상 상태로 복구

### 현재 재검증 상태
- `test.png`
  - sidecar와 다시 정상 정합
- `test3.png`
  - sidecar `27`
  - FFI `27`
  - 남은 대표 차이: `💯` vs `🙌`
- `test2.png`
  - sidecar `14`
  - FFI `12`
  - 남은 대표 차이:
    - `0` 누락
    - `舌` 누락
    - `中國語 []》` vs `中國語 [] `
    - `Ⅲ川人...` 앞부분 누락
    - `从中文词典...` 첫 글자 누락

### sidecar internal crop 재확인
- sidecar Python 내부 `_OCRPipeline`을 다시 추적해 `test2.png`의 small symbol 후보를 직접 확인했다.
- sidecar internal `rotated -> text_rec_model(batch)` 결과:
  - index `0`: `听0`, crop `109x47`
  - index `5`: `舌0`, crop `109x48`
  - index `10`: `舌`, crop `109x48`
- 같은 internal crop를 파일로 저장한 뒤 FFI에 단건 입력하면:
  - `det_boxes=1`
  - `rec`는 여전히 blank
- 즉 이 케이스는 다시 확인해도 `crop geometry`보다 **batch 실행 여부**가 더 큰 차이 축이다.

### sidecar 실제 rec batch 경계 확인
- `text_rec_model.batch_sampler.batch_size = 6`
- `test2.png`의 실제 `rotated` 순서는 그대로 다음과 같이 chunking된다.
  - batch 1: indices `0..5`
  - batch 2: indices `6..11`
  - batch 3: indices `12..17`
  - batch 4: index `18`
- 따라서:
  - `0`은 batch 1 안에서
  - `舌`은 batch 2 안에서
  각각 큰 폭 문자열들과 함께 zero-pad된 상태로 추론된다.

### 현재 가장 유력한 결론
- 남은 `test2` 차이는 여전히 `rec batch semantics` 축이다.
- 다만 이전에 넣었다가 되돌린 naive `run_rec_batch()`는 sidecar와 같은 batch 경계 문제가 아니라,
  **`N>1`일 때 predictor output shape / memory layout 해석이 잘못되었을 가능성**이 크다.
- 이유:
  - 단건 `run_rec()` 경로는 정상적으로 동작
  - naive batch를 넣자마자 `test.png`까지 광범위하게 regression
  - 같은 crop를 single-item path로 넣으면 sidecar도 blank가 되는 패턴과 일치

### 현재 기준 다음 작업
1. sidecar Python에서 실제 `6`개 batch `infer(x=...)`의 output shape를 직접 덤프
2. FFI batched predictor output shape와 memory layout을 직접 대조
3. 그 layout이 맞는지 확인한 뒤에만 `run_rec_batch()`를 다시 이식

즉 현재 단계의 핵심은 batch를 다시 “넣는 것”이 아니라, **batched predictor output layout을 정확히 맞추는 것**이다.

## 2026-04-17 추가 진행 상황 13

### sidecar `TextRecPredictor.process()` 실제 순서 확인
- sidecar `paddlex.inference.models.text_recognition.predictor.TextRecPredictor` 구현을 직접 대조했다.
- 중요한 실제 동작:
  1. `_crop_by_polys`로 sub-image 생성
  2. `textline_orientation` 적용
  3. 각 sub-image의 `width / height` 비율 계산
  4. `sub_img_ratio` 오름차순으로 정렬
  5. 정렬된 순서로 `text_rec_model(sorted_subs_of_img)` 실행
  6. 결과만 원래 box 순서로 복원
- 즉 이전 결론을 수정해야 한다.
  - sidecar는 `ImageBatchSampler`에서 정렬하지 않지만,
  - OCR pipeline 레벨에서 **crop ratio sorting**을 실제로 수행한다.

### FFI에 ratio sort + `6`개 chunk `rec batch` 반영
- `bridge.cc`에 다음을 반영했다.
  - `cls/rotate` 이후 crop별 `width / height` ratio 계산
  - ratio 오름차순 정렬
  - `6`개 chunk 단위 `run_rec_batch()`
  - 결과를 원래 detection 순서로 복원
- 이 수정은 특정 이미지 heuristic이 아니라 sidecar pipeline 규약 직접 복제다.

### 결과
- `test.png`
  - sidecar와 동일 유지
- `test2.png`
  - 기존 `12`건 -> `14`건
  - 복구된 항목:
    - `舌`
    - `个`
  - 아직 남은 차이:
    - `0` 누락
    - `中國語 []》` vs `中國語 [] `
    - `Ⅲ川人 中國語word processor` vs ` 川 中國語word processor`
    - `从中文词典。中文辞典。` vs `中文词典。中文辞典。`
- `test3.png`
  - count 유지
  - 남은 대표 차이: `💯` vs `🙌`

### 의미
- `rec batch semantics`는 실제 원인 축이 맞았고, 이번 수정은 그 일반 규약을 sidecar와 동기화한 것이다.
- 하지만 이것만으로 아직 완전 동일 결과에는 도달하지 못했다.

## 2026-04-17 추가 진행 상황 14

### sidecar crop operator 직접 대조
- sidecar `_OCRPipeline`은 `CropByPolys(det_box_type=\"quad\")`를 사용한다.
- 실제 crop 구현은 `crop_image_regions.py` 기준으로 다음 순서다.
  1. `cv2.minAreaRect(np.array(points).astype(np.int32))`
  2. `cv2.boxPoints(...)`
  3. 좌/우 점 정렬로 quad 재구성
  4. `cv2.getPerspectiveTransform`
  5. `cv2.warpPerspective(..., borderMode=cv2.BORDER_REPLICATE, flags=cv2.INTER_CUBIC)`
  6. `h / w >= 1.5`면 `np.rot90`

### `crop_to_bbox()`에 sidecar crop 규약 직접 이식 시도
- FFI `crop_to_bbox()`에서 raw quad 대신
  `minAreaRect(np.int32(points)) -> boxPoints -> warp`
  경로로 강제 변환하는 시도를 했다.
- 결과는 regression이었다.
  - `test.png`: `小听 -> 听`
  - `test3.png`: `💯` 탈락
- 따라서 이 시도는 즉시 revert했다.

### 결론
- sidecar의 crop 구현을 겉으로 그대로 옮겨도 현재 FFI baseline보다 좋아지지 않았다.
- 즉 남은 차이는 단순히 `crop_to_bbox` 한 군데만 바꾸는 것으로는 해결되지 않는다.

## 2026-04-17 추가 진행 상황 15

### 현재 stable baseline
- 유지 중인 변경:
  - `DetResizeForTest` round-to-32
  - `preprocess_rec` zero padding
  - `preprocess_rec` stride fix
  - explicit source/lang 전달
  - ratio sort + `6`개 chunk `rec batch`
- 되돌린 변경:
  - naive all-in-one `rec batch`
  - `crop_to_bbox`의 `minAreaRect(np.int32(points))` 강제 경로

### 현재 상태 요약
- `test.png`
  - sidecar와 동일
- `test2.png`
  - sidecar `14`
  - FFI `14`까지는 복구했지만 text fidelity는 아직 다름
  - 남은 핵심:
    - `0`
    - `》`
    - `Ⅲ`
    - `从`
- `test3.png`
  - count 유지
  - 남은 대표 차이: `💯` vs `🙌`

### 다음 타깃
- 이제 남은 차이는 box count보다 특정 token의 raw logits 차이다.
- 다음 단계는:
  1. `0`, `》`, `Ⅲ`, `从`, `💯` 후보의 sidecar raw top-k logits 덤프
  2. 같은 후보의 FFI raw top-k logits 덤프
  3. timestep별 divergence 지점 확인

즉 현재는 더 이상 crop 규약을 감으로 조정하는 단계가 아니라, **동일 후보 token의 rec logits를 sidecar/FFI에서 직접 비교하는 단계**다.

## 2026-04-17 추가 진행 상황 16

### sidecar internal det/crop dump 추가
- sidecar `ocr_server.py`에 추가 계측을 넣어 다음 두 dump를 모두 남길 수 있게 했다.
  - `BUZHIDAO_PADDLE_SIDECAR_DUMP_REC_LOGITS=1`
    - `sidecar_rec_batch_logits_*.json`
    - 각 rec batch에 대해 `original_index`, `ratio`, `polygon`, `image_width/height`, `rec_width`, `input_values`, `values`를 기록
  - `BUZHIDAO_PADDLE_SIDECAR_DUMP_DET=1`
    - `sidecar_det_candidates_*.json`
    - sidecar OCR pipeline이 실제로 rec에 넘기는 sorted `dt_polys` 기준으로
      `order_index`, `original_index`, `ratio`, `crop_width`, `crop_height`,
      `textline_orientation_angle`를 기록
- 중요한 점:
  - `TextRecPredictor.batch_sampler.batch_size = 6`이라 sidecar rec dump 메타도 실제 batch chunk 단위로 큐잉해야 정합이 맞는다.
  - 이 수정 후 `test2.png`, `test3.png`의 모든 rec batch item에 polygon 메타가 정상 매핑되는 것을 확인했다.

### sidecar 실제 textline orientation 규약 재확인
- sidecar `_OCRPipeline`은 `textline_orientation_model(...)` 결과에서 `class_ids[0]`만 보고 바로 회전한다.
- 즉 score threshold gate는 없다.
- FFI에서 사용 중이던 `cls score >= 0.9` 조건은 sidecar와 다르므로 제거했다.
- 이 변경 후:
  - `test3.png`는 다시 `27`건 유지
  - 하지만 `💯 -> 🙌` mismatch는 그대로 남음

### sidecar vs FFI det/crop 직접 비교 결과
- 이번 턴에서 처음으로 sidecar `dt_polys -> crop`와 FFI `db_postprocess -> crop`를 같은 후보 기준으로 직접 맞댔다.
- 대표 비교:
  - `test3.png` 아이콘 후보
    - sidecar: polygon `[[472,569],[506,569],[506,595],[472,595]]`, crop `34x26`, `textline_orientation_angle=1`
    - FFI: polygon `[[471.5,568.4],[508.0,568.4],[508.0,597.2],[471.5,597.2]]`, crop `36x28`
  - `test3.png` 숫자 `5`
    - sidecar: `25x32`
    - FFI: `26x33`
  - `test2.png` `0`
    - sidecar: `109x47`
    - FFI: `112x51`
  - `test2.png` `中國語 []》`
    - sidecar: `288x39`
    - FFI: `289x42`
  - `test2.png` `Ⅲ川人...`
    - sidecar: `613x47`
    - FFI: `613x48`
  - `test2.png` `从中文词典...`
    - sidecar: `350x36`
    - FFI: `352x38`
- 결론:
  - 남은 mismatch는 rec decode 자체보다, **FFI detector polygon/crop가 sidecar보다 일관되게 약간 큰 것**과 더 강하게 연결된다.

### rec logits 비교 결과 재확인
- `test3` 아이콘 후보는 sidecar/FFI rec dump를 직접 비교해도 다음이 유지된다.
  - input tensor mean abs diff: 약 `0.096`
  - sidecar는 해당 timestep에서 실제 문자 class가 올라오고
  - FFI는 blank 쪽으로 기운다
- 즉 `💯 -> 🙌` 문제도 현재는 rec 모델 자체보다는, **rec 입력 crop 차이**를 먼저 설명하는 편이 타당하다.

### `DBPostProcess` 구조 비교로 좁혀진 차이
- sidecar `DBPostProcess` 실제 구현을 다시 직접 대조했다.
  - contour 추출: `cv2.findContours(..., RETR_LIST, CHAIN_APPROX_SIMPLE)`
  - quad 생성: contour -> `get_mini_boxes(contour)`
  - score: `box_score_fast(pred, points)` = bbox mask 평균
  - unclip 후 다시 `get_mini_boxes(box)`
- FFI 기존 구현은 다음 점에서 달랐다.
  - connected-component cell center까지 포함한 `shape_points -> convex_hull -> min_area_rect`
  - `score_box()`도 quad 내부 샘플 평균
- 이번 턴에는 먼저 contour 기반으로만 `min_area_rect`를 계산하도록 바꿨다.
- 결과:
  - `test3.png`: 여전히 `27`건, 하지만 `💯 -> 🙌`는 그대로
  - `test2.png`: 여전히 `14`건, text fidelity mismatch도 그대로
- 즉 `shape_points/hull` 제거만으로는 마지막 차이가 해소되지 않았다.

### 시도했다가 되돌린 변경
- `DBPostProcess` 최종 scaled 좌표를 sidecar처럼 `round()`로 정수화하는 시도를 했다.
- 결과:
  - `test2.png`는 일부 box가 sidecar 좌표에 더 가까워졌지만
  - `test3.png`가 `27 -> 26`으로 회귀
- 따라서 이 변경은 즉시 되돌렸다.

### 현재 stable baseline
- 유지 중인 변경:
  - explicit source/lang 전달
  - rec ratio sort + `6`개 chunk `run_rec_batch()`
  - sidecar rec batch dump
  - sidecar det candidate dump
  - sidecar와 같은 textline orientation 회전 규약(`class_ids[0]` 즉시 회전)
  - FFI `db_postprocess()`의 contour-only `min_area_rect`
- 되돌린 변경:
  - `crop_to_bbox()`의 sidecar식 `minAreaRect(np.int32(points))` 강제 경로
  - `DBPostProcess` 최종 box 좌표 `round()` 정수화

### 현재 결론 업데이트
- 남은 차이는 계속해서 다음 두 축으로 수렴한다.
  1. sidecar `DBPostProcess`의 contour/score/unclip 세부 규약
  2. 그 결과 생기는 rec crop 크기 차이
- 이번 턴까지의 근거상, `💯`, `0`, `》`, `Ⅲ`, `从` mismatch를 설명하는 1차 원인은
  **FFI box geometry가 sidecar보다 약간 크게 나오는 것**이다.
- 다음 작업 우선순위는 raw logits 추가 덤프보다,
  sidecar `box_score_fast`와 contour extraction 규약을 더 직접적으로 맞추는 것이다.

## 2026-04-17 추가 진행 상황 17

### contour / score 규약 직접 치환 시도
- sidecar 구현을 더 직접적으로 따라가기 위해 두 실험을 했다.
  - component edge loop 대신, connected-component에서 **경계 픽셀 좌표만 추출해 convex hull 후 `min_area_rect`**
  - `score_box()`를 기존의 quad 내부 중심점 평균 대신, sidecar `box_score_fast`를 흉내 낸 **bbox-mask 평균** 방식으로 변경
- 목적은 두 가지였다.
  - `trace_component_contour()`의 half-pixel 바깥 경계 효과를 줄여 box를 더 작게 만들기
  - score 계산도 sidecar의 `cv2.fillPoly(mask, int32(box))` 경로에 가깝게 맞추기

### 결과: 둘 다 회귀
- 경계 픽셀 기반 contour 치환 결과:
  - `test3.png`가 `27 -> 26`으로 회귀
  - `💯` 후보 자체가 사라져 stable baseline보다 나빠짐
  - `test2.png`도 기존 mismatch를 줄이기는커녕 일부 텍스트가 더 깨짐
- contour 치환을 되돌리고 `score_box_fast`식 score만 단독으로 남겨 다시 확인한 결과:
  - `test3.png`는 여전히 `26`건
  - 즉 현재 FFI contour geometry와 조합될 때는 score 규약만 sidecar처럼 바꿔도 reject/merge 경계가 달라져 회귀가 발생함

### 이번 턴 결론
- 현재 FFI의 contour 좌표계와 score 규약은 **같이 바뀌어야** 할 가능성이 높다.
- `box_score_fast`만 먼저 맞추는 방식도 안전하지 않았고,
  경계 픽셀 contour로 단순 치환하는 방식도 sidecar `findContours`의 실제 동작을 충분히 재현하지 못했다.
- 따라서 이번 두 시도는 모두 되돌렸다.

### 현재 코드 상태 재확인
- 유지:
  - sidecar와 같은 textline orientation 회전 규약
  - rec ratio sort + batch size `6`
  - sidecar rec/det dump 계측
  - contour-only `min_area_rect`
- 되돌림:
  - 경계 픽셀 기반 contour 추출
  - sidecar식 bbox-mask 평균 `score_box_fast` 흉내 구현

### 다음 우선순위 업데이트
- 다음은 `score_box_fast` 단독 치환이 아니라,
  **sidecar `findContours(..., CHAIN_APPROX_SIMPLE)`와 더 가까운 contour point set 자체를 먼저 재현**하는 쪽이 맞다.
- 즉 다음 작업 단위는:
  1. FFI component -> contour point set 생성 규약을 sidecar와 더 직접적으로 맞추기
  2. 그 contour 기준으로 score/unclip을 다시 맞추기
  3. 이후에만 crop 크기(`34x26` vs `36x28`, `109x47` vs `112x51`) 재측정하기

## 2026-04-17 추가 진행 상황 18

### ordered boundary tracing 시도
- `trace_component_contour()`를 기존 edge-loop 대신,
  boundary pixel center를 따라 도는 ordered contour tracer로 바꿔봤다.
- 의도:
  - sidecar `findContours(..., CHAIN_APPROX_SIMPLE)`처럼
    점들이 셀 외곽 corner가 아니라 **경계 픽셀 중심 쪽**에 가깝게 모이게 만들기
  - 현재 edge-loop가 만드는 half-pixel outward bias를 줄이기
- 구현 방식:
  - connected-component mask에서 boundary cell을 찾고
  - Moore-neighborhood 스타일의 8방향 추적으로 contour를 만들고
  - 실패 시에만 기존 edge-loop fallback

### 결과: 여전히 회귀
- `cargo test -p buzhidao --features paddle-ffi --no-run`는 통과했지만,
  실제 `test3.png` 샘플 OCR에서는 다시 회귀했다.
- 관측 결과:
  - `test3.png`: `27 -> 26`
  - `💯` 아이콘 후보 box가 사라짐
  - 일부 긴 라인 box는 약간 작아졌지만, 전체적으로는 stable baseline보다 나빠짐
- 즉 단순한 ordered boundary tracer만으로는
  sidecar `findContours`의 contour point distribution을 충분히 재현하지 못했다.

### 이번 턴 결론
- sidecar 차이를 줄이려면 단순히 contour를 "정렬해서 뽑는 것"만으로는 부족하다.
- 실제 차이는 아마 다음까지 같이 묶여 있다.
  - contour sampling density / starting rule
  - diagonal connection 처리
  - `CHAIN_APPROX_SIMPLE` 축약 규약
  - 그 contour 위에서의 `minAreaRect` orientation 선택
- 따라서 ordered boundary tracing 시도도 되돌리고 stable baseline으로 복구했다.

## 2026-04-17 추가 진행 상황 19

### sidecar / FFI raw contour dump 추가
- 감으로 contour를 바꾸는 대신, 이제 양쪽이 실제로 어떤 point set을 쓰는지 직접 비교할 수 있게 dump를 추가했다.
- sidecar:
  - `BUZHIDAO_PADDLE_SIDECAR_DUMP_DET=1`
  - 기존 `sidecar_det_candidates_*.json` 외에 `sidecar_det_contours_*.json` 추가
  - 각 contour candidate에 대해
    - raw `contour`
    - `mini_box`
    - `mini_side`
    - `score`
    - `unclipped_box`
    - `unclipped_side`
    - `scaled_box`
    - `accepted`, `reject_reason`
    를 기록
- FFI:
  - `BUZHIDAO_PADDLE_FFI_DUMP_DET=1`
  - `ffi_det_contours_*.json` 추가
  - 각 candidate에 대해
    - `component_size`
    - raw `contour`
    - `rect`
    - `unclipped`
    - `scaled`
    - `score`
    - `accepted`, `reject_reason`
    를 기록

### `test3` 아이콘 후보 raw contour 비교
- sidecar 아이콘 후보:
  - contour point 수: `25`
  - `mini_box`: `[[480,571],[502,571],[502,585],[480,585]]`
  - `mini_side`: `14`
  - `unclipped_box`: `[[474,565],[508,565],[508,591],[474,591]]`
  - `scaled_box`: `[[472,569],[506,569],[506,595],[472,595]]`
  - `score`: `0.69207`
- FFI 아이콘 후보:
  - contour point 수: `28`
  - `rect`: `[[480,571],[503,571],[503,586],[480,586]]`
  - `unclipped`: `[[473.19,564.19],[509.81,564.19],[509.81,592.81],[473.19,592.81]]`
  - `scaled`: `[[471.52,568.37],[508.01,568.37],[508.01,597.20],[471.52,597.20]]`
  - `score`: `0.71758`

### 이번 비교로 확정된 점
- 차이는 이제 더 분명하다.
  - sidecar contour는 FFI보다 point 수가 적고
  - `mini_box` 단계에서 이미 우하단으로 정확히 `+1px`
  - `unclip` 단계에서는 양쪽으로 약 `+0.81px` 더 퍼진다
- 즉 현재 남은 차이의 핵심은 단순 score 규약이 아니라,
  **FFI contour가 sidecar보다 한 픽셀 정도 더 큰 사각형을 만들도록 sampling되고 있다는 점**이다.

### 다음 우선순위 업데이트
- 다음은 `score_box_fast` 치환이 아니라,
  FFI `trace_component_contour()`가 만드는 point set을
  sidecar `findContours(..., CHAIN_APPROX_SIMPLE)`의 출력과 더 비슷하게 줄이는 작업이다.
- 특히 다음을 직접 겨냥해야 한다.
  1. 우하단 edge가 `+1px` 커지는 원인 제거
  2. contour point 수를 sidecar 쪽(`25`)에 더 가깝게 줄이기
  3. 그 뒤 icon crop가 `36x28 -> 34x26`으로 줄어드는지 재측정하기

## 2026-04-17 추가 진행 상황 20

### contour 좌표계를 pixel grid 쪽으로 정규화
- raw contour dump 비교를 바탕으로,
  `trace_component_contour()`가 만드는 edge-loop 좌표가
  component의 오른쪽/아래쪽에서 `max_x + 1`, `max_y + 1` 바깥으로 나가고 있다는 점을 직접 보정했다.
- 구현:
  - contour 생성 뒤 `normalize_contour_to_pixel_grid()`를 추가
  - component bbox의 `max_x + 1`, `max_y + 1`에 걸린 점들을 각각 `max_x`, `max_y`로 당긴 뒤
    다시 `simplify_contour()` 적용
- 의도:
  - OpenCV `findContours()`가 pixel index 기준으로 내는 contour와 더 가까운 좌표계로 맞추기
  - 특히 `test3` 아이콘 후보의 우하단 `+1px`를 먼저 제거하기

### `test3` 아이콘 후보 개선
- 변경 후 FFI icon candidate는 다음까지 줄었다.
  - contour point 수: `28 -> 20`
  - `rect`: `[[480,571],[502,571],[502,585],[480,585]]`
  - `unclipped`: `[[473.58,564.58],[508.42,564.58],[508.42,591.42],[473.58,591.42]]`
  - `scaled`: `[[471.91,568.76],[506.62,568.76],[506.62,595.79],[471.91,595.79]]`
- 즉 `mini_box` 단계에서는 sidecar와 동일해졌다.
- rec batch dump 기준 crop도:
  - sidecar: `34x26`
  - FFI: `34x27`
  로 줄어서, 기존 `36x28` 대비 한 단계 더 근접했다.

### 함께 해본 정수화 실험은 되돌림
- sidecar dump를 보고 `scaled_box`의 `round()` 규약까지 같이 맞춰보려 했지만,
  현재 FFI `unclip`은 아직 float 오차가 남아 있어서 단독 적용 시 회귀했다.
- 시도한 것:
  - final scaled 좌표 `round()`
  - `unclipped_mini.first`를 먼저 정수 격자에 붙인 뒤 scaled `round()`
- 결과:
  - `test3` 아이콘이 `💯`로 가지 않고 `一` 또는 blank 쪽으로 흔들렸고
  - 전체 box 좌표도 sidecar보다 과하게 정수화되어 안정성이 떨어졌다.
- 따라서 이번 턴에서는 **contour 좌표계 정규화만 유지**하고,
  scaled/unclip 정수화 시도는 모두 되돌렸다.

### 이번 턴 결론
- 남은 차이는 여전히 `unclip` 단계에 있다.
- 하지만 차이의 축은 훨씬 좁아졌다.
  - 해결됨: contour point set / `mini_box`의 우하단 `+1px`
  - 남음: `unclip` 후 약 `0.4 ~ 0.8px` 바깥으로 퍼지는 차이
- 즉 다음 우선순위는 `trace_component_contour()`보다
  `unclip()` 구현을 sidecar `pyclipper -> get_mini_boxes` 경로에 더 가깝게 바꾸는 쪽이다.

## 2026-04-17 추가 진행 상황 21

### sidecar `pyclipper` 출력 직접 재현
- `ocr_server/.venv`의 `pyclipper`로 icon 후보 `mini_box=[[480,571],[502,571],[502,585],[480,585]]`를 직접 넣어봤다.
- `distance = area * 1.5 / perimeter = 6.416666...` 기준 실제 출력은 다음 `12`점 polygon이다.
  - `[[505,566],[508,568],[508,585],[507,588],[505,591],[480,591],[477,590],[474,588],[474,571],[475,568],[477,565],[502,565]]`
- 이 point set의 `min/max`가 sidecar dump의 `unclipped_box=[[474,565],[508,565],[508,591],[474,591]]`와 정확히 맞는다.
- 즉 sidecar 차이는 단순히 `expand_rect()`의 float 결과를 round하는 문제가 아니라,
  **`pyclipper`가 만드는 rounded-corner 정수 polygon 전체**에 있다.

### bbox snap류 근사는 모두 회귀
- 두 가지 실험을 해봤지만 둘 다 stable baseline보다 나빴다.
  1. `unclipped_mini.first` 전체를 정수 격자에 round
  2. axis-aligned + small box에만 `unclip` polygon의 bbox를 정수 격자로 snap
- 관측된 회귀:
  - `test3.png` 아이콘은 `💯`로 가지 못했고
  - 하단 숫자 라인도 `9 -> 6`처럼 다른 후보가 흔들렸다
- 즉 sidecar `pyclipper`를 bbox 수준으로만 흉내 내면,
  다른 small box들의 `minAreaRect` / crop가 더 나빠진다.
- 따라서 이번 두 시도는 모두 되돌렸다.

### 현재 결론 업데이트
- 유지:
  - contour pixel-grid 정규화
  - `mini_box` 단계 sidecar 정합성 개선
- 미해결:
  - `unclip` 단계는 여전히 sidecar `pyclipper` 구현과 다르다
- 다음으로 유효한 작업 단위는 heuristics가 아니라,
  **실제 polygon offset(clipper 계열) 구현을 FFI에 넣는 것**이다.

## 2026-04-17 추가 진행 상황 22

### native Clipper2 offset으로 `unclip` exact match
- `build.rs`에서 cargo registry 아래 `clipper2-sys-1.0.0/Clipper2/CPP/Clipper2Lib` C++ 소스를 직접 찾아 같이 컴파일하도록 붙였다.
- `bridge.cc`의 `unclip()`도 기존 `expand_rect()` 근사 대신
  `Clipper2Lib::InflatePaths(..., JoinType::Round, EndType::Polygon)`를 사용하도록 바꿨다.
- 결과:
  - `test3` 아이콘 후보의 `unclipped`가 sidecar와 완전히 같아졌다.
  - sidecar / FFI 공통:
    - `mini_box=[[480,571],[502,571],[502,585],[480,585]]`
    - `unclipped_box=[[474,565],[508,565],[508,591],[474,591]]`

### scaled box / crop도 sidecar와 일치
- final scaled 좌표를 `round()` 정수화한 상태에서 `test3` 아이콘 후보는 다음까지 맞았다.
  - `scaled_box=[[472,569],[506,569],[506,595],[472,595]]`
  - rec crop `34x26`
- 즉 이번 턴으로 `DBPostProcess` 계층의 차이는 사실상 정리됐다.
- 남은 차이는 detector geometry가 아니라 그 뒤 단계다.

### 그런데도 rec 입력은 여전히 다름
- 같은 icon 후보를 rec logits dump로 다시 비교하면:
  - sidecar:
    - polygon `[[472,569],[506,569],[506,595],[472,595]]`
    - crop `34x26`
    - `input_mean ~= 0.07218648`
    - `values_max ~= 0.9977527`
  - FFI:
    - polygon `[[472,569],[506,569],[506,595],[472,595]]`
    - crop `34x26`
    - `input_mean ~= 0.09078552`
    - `values_max ~= 0.9859711`
- 즉 **같은 polygon / 같은 crop size인데 rec 입력 분포가 이미 다르다.**
- 이 시점에서 `💯 -> blank/누락`은 더 이상 `DBPostProcess` 문제로 설명할 수 없다.

### crop_to_bbox sidecar화 실험은 효과 없음
- `crop_to_bbox()`를 sidecar `warpPerspective` 수학에 더 가깝게 맞추는 실험도 해봤다.
  - axis-aligned fast path 제거
  - destination quad를 `(0,0)-(w,h)` 규약으로 변경
  - `INTER_CUBIC` 근사용 bicubic sampler 추가
- 하지만 icon 후보 `input_mean`은 그대로 `0.09078552`였고, `test3` 출력도 그대로 `23`건이었다.
- 따라서 이 실험들은 모두 되돌렸다.

### 이번 턴 결론
- 이번 턴으로 확정된 것은 다음이다.
  1. `DBPostProcess`의 contour / unclip / scaled box 차이는 핵심 병목이 아니다.
  2. 현재 남은 차이는 **crop 이후 rec 입력 생성 과정** 또는 그보다 앞선 **원본 이미지 decode 차이**다.
- 특히 sidecar는 `cv2.imread` 기반이고, FFI는 Windows에서 `GDI+`로 PNG를 읽는다.
- 따라서 다음 우선순위는 다음 둘 중 하나다.
  - `load_bitmap_with_gdiplus()`가 sidecar와 다른 픽셀을 만들고 있는지 직접 덤프 비교
  - FFI crop / resize 직후 raw image tensor를 sidecar와 같은 후보 기준으로 직접 맞대기

## 2026-04-17 추가 진행 상황 23

### rec dump에 raw crop 바이트 추가
- sidecar `sidecar_rec_batch_logits_*.json`과 FFI `rec_batch_logits_*.json`에 다음 필드를 추가했다.
  - `raw_channel_means`
  - `raw_pixels`
- 목적은 detector / polygon / crop size가 같을 때도 실제 rec에 들어가는 crop 이미지가 같은지 바로 확인하는 것이다.

### 같은 icon polygon인데 raw crop가 전혀 다름
- `test3` 아이콘 후보를 다시 비교했다.
- 공통점:
  - polygon `[[472,569],[506,569],[506,595],[472,595]]`
  - crop size `34x26`
- 하지만 raw crop는 다음처럼 다르다.
  - sidecar:
    - `raw_channel_means ~= [232.2952, 153.3767, 137.0633]`
    - `raw_pixels[:64]`가 전부 `0`
  - FFI:
    - `raw_channel_means ~= [248.9400, 164.0498, 145.8631, 255]`
    - `raw_pixels[:64]`는 바로 밝은 배경(`254,247,246,...`)에서 시작
- 즉 현재 남은 차이는 단순 normalize/resize가 아니라,
  **crop 이미지 내용 자체가 sidecar와 다르다.**

### 이번 비교가 의미하는 것
- `DBPostProcess`는 이미 정리됐다.
- 같은 quad를 rec에 넘겨도 sidecar와 FFI가 만드는 crop 이미지가 다르므로,
  현재 병목은 다음 둘 중 하나로 더 좁혀진다.
  1. `crop_to_bbox()`가 sidecar `get_minarea_rect_crop -> getPerspectiveTransform -> warpPerspective`와 아직 다르다.
  2. 그보다 앞단에서 Windows 이미지 decode(`cv2.imread` vs `GDI+`)가 이미 다르다.

### 다음 우선순위
- 다음은 detector가 아니라 crop 경로를 직접 맞추는 일이다.
- 구체적으로는:
  - sidecar `get_minarea_rect_crop(np.int32(points))`를 FFI에 더 직접 복제해 raw crop 바이트가 맞는지 확인
  - 그래도 다르면 `load_bitmap_with_gdiplus()`와 `cv2.imread()`의 PNG decode 차이를 원본 이미지 기준으로 직접 비교

## 2026-04-17 추가 진행 상황 24

### raw crop 차이는 sidecar dump 해석 문제였다
- `test3` 아이콘 후보에 대해 PaddleX 실제 crop 경로를 Python에서 직접 다시 돌려봤다.
  - `CropByPolys().get_minarea_rect_crop(img, poly)`
  - 이후 textline orientation 결과에 맞춰 `np.rot90(crop, 2)`
- 결과:
  - 이 direct PaddleX crop+rotate의 raw pixels가 FFI `raw_pixels`와 **완전히 일치**했다.
  - channel별 diff:
    - `B mad = 0`
    - `G mad = 0`
    - `R mad = 0`
- 즉 이전에 sidecar dump에서 보였던 raw crop 차이는 실제 pipeline 차이가 아니라,
  우리가 넣어둔 sidecar dump 계측의 mapping/해석 문제였다.

### PNG decode 차이도 아니었다
- 같은 `test3.png`를 Python `cv2.imwrite()`로 BMP로 저장한 뒤,
  FFI를 그 BMP에 대해서 다시 돌려봤다.
- BMP에서도 icon 후보 crop / OCR 결과가 PNG와 동일했다.
- 추가로 회전까지 같은 기준으로 맞춰 direct PaddleX crop와 비교하면 BMP에서도 pixel diff는 `0`이었다.
- 따라서 현재 시점에서:
  - `cv2.imread` vs `GDI+`
  - PNG decode vs BMP decode
  는 핵심 원인이 아니다.

### rec resize/normalize도 사실상 정합
- direct PaddleX crop+rotate에 대해
  `OCRReisizeNormImg(rec_image_shape=[3,48,320]).resize(...)`를 적용한 결과를
  FFI `input_values`와 직접 비교했다.
- 결과:
  - mean abs diff `~= 0.0001598`
  - max abs diff `~= 0.0078433`
  - `input_mean_py ~= 0.09062568`
  - `input_mean_ffi ~= 0.09078553`
- 즉 현재 FFI는 다음 경로까지 PaddleX와 거의 같다.
  1. crop
  2. rotate180
  3. rec resize / normalize / pad

### 이번 턴 결론
- 이제 남은 차이는 detector나 crop이 아니라, 거의 확실하게 **rec predictor inference / decode 이후**다.
- 다음 우선순위는 더 좁혀졌다.
  - 동일 crop tensor를 sidecar rec model에 직접 넣은 logits
  - 같은 tensor를 FFI predictor에 넣은 logits
  를 후보 단위로 1:1 비교하는 단계다.

## 2026-04-17 추가 진행 상황 25

### sidecar에 FFI rec dump 재주입 경로 추가
- `ocr_server.py`에 비교 전용 CLI를 추가했다.
  - `--compare-ffi-rec-dump <rec_batch_logits_*.json>`
  - `--compare-original-index <n>`
- 이 경로는 FFI dump의 `input_values`와 `rec_width`로 batch tensor를 다시 만들고,
  sidecar `text_rec_model.infer(x=[batch_input])`에 그대로 넣어 logits를 다시 뽑는다.
- 출력은 `sidecar_vs_ffi_<원본 dump 이름>.json`으로 저장되고, 각 item마다 다음을 남긴다.
  - `sidecar_text`, `ffi_text`
  - `sidecar_score`, `ffi_score`
  - `mean_abs_diff`, `max_abs_diff`

### test3 아이콘 후보 logits는 사실상 완전 일치
- `test3` icon 후보(`original_index=4`, polygon `[[472,569],[506,569],[506,595],[472,595]]`)를
  FFI batch dump에서 그대로 재주입해 비교했다.
- 결과:
  - `sidecar_text = ""`
  - `ffi_text = ""`
  - `mean_abs_diff ~= 1.1e-11`
  - `max_abs_diff ~= 4.17e-07`
- 수치상 오차는 float round 수준이고, decode 결과도 완전히 같았다.

### 같은 n=6 batch 전체도 0 diff
- 같은 `rec_batch_logits_1_n6_w320_ts40_cls18385.json` batch 전체를 비교했다.
- 결과:
  - 일반 문자 후보 `2`, `新`, `热`, `4`는 sidecar/FFI text와 score가 동일
  - blank 후보(icon, tiny reject 인접 후보)도 sidecar/FFI가 모두 동일
  - 전 item에서 `mean_abs_diff = 0`, `max_abs_diff = 0`
- 즉 **동일 rec tensor에 대한 sidecar rec predictor와 FFI predictor의 logits/decode는 동일하다.**

### 이번 턴 결론
- 이로써 다음 두 층은 사실상 닫혔다.
  1. crop -> rotate180 -> resize/normalize/pad
  2. rec predictor inference / decode
- 따라서 현재 남은 차이는 “같은 tensor를 넣었을 때”가 아니라,
  **실제 end-to-end pipeline에서 어떤 candidate가 rec까지 살아남고 어떤 순서/조합으로 batch가 구성되는지** 쪽으로 좁혀진다.
- 다음 우선순위는 다음 둘이다.
  - sidecar와 FFI의 최종 accepted/rejected candidate 집합이 어디서 갈라지는지 추적
  - 특히 `test3` icon 후보가 sidecar end-to-end 결과에서 실제로 rec 후 채택되는지, 또는 이미 det/score-thresh 단계에서 같이 탈락하는지 다시 확정

## 2026-04-17 추가 진행 상황 26

### sidecar는 end-to-end에서 icon을 실제로 채택한다
- `ocr_server.py --image app/testdata/ocr/test3.png --source ch --score-thresh 0.1`를
  debug 후보 전체 포함 상태로 다시 확인했다.
- sidecar는 icon polygon `[[472,569],[506,569],[506,595],[472,595]]`에 대해:
  - `text = "💯"`
  - `score = 0.2058834583`
  - `accepted = true`
- 즉 sidecar 기준으로는 이 후보가 실제 최종 결과에 살아 있다.

### FFI는 같은 polygon을 rec 이후 빈 문자열로 버린다
- FFI JSON `debug_detections`에도 rejected 후보를 포함하도록 수정했다.
- 같은 `test3` 실행에서 FFI는 최종 `debug_len=25`였고,
  icon 포함 2개 후보가 `text=""`, `score=0`, `accepted=false`로 떨어진다.
- 따라서 현재 mismatch는 “sidecar도 사실 blank였다”가 아니라,
  **실제 end-to-end에서 sidecar와 FFI가 서로 다른 rec 후보를 만들거나 다른 후보를 같은 polygon으로 보고 있다** 쪽이다.

### rec batch 집합도 이미 다르다
- sidecar rec dump를 다시 뽑아 batch 집합을 FFI rec dump와 직접 비교했다.
- `test3` 기준:
  - sidecar rec batch 후보 수: `27`
  - FFI rec batch 후보 수: `24`
- sidecar에만 있고 FFI rec batch에는 없는 대표 polygon:
  - `[[1488,741],[1610,741],[1610,777],[1488,777]]` (`换一换`)
  - `[[916,983],[1424,983],[1424,1013],[916,1013]]` (`雷军：最大的心理负担是不能说错话`)
  - `[[136,1055],[595,1055],[595,1086],[136,1086]]` (`AI来了消博会消费行业会怎么变`)
- 반대로 FFI에는 sidecar에 없는 tiny blank 후보
  - `[[1191,575],[1207,575],[1207,587],[1191,587]]`
  가 rec batch까지 들어간다.

### icon polygon도 “같은 후보” 매핑이 아직 어긋난다
- 같은 icon polygon은 sidecar rec dump에서:
  - `sidecar_rec_batch_logits_1_n6_w320_ts40_cls18385.json`
  - `original_index=3`
  - `max_val ~= 0.9977527`
- FFI rec dump에서는:
  - `rec_batch_logits_1_n6_w320_ts40_cls18385.json`
  - `original_index=4`
  - `max_val ~= 0.9859711`
- 즉 polygon이 같아 보여도, 현재 양쪽 end-to-end에서 rec batch에 올라간 후보 매핑과 주변 candidate 집합이 이미 다르다.

### 이번 턴 결론
- rec predictor 자체는 닫혔고, 이제 남은 핵심은 `det -> rec candidate set/order` 정합성이다.
- 다음 우선순위는:
  - sidecar `dt_polys` 전체와 FFI `rec_candidates` 전체를 같은 이미지에서 polygon 단위로 대응시키기
  - 특히 sidecar에만 존재하는 `换一换`, `雷军...`, `AI来了...` 3개 후보가
    FFI에서 어디서 탈락하는지(`db_postprocess`, sorting, min-side, rec entry filtering) 추적하기

## 2026-04-17 추가 진행 상황 27

### FFI rec candidate dump 추가
- `bridge.cc`에 `ffi_rec_candidates_*.json` dump를 추가했다.
- `debug_trace` + `BUZHIDAO_PADDLE_FFI_DUMP_DIR`일 때 다음 정보를 남긴다.
  - `original_index`
  - `sorted_index`
  - `ratio`
  - `crop_width`, `crop_height`
  - `cls_label`, `cls_score`, `rotated_180`
  - `polygon`
- 이걸로 sidecar `sidecar_det_candidates_*.json`와 FFI rec 진입 후보 집합을 직접 비교할 수 있다.

### det->rec 후보 집합 비교 결과
- `test3` 기준:
  - sidecar det candidate 수: `27`
  - FFI rec candidate 수: `25`
- sidecar에만 있고 FFI rec candidate에는 없는 후보:
  - `[[1488,741],[1610,741],[1610,777],[1488,777]]` (`换一换`)
  - `[[136,1055],[595,1055],[595,1086],[136,1086]]` (`AI来了消博会消费行业会怎么变`)
  - `[[916,983],[1424,983],[1424,1013],[916,1013]]` (`雷军：最大的心理负担是不能说错话`)
- 따라서 이 3개는 단순 rec decode 문제가 아니라,
  **FFI에서 아예 rec candidate 집합에 들어오지 못한다.**

### 나머지 일부는 1px 수준 polygon 차이로 갈라진다
- 예를 들어 다음 줄들은 sidecar/FFI에 모두 “같은 위치의 텍스트”가 있지만 polygon이 1px씩 다르다.
  - `百度一下`
    - sidecar `[[1448,368],[1590,368],[1590,411],[1448,411]]`
    - FFI `[[1449,369],[1591,369],[1591,412],[1449,412]]`
  - `贵州纪委监委原主任被查`
    - sidecar `[[85,282],[437,281],[437,315],[85,316]]`
    - FFI `[[86,282],[437,282],[437,316],[86,316]]`
  - 긴 headline 일부도 같은 패턴으로 1px 차이가 있다.
- 이건 여전히 `DBPostProcess`/box geometry 차이가 완전히 닫히지 않았다는 뜻이지만,
  현재 큰 miss는 1px drift보다 **후보 누락 3건**이다.

### 이번 턴 결론
- rec predictor는 닫혔다.
- crop도 사실상 닫혔다.
- 지금 남은 최우선 병목은:
  1. `换一换`
  2. `AI来了消博会消费行业会怎么变`
  3. `雷军：最大的心理负担是不能说错话`
  이 3개 sidecar-only 후보가 FFI에서 왜 rec candidate로 승격되지 못하는지다.
- 다음 우선순위는 `ffi_rec_candidates_*.json`와 `sidecar_det_contours_*.json`/`ffi_det_contours_*.json`를 연결해서,
  이 3개 후보가 `db_postprocess`에서 이미 누락되는지, 아니면 polygon 차이 때문에 다른 candidate로 분열되는지 추적하는 것이다.

## 2026-04-17 추가 진행 상황 28

### sidecar-only 3개 후보는 모두 FFI `db_postprocess score` reject였다
- FFI `ffi_det_contours_*.json`와 sidecar `sidecar_det_contours_*.json`를
  같은 위치 후보 기준으로 직접 맞대봤다.
- 누락 3개 모두 패턴이 같다.

1. `换一换`
   - sidecar:
     - scaled box `[[1488,741],[1610,741],[1610,777],[1488,777]]`
     - `accepted = true`
     - `score ~= 0.9252`
   - FFI:
     - 같은 위치에 큰 contour는 존재
     - 하지만 `accepted = false`
     - `reject_reason = "score"`
     - `score ~= 0.5153`

2. `AI来了消博会消费行业会怎么变`
   - sidecar:
     - scaled box `[[136,1055],[595,1055],[595,1086],[136,1086]]`
     - `accepted = true`
     - `score ~= 0.8927`
   - FFI:
     - 겹치는 large contour 존재
     - `accepted = false`
     - `reject_reason = "score"`
     - `score ~= 0.5189`

3. `雷军：最大的心理负担是不能说错话`
   - sidecar:
     - scaled box `[[916,983],[1424,983],[1424,1013],[916,1013]]`
     - `accepted = true`
     - `score ~= 0.9126`
   - FFI:
     - 겹치는 large contour 존재
     - `accepted = false`
     - `reject_reason = "score"`
     - `score ~= 0.4708`

### 결론: 현재 큰 miss는 contour 누락이 아니라 score 계산 차이
- 이 3개는 FFI에서 contour 자체가 없는 것이 아니다.
- contour는 있는데 `box_threshold` 통과용 score가 sidecar보다 크게 낮게 계산되어
  `db_postprocess`에서 잘린다.
- 따라서 현재 최우선 병목은 `findContours`가 아니라
  **`box_score_fast` / polygon mask 평균 규약**이다.

### 다음 우선순위
- 다음은 `db_postprocess`의 contour sampling보다 먼저,
  sidecar `box_score_fast`와 FFI score 계산을 같은 contour 기준으로 1:1 맞대는 일이다.
- 특히 위 3개 후보의 bbox, mask 영역, 평균 포함 픽셀 수를 같이 덤프해
  왜 sidecar `~0.89~0.92`가 FFI `~0.47~0.52`로 떨어지는지 확인해야 한다.

## 2026-04-17 추가 진행 상황 29

### score 계산 입력 비교로 원인이 더 좁혀졌다
- sidecar/FFI det contour dump에 다음 필드를 추가해 같은 후보의 score 계산 입력을 직접 비교했다.
  - `score_bbox`
  - `score_mask_pixels`
  - `score_sum`
- 비교 대상은 sidecar-only 3개 후보(`换一换`, `AI来了...`, `雷军...`)다.

### 공통 패턴: FFI rect가 위로 기울고 bbox가 불필요하게 커진다
1. `换一换`
   - sidecar rect:
     - `[[1503,746],[1606,746],[1606,761],[1503,761]]`
     - `score_bbox=[1503,746,1606,761]`
     - `score_mask_pixels=1664`
     - `score_sum~=1539.56`
     - `score~=0.9252`
   - FFI rect:
     - `[[1501.24,747.15],[1603.91,731.91],[1606.00,746.00],[1503.33,761.25]]`
     - `score_bbox=[1501,731,1606,762]`
     - `score_mask_pixels=1479`
     - `score_sum~=762.08`
     - `score~=0.5153`

2. `AI来了消博会消费行业会怎么变`
   - sidecar:
     - `score_bbox=[145,1056,588,1069]`
     - `score_mask_pixels=6216`
     - `score_sum~=5548.97`
     - `score~=0.8927`
   - FFI:
     - `score_bbox=[144,1045,588,1070]`
     - `score_mask_pixels=5304`
     - `score_sum~=2752.40`
     - `score~=0.5189`

3. `雷军：最大的心理负担是不能说错话`
   - sidecar:
     - `score_bbox=[928,985,1420,997]`
     - `score_mask_pixels=6409`
     - `score_sum~=5849.14`
     - `score~=0.9126`
   - FFI:
     - `score_bbox=[927,974,1420,997]`
     - `score_mask_pixels=5870`
     - `score_sum~=2763.43`
     - `score~=0.4708`

### 해석
- FFI는 같은 위치 contour를 잡고도 `rect`가 sidecar보다 위쪽으로 기울어져 있다.
- 그 결과 `score_bbox`가 위로 11~15px 정도 더 커지고,
  실제 텍스트 줄 위의 저확률 영역까지 score 평균에 섞인다.
- 동시에 `score_sum` 자체가 sidecar의 절반 수준으로 떨어진다.
- 즉 현재 큰 오차는 단순 threshold 차이가 아니라,
  **FFI `min_area_rect` / `get_mini_box` 결과가 sidecar보다 더 기울어진 rect를 만들고,
  그 rect를 그대로 `score_box()`에 써서 평균 영역이 잘못 잡히는 것**이다.

### 이번 턴 결론
- 남은 최우선 병목은 `box_score_fast` 그 자체보다,
  그 입력으로 쓰는 `rect_pts`가 sidecar보다 더 기울어지는 점이다.
- 다음 우선순위는:
  - sidecar `get_mini_boxes(contour)` 출력과 FFI `get_mini_box(min_area_rect(contour))`를 같은 contour 기준으로 맞대기
  - 필요하면 `min_area_rect` 입력 contour sampling 또는 `get_mini_box` ordering/rounding 규약을 sidecar 쪽에 더 맞추기

## 2026-04-17 추가 진행 상황 30

### sidecar `get_mini_boxes(contour)`와 FFI `get_mini_box(min_area_rect(contour))`를 같은 후보 기준으로 직접 비교했다
- sidecar det contour dump에 `cv2.minAreaRect(contour)` 원값을 추가했다.
  - `min_area_rect.center`
  - `min_area_rect.width`
  - `min_area_rect.height`
  - `min_area_rect.angle`
  - `rect_points` (`cv2.boxPoints` 결과)
- FFI det contour dump에도 같은 축의 raw rect 정보를 추가했다.
  - `min_area_rect.center`
  - `min_area_rect.width`
  - `min_area_rect.height`
  - `min_area_rect.angle`
  - `rect_points` (`rect_to_points(rect)` 결과)
- 코드 동작은 바꾸지 않고, `test3.png`를 다시 실행해 sidecar/FFI det contour dump를 재생성했다.

### 비교 결과: 같은 후보에서 raw rect 단계부터 이미 FFI가 기울어져 있다
1. `换一换`
   - sidecar:
     - contour point 수 `22`
     - `minAreaRect(center=[1554.5,753.5], width=15, height=103, angle=90)`
     - `rect_points=[[1503,746],[1606,746],[1606,761],[1503,761]]`
   - FFI:
     - contour point 수 `11`
     - `min_area_rect(center=[1553.62,746.58], width=103.79, height=14.25, angle=2.99 rad)`
     - `rect_points=[[1606,746.00],[1503.33,761.25],[1501.24,747.15],[1603.91,731.91]]`

2. `AI来了消博会消费行业会怎么变`
   - sidecar:
     - contour point 수 `50`
     - `minAreaRect(center=[366.5,1062.5], width=13, height=443, angle=90)`
     - `rect_points=[[145,1056],[588,1056],[588,1069],[145,1069]]`
   - FFI:
     - contour point 수 `29`
     - `min_area_rect(center=[366.41,1057.06], width=443.03, height=11.97, angle=3.11 rad)`
     - `rect_points=[[588,1057],[145.14,1069.08],[144.81,1057.11],[587.67,1045.04]]`

3. `雷军：最大的心理负担是不能说错话`
   - sidecar:
     - contour point 수 `52`
     - `minAreaRect(center=[1174,991], width=12, height=492, angle=90)`
     - `rect_points=[[928,985],[1420,985],[1420,997],[928,997]]`
   - FFI:
     - contour point 수 `14`
     - `min_area_rect(center=[1173.38,985.53], width=491.15, height=11.95, angle=3.12 rad)`
     - `rect_points=[[1419.02,985.99],[928,997.00],[927.73,985.05],[1418.75,974.05]]`

### contour point set 차이도 동시에 확인했다
- 위 3개 모두 FFI contour point 수가 sidecar보다 크게 적다.
  - `22 -> 11`
  - `50 -> 29`
  - `52 -> 14`
- FFI contour는 sidecar처럼 bbox 가장자리의 axis-aligned 점들을 촘촘히 유지하지 않고,
  긴 수평 구간 끝점 몇 개와 계단 모서리 점 일부만 남기는 경향이 있다.
- 그 결과 raw `min_area_rect`가 sidecar의 수평 박스 대신, 위쪽으로 살짝 들린 얇은 rect로 계산된다.

### 해석
- 이번 비교로 큰 차이는 `get_mini_box` ordering보다 앞단에 있다.
- 즉 **같은 contour 후보를 본다고 해도 FFI `trace_component_contour()`가 만든 point set 자체가
  sidecar `findContours(..., CHAIN_APPROX_SIMPLE)` 결과와 다르고, 그 point set 차이가 바로
  `min_area_rect` 기울기 차이로 이어진다.**
- 따라서 score 저하의 1차 원인은 `box_score_fast`도, `get_mini_box` ordering도 아니고
  `trace_component_contour()` 출력 shape다.

### 다음 우선순위
- `min_area_rect` 수식 수정부터 들어가기보다,
  `trace_component_contour()`가 sidecar `CHAIN_APPROX_SIMPLE`처럼
  축 정렬된 경계 point를 더 보존하도록 바꾸는 것이 먼저다.
- 특히 긴 수평 텍스트 줄에서:
  - 좌우 끝점만 남기지 말고
  - 상단/하단 edge의 계단형 경계점 집합을 sidecar와 더 비슷하게 유지해야 한다.

## 2026-04-17 추가 진행 상황 31

### `trace_component_contour()`의 collinear 제거를 빼자 sidecar-only 3개 후보가 즉시 복구됐다
- 원인은 예상대로 `min_area_rect` 수식보다 앞단 contour 축약이었다.
- FFI는 unit-edge loop를 만든 뒤 `normalize_contour_to_pixel_grid()`에서
  `simplify_contour()`로 collinear point를 공격적으로 제거하고 있었다.
- 이 단순화가 긴 수평 텍스트 줄의 계단형 상단/하단 경계점을 너무 많이 날려,
  `min_area_rect`가 sidecar보다 기울어진 얇은 rect를 만들고 있었다.
- 이번 턴에는 snap 후에는 collinear 제거를 하지 않고,
  연속 중복점만 제거하도록 바꿨다.

### 결과: `test3.png`의 큰 누락 3개가 모두 복구됐다
- 검증은 `cargo test -p buzhidao --features paddle-ffi --no-run`과
  FFI `test3.png` 샘플 실행으로 다시 확인했다.
- 수정 후 FFI 최종 detections에는 다음 3개가 모두 다시 들어온다.
  - `换一换`
  - `AI来了消博会消费行业会怎么变`
  - `雷军：最大的心理负担是不能说错话`
- FFI `debug_len`도 `27`이 되었고, rec candidate dump 기준으로도
  `ffi_rec_candidates_*.json` 항목 수가 `27`까지 올라와 sidecar와 맞는다.

### score도 sidecar 수준으로 정상화됐다
- `换一换`: `0.5153 -> 0.9986`
- `AI来了...`: `0.5189 -> 0.9979`
- `雷军...`: `0.4708 -> 0.9954`
- 즉 이 3개는 `box_score_fast` 자체보다 contour 축약이 원인이었다는 게 실측으로 닫혔다.

### 남은 상태
- `test3.png`에서 이제 큰 candidate-set mismatch는 해소됐다.
- 다만 icon 후보 `[[472,569],[506,569],[506,595],[472,595]]`는
  FFI에서 여전히 rec 결과가 blank라 최종 detections에는 들어오지 않는다.
- 현재 icon 후보는 FFI에서도:
  - det accepted
  - `score_bbox=[480,571,502,585]`
  - `scaled=[[472,569],[506,569],[506,595],[472,595]]`
  - rec candidate 집합 포함 (`sorted_index=10`, `crop=34x26`, `rotated_180=true`)
  까지는 sidecar와 맞는다.

### 추가 확인
- 최신 FFI rec dump를 sidecar rec predictor에 다시 재주입해 보니,
  icon 후보는 여전히 `sidecar_text=""`, `ffi_text=""`로 동일했고 logits diff도 float noise 수준이었다.
- 즉 **현재 남은 icon mismatch는 rec predictor 런타임 차이가 아니라,
  sidecar end-to-end가 실제로 rec에 넣는 icon tensor와 FFI가 넣는 icon tensor가
  아직 같지 않다**는 쪽으로 다시 좁혀진다.

### 다음 우선순위
- sidecar rec batch dump와 FFI rec batch dump를 polygon 기준으로 직접 매칭해서,
  icon 후보의 `input_values` / `raw_pixels`가 실제로 어느 배치에서 갈라지는지 다시 확인해야 한다.
- 특히 sidecar 쪽 `original_index`는 det candidate 변화에 따라 불안정할 수 있으니,
  다음 비교는 `original_index`가 아니라 polygon과 crop size 기준으로 맞대는 것이 안전하다.

## 2026-04-17 추가 진행 상황 32

### icon 후보의 마지막 차이는 `rotate180` 구현 차이였다
- polygon 기준으로 rec batch dump를 다시 맞대보니,
  icon 후보 `[[472,569],[506,569],[506,595],[472,595]]`는
  sidecar/FFI 모두 같은 batch(`*_1_n6_w320_ts40_cls18385`)에 들어가지만
  raw crop와 input tensor가 달랐다.
  - sidecar dump:
    - `raw_channel_means ~= [232.2952, 153.3767, 137.0633]`
    - `raw_pixels[:24] = 0 ...`
    - `input_mean ~= 0.07218648`
  - FFI dump:
    - `raw_channel_means ~= [248.9400, 164.0498, 145.8631]`
    - 밝은 배경으로 시작
    - `input_mean ~= 0.09078552`
- 같은 polygon에 대해 PaddleX `get_minarea_rect_crop()`를 직접 호출해 보니,
  direct crop는 FFI 쪽과 일치했다.
- 따라서 차이는 crop이 아니라 **textline orientation 이후 180도 회전 구현**이었다.

### sidecar는 exact flip이 아니라 `warpAffine(..., INTER_CUBIC)`를 쓴다
- PaddleX `rotate_image(..., 180)` 구현을 다시 확인했다.
  - `cv2.getRotationMatrix2D(center, 180, 1.0)`
  - `cv2.warpAffine(..., flags=cv2.INTER_CUBIC)`
  - 기본 border는 `0`
- 이 경로는 단순 `cv2.rotate(..., ROTATE_180)`와 다르다.
  - 좌상단/우하단에 검은 border가 생긴다.
  - icon 같은 작은 crop에서는 이 차이가 rec 결과를 바꾼다.

### 실측 확인
- 같은 icon crop에 대해 Python에서 두 회전 방식을 직접 비교했다.
  1. exact flip (`cv2.rotate`)
     - `input_mean ~= 0.09062568`
     - rec 결과: `""`
  2. sidecar 방식 (`rotate_image(..., 180)` / warpAffine)
     - `input_mean ~= 0.07218648`
     - rec 결과: `"💯"`, `score ~= 0.20584`
- 즉 icon mismatch의 마지막 원인은 detector도 rec predictor도 아니라
  **FFI `rotate180`가 sidecar보다 너무 “정확한” flip을 하고 있던 것**이다.

### FFI 수정
- `rotate180()`를 단순 역순 복사에서,
  sidecar와 같은 성격의 `180도 warpAffine` 샘플링으로 바꿨다.
  - cubic interpolation
  - out-of-bounds는 0으로 취급
  - alpha는 `255` 유지
- 첫 시도에서 alpha를 `0`으로 뒀더니 rec 입력이 전부 죽어 blank가 되어,
  이건 바로 되돌리고 alpha를 `255`로 고정했다.

### 결과
- `test3.png` FFI 최종 detections에 icon이 다시 들어온다.
  - polygon `[[472,569],[506,569],[506,595],[472,595]]`
  - text `"💯"`
  - score `~0.2016`
- 최신 FFI rec dump를 sidecar rec predictor에 재주입해도
  이제 `sidecar_text="💯"`, `ffi_text="💯"`로 같고
  logits diff도 float noise 수준이다.
- 즉 `test3.png`에서는:
  - det candidate 집합
  - crop geometry
  - orientation 회전
  - rec tensor
  - rec logits/decode
  가 모두 sidecar와 실질적으로 맞는다.

### 이번 턴 결론
- `test3.png` 기준으로 남아 있던 큰 parity gap은 사실상 닫혔다.
- 다음 우선순위는 `test2.png`의 남은 차이(`0`, `》`, `Ⅲ`, `从` 계열)가
  이번 contour/rotate 수정 이후 얼마나 줄었는지 다시 재측정하는 것이다.

## 2026-04-17 추가 진행 상황 33

### `test2.png`를 다시 재측정했다
- sidecar와 FFI를 모두 최신 코드로 `test2.png`에 다시 실행했다.
- 결과는 이렇다.
  - sidecar final detections: `14`
  - FFI final detections: `13`
- 즉 `test3.png`처럼 완전 parity까지는 아직 못 갔다.

### 그래도 큰 변화는 있었다
- FFI `ffi_rec_candidates_0_1069x881.json`와 sidecar `sidecar_det_candidates_0_test2_1069x881.json`를 비교해 보니
  후보 수는 둘 다 `19`로 맞는다.
- 예전처럼 candidate set 자체가 크게 어긋나는 상태는 아니다.
- 남은 차이는 주로:
  - 일부 polygon의 `1px` 수준 box drift
  - 그리고 그 위에서의 rec 결과 차이
  로 보인다.

### 현재 눈에 띄는 남은 차이
- top-left box:
  - sidecar: `[[14,19],[124,19],[124,67],[14,67]] -> "0"`
  - FFI: 같은 polygon이 rec candidate에는 들어가지만 final debug에서는 `text=""`, `accepted=false`
- 상단 긴 줄:
  - sidecar: `中文，中国话...` 쪽 문자열이 상대적으로 정상
  - FFI: `ł`, `|`, `cnee` 같은 깨진 decode가 남음
- 중단 `word processor` 줄:
  - sidecar: `" Ⅲ川人 中國語word processor"`
  - FFI: `" 川 中國語word processor"`
- 하단 `从中文词典。中文辞典。`는 현재는 둘 다 살아 있다.

### 중요한 관찰: `test2`에서는 sidecar rec dump 매핑이 일부 후보에서 신뢰되지 않는다
- polygon `[[14,19],[124,19],[124,67],[14,67]]`에 대해:
  - sidecar end-to-end final output은 `"0"`를 낸다.
  - 하지만 sidecar rec batch dump에 기록된 같은 polygon item은,
    compare CLI로 재주입하면 `sidecar_text=""`가 나온다.
  - FFI 같은 polygon item도 동일하게 blank다.
- 즉 이 후보는 현재
  **“sidecar rec batch dump의 polygon -> tensor/logits 매핑이 실제 end-to-end 결과와 어긋나 있다”**
  는 쪽이 더 강하다.

### 해석
- `test3` icon 때처럼 sidecar rec dump를 그대로 기준 truth로 쓰면
  `test2`에서는 잘못된 결론으로 갈 수 있다.
- 특히 sidecar instrumentation의 `pending` metadata와 실제 rec output 순서가
  일부 batch에서 어긋날 가능성이 있다.
- 따라서 `test2`의 남은 차이를 닫으려면,
  먼저 **sidecar rec dump 매핑 정확도부터 고쳐야 한다.**

### 다음 우선순위
- sidecar `instrumented_process()`에서
  `batch_raw_imgs` / `batch_preds` / `pending metadata`가 실제 rec output 순서와 일치하는지 검증하고,
  필요하면 dump 매핑을 고친다.
- 그 다음에 `test2`의 `0`, `》`, `Ⅲ`, `从` 계열을 다시 polygon 기준으로 비교해야 한다.

## 2026-04-17 추가 진행 상황 34

### `crop_to_bbox()`를 sidecar 흐름으로 더 직접 맞춰봤다
- FFI `crop_to_bbox()`를 다음 기준으로 재구성했다.
  - `order_clockwise()` 대신 sidecar `get_minarea_rect_crop()`와 같은 성격의 box ordering
  - dst quad를 `[[0,0],[w,0],[w,h],[0,h]]`로 사용
  - perspective crop를 bilinear가 아니라 cubic replicate로 샘플링
  - axis-aligned fast path를 제거하고 같은 crop 경로로 통일
- 구현은 `app/native/paddle_bridge/bridge.cc`에서
  `order_crop_box_like_sidecar()`를 추가하고 `crop_to_bbox()`를 그 경로로 바꿨다.

### 결과: `test3`는 유지됐지만 `test2` crop size는 그대로였다
- 검증:
  - `cargo test -p buzhidao --features paddle-ffi --no-run`
  - FFI `test3.png` 샘플 실행
  - sidecar `test2.png` 실행
  - FFI `test2.png` 실행 + det/rec dump
- `test3.png`는 회귀 없이 그대로 `27`건이며 icon `"💯"`도 유지된다.
- 하지만 `test2.png`는 final output이 사실상 그대로였다.
  - top-left `0` 후보는 여전히 blank reject
  - 상단 `》` / 중단 `Ⅲ` 계열 차이도 그대로 남음

### 새로 확정된 점
- 이번 변경 뒤에도 FFI dump 파일명은 여전히:
  - `first_stage_14_19_110x48.bmp`
  - `first_stage_15_266_110x48.bmp`
  - `first_stage_126_267_475x44.bmp`
- sidecar dump는 계속:
  - `109x47`
  - `109x48`
  - `474x44`
- 즉 남은 차이는 `warpPerspective` 설정이 아니라, 그보다 앞단인
  `cv2.minAreaRect(np.int32(points)) + cv2.boxPoints(...)`가 만드는 width/height 수치 자체다.
- 현재 FFI custom `min_area_rect()`는 sidecar OpenCV가 내는 그 미세한 truncation을 아직 재현하지 못한다.

### 이번 턴 결론
- `crop_to_bbox()`의 큰 구조 차이는 상당 부분 정리됐지만,
  `test2`를 막는 마지막 1px 차이는 아직 OpenCV `minAreaRect/boxPoints` 쪽이다.
- 다음 타깃은 crop warp가 아니라,
  같은 integer quad에 대해 sidecar OpenCV가 왜 `110 -> 109`, `475 -> 474`를 내는지
  그 수치 규약을 더 직접 복제하는 쪽이다.

## 2026-04-17 추가 진행 상황 35

### OpenCV `rotatingCalipers`를 crop 전용으로 포팅했다
- OpenCV 공식 구현(`rotcalipers.cpp`)을 기준으로
  `min_area_rect_box_like_opencv()`를 추가했다.
- 이 함수는:
  - `convex_hull`
  - hull orientation 계산
  - `rotatingCalipers(..., CALIPERS_MINAREARECT)`와 같은 방식의
    `corner + vec1 + vec2` 계산
  을 그대로 따라가서, crop 직전 quad를 만든다.
- `crop_to_bbox()`는 이제
  `min_area_rect_box_like_opencv() -> order_crop_box_like_sidecar()`
  경로를 사용한다.

### 결과: `test2`의 마지막 1px crop mismatch가 실제로 닫혔다
- 최신 FFI dump에서 다음 crop들이 sidecar와 같아졌다.
  - `[[14,19],[124,19],[124,67],[14,67]]`: `109x47`
  - `[[15,266],[125,266],[125,314],[15,314]]`: `109x48`
  - `[[126,267],[601,267],[601,311],[126,311]]`: `474x44`
- 즉 이전의
  - `110x48`
  - `110x48`
  - `475x44`
  차이는 OpenCV `minAreaRect/boxPoints` 수치 규약 문제였고,
  이번 포팅으로 그 부분이 맞았다.

### end-to-end 변화
- `test3.png`는 회귀 없이 그대로 유지된다.
  - `27` detections
  - icon `"💯"` 유지
- `test2.png`에서는 top-left candidate가 복구됐다.
  - polygon `[[14,19],[124,19],[124,67],[14,67]]`
  - FFI final text `"0"`
  - debug score `~0.253`

### 아직 남은 차이
- `test2.png`는 top-left `0`는 닫혔지만, 전체 final parity는 아직 아니다.
- 현재 남은 핵심은 crop size가 아니라 rec text 품질이다.
  - `中國語 []》` vs `中國語 [] `
  - 상단 긴 줄의 garbled decode
  - `Ⅲ川人` 계열 누락
- 즉 다음 타깃은 이제 다시 rec 입력/logits 쪽으로 좁혀진다.

## 2026-04-17 추가 진행 상황 36

### contour 밀도 자체는 줄였지만, `test2` 남은 mismatch는 그대로였다
- `trace_component_contour()` 뒤에 큰 contour만 대상으로
  same-direction run 내부점을 제거하는 압축을 추가했다.
- 의도는 sidecar `findContours(..., CHAIN_APPROX_SIMPLE)`처럼
  수백~수천 점짜리 dense edge contour를 더 짧은 point set으로 줄이는 것이었다.

### 실제 수치 변화
- `test2`의 문제 줄들에서 FFI contour point 수는 많이 줄었다.
  - title: `564 -> 60`
  - long line: `2076 -> 38`
  - `word processor` 줄: `1220 -> 88`
- sidecar 같은 후보의 contour 수는 각각 `65 / 40 / 80`이라,
  point 개수만 보면 꽤 가까워졌다.

### 하지만 출력은 거의 안 움직였다
- `test3.png`는 회귀 없이 그대로 유지된다.
  - `27` detections
  - icon `"💯"` 유지
- 반면 `test2.png`의 남은 mismatch는 그대로다.
  - `中國語 []》` vs `中國語 [] `
  - 상단 긴 줄의 깨진 decode
  - `Ⅲ川人` 계열 누락
- 즉 contour 개수만 줄이는 것만으로는 sidecar parity가 닫히지 않았다.

### 시도했다가 되돌린 것
- 큰 component에 한해 boundary pixel center 집합으로 `min_area_rect`를 구하는 실험도 했다.
- 하지만 실제 출력 변화가 없어서 이 경로는 코드에 남기지 않았다.
- 현재 코드는:
  - dense contour run 압축은 유지
  - boundary-center 기반 rect 경로는 제거
  인 stable baseline 상태다.

### 이번 턴 결론
- 남은 차이는 이제 “point 수”보다 “point 좌표 체계”에 가깝다.
- sidecar는 `findContours(..., CHAIN_APPROX_SIMPLE)`가
  미세하게 기울어진 contour를 만들고,
  FFI는 아직 axis-aligned edge contour 성격이 강해서
  `min_area_rect` angle이 `0`으로 눌린다.
- 다음 타깃은 contour를 더 줄이는 게 아니라,
  OpenCV contour와 같은 좌표계/표현으로 바꾸는 쪽이다.

## 2026-04-17 추가 진행 상황 37

### 큰 contour에 `row/column extrema` 기반 rect fit도 시도했지만 유지하지 않았다
- `trace_component_contour()`는 그대로 두고,
  큰 component만 `min_area_rect()` 입력을
  row/column scanline의 extrema point set으로 바꿔보는 실험을 했다.
- 의도는 OpenCV `CHAIN_APPROX_SIMPLE`가 남기는 staircase contour corner를
  edge-corner polygon보다 더 가깝게 흉내 내는 것이었다.

### 결과: `title`만 일부 움직였고 핵심 줄들은 그대로였다
- `test2`의 title candidate는 이 경로에서
  scaled box가 `[[134,25],[422,22],[422,63],[134,65]]`로 바뀌고
  `min_area_rect.angle ~= 3.13289714`까지 움직였다.
- 하지만 같은 실험에서 남은 핵심 줄들은 여전히 axis-aligned였다.
  - long line: `angle = 0`
  - `word processor` 줄: `angle = 0`
- 즉 extrema point set만으로는
  sidecar contour의 미세한 기울기를 재현하지 못했다.

### 회귀도 있었다
- 이 경로를 켜면 top-left candidate
  `[[14,19],[124,19],[124,67],[14,67]]`
  가 다시 `"0"`에서 `"舌"`로 회귀했다.
- `test3.png`는 유지됐지만,
  `test2.png` baseline보다 나빠지므로 코드는 전부 되돌렸다.

### 이번 턴 결론
- dense contour run 압축은 유지한 채 stable baseline으로 복구했다.
- `cargo test -p buzhidao --features paddle-ffi --no-run`,
  FFI `test2.png`,
  FFI `test3.png`
  를 다시 실행해 baseline 복구를 확인했다.
- 이번 실험으로 더 분명해진 점은,
  남은 병목이 “경계점 수”나 “extrema 부족”이 아니라
  OpenCV `findContours(..., CHAIN_APPROX_SIMPLE)`가 생성하는
  contour 좌표계 자체라는 점이다.

## 2026-04-17 추가 진행 상황 38

### `trace_component_contour()`를 edge-corner가 아니라 interior-pixel contour로 바꿨다
- 기존 FFI contour는 boundary edge의 corner 좌표를 그대로 따라가서,
  점 수를 줄여도 segment가 전부 수평/수직으로만 이어졌다.
- 이제 각 boundary edge를 그 edge가 속한 interior pixel 좌표로 매핑해서
  contour point를 만든다.
- 이 변경으로 FFI contour도 sidecar `findContours(..., CHAIN_APPROX_SIMPLE)`처럼
  diagonal segment를 실제로 가지게 됐다.

### detector rect fit는 contour hull 기준으로 바꿨다
- `db_postprocess()`에서 detector rect를
  `min_area_rect(contour)` 대신
  `min_area_rect(convex_hull(contour))`
  기준으로 계산한다.
- 새 contour는 non-convex chain이므로,
  hull 기준으로 맞추지 않으면 angle이 `0/pi` 쪽으로 다시 눌렸다.
- sidecar `cv2.minAreaRect(contour)`도 의미상 hull에 의해 결정되므로,
  이 변경이 더 가깝다.

### 결과: `test2` 남은 비수평 줄들이 실제로 sidecar 쪽으로 움직였다
- title 줄:
  - 기존 FFI: `中國語 [] `
  - 현재 FFI: `中國語 [] 》`
  - sidecar: `中國語 []》`
- 상단 긴 줄:
  - 기존 FFI: ` 中文，中国话 。(=(中語)，ł叫(支那語)，叫(華語)，|(cnee)`
  - 현재 FFI: ` 中文，中国话。(=(中語)，叫(支那語)，叫(華語)，o(chinese))`
- `word processor` 줄:
  - 기존 FFI: ` 川 中國語word processor`
  - 현재 FFI: ` 川人 中國語word processor`
  - sidecar: ` Ⅲ川人 中國語word processor`

### geometry도 실제로 기울기 쪽으로 가까워졌다
- title scaled box:
  - 기존 FFI: `[[133,22],[422,22],[422,65],[133,65]]`
  - 현재 FFI: `[[133,25],[421,21],[421,62],[134,65]]`
  - sidecar: `[[133,24],[421,22],[421,62],[133,64]]`
- long line scaled box:
  - 기존 FFI: `[[17,112],[1069,112],[1069,148],[17,148]]`
  - 현재 FFI: `[[18,113],[1068,114],[1068,148],[18,146]]`
  - sidecar: `[[17,112],[1068,114],[1068,148],[17,146]]`
- `word processor` 줄도
  - 기존 FFI: `[[121,506],[733,506],[733,554],[121,554]]`
  - 현재 FFI: `[[121,506],[733,507],[733,554],[121,553]]`
  까지 좁혀졌다.

### 회귀는 없었다
- `test3.png`는 그대로 유지된다.
  - `27` detections
  - icon `"💯"` 유지
- `test2.png` top-left `"0"`도 그대로 유지된다.

### 이번 턴 결론
- 남은 차이는 이제 “완전히 축정렬로 눌린 detector quad” 단계는 벗어났다.
- 현재 남은 mismatch는 detector geometry 미세 오차 + rec 입력 차이가 결합된 상태로 보인다.
- 다음 타깃은 title/`word processor` 줄의 최신 FFI rec dump를 sidecar rec predictor에 재주입해서,
  geometry가 개선된 뒤에도 남는 차이가 순수 rec 입력인지 다시 좁히는 쪽이다.

## 2026-04-17 추가 진행 상황 39

### `compare-ffi-rec-dump` 도구 버그를 먼저 고쳤다
- compare CLI가 FFI dump의 `values`를
  sidecar predictor output shape로 다시 reshape하고 있어서
  `test2` 최신 dump(`58x18385`, `80x18385`)에서 실패했다.
- `ocr_server/ocr_server.py`에서
  FFI logits reshape는 dump의 `time_steps`/`num_classes`를 그대로 사용하도록 고쳤다.
- `python -m py_compile ocr_server/ocr_server.py`로 확인했다.

### title / `word processor`는 이제 rec predictor 차이가 아님이 확정됐다
- 최신 `test2` FFI rec dump를 sidecar rec predictor에 재주입했다.
- title (`original_index=1`)
  - FFI dump: polygon `[[133,25],[421,21],[421,62],[134,65]]`, `rec_width=346`
  - sidecar 재주입 결과: `sidecar_text="中國語 [] 》"`
  - FFI decode 결과: `ffi_text="中國語 [] 》"`
  - logits diff: `mean_abs_diff ~= 2.9e-12`, `max_abs_diff ~= 7.15e-07`
- `word processor` 줄 (`original_index=11`)
  - FFI dump: polygon `[[121,506],[733,507],[733,554],[121,553]]`, `rec_width=639`
  - sidecar 재주입 결과: `sidecar_text=" 川人 中國語word processor"`
  - FFI decode 결과: `ffi_text=" 川人 中國語word processor"`
  - logits diff: `mean_abs_diff ~= 3.7e-12`, `max_abs_diff ~= 1.43e-06`

### 따라서 남은 차이는 rec predictor가 아니라 rec tensor다
- 같은 FFI tensor를 sidecar rec model에 넣으면
  sidecar도 FFI와 똑같이 decode한다.
- 즉 현재 sidecar end-to-end와의 차이는
  detector/warp 이후 생성되는 rec tensor 자체다.

### sidecar vs FFI tensor 차이도 아직 크다
- title:
  - sidecar polygon `[[133,24],[421,22],[421,62],[133,64]]`, `rec_width=354`
  - FFI polygon `[[133,25],[421,21],[421,62],[134,65]]`, `rec_width=346`
  - input tensor mean abs diff `~0.301`
- `word processor` 줄:
  - sidecar polygon `[[121,505],[733,507],[733,554],[120,552]]`, `rec_width=626`
  - FFI polygon `[[121,506],[733,507],[733,554],[121,553]]`, `rec_width=639`
  - input tensor mean abs diff `~0.261`
- raw channel mean은 비슷하지만,
  crop width/quad가 아직 달라 rec tensor가 크게 달라진다.

### 이번 턴 결론
- `test2`의 남은 mismatch는 이제 다시 명확하다.
- title / 긴 줄 / `word processor` 모두
  “predictor가 다르게 decode한다” 문제가 아니라
  “FFI가 아직 sidecar와 다른 crop/warp tensor를 만든다” 문제다.
- 다음 타깃은 rec logits이 아니라,
  sidecar `get_minarea_rect_crop()`가 만드는 최종 quad와
  FFI `crop_to_bbox()` 직전 quad를 후보별로 직접 맞대는 쪽이다.

## 2026-04-17 추가 진행 상황 40

### rec dump에 sidecar `crop_box` / FFI `crop_quad`를 같이 남기도록 했다
- `ocr_server/ocr_server.py`
  sidecar rec dump metadata에
  - `crop_box`
  - `crop_width`
  - `crop_height`
  를 추가했다.
- `app/native/paddle_bridge/bridge.cc`
  FFI rec dump metadata에도
  - `crop_quad`
  - `crop_width`
  - `crop_height`
  를 추가했다.
- 검증은
  - `python -m py_compile ocr_server/ocr_server.py`
  - `cargo test -p buzhidao --features paddle-ffi --no-run`
  로 다시 확인했다.

### title / `word processor`는 quad 단계에서 아직 차이가 남는다
- title
  - sidecar polygon: `[[133,24],[421,22],[421,62],[133,64]]`
  - FFI polygon: `[[133,25],[421,21],[421,62],[134,65]]`
  - sidecar `crop_box`:
    `[[132.7223,24.0019],[421.0000,21.9999],[421.2778,61.9980],[133.0001,64.0000]]`
  - FFI `crop_quad`:
    `[[133.0000,25.0],[421.0000,21.0],[421.5694,61.9921],[133.5694,65.9921]]`
  - crop size:
    - sidecar `288x39`, `rec_width=354`
    - FFI `288x40`, `rec_width=346`
- `word processor`
  - sidecar polygon: `[[121,505],[733,507],[733,554],[120,552]]`
  - FFI polygon: `[[121,506],[733,507],[733,554],[121,553]]`
  - sidecar `crop_box`:
    `[[120.1533,504.9973],[733.1533,506.9973],[732.9999,554.0],[119.99997,552.00006]]`
  - FFI `crop_quad`:
    `[[121.0000,506.0],[733.0767,507.0001],[732.9999,554.0],[120.9232,552.9999]]`
  - crop size:
    - sidecar `613x47`, `rec_width=626`
    - FFI `612x46`, `rec_width=639`

### 이번 턴 결론
- title / `word processor`는 rec predictor 차이가 아니라
  quad 자체가 아직 다르다는 점이 rec dump metadata로 확정됐다.
- 특히 FFI는 sidecar보다 box가 약간 아래쪽/바깥쪽으로 밀려 있고,
  그 결과 crop height / rec width가 다르게 나온다.
- 다음 타깃은 이제 `crop_to_bbox()` 내부 보간보다 앞단,
  즉 detector polygon을 sidecar `get_minarea_rect_crop()`가 기대하는 quad로
  더 가깝게 정규화하는 단계다.

## 2026-04-18 추가 진행 상황 41

### sidecar 최종 scale/clip 규약은 이미 같다는 점을 다시 확인했다
- sidecar Python dump 코드를 다시 확인하니,
  최종 detector box는 여전히
  `round(box[i, 0] * width_scale)`, `round(box[i, 1] * height_scale)`로 정수화한다.
- FFI도 `std::round(unclipped[i].x * x_scale)`, `std::round(unclipped[i].y * y_scale)`를 쓰고 있어서,
  남은 1px 차이는 마지막 scale/clip 수식 때문이 아니다.

### 남은 차이는 `unclip` 이전의 point set에서 이미 결정된다
- title, `word processor` 모두 sidecar/FFI `mini_box`는 이미 거의 같다.
  - title
    - sidecar mini-box:
      `[[143.9591,36.6608],[403.9077,34.2084],[404.0685,51.2540],[144.1199,53.7064]]`
    - FFI rect:
      `[[143.9461,37.0142],[403.8581,33.5019],[404.0876,50.4853],[144.1756,53.9976]]`
  - `word processor`
    - sidecar mini-box:
      `[[134.0188,528.9050],[710.0366,530.2414],[709.9923,549.3294],[133.9745,547.9930]]`
    - FFI rect:
      `[[134.0187,528.9051],[710.0365,530.2385],[709.9923,549.3264],[133.9745,547.9930]]`
- 그런데 `unclipped_box`는 여전히 의미 있게 갈린다.
  - title
    - sidecar:
      `[[130.9008,24.0931],[415.7839,21.9016],[416.0993,62.9069],[131.2162,65.0984]]`
    - FFI:
      `[[131.6319,25.1081],[415.8941,21.8281],[416.3675,62.8573],[132.1053,66.1372]]`
  - `word processor`
    - sidecar:
      `[[119.1113,513.9483],[724.0519,516.0488],[723.8887,563.0517],[118.9481,560.9512]]`
    - FFI:
      `[[120.0209,514.9722],[724.0607,516.0209],[723.9791,563.0276],[119.9393,561.9789]]`
- 즉 지금 남은 차이는 `findContours(..., CHAIN_APPROX_SIMPLE)`가 만드는 contour와
  그 contour를 `get_mini_boxes()`/`unclip()`에 넣기 전 point set 자체에서 이미 결정된다.

### `unclip` 후처리만 sidecar식으로 바꾸는 실험은 회귀였다
- FFI `unclip()`이 inflate polygon을 custom `min_area_rect()`로 사각형화하는 대신,
  sidecar처럼 `minAreaRect/boxPoints` 쪽으로 정렬해 반환해봤다.
- 결과는 `test3.png`는 유지됐지만 `test2.png` 긴 줄이 오히려 더 흔들렸다.
  - 기존:
    `中文，中国话。(=(中語)，叫(支那語)，叫(華語)，o(chinese))`
  - 실험 후:
    `中文，中国话 。(=叫(中語)，ł叫(支那語)，叫(華語)，|(chinese))`
- top-left `"0"`와 `word processor` 줄은 그대로였고,
  전반적으로 개선보다 회귀가 커서 이 변경은 되돌렸다.

### 이번 턴 결론
- `unclip` 후처리 규약만 바꿔서는 남은 `test2` 차이를 못 닫는다.
- detector polygon 오차는 `inflate` 이후가 아니라
  `findContours(..., CHAIN_APPROX_SIMPLE)`가 만드는 원 contour 단계에서 이미 생긴다.
- 다음 유효한 단계는 `trace_component_contour()`와 OpenCV contour의 좌표계를
  더 직접 맞추는 쪽이다.

## 2026-04-18 추가 진행 상황 42

### sidecar `pyclipper`는 `round`가 아니라 `trunc/floor` 계열 정수화와 맞는다
- sidecar venv에서 `pyclipper`를 직접 재현해 보니,
  title / `word processor` 모두
  float `mini_box`를 그대로 `AddPath()`에 넣었을 때 결과는
  `round()`가 아니라 `floor()/int()`와 같은 polygon을 만든다.
- 특히 `unclip_ratio=1.5` 기준으로
  sidecar dump의 `unclipped_box`는 정확히 `floor/trunc` 경로와 일치했다.
  - title sidecar `unclipped_box`:
    `[[130.9008,24.0931],[415.7839,21.9016],[416.0993,62.9069],[131.2162,65.0984]]`
  - `word processor` sidecar `unclipped_box`:
    `[[119.1113,513.9483],[724.0519,516.0488],[723.8887,563.0517],[118.9481,560.9512]]`
- 현재 FFI는 Clipper2 입력을 `std::llround()`로 정수화하고 있어서
  이 단계에서 이미 sidecar와 다른 polygon을 만들고 있었다.

### 하지만 전역 `llround -> trunc` 치환은 `test2`를 더 나쁘게 만들었다
- FFI `unclip()` 입력을 `static_cast<int64_t>(pt.x/y)`로 바꿔봤다.
- 결과:
  - `test3.png`는 유지됐다.
  - `word processor` polygon은 sidecar와 정확히 같은
    `[[121,505],[733,507],[733,554],[120,552]]`로 맞았다.
  - 하지만 title과 긴 줄은 오히려 회귀했다.
    - title:
      `中國語 [] 》 -> 中國語 []`
    - 긴 줄:
      `...o(chinese)) -> ...叫(華语)，(e))`
- 즉 sidecar `pyclipper` 정수화 규약을 찾은 건 맞지만,
  지금 FFI 전체에 그대로 적용하면 다른 후보들의 detector/crop 균형이 깨진다.
  그래서 이 변경은 되돌렸다.

### 이번 턴 결론
- `unclip()` 입력 정수화 규약 차이는 실제로 존재한다.
- 다만 남은 `test2` 차이는 그 한 점만으로 닫히지 않고,
  detector polygon/mini-box와 함께 얽혀 있다.
- 다음 단계도 heuristic으로 분기하지 말고,
  `trace_component_contour()`가 sidecar contour와 다르게 만드는 후보를
  더 직접 줄이는 쪽으로 가야 한다.

## 2026-04-18 추가 진행 상황 43

### Clipper2 `PathsD` 실험도 sidecar 정합성으로 이어지지 않았다
- `unclip()`에서 `Path64 + llround` 대신
  Clipper2의 `PathsD` overload를 써서
  float 좌표를 유지한 채 offset을 해봤다.
- 기대는 sidecar `pyclipper(float path)`에 더 가까워지는 것이었지만,
  실제 결과는 그렇지 않았다.

### `test2.png`는 일부 좋아져도 전체적으로 더 불안정해졌다
- 긴 줄은 기존보다 일부 자연스러워졌다.
  - `...叫(華語)，o(chinese))` 복구
- 하지만 전체 결과는 더 나빠졌다.
  - title:
    `中國語 [] 》 -> 中國語 [] `
  - top-left `0` 후보 자체가 빠졌다.
  - `[[15,266],[125,266],[125,314],[15,314]]`가 새로 accepted되며 `"舌"`이 추가됐다.
  - `word processor` 줄도 `processor -> procesor`로 흔들렸다.
  - `[[214,755],[281,786]]` 같은 extra false positive `"个"`도 생겼다.

### `test3.png`도 geometry가 다시 흔들렸다
- icon 자체는 `"💯"`를 유지했지만,
  detector polygon이 다시 커졌다.
  - baseline:
    `[[472,569],[506,569],[506,595],[472,595]]`
  - `PathsD` 실험:
    `[[472,569],[507,569],[507,596],[472,596]]`
- 다른 line box들도 1px씩 더 바깥으로 벌어지는 경향이 있었다.

### 이번 턴 결론
- Clipper2 `PathsD`는 `pyclipper(float path)`를 자동으로 재현해주지 않는다.
- 남은 차이는 여전히 offset 구현보다 앞단,
  즉 detector contour/mini-box 단계에 더 가깝다.
- 그래서 `PathsD` 실험도 되돌렸고,
  현재 stable baseline은 유지된다.

## 2026-04-18 추가 진행 상황 44

### sidecar/FFI 모두 같은 component bitmap dump를 붙여서 비교했다
- FFI `ffi_det_contours_*.json`에
  `component_bbox`, `component_pixels`를 추가했다.
- sidecar `sidecar_det_contours_*.json`에도 같은 필드를 추가했다.
- 이걸로 동일 후보에 대해 contour만이 아니라
  contour의 입력이 되는 connected-component bitmap 자체를 직접 비교할 수 있게 됐다.

### FFI contour tracing은 이제 같은 bitmap에 대해 OpenCV와 완전히 일치한다
- FFI dump에서 title / long line / `word processor` 후보의
  `component_pixels`로 bitmap을 다시 만들고,
  Python OpenCV `findContours(..., CHAIN_APPROX_SIMPLE)`를 돌려봤다.
- 결과:
  - title: FFI contour `58`점, OpenCV contour `58`점, set diff `0`
  - long line: FFI contour `42`점, OpenCV contour `42`점, set diff `0`
  - `word processor`: FFI contour `78`점, OpenCV contour `78`점, set diff `0`
- 즉 현재 FFI의 남은 차이는 더 이상 `trace_component_contour()` 축약 규약이 아니다.
  같은 bitmap에 대해서는 sidecar/OpenCV와 같은 contour를 만든다.

### 남은 차이는 contour 이전의 component bitmap 자체에 있다
- 같은 후보의 `component_bbox`는 sidecar/FFI가 동일했다.
  - title: `[144,35,405,54]`
  - long line: `[28,125,1046,141]`
  - `word processor`: `[134,529,711,550]`
- 하지만 `component_pixels`는 몇 픽셀씩 다르다.
  - title: sidecar `4084`, FFI `4071`
    - sidecar-only `13`, FFI-only `0`
  - long line: sidecar `13761`, FFI `13769`
    - sidecar-only `1`, FFI-only `9`
  - `word processor`: sidecar `8957`, FFI `8962`
    - sidecar-only `3`, FFI-only `8`
- 이 차이가 그대로 detector polygon의 `1px` 차이와 rec tensor 차이로 이어진다.

### 이번 턴 결론
- 남은 `test2` 차이는 contour tracer가 아니라,
  contour에 들어가기 전의 binary bitmap / connected-component membership 단계다.
- 다음 단계는 `findContours` 이후가 아니라,
  `pred > threshold` 이후 bitmap에서 sidecar/FFI가 달라지는
  경계 픽셀의 점수와 이웃 연결 상태를 직접 덤프해 보는 쪽이다.

## 2026-04-18 추가 진행 상황 45

### 경계 픽셀 차이는 실제로 det probability map 자체에서 나온다
- sidecar/FFI det contour dump에 `component_pred`, `component_bitmap`을 추가해서
  같은 component bbox 안의 raw probability map을 직접 비교했다.
- 결과적으로 남은 차이는 대부분 `0.3` threshold 근처의 몇 픽셀에서만 발생한다.
  - title:
    sidecar-only 13픽셀, 각 픽셀 sidecar `0.304 ~ 0.357`, FFI `0.234 ~ 0.299`
  - long line:
    sidecar-only 1, FFI-only 9
    - 예: `(997,140)` sidecar `0.2305`, FFI `0.4902`
  - `word processor`:
    sidecar-only 3, FFI-only 8
    - 예: `(568,549)` sidecar `0.4872`, FFI `0.2822`
- 전체 bbox 평균 차이는 아주 작다.
  - title mean abs diff `~0.00112`
  - long line `~0.00117`
  - `word processor` `~0.00094`
- 하지만 국소적으로는 `0.07 ~ 0.26`까지 튀는 픽셀이 있고,
  이게 component membership과 polygon의 `1px` 차이로 이어진다.

### detector 입력 이미지는 sidecar/FFI가 사실상 같다
- sidecar `Resize` 직후 이미지와 FFI `resize_for_det()` 결과를
  각각 `sidecar_det_input_*.png`, `ffi_det_input_*.bmp`로 저장해 직접 비교했다.
- `test2.png`의 resized detector input은 둘 다 `1056x896`이고,
  픽셀 차이는 최대 `1` LSB 수준이었다.
  - mean abs diff `~0.0090`
  - max abs diff `1`
  - nonzero pixel `19486`, 모두 `1`단계 밝기 차이
- 즉 남은 차이는 큰 전처리 mismatch가 아니라,
  거의 같은 입력에서 Paddle sidecar와 FFI predictor가 경계 픽셀에서
  미세하게 다른 det output을 내는 쪽으로 좁혀졌다.

### 이번 턴 결론
- detector contour나 resize 규약이 아니라,
  det predictor output의 국소 수치 차이가 남은 `test2` 오차의 직접 원인이다.
- 다음 단계는 sidecar/FFI detector의 raw output tensor를 같은 입력 기준으로
  직접 재주입/비교하거나, FFI가 sidecar와 같은 backend/옵션 조합을 쓰는지
  더 좁혀보는 쪽이다.

## 2026-04-18 추가 진행 상황 46

### detector raw output 재주입 비교를 붙였다
- `ocr_server.py`에 `--compare-ffi-det-dump` CLI를 추가했다.
- FFI `det_dump_*.json`의 `input_values`를 그대로 sidecar
  `text_det_model.infer()`에 재주입하고,
  raw detector map을 FFI dump와 직접 비교한다.
- 비교 결과는 `sidecar_vs_ffi_det_dump_*.json`으로 저장된다.

### 같은 detector input tensor에서는 sidecar/FFI raw map이 완전히 같다
- `det_dump_0_1069x881_to_1056x896_pred_1056x896.json` 기준 비교 결과:
  - mean abs diff `0.0`
  - max abs diff `0.0`
  - threshold disagreement `0`
  - `sidecar_on == ffi_on == 67399`
- 즉 detector predictor 자체, 그리고 predictor backend inference 결과도
  현재는 sidecar와 FFI가 같다고 볼 수 있다.

### detector input image도 사실상 같다
- sidecar `sidecar_det_input_0_0_1056x896.png`와
  FFI `ffi_det_input_0_1056x896.bmp`를 직접 비교했다.
- 크기는 둘 다 `1056x896`, pixel diff는 최대 `1` LSB였다.
  - mean abs diff `~0.0090`
  - max abs diff `1`

### 이번 턴 결론
- 이제 detector 쪽은 사실상 닫혔다.
  - resize image 거의 동일
  - detector raw output tensor 완전 동일
  - 같은 bitmap이면 contour도 동일
- 따라서 남아 있는 `test2` 차이는 detector 구현이 아니라
  현재 sidecar instrumentation에서 후보/bitmap을 매핑하는 방식,
  혹은 det dump와 end-to-end final result를 연결하는 계측 해석 문제일 가능성이 크다.
- 이 지점부터 더 파는 건 parity 구현보다
  계측 정합성 정리에 가까운 작업이 된다.
