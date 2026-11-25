![Credit: 朧月](osama.jpg)

# OSama

Sistem operasi x86 32-bit (protected mode) untuk Tugas Besar IF2130 Sistem
Operasi, dijalankan di QEMU.

## Daftar Isi
- [OSama](#osama)
  - [Daftar Isi](#daftar-isi)
  - [Nama Kelompok \& Anggota](#nama-kelompok--anggota)
  - [Build dan Menjalankan OS](#build-dan-menjalankan-os)
  - [Fitur Wajib](#fitur-wajib)
  - [Fitur Bonus](#fitur-bonus)
  - [Perintah Shell](#perintah-shell)
  - [Program User](#program-user)
  - [Keyboard dan Mouse](#keyboard-dan-mouse)
  - [Tools](#tools)
  - [Batasan](#batasan)
  - [Self-Test](#self-test)

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

Kebutuhan: `make`, NASM, GCC dengan dukungan 32-bit (`-m32`), GNU `ld`,
`genisoimage`, `qemu-img`, dan `qemu-system-i386` (dengan tampilan GTK).
Suara PC speaker dikirim ke PulseAudio, yang di WSL disediakan oleh WSLg.

Cara menjalankan OS:

```bash
make run
```

`make run` akan:
1. Compile kernel dan membuat `bin/OSama.iso`.
2. Membuat `bin/storage.bin` (disk 4 MB) jika belum ada. Disk yang sudah ada
   tetap dipakai, jadi file yang dibuat di dalam OS tidak hilang.
3. Compile program user (`shell`, `clock`, `echo`, `hello`, `guess`,
   `badapple`) dan memasukkannya ke root filesystem EXT2 dengan host-side
   inserter, beserta data video `other/badapple.dat` sebagai `/badapple.dat`.
4. Menjalankan QEMU dengan suara PC speaker dan kursor mouse host yang tetap
   terlihat.

Target lain:

| Perintah | Kegunaan |
| :--- | :--- |
| `make build` | Compile kernel dan membuat ISO saja |
| `make disk` | Membuat disk 4 MB baru (**menghapus semua file di dalam OS**) |
| `make clean` | Menghapus object file, kernel, dan ISO |
| `make insert-<program>` | Compile dan memasukkan satu program, misalnya `make insert-shell` |

Semua isi `bin/` adalah hasil build dan tidak disimpan di Git.

Menjalankan QEMU secara manual setelah build (disk wajib disertakan karena
dibutuhkan driver ATA dan filesystem):

```bash
qemu-system-i386 \
  -rtc base=utc \
  -display gtk,show-cursor=on \
  -audiodev pa,id=spk -machine pcspk-audiodev=spk \
  -cdrom bin/OSama.iso \
  -drive file=bin/storage.bin,format=raw,if=ide,index=0,media=disk
```

Jika environment tidak punya PulseAudio, hapus baris `-audiodev ...`.

---

## Fitur Wajib

**Chapter 0: Toolchain, Kernel, GDT**
- Kernel 32-bit di-boot GRUB dan berjalan di higher half (`0xC0000000`).
- GDT dengan segmen kernel/user code dan data, serta TSS.

**Chapter 1: Interrupt, Driver**
- IDT dan remapping PIC, CPU exception, IRQ, serta syscall gate `int 0x30`.
- VGA text framebuffer 80×25 berwarna dengan hardware cursor.
- Driver keyboard berbasis interrupt: huruf kapital, simbol, Shift, Caps Lock,
  Ctrl, Ctrl+C, panah, Home/End/Delete, PageUp/PageDown, dan numpad.
- PIT 1 kHz untuk preemptive scheduling dan uptime.

**Chapter 2: File System**
- Driver disk ATA PIO.
- Filesystem EXT2 (versi IF2130) pada disk 4 MB: superblock, block group,
  inode, direktori, dan indirect block. Filesystem dibuat otomatis saat boot
  jika disk masih kosong.
- Operasi CRUD: `read`, `read_directory`, `write` (first fit), `delete`, dan
  `move`.

**Chapter 3: Paging, User Mode, Shell**
- Paging 4 KB dengan page frame allocator, page directory per process, swap ke
  disk, dan page fault handler.
- User mode (Ring 3) dengan system call.
- Shell interaktif: current directory di prompt, pipe `|`, output buffering,
  dan perintah filesystem (lihat [Perintah Shell](#perintah-shell)).

**Chapter 4: Process, Scheduler, Multitasking**
- Process Control Block (maksimal 16 process, 256 KB per process).
- Scheduler round robin preemptive dengan context switch assembly.
- `exec`, `ps`, `kill`, dan syscall `exit`. Program otomatis keluar saat
  `main()` selesai (lewat crt0).
- Driver CMOS dan aplikasi jam (`clock`) di background.
- Environment variable PATH yang tersimpan di `/path.env`.

---

## Fitur Bonus

| Bonus | Keterangan |
| :--- | :--- |
| File System Indexing | Hash table berisi semua path, jadi `find` tidak perlu menelusuri disk |
| HEAP Memory | Heap per process (first fit, split/merge block) dengan syscall `malloc`/`free` |
| Recent commands | ↑ / ↓ untuk 16 perintah terakhir |
| Scrollable shell | 200 baris scrollback (PageUp/PageDown atau scroll wheel) |
| Tab autocomplete | Melengkapi nama perintah dan path file/folder |
| Shell script | Menjalankan file `.sh` baris per baris (bersarang maksimal 3 level) |
| Basic C runtime | `int main(int argc, char *argv[])` lewat crt0 |
| PATH dynamic array | PATH berupa array di heap yang membesar/mengecil otomatis; `export dump` menampilkan isinya |
| Aplikasi lain (blocking) | `guess`: game tebak angka; shell menunggu sampai game selesai |
| Bad Apple!! (text-based) | `badapple` memutar video dari `/badapple.dat` pada 30 FPS, bisa dihentikan dengan Ctrl+C |
| Driver PC Speaker | Lagu saat boot dan bunyi beep saat perintah error |
| Driver Mouse | PS/2 mouse (IRQ12) dengan vmmouse untuk posisi absolut, pointer di layar, scroll wheel, dan klik |
| Kreativitas | Splash screen, tampilan pembuka ala neofetch dengan logo OSama, warna teks, `shutdown` (ACPI), `time`, `uptime`, edit baris dengan ← →, `kill` berdasarkan nama |

---

## Perintah Shell

```text
help                       show help
clear                      clear the screen
ls [path]                  list a directory
cd [path]                  change directory (default: /)
mkdir path                 create a directory
rmdir path                 remove an empty directory
cat file                   print a file (up to 8192 bytes)
cp source destination      copy a file (overwrites existing destination)
mv source destination      move/rename a file or folder
rm file                    remove a file
find [path] [pattern]      search files by name (uses the index)
grep pattern [file]        print matching lines (no file: piped input)
ps                         list processes
kill pid|name              terminate a process (PID 0, the shell, is protected)
exec file [args...]        run a flat binary with argc/argv
name [args...]             run a program from the current folder or PATH
file.sh                    run a shell script (up to 3 levels of nesting)
cmd1 | cmd2                pipe output, e.g. ls | grep sh
export                     show PATH
export add dir             add a directory to PATH
export remove dir          remove a directory from PATH (at least one stays)
export dump                show the PATH dynamic array on the heap
fetch / neofetch           show the OSama logo and system info
mouse                      show mouse position and buttons
time                       show current date and time (WIB)
uptime                     show uptime in ms
shutdown                   power off the system (ACPI)
exit                       exit this process
```

Contoh PATH: `mkdir apps`, `cp hello apps/hi`, `export add /apps`, lalu `hi`.

---

## Program User

Semua program ada di root folder `/`.

| Program | Keterangan |
| :--- | :--- |
| `hello` | Mencetak "Hello World!" |
| `echo [args...]` | Mencetak `argc` dan setiap `argv` |
| `clock` | Jam (WIB) di pojok kanan bawah; berjalan di background, hentikan dengan `kill clock` |
| `guess` | Game tebak angka 1–100 dengan 7 kesempatan |
| `badapple [file]` | Memutar Bad Apple!! dari `/badapple.dat`; hentikan dengan Ctrl+C atau `q` |

Semua program kecuali `clock` berjalan di foreground: shell menunggu sampai
program selesai, dan Ctrl+C menghentikan program tersebut.

---

## Keyboard dan Mouse

| Tombol | Fungsi |
| :--- | :--- |
| ↑ / ↓ | Riwayat perintah |
| ← / →, Home / End | Menggeser kursor di baris input |
| Backspace / Delete | Menghapus huruf sebelum / di posisi kursor |
| Tab | Autocomplete perintah dan path |
| PageUp / PageDown | Scroll output |
| Ctrl+C | Membatalkan baris input atau menghentikan program yang berjalan |

Mouse:
- Pointer (sel berwarna terbalik) mengikuti kursor host lewat vmmouse; tanpa
  vmmouse, driver memakai paket PS/2 relatif (klik jendela QEMU untuk grab,
  Ctrl+Alt+G untuk melepas).
- Scroll wheel menggulir output shell.
- Klik kiri pada baris input memindahkan kursor ke posisi yang diklik.

---

## Tools

| Tool | Kegunaan |
| :--- | :--- |
| `tools/external-inserter.c` | Memasukkan file dari host ke disk EXT2 (`bin/inserter`, dibuat otomatis oleh `make`) |
| `tools/badapple-convert.py` | Mengubah video menjadi `badapple.dat` (butuh ffmpeg) |
| `tools/logo-convert.py` | Mengubah gambar menjadi logo `user/osama-logo.h` (butuh ffmpeg) |

Contoh membuat ulang data video dan logo:

```bash
python3 tools/badapple-convert.py badapple.mp4 other/badapple.dat
python3 tools/logo-convert.py osama.jpg user/osama-logo.h --width 26 --height 28 --threshold 110
```

---

## Batasan

- `cat`, `cp`, `grep`, dan shell script menangani file sampai 8192 byte.
- Output pipe, `find`, dan `ps` dibatasi 512 byte; muncul peringatan kuning
  jika terpotong.
- `find` selalu mencari di seluruh disk dan mengindeks hingga 8 tingkat folder.
- Output program eksternal tidak bisa dialirkan lewat pipe.
- `rmdir` hanya untuk folder kosong; `cp` dan `rm` tidak bisa untuk folder.
- Belum ada perintah untuk membuat file teks; gunakan `cp` atau inserter dari
  host.
- Path maksimal 127 karakter, listing direktori maksimal 2048 byte.

---

## Self-Test

Self-test dijalankan saat boot dengan menambahkan flag ke `make run`:

| Perintah | Test |
| :--- | :--- |
| `make run DISK_TEST=1` | Driver ATA: tulis lalu baca ulang LBA 17 |
| `make run FS_TEST=1` | EXT2: integritas write dan read |
| `make run SWAP_TEST=1` | Swap dan page fault |

`DISK_TEST=1` menimpa LBA 17 (byte offset `0x2200` di `bin/storage.bin`), jadi
jangan dipakai setelah filesystem berisi data penting. Jalankan `make disk`
untuk membuat disk baru jika diperlukan.
