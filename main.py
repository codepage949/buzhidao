from fastapi import FastAPI, File, UploadFile
from fastapi.responses import PlainTextResponse
import subprocess
import threading

app = FastAPI()
output = ""
t = None

def _infer(img_path):
    global output

    output = subprocess.run(["paddleocr", "--image_dir", img_path, "--use_angle_cls", "true", "--lang", "ch", "--use_gpu", "false"], capture_output=True).stdout

@app.post("/infer", response_class=PlainTextResponse)
def infer(file: UploadFile):
    global output
    global t

    with open("img.png", "wb") as f:
        f.write(file.file.read())
    
    img_path = "./img.png"
    output = ""

    threading.Thread(target=_infer, args=(img_path,)).start()

    return output

@app.get("/get", response_class=PlainTextResponse)
def get():
    global output

    return output