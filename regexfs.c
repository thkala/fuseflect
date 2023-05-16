/*
 * regexfs - A FUSE filesystem for local directory mirroring
 *
 * Copyright (c) 2007 Theodoros V. Kalamatianos <nyb@users.sourceforge.net>
 * Copyright (c) 2023 Jendrik Weise <jewe37@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */



static const char* fuseregexfsVersion = "2023.05.09";

#define _GNU_SOURCE

#define FUSE_USE_VERSION 26

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <limits.h>
#include <pthread.h>
#include <search.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <libgen.h>

#include <uthash.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static char *src = NULL;
static int src_len;

#define SRC(p)    char __##p [PATH_MAX + 1]; \
                          int e; \
                          if ((e = regexfs_transform_path(p, __##p))) \
                            return e; \
                        p = __##p ;


#define TRY(x, s, f, r) int (r) = (x); if ((r) >= 0) { s; } else { int __e = -errno; f; return __e; }
#define RET(x, s, f)  TRY(x, s, f, __r); return 0;

char* replacement_str;

typedef struct {
    pcre2_code *regex;
    PCRE2_SPTR* replacements;
    int n_replacements;
} regex_replaces;

regex_replaces* results = NULL;
int n_results;

int regexfs_parse_replacements() {
    n_results = 0;
    int last_semicolon = 0;
    int full_len = strlen(replacement_str);
    for (int i = 0; i < full_len; i++) {
        switch (replacement_str[i]) {
        case ':':
            n_results++;
            if (last_semicolon)
                replacement_str[last_semicolon] = '\0';
            break;
        case ';':
            last_semicolon = i;
            break;
        default:
            break;
        }
    }

    results = malloc(sizeof(regex_replaces) * n_results);

    // iterate over the regex_replaces structs
    for (int i = 0; i < n_results; i++) {
        // parse the regex
        PCRE2_SPTR regex = (PCRE2_SPTR)replacement_str;
        replacement_str = strchr(replacement_str, ':');
        replacement_str[0] = '\0';
        replacement_str++;

        // parse the replacements
        results[i].n_replacements = 1;
        int j;
        for (j = 0; replacement_str[j] != '\0'; j++) {
            if (replacement_str[j] == ';') {
                replacement_str[j] = '\0';
                results[i].n_replacements++;
            }
        }
        printf("regex %s\n", regex);

        results[i].replacements = malloc(sizeof(PCRE2_SPTR) * results[i].n_replacements);
        // fill replacements with pointers to the start of each replacement
        for (int k = 0; k < results[i].n_replacements; k++) {
            results[i].replacements[k] = (PCRE2_SPTR)replacement_str;
            printf("replacement %s\n", replacement_str);
            replacement_str += strlen(replacement_str) + 1;
        }

        // compile the regex
        int errorcode;
        PCRE2_SIZE erroroffset;
        pcre2_code *compiled_regex = pcre2_compile(
            regex,
            PCRE2_ZERO_TERMINATED,
            0,
            &errorcode,
            &erroroffset,
            NULL
        );
        if (compiled_regex == NULL) {
            // Handle compile error
            PCRE2_UCHAR buffer[256];
            pcre2_get_error_message(errorcode, buffer, sizeof(buffer));
            fprintf(stderr, "PCRE2 compilation failed at offset %d: %s\n", (int)erroroffset, buffer);
            return -1;
        }

        results[i].regex = compiled_regex;
    }
    return 0;
}

// uthash table for caching
typedef struct {
    char* path;
    char* output_path;
    UT_hash_handle hh;
} path_cache_entry;

path_cache_entry* path_cache = NULL;

pthread_rwlock_t path_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

static int regexfs_transform_path(const char* path, char* output_path) {
    // idea for caching: path -> output_path is an injective function, so we can use a hash table to cache the results
    // if the hash table entry does not point to a file that exists anymore, we can remove it from the hash table and try again
    strncpy(output_path, src, PATH_MAX);
    strncat(output_path, path, PATH_MAX - src_len);

    struct stat statbuf;
    if (!stat(path, &statbuf) && S_ISDIR(statbuf.st_mode))
        return 0;
    
    char* dirc = strdupa(path);
    char* dirn = dirname(dirc);
    int dirn_len = strlen(dirn);
    output_path[src_len + dirn_len] = '\0';

    // check if path in hashtable
    path_cache_entry* entry;
    // lock
    pthread_rwlock_rdlock(&path_cache_lock);
    HASH_FIND_STR(path_cache, path, entry);
    if (entry != NULL) {
        // check if file exists
        if (!stat(entry->output_path, &statbuf) && S_ISREG(statbuf.st_mode)) {
            strncpy(output_path, entry->output_path, PATH_MAX);
            pthread_rwlock_unlock(&path_cache_lock);
            return 0;
        } else {
            pthread_rwlock_unlock(&path_cache_lock);
            pthread_rwlock_wrlock(&path_cache_lock);
            HASH_FIND_STR(path_cache, path, entry);
            if (entry != NULL) {
                // remove entry from hashtable
                HASH_DEL(path_cache, entry);
                free(entry->path);
                free(entry->output_path);
                free(entry);
            }
        }
    }
    pthread_rwlock_unlock(&path_cache_lock);

    char* basec = strdupa(path);
    char* base = basename(basec);

    DIR *dp;
    struct dirent *de;
    PCRE2_UCHAR new_name[NAME_MAX + 1];
    PCRE2_SIZE new_name_len;
    int num_matches;

    dp = opendir(output_path);
    if (dp == NULL)
        return -errno;

    strncat(output_path, "/", PATH_MAX - src_len - dirn_len);

    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) 
            continue;
        int found = 0;
        for (int i = 0; i < n_results; ++i) {
            pcre2_code* regex = results[i].regex;
            pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(regex, NULL);
            if (match_data == NULL) {
                fprintf(stderr, "pcre2_match_data_create_from_pattern failed!\n");
                exit(2);
            }
            num_matches = pcre2_match(regex, (PCRE2_SPTR)de->d_name, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL);
            if (num_matches < 0) {
                pcre2_match_data_free(match_data);
                continue;
            }

            for (int j = 0; j < results[i].n_replacements; ++j) {
                new_name_len = NAME_MAX + 1;
                num_matches = pcre2_substitute(
                    regex,  // Compiled regex
                    (PCRE2_SPTR)de->d_name,  // Subject string
                    PCRE2_ZERO_TERMINATED,  // Subject length
                    0,  // Start offset
                    PCRE2_SUBSTITUTE_MATCHED,  // Options
                    match_data,  // Match data
                    NULL,  // Match context
                    results[i].replacements[j],  // Replacement string
                    PCRE2_ZERO_TERMINATED,  // Replacement length
                    new_name, // output
                    &new_name_len  // Result length
                );
                if (num_matches < 1)
                    exit(1);
                new_name[new_name_len] = '\0';

                if (strcmp((char*)new_name, base) == 0) {
                    pcre2_match_data_free(match_data);
                    goto success;
                }
                found = 1;
            }
            pcre2_match_data_free(match_data);
        }
        if (!found && strcmp(de->d_name, base) == 0)
            goto success;
    }

    // no results
    closedir(dp);
    return -ENOENT;
success:
    strncat(output_path, de->d_name, PATH_MAX - src_len - dirn_len - 1);
    // add output_path to cache
    entry = malloc(sizeof(path_cache_entry));
    entry->path = strdup(path);
    entry->output_path = strdup(output_path);
    pthread_rwlock_wrlock(&path_cache_lock);
    HASH_ADD_KEYPTR(hh, path_cache, entry->path, strlen(entry->path), entry);
    pthread_rwlock_unlock(&path_cache_lock);

    closedir(dp);
    return 0;
}

static int regexfs_getattr(const char *path, struct stat *stbuf) {
  SRC(path)
  RET(lstat(path, stbuf),,)
}

static int regexfs_readlink(const char *path, char *buf, size_t size) {
  SRC(path)
  RET(readlink(path, buf, size - 1), buf[__r] = '\0',)
}

static int regexfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    (void)offset;
    (void)fi;
    DIR *dp;
    struct dirent *de;
    int num_matches;
    PCRE2_UCHAR new_name[NAME_MAX + 1];
    PCRE2_SIZE new_name_len;

    char new_path[PATH_MAX + 1];
    strncpy(new_path, src, PATH_MAX);
    strncat(new_path, path, PATH_MAX - strlen(src));
    path = new_path;

    dp = opendir(path);
    if (dp == NULL)
        return -errno;

    while ((de = readdir(dp)) != NULL) {
        int added = 0;
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        if (strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) {
            if (filler(buf, de->d_name, &st, 0))
                goto error;
            continue;
        }
        if (de->d_type == DT_DIR) {
            if (filler(buf, de->d_name, &st, 0))
                goto error;
            continue;
        }
        for (int i = 0; i < n_results; ++i) {
            pcre2_code* regex = results[i].regex;
            pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(regex, NULL);
            if (match_data == NULL) {
                fprintf(stderr, "pcre2_match_data_create_from_pattern failed!\n");
                exit(2);
            }
            num_matches = pcre2_match(regex, (PCRE2_SPTR)de->d_name, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL);
            if (num_matches < 0) {
                pcre2_match_data_free(match_data);
                continue;
            }

            for (int j = 0; j < results[i].n_replacements; ++j) {
                new_name_len = NAME_MAX + 1;
                num_matches = pcre2_substitute(
                    regex,  // Compiled regex
                    (PCRE2_SPTR)de->d_name,  // Subject string
                    PCRE2_ZERO_TERMINATED,  // Subject length
                    0,  // Start offset
                    PCRE2_SUBSTITUTE_MATCHED,  // Options
                    match_data,  // Match data
                    NULL,  // Match context
                    results[i].replacements[j],  // Replacement string
                    PCRE2_ZERO_TERMINATED,  // Replacement length
                    new_name, // output
                    &new_name_len  // Result length
                );
                if (num_matches < 1)
                    exit(1);
                new_name[new_name_len] = '\0';

                if (filler(buf, (const char*)new_name, &st, 0)) {
                    pcre2_match_data_free(match_data);
                    goto error;
                }
                added = 1;
            }
            pcre2_match_data_free(match_data);
        }
        if (!added && filler(buf, de->d_name, &st, 0))
            break;
    }
    closedir(dp);
    return 0;
error:
    closedir(dp);
    return -1;
}

static int regexfs_mknod(const char *path, mode_t mode, dev_t rdev) {
  (void)path;
  (void)mode;
  (void)rdev;
  return -EROFS;
}

static int regexfs_mkdir(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return -EROFS;
}

static int regexfs_unlink(const char *path) {
  (void)path;
  return -EROFS;
}

static int regexfs_rmdir(const char *path) {
  (void)path;
  return -EROFS;
}

static int regexfs_symlink(const char *from, const char *to) {
  (void)from;
  (void)to;
  return -EROFS;
}

static int regexfs_rename(const char *from, const char *to) {
  (void)from;
  (void)to;
  return -EROFS;
}

static int regexfs_link(const char *from, const char *to) {
  (void)from;
  (void)to;
  return -EROFS;
}

static int regexfs_chmod(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return -EROFS;
}

static int regexfs_chown(const char *path, uid_t uid, gid_t gid) {
  (void)path;
  (void)uid;
  (void)gid;
  return -EROFS;
}

static int regexfs_truncate(const char *path, off_t size) {
  (void)path;
  (void)size;
  return -EROFS;
}

static int regexfs_utimens(const char *path, const struct timespec ts[2]) {
  (void)path;
  (void)ts;
  return -EROFS;  
}

static int regexfs_open(const char *path, struct fuse_file_info *fi) {
  int flags = fi->flags;
  
  if ((flags & O_WRONLY) || (flags & O_RDWR) || (flags & O_CREAT) || (flags & O_EXCL) || (flags & O_TRUNC) || (flags & O_APPEND))
    return -EROFS;
  SRC(path)
  RET(open(path, fi->flags), close(__r),)
}

static int regexfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  (void)fi;
  SRC(path)
  TRY(open(path, O_RDONLY),,,fd)
  RET(pread(fd, buf, size, offset), close(fd); return __r, close(fd))
}

static int regexfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
  (void)path;
  (void)buf;
  (void)size;
  (void)offset;
  (void)fi;
  return -EROFS;
}

static int regexfs_statfs(const char *path, struct statvfs *stbuf) {
  SRC(path)
  RET(statvfs(path, stbuf),,)
}

static int regexfs_access(const char *path, int mask) {
  if (mask & W_OK)
    return -EROFS;
  SRC(path)
  RET(access(path, mask),,)
}

static int regexfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
  (void)path;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  return -EROFS;
}

static int regexfs_getxattr(const char *path, const char *name, char *value, size_t size) {
  SRC(path)
  RET(lgetxattr(path, name, value, size),,)
}

static int regexfs_listxattr(const char *path, char *list, size_t size) {
  SRC(path)
  RET(llistxattr(path, list, size),,)
}

static int regexfs_removexattr(const char *path, const char *name) {
  (void)path;
  (void)name;
  return -EROFS;
}


#define OP(x)    . x = regexfs_##x ,
static struct fuse_operations regexfs_oper = {
  OP(getattr)
  OP(access)
  OP(readlink)
  OP(readdir)
  OP(mknod)
  OP(mkdir)
  OP(symlink)
  OP(unlink)
  OP(rmdir)
  OP(rename)
  OP(link)
  OP(chmod)
  OP(chown)
  OP(truncate)
  OP(utimens)
  OP(open)
  OP(read)
  OP(write)
  OP(statfs)
  OP(setxattr)
  OP(getxattr)
  OP(listxattr)
  OP(removexattr)
};

enum {
    KEY_HELP,
    KEY_VERSION,
};

static void usage(const char* progname)
{
    fprintf(stdout,
"usage: %s readwritepath -o regex=... mountpoint [options]\n"
"\n"
"   Mounts readwritepath as a read-only mount at mountpoint\n"
"   Regex is used to replace filenames in the read-only mount\n"
"   The format for the regex is:\n"
"   regex1:replacement1;replacement2|regex2:replacement3...\n"
"\n"
"general options:\n"
"   -o opt,[opt...]     mount options\n"
"   -h  --help          print help\n"
"   -V  --version       print version\n"
"\n", progname);
}

static int regexfs_parse_opt(void *data, const char *arg, int key,
        struct fuse_args *outargs) {
    (void) data;

    switch (key) {
        case FUSE_OPT_KEY_NONOPT:
            if (src == NULL) {
                src = strdup(arg);
                src_len = strlen(src);
                return 0;
            } else {
                return 1;
            }
        case FUSE_OPT_KEY_OPT:
            return 1;
        case KEY_HELP:
            usage(outargs->argv[0]);
            exit(0);
        case KEY_VERSION:
            fprintf(stdout, "fuseregexfs version %s\n", fuseregexfsVersion);
            exit(0);
        default:
            fprintf(stderr, "see `%s -h' for usage\n", outargs->argv[0]);
            exit(1);
    }
    return 1;
}

static struct fuse_opt regexfs_opts[] = {
    FUSE_OPT_KEY("-h",          KEY_HELP),
    FUSE_OPT_KEY("--help",      KEY_HELP),
    FUSE_OPT_KEY("-V",          KEY_VERSION),
    FUSE_OPT_KEY("--version",   KEY_VERSION),
    { "regex=%s", 0, 0 },
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    int res;

    res = fuse_opt_parse(&args, &replacement_str, regexfs_opts, regexfs_parse_opt);
    if (res != 0) {
        fprintf(stderr, "Invalid arguments\n");
        fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
        exit(1);
    }
    if (src == 0) {
        fprintf(stderr, "Missing readwritepath\n");
        fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
        exit(1);
    }
    if (replacement_str == 0) {
        fprintf(stderr, "Missing regex\n");
        fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
        exit(1);
    }
    regexfs_parse_replacements();

    #if FUSE_VERSION >= 26
        fuse_main(args.argc, args.argv, &regexfs_oper, NULL);
    #else
        fuse_main(args.argc, args.argv, &regexfs_oper);
    #endif

	/* some array are not freed, but required for entire lifetime of program anyway */
    return 0;
}
