# 릴리스 publish 전 main 이동 감지 명확화

## 계획

릴리스 workflow가 시작된 뒤 main 브랜치에 새 커밋이 추가되면, release candidate는 더 이상 최신 main 기준의 커밋이 아니다. 이 상태에서 release candidate를 main에 push하면 새 커밋을 되돌리는 결과가 될 수 있으므로 publish를 중단해야 한다.

기존 workflow도 이를 차단했지만, `Create tag` step 안에서 뒤늦게 `Branch moved before release publish`만 출력해 원인과 후속 조치가 불명확했다.

## 변경 방향

- release job 초반에 main 브랜치가 `prepare`에서 고정한 base SHA와 같은지 먼저 확인한다.
- main이 이동했으면 release candidate를 publish하지 않고 즉시 실패한다.
- 실패 메시지에 현재 main SHA, 기대 SHA, release candidate SHA를 함께 출력한다.
- 사용자가 새 main 기준으로 release workflow를 다시 실행해야 함을 명시한다.
- tag 생성 step에서는 중복 branch freshness check를 제거하고 push/tag만 수행한다.

## 안전 조건

- main이 이동한 경우 main push, tag 생성, GitHub Release 생성은 실행되지 않는다.
- main이 그대로일 때만 release candidate commit을 main에 push한다.
- 기존의 release candidate SHA 검증은 유지한다.

## 검증 계획

- release workflow 정적 테스트로 branch freshness check가 별도 step으로 존재하는지 확인한다.
- `Create tag` step에는 중복 branch moved check가 남지 않는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

