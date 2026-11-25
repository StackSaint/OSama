![Credit: 朧月](osama.jpg)

# OSama

## Daftar Isi
- [OSama](#osama)
  - [Daftar Isi](#daftar-isi)
  - [Nama Kelompok \& Anggota](#nama-kelompok--anggota)
  - [Build dan Menjalankan OS](#build-dan-menjalankan-os)
  - [Fitur](#fitur)
  - [Perintah Shell](#perintah-shell)

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
- PIT channel 0 pada 1 kHz untuk IRQ0 serta jalur penyimpanan konteks dan
  context switch preemptive saat proses Ring 3 aktif.
- Driver keyboard berbasis interrupt untuk input karakter.
- GDT user code/data descriptor, TSS, dan transisi kernel ke Ring 3.
- Flat binary user program serta host-side inserter untuk EXT2.
- Output teks melalui VGA text framebuffer 80×25, termasuk cursor dan
  scrolling.
- Driver disk ATA PIO untuk membaca dan menulis block.
- Filesystem EXT2 pada disk 4 MB. Filesystem dibuat saat boot jika disk masih
  kosong; tersedia operasi untuk membuat, membaca, dan menghapus file/direktori,
  serta membaca isi direktori.
- Shell user-mode Ring 3 dengan input keyboard, process management, dan
  perintah dasar filesystem.

## Perintah Shell

Setelah OS boot, perintah berikut tersedia:

```text
help                     show help
clear                    clear the screen
ls [path]                list a directory
cd [path]                change directory (default: /)
mkdir path               create a directory
rmdir path               remove an empty directory
cat file                 print a file (up to 8192 bytes)
cp source destination    copy a file (overwrites existing destination)
mv source destination    move/rename a file or folder
rm file                  remove a file
find [path] [pattern]    search directory tree
grep pattern [file]      print matching lines (no file: piped input)
ps                       list processes
kill pid|name            terminate a process
exec file [args...]      run a flat binary with argc/argv
time                     show current date and time
uptime                   show uptime in ms
shutdown                 power off system (ACPI 0x604)
exit                     exit this process
export [add|remove] dir  manage PATH directories
name [args...]           run a program found in PATH
cmd1 | cmd2              pipe output, e.g. ls | grep sh
file.sh                  run a shell script (up to 3 levels of nesting)
```

**User programs** (located in `/bin`): `echo`, `hello`, `clock`.

`find` mencocokkan pola sebagai substring pada nama/path dan membatasi
penelusuran hingga 8 tingkat direktori. `cat`, `cp`, dan `grep` mendukung
file sampai 8192 byte; `cp` menimpa file tujuan yang sudah ada. `mv` mendukung
file maupun folder. `rmdir` hanya berhasil untuk direktori kosong.
Pembacaan/listing direktori dibatasi hingga 2048 byte. Path dibatasi hingga 127
karakter.

**Keyboard shortcuts:** Up/Down (command history), PageUp/PageDown (scroll),
Tab (autocomplete commands and paths), Ctrl+C (cancel current line).


```bash
make clean
make DISK_TEST=1 all
make run
```

Test ini menulis satu block (512 byte) ke LBA 17, membacanya kembali, lalu
membandingkan seluruh isinya. LBA 17 berada pada byte offset `0x2200` di
`bin/storage.bin`. Jangan gunakan `DISK_TEST=1` setelah filesystem mulai
menyimpan data karena test tersebut sengaja menimpa block LBA 17.
