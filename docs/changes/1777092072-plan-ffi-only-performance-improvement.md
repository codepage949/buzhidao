# FFI 단독 성능 개선 계획

## 배경

- 지금까지는 sidecar와 ffi의 동작 차이를 줄이는 작업이 우선이었다.
- 그 과정에서 parity와 전처리 정책 차이는 상당 부분 정리됐다.
- 이제 다음 단계는 sidecar와의 상대 비교보다, FFI 자체의 전후 비교를 기준으로 성능을 올리는 것이다.

## 이번 계획의 목표

- sidecar를 기준선으로 삼지 않는다.
- 같은 FFI 경로에서 전후 수치를 비교하며 점진적으로 성능을 올린다.
- 한 번에 하나의 영역만 바꾸고, 매 단계마다 수치와 parity를 확인한다.

## 기본 원칙

1. 각 단계는 FFI 내부의 한 축만 건드린다.
2. 성공 여부는 "같은 FFI 경로의 전후 비교"로 판단한다.
3. parity가 깨지면 그 단계는 되돌린다.
4. 체감 개선이 없으면 다음 축으로 넘어간다.
5. sidecar는 참고용일 뿐, 이번 라운드의 의사결정 기준은 아니다.

## 기준 입력과 측정 기준

### 기준 입력

- 우선 기준 fixture는 `testdata/ocr/test5.png`
- 실제 프로그램 로그의 대표 스크린샷 한 장 이상

### 기준 측정값

- 앱 로그
  - `prepare_image_ms`
  - `ffi_ms`
  - `spawn_wait_ms`
- native stage profile
  - `det_ms`
  - `cls_ms`
  - `rec_ms`
  - `total_ms`

### 비교 방식

- 같은 입력
- 같은 warmup 조건
- 같은 iteration 수
- 변경 전 / 변경 후 비교

## 단계별 계획

## 1단계: FFI 측정 기준 고정

### 목적

- 이후 모든 최적화의 기준이 흔들리지 않게 한다.

### 작업

- `test5.png` 기준 FFI benchmark를 고정한다.
- 실제 프로그램 로그에서 대표 입력의 `ffi_ms`와 stage profile을 함께 확보한다.
- 비교 스크립트와 앱 로그에서 같은 이름의 수치를 보게 맞춘다.

### 성공 기준

- 다음 단계부터 항상 같은 입력과 같은 형식의 수치를 비교할 수 있다.

### 중단 조건

- 입력 조건이 자꾸 바뀌면 이 단계부터 다시 고정한다.

## 2단계: FFI 입력 준비 미세 최적화

### 목적

- predictor 실행 전의 불필요한 메모리 이동을 줄인다.

### 작업 후보

- Rust -> native 메모리 전달 경로 점검
- RGBA/BGRA 해석 경계 재점검
- det/rec 전처리 직전의 큰 copy 제거

### 성공 기준

- parity 유지
- `prepare_image_ms` 또는 stage 전 준비 비용 감소

### 중단 조건

- 수치 개선이 미미하고 구조 복잡도만 커지면 멈춘다.

## 3단계: rec 경로 집중 최적화

### 목적

- 현재 가장 무거울 가능성이 큰 rec stage를 직접 줄인다.

### 작업 후보

- rec batch 준비
- tensor fill
- dynamic width 처리
- rec predictor 입력 shape 전략

### 성공 기준

- parity 유지
- `rec_ms` 감소

### 중단 조건

- predictor 설정 변경이 parity를 흔들면 해당 시도는 폐기한다.

## 4단계: det 경로 집중 최적화

### 목적

- det stage의 resize / preprocess / predictor 비용을 줄인다.

### 작업 후보

- det resize 경로
- det tensor 채우기
- predictor 직전 메모리 배치

### 성공 기준

- parity 유지
- `det_ms` 감소

### 중단 조건

- 정확도나 detection count가 흔들리면 되돌린다.

## 5단계: 앱 경계 비용 재확인

### 목적

- FFI 코어가 빨라진 뒤에도 실제 프로그램 체감이 맞는지 확인한다.

### 작업

- 앱 로그의 `spawn_wait_ms`, `emit_ms`, total time 재측정
- 오버레이 시간 표시와 함께 실제 프로그램 경로를 다시 본다

### 성공 기준

- 코어 개선이 실제 프로그램 총 시간에도 반영된다.

### 중단 조건

- 코어는 줄었는데 체감이 그대로면, 다시 앱 경계로 돌아가 원인을 분리한다.

## 우선순위

1. 측정 기준 고정
2. 입력 준비 미세 최적화
3. rec 경로
4. det 경로
5. 앱 경계 재확인

## 이번 문서의 역할

- 이번 문서는 구현 자체가 아니라, 다음 FFI 최적화 라운드의 실행 기준을 고정하는 문서다.
- 이후 구현 턴에서는 이 순서대로 한 단계씩만 수행한다.

## 진행 현황

### 1단계 FFI 측정 기준 고정

- 완료

### 실제 반영 내용

- `tools/scripts/ocr_sidecar_ffi.py`에 `profile-ffi` 명령을 추가했다.
- 이 명령은 sidecar 없이 FFI만 대상으로:
  - 단일 profile 실행
  - stage profile 수집
  - benchmark 요약
  를 한 번에 JSON으로 출력한다.
- profile line은 `profile_entries`로 파싱해서 후속 비교에 바로 사용할 수 있게 정리했다.

### 기준 명령

```powershell
python tools/scripts/ocr_sidecar_ffi.py profile-ffi --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline
```

### 현재 기준선

- `test5.png`
  - detection count: `45`
  - benchmark mean: `4080.55ms`
  - final pipeline profile:
    - `det_ms=805.421`
    - `cls_ms=253.299`
    - `rec_ms=3678.072`
    - `total_ms=4740.822`

### 해석

- `profile-ffi`는 이제 `1회 profiled run + 다회 unprofiled benchmark`로 분리돼서, stage 계측 오버헤드가 benchmark 평균을 오염시키지 않는다.
- 현재 FFI 단독 기준선에서 가장 큰 축은 여전히 `rec_ms`다.
- 따라서 다음 구현 라운드도 `rec` 경로를 계속 먼저 판다.

### 이번 라운드에서 실제로 남긴 개선

- `native/paddle_bridge/bridge.cc`
  - `run_rec_batch`의 input/output float 버퍼를 배치 내부에서 재사용 가능한 scratch buffer로 바꿨다.
  - predictor 출력 JSON dump에서도 불필요한 `single_values` 복사를 없앴다.
- 검증 결과
  - `test5.png` parity: `45/45 exact match`
  - benchmark mean: `4080.55ms -> 3888.79ms`
  - 약 `191.76ms`, `4.7%` 개선

### 이번 라운드에서 버린 시도

- `rec` scratch를 엔진 생애주기로 끌어올려 호출 간에도 재사용
  - 기대와 달리 benchmark가 다시 올라가서 유지하지 않았다.
  - 측정값: `4005.85ms`
  - 따라서 현재는 “배치 내부 재사용”까지만 남긴다.

### release 기준선 정리

- `tools/scripts/ocr_sidecar_ffi.py`
  - `compare`, `benchmark`, `verify-ffi`, `profile-ffi`에 `--cargo-profile debug|release`를 추가했다.
  - 기본값은 `release`다.
- 이유
  - 기존에는 benchmark는 release였지만, profiled 단일 실행과 compare는 debug `cargo test`를 타고 있었다.
  - 그래서 stage profile과 benchmark 평균을 바로 비교하기 어려웠다.
- 현재 release `profile-ffi` 기준(`test5.png`, `warmups=1`, `iterations=3`)
  - parity: `45/45 exact match`
  - pipeline profile:
    - `det_ms=848.111`
    - `cls_ms=258.964`
    - `rec_ms=4184.848`
    - `total_ms=5297.792`
- 해석
  - 이제 stage profile도 release 경로와 맞춰 볼 수 있다.
  - 남은 가장 큰 축은 여전히 `rec predictor_ms`다.

### 이번 라운드에서 확인한 것

- `BUZHIDAO_PADDLE_FFI_CLS_CPU_THREADS=4`
  - baseline보다 느렸다.
  - benchmark mean: `4146.58ms`
- `BUZHIDAO_PADDLE_FFI_DET_CPU_THREADS=6`
  - baseline보다 느렸다.
  - benchmark mean: `4090.61ms`
- `BUZHIDAO_PADDLE_FFI_REC_NEW_IR=1`
  - 크게 느려졌다.
  - benchmark mean: `7346.99ms`
- `BUZHIDAO_PADDLE_FFI_REC_MKLDNN=0`
  - 크게 느려졌다.
  - benchmark mean: `7302.25ms`
- `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET`
  - 실험용 환경변수로 추가했다.
  - `3000`은 일부 케이스에서 benchmark를 낮출 여지가 있었지만 `test5.png`에서 text exact match가 `44/45`로 흔들렸다.
  - `4200`은 exact match를 회복했지만 benchmark 이득이 사라졌다.
  - 그래서 기본값으로는 넣지 않고, 실험용 옵션으로만 남긴다.

### 이번 라운드에서 추가한 실험 도구

- `tools/scripts/ocr_sidecar_ffi.py`
  - `compare-ffi-self` 명령을 추가했다.
  - 이 명령은 sidecar 없이 같은 FFI baseline과 candidate env를 직접 비교한다.
  - 출력에는 다음이 함께 들어간다.
    - exact text match
    - baseline/candidate stage profile
    - baseline/candidate benchmark
    - mean delta
- 목적
  - 이제부터는 `FFI vs sidecar`가 아니라 `FFI baseline vs FFI candidate`를 같은 형식으로 반복 측정할 수 있다.

### rec OneDNN op 제한 실험

- `native/paddle_bridge/bridge.cc`
  - `BUZHIDAO_PADDLE_FFI_ONEDNN_OPS`
  - `BUZHIDAO_PADDLE_FFI_REC_ONEDNN_OPS`
  - 위 환경변수로 stage별 OneDNN op allowlist를 줄 수 있게 했다.
  - Paddle API는 `SetONEDNNOp(...)`를 사용한다.
- 함께 정리한 것
  - deprecated API였던 `EnableMKLDNN` / `SetMkldnnCacheCapacity`를
    `EnableONEDNN` / `SetOnednnCacheCapacity(10)`로 바꿨다.

### rec OneDNN op 제한 결과

- `test5.png` 단일 fixture에서 후보를 먼저 훑었다.
  - 후보:
    - `matmul`
    - `matmul_v2`
    - `matmul,matmul_v2`
    - `matmul,matmul_v2,fc`
    - `fc`
    - `conv2d,matmul,matmul_v2`
    - `conv2d,depthwise_conv2d,matmul,matmul_v2`
- 결과
  - 전부 parity는 유지됐다.
  - 하지만 강한 승자는 없었다.
  - `matmul,matmul_v2,fc`만 `test5.png` 단일 측정에서 약 `-104ms` 개선이 보였지만,
    대표 fixture 3개로 다시 보면:
    - `test3.png`: `+34.064ms` 느려짐
    - `test4.png`: `-92.192ms` 빨라짐
    - `test5.png`: `-16.533ms` 빨라짐
- 결론
  - 특정 fixture에서만 좋아지는 정도라 기본값으로 채택할 수준은 아니다.
  - 현재는 실험 훅만 남기고 제품 기본 설정은 유지한다.

### rec 사전/문자 집합 분석

- `tools/scripts/ocr_sidecar_ffi.py`
  - `analyze-ffi-corpus` 명령을 추가했다.
  - release FFI 앱 OCR 경로를 직접 실행해서:
    - 이미지별 detection count
    - 이미지별 unique chars
    - corpus 전체 unique chars
    - rec dict 크기 대비 coverage
    를 한 번에 JSON으로 뽑는다.
- `native/paddle_bridge/bridge.cc`
  - recognition dict는 `load_recognition_dict()`에서 모델 디렉터리의
    `rec_dict.txt`, `ppocr_keys_v1.txt`, `ppocr_keys_v2.txt`, `config.json`, `inference.yml` 등을 순서대로 읽는다.
  - 즉 현재 앱은 `PP-OCRv5_server_rec/config.json`의 `character_dict`를 그대로 dict로 쓸 수 있다.

### 저장소 fixture 전체 corpus 결과

- 대상
  - `test.png`
  - `test2.png`
  - `test3.png`
  - `test4.png`
  - `test5.png`
- 명령

```powershell
python tools/scripts/ocr_sidecar_ffi.py analyze-ffi-corpus --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --cargo-profile release
```

- 결과
  - `test.png`: detection `7`, unique chars `104`
  - `test2.png`: detection `10`, unique chars `52`
  - `test3.png`: detection `23`, unique chars `173`
  - `test4.png`: detection `34`, unique chars `80`
  - `test5.png`: detection `34`, unique chars `94`
  - corpus 전체:
    - detection `108`
    - unique chars `337`
- `PP-OCRv5_server_rec/config.json` 기준 dict 크기
  - `18383`
- coverage
  - `337 / 18383`
  - 약 `1.833%`

### dict 크기와 runtime class 수 차이

- stage profile의 `run_rec_batch` 로그에서는 현재 `num_classes=15631`이 반복적으로 찍힌다.
- 반면 모델 디렉터리의 `config.json`에서 읽은 `character_dict`는 `18383`자다.
- 즉 현재 패키지 기준으로도 “dict 크기”와 “runtime output class 수” 사이에 차이가 있다.
- 코드상 이 차이는 이미 `run_rec_batch dict/model class mismatch` debug 로그로 감지되도록 되어 있다.
- 해석
  - 지금 단계에서 custom dict만 줄여도 predictor 계산량이 바로 줄지는 않는다.
  - 실제 계산량을 줄이려면 결국 `num_classes` 자체가 작은 rec 모델이 필요하다.

### 현재 해석

- 저위험 runtime 토글은 계속 큰 이득을 못 주고 있다.
- 남은 큰 병목은 여전히 `rec predictor_ms`다.
- 그리고 저장소 fixture 전체 기준으로도 사용 문자 집합이 전체 사전에 비해 극단적으로 작다.
- 따라서 다음 큰 축은:
  1. 더 많은 실제 스크린샷으로 문자 집합 분석을 넓히기
  2. `server_rec` 사전/모델 축을 커스텀하는 가치가 있는지 판단하기
  3. 또는 품질 손실을 허용하는 빠른 모드를 제품 옵션으로 분리하기
- `BUZHIDAO_PADDLE_FFI_REC_MAX_W`
  - 실험용 override를 추가했다.
  - `960`, `1024` 모두 `test5.png`에서 exact match가 `44/45`로 흔들렸고 benchmark도 `~7.44s`대로 크게 악화됐다.
  - 마지막 wide batch를 강제로 줄이는 방향은 현재 경로에서는 효과가 없다고 판단한다.

### 검증

- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
- `python tools/scripts/ocr_sidecar_ffi.py profile-ffi --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`

### release 대표 fixture 재측정

- 기준 명령

```powershell
python tools/scripts/ocr_sidecar_ffi.py profile-ffi --image testdata/ocr/test3.png --image testdata/ocr/test4.png --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline --cargo-profile release
python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test3.png --image testdata/ocr/test4.png --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --cargo-profile release
```

- parity
  - `test3.png`: `23/23 exact match`
  - `test4.png`: `45/45 exact match`
  - `test5.png`: `45/45 exact match`

- release profile
  - `test3.png`
    - benchmark mean: `3130.51ms`
    - `det_ms=1150.675`
    - `cls_ms=120.752`
    - `rec_ms=2382.427`
    - `total_ms=3662.753`
  - `test4.png`
    - benchmark mean: `5549.30ms`
    - `det_ms=924.144`
    - `cls_ms=284.353`
    - `rec_ms=4551.820`
    - `total_ms=5766.833`
  - `test5.png`
    - benchmark mean: `5442.80ms`
    - `det_ms=907.340`
    - `cls_ms=265.196`
    - `rec_ms=3702.686`
    - `total_ms=4881.197`

- 해석
  - `test5.png`만의 특이값이 아니다.
  - 대표 fixture 3개 모두에서 가장 큰 축은 `rec_ms`다.
  - `det_ms`도 무시할 수준은 아니지만, 현재 우선순위는 계속 `rec predictor` 쪽이다.

### 이번 라운드에서 버린 구조 변경

- `rec predictor`를 `batch_w`별 clone cache로 분리하는 시도를 했다.
- 의도
  - 한 predictor가 `320`, `428`, `672`, `1117` 같은 서로 다른 폭을 번갈아 `Reshape`하면서 흔들리는 경로를 줄여 보려 했다.
- 결과
  - parity는 유지됐다.
  - 하지만 profiled run이 크게 악화됐다.
    - `test5.png` pipeline profile:
      - `rec_ms=6741.681`
      - `total_ms=17263.092`
  - benchmark mean도 기존 최저점보다 나빴다.
    - clone cache 시도: `4040.97ms`
    - 되돌림 후: `3891.02ms`
- 판단
  - predictor clone 생성/초기화 비용이 reshape/cache 이득보다 훨씬 컸다.
  - 이 방향은 유지하지 않는다.

### 이번 라운드에서 버린 설정 탐색

- 대표 `batch_w`를 직접 태우는 공격적인 `rec warmup`
  - `320x6`, `428x6`, `672x6`, `1117x3`까지 warmup에 넣어 봤다.
  - 결과는 오히려 worse였다.
  - `test5.png`, `warmups=0`, `iterations=1` 기준 profiled run이 `total_ms=10809.694`까지 악화됐다.
  - 유지하지 않는다.

- `rec` oneDNN cache capacity 확대 (`16`, `32`, `64`)
  - `test5.png`, release benchmark 기준 전부 크게 악화됐다.
  - 측정값은 `~10.4s ~ 10.7s` 수준으로 baseline보다 훨씬 느렸다.
  - 유지하지 않는다.

- 해석
  - 지금 남은 병목은 “warmup이 덜 됐다”거나 “oneDNN cache capacity가 작다” 수준으로 설명되지 않는다.
  - 다음 단계는 작은 runtime 설정 탐색보다, 모델/입력 정책 또는 Paddle predictor 경로 자체를 더 크게 보는 쪽이어야 한다.

### 모델/런타임 교체 실험

- 목적
  - `server_rec` 자체가 병목인지, 같은 공식 모델군 안에서 더 빠른 대체가 있는지 확인한다.

- `BUZHIDAO_PADDLE_FFI_MODEL_HINT=mobile`
  - 공식 `PP-OCRv5_mobile_rec_infer`를 쓰게 유도했다.
  - 결과
    - `test5.png` 기준 `rec_ms`와 전체 시간은 크게 줄었다.
    - 하지만 detection/recognition parity가 크게 깨졌다.
    - `sidecar_count=45`, `ffi_count=41`, `exact_text_matches=8`
  - 판단
    - 빠르지만 현재 중국어 UI 스크린샷 품질 기준으로는 사용할 수 없다.

- `BUZHIDAO_PADDLE_FFI_MODEL_HINT=doc`
  - 공식 `PP-OCRv4_server_rec_doc_infer`를 추가로 내려받아 시험했다.
  - 결과
    - `test5.png` parity는 `45/45 exact match`
    - 하지만 baseline보다 느렸다.
    - profile 예시: `rec_ms=4267.444`, `total_ms=5432.405`, benchmark mean `6413.077ms`
  - 판단
    - 품질은 유지되지만 성능 개선이 아니라서 채택하지 않는다.

- `OCR_SERVER_DEVICE=gpu`
  - 현재 장비(`RTX 3070 Laptop GPU`)에서 GPU 경로를 시험했다.
  - 결과
    - parity가 크게 깨졌다.
    - `sidecar_count=45`, `ffi_count=34`, `exact_text_matches=10`
    - 시간도 CPU baseline보다 명확히 낫지 않았다.
  - 판단
    - 현재 설정 그대로는 기본 경로로 사용할 수 없다.

- 해석
  - 남은 큰 병목은 결국 중국어 `server_rec` 모델의 predictor 경로다.
  - 현재 확보한 공식 대체 모델 중에서는 “충분히 빠르면서 품질도 유지”하는 드롭인 교체안이 없다.

### 이번 라운드에서 버린 shape 실험

- `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUCKET`
  - 배치 구성은 그대로 두고 `batch_w`만 더 큰 bucket으로 올려 predictor reshape churn을 줄여 보려 했다.
  - 실험값: `64`, `128`, `256`
  - 결과
    - detection count가 baseline과 달라졌다.
    - runtime도 baseline `~4.3s`대에서 `~13s`대로 크게 악화됐다.
  - 판단
    - 오른쪽 패딩만 늘리는 단순 shape 양자화도 현재 predictor cache 경로에서는 오히려 해롭다.
    - 코드에는 남기지 않고 바로 되돌렸다.

### 이번 라운드에서 유지한 변경

- `EnableMKLDNN` / `SetMkldnnCacheCapacity`
  - Paddle 3.x 런타임이 deprecated 경고를 계속 내고 있었다.
  - 최신 API인 `EnableONEDNN` / `SetOnednnCacheCapacity(10)`로 교체했다.
  - `test5.png`, release `profile-ffi` 기준으로 baseline과 동급 이상이었다.
  - 예시 측정
    - `det_ms=912.645`
    - `rec_ms=3951.403`
    - benchmark mean `4401.45ms` (`iterations=2`)
  - 판단
    - 큰 개선은 아니지만, 레거시 경로를 걷어냈고 성능 악화도 없다.
    - 이 변경은 유지한다.

### 이번 라운드에서 버린 설정 탐색

- `BUZHIDAO_PADDLE_FFI_REC_DISABLE_ONEDNN_FC_PASSES`
  - 의도: `rec` 모델의 FC/MatMul 계열 pass를 끊어서 predictor 경로를 단순화
  - 결과: 크게 악화
    - `rec_ms=7694.906`
    - `total_ms=9673.159`
  - 유지하지 않는다.

- `BUZHIDAO_PADDLE_FFI_REC_OPT_LEVEL`
  - `1`, `2`, `4`를 시험했다.
  - 결과
    - `1`: `rec_ms=32951.199`, `total_ms=34224.434`
    - `2`: `rec_ms=5175.029`, benchmark mean `5274.69ms`
    - `4`: benchmark mean `4404.52ms`로 baseline 대비 이득이 없었다.
  - 판단
    - 최적화 레벨 축은 닫는다.

- `BUZHIDAO_PADDLE_FFI_DET_MKLDNN=0`
- `BUZHIDAO_PADDLE_FFI_CLS_MKLDNN=0`
  - det/cls에서만 OneDNN을 빼는 실험
  - 결과: 둘 다 전체 시간이 `~8s`대로 크게 악화
  - 유지하지 않는다.

- `BUZHIDAO_PADDLE_FFI_REC_ONEDNN_CACHE_CAPACITY=0`
- `BUZHIDAO_PADDLE_FFI_REC_ONEDNN_CACHE_CAPACITY=4`
  - rec oneDNN cache 용량을 기본값 `10`보다 줄여 봤다.
  - 결과: 둘 다 전체 시간이 `~8s`대로 크게 악화
  - 유지하지 않는다.

### FFI self-compare 확인

- sidecar 기준이 아니라 현재 FFI baseline 자신을 기준으로 batching 실험을 다시 봤다.
- `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET`
  - `2200`, `2600`, `3000`, `3400`, `3800`
  - 결과: detection count는 같아도 텍스트 시퀀스가 baseline과 전부 달랐다.
- 해석
  - 현재 경로에서는 rec batch 묶는 규칙을 건드리면 sidecar parity 여부와 별개로 FFI baseline 자체도 흔들린다.
