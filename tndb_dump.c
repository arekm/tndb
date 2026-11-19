#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>


#include <trurl/nmalloc.h>

#include "tndb.h"

void *timethis_begin(void)
{
    struct timeval *tv;

    tv = n_malloc(sizeof(*tv));
    gettimeofday(tv, NULL);
    return tv;
}

void timethis_end(void *tvp, const char *prefix)
{
    struct timeval tv, *tv0 = (struct timeval *)tvp;

    gettimeofday(&tv, NULL);

    tv.tv_sec -= tv0->tv_sec;
    tv.tv_usec -= tv0->tv_usec;
    if (tv.tv_usec < 0) {
        tv.tv_sec--;
        tv.tv_usec = 1000000 + tv.tv_usec;
    }

    printf("time [%s] %lld.%ld\n", prefix, (long long)tv.tv_sec,
           (long)tv.tv_usec);
    free(tvp);
}

#define DUMP_DATA (1 << 0)

static
int do_walk(const char *name, unsigned flags)
{
    uint32_t        vlen;
    uint32_t        voffs;
    struct tndb     *db;
    struct tndb_it  it;
    char            key[TNDB_KEY_MAX + 1];
    int             rc;
    unsigned int    kn;

    if ((db = tndb_open(name)) == NULL) {
        fprintf(stderr, "%s: %m", name);
        return -1;
    }

    if (!tndb_verify(db)) {
        fprintf(stderr, "%s: BROKEN", name);
        return -1;
    }


    if (!tndb_it_start(db, &it)) {
        fprintf(stderr, "%s: tndb_it_start failed", name);
        return -1;
    }

    while ((rc = tndb_it_get_voff(&it, key, &kn, &voffs, &vlen)) > 0) {
        printf("KEY = %s\n", key);
        if (flags & DUMP_DATA) {
            unsigned char *buf;
            int i, n;

            /* tndb_read() returns the length read as an int */
            if (vlen > INT_MAX) {
                fprintf(stderr, "%s: %s: a value of %u bytes is too long to dump\n",
                        name, key, vlen);
                return -1;
            }

            buf = n_malloc(vlen + 1);
            if ((n = tndb_read(db, voffs, buf, vlen)) != (int)vlen) {
                /* the end of the file leaves errno as it was */
                if (n < 0)
                    fprintf(stderr, "%s: %s: cannot read the value\n", name, key);
                else
                    fprintf(stderr, "%s: %s: read %d of %u bytes of the value\n",
                            name, key, n, vlen);
                return -1;
            }
            buf[vlen] = '\0';

            for (i=0; i < (int)vlen; i++)
                if (!isprint(buf[i]))
                    buf[i] = '.';

            printf("DATA = %s\n\n--------------------------------------------------\n", buf);
        }
    }

    if (rc < 0) {
        fprintf(stderr, "%s: tndb_it_get_voff failed\n", name);
        return -1;
    }
    return 0;
}


int main(int argc, char *argv[])
{
    if (argc > 1 && do_walk(argv[1], DUMP_DATA) != 0)
        return EXIT_FAILURE;

    return 0;
}
