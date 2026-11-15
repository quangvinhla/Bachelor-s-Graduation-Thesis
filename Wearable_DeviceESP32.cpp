#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
// ================= WIFI =================
const char* ssid = "E318-1C7";
const char* password = "bme318c7";
// ================= MQTT =================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const char* topic_pub = "esp32/fall";
const char* topic_sub = "esp32/result";

WiFiClient espClient;
PubSubClient client(espClient);

// ================= MPU =================
Adafruit_MPU6050 mpu;

// ================= SIM =================
#define RXD2 16
#define TXD2 17
HardwareSerial simSerial(2);
String phoneNumber = "+84778568317";

// ================= FALL STATE =================
enum FallState {
  NORMAL,
  FREE_FALL,
  IMPACT
};
FallState state = NORMAL;
unsigned long impactTime = 0;
unsigned long lastFallTime = 0;
// ================= CONFIG =================
const float FREE_FALL_TH = 5.0;
const float IMPACT_TH = 20.0;
const float STABLE_MIN = 8.0;
const float STABLE_MAX = 11.5;

const float ROLL_SUPINE_TH  = 30.0;
const float ROLL_PRONE_TH  = 140.0; 
const float PITCH_SIDE_TH = 60.0; 
const unsigned long IMMOBILE_TIME = 1000;
const unsigned long CONFIRM_TIME = 2000;
const unsigned long FALL_COOLDOWN = 15000;
const unsigned long CALL_COOLDOWN = 60000;
unsigned long lastCallTime = 0;
float roll = 0;
float pitch = 0;
unsigned long stillStart = 0;

// ================= SYSTEM LOCK =================
bool systemLocked = false;

// ================= MQTT CALLBACK =================
void callback(char* topic, byte* payload, unsigned int length) {

  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("[MQTT RECEIVED] ");
  Serial.println(message);

  if (message == "CALL") {
    Serial.println("[ACTION] CALL TRIGGER");
    makeCall();
  } else {
    Serial.println("[ACTION] SAFE - No call");
  }
}

// ================= WIFI =================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("[WIFI] Reconnecting...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[WIFI] OK");
}

// ================= MQTT CONNECT =================
void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("[MQTT] Connecting...");

    if (client.connect("ESP32_FALL")) {
      Serial.println("OK");

      client.subscribe(topic_sub);

    } else {
      Serial.print("FAILED, rc=");
      Serial.print(client.state());
      delay(2000);
    }
  }
}

// ================= CALL =================
void makeCall() {
  if (millis() - lastCallTime < CALL_COOLDOWN) {
    Serial.println("[CALL] Cooldown -> Skip");
    return;
  }

  Serial.println("[CALL] Calling...");
  simSerial.println("ATD" + phoneNumber + ";");

  lastCallTime = millis();
}

// ================= SEND MQTT =================
void sendEvent(float A) {
  ensureWiFi();
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();
  String payload = "{\"event\":\"fall\",\"acc\":" + String(A,2) + "}";

  Serial.println("[MQTT] Sending alert...");
  client.publish(topic_pub, payload.c_str());
}
// ================= SETUP =================
void setup() {
  Serial.begin(9600);
  Wire.begin(21, 22);

  if (!mpu.begin()) {
    Serial.println("[ERROR] MPU FAIL");
    while (1);
  }
  Serial.println("[OK] MPU READY");

  simSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);

  Serial.print("[WIFI] Connecting");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] OK");
  Serial.println(WiFi.localIP());

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}
// ================= LOOP =================
void loop() {

  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  if (systemLocked) return;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float A = sqrt(a.acceleration.x * a.acceleration.x +
                 a.acceleration.y * a.acceleration.y +
                 a.acceleration.z * a.acceleration.z);

  roll = atan2(a.acceleration.y, a.acceleration.z) * 57.2958;
  pitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 57.2958;

  static unsigned long lastPrint = 0;

  if (millis() - lastPrint > 1000) {

    Serial.print("[MPU] A=");
    Serial.print(A);

    Serial.print("  Roll=");
    Serial.print(roll);

    Serial.print("  Pitch=");
    Serial.println(pitch);

    lastPrint = millis();
  }

  switch (state) {

    case NORMAL:
      if (A < FREE_FALL_TH) {
        Serial.println("[STATE] FREE FALL");
        state = FREE_FALL;
      }
      break;

    case FREE_FALL:
      if (A > IMPACT_TH) {
        Serial.println("[STATE] IMPACT (>20)");
        state = IMPACT;
        impactTime = millis();
      }
      break;

    case IMPACT:
      if (millis() - impactTime > CONFIRM_TIME) {

        bool stableAcc =
          (A > STABLE_MIN &&
           A < STABLE_MAX);
           
        bool supine =
          (abs(roll) < ROLL_SUPINE_TH);
        bool prone =
          (abs(roll) > ROLL_PRONE_TH);
        bool side =
          (abs(pitch) > PITCH_SIDE_TH);
        bool lyingPosture =
          supine || prone || side;

        if (stableAcc && lyingPosture) {

          if (stillStart == 0) {

            stillStart = millis();

            if (supine) {
              Serial.println("[POSTURE] SUPINE");
            }

            if (prone) {
              Serial.println("[POSTURE] PRONE");
            }

            if (side) {
              Serial.println("[POSTURE] SIDE");
            }

            Serial.print("[ROLL] ");
            Serial.println(roll);

            Serial.print("[PITCH] ");
            Serial.println(pitch);
          }

          if (millis() - stillStart >= IMMOBILE_TIME) {

            Serial.println("\n========== FALL CONFIRMED ==========");

            Serial.print("ROLL = ");
            Serial.println(roll);

            Serial.print("PITCH = ");
            Serial.println(pitch);

            systemLocked = true;

            if (millis() - lastFallTime > FALL_COOLDOWN) {

              sendEvent(A);
              lastFallTime = millis();

            } else {

              Serial.println("[SKIP] Cooldown active");
            }

            Serial.println("===================================\n");

            delay(3000);

            systemLocked = false;
            stillStart = 0;
            state = NORMAL;
          }

        } else {

          stillStart = 0;

          Serial.println("[FALSE] Posture not match");
          state = NORMAL;
        }
      }
      break;
  }
  while (simSerial.available()) {
    Serial.write(simSerial.read());
  }

  delay(100);
}
