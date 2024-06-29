from fastapi import FastAPI, UploadFile
from paddleocr import PaddleOCR
from PIL import Image, ImageEnhance, ImageFilter

app = FastAPI()

@app.post("/infer/{src}")
def infer(file: UploadFile, src: str):
    with open("img.jpg", "wb") as f:
        f.write(file.file.read())
    
    img_path = "./img.jpg"
    img = Image.open(img_path)
    sharpened_img = img.filter(ImageFilter.SHARPEN)
    enhancer = ImageEnhance.Contrast(sharpened_img)
    enhanced_img = enhancer.enhance(2)

    enhanced_img.save(img_path)

    ocr = PaddleOCR(use_angle_cls=True, lang=src)
    result = ocr.ocr(img_path, cls=True)

    return result