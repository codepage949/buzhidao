# 릴리스 단일 빌드 흐름 적용

## 계획

기존 release workflow는 base SHA에서 `verify` job으로 한 번 빌드하고, 그 뒤 `version` job에서 release candidate commit을 만든 다음 `build` job에서 릴리스 산출물을 다시 빌드했다. release candidate commit이 main에 즉시 push되지 않도록 이미 분리되어 있으므로, 검증과 산출물 생성을 같은 build matrix에서 수행해 중복 빌드를 제거한다.

## 변경 방향

- `verify` job을 제거한다.
- `version` job은 `prepare` 직후 release candidate commit bundle을 만든다.
- `build` job은 release candidate commit을 checkout한 뒤 빌드한다.
- CPU flavor에서는 빌드 직후 OCR smoke를 실행한다.
- smoke가 실패하면 archive 생성 및 upload가 실행되지 않는다.
- 모든 build matrix가 통과한 뒤에만 `release` job이 main push, tag 생성, GitHub Release 생성을 수행한다.
- Windows runner의 기본 stdout 인코딩 문제를 피하기 위해 CUDA runtime 구성 스크립트도 UTF-8 stdio 재설정을 수행한다.

## 안전 조건

- release candidate commit은 artifact bundle로만 전달하고 main에 즉시 push하지 않는다.
- `release` job은 main이 `prepare`에서 고정한 base SHA에 머물러 있는지 확인한 뒤에만 main을 release candidate commit으로 push한다.
- 중간 실패 시 main 브랜치, tag, GitHub Release에는 영향이 없다.

## 검증 계획

- release workflow 정적 테스트로 `verify` job이 제거되고 build job에 OCR smoke가 통합됐는지 확인한다.
- main push가 release job에만 남아 있는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `python -m unittest tools.scripts.test_setup_cuda_runtime`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사
