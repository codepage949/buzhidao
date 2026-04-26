# Deno setup action 제거 및 재시도 설치

## 계획

`denoland/setup-deno@v2` action이 자체 다운로드 단계에서 실패하면 workflow의 shell retry wrapper까지 도달하지 못한다. `uses:` action step은 `ci_retry`로 감쌀 수 없으므로, Deno 설치를 repository가 제어하는 스크립트로 옮긴다.

## 변경 방향

- `denoland/setup-deno@v2` 사용을 제거한다.
- `tools/scripts/setup_deno.py`를 추가해 Windows/Linux용 Deno release zip을 직접 다운로드한다.
- `v2.x` 입력은 GitHub releases API에서 최신 v2 태그를 찾아 해석한다.
- release API 조회와 zip 다운로드에 재시도 및 timeout을 적용한다.
- 설치 디렉터리의 `bin` 경로를 `GITHUB_PATH`에 기록해 후속 step에서 `deno`를 사용할 수 있게 한다.

## 검증 계획

- Deno 설치 스크립트 단위 테스트를 추가한다.
- release workflow 정적 테스트로 `denoland/setup-deno`가 남지 않고 자체 설치 스크립트가 사용되는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow tools.scripts.test_setup_deno`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

