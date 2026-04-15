# 릴리즈 설치 스크립트 첨부 및 PowerShell 경로 버그 수정

## 배경

### PowerShell 경로 버그

릴리즈 노트의 Windows 분할 파일 합치기 스니펫에서
`[System.IO.File]::Create(".\buzhidao-...zip")` 형태의 상대 경로를 사용했다.
.NET 메서드는 PowerShell의 `$PWD`가 아니라 터미널을 연 시점의 디렉터리(.NET process CWD)를
기준으로 경로를 해석하므로, `cd`로 이동 후 실행하면 원하지 않는 위치에 파일이 생성된다.

→ `Join-Path (Get-Location).Path "파일명"` 으로 교체해 PowerShell 현재 디렉터리를 사용한다.

### 설치 스크립트 부재

분할 파일 합치기와 압축 해제 절차를 사용자가 직접 입력해야 했다.

## 결정

- 릴리즈 노트 인라인 스니펫의 경로 버그를 수정한다.
- 각 매트릭스(windows-amd64-cpu/gpu, linux-amd64-cpu/gpu)에 맞는 설치 스크립트를
  릴리즈 자산으로 첨부한다.
- 스크립트는 분할 파일 합치기 + 압축 해제를 자동 처리한다.

## 변경 사항

### `scripts/release_helper.py`

- `make-install-script` 서브커맨드 추가.
  - Windows: `install-windows-<arch>-<flavor>.ps1` 생성
    - 분할 파일 합치기: `Join-Path (Get-Location).Path` 사용 (경로 버그 수정)
    - `Expand-Archive`로 현재 디렉터리에 압축 해제
  - Linux: `install-linux-<arch>-<flavor>.sh` 생성
    - 분할 파일 합치기 후 `tar xzf`로 압축 해제

### `scripts/test_release_helper.py`

- 설치 스크립트 생성 단위 테스트 추가.

### `.github/workflows/release.yml`

- 릴리즈 노트 Windows 스니펫 경로 버그 수정.
- 릴리즈 노트에 첨부 설치 스크립트 안내 섹션 추가.
- `Generate install scripts` 스텝 추가 (4개 매트릭스 × 2 아카이브).
- `gh release create`에 생성된 설치 스크립트 파일 포함.

## 테스트

- `python -m pytest scripts/test_release_helper.py`
