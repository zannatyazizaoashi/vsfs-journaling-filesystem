# VSFS Journaling Filesystem Tools

A small educational filesystem project in C. It creates a VSFS disk image, records a file-creation transaction in a journal, applies the committed updates, and checks the resulting image for consistency. This is an individual Operating Systems project by **Zannaty Aziza**.

## What is included

| File | Purpose |
| --- | --- |
| [`mkfs.c`](mkfs.c) | Creates an 85-block filesystem image with a superblock, bitmaps, inode area, journal area, and root directory |
| [`journal.c`](journal.c) | Records a file-creation transaction and installs committed journal records into the filesystem image |
| [`validator.c`](validator.c) | Checks filesystem metadata, bitmaps, inodes, data-block references, and directory entries |
| [`Makefile`](Makefile) | Builds the three command-line tools with GCC |

The format uses 4 KiB blocks. The generated image and compiled programs are ignored by Git because they can be rebuilt from the source.

## Build and try it

Run this in Linux or WSL with GCC and `make` installed:

```bash
make
./mkfs
./validator
./journal create sample.txt
./journal install
./validator
```

`mkfs` creates `vsfs.img` in the current directory. `journal create` records the transaction; `journal install` applies it. The final `validator` command should report that the image is consistent. You can pass an alternate image path to `mkfs` and `validator`; `journal` currently uses `vsfs.img` in the current directory.

## Verification

The three tools were compiled with GCC 13 on Ubuntu 24.04. A smoke test created a fresh image, validated it, recorded a sample file creation, installed the journal, and validated the image again. Both validator runs reported a consistent filesystem. This is a small teaching implementation and the smoke test does not cover every malformed-image or crash-recovery case.
