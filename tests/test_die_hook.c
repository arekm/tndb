/*
  A die hook may return (n_die_set_hook(); poldek's POLDEK_CONF_GOODBYE_CB
  returning 0 does that). Each call that reports through n_die() then
  returns an error, and the database stays as it was before the call.
*/
#include <config.h>

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

NTEST_RUNNER("tndb-die-hook",
             test_get_key_too_long,
             test_put_key_too_long);
