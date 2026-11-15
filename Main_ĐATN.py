import cv2
import threading
import time
import json
import requests
import paho.mqtt.client as mqtt
from ultralytics import YOLO
import mediapipe as mp
import atexit

MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
TOPIC_SUB = "esp32/fall"
TOPIC_PUB = "esp32/result"

# ================= BLYNK =================
BLYNK_TOKEN = "89XZfIq8hlJlglPBghUBzY6mlVG2IGtV"
# ================= CAMERA =================
RTSP_URL = "rtsp://admin:L28B993C@192.168.0.188:554/cam/realmonitor?channel=1&subtype=0"

# ================= GLOBAL =================
person_present = False
latest_frame = None
processing = False
lock = threading.Lock()

# ================= AI INIT =================
model = YOLO("yolov8n.pt")  
mp_pose = mp.solutions.pose
pose = mp_pose.Pose()
last_blynk_send = 0
prev_fall_state = False

def reset_blynk():
    try:
        url_v0 = f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V0=0"

        requests.get(url_v0, timeout=2)
        time.sleep(0.5)
        requests.get(url_v0, timeout=2)
        print("[BLYNK] FORCE RESET V0=0")

        url_v1 = f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V1=0"

        requests.get(url_v1, timeout=2)
        time.sleep(0.5)
        requests.get(url_v1, timeout=2)
        print("[BLYNK] FORCE RESET V1=0")
    except:
        print("[BLYNK] RESET FAIL")
atexit.register(reset_blynk)

def now():
    import datetime
    return datetime.datetime.now().strftime("%H:%M:%S")

def send_status(v0, v1):
    try:
        requests.get(
            f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V0={v0}&V1={v1}",
            timeout=1
        )
        print(f"[BLYNK] V0={v0} V1={v1}")
    except:
        print("[BLYNK] ERROR")

def send_time():
    try:
        requests.get(
            f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V2={now()}",
            timeout=1
        )
        print("[BLYNK] Sent TIME:", now())
    except:
        print("[BLYNK] ERROR TIME")

# ================= CAMERA THREAD =================
def camera_thread():
    global person_present, latest_frame

    cap = cv2.VideoCapture(RTSP_URL)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    frame_count = 0
    last_state = None
    prev_blynk_state = None   

    while True:
        ret, frame = cap.read()
        if not ret:
            continue

        latest_frame = frame.copy()
        display = cv2.resize(frame, (640, 360))

        frame_count += 1

        if frame_count % 10 == 0:
            results = model(display, verbose=False)

            found = False
            for r in results:
                for box in r.boxes:
                    if int(box.cls[0]) == 0:
                        found = True
                        break

            # ===== LOG CAMERA =====
            if found != last_state:
                print(f"[CAM] Person: {found}")
                last_state = found

            with lock:
                person_present = found

            # ===== GỬI BLYNK CHỈ KHI THAY ĐỔI =====
            current_state = 1 if found else 0

            if current_state != prev_blynk_state:
                try:
                    requests.get(
                        f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V0={current_state}",
                        timeout=1
                    )
                    print(f"[BLYNK] V0={current_state}")
                except:
                    print("[BLYNK] ERROR V0")

                prev_blynk_state = current_state

        cv2.imshow("CAMERA AI", display)
        if cv2.waitKey(1) & 0xFF == 27:
            break

# ================= POSE CHECK =================
def check_pose_fall(frame):
    try:
        img = cv2.resize(frame, (320, 240))
        img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

        results = pose.process(img_rgb)

        if not results.pose_landmarks:
            print("[POSE] No landmarks")
            return False

        lm = results.pose_landmarks.landmark

        # ===== LẤY ĐIỂM =====
        ls = lm[mp_pose.PoseLandmark.LEFT_SHOULDER.value]
        rs = lm[mp_pose.PoseLandmark.RIGHT_SHOULDER.value]
        lh = lm[mp_pose.PoseLandmark.LEFT_HIP.value]
        rh = lm[mp_pose.PoseLandmark.RIGHT_HIP.value]

        if min(ls.visibility, rs.visibility, lh.visibility, rh.visibility) < 0.5:
            print("[POSE] Low visibility")
            return False

        shoulder_y = (ls.y + rs.y) / 2
        hip_y = (lh.y + rh.y) / 2
        diff = abs(shoulder_y - hip_y)

        print(f"[POSE] diff={diff:.3f}")

        if diff < 0.08:
            print("[POSE] HORIZONTAL → FALL")
            return True
        else:
            print("[POSE] NORMAL")
            return False

    except Exception as e:
        print("[POSE ERROR]", e)
        return False

# ================= XỬ LÝ FALL =================
def handle_fall(client, data):
    global processing, prev_fall_state

    if processing:
        return

    processing = True

    print("\n🚨 FALL SIGNAL FROM ESP32 🚨")
    print("[DATA]", data)

    with lock:
        has_person = person_present
        frame = latest_frame.copy() if latest_frame is not None else None

    # ===== CHECK NGƯỜI REALTIME =====
    if frame is not None:
        try:
            test_frame = cv2.resize(frame, (320, 180))
            results = model(test_frame, verbose=False)

            found_now = False
            for r in results:
                for box in r.boxes:
                    if int(box.cls[0]) == 0:
                        found_now = True
                        break
            # AI thấy người → override lại
            if found_now:
                has_person = True

        except:
            pass

    print("[CHECK] Person (final):", has_person)

    is_fall = False

    # ===== KHÔNG CÓ NGƯỜI =====
    if not has_person:
        print("[RESULT] NO PERSON → CALL")
        client.publish(TOPIC_PUB, "CALL")
        is_fall = True
    else:
        if frame is None:
            print("[ERROR] No frame → SAFE")
            client.publish(TOPIC_PUB, "SAFE")
        else:
            print("[AI] Checking pose...")
            is_fall = check_pose_fall(frame)

            if is_fall:
                print("[RESULT] FALL → CALL")
                client.publish(TOPIC_PUB, "CALL")
            else:
                print("[RESULT] NORMAL → SAFE")
                client.publish(TOPIC_PUB, "SAFE")

    # ===== GỬI V1 CHỈ KHI THAY ĐỔI =====
    if is_fall != prev_fall_state:
        try:
            requests.get(
                f"https://blynk.cloud/external/api/update?token={BLYNK_TOKEN}&V1={int(is_fall)}",
                timeout=1
            )
            print(f"[BLYNK] V1 = {int(is_fall)}")
        except:
            print("[BLYNK] ERROR V1")

        if is_fall:
            send_time()

    prev_fall_state = is_fall

    print("=================================\n")

    processing = False
    
# ================= MQTT =================
def on_connect(client, userdata, flags, rc):
    print("[MQTT] Connected:", rc)
    client.subscribe(TOPIC_SUB)
    print("[MQTT] Subscribed:", TOPIC_SUB)


def on_message(client, userdata, msg):
    try:
        data = msg.payload.decode()
        print("[MQTT RECEIVED RAW]:", data)

        json_data = json.loads(data)

        if json_data.get("event") == "fall":
            handle_fall(client, json_data)

    except Exception as e:
        print("[ERROR]", e)

# ================= MAIN =================
if __name__ == "__main__":

    print("=== RESET SYSTEM ===")
    reset_blynk()
    time.sleep(2)   

    # ===== CAMERA THREAD =====
    t = threading.Thread(target=camera_thread)
    t.daemon = True
    t.start()

    # ===== MQTT =====
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    print("[MQTT] Connecting...")
    client.connect(MQTT_BROKER, MQTT_PORT)

    print("[SYSTEM] READY")

    client.loop_forever()
