from contextlib import asynccontextmanager
from pathlib import Path
from tempfile import NamedTemporaryFile
import os

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, UploadFile
from paddleocr import PaddleOCR
import uvicorn

load_dotenv()

MODEL_SAMPLE_PATH = Path(__file__).with_name("test.png")
SUPPORTED_LANGS = ("en", "ch")
DEFAULT_UPLOAD_SUFFIX = ".png"


def build_ocr(lang: str) -> PaddleOCR:
    return PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device="gpu",
        lang=lang,
    )


def load_ocrs() -> dict[str, PaddleOCR]:
    ocrs = {lang: build_ocr(lang) for lang in SUPPORTED_LANGS}

    # NOTE: 언어별 OCR 모델을 미리 로드
    sample_path = str(MODEL_SAMPLE_PATH)
    for ocr in ocrs.values():
        ocr.predict(sample_path)

    return ocrs


def save_upload_to_temp(file: UploadFile) -> str:
    suffix = Path(file.filename or f"upload{DEFAULT_UPLOAD_SUFFIX}").suffix or DEFAULT_UPLOAD_SUFFIX

    with NamedTemporaryFile(delete=False, suffix=suffix) as temp_file:
        temp_file.write(file.file.read())
        return temp_file.name


def score_threshold() -> float:
    return float(os.environ["SCORE_THRESH"])


def infer_texts(ocr: PaddleOCR, image_path: str) -> list[tuple[list, str]]:
    result = ocr.predict(
        image_path,
        text_rec_score_thresh=score_threshold(),
    )

    return list(
        zip([polygon.tolist() for polygon in result[0]["rec_polys"]], result[0]["rec_texts"])
    )


@asynccontextmanager
async def lifespan(_: FastAPI):
    app.state.ocrs = load_ocrs()
    yield


app = FastAPI(lifespan=lifespan)


@app.post("/infer/{src}")
def infer(file: UploadFile, src: str):
    ocr = app.state.ocrs.get(src)
    if ocr is None:
        raise HTTPException(status_code=400, detail=f"Unsupported source language: {src}")

    image_path = save_upload_to_temp(file)

    try:
        return infer_texts(ocr, image_path)
    finally:
        if os.path.exists(image_path):
            os.remove(image_path)


if __name__ == "__main__":
    uvicorn.run(app, host=os.environ["HTTP_HOST"], port=int(os.environ["HTTP_PORT"]))
