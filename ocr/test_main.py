import io
import os
from unittest.mock import MagicMock

import pytest
from fastapi.testclient import TestClient

from main import app, save_upload_to_temp


@pytest.fixture(autouse=True)
def reset_app_state():
    app.state.ocrs = {}
    yield
    app.state.ocrs = {}


# save_upload_to_temp

def test_임시_파일_저장_올바른_확장자():
    mock_file = MagicMock()
    mock_file.filename = "screenshot.png"
    mock_file.file = io.BytesIO(b"fake image data")

    path = save_upload_to_temp(mock_file)
    try:
        assert path.endswith(".png")
        assert os.path.exists(path)
    finally:
        os.remove(path)


def test_임시_파일_저장_내용이_정확히_기록됨():
    content = b"test image bytes 12345"
    mock_file = MagicMock()
    mock_file.filename = "image.png"
    mock_file.file = io.BytesIO(content)

    path = save_upload_to_temp(mock_file)
    try:
        with open(path, "rb") as f:
            assert f.read() == content
    finally:
        os.remove(path)


def test_임시_파일_저장_확장자_없으면_png_기본값():
    mock_file = MagicMock()
    mock_file.filename = "noextension"
    mock_file.file = io.BytesIO(b"data")

    path = save_upload_to_temp(mock_file)
    try:
        assert path.endswith(".png")
    finally:
        os.remove(path)


# /infer/{src} 엔드포인트

def test_지원하지_않는_언어_400_반환():
    client = TestClient(app)
    response = client.post(
        "/infer/jp",
        files={"file": ("test.png", b"fake", "image/png")},
    )
    assert response.status_code == 400
    assert "jp" in response.json()["detail"]


def test_지원하는_언어_OCR_결과_반환():
    mock_ocr = MagicMock()
    mock_ocr.predict.return_value = [
        {
            "rec_polys": [],
            "rec_texts": [],
        }
    ]
    app.state.ocrs = {"en": mock_ocr}

    client = TestClient(app)
    with pytest.MonkeyPatch.context() as mp:
        mp.setenv("SCORE_THRESH", "0.5")
        response = client.post(
            "/infer/en",
            files={"file": ("test.png", b"fake image", "image/png")},
        )

    assert response.status_code == 200
    assert response.json() == []


def test_지원하는_언어_OCR_텍스트_목록_반환():
    poly1 = MagicMock()
    poly1.tolist.return_value = [[0, 0], [10, 0], [10, 10], [0, 10]]
    poly2 = MagicMock()
    poly2.tolist.return_value = [[20, 0], [30, 0], [30, 10], [20, 10]]

    mock_ocr = MagicMock()
    mock_ocr.predict.return_value = [
        {
            "rec_polys": [poly1, poly2],
            "rec_texts": ["Hello", "World"],
        }
    ]
    app.state.ocrs = {"en": mock_ocr}

    client = TestClient(app)
    with pytest.MonkeyPatch.context() as mp:
        mp.setenv("SCORE_THRESH", "0.5")
        response = client.post(
            "/infer/en",
            files={"file": ("test.png", b"fake image", "image/png")},
        )

    assert response.status_code == 200
    result = response.json()
    assert len(result) == 2
    assert result[0][1] == "Hello"
    assert result[1][1] == "World"
