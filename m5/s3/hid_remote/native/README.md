# StickS3 Media Remote — native firmware

Це окремий C++/PlatformIO порт Python-пульта з `../`. Чип StickS3 — **ESP32-S3**. Проєкт використовує офіційну конфігурацію плати M5Stack, `M5Unified`/`M5GFX` для LCD, кнопок та BMI270, `M5PM1` для живлення й `NimBLE-Arduino` для BLE HID. Версії та commit SHA зафіксовані у `platformio.ini`.

## Стан

Код зібрано на Windows і прошито на фізичний StickS3 22 вересня 2026 року. Початкове pairing із Windows створило bond без PIN з другої спроби. Після повного вимкнення й увімкнення Stick журнал показав `BLE boot bonds 1`, автоматичне зашифроване підключення, підписки Windows на HID reports і команди `queued`. Користувач підтвердив, що кнопка після reconnect працює у Windows. Довготривала стабільність та окремо кожна з п'яти команд ще не перевірені. У нативній прошивці probe немає.

## Функції

- Короткий клік A: USB угору/униз — Volume +/−; горизонтально — Play/Pause; крен ліворуч/праворуч — клавіші Left/Right. Напрямок фіксується під час натискання. Утримання A понад 500 мс не створює команду.
- Три короткі кліки A поспіль перемикають Num Lock, лише якщо кожен клік почався в горизонтальному положенні; команди Play/Pause при цьому не відправляються. Якщо хоча б один клік зроблено під нахилом, усі три виконують звичайні команди. Один або два кліки очікують паузу 350 мс, після чого відправляються як раніше.
- Утримання B 1,5 с зупиняє пульт і BLE до reset.
- Ті самі пороги нахилу, EMA та 25 мс debounce, що у `../remote_input.py`; IMU опитується кожні 50 мс без натиснутої A для виявлення тряски.
- Статус, напрямок і батарея на дисплеї. Після натискання екран засинає за 5 с; два сильні відхилення прискорення за 300 мс будять його на 1,3 с. Повторне спрацювання обмежене паузою 1,6 с.
- Battery Service оновлюється після зміни відсотка; батарея опитується раз на 60 с. Wi-Fi, мікрофон, динамік і LED вимкнені; CPU 80 МГц; deep sleep немає.
- HID Service з тим самим 160-байтовим Consumer Control + Keyboard report map, Boot Keyboard Input/Output, Protocol Mode, Device Information і Battery Service. Реклама BLE: 100 мс протягом перших 30 с, потім 500 мс.

Нативна BLE-ідентичність `M5 Media Native` має окрему сталу випадкову статичну адресу, похідну від заводської MAC плати. Windows бачить її як новий пристрій. Режим безпеки — LE Secure Connections, bonding, Just Works без PIN. NimBLE зберігає peer bond у NVS; його збірка має `CONFIG_BT_NIMBLE_NVS_PERSIST=1`. Журнал USB показує кількість збережених peer bonds на старті та при зміні, а також encrypted/bonded/key size, підписки на HID reports і причину disconnect. Ключі та адреси в журналі не друкуються.

## Збірка на Windows

З папки `m5\s3\hid_remote\native`:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

Артефакт збірки: `.pio\build\sticks3\firmware.bin`. Команда `run` лише компілює, USB-пристрій не змінює.

Локальні перевірки:

```powershell
py -3 tests\verify_report_map.py
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
