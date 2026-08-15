# Pemecahan masalah

## Pengendali tersambung, lalu langsung terputus

Hampir pasti Anda memasangkannya dari panel Bluetooth sistem operasi. Jangan.

Web Bluetooth sama sekali tidak memakai pemasangan OS — peramban membuka
sambungan GATT-nya sendiri. "Add device" di Windows akan menyambung, mencari
profil yang dikenalinya, hanya menemukan layanan khusus yang tak berarti
baginya, lalu memutus.

Biarkan adaptor Bluetooth **menyala**, tetapi jangan pasangkan perangkatnya.
Sambungkan dari tombol di halaman game. Bila pengendali sudah terdaftar di
Settings → Bluetooth & devices, hapus — Windows menyambung ulang secara diam-diam
ke perangkat yang diingatnya dan akan terus merebut sambungan.

## Daftar perangkat di peramban kosong

Peripheral BLE hanya melayani satu sentral pada satu waktu dan **berhenti
beriklan selama tersambung**. Daftar pilihan hanya menampilkan perangkat yang
sedang beriklan, jadi apa pun yang memegang sambungan membuatnya tampak kosong.

Periksa berurutan:

1. Apakah OS tersambung ke sana? Hapus pemasangannya.
2. Apakah ada tab peramban lain yang tersambung? Tutup.
3. Apakah `chrome://bluetooth-internals` (atau `brave://…`) tersambung? Halaman
   itu punya tombol **Disconnect** — sangat mudah tertinggal memegang sambungan.
4. Apakah papannya beriklan sama sekali? OLED seharusnya menampilkan
   `BLE: advertising`, dan log menunjukkan `advertising as "IMU Racer"`.

`chrome://bluetooth-internals` → Devices → Start Scan memberi tahu apakah
papannya terlihat oleh peramban, terlepas dari halaman game.

## Muncul di bluetooth-internals tetapi tidak di daftar pilihan

Artinya iklan tidak membawa hal yang dicocokkan oleh penyaring Anda. Klien
menyaring berdasarkan UUID layanan **atau** awalan nama, jadi setidaknya satu
harus ada di iklan atau scan response.

Firmware mencatat byte iklan yang dikirimnya saat mulai:

```
I (1234) ble_ctrl: adv payload 21/31 bytes: 02 01 06 11 07 01 0a 9f ...
```

Cara membacanya: `02 01 06` adalah struktur flag, lalu `11 07` membuka "daftar
lengkap UUID layanan 128-bit" sepanjang 17 byte, diikuti UUID dalam urutan byte
terbalik. Bila struktur kedua itu tidak ada, penyaring layanan tidak akan cocok
dan penyaring namalah yang menyelamatkan.

## Perangkat diam setelah terputus dan hanya pulih setelah reset

Sudah diperbaiki, tetapi perlu dipahami. `ble_gap_adv_start()` sering
mengembalikan `BLE_HS_EBUSY` bila dipanggil langsung dari peristiwa disconnect,
karena pengendali masih membereskan sambungan lama. Kode awal mencatat kegagalan
itu lalu menyerah, sehingga tidak ada yang pernah memulai iklan kembali.

`ble_controller_maintain()` kini berjalan pada tik lambat dan menyalakan ulang
iklan setiap kali tidak ada yang tersambung dan tidak ada yang beriklan,
sehingga kegagalan sesaat pulih dalam sekitar seperempat detik.

## Kode alasan pemutusan

NimBLE melaporkan alasan HCI sebagai `0x200 | kode`. Firmware menerjemahkan yang
umum:

| Alasan | Arti |
| --- | --- |
| `0x208` | supervision timeout — di luar jangkauan, atau sentral berhenti menjawab |
| `0x213` | sentral memutus sambungan |
| `0x216` | diputus dari sisi perangkat |
| `0x205` | kegagalan autentikasi — sentral mengharapkan pemasangan |
| `0x206` | PIN atau kunci hilang — pemasangan usang tersimpan di sentral |
| `0x23e` | gagal membentuk sambungan |

## Tidak ada perangkat I²C yang ditemukan

Pemindaian saat boot mencetak setiap alamat yang menjawab di kedua jalur:

```
I (330) i2c_bus: Scanning sensor bus ...
I (340) i2c_bus:   found device at 0x68
```

Jalur kosong berarti masalah kabel, catu daya, atau pull-up. Pull-up internal
aktif tetapi lemah; tambahkan 4,7 kΩ ke 3V3 pada SDA dan SCL. Pastikan modul
diberi 3,3 V, dan SDA serta SCL tidak tertukar.

## Magnetometer atau barometer tidak terdeteksi

Bila `0x68` menjawab tetapi `0x1E`, `0x0D`, dan `0x77` tidak, bypass MPU6050
belum terbuka. Firmware membersihkan `USER_CTRL` lalu menyalakan
`I2C_BYPASS_EN`, dalam urutan itu, dan itulah yang membuat perangkat auxiliary
terlihat.

Bila barometer menjawab tetapi melaporkan ID chip `0x58`, itu BMP280, bukan
BMP180 — peta registernya berbeda sama sekali. Driver mendeteksi hal ini dan
memberi peringatan alih-alih menghasilkan angka ngawur.

Banyak papan yang dijual sebagai GY-87 memang tidak memasang magnetometernya.
Kemudi hanya memakai akselerometer, jadi hal ini tidak memengaruhi permainan.

## Tombol sambung nonaktif

Halaman menjelaskan alasannya pada baris status di bawahnya:

- *Needs HTTPS (or localhost)* — Web Bluetooth hanya ada pada konteks aman
- *Needs Chrome, Edge or Opera* — Firefox dan semua peramban iOS tidak punya API-nya

Di dalam iframe, halaman induk harus menyertakan `allow="bluetooth"`.

## Game tampil tetapi ukuran kanvasnya salah

Seharusnya tidak terjadi — ukuran kanvas dikendalikan `ResizeObserver` pada area
permainan, yang terpicu saat pengamatan dimulai maupun saat ukuran berubah. Bila
Anda melihat kanvas 300×150, callback pengamat tidak terkirim, dan itu terjadi
pada halaman yang tidak pernah dirender (tab tersembunyi, atau peramban otomatis
tanpa tampilan).
