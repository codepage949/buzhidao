# Release candidate checkout 보호 오류 수정

## 계획

릴리스 workflow의 `build`와 `release` job은 base SHA를 checkout한 뒤 `release-candidate.bundle`을 fetch하고 release candidate commit으로 전환한다. 이때 workspace에 `tauri.conf.json` 같은 버전 갱신 대상 파일의 로컬 변경이 남아 있으면 일반 `git checkout --detach FETCH_HEAD`는 변경 덮어쓰기를 막기 위해 중단된다.

GitHub Actions job workspace는 해당 job 내부에서 생성된 임시 작업 공간이며, 이 단계의 목적은 반드시 release candidate commit으로 전환하는 것이다. 따라서 release candidate checkout 단계에서는 작업 트리의 로컬 변경을 보존하지 않고 후보 커밋을 기준으로 강제 전환한다.

## 변경 방향

- `build` job의 release candidate checkout을 `git checkout --force --detach FETCH_HEAD`로 변경한다.
- `release` job의 release candidate checkout도 동일하게 변경한다.
- release candidate SHA 검증은 유지해 잘못된 bundle 또는 잘못된 commit으로 전환되는 경우를 계속 차단한다.

## 검증 계획

- release workflow 정적 테스트로 release candidate checkout이 두 곳 모두 강제 전환을 사용하도록 확인한다.
- 임시 git clone에서 dirty tracked file이 있는 상태로 bundle fetch 후 forced checkout이 성공하는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

