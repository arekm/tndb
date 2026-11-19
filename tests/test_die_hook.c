/*
  A die hook may return (n_die_set_hook(); poldek's POLDEK_CONF_GOODBYE_CB
  returning 0 does that). Each call that reports through n_die() then
  returns an error, and the database stays as it was before the call.
*/
#include <config.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <trurl/n_check.h>
#include <trurl/ndie.h>

#include "tndb.h"

static char die_msg[1024];
static int ndies;

static void keep_going(const char *msg)
{
    snprintf(die_msg, sizeof(die_msg), "%s", msg);
    ndies++;
}

static void set_returning_die_hook(void)
{
    *die_msg = '\0';
    ndies = 0;
    n_die_set_hook(keep_going);
}

/* the length of a longer key cut to 8 bits is 1: the key "a" */
#define CUT_TO_A (TNDB_KEY_MAX + 2)

START_TEST(test_get_key_too_long)
{
    char *path = NTEST_TMPPATH("tndb_die_getkey.db");
    char key[CUT_TO_A], buf[16];
    unsigned int vlen;
    struct tndb *db;
    uint32_t voff;

    unlink(path);
    db = tndb_creat(path, -1, 0);
    expect_notnull(db);
    expect_int(tndb_put(db, "a", 1, "value", 5), 1);
    expect_int(tndb_close(db), 1);

    memset(key, 'a', sizeof(key));
    set_returning_die_hook();
    db = tndb_open(path);
    expect_notnull(db);
    expect_int(tndb_get_voff(db, key, sizeof(key), &voff, &vlen), -1);
    expect_int(ndies, 1);
    fail_unless(strstr(die_msg, "key too long") != NULL, "got '%s'", die_msg);
    expect_int(tndb_get(db, key, sizeof(key), buf, sizeof(buf)), 0);
    expect_int(ndies, 2);
    expect_int(tndb_close(db), 1);
    unlink(path);
}
END_TEST

START_TEST(test_put_key_too_long)
{
    char *path = NTEST_TMPPATH("tndb_die_putkey.db");
    char key[CUT_TO_A], buf[16];
    struct tndb *db;

    memset(key, 'a', sizeof(key));
    unlink(path);
    set_returning_die_hook();
    db = tndb_creat(path, -1, 0);
    expect_notnull(db);
    expect_int(tndb_put(db, key, sizeof(key), "value", 5), 0);
    expect_int(ndies, 1);
    fail_unless(strstr(die_msg, "too long") != NULL, "got '%s'", die_msg);
    expect_int(tndb_put(db, "b", 1, "bvalue", 6), 1);
    expect_int(tndb_close(db), 1);

    db = tndb_open(path);
    expect_notnull(db);
    expect_int((int)tndb_size(db), 1);
    expect_int(tndb_get(db, "a", 1, buf, sizeof(buf)), 0);
    expect_int(tndb_get(db, "b", 1, buf, sizeof(buf)), 6);
    expect_int(tndb_close(db), 1);
    unlink(path);
}
END_TEST

/* as the package index of poldek, which is iterated */
START_TEST(test_get_without_hash_table)
{
    char *path = NTEST_TMPPATH("tndb_die_nohash.db");
    char key[TNDB_KEY_MAX + 1], buf[16];
    unsigned int klen, vlen;
    struct tndb_it it;
    struct tndb *db;
    uint32_t voff;

    unlink(path);
    db = tndb_creat(path, -1, TNDB_NOHASH);
    expect_notnull(db);
    expect_int(tndb_put(db, "a", 1, "value", 5), 1);
    expect_int(tndb_close(db), 1);

    set_returning_die_hook();
    db = tndb_open(path);
    expect_notnull(db);
    expect_int(tndb_get_voff(db, "a", 1, &voff, &vlen), -1);
    expect_int(ndies, 1);
    fail_unless(strstr(die_msg, "without hash table") != NULL, "got '%s'", die_msg);

    vlen = sizeof(buf);
    expect_int(tndb_it_start(db, &it), 1);
    expect_int(tndb_it_get(&it, key, &klen, buf, &vlen), 1);
    expect_str(key, "a");
    expect_str(buf, "value");
    vlen = sizeof(buf);
    expect_int(tndb_it_get(&it, key, &klen, buf, &vlen), 0);   /* the end */
    expect_int(tndb_close(db), 1);
    unlink(path);
}
END_TEST

#define NREC 64

/* NREC keys "key00".. with the value "value"; then where the offset of a
   hash table from the second half is kept, and where that table is. The
   offsets of the 256 hash tables follow the header, then each table takes
   4 bytes and each record 8 more, up to the data. */
static void creat_with_tables(const char *path, uint32_t *entry, uint32_t *table)
{
    unsigned char offs[256 * 4];
    struct tndb_it it;
    struct tndb *db;
    uint32_t htt, off;
    char key[32];
    int fd, i, first = 1;

    unlink(path);
    db = tndb_creat(path, -1, 0);
    expect_notnull(db);
    for (i = 0; i < NREC; i++) {
        snprintf(key, sizeof(key), "key%.2d", i);
        expect_int(tndb_put(db, key, strlen(key), "value", 5), 1);
    }
    expect_int(tndb_close(db), 1);

    db = tndb_open(path);
    expect_notnull(db);
    expect_int(tndb_it_start(db, &it), 1);
    htt = it._off - 2 * sizeof(offs) - 8 * NREC;
    expect_int(tndb_close(db), 1);

    fd = open(path, O_RDONLY);
    fail_unless(fd >= 0, "open %s: %m", path);
    expect_int((int)pread(fd, offs, sizeof(offs), htt), (int)sizeof(offs));
    close(fd);

    for (i = 0; i < 256; i++) {
        unsigned char *o = offs + 4 * i;

        off = (uint32_t)o[0] << 24 | o[1] << 16 | o[2] << 8 | o[3];
        if (off == 0)
            continue;

        /* the tables before the first one are empty, 4 bytes each */
        if (first)
            fail_unless(off == htt + sizeof(offs) + 4 * i,
                        "%s: table %d at %u, not after the offsets", path, i, off);
        first = 0;

        if (i >= 128) {
            *entry = htt + 4 * i;
            *table = off;
            return;
        }
    }
    fail_unless(0, "%s: no hash table in the second half", path);
}

static void write_be32(const char *path, uint32_t off, uint32_t val)
{
    unsigned char be[4] = { val >> 24, val >> 16, val >> 8, val };
    int fd = open(path, O_WRONLY);

    fail_unless(fd >= 0, "open %s: %m", path);
    fail_unless(pwrite(fd, be, sizeof(be), off) == (ssize_t)sizeof(be),
                "write at %u: %m", off);
    close(fd);
}

/* each key of creat_with_tables() looked up: found, and failed */
static void get_keys(struct tndb *db, int *nfound, int *nfailed)
{
    unsigned int vlen;
    uint32_t voff;
    char key[32];
    int i, rc;

    *nfound = *nfailed = 0;
    for (i = 0; i < NREC; i++) {
        snprintf(key, sizeof(key), "key%.2d", i);
        rc = tndb_get_voff(db, key, strlen(key), &voff, &vlen);
        if (rc == 1)
            (*nfound)++;
        else if (rc == -1)
            (*nfailed)++;
    }
}

/* a hash table that cannot be read in full is not used in part: each
   lookup reports it and fails */
START_TEST(test_get_hash_table_broken)
{
    char *path = NTEST_TMPPATH("tndb_die_htbroken.db");
    uint32_t entry, table;
    struct tndb *db;
    int nfound, nfailed;

    creat_with_tables(path, &entry, &table);
    write_be32(path, entry, 0xffffff00);   /* past the end of the file */

    set_returning_die_hook();
    db = tndb_open(path);
    expect_notnull(db);
    get_keys(db, &nfound, &nfailed);
    expect_int(nfound, 0);
    expect_int(nfailed, NREC);
    expect_int(ndies, NREC);
    fail_unless(strstr(die_msg, "htt_read failed") != NULL, "got '%s'", die_msg);
    expect_int(tndb_close(db), 1);
    unlink(path);
}
END_TEST

NTEST_RUNNER("tndb-die-hook",
             test_get_key_too_long,
             test_put_key_too_long,
             test_get_without_hash_table,
             test_get_hash_table_broken);
