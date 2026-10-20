/*
 * machopatch.c  -  dependency-free port of tools/patch-roblox.py
 *
 * Inserts an LC_LOAD_WEAK_DYLIB entry into a thin 64-bit Mach-O (RobloxPlayer)
 * so the executor dylib loads on every launch, without DYLD_INSERT_LIBRARIES.
 * The command is written into the zero padding between the end of the load
 * commands and the first section, so nothing in the file moves and every code
 * offset stays valid.
 *
 * This exists as a compiled binary, not a script, because install.sh runs on
 * end-user Macs where /usr/bin/python3 is only a Command Line Tools stub. It is
 * bundled into Nexa.app/Contents/Resources and used by both install.sh and the
 * GUI's self-reinstall path.
 *
 * Weak on purpose (dyld carries on if the dylib is missing), reversible
 * (--remove strips the command, --restore goes back to the .unix-backup),
 * idempotent, and it refuses to run if the target region is not zero padding
 * or a code signature is still present - exactly like the Python original, and
 * it shares the same .unix-backup file so the two interoperate.
 *
 * --remove exists because a binary patched by an older executor carries a
 * load command for a dylib that is no longer ours, and loading two executors
 * into one client freezes it. The command is deleted from the header (the
 * following commands are shifted down and ncmds/sizeofcmds shrink), so no
 * section offset moves and no pristine backup is needed.
 *
 *   machopatch --binary <path> --dylib <abs path>   insert (default action)
 *   machopatch --binary <path> --remove             strip executor load commands
 *   machopatch --binary <path> --status             report patch state
 *   machopatch --binary <path> --restore            undo from backup
 *   [--quiet]
 *
 * Exit codes: 0 success / already-patched, 1 usage or precondition error,
 * 2 I/O error. Prints a single reason line to stderr on failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MH_MAGIC_64          0xFEEDFACFu
#define HEADER_SIZE          32
#define LC_REQ_DYLD          0x80000000u
#define LC_SEGMENT_64        0x19u
#define LC_LOAD_DYLIB        0x0Cu
#define LC_LOAD_WEAK_DYLIB   (0x18u | LC_REQ_DYLD)
#define LC_CODE_SIGNATURE    0x1Du

#define BACKUP_SUFFIX ".unix-backup"

static int quiet = 0;
static void say(const char *msg) { if (!quiet) printf("%s\n", msg); }
static int fail(const char *msg) { fprintf(stderr, "machopatch: %s\n", msg); return 1; }

/* Little-endian reads (macOS is LE on x86_64 and arm64). */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static int dump(const char *path, const uint8_t *buf, size_t len) {
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(buf, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    if (fflush(f) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    fclose(f);
    chmod(tmp, 0755);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    size_t len = 0;
    uint8_t *buf = slurp(src, &len);
    if (!buf) return -1;
    int rc = dump(dst, buf, len);
    free(buf);
    return rc;
}

/* Find the file offset of the lowest section (the end of the header region we
 * may write into). Returns 0 if none found. */
static uint32_t first_section_offset(const uint8_t *data, size_t len, uint32_t ncmds) {
    uint32_t lowest = 0; int have = 0;
    size_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > len) break;
        uint32_t cmd = rd32(data + off);
        uint32_t cmdsize = rd32(data + off + 4);
        if (cmdsize == 0) break;
        if (cmd == LC_SEGMENT_64) {
            if (off + 68 > len) break;
            uint32_t nsects = rd32(data + off + 64);
            size_t sec = off + 72;
            for (uint32_t s = 0; s < nsects; s++) {
                if (sec + 52 > len) break;
                uint32_t fo = rd32(data + sec + 48);
                if (fo > 0 && (!have || fo < lowest)) { lowest = fo; have = 1; }
                sec += 80;
            }
        }
        off += cmdsize;
    }
    return have ? lowest : 0;
}

static int has_code_signature(const uint8_t *data, size_t len, uint32_t ncmds) {
    size_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > len) break;
        uint32_t cmd = rd32(data + off);
        uint32_t cmdsize = rd32(data + off + 4);
        if (cmdsize == 0) break;
        if (cmd == LC_CODE_SIGNATURE) return 1;
        off += cmdsize;
    }
    return 0;
}

/* Every executor this repository has ever shipped under. A stale command for
 * any of them is a second executor in the process. */
static int is_executor_dylib(const char *name, size_t maxn) {
    static const char *libs[] = { "libNexa.dylib", "libUnix.dylib", "libVanta.dylib" };
    for (size_t k = 0; k < sizeof(libs)/sizeof(libs[0]); k++) {
        if (memmem(name, maxn, libs[k], strlen(libs[k])))
            return 1;
    }
    return 0;
}

/* Copy the name of an LC_LOAD*_DYLIB command, NUL-terminated. Returns 0 if the
 * command's name is malformed. */
static int dylib_command_name(const uint8_t *data, size_t len, size_t off,
                              uint32_t cmdsize, char *out, size_t out_sz) {
    if (off + 12 > len) return 0;
    uint32_t name_off = rd32(data + off + 8);
    if (name_off < 12 || name_off >= cmdsize || off + cmdsize > len)
        return 0;
    const char *name = (const char *)(data + off + name_off);
    size_t maxn = cmdsize - name_off;
    size_t n = strnlen(name, maxn);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, name, n); out[n] = '\0';
    return 1;
}

/* Find the first LC_LOAD*_DYLIB whose name contains one of the executor lib
 * names; copy its path into out (size cap). Returns 1 if found. */
static int find_executor_dylib(const uint8_t *data, size_t len, uint32_t ncmds,
                               char *out, size_t out_sz) {
    size_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > len) break;
        uint32_t cmd = rd32(data + off);
        uint32_t cmdsize = rd32(data + off + 4);
        if (cmdsize == 0) break;
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB) {
            char name[4096];
            if (dylib_command_name(data, len, off, cmdsize, name, sizeof(name)) &&
                is_executor_dylib(name, strlen(name))) {
                snprintf(out, out_sz, "%s", name);
                return 1;
            }
        }
        off += cmdsize;
    }
    return 0;
}

static int do_restore(const char *binary) {
    char backup[4096];
    snprintf(backup, sizeof(backup), "%s%s", binary, BACKUP_SUFFIX);
    struct stat st;
    if (stat(backup, &st) != 0) {
        fprintf(stderr, "machopatch: no backup at %s\n", backup);
        return 1;
    }
    if (copy_file(backup, binary) != 0) return fail("restore: could not copy backup");
    if (!quiet) printf("restored original from %s\n", backup);
    return 0;
}

static int do_patch(const char *binary, const char *dylib);

static int patch_after_restore(const char *binary, const char *dylib) {
    int rc = do_restore(binary);
    if (rc != 0) return rc;
    return do_patch(binary, dylib);
}

static int do_patch(const char *binary, const char *dylib) {
    size_t len = 0;
    uint8_t *data = slurp(binary, &len);
    if (!data) return fail("could not read binary");
    if (len < HEADER_SIZE || rd32(data) != MH_MAGIC_64) {
        free(data);
        return fail("not a thin 64-bit Mach-O (a fat binary would need lipo first)");
    }
    uint32_t ncmds = rd32(data + 16);
    uint32_t sizeofcmds = rd32(data + 20);

    char existing[4096];
    if (find_executor_dylib(data, len, ncmds, existing, sizeof(existing))) {
        if (strcmp(existing, dylib) == 0) {
            free(data);
            if (!quiet) printf("already patched -> %s\n", dylib);
            return 0;
        }
        /* Patched with a different path (e.g. the pre-rebrand libUnix.dylib, or
         * a moved repo): restore the pristine binary, then patch fresh. */
        free(data);
        if (!quiet) printf("patched with a different path (%s); restoring first\n", existing);
        return patch_after_restore(binary, dylib);
    }

    if (has_code_signature(data, len, ncmds)) {
        free(data);
        return fail("binary still carries a code signature; run codesign --remove-signature first");
    }

    /* Build the LC_LOAD_WEAK_DYLIB command. */
    size_t path_len = strlen(dylib) + 1;               /* include NUL */
    size_t cmdsize = (24 + path_len + 7) / 8 * 8;       /* 8-byte aligned */
    uint8_t *cmd = calloc(1, cmdsize);
    if (!cmd) { free(data); return fail("out of memory"); }
    wr32(cmd + 0,  LC_LOAD_WEAK_DYLIB);
    wr32(cmd + 4,  (uint32_t)cmdsize);
    wr32(cmd + 8,  24);          /* name offset within the command */
    wr32(cmd + 12, 0);           /* timestamp */
    wr32(cmd + 16, 0x10000);     /* current version 1.0.0 */
    wr32(cmd + 20, 0x10000);     /* compatibility version 1.0.0 */
    memcpy(cmd + 24, dylib, strlen(dylib));            /* NUL already from calloc */

    size_t end_of_commands = HEADER_SIZE + sizeofcmds;
    uint32_t first_sec = first_section_offset(data, len, ncmds);
    if (first_sec == 0 || first_sec < end_of_commands) {
        free(cmd); free(data);
        return fail("could not locate section padding region");
    }
    size_t available = first_sec - end_of_commands;
    if (cmdsize > available) {
        free(cmd); free(data);
        fprintf(stderr, "machopatch: only %zu bytes of padding after the load commands, need %zu\n",
                available, cmdsize);
        return 1;
    }
    /* Refuse to overwrite anything that is not zero padding. */
    for (size_t i = 0; i < cmdsize; i++) {
        if (data[end_of_commands + i] != 0) {
            free(cmd); free(data);
            return fail("the space after the load commands is not zero padding; refusing to write");
        }
    }

    /* Back up the pristine binary once. */
    char backup[4096];
    snprintf(backup, sizeof(backup), "%s%s", binary, BACKUP_SUFFIX);
    struct stat st;
    if (stat(backup, &st) != 0) {
        if (copy_file(binary, backup) != 0) { free(cmd); free(data); return fail("could not write backup"); }
        if (!quiet) { char m[4200]; snprintf(m, sizeof(m), "backed up original -> %s", backup); say(m); }
    }

    /* Splice the command in and bump ncmds / sizeofcmds. */
    memcpy(data + end_of_commands, cmd, cmdsize);
    wr32(data + 16, ncmds + 1);
    wr32(data + 20, sizeofcmds + (uint32_t)cmdsize);

    int rc = dump(binary, data, len);
    free(cmd); free(data);
    if (rc != 0) return fail("could not write patched binary");
    if (!quiet) { char m[4200]; snprintf(m, sizeof(m), "inserted LC_LOAD_WEAK_DYLIB -> %s", dylib); say(m); }
    return 0;
}

static int do_status(const char *binary) {
    size_t len = 0;
    uint8_t *data = slurp(binary, &len);
    if (!data) return fail("could not read binary");
    if (len < HEADER_SIZE || rd32(data) != MH_MAGIC_64) { free(data); return fail("not a thin 64-bit Mach-O"); }
    uint32_t ncmds = rd32(data + 16);
    char existing[4096];
    if (find_executor_dylib(data, len, ncmds, existing, sizeof(existing)))
        printf("patched -> %s\n", existing);
    else
        printf("not patched\n");
    free(data);
    return 0;
}

/* Delete every executor load command from the header. The commands that follow
 * each removed one are moved down by its size, ncmds and sizeofcmds shrink, and
 * the freed tail (which was zero padding when the command was inserted) is
 * zeroed again. Nothing outside the header region is touched, so every section
 * offset and the code itself stay valid. */
static int do_remove(const char *binary) {
    size_t len = 0;
    uint8_t *data = slurp(binary, &len);
    if (!data) return fail("could not read binary");
    if (len < HEADER_SIZE || rd32(data) != MH_MAGIC_64) {
        free(data);
        return fail("not a thin 64-bit Mach-O (a fat binary would need lipo first)");
    }
    uint32_t ncmds = rd32(data + 16);
    uint32_t sizeofcmds = rd32(data + 20);
    size_t end_of_commands = HEADER_SIZE + sizeofcmds;
    if (end_of_commands > len) { free(data); return fail("load commands run past the end of the file"); }
    if (has_code_signature(data, len, ncmds)) {
        free(data);
        return fail("binary still carries a code signature; run codesign --remove-signature first");
    }

    size_t read = HEADER_SIZE;
    size_t write = HEADER_SIZE;
    uint32_t kept = 0;
    int removed = 0;
    char removed_name[4096] = { 0 };
    for (uint32_t i = 0; i < ncmds; i++) {
        if (read + 8 > end_of_commands) { free(data); return fail("malformed load commands"); }
        uint32_t cmd = rd32(data + read);
        uint32_t cmdsize = rd32(data + read + 4);
        if (cmdsize < 8 || read + cmdsize > end_of_commands) {
            free(data);
            return fail("malformed load commands");
        }

        int drop = 0;
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB) {
            char name[4096];
            if (dylib_command_name(data, len, read, cmdsize, name, sizeof(name)) &&
                is_executor_dylib(name, strlen(name))) {
                drop = 1;
                if (removed == 0)
                    snprintf(removed_name, sizeof(removed_name), "%s", name);
                removed++;
            }
        }

        if (!drop) {
            if (write != read)
                memmove(data + write, data + read, cmdsize);
            write += cmdsize;
            kept++;
        }
        read += cmdsize;
    }

    if (removed == 0) {
        free(data);
        say("not patched");
        return 0;
    }

    memset(data + write, 0, end_of_commands - write);
    wr32(data + 16, kept);
    wr32(data + 20, (uint32_t)(write - HEADER_SIZE));

    int rc = dump(binary, data, len);
    free(data);
    if (rc != 0) return fail("could not write the unpatched binary");
    if (!quiet) {
        char m[4200];
        snprintf(m, sizeof(m), "removed %d executor load command%s -> %s",
                 removed, removed == 1 ? "" : "s", removed_name);
        say(m);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *binary = "/Applications/Roblox.app/Contents/MacOS/RobloxPlayer";
    const char *dylib = NULL;
    int want_restore = 0, want_status = 0, want_remove = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--binary") && i + 1 < argc) binary = argv[++i];
        else if (!strcmp(argv[i], "--dylib") && i + 1 < argc) dylib = argv[++i];
        else if (!strcmp(argv[i], "--restore")) want_restore = 1;
        else if (!strcmp(argv[i], "--remove")) want_remove = 1;
        else if (!strcmp(argv[i], "--status")) want_status = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { fprintf(stderr, "machopatch: unknown argument %s\n", argv[i]); return 1; }
    }

    if (want_restore) return do_restore(binary);
    if (want_status)  return do_status(binary);
    if (want_remove)  return do_remove(binary);
    if (!dylib)       return fail("--dylib is required");
    if (access(dylib, F_OK) != 0) return fail("dylib not found");
    return do_patch(binary, dylib);
}
