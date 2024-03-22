from dotenv import load_dotenv
from fastapi import FastAPI, UploadFile
import uvicorn
from paddleocr import PaddleOCR
import os

load_dotenv()

ocrs = {
    "en": PaddleOCR(use_angle_cls=True, lang="en"),
    "ch": PaddleOCR(use_angle_cls=True, lang="ch"),
}

# NOTE: 언어별 OCR 모델을 미리 로드
ocrs["en"].ocr("test.png", cls=True)
ocrs["ch"].ocr("test.png", cls=True)

app = FastAPI()

@app.post("/infer/{src}")
def infer(file: UploadFile, src: str):
    with open("output.png", "wb") as f:
        f.write(file.file.read())
    
    img_path = "./output.png"
    ocr = PaddleOCR(use_angle_cls=True, lang=src)
    result = ocr.ocr(img_path, cls=True)

    return result

uvicorn.run(app, host=os.environ["HTTP_HOST"], port=int(os.environ["HTTP_PORT"]))
