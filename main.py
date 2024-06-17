from fastapi import FastAPI, UploadFile
from paddleocr import PaddleOCR

app = FastAPI()

@app.post("/infer/{src}")
def infer(file: UploadFile, src: str):
    with open("img.jpg", "wb") as f:
        f.write(file.file.read())
    
    img_path = "./img.jpg"
    ocr = PaddleOCR(use_angle_cls=True, lang=src)
    result = ocr.ocr(img_path, cls=True)

    return result