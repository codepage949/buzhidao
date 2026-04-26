# 릴리스 Rust 빌드 캐시 적용

## 계획

릴리스 workflow는 `verify` job에서 smoke 테스트를 위해 한 번 빌드한 뒤, `version` job에서 릴리스 버전을 갱신하고, `build` job에서 릴리스 산출물을 다시 빌드한다. 버전 갱신 후에는 최종 실행 파일의 메타데이터가 달라질 수 있으므로 이전 실행 파일을 그대로 재사용하지 않는다.

대신 GitHub Actions cache를 사용해 Rust 의존성 및 `target` 중간 산출물을 OS/flavor별로 안전하게 재사용한다. Cargo fingerprint가 현재 소스, manifest, feature, build script 입력을 기준으로 필요한 항목만 다시 빌드하므로, 캐시는 재빌드 시간을 줄이되 최종 릴리스 바이너리 생성은 그대로 수행한다.

## 적용 범위

- `verify`와 `build` matrix job에 동일한 Rust 빌드 캐시 키를 적용한다.
- 캐시 키는 `matrix.label`과 `github.run_id`를 포함해 같은 릴리스 실행 안에서 verify 산출물을 build가 복원할 수 있게 한다.
- `restore-keys`는 같은 OS/flavor의 이전 릴리스 실행 캐시를 보조적으로 사용할 수 있게 제한한다.
- CPU/GPU 및 Windows/Linux 캐시를 섞지 않는다.

## 릴리스 버전 커밋 게시 지연

`version` job은 릴리스 버전 커밋을 만들지만 main 브랜치에 즉시 push하지 않는다. 대신 release candidate commit을 git bundle artifact로 업로드한다.

`build` job은 `prepare`에서 고정한 base SHA를 checkout한 뒤 release candidate bundle을 fetch해 동일한 릴리스 버전 커밋으로 빌드한다. 모든 build matrix가 통과한 뒤 `release` job에서 main 브랜치가 여전히 base SHA에 머물러 있는지 확인하고, 그때만 release candidate commit을 main에 push한다. 따라서 빌드 또는 릴리스 생성 전 단계에서 중단되면 main에는 버전 업데이트 커밋이 남지 않는다.

## 검증 계획

- release workflow 정적 테스트를 보강해 Rust 빌드 캐시가 `verify`와 `build` 양쪽에 존재하는지 확인한다.
- release workflow 정적 테스트를 보강해 버전 커밋이 build 통과 전 main에 push되지 않는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사
