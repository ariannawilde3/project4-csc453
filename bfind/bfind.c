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

typedef struct {
    dev_ino_t *items;
    size_t size;
    size_t capacity;
} visited_set_t;

static void visited_set_init(visited_set_t *set) {
    set->items = NULL;
    set->size = 0;
    set->capacity = 0;
}

static void visited_set_destroy(visited_set_t *set) {
    free(set->items);
    set->items = NULL;
    set->size = 0;
    set->capacity = 0;
}

static bool visited_set_contains(const visited_set_t *set, dev_ino_t item) {
    for (size_t i = 0; i < set->size; i++) {
        if (set->items[i].dev == item.dev && set->items[i].ino == item.ino) {
            return true;
        }
    }
    return false;
}

static int visited_set_add(visited_set_t *set, dev_ino_t item) {
    if (visited_set_contains(set, item)) {
        return 0;
    }
    if (set->size == set->capacity) {
        size_t new_capacity = set->capacity ? set->capacity * 2 : 16;
        dev_ino_t *new_items = realloc(set->items,
                                       new_capacity * sizeof(dev_ino_t));
        if (!new_items) {
            return -1;
        }
        set->items = new_items;
        set->capacity = new_capacity;
    }
    set->items[set->size++] = item;
    return 0;
}

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

static bool filter_matches(const filter_t *f, const char *path,
                           const struct stat *sb) {
    switch (f->kind) {
    case FILTER_NAME:
        return fnmatch(f->filter.pattern, basename_of(path), 0) == 0;

    case FILTER_TYPE:
        switch (f->filter.type_char) {
        case 'f': return S_ISREG(sb->st_mode);
        case 'd': return S_ISDIR(sb->st_mode);
        case 'l': return S_ISLNK(sb->st_mode);
        default: return false;
        }

    case FILTER_MTIME:
        return difftime(g_now, sb->st_mtime) / 86400 <=
               f->filter.mtime_days;

    case FILTER_SIZE:
        switch (f->filter.size.size_cmp) {
        case SIZE_CMP_EXACT:
            return sb->st_size == f->filter.size.size_bytes;
        case SIZE_CMP_GREATER:
            return sb->st_size > f->filter.size.size_bytes;
        case SIZE_CMP_LESS:
            return sb->st_size < f->filter.size.size_bytes;
        }
        return false;

    case FILTER_PERM:
        return (sb->st_mode & 07777) == f->filter.perm_mode;

    case FILTER_LINKS:
        return sb->st_nlink == f->filter.nlinks;

    case FILTER_SAMEFILE:
        return sb->st_dev == f->filter.samefile.dev &&
               sb->st_ino == f->filter.samefile.ino;
    }

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

static char *joinPath(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);

    char *out = malloc(dlen + nlen + 2);
    if (!out) {
        return NULL;
    }

    if (dlen > 0 && dir[dlen -1] == '/') {
        snprintf(out, dlen + nlen + 2, "%s%s", dir, name);
    } else {
        snprintf(out, dlen + nlen + 2, "%s/%s", dir, name);
    }

    return out;
}

static int stat_for_traversal(const char *path, struct stat *sb) {
    return g_follow_links ? stat(path, sb) : lstat(path, sb);
}

/* ------------------------------------------------------------------ */
/*  BFS traversal                                                      */
/* ------------------------------------------------------------------ */

/*
 * Traverse the filesystem breadth-first starting from the given paths.
 * For each entry, check the filters and print matching paths to stdout.
 */
static void bfs_traverse(char **start_paths, int npaths) {
    queue_t queue;
    visited_set_t visited;
    bool xdev_active = false;

    queue_init(&queue);
    visited_set_init(&visited);

    if (g_xdev && npaths > 0) {
        struct stat start_sb;
        if (stat_for_traversal(start_paths[0], &start_sb) == 0) {
            g_start_dev = start_sb.st_dev;
            xdev_active = true;
        }
    }

    for(int i = 0; i < npaths; i++) {
        char *duplicate = strdup(start_paths[i]);
        if(!duplicate) {
            fprintf(stderr, "bfind: out of memory\n");
            continue;
        }
        if(queue_enqueue(&queue, duplicate) != 0) {
            fprintf(stderr, "bfind: out of memory\n");
            free(duplicate);
        }
    }

    while(!queue_is_empty(&queue)) {

        char *path = (char*)queue_dequeue(&queue);

        struct stat sb;
        if(stat_for_traversal(path, &sb) < 0) {
            fprintf(stderr, "bfind: cannot stat '%s': %s\n", path, strerror(errno));
            free(path);
            continue;
        }

        if (g_follow_links && S_ISDIR(sb.st_mode)) {
            struct stat link_sb;
            if (lstat(path, &link_sb) < 0) {
                fprintf(stderr, "bfind: cannot stat '%s': %s\n", path, strerror(errno));
                free(path);
                continue;
            }
            if (S_ISLNK(link_sb.st_mode)) {
                dev_ino_t target_id = { sb.st_dev, sb.st_ino };
                if (visited_set_contains(&visited, target_id)) {
                    free(path);
                    continue;
                }
                if (visited_set_add(&visited, target_id) != 0) {
                    fprintf(stderr, "bfind: out of memory\n");
                    free(path);
                    continue;
                }
            }
        }

        if (xdev_active && sb.st_dev != g_start_dev) {
            free(path);
            continue;
        }

        if (matches_all_filters(path, &sb)) {
            printf("%s\n", path);
        }

        if(!S_ISDIR(sb.st_mode)) {
            free(path);
            continue;
        }

        DIR *directory = opendir(path);
        if(!directory) {
            fprintf(stderr, "bfind: cannot open '%s': %s\n", path, strerror(errno));
            free(path);
            continue;
        }

        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(directory)) != NULL) {
            if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0) {
                continue;
            }

            char *child = joinPath(path, entry -> d_name);
            if (!child) {
                fprintf(stderr, "bfind: out of memory\n");
                continue;
            }
            if (queue_enqueue(&queue, child) != 0) {
                fprintf(stderr, "bfind: out of memory\n");
                free(child);
            }
            errno = 0;
        }
        if (errno != 0) {
            fprintf(stderr, "bfind: readdir error in '%s': %s\n", path, strerror(errno));
        }

        closedir(directory);
        free(path);
    }

    queue_destroy(&queue);
    visited_set_destroy(&visited);
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
