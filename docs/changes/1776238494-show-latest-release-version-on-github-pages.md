# GitHub Pages에 최신 릴리즈 버전 링크 표시

## 배경

현재 GitHub Pages 소개 페이지 우상단에는 GitHub 저장소 아이콘만 있다.
사용자는 최신 배포 버전을 바로 확인할 수 없고, 릴리즈 페이지로 가려면
저장소에 들어가서 다시 Releases로 이동해야 한다.

## 결정

GitHub 아이콘 왼쪽에 최신 릴리즈 버전을 표시하는 링크를 추가한다.
버전 링크를 누르면 해당 릴리즈 페이지로 바로 이동한다.

버전은 정적으로 박아두지 않고 GitHub API의 최신 릴리즈 정보를 읽는다.
API 요청이 실패하면 특정 버전을 고정하지 않고 `Releases`로 표시하고
릴리즈 목록 페이지로 이동하게 한다.

## 변경 사항

### GitHub Pages
- `docs/index.html`
  - 우상단 액션 영역을 `top-actions` 묶음으로 재구성.
  - GitHub 아이콘 왼쪽에 `latest-release-link` pill UI를 추가.
  - 페이지 로드 후 `https://api.github.com/repos/codepage949/buzhidao/releases/latest`
    에서 `tag_name`, `html_url`을 읽어 링크 텍스트와 이동 URL을 갱신.
  - API 실패 시에는 `Releases` 라벨과 릴리즈 목록 링크를 fallback으로 사용.
  - 모바일에서 pill과 아이콘 간격, 크기를 함께 조정.

## 테스트

- 자동 테스트 없음

## 수동 검증

- GitHub Pages를 열었을 때 우상단에 최신 릴리즈 버전 pill이 GitHub 아이콘 왼쪽에 보이는지.
- 버전 pill 클릭 시 해당 릴리즈 페이지로 이동하는지.
- GitHub API 요청이 실패해도 fallback 버전 링크가 유지되는지.
