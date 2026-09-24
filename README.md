# SEN66 → Apple HomeKit

This project uses an **ESP32-S3** to connect the **Sensirion SEN66** environmental sensor directly to **Apple HomeKit** using [HomeSpan](https://github.com/HomeSpan/HomeSpan).

The SEN66 can measure more environmental parameters than Apple HomeKit has native support for. As a result, **not every SEN66 measurement is exposed to HomeKit**.

The ESP32 reads all available SEN66 measurements and:

* Displays **all SEN66 measurements** locally on a SH1106 OLED display.
* Exposes **HomeKit-compatible measurements** to Apple Home.
* Keeps measurements without a suitable HomeKit representation local to the device.

### SEN66 Measurements

| Measurement       | OLED | Apple Home |
| ----------------- | :--: | :--------: |
| PM1.0             |   ✓  |      —     |
| PM2.5             |   ✓  |      ✓     |
| PM4.0             |   ✓  |      —     |
| PM10              |   ✓  |      ✓     |
| CO₂               |   ✓  |      ✓     |
| Temperature       |   ✓  |      ✓     |
| Relative Humidity |   ✓  |      ✓     |
| VOC Index         |   ✓  |      —     |
| NOx Index         |   ✓  |      —     |

The goal is to make use of as much of the SEN66's data as possible while only exposing measurements to HomeKit where the representation is appropriate.
