# Semáforo de sitios caídos (ESP32)

Hardware: LED verde → GPIO 25, LED rojo → GPIO 26 (ambos con 220 Ω a GND), buzzer activo → GPIO 27.

## Preparación (una vez)

```sh
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
cp semaforo_sitios/secrets.example.h semaforo_sitios/secrets.h   # y edita SSID/clave
```

## Compilar, subir y ver el monitor serie

```sh
arduino-cli board list                     # muestra el puerto, p. ej. /dev/ttyUSB0 o COM3
arduino-cli compile --fqbn esp32:esp32:esp32 semaforo_sitios
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 semaforo_sitios
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
```

Si la subida se queda en `Connecting....`, mantén pulsado el botón BOOT de la placa hasta que empiece a escribir.

## Configuración

Al inicio de `semaforo_sitios/semaforo_sitios.ino`:

- `SITIOS[]`: lista de URLs (http o https), una por línea.
- `INTERVALO_REVISION_MS`: cada cuánto se revisa (por defecto `5UL * 60UL * 1000UL` = 5 min).
