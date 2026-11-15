#define BLYNK_TEMPLATE_ID "TMPL6JONx9Rrd"
#define BLYNK_TEMPLATE_NAME "ĐATN Detection"
#define BLYNK_AUTH_TOKEN "89XZfIq8hlJlglPBghUBzY6mlVG2IGtV"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

char ssid[] = "E318-1C7";
char pass[] = "bme318c7";

#define RELAY_LIGHT 26
#define RELAY_BUZZER 27

#define ON_TIME 1000
#define OFF_TIME 1000
#define REST_TIME 10000
#define MAX_BLINK 3
#define BUZZER_TIME 3000

int alarmSignal = 0;
int lastSignal = -1;

// ===== TIMER =====
unsigned long blinkTimer = 0;
unsigned long restStart = 0;
unsigned long buzzerStart = 0;

// ===== STATE =====
int blinkCount = 0;
bool lightState = false;
bool isResting = false;
bool buzzerActive = false;

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_BUZZER, OUTPUT);

  digitalWrite(RELAY_LIGHT, LOW);
  digitalWrite(RELAY_BUZZER, LOW);

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(3000);

  Serial.println("READY");
}

// ===== SYNC ĐÚNG CHỖ =====
BLYNK_CONNECTED() {
  Serial.println("SYNC V1");
  Blynk.syncVirtual(V1);
}
BLYNK_WRITE(V1) {
  alarmSignal = param.asInt();

  Serial.print("V1 = ");
  Serial.println(alarmSignal);
}

// ===== RESET FULL =====
void resetAll() {
  digitalWrite(RELAY_LIGHT, LOW);
  digitalWrite(RELAY_BUZZER, LOW);

  blinkCount = 0;
  lightState = false;
  isResting = false;
  buzzerActive = false;

  blinkTimer = millis();
  restStart = millis();
}

// ===== LOOP =====
void loop() {
  if (!Blynk.connected()) {
    Serial.println("Reconnecting Blynk...");
    Blynk.connect(1000);
  }

  Blynk.run();

  // ===== XỬ LÝ CHUYỂN TRẠNG THÁI =====
  if (alarmSignal != lastSignal) {

    if (alarmSignal == 0) {
      Serial.println("STOP ALL");
      digitalWrite(RELAY_LIGHT, LOW);
      digitalWrite(RELAY_BUZZER, LOW);

      blinkCount = 0;
      lightState = false;
      isResting = false;
      buzzerActive = false;
      blinkTimer = millis();   
      restStart = millis();    
    }
    else if (alarmSignal == 1) {
      Serial.println("START ALARM");

      blinkCount = 0;
      lightState = false;
      isResting = false;
      buzzerActive = false;
      blinkTimer = millis();
      restStart = millis();
    }
    lastSignal = alarmSignal;
  }

  // ===== V1 = 1 → CẢNH BÁO =====
  if (alarmSignal == 1) {
    if (!isResting) {

      if (!buzzerActive) {
        buzzerActive = true;
        buzzerStart = millis();
        digitalWrite(RELAY_BUZZER, HIGH);
      }
      if (buzzerActive && millis() - buzzerStart >= BUZZER_TIME) {
        digitalWrite(RELAY_BUZZER, LOW);
        buzzerActive = false;
      }

      if (millis() - blinkTimer >= (lightState ? ON_TIME : OFF_TIME)) {
        blinkTimer = millis();
        lightState = !lightState;
        digitalWrite(RELAY_LIGHT, lightState ? HIGH : LOW);
        
        if (!lightState) {
          blinkCount++;
        }

        if (blinkCount >= MAX_BLINK) {
          isResting = true;
          restStart = millis();

          digitalWrite(RELAY_LIGHT, LOW);
          digitalWrite(RELAY_BUZZER, LOW);
        }
      }

    } else {
      // ===== NGHỈ =====
      if (millis() - restStart >= REST_TIME) {
        isResting = false;
        blinkCount = 0;
        lightState = false;
        buzzerActive = false;
        blinkTimer = millis();
      }
    }

  } else {
    // ===== V1 = 0 → TẮT =====
    digitalWrite(RELAY_LIGHT, LOW);
    digitalWrite(RELAY_BUZZER, LOW);
  }
}
