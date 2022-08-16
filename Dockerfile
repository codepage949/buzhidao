FROM python:3.7

WORKDIR /app

COPY main.py main.py
COPY test.png test.png
COPY paddlepaddle-0.0.0-cp37-cp37m-linux_aarch64.whl paddlepaddle-0.0.0-cp37-cp37m-linux_aarch64.whl
COPY paddleocr-2.5.0.3-py2.py3-none-any.whl paddleocr-2.5.0.3-py2.py3-none-any.whl

RUN apt update
RUN apt install -y libgl1-mesa-glx libgeos-dev
RUN pip install paddlepaddle-0.0.0-cp37-cp37m-linux_aarch64.whl paddleocr-2.5.0.3-py2.py3-none-any.whl fastapi uvicorn python-multipart
RUN paddleocr --image_dir ./test.png --use_angle_cls true --lang ch --use_gpu false

EXPOSE 8000
CMD uvicorn main:app --host 0.0.0.0