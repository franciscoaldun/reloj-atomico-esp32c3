# Reloj atómico NTP + clima — ESP32-C3

Reloj de escritorio para **ESP32-C3 SuperMini** con pantalla OLED de 0,91". Sincroniza la hora con la red mundial **NTP** (exactitud de milisegundos, re-sincronización automática) y muestra el **clima en vivo** de dos ciudades más el **pronóstico de lluvia por horas**, todo en una fuente futurista de 7 segmentos dibujada píxel a píxel.

Escrito en C sobre **ESP-IDF v6.1**.

![target](https://img.shields.io/badge/target-ESP32--C3-red) ![framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.1-blue)

## Características
- **Hora por NTP** con exactitud de milisegundos; re-sincroniza cada 5 min y corrige la deriva del cristal.
- **3 pantallas que rotan solas:** reloj grande (HH:MM:SS + ms), temperatura actual de dos ciudades, y pronóstico de mañana.
- **Pronóstico de lluvia por horas**: parsea el JSON de OpenWeather *dentro del microcontrolador*, sin librerías pesadas, e indica el rango horario y la hora pico de lluvia.
- **Fuente propia** de 7 segmentos hexagonal, renderizada píxel a píxel.
- **Red en tarea aparte** (FreeRTOS): la pantalla nunca se congela mientras baja datos.
- Zona horaria de Chile con horario de verano automático.

## Hardware
| Componente | Detalle |
|---|---|
| MCU | ESP32-C3 SuperMini |
| Pantalla | OLED SSD1306 128×32 (I2C, dir. 0x3C) |
| Conexión | `SDA → GPIO8`, `SCL → GPIO9`, `VCC → 3V3`, `GND → GND` |

## Configuración
Las credenciales van en `main/secrets.h`, que **no** se versiona. Crea el tuyo a partir de la plantilla:

```bash
cp main/secrets.h.example main/secrets.h
# edita main/secrets.h con tu WiFi y tu API key de OpenWeather
```

La ciudad/coordenadas y la zona horaria se ajustan al inicio de `main/main.c`.

## Compilar y flashear
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

## Licencia
MIT — ver [LICENSE](LICENSE).
