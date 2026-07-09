#include <Arduino.h>
#include <U8g2lib.h>
#include <math.h>

// Піни дисплея
#define PIN_DISP_GND D2
#define PIN_DISP_VCC D3
#define PIN_DISP_SCL D4
#define PIN_DISP_SDA D5

// Пін датчика температури підключеного напряму до nucleo
#define PIN_LM35D_DIRECT PC2

String esp_ip = "немає";
String esp_rssi = "0"; // Зберігатиме поточний рівень сигналу

// Ініціалізація USART6 на пінах Morpho (RX = PA12, TX = PA11)
HardwareSerial espSerial(PA12, PA11);

// Ініціалізація дисплея через програмний I2C
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/PIN_DISP_SCL, /* data=*/PIN_DISP_SDA, /* reset=*/U8X8_PIN_NONE);

/**
 * Оцінює якість літньої температури за гаусіаною субʼєктивності якості літа за температурою.
 * Пік (mu) = 24.0 -- літо як треба, відхилення (sigma) = 3.4 (субʼєктивно)
 * @param temperature Поточна температура у градусах Цельсія (формат xx.x).
 * @return Вказівник на рядок (зберігається у Flash/ROM) з вердиктом.
 */
const char *get_summer_verdict(float temperature)
{
    // Формула: f(T) = exp( -(T - 24)^2 / (2 * 3.4^2) )
    // 2 * 3.4^2 = 23.12
    float diff = temperature - 24.0f;
    float exponent = -(diff * diff) / 23.12f;
    float y = expf(exponent);

    if (y >= 0.9f)
    {
        return "літо як треба";
    }
    else if (y >= 0.7f)
    {
        return "хороше літо";
    }
    else if (y >= 0.5f)
    {
        return "нормальне літо";
    }
    else if (y >= 0.3f)
    {
        return "можна краще";
    }
    else if (y >= 0.1f)
    {
        return "нікуди не годиться";
    }
    else
    {
        return "ми в Арктиці чи в Сахарі";
    }
}

void setup()

{
    // датчик LM35DZ підключений напряму до STM32 та підсилювач, тому використовуємо апаратний АЦП
    Serial.begin(9600);

    // Апаратний UART на пінах D0 (RX) та D1 (TX) для зв'язку з ESP32-C3
    espSerial.begin(115200);

    // Формуємо живлення для дисплея
    pinMode(PIN_DISP_VCC, OUTPUT);
    pinMode(PIN_DISP_GND, OUTPUT);
    digitalWrite(PIN_DISP_GND, LOW);
    digitalWrite(PIN_DISP_VCC, HIGH);

    delay(2000); // Час на стабілізацію напруги дисплея

    u8g2.begin();

    // Активуємо обробку кирилиці (UTF-8)
    u8g2.enableUTF8Print();

    // Вмикаємо 12-бітний режим АЦП для STM32
    analogReadResolution(12);
}

void loop()
{

    // --- Зчитування вхідних даних від ESP32-C3 ---
    if (espSerial.available()) 
    {
        String incoming = espSerial.readStringUntil('\n');
        incoming.trim();
        
        // Перевірка префіксу з IP-адресою
        if (incoming.startsWith("IP:")) 
        {
            esp_ip = incoming.substring(3);
        }
        else if (incoming.startsWith("RSSI:"))
        {
            esp_rssi = incoming.substring(5);
        }
    }


    long totalReading = 0;
    const int numSamples = 32;

    // Збираємо пакет вимірів
    for (int i = 0; i < numSamples; i++) {
      totalReading += analogRead(PIN_LM35D_DIRECT);
      delay(2);
    }

    // Обчислення
    float averageReading = (float)totalReading / numSamples;


    float pin_voltage_mv = averageReading * (3300.0 / 4095.0);

    // Ділимо на коефіцієнт підсилення ОУ (3.0), щоб отримати напругу датчика
    float sensor_voltage_mv = pin_voltage_mv / 3.0;

    // Переводимо у градуси (10 мВ = 1 °C)
    float temperature = sensor_voltage_mv / 10.0;

    // Вивід на дисплей
    u8g2.clearBuffer();

    u8g2.setCursor(0, 16);
    u8g2.setFont(u8g2_font_unifont_t_cyrillic);
    u8g2.print("ТЕМПЕРАТУРА:");

    u8g2.print(temperature, 1);
    u8g2.print(" \xC2\xB0"
               "C");


    // Виведення RSSI у верхньому правому кутку або другим рядком
    u8g2.setCursor(0, 28);
    u8g2.print("Wi-Fi: ");
    u8g2.print(esp_rssi);
    u8g2.print(" dBm");

    const char *verdict = get_summer_verdict(temperature);
    u8g2.setFont(u8g2_font_unifont_t_cyrillic);
    u8g2.drawUTF8(4, 40, verdict);
    u8g2.sendBuffer();

    // Дублювання у Serial
    Serial.print(temperature, 1);
    Serial.print(" C ");
    Serial.println(verdict);

    // Відправка JSON-пакету на ESP32-C3 через espSerial (USART6)
    espSerial.print("{\"temp\":");
    espSerial.print(temperature, 1);
    espSerial.print(", \"verdict\":\"");
    espSerial.print(verdict);
    espSerial.print("\", \"ip\":\"");
    espSerial.print(esp_ip);
    espSerial.println("\"}");

    delay(1000);
}
