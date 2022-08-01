FROM python:3.7

WORKDIR /app

COPY main.py main.py
RUN apt update
RUN apt install -y libgl1-mesa-glx
RUN pip install paddlepaddle "paddleocr>=2.0.1" fastapi uvicorn python-multipart

EXPOSE 8000
CMD uvicorn main:app --host 0.0.0.0