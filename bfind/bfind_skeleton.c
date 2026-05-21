/*
 * bfind - Breadth-first find
 *
 * A BFS version of the UNIX find utility using POSIX system calls.
 *
 * Usage: ./bfind [-L] [-xdev] [path...] [filters...]
 *
 * Filters:
 *   -name PATTERN   Glob match on filename (fnmatch)
 *   -type TYPE      f (file), d (directory), l (symlink)
 *   -mtime N        Modified within the last N days
 *   -size SPEC      File size: [+|-]N[c|k|M]
 *   -perm MODE      Exact octal permission match
 *   -links N        Exact hard link count match
 *   -samefile FILE  Same inode as FILE (st_dev + st_ino)
 *
 * Options:
 *   -L              Follow symbolic links (default: no)
 *   -xdev           Do not cross filesystem boundaries
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "queue.h"

/* ------------------------------------------------------------------ */
/*  Inode identity (used for -samefile and cycle detection)            */
/* ------------------------------------------------------------------ */

typedef struct {
    dev_t dev;
    ino_t ino;
} dev_ino_t;

/* ------------------------------------------------------------------ */
/*  Filter definitions                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    FILTER_NAME,
    FILTER_TYPE,
    FILTER_MTIME,
    FILTER_SIZE,
    FILTER_PERM,
    FILTER_LINKS,
    FILTER_SAMEFILE
} filter_kind_t;

typedef enum {
    SIZE_CMP_EXACT,
    SIZE_CMP_GREATER,
    SIZE_CMP_LESS
} size_cmp_t;

typedef struct {
    filter_kind_t kind;
    union {
        char *pattern;       /* -name */
        char type_char;      /* -type: 'f', 'd', or 'l' */
        int mtime_days;      /* -mtime */
        struct {
            off_t size_bytes;
            size_cmp_t size_cmp;
        } size;              /* -size */
        mode_t perm_mode;    /* -perm */
        nlink_t nlinks;      /* -links */
        dev_ino_t samefile;  /* -samefile */
    } filter;
} filter_t;

/* ------------------------------------------------------------------ */
/*  Global configuration                                               */
/* ------------------------------------------------------------------ */

static filter_t *g_filters = NULL;
static int g_nfilters = 0;
static bool g_follow_links = false;
static bool g_xdev = false;
static dev_t g_start_dev = 0;
static time_t g_now;

/* ------------------------------------------------------------------ */
/*  Filter matching                                                    */
/* ------------------------------------------------------------------ */

/* Returns the filename component of a path (everything after the last '/'). */
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/*
 * TODO 1: Implement this function.
 *
 * Return true if the single filter 'f' matches the file at 'path' with
 * metadata 'sb'. Use a switch on f->kind and handle all seven cases:
 *
 *   FILTER_NAME     - fnmatch on basename_of(path)
 *   FILTER_TYPE     - S_ISREG / S_ISDIR / S_ISLNK on sb->st_mode
 *   FILTER_MTIME    - age in days vs f->filter.mtime_days
 *   FILTER_SIZE     - sb->st_size vs f->filter.size (with size_cmp_t)
 *   FILTER_PERM     - (sb->st_mode & 07777) vs f->filter.perm_mode
 *   FILTER_LINKS    - sb->st_nlink vs f->filter.nlinks
 *   FILTER_SAMEFILE - sb->st_dev + sb->st_ino vs f->filter.samefile
 *
 * Relevant man pages: fnmatch(3), stat(2).
 */
static bool filter_matches(const filter_t *f, const char *path,
                           const struct stat *sb) {
    (void)f;
    (void)path;
    (void)sb;
    /* TODO: Your implementation here */
    return false;
}

/* Check if ALL filters match (AND semantics).
 * Returns true if every filter matches, false otherwise. */
static bool matches_all_filters(const char *path, const struct stat *sb) {
    for (int i = 0; i < g_nfilters; i++) {
        if (!filter_matches(&g_filters[i], path, sb)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *progname) {
    printf("Usage: %s [-L] [-xdev] [path...] [filters...]\n"
           "\n"
           "Breadth-first search for files in a directory hierarchy.\n"
           "\n"
           "Options:\n"
           "  -L              Follow symbolic links\n"
           "  -xdev           Do not cross filesystem boundaries\n"
           "  --help          Display this help message and exit\n"
           "\n"
           "Filters (all filters are ANDed together):\n"
           "  -name PATTERN   Match filename against a glob pattern\n"
           "  -type TYPE      Match file type: f (file), d (dir), l (symlink)\n"
           "  -mtime N        Match files modified within the last N days\n"
           "  -size [+|-]N[c|k|M]\n"
           "                  Match file size (c=bytes, k=KiB, M=MiB)\n"
           "                  Prefix + means greater than, - means less than\n"
           "  -perm MODE      Match exact octal permission bits\n"
           "  -links N        Match entries with exactly N hard links\n"
           "  -samefile FILE  Match entries that refer to the same inode as FILE\n"
           "\n"
           "If no path is given, defaults to the current directory.\n",
           progname);
}

/* ------------------------------------------------------------------ */
/*  Argument parsing  (provided — do not modify)                       */
/* ------------------------------------------------------------------ */

static off_t parse_size(const char *arg) {
    char *end;
    long long val = strtoll(arg, &end, 10);
    if (val < 0) val = -val;
    switch (*end) {
    case 'c': case '\0': break;
    case 'k': val *= 1024; break;
    case 'M': val *= 1024 * 1024; break;
    default:
        fprintf(stderr, "bfind: unknown size suffix '%c'\n", *end);
        exit(1);
    }
    return (off_t)val;
}

static char **parse_args(int argc, char *argv[], int *npaths) {
    char **paths = malloc(argc * sizeof(char *));
    g_filters = malloc(argc * sizeof(filter_t));
    if (!paths || !g_filters) {
        fprintf(stderr, "bfind: out of memory\n");
        exit(1);
    }

    *npaths = 0;
    g_nfilters = 0;
    int i = 1;

    for (int h = 1; h < argc; h++) {
        if (strcmp(argv[h], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
    }

    /* Leading options: -L and -xdev */
    while (i < argc) {
        if (strcmp(argv[i], "-L") == 0) {
            g_follow_links = true; i++;
        } else if (strcmp(argv[i], "-xdev") == 0) {
            g_xdev = true; i++;
        } else {
            break;
        }
    }

    /* Paths: non-option arguments before the first filter */
    while (i < argc && argv[i][0] != '-') {
        paths[(*npaths)++] = argv[i++];
    }
    if (*npaths == 0) { paths[0] = "."; *npaths = 1; }

    /* Filters */
    while (i < argc) {
        if (strcmp(argv[i], "-name") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -name requires an argument\n"); exit(1); }
            g_filters[g_nfilters].kind = FILTER_NAME;
            g_filters[g_nfilters].filter.pattern = argv[i + 1];
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-type") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -type requires an argument\n"); exit(1); }
            char t = argv[i + 1][0];
            if (t != 'f' && t != 'd' && t != 'l') { fprintf(stderr, "bfind: -type must be f, d, or l\n"); exit(1); }
            g_filters[g_nfilters].kind = FILTER_TYPE;
            g_filters[g_nfilters].filter.type_char = t;
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-mtime") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -mtime requires an argument\n"); exit(1); }
            int days = atoi(argv[i + 1]);
            if (days < 0) { fprintf(stderr, "bfind: -mtime must be non-negative\n"); exit(1); }
            g_filters[g_nfilters].kind = FILTER_MTIME;
            g_filters[g_nfilters].filter.mtime_days = days;
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-size") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -size requires an argument\n"); exit(1); }
            const char *arg = argv[i + 1];
            g_filters[g_nfilters].kind = FILTER_SIZE;
            if (arg[0] == '+') {
                g_filters[g_nfilters].filter.size.size_cmp = SIZE_CMP_GREATER; arg++;
            } else if (arg[0] == '-') {
                g_filters[g_nfilters].filter.size.size_cmp = SIZE_CMP_LESS; arg++;
            } else {
                g_filters[g_nfilters].filter.size.size_cmp = SIZE_CMP_EXACT;
            }
            g_filters[g_nfilters].filter.size.size_bytes = parse_size(arg);
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-perm") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -perm requires an argument\n"); exit(1); }
            g_filters[g_nfilters].kind = FILTER_PERM;
            g_filters[g_nfilters].filter.perm_mode = (mode_t)strtoul(argv[i + 1], NULL, 8);
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-links") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -links requires an argument\n"); exit(1); }
            g_filters[g_nfilters].kind = FILTER_LINKS;
            g_filters[g_nfilters].filter.nlinks = (nlink_t)atoi(argv[i + 1]);
            g_nfilters++; i += 2;

        } else if (strcmp(argv[i], "-samefile") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "bfind: -samefile requires an argument\n"); exit(1); }
            struct stat ref_sb;
            if (stat(argv[i + 1], &ref_sb) < 0) {
                fprintf(stderr, "bfind: -samefile: cannot stat '%s': %s\n", argv[i + 1], strerror(errno));
                exit(1);
            }
            g_filters[g_nfilters].kind = FILTER_SAMEFILE;
            g_filters[g_nfilters].filter.samefile.dev = ref_sb.st_dev;
            g_filters[g_nfilters].filter.samefile.ino = ref_sb.st_ino;
            g_nfilters++; i += 2;

        } else {
            fprintf(stderr, "bfind: unknown option '%s'\n", argv[i]);
            exit(1);
        }
    }

    return paths;
}

/* ------------------------------------------------------------------ */
/*  BFS traversal                                                      */
/* ------------------------------------------------------------------ */

/*
 * TODO 2: Implement this function.
 *
 * Traverse the filesystem breadth-first starting from the given paths.
 * For each entry, check the filters and print matching paths to stdout.
 *
 * You must handle:
 *   - The -L flag: controls whether symlinks are followed. Think about
 *     when to use stat(2) vs lstat(2) and what that means for descending
 *     into directories.
 *   - The -xdev flag: do not descend into directories on a different
 *     filesystem than the starting path (compare st_dev values).
 *   - Cycle detection (only relevant with -L): a symlink can point back
 *     to an ancestor directory, creating an infinite loop. Only symlinks
 *     can create cycles — the OS forbids hard links to directories.
 *     Track visited (st_dev, st_ino) pairs using the dev_ino_t type.
 *     Important: record symlinks in the visited set, NOT real directories.
 *     Recording real directories would incorrectly block legitimate symlinks
 *     to non-ancestor directories depending on BFS traversal order. Real
 *     directories should always be descended into unconditionally.
 *   - Errors: if stat or opendir fails, print a message to stderr
 *     and continue traversing. Do not exit.
 *
 * The provided queue library (queue.h) implements a generic FIFO queue.
 */
static void bfs_traverse(char **start_paths, int npaths) {
    (void)start_paths;
    (void)npaths;
    /* TODO: Your implementation here */
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    g_now = time(NULL);

    int npaths;
    char **paths = parse_args(argc, argv, &npaths);

    bfs_traverse(paths, npaths);

    free(paths);
    free(g_filters);
    return 0;
}
