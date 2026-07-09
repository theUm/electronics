#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "MYSSID";
const char* password = "PASSWORD";

WebServer server(80);
String lastJsonData = "{\"temp\": 0.0, \"verdict\": \"Очікування даних від Nucleo...\"}";

unsigned long previousMillis = 0;
const long interval = 10000;
int wifi_retries = 0;

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>Метеостанція STM32</title>";
    html += "<style>body{font-family:sans-serif; margin:40px; background:#121212; color:#00ff00;}";
    html += ".card{background:#1e1e1e; padding:20px; border-radius:10px; max-width:400px;}</style>";
    html += "<script>setInterval(() => { location.reload(); }, 2000);</script>";
    html += "</head><body><div class='card'>";
    html += "<h2>Статус сенсорів</h2>";
    html += "<p><b>Сигнал Wi-Fi:</b> " + String(WiFi.RSSI()) + " dBm</p>";
    html += "<pre>" + lastJsonData + "</pre>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
}

void handleApi() {
    server.send(200, "application/json", lastJsonData);
}

void setup() {
    // Дебаг-порт у ПК
    Serial.begin(115200);

    // Ініціалізація UART2 для Nucleo на пінах 16 (RX) та 17 (TX)
    Serial2.begin(115200, SERIAL_8N1, 16, 17);

    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(ssid, password);

    Serial.print("Підключення до Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nПідключено!");
    Serial.print("IP адреса веб-сервера: ");
    Serial.println(WiFi.localIP());

    server.on("/", handleRoot);
    server.on("/api", handleApi);
    server.begin();

    // Відправляємо дані на Nucleo через Serial2
    Serial2.print("IP:");
    Serial2.println(WiFi.localIP().toString());
}

void loop() {
    server.handleClient();

    unsigned long currentMillis = millis();

    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >= interval)) {
        wifi_retries++;
        Serial.print(currentMillis);
        Serial.print(" Втрачено з'єднання. Спроба ");
        Serial.println(wifi_retries);

        if (wifi_retries > 3) {
            Serial.println("Радіомодуль не відповідає. Апаратне перезавантаження...");
            delay(1000);
            ESP.restart();
        }

        WiFi.disconnect(true);
        delay(100);
        WiFi.begin(ssid, password);

        previousMillis = currentMillis;
    } else if (WiFi.status() == WL_CONNECTED) {
        wifi_retries = 0;
    }

    // Читання даних від Nucleo
    if (Serial2.available()) {
        String incomingData = Serial2.readStringUntil('\n');
        incomingData.trim();

        if (incomingData.length() > 0 && incomingData.startsWith("{")) {
            lastJsonData = incomingData;

            Serial.print("Отримано від Nucleo: " + lastJsonData);
            Serial.print(" | Поточний RSSI: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");

            if (WiFi.status() == WL_CONNECTED) {
                Serial2.print("IP:");
                Serial2.println(WiFi.localIP().toString());
                Serial2.print("RSSI:");
                Serial2.println(WiFi.RSSI());
            } else {
                Serial2.print("IP:0.0.0.0\n");
                Serial2.print("RSSI:0\n");
            }
        }
    }
}
