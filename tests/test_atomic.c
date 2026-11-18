/* the database is written as the hidden .<name>.tmp.XXXXXX next to it and
   renamed into place by tndb_close(): a failed close leaves an existing
   file as it was, and a new file gets the mode (and, with the privileges,
   the owner) of the old one.  rename(), n_stream_close() and
   copy_file_range() are wrapped to fail on demand. */
#define _GNU_SOURCE 1           /* copy_file_range(), loff_t */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <trurl/n_check.h>

#include "tndb.h"

static int rename_fails;
static int close_calls, close_fail_at;

int __real_rename(const char *oldpath, const char *newpath);
int __real_n_stream_close(tn_stream *st);

int __wrap_rename(const char *oldpath, const char *newpath)
{
    if (rename_fails) {
        errno = ENOSPC;
        return -1;
    }

    return __real_rename(oldpath, newpath);
}

static int cfr_mode;            /* 0 real, 1 always ENOSYS, 2 a part then ENOSYS */
static int cfr_calls;

#ifdef HAVE_COPY_FILE_RANGE
ssize_t __real_copy_file_range(int fd_in, loff_t *off_in, int fd_out,
                               loff_t *off_out, size_t len, unsigned int flags);

ssize_t __wrap_copy_file_range(int fd_in, loff_t *off_in, int fd_out,
                               loff_t *off_out, size_t len, unsigned int flags)
{
    if (cfr_mode == 1) {
        errno = ENOSYS;
        return -1;
    }

    if (cfr_mode == 2) {
        if (++cfr_calls == 1)
            return __real_copy_file_range(fd_in, off_in, fd_out, off_out,
                                          4096 < len ? 4096 : len, flags);
        errno = ENOSYS;
        return -1;
    }

    return __real_copy_file_range(fd_in, off_in, fd_out, off_out, len, flags);
}
#endif

int __wrap_n_stream_close(tn_stream *st)
{
    int rc = __real_n_stream_close(st);

    close_calls++;
    if (close_calls == close_fail_at) {
        errno = EIO;
        return -1;
    }

    return rc;
}

/* <dir>/.<name>.tmp.XXXXXX; keep in sync with open_tmpfile() */
static int tmp_files(const char *path)
{
    char pattern[PATH_MAX + 16];
    const char *base = strrchr(path, '/');
    glob_t g;
    int n;

    if (base != NULL)
        snprintf(pattern, sizeof(pattern), "%.*s.%s.tmp.*",
                 (int)(base - path + 1), path, base + 1);
    else
        snprintf(pattern, sizeof(pattern), ".%s.tmp.*", path);

    n = glob(pattern, 0, NULL, &g) == 0 ? (int)g.gl_pathc : 0;
    globfree(&g);
    return n;
}

static void write_db(const char *path, const char *val)
{
    struct tndb *db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);

    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "key", 3, val, strlen(val)), 1);
    expect_int(tndb_close(db), 1);
}

static void expect_db(const char *path, const char *val)
{
    struct tndb *db;
    char buf[64];
    int n;

    db = tndb_open(path);
    fail_if(db == NULL, "cannot open %s", path);
    expect_int(tndb_verify(db), 1);
    expect_int(tndb_size(db), 1);

    n = tndb_get(db, "key", 3, buf, sizeof(buf));
    expect_int(n, (int)strlen(val));
    buf[n] = '\0';
    expect_str(buf, val);

    expect_int(tndb_close(db), 1);
}

START_TEST(test_atomic_write)
{
    const char *path = NTEST_TMPPATH("tndb-atomic.db");

    unlink(path);
    write_db(path, "first");
    expect_db(path, "first");
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

/* a new file gets what open(..., 0666) would have given it, and a replaced
   one keeps the mode of the file it replaces, not the 0600 of the temp */
START_TEST(test_atomic_mode)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-mode.db");
    struct stat sb;
    mode_t mask;

    unlink(path);
    mask = umask(022);
    write_db(path, "new");
    umask(mask);

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int(sb.st_mode & 0777, 0644);

    fail_if(chmod(path, 0640) != 0, "cannot chmod %s", path);
    write_db(path, "newer");
    expect_db(path, "newer");

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int(sb.st_mode & 0777, 0640);
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

START_TEST(test_atomic_rename_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-rename.db");
    struct tndb *db;

    unlink(path);
    write_db(path, "old");

    rename_fails = 1;
    db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "key", 3, "new", 3), 1);
    expect_int(tndb_close(db), 0);
    rename_fails = 0;

    expect_db(path, "old");
    expect_int(tmp_files(path), 0);
    unlink(path);
}
END_TEST

/* both closes of the writer: the body's and the header's */
START_TEST(test_atomic_close_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-close.db");
    struct tndb *db;
    int n;

    for (n = 1; n <= 2; n++) {
        unlink(path);
        write_db(path, "old");

        close_calls = 0;
        close_fail_at = n;
        db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
        fail_if(db == NULL, "cannot create %s", path);
        expect_int(tndb_put(db, "key", 3, "new", 3), 1);
        expect_int(tndb_close(db), 0);
        close_fail_at = 0;

        expect_db(path, "old");
        expect_int(tmp_files(path), 0);
    }
    unlink(path);
}
END_TEST

#ifdef HAVE_COPY_FILE_RANGE
# define CFR_MODES 2          /* unusable, and a part then unusable */
#else
# define CFR_MODES 1
#endif

/* the body appended by read/write, when copy_file_range() is unusable or
   stops half way, is the same database */
START_TEST(test_atomic_copy_fallback)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-copy.db");
    size_t len = 64 * 1024;
    char *val, *got;
    struct tndb *db;
    int mode, n;

    val = malloc(len);
    got = malloc(len + 1);
    fail_if(val == NULL || got == NULL, "out of memory");

    for (size_t i = 0; i < len; i++)
        val[i] = (char)('a' + (i % 26));

    for (mode = 1; mode <= CFR_MODES; mode++) {
        unlink(path);

        cfr_mode = mode;
        cfr_calls = 0;

        db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
        fail_if(db == NULL, "cannot create %s", path);
        expect_int(tndb_put(db, "big", 3, val, len), 1);
        expect_int(tndb_close(db), 1);

        cfr_mode = 0;

        db = tndb_open(path);
        fail_if(db == NULL, "cannot open %s", path);
        expect_int(tndb_verify(db), 1);
        n = tndb_get(db, "big", 3, got, len + 1);
        expect_int(n, (int)len);
        fail_if(memcmp(got, val, len) != 0, "value differs (mode %d)", mode);
        expect_int(tndb_close(db), 1);
    }

    unlink(path);
    free(val);
    free(got);
}
END_TEST

NTEST_RUNNER("tndb-atomic",
             test_atomic_write,
             test_atomic_mode,
             test_atomic_rename_failure,
             test_atomic_close_failure,
             test_atomic_copy_fallback);
