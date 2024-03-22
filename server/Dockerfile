FROM python:3.10-slim

WORKDIR /app

COPY ./ ./

RUN apt update
RUN apt install -y libgl1-mesa-glx libgeos-dev libgomp1 libglib2.0-0
RUN pip install -r requirements.txt

CMD python main.py
