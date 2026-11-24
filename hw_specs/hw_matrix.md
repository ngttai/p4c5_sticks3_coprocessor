# KSDIY ESP32-P4/C5 4.3" – Hardware Matrix / Ma trận phần cứng

> As of / Cập nhật: 2026-11-13 (rev 5)
> Sources / Nguồn: `ksdiy_p4c5_bsp` v1.3.3, `ksdiy_c5_sdio_flasher` v1.1.0, esptool output, **stock firmware boot log**, `managed_components` (`esp_lcd_st7102` 1.0.4, `esp_lcd_touch_st7123` 1.0.2~1, `pmic_axp2101` 1.1.2); rev 4 adds / rev 4 bổ sung: `hw_specs/RESOURCE` (`schematic.pdf` dated 2026-06-04, `pin_out.md`, board photos / ảnh board).
> Upstream: https://github.com/kevincoooool/ESP32P4_KSDIY
> **(?)** = not yet known / chưa biết. **[log]** = stock boot log / log boot firmware gốc. **[sch]** = schematic / sơ đồ nguyên lý. **[pin]** = `pin_out.md`. **[photo]** = annotated board photo / ảnh board có chú thích.

---

## 1. English

### 1.1 Hardware matrix

| # | Subsystem | Component | Key specs | Interface | Pins (P4 GPIO) / address | Source |
|---|-----------|-----------|-----------|-----------|--------------------------|--------|
| 1 | Host MCU | ESP32-P4 (Yuying ESP32-P4C5 module) | Rev v3.2, eFuse block rev v1.2, dual core + LP core, 400 MHz, 40 MHz crystal, MAC 80:f1:b2:d6:1f:70 | – | UART0 TXD = 37, RXD = 38 | [log], esptool, [sch] |
| 2 | Flash | Boya SPI flash | 16 MB, QIO 80 MHz, mfr ID 0x68, dev 0x4018 | SPI | – | [log]. Enable `SPI_FLASH_SUPPORT_BOYA_CHIP` (currently generic driver) |
| 3 | PSRAM | AP Memory hex PSRAM (gen 4) | 32 MB (256 Mbit density per die ID), X16 hex mode, 250 MHz, 2048 B burst | Hex PSRAM | – | [log] |
| 4 | Wi-Fi/BLE | ESP32-C5 (inside the module) | ESP-Hosted slave FW 2.12.13, chip id 0x17, board type 23; WLAN + BLE-only HCI over SDIO; IPEX antenna | SDIO slot 1, 4-bit, 40 MHz, streaming | D0–D3 = 14–17, CLK = 18, CMD = 19 (internal to the module, not on the schematic) | [log], flasher |
| 5 | C5 control | EN/RESET, BOOT | RESET high = run; BOOT low at reset = download | GPIO | RESET = 54, BOOT = 2 (net `C5_BOOT`) | flasher, [sch] |
| 6 | 5 V boost | SY7088 | AXP_VSYS → BOOST_5V; feeds USB-A VBUS, JP1 pin 2, RS485 transceiver and MCP4725 | GPIO enable, active high; off at boot (EN 1 MΩ pull-down) | 20 (`BOOST_ON`) | flasher, [sch] |
| 7 | Display | 4.3" ST7102, panel YDP430BT009-V1 | Yuying (鱼鹰光电) module; 480×800, 60 Hz, RGB565 (default)/RGB888; LCD ID 80 A0 FB (read from reg 0x04); DPI clock 37.8 MHz; H sync/back/front = 2/40/40, V sync/back/front = 2/10/310; COLMOD 0x55/0x66/0x77; 30-pin 0.5 mm FPC | MIPI DSI 2 lanes @ 820 Mbps [log] (driver macro default: 500 Mbps) | RST = 22 (shared with touch; 10 kΩ pull-up + 1 µF) | BSP, [log], `esp_lcd_st7102` 1.0.4, [sch] |
| 8 | Backlight | SY7200 LED driver | 40 mA (200 mV / 5.1 Ω); PWM 10 kHz, 10-bit, active high, LEDC T1/CH1 | GPIO PWM on EN | 6 (`LCD_BL`) | BSP, [log], [sch] |
| 9 | Touch | ST7123 | FW 1.71.1.5, 480×800, up to 5 touches, 400 kHz (driver macro default: 100 kHz); 16-bit registers; INT active-low open-drain with internal pull-up; per-finger track IDs; max 10 reports | I2C0 | addr 0x55; SDA = 7, SCL = 8, INT = 23, RST = 22 | BSP, [log], `esp_lcd_touch_st7123` 1.0.2~1, [sch] |
| 10 | PMIC | AXP2101 | DCDC1 3.3 V → VCC3V3; ALDO1 1.8 V → camera; ALDO3 3.3 V → codecs + IMU; ALDO4 2.9 V → camera AVDD; DCDC2–4, ALDO2, BLDO, DLDO unused; driver I2C at 100 kHz, 100 ms timeout; `axp2101_init()` sets no charge parameters | I2C0 | addr 0x34; IRQ = 21 (via 2N7002 level shifter) | BSP, [log], `pmic_axp2101` 1.1.2, [sch] |
| 11 | Battery / charging | AXP2101 charger | Battery connector J2 (MX1.25-2P); 10 kΩ B3950 NTC on TS; red charge LED on CHGLED; power key BTN1 → PWRON | – | – | [sch] |
| 12 | Audio DAC/codec | ES8311 | Slave mode; I2S TX 16 kHz, MCLK 256×; differential out to the amp; powered from ALDO3 | I2C0 + I2S0 | addr 0x18; MCLK = 13, SCLK = 12, LRCK = 10, DSDIN (P4 → codec) = 9 | [log], [sch], [pin] |
| 13 | Speaker amp | NS4150B (mono class D) | Powered from AXP_VSYS; speaker connector J1 (MX1.25-2P) | GPIO enable, active high; off at boot | `PA_CTRL` = 3 (10 kΩ pull-down) | [sch], [pin] |
| 14 | Audio ADC | ES7210 | Slave mode; TDM RX 16-bit 16 kHz; MIC1 + MIC2 = two on-board MEMS mics (MSM381A3729H9CP); MIC3 = AEC reference looped back from the ES8311 output; MIC4 unused | I2C0 + I2S0 TDM | addr 0x40 (AD0 = AD1 = GND); SDOUT (codec → P4) = 11; shares MCLK/SCLK/LRCK with ES8311 | [log], [sch], [pin] |
| 15 | Camera | SC2336 / OV2710 (CSI) | Sensor PID 0xcb3a; `esp_video`; 24-pin 0.5 mm FPC; 2 data lanes; 24 MHz on-board oscillator for XCLK; reset by RC only (no GPIO); AVDD from ALDO4 (2.9 V) | MIPI-CSI + SCCB on I2C0 | addr 0x30; dedicated CSI pins | [log], [sch] |
| 16 | USB HS | P4 high-speed USB | One port wired to both a Type-C (J3) and a Type-A host socket (VBUS from BOOST_5V); used by the stock "USB camera" page | USB 2.0 HS | Dedicated DP/DM pins | [sch], [photo], [log] |
| 17 | USB debug | USB-Serial/JTAG | Type-C "DEBUG" (J5) | USB FS | 24 / 25 | [sch], esptool |
| 18 | USB-UART | CH343P | Type-C "USB-TTL" (J6); RTS → CHIP_PU, DTR → GPIO35 (auto-download) | UART0 | TXD = 37, RXD = 38 | [sch] |
| 19 | microSD | TF card socket J4 | 4-bit SDMMC; powered from ESP_LDO_VO4; 51 kΩ pull-ups; no card-detect | SDMMC | CLK = 43, CMD = 44, D0–D3 = 39–42 | [sch], [pin] |
| 20 | 4G modem | ML307C (CMCC AT firmware) | Supplied straight from the battery rail through a P-MOSFET; nano-SIM socket; IPEX antenna | UART (level-shifted) | P4 TX = 53, P4 RX = 52, DTR = 51, power enable = 4 (active high; off at boot) | [sch], [pin], [photo] |
| 21 | RS485 | SN65HVD3082E | Powered from BOOST_5V; 120 Ω termination fitted; A/B on JP1 pins 20/19 | UART | TX = 31, RX = 33, DE = 32 | [sch], [pin] |
| 22 | Analog out | MCP4725 12-bit DAC | Powered from BOOST_5V, so it only answers when GPIO20 is high; output on JP1 pin 1 | I2C0 | addr 0x60 expected (A0 = GND); not seen in the stock scan | [sch], [photo] |
| 23 | IMU | LSM6DS3TR-C | 6-axis; powered from ALDO3; INT1/INT2 not connected | I2C0 | addr 0x6A | [log], [sch] |
| 24 | Unknown I2C | ? | Responds on the I2C scan; no discrete part at this address on the schematic | I2C0 | addr 0x28 | [log] |
| 25 | RGB LED | WS2812B | One addressable LED; a separate white LED shows 3.3 V power | GPIO | 34 | [sch], [pin] |
| 26 | Buttons | BOOT, IO0, RST, POWER | BOOT and IO0 active low with 10 kΩ pull-ups; RST pulls CHIP_PU low; POWER goes to the PMIC. Stock log names: BTN1 = GPIO35, BTN2 = GPIO0. Schematic designators: BTN1 = POWER, BTN2 = GPIO35, BTN3 = GPIO0 | GPIO | BOOT = 35, IO0 = 0 | [log], [sch] |
| 27 | Expansion | JP1 2×10 header, 2.54 mm | 11 GPIOs through 100 Ω series resistors, DAC, RS485, 5 V, 3.3 V | – | 26–30, 45–50 | [sch] |
| 28 | Mechanical | PCB | 61.28 × 106.48 mm; M3 holes on 53.22 × 98.42 mm centres; silkscreen date 20260521 | – | – | RESOURCE |

### 1.2 I2C0 bus map (SDA = 7, SCL = 8, 2.2 kΩ pull-ups to 3.3 V)

| Address | Device |
|---------|--------|
| 0x18 | ES8311 codec |
| 0x28 | unknown (not on the schematic) |
| 0x30 | SC2336 camera (SCCB) |
| 0x34 | AXP2101 PMIC |
| 0x40 | ES7210 ADC |
| 0x55 | ST7123 touch |
| 0x60 | MCP4725 DAC (expected; needs BOOST_5V) |
| 0x6A | LSM6DS3TR-C IMU |

### 1.3 Pin map (P4 GPIOs, sorted)

| GPIO | Function |
|------|----------|
| 0 | IO0 button (active low); also P4 strapping pin |
| 1 | Not used on the board |
| 2 | C5 BOOT strap |
| 3 | Speaker amp enable (`PA_CTRL`), active high |
| 4 | 4G power enable, active high |
| 5 | Not used on the board |
| 6 | LCD backlight PWM |
| 7 | I2C0 SDA |
| 8 | I2C0 SCL |
| 9 | I2S0 DSDIN (P4 → ES8311) |
| 10 | I2S0 LRCK |
| 11 | I2S0 SDOUT (ES7210 → P4) |
| 12 | I2S0 SCLK |
| 13 | I2S0 MCLK |
| 14–17 | SDIO D0–D3 (C5, inside module) |
| 18 | SDIO CLK (C5, inside module) |
| 19 | SDIO CMD (C5, inside module) |
| 20 | 5 V boost enable |
| 21 | AXP2101 IRQ |
| 22 | LCD + touch RESET (shared) |
| 23 | Touch INT |
| 24 / 25 | USB-Serial/JTAG (DEBUG Type-C) |
| 26–30 | JP1 header |
| 31 | RS485 TX |
| 32 | RS485 DE |
| 33 | RS485 RX |
| 34 | WS2812B data |
| 35 | BOOT button (active low); boot-mode strap; CH343P DTR |
| 36 | 10 kΩ pull-up only |
| 37 | UART0 TXD (console, to CH343P) |
| 38 | UART0 RXD (console, from CH343P) |
| 39–42 | microSD D0–D3 |
| 43 | microSD CLK |
| 44 | microSD CMD |
| 45–50 | JP1 header |
| 51 | 4G DTR |
| 52 | 4G UART, P4 RX |
| 53 | 4G UART, P4 TX |
| 54 | C5 EN/RESET (inside module) |

### 1.4 JP1 expansion header (2×10, 2.54 mm)

| Pin | Signal | Pin | Signal |
|-----|--------|-----|--------|
| 1 | DAC (MCP4725 out) | 2 | BOOST_5V |
| 3 | GND | 4 | VCC3V3 |
| 5 | GPIO50 | 6 | GPIO49 |
| 7 | GPIO48 | 8 | GPIO47 |
| 9 | GPIO46 | 10 | GND |
| 11 | GPIO45 | 12 | GPIO26 |
| 13 | GPIO27 | 14 | GPIO28 |
| 15 | GPIO29 | 16 | GPIO30 |
| 17 | GND | 18 | GND |
| 19 | RS485 B | 20 | RS485 A |

### 1.5 Open questions
- Identity of I2C device 0x28 (no discrete part on the schematic; it may sit inside the LCD/touch or camera module).
- Charge current and charge voltage actually programmed into the AXP2101 (hardware is known; the driver init sets nothing).
- Source of the 820 Mbps DSI lane rate (the `esp_lcd_st7102` macro default is 500 Mbps).
- Whether the MCP4725 answers at 0x60 once BOOST_5V is on.

---

## 2. Tiếng Việt

### 2.1 Ma trận phần cứng

| # | Phân hệ | Linh kiện | Thông số chính | Giao tiếp | Chân (GPIO của P4) / địa chỉ | Nguồn |
|---|---------|-----------|----------------|-----------|------------------------------|-------|
| 1 | MCU chính | ESP32-P4 (module Yuying ESP32-P4C5) | Rev v3.2, eFuse block rev v1.2, 2 core + LP core, 400 MHz, thạch anh 40 MHz, MAC 80:f1:b2:d6:1f:70 | – | UART0 TXD = 37, RXD = 38 | [log], esptool, [sch] |
| 2 | Flash | Flash SPI hãng Boya | 16 MB, QIO 80 MHz, mfr ID 0x68, dev 0x4018 | SPI | – | [log]. Nên bật `SPI_FLASH_SUPPORT_BOYA_CHIP` (hiện dùng driver chung) |
| 3 | PSRAM | AP Memory hex PSRAM (gen 4) | 32 MB (mật độ die 256 Mbit), hex X16, 250 MHz, burst 2048 B | Hex PSRAM | – | [log] |
| 4 | Wi-Fi/BLE | ESP32-C5 (nằm trong module) | Slave ESP-Hosted FW 2.12.13, chip id 0x17, board type 23; WLAN + HCI BLE-only qua SDIO; anten IPEX | SDIO slot 1, 4-bit, 40 MHz, streaming | D0–D3 = 14–17, CLK = 18, CMD = 19 (nối bên trong module, không có trên sơ đồ) | [log], flasher |
| 5 | Điều khiển C5 | EN/RESET, BOOT | RESET cao = chạy; BOOT thấp khi reset = chế độ nạp | GPIO | RESET = 54, BOOT = 2 (net `C5_BOOT`) | flasher, [sch] |
| 6 | Tăng áp 5 V | SY7088 | AXP_VSYS → BOOST_5V; cấp cho VBUS cổng USB-A, chân 2 của JP1, IC RS485 và MCP4725 | GPIO bật, mức cao = bật; tắt khi boot (EN kéo xuống 1 MΩ) | 20 (`BOOST_ON`) | flasher, [sch] |
| 7 | Màn hình | ST7102 4.3", tấm YDP430BT009-V1 | Module Yuying (鱼鹰光电); 480×800, 60 Hz, RGB565 (mặc định)/RGB888; LCD ID 80 A0 FB (đọc từ thanh ghi 0x04); DPI clock 37.8 MHz; H sync/back/front = 2/40/40, V sync/back/front = 2/10/310; COLMOD 0x55/0x66/0x77; FPC 30 chân 0.5 mm | MIPI DSI 2 lane @ 820 Mbps [log] (macro driver mặc định: 500 Mbps) | RST = 22 (chung với cảm ứng; pull-up 10 kΩ + 1 µF) | BSP, [log], `esp_lcd_st7102` 1.0.4, [sch] |
| 8 | Đèn nền | IC lái LED SY7200 | 40 mA (200 mV / 5.1 Ω); PWM 10 kHz, 10-bit, tích cực mức cao, LEDC T1/CH1 | PWM vào chân EN | 6 (`LCD_BL`) | BSP, [log], [sch] |
| 9 | Cảm ứng | ST7123 | FW 1.71.1.5, 480×800, tối đa 5 điểm, 400 kHz (macro driver mặc định: 100 kHz); thanh ghi 16-bit; INT tích cực mức thấp, open-drain, có pull-up nội; track ID riêng từng ngón; tối đa 10 báo cáo | I2C0 | addr 0x55; SDA = 7, SCL = 8, INT = 23, RST = 22 | BSP, [log], `esp_lcd_touch_st7123` 1.0.2~1, [sch] |
| 10 | Quản lý nguồn | AXP2101 | DCDC1 3.3 V → VCC3V3; ALDO1 1.8 V → camera; ALDO3 3.3 V → codec + IMU; ALDO4 2.9 V → AVDD camera; DCDC2–4, ALDO2, BLDO, DLDO không dùng; driver chạy I2C 100 kHz, timeout 100 ms; `axp2101_init()` không đặt thông số sạc | I2C0 | addr 0x34; IRQ = 21 (qua mạch dịch mức 2N7002) | BSP, [log], `pmic_axp2101` 1.1.2, [sch] |
| 11 | Pin / sạc | Mạch sạc AXP2101 | Đầu nối pin J2 (MX1.25-2P); NTC 10 kΩ B3950 ở chân TS; LED sạc đỏ ở CHGLED; nút nguồn BTN1 → PWRON | – | – | [sch] |
| 12 | DAC/codec âm thanh | ES8311 | Chế độ slave; I2S TX 16 kHz, MCLK 256×; ngõ ra vi sai tới ampli; cấp nguồn từ ALDO3 | I2C0 + I2S0 | addr 0x18; MCLK = 13, SCLK = 12, LRCK = 10, DSDIN (P4 → codec) = 9 | [log], [sch], [pin] |
| 13 | Ampli loa | NS4150B (class D mono) | Cấp nguồn từ AXP_VSYS; đầu nối loa J1 (MX1.25-2P) | GPIO bật, mức cao = bật; tắt khi boot | `PA_CTRL` = 3 (pull-down 10 kΩ) | [sch], [pin] |
| 14 | ADC âm thanh | ES7210 | Chế độ slave; TDM RX 16-bit 16 kHz; MIC1 + MIC2 = hai mic MEMS trên board (MSM381A3729H9CP); MIC3 = tín hiệu tham chiếu AEC lấy vòng từ ngõ ra ES8311; MIC4 không dùng | I2C0 + I2S0 TDM | addr 0x40 (AD0 = AD1 = GND); SDOUT (codec → P4) = 11; dùng chung MCLK/SCLK/LRCK với ES8311 | [log], [sch], [pin] |
| 15 | Camera | SC2336 / OV2710 (CSI) | PID cảm biến 0xcb3a; `esp_video`; FPC 24 chân 0.5 mm; 2 lane dữ liệu; dao động 24 MHz trên board cấp XCLK; reset chỉ bằng mạch RC (không có GPIO); AVDD từ ALDO4 (2.9 V) | MIPI-CSI + SCCB trên I2C0 | addr 0x30; chân CSI chuyên dụng | [log], [sch] |
| 16 | USB HS | USB tốc độ cao của P4 | Một cổng nối chung ra Type-C (J3) và ổ Type-A host (VBUS lấy từ BOOST_5V); trang "USB camera" của firmware gốc dùng cổng này | USB 2.0 HS | Chân DP/DM chuyên dụng | [sch], [photo], [log] |
| 17 | USB debug | USB-Serial/JTAG | Type-C "DEBUG" (J5) | USB FS | 24 / 25 | [sch], esptool |
| 18 | USB-UART | CH343P | Type-C "USB-TTL" (J6); RTS → CHIP_PU, DTR → GPIO35 (tự vào chế độ nạp) | UART0 | TXD = 37, RXD = 38 | [sch] |
| 19 | microSD | Khe thẻ TF J4 | SDMMC 4-bit; cấp nguồn từ ESP_LDO_VO4; pull-up 51 kΩ; không có chân phát hiện thẻ | SDMMC | CLK = 43, CMD = 44, D0–D3 = 39–42 | [sch], [pin] |
| 20 | Modem 4G | ML307C (firmware AT của CMCC) | Lấy nguồn thẳng từ đường pin qua P-MOSFET; khe nano-SIM; anten IPEX | UART (có dịch mức) | P4 TX = 53, P4 RX = 52, DTR = 51, bật nguồn = 4 (mức cao = bật; tắt khi boot) | [sch], [pin], [photo] |
| 21 | RS485 | SN65HVD3082E | Cấp nguồn từ BOOST_5V; có sẵn điện trở đầu cuối 120 Ω; A/B ra chân 20/19 của JP1 | UART | TX = 31, RX = 33, DE = 32 | [sch], [pin] |
| 22 | Ngõ ra analog | DAC 12-bit MCP4725 | Cấp nguồn từ BOOST_5V nên chỉ phản hồi khi GPIO20 ở mức cao; ngõ ra ở chân 1 của JP1 | I2C0 | dự kiến addr 0x60 (A0 = GND); không thấy trong lần quét của firmware gốc | [sch], [photo] |
| 23 | IMU | LSM6DS3TR-C | 6 trục; cấp nguồn từ ALDO3; INT1/INT2 không nối | I2C0 | addr 0x6A | [log], [sch] |
| 24 | I2C chưa rõ | ? | Phản hồi khi quét I2C; sơ đồ không có linh kiện rời nào ở địa chỉ này | I2C0 | addr 0x28 | [log] |
| 25 | LED RGB | WS2812B | Một LED địa chỉ hoá; LED trắng riêng báo nguồn 3.3 V | GPIO | 34 | [sch], [pin] |
| 26 | Nút bấm | BOOT, IO0, RST, POWER | BOOT và IO0 tích cực mức thấp, pull-up 10 kΩ; RST kéo CHIP_PU xuống thấp; POWER nối vào PMIC. Tên trong log firmware gốc: BTN1 = GPIO35, BTN2 = GPIO0. Ký hiệu trên sơ đồ: BTN1 = POWER, BTN2 = GPIO35, BTN3 = GPIO0 | GPIO | BOOT = 35, IO0 = 0 | [log], [sch] |
| 27 | Mở rộng | Hàng chân JP1 2×10, 2.54 mm | 11 GPIO qua điện trở nối tiếp 100 Ω, DAC, RS485, 5 V, 3.3 V | – | 26–30, 45–50 | [sch] |
| 28 | Cơ khí | PCB | 61.28 × 106.48 mm; lỗ M3 cách tâm 53.22 × 98.42 mm; ngày in trên board 20260521 | – | – | RESOURCE |

### 2.2 Bản đồ bus I2C0 (SDA = 7, SCL = 8, pull-up 2.2 kΩ lên 3.3 V)

| Địa chỉ | Thiết bị |
|---------|----------|
| 0x18 | Codec ES8311 |
| 0x28 | chưa rõ (không có trên sơ đồ) |
| 0x30 | Camera SC2336 (SCCB) |
| 0x34 | PMIC AXP2101 |
| 0x40 | ADC ES7210 |
| 0x55 | Cảm ứng ST7123 |
| 0x60 | DAC MCP4725 (dự kiến; cần BOOST_5V) |
| 0x6A | IMU LSM6DS3TR-C |

### 2.3 Bản đồ chân (GPIO của P4, theo thứ tự)

| GPIO | Chức năng |
|------|-----------|
| 0 | Nút IO0 (mức thấp); cũng là chân strapping của P4 |
| 1 | Board không dùng |
| 2 | Chân BOOT của C5 |
| 3 | Bật ampli loa (`PA_CTRL`), mức cao |
| 4 | Bật nguồn 4G, mức cao |
| 5 | Board không dùng |
| 6 | PWM đèn nền LCD |
| 7 | I2C0 SDA |
| 8 | I2C0 SCL |
| 9 | I2S0 DSDIN (P4 → ES8311) |
| 10 | I2S0 LRCK |
| 11 | I2S0 SDOUT (ES7210 → P4) |
| 12 | I2S0 SCLK |
| 13 | I2S0 MCLK |
| 14–17 | SDIO D0–D3 (C5, trong module) |
| 18 | SDIO CLK (C5, trong module) |
| 19 | SDIO CMD (C5, trong module) |
| 20 | Bật tăng áp 5 V |
| 21 | IRQ của AXP2101 |
| 22 | RESET chung LCD + cảm ứng |
| 23 | INT cảm ứng |
| 24 / 25 | USB-Serial/JTAG (Type-C DEBUG) |
| 26–30 | Hàng chân JP1 |
| 31 | RS485 TX |
| 32 | RS485 DE |
| 33 | RS485 RX |
| 34 | Dữ liệu WS2812B |
| 35 | Nút BOOT (mức thấp); chân chọn chế độ boot; DTR của CH343P |
| 36 | Chỉ có pull-up 10 kΩ |
| 37 | UART0 TXD (console, tới CH343P) |
| 38 | UART0 RXD (console, từ CH343P) |
| 39–42 | microSD D0–D3 |
| 43 | microSD CLK |
| 44 | microSD CMD |
| 45–50 | Hàng chân JP1 |
| 51 | DTR của 4G |
| 52 | UART 4G, P4 RX |
| 53 | UART 4G, P4 TX |
| 54 | EN/RESET của C5 (trong module) |

### 2.4 Hàng chân mở rộng JP1 (2×10, 2.54 mm)

| Chân | Tín hiệu | Chân | Tín hiệu |
|------|----------|------|----------|
| 1 | DAC (ngõ ra MCP4725) | 2 | BOOST_5V |
| 3 | GND | 4 | VCC3V3 |
| 5 | GPIO50 | 6 | GPIO49 |
| 7 | GPIO48 | 8 | GPIO47 |
| 9 | GPIO46 | 10 | GND |
| 11 | GPIO45 | 12 | GPIO26 |
| 13 | GPIO27 | 14 | GPIO28 |
| 15 | GPIO29 | 16 | GPIO30 |
| 17 | GND | 18 | GND |
| 19 | RS485 B | 20 | RS485 A |

### 2.5 Câu hỏi còn mở
- Thiết bị I2C địa chỉ 0x28 là gì (sơ đồ không có linh kiện rời; có thể nằm trong module LCD/cảm ứng hoặc module camera).
- Dòng sạc và áp sạc thực tế được nạp vào AXP2101 (phần cứng đã rõ; hàm init của driver không đặt gì).
- Nguồn gốc tốc độ DSI 820 Mbps/lane (macro `esp_lcd_st7102` mặc định 500 Mbps).
- MCP4725 có phản hồi ở 0x60 khi đã bật BOOST_5V hay không.
