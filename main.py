import os
from fastapi import FastAPI, File, UploadFile
from paddleocr import PaddleOCR, draw_ocr

app = FastAPI()
ocr = PaddleOCR(use_angle_cls=True, lang='ch')

@app.post("/infer")
def infer(file: UploadFile):
    with open("img.png", "wb") as f:
        f.write(file.file.read())
    
    ocr = PaddleOCR(use_angle_cls=True, lang='ch')
    img_path = './img.png'
    result = ocr.ocr(img_path, cls=True)
    output = []

    for line in result:
        (text, _) = line[1]
        
        output.append(text)

    return output