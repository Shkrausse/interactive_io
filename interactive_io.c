/*
 * interactive_io.c
 *
 * An interactive shell that demonstrates the two-layer I/O model in Linux:
 *
 *   Layer 1 — GNU libc (glibc):  FILE *, fwrite, fread, fflush, setvbuf, ftell
 *   Layer 2 — POSIX / Kernel:    fd,    write,  pread,          lseek
 *
 * The key struct behind FILE * is _IO_FILE, defined in glibc at:
 *   glibc/libio/bits/types/struct_FILE.h   (public-facing fields)
 *   glibc/libio/libio.h                    (full internal definition)
 *
 * Relevant _IO_FILE fields used here:
 *
 *   _IO_buf_base   — start of the glibc-allocated buffer in RAM
 *   _IO_buf_end    — one-past-end of that buffer
 *   _IO_write_base — start of the writable region
 *   _IO_write_ptr  — current write position (advances on every fwrite)
 *   _IO_write_end  — end of the writable region
 *   _IO_read_ptr   — current read position inside the read buffer
 *   _IO_read_end   — end of readable data in the read buffer
 *   _fileno        — the underlying POSIX file descriptor
 *
 * Commands added beyond the original:
 *   bufinfo  — dumps the live _IO_FILE buffer state
 *   diverge  — runs ftell + lseek back-to-back to show they can disagree
 *   fread    — reads via the glibc buffer (contrast with pread)
 *   setvbuf  — disables buffering AND shows a before/after bufinfo diff
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* _IO_FILE internals                                                   */
/*                                                                      */
/* glibc exposes the _IO_FILE struct fields through the public header   */
/* <bits/types/struct_FILE.h>. We cast (FILE *) to (struct _IO_FILE *) */
/* to read them directly — no hacks required on Linux/glibc.           */
/* ------------------------------------------------------------------ */
#include <bits/types/struct_FILE.h>   /* gives us struct _IO_FILE      */

/*
 * print_buf_state()
 *
 * Casts fp to its underlying struct _IO_FILE * and prints every
 * buffer-pointer field so the user can watch them change in real time.
 *
 * Key relationships to understand:
 *
 *   _IO_buf_base ... _IO_buf_end        total buffer allocation in RAM
 *   _IO_write_base .. _IO_write_ptr     bytes queued but NOT yet flushed
 *   _IO_write_ptr  .. _IO_write_end     free space left in the buffer
 *
 * When _IO_write_ptr == _IO_write_base  → buffer is empty (nothing pending)
 * When _IO_write_ptr == _IO_write_end   → buffer is full (next fwrite flushes)
 */
static void print_buf_state(FILE *fp)
{
    struct _IO_FILE *f = (struct _IO_FILE *)fp;

    /* Capacity of the entire glibc buffer */
    long total   = (f->_IO_buf_end   > f->_IO_buf_base)
                 ? (long)(f->_IO_buf_end   - f->_IO_buf_base) : 0;

    /* Bytes sitting in the write buffer, not yet sent to the kernel */
    long pending = (f->_IO_write_ptr > f->_IO_write_base)
                 ? (long)(f->_IO_write_ptr - f->_IO_write_base) : 0;

    /* Free space remaining before the buffer would overflow and auto-flush */
    long free_sp = (f->_IO_write_end > f->_IO_write_ptr)
                 ? (long)(f->_IO_write_end - f->_IO_write_ptr) : 0;

    /* Bytes available in the read buffer (non-zero after an fread) */
    long rbuf    = (f->_IO_read_end  > f->_IO_read_ptr)
                 ? (long)(f->_IO_read_end  - f->_IO_read_ptr)  : 0;

    printf("  [_IO_FILE internals]\n");
    printf("    _fileno        = %d\n",   f->_fileno);
    printf("    _IO_buf_base   = %p\n",   (void *)f->_IO_buf_base);
    printf("    _IO_buf_end    = %p   (total buffer = %ld bytes)\n",
           (void *)f->_IO_buf_end, total);
    printf("    _IO_write_base = %p\n",   (void *)f->_IO_write_base);
    printf("    _IO_write_ptr  = %p   (%ld bytes pending / unflushed)\n",
           (void *)f->_IO_write_ptr, pending);
    printf("    _IO_write_end  = %p   (%ld bytes free in write buffer)\n",
           (void *)f->_IO_write_end, free_sp);
    printf("    _IO_read_ptr   = %p\n",   (void *)f->_IO_read_ptr);
    printf("    _IO_read_end   = %p   (%ld bytes in read buffer)\n",
           (void *)f->_IO_read_end, rbuf);
}

/* ------------------------------------------------------------------ */
/* Main interactive shell                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    char  cmd[256];
    int   fd = -1;
    FILE *fp = NULL;

    printf("--- Interactive I/O Shell ---\n");
    printf("Commands:\n");
    printf("  open          - Opens 'test.txt'; gets fd and FILE*\n");
    printf("  fwrite <text> - Buffered write via glibc (FILE*)\n");
    printf("  write <text>  - Direct POSIX write to fd\n");
    printf("  pread         - POSIX pread from offset 0 (bypasses glibc buffer)\n");
    printf("  fread         - glibc fread from position 0 (goes through buffer)\n");
    printf("  fflush        - Flush the FILE* write buffer to kernel\n");
    printf("  setvbuf       - Disable FILE* buffering + show before/after bufinfo\n");
    printf("  bufinfo       - Dump live _IO_FILE buffer state\n");
    printf("  diverge       - Show ftell vs lseek disagreeing after fwrite\n");
    printf("  offset        - Kernel file offset via lseek\n");
    printf("  ftell         - FILE* logical position\n");
    printf("  fileno        - Show fd and fileno(fp)\n");
    printf("  exit          - Quit\n");

    while (1) {
        printf("\n> ");
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        cmd[strcspn(cmd, "\n")] = 0;   /* strip trailing newline */

        /* ---- exit ------------------------------------------------- */
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;

        /* ---- open -------------------------------------------------- */
        } else if (strcmp(cmd, "open") == 0) {
            fd = open("test.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open"); continue; }
            fp = fdopen(fd, "w+");
            if (!fp) { perror("fdopen"); continue; }
            printf("[System] Opened test.txt\n");
            printf("  fd = %d,  fileno(fp) = %d\n", fd, fileno(fp));

        /* ---- guard: must open first ------------------------------- */
        } else if (!fp) {
            printf("[Error] You must type 'open' first!\n");

        /* ---- fwrite ----------------------------------------------- */
        } else if (strncmp(cmd, "fwrite ", 7) == 0) {
            char *text = cmd + 7;
            fwrite(text, 1, strlen(text), fp);
            printf("[C Library] Buffered '%s' into glibc RAM buffer.\n", text);
            printf("            Data has NOT reached the kernel yet.\n");
            print_buf_state(fp);

        /* ---- write ------------------------------------------------- */
        } else if (strncmp(cmd, "write ", 6) == 0) {
            char *text = cmd + 6;
            write(fd, text, strlen(text));
            printf("[Kernel] Wrote '%s' directly to kernel page cache.\n", text);

        /* ---- pread ------------------------------------------------- */
        } else if (strcmp(cmd, "pread") == 0) {
            /*
             * pread() is a pure POSIX syscall — it has zero awareness of
             * the glibc buffer. If you fwrite'd something but haven't
             * fflush'd, pread will NOT see it because it is still in RAM
             * inside the _IO_FILE write buffer.
             */
            char buf[256] = {0};
            int  bytes    = pread(fd, buf, sizeof(buf) - 1, 0);
            printf("[Kernel] pread from offset 0: %d bytes read\n", bytes);
            printf("         Content: '%s'\n", buf);
            printf("         (pread bypasses glibc buffer entirely)\n");

        /* ---- fread ------------------------------------------------- */
        } else if (strcmp(cmd, "fread") == 0) {
            /*
             * fread() goes through the glibc layer. It will first flush
             * any pending write buffer, then reposition the FILE* to 0
             * using fseek, and read through the glibc read buffer.
             * Compare this with pread which skips all of that.
             *
             * After fread, the _IO_read_ptr / _IO_read_end fields inside
             * _IO_FILE will be non-NULL — you can see them in bufinfo.
             */
            fflush(fp);                        /* flush write side first  */
            fseek(fp, 0, SEEK_SET);            /* rewind FILE* to start   */
            char buf[256] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
            printf("[C Library] fread from position 0: %zu bytes\n", n);
            printf("            Content: '%s'\n", buf);
            printf("            (fread fills the glibc read buffer)\n");
            print_buf_state(fp);

        /* ---- fflush ------------------------------------------------ */
        } else if (strcmp(cmd, "fflush") == 0) {
            fflush(fp);
            printf("[C Library] Flushed glibc write buffer → kernel.\n");
            printf("            _IO_write_ptr should now equal _IO_write_base.\n");
            print_buf_state(fp);

        /* ---- setvbuf ----------------------------------------------- */
        } else if (strcmp(cmd, "setvbuf") == 0) {
            /*
             * setvbuf(fp, NULL, _IONBF, 0) tells glibc to disable its
             * internal buffer entirely. After this call:
             *   - _IO_buf_base and _IO_buf_end will both be NULL (or equal)
             *   - every fwrite() goes straight to write() with no staging
             *
             * This is exactly how you make a FILE* behave like a raw fd.
             * The before/after bufinfo diff makes the struct change visible.
             */
            printf("[Before setvbuf]\n");
            print_buf_state(fp);

            setvbuf(fp, NULL, _IONBF, 0);

            printf("[After  setvbuf]\n");
            print_buf_state(fp);
            printf("[C Library] Buffering disabled. fwrite now acts like write().\n");

        /* ---- bufinfo ----------------------------------------------- */
        } else if (strcmp(cmd, "bufinfo") == 0) {
            /*
             * Direct window into the _IO_FILE struct. Call this after
             * any command to see the live state of the glibc buffer.
             * The write pointer advancing is the smoking gun that data
             * is sitting in RAM and has not hit the kernel yet.
             */
            print_buf_state(fp);

        /* ---- diverge ----------------------------------------------- */
        } else if (strcmp(cmd, "diverge") == 0) {
            /*
             * The "diverge" command is the clearest demonstration of the
             * two-layer model:
             *
             *   ftell()  — asks glibc where it thinks the logical position
             *               is, including bytes that haven't been flushed.
             *               It reports the INTENDED position.
             *
             *   lseek()  — asks the kernel where the actual file offset is.
             *               Bytes still inside _IO_FILE's write buffer have
             *               NOT moved the kernel offset yet.
             *
             * After an fwrite without fflush, these two numbers DIFFER.
             * That gap is exactly how many bytes are sitting unflushed in
             * the glibc buffer.  After fflush, they converge.
             */
            long   glibc_pos  = ftell(fp);
            off_t  kernel_pos = lseek(fd, 0, SEEK_CUR);

            printf("[Divergence Report]\n");
            printf("  ftell(fp)  = %ld   (glibc's logical position)\n", glibc_pos);
            printf("  lseek(fd)  = %lld  (kernel's actual offset)\n",
                   (long long)kernel_pos);

            long gap = glibc_pos - (long)kernel_pos;
            if (gap > 0)
                printf("  GAP = %ld bytes sitting UNFLUSHED in glibc buffer!\n", gap);
            else if (gap == 0)
                printf("  GAP = 0  (buffer is empty, positions agree)\n");
            else
                printf("  GAP = %ld (kernel ahead — unusual; raw write?\n", gap);

        /* ---- offset ------------------------------------------------ */
        } else if (strcmp(cmd, "offset") == 0) {
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos == (off_t)-1) perror("lseek");
            else printf("[Kernel] Current file offset = %lld\n", (long long)pos);

        /* ---- ftell ------------------------------------------------- */
        } else if (strcmp(cmd, "ftell") == 0) {
            long pos = ftell(fp);
            if (pos == -1L) perror("ftell");
            else printf("[C Library] FILE* logical position = %ld\n", pos);

        /* ---- fileno ------------------------------------------------ */
        } else if (strcmp(cmd, "fileno") == 0) {
            printf("  fd           = %d\n", fd);
            printf("  fileno(fp)   = %d\n", fileno(fp));
            printf("  (Both refer to the same kernel file description)\n");

        /* ---- unknown ----------------------------------------------- */
        } else {
            printf("[Error] Unknown command.\n");
        }
    }

    if (fp) {
        fclose(fp);
        unlink("test.txt");
    }
    return 0;
}