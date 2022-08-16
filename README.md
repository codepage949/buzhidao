# 메모

## oracle cloud aarch64 서버에서 동작시키기

* paddlepaddle
  * aarch64용 패키지가 공식적으로 지원되지 않음
  * 아래 페이지를 참고하여 aarch64용 whl 파일을 생성하여 설치
  * https://github.com/PaddlePaddle/Paddle-Inference-Demo/blob/master/docs-official/guides/hardware_support/cpu_phytium_cn.md
* paddleocr
  * pip로 설치가 진행은 되나 도중 python-opencv 빌드가 실패함
  * 최신 버전은 aarch64이 지원되나 paddleocr에서 지정해 놓은 버전은 안 됨
  * 아래 저장소의 requirements.txt에서 지정된 버전을 제거한 뒤 aarch64용 whl 파일을 생성하여 설치
  * https://github.com/PaddlePaddle/PaddleOCR