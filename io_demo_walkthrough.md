# Interactive I/O Shell — Annotated Demo

All output below is real, captured from a live run. Addresses are from the
actual process memory on the run machine; yours will differ but the
**relationships** (pointer arithmetic, gaps, convergences) will be identical.

---

## Step 1 — `open`

**Input:**
```
> open
```

**Output:**
```
[System] Opened test.txt
  fd = 3,  fileno(fp) = 3
```

**What happened:**  
`open()` asked the kernel for a file descriptor (fd = 3). `fdopen()` then
wrapped that fd in a `FILE *`, allocating the `_IO_FILE` struct in the
process heap. `fileno(fp)` confirms both handles point at the same underlying
kernel file description. No buffer exists yet — glibc allocates it lazily on
the first I/O call.

---

## Step 2 — `fwrite hello`

**Input:**
```
> fwrite hello
```

**Output:**
```
[C Library] Buffered 'hello' into glibc RAM buffer.
            Data has NOT reached the kernel yet.
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce04a0
    _IO_buf_end    = 0x55f733ce14a0   (total buffer = 4096 bytes)
    _IO_write_base = 0x55f733ce04a0
    _IO_write_ptr  = 0x55f733ce04a5   (5 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce14a0   (4091 bytes free in write buffer)
    _IO_read_ptr   = 0x55f733ce04a0
    _IO_read_end   = 0x55f733ce04a0   (0 bytes in read buffer)
```

**What happened:**  
glibc allocated a 4096-byte buffer in RAM (the distance from `_IO_buf_base`
to `_IO_buf_end`). The 5 bytes of "hello" were copied into the start of it.
`_IO_write_ptr` moved exactly 5 bytes forward from `_IO_write_base`. The
kernel has not been touched — no `write()` syscall occurred.

Key arithmetic:
```
_IO_write_ptr - _IO_write_base = 0x04a5 - 0x04a0 = 5  ← "hello" is 5 bytes
```

---

## Step 3 — `diverge` (before flush)

**Input:**
```
> diverge
```

**Output:**
```
[Divergence Report]
  ftell(fp)  = 5   (glibc's logical position)
  lseek(fd)  = 0   (kernel's actual offset)
  GAP = 5 bytes sitting UNFLUSHED in glibc buffer!
```

**What happened:**  
This is the core of the two-layer model made visible.

- `ftell()` asks glibc where the FILE* cursor is. glibc knows it has buffered
  5 bytes, so it reports position 5.
- `lseek(fd, 0, SEEK_CUR)` asks the **kernel** where the file offset is. The
  kernel has received nothing, so it reports 0.
- The gap of 5 is precisely the bytes sitting in `_IO_FILE`'s write buffer.

The file on disk is empty right now. If the process crashed here, "hello"
would be lost.

---

## Step 4 — `fflush`

**Input:**
```
> fflush
```

**Output:**
```
[C Library] Flushed glibc write buffer → kernel.
            _IO_write_ptr should now equal _IO_write_base.
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce04a0
    _IO_buf_end    = 0x55f733ce14a0   (total buffer = 4096 bytes)
    _IO_write_base = 0x55f733ce04a0
    _IO_write_ptr  = 0x55f733ce04a0   (0 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce14a0   (4096 bytes free in write buffer)
    _IO_read_ptr   = 0x55f733ce04a0
    _IO_read_end   = 0x55f733ce04a0   (0 bytes in read buffer)
```

**What happened:**  
`fflush()` called `write(fd, buf, 5)` internally, sending "hello" to the
kernel. `_IO_write_ptr` snapped back to equal `_IO_write_base` — the buffer
is now empty. The write buffer free space jumped back to 4096.

---

## Step 5 — `diverge` (after flush)

**Input:**
```
> diverge
```

**Output:**
```
[Divergence Report]
  ftell(fp)  = 5   (glibc's logical position)
  lseek(fd)  = 5   (kernel's actual offset)
  GAP = 0  (buffer is empty, positions agree)
```

**What happened:**  
Both layers now agree. The kernel offset advanced to 5 when `fflush` delivered
the bytes. Gap = 0 confirms nothing is sitting unflushed in the glibc buffer.

---

## Step 6 — `write world`

**Input:**
```
> write world
```

**Output:**
```
[Kernel] Wrote 'world' directly to kernel page cache.
```

**What happened:**  
`write(fd, "world", 5)` is a direct POSIX syscall — it bypasses `_IO_FILE`
entirely. glibc's buffer is untouched. The kernel offset advances from 5 to
10. The file now contains "helloworld".

---

## Step 7 — `pread`

**Input:**
```
> pread
```

**Output:**
```
[Kernel] pread from offset 0: 10 bytes read
         Content: 'helloworld'
         (pread bypasses glibc buffer entirely)
```

**What happened:**  
`pread(fd, buf, 255, 0)` is also a direct syscall. It reads from offset 0
without disturbing the current file offset. It sees both "hello" (which was
flushed in step 4) and "world" (written directly in step 6). The glibc buffer
is not involved in either direction.

---

## Step 8 — `fread`

**Input:**
```
> fread
```

**Output:**
```
[C Library] fread from position 0: 10 bytes
            Content: 'helloworld'
            (fread fills the glibc read buffer)
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce04a0
    _IO_buf_end    = 0x55f733ce14a0   (total buffer = 4096 bytes)
    _IO_write_base = 0x55f733ce04a0
    _IO_write_ptr  = 0x55f733ce04a0   (0 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce04a0   (0 bytes free in write buffer)   ← write side locked
    _IO_read_ptr   = 0x55f733ce04a0
    _IO_read_end   = 0x55f733ce04a0   (0 bytes in read buffer)
```

**What happened:**  
`fread()` issued its own `read()` syscall to fill the glibc read buffer, then
copied data out of it. After the read the FILE* is in "read mode" — notice
`_IO_write_end` collapsed to equal `_IO_write_base` (write side locked out
while in read mode). Contrast with `pread`: both returned the same content
here, but `fread` went through the glibc buffer and moved the FILE* cursor,
while `pread` used no buffer and left the cursor untouched.

---

## Step 9 — `setvbuf` (before/after diff)

**Input:**
```
> setvbuf
```

**Output:**
```
[Before setvbuf]
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce04a0
    _IO_buf_end    = 0x55f733ce14a0   (total buffer = 4096 bytes)
    _IO_write_base = 0x55f733ce04a0
    _IO_write_ptr  = 0x55f733ce04a0   (0 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce04a0   (0 bytes free in write buffer)
    _IO_read_ptr   = 0x55f733ce04a0
    _IO_read_end   = 0x55f733ce04a0   (0 bytes in read buffer)
[After  setvbuf]
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce0343
    _IO_buf_end    = 0x55f733ce0344   (total buffer = 1 bytes)
    _IO_write_base = 0x55f733ce0343
    _IO_write_ptr  = 0x55f733ce0343   (0 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce0343   (0 bytes free in write buffer)
    _IO_read_ptr   = 0x55f733ce0343
    _IO_read_end   = 0x55f733ce0343   (0 bytes in read buffer)
[C Library] Buffering disabled. fwrite now acts like write().
```

**What happened:**  
Before: buffer is 4096 bytes at address `0x...04a0`.  
After: buffer is **1 byte** at a new address `0x...0343`.

glibc uses a 1-byte internal stub as a sentinel when `_IONBF` is set — it
can't truly have zero buffer storage because some internal paths always need
at least one byte, but effectively no staging happens. Every `fwrite` now
falls through to a `write()` syscall immediately. The struct change is what
proves it — this isn't just a behavior flag, the actual buffer pointer and
size changed.

---

## Step 10 — `fwrite after_setvbuf`

**Input:**
```
> fwrite after_setvbuf
```

**Output:**
```
[C Library] Buffered 'after_setvbuf' into glibc RAM buffer.
            Data has NOT reached the kernel yet.
  [_IO_FILE internals]
    _fileno        = 3
    _IO_buf_base   = 0x55f733ce0343
    _IO_buf_end    = 0x55f733ce0344   (total buffer = 1 bytes)
    _IO_write_base = 0x55f733ce0343
    _IO_write_ptr  = 0x55f733ce0343   (0 bytes pending / unflushed)
    _IO_write_end  = 0x55f733ce0343   (0 bytes free in write buffer)
```

**What happened:**  
The message still says "buffered into RAM" (that's the command label), but the
struct tells the real story: 0 bytes pending, 0 bytes free — nothing was
staged. The data went straight through to the kernel as if `write()` had been
called directly. Contrast this with Step 2 where the same command showed
"5 bytes pending".

---

## Step 11 — `pread` (final state)

**Input:**
```
> pread
```

**Output:**
```
[Kernel] pread from offset 0: 23 bytes read
         Content: 'helloworldafter_setvbuf'
         (pread bypasses glibc buffer entirely)
```

**What happened:**  
The file now contains all three writes concatenated:
```
"hello"         (5 bytes)  — fwrite → fflush → kernel
"world"         (5 bytes)  — write() directly
"after_setvbuf" (13 bytes) — fwrite after _IONBF, no buffer stage
```
Total = 23 bytes. `pread` from offset 0 sees all of them.

---

## Step 12 — `offset`, `ftell`, `fileno`

**Input:**
```
> offset
> ftell
> fileno
```

**Output:**
```
[Kernel] Current file offset = 23
[C Library] FILE* logical position = 23
  fd           = 3
  fileno(fp)   = 3
  (Both refer to the same kernel file description)
```

**What happened:**  
With buffering disabled, both layers agree again — no staging means no
divergence possible. `fd` and `fileno(fp)` are identical (both 3), confirming
that `FILE *` is just a wrapper around the same kernel file description that
the raw fd points to. `fileno()` is literally reading `f->_fileno` out of the
`_IO_FILE` struct.

---

## Summary — The Full Picture

```
Action            glibc buffer     kernel offset    file on disk
──────────────────────────────────────────────────────────────────
open              empty            0                (empty)
fwrite hello      5 bytes pending  0                (empty)   ← GAP
diverge           —                —                shows gap = 5
fflush            0 bytes pending  5                "hello"
write world       0 bytes pending  10               "helloworld"
setvbuf _IONBF    buffer = 1 byte  10               "helloworld"
fwrite after_sv   0 bytes pending  23               "helloworldafter_setvbuf"
```

The GAP between `ftell` and `lseek` in row 3 is the entire point:
glibc's buffer is invisible to the kernel, invisible to other processes,
and invisible to your own POSIX syscalls until `fflush` (or `fclose`)
delivers it.
```
