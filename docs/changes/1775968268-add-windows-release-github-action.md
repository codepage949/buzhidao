# Windows 릴리스 GitHub Actions 추가

## 배경

- 현재 저장소에는 GitHub Actions 기반 배포 파이프라인이 없다.
- 배포 기준 흐름은 `workflow_dispatch -> 버전 반영 -> 빌드 -> 아카이브 업로드 -> GitHub Release 생성` 순서를 따른다.
- OCR GPU 빌드는 `--features gpu`와 `cuda/` 런타임 포함 여부가 CPU 빌드와 다르므로 별도 산출물 관리가 필요하다.
- zip 배포를 사용할 경우 실행 시 `models/`를 번들 리소스 외에 실행 파일 옆에서도 찾을 수 있어야 한다.

## 구현 계획

1. 릴리스 버전 입력값으로 `Cargo.toml`과 `tauri.conf.json` 버전을 동기화하는 보조 스크립트를 추가한다.
2. Windows 전용 `release.yml` workflow를 추가하고 CPU/GPU 매트릭스 빌드 및 zip 아카이브 업로드를 구성한다.
3. zip 배포에서도 앱이 `models/`를 찾도록 실행 파일 옆 리소스 fallback을 추가한다.
4. 보조 스크립트와 Rust 변경에 대한 테스트를 추가하고 실행한다.

## 구현 내용

- `scripts/release_helper.py`
  - `set-version`: `v1.2.3` 입력을 받아 Cargo/Tauri 버전을 `1.2.3`으로 동기화
  - `make-archive`: Windows CPU/GPU 아카이브 생성, GPU 모드에서만 `cuda/` 포함
  - `extract-cuda`: NVIDIA wheel에서 배포에 필요한 CUDA/cuDNN DLL만 추출
- `.github/workflows/release.yml`
  - `workflow_dispatch` 입력 버전으로 릴리스 실행
  - prepare job에서 버전 반영, `Cargo.lock` 갱신, 커밋/푸시, release SHA 캡처
  - models job에서 Docker 기반으로 ONNX 모델을 직접 생성하고 workflow artifact로 업로드
  - build job에서 Windows `cpu` / `gpu` 매트릭스 빌드, `ui` 빌드 후 zip 업로드
  - 각 Windows build job은 생성된 `models/` artifact를 받아 패키징에 포함
  - GPU build job에서 PyPI wheel로부터 CUDA/cuDNN redistributable DLL을 다운로드해 `cuda/` 구성
  - release job에서 태그 생성, 릴리스 노트 생성, GitHub Release 게시
- `src/lib.rs`
  - `resolve_models_dir` 헬퍼 추가
  - 번들 리소스 디렉토리에 `models/`가 없으면 실행 파일 옆 `models/`를 fallback으로 사용
- `README.md`
  - GitHub Actions 릴리스와 CPU/GPU 아티팩트 차이를 문서화

## 테스트 계획

- `python -m unittest scripts.test_release_helper`
- `cargo test`

## 테스트 결과

- `cargo test` 통과
  - 54개 테스트 성공
- `python -m unittest scripts.test_release_helper`
  - 로컬 환경에 실행 가능한 Python 인터프리터가 없어 검증하지 못함
  - CI에서는 `actions/setup-python`으로 실행 가능하도록 workflow를 구성함

## 리팩토링 검토

- 보조 스크립트를 별도 파일로 분리해 workflow inline 스크립트 중복을 줄인다.
- 현재 범위에서는 추가 리팩토링 필요성 없음.
