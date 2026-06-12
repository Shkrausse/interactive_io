# interactive_io — GNU libc vs POSIX I/O Shell

An interactive C program that lets you drive the two-layer Linux I/O model
one command at a time and watch the internal state of `FILE *` change in real
time through the `_IO_FILE` struct fields.

---

## What Problem This Solves

When you call `fwrite()` in C, your data does **not** go to disk. It goes into
a RAM buffer inside glibc. The kernel knows nothing about it until `fflush()`
is called (or the buffer fills, or the file is closed). Meanwhile, POSIX calls
like `write()` and `pread()` bypass that buffer entirely and talk straight to
the kernel.

This creates two layers that can disagree with each other:

```
fwrite("hello")
      ↓
[ glibc _IO_FILE buffer in process RAM ]   ← invisible to kernel
      ↓  only on fflush() / buffer full
   write(fd, ...)
      ↓
   kernel page cache → disk
```

This shell makes that gap visible and explorable.

---

## Build

```bash
gcc -Wall -o interactive_io interactive_io.c
./interactive_io
```

Requires glibc (standard on any Linux system). The `_IO_FILE` struct fields
are accessed via `#include <bits/types/struct_FILE.h>`, which is part of the
public glibc headers — no hacks or private includes required.

---

## Commands

| Command | Layer | What it does |
|---|---|---|
| `open` | Both | Opens `test.txt`; creates fd (POSIX) and wraps it in FILE* (glibc) |
| `fwrite <text>` | glibc | Copies text into the glibc RAM buffer. Kernel untouched. |
| `write <text>` | POSIX | Writes directly to the kernel. Bypasses glibc buffer entirely. |
| `fread` | glibc | Reads via the glibc read buffer. Fills `_IO_read_ptr`/`_IO_read_end`. |
| `pread` | POSIX | Reads from kernel offset 0. Bypasses glibc buffer entirely. |
| `fflush` | glibc | Delivers the pending glibc write buffer to the kernel via `write()`. |
| `setvbuf` | glibc | Disables buffering. Shows before/after `_IO_FILE` struct diff. |
| `bufinfo` | glibc | Dumps all live `_IO_FILE` buffer pointer fields. |
| `diverge` | Both | Runs `ftell` and `lseek` back to back and reports the gap. |
| `ftell` | glibc | FILE* logical position (includes unflushed bytes). |
| `offset` | POSIX | Kernel file offset via `lseek(SEEK_CUR)` (only flushed bytes). |
| `fileno` | Both | Shows `fd` and `fileno(fp)` — confirms they are the same descriptor. |
| `exit` | — | Closes file, deletes `test.txt`, quits. |

---

## The `_IO_FILE` Struct

`FILE *` is a typedef for `struct _IO_FILE *`, defined in glibc at:

```
glibc/libio/bits/types/struct_FILE.h   ← public typedef and field declarations
glibc/libio/libio.h                    ← full internal definition
```

The fields this program exposes:

```c
struct _IO_FILE {
    int    _fileno;         // the underlying POSIX fd — what fileno() returns

    char  *_IO_buf_base;    // start of the glibc-allocated buffer in RAM
    char  *_IO_buf_end;     // one-past-end of that buffer
                            // _IO_buf_end - _IO_buf_base = total capacity

    char  *_IO_write_base;  // start of the write staging region
    char  *_IO_write_ptr;   // current write position — advances on fwrite()
    char  *_IO_write_end;   // end of the write region
                            // _IO_write_ptr - _IO_write_base = bytes pending

    char  *_IO_read_ptr;    // current read position inside the read buffer
    char  *_IO_read_end;    // end of readable data in the read buffer
                            // _IO_read_end - _IO_read_ptr = bytes buffered

    // ... many more fields (lock, markers, chain, etc.)
};
```

The program casts `(FILE *)` to `(struct _IO_FILE *)` directly — legal on
Linux/glibc because they are the same type in memory.

---

## Recommended Demo Sequence

Run these commands in order for the full demonstration. Each step is
annotated in `io_demo_walkthrough.md`.

```
open
fwrite hello        ← watch _IO_write_ptr advance 5 bytes
diverge             ← ftell=5, lseek=0, GAP=5 (smoking gun)
fflush              ← watch _IO_write_ptr snap back to _IO_write_base
diverge             ← GAP=0, both layers now agree
write world         ← POSIX write, no buffer involved
pread               ← kernel sees "helloworld" (10 bytes)
fread               ← glibc reads same content, fills read buffer
setvbuf             ← watch buffer shrink from 4096 bytes to 1 byte
fwrite after_setvbuf← 0 bytes pending — went straight to kernel
pread               ← kernel sees all 23 bytes: "helloworldafter_setvbuf"
offset              ← kernel offset = 23
ftell               ← FILE* position = 23 (agree because no pending buffer)
fileno              ← fd=3, fileno(fp)=3 — same underlying file description
exit
```

---

## Key Concepts Demonstrated

### The write gap

After `fwrite` but before `fflush`, the file is empty on disk. The kernel
offset is 0. glibc's `ftell` reports 5. `pread` returns 0 bytes. This is
the gap. A process crash here loses the data.

### Pointer arithmetic as proof

```
_IO_write_ptr  = 0x...04a5
_IO_write_base = 0x...04a0
               = 5 bytes pending    ← exactly strlen("hello")
```

The difference between the two pointers is the byte count of unflushed data.
After `fflush` they become equal and the pending count drops to 0.

### `setvbuf` changes the struct, not just behavior

Before `setvbuf(_IONBF)`:
```
_IO_buf_base = 0x...04a0
_IO_buf_end  = 0x...14a0   (total = 4096 bytes)
```

After `setvbuf(_IONBF)`:
```
_IO_buf_base = 0x...0343
_IO_buf_end  = 0x...0344   (total = 1 byte)
```

glibc frees the 4096-byte buffer and replaces it with a 1-byte internal
sentinel. The buffer pointer address changes. Every subsequent `fwrite`
falls through to `write()` immediately — no staging.

### `pread` vs `fread`

Both can return the same content, but through opposite paths:

- `pread()` — pure POSIX syscall, bypasses `_IO_FILE` entirely, does not
  move the FILE* cursor, does not touch `_IO_read_ptr`
- `fread()` — goes through glibc, fills the read buffer in `_IO_FILE`,
  advances `_IO_read_ptr`, moves the FILE* cursor

After `fread`, `bufinfo` shows non-zero values in the read pointer fields.
After `pread`, they are unchanged.

### `ftell` vs `lseek` — two views of position

| Function | Layer | Reports |
|---|---|---|
| `ftell(fp)` | glibc | Logical position including unflushed write buffer |
| `lseek(fd, 0, SEEK_CUR)` | kernel | Actual file offset of bytes delivered |

These agree only when the glibc buffer is empty. The `diverge` command
reports both and computes the gap explicitly.

---

## Files

| File | Description |
|---|---|
| `interactive_io.c` | The interactive shell source |
| `io_demo_walkthrough.md` | Full annotated input/output from a live run |
| `README.md` | This file |

---

## Further Reading

- `man 3 fwrite` / `man 3 fflush` / `man 3 setvbuf` — glibc stdio
- `man 2 write` / `man 2 pread` / `man 2 lseek` — POSIX syscalls
- glibc source: `glibc/libio/libio.h` — full `_IO_FILE` definition
- glibc source: `glibc/libio/iofwrite.c` — what `fwrite` actually does
- glibc source: `glibc/libio/genops.c` — `_IO_default_xsputn`, the path
  from `fwrite` down to `write()`
