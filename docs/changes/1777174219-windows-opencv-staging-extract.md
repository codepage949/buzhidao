# Windows OpenCV SDK 추출 대기 원인 완화

## 배경

GitHub Actions Windows 빌드의 `Prepare native SDKs` 단계가 20분 이상 진행되는 문제가 있었다.

해당 단계는 Paddle Inference, OpenCV Windows SDK, pyclipper 소스를 준비한다.

로컬 Windows에서 확인한 결과, OpenCV self-extract exe는 짧은 대상 경로에서는 빠르게 완료되지만
긴 대상 경로에서는 파일 대부분을 쓴 뒤에도 종료하지 않고 대기할 수 있었다. 대상 경로가 길면
OpenCV 샘플 파일의 상대 경로와 합쳐져 Windows legacy `MAX_PATH` 경계를 넘는다.

## 계획

- GitHub Actions 로그에서 Python 출력이 즉시 보이도록 한다.
- 기존 OpenCV SDK가 유효하면 다운로드 전에 바로 재사용한다.
- Windows OpenCV self-extract exe를 최종 destination에 직접 풀지 않는다.
- 짧은 임시 staging 경로에 먼저 추출하고, 검증 후 최종 SDK 경로로 복사한다.
- 추출 단계에는 timeout을 적용해 비정상 장기 대기를 명확한 실패로 바꾼다.

## 구현

- `setup_paddle_inference.py` 실행은 `python -u`로 바꿔 GitHub Actions 로그 버퍼링을 줄인다.
- Windows OpenCV SDK 준비 순서를 바꾼다.
  - 기존 OpenCV SDK가 유효하면 다운로드 전에 재사용한다.
  - self-extract exe는 OS 임시 디렉터리 아래 짧은 staging 경로에 먼저 추출한다.
  - staging SDK를 검증한 뒤 최종 `opencv-sdk/<platform>` 경로에는 `opencv/build`만 복사한다.
  - self-extract exe 실행에는 timeout을 적용한다.
- 모든 주요 단계 로그에 UTC timestamp를 붙이고 즉시 flush한다.

## 검증 결과

- 로컬 Windows self-extract 비교
  - 짧은 임시 경로: 약 20초에 완료.
  - 긴 workspace probe 경로: 120초 timeout, 파일 대부분을 쓴 뒤 프로세스가 종료되지 않음.
- 로컬 Windows fresh destination probe
  - 긴 최종 경로에서도 staging 추출 후 `opencv/build`만 배치하면 약 27초에 완료.
  - 최종 SDK에는 `opencv/sources`가 남지 않고 `opencv/build`만 남는 것을 확인.
- `python -m unittest tools.scripts.test_setup_paddle_inference`
  - 결과: 통과.
- `python -m unittest tools.scripts.test_release_workflow`
  - 결과: 통과.
- `git diff --check`
  - 결과: 통과.
