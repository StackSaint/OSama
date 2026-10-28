![Credit: 朧月](mascot.jpg)

# Kelompok OSama

## Daftar Isi
- [Kelompok OSama](#kelompok-osama)
  - [Daftar Isi](#daftar-isi)
  - [Nama Kelompok \& Anggota](#nama-kelompok--anggota)
  - [Build dan Menjalankan OS](#build-dan-menjalankan-os)
  - [Fitur](#fitur)

---

## Nama Kelompok & Anggota

**Nama Kelompok:** OSama

| Nama | NIM |
| :--- | :--- |
| Fathar Atandra Denaya | 13525067 |
| Rivan Cahyadi | 13525007 |
| Fabian Amzar Susanto | 13525010 |
| Sebastio Nugroho | 13525123 |

---

## Build dan Menjalankan OS

Pastikan `make`, NASM, GCC dengan dukungan 32-bit (`-m32`), GNU `ld`,
`genisoimage`, dan QEMU tersedia di environment yang digunakan.

Untuk compile kernel dan membuat ISO:

```bash
make build
```

Untuk membuat flat binary user program dan memasukkannya ke root filesystem:

```bash
make insert-shell
```

Target tersebut membuat `bin/shell`, membangun host-side EXT2 inserter, dan
menulis program bernama `shell` ke `bin/storage.bin`. Kernel kemudian memuat
program itu ke user space, memasang TSS, dan berpindah ke Ring 3 melalui
`iret`.

Cara yang disarankan untuk menjalankan OS:

```bash
make run
```

Target `make run` akan membangun ISO jika diperlukan, membuat
`bin/storage.bin` berukuran 4 MB jika belum ada, lalu menjalankan QEMU dengan
ISO dan disk tersebut. Disk diperlukan oleh driver ATA dan filesystem EXT2.
Image yang sudah ada akan tetap digunakan agar data filesystem tidak hilang.

Jika ingin menjalankan QEMU secara manual setelah `make build`, sertakan disk
seperti berikut:

```bash
qemu-system-i386 \
  -cdrom bin/OSama.iso \
  -drive file=bin/storage.bin,format=raw,if=ide,index=0,media=disk
```

Perintah `qemu-system-i386 -cdrom bin/OSama.iso` saja tidak menyertakan disk
penyimpanan yang dibutuhkan OS. Untuk membuat image disk baru secara sengaja,
jalankan `make disk`.

## Fitur

- Inisialisasi kernel 32-bit dengan GDT, IDT, dan remapping PIC.
- Driver keyboard berbasis interrupt untuk input karakter.
- GDT user code/data descriptor, TSS, dan transisi kernel ke Ring 3.
- Flat binary user program serta host-side inserter untuk EXT2.
- Output teks melalui VGA text framebuffer 80×25, termasuk cursor dan
  scrolling.
- Driver disk ATA PIO untuk membaca dan menulis block.
- Filesystem EXT2 pada disk 4 MB. Filesystem dibuat saat boot jika disk masih
  kosong; tersedia operasi untuk membuat, membaca, dan menghapus file/direktori,
  serta membaca isi direktori.
- Input keyboard saat ini ditampilkan sebagai teks pada layar. Belum ada shell
  untuk menjalankan perintah.


```bash
make clean
make DISK_TEST=1 all
make run
```

Test ini menulis satu block (512 byte) ke LBA 17, membacanya kembali, lalu
membandingkan seluruh isinya. LBA 17 berada pada byte offset `0x2200` di
`bin/storage.bin`. Jangan gunakan `DISK_TEST=1` setelah filesystem mulai
menyimpan data karena test tersebut sengaja menimpa block LBA 17.
