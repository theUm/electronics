# StickS3 Media Remote — native firmware

Це C++/PlatformIO порт Python-пульта з `../` для **ESP32-S3**. `M5Unified`/`M5GFX` обслуговують LCD, кнопки та PM1; Bosch BMI270 SensorAPI 2.86.1 — акселерометр і motion interrupt; `NimBLE-Arduino` — BLE HID. Arduino 2.0.17 збирається як компонент ESP-IDF 4.4.7, щоб SDK підтримував автоматичний light sleep. Версії зафіксовані у `platformio.ini`; драйвер Bosch з ліцензією включено в `lib/BMI270`.

## Стан

Код зібрано на Windows і прошито на фізичний StickS3 22 вересня 2026 року. Початкове pairing із Windows створило bond без PIN з другої спроби. Після повного вимкнення й увімкнення Stick журнал показав `BLE boot bonds 1`, автоматичне зашифроване підключення, підписки Windows на HID reports і команди `queued`. Користувач підтвердив, що кнопка після reconnect працює у Windows. Довготривала стабільність та окремо кожна з п'яти команд ще не перевірені. У нативній прошивці probe немає.

## Функції

- Короткий клік A: USB угору/униз — Volume +/−; горизонтально — Play/Pause; крен ліворуч/праворуч — клавіші Left/Right. Напрямок фіксується під час натискання. Утримання A понад 500 мс не створює команду.
- Три горизонтальні короткі кліки A перемикають Num Lock. Один або два горизонтальні кліки очікують 350 мс. Клік під нахилом відправляється відразу після debounce відпускання; попередні горизонтальні кліки перед ним відправляються в початковому порядку.
- Коротке натискання бічної B вмикає екран на 5 с без медіакоманди. Утримання B 1,5 с зупиняє пульт і BLE до reset; повідомлення гасне через 3 с.
- Reset/Power обробляється самим PM1: коротке натискання перезапускає, подвійне вимикає живлення. Застосунок не читає/не очищає прапорці цієї кнопки. IMU використовує GPIO IRQ для light sleep; GPIO4 power-on wake вимкнений, щоб рух не запускав вимкнений пульт. Після виправлення від 29 вересня користувач підтвердив роботу вимкнення.
- Збережено пороги нахилу, EMA та 25 мс debounce. Акселерометр працює на 50 Гц, gyro і temperature вимкнено. Через 3 с без руху/натискань застосунок припиняє опитування IMU. Any-motion (~100 mg, 40 мс) будить через BMI270 INT1 → PM1 GPIO4 → PM1 IRQ GPIO1 → ESP GPIO13. Кнопки GPIO11/12 також можуть будити. Pickup не вмикає екран.
- Статус, напрямок і батарея на дисплеї. Кнопки вмикають екран; після останнього натискання він засинає за 5 с. Рух і струшування будять пристрій для обробки кнопок, але не вмикають екран.
- Батарея опитується раз на 60 с. Wi-Fi, мікрофон, динамік і LED вимкнені. CPU 40–80 МГц; автоматичний light sleep + BLE modem sleep зберігають з'єднання. За наявності USB живлення light sleep блокується для стабільної USB-діагностики. Поки екран увімкнений, окремі PM locks блокують light sleep і фіксують APB для стабільних PWM підсвітки та SPI; після вимкнення екрана обидва звільняються. Джерело живлення перевіряється через IRQ та резервно раз на 1 с від USB / 5 с від батареї. Deep sleep немає.
- HID Service з тим самим 160-байтовим Consumer Control + Keyboard report map, Boot Keyboard Input/Output, Protocol Mode, Device Information і Battery Service. Реклама BLE: 100 мс протягом перших 30 с, потім 500 мс.

Нативна BLE-ідентичність `M5 Media Native` має окрему сталу випадкову статичну адресу, похідну від заводської MAC плати. Windows бачить її як новий пристрій. Режим безпеки — LE Secure Connections, bonding, Just Works без PIN. NimBLE зберігає peer bond у NVS; його збірка має `CONFIG_BT_NIMBLE_NVS_PERSIST=1`. Журнал USB показує кількість збережених peer bonds на старті та при зміні, а також encrypted/bonded/key size, підписки на HID reports і причину disconnect. Ключі та адреси в журналі не друкуються.

## Збірка на Windows

З папки `m5\s3\hid_remote\native`:

```powershell
$env:PLATFORMIO_CORE_DIR = Join-Path (Get-Location) '.pio-core'
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

Артефакт збірки: `.pio\build\sticks3\firmware.bin`. Команда `run` лише компілює, USB-пристрій не змінює.

`sdkconfig.defaults` є джерелом конфігурації SDK; `sdkconfig.sticks3` генерується
локально. Після зміни defaults видаліть лише цей згенерований файл і перебудуйте.
Попередження `int-in-bool-context` у NimBLE 2.5.1 залишене попередженням, а не
помилкою ESP-IDF; бібліотека не патчиться.

Локальні перевірки:

```powershell
py -3 tests\verify_report_map.py
py -3 tests\verify_build.py
g++ -std=c++11 -Wall -Wextra -Werror tests\input_test.cpp -o .pio\input_test.exe
& .pio\input_test.exe
```

## Прошивання та діагностика

Прошивання замінює UiFlow та його таблицю розділів. Повна перевірена 8 MB копія попередньої флешпам'яті лежить у `../backups/sticks3-20260922-full-flash/flash.bin`; її SHA-256 записаний у `manifest.txt`. Ця копія містить стан на момент знімка, включно з тимчасовим bond probe. Попередній `main.py` збережений усередині копії як `/flash/main.py.bond-probe-backup`; після повного відновлення образу його потрібно буде повернути на місце для автозапуску Python-пульта.

Для StickS3 перед upload затисніть бічну Reset приблизно на 2 с, дочекайтеся миготіння зеленого LED і відпустіть. У `platformio.ini` встановлено `board_upload.before_reset = no_reset`, щоб esptool не виводив плату з цього режиму перед записом.

Після upload коротко натисніть Reset для виходу з download mode. У журналі має бути `SPI_FAST_FLASH_BOOT`, а не `DOWNLOAD(USB/UART0)`. Для читання журналу, який переживає перепідключення USB:

```powershell
py -3 tools\monitor_reconnect.py --seconds 120
```

Відкриття USB-порту може перезапустити Stick. Після старту `BLE boot bonds 1` означає, що peer bond збережено в NVS. Стан `STATUS Connected encrypted=1 bonded=1` і підписки на HID reports підтверджують відновлення BLE HID. Якщо reconnect знову зривається, збережіть журнали USB і Windows від двох стартів.

Оновлення поточної нативної прошивки використовує ту саму таблицю розділів і
не стирає NVS. Команда upload з цієї папки:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload --upload-port COM4
```

### Motion / power diagnostics

- `IMU accel=50Hz gyro=off motion_irq=1` — hardware motion wake налаштовано.
- `POWER idle: waiting for motion` — застосунок заблокований до події/дедлайну;
  це саме по собі не доводить фактичного сну ESP.
- `POWER active` — застосунок повернувся до читання нахилу.
- `INPUT release_to_queue_ms=...` — час від останнього raw відпускання до черги;
  це не вимір затримки Windows, і для buffered/mixed кліків він не є end-to-end.
- На USB раз на хвилину друкується кількість motion wake та таблиця PM locks/stats.
  Для виміру економії потрібна робота від батареї; USB навмисно тримає PM lock.
- Помилка motion IRQ вмикає fallback polling; помилка читання прискорення
  блокує команду до валідного виміру.

### Rollback

Перед міграцією збережено локальні bootloader/partition/app binaries у
`../backups/native-185a360-before-motion/` (ігнорується Git).
SHA-256 попереднього `firmware.bin`:
`726FF2A9DB3D73D09A1531EA63125C2BA14CA985EE97833BD327CFBEE46CF889`.
У download mode поверніть ці образи за адресами bootloader `0x0`, partitions
`0x8000`, firmware `0x10000` через esptool `--before no_reset write_flash`.
Не використовуйте `erase_flash`: NVS і поточний Windows bond мають зберегтися.

Приймальні перевірки та результати: `POWER_VALIDATION.md`.
