# IMU Racer

**Game balap di browser yang dikemudikan dengan memiringkan papan rangkaian sungguhan.**

Sebuah ESP32-S3 membaca modul inersia GY-87, mengubah sudut kemiringan papan
menjadi nilai kemudi, lalu mengirimkannya lewat Bluetooth Low Energy. Game
three.js di browser menerimanya melalui Web Bluetooth API — tanpa driver, tanpa
aplikasi, tanpa instalasi. Miringkan papannya, mobilnya bergerak.

**▶ Mainkan: https://saufik.web.id/imu-racer/** — tanpa perangkat keras pun
bisa, kendali papan ketik dan sentuh berfungsi sendiri.

*[Baca dalam bahasa Inggris → `README.md`](README.md)*

---

## Daftar isi

- [Cara mainnya](#cara-mainnya)
- [Struktur repositori](#struktur-repositori)
- [Perangkat keras](#perangkat-keras)
- [Firmware](#firmware)
- [Gamenya](#gamenya)
- [Protokol BLE](#protokol-ble)
- [Deploy ke situs web](#deploy-ke-situs-web)
- [Catatan desain](#catatan-desain)
- [Pemecahan masalah](#pemecahan-masalah)
- [Kredit dan lisensi](#kredit-dan-lisensi)

---

## Cara mainnya

Hindari kendaraan lain di jalan tiga lajur. Jalan makin cepat selama Anda
bertahan, lalu lintas makin padat, dan permainan berakhir begitu Anda menabrak.
Skor berasal dari jarak tempuh — dibobot oleh kecepatan, jadi bertahan dalam
keadaan cepat bernilai lebih daripada bertahan pelan — ditambah bonus untuk tiap
mobil yang berhasil dilewati. Delapan permainan terbaik disimpan.

Tiga cara mengemudi, semuanya aktif bersamaan:

| Masukan | Cara |
| --- | --- |
| **Papan** | Miringkan ke kiri dan kanan. Posisi datar direkam sedetik setelah reset. |
| Papan ketik | `←` `→` atau `A` `D`; `Space` / `Enter` untuk mulai |
| Sentuh atau tetikus | Seret di area permainan |

Alternatif ini bukan sekadar pelengkap. Web Bluetooth tidak tersedia di Firefox
maupun peramban apa pun di iOS, sehingga halaman publik harus tetap bisa
dimainkan tanpanya.

## Struktur repositori

```
firmware/   Proyek ESP-IDF v6.0.2 — sensor, OLED, peripheral BLE
web/        Game Vite + three.js — render, klien Web Bluetooth
assets/     Aset gambar asli (lembar mobil, tekstur jalan, ledakan)
docs/       Rincian protokol, perangkat keras, dan pemecahan masalah
```

## Perangkat keras

| Komponen | Catatan |
| --- | --- |
| Papan pengembangan ESP32-S3 | Varian apa pun; BLE 5 sudah terpasang |
| GY-87 | MPU6050 + magnetometer + BMP180 dalam satu papan |
| OLED SSD1306 | 128×64, I²C |

Dua jalur I²C terpisah, agar penyegaran layar yang lambat tidak pernah menunda
pembacaan sensor:

| Jalur | Sinyal | GPIO |
| --- | --- | --- |
| Sensor | SDA | 5 |
| Sensor | SCL | 4 |
| Layar | SDA | 7 |
| Layar | SCL | 6 |

Keduanya bekerja pada 3,3 V. Pull-up internal aktif tetapi lemah — bila
pemindaian jalur tidak menemukan apa pun, tambahkan resistor 4,7 kΩ ke 3V3 pada
SDA dan SCL.

Definisi pin ada di bagian atas
[`firmware/main/i2c_bus.h`](firmware/main/i2c_bus.h).

### Apa saja isi GY-87

Magnetometer dan barometer berada di balik jalur I²C *auxiliary* milik MPU6050.
Keduanya tidak terlihat sampai firmware membersihkan `USER_CTRL` **dan**
menyalakan `I2C_BYPASS_EN` pada `INT_PIN_CFG`. Melakukan yang kedua saja —
kesalahan yang umum terjadi — sudah cukup membuat keduanya tetap tersembunyi.

Varian papan berbeda-beda, jadi driver-nya memeriksa alih-alih berasumsi:

| Chip | Alamat | Catatan |
| --- | --- | --- |
| MPU6050 | `0x68` | akselerometer + giroskop |
| HMC5883L | `0x1E` | magnetometer, komponen asli |
| QMC5883L | `0x0D` | magnetometer, tiruan; urutan byte berbeda |
| BMP180 | `0x77` | barometer; ID chip `0x58` berarti BMP280 |

Chip yang tidak ada tidak membuat program gagal. Papan tanpa magnetometer tetap
melaporkan percepatan, dan kemudi memang hanya membutuhkan akselerometer.

## Firmware

Dibangun dan diuji dengan **ESP-IDF v6.0.2**, target **esp32s3**.

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p COM11 flash monitor
```

### Struktur

| Berkas | Peran |
| --- | --- |
| [`main.c`](firmware/main/main.c) | loop kendali, perhitungan kemiringan |
| [`i2c_bus.c`](firmware/main/i2c_bus.c) | penyiapan jalur, pemindai alamat, pembantu register |
| [`gy87.c`](firmware/main/gy87.c) | MPU6050, HMC5883L/QMC5883L, BMP180 |
| [`ssd1306.c`](firmware/main/ssd1306.c) | driver layar, framebuffer, fon 5×7 |
| [`ble_controller.c`](firmware/main/ble_controller.c) | peripheral NimBLE |

### Ritme loop

Dua laju dalam satu task:

- **50 Hz** — baca akselerometer, saring, kirim lewat BLE
- **~240 ms** — barometer, magnetometer, gambar ulang OLED, log serial, perawatan BLE

Pemisahan ini penting. Konversi tekanan BMP180 memblokir sekitar 35 ms, sehingga
menaruhnya di jalur cepat akan merusak ritme kendali. Kemudi pada 4 Hz tidak
bisa dimainkan; barometer pada 4 Hz sudah lebih dari cukup.

### Dari kemiringan ke kemudi

Perhitungan kemiringan akselerometer standar, memakai besaran dua sumbu lain
pada penyebut agar kedua sudut tetap stabil saat papan mendekati posisi tegak:

```c
roll  = atan2f(ax, sqrtf(ay*ay + az*az)) * 57.29578f;
pitch = atan2f(ay, sqrtf(ax*ax + az*az)) * 57.29578f;
```

Tapis lolos-rendah (`TILT_ALPHA`) meredam getaran tangan. Posisi datar direkam
sekitar sedetik setelah reset, setelah tapisnya tenang, sehingga papan yang
masih sedang diletakkan tidak menjadi acuan. `STEER_RANGE_DEG` menentukan
seberapa jauh papan harus dimiringkan untuk belok penuh — kecilkan bila ingin
kemudi lebih peka.

Zona mati diskalakan ulang, bukan dipotong, sehingga tidak ada lonjakan di
tepinya.

### Layar di papan

OLED menampilkan status BLE, nilai kemudi, bilah kemudi langsung, sudut
kemiringan, dan bacaan barometer. Artinya Anda bisa memastikan pengendali
berfungsi sebelum membuka peramban sama sekali — sangat membantu saat menelusuri
masalah sambungan nirkabel.

## Gamenya

```bash
cd web
npm install
npm run dev      # http://localhost:5180
npm run build    # dist/ yang mandiri
```

### Aset

Ketiga aset diolah saat dimuat, bukan disiapkan sebelumnya:

- **Lembar mobil** — satu SVG berisi empat mobil. Dirasterkan, lalu dipotong
  pada kolom yang sepenuhnya transparan dan dipangkas ke kotak batas tiap mobil.
  Lebih andal daripada mengasumsikan kisi seragam, karena jarak antarmobil tidak
  merata. Bila bentuk lembarnya berubah, sistem beralih ke pembagian empat sama
  besar.
- **Tekstur jalan** — aslinya mendatar (garis lajur kiri-ke-kanan, garis kuning
  di atas dan bawah). Diputar 90° sekali ke dalam kanvas saat dimuat, alih-alih
  melawan rotasi UV di setiap frame.
- **Ledakan** — strip 96×32 dipotong menjadi tiga frame 32×32.

Pemain mengendarai mobil merah; lalu lintas berwarna hijau, kuning, dan biru.

### Render

Kamera ortografis dari atas dengan bidang pandang vertikal tetap 16 satuan. Area
permainan berupa panel potret 9:16 sehingga geometri lajur terbaca sama di
ponsel maupun di monitor lebar. Ukuran kanvas dikendalikan `ResizeObserver` —
pendengar `resize` pada `window` akan melewatkan kasus ketika elemennya masih
berukuran nol saat game dibuat, dan setelah itu tidak pernah terpicu untuk
memperbaikinya.

## Protokol BLE

Rincian lengkap di [`docs/PROTOCOL.id.md`](docs/PROTOCOL.id.md). Ringkasnya:

| Peran | UUID |
| --- | --- |
| Layanan | `4f1a0000-8b2c-4f5e-9d3a-1c7e6b9f0a01` |
| Telemetri (notify) | `4f1a0001-8b2c-4f5e-9d3a-1c7e6b9f0a01` |
| Perintah (write) | `4f1a0002-8b2c-4f5e-9d3a-1c7e6b9f0a01` |

8 byte, little-endian, pada 50 Hz: `int16 steer`, `int16 pitch`, `uint8 buttons`,
`uint8 flags`, `uint16 seq`. Menulis `0x01` ke karakteristik perintah akan
merekam ulang acuan posisi datar.

## Deploy ke situs web

`npm run build` menghasilkan `dist/` yang mandiri. `base: './'` sudah disetel,
jadi berfungsi dari subfolder mana pun.

Tiga hal yang mudah terlewat:

**Sajikan lewat HTTPS.** Web Bluetooth hanya ada pada konteks aman. Pada http
biasa, `navigator.bluetooth` tidak terdefinisi, tombol sambung menonaktifkan
diri dan menjelaskan alasannya, dan game tetap bisa dimainkan dengan papan ketik
serta sentuhan. `localhost` terhitung aman, itulah sebabnya server pengembangan
tetap berfungsi.

**Tautkan dengan garis miring di akhir** — `/games/imu-racer/`, bukan
`/games/imu-racer`. URL aset relatif diselesaikan terhadap direktorinya.

**Penyematan butuh izin eksplisit.** Di dalam iframe, halaman induk harus
memberikannya, atau proses sambung akan gagal dengan galat kebijakan:

```html
<iframe src="/games/imu-racer/" allow="bluetooth" width="405" height="720"></iframe>
```

### Salinan yang tayang

Game yang tayang adalah salinan `web/dist/` yang diletakkan di folder
`imu-racer/` pada repositori
[`personal-web`](https://github.com/saufik-ramadhan/personal-web), yang disajikan
GitHub Pages lewat domain kustom. Untuk memperbaruinya setelah ada perubahan:

```bash
cd web && npm run build
# lalu ganti isi personal-web/imu-racer/ dengan isi web/dist/
```

Ini salinan, bukan submodule, jadi tidak memperbarui diri sendiri — build dan
salin ulang, atau versi yang tayang diam-diam menjadi usang.

### Dukungan peramban

Chrome, Edge, dan Opera di desktop serta Android. Tidak di Firefox, tidak di
Safari, tidak di peramban mana pun pada iOS. Pengunjung tersebut memakai kendali
sentuh.

## Catatan desain

### Bug lalu lintas yang layak diketahui

Versi pertama memberi tiap mobil kecepatannya sendiri, dan itu terlihat lebih
alami. Ternyata tidak: mobil lambat dari satu gelombang dapat berjalan
berdampingan dengan mobil cepat dari gelombang berikutnya, sehingga ketiganya
menutup semua lajur. Diukur selama dua menit simulasi, **9,5% frame tidak
menyisakan satu lajur pun yang kosong** — kekalahan yang bukan kesalahan pemain.

Kini seluruh lalu lintas memakai satu laju pendekatan yang sama, sehingga jarak
vertikal antargelombang terjaga persis. Sebuah gelombang juga menolak muncul
dalam jarak `MIN_WAVE_GAP` dari gelombang sebelumnya, dan lajur kosong hanya
bergeser satu langkah, sehingga gelombang berurutan selalu terjangkau. Setelah
diukur ulang: nol frame dengan ketiga lajur tertutup, selalu ada minimal satu
lajur kosong.

Pelajarannya berlaku umum. "Terlihat lebih bervariasi" dan "tetap adil" adalah
dua sifat yang berbeda, dan hanya satu di antaranya yang tampak pada tangkapan
layar.

### Tingkat kesulitan

Kecepatan naik dari 1,0× hingga batas 3,6× dalam kurang lebih 90 detik. Jeda
kemunculan berbanding terbalik dengan kecepatan, dan gelombang berisi dua mobil
berubah dari jarang menjadi lazim seiring berjalannya permainan. Bot sederhana
yang selalu mengarah ke lajur terkosong bertahan 23–36 detik dan melewati 23–47
mobil, menyisakan ruang yang jelas bagi manusia yang bisa mengantisipasi.

## Pemecahan masalah

Versi lebih lengkap di [`docs/TROUBLESHOOTING.id.md`](docs/TROUBLESHOOTING.id.md).

**Jangan memasangkan pengendali dari panel Bluetooth sistem operasi.** Web
Bluetooth tidak memakai pemasangan OS — peramban membuka sambungan GATT-nya
sendiri. Windows akan menyambung, tidak menemukan profil yang dikenalinya, lalu
memutus. Lebih repot lagi, peripheral BLE berhenti beriklan selama tersambung,
sehingga daftar pilihan di peramban menjadi kosong selama OS memegang
sambungan. Biarkan Bluetooth menyala dan sambungkan dari halaman game.

**Tidak ada apa pun di sebuah jalur.** Pemindaian saat boot mencetak setiap
alamat yang menjawab. Jalur kosong berarti masalah kabel atau pull-up.

**Alasan pemutusan diterjemahkan di log:**

```
I (12345) ble_ctrl: disconnected, reason 0x213 -- remote terminated the connection
```

`0x208` berarti supervision timeout, `0x213` berarti pihak sentral memutus
sambungan, `0x206` berarti ada kunci pemasangan usang yang masih tersimpan.

## Kredit dan lisensi

Firmware ini berawal dari contoh `blink` milik ESP-IDF (Apache-2.0) dan telah
ditulis ulang; kode LED-nya sudah dihapus sepenuhnya.

Seluruh karya seni berasal dari [OpenGameArt.org](https://opengameart.org):

| Aset | Judul | Penulis | Lisensi |
| --- | --- | --- | --- |
| Mobil | [Car — Racer](https://opengameart.org/content/car-racer) | Bahi | CC-BY 3.0 |
| Jalan | [Toon Road Texture](https://opengameart.org/content/toon-road-texture) | [da_st](https://www.davidstenfors.com) | CC-BY 3.0 |
| Ledakan | [Simple Explosion](https://opengameart.org/content/simple-explosion) | NiceGraphic | CC0 |

Dua dari tiga aset berlisensi **CC-BY 3.0**, sehingga atribusinya harus ikut ke
mana pun karya itu didistribusikan ulang — termasuk pada game hasil build, bukan
hanya repositori ini. Karena itu kreditnya juga tampil di layar judul game.
Rincian lengkap, termasuk perubahan yang dilakukan pada tiap aset, ada di
[`docs/CREDITS.id.md`](docs/CREDITS.id.md).

Kode proyek berlisensi MIT — lihat [`LICENSE`](LICENSE). Karya seninya tidak
tercakup lisensi itu; keduanya tetap tunduk pada lisensi di atas.
