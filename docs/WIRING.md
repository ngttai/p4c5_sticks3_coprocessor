# Sơ đồ đấu dây

Hai board nối bằng 11 dây tín hiệu và GND. Phía P4C5 lấy từ header JP1 (11 GPIO trống: 26 đến 30 và 45 đến 50, mỗi chân qua điện trở nối tiếp 100 Ω). Phía StickS3 lấy từ header Hat2 và cổng Grove.

## Sơ đồ

```mermaid
flowchart LR
    subgraph P4C5["KSDIY P4C5 (JP1)"]
        P26["GPIO26 SPI CLK"]
        P27["GPIO27 SPI MOSI"]
        P28["GPIO28 SPI MISO"]
        P29["GPIO29 SPI CS"]
        P30["GPIO30 Handshake"]
        P45["GPIO45 Data ready"]
        P46["GPIO46 I2S BCLK"]
        P47["GPIO47 I2S WS"]
        P48["GPIO48 I2S mic in"]
        P49["GPIO49 I2S playback out"]
        P50["GPIO50 Reset mềm"]
        PG["GND"]
        P54["GPIO54 EN của C5 (kéo thấp, giữ C5 im)"]
    end
    subgraph S3["M5StickS3 (Hat2 + Grove)"]
        G5["G5 (Hat2-2)"]
        G6["G6 (Hat2-6)"]
        G7["G7 (Hat2-8)"]
        G8["G8 (Hat2-9)"]
        G1["G1 (Hat2-7)"]
        G2["G2 (Hat2-14)"]
        G43["G43 U0TXD (Hat2-10)"]
        G44["G44 U0RXD (Hat2-12)"]
        G4["G4 (Hat2-4)"]
        G3["G3 (Hat2-16) strapping"]
        G9["G9 (Grove)"]
        SG["GND (Hat2-1)"]
    end
    P26 --> G5
    P27 --> G6
    G7 --> P28
    P29 --> G8
    G1 --> P30
    G2 --> P45
    G43 --> P46
    G44 --> P47
    G4 --> P48
    P49 --> G3
    P50 --> G9
    PG --- SG
```

Mũi tên chỉ hướng tín hiệu. S3 là master của I2S (cấp BCLK và WS), P4 là slave.

## Bảng nối dây

| Tín hiệu | P4C5 (JP1) | StickS3 | Chân Hat2 | Hướng | Ghi chú |
| --- | --- | --- | --- | --- | --- |
| SPI CLK | GPIO26 | G5 | 2 | P4 → S3 | 26 MHz |
| SPI MOSI | GPIO27 | G6 | 6 | P4 → S3 | |
| SPI MISO | GPIO28 | G7 | 8 | S3 → P4 | |
| SPI CS | GPIO29 | G8 | 9 | P4 → S3 | |
| Handshake | GPIO30 | G1 | 7 | S3 → P4 | |
| Data ready | GPIO45 | G2 | 14 | S3 → P4 | |
| Reset mềm | GPIO50 | G9 | Grove | P4 → S3 | S3 tự khởi động lại khi GPIO50 xuống thấp 2 ms trở lên (`EH_TRANSPORT_CP_SPI_GPIO_RESET`) |
| I2S BCLK | GPIO46 | G43 (U0TXD) | 10 | S3 → P4 | mất UART0 của S3 (console dùng USB-Serial/JTAG) |
| I2S WS | GPIO47 | G44 (U0RXD) | 12 | S3 → P4 | |
| I2S mic sạch | GPIO48 | G4 | 4 | S3 → P4 | L = mic sau AFE, R = bộ đếm |
| I2S playback | GPIO49 | G3 | 16 | P4 → S3 | G3 là chân strapping, chỉ dùng làm ngõ vào |
| GND | GND | GND | 1 | | |

## Chân không đụng trên StickS3

LCD G38 đến G41, G21 và G45; I2C của IMU và PMIC G47 và G48; ES8311 G14 đến G18; nút G11 và G12; IR G42 và G46; PMIC G13.

## Trên P4C5

- **C5 phải nằm im:** kéo GPIO54 (EN của C5) xuống thấp và không khởi tạo SDIO (GPIO14 đến 19).
- Bản đồ chân đầy đủ của P4C5 (LCD, camera, thẻ SD, codec, I2C): [../hw_specs/hw_matrix.md](../hw_specs/hw_matrix.md).
- Bản đồ chân StickS3 và schematic: [../hw_specs/StickS3_docs_pinmap.txt](../hw_specs/StickS3_docs_pinmap.txt), [../hw_specs/StickS3_schematic_V0.6.pdf](../hw_specs/StickS3_schematic_V0.6.pdf).

## Lưu ý khi nối

- Dây ngắn, dưới 10 cm; nên có GND xen kẽ với đường SPI. SPI ở cả hai phía đi qua GPIO matrix nên giới hạn xung: 26 MHz ổn định, 40 MHz không ổn định.
- **G3 là chân strapping** của S3. P4 nên để GPIO49 ở high-Z cho đến khi S3 chạy xong; khung playback chỉ hợp lệ khi có magic `0xA55A` ở kênh R (xem [I2S_LINK.md](I2S_LINK.md)).
- Reset P4 (kể cả do công cụ nạp hoặc monitor kéo DTR/RTS) cũng reset S3 qua GPIO50. Khi ghi log, mở cổng không kéo DTR/RTS (xem [../tools/README.md](../tools/README.md)).
- Kiểm tra mức 3,3 V của GPIO26 đến 30 và 45 đến 50 trên P4C5 (qua điện trở 100 Ω) và StickS3.
