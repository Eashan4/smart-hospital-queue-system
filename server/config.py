import os
from dotenv import load_dotenv

load_dotenv()

HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", 5000))
DEBUG = os.getenv("DEBUG", "True").lower() == "true"
SECRET_KEY = os.getenv("SECRET_KEY", "devkey")
SERVER_IP = os.getenv("SERVER_IP", "http://localhost:5000")
DEVICE_TIMEOUT = int(os.getenv("DEVICE_TIMEOUT", 10))
MIN_DATA_THRESHOLD = int(os.getenv("MIN_DATA_THRESHOLD", 10))
DATA_FILE = os.path.join(os.path.dirname(__file__), "data.csv")
