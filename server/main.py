from dotenv import load_dotenv

load_dotenv()

from fastapi import FastAPI, UploadFile
import uvicorn
from paddleocr import PaddleOCR
import os

ocrs = {
    "en": PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device="gpu",
        lang="en",
    ),
    "ch": PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device="gpu",
        lang="ch",
    ),
}

# NOTE: 언어별 OCR 모델을 미리 로드
ocrs["en"].predict("test.png")
ocrs["ch"].predict("test.png")

app = FastAPI()


@app.post("/infer/{src}")
def infer(file: UploadFile, src: str):
    with open("output.png", "wb") as f:
        f.write(file.file.read())

    img_path = "./output.png"
    result = ocrs[src].predict(
        img_path, text_rec_score_thresh=float(os.environ["SCORE_THRESH"])
    )

    return list(
        zip([x.tolist() for x in result[0]["rec_polys"]], result[0]["rec_texts"])
    )


uvicorn.run(app, host=os.environ["HTTP_HOST"], port=int(os.environ["HTTP_PORT"]))
