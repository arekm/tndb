/* the database is written as the hidden .<name>.tmp.XXXXXX next to it and
   renamed into place by tndb_close(): a failed close leaves an existing
   file as it was, and a new file gets the mode (and, with the privileges,
   the owner) of the old one.  rename(), n_stream_close(), copy_file_range(),
   write(), close() and fchown() are wrapped to fail on demand, and so is
   n_stream_dopen(), to make a stream's flush fail. */
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
static char renamed_from[PATH_MAX];
static int close_calls, close_fail_at;
static int dopen_calls, flush_fail_at;

int __real_rename(const char *oldpath, const char *newpath);
int __real_n_stream_close(tn_stream *st);
tn_stream *__real_n_stream_dopen(int fd, const char *mode, int type);

int __wrap_rename(const char *oldpath, const char *newpath)
{
    snprintf(renamed_from, sizeof(renamed_from), "%s", oldpath);

    if (rename_fails) {
        errno = ENOSPC;
        return -1;
    }

    return __real_rename(oldpath, newpath);
}

/* 0 real, 1 always ENOSYS, 2 a part then ENOSYS, 3 a part then EIO */
static int cfr_mode;
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

    if (cfr_mode == 2 || cfr_mode == 3) {
        if (++cfr_calls == 1)
            return __real_copy_file_range(fd_in, off_in, fd_out, off_out,
                                          4096 < len ? 4096 : len, flags);
        errno = cfr_mode == 2 ? ENOSYS : EIO;
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

/* n_stream_flush() is inline, so the stream's own flush is the one
   replaced: of the flush_fail_at'th stream opened, 1 the body, 2 the
   header */
static int (*real_st_flush)(void *);
static int flush_failed;

static int failing_st_flush(void *stream)
{
    real_st_flush(stream);
    flush_failed++;
    errno = EIO;
    return EOF;
}

tn_stream *__wrap_n_stream_dopen(int fd, const char *mode, int type)
{
    tn_stream *st = __real_n_stream_dopen(fd, mode, type);

    if (st != NULL && ++dopen_calls == flush_fail_at) {
        real_st_flush = st->st_flush;
        st->st_flush = failing_st_flush;
    }

    return st;
}

/* the read/write fallback: an EINTR first, then at most 1000 bytes a call */
static int short_writes, write_calls;

ssize_t __real_write(int fd, const void *buf, size_t len);

ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
    if (short_writes) {
        if (write_calls++ == 0) {
            errno = EINTR;
            return -1;
        }

        if (len > 1000)
            len = 1000;
    }

    return __real_write(fd, buf, len);
}

/* fails the close of the final temp, the one fd tndb closes while the file
   still has a name (the body temp is unlinked at once); the fd is closed
   anyway, as on Linux */
static int close_named_fails;

int __real_close(int fd);

int __wrap_close(int fd)
{
    struct stat sb;

    if (close_named_fails && fstat(fd, &sb) == 0 && sb.st_nlink > 0) {
        __real_close(fd);
        errno = EIO;
        return -1;
    }

    return __real_close(fd);
}

static int fchown_calls, fchown_errno;
static uid_t fchown_uid;
static gid_t fchown_gid;

int __real_fchown(int fd, uid_t uid, gid_t gid);

int __wrap_fchown(int fd, uid_t uid, gid_t gid)
{
    fchown_calls++;
    fchown_uid = uid;
    fchown_gid = gid;

    if (fchown_errno) {
        errno = fchown_errno;
        return -1;
    }

    return __real_fchown(fd, uid, gid);
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

/* path and the .md5 tndb_verify() leaves next to it */
static void unlink_db(const char *path)
{
    char md5[PATH_MAX + 8];

    snprintf(md5, sizeof(md5), "%s.md5", path);
    unlink(md5);
    unlink(path);
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

/* a rewrite of path with "new" fails in tndb_close(), and leaves "old" */
static void expect_failed_rewrite(const char *path)
{
    struct tndb *db;

    db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "key", 3, "new", 3), 1);
    expect_int(tndb_close(db), 0);
}

/* the file renamed into place is the hidden temp next to path */
START_TEST(test_atomic_write)
{
    const char *path = NTEST_TMPPATH("tndb-atomic.db");
    const char *base = strrchr(path, '/') + 1;
    const char *from;
    char prefix[PATH_MAX];

    unlink_db(path);
    renamed_from[0] = '\0';
    write_db(path, "first");
    expect_db(path, "first");
    expect_int(tmp_files(path), 0);

    from = strrchr(renamed_from, '/');
    fail_if(from == NULL, "nothing renamed into %s", path);
    expect_int((int)(from - renamed_from), (int)(base - 1 - path));
    fail_if(strncmp(renamed_from, path, base - path) != 0,
            "%s is not next to %s", renamed_from, path);

    snprintf(prefix, sizeof(prefix), ".%s.tmp.", base);
    fail_if(strncmp(from + 1, prefix, strlen(prefix)) != 0,
            "%s is not %sXXXXXX", from + 1, prefix);
    expect_int((int)strlen(from + 1), (int)strlen(prefix) + 6);
    unlink_db(path);
}
END_TEST

/* a new file gets what open(..., 0666) would have given it, and a replaced
   one keeps the mode of the file it replaces, not the 0600 of the temp */
START_TEST(test_atomic_mode)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-mode.db");
    struct stat sb;
    mode_t mask;

    unlink_db(path);
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
    unlink_db(path);
}
END_TEST

/* both flushes of the writer, the body's and the header's, and the close()
   of the final temp */
START_TEST(test_atomic_flush_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-flush.db");
    int n;

    for (n = 1; n <= 2; n++) {
        unlink_db(path);
        write_db(path, "old");

        dopen_calls = flush_failed = 0;
        flush_fail_at = n;
        expect_failed_rewrite(path);
        flush_fail_at = 0;
        expect_int(flush_failed, 1);

        expect_db(path, "old");
        expect_int(tmp_files(path), 0);
    }
    unlink_db(path);
}
END_TEST

START_TEST(test_atomic_fdclose_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-fdclose.db");
    struct tndb *db;

    unlink_db(path);
    write_db(path, "old");

    db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "key", 3, "new", 3), 1);
    close_named_fails = 1;
    expect_int(tndb_close(db), 0);
    close_named_fails = 0;

    expect_db(path, "old");
    expect_int(tmp_files(path), 0);
    unlink_db(path);
}
END_TEST

START_TEST(test_atomic_rename_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-rename.db");
    struct tndb *db;

    unlink_db(path);
    write_db(path, "old");

    rename_fails = 1;
    db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "key", 3, "new", 3), 1);
    expect_int(tndb_close(db), 0);
    rename_fails = 0;

    expect_db(path, "old");
    expect_int(tmp_files(path), 0);
    unlink_db(path);
}
END_TEST

/* path, read through its symlinks, is a regular file holding val */
static void expect_link(const char *path, const char *val)
{
    struct stat sb;

    fail_if(lstat(path, &sb) != 0, "cannot lstat %s", path);
    fail_unless(S_ISLNK(sb.st_mode), "%s is no longer a symlink", path);
    expect_db(path, val);
}

/* the directories of the two paths are the same one */
static int same_dir(const char *a, const char *b)
{
    char da[PATH_MAX], db[PATH_MAX];
    struct stat sa, sb;

    snprintf(da, sizeof(da), "%.*s", (int)(strrchr(a, '/') - a), a);
    snprintf(db, sizeof(db), "%.*s", (int)(strrchr(b, '/') - b), b);
    fail_if(stat(da, &sa) != 0 || stat(db, &sb) != 0,
            "cannot stat %s or %s", da, db);

    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* a write through a symlink replaces its target, as the open(O_TRUNC) of
   the in-place write did: absolute, relative to the link's directory, a
   chain, and a dangling one, whose target is created */
START_TEST(test_atomic_symlink)
{
    const char *real = NTEST_TMPPATH("tndb-atomic-real.db");
    const char *link1 = NTEST_TMPPATH("tndb-atomic-link1.db");
    const char *link2 = NTEST_TMPPATH("tndb-atomic-link2.db");
    const char *dir = NTEST_TMPPATH("tndb-atomic-sub");
    char sublink[PATH_MAX], rel[PATH_MAX];

    snprintf(sublink, sizeof(sublink), "%s/link.db", dir);
    snprintf(rel, sizeof(rel), "../%s", strrchr(real, '/') + 1);

    unlink_db(real);
    unlink_db(link1);
    unlink_db(link2);
    unlink_db(sublink);
    rmdir(dir);
    write_db(real, "old");

    /* absolute */
    fail_if(symlink(real, link1) != 0, "cannot symlink %s", link1);
    write_db(link1, "abs");
    expect_link(link1, "abs");
    expect_db(real, "abs");
    expect_int(tmp_files(real), 0);
    expect_int(tmp_files(link1), 0);

    /* a chain: link2 -> link1 -> real */
    fail_if(symlink(link1, link2) != 0, "cannot symlink %s", link2);
    write_db(link2, "chain");
    expect_link(link2, "chain");
    expect_link(link1, "chain");
    expect_db(real, "chain");

    /* relative to the link's directory, not to the cwd */
    fail_if(mkdir(dir, 0755) != 0, "cannot mkdir %s", dir);
    fail_if(symlink(rel, sublink) != 0, "cannot symlink %s", sublink);
    renamed_from[0] = '\0';
    write_db(sublink, "rel");
    expect_link(sublink, "rel");
    expect_db(real, "rel");
    expect_int(tmp_files(sublink), 0);

    /* the temp is next to the target, so the rename() does not cross a
       filesystem */
    expect_int(same_dir(renamed_from, real), 1);

    /* dangling */
    unlink_db(real);
    write_db(link1, "dangling");
    expect_link(link1, "dangling");
    expect_db(real, "dangling");

    unlink_db(sublink);
    rmdir(dir);
    unlink_db(link2);
    unlink_db(link1);
    unlink_db(real);
}
END_TEST

/* a symlink loop fails the close, and nothing is left */
START_TEST(test_atomic_symlink_loop)
{
    const char *a = NTEST_TMPPATH("tndb-atomic-loop-a.db");
    const char *b = NTEST_TMPPATH("tndb-atomic-loop-b.db");
    struct tndb *db;

    unlink_db(a);
    unlink_db(b);
    fail_if(symlink(b, a) != 0 || symlink(a, b) != 0, "cannot symlink");

    db = tndb_creat(a, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", a);
    expect_int(tndb_put(db, "key", 3, "new", 3), 1);
    errno = 0;
    expect_int(tndb_close(db), 0);
    expect_int(errno, ELOOP);

    expect_int(tmp_files(a), 0);
    expect_int(tmp_files(b), 0);
    unlink_db(a);
    unlink_db(b);
}
END_TEST

/* the owner and group of a replaced file are passed to fchown(); a refused
   fchown() (EPERM, another user's file) still writes, any other error does
   not */
START_TEST(test_atomic_owner)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-owner.db");
    struct stat sb;

    unlink_db(path);
    fchown_calls = 0;
    write_db(path, "old");
    expect_int(fchown_calls, 0);        /* a new file is the writer's */

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    write_db(path, "newer");
    expect_int(fchown_calls, 1);
    expect_int((int)fchown_uid, (int)sb.st_uid);
    expect_int((int)fchown_gid, (int)sb.st_gid);

    fchown_errno = EPERM;
    write_db(path, "newest");
    expect_db(path, "newest");

    unlink_db(path);
    write_db(path, "old");
    fchown_errno = EIO;
    expect_failed_rewrite(path);
    fchown_errno = 0;

    expect_db(path, "old");
    expect_int(tmp_files(path), 0);
    unlink_db(path);
}
END_TEST

/* the real thing: a file chgrp'ed to another group of the writer keeps it */
START_TEST(test_atomic_group)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-group.db");
    gid_t groups[NGROUPS_MAX], gid = (gid_t)-1;
    struct stat sb;
    int i, n;

    n = getgroups(NGROUPS_MAX, groups);
    for (i = 0; i < n; i++) {
        if (groups[i] != getegid()) {
            gid = groups[i];
            break;
        }
    }

    if (gid == (gid_t)-1) {
        NTEST_LOG("skipped: no supplementary group\n");
        return;
    }

    unlink_db(path);
    write_db(path, "old");
    fail_if(chown(path, (uid_t)-1, gid) != 0, "cannot chgrp %s", path);
    write_db(path, "new");
    expect_db(path, "new");

    fail_if(stat(path, &sb) != 0, "cannot stat %s", path);
    expect_int((int)sb.st_gid, (int)gid);
    unlink_db(path);
}
END_TEST

/* both closes of the writer: the body's and the header's */
START_TEST(test_atomic_close_failure)
{
    const char *path = NTEST_TMPPATH("tndb-atomic-close.db");
    struct tndb *db;
    int n;

    for (n = 1; n <= 2; n++) {
        unlink_db(path);
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
    unlink_db(path);
}
END_TEST

#ifdef HAVE_COPY_FILE_RANGE
# define CFR_MODES 2          /* unusable, and a part then unusable */
#else
# define CFR_MODES 1
#endif

/* the body appended by read/write, when copy_file_range() is unusable or
   stops half way, is the same database; the last round (mode 0, read/write
   only without copy_file_range(), else mode 1) has short and interrupted
   writes */
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

    for (mode = 1; mode <= CFR_MODES + 1; mode++) {
        unlink_db(path);

        short_writes = mode > CFR_MODES;
        write_calls = 0;
        cfr_mode = short_writes ? 1 : mode;
        cfr_calls = 0;

        db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
        fail_if(db == NULL, "cannot create %s", path);
        expect_int(tndb_put(db, "big", 3, val, len), 1);
        expect_int(tndb_close(db), 1);

        cfr_mode = 0;
        if (short_writes) {
            short_writes = 0;
            fail_if(write_calls < 3, "no short writes (%d)", write_calls);
        }

        db = tndb_open(path);
        fail_if(db == NULL, "cannot open %s", path);
        expect_int(tndb_verify(db), 1);
        n = tndb_get(db, "big", 3, got, len + 1);
        expect_int(n, (int)len);
        fail_if(memcmp(got, val, len) != 0, "value differs (mode %d)", mode);
        expect_int(tndb_close(db), 1);
    }

    unlink_db(path);
    free(val);
    free(got);
}
END_TEST

/* a real error of copy_file_range() is not a reason to fall back: the
   close fails and the old file stays */
START_TEST(test_atomic_copy_failure)
{
#ifdef HAVE_COPY_FILE_RANGE
    const char *path = NTEST_TMPPATH("tndb-atomic-copyerr.db");
    size_t len = 64 * 1024;
    struct tndb *db;
    char *val;

    val = malloc(len);
    fail_if(val == NULL, "out of memory");
    memset(val, 'x', len);

    unlink_db(path);
    write_db(path, "old");

    cfr_mode = 3;
    cfr_calls = 0;
    db = tndb_creat(path, -1, TNDB_SIGN_DIGEST);
    fail_if(db == NULL, "cannot create %s", path);
    expect_int(tndb_put(db, "big", 3, val, len), 1);
    expect_int(tndb_close(db), 0);
    cfr_mode = 0;
    expect_int(cfr_calls, 2);

    expect_db(path, "old");
    expect_int(tmp_files(path), 0);
    unlink_db(path);
    free(val);
#endif
}
END_TEST

NTEST_RUNNER("tndb-atomic",
             test_atomic_write,
             test_atomic_mode,
             test_atomic_symlink,
             test_atomic_symlink_loop,
             test_atomic_owner,
             test_atomic_group,
             test_atomic_flush_failure,
             test_atomic_fdclose_failure,
             test_atomic_rename_failure,
             test_atomic_close_failure,
             test_atomic_copy_failure,
             test_atomic_copy_fallback);
