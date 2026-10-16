![Credit: 朧月](mascot.jpg)

# Kelompok OSama

## Daftar Isi
- [Kelompok OSama](#kelompok-osama)
  - [Daftar Isi](#daftar-isi)
  - [Nama Kelompok \& Anggota](#nama-kelompok--anggota)
  - [Cara Run](#cara-run)
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

## Cara Run
```bash
make all
make run
```

`make run` membuat `bin/storage.bin` jika image belum ada, tetapi tidak
menimpa image yang sudah ada. Untuk membuat image kosong secara sengaja,
jalankan `make disk`.

### Pengujian Disk Driver

Uji round-trip write/read pada LBA 17 dapat dijalankan dengan:

```bash
make clean
make DISK_TEST=1 all
make run
```

Test ini menulis satu block (512 byte) ke LBA 17, membacanya kembali, lalu
membandingkan seluruh isinya. LBA 17 berada pada byte offset `0x2200` di
`bin/storage.bin`. Jangan gunakan `DISK_TEST=1` setelah filesystem mulai
menyimpan data karena test tersebut sengaja menimpa block LBA 17.

## Fitur
