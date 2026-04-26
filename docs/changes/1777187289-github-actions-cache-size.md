# GitHub Actions Rust 캐시 용량 절감

## 계획

릴리스 workflow의 Rust cache가 `target` 전체를 포함하면서 GPU 빌드에서 단일 캐시가 수 GiB까지 커질 수 있다. GitHub Actions cache quota가 제한적이므로 OS/flavor matrix별 `target` 캐시를 유지하면 캐시가 빠르게 밀려나거나 quota를 소진한다.

속도 개선 효과를 완전히 버리지는 않기 위해 Cargo dependency 다운로드 캐시는 유지하고, 대용량 빌드 산출물 캐시만 제거한다.

## 변경 방향

- Rust cache 대상에서 `target`을 제거한다.
- `~/.cargo/registry`와 `~/.cargo/git` 캐시는 유지한다.
- Cargo 네트워크 재시도 환경 변수와 `ci_retry` 기반 다운로드 재시도는 유지한다.
- 최종 바이너리는 지금처럼 릴리스 후보 커밋 기준으로 매번 빌드한다.

## 기대 효과

- GPU matrix에서 `target` 전체가 캐시에 저장되어 5 GiB 이상을 차지하는 문제를 피한다.
- 의존성 index/source 재다운로드 비용은 줄인다.
- 컴파일 산출물 재사용 효과는 줄어들지만, cache quota 초과로 전체 캐시가 불안정해지는 상황을 완화한다.

## 검증 계획

- release workflow 정적 테스트로 `target`이 cache path에 포함되지 않는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

