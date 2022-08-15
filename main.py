from fastapi import FastAPI, UploadFile
from paddleocr import PaddleOCR

app = FastAPI()

@app.post("/infer")
def infer(file: UploadFile):
    with open("img.png", "wb") as f:
        f.write(file.file.read())
    
    img_path = "./img.png"
    ocr = PaddleOCR(use_angle_cls=True, lang="ch")
    result = ocr.ocr(img_path, cls=True)

    return result