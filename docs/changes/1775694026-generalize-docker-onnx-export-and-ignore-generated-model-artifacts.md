# ONNX 모델 변환을 Docker 기반으로 일반화하고 생성 모델 산출물을 Git 관리에서 제외

## 배경

Windows에서 `paddle2onnx`와 관련 Python 의존성이 연쇄적으로 누락되어
로컬 가상환경 기반 변환 경로의 유지 비용이 커졌다.

원래 Docker를 사용했던 이유는 운영체제별 Python wheel, DLL, 빌드 의존성 차이를
컨테이너 내부 Linux 환경으로 고정하기 위해서였다.

이번 변경의 목표는 Docker 기반 접근으로 되돌리되,
Windows 전용 `%cd%` 같은 셸 문법이나 수동 명령 조합에 기대지 않도록
호스트 실행 경로를 일반화하는 것이다.

추가로 ONNX와 사전 파일은 생성 산출물이므로 Git에서 직접 관리하지 않도록 정리한다.

## 구현 계획

1. `export_onnx.py`를 호스트용 Docker 런처로 재정의한다.
2. `export_onnx_docker.py`를 컨테이너 내부 변환 전용 스크립트로 복원한다.
3. Docker 명령은 셸 문자열이 아니라 인자 배열로 구성해 운영체제별 quoting 차이를 줄인다.
4. `rec_dict.txt`가 모델 패키지 내 YAML에만 있는 경우도 추출되도록 보완한다.
5. 생성 모델 산출물과 Python 캐시를 Git 관리에서 제외한다.

## 구현

- `scripts/export_onnx.py`
  - 호스트 진입점으로 역할 변경
  - Docker 존재 여부 확인 및 설치 안내 출력
  - `scripts/`와 `models/`를 절대 경로로 마운트
  - `docker run` 명령을 `subprocess` 인자 배열로 구성
  - 기본 이미지 `python:3.11-slim` 사용
  - 컨테이너 내부에서 `libgomp1`, `paddlepaddle`, `paddle2onnx`, `packaging`, `setuptools` 설치 후 변환 스크립트 실행
  - `--print-only` 옵션으로 실제 실행 없이 최종 Docker 명령 확인 가능
- `scripts/export_onnx_docker.py`
  - 컨테이너 내부 변환 로직 담당
  - Paddle 3.x `inference.json` 형식과 `.pdmodel` 형식 모두 처리 유지
  - `rec_dict.txt` 추출 시 별도 dict 파일, JSON config, YAML config를 순서대로 탐색
  - `inference.yml`의 `character_dict` 리스트를 직접 파싱해 `rec_dict.txt` 생성
- `.gitignore`
  - `models/*.onnx`
  - `models/rec_dict.txt`
  - `__pycache__/`
  - `*.py[cod]`
  - `scripts/_export_tmp/`
- Git 인덱스
  - 이미 추적 중이던 `models/rec_dict.txt`를 인덱스에서 제거해 이후 생성 파일로 취급

## 테스트 계획

- `python -m py_compile scripts/export_onnx.py scripts/export_onnx_docker.py`
- `python scripts/export_onnx.py --help`
- `python scripts/export_onnx.py --print-only`
- `export_onnx_docker.extract_dict()`로 `inference.yml` 기반 사전 추출 검증

## 테스트 결과

- `uv run --no-project --python 3.11 python -m py_compile scripts/export_onnx.py scripts/export_onnx_docker.py`
- `uv run --no-project --python 3.11 scripts/export_onnx.py --help`
- `uv run --no-project --python 3.11 scripts/export_onnx.py --print-only`
- `uv run --no-project --python 3.11 python -c "... import scripts.export_onnx_docker as m; m.extract_dict(...)"`로 `18383`줄 `rec_dict.txt` 생성 확인

이 환경에서는 실제 Docker 변환 전체는 수행하지 않았고,
호스트에서 생성되는 Docker 명령, 스크립트 문법, YAML 기반 사전 추출을 검증했다.

## 리팩토링 검토

호스트 실행 책임과 컨테이너 내부 변환 책임이 분리되어 있고,
생성 산출물도 Git 관리 대상에서 제외되어 추가 리팩토링 필요성은 크지 않다.
