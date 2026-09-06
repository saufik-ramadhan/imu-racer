# Protokol BLE

Kesepakatan antara firmware dan peramban. Didefinisikan tepat di dua tempat, dan
keduanya harus diubah bersama-sama:

- `firmware/esp32s3/main/ble_controller.c` — sisi peripheral
- `web/src/ble.js` — klien Web Bluetooth

## Susunan GATT

| Peran | UUID | Properti |
| --- | --- | --- |
| Layanan | `4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01` | primary |
| Telemetri | `4f1a0001-8b2c-4f5e-9d3a-1c7e6b9f0a01` | read, notify |
| Perintah | `4f1a0002-8b2c-4f5e-9d3a-1c7e6b9f0a01` | write, write-without-response |

NimBLE menyimpan UUID 128-bit dengan byte paling tidak signifikan lebih dulu,
jadi larik di firmware adalah bentuk tertulisnya yang dibalik:

```c
/* 4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01 */
BLE_UUID128_INIT(0x01, 0x0a, 0x9f, 0x6b, 0x7e, 0x1c, 0x3a, 0x9d,
                 0x5e, 0x4f, 0x2c, 0x8b, 0x00, 0x00, 0x1a, 0x4f)
```

Salah urutan di sini adalah kegagalan yang senyap — peripheral tetap beriklan
dengan tenang dan peramban sekadar tidak pernah menemukannya. Cara cepat
memeriksanya: balik lariknya lalu susun sebagai teks UUID; hasilnya harus sama
dengan nilai di `ble.js`.

## Paket telemetri

8 byte, little-endian, dikirim pada 50 Hz selama ada sentral yang berlangganan.

| Offset | Tipe | Medan | Rentang | Arti |
| --- | --- | --- | --- | --- |
| 0 | `int16` | `steer` | -1000..1000 | sudut guling, dinormalkan ke belok penuh |
| 2 | `int16` | `pitch` | -1000..1000 | sudut angguk, cadangan untuk gas |
| 4 | `uint8` | `buttons` | bitfield | bit0 = tombol pengguna (belum dipakai) |
| 5 | `uint8` | `flags` | bitfield | bit0 = acuan datar sudah terekam |
| 6 | `uint16` | `seq` | berputar | naik tiap paket |

`seq` ada agar klien dapat mendeteksi notifikasi yang hilang:

```js
const gap = (seq - this.lastSeq) & 0xffff;
if (gap > 1) this.dropped += gap - 1;
```

## Perintah

Satu byte yang ditulis ke karakteristik perintah.

| Nilai | Nama | Efek |
| --- | --- | --- |
| `0x01` | `CMD_ZERO` | Merekam ulang sikap papan saat ini sebagai posisi datar |

Firmware memperlakukannya sebagai kait: `ble_controller_take_zero_request()`
mengembalikan `true` sekali lalu menghapus penandanya, sehingga loop kendali
memeriksanya sendiri alih-alih menjalankan pekerjaan di dalam callback BLE.

## Iklan (advertising)

UUID layanan 128-bit memakan 18 dari 31 byte yang tersedia pada muatan iklan
legacy. Dengan struktur flag sebesar 3 byte, tersisa 10 byte — tidak cukup untuk
namanya. Karena itu:

- **Iklan** — flag + daftar lengkap UUID layanan 128-bit
- **Scan response** — nama lokal lengkap + daya pancar

Keduanya terlihat oleh peramban yang melakukan pemindaian aktif, dan Chrome
memang melakukannya.

Klien web menyaring salah satu dari keduanya, karena isi larik `filters`
digabung dengan ATAU:

```js
filters: [
  { services: [SERVICE_UUID] },
  { namePrefix: 'IMU' },
],
optionalServices: [SERVICE_UUID],
```

`optionalServices` adalah yang sesungguhnya memberi akses ke layanan setelah
tersambung, jadi GATT tetap berfungsi apa pun penyaring yang cocok.

## Parameter sambungan

Setelah sentral berlangganan, peripheral meminta interval 15–30 ms dengan
supervision timeout 4 detik. Sentral berhak menolak; firmware mencatatnya lalu
melanjutkan pengiriman pada laju yang diberikan.

Permintaan ini sengaja dilakukan pada peristiwa *subscribe*, bukan saat
tersambung. Meminta parameter baru tepat ketika sambungan terbentuk mengganggu
sebagian sentral, dan sebelum ada yang berlangganan pun tidak ada yang perlu
direspons cepat.
