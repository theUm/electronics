#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>


// const char* ssid = "your_ssid";
// const char* password = "your_password";
#include "wifi_credentials.h"
 

WebServer server(80);
float currentTemp = 0.0;
String currentVerdict = "Очікування даних від Nucleo...";

String getJsonData() {
    return "{\"temp\": " + String(currentTemp, 1) + 
           ", \"verdict\": \"" + currentVerdict + 
           "\", \"ip\": \"" + WiFi.localIP().toString() + "\"" + 
           ", \"rssi\": " + String(WiFi.RSSI()) + "}";
}

unsigned long previousMillis = 0;
const long interval = 10000;
int wifi_retries = 0;

void handleRoot() {
    static const char html[] = R"rawhtml(
<!DOCTYPE html>
<html lang="uk">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Метеостанція STM32 & ESP32</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
    <style>
        :root {
            --primary: #3b82f6;
            --success: #10b981;
            --warning: #f59e0b;
            --danger: #ef4444;
            --bg-start: #0f172a;
            --bg-end: #1e293b;
        }
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            font-family: 'Inter', sans-serif;
            background: linear-gradient(135deg, var(--bg-start), var(--bg-end));
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            color: #f8fafc;
            padding: 20px;
        }
        .container {
            width: 100%;
            max-width: 480px;
        }
        .card {
            background: rgba(30, 41, 59, 0.7);
            backdrop-filter: blur(16px);
            -webkit-backdrop-filter: blur(16px);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 24px;
            padding: 32px;
            box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.3), 0 10px 10px -5px rgba(0, 0, 0, 0.2);
            transition: transform 0.3s ease, box-shadow 0.3s ease;
        }
        .card:hover {
            transform: translateY(-2px);
            box-shadow: 0 25px 30px -5px rgba(0, 0, 0, 0.4), 0 15px 15px -5px rgba(0, 0, 0, 0.3);
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 28px;
            border-bottom: 1px solid rgba(255, 255, 255, 0.08);
            padding-bottom: 16px;
        }
        .title {
            font-size: 20px;
            font-weight: 600;
            background: linear-gradient(to right, #60a5fa, #34d399);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .status-badge {
            background: rgba(16, 185, 129, 0.2);
            color: #34d399;
            padding: 4px 10px;
            border-radius: 12px;
            font-size: 12px;
            font-weight: 500;
            display: flex;
            align-items: center;
            gap: 6px;
        }
        .status-dot {
            width: 8px;
            height: 8px;
            background-color: #10b981;
            border-radius: 50%;
            animation: pulse 2s infinite;
        }
        @keyframes pulse {
            0% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(16, 185, 129, 0.7); }
            70% { transform: scale(1); box-shadow: 0 0 0 6px rgba(16, 185, 129, 0); }
            100% { transform: scale(0.95); box-shadow: 0 0 0 0 rgba(16, 185, 129, 0); }
        }
        .metric-group {
            display: flex;
            flex-direction: column;
            gap: 24px;
        }
        .metric {
            background: rgba(15, 23, 42, 0.3);
            border-radius: 16px;
            padding: 20px;
            border: 1px solid rgba(255, 255, 255, 0.03);
        }
        .metric-label {
            font-size: 13px;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: #94a3b8;
            margin-bottom: 8px;
            font-weight: 500;
        }
        .temp-display {
            display: flex;
            align-items: baseline;
        }
        .temp-val {
            font-size: 48px;
            font-weight: 700;
            color: #f8fafc;
            line-height: 1;
        }
        .temp-unit {
            font-size: 24px;
            color: #64748b;
            margin-left: 4px;
            font-weight: 500;
        }
        .verdict-val {
            font-size: 18px;
            font-weight: 500;
            color: #38bdf8;
            margin-top: 4px;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            margin-top: 16px;
            font-size: 14px;
            color: #94a3b8;
        }
        .info-item {
            display: flex;
            flex-direction: column;
            gap: 4px;
        }
        .info-value {
            color: #cbd5e1;
            font-weight: 500;
        }
        .footer {
            margin-top: 24px;
            text-align: center;
            font-size: 11px;
            color: #475569;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <div class="header">
                <span class="title">Метеостанція</span>
                <div class="status-badge">
                    <span class="status-dot"></span>
                    <span>Live</span>
                </div>
            </div>
            <div class="metric-group">
                <div class="metric">
                    <div class="metric-label">Температура</div>
                    <div class="temp-display">
                        <span id="temp" class="temp-val">--.-</span>
                        <span class="temp-unit">°C</span>
                    </div>
                </div>
                <div class="metric">
                    <div class="metric-label">Аналіз погоди</div>
                    <div id="verdict" class="verdict-val">Очікування...</div>
                </div>
                <div>
                    <div class="info-row">
                        <div class="info-item">
                            <span class="metric-label" style="margin-bottom:0">Сигнал Wi-Fi</span>
                            <span id="wifi-rssi" class="info-value">-- dBm</span>
                        </div>
                        <div class="info-item" style="text-align: right;">
                            <span class="metric-label" style="margin-bottom:0">IP адреса</span>
                            <span id="ip-addr" class="info-value">--.---.---.---</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
        <div class="footer">
            STM32 Nucleo F411RE + ESP32 WROOM
        </div>
    </div>
    <script>
        function updateStats() {
            fetch('/api')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('temp').innerText = data.temp !== undefined ? data.temp.toFixed(1) : '--.-';
                    document.getElementById('verdict').innerText = data.verdict || 'Очікування...';
                    document.getElementById('ip-addr').innerText = data.ip || '--.---.---.---';
                    document.getElementById('wifi-rssi').innerText = data.rssi !== undefined ? data.rssi + ' dBm' : '-- dBm';
                })
                .catch(err => {
                    console.error('Error fetching data:', err);
                });
        }
        setInterval(updateStats, 1000);
        updateStats();
    </script>
</body>
</html>
)rawhtml";
    server.send(200, "text/html", html);
}

void handleApi() {
    server.send(200, "application/json", getJsonData());
}

void setup() {
    // Дебаг-порт у ПК
    Serial.begin(115200);

    // Ініціалізація UART2 для Nucleo на пінах 16 (RX) та 17 (TX)
    Serial2.begin(115200, SERIAL_8N1, 16, 17);
    Serial2.setTimeout(10);

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

    // Періодична відправка IP та RSSI на Nucleo
    static unsigned long lastTxMillis = 0;
    const long txInterval = 1000;
    if (currentMillis - lastTxMillis >= txInterval) {
        lastTxMillis = currentMillis;
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

    // Читання даних від Nucleo
    while (Serial2.available() > 0) {
        String incomingData = Serial2.readStringUntil('\n');
        incomingData.trim();

        if (incomingData.length() > 0) {
            // Логування всього отриманого
            Serial.print("[RX from Nucleo]: ");
            Serial.println(incomingData);

            if (incomingData.startsWith("TEMP:")) {
                currentTemp = incomingData.substring(5).toFloat();
            } else if (incomingData.startsWith("VERDICT:")) {
                currentVerdict = incomingData.substring(8);
            } else if (incomingData.startsWith("{")) {
                // Парсинг старого формату JSON для зворотної сумісності
                int tempIdx = incomingData.indexOf("\"temp\":");
                if (tempIdx != -1) {
                    int valStart = tempIdx + 7;
                    int commaIdx = incomingData.indexOf(",", valStart);
                    if (commaIdx != -1) {
                        currentTemp = incomingData.substring(valStart, commaIdx).toFloat();
                    }
                }
                int verdictIdx = incomingData.indexOf("\"verdict\":\"");
                if (verdictIdx != -1) {
                    int valStart = verdictIdx + 11;
                    int endQuoteIdx = incomingData.indexOf("\"", valStart);
                    if (endQuoteIdx != -1) {
                        currentVerdict = incomingData.substring(valStart, endQuoteIdx);
                    }
                }
            }
        }
    }
}
