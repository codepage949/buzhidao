# GitHub Actions Node 24 전환

## 계획

GitHub Actions runner는 Node.js 20 action에 대해 deprecation 경고를 표시한다. GitHub 공지에 따르면 Node.js 24를 미리 사용하려면 workflow env로 강제할 수 있지만, 최신 action 버전이 Node.js 24를 지원한다면 action 버전을 올리는 것이 우선이다.

현재 release workflow에서 확인된 Node.js 20 기반 action은 다음과 같다.

- `actions/cache@v4`
- `actions/download-artifact@v6`

## 변경 방향

- `actions/cache@v4`를 Node.js 24 기반 `actions/cache@v5`로 업데이트한다.
- `actions/download-artifact@v6`를 Node.js 24 기반 `actions/download-artifact@v8`로 업데이트한다.
- 이미 Node.js 24 또는 composite action으로 동작하는 action은 변경하지 않는다.

## 검증 계획

- release workflow 정적 테스트로 Node.js 20 기반 action 버전이 남아 있지 않은지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

