FROM python:3.13-slim

WORKDIR /app

RUN apt update
RUN apt install -y libgl1 libgeos-dev libgomp1 libglib2.0-0

COPY requirements.txt requirements.txt

RUN pip install -r requirements.txt

COPY ./ ./

CMD ["python", "main.py"]
